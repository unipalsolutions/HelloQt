#include "recorder.h"

#include "imageio.h"
#include "theme.h"
#include "videofilters.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <QUuid>

#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <map>
#include <utility>

namespace {

constexpr size_t MaxQueue = 90;                 // نحو 3 ثوانٍ؛ إن تأخرت الكتابة تُترك أقدم الإطارات
constexpr qint64 MinFreeBytes = 500ll << 20;    // يتوقف التسجيل قبل أن يمتلئ القرص
constexpr double MaxGapSeconds = 2.0;           // انقطاع أطول لا يُملأ بإطار متجمد
// جهاز لا يلحق بترميز كل إطارات الكاميرا: تُترك الإطارات حين يمتلئ الطابور ويُكرر ما قبلها لملء الوقت، فيظهر
// التسجيل متجمداً ثم يقفز، والتكرار نفسه ترميز إضافي يزيد التأخر. فإن تجاوز الترميز هذه النسبة من الوقت يُسجَّل
// بجزء صحيح من سرعة الكاميرا (النصف، الثلث…) في ملف جديد، فتبقى الحركة منتظمة بسرعة أقل. وإن خف الحمل بعد ذلك
// (انتهى برنامج آخر مثلاً) ترتفع السرعة بالقياس نفسه، فهي تتبع قدرة الجهاز في كل وقت لا قيمة ثابتة
constexpr double EncodeBudget = 0.7;
constexpr double EncodeHeadroom = 0.8;
constexpr int EncodeSamples = 50;               // إطارات تُقاس قبل الحكم على سرعة الترميز
constexpr double MinRecordFps = 5.0;
constexpr auto RaiseAfter = std::chrono::seconds(60); // أقل مدة بين تغيير السرعة ورفعها، فلا تتكاثر الملفات
// توقف وصول الإطارات أطول من هذا (انقطاع الكاميرا) يبدأ ملفاً جديداً باسم وقت العودة، فيبقى الوقت داخل كل ملف
// مطابقاً للوقت الفعلي. أما هدوء وضع الحركة فالإطارات تصل فيه ولا يُعد انقطاعاً
constexpr auto FeedGap = std::chrono::milliseconds(2000);
// ملفات التسجيل الجارية: «malamih-rec-…mkv»، وبجانب كل منها ملف «.target» فيه اسمه النهائي، فإن انطفأ
// الجهاز قبل إغلاقه يُستعاد بهذا الاسم عند التشغيل التالي
const QString WritingPrefix = QStringLiteral("malamih-rec-");
const QString TargetSuffix = QStringLiteral(".target");
// نسخة يُعاد بناؤها من تسجيل مستعاد؛ إن بقيت فقد انقطع بناؤها هي أيضاً فتُحذف
const QString RebuildPrefix = QStringLiteral("malamih-fix-");
// وضع الحركة فقط
constexpr auto PreRoll = std::chrono::seconds(2);       // يُحفظ ما قبل الحركة ليظهر من دخل المشهد
constexpr auto PreRollStep = std::chrono::milliseconds(250); // إطار كل ربع ثانية فقط قبل الحركة، فتبقى الذاكرة صغيرة
constexpr auto PostRoll = std::chrono::seconds(10);     // يستمر التسجيل بعد آخر حركة
constexpr auto EventGap = std::chrono::seconds(60);     // هدوء أطول يجعل الحدث التالي ملفاً جديداً
constexpr auto MotionReferenceAge = std::chrono::milliseconds(500);
constexpr double MotionPixels = 0.005; // نسبة البكسلات المتغيرة التي تُعد حركة
constexpr int MotionPixelDifference = 22;
constexpr auto OverlayLifetime = std::chrono::milliseconds(1500); // مربعات أقدم من هذا لا تُرسم

std::mutex &manifestMutex()
{
    static std::mutex mutex;
    return mutex;
}

QString manifestPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/recordings.txt");
}

QStringList readManifest()
{
    QFile file(manifestPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList paths;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QString path = QString::fromUtf8(line).trimmed();
        if (!path.isEmpty() && !paths.contains(path))
            paths << path;
    }
    return paths;
}

void writeManifest(const QStringList &paths)
{
    QDir().mkpath(QFileInfo(manifestPath()).absolutePath());
    QSaveFile file(manifestPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(paths.join(QLatin1Char('\n')).toUtf8());
    file.commit();
}

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

bool isAscii(const QString &text)
{
    return std::all_of(text.begin(), text.end(), [](QChar c) { return c.unicode() < 128; });
}

// OpenCV لا يكتب إلى المسارات العربية على Windows: يُكتب في المجلد نفسه عبر مساره القصير (8.3)
// إن أمكن، وإلا في مجلد مؤقت، ثم يُعطى الملف اسمه النهائي عند إغلاقه
QString writableFolder(const QString &folder)
{
    if (isAscii(folder))
        return folder;
#ifdef _WIN32
    const std::wstring native = QDir::toNativeSeparators(folder).toStdWString();
    const DWORD length = GetShortPathNameW(native.c_str(), nullptr, 0);
    if (length > 0) {
        std::wstring shortPath(length, L'\0');
        const DWORD written = GetShortPathNameW(native.c_str(), shortPath.data(), length);
        if (written > 0 && written < length) {
            shortPath.resize(written);
            const QString result = QString::fromStdWString(shortPath);
            if (isAscii(result))
                return result;
        }
    }
#endif
    return QDir::tempPath();
}

QString safeName(QString name)
{
    static const QString forbidden = QStringLiteral("\\/:*?\"<>|");
    for (QChar &c : name) {
        if (forbidden.contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    }
    name = name.simplified();
    return name.isEmpty() ? QStringLiteral("الكاميرا") : name;
}

// الملفات التي يكتب إليها مسجّل يعمل الآن، فلا تمسها الاستعادة
std::mutex &activeMutex()
{
    static std::mutex mutex;
    return mutex;
}

QStringList &activeFiles()
{
    static QStringList files;
    return files;
}

// آخر سرعة تسجيل لحق بها الجهاز لكل كاميرا (نسبةً من سرعتها)، فيبدأ تسجيلها التالي بها مباشرة بدل ملف قصير
// بالسرعة الكاملة قبل الخفض. في الذاكرة فقط، فكل تشغيل للبرنامج يقيس الجهاز من جديد
std::mutex &rateMutex()
{
    static std::mutex mutex;
    return mutex;
}

QHash<QString, double> &learnedRates()
{
    static QHash<QString, double> rates;
    return rates;
}

bool moveFile(const QString &from, const QString &to)
{
    if (QFile::rename(from, to))
        return true;
    // من المجلد المؤقت إلى قرص آخر: نسخ ثم حذف
    if (!QFile::copy(from, to))
        return false;
    QFile::remove(from);
    return true;
}

// MKV انقطعت كتابته يُشغَّل، لكن بلا مدة ولا فهرس فلا يُعرف طوله ولا يُقفز فيه. تُقرأ إطاراته وتُكتب
// في ملف مكتمل يحل محله؛ يعيد هل نجح (وإلا يبقى الأصل كما هو). المسار لاتيني (مجلد الكتابة)
bool rebuildRecording(const QString &path)
{
    cv::VideoCapture input;
    if (!input.open(QDir::toNativeSeparators(path).toStdString(), cv::CAP_FFMPEG))
        return false;
    const double reported = input.get(cv::CAP_PROP_FPS);
    const double fps = reported > 1.0 && reported <= 60.0 ? reported : 25.0;
    const QString rebuilt = QFileInfo(path).dir().filePath(RebuildPrefix + QFileInfo(path).fileName().mid(WritingPrefix.size()));
    cv::VideoWriter output;
    qint64 frames = 0;
    cv::Mat frame;
    while (input.read(frame)) {
        if (frame.empty())
            continue;
        if (!output.isOpened()) {
            const std::string native = QDir::toNativeSeparators(rebuilt).toStdString();
            if (!output.open(native, cv::CAP_FFMPEG, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, frame.size()))
                return false;
        }
        output.write(frame);
        ++frames;
    }
    input.release();
    output.release();
    if (frames == 0 || !QFile::remove(path)) {
        QFile::remove(rebuilt);
        return false;
    }
    return QFile::rename(rebuilt, path);
}

} // namespace

CameraRecorder::CameraRecorder(QObject *parent)
    : QObject(parent)
{
}

CameraRecorder::~CameraRecorder()
{
    stop();
}

QString CameraRecorder::defaultFolder()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    return QDir(base).filePath(QStringLiteral("ملامح - تسجيلات الكاميرا"));
}

QString CameraRecorder::savedFolder()
{
    const QString folder = QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("recordFolder")).toString();
    return folder.isEmpty() ? defaultFolder() : folder;
}

void CameraRecorder::setSavedFolder(const QString &folder)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("recordFolder"), folder);
}

bool CameraRecorder::autoRecord()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("autoRecord"), false).toBool();
}

void CameraRecorder::setAutoRecord(bool enabled)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("autoRecord"), enabled);
}

bool CameraRecorder::start(const QString &folder, const QString &cameraName, double fps, const Options &options, QString *error)
{
    stop();
    if (!QDir().mkpath(folder)) {
        if (error)
            *error = QStringLiteral("تعذر إنشاء مجلد التسجيلات:\n%1").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    removeExpired();
    if (!freeSpace(folder, MinFreeBytes)) {
        if (error)
            *error = QStringLiteral("لا توجد مساحة كافية على القرص لتسجيل الكاميرا (أقل من 500 ميغابايت)");
        return false;
    }
    m_folder = folder;
    m_cameraName = safeName(cameraName);
    m_fps = m_cameraFps = fps > 1.0 && fps <= 60.0 ? fps : 25.0;
    {
        std::lock_guard<std::mutex> lock(rateMutex());
        const double ratio = learnedRates().value(m_cameraName, 1.0);
        if (ratio < 1.0 && !options.fullRate)
            m_fps = std::max(std::min(MinRecordFps, m_cameraFps), m_cameraFps * ratio);
    }
    m_encodeMs = 0.0;
    m_encodeSamples = 0;
    m_rateChanged = Clock::now();
    m_options = options;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        m_files.clear();
        m_overlay.clear();
        m_livePath.clear();
        m_stop = false;
    }
    m_liveMs = -1;
    m_activity = 0;
    m_received = m_dropped = m_writtenTotal = 0;
    m_preRoll.clear();
    m_motionReference.release();
    m_hadMotion = false;
    m_lastFrame = Clock::time_point();
    m_startedAt = Clock::now();
    m_spaceChecked = m_cleaned = m_startedAt;
    m_waiting = options.motionOnly;
    m_offSchedule = options.schedule.enabled && !options.schedule.active();
    m_scheduleChecked = Clock::time_point();
    m_recording = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void CameraRecorder::stop()
{
    m_recording = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    m_waiting = false;
}

qint64 CameraRecorder::elapsedMs() const
{
    if (!m_recording)
        return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_startedAt).count();
}

QStringList CameraRecorder::files() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_files;
}

CameraRecorder::Position CameraRecorder::position() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Position result;
    if (!m_livePath.isEmpty() && m_recording) {
        result.path = m_livePath;
        result.ms = m_liveMs;
    }
    return result;
}

CameraRecorder::Stats CameraRecorder::stats() const
{
    return {m_received.load(), m_dropped.load(), m_writtenTotal.load()};
}

void CameraRecorder::push(const cv::Mat &frame)
{
    if (!m_recording || frame.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop)
            return;
        ++m_received;
        if (m_queue.size() >= MaxQueue) {
            m_queue.pop_front();
            ++m_dropped;
        }
        // الإطار لا يُنسخ: خيط القراءة يقرأ كل إطار في ذاكرة جديدة، والمعالجات لا تكتب في الأصل
        m_queue.push_back({frame, Clock::now()});
    }
    m_wake.notify_one();
}

void CameraRecorder::setOverlay(const QList<OverlayBox> &boxes)
{
    if (!m_recording)
        return;
    const Clock::time_point now = Clock::now();
    if (!boxes.isEmpty())
        m_activity = now.time_since_epoch().count();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlay = boxes;
    m_overlayTime = now;
}

void CameraRecorder::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty())
            break; // أُوقف وكُتب كل ما وصل
        const Pending item = std::move(m_queue.front());
        m_queue.pop_front();
        lock.unlock();
        if (m_recording || m_stop) {
            // خيط مستقل: خطأ في الكتابة أو الترميز يوقف التسجيل برسالة لا البرنامج كله
            try {
                handle(item);
            } catch (const std::exception &e) {
                qCritical("خطأ في تسجيل الكاميرا: %s", e.what());
                try {
                    closeSegment();
                } catch (const std::exception &) {
                }
                fail(QStringLiteral("توقف التسجيل بسبب خطأ: %1").arg(QString::fromLocal8Bit(e.what())));
            }
        }
        lock.lock();
    }
    lock.unlock();
    closeSegment();
}

void CameraRecorder::handle(const Pending &item)
{
    if (!m_recording && !m_writer.isOpened())
        return; // فشل سابق

    if (item.time - m_spaceChecked >= std::chrono::seconds(30)) {
        m_spaceChecked = item.time;
        // مع الحذف التلقائي تُحذف أقدم التسجيلات لتتسع للجديدة، وإلا يتوقف التسجيل
        if (!freeSpace(m_folder, MinFreeBytes)) {
            closeSegment();
            fail(retentionDays() > 0 ? QStringLiteral("توقف التسجيل لأن القرص كاد يمتلئ ولم يبقَ تسجيل قديم يُحذف؛ حُفظ ما سُجِّل")
                                     : QStringLiteral("توقف التسجيل لأن القرص كاد يمتلئ؛ حُفظ ما سُجِّل حتى الآن"));
            return;
        }
    }
    if (item.time - m_cleaned >= std::chrono::hours(1)) {
        m_cleaned = item.time;
        removeExpired();
    }

    // عادت الكاميرا بعد انقطاع: ملف جديد يبدأ بوقت العودة بدل وصل ما بعد الانقطاع بما قبله
    const bool resumed = m_lastFrame != Clock::time_point() && item.time - m_lastFrame > FeedGap;
    m_lastFrame = item.time;
    if (resumed && m_writer.isOpened())
        closeSegment();

    if (m_options.schedule.enabled) {
        // يُفحص الجدول مرة في الثانية تقريباً؛ خارج ساعاته يُغلق الملف ولا يُكتب شيء
        if (item.time - m_scheduleChecked >= std::chrono::seconds(1) || m_scheduleChecked == Clock::time_point()) {
            m_scheduleChecked = item.time;
            m_offSchedule = !m_options.schedule.active();
        }
        if (m_offSchedule) {
            if (m_writer.isOpened())
                closeSegment();
            m_preRoll.clear();
            m_hadMotion = false;
            return;
        }
    }

    if (!m_options.motionOnly) {
        write(prepare(item), item.time);
        return;
    }

    if (detectMotion(item.frame, item.time)) {
        m_lastMotion = item.time;
        m_hadMotion = true;
    }
    if (m_hadMotion && item.time - m_lastMotion <= PostRoll) {
        for (const Pending &earlier : m_preRoll)
            write(prepare(earlier), earlier.time);
        m_preRoll.clear();
        write(prepare(item), item.time);
        m_waiting = false;
        return;
    }
    m_waiting = true;
    if (m_preRoll.empty() || item.time - m_preRoll.back().time >= PreRollStep)
        m_preRoll.push_back(item);
    while (!m_preRoll.empty() && item.time - m_preRoll.front().time > PreRoll)
        m_preRoll.pop_front();
    // هدوء طويل: الحدث التالي في ملف خاص به، فيسهل الوصول إليه
    if (m_writer.isOpened() && item.time - m_lastMotion >= EventGap)
        closeSegment();
}

cv::Mat CameraRecorder::prepare(const Pending &item)
{
    cv::Mat frame = item.frame;
    const VideoFilters::Settings filters = m_options.ownFilters ? m_options.filters : VideoFilters::current();
    if (!filters.isIdentity())
        frame = VideoFilters::apply(frame, filters);
    if (frame.channels() == 1)
        cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
    else if (frame.channels() == 4)
        cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
    // أبعاد فردية لا تقبلها بعض مشغلات MPEG-4
    if ((frame.cols % 2) || (frame.rows % 2))
        frame = frame(cv::Rect(0, 0, frame.cols & ~1, frame.rows & ~1));
    if (m_options.overlay)
        drawOverlay(frame, item.time);
    return frame;
}

void CameraRecorder::drawOverlay(cv::Mat &frame, Clock::time_point time)
{
    QList<OverlayBox> boxes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (time - m_overlayTime > OverlayLifetime)
            return;
        boxes = m_overlay;
    }
    if (boxes.isEmpty() || frame.empty() || frame.type() != CV_8UC3)
        return;
    // الإطار قد يشارك ذاكرته خيط القراءة والتحليل، فيُرسم على نسخة
    frame = frame.clone();
    QImage image(frame.data, frame.cols, frame.rows, int(frame.step), QImage::Format_BGR888);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal line = std::max(2.0, frame.rows / 300.0);
    QFont font = painter.font();
    font.setPixelSize(std::max(12, frame.rows / 36));
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    for (const OverlayBox &overlay : std::as_const(boxes)) {
        const QRectF box(overlay.box.x() * frame.cols, overlay.box.y() * frame.rows, overlay.box.width() * frame.cols,
                         overlay.box.height() * frame.rows);
        painter.setPen(QPen(overlay.color, line));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
        if (overlay.label.isEmpty())
            continue;
        const int width = metrics.horizontalAdvance(overlay.label) + 12;
        const int height = metrics.height() + 4;
        QRectF tag(box.left(), box.top() - height, width, height);
        if (tag.top() < 0)
            tag.moveTop(box.bottom());
        painter.fillRect(tag, overlay.color);
        painter.setPen(Qt::black);
        painter.drawText(tag, Qt::AlignCenter, overlay.label);
    }
}

bool CameraRecorder::detectMotion(const cv::Mat &frame, Clock::time_point time)
{
    // وجه ظهر في التحليل يُعد حركة ولو كان صاحبه واقفاً
    const Clock::time_point activity{Clock::duration(m_activity.load())};
    bool moved = m_activity.load() != 0 && time - activity <= std::chrono::seconds(2);

    cv::Mat scaled, gray;
    const int width = 96;
    scaled = ImageIO::shrink(frame, cv::Size(width, std::max(1, width * frame.rows / std::max(1, frame.cols))));
    if (scaled.channels() == 3)
        cv::cvtColor(scaled, gray, cv::COLOR_BGR2GRAY);
    else if (scaled.channels() == 4)
        cv::cvtColor(scaled, gray, cv::COLOR_BGRA2GRAY);
    else
        gray = scaled;
    cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0);
    if (!m_motionReference.empty() && m_motionReference.size() == gray.size()) {
        cv::Mat difference;
        cv::absdiff(gray, m_motionReference, difference);
        const int changed = cv::countNonZero(difference > MotionPixelDifference);
        moved = moved || changed > MotionPixels * double(difference.total());
    }
    // المقارنة بإطار من نصف ثانية مضت، فتظهر الحركة البطيئة أيضاً مع الكاميرات السريعة
    if (m_motionReference.empty() || m_motionReference.size() != gray.size() || time - m_motionReferenceTime >= MotionReferenceAge) {
        m_motionReference = gray;
        m_motionReferenceTime = time;
    }
    return moved;
}

void CameraRecorder::write(const cv::Mat &frame, Clock::time_point time)
{
    if (frame.cols < 2 || frame.rows < 2 || (!m_recording && !m_stop))
        return;

    // ملف جديد كل SegmentMinutes دقيقة، أو إذا تغيرت أبعاد الإطار (تدوير الكاميرا من المعالجات)
    if (m_writer.isOpened() && (frame.size() != m_size || time - m_segmentStart >= std::chrono::minutes(SegmentMinutes)))
        closeSegment();

    if (!m_writer.isOpened()) {
        QString error;
        if (!openSegment(frame.size(), time, &error)) {
            fail(error);
            return;
        }
        m_segmentStart = m_timeBase = time;
        m_written = 0;
    }

    // عدد الإطارات يتبع الوقت الفعلي، فمدة التسجيل صحيحة ولو اختلفت سرعة الكاميرا عن المعلنة:
    // يُكرر الإطار إن تأخرت الكاميرا، ويُترك إن سبقت
    const double seconds = std::chrono::duration<double>(time - m_timeBase).count();
    qint64 target = qint64(seconds * m_fps) + 1;
    if (double(target - m_written) > MaxGapSeconds * m_fps) {
        // عادت الكاميرا بعد انقطاع (أو بدأت حركة بعد هدوء): يكمل التسجيل من حيث توقف دون ملء الفترة بصورة متجمدة
        m_timeBase = time - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(m_written / m_fps));
        target = m_written + 1;
    }
    while (m_written < target) {
        const Clock::time_point began = Clock::now();
        m_writer.write(frame);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        m_encodeMs = m_encodeSamples++ == 0 ? ms : m_encodeMs * 0.95 + ms * 0.05;
        ++m_written;
        ++m_writtenTotal;
    }
    m_liveMs = qint64(double(m_written - 1) * 1000.0 / m_fps);

    if (m_encodeSamples < EncodeSamples || m_options.fullRate)
        return;
    // السرعة التي يتسع لها الجهاز الآن، بهامش تحت الحد فلا تتأرجح (ومعها ملف جديد) إن تغير زمن الترميز قليلاً
    const int divisor = std::max(1, int(std::ceil(m_encodeMs * m_cameraFps / (EncodeHeadroom * EncodeBudget * 1000.0))));
    const double fitting = std::max(std::min(MinRecordFps, m_cameraFps), m_cameraFps / divisor);
    const bool lower = m_encodeMs * m_fps > EncodeBudget * 1000.0 && fitting < m_fps - 0.01;
    const bool raise = fitting > m_fps + 0.01 && time - m_rateChanged >= RaiseAfter;
    if (m_encodeMs * m_fps > EncodeBudget * 1000.0)
        m_encodeSamples = 0; // يُعاد القياس بعد كل حكم بالخفض، أما الرفع فينتظر مرور المدة فقط
    if (!lower && !raise)
        return;
    qWarning("زمن ترميز الإطار %.0f ms: يُسجَّل بسرعة %.1f إطاراً في الثانية (الكاميرا %.1f)", m_encodeMs, fitting, m_cameraFps);
    m_fps = fitting;
    m_rateChanged = time;
    {
        std::lock_guard<std::mutex> lock(rateMutex());
        learnedRates().insert(m_cameraName, m_fps / m_cameraFps);
    }
    m_encodeSamples = 0;
    closeSegment();
}

bool CameraRecorder::openSegment(const cv::Size &size, Clock::time_point time, QString *error)
{
    // الاسم بوقت الإطار الأول لا وقت فتح الملف (ثواني ما قبل الحركة، والإطارات المنتظرة في الطابور)
    const qint64 delayMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - time).count();
    const QString stamp = QDateTime::currentDateTime().addMSecs(-std::max<qint64>(0, delayMs)).toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"));
    QString finalPath = QDir(m_folder).filePath(QStringLiteral("%1 %2.mkv").arg(m_cameraName, stamp));
    for (int n = 2; QFile::exists(finalPath); ++n)
        finalPath = QDir(m_folder).filePath(QStringLiteral("%1 %2 (%3).mkv").arg(m_cameraName, stamp).arg(n));

    const QString ascii = QStringLiteral("%1%2.mkv").arg(WritingPrefix, QUuid::createUuid().toString(QUuid::Id128).left(12));
    const QString writing = QDir(writableFolder(m_folder)).filePath(ascii);
    {
        std::lock_guard<std::mutex> lock(activeMutex());
        activeFiles().append(writing);
    }
    const std::string native = QDir::toNativeSeparators(writing).toStdString();
    const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
    if (!m_writer.open(native, cv::CAP_FFMPEG, fourcc, m_fps, size) && !m_writer.open(native, fourcc, m_fps, size)) {
        std::lock_guard<std::mutex> lock(activeMutex());
        activeFiles().removeAll(writing);
        if (error)
            *error = QStringLiteral("تعذر إنشاء ملف التسجيل في:\n%1").arg(QDir::toNativeSeparators(m_folder));
        return false;
    }
    QSaveFile target(writing + TargetSuffix);
    if (target.open(QIODevice::WriteOnly)) {
        target.write(finalPath.toUtf8());
        target.commit();
    }
    m_size = size;
    m_writingPath = writing;
    m_finalPath = finalPath;
    m_liveMs = -1;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_livePath = finalPath;
    return true;
}

void CameraRecorder::closeSegment()
{
    if (!m_writer.isOpened())
        return;
    m_writer.release();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_livePath.clear();
    }
    m_liveMs = -1;
    const QString from = std::exchange(m_writingPath, QString());
    const QString to = std::exchange(m_finalPath, QString());
    const auto finish = [&from] {
        QFile::remove(from + TargetSuffix);
        std::lock_guard<std::mutex> lock(activeMutex());
        activeFiles().removeAll(from);
    };
    if (m_written == 0) {
        QFile::remove(from);
        finish();
        return;
    }
    // إن تعذرت إعادة التسمية يبقى الملف باسمه اللاتيني، ولا يضيع التسجيل
    const QString saved = moveFile(from, to) ? to : from;
    finish();
    rememberFile(saved);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_files.append(saved);
    }
    emit fileSaved(saved);
}

void CameraRecorder::fail(const QString &error)
{
    if (!m_recording)
        return;
    m_recording = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }
    emit failed(error);
}

// ───────────── الإعدادات والحذف التلقائي ─────────────

CameraRecorder::Options CameraRecorder::savedOptions()
{
    const QSettings settings(settingsPath(), QSettings::IniFormat);
    Options options;
    options.motionOnly = settings.value(QStringLiteral("recordMotionOnly"), false).toBool();
    options.overlay = settings.value(QStringLiteral("recordOverlay"), false).toBool();
    options.fullRate = settings.value(QStringLiteral("recordFullRate"), false).toBool();
    return options;
}

void CameraRecorder::setSavedOptions(const Options &options)
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("recordMotionOnly"), options.motionOnly);
    settings.setValue(QStringLiteral("recordOverlay"), options.overlay);
    settings.setValue(QStringLiteral("recordFullRate"), options.fullRate);
}

int CameraRecorder::retentionDays()
{
    return std::max(0, QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("recordRetentionDays"), 0).toInt());
}

void CameraRecorder::setRetentionDays(int days)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("recordRetentionDays"), std::max(0, days));
}

void CameraRecorder::rememberFile(const QString &path)
{
    std::lock_guard<std::mutex> lock(manifestMutex());
    QStringList paths = readManifest();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!paths.contains(absolute)) {
        paths << absolute;
        writeManifest(paths);
    }
}

int CameraRecorder::recoverInterrupted()
{
    QStringList folders = {QDir::tempPath(), savedFolder(), writableFolder(savedFolder())};
    folders.removeDuplicates();
    // ملف تعذرت إعادة تسميته عند إغلاقه بقي باسمه اللاتيني وهو تسجيل محفوظ في القائمة، فلا يُمس
    QStringList saved;
    {
        std::lock_guard<std::mutex> lock(manifestMutex());
        saved = readManifest();
    }
    int recovered = 0;
    for (const QString &folder : std::as_const(folders)) {
        const QDir dir(folder);
        const QStringList names = dir.entryList({WritingPrefix + QLatin1Char('*'), RebuildPrefix + QLatin1Char('*')}, QDir::Files);
        for (const QString &name : names) {
            const QString path = dir.filePath(name);
            {
                std::lock_guard<std::mutex> lock(activeMutex());
                if (activeFiles().contains(path) || activeFiles().contains(path.chopped(TargetSuffix.size())))
                    continue;
            }
            if (saved.contains(QFileInfo(path).absoluteFilePath()))
                continue;
            if (name.startsWith(RebuildPrefix)) {
                QFile::remove(path);
                continue;
            }
            if (name.endsWith(TargetSuffix)) {
                // اسم نهائي بقي بلا ملفه (نُقل قبل أن يُحذف هذا)
                if (!QFile::exists(path.chopped(TargetSuffix.size())))
                    QFile::remove(path);
                continue;
            }
            // MP4 لم يُغلق لا يُشغَّل (فهرسه يُكتب عند الإغلاق)، فلا فائدة من إبقائه
            if (name.endsWith(QStringLiteral(".mp4"))) {
                QFile::remove(path);
                continue;
            }
            QFile target(path + TargetSuffix);
            if (!target.open(QIODevice::ReadOnly))
                continue;
            QString finalPath = QString::fromUtf8(target.readAll()).trimmed();
            target.close();
            if (QFileInfo(path).size() == 0) {
                QFile::remove(path);
                QFile::remove(target.fileName());
                continue;
            }
            if (finalPath.isEmpty() || !QDir().mkpath(QFileInfo(finalPath).absolutePath()))
                continue;
            const QString base = finalPath.chopped(QFileInfo(finalPath).suffix().size() + 1);
            for (int n = 2; QFile::exists(finalPath); ++n)
                finalPath = QStringLiteral("%1 (%2).mkv").arg(base).arg(n);
            // نهاية التسجيل في قائمة التسجيلات هي آخر تعديل للملف، وإعادة البناء والنسخ إلى قرص آخر يغيّرانه
            const QDateTime modified = QFileInfo(path).lastModified();
            rebuildRecording(path);
            if (!moveFile(path, finalPath))
                continue;
            QFile moved(finalPath);
            if (moved.open(QIODevice::ReadWrite))
                moved.setFileTime(modified, QFileDevice::FileModificationTime);
            moved.close();
            QFile::remove(target.fileName());
            rememberFile(finalPath);
            qInfo("استُعيد تسجيل انقطعت كتابته: %s", qUtf8Printable(QDir::toNativeSeparators(finalPath)));
            ++recovered;
        }
    }
    return recovered;
}

QString CameraRecorder::fileCameraName(const QString &cameraName)
{
    return safeName(cameraName);
}

QList<CameraRecorder::RecordingFile> CameraRecorder::recordings(const QDateTime &from, const QDateTime &to)
{
    QStringList paths;
    {
        std::lock_guard<std::mutex> lock(manifestMutex());
        paths = readManifest();
    }
    static const QRegularExpression pattern(QStringLiteral("^(.*) (\\d{4}-\\d{2}-\\d{2} \\d{2}-\\d{2}-\\d{2})(?: \\(\\d+\\))?$"));
    QList<RecordingFile> list;
    for (const QString &path : std::as_const(paths)) {
        const QFileInfo info(path);
        if (!info.exists())
            continue;
        const QRegularExpressionMatch match = pattern.match(info.completeBaseName());
        if (!match.hasMatch())
            continue;
        RecordingFile file;
        file.path = info.absoluteFilePath();
        file.camera = match.captured(1);
        file.start = QDateTime::fromString(match.captured(2), QStringLiteral("yyyy-MM-dd HH-mm-ss"));
        file.end = info.lastModified();
        if (!file.start.isValid())
            continue;
        if (file.end < file.start)
            file.end = file.start;
        if (file.end < from || file.start >= to)
            continue;
        list.append(file);
    }
    std::sort(list.begin(), list.end(), [](const RecordingFile &a, const RecordingFile &b) { return a.start < b.start; });
    return list;
}

int CameraRecorder::removeExpired()
{
    const int days = retentionDays();
    if (days <= 0)
        return 0;
    std::lock_guard<std::mutex> lock(manifestMutex());
    const QDateTime limit = QDateTime::currentDateTime().addDays(-days);
    const QStringList paths = readManifest();
    QStringList kept;
    int removed = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists())
            continue; // حذفه المستخدم أو نقله
        if (info.lastModified() < limit && QFile::remove(path)) {
            ++removed;
            continue;
        }
        kept << path;
    }
    if (kept != paths)
        writeManifest(kept);
    return removed + removeExpiredSnapshots(limit.date());
}

QString CameraRecorder::snapshotsFolder()
{
    return QDir(savedFolder()).filePath(QStringLiteral("لقطات الظهور"));
}

int CameraRecorder::removeExpiredSnapshots(const QDate &before)
{
    // لقطات كل يوم في مجلد باسم تاريخه؛ لا يُحذف إلا ما كتبه البرنامج (صور JPG) ثم المجلد إن فرغ
    QDir root(snapshotsFolder());
    if (!root.exists())
        return 0;
    int removed = 0;
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDate day = QDate::fromString(name, QStringLiteral("yyyy-MM-dd"));
        if (!day.isValid() || day >= before)
            continue;
        QDir folder(root.filePath(name));
        for (const QString &file : folder.entryList({QStringLiteral("*.jpg")}, QDir::Files)) {
            if (folder.remove(file))
                ++removed;
        }
        root.rmdir(name); // يفشل بصمت إن بقي فيه ملف للمستخدم
    }
    return removed;
}

bool CameraRecorder::freeSpace(const QString &folder, qint64 needBytes)
{
    QStorageInfo storage(folder);
    if (!storage.isValid() || storage.bytesAvailable() >= needBytes)
        return true;
    // دون حذف تلقائي لا يُحذف شيء دون علم المستخدم
    if (retentionDays() <= 0)
        return false;
    std::lock_guard<std::mutex> lock(manifestMutex());
    QStringList paths = readManifest();
    std::multimap<QDateTime, QString> oldest;
    for (const QString &path : std::as_const(paths)) {
        const QFileInfo info(path);
        if (info.exists() && QStorageInfo(path).rootPath() == storage.rootPath())
            oldest.emplace(info.lastModified(), path);
    }
    for (const auto &[time, path] : oldest) {
        storage.refresh();
        if (storage.bytesAvailable() >= needBytes)
            break;
        if (QFile::remove(path))
            paths.removeAll(path);
    }
    writeManifest(paths);
    storage.refresh();
    return storage.bytesAvailable() >= needBytes;
}

QString CameraRecorder::saveSnapshot(const QImage &frame, const QRectF &box, const QString &name)
{
    if (frame.isNull())
        return {};
    const QDateTime now = QDateTime::currentDateTime();
    const QString folder = QDir(snapshotsFolder()).filePath(now.toString(QStringLiteral("yyyy-MM-dd")));
    if (!QDir().mkpath(folder))
        return {};
    QString safe = name;
    for (QChar &c : safe) {
        if (QStringLiteral("\\/:*?\"<>|").contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    }
    QString path = QDir(folder).filePath(QStringLiteral("%1 %2.jpg").arg(safe.simplified(), now.toString(QStringLiteral("HH-mm-ss"))));
    for (int n = 2; QFile::exists(path); ++n)
        path = QDir(folder).filePath(QStringLiteral("%1 %2 (%3).jpg").arg(safe.simplified(), now.toString(QStringLiteral("HH-mm-ss"))).arg(n));
    // الإطار كاملاً مع مربع حول الشخص؛ الحفظ في الخلفية فلا يتأخر عرض الكاميرا
    QImage image = frame.convertToFormat(QImage::Format_RGB32);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Theme::Watched, std::max(2.0, image.height() / 300.0)));
        painter.drawRect(box);
    }
    (void)QtConcurrent::run([image, path] { image.save(path, "JPG", 88); });
    return path;
}

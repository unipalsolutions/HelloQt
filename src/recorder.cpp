#include "recorder.h"

#include "videofilters.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>

#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <utility>

namespace {

constexpr size_t MaxQueue = 90;                 // نحو 3 ثوانٍ؛ إن تأخرت الكتابة تُترك أقدم الإطارات
constexpr qint64 MinFreeBytes = 500ll << 20;    // يتوقف التسجيل قبل أن يمتلئ القرص
constexpr double MaxGapSeconds = 2.0;           // انقطاع أطول لا يُملأ بإطار متجمد

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

bool CameraRecorder::start(const QString &folder, const QString &cameraName, double fps, QString *error)
{
    stop();
    if (!QDir().mkpath(folder)) {
        if (error)
            *error = QStringLiteral("تعذر إنشاء مجلد التسجيلات:\n%1").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    const QStorageInfo storage(folder);
    if (storage.isValid() && storage.bytesAvailable() < MinFreeBytes) {
        if (error)
            *error = QStringLiteral("لا توجد مساحة كافية على القرص لتسجيل الكاميرا (أقل من 500 ميغابايت)");
        return false;
    }
    m_folder = folder;
    m_cameraName = safeName(cameraName);
    m_fps = fps > 1.0 && fps <= 60.0 ? fps : 25.0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        m_files.clear();
        m_stop = false;
    }
    m_startedAt = Clock::now();
    m_spaceChecked = m_startedAt;
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

void CameraRecorder::push(const cv::Mat &frame)
{
    if (!m_recording || frame.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop)
            return;
        if (m_queue.size() >= MaxQueue)
            m_queue.pop_front();
        // الإطار لا يُنسخ: خيط القراءة يقرأ كل إطار في ذاكرة جديدة، والمعالجات لا تكتب في الأصل
        m_queue.push_back({frame, Clock::now()});
    }
    m_wake.notify_one();
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
        if (m_recording || m_stop)
            write(item);
        lock.lock();
    }
    lock.unlock();
    closeSegment();
}

void CameraRecorder::write(const Pending &item)
{
    if (!m_recording && !m_writer.isOpened())
        return; // فشل سابق
    cv::Mat frame = item.frame;
    const VideoFilters::Settings filters = VideoFilters::current();
    if (!filters.isIdentity())
        frame = VideoFilters::apply(frame, filters);
    if (frame.channels() == 1)
        cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
    else if (frame.channels() == 4)
        cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
    // أبعاد فردية لا تقبلها بعض مشغلات MPEG-4
    if ((frame.cols % 2) || (frame.rows % 2))
        frame = frame(cv::Rect(0, 0, frame.cols & ~1, frame.rows & ~1));
    if (frame.cols < 2 || frame.rows < 2)
        return;

    // ملف جديد كل SegmentMinutes دقيقة، أو إذا تغيرت أبعاد الإطار (تدوير الكاميرا من المعالجات)
    if (m_writer.isOpened()
        && (frame.size() != m_size || item.time - m_segmentStart >= std::chrono::minutes(SegmentMinutes)))
        closeSegment();

    if (item.time - m_spaceChecked >= std::chrono::seconds(30)) {
        m_spaceChecked = item.time;
        const QStorageInfo storage(m_folder);
        if (storage.isValid() && storage.bytesAvailable() < MinFreeBytes) {
            closeSegment();
            fail(QStringLiteral("توقف التسجيل لأن القرص كاد يمتلئ؛ حُفظ ما سُجِّل حتى الآن"));
            return;
        }
    }

    if (!m_writer.isOpened()) {
        QString error;
        if (!openSegment(frame.size(), &error)) {
            fail(error);
            return;
        }
        m_segmentStart = m_timeBase = item.time;
        m_written = 0;
    }

    // عدد الإطارات يتبع الوقت الفعلي، فمدة التسجيل صحيحة ولو اختلفت سرعة الكاميرا عن المعلنة:
    // يُكرر الإطار إن تأخرت الكاميرا، ويُترك إن سبقت
    const double seconds = std::chrono::duration<double>(item.time - m_timeBase).count();
    qint64 target = qint64(seconds * m_fps) + 1;
    if (double(target - m_written) > MaxGapSeconds * m_fps) {
        // عادت الكاميرا بعد انقطاع: يكمل التسجيل من حيث توقف دون ملء الفترة بصورة متجمدة
        m_timeBase = item.time - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(m_written / m_fps));
        target = m_written + 1;
    }
    while (m_written < target) {
        m_writer.write(frame);
        ++m_written;
    }
}

bool CameraRecorder::openSegment(const cv::Size &size, QString *error)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"));
    QString finalPath = QDir(m_folder).filePath(QStringLiteral("%1 %2.mp4").arg(m_cameraName, stamp));
    for (int n = 2; QFile::exists(finalPath); ++n)
        finalPath = QDir(m_folder).filePath(QStringLiteral("%1 %2 (%3).mp4").arg(m_cameraName, stamp).arg(n));

    const QString ascii = QStringLiteral("malamih-rec-%1.mp4").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    const QString writing = QDir(writableFolder(m_folder)).filePath(ascii);
    const std::string native = QDir::toNativeSeparators(writing).toStdString();
    const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
    if (!m_writer.open(native, cv::CAP_FFMPEG, fourcc, m_fps, size) && !m_writer.open(native, fourcc, m_fps, size)) {
        if (error)
            *error = QStringLiteral("تعذر إنشاء ملف التسجيل في:\n%1").arg(QDir::toNativeSeparators(m_folder));
        return false;
    }
    m_size = size;
    m_writingPath = writing;
    m_finalPath = finalPath;
    return true;
}

void CameraRecorder::closeSegment()
{
    if (!m_writer.isOpened())
        return;
    m_writer.release();
    const QString from = std::exchange(m_writingPath, QString());
    const QString to = std::exchange(m_finalPath, QString());
    if (m_written == 0) {
        QFile::remove(from);
        return;
    }
    // إن تعذرت إعادة التسمية يبقى الملف باسمه اللاتيني، ولا يضيع التسجيل
    const QString saved = moveFile(from, to) ? to : from;
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

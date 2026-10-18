#include "videoplayer.h"

#include "cameras.h"
#include "imageio.h"
#include "scenecut.h"
#include "videofilters.h"

#include <QElapsedTimer>
#include <QLocale>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace {

constexpr int MaxDisplaySide = 1920; // القاعدة نفسها في VideoWorker، فتنطبق مواضع الوجوه
constexpr int MaxPendingFrames = 2;
constexpr int MaxLateGrabs = 4;      // إطارات متأخرة تُتخطى دون تحويلها قبل عرض واحد منها
constexpr double MaxLagMs = 1000.0;  // تأخر أكبر من هذا (جهاز لا يلحق بفك الترميز) يُبطئ التشغيل بدل القفز

} // namespace

VideoPlayer::VideoPlayer(QObject *parent)
    : QObject(parent)
{
}

VideoPlayer::~VideoPlayer()
{
    close();
}

QString VideoPlayer::analysisLagText(qint64 lagMs, bool following, bool playing)
{
    // أبعد من دقيقة: الأرجح أن المستخدم انتقل إلى جزء لم يصله التحليل، لا أن التحليل يتأخر
    QString text = lagMs >= 0 && lagMs <= 60000
        ? QStringLiteral("التحليل متأخر %1 ث عن العرض").arg(QLocale().toString(std::max<qint64>(1, (lagMs + 500) / 1000)))
        : QStringLiteral("هذا الجزء لم يُحلَّل بعد");
    if (!playing)
        text += QStringLiteral(" · التحليل يتابع والعرض متوقف");
    else if (following)
        text += QStringLiteral(" · يلحق بالعرض");
    else
        text += QStringLiteral(" · المربعات تظهر حين يصله");
    return text;
}

QImage VideoPlayer::displayImage(const cv::Mat &frame, const QSize &fit, QImage *fitted)
{
    if (frame.empty())
        return {};
    const VideoFilters::Settings filters = VideoFilters::current();
    cv::Mat filtered = filters.isIdentity() ? frame : VideoFilters::apply(frame, filters);
    const double scale = std::min(1.0, double(MaxDisplaySide) / std::max(filtered.cols, filtered.rows));
    if (scale < 1.0)
        filtered = ImageIO::shrink(filtered, cv::Size(std::max(1, cvRound(filtered.cols * scale)), std::max(1, cvRound(filtered.rows * scale))));
    // BGRA بترتيب ذاكرة RGB32 نفسه، فتعرضه الواجهة دون تحويل أو نسخ آخر
    auto wrap = [](const cv::Mat &bgr) {
        auto *holder = new cv::Mat;
        cv::cvtColor(bgr, *holder, cv::COLOR_BGR2BGRA);
        return QImage(holder->data, holder->cols, holder->rows, int(holder->step), QImage::Format_RGB32,
                      [](void *data) { delete static_cast<cv::Mat *>(data); }, holder);
    };
    if (fitted) {
        *fitted = QImage();
        if (fit.width() > 0 && fit.height() > 0 && fit != QSize(filtered.cols, filtered.rows)) {
            // متوسط المساحة بنسبة صحيحة (سريع، ويمنع التشوه كالتصغير الناعم في الواجهة) ثم خطي إلى الحجم بالضبط؛
            // متوسط المساحة بنسبة كسرية أبطأ من تصغير الواجهة نفسه بثلاث مرات
            const int factor = std::min(filtered.cols / fit.width(), filtered.rows / fit.height());
            cv::Mat resized = filtered;
            if (factor >= 2)
                cv::resize(filtered, resized, cv::Size(filtered.cols / factor, filtered.rows / factor), 0, 0, cv::INTER_AREA);
            if (resized.cols != fit.width() || resized.rows != fit.height())
                cv::resize(resized, resized, cv::Size(fit.width(), fit.height()), 0, 0, cv::INTER_LINEAR);
            *fitted = wrap(resized);
        }
    }
    return wrap(filtered);
}

void VideoPlayer::setFitSize(const QSize &size)
{
    // أحجام معقولة فقط؛ غيرها (تكبير شديد أو لوحة مخفية) يُترك تصغيره للواجهة
    const bool valid = size.width() > 0 && size.height() > 0 && size.width() <= 4096 && size.height() <= 4096;
    m_fitSize = valid ? quint32(size.width()) << 16 | quint32(size.height()) : 0;
}

bool VideoPlayer::openFile(const QString &path, qint64 startMs)
{
    close();
    auto capture = std::make_shared<cv::VideoCapture>();
    if (!ImageIO::openVideo(*capture, path))
        return false;
    const double fps = capture->get(cv::CAP_PROP_FPS);
    m_fps = fps > 1.0 && fps < 1000.0 ? fps : 25.0;
    const double frames = capture->get(cv::CAP_PROP_FRAME_COUNT);
    m_durationMs = frames > 0 ? qint64(frames * 1000.0 / m_fps) : 0;
    m_path = path;
    m_live = false;
    m_stop = false;
    m_playing = false;
    m_pending = 0;
    m_position = std::max<qint64>(0, startMs);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seekMs = std::max<qint64>(0, startMs);
        m_stopAtMs = 0;
        m_restartClock = true;
    }
    m_thread = std::thread([this, capture] { runFile(*capture); });
    return true;
}

void VideoPlayer::openLive(std::shared_ptr<Cameras::FrameSlot> slot)
{
    close();
    if (!slot)
        return;
    m_slot = std::move(slot);
    m_live = true;
    m_stop = false;
    m_playing = true;
    m_pending = 0;
    m_position = 0;
    m_durationMs = 0;
    m_thread = std::thread([this] { runLive(); });
}

void VideoPlayer::close()
{
    m_stop = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wake.notify_all();
    }
    if (m_slot)
        m_slot->close();
    if (m_thread.joinable())
        m_thread.join();
    m_slot.reset();
    m_playing = false;
    m_live = false;
    m_path.clear();
}

void VideoPlayer::play()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_playing = true;
    m_restartClock = true;
    m_wake.notify_all();
}

void VideoPlayer::pause()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_playing = false;
    m_wake.notify_all();
}

void VideoPlayer::seek(qint64 positionMs)
{
    if (m_live)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_seekMs = std::max<qint64>(0, positionMs);
    m_position = m_seekMs;
    m_wake.notify_all();
}

void VideoPlayer::stepFrames(int frames)
{
    if (m_live || frames == 0)
        return;
    const double interval = 1000.0 / m_fps;
    const qint64 current = std::llround(double(m_position) / interval);
    qint64 target = std::max<qint64>(0, current + frames);
    if (m_durationMs > 0)
        target = std::min<qint64>(target, qint64(double(m_durationMs) / interval) - 1);
    pause();
    seek(qint64(std::ceil(double(target) * interval)));
}

void VideoPlayer::setRate(double rate)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rate = std::clamp(rate, 0.1, 16.0);
    m_restartClock = true;
    m_wake.notify_all();
}

void VideoPlayer::setStopAt(qint64 positionMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopAtMs = std::max<qint64>(0, positionMs);
}

void VideoPlayer::frameShown()
{
    // إطارات أُرسلت قبل إغلاق سابق قد تصل بعده، فلا ينزل العدد تحت الصفر
    int pending = m_pending.load();
    while (pending > 0 && !m_pending.compare_exchange_weak(pending, pending - 1)) {
    }
}

void VideoPlayer::deliver(const cv::Mat &frame, qint64 positionMs, bool force)
{
    if (frame.empty() || (!force && m_pending >= MaxPendingFrames))
        return;
    ++m_pending;
    m_position = positionMs;
    const quint32 fit = m_fitSize;
    QImage fitted;
    const QImage image = displayImage(frame, QSize(int(fit >> 16), int(fit & 0xffff)), &fitted);
    emit frameReady(image, positionMs, fitted);
}

void VideoPlayer::runFile(cv::VideoCapture &capture)
{
    using Clock = std::chrono::steady_clock;
    const double interval = 1000.0 / m_fps;
    // موضع الإطار = رقمه × مدة الإطار، بالحساب نفسه في التحليل فتتطابق المواضع
    qint64 next = 0; // رقم الإطار التالي
    Clock::time_point anchor = Clock::now();
    double anchorMs = 0.0; // موضع الإطار التالي لحظة anchor
    int lateGrabs = 0;
    cv::Mat frame;
    SceneCutDetector sceneCuts;
    auto elapsedMs = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - anchor).count(); };

    while (!m_stop) {
        qint64 seekMs = -1;
        qint64 stopAt = 0;
        bool restart = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || m_seekMs >= 0 || m_playing; });
            if (m_stop)
                break;
            seekMs = std::exchange(m_seekMs, -1);
            stopAt = m_stopAtMs;
            restart = std::exchange(m_restartClock, false);
        }
        if (seekMs >= 0) {
            // القفز برقم الإطار لا بالزمن، فيصل إلى الإطار نفسه الذي حلله التحليل عند هذا الموضع
            next = qint64(std::floor(double(seekMs) / interval + 1e-6));
            capture.set(cv::CAP_PROP_POS_FRAMES, double(next));
            // بعد القفز يُقارن بالإطار الجديد لا بإطار من موضع آخر
            sceneCuts.reset();
            if (capture.read(frame)) {
                if (frame.channels() == 3)
                    sceneCuts.update(frame);
                deliver(frame, qint64(double(next) * interval), true);
                ++next;
            }
            restart = true;
        }
        if (restart) {
            anchor = Clock::now();
            anchorMs = double(next) * interval;
            lateGrabs = 0;
        }
        if (seekMs >= 0 || !m_playing)
            continue;

        const double nextMs = double(next) * interval;
        if (stopAt > 0 && nextMs >= double(stopAt)) {
            m_playing = false;
            emit reachedEnd();
            continue;
        }
        const double rate = m_rate;
        const double target = anchorMs + elapsedMs() * rate;
        if (nextMs > target) {
            // لم يحن موعده: انتظار يقطعه الإيقاف أو القفز
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::microseconds(qint64(std::min((nextMs - target) / rate, 50.0) * 1000.0)),
                            [&] { return m_stop || m_seekMs >= 0 || !m_playing || m_restartClock; });
            continue;
        }
        if (target - nextMs > MaxLagMs * std::max(1.0, rate)) {
            anchor = Clock::now();
            anchorMs = nextMs;
        }
        if (target - nextMs > interval * 2.0 && lateGrabs < MaxLateGrabs) {
            // متأخر: الإطار يُقرأ دون تحويله إلى صورة
            ++lateGrabs;
            if (!capture.grab()) {
                m_playing = false;
                emit reachedEnd();
                continue;
            }
            ++next;
            continue;
        }
        lateGrabs = 0;
        if (!capture.read(frame)) {
            m_playing = false;
            emit reachedEnd();
            continue;
        }
        if (frame.channels() == 3 && sceneCuts.update(frame))
            emit fileSceneCut(qint64(nextMs));
        deliver(frame, qint64(nextMs), false);
        ++next;
    }
}

void VideoPlayer::runLive()
{
    QElapsedTimer clock;
    clock.start();
    quint64 seen = 0;
    cv::Mat frame;
    SceneCutDetector sceneCuts;
    while (!m_stop) {
        if (!m_slot->wait(seen, frame, 100))
            continue;
        if (!m_playing)
            continue;
        // مربعات المشهد السابق لا تبقى على المشهد الجديد حتى يصل تحليله
        if (frame.channels() == 3 && sceneCuts.update(frame))
            emit liveSceneCut();
        deliver(frame, clock.elapsed(), false);
    }
}

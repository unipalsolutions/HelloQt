#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <opencv2/core.hpp>

namespace Cameras {
class FrameSlot;
}
namespace cv {
class VideoCapture;
}

// يعرض الفيديو أو البث بسرعته الأصلية في خيط خاص، منفصلاً عن التحليل: التحليل يعمل في خيطه
// على ما يستطيع من الإطارات، والعرض يبقى سلساً مهما أبطأ. الإطارات تمر بمعالجات الفيديو نفسها
// وتُصغَّر بالقاعدة نفسها التي يحسب بها التحليل مواضع الوجوه، فتنطبق المربعات عليها
class VideoPlayer : public QObject
{
    Q_OBJECT

public:
    // أبعد فجوة بين إطار معروض وأقرب إطار محلل قبله تُعرض فيها نتيجته
    static constexpr qint64 AnalysisToleranceMs = 600;

    explicit VideoPlayer(QObject *parent = nullptr);
    ~VideoPlayer() override;

    // يفتح ملفاً ويعرض أول إطار عند startMs متوقفاً
    bool openFile(const QString &path, qint64 startMs);
    // يعرض كل إطار تنشره الكاميرا في slot
    void openLive(std::shared_ptr<Cameras::FrameSlot> slot);
    void close();
    bool isOpen() const { return m_thread.joinable(); }
    bool isLive() const { return m_live; }

    void play();
    void pause();
    bool isPlaying() const { return m_playing; }
    void seek(qint64 positionMs);
    // يتقدم أو يرجع عدداً من الإطارات متوقفاً
    void stepFrames(int frames);
    // سرعة التشغيل (1 = الأصلية)؛ التحليل لا يتأثر بها
    void setRate(double rate);
    double rate() const { return m_rate; }
    // يتوقف التشغيل عند هذا الموضع (نهاية الجزء المطلوب)؛ 0 = حتى نهاية الملف
    void setStopAt(qint64 positionMs);
    double fps() const { return m_fps; }
    qint64 durationMs() const { return m_durationMs; }
    qint64 position() const { return m_position; }
    // يُستدعى بعد عرض كل إطار، فلا تتراكم الإطارات إن انشغلت الواجهة
    void frameShown();

    // نص شارة الإطار المعروض حين لا نتيجة تحليل له: lagMs بعده عن آخر إطار محلل قبله (سالب = لا شيء قبله)،
    // و following أن التحليل يقفز إليه، و playing أن العرض يتقدم
    static QString analysisLagText(qint64 lagMs, bool following, bool playing);

    // تحويل إطار للعرض: المعالجات ثم التصغير، بصورة تشير إلى بيانات الإطار دون نسخها
    static QImage displayImage(const cv::Mat &frame);

signals:
    void frameReady(const QImage &frame, qint64 positionMs);
    void reachedEnd();
    // البث المباشر: تبدّل المشهد في الإطار المعروض، قبل أن يصل تحليله
    void liveSceneCut();

private:
    void runFile(cv::VideoCapture &capture);
    void runLive();
    void deliver(const cv::Mat &frame, qint64 positionMs, bool force);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_playing{false};
    std::atomic<int> m_pending{0};
    std::atomic<qint64> m_position{0};
    std::atomic<double> m_rate{1.0};
    bool m_live = false;
    QString m_path;
    std::shared_ptr<Cameras::FrameSlot> m_slot;
    double m_fps = 25.0;
    qint64 m_durationMs = 0;
    // يحميها m_mutex
    qint64 m_seekMs = -1;
    qint64 m_stopAtMs = 0;
    bool m_restartClock = false;
};

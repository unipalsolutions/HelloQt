#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// يسجّل بث الكاميرا إلى ملفات MP4: كل إطار يصل من الكاميرا، لا الإطارات التي حُلِّلت فقط، فيستمر
// التسجيل بسرعته الكاملة ولو كان التحليل أبطأ أو متوقفاً مؤقتاً. معالجات الفيديو (التدوير والإضاءة…)
// تُطبَّق على المسجَّل كما تظهر، دون مربعات الوجوه، فيمكن تحليل التسجيل لاحقاً كأي فيديو.
// التسجيل الطويل يُقسَّم ملفات كل SegmentMinutes دقيقة، فلا يضيع إلا الجزء الأخير إن انطفأ الجهاز فجأة
class CameraRecorder : public QObject
{
    Q_OBJECT

public:
    static constexpr int SegmentMinutes = 15;

    explicit CameraRecorder(QObject *parent = nullptr);
    ~CameraRecorder() override;

    // fps سرعة الكاميرا المعلنة (0 = غير معروفة فتُعتبر 25)؛ المدة تُضبط على الوقت الفعلي على أي حال
    bool start(const QString &folder, const QString &cameraName, double fps, QString *error);
    // يغلق الملف الحالي وينتظر حفظه
    void stop();
    bool isRecording() const { return m_recording; }
    // مدة التسجيل الحالي بالمللي ثانية
    qint64 elapsedMs() const;
    // ملفات التسجيل الأخير التي حُفظت
    QStringList files() const;

    // من خيط قراءة الكاميرا؛ لا ينتظر، ولا يفعل شيئاً إن لم يكن التسجيل جارياً
    void push(const cv::Mat &frame);

    static QString defaultFolder();
    static QString savedFolder();
    static void setSavedFolder(const QString &folder);
    static bool autoRecord();
    static void setAutoRecord(bool enabled);

signals:
    void fileSaved(const QString &path);
    // توقف التسجيل من تلقاء نفسه (امتلأ القرص أو تعذرت الكتابة)
    void failed(const QString &error);

private:
    using Clock = std::chrono::steady_clock;
    struct Pending
    {
        cv::Mat frame;
        Clock::time_point time;
    };

    void run();
    bool openSegment(const cv::Size &size, QString *error);
    void closeSegment();
    void write(const Pending &item);
    void fail(const QString &error);

    QString m_folder;
    QString m_cameraName;
    double m_fps = 25.0;
    std::atomic<bool> m_recording{false};
    Clock::time_point m_startedAt;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Pending> m_queue;
    bool m_stop = false;
    QStringList m_files;

    // يخص خيط الكتابة وحده
    cv::VideoWriter m_writer;
    cv::Size m_size;
    QString m_writingPath; // ما تكتب إليه OpenCV (اسم لاتيني)
    QString m_finalPath;   // الاسم النهائي بعد الإغلاق
    Clock::time_point m_segmentStart; // لتقسيم الملفات
    Clock::time_point m_timeBase;     // زمن الإطار الأول في الملف، بعد طرح فترات الانقطاع
    qint64 m_written = 0;
    Clock::time_point m_spaceChecked;
};

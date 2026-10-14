#pragma once

#include <QColor>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QRectF>
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
// تُطبَّق على المسجَّل كما تظهر، ومربعات الوجوه وأسماؤها اختيارية. التسجيل الطويل يُقسَّم ملفات كل
// SegmentMinutes دقيقة، فلا يضيع إلا الجزء الأخير إن انطفأ الجهاز فجأة
class CameraRecorder : public QObject
{
    Q_OBJECT

public:
    static constexpr int SegmentMinutes = 15;

    struct Options
    {
        // يُسجَّل عند الحركة فقط (مع ثوانٍ قبلها وبعدها)، وكل حدث في ملف إن طال الهدوء بينهما
        bool motionOnly = false;
        // مربعات الوجوه وأسماؤها تُرسم في التسجيل (فلا يصلح التسجيل حينها لإعادة التحليل بدقة)
        bool overlay = false;
    };

    // مربع وجه بإحداثيات نسبية من 0 إلى 1 من الإطار بعد المعالجات
    struct OverlayBox
    {
        QRectF box;
        QString label;
        QColor color;
    };

    // موضع الإطار الأخير المكتوب في الملف الجاري (للربط بين ظهور شخص وموضعه في التسجيل)
    struct Position
    {
        QString path; // الاسم النهائي للملف بعد إغلاقه؛ فارغ إن لم يُفتح ملف بعد
        qint64 ms = -1;
    };

    explicit CameraRecorder(QObject *parent = nullptr);
    ~CameraRecorder() override;

    // fps سرعة الكاميرا المعلنة (0 = غير معروفة فتُعتبر 25)؛ المدة تُضبط على الوقت الفعلي على أي حال
    bool start(const QString &folder, const QString &cameraName, double fps, const Options &options, QString *error);
    // يغلق الملف الحالي وينتظر حفظه
    void stop();
    bool isRecording() const { return m_recording; }
    // في وضع الحركة فقط: لا حركة الآن فلا يُكتب شيء
    bool isWaitingForMotion() const { return m_recording && m_waiting; }
    // مدة التسجيل الحالي بالمللي ثانية
    qint64 elapsedMs() const;
    // ملفات التسجيل الأخير التي حُفظت
    QStringList files() const;
    Position position() const;

    // من خيط قراءة الكاميرا؛ لا ينتظر، ولا يفعل شيئاً إن لم يكن التسجيل جارياً
    void push(const cv::Mat &frame);
    // من الواجهة: مربعات الوجوه في آخر إطار حُلِّل. وجود وجه يُعد حركة في وضع الحركة فقط
    void setOverlay(const QList<OverlayBox> &boxes);

    static QString defaultFolder();
    static QString savedFolder();
    static void setSavedFolder(const QString &folder);
    static bool autoRecord();
    static void setAutoRecord(bool enabled);
    static Options savedOptions();
    static void setSavedOptions(const Options &options);
    // التسجيلات الأقدم من هذا العدد من الأيام تُحذف تلقائياً (0 = لا تُحذف)
    static int retentionDays();
    static void setRetentionDays(int days);
    // يحذف تسجيلات البرنامج (لا غيرها من ملفات المجلد) الأقدم من retentionDays؛ يعيد عدد المحذوف
    static int removeExpired();
    // يحذف أقدم تسجيلات البرنامج حتى تتوفر needBytes في قرص المجلد؛ يعيد هل توفرت
    static bool freeSpace(const QString &folder, qint64 needBytes);
    // كل ملف حفظه البرنامج يُضاف إلى قائمته، فالحذف التلقائي لا يمس ملفات أخرى في المجلد
    static void rememberFile(const QString &path);

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
    void handle(const Pending &item);
    void write(const cv::Mat &frame, Clock::time_point time);
    cv::Mat prepare(const Pending &item);
    bool detectMotion(const cv::Mat &frame, Clock::time_point time);
    void drawOverlay(cv::Mat &frame, Clock::time_point time);
    void fail(const QString &error);

    QString m_folder;
    QString m_cameraName;
    double m_fps = 25.0;
    Options m_options;
    std::atomic<bool> m_recording{false};
    std::atomic<bool> m_waiting{false};
    Clock::time_point m_startedAt;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Pending> m_queue;
    std::atomic<bool> m_stop{false};
    QStringList m_files;
    QString m_livePath;   // يحميه m_mutex
    std::atomic<qint64> m_liveMs{-1};
    QList<OverlayBox> m_overlay;          // يحميه m_mutex
    Clock::time_point m_overlayTime;      // يحميه m_mutex
    std::atomic<Clock::rep> m_activity{0}; // آخر ظهور وجه (زمن Clock) يُعد حركة

    // يخص خيط الكتابة وحده
    cv::VideoWriter m_writer;
    cv::Size m_size;
    QString m_writingPath; // ما تكتب إليه OpenCV (اسم لاتيني)
    QString m_finalPath;   // الاسم النهائي بعد الإغلاق
    Clock::time_point m_segmentStart; // لتقسيم الملفات
    Clock::time_point m_timeBase;     // زمن الإطار الأول في الملف، بعد طرح فترات الانقطاع
    qint64 m_written = 0;
    Clock::time_point m_spaceChecked;
    Clock::time_point m_cleaned;
    // وضع الحركة فقط
    std::deque<Pending> m_preRoll; // آخر ثوانٍ قبل الحركة، تُكتب حين تبدأ
    cv::Mat m_motionReference;
    Clock::time_point m_motionReferenceTime;
    Clock::time_point m_lastMotion;
    bool m_hadMotion = false;
};

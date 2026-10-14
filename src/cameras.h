#pragma once

#include "videofilters.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class CameraRecorder;

// الكاميرات: ما يتصل بالجهاز مباشرة (USB، محوّل الكاميرات التناظرية، بطاقة التقاط HDMI) يظهر
// للنظام كاميرا برقم، وكاميرات الشبكة وأجهزة التسجيل تُفتح بعنوان بث RTSP أو HTTP
namespace Cameras {

enum class Kind { Local, Network };

// طريقة الالتقاط على Windows؛ بعض بطاقات الالتقاط لا تعمل إلا بإحداها
enum class Backend { Auto, DirectShow, MediaFoundation };

// صيغة عنوان البث لشركة؛ {host} {port} {ch} {ch2} (رقمان) {path} و {s0} {s1} {ms} للبث الرئيسي أو الفرعي
struct Brand
{
    QString key;
    QString name;
    QString pattern; // فارغ = عنوان يكتبه المستخدم كاملاً
    int port = 554;
    bool channels = false; // أجهزة التسجيل والكاميرات متعددة القنوات
    bool streams = false;  // بث رئيسي بدقة كاملة وفرعي بدقة أقل
    QString hint;
};
const QList<Brand> &brands();
const Brand &brand(const QString &key);

// طريقة توصيل الكاميرا كما يفهمها المستخدم؛ تحدد نوعها وخطوات الشرح المناسبة لها
struct Connection
{
    QString key;
    QString title;
    QString description;
    Kind kind = Kind::Local;
    QString brand; // نوع الكاميرا الافتراضي لكاميرات الشبكة
};
const QList<Connection> &connections();
const Connection &connection(const QString &key);
// خطوات التوصيل بلغة بسيطة (HTML)
QString steps(const QString &key);

struct Camera
{
    QString id; // فارغ لكاميرا موصولة غير محفوظة
    QString name;
    Kind kind = Kind::Local;
    QString via; // مفتاح Connection؛ فارغ للكاميرات المحفوظة قبل إضافته

    // كاميرا موصولة بالجهاز
    int index = 0;
    Backend backend = Backend::Auto;
    int width = 0; // 0 = دقة الكاميرا الافتراضية
    int height = 0;

    // كاميرا شبكة
    QString brand = QStringLiteral("hikvision");
    QString host;
    int port = 554;
    int channel = 1;
    bool substream = false;
    QString path; // المسار أو العنوان الكامل للصيغ العامة
    QString user;
    QString password;

    bool hasFilters = false; // معالجات الفيديو محفوظة مع هذه الكاميرا
    VideoFilters::Settings filters;

    // مصدر التحليل: "camera:<id>" أو "camera:<index>"
    QString source() const;
    // عنوان البث؛ withCredentials = false يخفي كلمة المرور للعرض
    QString url(bool withCredentials = true) const;
    QString description() const;
};

QList<Camera> load();
void save(const QList<Camera> &cameras);
QString newId();
// الكاميرا لمصدر بعد "camera:" (رقم كاميرا موصولة، أو معرّف كاميرا محفوظة)؛ false إن لم توجد
bool resolve(const QString &key, Camera *camera);
// اسم الكاميرا للعرض من مصدرها "camera:…" دون البحث عن الأجهزة الموصولة (للسجلات والقوائم)
QString displayName(const QString &source);
void saveFilters(const QString &id, const VideoFilters::Settings &filters);
QString lastSource();
void setLastSource(const QString &source);

struct LocalDevice
{
    int index = 0;
    QString name;
};
// الكاميرات الموصولة بالجهاز بأسمائها (Windows و Linux)؛ قد تكون فارغة إن تعذرت القراءة
QList<LocalDevice> localDevices();
QString localName(int index, const QList<LocalDevice> &devices);

// عناوين IPv4 لهذا الجهاز، لضبط الكاميرا الموصولة بسلك مباشر على النطاق نفسه
QStringList localAddresses();
// هل العنوان على إحدى شبكات هذا الجهاز؛ true لاسم نطاق أو عنوان لا يُفحص
bool onLocalNetwork(const QString &host);
// عنوان يُعطى للابتوب ليصل إلى كاميرا موصولة بسلك مباشر، مثل 192.168.1.200 للكاميرا 192.168.1.64
QString suggestedAddress(const QString &cameraHost);

// كاميرا وُجدت في الشبكة
struct Found
{
    QString host;
    QString brand; // مفتاح Brand إن عُرفت الشركة، وإلا "rtsp"
    QString model; // الاسم أو الطراز كما تعلنه الكاميرا
    int port = 554;
    bool sameNetwork = true; // false: تحتاج ضبط عنوان اللابتوب أولاً
};
// يبحث بـ ONVIF ثم بفحص منافذ البث في شبكة اللابتوب؛ يستغرق ثواني ويُستدعى خارج خيط الواجهة
QList<Found> discover();
// يجرّب صيغ الشركات المعروفة على عنوان الكاميرا ومستخدمها حتى تعطي صورة، ويضبط camera عليها
bool detect(Camera &camera, cv::Mat *frame, QString *error, const std::function<void(const QString &)> &progress = {});

// كلمة المرور لا تُحفظ نصاً مكشوفاً (DPAPI على Windows)
QString protect(const QString &secret);
QString unprotect(const QString &stored);

bool open(cv::VideoCapture &capture, const Camera &camera, QString *error);
// سبب الفشل بالعربية: لا يرد جهاز على العنوان، أم يرد ويرفض البث
QString diagnose(const Camera &camera);
// يجرّب كل كاميرا موصولة بكل طريقة التقاط ويصف النتيجة سطراً لكل محاولة (لتشخيص كاميرا لا تفتح)
QString probeLocal();
// أذونات الخصوصية في ويندوز تمنع برامج سطح المكتب من الكاميرا
bool cameraBlocked();
// صورة واحدة للاختبار؛ info مثل «1280×720 · 25 إطاراً/ث»
cv::Mat snapshot(const Camera &camera, QString *error, QString *info = nullptr);
// يحوّل إطار الكاميرا إلى BGR بثماني بتات (رمادي، أربع قنوات، YUY2، 16 بت)، false إن تعذر
bool normalizeFrame(cv::Mat &frame);

// يقرأ الكاميرا في خيط خاص ويحتفظ بأحدث إطار فقط، فلا يتأخر التحليل عن البث الحي
// ولا تتراكم الإطارات عندما يكون التحليل أبطأ من الكاميرا
class LiveReader
{
public:
    // recorder (اختياري) يتسلم كل إطار يصل من الكاميرا، لا ما يُحلَّل فقط
    explicit LiveReader(const Camera &camera, CameraRecorder *recorder = nullptr);
    ~LiveReader();

    bool open(QString *error);
    // ينتظر إطاراً أحدث من آخر ما أُعطي؛ false إذا انقطعت الكاميرا
    bool read(cv::Mat &frame, int timeoutMs = 12000);
    double fps() const { return m_fps; }
    // انقطعت الكاميرا (لا يصل منها شيء بعد الآن)
    bool failed();

private:
    void run();
    void loop();

    Camera m_camera;
    CameraRecorder *m_recorder = nullptr;
    cv::VideoCapture m_capture;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    cv::Mat m_frame;
    quint64 m_sequence = 0;
    quint64 m_taken = 0;
    bool m_failed = false;
    std::atomic<bool> m_stop{false};
    double m_fps = 0.0;
};

} // namespace Cameras

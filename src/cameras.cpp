#include "cameras.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <chrono>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#include <dshow.h>
#endif

namespace Cameras {

namespace {

constexpr int OpenTimeoutMs = 8000;
constexpr int ReadTimeoutMs = 8000;

QString streamValue(const QString &pattern, const Camera &c)
{
    QString url = pattern;
    url.replace(QStringLiteral("{host}"), c.host.trimmed());
    url.replace(QStringLiteral("{port}"), QString::number(c.port));
    url.replace(QStringLiteral("{ch2}"), QStringLiteral("%1").arg(std::max(1, c.channel), 2, 10, QLatin1Char('0')));
    url.replace(QStringLiteral("{ch}"), QString::number(std::max(1, c.channel)));
    url.replace(QStringLiteral("{s0}"), c.substream ? QStringLiteral("1") : QStringLiteral("0"));
    url.replace(QStringLiteral("{s1}"), c.substream ? QStringLiteral("2") : QStringLiteral("1"));
    url.replace(QStringLiteral("{ms}"), c.substream ? QStringLiteral("sub") : QStringLiteral("main"));
    QString path = c.path.trimmed();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    url.replace(QStringLiteral("{path}"), path);
    return url;
}

QByteArray obfuscate(QByteArray data)
{
    static const QByteArray key = QByteArrayLiteral("Malamih-camera-secret");
    for (int i = 0; i < data.size(); ++i)
        data[i] = char(data[i] ^ key[i % key.size()]);
    return data;
}

void writeCamera(QSettings &store, const Camera &c)
{
    store.setValue(QStringLiteral("id"), c.id);
    store.setValue(QStringLiteral("name"), c.name);
    store.setValue(QStringLiteral("kind"), c.kind == Kind::Network ? QStringLiteral("network") : QStringLiteral("local"));
    store.setValue(QStringLiteral("index"), c.index);
    store.setValue(QStringLiteral("backend"), int(c.backend));
    store.setValue(QStringLiteral("width"), c.width);
    store.setValue(QStringLiteral("height"), c.height);
    store.setValue(QStringLiteral("brand"), c.brand);
    store.setValue(QStringLiteral("host"), c.host);
    store.setValue(QStringLiteral("port"), c.port);
    store.setValue(QStringLiteral("channel"), c.channel);
    store.setValue(QStringLiteral("substream"), c.substream);
    store.setValue(QStringLiteral("path"), c.path);
    store.setValue(QStringLiteral("user"), c.user);
    store.setValue(QStringLiteral("password"), protect(c.password));
    store.setValue(QStringLiteral("hasFilters"), c.hasFilters);
    store.beginGroup(QStringLiteral("filters"));
    if (c.hasFilters)
        VideoFilters::write(store, c.filters);
    store.endGroup();
}

Camera readCamera(QSettings &store)
{
    Camera c;
    c.id = store.value(QStringLiteral("id")).toString();
    c.name = store.value(QStringLiteral("name")).toString();
    c.kind = store.value(QStringLiteral("kind")).toString() == QStringLiteral("network") ? Kind::Network : Kind::Local;
    c.index = std::max(0, store.value(QStringLiteral("index"), 0).toInt());
    c.backend = Backend(std::clamp(store.value(QStringLiteral("backend"), 0).toInt(), 0, 2));
    c.width = std::max(0, store.value(QStringLiteral("width"), 0).toInt());
    c.height = std::max(0, store.value(QStringLiteral("height"), 0).toInt());
    c.brand = store.value(QStringLiteral("brand"), c.brand).toString();
    c.host = store.value(QStringLiteral("host")).toString();
    c.port = std::clamp(store.value(QStringLiteral("port"), 554).toInt(), 1, 65535);
    c.channel = std::max(1, store.value(QStringLiteral("channel"), 1).toInt());
    c.substream = store.value(QStringLiteral("substream"), false).toBool();
    c.path = store.value(QStringLiteral("path")).toString();
    c.user = store.value(QStringLiteral("user")).toString();
    c.password = unprotect(store.value(QStringLiteral("password")).toString());
    c.hasFilters = store.value(QStringLiteral("hasFilters"), false).toBool();
    if (c.hasFilters) {
        store.beginGroup(QStringLiteral("filters"));
        c.filters = VideoFilters::read(store);
        store.endGroup();
    }
    return c;
}

} // namespace

const QList<Brand> &brands()
{
    static const QList<Brand> list = {
        {QStringLiteral("hikvision"), QStringLiteral("Hikvision / HiLook / HiWatch"),
         QStringLiteral("rtsp://{host}:{port}/Streaming/Channels/{ch}0{s1}"), 554, true, true,
         QStringLiteral("العنوان الافتراضي للكاميرا 192.168.1.64 والمستخدم admin. القناة رقم الكاميرا في جهاز التسجيل.")},
        {QStringLiteral("dahua"), QStringLiteral("Dahua / Imou / Lorex"),
         QStringLiteral("rtsp://{host}:{port}/cam/realmonitor?channel={ch}&subtype={s0}"), 554, true, true,
         QStringLiteral("العنوان الافتراضي للكاميرا 192.168.1.108 والمستخدم admin.")},
        {QStringLiteral("uniview"), QStringLiteral("Uniview (UNV)"), QStringLiteral("rtsp://{host}:{port}/unicast/c{ch}/s{s0}/live"),
         554, true, true, QString()},
        {QStringLiteral("reolink"), QStringLiteral("Reolink"), QStringLiteral("rtsp://{host}:{port}/h264Preview_{ch2}_{ms}"), 554,
         true, true, QStringLiteral("فعّل RTSP من إعدادات الكاميرا في تطبيق Reolink (الشبكة ← متقدم ← المنافذ).")},
        {QStringLiteral("tapo"), QStringLiteral("TP-Link Tapo / VIGI"), QStringLiteral("rtsp://{host}:{port}/stream{s1}"), 554, false,
         true, QStringLiteral("أنشئ «حساب الكاميرا» من تطبيق Tapo: الإعدادات ← إعدادات متقدمة ← حساب الكاميرا، واستخدمه هنا.")},
        {QStringLiteral("ezviz"), QStringLiteral("EZVIZ"), QStringLiteral("rtsp://{host}:{port}/H.264"), 554, false, false,
         QStringLiteral("المستخدم admin وكلمة المرور رمز التحقق المطبوع على ملصق الكاميرا.")},
        {QStringLiteral("axis"), QStringLiteral("Axis"), QStringLiteral("rtsp://{host}:{port}/axis-media/media.amp"), 554, false,
         false, QString()},
        {QStringLiteral("rtsp"), QStringLiteral("RTSP عام (ONVIF وغيرها)"), QStringLiteral("rtsp://{host}:{port}/{path}"), 554, false,
         false, QStringLiteral("اكتب مسار البث من دليل الكاميرا، أو اعرفه ببرنامج ONVIF Device Manager.")},
        {QStringLiteral("phone"), QStringLiteral("هاتف (تطبيق IP Webcam)"), QStringLiteral("http://{host}:{port}/video"), 8080, false,
         false, QStringLiteral("شغّل التطبيق على الهاتف واضغط «Start server»، ثم اكتب العنوان الظاهر أسفل الشاشة. "
                               "الهاتف واللابتوب على شبكة Wi-Fi نفسها.")},
        {QStringLiteral("http"), QStringLiteral("بث HTTP / MJPEG"), QStringLiteral("http://{host}:{port}/{path}"), 80, false, false,
         QString()},
        {QStringLiteral("custom"), QStringLiteral("عنوان بث كامل"), QString(), 554, false, false,
         QStringLiteral("مثل rtsp://192.168.1.20:554/live أو http://192.168.1.30/mjpg/video.mjpg")},
    };
    return list;
}

const Brand &brand(const QString &key)
{
    const QList<Brand> &list = brands();
    for (const Brand &b : list) {
        if (b.key == key)
            return b;
    }
    return list.first();
}

QString Camera::source() const
{
    return QStringLiteral("camera:") + (id.isEmpty() ? QString::number(index) : id);
}

QString Camera::url(bool withCredentials) const
{
    const Brand &b = Cameras::brand(brand);
    const QString raw = b.pattern.isEmpty() ? path.trimmed() : streamValue(b.pattern, *this);
    QUrl parsed(raw, QUrl::TolerantMode);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return raw;
    if (!user.isEmpty() && parsed.userName().isEmpty()) {
        // كل رمز خاص يُرمَّز، فكلمة مرور فيها @ أو : أو / لا تُفسد العنوان
        parsed.setUserName(QString::fromLatin1(QUrl::toPercentEncoding(user)), QUrl::TolerantMode);
        if (!password.isEmpty())
            parsed.setPassword(QString::fromLatin1(QUrl::toPercentEncoding(password)), QUrl::TolerantMode);
    }
    if (!withCredentials && !parsed.password().isEmpty())
        parsed.setPassword(QStringLiteral("****"));
    return parsed.toString(QUrl::FullyEncoded);
}

QString Camera::description() const
{
    if (kind == Kind::Network)
        return url(false);
    QString text = QStringLiteral("موصولة بالجهاز · رقم %1").arg(index);
    if (width > 0 && height > 0)
        text += QStringLiteral(" · %1×%2").arg(width).arg(height);
    return text;
}

QList<Camera> load()
{
    QSettings store;
    QList<Camera> list;
    const int count = store.beginReadArray(QStringLiteral("cameras/list"));
    for (int i = 0; i < count; ++i) {
        store.setArrayIndex(i);
        Camera c = readCamera(store);
        if (!c.id.isEmpty())
            list.append(c);
    }
    store.endArray();
    return list;
}

void save(const QList<Camera> &cameras)
{
    QSettings store;
    store.remove(QStringLiteral("cameras/list"));
    store.beginWriteArray(QStringLiteral("cameras/list"), int(cameras.size()));
    for (int i = 0; i < cameras.size(); ++i) {
        store.setArrayIndex(i);
        writeCamera(store, cameras.at(i));
    }
    store.endArray();
}

QString newId()
{
    return QStringLiteral("c") + QUuid::createUuid().toString(QUuid::Id128).left(10);
}

bool resolve(const QString &key, Camera *camera)
{
    bool number = false;
    const int index = key.toInt(&number);
    if (number) {
        Camera c;
        c.index = std::max(0, index);
        c.name = localName(c.index, localDevices());
        *camera = c;
        return true;
    }
    for (const Camera &c : load()) {
        if (c.id == key) {
            *camera = c;
            return true;
        }
    }
    return false;
}

void saveFilters(const QString &id, const VideoFilters::Settings &filters)
{
    if (id.isEmpty())
        return;
    QList<Camera> list = load();
    for (Camera &c : list) {
        if (c.id == id) {
            if (c.hasFilters && c.filters == filters)
                return;
            c.hasFilters = true;
            c.filters = filters;
            save(list);
            return;
        }
    }
}

QString lastSource()
{
    return QSettings().value(QStringLiteral("cameras/last"), QStringLiteral("camera:0")).toString();
}

void setLastSource(const QString &source)
{
    QSettings().setValue(QStringLiteral("cameras/last"), source);
}

QList<LocalDevice> localDevices()
{
    QList<LocalDevice> devices;
#ifdef Q_OS_WIN
    // ترتيب DirectShow هو ترتيب أرقام الكاميرات عند فتحها به
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ICreateDevEnum *deviceEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                                   reinterpret_cast<void **>(&deviceEnum)))) {
        IEnumMoniker *monikers = nullptr;
        if (deviceEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) == S_OK) {
            IMoniker *moniker = nullptr;
            int index = 0;
            while (monikers->Next(1, &moniker, nullptr) == S_OK) {
                QString name;
                IPropertyBag *bag = nullptr;
                if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, reinterpret_cast<void **>(&bag)))) {
                    VARIANT value;
                    VariantInit(&value);
                    if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR)
                        name = QString::fromWCharArray(value.bstrVal);
                    VariantClear(&value);
                    bag->Release();
                }
                devices.append({index++, name});
                moniker->Release();
            }
            monikers->Release();
        }
        deviceEnum->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
#elif defined(Q_OS_LINUX)
    const QDir dir(QStringLiteral("/sys/class/video4linux"));
    QStringList nodes = dir.entryList({QStringLiteral("video*")}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    std::sort(nodes.begin(), nodes.end(), [](const QString &a, const QString &b) { return a.mid(5).toInt() < b.mid(5).toInt(); });
    for (const QString &node : nodes) {
        // كل كاميرا تظهر عادة بعقدتين: الصورة (index 0) وبيانات وصفية
        QFile indexFile(dir.filePath(node + QStringLiteral("/index")));
        if (indexFile.open(QIODevice::ReadOnly) && indexFile.readAll().trimmed() != "0")
            continue;
        QFile nameFile(dir.filePath(node + QStringLiteral("/name")));
        const QString name = nameFile.open(QIODevice::ReadOnly) ? QString::fromUtf8(nameFile.readAll()).trimmed() : QString();
        devices.append({node.mid(5).toInt(), name});
    }
#endif
    return devices;
}

QString localName(int index, const QList<LocalDevice> &devices)
{
    for (const LocalDevice &d : devices) {
        if (d.index == index && !d.name.isEmpty())
            return d.name;
    }
    return index == 0 ? QStringLiteral("كاميرا الجهاز") : QStringLiteral("كاميرا رقم %1").arg(index);
}

QStringList localAddresses()
{
    QStringList list;
    for (const QNetworkInterface &face : QNetworkInterface::allInterfaces()) {
        if (!(face.flags() & QNetworkInterface::IsUp) || (face.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : face.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
                list.append(address.toString());
        }
    }
    list.removeDuplicates();
    return list;
}

QString protect(const QString &secret)
{
    if (secret.isEmpty())
        return QString();
    const QByteArray plain = secret.toUtf8();
#ifdef Q_OS_WIN
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()))};
    DATA_BLOB out{};
    if (CryptProtectData(&in, L"Malamih", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const QByteArray sealed(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
        LocalFree(out.pbData);
        return QStringLiteral("d:") + QString::fromLatin1(sealed.toBase64());
    }
#endif
    return QStringLiteral("x:") + QString::fromLatin1(obfuscate(plain).toBase64());
}

QString unprotect(const QString &stored)
{
    if (stored.startsWith(QStringLiteral("x:")))
        return QString::fromUtf8(obfuscate(QByteArray::fromBase64(stored.mid(2).toLatin1())));
#ifdef Q_OS_WIN
    if (stored.startsWith(QStringLiteral("d:"))) {
        QByteArray sealed = QByteArray::fromBase64(stored.mid(2).toLatin1());
        DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE *>(sealed.data())};
        DATA_BLOB out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            const QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
            LocalFree(out.pbData);
            return plain;
        }
        return QString();
    }
#endif
    if (stored.startsWith(QStringLiteral("d:")))
        return QString(); // محفوظة على Windows لمستخدم آخر
    return stored;
}

bool open(cv::VideoCapture &capture, const Camera &camera, QString *error)
{
    capture.release();
    if (camera.kind == Kind::Local) {
        std::vector<int> apis;
#ifdef Q_OS_WIN
        if (camera.backend != Backend::MediaFoundation)
            apis.push_back(cv::CAP_DSHOW);
        if (camera.backend != Backend::DirectShow)
            apis.push_back(cv::CAP_MSMF);
#elif defined(Q_OS_LINUX)
        apis.push_back(cv::CAP_V4L2);
#endif
        apis.push_back(cv::CAP_ANY);
        for (int api : apis) {
            try {
                if (capture.open(camera.index, api))
                    break;
            } catch (const cv::Exception &) {
            }
        }
        if (!capture.isOpened()) {
            if (error)
                *error = diagnose(camera);
            return false;
        }
        if (camera.width > 0 && camera.height > 0) {
            // الدقة العالية عبر USB 2 تحتاج ضغط MJPG، وإلا تنخفض الإطارات أو ترفض الكاميرا
            if (camera.width >= 1280)
                capture.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
            capture.set(cv::CAP_PROP_FRAME_WIDTH, camera.width);
            capture.set(cv::CAP_PROP_FRAME_HEIGHT, camera.height);
        }
        capture.set(cv::CAP_PROP_BUFFERSIZE, 1);
        return true;
    }

    const QString url = camera.url(true);
    if (url.isEmpty() || (camera.brand != QStringLiteral("custom") && camera.host.trimmed().isEmpty())) {
        if (error)
            *error = QStringLiteral("اكتب عنوان الكاميرا أولاً");
        return false;
    }
    // TCP أثبت من UDP عبر Wi-Fi والشبكات المزدحمة، ولا تضيع فيه أجزاء الإطار
    if (url.startsWith(QStringLiteral("rtsp"), Qt::CaseInsensitive) && qEnvironmentVariableIsEmpty("OPENCV_FFMPEG_CAPTURE_OPTIONS"))
        qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp");
    try {
        capture.open(url.toStdString(), cv::CAP_FFMPEG,
                     {cv::CAP_PROP_OPEN_TIMEOUT_MSEC, OpenTimeoutMs, cv::CAP_PROP_READ_TIMEOUT_MSEC, ReadTimeoutMs});
    } catch (const cv::Exception &) {
    }
    if (!capture.isOpened()) {
        if (error)
            *error = diagnose(camera);
        return false;
    }
    return true;
}

QString diagnose(const Camera &camera)
{
    if (camera.kind == Kind::Local) {
        return QStringLiteral("تعذر فتح الكاميرا رقم %1. تأكد أنها موصولة، وأن برنامجاً آخر (Zoom أو Teams أو تطبيق الكاميرا) "
                              "لا يستخدمها، وجرّب طريقة التقاط أخرى أو منفذ USB آخر.")
            .arg(camera.index);
    }
    const QUrl url(camera.url(false), QUrl::TolerantMode);
    const QString host = url.host();
    if (host.isEmpty())
        return QStringLiteral("العنوان غير صحيح: %1").arg(camera.url(false));
    const bool http = url.scheme().startsWith(QStringLiteral("http"), Qt::CaseInsensitive);
    const int port = url.port(http ? 80 : 554);
    QTcpSocket socket;
    socket.connectToHost(host, quint16(port));
    if (!socket.waitForConnected(3000)) {
        const QStringList mine = localAddresses();
        return QStringLiteral("لا يرد أي جهاز على %1 (المنفذ %2). تحقق من السلك والكهرباء وعنوان الكاميرا، "
                              "وأن عنوان اللابتوب في النطاق نفسه (عناوين هذا الجهاز: %3).")
            .arg(host)
            .arg(port)
            .arg(mine.isEmpty() ? QStringLiteral("غير متصل بأي شبكة") : mine.join(QStringLiteral("، ")));
    }
    socket.abort();
    return QStringLiteral("الكاميرا موجودة على %1 لكنها لم تعطِ البث. تحقق من اسم المستخدم وكلمة المرور، "
                          "ومن نوع الكاميرا ورقم القناة (أو مسار البث)، وأن RTSP مفعّل في إعداداتها.")
        .arg(host);
}

cv::Mat snapshot(const Camera &camera, QString *error, QString *info)
{
    cv::VideoCapture capture;
    if (!open(capture, camera, error))
        return {};
    // الكاميرات الموصولة تضبط الإضاءة في الإطارات الأولى
    const int frames = camera.kind == Kind::Local ? 8 : 2;
    cv::Mat frame, last;
    for (int i = 0; i < frames; ++i) {
        if (!capture.read(frame) || frame.empty())
            break;
        last = frame.clone();
    }
    if (last.empty()) {
        if (error)
            *error = QStringLiteral("فُتح الاتصال لكن لم يصل أي إطار من الكاميرا");
        return {};
    }
    if (info) {
        const double fps = capture.get(cv::CAP_PROP_FPS);
        *info = QStringLiteral("%1×%2").arg(last.cols).arg(last.rows);
        if (fps > 1.0 && fps < 240.0)
            *info += QStringLiteral(" · %1 إطاراً/ث").arg(qRound(fps));
    }
    return last;
}

LiveReader::LiveReader(const Camera &camera)
    : m_camera(camera)
{
}

LiveReader::~LiveReader()
{
    m_stop = true;
    m_ready.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    m_capture.release();
}

bool LiveReader::open(QString *error)
{
    if (!Cameras::open(m_capture, m_camera, error))
        return false;
    const double fps = m_capture.get(cv::CAP_PROP_FPS);
    m_fps = fps > 1.0 && fps < 240.0 ? fps : 0.0;
    m_thread = std::thread([this] { run(); });
    return true;
}

void LiveReader::run()
{
    int failures = 0;
    while (!m_stop) {
        cv::Mat frame;
        const bool ok = m_capture.read(frame) && !frame.empty();
        if (m_stop)
            break;
        if (!ok) {
            if (++failures >= 3) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_failed = true;
                m_ready.notify_all();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            continue;
        }
        failures = 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frame = frame;
        ++m_sequence;
        m_ready.notify_all();
    }
}

bool LiveReader::read(cv::Mat &frame, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_ready.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                     [this] { return m_sequence > m_taken || m_failed || m_stop; });
    if (m_sequence <= m_taken)
        return false;
    frame = m_frame;
    m_taken = m_sequence;
    return true;
}

} // namespace Cameras

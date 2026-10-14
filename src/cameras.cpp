#include "cameras.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>
#include <QThreadPool>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>

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
    store.setValue(QStringLiteral("via"), c.via);
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

bool openStream(cv::VideoCapture &capture, const QString &url, int timeoutMs)
{
    capture.release();
    // TCP أثبت من UDP عبر Wi-Fi والشبكات المزدحمة، ولا تضيع فيه أجزاء الإطار
    if (url.startsWith(QStringLiteral("rtsp"), Qt::CaseInsensitive) && qEnvironmentVariableIsEmpty("OPENCV_FFMPEG_CAPTURE_OPTIONS"))
        qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp");
    try {
        capture.open(url.toStdString(), cv::CAP_FFMPEG,
                     {cv::CAP_PROP_OPEN_TIMEOUT_MSEC, timeoutMs, cv::CAP_PROP_READ_TIMEOUT_MSEC, timeoutMs});
    } catch (const cv::Exception &) {
    }
    return capture.isOpened();
}

bool portOpen(const QString &host, int port, int timeoutMs)
{
    QTcpSocket socket;
    socket.connectToHost(host, quint16(port));
    const bool ok = socket.waitForConnected(timeoutMs);
    socket.abort();
    return ok;
}

// الشركة من اسمها كما تعلنه الكاميرا في ONVIF
QString brandFromText(const QString &text)
{
    const QString t = text.toLower();
    static const QList<QPair<QString, QStringList>> marks = {
        {QStringLiteral("hikvision"), {QStringLiteral("hikvision"), QStringLiteral("hiwatch"), QStringLiteral("hilook")}},
        {QStringLiteral("dahua"), {QStringLiteral("dahua"), QStringLiteral("imou"), QStringLiteral("lorex"), QStringLiteral("amcrest")}},
        {QStringLiteral("uniview"), {QStringLiteral("uniview"), QStringLiteral("unv")}},
        {QStringLiteral("reolink"), {QStringLiteral("reolink")}},
        {QStringLiteral("tapo"), {QStringLiteral("tapo"), QStringLiteral("tp-link"), QStringLiteral("vigi")}},
        {QStringLiteral("ezviz"), {QStringLiteral("ezviz")}},
        {QStringLiteral("axis"), {QStringLiteral("axis")}},
    };
    for (const auto &[key, words] : marks) {
        for (const QString &w : words) {
            if (t.contains(w))
                return key;
        }
    }
    return QString();
}

QList<Found> onvifProbe(int waitMs)
{
    QList<Found> found;
    const QByteArray probe = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header><w:MessageID>uuid:%1</w:MessageID>"
        "<w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action e:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
        "</e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body></e:Envelope>")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
                                 .toUtf8();
    // من كل شبكة في اللابتوب، فتصل الكاميرا الموصولة بسلك مباشر حتى لو كان نطاقها مختلفاً
    std::vector<std::unique_ptr<QUdpSocket>> sockets;
    for (const QString &address : localAddresses()) {
        auto socket = std::make_unique<QUdpSocket>();
        if (!socket->bind(QHostAddress(address), 0))
            continue;
        socket->writeDatagram(probe, QHostAddress(QStringLiteral("239.255.255.250")), 3702);
        sockets.push_back(std::move(socket));
    }
    static const QRegularExpression xaddrs(QStringLiteral("<[^>]*XAddrs>([^<]+)<"));
    static const QRegularExpression scopes(QStringLiteral("<[^>]*Scopes>([^<]+)<"));
    QElapsedTimer timer;
    timer.start();
    while (!sockets.empty() && timer.elapsed() < waitMs) {
        for (auto &socket : sockets) {
            if (!socket->waitForReadyRead(40))
                continue;
            while (socket->hasPendingDatagrams()) {
                const QNetworkDatagram datagram = socket->receiveDatagram();
                const QString reply = QString::fromUtf8(datagram.data());
                QString host = datagram.senderAddress().toString();
                const QRegularExpressionMatch addr = xaddrs.match(reply);
                if (addr.hasMatch()) {
                    const QUrl url(addr.captured(1).split(QLatin1Char(' '), Qt::SkipEmptyParts).value(0));
                    if (!url.host().isEmpty())
                        host = url.host();
                }
                host.remove(QStringLiteral("::ffff:"));
                if (host.isEmpty())
                    continue;
                QString name, hardware;
                for (const QString &scope : scopes.match(reply).captured(1).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    const QString value = QUrl::fromPercentEncoding(scope.section(QLatin1Char('/'), -1).toUtf8());
                    if (scope.contains(QStringLiteral("/name/")))
                        name = value;
                    else if (scope.contains(QStringLiteral("/hardware/")))
                        hardware = value;
                }
                Found f;
                f.host = host;
                QStringList model;
                if (!name.isEmpty())
                    model << name;
                if (!hardware.isEmpty() && hardware != name)
                    model << hardware;
                f.model = model.join(QStringLiteral(" · "));
                f.brand = brandFromText(name + QLatin1Char(' ') + hardware);
                found.append(f);
            }
        }
    }
    return found;
}

// العناوين المجاورة في شبكات اللابتوب (حتى 254 عنواناً لكل شبكة)
QStringList neighbours()
{
    QStringList hosts;
    for (const QNetworkInterface &face : QNetworkInterface::allInterfaces()) {
        if (!(face.flags() & QNetworkInterface::IsUp) || (face.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : face.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback() || ip.isLinkLocal())
                continue;
            const int prefix = std::clamp(entry.prefixLength(), 24, 30);
            const quint32 mask = prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
            const quint32 net = ip.toIPv4Address() & mask;
            for (quint32 a = net + 1; a < (net | ~mask); ++a) {
                if (a != ip.toIPv4Address())
                    hosts.append(QHostAddress(a).toString());
            }
        }
    }
    hosts.removeDuplicates();
    return hosts;
}

Camera readCamera(QSettings &store)
{
    Camera c;
    c.id = store.value(QStringLiteral("id")).toString();
    c.name = store.value(QStringLiteral("name")).toString();
    c.kind = store.value(QStringLiteral("kind")).toString() == QStringLiteral("network") ? Kind::Network : Kind::Local;
    c.via = store.value(QStringLiteral("via")).toString();
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

const QList<Connection> &connections()
{
    static const QList<Connection> list = {
        {QStringLiteral("usb"), QStringLiteral("كاميرا USB أو كاميرا اللابتوب"),
         QStringLiteral("كاميرا ويب أو مؤتمرات بسلك USB، أو الكاميرا المدمجة في الشاشة"), Kind::Local, QString()},
        {QStringLiteral("analog"), QStringLiteral("كاميرا مراقبة بسلك محوري (BNC)"),
         QStringLiteral("كاميرا مراقبة قديمة توصل باللابتوب عبر محوّل فيديو إلى USB مثل EasyCAP"), Kind::Local, QString()},
        {QStringLiteral("hdmi"), QStringLiteral("جهاز بمخرج HDMI"),
         QStringLiteral("كاميرا فيديو أو احترافية، أو شاشة جهاز تسجيل، عبر بطاقة التقاط HDMI إلى USB"), Kind::Local, QString()},
        {QStringLiteral("phone_usb"), QStringLiteral("الهاتف بسلك USB"),
         QStringLiteral("كاميرا الهاتف كأنها كاميرا ويب، بتطبيق DroidCam أو Camo"), Kind::Local, QString()},
        {QStringLiteral("router"), QStringLiteral("كاميرا Wi-Fi أو كاميرا شبكة عبر الراوتر"),
         QStringLiteral("الكاميرا واللابتوب على شبكة الإنترنت نفسها في المكان"), Kind::Network, QStringLiteral("hikvision")},
        {QStringLiteral("direct"), QStringLiteral("كاميرا شبكة بسلك مباشر في اللابتوب"),
         QStringLiteral("سلك شبكة من الكاميرا إلى منفذ الإنترنت في اللابتوب، بلا راوتر"), Kind::Network, QStringLiteral("hikvision")},
        {QStringLiteral("recorder"), QStringLiteral("جهاز تسجيل DVR أو NVR"),
         QStringLiteral("كاميرات جهاز التسجيل عبر الشبكة، كل كاميرا برقم قناة"), Kind::Network, QStringLiteral("hikvision")},
        {QStringLiteral("phone_wifi"), QStringLiteral("الهاتف عبر Wi-Fi"),
         QStringLiteral("تطبيق IP Webcam على الهاتف، والهاتف واللابتوب على الشبكة نفسها"), Kind::Network, QStringLiteral("phone")},
    };
    return list;
}

const Connection &connection(const QString &key)
{
    const QList<Connection> &list = connections();
    for (const Connection &c : list) {
        if (c.key == key)
            return c;
    }
    return list.first();
}

QString steps(const QString &key)
{
    const auto page = [](const QString &title, const QStringList &items, const QString &note = QString()) {
        QString html = QStringLiteral("<b>%1</b><ol style='margin-right:-20px'>").arg(title);
        for (const QString &item : items)
            html += QStringLiteral("<li style='margin-bottom:4px'>%1</li>").arg(item);
        html += QStringLiteral("</ol>");
        if (!note.isEmpty())
            html += QStringLiteral("<p>%1</p>").arg(note);
        return html;
    };
    const QStringList mine = localAddresses();
    const QString addresses = QStringLiteral("<p>عناوين هذا اللابتوب الآن: <b>%1</b></p>")
                                  .arg(mine.isEmpty() ? QStringLiteral("غير متصل بأي شبكة") : mine.join(QStringLiteral("، ")));

    if (key == QLatin1String("analog"))
        return page(QStringLiteral("خطوات توصيل كاميرا المراقبة التناظرية"),
                    {QStringLiteral("وصّل سلك الكاميرا (BNC) بوصلة BNC إلى RCA، ثم بالمدخل <b>الأصفر</b> في المحوّل، "
                                    "ووصّل المحوّل بمنفذ USB."),
                     QStringLiteral("الكاميرا تحتاج كهرباء من محوّلها الخاص (غالباً 12 فولت)؛ المحوّل لا يعطيها كهرباء."),
                     QStringLiteral("اضغط «تحديث» واختر المحوّل من «الجهاز» (اسمه غالباً USB Video أو AV TO USB)."),
                     QStringLiteral("اضغط «اختبار». إن ظهرت الصورة اضغط «تشغيل الكاميرا».")},
                    QStringLiteral("<b>إن لم تظهر صورة:</b> جرّب «طريقة الالتقاط» الأخرى أو الدقة 720×576. "
                                   "إن ظهرت خطوط مشطية عند الحركة فعّل «إزالة التشابك» من «معالجات». "
                                   "إن بقيت الصورة سوداء أو مشوشة فالكاميرا غالباً AHD أو TVI: حوّلها إلى وضع CVBS من قائمتها، "
                                   "أو استخدم محوّل AHD إلى HDMI ثم بطاقة التقاط HDMI."));
    if (key == QLatin1String("hdmi"))
        return page(QStringLiteral("خطوات التوصيل عبر HDMI"),
                    {QStringLiteral("وصّل سلك HDMI من الكاميرا أو جهاز التسجيل إلى بطاقة الالتقاط، والبطاقة بمنفذ USB "
                                    "(الأزرق USB 3 إن وُجد)."),
                     QStringLiteral("شغّل مخرج HDMI في الكاميرا، واخفِ بيانات الشاشة من قائمتها إن أمكن."),
                     QStringLiteral("اضغط «تحديث» واختر البطاقة من «الجهاز»، والدقة 1920×1080."),
                     QStringLiteral("اضغط «اختبار» ثم «تشغيل الكاميرا».")},
                    QStringLiteral("من جهاز تسجيل تظهر شاشته كما هي: كبّر الكاميرا المطلوبة لتملأ الشاشة، "
                                   "أو اختر «جهاز تسجيل DVR أو NVR» لصورة أوضح عبر الشبكة. كاميرات SDI تحتاج محوّل SDI إلى HDMI."));
    if (key == QLatin1String("phone_usb"))
        return page(QStringLiteral("خطوات استخدام الهاتف بسلك USB"),
                    {QStringLiteral("ثبّت تطبيق <b>DroidCam</b> أو <b>Camo</b> على الهاتف، وبرنامجه المرافق على اللابتوب."),
                     QStringLiteral("وصّل الهاتف بالسلك وافتح التطبيق على الجهازين."),
                     QStringLiteral("اضغط «تحديث» واختر DroidCam أو Camo من «الجهاز»."),
                     QStringLiteral("اضغط «اختبار» ثم «تشغيل الكاميرا».")});
    if (key == QLatin1String("router"))
        return page(QStringLiteral("خطوات كاميرا الشبكة عبر الراوتر"),
                    {QStringLiteral("تأكد أن الكاميرا تعمل في تطبيقها، وأن اللابتوب على شبكة Wi-Fi نفسها."),
                     QStringLiteral("اضغط <b>«البحث في الشبكة»</b> واختر الكاميرا، أو اكتب عنوانها من تطبيقها "
                                    "(معلومات الجهاز ← عنوان IP)."),
                     QStringLiteral("اكتب اسم المستخدم وكلمة المرور: من ملصق الكاميرا، أو اللذين اخترتهما في تطبيقها."),
                     QStringLiteral("لا تعرف نوع الكاميرا؟ اضغط <b>«اكتشاف تلقائي»</b> فيجرّب البرنامج كل الأنواع المعروفة."),
                     QStringLiteral("اضغط «اختبار» ثم «تشغيل الكاميرا».")},
                    QStringLiteral("كاميرات Tapo وReolink وEZVIZ تحتاج تفعيل RTSP أو إنشاء حساب للكاميرا من تطبيقها؛ "
                                   "تظهر الملاحظة تحت «نوع الكاميرا».")
                        + addresses);
    if (key == QLatin1String("direct"))
        return page(QStringLiteral("خطوات كاميرا الشبكة بسلك مباشر"),
                    {QStringLiteral("وصّل سلك الشبكة من الكاميرا إلى منفذ الإنترنت في اللابتوب. كاميرا PoE تحتاج "
                                    "<b>حاقن PoE</b> بينهما ليعطيها الكهرباء، وإلا فمحوّل كهرباء 12 فولت."),
                     QStringLiteral("اضغط <b>«البحث في الشبكة»</b>؛ يجد البرنامج الكاميرا ويخبرك بالعنوان الذي تضعه للابتوب."),
                     QStringLiteral("اضغط <b>«إعدادات الشبكة»</b> ← زر الفأرة الأيمن على Ethernet ← خصائص ← "
                                    "Internet Protocol Version 4 ← «استخدام عنوان IP التالي»، واكتب العنوان المقترح "
                                    "والقناع 255.255.255.0 واترك البوابة فارغة."),
                     QStringLiteral("عد هنا، واكتب مستخدم الكاميرا وكلمة مرورها، ثم «اكتشاف تلقائي» أو «اختبار».")},
                    QStringLiteral("العنوان الافتراضي غالباً: Hikvision ‏192.168.1.64، Dahua ‏192.168.1.108. "
                                   "بعد الانتهاء أعد Ethernet إلى «الحصول على عنوان تلقائياً» ليعمل الإنترنت بالسلك كالعادة.")
                        + addresses);
    if (key == QLatin1String("recorder"))
        return page(QStringLiteral("خطوات جهاز التسجيل DVR / NVR"),
                    {QStringLiteral("وصّل جهاز التسجيل بالراوتر، أو مباشرة باللابتوب ثم اتبع خطوات «السلك المباشر»."),
                     QStringLiteral("اعرف عنوان الجهاز من شاشته (القائمة ← الإعدادات ← الشبكة)، أو اضغط «البحث في الشبكة»."),
                     QStringLiteral("اختر نوع الجهاز، واكتب مستخدم الجهاز نفسه وكلمة مروره."),
                     QStringLiteral("<b>القناة</b> رقم الكاميرا في الجهاز: الأولى 1 والثانية 2 وهكذا."),
                     QStringLiteral("اضغط «اختبار» ثم «تشغيل الكاميرا».")},
                    QStringLiteral("لكل كاميرا في الجهاز أضف كاميرا هنا بالعنوان نفسه ورقم قناة مختلف. "
                                   "الكاميرات التناظرية الموصولة بجهاز DVR تصل بهذه الطريقة أيضاً.")
                        + addresses);
    if (key == QLatin1String("phone_wifi"))
        return page(QStringLiteral("خطوات الهاتف عبر Wi-Fi"),
                    {QStringLiteral("ثبّت تطبيق <b>IP Webcam</b> على هاتف أندرويد."),
                     QStringLiteral("اجعل الهاتف واللابتوب على شبكة Wi-Fi نفسها."),
                     QStringLiteral("في التطبيق انزل إلى آخر القائمة واضغط <b>Start server</b>."),
                     QStringLiteral("اكتب العنوان الظاهر على شاشة الهاتف (مثل 192.168.1.23) والمنفذ 8080، أو اضغط «البحث في الشبكة»."),
                     QStringLiteral("اضغط «اختبار» ثم «تشغيل الكاميرا».")},
                    addresses);
    return page(QStringLiteral("خطوات توصيل كاميرا USB"),
                {QStringLiteral("وصّل الكاميرا بمنفذ USB وانتظر ثواني حتى يتعرف عليها ويندوز."),
                 QStringLiteral("اختر اسمها من «الجهاز»، واضغط «تحديث» إن لم تظهر."),
                 QStringLiteral("اضغط «اختبار»؛ إن ظهرت الصورة اضغط «تشغيل الكاميرا».")},
                QStringLiteral("أغلق Zoom وTeams وتطبيق الكاميرا قبل الاختبار: الكاميرا لا تعمل في برنامجين معاً. "
                               "إن لم تعمل جرّب منفذ USB آخر أو «طريقة الالتقاط» الأخرى."));
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

bool onLocalNetwork(const QString &host)
{
    const QHostAddress address(host.trimmed());
    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback())
        return true;
    for (const QNetworkInterface &face : QNetworkInterface::allInterfaces()) {
        if (!(face.flags() & QNetworkInterface::IsUp))
            continue;
        for (const QNetworkAddressEntry &entry : face.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && entry.prefixLength() > 0
                && address.isInSubnet(entry.ip(), entry.prefixLength()))
                return true;
        }
    }
    return false;
}

QString suggestedAddress(const QString &cameraHost)
{
    const QHostAddress address(cameraHost.trimmed());
    if (address.protocol() != QAbstractSocket::IPv4Protocol)
        return QStringLiteral("192.168.1.200");
    const quint32 ip = address.toIPv4Address();
    const quint32 last = (ip & 0xFF) == 200 ? 201 : 200;
    return QHostAddress((ip & 0xFFFFFF00u) | last).toString();
}

QList<Found> discover()
{
    QList<Found> found = onvifProbe(2500);
    // كاميرات لا تعلن نفسها بـ ONVIF: منفذ البث وما يميز الشركات (8000 لـ Hikvision و 37777 لـ Dahua)
    struct Ports
    {
        QString host;
        bool rtsp = false, hik = false, dahua = false, phone = false;
    };
    QThreadPool pool;
    pool.setMaxThreadCount(64);
    const QList<Ports> scanned = QtConcurrent::blockingMapped(&pool, neighbours(), [](const QString &host) {
        Ports p;
        p.host = host;
        p.rtsp = portOpen(host, 554, 350);
        p.hik = portOpen(host, 8000, 350);
        p.dahua = portOpen(host, 37777, 350);
        p.phone = !p.rtsp && portOpen(host, 8080, 350);
        return p;
    });
    for (const Ports &p : scanned) {
        if (!p.rtsp && !p.hik && !p.dahua && !p.phone)
            continue;
        auto known = std::find_if(found.begin(), found.end(), [&p](const Found &f) { return f.host == p.host; });
        const QString guess = p.hik ? QStringLiteral("hikvision") : p.dahua ? QStringLiteral("dahua") : QString();
        if (known != found.end()) {
            if (known->brand.isEmpty())
                known->brand = guess;
            continue;
        }
        Found f;
        f.host = p.host;
        if (p.phone && !p.hik && !p.dahua) {
            f.brand = QStringLiteral("phone");
            f.port = 8080;
            f.model = QStringLiteral("ربما هاتف (IP Webcam)");
        } else {
            f.brand = guess;
        }
        found.append(f);
    }
    for (Found &f : found) {
        if (f.brand.isEmpty())
            f.brand = QStringLiteral("rtsp");
        f.sameNetwork = onLocalNetwork(f.host);
    }
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        return QHostAddress(a.host).toIPv4Address() < QHostAddress(b.host).toIPv4Address();
    });
    return found;
}

bool detect(Camera &camera, cv::Mat *frame, QString *error, const std::function<void(const QString &)> &progress)
{
    const QString host = camera.host.trimmed();
    if (host.isEmpty()) {
        if (error)
            *error = QStringLiteral("اكتب عنوان الكاميرا أولاً، أو اضغط «البحث في الشبكة»");
        return false;
    }
    const auto say = [&progress](const QString &text) {
        if (progress)
            progress(text);
    };
    say(QStringLiteral("فحص المنافذ في %1…").arg(host));
    const int preferred = camera.port != 80 && camera.port != 8080 && camera.port != 8000 ? camera.port : 554;
    const int rtspPort = portOpen(host, preferred, 2500) ? preferred : preferred != 554 && portOpen(host, 554, 2500) ? 554 : 0;
    const bool rtsp = rtspPort > 0;
    const bool phone = portOpen(host, 8080, 1500);
    const bool web = portOpen(host, 80, 1500);
    if (!rtsp && !phone && !web) {
        if (error)
            *error = diagnose(camera);
        return false;
    }

    // الصيغ المرشحة: الشركة المعروفة أولاً، ثم بقية الشركات، ثم المسارات الشائعة لكاميرات ONVIF
    QList<Camera> candidates;
    const auto add = [&candidates, &camera](const QString &brand, int port, const QString &path = QString()) {
        Camera c = camera;
        c.brand = brand;
        c.port = port;
        c.path = path;
        c.substream = false;
        candidates.append(c);
    };
    if (rtsp) {
        const int port = rtspPort;
        QStringList order;
        if (portOpen(host, 8000, 800))
            order << QStringLiteral("hikvision");
        if (portOpen(host, 37777, 800))
            order << QStringLiteral("dahua");
        if (Cameras::brand(camera.brand).pattern.contains(QStringLiteral("rtsp")) && camera.brand != QStringLiteral("rtsp"))
            order.prepend(camera.brand);
        order << QStringLiteral("hikvision") << QStringLiteral("dahua") << QStringLiteral("uniview") << QStringLiteral("reolink")
              << QStringLiteral("tapo") << QStringLiteral("ezviz") << QStringLiteral("axis");
        order.removeDuplicates();
        for (const QString &b : order)
            add(b, port);
        for (const QString &path : {QStringLiteral("live"), QStringLiteral("stream1"), QStringLiteral("h264"), QStringLiteral("onvif1"),
                                    QStringLiteral("11"), QStringLiteral("live/ch00_0"), QStringLiteral("ch0_0.264"),
                                    QStringLiteral("live.sdp"), QStringLiteral("media/video1"), QStringLiteral("videoMain"), QString()})
            add(QStringLiteral("rtsp"), port, path);
    }
    if (phone)
        add(QStringLiteral("phone"), 8080);
    if (web) {
        for (const QString &path : {QStringLiteral("video"), QStringLiteral("mjpg/video.mjpg"), QStringLiteral("video.mjpg"),
                                    QStringLiteral("videostream.cgi"), QStringLiteral("cgi-bin/mjpg/video.cgi")})
            add(QStringLiteral("http"), 80, path);
    }

    for (const Camera &c : candidates) {
        const Brand &b = Cameras::brand(c.brand);
        say(QStringLiteral("تجربة %1%2…").arg(b.name, c.path.isEmpty() ? QString() : QStringLiteral(" /") + c.path));
        cv::VideoCapture capture;
        bool opened = openStream(capture, c.url(true), 4000);
        // خوادم HTTP البسيطة (تطبيقات الهاتف) قد ترفض الاتصال التالي لفحص المنفذ مباشرة
        if (!opened && b.pattern.startsWith(QStringLiteral("http"))) {
            QThread::msleep(1000);
            opened = openStream(capture, c.url(true), 4000);
        }
        if (!opened)
            continue;
        cv::Mat image;
        if (!capture.read(image) || image.empty())
            continue;
        camera = c;
        if (frame)
            *frame = image;
        return true;
    }
    if (error) {
        *error = camera.password.isEmpty()
            ? QStringLiteral("الكاميرا موجودة على %1 لكنها لم تعطِ البث بأي صيغة معروفة. لم تكتب كلمة المرور: "
                             "اكتبها (من ملصق الكاميرا أو تطبيقها) وجرّب مرة أخرى.")
                  .arg(host)
            : QStringLiteral("الكاميرا موجودة على %1 لكنها رفضت كل الصيغ المعروفة. الأرجح أن اسم المستخدم أو كلمة المرور "
                             "غير صحيحين، أو أن RTSP غير مفعّل في إعداداتها (راجع تطبيق الكاميرا).")
                  .arg(host);
    }
    return false;
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
    if (!openStream(capture, url, OpenTimeoutMs)) {
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
        if (!onLocalNetwork(host))
            return QStringLiteral("لا يصل اللابتوب إلى %1 لأن الكاميرا على شبكة غير شبكته. إن كانت موصولة بسلك مباشر "
                                  "فاضبط عنوان اللابتوب يدوياً على %2 والقناع 255.255.255.0 (زر «إعدادات الشبكة»)، "
                                  "أو وصّل الكاميرا واللابتوب بالراوتر نفسه.")
                .arg(host, suggestedAddress(host));
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

#include "applog.h"
#include "diagnostics.h"
#include "mainwindow.h"
#include "performance.h"
#include "theme.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QStyle>

namespace {

// ينقل بيانات الإصدارات السابقة (HelloQt) إلى مجلد الاسم الجديد مرة واحدة
void migrateLegacyData()
{
    const QString current = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir parent(current);
    if (QFileInfo::exists(current) || !parent.cdUp())
        return;
    const QString legacy = parent.filePath(QStringLiteral("HelloQt"));
    if (QFileInfo::exists(legacy))
        QDir().rename(legacy, current);
}

// اسم قناة النسخة الأولى للمستخدم الحالي (نسخة كل مستخدم في ويندوز مستقلة)
QString instanceName()
{
    QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));
    user.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")));
    return QStringLiteral("malamih-instance-%1").arg(user.isEmpty() ? QStringLiteral("user") : user);
}

// يرسل الطلب إلى النسخة العاملة: إظهار نافذتها، أو فتح ملف فيها
bool sendToRunningInstance(const QString &message)
{
    QLocalSocket socket;
    socket.connectToServer(instanceName());
    if (!socket.waitForConnected(1000))
        return false;
    socket.write(message.toUtf8() + '\n');
    socket.flush();
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

} // namespace

// كل نافذة حوار تُفتح أكبر من المساحة المتاحة (شاشة لابتوب 768 بكسل مع شريط المهام) تُصغَّر
// وتُنقل إلى داخلها، فلا يختفي أسفلها وأزرارها تحت شريط المهام
class FitToScreen : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *dialog = qobject_cast<QDialog *>(object);
            if (dialog && dialog->isWindow() && dialog->screen())
                fit(dialog);
        }
        return QObject::eventFilter(object, event);
    }

private:
    static void fit(QWidget *window)
    {
        const QRect available = window->screen()->availableGeometry();
        // شريط العنوان والإطار حول النافذة (قبل ظهورها أول مرة قد لا يُعرفان بعد)
        QSize frame = window->frameGeometry().size() - window->size();
        if (frame.width() <= 0 || frame.height() <= 0)
            frame = QSize(16, 40);
        const QSize limit = available.size() - frame;
        QSize size = window->size();
        if (size.width() > limit.width() || size.height() > limit.height()) {
            size = size.boundedTo(limit).expandedTo(window->minimumSize().boundedTo(limit));
            window->resize(size);
        }
        const QSize outer = size + frame;
        QPoint position = window->frameGeometry().topLeft();
        position.setX(std::clamp(position.x(), available.left(), std::max(available.left(), available.right() + 1 - outer.width())));
        position.setY(std::clamp(position.y(), available.top(), std::max(available.top(), available.bottom() + 1 - outer.height())));
        if (position != window->frameGeometry().topLeft())
            window->move(position);
    }
};

int main(int argc, char *argv[])
{
    // تحويلات العتاد في Media Foundation تمنع بعض كاميرات USB من الفتح أو تبطئه كثيراً
    if (!qEnvironmentVariableIsSet("OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS"))
        qputenv("OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS", "0");
    // صوت الفيديو من واجهة FFmpeg في Qt Multimedia: الافتراضية على ويندوز، وعلى لينكس أثبت من GStreamer
    // الذي يتعطل إن نقصت بعض إضافاته
    if (!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Malamih"));
    // بلا اسم منظمة لا يحفظ سجل ويندوز أي إعداد، وملف INI يعمل كما على لينكس
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QApplication::setApplicationDisplayName(QStringLiteral("ملامح"));
    QApplication::setApplicationVersion(QStringLiteral(MALAMIH_VERSION));
    QIcon appIcon;
    appIcon.addFile(QStringLiteral(":/resources/app-256.png"));
    appIcon.addFile(QStringLiteral(":/resources/app-512.png"));
    QApplication::setWindowIcon(appIcon);
    migrateLegacyData();
    QApplication::setLayoutDirection(Qt::RightToLeft);
    Theme::apply(app);
    app.installEventFilter(new FitToScreen(&app));
    qRegisterMetaType<FaceItem>();
    qRegisterMetaType<QList<FaceItem>>();

    const QStringList args = app.arguments();
    const bool startup = args.contains(QStringLiteral("--startup"));
    if (args.size() > 1 && args.at(1).startsWith(QLatin1String("--")) && !startup) {
        Performance::force(Performance::Level::Max);
        // نتائج الفحوص ثابتة على المعالج؛ MALAMIH_GPU=1 لتجربتها على كرت الشاشة
        if (qEnvironmentVariable("MALAMIH_GPU") != QLatin1String("1"))
            Performance::forceCpu();
    }
    Performance::apply(Performance::saved());
    const int diagnostics = Diagnostics::run(args);
    if (diagnostics >= 0)
        return diagnostics;

    // نسخة واحدة فقط: فتح البرنامج مرة ثانية (أو مع ويندوز وهو مفتوح) يُظهر النسخة العاملة بدل أن
    // تكتب نسختان في القاعدة نفسها وتسجّلا الكاميرا نفسها
    const QString openPath = args.size() == 2 && QFile::exists(args.at(1)) ? QFileInfo(args.at(1)).absoluteFilePath() : QString();
    const QString request = startup ? QStringLiteral("startup") : openPath.isEmpty() ? QStringLiteral("show") : QStringLiteral("open:") + openPath;
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/instance.lock"));
    if (!lock.tryLock(0)) {
        // النسخة الأولى قد تكون في أول تشغيلها ولم تفتح قناتها بعد
        for (int attempt = 0; attempt < 10 && !lock.isLocked(); ++attempt) {
            if (sendToRunningInstance(request))
                return 0;
            lock.tryLock(300);
        }
        // نسخة عاملة لا ترد: لا تُفتح ثانية فوقها
        if (!lock.isLocked())
            return 0;
    }
    QLocalServer::removeServer(instanceName());
    QLocalServer server;
    server.listen(instanceName());

    const QDateTime unclean = AppLog::start();
    // انهار التشغيل السابق وهو يحلل على كرت الشاشة: لا يُستخدم الكرت حتى يجتاز الفحص من جديد
    if (unclean.isValid() && Performance::lastRunOnGpu()) {
        Performance::setGpuVerified(QString(), false);
        Performance::setLastRunOnGpu(false);
        AppLog::write(QStringLiteral("[تحذير] انهار التشغيل السابق أثناء التحليل على كرت الشاشة؛ يعود التحليل إلى المعالج حتى يُفحص الكرت"));
    }

    MainWindow window;
    if (unclean.isValid())
        window.setPreviousCrash(unclean);
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, &window, [socket, &window] {
                while (socket->canReadLine()) {
                    const QString message = QString::fromUtf8(socket->readLine()).trimmed();
                    window.activateFromOtherInstance(message);
                }
            });
        }
    });
    const QRect available = app.primaryScreen()->availableGeometry();
    window.setMinimumSize(QSize(760, 520).boundedTo(available.size()));
    if (window.startHidden()) {
        // فُتح مع ويندوز: يبقى بجانب الساعة، ويظهر بالنقر على أيقونته
        window.setGeometry(QStyle::alignedRect(Qt::RightToLeft, Qt::AlignCenter, QSize(1320, 820).boundedTo(available.size()), available));
    } else if (available.width() < 1400 || available.height() < 860) {
        // شاشات اللابتوب: النافذة تملأ الشاشة والواجهة تصغّر نفسها لتظهر كل أجزائها
        window.setGeometry(available);
        window.showMaximized();
    } else {
        window.setGeometry(QStyle::alignedRect(Qt::RightToLeft, Qt::AlignCenter, QSize(1320, 820), available));
        window.show();
    }
    if (args.size() == 2 && QFile::exists(args.at(1)))
        window.openFile(args.at(1));

    const int result = app.exec();
    AppLog::finish();
    return result;
}

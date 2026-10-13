#include "diagnostics.h"
#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QScreen>
#include <QStandardPaths>
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

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Malamih"));
    QApplication::setApplicationDisplayName(QStringLiteral("ملامح"));
    QApplication::setApplicationVersion(QStringLiteral("3.0.0"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/resources/app-256.png")));
    migrateLegacyData();
    QApplication::setLayoutDirection(Qt::RightToLeft);
    Theme::apply(app);
    qRegisterMetaType<FaceItem>();
    qRegisterMetaType<QList<FaceItem>>();

    const QStringList args = app.arguments();
    const int diagnostics = Diagnostics::run(args);
    if (diagnostics >= 0)
        return diagnostics;

    MainWindow window;
    // Malamih --screenshot <العرض>x<الارتفاع> <ملف الصورة>: لقطة للنافذة بحجم شاشة معينة لفحص تكيّف الواجهة
    if (args.size() == 4 && args.at(1) == QLatin1String("--screenshot")) {
        const QStringList parts = args.at(2).split(QLatin1Char('x'));
        if (parts.size() != 2)
            return 2;
        window.resize(parts.at(0).toInt(), parts.at(1).toInt());
        window.show();
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 1500)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        return window.grab().save(args.at(3)) ? 0 : 1;
    }
    const QRect available = app.primaryScreen()->availableGeometry();
    const QSize size = QSize(1280, 800).boundedTo(available.size() * 0.95);
    window.setGeometry(QStyle::alignedRect(Qt::RightToLeft, Qt::AlignCenter, size, available));
    window.show();
    if (args.size() == 2 && QFile::exists(args.at(1)))
        window.openFile(args.at(1));

    return app.exec();
}

#include "diagnostics.h"
#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
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
    QIcon appIcon;
    appIcon.addFile(QStringLiteral(":/resources/app-256.png"));
    appIcon.addFile(QStringLiteral(":/resources/app-512.png"));
    QApplication::setWindowIcon(appIcon);
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
    const QRect available = app.primaryScreen()->availableGeometry();
    window.setMinimumSize(QSize(760, 520).boundedTo(available.size()));
    if (available.width() < 1400 || available.height() < 860) {
        // شاشات اللابتوب: النافذة تملأ الشاشة والواجهة تصغّر نفسها لتظهر كل أجزائها
        window.setGeometry(available);
        window.showMaximized();
    } else {
        window.setGeometry(QStyle::alignedRect(Qt::RightToLeft, Qt::AlignCenter, QSize(1320, 820), available));
        window.show();
    }
    if (args.size() == 2 && QFile::exists(args.at(1)))
        window.openFile(args.at(1));

    return app.exec();
}

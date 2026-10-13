#include "diagnostics.h"
#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QScreen>
#include <QStyle>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("HelloQt"));
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
    const QSize size = QSize(1280, 800).boundedTo(available.size() * 0.95);
    window.setGeometry(QStyle::alignedRect(Qt::RightToLeft, Qt::AlignCenter, size, available));
    window.show();
    if (args.size() == 2 && QFile::exists(args.at(1)))
        window.openFile(args.at(1));

    return app.exec();
}

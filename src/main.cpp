#include "mainwindow.h"

#include <QApplication>
#include <QScreen>
#include <QStyle>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setLayoutDirection(Qt::RightToLeft);

    MainWindow window;
    window.setGeometry(QStyle::alignedRect(Qt::RightToLeft, Qt::AlignCenter, QSize(800, 600),
                                           app.primaryScreen()->availableGeometry()));
    window.show();

    return app.exec();
}

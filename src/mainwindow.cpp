#include "mainwindow.h"

#include <QFont>
#include <QLabel>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("مرحبا"));

    auto *label = new QLabel(QStringLiteral("مرحبا"), this);
    label->setAlignment(Qt::AlignCenter);

    QFont font = label->font();
    font.setPointSize(32);
    label->setFont(font);

    setCentralWidget(label);
}

#include "startscreen.h"

#include "cameras.h"
#include "theme.h"

#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int MaxRecent = 6;
constexpr int MaxCameras = 5;

const QStringList VideoSuffixes = {QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("mkv"), QStringLiteral("mov"),
                                   QStringLiteral("wmv"), QStringLiteral("m4v"), QStringLiteral("webm"), QStringLiteral("mpg"),
                                   QStringLiteral("mpeg"), QStringLiteral("flv"), QStringLiteral("ts"), QStringLiteral("3gp")};

QLabel *makeLabel(const QString &text, const QString &object, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(object);
    return label;
}

} // namespace

StartScreen::StartScreen(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(QStringLiteral(
        "QToolButton#startTile { background: %1; border: 1px solid %2; border-radius: 14px; padding: 12px 8px 10px 8px;"
        " color: %3; font-size: 11pt; font-weight: 600; }"
        "QToolButton#startTile:hover { background: %4; border-color: %5; }"
        "QToolButton#startTile:disabled { color: %6; }"
        "QToolButton#startItem { background: transparent; border: none; border-radius: 8px; padding: 5px 8px; color: %3;"
        " text-align: right; }"
        "QToolButton#startItem:hover { background: %1; }"
        "QToolButton#startItem:disabled { color: %6; }")
                      .arg(Theme::Surface.name(), Theme::Border.name(), Theme::Text.name(), Theme::SurfaceHigh.name(),
                           Theme::Accent.name(), Theme::Muted.name()));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(32, 24, 32, 24);
    outer->addStretch(2);

    auto *column = new QWidget(this);
    column->setMaximumWidth(760);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    auto *title = makeLabel(QStringLiteral("ابدأ من هنا"), QStringLiteral("title"), column);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    m_status = makeLabel(QString(), QStringLiteral("subtitle"), column);
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *tiles = new QHBoxLayout;
    tiles->setSpacing(12);
    QToolButton *image = tile(Icons::Name::Image, QStringLiteral("صورة"), QStringLiteral("Ctrl+O"));
    QToolButton *video = tile(Icons::Name::Video, QStringLiteral("فيديو"), QStringLiteral("Ctrl+Shift+O"));
    QToolButton *camera = tile(Icons::Name::Camera, QStringLiteral("كاميرا"), QStringLiteral("Ctrl+Shift+C آخر كاميرا"));
    QToolButton *folder = tile(Icons::Name::Folder, QStringLiteral("مجلد صور"), QStringLiteral("Ctrl+Shift+F"));
    connect(image, &QToolButton::clicked, this, &StartScreen::openImageRequested);
    connect(video, &QToolButton::clicked, this, &StartScreen::openVideoRequested);
    connect(camera, &QToolButton::clicked, this, &StartScreen::openCameraRequested);
    connect(folder, &QToolButton::clicked, this, &StartScreen::openFolderRequested);
    tiles->addStretch();
    for (QToolButton *button : std::as_const(m_tiles))
        tiles->addWidget(button);
    tiles->addStretch();
    layout->addLayout(tiles);
    layout->addSpacing(6);

    // آخر الملفات والكاميرات جنباً إلى جنب
    auto *lists = new QGridLayout;
    lists->setHorizontalSpacing(28);
    lists->setVerticalSpacing(4);
    lists->addWidget(makeLabel(QStringLiteral("آخر الملفات"), QStringLiteral("sectionTitle"), column), 0, 0);
    lists->addWidget(makeLabel(QStringLiteral("الكاميرات"), QStringLiteral("sectionTitle"), column), 0, 1);
    m_recentLayout = new QVBoxLayout;
    m_recentLayout->setSpacing(0);
    m_cameraLayout = new QVBoxLayout;
    m_cameraLayout->setSpacing(0);
    lists->addLayout(m_recentLayout, 1, 0, Qt::AlignTop);
    lists->addLayout(m_cameraLayout, 1, 1, Qt::AlignTop);
    lists->setColumnStretch(0, 3);
    lists->setColumnStretch(1, 2);
    layout->addLayout(lists);
    layout->addSpacing(4);

    auto *keys = makeLabel(QStringLiteral("اسحب صورة أو فيديو إلى هنا · Ctrl+V للصق صورة · F1 لكل الاختصارات"),
                           QStringLiteral("muted"), column);
    keys->setAlignment(Qt::AlignCenter);
    keys->setWordWrap(true);
    layout->addWidget(keys);
    auto *help = new QToolButton(column);
    help->setObjectName(QStringLiteral("startItem"));
    help->setText(QStringLiteral("عرض الاختصارات"));
    help->setIcon(Icons::icon(Icons::Name::Keyboard, Theme::Accent2, 18));
    help->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    help->setCursor(Qt::PointingHandCursor);
    connect(help, &QToolButton::clicked, this, &StartScreen::shortcutsRequested);
    layout->addWidget(help, 0, Qt::AlignHCenter);

    auto *center = new QHBoxLayout;
    center->addStretch();
    center->addWidget(column, 10);
    center->addStretch();
    outer->addLayout(center);
    outer->addStretch(3);
    setReady(false);
}

QToolButton *StartScreen::tile(Icons::Name icon, const QString &title, const QString &hint)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("startTile"));
    button->setText(title);
    button->setToolTip(hint);
    button->setIcon(Icons::icon(icon, Theme::Accent2, 40));
    button->setIconSize(QSize(40, 40));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setFixedSize(138, 108);
    button->setCursor(Qt::PointingHandCursor);
    m_tiles.append(button);
    return button;
}

QToolButton *StartScreen::listButton(Icons::Name icon, const QString &text, const QString &tip)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("startItem"));
    button->setText(text);
    button->setToolTip(tip);
    button->setIcon(Icons::icon(icon, Theme::Muted, 18));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

void StartScreen::clearList(QVBoxLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void StartScreen::refresh()
{
    clearList(m_recentLayout);
    clearList(m_cameraLayout);

    const QStringList files = QSettings().value(QStringLiteral("recentFiles")).toStringList();
    int shown = 0;
    for (const QString &file : files) {
        if (shown >= MaxRecent)
            break;
        const QFileInfo info(file);
        if (!info.exists())
            continue;
        const bool video = VideoSuffixes.contains(info.suffix().toLower());
        // ⁨…⁩ حتى لا يقلب اسم الملف اللاتيني ترتيب السطر
        QToolButton *button = listButton(video ? Icons::Name::Video : Icons::Name::Image,
                                         QStringLiteral("⁨%1⁩").arg(info.fileName()),
                                         QDir::toNativeSeparators(info.absoluteFilePath()));
        button->setEnabled(m_ready);
        connect(button, &QToolButton::clicked, this, [this, file] { emit fileRequested(file); });
        m_recentLayout->addWidget(button);
        ++shown;
    }
    if (shown == 0)
        m_recentLayout->addWidget(makeLabel(QStringLiteral("لم يُفتح أي ملف بعد"), QStringLiteral("muted"), this));

    // آخر كاميرا أولاً ثم بقية الكاميرات المحفوظة
    const QString last = Cameras::lastSource();
    QList<QPair<QString, QString>> cameras;
    if (!last.isEmpty())
        cameras.append({last, Cameras::displayName(last)});
    for (const Cameras::Camera &camera : Cameras::load()) {
        if (camera.source() != last)
            cameras.append({camera.source(), camera.name});
    }
    for (int i = 0; i < cameras.size() && i < MaxCameras; ++i) {
        const QString source = cameras.at(i).first;
        QToolButton *button = listButton(Icons::Name::Camera, cameras.at(i).second,
                                         i == 0 && source == last ? QStringLiteral("آخر كاميرا استُخدمت (Ctrl+Shift+C)") : QString());
        if (i == 0 && source == last) {
            QFont font = button->font();
            font.setBold(true);
            button->setFont(font);
        }
        button->setEnabled(m_ready);
        connect(button, &QToolButton::clicked, this, [this, source] { emit cameraRequested(source); });
        m_cameraLayout->addWidget(button);
    }
    if (cameras.isEmpty())
        m_cameraLayout->addWidget(makeLabel(QStringLiteral("لا كاميرات محفوظة بعد"), QStringLiteral("muted"), this));
}

void StartScreen::setReady(bool ready)
{
    m_ready = ready;
    for (QToolButton *button : std::as_const(m_tiles))
        button->setEnabled(ready);
    m_status->setText(ready ? QStringLiteral("افتح صورة أو فيديو أو شغّل كاميرا، ليُكشف كل وجه ويُطابق مع قاعدة البيانات")
                            : QStringLiteral("جارٍ تحميل نماذج التعرف… تعمل الأزرار بعد لحظات"));
    if (isVisible())
        refresh();
}

void StartScreen::setDropHighlight(bool on)
{
    if (m_dropHighlight == on)
        return;
    m_dropHighlight = on;
    update();
}

void StartScreen::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
}

void StartScreen::paintEvent(QPaintEvent *)
{
    // إطار متقطع يدل على أن المساحة كلها منطقة إفلات
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(m_dropHighlight ? Theme::Accent2 : Theme::Border, m_dropHighlight ? 2.5 : 1.5, Qt::DashLine);
    pen.setDashPattern({6, 5});
    p.setPen(pen);
    p.setBrush(m_dropHighlight ? Theme::withAlpha(Theme::Accent, 28) : Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(12, 12, -12, -12), 16, 16);
}

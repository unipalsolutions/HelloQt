#include "startscreen.h"

#include "cameras.h"
#include "theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QTextOption>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

constexpr int MaxRecent = 4;
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

// نص يُرسم باتجاه الواجهة: يميناً في العربية ويساراً في غيرها
void drawLine(QPainter &p, const QRectF &rect, const QString &text, Qt::LayoutDirection direction)
{
    QTextOption option(Qt::AlignVCenter | (direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft));
    option.setTextDirection(direction);
    option.setWrapMode(QTextOption::NoWrap);
    p.drawText(rect, p.fontMetrics().elidedText(text, Qt::ElideMiddle, int(rect.width())), option);
}

// حركة ناعمة بين 0 و1 عند مرور الفأرة، يشترك فيها البلاط والصفوف
class HoverButton : public QAbstractButton
{
public:
    explicit HoverButton(QWidget *parent)
        : QAbstractButton(parent)
        , m_animation(new QVariantAnimation(this))
    {
        setCursor(Qt::PointingHandCursor);
        m_animation->setDuration(170);
        m_animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_hover = value.toDouble();
            update();
        });
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QAbstractButton::enterEvent(event);
        animateTo(1.0);
    }
    void leaveEvent(QEvent *event) override
    {
        QAbstractButton::leaveEvent(event);
        animateTo(0.0);
    }
    void changeEvent(QEvent *event) override
    {
        QAbstractButton::changeEvent(event);
        if (event->type() == QEvent::EnabledChange && !isEnabled())
            animateTo(0.0);
    }

    double m_hover = 0.0;

private:
    void animateTo(double target)
    {
        m_animation->stop();
        m_animation->setStartValue(m_hover);
        m_animation->setEndValue(isEnabled() ? target : 0.0);
        m_animation->start();
    }

    QVariantAnimation *m_animation;
};

// بطاقة كبيرة: أيقونة في دائرة مضيئة، والعنوان، والاختصار في مفتاح صغير؛ ترتفع قليلاً ويضيء إطارها عند المرور
class StartTile : public HoverButton
{
public:
    StartTile(Icons::Name icon, const QString &title, const QString &key, QWidget *parent)
        : HoverButton(parent)
        , m_icon(icon)
        , m_key(key)
    {
        setText(title);
        setFixedSize(148, 150);
    }

    // على الشاشات القصيرة: بطاقة أقصر بدائرة أصغر
    void setCompact(bool compact)
    {
        if (m_compact == compact)
            return;
        m_compact = compact;
        setFixedSize(148, compact ? 124 : 150);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool enabled = isEnabled();
        const double lift = 3.0 * m_hover;
        const QRectF card = QRectF(rect()).adjusted(1.5, 4.5 - lift, -1.5, -1.5 - lift);

        // ظل خفيف يكبر مع الارتفاع
        if (m_hover > 0.0) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::withAlpha(Theme::Accent, int(40 * m_hover)));
            p.drawRoundedRect(card.translated(0, 3), 18, 18);
        }
        p.setPen(QPen(Theme::mix(Theme::Border, Theme::Accent2, 0.75 * m_hover), 1.2));
        p.setBrush(Theme::mix(Theme::Surface, Theme::SurfaceHigh, 0.35 + 0.65 * m_hover));
        p.drawRoundedRect(card, 18, 18);

        const QPointF center(card.center().x(), card.top() + (m_compact ? 36 : 46));
        const double radius = (m_compact ? 21 : 26) + 2 * m_hover;
        p.setPen(Qt::NoPen);
        p.setBrush(enabled ? Theme::withAlpha(Theme::Accent, int(55 + 50 * m_hover)) : Theme::withAlpha(Theme::Muted, 30));
        p.drawEllipse(center, radius, radius);
        const QColor iconColor = enabled ? Theme::mix(Theme::Accent2, Theme::Text, 0.3 * m_hover) : Theme::Muted;
        Icons::icon(m_icon, iconColor, 26).paint(&p, QRect(int(center.x()) - 13, int(center.y()) - 13, 26, 26));

        QFont font = this->font();
        font.setPointSizeF(11.5);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(enabled ? Theme::Text : Theme::Muted);
        p.drawText(QRectF(card.left() + 6, card.top() + (m_compact ? 62 : 80), card.width() - 12, 24), Qt::AlignCenter, text());

        // الاختصار في مفتاح بإطار
        QFont keyFont = this->font();
        keyFont.setPointSizeF(8.2);
        p.setFont(keyFont);
        const double keyWidth = p.fontMetrics().horizontalAdvance(m_key) + 14;
        const QRectF key(card.center().x() - keyWidth / 2, card.top() + (m_compact ? 90 : 110), keyWidth, 20);
        p.setPen(QPen(Theme::withAlpha(Theme::Border, 255), 1));
        p.setBrush(Theme::withAlpha(Theme::Background, 160));
        p.drawRoundedRect(key, 6, 6);
        p.setPen(Theme::Muted);
        p.drawText(key, Qt::AlignCenter, m_key);
    }

private:
    Icons::Name m_icon;
    QString m_key;
    bool m_compact = false;
};

// صف في قائمة: صورة مصغرة أو أيقونة، والاسم وتحته سطر خافت، وشارة اختيارية
class StartRow : public HoverButton
{
public:
    StartRow(const QPixmap &thumb, Icons::Name icon, const QString &title, const QString &detail, QWidget *parent)
        : HoverButton(parent)
        , m_thumb(thumb)
        , m_icon(icon)
        , m_detail(detail)
    {
        setText(title);
        setFixedHeight(48);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setBadge(const QString &badge) { m_badge = badge; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        const QRectF row = QRectF(rect()).adjusted(1, 2, -1, -2);
        if (m_hover > 0.0) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::withAlpha(Theme::SurfaceHigh, int(255 * m_hover)));
            p.drawRoundedRect(row, 12, 12);
        }

        const QRectF thumb(rtl ? row.right() - 50 : row.left() + 8, row.center().y() - 21, 42, 42);
        QPainterPath clip;
        clip.addRoundedRect(thumb, 10, 10);
        if (!m_thumb.isNull()) {
            p.save();
            p.setClipPath(clip);
            p.drawPixmap(thumb.toRect(), m_thumb);
            p.restore();
            p.setPen(QPen(Theme::withAlpha(Theme::Text, 30), 1));
            p.setBrush(Qt::NoBrush);
            p.drawPath(clip);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::withAlpha(Theme::Accent, isEnabled() ? 60 : 25));
            p.drawPath(clip);
            Icons::icon(m_icon, isEnabled() ? Theme::Accent2 : Theme::Muted, 20)
                .paint(&p, QRect(int(thumb.center().x()) - 10, int(thumb.center().y()) - 10, 20, 20));
        }

        // الشارة على الطرف المقابل للصورة
        double badgeWidth = 0;
        QFont small = font();
        small.setPointSizeF(8.2);
        if (!m_badge.isEmpty()) {
            p.setFont(small);
            badgeWidth = p.fontMetrics().horizontalAdvance(m_badge) + 16;
            const QRectF badge(rtl ? row.left() + 8 : row.right() - 8 - badgeWidth, row.center().y() - 10, badgeWidth, 20);
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::withAlpha(Theme::Highlight, 40));
            p.drawRoundedRect(badge, 10, 10);
            p.setPen(Theme::Highlight);
            p.drawText(badge, Qt::AlignCenter, m_badge);
            badgeWidth += 8;
        }

        const double textLeft = rtl ? row.left() + 10 + badgeWidth : thumb.right() + 12;
        const double textRight = rtl ? thumb.left() - 12 : row.right() - 10 - badgeWidth;
        const QRectF textRect(textLeft, row.top(), std::max(0.0, textRight - textLeft), row.height());
        const Qt::LayoutDirection direction = layoutDirection();

        QFont bold = font();
        bold.setPointSizeF(10.2);
        bold.setWeight(QFont::DemiBold);
        p.setFont(bold);
        p.setPen(isEnabled() ? Theme::Text : Theme::Muted);
        const bool twoLines = !m_detail.isEmpty();
        drawLine(p, twoLines ? textRect.adjusted(0, 7, 0, -textRect.height() / 2 + 1) : textRect, text(), direction);
        if (twoLines) {
            p.setFont(small);
            p.setPen(Theme::Muted);
            drawLine(p, textRect.adjusted(0, textRect.height() / 2 - 1, 0, -7), m_detail, direction);
        }
    }

private:
    QPixmap m_thumb;
    Icons::Name m_icon;
    QString m_detail;
    QString m_badge;
};

// صورة مصغرة مربعة مقصوصة من الوسط، تُقرأ مصغرة من الملف وتُحفظ حتى يتغير الملف
QPixmap thumbnail(const QFileInfo &info)
{
    static QHash<QString, QPair<QDateTime, QPixmap>> cache;
    const QString path = info.absoluteFilePath();
    const QDateTime modified = info.lastModified();
    const auto cached = cache.constFind(path);
    if (cached != cache.constEnd() && cached->first == modified)
        return cached->second;

    constexpr int Size = 96;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid() && !full.isEmpty())
        reader.setScaledSize(full.scaled(Size, Size, Qt::KeepAspectRatioByExpanding));
    QImage image = reader.read();
    QPixmap pixmap;
    if (!image.isNull()) {
        image = image.scaled(Size, Size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        pixmap = QPixmap::fromImage(image.copy((image.width() - Size) / 2, (image.height() - Size) / 2, Size, Size));
    }
    cache.insert(path, {modified, pixmap});
    return pixmap;
}

// «منذ ساعتين» بدل التاريخ الكامل للملفات الحديثة
QString ago(const QDateTime &time)
{
    const qint64 minutes = time.secsTo(QDateTime::currentDateTime()) / 60;
    if (minutes < 1)
        return QStringLiteral("الآن");
    if (minutes < 60)
        return QStringLiteral("منذ %1 د").arg(minutes);
    if (minutes < 24 * 60)
        return QStringLiteral("منذ %1 س").arg(minutes / 60);
    if (minutes < 7 * 24 * 60)
        return QStringLiteral("منذ %1 يوم").arg(minutes / (24 * 60));
    return QLocale().toString(time.date(), QLocale::ShortFormat);
}

} // namespace

StartScreen::StartScreen(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(QStringLiteral(
        "QFrame#startPanel { background: %1; border: 1px solid %2; border-radius: 18px; }"
        "QLabel#startCount { background: %3; color: %4; border-radius: 9px; padding: 1px 8px; font-size: 8.5pt; }"
        "QLabel#keycap { background: %5; border: 1px solid %2; border-radius: 6px; padding: 1px 7px; color: %6;"
        " font-family: monospace; font-size: 8.5pt; }"
        "QLabel#startHero { font-size: 22pt; font-weight: 800; }"
        "QPushButton#startLink { background: transparent; border: none; color: %7; padding: 2px 4px; min-height: 0; }"
        "QPushButton#startLink:hover { color: %6; text-decoration: underline; }")
                      .arg(Theme::withAlpha(Theme::Surface, 215).name(QColor::HexArgb), Theme::Border.name(),
                           Theme::SurfaceHigh.name(), Theme::Muted.name(), Theme::Background.name(), Theme::Text.name(),
                           Theme::Accent2.name()));

    m_outer = new QVBoxLayout(this);
    QVBoxLayout *outer = m_outer;
    outer->setContentsMargins(36, 24, 36, 24);
    outer->addStretch(2);

    auto *column = new QWidget(this);
    column->setMaximumWidth(820);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // الواجهة: شعار البرنامج والعنوان وسطر الحالة
    m_logo = new QLabel(column);
    m_logo->setPixmap(QApplication::windowIcon().pixmap(56, 56));
    m_logo->setAlignment(Qt::AlignCenter);
    m_logo->setContentsMargins(0, 0, 0, 8);
    layout->addWidget(m_logo);
    auto *title = makeLabel(QStringLiteral("ابدأ من هنا"), QStringLiteral("startHero"), column);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    layout->addSpacing(4);
    m_status = makeLabel(QString(), QStringLiteral("subtitle"), column);
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_gaps.append(new QSpacerItem(0, 22, QSizePolicy::Minimum, QSizePolicy::Fixed));
    layout->addItem(m_gaps.last());

    auto *tiles = new QHBoxLayout;
    tiles->setSpacing(12);
    QAbstractButton *image = tile(Icons::Name::Image, QStringLiteral("صورة"), QStringLiteral("Ctrl+O"));
    QAbstractButton *video = tile(Icons::Name::Video, QStringLiteral("فيديو"), QStringLiteral("Ctrl+Shift+O"));
    QAbstractButton *camera = tile(Icons::Name::Camera, QStringLiteral("كاميرا"), QStringLiteral("Ctrl+Shift+C"));
    QAbstractButton *folder = tile(Icons::Name::Folder, QStringLiteral("مجلد صور"), QStringLiteral("Ctrl+Shift+F"));
    QAbstractButton *archive = tile(Icons::Name::Film, QStringLiteral("أرشيف تسجيلات"), QStringLiteral("Ctrl+Shift+A"));
    image->setToolTip(QStringLiteral("فتح صورة والتعرف على كل وجه فيها"));
    video->setToolTip(QStringLiteral("فتح فيديو وتحليله وجهاً وجهاً على خط زمني"));
    camera->setToolTip(QStringLiteral("تشغيل آخر كاميرا، أو اختيار كاميرا"));
    folder->setToolTip(QStringLiteral("البحث عن الأشخاص في كل صور مجلد"));
    archive->setToolTip(QStringLiteral("مسح مجلد تسجيلات كاميرات كاملاً"));
    connect(image, &QAbstractButton::clicked, this, &StartScreen::openImageRequested);
    connect(video, &QAbstractButton::clicked, this, &StartScreen::openVideoRequested);
    connect(camera, &QAbstractButton::clicked, this, &StartScreen::openCameraRequested);
    connect(folder, &QAbstractButton::clicked, this, &StartScreen::openFolderRequested);
    connect(archive, &QAbstractButton::clicked, this, &StartScreen::openArchiveRequested);
    tiles->addStretch();
    for (QAbstractButton *button : std::as_const(m_tiles))
        tiles->addWidget(button);
    tiles->addStretch();
    layout->addLayout(tiles);
    m_gaps.append(new QSpacerItem(0, 22, QSizePolicy::Minimum, QSizePolicy::Fixed));
    layout->addItem(m_gaps.last());

    // آخر الملفات والكاميرات في لوحتين متجاورتين
    auto *lists = new QHBoxLayout;
    lists->setSpacing(14);
    lists->addWidget(panel(Icons::Name::History, QStringLiteral("آخر الملفات"), &m_recentLayout, &m_recentCount), 3);
    lists->addWidget(panel(Icons::Name::Camera, QStringLiteral("الكاميرات"), &m_cameraLayout, &m_cameraCount), 2);
    layout->addLayout(lists);
    m_gaps.append(new QSpacerItem(0, 18, QSizePolicy::Minimum, QSizePolicy::Fixed));
    layout->addItem(m_gaps.last());

    // أهم الاختصارات في مفاتيح صغيرة
    auto *keys = new QHBoxLayout;
    keys->setSpacing(6);
    keys->addStretch();
    const QList<QPair<QString, QString>> hints = {{QStringLiteral("اسحب ملفاً إلى هنا"), QString()},
                                                  {QStringLiteral("أو الصق صورة"), QStringLiteral("Ctrl+V")}};
    for (const auto &hint : hints) {
        keys->addWidget(makeLabel(hint.first, QStringLiteral("muted"), column));
        if (!hint.second.isEmpty())
            keys->addWidget(makeLabel(hint.second, QStringLiteral("keycap"), column));
        keys->addSpacing(10);
    }
    keys->addWidget(makeLabel(QStringLiteral("F1"), QStringLiteral("keycap"), column));
    auto *help = new QPushButton(QStringLiteral("كل الاختصارات"), column);
    help->setObjectName(QStringLiteral("startLink"));
    help->setCursor(Qt::PointingHandCursor);
    connect(help, &QPushButton::clicked, this, &StartScreen::shortcutsRequested);
    keys->addWidget(help);
    keys->addStretch();
    layout->addLayout(keys);

    auto *center = new QHBoxLayout;
    center->addStretch();
    center->addWidget(column, 10);
    center->addStretch();
    outer->addLayout(center);
    outer->addStretch(3);
    setReady(false);
}

QAbstractButton *StartScreen::tile(Icons::Name icon, const QString &title, const QString &hint)
{
    auto *button = new StartTile(icon, title, hint, this);
    m_tiles.append(button);
    return button;
}

QWidget *StartScreen::panel(Icons::Name icon, const QString &title, QVBoxLayout **list, QLabel **count)
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("startPanel"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(4);
    auto *header = new QHBoxLayout;
    header->setContentsMargins(6, 0, 6, 4);
    header->setSpacing(8);
    auto *iconLabel = new QLabel(frame);
    iconLabel->setPixmap(Icons::icon(icon, Theme::Accent2, 18).pixmap(18, 18));
    header->addWidget(iconLabel);
    header->addWidget(makeLabel(title, QStringLiteral("sectionTitle"), frame));
    *count = makeLabel(QString(), QStringLiteral("startCount"), frame);
    header->addWidget(*count);
    header->addStretch();
    layout->addLayout(header);
    *list = new QVBoxLayout;
    (*list)->setSpacing(0);
    layout->addLayout(*list);
    layout->addStretch();
    return frame;
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
    const QVariantMap opened = QSettings().value(QStringLiteral("recentOpened")).toMap();
    int shown = 0;
    QStringList seen;
    for (const QString &file : files) {
        if (shown >= MaxRecent)
            break;
        const QFileInfo info(file);
        if (!info.exists() || seen.contains(info.absoluteFilePath()))
            continue;
        seen.append(info.absoluteFilePath());
        const bool video = VideoSuffixes.contains(info.suffix().toLower());
        const QDateTime openedAt = opened.value(info.absoluteFilePath(), info.lastModified()).toDateTime();
        // ⁨…⁩ حتى لا يقلب اسم الملف اللاتيني ترتيب السطر
        auto *row = new StartRow(video ? QPixmap() : thumbnail(info), video ? Icons::Name::Video : Icons::Name::Image,
                                 QStringLiteral("⁨%1⁩").arg(info.fileName()),
                                 QStringLiteral("%1 · ⁨%2⁩").arg(ago(openedAt), info.dir().dirName()), this);
        row->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
        row->setEnabled(m_ready);
        connect(row, &QAbstractButton::clicked, this, [this, file] { emit fileRequested(file); });
        m_recentLayout->addWidget(row);
        ++shown;
    }
    m_recentCount->setText(QString::number(shown));
    m_recentCount->setVisible(shown > 0);
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
    const int cameraCount = std::min(int(cameras.size()), MaxCameras);
    for (int i = 0; i < cameraCount; ++i) {
        const QString source = cameras.at(i).first;
        const bool isLast = i == 0 && source == last;
        auto *row = new StartRow(QPixmap(), Icons::Name::Camera, cameras.at(i).second, QString(), this);
        if (isLast) {
            row->setBadge(QStringLiteral("الأخيرة"));
            row->setToolTip(QStringLiteral("آخر كاميرا استُخدمت (Ctrl+Shift+C)"));
        }
        row->setEnabled(m_ready);
        connect(row, &QAbstractButton::clicked, this, [this, source] { emit cameraRequested(source); });
        m_cameraLayout->addWidget(row);
    }
    m_cameraCount->setText(QString::number(cameraCount));
    m_cameraCount->setVisible(cameraCount > 0);
    if (cameras.isEmpty())
        m_cameraLayout->addWidget(makeLabel(QStringLiteral("لا كاميرات محفوظة بعد"), QStringLiteral("muted"), this));
}

void StartScreen::setReady(bool ready)
{
    m_ready = ready;
    for (QAbstractButton *button : std::as_const(m_tiles))
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

void StartScreen::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // على الشاشات القصيرة يُخفى الشعار وتقصر البطاقات والفراغات حتى تبقى القوائم كاملة
    m_logo->setVisible(height() >= 690);
    const bool compact = height() < 615;
    for (QAbstractButton *button : std::as_const(m_tiles))
        static_cast<StartTile *>(button)->setCompact(compact);
    for (QSpacerItem *gap : std::as_const(m_gaps))
        gap->changeSize(0, compact ? 12 : 20, QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_outer->setContentsMargins(36, compact ? 14 : 24, 36, compact ? 14 : 24);
    m_outer->invalidate();
}

void StartScreen::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
}

void StartScreen::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath clip;
    clip.addRoundedRect(area, 15, 15);
    p.setClipPath(clip);

    // خلفية دافئة بتوهج أخضر خافت خلف العنوان
    p.fillRect(rect(), Theme::Background);
    QRadialGradient glow(QPointF(width() / 2.0, height() * 0.18), std::max(width(), height()) * 0.6);
    glow.setColorAt(0.0, Theme::withAlpha(Theme::Accent, 46));
    glow.setColorAt(0.6, Theme::withAlpha(Theme::Accent, 8));
    glow.setColorAt(1.0, Qt::transparent);
    p.fillRect(rect(), glow);

    // الإطار المتقطع يظهر بارزاً أثناء السحب فقط، وإلا يبقى خافتاً
    QPen pen(m_dropHighlight ? Theme::Accent2 : Theme::withAlpha(Theme::Border, 150), m_dropHighlight ? 2.5 : 1.2, Qt::DashLine);
    pen.setDashPattern({6, 5});
    p.setPen(pen);
    // QBrush صريح: Qt::NoBrush في عبارة شرطية مع QColor يتحول إلى لون أسود معتم
    p.setBrush(m_dropHighlight ? QBrush(Theme::withAlpha(Theme::Accent, 28)) : QBrush(Qt::NoBrush));
    p.drawRoundedRect(area.adjusted(10, 10, -10, -10), 16, 16);
}

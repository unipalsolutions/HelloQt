#include "matchpage.h"

#include "carddelegates.h"
#include "facedatabase.h"
#include "faceengine.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>

using namespace Theme;

namespace {

// ألوان الحكم: نفس الشخص، محتمل، مختلف
const QColor SameColor = Theme::Success;
const QColor PossibleColor = Theme::Accent;
const QColor DifferentColor(0xfb, 0x71, 0x85);

QLabel *makeLabel(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(objectName);
    return l;
}

QFrame *makeCard(QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("card"));
    return frame;
}

bool isImageFile(const QString &path)
{
    static const QStringList suffixes = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                                         QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("tif"),
                                         QStringLiteral("tiff"), QStringLiteral("jfif")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

// الوجه الافتراضي: الأكبر والأوضح
int bestFaceIndex(const QList<FaceItem> &faces)
{
    int best = -1;
    double bestScore = -1;
    for (int i = 0; i < faces.size(); ++i) {
        const double area = faces[i].box.width() * faces[i].box.height();
        const double score = std::sqrt(area) * (0.4 + faces[i].quality);
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

} // namespace

// ───────────── خانة صورة ─────────────

// خانة تقبل صورة بالسحب أو النقر، وتعرضها مع إطارات الوجوه؛ النقر على وجه يختاره للمقارنة
class FaceSlot : public QWidget
{
public:
    FaceSlot(const QString &title, QWidget *parent)
        : QWidget(parent)
        , m_title(title)
    {
        setAcceptDrops(true);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setMinimumSize(200, 200);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    std::function<void()> onBrowse;
    std::function<void(const QString &)> onFile;
    std::function<void(int)> onFace;

    void setBusy(bool busy)
    {
        m_busy = busy;
        update();
    }
    void setImage(const QImage &image, const QList<QRectF> &boxes, int selected, const QString &message)
    {
        m_image = image;
        m_boxes = boxes;
        m_selected = selected;
        m_message = message;
        update();
    }
    void setSelected(int selected)
    {
        m_selected = selected;
        update();
    }
    void clear() { setImage({}, {}, -1, {}); }
    bool isEmpty() const { return m_image.isNull() && !m_busy; }

    QSize sizeHint() const override { return {320, 300}; }

protected:
    QRectF imageRect() const
    {
        const QRectF area = QRectF(rect()).adjusted(10, 10, -10, -10);
        if (m_image.isNull())
            return area;
        QSizeF size = m_image.size();
        size.scale(area.size(), Qt::KeepAspectRatio);
        return QRectF(area.center() - QPointF(size.width() / 2, size.height() / 2), size);
    }
    QRectF mapBox(const QRectF &box) const
    {
        const QRectF target = imageRect();
        const double s = target.width() / std::max(1, m_image.width());
        return QRectF(target.left() + box.left() * s, target.top() + box.top() * s, box.width() * s, box.height() * s);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
        QPainterPath shape;
        shape.addRoundedRect(bounds, 14, 14);
        p.fillPath(shape, mix(Background, Surface, 0.5 + 0.3 * m_hover));

        if (m_image.isNull()) {
            QPen pen(m_dragging ? Accent2 : mix(Border, Accent, m_hover), m_dragging ? 2.0 : 1.5, Qt::DashLine);
            pen.setDashPattern({5, 4});
            p.setPen(pen);
            p.drawPath(shape);
            const QPointF c = bounds.center();
            QRectF circle(c.x() - 34, c.y() - 62, 68, 68);
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(Accent, 40 + int(30 * m_hover)));
            p.drawEllipse(circle);
            Icons::paint(p, Icons::Name::PhotoAdd, circle.adjusted(18, 18, -18, -18), Accent2);
            QFont font = p.font();
            font.setPointSizeF(font.pointSizeF() + 1.5);
            font.setWeight(QFont::DemiBold);
            p.setFont(font);
            p.setPen(Text);
            p.drawText(QRectF(bounds.left() + 10, c.y() + 14, bounds.width() - 20, 26), Qt::AlignCenter, m_title);
            font.setPointSizeF(font.pointSizeF() - 2.5);
            font.setWeight(QFont::Normal);
            p.setFont(font);
            p.setPen(Muted);
            p.drawText(QRectF(bounds.left() + 10, c.y() + 40, bounds.width() - 20, 40), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                       m_busy ? QStringLiteral("جارٍ التحليل…") : QStringLiteral("اسحب صورة إلى هنا أو انقر للاختيار"));
            return;
        }

        const QRectF target = imageRect();
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(target, 10, 10);
        p.setClipPath(clip);
        p.drawImage(target, m_image);
        // تعتيم ما حول الوجه المختار حتى يبرز
        if (m_selected >= 0 && m_selected < m_boxes.size()) {
            QPainterPath dim;
            dim.addRect(target);
            QPainterPath hole;
            hole.addRoundedRect(mapBox(m_boxes[m_selected]).adjusted(-6, -6, 6, 6), 8, 8);
            p.fillPath(dim.subtracted(hole), QColor(4, 10, 22, 110));
        }
        p.restore();

        for (int i = 0; i < m_boxes.size(); ++i) {
            const QRectF r = mapBox(m_boxes[i]);
            const bool selected = i == m_selected;
            const bool hovered = r.contains(m_mouse);
            if (selected) {
                // زوايا تركيز حول الوجه المختار
                const double len = std::min(r.width(), r.height()) * 0.28;
                p.setPen(QPen(SameColor, 3, Qt::SolidLine, Qt::RoundCap));
                const QRectF b = r.adjusted(-3, -3, 3, 3);
                p.drawLine(b.topLeft(), b.topLeft() + QPointF(len, 0));
                p.drawLine(b.topLeft(), b.topLeft() + QPointF(0, len));
                p.drawLine(b.topRight(), b.topRight() - QPointF(len, 0));
                p.drawLine(b.topRight(), b.topRight() + QPointF(0, len));
                p.drawLine(b.bottomLeft(), b.bottomLeft() + QPointF(len, 0));
                p.drawLine(b.bottomLeft(), b.bottomLeft() - QPointF(0, len));
                p.drawLine(b.bottomRight(), b.bottomRight() - QPointF(len, 0));
                p.drawLine(b.bottomRight(), b.bottomRight() - QPointF(0, len));
            } else {
                p.setPen(QPen(hovered ? Accent2 : QColor(255, 255, 255, 150), hovered ? 2 : 1.4, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r, 4, 4);
            }
        }

        // تلميح أعلى الخانة وعدد الوجوه
        auto pill = [&](const QString &text, const QPointF &anchor, bool top, const QColor &color) {
            QFont font = p.font();
            font.setWeight(QFont::DemiBold);
            p.setFont(font);
            const double w = QFontMetricsF(font).horizontalAdvance(text) + 22;
            const QRectF r(anchor.x() - w / 2, top ? anchor.y() : anchor.y() - 28, w, 28);
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(Background, 215));
            p.drawRoundedRect(r, 14, 14);
            p.setPen(color);
            p.drawText(r, Qt::AlignCenter, text);
        };
        if (m_busy)
            pill(QStringLiteral("جارٍ التحليل…"), QPointF(bounds.center().x(), bounds.top() + 14), true, Accent2);
        else if (!m_message.isEmpty())
            pill(m_message, QPointF(bounds.center().x(), bounds.bottom() - 14), false, m_boxes.isEmpty() ? DifferentColor : Muted);
        if (m_hover > 0.5 && !m_busy)
            pill(QStringLiteral("انقر خارج الوجوه لتغيير الصورة"), QPointF(bounds.center().x(), bounds.top() + 14), true, Text);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_busy)
            return;
        if (!m_image.isNull()) {
            for (int i = 0; i < m_boxes.size(); ++i) {
                if (mapBox(m_boxes[i]).adjusted(-6, -6, 6, 6).contains(event->position())) {
                    if (onFace)
                        onFace(i);
                    return;
                }
            }
        }
        if (onBrowse)
            onBrowse();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_mouse = event->position();
        update();
    }
    void enterEvent(QEnterEvent *) override
    {
        m_hover = 1.0;
        update();
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = 0.0;
        m_mouse = {-1, -1};
        update();
    }
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        const QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile() && isImageFile(urls.first().toLocalFile())) {
            m_dragging = true;
            update();
            event->acceptProposedAction();
        }
    }
    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_dragging = false;
        update();
    }
    void dropEvent(QDropEvent *event) override
    {
        m_dragging = false;
        update();
        const QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty() && onFile) {
            event->acceptProposedAction();
            onFile(urls.first().toLocalFile());
        }
    }

private:
    QString m_title;
    QImage m_image;
    QList<QRectF> m_boxes;
    int m_selected = -1;
    QString m_message;
    bool m_busy = false;
    bool m_dragging = false;
    double m_hover = 0.0;
    QPointF m_mouse{-1, -1};
};

// ───────────── مقياس التشابه ─────────────

// قوس متدرج يعرض نسبة التشابه ومناطق الحكم الثلاث، مع حركة ناعمة عند التغير
class SimilarityGauge : public QWidget
{
public:
    explicit SimilarityGauge(QWidget *parent)
        : QWidget(parent)
        , m_animation(new QVariantAnimation(this))
    {
        setMinimumSize(200, 150);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_animation->setDuration(650);
        m_animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_shown = v.toDouble();
            update();
        });
    }

    void setThreshold(double threshold)
    {
        m_threshold = threshold;
        update();
    }
    // سالب = لا قيمة بعد
    void setValue(double value)
    {
        m_value = value;
        m_animation->stop();
        if (value < 0) {
            m_shown = -1;
            update();
            return;
        }
        m_animation->setStartValue(std::max(0.0, m_shown));
        m_animation->setEndValue(value);
        m_animation->start();
    }

    QSize sizeHint() const override { return {240, 170}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double size = std::min(width() - 20.0, (height() - 10.0) * 1.6);
        const QRectF arcRect(width() / 2.0 - size / 2, 10, size, size);
        const double thick = std::max(10.0, size * 0.075);
        // القوس 220 درجة يبدأ من أسفل اليسار
        const double startAngle = 200, span = -220;
        auto angleOf = [&](double v) { return startAngle + span * std::clamp(v, 0.0, 1.0); };
        auto pointAt = [&](double v, double radiusOffset) {
            const double a = qDegreesToRadians(angleOf(v));
            const double r = size / 2 + radiusOffset;
            const QPointF c = arcRect.center();
            return QPointF(c.x() + r * std::cos(a), c.y() - r * std::sin(a));
        };
        const QRectF track = arcRect.adjusted(thick / 2, thick / 2, -thick / 2, -thick / 2);
        auto arc = [&](double from, double to, const QColor &color, double width) {
            QPainterPath path;
            path.arcMoveTo(track, angleOf(from));
            path.arcTo(track, angleOf(from), angleOf(to) - angleOf(from));
            p.strokePath(path, QPen(color, width, Qt::SolidLine, Qt::FlatCap));
        };
        // مناطق الحكم بخفوت
        const double possible = std::max(0.0, m_threshold - 0.1);
        arc(0, possible, withAlpha(DifferentColor, 45), thick);
        arc(possible, m_threshold, withAlpha(PossibleColor, 60), thick);
        arc(m_threshold, 1.0, withAlpha(SameColor, 45), thick);

        const double v = m_shown;
        const QColor color = v >= m_threshold ? SameColor : (v >= possible ? PossibleColor : DifferentColor);
        if (v > 0.001) {
            QPainterPath path;
            path.arcMoveTo(track, angleOf(0));
            path.arcTo(track, angleOf(0), angleOf(v) - angleOf(0));
            p.strokePath(path, QPen(QBrush(color), thick, Qt::SolidLine, Qt::RoundCap));
            // نقطة متوهجة عند الطرف
            const QPointF tip = pointAt(v, -thick / 2);
            QRadialGradient glow(tip, thick * 1.6);
            glow.setColorAt(0, withAlpha(color, 170));
            glow.setColorAt(1, withAlpha(color, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawEllipse(tip, thick * 1.6, thick * 1.6);
            p.setBrush(QColor(255, 255, 255));
            p.drawEllipse(tip, thick * 0.32, thick * 0.32);
        }
        // علامة حد التأكيد
        p.setPen(QPen(Text, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(pointAt(m_threshold, 2), pointAt(m_threshold, -thick - 3));

        // النسبة في الوسط
        QFont font = p.font();
        font.setPointSizeF(size * 0.16);
        font.setWeight(QFont::Bold);
        p.setFont(font);
        p.setPen(v >= 0 ? Text : Muted);
        const QRectF center(arcRect.left(), arcRect.center().y() - size * 0.2, arcRect.width(), size * 0.26);
        p.drawText(center, Qt::AlignCenter, v >= 0 ? QStringLiteral("%1%").arg(qRound(v * 100)) : QStringLiteral("—"));
        font.setPointSizeF(std::max(8.0, size * 0.055));
        font.setWeight(QFont::Normal);
        p.setFont(font);
        p.setPen(Muted);
        p.drawText(QRectF(arcRect.left(), center.bottom(), arcRect.width(), size * 0.1), Qt::AlignCenter, QStringLiteral("نسبة التشابه"));
    }

private:
    QVariantAnimation *m_animation;
    double m_threshold = 0.3;
    double m_value = -1;
    double m_shown = -1;
};

// ───────────── الصفحة ─────────────

MatchPage::MatchPage(const FaceEngine *engine, FaceDatabase *database, std::function<float()> threshold, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_database(database)
    , m_threshold(std::move(threshold))
{
    m_lastDirectory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(16);

    // العنوان وأزرار القسمين
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(makeLabel(QStringLiteral("المطابقة"), QStringLiteral("title"), this));
    m_subtitle = makeLabel(QStringLiteral("قارن بين وجهين، أو ابحث في القاعدة عن أقرب الأشخاص لصورة"), QStringLiteral("subtitle"), this);
    titles->addWidget(m_subtitle);
    header->addLayout(titles, 1);
    auto *compareTab = new FancyButton(Icons::Name::Compare, QStringLiteral("مقارنة صورتين"), FancyButton::Kind::Nav, this);
    auto *searchTab = new FancyButton(Icons::Name::Search, QStringLiteral("البحث في القاعدة"), FancyButton::Kind::Nav, this);
    for (FancyButton *tab : {compareTab, searchTab}) {
        tab->setFixedWidth(200);
        tab->setAutoExclusive(true);
        header->addWidget(tab);
    }
    m_tabs = {compareTab, searchTab};
    compareTab->setChecked(true);
    layout->addLayout(header);

    m_stack = new AnimatedStack(this);
    layout->addWidget(m_stack, 1);
    connect(compareTab, &QAbstractButton::clicked, this, [this] { m_stack->slideTo(0); });
    connect(searchTab, &QAbstractButton::clicked, this, [this] { m_stack->slideTo(1); });

    for (int i = 0; i < SlotCount; ++i) {
        m_watchers[i] = new QFutureWatcher<MatchImage>(this);
        connect(m_watchers[i], &QFutureWatcherBase::finished, this, [this, i] { onLoaded(i); });
    }

    // ── مقارنة صورتين ──
    auto *compareCard = makeCard(m_stack);
    auto *compareLayout = new QVBoxLayout(compareCard);
    compareLayout->setContentsMargins(20, 16, 20, 18);
    compareLayout->setSpacing(12);
    auto *compareHeader = new QHBoxLayout;
    compareHeader->addWidget(makeLabel(QStringLiteral("هل هما الشخص نفسه؟"), QStringLiteral("sectionTitle"), compareCard));
    auto *compareHint = makeLabel(QStringLiteral("إن كان في الصورة أكثر من وجه فانقر على الوجه المطلوب"), QStringLiteral("muted"), compareCard);
    compareHeader->addWidget(compareHint, 1);
    auto *clearButton = new FancyButton(Icons::Name::Trash, QStringLiteral("مسح"), FancyButton::Kind::Secondary, compareCard);
    connect(clearButton, &QAbstractButton::clicked, this, &MatchPage::clearComparison);
    compareHeader->addWidget(clearButton);
    compareLayout->addLayout(compareHeader);

    auto *compareBody = new QHBoxLayout;
    compareBody->setSpacing(18);
    m_slots[First] = new FaceSlot(QStringLiteral("الصورة الأولى"), compareCard);
    m_slots[Second] = new FaceSlot(QStringLiteral("الصورة الثانية"), compareCard);

    auto *middle = new QWidget(compareCard);
    middle->setFixedWidth(270);
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->setSpacing(8);
    middleLayout->addStretch();
    auto *avatars = new QHBoxLayout;
    avatars->setSpacing(10);
    m_avatarA = new QLabel(middle);
    m_avatarB = new QLabel(middle);
    auto *link = new QLabel(middle);
    link->setPixmap(Icons::icon(Icons::Name::Compare, Muted, 22).pixmap(22, 22));
    for (QLabel *avatar : {m_avatarA, m_avatarB}) {
        avatar->setFixedSize(72, 72);
        avatar->setAlignment(Qt::AlignCenter);
    }
    avatars->addStretch();
    avatars->addWidget(m_avatarA);
    avatars->addWidget(link);
    avatars->addWidget(m_avatarB);
    avatars->addStretch();
    middleLayout->addLayout(avatars);
    m_gauge = new SimilarityGauge(middle);
    middleLayout->addWidget(m_gauge);
    m_verdict = makeLabel(QString(), QStringLiteral("sectionTitle"), middle);
    m_verdict->setAlignment(Qt::AlignCenter);
    middleLayout->addWidget(m_verdict);
    m_verdictDetail = makeLabel(QString(), QStringLiteral("muted"), middle);
    m_verdictDetail->setAlignment(Qt::AlignCenter);
    m_verdictDetail->setWordWrap(true);
    middleLayout->addWidget(m_verdictDetail);
    middleLayout->addStretch();

    compareBody->addWidget(m_slots[First], 1);
    compareBody->addWidget(middle);
    compareBody->addWidget(m_slots[Second], 1);
    compareLayout->addLayout(compareBody, 1);
    m_stack->addWidget(compareCard);

    // ── البحث في القاعدة ──
    auto *searchCard = makeCard(m_stack);
    auto *searchLayout = new QHBoxLayout(searchCard);
    searchLayout->setContentsMargins(20, 16, 20, 18);
    searchLayout->setSpacing(18);
    auto *queryColumn = new QVBoxLayout;
    queryColumn->setSpacing(10);
    queryColumn->addWidget(makeLabel(QStringLiteral("صورة البحث"), QStringLiteral("sectionTitle"), searchCard));
    m_slots[Query] = new FaceSlot(QStringLiteral("صورة الشخص المطلوب"), searchCard);
    queryColumn->addWidget(m_slots[Query], 1);
    searchLayout->addLayout(queryColumn, 5);

    auto *resultsColumn = new QVBoxLayout;
    resultsColumn->setSpacing(10);
    resultsColumn->addWidget(makeLabel(QStringLiteral("أقرب الأشخاص في القاعدة"), QStringLiteral("sectionTitle"), searchCard));
    m_resultsHint = makeLabel(QStringLiteral("اختر صورة لعرض أقرب عشرة أشخاص إليها مع نسبة التشابه"), QStringLiteral("muted"), searchCard);
    m_resultsHint->setWordWrap(true);
    resultsColumn->addWidget(m_resultsHint);
    m_results = new QListWidget(searchCard);
    m_results->setItemDelegate(new FaceCardDelegate(m_results));
    m_results->setUniformItemSizes(true);
    m_results->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_results->setMouseTracking(true);
    resultsColumn->addWidget(m_results, 1);
    auto *actions = new QHBoxLayout;
    m_openPerson = new FancyButton(Icons::Name::Users, QStringLiteral("فتح الملف"), FancyButton::Kind::Secondary, searchCard);
    m_enroll = new FancyButton(Icons::Name::PhotoAdd, QStringLiteral("إضافة الصورة لملفه"), FancyButton::Kind::Primary, searchCard);
    m_enroll->setToolTip(QStringLiteral("يضيف وجه صورة البحث إلى صور الشخص المختار فيتعرف عليه البرنامج أسرع مستقبلاً"));
    actions->addWidget(m_openPerson);
    actions->addWidget(m_enroll);
    actions->addStretch();
    resultsColumn->addLayout(actions);
    searchLayout->addLayout(resultsColumn, 6);
    m_stack->addWidget(searchCard);

    auto currentResult = [this] { return m_results->currentItem() ? m_results->currentItem()->data(CardRole::Id).toString() : QString(); };
    connect(m_results, &QListWidget::currentRowChanged, this, [this] {
        const bool has = m_results->currentItem();
        m_openPerson->setEnabled(has);
        m_enroll->setEnabled(has && selectedFace(Query));
    });
    connect(m_results, &QListWidget::itemDoubleClicked, this, [this, currentResult] { emit openPersonRequested(currentResult()); });
    connect(m_openPerson, &QAbstractButton::clicked, this, [this, currentResult] {
        if (!currentResult().isEmpty())
            emit openPersonRequested(currentResult());
    });
    connect(m_enroll, &QAbstractButton::clicked, this, [this, currentResult] {
        const FaceItem *face = selectedFace(Query);
        if (face && !currentResult().isEmpty())
            emit enrollRequested(currentResult(), *face);
    });
    m_openPerson->setEnabled(false);
    m_enroll->setEnabled(false);

    for (int i = 0; i < SlotCount; ++i) {
        m_slots[i]->onBrowse = [this, i] { browse(i); };
        m_slots[i]->onFile = [this, i](const QString &path) { load(i, path); };
        m_slots[i]->onFace = [this, i](int index) { selectFace(i, index); };
    }
    updateComparison();
}

MatchPage::~MatchPage()
{
    for (auto *watcher : m_watchers)
        watcher->waitForFinished();
}

void MatchPage::setReady(bool ready)
{
    m_ready = ready;
    setEnabled(ready);
}

void MatchPage::setCompact(bool compact)
{
    m_subtitle->setVisible(!compact);
    for (FancyButton *tab : std::as_const(m_tabs))
        tab->setFixedWidth(compact ? 170 : 200);
}

void MatchPage::refresh()
{
    updateComparison();
    updateSearch();
}

void MatchPage::openFile(const QString &path)
{
    if (m_stack->currentIndex() == 1) {
        load(Query, path);
        return;
    }
    load(m_slots[First]->isEmpty() ? First : Second, path);
}

void MatchPage::browse(int slot)
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("اختر صورة"), m_lastDirectory,
                                                      QStringLiteral("الصور (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff *.jfif)"));
    if (!path.isEmpty())
        load(slot, path);
}

void MatchPage::load(int slot, const QString &path)
{
    if (!m_ready || m_watchers[slot]->isRunning())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    m_images[slot] = {};
    m_selected[slot] = -1;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    m_slots[slot]->setImage(reader.read(), {}, -1, {});
    m_slots[slot]->setBusy(true);
    if (slot == Query)
        updateSearch();
    else
        updateComparison();

    const FaceEngine *engine = m_engine;
    m_watchers[slot]->setFuture(QtConcurrent::run([engine, path] {
        MatchImage result;
        result.path = path;
        const cv::Mat image = ImageIO::loadImage(path);
        if (image.empty()) {
            result.error = QStringLiteral("تعذر قراءة الصورة");
            return result;
        }
        const std::vector<AnalyzedFace> faces = engine->analyzeRobust(image);
        result.image = ImageIO::toQImage(image);
        for (size_t i = 0; i < faces.size(); ++i)
            result.faces.append(ImageIO::makeFaceItem(image, faces[i], int(i) + 1));
        return result;
    }));
}

void MatchPage::onLoaded(int slot)
{
    m_images[slot] = m_watchers[slot]->result();
    const MatchImage &image = m_images[slot];
    m_slots[slot]->setBusy(false);
    QList<QRectF> boxes;
    for (const FaceItem &face : image.faces)
        boxes << face.box;
    m_selected[slot] = bestFaceIndex(image.faces);
    QString message;
    if (!image.error.isEmpty())
        message = image.error;
    else if (image.faces.isEmpty())
        message = QStringLiteral("لم يُعثر على وجه في الصورة");
    else if (image.faces.size() > 1)
        message = QStringLiteral("%1 وجوه · انقر على الوجه المطلوب").arg(image.faces.size());
    m_slots[slot]->setImage(image.image, boxes, m_selected[slot], message);
    if (slot == Query)
        updateSearch();
    else
        updateComparison();
}

void MatchPage::selectFace(int slot, int index)
{
    if (index < 0 || index >= m_images[slot].faces.size())
        return;
    m_selected[slot] = index;
    m_slots[slot]->setSelected(index);
    if (slot == Query)
        updateSearch();
    else
        updateComparison();
}

const FaceItem *MatchPage::selectedFace(int slot) const
{
    const int index = m_selected[slot];
    if (index < 0 || index >= m_images[slot].faces.size())
        return nullptr;
    return &m_images[slot].faces[index];
}

void MatchPage::clearComparison()
{
    for (int slot : {First, Second}) {
        if (m_watchers[slot]->isRunning())
            continue;
        m_images[slot] = {};
        m_selected[slot] = -1;
        m_slots[slot]->clear();
    }
    updateComparison();
}

void MatchPage::updateComparison()
{
    const float threshold = m_threshold();
    m_gauge->setThreshold(threshold);
    const FaceItem *a = selectedFace(First);
    const FaceItem *b = selectedFace(Second);
    auto avatar = [](QLabel *label, const FaceItem *face) {
        if (face) {
            label->setPixmap(circularPixmap(ImageIO::faceCrop(*face), 72));
        } else {
            QPixmap empty(144, 144);
            empty.setDevicePixelRatio(2.0);
            empty.fill(Qt::transparent);
            QPainter p(&empty);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(Border, 1.5, Qt::DashLine));
            p.setBrush(Surface);
            p.drawEllipse(QRectF(2, 2, 68, 68));
            Icons::paint(p, Icons::Name::Users, QRectF(24, 24, 24, 24), Muted);
            p.end();
            label->setPixmap(empty);
        }
    };
    avatar(m_avatarA, a);
    avatar(m_avatarB, b);

    if (!a || !b) {
        m_gauge->setValue(-1);
        m_verdict->setText(QStringLiteral("أضف صورتين للمقارنة"));
        m_verdict->setStyleSheet(QString());
        m_verdictDetail->setText(QStringLiteral("حد التأكيد الحالي %1% ويمكن تغييره من صفحة التعرف").arg(qRound(threshold * 100)));
        return;
    }
    const float score = std::max(0.0f, FaceEngine::cosine(a->embedding, b->embedding));
    m_gauge->setValue(score);
    QString verdict;
    QColor color;
    if (score >= threshold) {
        verdict = QStringLiteral("الشخص نفسه");
        color = SameColor;
    } else if (score >= threshold - 0.1f) {
        verdict = QStringLiteral("تشابه محتمل");
        color = PossibleColor;
    } else {
        verdict = QStringLiteral("شخصان مختلفان");
        color = DifferentColor;
    }
    m_verdict->setText(verdict);
    m_verdict->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    QString detail = QStringLiteral("حد التأكيد %1% · جودة الوجهين %2% و%3%")
                         .arg(qRound(threshold * 100))
                         .arg(qRound(a->quality * 100))
                         .arg(qRound(b->quality * 100));
    if (std::min(a->quality, b->quality) < 0.35f)
        detail += QStringLiteral("\nإحدى الصورتين ضعيفة الوضوح فالنتيجة أقل دقة");
    m_verdictDetail->setText(detail);
}

void MatchPage::updateSearch()
{
    m_results->clear();
    m_openPerson->setEnabled(false);
    m_enroll->setEnabled(false);
    const FaceItem *face = selectedFace(Query);
    if (!face) {
        m_resultsHint->setText(m_watchers[Query]->isRunning() ? QStringLiteral("جارٍ تحليل الصورة…")
                                                              : QStringLiteral("اختر صورة لعرض أقرب عشرة أشخاص إليها مع نسبة التشابه"));
        return;
    }
    if (m_database->personCount() == 0) {
        m_resultsHint->setText(QStringLiteral("القاعدة فارغة، أضف أشخاصاً أولاً من صفحة قاعدة البيانات"));
        return;
    }
    const float threshold = m_threshold();
    GalleryPtr gallery = m_database->gallery();
    const QList<MatchResult> found = FaceDatabase::candidates(*gallery, face->embedding, 10, threshold);
    int confirmed = 0;
    for (const MatchResult &match : found) {
        const Person person = m_database->person(match.personId);
        auto *item = new QListWidgetItem(m_results);
        item->setData(CardRole::Id, match.personId);
        item->setData(CardRole::Title, match.name);
        QString subtitle = QStringLiteral("%1 صور").arg(person.photoCount);
        if (!person.tags.isEmpty())
            subtitle += QStringLiteral(" · %1").arg(person.tags);
        item->setData(CardRole::Subtitle, subtitle);
        item->setData(CardRole::Status, int(match.status));
        if (match.status == MatchStatus::Unknown)
            item->setData(CardRole::StatusText, QStringLiteral("غير مطابق"));
        item->setData(CardRole::Score, match.score);
        const QImage cover = person.coverPhoto ? m_database->photo(person.coverPhoto, 128) : QImage();
        item->setData(CardRole::Avatar, circularPixmap(cover.isNull() ? ImageIO::faceCrop(*face) : cover, 54));
        if (match.status == MatchStatus::Confirmed)
            ++confirmed;
    }
    if (found.isEmpty())
        m_resultsHint->setText(QStringLiteral("لا يوجد أشخاص قريبون من هذه الصورة"));
    else if (confirmed > 0)
        m_resultsHint->setText(QStringLiteral("أقرب نتيجة: %1 بتشابه %2% · انقر نقراً مزدوجاً لفتح الملف")
                                   .arg(found.first().name)
                                   .arg(qRound(found.first().score * 100)));
    else
        m_resultsHint->setText(QStringLiteral("لا يوجد تطابق مؤكد؛ هذه أقرب النتائج فقط. إن عرفت الشخص فأضف الصورة لملفه"));
    if (m_results->count() > 0)
        m_results->setCurrentRow(0);
}

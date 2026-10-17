#include "enhancedialog.h"

#include "controls.h"
#include "faceengine.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImageReader>
#include <QMimeData>
#include <QShortcut>
#include <QEventLoop>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <opencv2/imgproc.hpp>

#include <algorithm>

// معاينة قبل/بعد بخط فاصل يُسحب
class BeforeAfterView : public QWidget
{
public:
    explicit BeforeAfterView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(420, 320);
        setMouseTracking(true);
        setCursor(Qt::SplitHCursor);
        setToolTip(QStringLiteral("اسحب الخط للمقارنة بين الأصل (يمين) والمحسّنة (يسار)"));
    }

    std::function<void()> onEmptyClick;

    // وضع القص: تُعرض الصورة كاملة ويُرسم فوقها مستطيل القص بمقابض للسحب
    void setCropMode(bool on, const QRectF &crop)
    {
        m_cropMode = on;
        m_crop = crop;
        setCursor(on ? Qt::CrossCursor : Qt::SplitHCursor);
        setToolTip(on ? QString() : QStringLiteral("اسحب الخط للمقارنة بين الأصل (يمين) والمحسّنة (يسار)"));
        update();
    }
    QRectF crop() const { return m_crop; }

    void setBefore(const QImage &image)
    {
        m_before = image;
        update();
    }
    void setAfter(const QImage &image)
    {
        m_after = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath frame;
        frame.addRoundedRect(QRectF(rect()), 12, 12);
        p.fillPath(frame, Theme::Background);
        const QImage &shown = m_after.isNull() ? m_before : m_after;
        if (shown.isNull()) {
            QPen pen(Theme::Border, 1.5, Qt::DashLine);
            p.setPen(pen);
            p.drawPath(frame);
            const QPointF c = rect().center();
            Icons::paint(p, Icons::Name::PhotoAdd, QRectF(c.x() - 28, c.y() - 70, 56, 56), Theme::Accent2);
            QFont font = p.font();
            font.setPointSizeF(font.pointSizeF() + 2);
            font.setWeight(QFont::DemiBold);
            p.setFont(font);
            p.setPen(Theme::Text);
            p.drawText(QRectF(0, c.y() - 4, width(), 30), Qt::AlignCenter, QStringLiteral("اسحب صورة إلى هنا، أو افتحها، أو الصقها بـ Ctrl+V"));
            font.setPointSizeF(font.pointSizeF() - 3);
            font.setWeight(QFont::Normal);
            p.setFont(font);
            p.setPen(Theme::Muted);
            p.drawText(QRectF(0, c.y() + 28, width(), 24), Qt::AlignCenter,
                       QStringLiteral("ثم حسّنها: التشويش، الضبابية، الحدة، الدقة، الإضاءة، الزاوية والمنظور"));
            return;
        }
        const QRectF target = imageRect(shown);
        if (m_cropMode) {
            paintCrop(p, shown, target);
            return;
        }
        const double split = target.left() + target.width() * m_split;
        p.save();
        p.setClipRect(QRectF(target.left(), target.top(), split - target.left(), target.height()));
        p.drawImage(target, shown);
        p.restore();
        if (!m_before.isNull()) {
            p.save();
            p.setClipRect(QRectF(split, target.top(), target.right() - split, target.height()));
            // الأصل بنفس إطار المحسّنة (التدوير والقص نفسه) فيتطابقان عند الخط الفاصل
            p.drawImage(target, m_before);
            p.restore();
        }
        p.setPen(QPen(Qt::white, 2));
        p.drawLine(QPointF(split, target.top()), QPointF(split, target.bottom()));
        p.setBrush(Theme::Accent);
        p.setPen(QPen(Qt::white, 2));
        p.drawEllipse(QPointF(split, target.center().y()), 9, 9);

        QFont font = p.font();
        font.setPointSizeF(9);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        auto badge = [&](const QString &text, const QPointF &at, bool alignRight) {
            const QRectF box(alignRight ? at.x() - 70 : at.x(), at.y(), 70, 24);
            QPainterPath path;
            path.addRoundedRect(box, 12, 12);
            p.fillPath(path, QColor(0, 0, 0, 150));
            p.setPen(Qt::white);
            p.drawText(box, Qt::AlignCenter, text);
        };
        badge(QStringLiteral("المحسّنة"), target.topLeft() + QPointF(10, 10), false);
        badge(QStringLiteral("الأصل"), target.topRight() + QPointF(-10, 10), true);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_before.isNull() && m_after.isNull()) {
            if (onEmptyClick)
                onEmptyClick();
            return;
        }
        if (m_cropMode) {
            m_drag = hitTest(event->position());
            // بلا قص سابق يرسم السحب داخل الصورة مستطيلاً جديداً بدل تحريك الصورة كلها
            if (m_drag == Drag::Move && m_crop.width() > 0.98 && m_crop.height() > 0.98)
                m_drag = Drag::None;
            m_pressPoint = toImage(event->position());
            m_pressCrop = m_crop;
            if (m_drag == Drag::None) {
                m_drag = Drag::New;
                m_crop = QRectF(m_pressPoint, QSizeF(0, 0));
            }
            update();
            return;
        }
        moveSplit(event->position().x());
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_cropMode) {
            if (event->buttons() & Qt::LeftButton)
                dragCrop(toImage(event->position()));
            else
                setCursor(cursorFor(hitTest(event->position())));
            return;
        }
        if (event->buttons() & Qt::LeftButton)
            moveSplit(event->position().x());
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (!m_cropMode)
            return;
        // نقرة بلا سحب لا تترك مستطيلاً فارغاً
        if (m_crop.width() < 0.02 || m_crop.height() < 0.02)
            m_crop = m_pressCrop.isValid() && m_pressCrop.width() >= 0.02 ? m_pressCrop : QRectF(0, 0, 1, 1);
        m_drag = Drag::None;
        update();
    }

private:
    enum class Drag { None, New, Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

    QRectF imageRect(const QImage &image) const
    {
        const QSizeF fitted = QSizeF(image.size()).scaled(QSizeF(size()) - QSizeF(16, 16), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
    }
    QRectF shownRect() const { return imageRect(m_after.isNull() ? m_before : m_after); }
    QPointF toImage(const QPointF &pos) const
    {
        const QRectF target = shownRect();
        return QPointF(std::clamp((pos.x() - target.left()) / std::max(1.0, target.width()), 0.0, 1.0),
                       std::clamp((pos.y() - target.top()) / std::max(1.0, target.height()), 0.0, 1.0));
    }
    QRectF cropOnScreen() const
    {
        const QRectF target = shownRect();
        return QRectF(target.left() + m_crop.left() * target.width(), target.top() + m_crop.top() * target.height(),
                      m_crop.width() * target.width(), m_crop.height() * target.height());
    }
    Drag hitTest(const QPointF &pos) const
    {
        const QRectF r = cropOnScreen();
        const double grip = 12;
        const bool left = std::abs(pos.x() - r.left()) < grip, right = std::abs(pos.x() - r.right()) < grip;
        const bool top = std::abs(pos.y() - r.top()) < grip, bottom = std::abs(pos.y() - r.bottom()) < grip;
        const bool insideX = pos.x() > r.left() - grip && pos.x() < r.right() + grip;
        const bool insideY = pos.y() > r.top() - grip && pos.y() < r.bottom() + grip;
        if (top && left)
            return Drag::TopLeft;
        if (top && right)
            return Drag::TopRight;
        if (bottom && left)
            return Drag::BottomLeft;
        if (bottom && right)
            return Drag::BottomRight;
        if (left && insideY)
            return Drag::Left;
        if (right && insideY)
            return Drag::Right;
        if (top && insideX)
            return Drag::Top;
        if (bottom && insideX)
            return Drag::Bottom;
        return r.contains(pos) ? Drag::Move : Drag::None;
    }
    static Qt::CursorShape cursorFor(Drag drag)
    {
        switch (drag) {
        case Drag::Move:
            return Qt::SizeAllCursor;
        case Drag::Left:
        case Drag::Right:
            return Qt::SizeHorCursor;
        case Drag::Top:
        case Drag::Bottom:
            return Qt::SizeVerCursor;
        case Drag::TopLeft:
        case Drag::BottomRight:
            return Qt::SizeFDiagCursor;
        case Drag::TopRight:
        case Drag::BottomLeft:
            return Qt::SizeBDiagCursor;
        default:
            return Qt::CrossCursor;
        }
    }
    void dragCrop(const QPointF &at)
    {
        QRectF r = m_pressCrop;
        switch (m_drag) {
        case Drag::New:
            r = QRectF(m_pressPoint, at).normalized();
            break;
        case Drag::Move: {
            const QPointF delta = at - m_pressPoint;
            r.translate(std::clamp(delta.x(), -r.left(), 1.0 - r.right()), std::clamp(delta.y(), -r.top(), 1.0 - r.bottom()));
            break;
        }
        case Drag::Left:
        case Drag::TopLeft:
        case Drag::BottomLeft:
            r.setLeft(std::min(at.x(), r.right() - 0.02));
            break;
        case Drag::Right:
        case Drag::TopRight:
        case Drag::BottomRight:
            r.setRight(std::max(at.x(), r.left() + 0.02));
            break;
        default:
            break;
        }
        if (m_drag == Drag::Top || m_drag == Drag::TopLeft || m_drag == Drag::TopRight)
            r.setTop(std::min(at.y(), r.bottom() - 0.02));
        if (m_drag == Drag::Bottom || m_drag == Drag::BottomLeft || m_drag == Drag::BottomRight)
            r.setBottom(std::max(at.y(), r.top() + 0.02));
        m_crop = r.intersected(QRectF(0, 0, 1, 1));
        update();
    }
    void paintCrop(QPainter &p, const QImage &shown, const QRectF &target)
    {
        p.drawImage(target, shown);
        const QRectF r = cropOnScreen();
        QPainterPath outside;
        outside.addRect(target);
        outside.addRect(r);
        p.fillPath(outside, QColor(0, 0, 0, 150));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 90), 1));
        for (int i = 1; i < 3; ++i) {
            p.drawLine(QPointF(r.left() + r.width() * i / 3, r.top()), QPointF(r.left() + r.width() * i / 3, r.bottom()));
            p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 3), QPointF(r.right(), r.top() + r.height() * i / 3));
        }
        p.setPen(QPen(Qt::white, 2));
        p.drawRect(r);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::Accent2);
        for (const QPointF &corner : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()})
            p.drawEllipse(corner, 6, 6);
        for (const QPointF &edge : {QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()),
                                    QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y())})
            p.drawEllipse(edge, 4, 4);
        // الأبعاد الناتجة بالبكسل من الصورة الأصلية
        if (m_sourceSize.isValid()) {
            // عزل لاتيني حتى لا يعكس الاتجاه العربي ترتيب العرض والارتفاع
            const QString size = QStringLiteral("\u2066%1 × %2\u2069").arg(qRound(m_crop.width() * m_sourceSize.width()))
                                     .arg(qRound(m_crop.height() * m_sourceSize.height()));
            QFont font = p.font();
            font.setPointSizeF(9);
            p.setFont(font);
            const QRectF box(r.center().x() - 50, r.bottom() + 8 > target.bottom() - 26 ? r.bottom() - 32 : r.bottom() + 8, 100, 24);
            QPainterPath pill;
            pill.addRoundedRect(box, 12, 12);
            p.fillPath(pill, QColor(0, 0, 0, 170));
            p.setPen(Qt::white);
            p.drawText(box, Qt::AlignCenter, size);
        }
    }

public:
    void setSourceSize(const QSize &size) { m_sourceSize = size; }

private:
    void moveSplit(double x)
    {
        m_split = std::clamp(x / std::max(1, width()), 0.0, 1.0);
        update();
    }

    QImage m_before, m_after;
    double m_split = 0.5;
    bool m_cropMode = false;
    QRectF m_crop{0, 0, 1, 1};
    QRectF m_pressCrop;
    QPointF m_pressPoint;
    Drag m_drag = Drag::None;
    QSize m_sourceSize;
};

namespace {

// يشغّل عملاً ثقيلاً مع إبقاء النافذة تُرسم
template<typename Function>
auto waitResponsive(Function function)
{
    QFutureWatcher<decltype(function())> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(std::move(function)));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    QApplication::restoreOverrideCursor();
    return watcher.result();
}

} // namespace

EnhanceDialog::EnhanceDialog(const cv::Mat &image, const FaceEngine *engine, const QString &title, const QString &useText,
                             QWidget *parent)
    : QDialog(parent)
    , m_source(image.clone())
    , m_engine(engine)
    , m_view(new BeforeAfterView(this))
    , m_info(new QLabel(this))
    , m_upscale(new SelectBox(this))
    , m_debounce(new QTimer(this))
{
    setWindowTitle(title);
    resize(1180, 720);
    setAcceptDrops(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(16);

    auto *panel = new QWidget(this);
    panel->setFixedWidth(330);
    auto *side = new QVBoxLayout(panel);
    side->setContentsMargins(0, 0, 0, 0);
    side->setSpacing(8);
    auto *heading = new QLabel(QStringLiteral("تحسين الصورة"), panel);
    heading->setObjectName(QStringLiteral("sectionTitle"));
    side->addWidget(heading);

    auto *openButton = new FancyButton(Icons::Name::Image, QStringLiteral("فتح صورة…"), FancyButton::Kind::Secondary, panel);
    openButton->setToolTip(QStringLiteral("اختيار صورة أخرى للتحسين (أو اسحبها إلى النافذة، أو الصقها بـ Ctrl+V)"));
    connect(openButton, &QAbstractButton::clicked, this, &EnhanceDialog::openImage);
    auto *autoButton = new FancyButton(Icons::Name::Sparkle, QStringLiteral("تحسين تلقائي"), FancyButton::Kind::Primary, panel);
    autoButton->setToolTip(QStringLiteral("يقيس الإضاءة والتشويش والوضوح والحجم ويقترح القيم المناسبة"));
    auto *topRow = new QHBoxLayout;
    topRow->addWidget(autoButton, 1);
    topRow->addWidget(openButton);
    side->addLayout(topRow);

    auto *controls = new QWidget(panel);
    auto *grid = new QGridLayout(controls);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(3);
    int row = 0;
    auto section = [&](const QString &text) {
        auto *label = new QLabel(text, controls);
        label->setObjectName(QStringLiteral("muted"));
        grid->addWidget(label, row++, 0, 1, 3);
    };
    section(QStringLiteral("الوضوح"));
    m_denoise = addSlider(grid, row++, QStringLiteral("إزالة التشويش"), QStringLiteral("يزيل الحبيبات والتشويش مع الحفاظ على الحواف"), 0, 100);
    m_deblur = addSlider(grid, row++, QStringLiteral("إزالة الضبابية"), QStringLiteral("يستعيد التفاصيل في الصور المهزوزة أو غير المركّزة"), 0, 100);
    m_sharpen = addSlider(grid, row++, QStringLiteral("الحدة"), QStringLiteral("يبرز الحواف والملامح"), 0, 100);
    auto *upscaleLabel = new QLabel(QStringLiteral("تكبير الدقة"), controls);
    m_upscale->addItems({QStringLiteral("بدون"), QStringLiteral("×2"), QStringLiteral("×3"), QStringLiteral("×4")});
    m_upscale->setToolTip(QStringLiteral("يكبّر الصور الصغيرة والوجوه البعيدة قبل التوضيح"));
    grid->addWidget(upscaleLabel, row, 0);
    grid->addWidget(m_upscale, row++, 1, 1, 2);
    section(QStringLiteral("الإضاءة"));
    m_brightness = addSlider(grid, row++, QStringLiteral("الإضاءة"), QStringLiteral("يفتح الظلال في الصور المظلمة أو يخفف الإضاءة الزائدة"), -100, 100);
    m_contrast = addSlider(grid, row++, QStringLiteral("التباين"), QStringLiteral("موازنة التباين محلياً حتى تظهر الملامح في الظل والضوء"), 0, 100);
    section(QStringLiteral("الزاوية والمنظور"));
    m_rotation = addSlider(grid, row++, QStringLiteral("التدوير"), QStringLiteral("تدوير دقيق بالدرجات"), -45, 45);
    m_keystoneV = addSlider(grid, row++, QStringLiteral("منظور عمودي"),
                            QStringLiteral("للصور الملتقطة من أعلى (كاميرا مراقبة) أو من أسفل: يعيد الوجه والأجسام إلى نسبها"), -100, 100);
    m_keystoneH = addSlider(grid, row++, QStringLiteral("منظور أفقي"), QStringLiteral("للصور الملتقطة من جانب بزاوية مائلة"), -100, 100);
    side->addWidget(controls);

    auto *turnRow = new QHBoxLayout;
    auto *level = new FancyButton(Icons::Name::Level, QStringLiteral("تسوية ميلان الوجه"), FancyButton::Kind::Secondary, panel);
    level->setToolTip(QStringLiteral("تسوية ميلان الوجه: يجعل خط العينين أفقياً تلقائياً"));
    auto *turn = new FancyButton(Icons::Name::Rotate, QStringLiteral("90°"), FancyButton::Kind::Secondary, panel);
    turn->setToolTip(QStringLiteral("تدوير 90 درجة"));
    auto *mirror = new FancyButton(Icons::Name::Flip, QStringLiteral("عكس"), FancyButton::Kind::Secondary, panel);
    mirror->setToolTip(QStringLiteral("عكس الصورة أفقياً (صور كاميرا السيلفي)"));
    // صف أدوات واحد: القص بنصه لأنه يتحول إلى «تطبيق القص»، والبقية أيقونات وأسماؤها في التلميح
    auto *cropRow = turnRow;
    cropRow->setSpacing(6);
    m_cropButton = new FancyButton(Icons::Name::Crop, QStringLiteral("قص"), FancyButton::Kind::Secondary, panel);
    m_cropButton->setToolTip(QStringLiteral("حدّد منطقة من الصورة بالسحب، ثم اضغط \"تطبيق القص\""));
    auto *faceCrop = new FancyButton(Icons::Name::Scan, QStringLiteral("حول الوجه"), FancyButton::Kind::Secondary, panel);
    faceCrop->setToolTip(QStringLiteral("قص تلقائي بنسبة صورة شخصية (3:4) حول أكبر وجه"));
    m_clearCrop = new FancyButton(Icons::Name::Reject, QStringLiteral("إلغاء القص"), FancyButton::Kind::Secondary, panel);
    m_clearCrop->setToolTip(QStringLiteral("إلغاء القص"));
    for (FancyButton *button : {level, turn, mirror, faceCrop, m_clearCrop})
        button->setCompact(true);
    cropRow->addWidget(m_cropButton, 1);
    cropRow->addWidget(faceCrop);
    cropRow->addWidget(m_clearCrop);
    cropRow->addSpacing(6);
    cropRow->addWidget(level);
    cropRow->addWidget(turn);
    cropRow->addWidget(mirror);
    side->addLayout(cropRow);
    connect(m_cropButton, &QAbstractButton::clicked, this, [this] { setCropMode(!m_cropMode); });
    connect(faceCrop, &QAbstractButton::clicked, this, &EnhanceDialog::cropToFace);
    connect(m_clearCrop, &QAbstractButton::clicked, this, [this] {
        m_crop = cv::Rect2d(0, 0, 1, 1);
        setCropMode(false);
    });

    m_info->setObjectName(QStringLiteral("muted"));
    m_info->setWordWrap(true);
    side->addWidget(m_info);
    side->addStretch();

    // أزرار الحفظ خارج منطقة التمرير حتى تبقى ظاهرة في الشاشات القصيرة
    auto *footer = new QWidget(this);
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(20, 10, 0, 0);
    footerLayout->setSpacing(8);
    auto *reset = new FancyButton(Icons::Name::Undo, QStringLiteral("إعادة الضبط"), FancyButton::Kind::Secondary, footer);
    auto *saveButton = new FancyButton(Icons::Name::Save, QStringLiteral("حفظ…"), FancyButton::Kind::Secondary, footer);
    auto *copyButton = new FancyButton(Icons::Name::Copy, QStringLiteral("نسخ"), FancyButton::Kind::Secondary, footer);
    auto *actions = new QHBoxLayout;
    actions->addWidget(reset, 1);
    actions->addWidget(saveButton, 1);
    actions->addWidget(copyButton, 1);
    footerLayout->addLayout(actions);
    m_close = new FancyButton(Icons::Name::Reject, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, footer);
    connect(m_close, &QAbstractButton::clicked, this, &QDialog::reject);
    if (!useText.isEmpty()) {
        auto *use = new FancyButton(Icons::Name::Search, useText, FancyButton::Kind::Primary, footer);
        connect(use, &QAbstractButton::clicked, this, [this] {
            if (m_source.empty())
                return;
            m_result = renderFull();
            if (onUse)
                onUse(m_result);
            else
                accept();
        });
        footerLayout->addWidget(use);
        m_needImage << use;
    }
    footerLayout->addWidget(m_close);
    m_needImage << autoButton << controls << level << turn << mirror << reset << saveButton << copyButton << m_cropButton << faceCrop;
    m_view->onEmptyClick = [this] { openImage(); };
    // داخل النافذة الرئيسية يمرّر اختصارها اللصق إلى هنا؛ في النافذة المنفصلة يكون الاختصار خاصاً بها
    m_paste = new QShortcut(QKeySequence::Paste, this);
    connect(m_paste, &QShortcut::activated, this, &EnhanceDialog::pasteImage);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *column = new QVBoxLayout;
    column->setSpacing(0);
    column->addWidget(scroll, 1);
    column->addWidget(footer);
    footer->setFixedWidth(350);
    scroll->setFixedWidth(350);
    layout->addLayout(column);
    layout->addWidget(m_view, 1);

    m_debounce->setSingleShot(true);
    m_debounce->setInterval(120);
    connect(m_debounce, &QTimer::timeout, this, &EnhanceDialog::renderPreview);
    connect(&m_watcher, &QFutureWatcher<QPair<QImage, QImage>>::finished, this, [this] {
        const QPair<QImage, QImage> images = m_watcher.result();
        m_view->setBefore(images.second);
        m_view->setAfter(images.first);
        if (std::exchange(m_pending, false))
            renderPreview();
    });
    connect(m_upscale, &QComboBox::currentIndexChanged, this, &EnhanceDialog::schedulePreview);
    connect(autoButton, &QAbstractButton::clicked, this, [this] {
        ImageLab::Settings suggested = ImageLab::suggest(m_source);
        suggested.quarterTurns = m_quarterTurns;
        suggested.mirror = m_mirror;
        suggested.rotation = m_rotation.slider->value();
        suggested.keystoneH = m_keystoneH.slider->value();
        suggested.keystoneV = m_keystoneV.slider->value();
        setSettings(suggested);
    });
    connect(level, &QAbstractButton::clicked, this, &EnhanceDialog::levelFace);
    connect(turn, &QAbstractButton::clicked, this, [this] {
        m_quarterTurns = (m_quarterTurns + 3) % 4; // مع عقارب الساعة
        schedulePreview();
    });
    connect(mirror, &QAbstractButton::clicked, this, [this] {
        m_mirror = !m_mirror;
        schedulePreview();
    });
    connect(reset, &QAbstractButton::clicked, this, [this] {
        m_quarterTurns = 0;
        m_mirror = false;
        m_crop = cv::Rect2d(0, 0, 1, 1);
        setCropMode(false);
        setSettings({});
    });
    connect(saveButton, &QAbstractButton::clicked, this, &EnhanceDialog::save);
    connect(copyButton, &QAbstractButton::clicked, this, &EnhanceDialog::copy);

    setImage(m_source);
}

void EnhanceDialog::setEmbedded(bool embedded)
{
    setWindowFlags(embedded ? Qt::Widget : Qt::Dialog);
    m_close->setVisible(!embedded);
    m_paste->setEnabled(!embedded);
}

void EnhanceDialog::pasteImage()
{
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        const QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile()) {
            const cv::Mat loaded = ImageIO::loadImage(urls.first().toLocalFile());
            if (!loaded.empty())
                setImage(loaded, QFileInfo(urls.first().toLocalFile()).completeBaseName());
        }
        return;
    }
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    const cv::Mat wrapped(rgb.height(), rgb.width(), CV_8UC3, const_cast<uchar *>(rgb.constBits()), size_t(rgb.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(wrapped, bgr, cv::COLOR_RGB2BGR);
    setImage(bgr, QStringLiteral("صورة_ملصقة"));
}

void EnhanceDialog::setImage(const cv::Mat &image, const QString &name)
{
    m_watcher.waitForFinished();
    m_source = image.clone();
    m_name = name;
    m_result.release();
    m_preview.release();
    if (!m_source.empty()) {
        const int longSide = std::max(m_source.cols, m_source.rows);
        const double scale = std::min(1.0, 900.0 / std::max(1, longSide));
        if (scale < 1.0)
            cv::resize(m_source, m_preview, cv::Size(), scale, scale, cv::INTER_AREA);
        else
            m_preview = m_source;
    }
    m_quarterTurns = 0;
    m_mirror = false;
    m_crop = cv::Rect2d(0, 0, 1, 1);
    m_cropMode = false;
    m_view->setCropMode(false, QRectF(0, 0, 1, 1));
    m_view->setSourceSize(QSize(m_source.cols, m_source.rows));
    updateCropButtons();
    m_updating = true;
    for (Control *c : {&m_brightness, &m_contrast, &m_denoise, &m_deblur, &m_sharpen, &m_rotation, &m_keystoneV, &m_keystoneH})
        c->slider->setValue(0);
    m_upscale->setCurrentIndex(0);
    m_updating = false;
    m_view->setBefore(m_preview.empty() ? QImage() : ImageIO::toQImage(m_preview));
    m_view->setAfter({});
    m_info->setText(m_source.empty() ? QString()
                                     : QStringLiteral("%1الأبعاد: \u2066%2×%3\u2069")
                                           .arg(name.isEmpty() ? QString() : name + QStringLiteral(" · "))
                                           .arg(m_source.cols)
                                           .arg(m_source.rows));
    updateEnabled();
    if (!m_source.empty())
        renderPreview();
}

void EnhanceDialog::updateEnabled()
{
    for (QWidget *widget : std::as_const(m_needImage))
        widget->setEnabled(!m_source.empty());
    updateCropButtons();
}

void EnhanceDialog::updateCropButtons()
{
    m_cropButton->setText(m_cropMode ? QStringLiteral("تطبيق القص") : QStringLiteral("قص"));
    m_cropButton->setIconName(m_cropMode ? Icons::Name::Check : Icons::Name::Crop);
    m_clearCrop->setEnabled(!m_source.empty() && (m_cropMode || cropActive()));
}

bool EnhanceDialog::cropActive() const
{
    ImageLab::Settings s;
    s.crop = m_crop;
    return s.hasCrop();
}

void EnhanceDialog::setCropMode(bool on)
{
    if (m_source.empty())
        return;
    if (m_cropMode && !on) {
        // تطبيق المستطيل المحدد
        const QRectF r = m_view->crop();
        m_crop = cv::Rect2d(r.x(), r.y(), r.width(), r.height());
    }
    m_cropMode = on;
    const bool swapped = (((m_quarterTurns % 4) + 4) % 4) % 2 == 1;
    m_view->setSourceSize(swapped ? QSize(m_source.rows, m_source.cols) : QSize(m_source.cols, m_source.rows));
    m_view->setCropMode(on, QRectF(m_crop.x, m_crop.y, m_crop.width, m_crop.height));
    updateCropButtons();
    renderPreview();
}

void EnhanceDialog::cropToFace()
{
    ImageLab::Settings base = settings().geometryOnly();
    base.crop = cv::Rect2d(0, 0, 1, 1);
    const cv::Mat oriented = ImageLab::apply(m_source, base);
    const FaceEngine *engine = m_engine;
    cv::Rect2d crop;
    const bool found = waitResponsive([engine, oriented, &crop] { return ImageLab::faceCrop(oriented, engine, &crop); });
    if (!found) {
        Toast::show(this, QStringLiteral("لم يُعثر على وجه"), Toast::Kind::Warning);
        return;
    }
    m_crop = crop;
    if (m_cropMode)
        m_view->setCropMode(true, QRectF(crop.x, crop.y, crop.width, crop.height)); // يبقى للتعديل اليدوي
    updateCropButtons();
    renderPreview();
}

void EnhanceDialog::openImage()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("اختر صورة لتحسينها"), m_lastDirectory,
                                                      QStringLiteral("الصور (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff *.jfif)"));
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    const cv::Mat image = ImageIO::loadImage(path);
    if (image.empty()) {
        Toast::show(this, QStringLiteral("تعذر قراءة الصورة"), Toast::Kind::Warning);
        return;
    }
    setImage(image, QFileInfo(path).completeBaseName());
}

void EnhanceDialog::dragEnterEvent(QDragEnterEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if ((!urls.isEmpty() && urls.first().isLocalFile() && !QImageReader::imageFormat(urls.first().toLocalFile()).isEmpty())
        || event->mimeData()->hasImage())
        event->acceptProposedAction();
}

void EnhanceDialog::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile()) {
        const QString path = urls.first().toLocalFile();
        const cv::Mat image = ImageIO::loadImage(path);
        if (!image.empty()) {
            event->acceptProposedAction();
            setImage(image, QFileInfo(path).completeBaseName());
        }
    }
}

EnhanceDialog::~EnhanceDialog()
{
    m_watcher.waitForFinished();
}

EnhanceDialog::Control EnhanceDialog::addSlider(QGridLayout *grid, int row, const QString &name, const QString &tip, int minimum, int maximum)
{
    Control control;
    auto *label = new QLabel(name, grid->parentWidget());
    label->setToolTip(tip);
    control.slider = new QSlider(Qt::Horizontal, grid->parentWidget());
    control.slider->setRange(minimum, maximum);
    control.slider->setValue(0);
    control.slider->setToolTip(tip);
    control.slider->setCursor(Qt::PointingHandCursor);
    // الصفر في البداية والقيمة تزيد يميناً كالأرقام بجانبها
    control.slider->setLayoutDirection(Qt::LeftToRight);
    control.value = new QLabel(QStringLiteral("0"), grid->parentWidget());
    control.value->setObjectName(QStringLiteral("badge"));
    control.value->setMinimumWidth(38);
    control.value->setAlignment(Qt::AlignCenter);
    grid->addWidget(label, row, 0);
    grid->addWidget(control.slider, row, 1);
    grid->addWidget(control.value, row, 2);
    QLabel *value = control.value;
    connect(control.slider, &QSlider::valueChanged, this, [this, value](int v) {
        value->setText(QString::number(v));
        if (!m_updating)
            schedulePreview();
    });
    return control;
}

ImageLab::Settings EnhanceDialog::settings() const
{
    ImageLab::Settings s;
    s.brightness = m_brightness.slider->value();
    s.contrast = m_contrast.slider->value();
    s.denoise = m_denoise.slider->value();
    s.deblur = m_deblur.slider->value();
    s.sharpen = m_sharpen.slider->value();
    s.upscale = m_upscale->currentIndex() + 1;
    s.rotation = m_rotation.slider->value();
    s.keystoneH = m_keystoneH.slider->value();
    s.keystoneV = m_keystoneV.slider->value();
    s.quarterTurns = m_quarterTurns;
    s.mirror = m_mirror;
    if (m_cropMode) {
        const QRectF r = m_view->crop();
        s.crop = cv::Rect2d(r.x(), r.y(), r.width(), r.height());
    } else {
        s.crop = m_crop;
    }
    return s;
}

void EnhanceDialog::setSettings(const ImageLab::Settings &s)
{
    m_updating = true;
    m_brightness.slider->setValue(s.brightness);
    m_contrast.slider->setValue(s.contrast);
    m_denoise.slider->setValue(s.denoise);
    m_deblur.slider->setValue(s.deblur);
    m_sharpen.slider->setValue(s.sharpen);
    m_upscale->setCurrentIndex(std::clamp(s.upscale, 1, 4) - 1);
    m_rotation.slider->setValue(int(std::lround(s.rotation)));
    m_keystoneH.slider->setValue(s.keystoneH);
    m_keystoneV.slider->setValue(s.keystoneV);
    m_quarterTurns = s.quarterTurns;
    m_mirror = s.mirror;
    m_updating = false;
    schedulePreview();
}

void EnhanceDialog::schedulePreview()
{
    m_debounce->start();
}

void EnhanceDialog::renderPreview()
{
    if (m_preview.empty())
        return;
    if (m_watcher.isRunning()) {
        m_pending = true;
        return;
    }
    ImageLab::Settings s = settings();
    // المعاينة لا تحتاج التكبير الفعلي إلا بقدر ما تظهر به على الشاشة
    const int maxSide = std::max(m_preview.cols, m_preview.rows);
    s.upscale = std::clamp(int(std::lround(1400.0 / std::max(1, maxSide))), 1, s.upscale);
    // أثناء القص تُعرض الصورة كاملة ليُحدَّد عليها المستطيل
    if (m_cropMode)
        s.crop = cv::Rect2d(0, 0, 1, 1);
    const cv::Mat preview = m_preview;
    m_watcher.setFuture(QtConcurrent::run([preview, s] {
        const ImageLab::Settings geometry = s.geometryOnly();
        return qMakePair(ImageIO::toQImage(ImageLab::apply(preview, s)), ImageIO::toQImage(ImageLab::apply(preview, geometry)));
    }));
}

cv::Mat EnhanceDialog::renderFull()
{
    const cv::Mat source = m_source;
    const ImageLab::Settings s = settings();
    if (s.isIdentity())
        return source;
    return waitResponsive([source, s] { return ImageLab::apply(source, s); });
}

void EnhanceDialog::save()
{
    if (m_source.empty())
        return;
    const QString suggested = (m_name.isEmpty() ? QStringLiteral("صورة") : m_name) + QStringLiteral("_محسنة.png");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ الصورة المحسّنة"), QDir(m_lastDirectory).filePath(suggested),
                                                      QStringLiteral("صور (*.png *.jpg)"));
    if (path.isEmpty())
        return;
    const QImage image = ImageIO::toQImage(renderFull());
    if (image.save(path))
        Toast::show(this, QStringLiteral("تم حفظ الصورة"));
    else
        Toast::show(this, QStringLiteral("تعذر حفظ الصورة"), Toast::Kind::Warning);
}

void EnhanceDialog::copy()
{
    if (m_source.empty())
        return;
    QApplication::clipboard()->setImage(ImageIO::toQImage(renderFull()));
    Toast::show(this, QStringLiteral("نُسخت الصورة المحسّنة"));
}

void EnhanceDialog::levelFace()
{
    ImageLab::Settings base;
    base.quarterTurns = m_quarterTurns;
    base.mirror = m_mirror;
    base.keystoneH = m_keystoneH.slider->value();
    base.keystoneV = m_keystoneV.slider->value();
    const cv::Mat oriented = ImageLab::apply(m_source, base);
    const FaceEngine *engine = m_engine;
    double angle = 0.0;
    const bool found = waitResponsive([engine, oriented, &angle] { return ImageLab::levelFace(oriented, engine, &angle); });
    if (!found) {
        Toast::show(this, QStringLiteral("لم يُعثر على وجه لتسويته"), Toast::Kind::Warning);
        return;
    }
    m_rotation.slider->setValue(int(std::lround(std::clamp(angle, -45.0, 45.0))));
    Toast::show(this, QStringLiteral("سُوّي ميلان الوجه (%1°)").arg(qRound(angle)), Toast::Kind::Info);
}

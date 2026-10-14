#include "enhancedialog.h"

#include "faceengine.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
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
        if (shown.isNull())
            return;
        const QSizeF fitted = QSizeF(shown.size()).scaled(QSizeF(size()) - QSizeF(16, 16), Qt::KeepAspectRatio);
        const QRectF target(QPointF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
        const double split = target.left() + target.width() * m_split;
        p.save();
        p.setClipRect(QRectF(target.left(), target.top(), split - target.left(), target.height()));
        p.drawImage(target, shown);
        p.restore();
        if (!m_before.isNull()) {
            p.save();
            p.setClipRect(QRectF(split, target.top(), target.right() - split, target.height()));
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

    void mousePressEvent(QMouseEvent *event) override { moveSplit(event->position().x()); }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton)
            moveSplit(event->position().x());
    }

private:
    void moveSplit(double x)
    {
        m_split = std::clamp(x / std::max(1, width()), 0.0, 1.0);
        update();
    }

    QImage m_before, m_after;
    double m_split = 0.5;
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
    , m_upscale(new QComboBox(this))
    , m_debounce(new QTimer(this))
{
    setWindowTitle(title);
    resize(1180, 720);
    const int longSide = std::max(m_source.cols, m_source.rows);
    const double scale = std::min(1.0, 900.0 / std::max(1, longSide));
    if (scale < 1.0)
        cv::resize(m_source, m_preview, cv::Size(), scale, scale, cv::INTER_AREA);
    else
        m_preview = m_source;

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(16);

    auto *panel = new QWidget(this);
    panel->setFixedWidth(330);
    auto *side = new QVBoxLayout(panel);
    side->setContentsMargins(0, 0, 0, 0);
    side->setSpacing(10);
    auto *heading = new QLabel(QStringLiteral("تحسين الصورة"), panel);
    heading->setObjectName(QStringLiteral("sectionTitle"));
    side->addWidget(heading);

    auto *autoButton = new FancyButton(Icons::Name::Sparkle, QStringLiteral("تحسين تلقائي"), FancyButton::Kind::Primary, panel);
    autoButton->setToolTip(QStringLiteral("يقيس الإضاءة والتشويش والوضوح والحجم ويقترح القيم المناسبة"));
    side->addWidget(autoButton);

    auto *controls = new QWidget(panel);
    auto *grid = new QGridLayout(controls);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
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
    auto *level = new FancyButton(Icons::Name::Scan, QStringLiteral("تسوية ميلان الوجه"), FancyButton::Kind::Secondary, panel);
    level->setToolTip(QStringLiteral("يجعل خط العينين أفقياً تلقائياً"));
    auto *turn = new FancyButton(Icons::Name::History, QStringLiteral("90°"), FancyButton::Kind::Secondary, panel);
    turn->setToolTip(QStringLiteral("تدوير 90 درجة"));
    auto *mirror = new FancyButton(Icons::Name::Compare, QStringLiteral("عكس"), FancyButton::Kind::Secondary, panel);
    mirror->setToolTip(QStringLiteral("عكس الصورة أفقياً (صور كاميرا السيلفي)"));
    turnRow->addWidget(level, 1);
    turnRow->addWidget(turn);
    turnRow->addWidget(mirror);
    side->addLayout(turnRow);

    m_info->setObjectName(QStringLiteral("muted"));
    m_info->setWordWrap(true);
    side->addWidget(m_info);
    side->addStretch();

    auto *reset = new FancyButton(Icons::Name::Stop, QStringLiteral("إعادة الضبط"), FancyButton::Kind::Secondary, panel);
    auto *saveButton = new FancyButton(Icons::Name::Save, QStringLiteral("حفظ…"), FancyButton::Kind::Secondary, panel);
    auto *copyButton = new FancyButton(Icons::Name::Copy, QStringLiteral("نسخ"), FancyButton::Kind::Secondary, panel);
    auto *actions = new QHBoxLayout;
    actions->addWidget(reset);
    actions->addWidget(saveButton);
    actions->addWidget(copyButton);
    side->addLayout(actions);
    auto *close = new FancyButton(Icons::Name::Stop, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, panel);
    connect(close, &QAbstractButton::clicked, this, &QDialog::reject);
    if (!useText.isEmpty()) {
        auto *use = new FancyButton(Icons::Name::Search, useText, FancyButton::Kind::Primary, panel);
        connect(use, &QAbstractButton::clicked, this, [this] {
            m_result = renderFull();
            accept();
        });
        side->addWidget(use);
    }
    side->addWidget(close);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(350);
    layout->addWidget(scroll);
    layout->addWidget(m_view, 1);

    m_debounce->setSingleShot(true);
    m_debounce->setInterval(120);
    connect(m_debounce, &QTimer::timeout, this, &EnhanceDialog::renderPreview);
    connect(&m_watcher, &QFutureWatcher<QImage>::finished, this, [this] {
        m_view->setAfter(m_watcher.result());
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
        setSettings({});
    });
    connect(saveButton, &QAbstractButton::clicked, this, &EnhanceDialog::save);
    connect(copyButton, &QAbstractButton::clicked, this, &EnhanceDialog::copy);

    m_view->setBefore(ImageIO::toQImage(m_preview));
    m_info->setText(QStringLiteral("الأبعاد: %1×%2").arg(m_source.cols).arg(m_source.rows));
    renderPreview();
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
    if (m_watcher.isRunning()) {
        m_pending = true;
        return;
    }
    ImageLab::Settings s = settings();
    // المعاينة لا تحتاج التكبير الفعلي إلا بقدر ما تظهر به على الشاشة
    const int maxSide = std::max(m_preview.cols, m_preview.rows);
    s.upscale = std::clamp(int(std::lround(1400.0 / std::max(1, maxSide))), 1, s.upscale);
    const cv::Mat preview = m_preview;
    m_watcher.setFuture(QtConcurrent::run([preview, s] { return ImageIO::toQImage(ImageLab::apply(preview, s)); }));
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
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ الصورة المحسّنة"), QStringLiteral("صورة_محسنة.png"),
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

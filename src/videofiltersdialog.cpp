#include "videofiltersdialog.h"

#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <opencv2/imgproc.hpp>

namespace {

constexpr int PreviewWidth = 960;

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QLabel *previewLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumSize(200, 120);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    label->setStyleSheet(QStringLiteral("background: #0e0b09; border-radius: 10px;"));
    return label;
}

} // namespace

VideoFiltersDialog::VideoFiltersDialog(const cv::Mat &frame, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("معالجات الفيديو"));
    resize(1000, 700);
    if (!frame.empty()) {
        if (frame.cols > PreviewWidth)
            cv::resize(frame, m_frame, cv::Size(PreviewWidth, std::max(1, PreviewWidth * frame.rows / frame.cols)), 0, 0, cv::INTER_AREA);
        else
            m_frame = frame.clone();
    }

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    auto *hint = new QLabel(QStringLiteral("تُطبَّق على كل إطار قبل البحث عن الوجوه، وعلى الإطارات المحفوظة والمقاطع المصدَّرة. "
                                           "التغيير يسري فوراً، حتى أثناء التحليل أو مع الكاميرا."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // المعاينة
    auto *previews = new QHBoxLayout;
    auto previewColumn = [this, previews](const QString &caption, QLabel *label) {
        auto *column = new QVBoxLayout;
        auto *title = new QLabel(caption, this);
        title->setObjectName(QStringLiteral("muted"));
        title->setAlignment(Qt::AlignCenter);
        column->addWidget(title);
        column->addWidget(label, 1);
        previews->addLayout(column, 1);
    };
    m_before = previewLabel(this);
    m_after = previewLabel(this);
    previewColumn(QStringLiteral("الأصل"), m_before);
    previewColumn(QStringLiteral("بعد المعالجة"), m_after);
    if (m_frame.empty()) {
        m_before->setText(QStringLiteral("لا يوجد إطار للمعاينة"));
        m_after->setText(QStringLiteral("افتح فيديو لترى أثر المعالجات"));
    }
    layout->addLayout(previews, 2);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(24);

    // الاتجاه وخيارات التحليل
    auto *left = new QVBoxLayout;
    left->addWidget(sectionTitle(QStringLiteral("الاتجاه"), this));
    auto *rotateLeft = new FancyButton(Icons::Name::Previous, QStringLiteral("تدوير يساراً"), FancyButton::Kind::Secondary, this);
    auto *rotateRight = new FancyButton(Icons::Name::Next, QStringLiteral("تدوير يميناً"), FancyButton::Kind::Secondary, this);
    rotateLeft->setToolTip(QStringLiteral("90 درجة عكس عقارب الساعة"));
    rotateRight->setToolTip(QStringLiteral("90 درجة مع عقارب الساعة"));
    m_rotationValue = new QLabel(this);
    m_rotationValue->setObjectName(QStringLiteral("badge"));
    auto *rotateRow = new QHBoxLayout;
    rotateRow->addWidget(rotateRight);
    rotateRow->addWidget(rotateLeft);
    rotateRow->addWidget(m_rotationValue);
    rotateRow->addStretch();
    left->addLayout(rotateRow);
    m_mirror = new ToggleSwitch(QStringLiteral("مرآة (قلب أفقي)"), this);
    m_mirror->setToolTip(QStringLiteral("كاميرات الويب تعكس الصورة فتظهر الكتابة مقلوبة"));
    m_flip = new ToggleSwitch(QStringLiteral("قلب عمودي"), this);
    m_flip->setToolTip(QStringLiteral("كاميرا مركبة في السقف مقلوبة"));
    left->addWidget(m_mirror);
    left->addWidget(m_flip);
    connect(rotateLeft, &QAbstractButton::clicked, this, [this] {
        m_quarterTurns = (m_quarterTurns + 3) % 4;
        commit();
    });
    connect(rotateRight, &QAbstractButton::clicked, this, [this] {
        m_quarterTurns = (m_quarterTurns + 1) % 4;
        commit();
    });

    left->addSpacing(8);
    left->addWidget(sectionTitle(QStringLiteral("خيارات التحليل"), this));
    m_minFace = new QSpinBox(this);
    m_minFace->setRange(0, 400);
    m_minFace->setSingleStep(10);
    m_minFace->setSpecialValueText(QStringLiteral("كل الوجوه"));
    m_minFace->setSuffix(QStringLiteral(" بكسل"));
    m_minFace->setToolTip(QStringLiteral("تُتجاهل الوجوه الأصغر من هذا العرض في الفيديو:\n"
                                         "وجوه المارة البعيدة في الخلفية والصور على الجدران والشاشات.\n"
                                         "يقلل الأشخاص غير المهمين في النتائج."));
    auto *minRow = new QHBoxLayout;
    minRow->addWidget(new QLabel(QStringLiteral("أصغر وجه"), this));
    minRow->addWidget(m_minFace);
    minRow->addStretch();
    left->addLayout(minRow);
    m_skipStill = new ToggleSwitch(QStringLiteral("تخطي الإطارات الساكنة"), this);
    m_skipStill->setToolTip(QStringLiteral("لكاميرات المراقبة الثابتة: إطار لم يتحرك فيه شيء والمشهد خالٍ من الوجوه\n"
                                           "لا يُبحث فيه، فيصبح تحليل الساعات الطويلة أسرع بكثير. أي حركة تعيد البحث فوراً."));
    left->addWidget(m_skipStill);
    left->addStretch();
    columns->addLayout(left, 1);

    // تحسين الصورة
    auto *right = new QVBoxLayout;
    right->addWidget(sectionTitle(QStringLiteral("تحسين الصورة"), this));
    m_preset = new QComboBox(this);
    m_preset->addItems({QStringLiteral("بلا تحسين"), QStringLiteral("ليلي ومظلم"), QStringLiteral("تسجيل جهاز مراقبة قديم"),
                        QStringLiteral("مخصص")});
    m_preset->setToolTip(QStringLiteral("قيم جاهزة لتحسين الصورة؛ تعديل أي قيمة يجعلها «مخصص»"));
    auto *presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel(QStringLiteral("إعداد جاهز"), this));
    presetRow->addWidget(m_preset, 1);
    right->addLayout(presetRow);
    connect(m_preset, &QComboBox::activated, this, [this](int index) {
        if (index > 2)
            return;
        setSettings(VideoFilters::preset(VideoFilters::Preset(index), settings()));
        commit();
    });

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    m_brightness = addSlider(grid, 0, QStringLiteral("الإضاءة"), QStringLiteral("يفتح الظلال أو يعتّم الصورة الساطعة"), -100, 100,
                             &m_brightnessValue);
    m_contrast = addSlider(grid, 1, QStringLiteral("التباين"), QStringLiteral("يبرز ملامح الوجه في الإضاءة المسطحة أو الضبابية"), 0, 100,
                           &m_contrastValue);
    m_denoise = addSlider(grid, 2, QStringLiteral("إزالة التشويش"), QStringLiteral("للتصوير الليلي والكاميرات الرخيصة؛ يبطئ التحليل"), 0,
                          100, &m_denoiseValue);
    m_sharpen = addSlider(grid, 3, QStringLiteral("الحدة"), QStringLiteral("يوضح الحواف في الفيديو المضغوط أو الضبابي قليلاً"), 0, 100,
                          &m_sharpenValue);
    right->addLayout(grid);
    m_autoLevels = new ToggleSwitch(QStringLiteral("إضاءة تلقائية لكل إطار"), this);
    m_autoLevels->setToolTip(QStringLiteral("يمدّ مدى الإضاءة في كل إطار وحده: مفيد حين تتغير الإضاءة (أبواب، سيارات، ليل ونهار)"));
    m_deinterlace = new ToggleSwitch(QStringLiteral("إزالة التشابك"), this);
    m_deinterlace->setToolTip(QStringLiteral("خطوط أفقية متعرجة على الأجسام المتحركة في تسجيلات أجهزة DVR القديمة"));
    m_grayscale = new ToggleSwitch(QStringLiteral("أبيض وأسود"), this);
    m_grayscale->setToolTip(QStringLiteral("يوحّد الألوان حين تتبدل الكاميرا بين الملون والأشعة تحت الحمراء ليلاً"));
    right->addWidget(m_autoLevels);
    right->addWidget(m_deinterlace);
    right->addWidget(m_grayscale);
    right->addStretch();
    columns->addLayout(right, 1);
    // الخيارات في منطقة قابلة للتمرير: على شاشة لابتوب صغيرة لا تنزل النافذة تحت شريط المهام
    // ويبقى زر «تم» ظاهراً دائماً
    auto *controls = new QWidget;
    auto *controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 6, 0);
    controlsLayout->addLayout(columns);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    scroll->setWidget(controls);
    layout->addWidget(scroll, 3);

    for (ToggleSwitch *toggle : {m_mirror, m_flip, m_deinterlace, m_autoLevels, m_grayscale, m_skipStill})
        connect(toggle, &QAbstractButton::toggled, this, &VideoFiltersDialog::commit);
    connect(m_minFace, &QSpinBox::valueChanged, this, &VideoFiltersDialog::commit);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_speed = new QLabel(this);
    m_speed->setObjectName(QStringLiteral("muted"));
    controlsLayout->addWidget(m_summary);
    controlsLayout->addWidget(m_speed);

    auto *buttons = new QHBoxLayout;
    auto *reset = new FancyButton(Icons::Name::Stop, QStringLiteral("إلغاء كل المعالجات"), FancyButton::Kind::Secondary, this);
    auto *close = new FancyButton(Icons::Name::Check, QStringLiteral("تم"), FancyButton::Kind::Primary, this);
    connect(reset, &QAbstractButton::clicked, this, [this] {
        setSettings(VideoFilters::Settings());
        commit();
    });
    connect(close, &QAbstractButton::clicked, this, &QDialog::accept);
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(90);
    connect(m_debounce, &QTimer::timeout, this, &VideoFiltersDialog::renderPreview);

    setSettings(VideoFilters::current());
    commit();
}

QSlider *VideoFiltersDialog::addSlider(QGridLayout *grid, int row, const QString &name, const QString &tip, int minimum, int maximum,
                                       QLabel **value)
{
    auto *label = new QLabel(name, this);
    label->setToolTip(tip);
    auto *slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(minimum, maximum);
    slider->setToolTip(tip);
    slider->setLayoutDirection(Qt::LeftToRight); // الصفر يساراً كما في أدوات التحسين
    *value = new QLabel(QStringLiteral("0"), this);
    (*value)->setObjectName(QStringLiteral("badge"));
    (*value)->setMinimumWidth(48);
    (*value)->setAlignment(Qt::AlignCenter);
    grid->addWidget(label, row, 0);
    grid->addWidget(slider, row, 1);
    grid->addWidget(*value, row, 2);
    connect(slider, &QSlider::valueChanged, this, &VideoFiltersDialog::commit);
    return slider;
}

VideoFilters::Settings VideoFiltersDialog::settings() const
{
    VideoFilters::Settings s;
    s.quarterTurns = m_quarterTurns;
    s.mirror = m_mirror->isChecked();
    s.flip = m_flip->isChecked();
    s.deinterlace = m_deinterlace->isChecked();
    s.autoLevels = m_autoLevels->isChecked();
    s.grayscale = m_grayscale->isChecked();
    s.brightness = m_brightness->value();
    s.contrast = m_contrast->value();
    s.denoise = m_denoise->value();
    s.sharpen = m_sharpen->value();
    s.minFaceSize = m_minFace->value();
    s.skipStill = m_skipStill->isChecked();
    return s;
}

void VideoFiltersDialog::setSettings(const VideoFilters::Settings &s)
{
    m_updating = true;
    m_quarterTurns = s.quarterTurns % 4;
    m_mirror->setChecked(s.mirror);
    m_flip->setChecked(s.flip);
    m_deinterlace->setChecked(s.deinterlace);
    m_autoLevels->setChecked(s.autoLevels);
    m_grayscale->setChecked(s.grayscale);
    m_brightness->setValue(s.brightness);
    m_contrast->setValue(s.contrast);
    m_denoise->setValue(s.denoise);
    m_sharpen->setValue(s.sharpen);
    m_minFace->setValue(s.minFaceSize);
    m_skipStill->setChecked(s.skipStill);
    m_updating = false;
}

void VideoFiltersDialog::commit()
{
    if (m_updating)
        return;
    const VideoFilters::Settings s = settings();
    m_brightnessValue->setText(QString::number(s.brightness));
    m_contrastValue->setText(QString::number(s.contrast));
    m_denoiseValue->setText(QString::number(s.denoise));
    m_sharpenValue->setText(QString::number(s.sharpen));
    m_rotationValue->setText(QStringLiteral("%1°").arg(s.quarterTurns * 90));
    // الإعداد الجاهز المطابق للقيم الحالية، وإلا «مخصص»
    int preset = 3;
    for (int i = 0; i < 3; ++i) {
        if (VideoFilters::preset(VideoFilters::Preset(i), s) == s)
            preset = i;
    }
    m_preset->setCurrentIndex(preset);
    const QString summary = s.summary();
    m_summary->setText(summary.isEmpty() ? QStringLiteral("لا معالجات: يُحلَّل الفيديو كما هو")
                                         : QStringLiteral("<b>المفعّل:</b> ") + summary.toHtmlEscaped());
    QStringList notes;
    if (s.denoise > 0)
        notes << QStringLiteral("إزالة التشويش تبطئ التحليل");
    if (s.skipStill)
        notes << QStringLiteral("تخطي الساكن يسرّع كاميرات المراقبة الثابتة");
    if (s.changesGeometry())
        notes << QStringLiteral("بعد تغيير الاتجاه أعد تحليل الفيديو لتتطابق مواضع الوجوه المحفوظة");
    m_speed->setText(notes.join(QStringLiteral(" · ")));
    if (VideoFilters::current() != s) {
        VideoFilters::setCurrent(s, m_persistent);
        emit changed();
    }
    m_debounce->start();
}

void VideoFiltersDialog::renderPreview()
{
    if (m_frame.empty())
        return;
    auto show = [](QLabel *label, const cv::Mat &image) {
        const QPixmap pixmap = QPixmap::fromImage(ImageIO::toQImage(image));
        label->setPixmap(pixmap.scaled(label->size() - QSize(8, 8), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    };
    show(m_before, m_frame);
    show(m_after, VideoFilters::apply(m_frame, settings()));
}

void VideoFiltersDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_debounce)
        m_debounce->start();
}

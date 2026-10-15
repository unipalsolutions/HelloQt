#include "camerarulesdialog.h"

#include "cameras.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadioButton>
#include <QTimeEdit>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

ZoneCanvas::ZoneCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::CrossCursor);
    setMouseTracking(false);
}

void ZoneCanvas::setImage(const QImage &image)
{
    m_image = image;
    m_message.clear();
    update();
}

void ZoneCanvas::setZones(const QList<QRectF> &zones)
{
    m_zones = zones;
    update();
}

void ZoneCanvas::setExclude(bool exclude)
{
    m_exclude = exclude;
    update();
}

void ZoneCanvas::setMessage(const QString &message)
{
    m_message = message;
    update();
}

QRectF ZoneCanvas::imageRect() const
{
    // بلا صورة: مساحة بنسبة 16:9 تُرسم عليها المناطق
    const QSizeF source = m_image.isNull() ? QSizeF(16, 9) : QSizeF(m_image.size());
    QSizeF size = source.scaled(QSizeF(width() - 2, height() - 2), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
}

QPointF ZoneCanvas::toNormalized(const QPointF &point) const
{
    const QRectF area = imageRect();
    if (area.width() <= 0 || area.height() <= 0)
        return {};
    return QPointF(std::clamp((point.x() - area.x()) / area.width(), 0.0, 1.0),
                   std::clamp((point.y() - area.y()) / area.height(), 0.0, 1.0));
}

void ZoneCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(0x05, 0x0d, 0x19));
    const QRectF area = imageRect();
    if (!m_image.isNull())
        painter.drawImage(area, m_image);
    else
        painter.fillRect(area, Theme::Surface);
    if (!m_message.isEmpty()) {
        painter.setPen(Theme::Muted);
        painter.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, m_message);
    }

    const auto toWidget = [&area](const QRectF &zone) {
        return QRectF(area.x() + zone.x() * area.width(), area.y() + zone.y() * area.height(), zone.width() * area.width(),
                      zone.height() * area.height());
    };
    // وضع «داخل المناطق فقط»: ما خارجها يُعتّم ليظهر ما سيُتجاهل
    const QColor zoneColor = m_exclude ? QColor(0xff, 0x5c, 0x5c) : QColor(0x22, 0xd3, 0xee);
    if (!m_exclude && !m_zones.isEmpty()) {
        QPainterPath outside;
        outside.addRect(area);
        QPainterPath inside;
        for (const QRectF &zone : std::as_const(m_zones))
            inside.addRect(toWidget(zone));
        painter.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 140));
    }
    for (int i = 0; i < m_zones.size(); ++i) {
        const QRectF box = toWidget(m_zones.at(i));
        if (m_exclude)
            painter.fillRect(box, QColor(0xff, 0x5c, 0x5c, 70));
        painter.setPen(QPen(zoneColor, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
        const QRectF tag(box.topLeft(), QSizeF(24, 20));
        painter.fillRect(tag, zoneColor);
        painter.setPen(Qt::black);
        painter.drawText(tag, Qt::AlignCenter, QString::number(i + 1));
    }
    if (m_dragging) {
        painter.setPen(QPen(Theme::Accent2, 2, Qt::DashLine));
        painter.drawRect(QRectF(m_start, m_current).normalized());
    }
}

void ZoneCanvas::mousePressEvent(QMouseEvent *event)
{
    const QPointF point = event->position();
    if (event->button() == Qt::RightButton) {
        // أحدث منطقة تحت المؤشر تُحذف
        const QPointF normalized = toNormalized(point);
        for (int i = int(m_zones.size()) - 1; i >= 0; --i) {
            if (m_zones.at(i).contains(normalized)) {
                m_zones.removeAt(i);
                update();
                emit zonesChanged();
                return;
            }
        }
        return;
    }
    if (event->button() != Qt::LeftButton || !imageRect().contains(point))
        return;
    m_dragging = true;
    m_start = m_current = point;
    update();
}

void ZoneCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    const QRectF area = imageRect();
    m_current = QPointF(std::clamp(event->position().x(), area.left(), area.right()),
                        std::clamp(event->position().y(), area.top(), area.bottom()));
    update();
}

void ZoneCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    const QRectF zone = QRectF(toNormalized(m_start), toNormalized(m_current)).normalized();
    if (zone.width() >= 0.02 && zone.height() >= 0.02) {
        m_zones.append(zone);
        emit zonesChanged();
    }
    update();
}

CameraRulesDialog::CameraRulesDialog(const QString &source, const QString &cameraName, const QImage &frame, QWidget *parent)
    : QDialog(parent)
    , m_source(source)
{
    setWindowTitle(QStringLiteral("قواعد الكاميرا «%1»").arg(cameraName));
    resize(900, 680);
    const CameraRules::Rules rules = CameraRules::load(source);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("مناطق التحليل داخل الكادر"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("اسحب بالفأرة على الصورة لرسم منطقة، وانقر بالزر الأيمن على منطقة لحذفها. "
                                           "الوجه يُحسب حسب موقع مركزه. بلا مناطق يُحلَّل الكادر كله."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_canvas = new ZoneCanvas(this);
    m_canvas->setZones(rules.zones);
    layout->addWidget(m_canvas, 1);

    auto *modeRow = new QHBoxLayout;
    m_include = new QRadioButton(QStringLiteral("التحليل داخل المناطق فقط (باب، ممر)"), this);
    m_exclude = new QRadioButton(QStringLiteral("تجاهل ما داخل المناطق (صورة على الجدار، تلفاز، شارع)"), this);
    auto *modes = new QButtonGroup(this);
    modes->addButton(m_include);
    modes->addButton(m_exclude);
    (rules.mode == CameraRules::ZoneMode::Exclude ? m_exclude : m_include)->setChecked(true);
    m_canvas->setExclude(rules.mode == CameraRules::ZoneMode::Exclude);
    auto *clear = new FancyButton(Icons::Name::Trash, QStringLiteral("مسح المناطق"), FancyButton::Kind::Secondary, this);
    modeRow->addWidget(m_include);
    modeRow->addWidget(m_exclude);
    modeRow->addStretch();
    modeRow->addWidget(clear);
    layout->addLayout(modeRow);

    auto *scheduleTitle = new QLabel(QStringLiteral("الجدول الزمني"), this);
    scheduleTitle->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(scheduleTitle);
    auto *grid = new QGridLayout;
    const auto timeEdit = [this](const QTime &time) {
        auto *edit = new QTimeEdit(time, this);
        edit->setDisplayFormat(QStringLiteral("HH:mm"));
        edit->setLayoutDirection(Qt::LeftToRight);
        return edit;
    };
    m_alertsEnabled = new QCheckBox(QStringLiteral("التنبيهات في هذه الساعات فقط"), this);
    m_alertsEnabled->setToolTip(QStringLiteral("خارجها يُسجَّل الظهور في السجل دون تنبيه، مثلاً لا تنبيه أثناء ساعات الدوام"));
    m_alertsEnabled->setChecked(rules.alerts.enabled);
    m_alertsFrom = timeEdit(rules.alerts.from);
    m_alertsTo = timeEdit(rules.alerts.to);
    m_recordEnabled = new QCheckBox(QStringLiteral("التسجيل في هذه الساعات فقط"), this);
    m_recordEnabled->setToolTip(QStringLiteral("خارجها لا يُكتب شيء ولو كان التسجيل مفعّلاً، مثلاً التسجيل ليلاً فقط"));
    m_recordEnabled->setChecked(rules.recording.enabled);
    m_recordFrom = timeEdit(rules.recording.from);
    m_recordTo = timeEdit(rules.recording.to);
    grid->addWidget(m_alertsEnabled, 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("من"), this), 0, 1);
    grid->addWidget(m_alertsFrom, 0, 2);
    grid->addWidget(new QLabel(QStringLiteral("إلى"), this), 0, 3);
    grid->addWidget(m_alertsTo, 0, 4);
    grid->addWidget(m_recordEnabled, 1, 0);
    grid->addWidget(new QLabel(QStringLiteral("من"), this), 1, 1);
    grid->addWidget(m_recordFrom, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("إلى"), this), 1, 3);
    grid->addWidget(m_recordTo, 1, 4);
    grid->setColumnStretch(5, 1);
    layout->addLayout(grid);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("muted"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *buttons = new QHBoxLayout;
    auto *save = new FancyButton(Icons::Name::Check, QStringLiteral("حفظ"), FancyButton::Kind::Primary, this);
    auto *cancel = new FancyButton(Icons::Name::Stop, QStringLiteral("إلغاء"), FancyButton::Kind::Secondary, this);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    layout->addLayout(buttons);

    connect(save, &QAbstractButton::clicked, this, &QDialog::accept);
    connect(cancel, &QAbstractButton::clicked, this, &QDialog::reject);
    connect(clear, &QAbstractButton::clicked, this, [this] {
        m_canvas->setZones({});
        updateSummary();
    });
    connect(m_canvas, &ZoneCanvas::zonesChanged, this, &CameraRulesDialog::updateSummary);
    connect(m_exclude, &QRadioButton::toggled, this, [this](bool on) {
        m_canvas->setExclude(on);
        updateSummary();
    });
    for (QCheckBox *box : {m_alertsEnabled, m_recordEnabled})
        connect(box, &QCheckBox::toggled, this, &CameraRulesDialog::updateSummary);
    for (QTimeEdit *edit : {m_alertsFrom, m_alertsTo, m_recordFrom, m_recordTo})
        connect(edit, &QTimeEdit::timeChanged, this, &CameraRulesDialog::updateSummary);
    updateSummary();

    if (!frame.isNull()) {
        m_canvas->setImage(frame);
    } else {
        // صورة من الكاميرا في الخلفية، فلا تتجمد النافذة أثناء الاتصال
        m_canvas->setMessage(QStringLiteral("جارٍ التقاط صورة من الكاميرا…"));
        connect(&m_snapshot, &QFutureWatcher<QImage>::finished, this, [this] {
            const QImage image = m_snapshot.result();
            if (image.isNull())
                m_canvas->setMessage(QStringLiteral("تعذر التقاط صورة من الكاميرا؛ يمكن رسم المناطق على المساحة الفارغة"));
            else
                m_canvas->setImage(image);
        });
        const QString key = source.mid(7);
        m_snapshot.setFuture(QtConcurrent::run([key] {
            Cameras::Camera camera;
            if (!Cameras::resolve(key, &camera))
                return QImage();
            QString error;
            const cv::Mat frame = Cameras::snapshot(camera, &error);
            return frame.empty() ? QImage() : ImageIO::toQImage(frame);
        }));
    }
}

CameraRulesDialog::~CameraRulesDialog()
{
    m_snapshot.waitForFinished();
}

CameraRules::Rules CameraRulesDialog::rules() const
{
    CameraRules::Rules rules;
    rules.zones = m_canvas->zones();
    rules.mode = m_exclude->isChecked() ? CameraRules::ZoneMode::Exclude : CameraRules::ZoneMode::Include;
    rules.alerts = {m_alertsEnabled->isChecked(), m_alertsFrom->time(), m_alertsTo->time()};
    rules.recording = {m_recordEnabled->isChecked(), m_recordFrom->time(), m_recordTo->time()};
    return rules;
}

void CameraRulesDialog::updateSummary()
{
    m_summary->setText(QStringLiteral("الخلاصة: %1").arg(rules().summary()));
}

void CameraRulesDialog::accept()
{
    CameraRules::save(m_source, rules());
    QDialog::accept();
}

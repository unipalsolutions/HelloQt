#include "recordingsdialog.h"

#include "clipexport.h"

#include "cameras.h"
#include "carddelegates.h"
#include "facetypes.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int NameWidth = 130;
constexpr int AxisHeight = 22;
constexpr int LaneHeight = 30;
constexpr int LaneGap = 6;
constexpr int WeekDays = 7;

const QColor WatchedColor(0xff, 0x5c, 0x5c);
const QColor VisitorColor(0x9c, 0xa3, 0xaf);

QString timeText(const QDateTime &time)
{
    return QStringLiteral("⁦%1⁩").arg(time.toString(QStringLiteral("HH:mm:ss")));
}

} // namespace

DayTimeline::DayTimeline(QWidget *parent)
    : QWidget(parent)
{
    setLayoutDirection(Qt::LeftToRight); // الزمن يتقدم من اليسار إلى اليمين كما في الخط الزمني للفيديو
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize DayTimeline::sizeHint() const
{
    const int lanes = std::max<qsizetype>(1, m_lanes.size());
    return {900, AxisHeight + 8 + int(lanes) * (LaneHeight + LaneGap)};
}

QSize DayTimeline::minimumSizeHint() const
{
    return {400, sizeHint().height()};
}

void DayTimeline::setDay(const QDate &day, const QList<Lane> &lanes)
{
    m_day = day;
    m_lanes = lanes;
    m_selected = -1;
    updateGeometry();
    setFixedHeight(sizeHint().height());
    update();
}

void DayTimeline::setSelected(int index)
{
    m_selected = index;
    update();
}

QRectF DayTimeline::plotRect() const
{
    return QRectF(NameWidth, AxisHeight, width() - NameWidth - 12, height() - AxisHeight - 4);
}

QRectF DayTimeline::laneRect(int lane) const
{
    const QRectF plot = plotRect();
    return QRectF(plot.left(), plot.top() + 4 + lane * (LaneHeight + LaneGap), plot.width(), LaneHeight);
}

double DayTimeline::xOf(const QDateTime &time) const
{
    const QRectF plot = plotRect();
    const qint64 start = QDateTime(m_day, QTime(0, 0)).toMSecsSinceEpoch();
    const double t = double(time.toMSecsSinceEpoch() - start) / (24.0 * 3600 * 1000);
    return plot.left() + std::clamp(t, 0.0, 1.0) * plot.width();
}

QDateTime DayTimeline::timeAt(double x) const
{
    const QRectF plot = plotRect();
    const double t = std::clamp((x - plot.left()) / std::max(1.0, plot.width()), 0.0, 1.0);
    return QDateTime(m_day, QTime(0, 0)).addMSecs(qint64(t * 24 * 3600 * 1000));
}

int DayTimeline::laneAt(const QPointF &point) const
{
    for (int i = 0; i < m_lanes.size(); ++i) {
        if (laneRect(i).adjusted(-NameWidth, -LaneGap / 2.0, 0, LaneGap / 2.0).contains(point))
            return i;
    }
    return -1;
}

const DayTimeline::Marker *DayTimeline::markerAt(const QPointF &point) const
{
    const int lane = laneAt(point);
    if (lane < 0)
        return nullptr;
    const Marker *best = nullptr;
    double bestDistance = 6.0;
    for (const Marker &marker : m_lanes.at(lane).markers) {
        const double distance = std::abs(xOf(marker.time) - point.x());
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = &marker;
        }
    }
    return best;
}

void DayTimeline::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Theme::Surface);
    const QRectF plot = plotRect();

    // محور الساعات
    QFont font = painter.font();
    font.setPointSizeF(8.5);
    painter.setFont(font);
    for (int hour = 0; hour <= 24; ++hour) {
        const double x = plot.left() + plot.width() * hour / 24.0;
        painter.setPen(QPen(Theme::withAlpha(Theme::Border, hour % 6 == 0 ? 255 : 120), 1));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        if (hour % 2 == 0 && hour < 24) {
            painter.setPen(Theme::Muted);
            painter.drawText(QRectF(x - 20, 2, 40, AxisHeight - 4), Qt::AlignCenter, QStringLiteral("%1").arg(hour, 2, 10, QLatin1Char('0')));
        }
    }
    // الآن
    if (m_day == QDate::currentDate()) {
        const double x = xOf(QDateTime::currentDateTime());
        painter.setPen(QPen(Theme::Accent2, 1.5, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    if (m_lanes.isEmpty()) {
        painter.setPen(Theme::Muted);
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("لا تسجيلات ولا ظهور في هذا اليوم"));
        return;
    }
    for (int i = 0; i < m_lanes.size(); ++i) {
        const Lane &lane = m_lanes.at(i);
        const QRectF row = laneRect(i);
        painter.setPen(Theme::Text);
        painter.drawText(QRectF(6, row.top(), NameWidth - 12, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(lane.name, Qt::ElideRight, NameWidth - 12));
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::SurfaceHigh);
        painter.drawRoundedRect(row, 4, 4);
        // فترات التسجيل
        painter.setBrush(Theme::withAlpha(Theme::Accent, 150));
        for (const CameraRecorder::RecordingFile &file : lane.recordings) {
            const double left = xOf(file.start);
            const double right = std::max(left + 2.0, xOf(file.end));
            painter.drawRoundedRect(QRectF(left, row.top() + 6, right - left, row.height() - 12), 2, 2);
        }
        // الظهور
        for (const Marker &marker : lane.markers) {
            const double x = xOf(marker.time);
            const bool selected = marker.index == m_selected;
            painter.setPen(selected ? QPen(Theme::Text, 2) : Qt::NoPen);
            painter.setBrush(marker.color);
            const double r = selected ? 6.5 : 4.5;
            painter.drawEllipse(QPointF(x, row.center().y()), r, r);
        }
    }
}

void DayTimeline::mousePressEvent(QMouseEvent *event)
{
    if (const Marker *marker = markerAt(event->position())) {
        m_selected = marker->index;
        update();
        emit markerClicked(marker->index);
    }
}

void DayTimeline::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (markerAt(event->position()))
        return; // الظهور يُفتح من قائمته عند لحظته بالضبط
    const int lane = laneAt(event->position());
    if (lane < 0)
        return;
    const QDateTime time = timeAt(event->position().x());
    for (const CameraRecorder::RecordingFile &file : m_lanes.at(lane).recordings) {
        if (time >= file.start && time <= file.end) {
            emit recordingActivated(file.path, file.start.msecsTo(time));
            return;
        }
    }
}

void DayTimeline::mouseMoveEvent(QMouseEvent *event)
{
    const int lane = laneAt(event->position());
    if (lane < 0 || event->position().x() < plotRect().left()) {
        QToolTip::hideText();
        return;
    }
    const QDateTime time = timeAt(event->position().x());
    QString text = time.toString(QStringLiteral("HH:mm"));
    for (const CameraRecorder::RecordingFile &file : m_lanes.at(lane).recordings) {
        if (time >= file.start && time <= file.end) {
            text += QStringLiteral("\nتسجيل %1 ← %2 (نقر مزدوج لفتحه هنا)")
                        .arg(file.start.toString(QStringLiteral("HH:mm")), file.end.toString(QStringLiteral("HH:mm")));
            break;
        }
    }
    QToolTip::showText(event->globalPosition().toPoint(), text, this);
}

RecordingsDialog::RecordingsDialog(const FaceDatabase *database, const QSet<QString> &watched, QWidget *parent)
    : QDialog(parent)
    , m_database(database)
    , m_watched(watched)
    , m_date(new QDateEdit(QDate::currentDate(), this))
    , m_camera(new QComboBox(this))
    , m_person(new QComboBox(this))
    , m_summary(new QLabel(this))
    , m_timeline(new DayTimeline(this))
    , m_list(new QListWidget(this))
{
    setWindowTitle(QStringLiteral("تسجيلات الكاميرات"));
    resize(1100, 740);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("تسجيلات الكاميرات"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("لكل كاميرا صف فيه فترات تسجيلها (أزرق) ومن ظهر أمامها: المراقَبون بالأحمر، "
                                           "المعروفون بالسماوي، والزوار المجهولون بالرمادي. انقر نقراً مزدوجاً على فترة تسجيل لفتحها "
                                           "عند تلك اللحظة، أو على ظهور في القائمة لفتح التسجيل عند لحظته."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(8);
    auto *previous = new FancyButton(Icons::Name::Next, QString(), FancyButton::Kind::Secondary, this);
    previous->setToolTip(QStringLiteral("اليوم السابق"));
    auto *next = new FancyButton(Icons::Name::Previous, QString(), FancyButton::Kind::Secondary, this);
    next->setToolTip(QStringLiteral("اليوم التالي"));
    auto *today = new FancyButton(Icons::Name::Clock, QStringLiteral("اليوم"), FancyButton::Kind::Secondary, this);
    m_date->setCalendarPopup(true);
    m_date->setDisplayFormat(QStringLiteral("yyyy/MM/dd"));
    m_date->setMaximumDate(QDate::currentDate());
    m_camera->addItem(QStringLiteral("كل الكاميرات"), QString());
    m_person->setMinimumWidth(200);
    m_person->setToolTip(QStringLiteral("اختر شخصاً لعرض كل ظهوره في الأيام السبعة المنتهية بالتاريخ المختار"));
    controls->addWidget(new QLabel(QStringLiteral("اليوم:"), this));
    controls->addWidget(previous);
    controls->addWidget(m_date);
    controls->addWidget(next);
    controls->addWidget(today);
    controls->addSpacing(10);
    controls->addWidget(new QLabel(QStringLiteral("الكاميرا:"), this));
    controls->addWidget(m_camera);
    controls->addSpacing(10);
    controls->addWidget(new QLabel(QStringLiteral("أين ظهر:"), this));
    controls->addWidget(m_person);
    controls->addStretch(1);
    layout->addLayout(controls);

    m_summary->setObjectName(QStringLiteral("muted"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    layout->addWidget(m_timeline);

    m_list->setItemDelegate(new FaceCardDelegate(m_list));
    m_list->setUniformItemSizes(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setMouseTracking(true);
    layout->addWidget(m_list, 1);

    auto *buttons = new QHBoxLayout;
    m_open = new FancyButton(Icons::Name::Film, QStringLiteral("فتح التسجيل"), FancyButton::Kind::Primary, this);
    m_open->setToolTip(QStringLiteral("فتح تسجيل الكاميرا عند لحظة الظهور المختار"));
    m_snapshot = new FancyButton(Icons::Name::Camera, QStringLiteral("لقطة الإطار"), FancyButton::Kind::Secondary, this);
    m_clip = new FancyButton(Icons::Name::Export, QStringLiteral("تصدير المقطع…"), FancyButton::Kind::Secondary, this);
    m_clip->setToolTip(QStringLiteral("حفظ 30 ثانية حول لحظة الظهور في ملف واحد جاهز للتسليم"));
    auto *folder = new FancyButton(Icons::Name::Folder, QStringLiteral("مجلد التسجيلات"), FancyButton::Kind::Secondary, this);
    auto *close = new FancyButton(Icons::Name::Stop, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, this);
    buttons->addWidget(m_open);
    buttons->addWidget(m_snapshot);
    buttons->addWidget(m_clip);
    buttons->addWidget(folder);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(previous, &QAbstractButton::clicked, this, [this] { m_date->setDate(m_date->date().addDays(-1)); });
    connect(next, &QAbstractButton::clicked, this, [this] { m_date->setDate(m_date->date().addDays(1)); });
    connect(today, &QAbstractButton::clicked, this, [this] { m_date->setDate(QDate::currentDate()); });
    connect(m_date, &QDateEdit::dateChanged, this, &RecordingsDialog::reload);
    connect(m_camera, &QComboBox::currentIndexChanged, this, &RecordingsDialog::reload);
    connect(m_person, &QComboBox::currentIndexChanged, this, &RecordingsDialog::reload);
    connect(m_timeline, &DayTimeline::markerClicked, this, [this](int index) {
        for (int row = 0; row < m_list->count(); ++row) {
            if (m_list->item(row)->data(Qt::UserRole + 70).toInt() == index) {
                m_list->setCurrentRow(row);
                m_list->scrollToItem(m_list->item(row));
                break;
            }
        }
    });
    connect(m_timeline, &DayTimeline::recordingActivated, this, [this](const QString &path, qint64 positionMs) {
        emit openFileRequested(path, positionMs);
        accept();
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        select(row >= 0 ? m_list->item(row)->data(Qt::UserRole + 70).toInt() : -1);
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { openEntry(item->data(Qt::UserRole + 70).toInt()); });
    connect(m_open, &QAbstractButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_list->currentItem())
            openEntry(item->data(Qt::UserRole + 70).toInt());
    });
    connect(m_snapshot, &QAbstractButton::clicked, this, [this] {
        const QListWidgetItem *item = m_list->currentItem();
        const int index = item ? item->data(Qt::UserRole + 70).toInt() : -1;
        if (index >= 0 && index < m_entries.size())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_entries.at(index).sighting.snapshot));
    });
    connect(m_clip, &QAbstractButton::clicked, this, [this] {
        const QListWidgetItem *item = m_list->currentItem();
        const int index = item ? item->data(Qt::UserRole + 70).toInt() : -1;
        if (index < 0 || index >= m_entries.size())
            return;
        const Entry &entry = m_entries.at(index);
        ClipExport::exportAround(this, entry.sighting.source, entry.sighting.positionMs,
                                 QStringLiteral("%1 %2").arg(entry.name, entry.sighting.seen.toString(QStringLiteral("yyyy-MM-dd HH-mm"))));
    });
    connect(folder, &QAbstractButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(CameraRecorder::savedFolder())); });
    connect(close, &QAbstractButton::clicked, this, &QDialog::reject);

    fillPersons();
    reload();
}

QString RecordingsDialog::laneOf(const QString &camera) const
{
    return CameraRecorder::fileCameraName(Cameras::displayName(camera));
}

void RecordingsDialog::fillPersons()
{
    // من ظهر أمام الكاميرات في آخر 30 يوماً، الأكثر ظهوراً أولاً
    const QDateTime to = QDateTime(QDate::currentDate().addDays(1), QTime(0, 0));
    QHash<QString, QPair<QString, int>> counts;
    for (const SightingEntry &entry : m_database->cameraSightingsBetween(to.addDays(-30), to)) {
        auto &count = counts[entry.personId];
        count.first = entry.name;
        ++count.second;
    }
    QList<QPair<QString, QPair<QString, int>>> persons;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        persons.append({it.key(), it.value()});
    std::sort(persons.begin(), persons.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
    const QSignalBlocker blocker(m_person);
    m_person->clear();
    m_person->addItem(QStringLiteral("الكل في هذا اليوم"), QString());
    for (const auto &person : persons) {
        const QString name = m_watched.contains(person.first) ? QStringLiteral("⚠ ") + person.second.first : person.second.first;
        m_person->addItem(QStringLiteral("%1 (%2)").arg(name).arg(person.second.second), person.first);
    }
}

void RecordingsDialog::reload()
{
    const QDate day = m_date->date();
    const QDateTime dayStart(day, QTime(0, 0));
    const QDateTime dayEnd(day.addDays(1), QTime(0, 0));
    const QString personId = m_person->currentData().toString();
    const QString cameraFilter = m_camera->currentData().toString();
    // شخص مختار: ظهوره في الأيام السبعة المنتهية بهذا اليوم
    const QDateTime listStart = personId.isEmpty() ? dayStart : dayEnd.addDays(-WeekDays);

    m_entries.clear();
    for (const SightingEntry &found : m_database->cameraSightingsBetween(listStart, dayEnd, personId)) {
        Entry entry;
        entry.sighting = found.sighting;
        entry.personId = found.personId;
        entry.name = found.name;
        entry.watched = m_watched.contains(found.personId);
        entry.lane = laneOf(found.sighting.camera);
        m_entries.append(entry);
    }
    if (personId.isEmpty()) {
        for (const VisitorSighting &found : m_database->visitorSightingsBetween(dayStart, dayEnd)) {
            Entry entry;
            entry.sighting = found.sighting;
            entry.name = QStringLiteral("زائر مجهول #%1").arg(found.visitorId);
            entry.lane = laneOf(found.sighting.camera);
            m_entries.append(entry);
        }
        std::sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) { return a.sighting.seen < b.sighting.seen; });
    }

    const QList<CameraRecorder::RecordingFile> files = CameraRecorder::recordings(dayStart, dayEnd);
    // صفوف الكاميرات: كل كاميرا لها تسجيل أو ظهور في هذا اليوم
    QMap<QString, DayTimeline::Lane> lanes;
    QStringList cameras;
    for (const CameraRecorder::RecordingFile &file : files) {
        lanes[file.camera].recordings.append(file);
        cameras << file.camera;
    }
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        cameras << entry.lane;
        if (entry.sighting.seen < dayStart)
            continue;
        const QColor color = entry.personId.isEmpty() ? VisitorColor : entry.watched ? WatchedColor : Theme::Success;
        lanes[entry.lane].markers.append({entry.sighting.seen, color, i});
    }
    cameras.removeDuplicates();
    std::sort(cameras.begin(), cameras.end());
    {
        const QSignalBlocker blocker(m_camera);
        m_camera->clear();
        m_camera->addItem(QStringLiteral("كل الكاميرات"), QString());
        for (const QString &camera : std::as_const(cameras))
            m_camera->addItem(camera, camera);
        const int index = m_camera->findData(cameraFilter);
        m_camera->setCurrentIndex(index >= 0 ? index : 0);
    }
    const QString camera = m_camera->currentData().toString();
    QList<DayTimeline::Lane> shown;
    for (auto it = lanes.begin(); it != lanes.end(); ++it) {
        if (!camera.isEmpty() && it.key() != camera)
            continue;
        it->name = it.key();
        shown.append(*it);
    }
    m_timeline->setDay(day, shown);

    m_list->clear();
    int known = 0, visitors = 0, alerts = 0;
    QSet<QString> persons;
    for (int i = m_entries.size() - 1; i >= 0; --i) { // الأحدث أولاً
        const Entry &entry = m_entries.at(i);
        if (!camera.isEmpty() && entry.lane != camera)
            continue;
        if (entry.personId.isEmpty()) {
            ++visitors;
        } else {
            ++known;
            persons.insert(entry.personId);
            alerts += entry.watched ? 1 : 0;
        }
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole + 70, i);
        item->setData(CardRole::Id, entry.personId);
        item->setData(CardRole::Title, entry.watched ? QStringLiteral("⚠ ") + entry.name : entry.name);
        QString where = QStringLiteral("⁦%1⁩ · %2")
                            .arg(entry.sighting.seen.toString(personId.isEmpty() ? QStringLiteral("HH:mm:ss")
                                                                                 : QStringLiteral("yyyy/MM/dd HH:mm:ss")),
                                 entry.lane);
        if (entry.sighting.positionMs >= 0 && !entry.sighting.source.startsWith(QStringLiteral("camera:"))
            && !entry.sighting.source.isEmpty())
            where += QStringLiteral(" · في التسجيل عند ⁦%1⁩").arg(ImageIO::formatTime(entry.sighting.positionMs));
        else
            where += QStringLiteral(" · لم يكن يُسجَّل");
        item->setData(CardRole::Subtitle, where);
        item->setData(CardRole::Status, int(entry.personId.isEmpty() ? MatchStatus::Unknown : MatchStatus::Confirmed));
        item->setData(CardRole::StatusText, entry.personId.isEmpty() ? QStringLiteral("مجهول")
                                            : entry.watched           ? QStringLiteral("مراقَب")
                                                                      : QStringLiteral("معروف"));
        if (!entry.personId.isEmpty())
            item->setData(CardRole::Score, entry.sighting.score);
        item->setData(CardRole::Avatar, circularPixmap(entry.sighting.thumbnail, 54));
    }

    qint64 recordedMs = 0;
    for (const DayTimeline::Lane &lane : std::as_const(shown)) {
        for (const CameraRecorder::RecordingFile &file : lane.recordings)
            recordedMs += std::max(file.start, dayStart).msecsTo(std::min(file.end, dayEnd));
    }
    QString summary;
    if (!personId.isEmpty()) {
        summary = QStringLiteral("«%1»: %2 مرة ظهور في الأيام السبعة حتى ⁦%3⁩")
                      .arg(m_person->currentText().section(QStringLiteral(" ("), 0, 0))
                      .arg(known)
                      .arg(day.toString(QStringLiteral("yyyy/MM/dd")));
    } else {
        summary = QStringLiteral("الكاميرات: %1 · ساعات التسجيل: %2 · ظهور معروفين: %3 (%4 أشخاص) · تنبيهات: %5 · زوار مجهولون: %6")
                      .arg(shown.size())
                      .arg(QString::number(recordedMs / 3600000.0, 'f', 1))
                      .arg(known)
                      .arg(persons.size())
                      .arg(alerts)
                      .arg(visitors);
    }
    m_summary->setText(summary);
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    else
        select(-1);
}

void RecordingsDialog::select(int index)
{
    m_timeline->setSelected(index);
    const bool valid = index >= 0 && index < m_entries.size();
    const Sighting *sighting = valid ? &m_entries.at(index).sighting : nullptr;
    m_open->setEnabled(sighting && !sighting->source.isEmpty() && !sighting->source.startsWith(QStringLiteral("camera:")));
    m_snapshot->setEnabled(sighting && !sighting->snapshot.isEmpty() && QFileInfo::exists(sighting->snapshot));
    m_clip->setEnabled(m_open->isEnabled() && sighting->positionMs >= 0);
}

void RecordingsDialog::openEntry(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;
    const Sighting &sighting = m_entries.at(index).sighting;
    if (sighting.source.isEmpty() || sighting.source.startsWith(QStringLiteral("camera:"))) {
        Toast::show(this, QStringLiteral("لم تكن الكاميرا تُسجَّل لحظة هذا الظهور؛ لقطة الإطار وحدها محفوظة"), Toast::Kind::Info);
        return;
    }
    if (!QFileInfo::exists(sighting.source)) {
        Toast::show(this, QStringLiteral("التسجيل غير موجود: ما زال يُسجَّل (يُفتح بعد إيقاف التسجيل أو انتهاء جزئه)، أو حُذف"),
                    Toast::Kind::Warning);
        return;
    }
    emit openFileRequested(sighting.source, sighting.positionMs);
    accept();
}

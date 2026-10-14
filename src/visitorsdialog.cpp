#include "visitorsdialog.h"

#include "cameras.h"
#include "camerawatch.h"
#include "carddelegates.h"
#include "facetypes.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int IndexRole = Qt::UserRole + 60;
constexpr int MaxVisitors = 500;

QString camerasText(const QStringList &cameras)
{
    QStringList names;
    for (const QString &camera : cameras)
        names << Cameras::displayName(camera);
    return names.join(QStringLiteral("، "));
}

} // namespace

VisitorsDialog::VisitorsDialog(FaceDatabase *database, QWidget *parent)
    : QDialog(parent)
    , m_database(database)
    , m_period(new QComboBox(this))
    , m_repeated(new QCheckBox(QStringLiteral("من تكرر ظهوره فقط"), this))
    , m_enabled(new QCheckBox(QStringLiteral("حفظ الزوار المجهولين"), this))
    , m_summary(new QLabel(this))
    , m_list(new QListWidget(this))
    , m_details(new QLabel(this))
    , m_sightings(new QListWidget(this))
{
    setWindowTitle(QStringLiteral("الزوار المجهولون"));
    resize(980, 680);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("الزوار المجهولون"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("وجوه غير مسجلة ظهرت أمام الكاميرات. الوجه نفسه يُجمع تحت زائر واحد ببصمته مهما تكرر ظهوره، "
                                           "ويُكتب ظهوره من جديد إن عاد بعد غياب 5 دقائق. سمِّ الزائر بإضافته إلى شخص فينتقل ظهوره كله إلى سجله. "
                                           "يُحذف تلقائياً من لم يظهر 90 يوماً."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(10);
    m_period->addItem(QStringLiteral("اليوم"), 0);
    m_period->addItem(QStringLiteral("آخر 7 أيام"), 7);
    m_period->addItem(QStringLiteral("آخر 30 يوماً"), 30);
    m_period->addItem(QStringLiteral("كل الأوقات"), -1);
    m_period->setCurrentIndex(1);
    m_repeated->setToolTip(QStringLiteral("من ظهر مرتين على الأقل (في أوقات متباعدة)، فيظهر المترددون ويختفي المارّون مرة واحدة"));
    m_enabled->setChecked(CameraWatch::visitorsEnabled());
    m_enabled->setToolTip(QStringLiteral("عند إيقافه لا تُحفظ الوجوه غير المسجلة، ويبقى سجل ظهور الأشخاص المعروفين"));
    controls->addWidget(new QLabel(QStringLiteral("الفترة:"), this));
    controls->addWidget(m_period);
    controls->addSpacing(8);
    controls->addWidget(m_repeated);
    controls->addStretch(1);
    controls->addWidget(m_enabled);
    layout->addLayout(controls);

    m_summary->setObjectName(QStringLiteral("muted"));
    layout->addWidget(m_summary);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    m_list->setItemDelegate(new FaceCardDelegate(m_list));
    m_list->setUniformItemSizes(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setMouseTracking(true);
    splitter->addWidget(m_list);

    auto *side = new QWidget(splitter);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(8);
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    sideLayout->addWidget(m_details);
    m_sightings->setItemDelegate(new FaceCardDelegate(m_sightings));
    m_sightings->setUniformItemSizes(true);
    m_sightings->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_sightings->setToolTip(QStringLiteral("نقر مزدوج لفتح التسجيل عند لحظة الظهور"));
    sideLayout->addWidget(m_sightings, 1);
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto *buttons = new QHBoxLayout;
    m_assign = new FancyButton(Icons::Name::UserPlus, QStringLiteral("تسمية…"), FancyButton::Kind::Primary, this);
    m_assign->setToolTip(QStringLiteral("إضافة الزائر إلى شخص جديد أو موجود: تُضاف صورته إلى صوره وينتقل ظهوره إلى سجله"));
    m_remove = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف"), FancyButton::Kind::Danger, this);
    m_open = new FancyButton(Icons::Name::Film, QStringLiteral("فتح التسجيل"), FancyButton::Kind::Secondary, this);
    m_open->setToolTip(QStringLiteral("فتح تسجيل الكاميرا عند لحظة الظهور المختار"));
    m_snapshot = new FancyButton(Icons::Name::Camera, QStringLiteral("لقطة الإطار"), FancyButton::Kind::Secondary, this);
    auto *close = new FancyButton(Icons::Name::Stop, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, this);
    buttons->addWidget(m_assign);
    buttons->addWidget(m_remove);
    buttons->addSpacing(12);
    buttons->addWidget(m_open);
    buttons->addWidget(m_snapshot);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_period, &QComboBox::currentIndexChanged, this, &VisitorsDialog::reload);
    connect(m_repeated, &QCheckBox::toggled, this, &VisitorsDialog::reload);
    connect(m_enabled, &QCheckBox::toggled, this, [](bool on) { CameraWatch::setVisitorsEnabled(on); });
    connect(m_list, &QListWidget::currentRowChanged, this, &VisitorsDialog::showSightings);
    connect(m_sightings, &QListWidget::currentRowChanged, this, &VisitorsDialog::updateButtons);
    connect(m_sightings, &QListWidget::itemDoubleClicked, this, &VisitorsDialog::openSighting);
    connect(m_open, &QAbstractButton::clicked, this, &VisitorsDialog::openSighting);
    connect(m_snapshot, &QAbstractButton::clicked, this, [this] {
        if (const Sighting *sighting = currentSighting())
            QDesktopServices::openUrl(QUrl::fromLocalFile(sighting->snapshot));
    });
    connect(m_assign, &QAbstractButton::clicked, this, [this] {
        const qint64 id = currentVisitor();
        if (id > 0)
            emit assignRequested(id);
    });
    connect(m_remove, &QAbstractButton::clicked, this, [this] {
        const qint64 id = currentVisitor();
        if (id <= 0)
            return;
        if (QMessageBox::question(this, QStringLiteral("حذف الزائر"),
                                  QStringLiteral("حذف الزائر وكل ظهوره؟ إن عاد للظهور يُحفظ زائراً جديداً."))
            != QMessageBox::Yes)
            return;
        const int row = m_list->currentRow();
        if (m_database->removeVisitor(id)) {
            reload();
            m_list->setCurrentRow(std::min(row, m_list->count() - 1));
        }
    });
    connect(close, &QAbstractButton::clicked, this, &QDialog::reject);
    connect(m_database, &FaceDatabase::visitorsChanged, this, [this] {
        // ظهور جديد أثناء فتح النافذة: يبقى الاختيار على الزائر نفسه
        const qint64 id = currentVisitor();
        reload();
        for (int i = 0; i < m_visitors.size(); ++i) {
            if (m_visitors.at(i).id == id) {
                m_list->setCurrentRow(i);
                break;
            }
        }
    });

    reload();
}

QDateTime VisitorsDialog::since() const
{
    const int days = m_period->currentData().toInt();
    if (days == 0)
        return QDateTime(QDate::currentDate(), QTime(0, 0));
    if (days > 0)
        return QDateTime::currentDateTime().addDays(-days);
    return {};
}

void VisitorsDialog::reload()
{
    m_visitors = m_database->visitors(since(), m_repeated->isChecked() ? 2 : 1, MaxVisitors);
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    int total = 0;
    for (int i = 0; i < m_visitors.size(); ++i) {
        const Visitor &visitor = m_visitors.at(i);
        total += visitor.seenCount;
        auto *item = new QListWidgetItem(m_list);
        item->setData(IndexRole, i);
        item->setData(CardRole::Id, QString::number(visitor.id));
        item->setData(CardRole::Title, QStringLiteral("زائر مجهول #%1").arg(visitor.id));
        item->setData(CardRole::Subtitle, QStringLiteral("آخر ظهور ⁦%1⁩ · %2")
                                              .arg(visitor.lastSeen.toString(QStringLiteral("yyyy/MM/dd hh:mm")),
                                                   camerasText(visitor.cameras)));
        item->setData(CardRole::Status, int(visitor.seenCount > 1 ? MatchStatus::Possible : MatchStatus::Unknown));
        item->setData(CardRole::StatusText, visitor.seenCount > 1 ? QStringLiteral("ظهر %1 مرات").arg(visitor.seenCount)
                                                                   : QStringLiteral("مرة واحدة"));
        item->setData(CardRole::Avatar, circularPixmap(visitor.image, 54));
    }
    if (m_visitors.isEmpty()) {
        m_summary->setText(CameraWatch::visitorsEnabled()
                               ? QStringLiteral("لا زوار مجهولين في هذه الفترة. يُحفظون أثناء مراقبة الكاميرا (الواحدة أو عدة كاميرات).")
                               : QStringLiteral("حفظ الزوار المجهولين متوقف؛ فعّله من الخيار أعلاه."));
    } else {
        QString text = QStringLiteral("الزوار: %1 · مرات الظهور: %2").arg(m_visitors.size()).arg(total);
        if (m_visitors.size() >= MaxVisitors)
            text += QStringLiteral(" (أحدث %1 فقط)").arg(MaxVisitors);
        m_summary->setText(text);
    }
    if (!m_visitors.isEmpty())
        m_list->setCurrentRow(0);
    showSightings();
}

qint64 VisitorsDialog::currentVisitor() const
{
    const QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return -1;
    const int index = item->data(IndexRole).toInt();
    return index >= 0 && index < m_visitors.size() ? m_visitors.at(index).id : -1;
}

const Sighting *VisitorsDialog::currentSighting() const
{
    const int row = m_sightings->currentRow();
    return row >= 0 && row < m_current.size() ? &m_current.at(row) : nullptr;
}

void VisitorsDialog::showSightings()
{
    m_sightings->clear();
    m_current.clear();
    const int row = m_list->currentRow();
    const QListWidgetItem *item = m_list->currentItem();
    const int index = item ? item->data(IndexRole).toInt() : -1;
    if (row < 0 || index < 0 || index >= m_visitors.size()) {
        m_details->setText(QString());
        updateButtons();
        return;
    }
    const Visitor &visitor = m_visitors.at(index);
    m_details->setText(QStringLiteral("<div style='font-size:13pt; font-weight:600; color:%1'>زائر مجهول #%2</div>"
                                      "<div style='color:%3'>أول ظهور ⁦%4⁩ · آخر ظهور ⁦%5⁩ · %6 مرات<br>الكاميرات: %7</div>")
                           .arg(Theme::Text.name())
                           .arg(visitor.id)
                           .arg(Theme::Muted.name(), visitor.firstSeen.toString(QStringLiteral("yyyy/MM/dd hh:mm")),
                                visitor.lastSeen.toString(QStringLiteral("yyyy/MM/dd hh:mm")))
                           .arg(visitor.seenCount)
                           .arg(camerasText(visitor.cameras).toHtmlEscaped()));
    m_current = m_database->visitorSightings(visitor.id);
    for (const Sighting &sighting : std::as_const(m_current)) {
        auto *entry = new QListWidgetItem(m_sightings);
        entry->setData(CardRole::Title, sighting.seen.toString(QStringLiteral("yyyy/MM/dd hh:mm:ss")));
        QString where = Cameras::displayName(sighting.camera);
        if (sighting.positionMs >= 0 && !sighting.source.isEmpty())
            where += QStringLiteral(" · تسجيل عند ⁦%1⁩").arg(ImageIO::formatTime(sighting.positionMs));
        entry->setData(CardRole::Subtitle, where);
        entry->setData(CardRole::Status, int(MatchStatus::Unknown));
        entry->setData(CardRole::StatusText, QStringLiteral("كاميرا"));
        entry->setData(CardRole::Avatar, circularPixmap(sighting.thumbnail, 54));
    }
    if (!m_current.isEmpty())
        m_sightings->setCurrentRow(0);
    updateButtons();
}

void VisitorsDialog::updateButtons()
{
    const bool visitor = currentVisitor() > 0;
    const Sighting *sighting = currentSighting();
    m_assign->setEnabled(visitor);
    m_remove->setEnabled(visitor);
    m_open->setEnabled(sighting && !sighting->source.isEmpty() && !sighting->source.startsWith(QStringLiteral("camera:")));
    m_snapshot->setEnabled(sighting && !sighting->snapshot.isEmpty() && QFileInfo::exists(sighting->snapshot));
}

void VisitorsDialog::openSighting()
{
    const Sighting *sighting = currentSighting();
    if (!sighting || sighting->source.isEmpty() || sighting->source.startsWith(QStringLiteral("camera:")))
        return;
    if (!QFileInfo::exists(sighting->source)) {
        Toast::show(this, QStringLiteral("التسجيل غير موجود: ما زال يُسجَّل (يُفتح بعد إيقاف التسجيل أو انتهاء جزئه)، أو حُذف"),
                    Toast::Kind::Warning);
        return;
    }
    emit openFileRequested(sighting->source, sighting->positionMs);
    accept();
}

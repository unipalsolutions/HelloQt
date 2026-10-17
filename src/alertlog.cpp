#include "alertlog.h"

#include "clipexport.h"

#include "cameras.h"
#include "carddelegates.h"
#include "facetypes.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSet>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {

constexpr int EntryRole = Qt::UserRole + 50;
constexpr int MaxEntries = 2000;

bool isCamera(const QString &source)
{
    return source.startsWith(QStringLiteral("camera:"));
}

bool fromCamera(const Sighting &sighting)
{
    return !sighting.camera.isEmpty() || isCamera(sighting.source);
}

QString sourceName(const Sighting &sighting)
{
    if (isCamera(sighting.source))
        return Cameras::displayName(sighting.source);
    if (!sighting.camera.isEmpty())
        return QStringLiteral("%1 · تسجيل %2").arg(Cameras::displayName(sighting.camera), QFileInfo(sighting.source).fileName());
    return QFileInfo(sighting.source).fileName();
}

QString csvField(QString text)
{
    // Excel ينفّذ الخلية التي تبدأ بهذه الرموز كمعادلة؛ اسم شخص أو ملف قد يبدأ بها
    if (!text.isEmpty() && QStringLiteral("=+-@\t\r").contains(text.front()))
        text.prepend(QLatin1Char('\''));
    if (text.contains(QLatin1Char(',')) || text.contains(QLatin1Char('"')) || text.contains(QLatin1Char('\n')))
        text = QLatin1Char('"') + text.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
    return text;
}

} // namespace

AlertLogDialog::AlertLogDialog(const FaceDatabase *database, QWidget *parent)
    : QDialog(parent)
    , m_database(database)
    , m_scope(new QComboBox(this))
    , m_period(new QComboBox(this))
    , m_summary(new QLabel(this))
    , m_list(new QListWidget(this))
{
    setWindowTitle(QStringLiteral("سجل التنبيهات"));
    resize(760, 640);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("سجل التنبيهات"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("كل مرة ظهر فيها شخص من قائمة المراقبة في صورة أو فيديو أو أمام الكاميرا، الأحدث أولاً. "
                                           "ظهور الكاميرا يُسجَّل كلما عاد الشخص بعد غياب 5 دقائق، مع صورة الإطار وموضعه في تسجيل الكاميرا إن كانت تُسجَّل. "
                                           "انقر نقراً مزدوجاً على ظهور لفتح الملف أو التسجيل عند لحظته."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(10);
    m_scope->addItem(QStringLiteral("قائمة المراقبة فقط"), true);
    m_scope->addItem(QStringLiteral("كل الأشخاص"), false);
    m_period->addItem(QStringLiteral("اليوم"), 0);
    m_period->addItem(QStringLiteral("آخر 7 أيام"), 7);
    m_period->addItem(QStringLiteral("آخر 30 يوماً"), 30);
    m_period->addItem(QStringLiteral("كل الأوقات"), -1);
    m_period->setCurrentIndex(3);
    controls->addWidget(new QLabel(QStringLiteral("الأشخاص:"), this));
    controls->addWidget(m_scope);
    controls->addSpacing(8);
    controls->addWidget(new QLabel(QStringLiteral("الفترة:"), this));
    controls->addWidget(m_period);
    controls->addStretch(1);
    layout->addLayout(controls);

    m_summary->setObjectName(QStringLiteral("muted"));
    layout->addWidget(m_summary);

    m_list->setItemDelegate(new FaceCardDelegate(m_list));
    m_list->setUniformItemSizes(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setMouseTracking(true);
    layout->addWidget(m_list, 1);

    auto *buttons = new QHBoxLayout;
    m_open = new FancyButton(Icons::Name::Image, QStringLiteral("فتح الملف"), FancyButton::Kind::Primary, this);
    m_open->setToolTip(QStringLiteral("فتح الصورة أو الفيديو الذي ظهر فيه الشخص في صفحة التعرف"));
    m_snapshot = new FancyButton(Icons::Name::Camera, QStringLiteral("لقطة الإطار"), FancyButton::Kind::Secondary, this);
    m_snapshot->setToolTip(QStringLiteral("صورة الإطار كاملاً لحظة ظهور الشخص أمام الكاميرا"));
    m_clip = new FancyButton(Icons::Name::Film, QStringLiteral("تصدير المقطع…"), FancyButton::Kind::Secondary, this);
    m_clip->setToolTip(QStringLiteral("حفظ 30 ثانية من التسجيل حول لحظة الظهور في ملف واحد جاهز للتسليم"));
    m_person = new FancyButton(Icons::Name::Users, QStringLiteral("ملف الشخص"), FancyButton::Kind::Secondary, this);
    m_export = new FancyButton(Icons::Name::Export, QStringLiteral("تصدير CSV…"), FancyButton::Kind::Secondary, this);
    m_export->setToolTip(QStringLiteral("حفظ السجل المعروض في ملف يُفتح ببرنامج Excel"));
    auto *close = new FancyButton(Icons::Name::Reject, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, this);
    buttons->addWidget(m_open);
    buttons->addWidget(m_snapshot);
    buttons->addWidget(m_clip);
    buttons->addWidget(m_person);
    buttons->addWidget(m_export);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_scope, &QComboBox::currentIndexChanged, this, &AlertLogDialog::reload);
    connect(m_period, &QComboBox::currentIndexChanged, this, &AlertLogDialog::reload);
    connect(m_list, &QListWidget::currentRowChanged, this, &AlertLogDialog::updateButtons);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &AlertLogDialog::openCurrent);
    connect(m_open, &QAbstractButton::clicked, this, &AlertLogDialog::openCurrent);
    connect(m_snapshot, &QAbstractButton::clicked, this, [this] {
        if (const SightingEntry *entry = currentEntry())
            QDesktopServices::openUrl(QUrl::fromLocalFile(entry->sighting.snapshot));
    });
    connect(m_clip, &QAbstractButton::clicked, this, [this] {
        if (const SightingEntry *entry = currentEntry())
            ClipExport::exportAround(this, entry->sighting.source, entry->sighting.positionMs,
                                     QStringLiteral("%1 %2").arg(entry->name, entry->sighting.seen.toString(QStringLiteral("yyyy-MM-dd HH-mm"))));
    });
    connect(m_person, &QAbstractButton::clicked, this, [this] {
        if (const SightingEntry *entry = currentEntry()) {
            emit showPersonRequested(entry->personId);
            accept();
        }
    });
    connect(m_export, &QAbstractButton::clicked, this, &AlertLogDialog::exportCsv);
    connect(close, &QAbstractButton::clicked, this, &QDialog::reject);

    reload();
}

QDateTime AlertLogDialog::since() const
{
    const int days = m_period->currentData().toInt();
    if (days == 0)
        return QDateTime(QDate::currentDate(), QTime(0, 0));
    if (days > 0)
        return QDateTime::currentDateTime().addDays(-days);
    return {};
}

void AlertLogDialog::reload()
{
    const bool watchedOnly = m_scope->currentData().toBool();
    m_entries = m_database->recentSightings(watchedOnly, since(), MaxEntries);

    m_list->clear();
    QSet<QString> persons;
    for (int i = 0; i < m_entries.size(); ++i) {
        const SightingEntry &entry = m_entries.at(i);
        const Sighting &sighting = entry.sighting;
        persons.insert(entry.personId);
        auto *item = new QListWidgetItem(m_list);
        item->setData(EntryRole, i);
        item->setData(CardRole::Id, entry.personId);
        QString title = entry.name;
        if (FaceDatabase::isWatched(entry.tags))
            title = QStringLiteral("⚠ ") + title;
        item->setData(CardRole::Title, title);
        QString where = QStringLiteral("⁦%1⁩ · ⁨%2⁩")
                            .arg(sighting.seen.toString(QStringLiteral("yyyy/MM/dd hh:mm:ss")), sourceName(sighting));
        if (sighting.positionMs >= 0)
            where += QStringLiteral(" · عند ⁦%1⁩").arg(ImageIO::formatTime(sighting.positionMs));
        item->setData(CardRole::Subtitle, where);
        item->setData(CardRole::Status, int(MatchStatus::Confirmed));
        item->setData(CardRole::StatusText, fromCamera(sighting) ? QStringLiteral("كاميرا")
                                            : sighting.positionMs >= 0 ? QStringLiteral("فيديو")
                                                                       : QStringLiteral("صورة"));
        item->setData(CardRole::Score, sighting.score);
        item->setData(CardRole::Avatar, circularPixmap(sighting.thumbnail, 54));
        item->setToolTip(isCamera(sighting.source) ? sourceName(sighting) : QDir::toNativeSeparators(sighting.source));
    }

    if (m_entries.isEmpty()) {
        m_summary->setText(watchedOnly ? QStringLiteral("لم يظهر أي شخص من قائمة المراقبة في هذه الفترة. "
                                                        "أضف أشخاصاً إلى القائمة من زر «إضافة للمراقبة» في ملف الشخص.")
                                       : QStringLiteral("لا ظهور مسجل في هذه الفترة."));
    } else {
        QString text = QStringLiteral("مرات الظهور: %1 · الأشخاص: %2").arg(m_entries.size()).arg(persons.size());
        if (m_entries.size() >= MaxEntries)
            text += QStringLiteral(" (أحدث %1 فقط)").arg(MaxEntries);
        m_summary->setText(text);
        m_list->setCurrentRow(0);
    }
    updateButtons();
}

const SightingEntry *AlertLogDialog::currentEntry() const
{
    const QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return nullptr;
    const int index = item->data(EntryRole).toInt();
    return index >= 0 && index < m_entries.size() ? &m_entries.at(index) : nullptr;
}

void AlertLogDialog::updateButtons()
{
    const SightingEntry *entry = currentEntry();
    m_open->setEnabled(entry && !isCamera(entry->sighting.source));
    m_snapshot->setEnabled(entry && !entry->sighting.snapshot.isEmpty() && QFileInfo::exists(entry->sighting.snapshot));
    m_clip->setEnabled(entry && !entry->sighting.camera.isEmpty() && !isCamera(entry->sighting.source) && entry->sighting.positionMs >= 0);
    m_person->setEnabled(entry != nullptr);
    m_export->setEnabled(!m_entries.isEmpty());
}

void AlertLogDialog::openCurrent()
{
    const SightingEntry *entry = currentEntry();
    if (!entry || isCamera(entry->sighting.source))
        return;
    if (!QFileInfo::exists(entry->sighting.source) && !entry->sighting.camera.isEmpty()) {
        Toast::show(this, QStringLiteral("التسجيل غير موجود: ما زال يُسجَّل (يُفتح بعد إيقاف التسجيل أو انتهاء جزئه كل 15 دقيقة)، أو حُذف"),
                    Toast::Kind::Warning);
        return;
    }
    if (!QFileInfo::exists(entry->sighting.source)) {
        Toast::show(this, QStringLiteral("الملف غير موجود: %1").arg(QDir::toNativeSeparators(entry->sighting.source)),
                    Toast::Kind::Warning);
        return;
    }
    emit openFileRequested(entry->sighting.source, entry->sighting.positionMs);
    accept();
}

void AlertLogDialog::exportCsv()
{
    const QString initial = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                .filePath(QStringLiteral("سجل-التنبيهات-%1.csv").arg(QDate::currentDate().toString(Qt::ISODate)));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("تصدير السجل"), initial, QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    // القائمة تعرض أحدث MaxEntries فقط، والتصدير يشمل كل الفترة المختارة
    const QList<SightingEntry> entries = m_entries.size() >= MaxEntries
        ? m_database->recentSightings(m_scope->currentData().toBool(), since(), -1)
        : m_entries;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Toast::show(this, QStringLiteral("تعذر حفظ الملف"), Toast::Kind::Warning);
        return;
    }
    QString csv = QStringLiteral("﻿التاريخ,الوقت,الاسم,التصنيفات,التشابه %,المصدر,الموضع في الفيديو,الكاميرا,لقطة الإطار\n");
    for (const SightingEntry &entry : entries) {
        const Sighting &s = entry.sighting;
        const QStringList row{
            s.seen.toString(QStringLiteral("yyyy-MM-dd")),
            s.seen.toString(QStringLiteral("HH:mm:ss")),
            entry.name,
            entry.tags,
            QString::number(qRound(s.score * 100)),
            isCamera(s.source) ? sourceName(s) : QDir::toNativeSeparators(s.source),
            s.positionMs >= 0 ? ImageIO::formatTime(s.positionMs) : QString(),
            s.camera.isEmpty() ? QString() : Cameras::displayName(s.camera),
            QDir::toNativeSeparators(s.snapshot),
        };
        QStringList fields;
        for (const QString &field : row)
            fields << csvField(field);
        csv += fields.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    const QByteArray data = csv.toUtf8();
    const bool written = file.write(data) == data.size() && file.flush();
    file.close();
    if (!written) {
        Toast::show(this, QStringLiteral("تعذر حفظ الملف كاملاً؛ تحقق من المساحة الفارغة"), Toast::Kind::Warning);
        return;
    }
    Toast::show(this, QStringLiteral("حُفظ السجل: %1 مرة ظهور").arg(entries.size()), Toast::Kind::Success);
}

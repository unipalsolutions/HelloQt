#include "settingsdialog.h"

#include "alertsound.h"
#include "appsettings.h"
#include "camerarules.h"
#include "cameras.h"
#include "camerawatch.h"
#include "icons.h"
#include "maintenance.h"
#include "recorder.h"
#include "theme.h"
#include "widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimeEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QLabel *note(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

// صفحة قابلة للتمرير، فلا تخرج النافذة عن الشاشات الصغيرة
QWidget *scrolled(QWidget *content)
{
    auto *area = new QScrollArea;
    area->setWidget(content);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return area;
}

QComboBox *daysCombo(const QList<int> &options, int current, const QString &zero, QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    for (const int days : options)
        combo->addItem(days == 0 ? zero : QStringLiteral("الأقدم من %1 يوماً").arg(days), days);
    int index = combo->findData(current);
    if (index < 0) {
        combo->addItem(QStringLiteral("الأقدم من %1 يوماً").arg(current), current);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
    return combo;
}

QWidget *folderRow(QLineEdit *edit, const QString &title, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *browse = new FancyButton(Icons::Name::Folder, QStringLiteral("اختيار…"), FancyButton::Kind::Secondary, row);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    QObject::connect(browse, &QAbstractButton::clicked, row, [edit, title, row] {
        const QString chosen = QFileDialog::getExistingDirectory(row, title, edit->text());
        if (!chosen.isEmpty())
            edit->setText(QDir::toNativeSeparators(chosen));
    });
    return row;
}

QTimeEdit *timeEdit(const QTime &time, QWidget *parent)
{
    auto *edit = new QTimeEdit(time, parent);
    edit->setDisplayFormat(QStringLiteral("HH:mm"));
    edit->setLayoutDirection(Qt::LeftToRight);
    return edit;
}

} // namespace

SettingsDialog::SettingsDialog(FaceDatabase *database, QWidget *parent)
    : QDialog(parent)
    , m_database(database)
{
    setWindowTitle(QStringLiteral("الإعدادات"));
    resize(720, 620);
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->addTab(scrolled(generalPage()), QStringLiteral("عام"));
    tabs->addTab(scrolled(recordingPage()), QStringLiteral("التسجيل"));
    tabs->addTab(scrolled(alertsPage()), QStringLiteral("التنبيهات"));
    tabs->addTab(scrolled(maintenancePage()), QStringLiteral("الصيانة والنسخ الاحتياطي"));
    tabs->addTab(camerasPage(), QStringLiteral("الكاميرات"));
    layout->addWidget(tabs, 1);

    auto *buttons = new QHBoxLayout;
    auto *save = new FancyButton(Icons::Name::Check, QStringLiteral("حفظ"), FancyButton::Kind::Primary, this);
    auto *cancel = new FancyButton(Icons::Name::Reject, QStringLiteral("إلغاء"), FancyButton::Kind::Secondary, this);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    layout->addLayout(buttons);
    connect(save, &QAbstractButton::clicked, this, &QDialog::accept);
    connect(cancel, &QAbstractButton::clicked, this, &QDialog::reject);
}

QWidget *SettingsDialog::generalPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_tray = new QCheckBox(QStringLiteral("إبقاء الكاميرا تعمل عند إغلاق النافذة"), page);
    m_tray->setChecked(AppSettings::keepRunningInTray());
    m_startCamera = new QCheckBox(QStringLiteral("تشغيل آخر كاميرا عند فتح البرنامج"), page);
    m_startCamera->setChecked(AppSettings::startCameraOnLaunch());
    m_startup = new QCheckBox(QStringLiteral("فتح البرنامج مع ويندوز"), page);
    m_startup->setChecked(AppSettings::launchWithWindows());
    m_startup->setVisible(AppSettings::canLaunchWithWindows());
    m_visitors = new QCheckBox(QStringLiteral("حفظ الزوار المجهولين أمام الكاميرات"), page);
    m_visitors->setChecked(CameraWatch::visitorsEnabled());
    layout->addWidget(m_tray);
    layout->addWidget(note(QStringLiteral("تختفي النافذة إلى جانب الساعة وتستمر المراقبة والتسجيل والتنبيهات."), page));
    layout->addWidget(m_startCamera);
    layout->addWidget(m_startup);
    layout->addWidget(note(QStringLiteral("معاً: تعود المراقبة وحدها بعد إعادة تشغيل الجهاز."), page));
    layout->addWidget(m_visitors);
    layout->addWidget(note(QStringLiteral("الوجه نفسه يُجمع تحت رقم واحد لتسميته لاحقاً، ويُنسى من لم يظهر 90 يوماً."), page));
    layout->addStretch();
    return page;
}

QWidget *SettingsDialog::recordingPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    const CameraRecorder::Options options = CameraRecorder::savedOptions();
    m_recordFolder = new QLineEdit(QDir::toNativeSeparators(CameraRecorder::savedFolder()), page);
    m_autoRecord = new QCheckBox(QStringLiteral("يبدأ التسجيل وحده كلما شُغّلت كاميرا"), page);
    m_autoRecord->setChecked(CameraRecorder::autoRecord());
    m_motionOnly = new QCheckBox(QStringLiteral("التسجيل عند الحركة فقط (مع ثانيتين قبلها و10 ثوانٍ بعدها)"), page);
    m_motionOnly->setChecked(options.motionOnly);
    m_overlay = new QCheckBox(QStringLiteral("رسم مربعات الوجوه وأسمائها في التسجيل"), page);
    m_overlay->setChecked(options.overlay);
    m_recordRate = new QComboBox(page);
    m_recordRate->addItem(QStringLiteral("تلقائية حسب قدرة الجهاز (مستحسن)"), false);
    m_recordRate->addItem(QStringLiteral("سرعة الكاميرا كاملة دائماً"), true);
    m_recordRate->setCurrentIndex(options.fullRate ? 1 : 0);
    m_retention = daysCombo({0, 3, 7, 14, 30, 90}, CameraRecorder::retentionDays(), QStringLiteral("لا تُحذف"), page);
    form->addRow(QStringLiteral("مجلد التسجيلات"), folderRow(m_recordFolder, QStringLiteral("مجلد تسجيلات الكاميرا"), page));
    form->addRow(QString(), m_autoRecord);
    form->addRow(QString(), m_motionOnly);
    form->addRow(QString(), m_overlay);
    form->addRow(QStringLiteral("سرعة التسجيل"), m_recordRate);
    form->addRow(QString(), note(QStringLiteral("التلقائية تسجّل بسرعة الكاميرا كاملة ما دام الجهاز يلحق بها، وإن لم يلحق تحفظ صورة من كل "
                                               "صورتين أو ثلاث فيبقى التسجيل منتظماً بلا تجمد. «كاملة دائماً» قد يتقطع بها التسجيل على "
                                               "الأجهزة الضعيفة. العرض والتعرف على الوجوه لا يتأثران بهذا."),
                                          page));
    form->addRow(QStringLiteral("حذف التسجيلات ولقطات الظهور"), m_retention);
    form->addRow(QString(), note(QStringLiteral("لا يُحذف إلا ما سجّله البرنامج. ومع الحذف التلقائي تُحذف الأقدم أيضاً حين يقترب القرص "
                                               "من الامتلاء بدل أن يتوقف التسجيل. ساعات التسجيل لكل كاميرا في صفحة «الكاميرات»."),
                                          page));
    return page;
}

QWidget *SettingsDialog::alertsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_sound = new QCheckBox(QStringLiteral("أصوات التنبيه"), page);
    m_sound->setChecked(AlertSound::enabled());
    auto *testRow = new QHBoxLayout;
    auto *chime = new FancyButton(Icons::Name::Play, QStringLiteral("تجربة التنبيه العادي"), FancyButton::Kind::Secondary, page);
    auto *alarm = new FancyButton(Icons::Name::Play, QStringLiteral("تجربة الإنذار"), FancyButton::Kind::Secondary, page);
    auto *visitor = new FancyButton(Icons::Name::Play, QStringLiteral("تجربة تنبيه المجهول"), FancyButton::Kind::Secondary, page);
    testRow->addWidget(chime);
    testRow->addWidget(alarm);
    testRow->addWidget(visitor);
    testRow->addStretch();
    // التجربة تعمل ولو كانت الأصوات مطفأة الآن في النافذة
    const auto test = [this](AlertSound::Sound sound) {
        const bool before = AlertSound::enabled();
        AlertSound::setEnabled(true);
        AlertSound::play(sound);
        AlertSound::setEnabled(before);
    };
    connect(chime, &QAbstractButton::clicked, this, [test] { test(AlertSound::Sound::Chime); });
    connect(alarm, &QAbstractButton::clicked, this, [test] { test(AlertSound::Sound::Alarm); });
    connect(visitor, &QAbstractButton::clicked, this, [test] { test(AlertSound::Sound::Visitor); });
    layout->addWidget(m_sound);
    layout->addLayout(testRow);
    layout->addWidget(note(QStringLiteral("مستوى التنبيه لكل شخص مراقَب (عادي، إنذار عالٍ، صامت، إشعار فقط) يُختار من ملفه "
                                          "بزر «التنبيه» بجانب «إلغاء المراقبة»."),
                           page));

    auto *title = new QLabel(QStringLiteral("تنبيه المجهول المريب"), page);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addSpacing(8);
    layout->addWidget(title);
    const CameraWatch::StrangerAlerts strangers = CameraWatch::strangerAlerts();
    auto *countRow = new QHBoxLayout;
    m_strangerCount = new QSpinBox(page);
    m_strangerCount->setRange(0, 100);
    m_strangerCount->setValue(strangers.dailyCount);
    m_strangerCount->setSpecialValueText(QStringLiteral("لا تنبيه"));
    countRow->addWidget(new QLabel(QStringLiteral("مجهول ظهر في اليوم نفسه"), page));
    countRow->addWidget(m_strangerCount);
    countRow->addWidget(new QLabel(QStringLiteral("مرات أو أكثر"), page));
    countRow->addStretch();
    layout->addLayout(countRow);
    auto *nightRow = new QHBoxLayout;
    m_strangerNight = new QCheckBox(QStringLiteral("مجهول ظهر ليلاً، من"), page);
    m_strangerNight->setChecked(strangers.night);
    m_nightFrom = timeEdit(strangers.nightWindow.from, page);
    m_nightTo = timeEdit(strangers.nightWindow.to, page);
    nightRow->addWidget(m_strangerNight);
    nightRow->addWidget(m_nightFrom);
    nightRow->addWidget(new QLabel(QStringLiteral("إلى"), page));
    nightRow->addWidget(m_nightTo);
    nightRow->addStretch();
    layout->addLayout(nightRow);
    layout->addWidget(note(QStringLiteral("يعمل مع «حفظ الزوار المجهولين». تنبيه واحد لكل زائر في اليوم لكل سبب، ويصل إشعاراً من ويندوز "
                                          "ويُكتب في سجل الأحداث. ساعات التنبيه لكل كاميرا في صفحة «الكاميرات»."),
                           page));
    layout->addStretch();
    return page;
}

QWidget *SettingsDialog::maintenancePage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    m_sightingRetention = daysCombo({0, 30, 90, 180, 365}, Maintenance::sightingRetentionDays(), QStringLiteral("يبقى دائماً"), page);
    form->addRow(QStringLiteral("حذف ظهور الكاميرات من السجل"), m_sightingRetention);
    form->addRow(QString(), note(QStringLiteral("سجل ظهور المعروفين والزوار أمام الكاميرات. ظهور الصور والفيديو يبقى دائماً."), page));

    const Maintenance::BackupSettings backup = Maintenance::backupSettings();
    m_backupInterval = new QComboBox(page);
    m_backupInterval->addItem(QStringLiteral("متوقفة"), 0);
    m_backupInterval->addItem(QStringLiteral("يومياً"), 1);
    m_backupInterval->addItem(QStringLiteral("أسبوعياً"), 7);
    m_backupInterval->setCurrentIndex(std::max(0, m_backupInterval->findData(backup.intervalDays)));
    m_backupFolder = new QLineEdit(QDir::toNativeSeparators(backup.folder), page);
    m_backupKeep = new QSpinBox(page);
    m_backupKeep->setRange(1, 365);
    m_backupKeep->setValue(backup.keep);
    m_backupKeep->setSuffix(QStringLiteral(" نسخة"));
    m_backupState = new QLabel(page);
    m_backupState->setObjectName(QStringLiteral("muted"));
    m_backupState->setWordWrap(true);
    auto *now = new FancyButton(Icons::Name::Save, QStringLiteral("نسخة الآن"), FancyButton::Kind::Secondary, page);
    connect(now, &QAbstractButton::clicked, this, [this] {
        Maintenance::BackupSettings settings = Maintenance::backupSettings();
        settings.folder = QDir::fromNativeSeparators(m_backupFolder->text().trimmed());
        settings.keep = m_backupKeep->value();
        settings.intervalDays = m_backupInterval->currentData().toInt();
        Maintenance::setBackupSettings(settings);
        emit backupRequested();
    });
    form->addRow(QStringLiteral("نسخة احتياطية تلقائية"), m_backupInterval);
    form->addRow(QStringLiteral("مجلد النسخ"), folderRow(m_backupFolder, QStringLiteral("مجلد النسخ الاحتياطية"), page));
    form->addRow(QStringLiteral("الاحتفاظ بآخر"), m_backupKeep);
    form->addRow(QString(), now);
    form->addRow(QString(), m_backupState);
    form->addRow(QString(), note(QStringLiteral("يُفضَّل مجلد على قرص آخر أو ذاكرة خارجية أو مجلد مزامنة سحابية، فلا يضيع كل شيء مع تلف "
                                               "قرص الجهاز. النسخة ملف واحد يُستعاد من «نسخ احتياطي ← استعادة»."),
                                          page));
    updateBackupState();
    return page;
}

void SettingsDialog::updateBackupState()
{
    const QDateTime last = Maintenance::lastBackup();
    const QStringList existing = Maintenance::autoBackups(QDir::fromNativeSeparators(m_backupFolder->text().trimmed()));
    m_backupState->setText(QStringLiteral("آخر نسخة تلقائية: %1 · في المجلد %2 نسخة")
                               .arg(last.isValid() ? last.toString(QStringLiteral("yyyy/MM/dd HH:mm")) : QStringLiteral("لا يوجد"))
                               .arg(existing.size()));
}

QWidget *SettingsDialog::camerasPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(note(QStringLiteral("لكل كاميرا: مناطق التحليل داخل الكادر، وساعات التنبيه، وساعات التسجيل. "
                                          "اختر كاميرا ثم «تعديل»."),
                           page));
    auto *list = new QListWidget(page);
    layout->addWidget(list, 1);
    const auto fill = [list] {
        list->clear();
        QList<QPair<QString, QString>> cameras;
        const QList<Cameras::Camera> saved = Cameras::load();
        for (const Cameras::Camera &camera : saved)
            cameras.append({camera.source(), camera.name});
        const QList<Cameras::LocalDevice> devices = Cameras::localDevices();
        for (const Cameras::LocalDevice &device : devices) {
            const bool known = std::any_of(saved.begin(), saved.end(), [&](const Cameras::Camera &c) {
                return c.kind == Cameras::Kind::Local && c.index == device.index;
            });
            if (!known)
                cameras.append({QStringLiteral("camera:%1").arg(device.index), Cameras::localName(device.index, devices)});
        }
        if (cameras.isEmpty())
            cameras.append({QStringLiteral("camera:0"), QStringLiteral("كاميرا الجهاز")});
        for (const auto &[source, name] : cameras) {
            auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(name, CameraRules::load(source).summary()), list);
            item->setData(Qt::UserRole, source);
            item->setData(Qt::UserRole + 1, name);
        }
    };
    fill();
    auto *edit = new FancyButton(Icons::Name::Region, QStringLiteral("تعديل…"), FancyButton::Kind::Primary, page);
    auto *row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(edit);
    layout->addLayout(row);
    const auto open = [this, list, fill] {
        const QListWidgetItem *item = list->currentItem();
        if (!item)
            return;
        emit editRulesRequested(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString());
        const int row = list->currentRow();
        fill();
        list->setCurrentRow(row);
    };
    connect(edit, &QAbstractButton::clicked, this, open);
    connect(list, &QListWidget::itemDoubleClicked, this, open);
    if (list->count() > 0)
        list->setCurrentRow(0);
    return page;
}

void SettingsDialog::accept()
{
    AppSettings::setKeepRunningInTray(m_tray->isChecked());
    AppSettings::setStartCameraOnLaunch(m_startCamera->isChecked());
    if (AppSettings::canLaunchWithWindows() && m_startup->isChecked() != AppSettings::launchWithWindows())
        AppSettings::setLaunchWithWindows(m_startup->isChecked());
    CameraWatch::setVisitorsEnabled(m_visitors->isChecked());

    const QString folder = QDir::fromNativeSeparators(m_recordFolder->text().trimmed());
    if (!folder.isEmpty())
        CameraRecorder::setSavedFolder(folder);
    CameraRecorder::setAutoRecord(m_autoRecord->isChecked());
    CameraRecorder::Options options = CameraRecorder::savedOptions();
    options.motionOnly = m_motionOnly->isChecked();
    options.overlay = m_overlay->isChecked();
    options.fullRate = m_recordRate->currentData().toBool();
    CameraRecorder::setSavedOptions(options);
    CameraRecorder::setRetentionDays(m_retention->currentData().toInt());

    AlertSound::setEnabled(m_sound->isChecked());
    CameraWatch::StrangerAlerts strangers;
    strangers.dailyCount = m_strangerCount->value();
    strangers.night = m_strangerNight->isChecked();
    strangers.nightWindow = {true, m_nightFrom->time(), m_nightTo->time()};
    CameraWatch::setStrangerAlerts(strangers);

    Maintenance::setSightingRetentionDays(m_sightingRetention->currentData().toInt());
    Maintenance::BackupSettings backup;
    backup.intervalDays = m_backupInterval->currentData().toInt();
    backup.folder = QDir::fromNativeSeparators(m_backupFolder->text().trimmed());
    if (backup.folder.isEmpty())
        backup.folder = Maintenance::defaultBackupFolder();
    backup.keep = m_backupKeep->value();
    Maintenance::setBackupSettings(backup);
    QDialog::accept();
}

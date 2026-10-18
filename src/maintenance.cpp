#include "maintenance.h"

#include "applog.h"
#include "facedatabase.h"
#include "recorder.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

constexpr int RunIntervalMs = 60 * 60 * 1000;
const QString BackupPrefix = QStringLiteral("ملامح-تلقائي-");
const QString BackupSuffix = QStringLiteral(".malamih");

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

} // namespace

Maintenance::Maintenance(FaceDatabase *database, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(RunIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &Maintenance::run);
}

int Maintenance::sightingRetentionDays()
{
    return std::max(0, QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("sightingRetentionDays"), 0).toInt());
}

void Maintenance::setSightingRetentionDays(int days)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("sightingRetentionDays"), std::max(0, days));
}

QString Maintenance::defaultBackupFolder()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("ملامح - نسخ احتياطية"));
}

Maintenance::BackupSettings Maintenance::backupSettings()
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    BackupSettings result;
    result.intervalDays = std::max(0, settings.value(QStringLiteral("autoBackupDays"), 0).toInt());
    result.folder = settings.value(QStringLiteral("autoBackupFolder")).toString();
    if (result.folder.isEmpty())
        result.folder = defaultBackupFolder();
    result.keep = std::clamp(settings.value(QStringLiteral("autoBackupKeep"), 7).toInt(), 1, 365);
    return result;
}

void Maintenance::setBackupSettings(const BackupSettings &value)
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("autoBackupDays"), std::max(0, value.intervalDays));
    settings.setValue(QStringLiteral("autoBackupFolder"), value.folder);
    settings.setValue(QStringLiteral("autoBackupKeep"), std::clamp(value.keep, 1, 365));
}

QDateTime Maintenance::lastBackup()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("autoBackupLast")).toDateTime();
}

QStringList Maintenance::autoBackups(const QString &folder)
{
    QDir dir(folder);
    // الاسم يحمل التاريخ والوقت، فترتيبه الأبجدي ترتيب زمني
    QStringList names = dir.entryList({BackupPrefix + QStringLiteral("*") + BackupSuffix}, QDir::Files, QDir::Name | QDir::Reversed);
    QStringList paths;
    for (const QString &name : std::as_const(names))
        paths << dir.filePath(name);
    return paths;
}

bool Maintenance::backupNow(const QString &databaseFile, const QString &folder, int keep, QString *path, QString *error)
{
    if (!QDir().mkpath(folder)) {
        if (error)
            *error = QStringLiteral("تعذر إنشاء المجلد %1").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    const QString target = QDir(folder).filePath(
        BackupPrefix + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")) + BackupSuffix);
    // تُكتب باسم مؤقت ثم تُسمّى، فلا تبقى نسخة ناقصة باسم صالح إن انقطع الجهاز أثناءها
    const QString partial = target + QStringLiteral(".part");
    if (!FaceDatabase::backupFile(databaseFile, partial, error))
        return false;
    QFile::remove(target);
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        if (error)
            *error = QStringLiteral("تعذر حفظ الملف في %1").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    const QStringList existing = autoBackups(folder);
    for (int i = std::max(1, keep); i < existing.size(); ++i)
        QFile::remove(existing.at(i));
    if (path)
        *path = target;
    return true;
}

void Maintenance::start()
{
    // تسجيل انقطع بانطفاء مفاجئ يُستعاد فوراً، فيظهر في قائمة التسجيلات من أول تشغيل
    auto *recovery = new QFutureWatcher<int>(this);
    connect(recovery, &QFutureWatcher<int>::finished, this, [recovery] {
        const int recovered = recovery->result();
        recovery->deleteLater();
        if (recovered > 0)
            AppLog::write(QStringLiteral("استُعيد %1 من تسجيلات الكاميرا التي انقطعت كتابتها بإغلاق مفاجئ").arg(recovered));
    });
    recovery->setFuture(QtConcurrent::run([] { return CameraRecorder::recoverInterrupted(); }));
    QTimer::singleShot(60 * 1000, this, &Maintenance::run);
    m_timer->start();
}

void Maintenance::runNow()
{
    run();
}

void Maintenance::run()
{
    // التسجيلات واللقطات: حذف الملفات قد يطول، فيجري في الخلفية
    auto *files = new QFutureWatcher<int>(this);
    connect(files, &QFutureWatcher<int>::finished, this, [this, files] {
        const int recordings = files->result();
        files->deleteLater();
        const int days = sightingRetentionDays();
        removeSightings(recordings, 0, days > 0 ? QDateTime::currentDateTime().addDays(-days) : QDateTime());
    });
    files->setFuture(QtConcurrent::run([] { return CameraRecorder::removeExpired(); }));
    startBackup(false);
}

void Maintenance::removeSightings(int recordings, int sightings, const QDateTime &before)
{
    // دفعات صغيرة بينها فاصل، فتبقى الواجهة والكاميرا تعملان أثناء حذف سجل طويل
    bool more = false;
    if (before.isValid() && m_database)
        sightings += m_database->removeCameraSightingsBefore(before, 2000, &more);
    if (more) {
        QTimer::singleShot(30, this, [this, recordings, sightings, before] { removeSightings(recordings, sightings, before); });
        return;
    }
    if (recordings > 0 || sightings > 0) {
        AppLog::write(QStringLiteral("الصيانة: حُذف %1 ملف تسجيل ولقطة قديم و%2 ظهوراً قديماً من السجل").arg(recordings).arg(sightings));
        emit cleaned(recordings, sightings);
    }
}

void Maintenance::backupNow()
{
    startBackup(true);
}

void Maintenance::startBackup(bool force)
{
    const BackupSettings backup = backupSettings();
    if ((!force && backup.intervalDays <= 0) || m_backupRunning || !m_database || m_database->directory().isEmpty())
        return;
    const QDateTime last = lastBackup();
    // هامش ساعة، فلا تتأخر النسخة اليومية يوماً كاملاً بسبب دقائق
    if (!force && last.isValid() && last.secsTo(QDateTime::currentDateTime()) < qint64(backup.intervalDays) * 86400 - 3600)
        return;
    m_backupRunning = true;
    const QString databaseFile = m_database->databaseFile();
    struct Outcome
    {
        bool ok = false;
        QString path;
        QString error;
    };
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        m_backupRunning = false;
        if (outcome.ok) {
            QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("autoBackupLast"), QDateTime::currentDateTime());
            AppLog::write(QStringLiteral("نسخة احتياطية تلقائية: %1").arg(QDir::toNativeSeparators(outcome.path)));
        } else {
            AppLog::write(QStringLiteral("فشلت النسخة الاحتياطية التلقائية: %1").arg(outcome.error));
        }
        emit backupFinished(outcome.ok, outcome.path, outcome.error);
    });
    watcher->setFuture(QtConcurrent::run([databaseFile, backup] {
        Outcome outcome;
        outcome.ok = backupNow(databaseFile, backup.folder, backup.keep, &outcome.path, &outcome.error);
        return outcome;
    }));
}

#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

class FaceDatabase;
class QTimer;

// صيانة جهاز يعمل أشهراً دون تدخل، تجري عند الفتح ثم كل ساعة:
//   - حذف التسجيلات ولقطات الظهور الأقدم من مدة الاحتفاظ بالتسجيلات
//   - حذف ظهور الكاميرات من القاعدة الأقدم من مدته (اختياري)
//   - نسخة احتياطية تلقائية للقاعدة إلى مجلد يختاره المستخدم مع الاحتفاظ بآخر عدد منها
class Maintenance : public QObject
{
    Q_OBJECT

public:
    explicit Maintenance(FaceDatabase *database, QObject *parent = nullptr);

    // ظهور الكاميرات الأقدم من هذا العدد من الأيام يُحذف (0 = يبقى)
    static int sightingRetentionDays();
    static void setSightingRetentionDays(int days);

    struct BackupSettings
    {
        int intervalDays = 0; // 0 متوقف، 1 يومي، 7 أسبوعي
        QString folder;
        int keep = 7;         // عدد النسخ التلقائية التي تبقى
    };
    static BackupSettings backupSettings();
    static void setBackupSettings(const BackupSettings &settings);
    static QString defaultBackupFolder();
    static QDateTime lastBackup();

    // ينشئ نسخة في folder ثم يحذف الأقدم من النسخ التلقائية حتى يبقى keep؛ آمن من أي خيط
    static bool backupNow(const QString &databaseFile, const QString &folder, int keep, QString *path, QString *error);
    // النسخ التلقائية في المجلد، الأحدث أولاً
    static QStringList autoBackups(const QString &folder);

    // يبدأ الجدولة (أول تشغيل بعد دقيقة من الفتح)
    void start();
    // صيانة فورية (من الإعدادات بعد تغيير المدة)
    void runNow();
    // نسخة احتياطية الآن ولو لم يحن موعدها (إلى مجلد الإعدادات)
    void backupNow();
    bool isBackupRunning() const { return m_backupRunning; }

signals:
    void backupFinished(bool ok, const QString &path, const QString &error);
    void cleaned(int recordings, int sightings);

private:
    void run();
    void startBackup(bool force);

    FaceDatabase *m_database;
    QTimer *m_timer;
    bool m_backupRunning = false;
};

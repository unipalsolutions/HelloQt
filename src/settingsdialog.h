#pragma once

#include <QDialog>

class FaceDatabase;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTimeEdit;
class ToggleSwitch;

// كل إعدادات المراقبة والتشغيل الطويل في نافذة واحدة: العمل في الخلفية، والتسجيل، والتنبيهات،
// والصيانة والنسخ الاحتياطي، وقواعد الكاميرات. الإعدادات الخاصة بالتحليل نفسه تبقى في لوحته
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(FaceDatabase *database, QWidget *parent = nullptr);

    void accept() override;

signals:
    void editRulesRequested(const QString &source, const QString &name);
    // نسخة احتياطية الآن (تجري في الخلفية لدى النافذة الرئيسية)
    void backupRequested();

private:
    QWidget *generalPage();
    QWidget *recordingPage();
    QWidget *alertsPage();
    QWidget *maintenancePage();
    QWidget *camerasPage();
    void updateBackupState();

    FaceDatabase *m_database;
    // عام
    QCheckBox *m_tray;
    QCheckBox *m_startCamera;
    QCheckBox *m_startup;
    QCheckBox *m_visitors;
    // التسجيل
    QLineEdit *m_recordFolder;
    QCheckBox *m_autoRecord;
    QCheckBox *m_motionOnly;
    QCheckBox *m_overlay;
    QComboBox *m_retention;
    QComboBox *m_recordRate;
    // التنبيهات
    QCheckBox *m_sound;
    QSpinBox *m_strangerCount;
    QCheckBox *m_strangerNight;
    QTimeEdit *m_nightFrom;
    QTimeEdit *m_nightTo;
    // الصيانة
    QComboBox *m_sightingRetention;
    QComboBox *m_backupInterval;
    QLineEdit *m_backupFolder;
    QSpinBox *m_backupKeep;
    QLabel *m_backupState;
};

#pragma once

#include "cameras.h"

#include <QDialog>
#include <QImage>

class FancyButton;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QWidget;

// إدارة الكاميرات: إضافة كاميرا موصولة بالجهاز أو كاميرا شبكة وضبطها واختبارها قبل التشغيل
class CameraDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CameraDialog(QWidget *parent = nullptr, const QString &selectId = QString());

    // مصدر الكاميرا التي اختار المستخدم تشغيلها، أو فارغ
    QString startSource() const { return m_startSource; }

    void done(int result) override;

private:
    void rebuildList();
    void select(int row);
    // يسأل كيف الكاميرا موصولة؛ مفتاح Connection أو فارغ إن ألغى
    QString chooseConnection();
    void addCamera(const QString &via);
    void removeCamera();
    void refreshDevices();
    void loadForm();
    void storeForm();
    void updateUrl();
    void updateHelp();
    void testConnection();
    void probeCameras();
    void findCameras();
    void detectCamera();
    void setBusy(bool busy);
    void showPreview(const QImage &image, const QString &text, bool ok);
    Cameras::Camera *current();

    QList<Cameras::Camera> m_cameras;
    QList<Cameras::LocalDevice> m_devices;
    int m_current = -1;
    bool m_loading = false;
    int m_testToken = 0;
    QString m_startSource;

    QListWidget *m_list = nullptr;
    FancyButton *m_removeButton = nullptr;
    QWidget *m_form = nullptr;
    QLineEdit *m_name = nullptr;

    QWidget *m_localBox = nullptr;
    QComboBox *m_device = nullptr;
    QComboBox *m_resolution = nullptr;
    QComboBox *m_backend = nullptr;

    QWidget *m_networkBox = nullptr;
    QComboBox *m_brand = nullptr;
    QLabel *m_brandHint = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QWidget *m_channelRow = nullptr;
    QSpinBox *m_channel = nullptr;
    QWidget *m_streamRow = nullptr;
    QComboBox *m_stream = nullptr;
    QWidget *m_pathRow = nullptr;
    QLabel *m_pathLabel = nullptr;
    QLineEdit *m_path = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QLabel *m_url = nullptr;
    QLabel *m_hostCheck = nullptr;
    FancyButton *m_findButton = nullptr;
    FancyButton *m_detectButton = nullptr;
    FancyButton *m_netSettingsButton = nullptr;

    QLabel *m_preview = nullptr;
    QLabel *m_status = nullptr;
    FancyButton *m_testButton = nullptr;
    FancyButton *m_startButton = nullptr;
    QLabel *m_help = nullptr;
};

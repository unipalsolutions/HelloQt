#pragma once

#include "archivescan.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QList>

class FaceDatabase;
class FaceEngine;
class FancyButton;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QTimer;
class QTreeWidget;

// مسح أرشيف التسجيلات: قائمة ملفات أو مجلدات كاملة تُحلل بالفحص السريع دون تشغيلها على الشاشة،
// مع معاينة كل ثانية وتقدم دقيق ووقت متبقٍ، ويتابع من حيث توقف بعد إغلاق البرنامج،
// ثم تقرير موحّد بمن ظهر وفي أي ملف ومتى
class ArchiveScanDialog : public QDialog
{
    Q_OBJECT

public:
    ArchiveScanDialog(const FaceEngine *engine, FaceDatabase *database, float threshold, const QString &directory,
                      QWidget *parent = nullptr);
    ~ArchiveScanDialog() override;

    QString lastDirectory() const { return m_directory; }
    void addPaths(const QStringList &paths); // ملفات أو مجلدات

signals:
    void openRequested(const QString &path);

protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;

private:
    void addFiles();
    void addFolder();
    void removeSelected();
    void clearAll();
    void toggleRun();
    int parallel() const;
    // يوقف المسح وينتظر حفظ موضع كل خيط قبل الإغلاق
    void stopAndWait();
    void refreshFiles();
    void refreshStatus();
    void showPreview(int slot, const QImage &image, const QString &path, qint64 positionMs);
    void schedulePeople();
    void computePeople();
    void showPeople();
    void reloadGallery();
    void saveReport(bool csv);

    const FaceEngine *m_engine;
    FaceDatabase *m_database;
    float m_threshold;
    QString m_directory;
    ArchiveScan *m_scan;
    QList<Archive::Person> m_people;
    QFutureWatcher<QList<Archive::Person>> m_peopleWatcher;
    bool m_peopleDirty = false;
    bool m_closing = false;

    QTreeWidget *m_files;
    QComboBox *m_parallel;
    QWidget *m_previews;
    QList<QLabel *> m_previewLabels;
    QProgressBar *m_bar;
    QLabel *m_status;
    QLabel *m_estimate;
    QLabel *m_peopleTitle;
    QListWidget *m_grid;
    FancyButton *m_addFiles;
    FancyButton *m_addFolder;
    FancyButton *m_remove;
    FancyButton *m_clear;
    FancyButton *m_run;
    FancyButton *m_html;
    FancyButton *m_csv;
    QTimer *m_tick;
    QTimer *m_peopleTimer;
};

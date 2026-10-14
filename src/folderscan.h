#pragma once

#include "facetypes.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QMutex>

#include <atomic>
#include <memory>

class FaceDatabase;
class FaceEngine;
class FancyButton;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QTimer;
class ToggleSwitch;

// وجه وُجد في إحدى صور المجلد
struct FolderHit
{
    QString path;
    QImage crop;
    MatchResult match;
    float quality = 0.0f;
};

struct FolderScanState
{
    std::atomic<int> done{0};
    std::atomic<int> total{0};
    std::atomic<int> faces{0};
    std::atomic<bool> cancel{false};
    QMutex mutex;
    QList<FolderHit> pending; // نتائج لم تُعرض بعد
};

// البحث في مجلد صور كامل: يكشف الوجوه في كل الصور ويتعرف عليها من القاعدة، ثم يعرضها
// مجمّعة حسب الشخص، فيمكن إيجاد كل صور شخص معين بين آلاف الصور ونسخها أو تسجيلها في سجل ظهوره
class FolderScanDialog : public QDialog
{
    Q_OBJECT

public:
    FolderScanDialog(const FaceEngine *engine, FaceDatabase *database, float threshold, const QString &directory,
                     QWidget *parent = nullptr);
    ~FolderScanDialog() override;

    void start(const QString &folder);
    void chooseFolder();
    QString lastDirectory() const { return m_directory; }

signals:
    void analyzeRequested(const QString &path);

protected:
    void reject() override;

private:
    void stop();
    void drain();
    void onFinished();
    void addHit(const FolderHit &hit);
    void rebuildFilter();
    void applyFilter();
    bool hitVisible(const FolderHit &hit) const;
    QStringList visiblePaths() const;
    void copyVisible();
    void recordSightings();
    void showMenu(const QPoint &pos);
    void updateStatus();

    const FaceEngine *m_engine;
    FaceDatabase *m_database;
    float m_threshold;
    QString m_directory;
    QString m_folder;
    std::shared_ptr<FolderScanState> m_state;
    QFutureWatcher<void> m_watcher;
    QList<FolderHit> m_hits;
    QHash<QString, int> m_personCounts; // رقم الشخص ← عدد صوره في المجلد
    QHash<QString, QString> m_personNames;

    QLabel *m_folderLabel;
    QLabel *m_status;
    QProgressBar *m_bar;
    QComboBox *m_filter;
    QListWidget *m_grid;
    ToggleSwitch *m_recursive;
    ToggleSwitch *m_thorough;
    FancyButton *m_choose;
    FancyButton *m_stop;
    FancyButton *m_copy;
    FancyButton *m_record;
    QTimer *m_timer;
};

#pragma once

#include "facetypes.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QList>
#include <QMutex>
#include <QStringList>

#include <atomic>
#include <memory>
#include <vector>

class FaceDatabase;
class FaceEngine;
class FancyButton;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class ToggleSwitch;

// شخص سيُستورد: اسمه من اسم المجلد الفرعي أو اسم الصورة
struct ImportPerson
{
    QString name;
    QString nationalId;
    QStringList files;
    QString existingId; // شخص مسجل بالاسم أو رقم الهوية نفسه
    QString personId;   // يُعرف بعد إضافة أول صورة
    bool include = true;
    int added = 0;
    int noFace = 0;
    int mismatched = 0;
    QString duplicateOf; // اسم شخص آخر مسجل يشبهه بشدة
};

struct ImportResult
{
    enum class Status { Added, NoFace, Mismatch };
    int person = -1;
    QString file;
    Status status = Status::NoFace;
    int faces = 0;
    FaceItem face;
    std::vector<std::vector<float>> embeddings;
};

struct ImportState
{
    std::atomic<int> done{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> paused{false};
    QMutex mutex;
    QList<ImportResult> pending;
};

// استيراد أشخاص كثيرين دفعة واحدة من مجلد: كل مجلد فرعي شخص باسمه وفيه صور وجهه،
// أو صور مسمّاة بأسماء أصحابها («أحمد علي 2.jpg»)، ويمكن أن يحمل الاسم رقم الهوية («أحمد علي - 123456789»)
class ImportDialog : public QDialog
{
    Q_OBJECT

public:
    ImportDialog(const FaceEngine *engine, FaceDatabase *database, float threshold, const QString &directory,
                 QWidget *parent = nullptr);
    ~ImportDialog() override;

    void chooseFolder();
    QString lastDirectory() const { return m_directory; }
    // اسم الشخص ورقم هويته من اسم مجلد أو صورة
    static void parseName(const QString &text, bool fromFile, QString *name, QString *nationalId);

signals:
    void showPersonRequested(const QString &personId);

protected:
    void reject() override;

private:
    void load(const QString &folder);
    void start();
    void drain();
    void onFinished();
    void updateItem(int index);
    void updateSummary();
    void setRunning(bool running);
    int totalFiles() const;

    const FaceEngine *m_engine;
    FaceDatabase *m_database;
    float m_threshold;
    QString m_directory;
    QList<ImportPerson> m_persons;
    QList<QTreeWidgetItem *> m_items;
    std::shared_ptr<ImportState> m_state;
    QFutureWatcher<void> m_watcher;
    bool m_finished = false;
    int m_sinceCommit = 0;

    QLabel *m_folderLabel;
    QLabel *m_summary;
    QProgressBar *m_bar;
    QTreeWidget *m_tree;
    QLineEdit *m_tags;
    ToggleSwitch *m_mergeExisting;
    FancyButton *m_choose;
    FancyButton *m_start;
    FancyButton *m_pause;
    FancyButton *m_close;
    QTimer *m_timer;
};

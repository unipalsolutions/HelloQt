#pragma once

#include "facedatabase.h"
#include "facetypes.h"
#include "report.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>

#include <vector>

class FaceEngine;
class QThread;
class VideoWorker;

// مسح الأرشيف الطويل: قائمة ملفات فيديو تُقسَّم إلى أجزاء وتُحلل بالفحص السريع في عدة خيوط معاً،
// وتُحفظ حالتها بعد كل جزء فيتابع المسح من حيث توقف ولو أُغلق البرنامج، ثم يُجمع من ظهر وفي أي ملف ومتى
namespace Archive {

struct File
{
    QString path;
    qint64 durationMs = 0;
    QString error;
};

// جزء من ملف يحلله خيط واحد؛ الوجوه بعد انتهائه (أو حتى موضع توقفه إن أُوقف)
struct Chunk
{
    int file = 0;
    qint64 startMs = 0;
    qint64 endMs = 0;
    bool done = false;
    QList<FaceItem> faces;
};

// ظهور شخص في ملف واحد
struct Sighting
{
    int file = 0;
    QList<QPair<qint64, qint64>> appearances;
    qint64 firstMs = -1;
    qint64 lastMs = -1;
    qint64 screenMs = 0;
};

// شخص واحد عبر كل الملفات: معروف من القاعدة، أو مجهول جُمعت ظهوراته بتشابه البصمة
struct Person
{
    MatchResult match;
    FaceItem face; // أوضح لقطة
    QList<Sighting> sightings;
    qint64 screenMs = 0;
    qint64 bestFileMs = -1; // موضع اللقطة في ملفها
    int bestFile = -1;
};

// يجمع وجوه الأجزاء في أشخاص؛ يُعاد التعرف عليها من القاعدة الحالية إن أُعطيت
QList<Person> people(const QList<Chunk> &chunks, int fileCount, const GalleryPtr &gallery, float threshold);
// للتقرير الموحّد (Report::writeArchiveHtml)
QList<Report::ArchivePerson> toReport(const QList<Person> &persons, const QList<File> &files);

} // namespace Archive

class ArchiveScan : public QObject
{
    Q_OBJECT

public:
    enum class FileState { Waiting, Running, Done, Failed };

    explicit ArchiveScan(const FaceEngine *engine, QObject *parent = nullptr);
    ~ArchiveScan() override; // يوقف الخيوط ويحفظ ما حُلل

    static QString defaultStatePath();
    // ملف الحالة؛ يُقرأ بـ load ويُكتب بعد كل جزء وعند الإيقاف
    void setStatePath(const QString &path) { m_statePath = path; }
    bool load();
    bool save() const;
    // طول الجزء: أقصر = توازٍ أكثر في الملف الواحد واستئناف أدق، وأطول = تقطيع أقل لظهور الأشخاص
    void setChunkMs(qint64 ms) { m_chunkMs = std::max<qint64>(10000, ms); }

    void setGallery(GalleryPtr gallery);
    void setThreshold(float threshold);
    // عدد الخيوط المقترح: على الكرت يتوازى فك الفيديو مع التحليل، وعلى المعالج حسب الأنوية
    static int autoParallel(bool gpu);

    // يضيف ملفات الفيديو الصالحة غير المضافة من قبل، ويعيد عددها
    int addFiles(const QStringList &paths);
    void removeFile(int file);
    void clear();
    void start(int parallel);
    void stop(); // يحفظ موضع كل خيط فيتابع منه لاحقاً
    bool isRunning() const { return m_running; }
    bool isStopping() const { return m_stopping; }
    bool hasPending() const;

    const QList<Archive::File> &files() const { return m_files; }
    FileState fileState(int file) const;
    double fileProgress(int file) const;
    const QList<Archive::Chunk> &chunks() const { return m_chunks; }
    // الأجزاء المنتهية مع ما وُجد حتى الآن في الأجزاء الجارية
    QList<Archive::Chunk> snapshot() const;

    qint64 totalMs() const;
    qint64 doneMs() const;
    // مللي ثانية من الفيديو تُحلل في كل مللي ثانية فعلية (المقيسة في هذا التشغيل، أو المحفوظة من سابقه)
    double speed() const;
    qint64 remainingMs() const;
    int activeWorkers() const;

signals:
    void filesChanged();
    void progressChanged();
    void chunkFinished();
    void previewReady(int slot, const QImage &image, const QString &path, qint64 positionMs);
    void finished();

private:
    struct Slot
    {
        QThread *thread = nullptr;
        VideoWorker *worker = nullptr;
        int chunk = -1;
        qint64 positionMs = -1;
        bool stopping = false;
        QHash<int, FaceItem> faces;
    };

    void dispatch();
    void runChunk(int slot, int chunk);
    void onWorkerFinished(int slot, const QString &error);
    void shutdownSlots();
    qint64 chunkDoneMs(int chunk) const;

    const FaceEngine *m_engine;
    GalleryPtr m_gallery;
    float m_threshold = 0.30f;
    QString m_statePath;
    qint64 m_chunkMs = 10 * 60 * 1000;
    QList<Archive::File> m_files;
    QList<Archive::Chunk> m_chunks;
    std::vector<Slot> m_slots;
    bool m_running = false;
    bool m_stopping = false;
    QElapsedTimer m_clock;
    qint64 m_doneAtStart = 0;
    mutable double m_savedSpeed = 0.0; // آخر سرعة مقيسة، تُحفظ في الإعدادات للتقدير قبل البدء
};

#include "diagnostics.h"

#include "facedatabase.h"
#include "faceengine.h"
#include "imageio.h"
#include "videoworker.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace Diagnostics {

namespace {

// أعلى استهلاك للذاكرة منذ بدء العملية بالميجابايت
double peakMemoryMb()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return counters.PeakWorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024.0;
#endif
}

qint64 folderSize(const QString &path)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

bool loadEngine(FaceEngine &engine, QTextStream &out)
{
    QString error;
    if (engine.load(FaceEngine::defaultModelsDir(), &error))
        return true;
    out << "error=" << error << "\n";
    return false;
}

// يسجّل أكبر وجه في الصورة الأولى كما في قاعدة البيانات،
// ثم يكتب أعلى تشابه بينه وبين وجوه الصورة الثانية
int selfTest(const QString &first, const QString &second, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;

    QElapsedTimer timer;
    timer.start();
    const std::vector<AnalyzedFace> a = engine.analyze(ImageIO::loadImage(first), true);
    const std::vector<AnalyzedFace> b = engine.analyze(ImageIO::loadImage(second), true);
    out << "faces_first=" << a.size() << "\n";
    out << "faces_second=" << b.size() << "\n";
    if (a.empty() || b.empty())
        return 1;

    const auto largest = std::max_element(a.begin(), a.end(), [](const AnalyzedFace &x, const AnalyzedFace &y) {
        return x.detection.box.area() < y.detection.box.area();
    });
    const std::vector<std::vector<float>> templates = engine.enrollmentEmbeddings(largest->aligned);
    float best = -1.0f;
    for (const AnalyzedFace &face : b) {
        for (const std::vector<float> &t : templates)
            best = std::max(best, FaceEngine::cosine(t, face.embedding));
    }
    out << "best_similarity=" << best << "\n";
    out << "elapsed_ms=" << timer.elapsed() << "\n";
    return 0;
}

// يقارن التعرف على صورة رديئة قبل التحسين وبعده، ويحفظ النسخة المحسّنة
int enhanceTest(const QString &reference, const QString &probe, const QString &enhancedPath, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    const std::vector<AnalyzedFace> ref = engine.analyze(ImageIO::loadImage(reference), true);
    if (ref.empty())
        return 1;
    const std::vector<std::vector<float>> templates = engine.enrollmentEmbeddings(ref.front().aligned);
    const auto measure = [&](const std::vector<AnalyzedFace> &faces, const char *name) {
        float best = -1.0f;
        for (const AnalyzedFace &face : faces) {
            for (const std::vector<float> &t : templates)
                best = std::max(best, FaceEngine::cosine(t, face.embedding));
        }
        out << name << "_faces=" << faces.size() << "\n" << name << "_similarity=" << best << "\n";
    };
    const cv::Mat original = ImageIO::loadImage(probe);
    if (original.empty())
        return 1;
    measure(engine.analyze(original, true), "original");
    QElapsedTimer timer;
    timer.start();
    const cv::Mat enhanced = ImageIO::enhance(original);
    out << "enhance_ms=" << timer.elapsed() << "\n";
    measure(engine.analyze(enhanced, true), "enhanced");
    measure(engine.analyzeEnhanced(original, enhanced, true), "fused");
    ImageIO::toQImage(enhanced).save(enhancedPath);
    return 0;
}

// يحلل فيديو كاملاً كما تفعل الواجهة ويقيس السرعة والذاكرة وعدد الأشخاص
int videoTest(const QString &path, int step, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;

    // زمن الكشف واستخراج البصمة لإطار واحد
    {
        cv::VideoCapture capture;
        cv::Mat frame;
        if (ImageIO::openVideo(capture, path) && capture.read(frame)) {
            QElapsedTimer timer;
            engine.detect(frame, false);
            timer.start();
            for (int i = 0; i < 5; ++i)
                engine.detect(frame, false);
            out << "detect_ms=" << timer.elapsed() / 5.0 << "\n";
            const cv::Mat aligned(112, 112, CV_8UC3, cv::Scalar(90, 120, 160));
            engine.embed(aligned);
            timer.restart();
            for (int i = 0; i < 5; ++i)
                engine.embed(aligned);
            out << "embed_ms=" << timer.elapsed() / 5.0 << "\n";
        }
    }

    VideoWorker worker(&engine);
    worker.setFrameStep(step);
    int frames = 0;
    double fps = 25.0;
    int shown = 0;
    int updates = 0;
    QSet<int> tracks;
    QString error;
    // MALAMIH_TRACE: يسجل مواضع الوجوه وهوياتها في كل إطار لقياس دقة التتبع خارج البرنامج
    QFile traceFile(qEnvironmentVariable("MALAMIH_TRACE"));
    QTextStream trace(&traceFile);
    if (!traceFile.fileName().isEmpty())
        traceFile.open(QIODevice::WriteOnly | QIODevice::Text);
    QObject::connect(&worker, &VideoWorker::frameReady, [&](const QImage &image, const QList<FaceItem> &faces, qint64 positionMs, double) {
        frames = int(positionMs * std::max(1.0, fps) / 1000.0) + 1;
        ++shown;
        worker.frameShown();
        if (traceFile.isOpen()) {
            trace << "F " << positionMs << "\n";
            for (const FaceItem &face : faces) {
                trace << "B " << face.id << ' ' << face.box.x() / image.width() << ' ' << face.box.y() / image.height()
                      << ' ' << face.box.width() / image.width() << ' ' << face.box.height() / image.height()
                      << ' ' << face.quality << ' ' << face.match.personId.size() << ' ' << face.match.score << "\n";
            }
        }
    });
    QObject::connect(&worker, &VideoWorker::started, [&](int, double rate, qint64) { fps = rate; });
    QObject::connect(&worker, &VideoWorker::trackUpdated, [&](const FaceItem &track) {
        ++updates;
        tracks.insert(track.id);
    });
    int merged = 0;
    QObject::connect(&worker, &VideoWorker::identitiesMerged, [&](int keepId, int removedId) {
        ++merged;
        tracks.remove(removedId);
        if (traceFile.isOpen())
            trace << "M " << keepId << ' ' << removedId << "\n";
    });
    QObject::connect(&worker, &VideoWorker::finished, [&](const QString &message) { error = message; });

    QElapsedTimer timer;
    timer.start();
    worker.process(path);
    const double seconds = std::max(0.001, timer.elapsed() / 1000.0);

    out << "error=" << error << "\n";
    out << "frames=" << frames << "\n";
    out << "frames_shown=" << shown << "\n";
    out << "people=" << tracks.size() << "\n";
    out << "merged=" << merged << "\n";
    out << "track_updates=" << updates << "\n";
    out << "elapsed_s=" << seconds << "\n";
    out << "frames_per_second=" << frames / seconds << "\n";
    out << "peak_memory_mb=" << peakMemoryMb() << "\n";
    return error.isEmpty() && frames > 0 ? 0 : 1;
}

// يستخرج بصمة كل وجه في إطارات الفيديو كما هي، لدراسة توزيع التشابه في الفيديو الرديء
int embedDump(const QString &path, int step, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    const bool enhance = qEnvironmentVariableIntValue("MALAMIH_ENHANCE") != 0;
    cv::VideoCapture capture;
    if (!ImageIO::openVideo(capture, path))
        return 1;
    cv::Mat frame;
    for (int index = 1; capture.read(frame); ++index) {
        if ((index - 1) % std::max(1, step) != 0 || frame.empty())
            continue;
        const std::vector<AnalyzedFace> faces = enhance ? engine.analyzeEnhanced(frame, ImageIO::enhance(frame), false)
                                                        : engine.analyze(frame, false);
        for (const AnalyzedFace &face : faces) {
            const cv::Rect2f &b = face.detection.box;
            out << index << ' ' << b.x / frame.cols << ' ' << b.y / frame.rows << ' ' << b.width / frame.cols << ' '
                << b.height / frame.rows << ' ' << face.quality;
            for (float v : face.embedding)
                out << ' ' << v;
            out << "\n";
        }
    }
    return 0;
}

std::vector<float> randomEmbedding(std::mt19937 &random)
{
    std::normal_distribution<float> normal;
    std::vector<float> v(FaceEngine::EmbeddingSize);
    for (float &x : v)
        x = normal(random);
    FaceEngine::normalize(v);
    return v;
}

// بصمة "الهوية" لكل شخص اصطناعي تُولَّد من رقمه، فلا حاجة لحفظ ملايين البصمات في الذاكرة
std::vector<float> identityOf(int person)
{
    std::mt19937 random(quint32(person) * 2654435761u + 17u);
    return randomEmbedding(random);
}

// بصمة للشخص نفسه في ظروف مختلفة: كلما زاد noise قلّ التشابه (0.8 ≈ صورة جيدة، 1.5 ≈ صورة رديئة)
std::vector<float> noisy(const std::vector<float> &identity, float noise, std::mt19937 &random)
{
    std::vector<float> e = randomEmbedding(random);
    for (size_t d = 0; d < e.size(); ++d)
        e[d] = identity[d] + noise * e[d];
    FaceEngine::normalize(e);
    return e;
}

double currentMemoryMb()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return counters.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
#else
    QFile file(QStringLiteral("/proc/self/statm"));
    if (!file.open(QIODevice::ReadOnly))
        return 0.0;
    const QList<QByteArray> fields = file.readAll().split(' ');
    return fields.size() > 1 ? fields.at(1).toDouble() * 4096.0 / (1024.0 * 1024.0) : 0.0;
#endif
}

// ينشئ قاعدة بيانات اصطناعية كبيرة (حتى ملايين الأشخاص) ويقيس الإضافة والفتح والبحث والمطابقة والمساحة
// persons = 0: يُعاد استخدام قاعدة أنشأها اختبار سابق في المجلد نفسه دون بنائها من جديد
int databaseTest(const QString &directory, int persons, int photosPerPerson, QTextStream &out)
{
    const bool reuse = persons <= 0;
    if (!reuse)
        QDir(directory).removeRecursively();
    std::mt19937 random(7);
    // صورة صغيرة للاختبار حتى لا يملأ القرص؛ الصور الحقيقية 192 بكسل (نحو 7KB)
    QImage crop(48, 48, QImage::Format_RGB888);

    QString error;
    QElapsedTimer timer;
    timer.start();
    if (!reuse) {
        FaceDatabase database;
        if (!database.open(directory, &error)) {
            out << "error=" << error << "\n";
            return 1;
        }
        constexpr int BatchSize = 5000;
        int inBatch = 0;
        database.beginBatch();
        for (int p = 0; p < persons; ++p) {
            Person person;
            person.name = QStringLiteral("شخص %1").arg(p + 1);
            person.nationalId = QString::number(1000000000LL + p);
            const QString id = database.addPerson(person);
            const std::vector<float> identity = identityOf(p);
            for (int i = 0; i < photosPerPerson; ++i) {
                crop.fill(QColor::fromHsv((p * 37 + i * 11) % 360, 120, 200));
                std::vector<std::vector<float>> embeddings;
                for (int k = 0; k < 4; ++k)
                    embeddings.push_back(noisy(identity, 0.8f, random));
                if (!database.addPhoto(id, crop, embeddings, 0.8f)) {
                    out << "error=addPhoto failed\n";
                    return 1;
                }
            }
            if (++inBatch >= BatchSize) {
                database.endBatch();
                database.beginBatch();
                inBatch = 0;
            }
            if ((p + 1) % 100000 == 0)
                qInfo("%d persons, %lld s", p + 1, timer.elapsed() / 1000);
        }
        database.endBatch();
    }
    const qint64 buildMs = timer.elapsed();
    if (!reuse) {
        out << "persons=" << persons << "\n";
        out << "photos=" << qint64(persons) * photosPerPerson << "\n";
        out << "build_s=" << buildMs / 1000.0 << "\n";
        out.flush();
    }

    // فتح القاعدة من جديد كما يفعل التطبيق عند تشغيله
    timer.restart();
    FaceDatabase database;
    if (!database.open(directory, &error)) {
        out << "error=" << error << "\n";
        return 1;
    }
    out << "open_ms=" << timer.elapsed() << "\n";
    out << "memory_after_open_mb=" << currentMemoryMb() << "\n";
    if (reuse) {
        // آخر شخص اصطناعي هو الأحدث الذي له رقم هوية
        for (const Person &person : database.findPersons(QString(), 0, 100)) {
            if (!person.nationalId.isEmpty()) {
                persons = int(person.nationalId.toLongLong() - 1000000000LL) + 1;
                break;
            }
        }
        out << "persons=" << persons << "\n";
        if (persons <= 0)
            return 1;
    }

    // إضافة صورة واحدة بعد امتلاء القاعدة (معاملة كاملة كما في الواجهة)
    {
        const int p = persons / 2;
        std::vector<std::vector<float>> embeddings;
        for (int k = 0; k < 4; ++k)
            embeddings.push_back(noisy(identityOf(p), 0.8f, random));
        const QString id = database.findPersons(QString::number(1000000000LL + p), 0, 1).value(0).id;
        timer.restart();
        database.addPhoto(id, crop, embeddings, 0.8f);
        out << "photo_add_ms=" << timer.elapsed() << "\n";
        timer.restart();
        Person person;
        person.name = QStringLiteral("شخص إضافي");
        database.addPerson(person);
        out << "person_add_ms=" << timer.elapsed() << "\n";
    }

    // المطابقة: صور جيدة ورديئة لأشخاص عشوائيين، وصور لأشخاص غير مسجلين
    const GalleryPtr gallery = database.gallery();
    auto expectedId = [&](int p) { return QString::number(qint64(p) + 1); }; // الأرقام تبدأ من 1 بترتيب الإضافة
    const auto measure = [&](float noise, const char *name) {
        const int queries = 200;
        int correct = 0;
        FaceDatabase::identify(*gallery, noisy(identityOf(0), noise, random), 0.3f); // تهيئة
        QElapsedTimer t;
        t.start();
        for (int q = 0; q < queries; ++q) {
            const int target = int(random() % quint32(persons));
            const MatchResult match = FaceDatabase::identify(*gallery, noisy(identityOf(target), noise, random), 0.3f);
            correct += match.personId == expectedId(target) ? 1 : 0;
        }
        out << name << "_identify_ms=" << t.elapsed() / double(queries) << "\n";
        out << name << "_identify_correct=" << correct << "/" << queries << "\n";
        return correct == queries;
    };
    const bool goodOk = measure(0.8f, "good");
    measure(1.5f, "poor");
    {
        int falseMatches = 0;
        for (int q = 0; q < 200; ++q) {
            const MatchResult match = FaceDatabase::identify(*gallery, randomEmbedding(random), 0.3f);
            falseMatches += match.status == MatchStatus::Confirmed ? 1 : 0;
        }
        out << "unknown_false_matches=" << falseMatches << "/200\n";
    }

    // ملف الشخص: المرشحون، تماسك الصور، سجل الظهور، ودمج شخصين
    bool profileOk = true;
    {
        const int p = persons / 3;
        const QString id = expectedId(p);
        const QList<MatchResult> top = FaceDatabase::candidates(*gallery, noisy(identityOf(p), 0.8f, random), 5, 0.3f);
        const bool candidatesOk = top.size() == 5 && top.first().personId == id
            && std::is_sorted(top.cbegin(), top.cend(), [](const MatchResult &a, const MatchResult &b) { return a.score > b.score; });
        out << "candidates_ok=" << candidatesOk << " top=" << (top.isEmpty() ? 0.0f : top.first().score) << "\n";
        const QHash<qint64, float> consistency = database.photoConsistency(id);
        float minConsistency = 1.0f;
        for (float value : consistency)
            minConsistency = std::min(minConsistency, value);
        out << "consistency_photos=" << consistency.size() << " min=" << minConsistency << "\n";

        database.addSighting(id, QStringLiteral("video.mp4"), 12000, 0.7f, crop);
        database.addSighting(id, QStringLiteral("video.mp4"), 13500, 0.8f, crop); // الموضع نفسه تقريباً: لا يتكرر
        database.addSighting(id, QStringLiteral("photo.jpg"), -1, 0.6f, crop);
        const QList<Sighting> seen = database.sightings(id);
        const Person seenPerson = database.person(id);
        const bool sightingsOk = seen.size() == 2 && seenPerson.seenCount == 2 && seenPerson.lastSeen.isValid();
        out << "sightings=" << seen.size() << " seen_count=" << seenPerson.seenCount << " ok=" << sightingsOk << "\n";

        // الصورة الرئيسية وحذف صورة أخرى (يجب أن تبقى الرئيسية كما هي)
        bool photosOk = true;
        const Person before = database.person(id);
        if (before.photos.size() >= 2) {
            const qint64 cover = before.photos.last().id;
            database.setCoverPhoto(id, cover);
            database.removePhoto(id, before.photos.first().id);
            const Person afterRemove = database.person(id);
            photosOk = afterRemove.photoCount == before.photoCount - 1 && afterRemove.coverPhoto == cover;
            out << "remove_photo_ok=" << photosOk << " cover_kept=" << (afterRemove.coverPhoto == cover) << "\n";
        }

        // دمج شخص آخر فيه: تنتقل صوره وسجله ويُحذف
        const QString other = expectedId(p + 1);
        const int photosBefore = database.person(id).photoCount + database.person(other).photoCount;
        database.addSighting(other, QStringLiteral("other.mp4"), 5000, 0.9f, crop);
        timer.restart();
        const bool merged = database.mergePersons(id, other);
        out << "merge_ms=" << timer.elapsed() << "\n";
        const Person after = database.person(id);
        const bool mergeOk = merged && database.person(other).id.isEmpty() && after.photoCount == photosBefore
            && database.sightings(id).size() == 3;
        out << "merge_ok=" << mergeOk << " photos=" << after.photoCount << "\n";
        const MatchResult otherMatch = FaceDatabase::identify(*database.gallery(), noisy(identityOf(p + 1), 0.8f, random), 0.3f);
        const bool mergedIdentifyOk = otherMatch.personId == id;
        out << "merged_identify_ok=" << mergedIdentifyOk << "\n";
        profileOk = candidatesOk && sightingsOk && photosOk && mergeOk && mergedIdentifyOk && consistency.size() == photosPerPerson;
    }

    // البحث بالاسم ورقم الهوية
    timer.restart();
    const QList<Person> firstPage = database.findPersons(QString(), 0, 100);
    out << "first_page_ms=" << timer.elapsed() << "\n";
    timer.restart();
    const QList<Person> byId = database.findPersons(QString::number(1000000000LL + persons - 7), 0, 100);
    out << "search_id_ms=" << timer.elapsed() << " found=" << byId.size() << "\n";
    timer.restart();
    const QList<Person> byName = database.findPersons(QStringLiteral("شخص %1").arg(persons / 3), 0, 100);
    out << "search_name_ms=" << timer.elapsed() << " found=" << byName.size() << "\n";

    // إعادة بناء فهرس المطابقة (يحدث تلقائياً إن فُقد أو تلف)
    database.close();
    QFile::remove(QDir(directory).filePath(QStringLiteral("index.codes")));
    timer.restart();
    FaceDatabase rebuilt;
    rebuilt.open(directory, &error);
    out << "index_rebuild_ms=" << timer.elapsed() << "\n";
    rebuilt.close();

    out << "disk_mb=" << folderSize(directory) / (1024.0 * 1024.0) << "\n";
    out << "peak_memory_mb=" << peakMemoryMb() << "\n";
    return goodOk && profileOk && !firstPage.isEmpty() && !byId.isEmpty() ? 0 : 1;
}

} // namespace

int run(const QStringList &args)
{
    if (args.size() < 2)
        return -1;
    const QString command = args.at(1);
    if (command != QLatin1String("--self-test") && command != QLatin1String("--video-test")
        && command != QLatin1String("--db-test") && command != QLatin1String("--enhance-test")
        && command != QLatin1String("--embed-dump"))
        return -1;

    QFile output(args.last());
    if (args.size() < 4 || !output.open(QIODevice::WriteOnly | QIODevice::Text))
        return 2;
    QTextStream out(&output);

    // Malamih --self-test <صورة1> <صورة2> <ملف النتائج>
    if (command == QLatin1String("--self-test") && args.size() == 5)
        return selfTest(args.at(2), args.at(3), out);
    // Malamih --video-test <فيديو> [تحليل إطار من كل] <ملف النتائج>
    if (command == QLatin1String("--video-test") && (args.size() == 4 || args.size() == 5))
        return videoTest(args.at(2), args.size() == 5 ? args.at(3).toInt() : 1, out);
    // Malamih --embed-dump <فيديو> <تحليل إطار من كل> <ملف النتائج>
    if (command == QLatin1String("--embed-dump") && args.size() == 5)
        return embedDump(args.at(2), args.at(3).toInt(), out);
    // Malamih --db-test <مجلد> <عدد الأشخاص> <صور لكل شخص> <ملف النتائج>
    if (command == QLatin1String("--db-test") && args.size() == 6)
        return databaseTest(args.at(2), args.at(3).toInt(), args.at(4).toInt(), out);
    // Malamih --enhance-test <صورة مرجعية> <صورة رديئة> <حفظ المحسّنة> <ملف النتائج>
    if (command == QLatin1String("--enhance-test") && args.size() == 6)
        return enhanceTest(args.at(2), args.at(3), args.at(4), out);
    out << "error=invalid arguments\n";
    return 2;
}

} // namespace Diagnostics

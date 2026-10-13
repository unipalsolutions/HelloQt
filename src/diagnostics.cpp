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
    int shown = 0;
    int updates = 0;
    QSet<int> tracks;
    QString error;
    QObject::connect(&worker, &VideoWorker::frameReady, [&](const QImage &, const QList<FaceItem> &, int index, int) {
        frames = index;
        ++shown;
        worker.frameShown();
    });
    QObject::connect(&worker, &VideoWorker::trackUpdated, [&](const FaceItem &track) {
        ++updates;
        tracks.insert(track.id);
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
    out << "track_updates=" << updates << "\n";
    out << "elapsed_s=" << seconds << "\n";
    out << "frames_per_second=" << frames / seconds << "\n";
    out << "peak_memory_mb=" << peakMemoryMb() << "\n";
    return error.isEmpty() && frames > 0 ? 0 : 1;
}

std::vector<float> randomEmbedding(std::mt19937 &random)
{
    std::normal_distribution<float> normal;
    std::vector<float> v(512);
    for (float &x : v)
        x = normal(random);
    FaceEngine::normalize(v);
    return v;
}

// ينشئ قاعدة بيانات اصطناعية كبيرة ويقيس الإضافة والفتح والبحث والمساحة
int databaseTest(const QString &directory, int persons, int photosPerPerson, QTextStream &out)
{
    QDir(directory).removeRecursively();
    std::mt19937 random(7);
    QImage crop(240, 240, QImage::Format_RGB888);

    FaceDatabase database;
    QString error;
    if (!database.open(directory, &error)) {
        out << "error=" << error << "\n";
        return 1;
    }

    QElapsedTimer timer;
    timer.start();
    QElapsedTimer recent;
    recent.start();
    qint64 lastPhotoMs = 0;
    std::vector<std::vector<float>> identities;
    for (int p = 0; p < persons; ++p) {
        Person person;
        person.name = QStringLiteral("شخص %1").arg(p + 1);
        const QString id = database.addPerson(person);
        const std::vector<float> identity = randomEmbedding(random);
        identities.push_back(identity);
        for (int i = 0; i < photosPerPerson; ++i) {
            crop.fill(QColor::fromHsv((p * 37 + i * 11) % 360, 120, 200));
            QPainter(&crop).drawEllipse(60 + i * 5, 50, 120, 150);
            // صور الشخص نفسه بصمات متقاربة حول بصمته الأساسية
            std::vector<std::vector<float>> embeddings;
            for (int k = 0; k < 4; ++k) {
                std::vector<float> e = randomEmbedding(random);
                for (size_t d = 0; d < e.size(); ++d)
                    e[d] = identity[d] + 0.8f * e[d];
                FaceEngine::normalize(e);
                embeddings.push_back(e);
            }
            if (p == persons - 1 && i == photosPerPerson - 1)
                recent.restart();
            if (!database.addPhoto(id, crop, embeddings, 0.8f)) {
                out << "error=addPhoto failed\n";
                return 1;
            }
            if (p == persons - 1 && i == photosPerPerson - 1)
                lastPhotoMs = recent.elapsed();
        }
    }
    const qint64 buildMs = timer.elapsed();

    timer.restart();
    const GalleryPtr gallery = database.gallery();
    const int queries = 200;
    int correct = 0;
    for (int q = 0; q < queries; ++q) {
        const int target = int(random() % identities.size());
        std::vector<float> probe = randomEmbedding(random);
        for (size_t d = 0; d < probe.size(); ++d)
            probe[d] = identities[size_t(target)][d] + 0.8f * probe[d];
        FaceEngine::normalize(probe);
        const MatchResult match = FaceDatabase::identify(*gallery, probe, 0.3f);
        correct += match.personId == database.persons().at(target).id ? 1 : 0;
    }
    const double identifyMs = timer.elapsed() / double(queries);

    timer.restart();
    FaceDatabase reopened;
    if (!reopened.open(directory, &error)) {
        out << "error=" << error << "\n";
        return 1;
    }
    const qint64 openMs = timer.elapsed();

    out << "persons=" << reopened.persons().size() << "\n";
    out << "photos=" << persons * photosPerPerson << "\n";
    out << "build_ms=" << buildMs << "\n";
    out << "last_photo_add_ms=" << lastPhotoMs << "\n";
    out << "open_ms=" << openMs << "\n";
    out << "identify_ms=" << identifyMs << "\n";
    out << "identify_correct=" << correct << "/" << queries << "\n";
    out << "disk_mb=" << folderSize(directory) / (1024.0 * 1024.0) << "\n";
    out << "peak_memory_mb=" << peakMemoryMb() << "\n";
    return correct == queries ? 0 : 1;
}

} // namespace

int run(const QStringList &args)
{
    if (args.size() < 2)
        return -1;
    const QString command = args.at(1);
    if (command != QLatin1String("--self-test") && command != QLatin1String("--video-test")
        && command != QLatin1String("--db-test"))
        return -1;

    QFile output(args.last());
    if (args.size() < 4 || !output.open(QIODevice::WriteOnly | QIODevice::Text))
        return 2;
    QTextStream out(&output);

    // HelloQt --self-test <صورة1> <صورة2> <ملف النتائج>
    if (command == QLatin1String("--self-test") && args.size() == 5)
        return selfTest(args.at(2), args.at(3), out);
    // HelloQt --video-test <فيديو> [تحليل إطار من كل] <ملف النتائج>
    if (command == QLatin1String("--video-test") && (args.size() == 4 || args.size() == 5))
        return videoTest(args.at(2), args.size() == 5 ? args.at(3).toInt() : 1, out);
    // HelloQt --db-test <مجلد> <عدد الأشخاص> <صور لكل شخص> <ملف النتائج>
    if (command == QLatin1String("--db-test") && args.size() == 6)
        return databaseTest(args.at(2), args.at(3).toInt(), args.at(4).toInt(), out);
    out << "error=invalid arguments\n";
    return 2;
}

} // namespace Diagnostics

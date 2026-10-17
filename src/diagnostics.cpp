#include "diagnostics.h"

#include "alertsound.h"
#include "camerarules.h"
#include "cameras.h"
#include "clipexport.h"
#include "maintenance.h"
#include "dailyreport.h"
#include "personprofile.h"
#include "camerawatch.h"
#include "facedatabase.h"
#include "importdialog.h"
#include "recorder.h"
#include "faceengine.h"
#include "gpucheck.h"
#include "performance.h"
#include "imageio.h"
#include "report.h"
#include "videoexport.h"
#include "videofilters.h"
#include "playbackbar.h"
#include "videomarkers.h"
#include "videoplayer.h"
#include "videoworker.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <functional>
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
    // MALAMIH_ADAPTIVE=1: التكيّف مع سرعة الجهاز كما في الواجهة
    worker.setAdaptive(qEnvironmentVariableIntValue("MALAMIH_ADAPTIVE") != 0);
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
    // سرعة التحليل بالنسبة للتشغيل العادي: 1 فأكثر يعني أنه يلحق بالعرض
    out << "realtime_ratio=" << frames / std::max(1.0, fps) / seconds << "\n";
    out << "peak_memory_mb=" << peakMemoryMb() << "\n";
    return error.isEmpty() && frames > 0 ? 0 : 1;
}

// يشغّل الفيديو كما تفعل الواجهة: العرض من VideoPlayer والتحليل في خيطه معاً، ويقيس سلاسة العرض
int playerTest(const QString &path, int seconds, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    QThread thread;
    auto *worker = new VideoWorker(&engine);
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    worker->setDeliverFrames(false);
    worker->setAdaptive(true);
    int analyzed = 0;
    int boxes = 0;
    qint64 analyzedMs = 0;
    QObject::connect(worker, &VideoWorker::frameReady, [&](const QImage &, const QList<FaceItem> &faces, qint64 ms, double) {
        ++analyzed;
        boxes += int(faces.size());
        analyzedMs = ms;
    });
    bool finished = false;
    QObject::connect(worker, &VideoWorker::finished, [&](const QString &) { finished = true; });

    VideoPlayer player;
    int shown = 0;
    qint64 playedMs = 0;
    double worstGapMs = 0.0;
    QElapsedTimer gap;
    QObject::connect(&player, &VideoPlayer::frameReady, [&](const QImage &frame, qint64 ms) {
        if (!frame.isNull())
            ++shown;
        // الفجوة بين إطارين معروضين بعد أول ثانية (فتح الملف وتحميل التحليل)
        if (gap.isValid() && playedMs > 1000)
            worstGapMs = std::max(worstGapMs, gap.nsecsElapsed() / 1e6);
        gap.start();
        playedMs = ms;
        player.frameShown();
    });
    if (!player.openFile(path, 0)) {
        out << "error=cannot open video\n";
        return 1;
    }
    thread.start();
    worker->prepareStart();
    QMetaObject::invokeMethod(worker, "process", Qt::QueuedConnection, Q_ARG(QString, path));
    QElapsedTimer clock;
    clock.start();
    player.play();
    QEventLoop loop;
    QTimer::singleShot(seconds * 1000, &loop, &QEventLoop::quit);
    loop.exec();
    const double elapsed = clock.elapsed() / 1000.0;
    // مقاييس السلاسة قبل تجربة الأدوات (الإيقاف والسرعة يغيّرانها)
    const int shownPlain = shown;
    const double worstGapPlain = worstGapMs;
    const qint64 playedPlain = playedMs;

    // أدوات التشغيل: إطار بإطار، والسرعة، وتحليل الوقت المكتوب، والعلامات
    const auto wait = [](int ms) {
        QEventLoop pause;
        QTimer::singleShot(ms, &pause, &QEventLoop::quit);
        pause.exec();
    };
    player.pause();
    wait(250);
    const qint64 stepBase = playedMs;
    player.stepFrames(1);
    wait(300);
    const qint64 stepForwardMs = playedMs - stepBase;
    player.stepFrames(-1);
    wait(300);
    const qint64 stepBackMs = playedMs - stepBase;
    const qint64 rateBase = playedMs;
    player.setRate(2.0);
    QElapsedTimer rateClock;
    rateClock.start();
    player.play();
    wait(2000);
    const double measuredRate = (playedMs - rateBase) / double(std::max<qint64>(1, rateClock.elapsed()));
    player.setRate(1.0);
    const bool parseOk = PlaybackBar::parseTime(QStringLiteral("1:23")) == 83000
        && PlaybackBar::parseTime(QStringLiteral("01:02:03.5")) == 3723500
        && PlaybackBar::parseTime(QStringLiteral("٨٣")) == 83000 && PlaybackBar::parseTime(QStringLiteral("1,5")) == 1500
        && PlaybackBar::parseTime(QStringLiteral("٢:٠٥")) == 125000 && PlaybackBar::parseTime(QStringLiteral("abc")) < 0
        && PlaybackBar::parseTime(QString()) < 0;
    const QString markerVideo = QDir(QDir::tempPath()).filePath(QStringLiteral("malamih-marker-test.mp4"));
    const QMap<qint64, QString> markers = {{1500, QStringLiteral("دخول")}, {64000, QString()}};
    VideoMarkers::save(markerVideo, markers);
    const bool markersOk = VideoMarkers::load(markerVideo) == markers;
    VideoMarkers::save(markerVideo, {});
    const bool markersCleared = VideoMarkers::load(markerVideo).isEmpty();

    player.close();
    worker->requestStop();
    thread.quit();
    thread.wait();

    const double expected = std::min(elapsed, double(player.durationMs()) / 1000.0) * player.fps();
    out << "native_fps=" << player.fps() << "\n";
    out << "shown_frames=" << shownPlain << "\n";
    out << "display_fps=" << shownPlain / elapsed << "\n";
    out << "display_ratio=" << (expected > 0 ? shownPlain / expected : 0.0) << "\n";
    out << "worst_gap_ms=" << worstGapPlain << "\n";
    out << "played_ms=" << playedPlain << "\n";
    out << "analyzed_frames=" << analyzed << "\n";
    out << "analyzed_ms=" << analyzedMs << "\n";
    out << "analysis_lag_ms=" << playedPlain - analyzedMs << "\n";
    out << "boxes=" << boxes << "\n";
    out << "analysis_finished=" << int(finished) << "\n";
    out << "step_forward_ms=" << stepForwardMs << "\n";
    out << "step_back_ms=" << stepBackMs << "\n";
    out << "rate_2x_measured=" << measuredRate << "\n";
    out << "parse_time_ok=" << int(parseOk) << "\n";
    out << "markers_ok=" << int(markersOk && markersCleared) << "\n";
    out << "peak_memory_mb=" << peakMemoryMb() << "\n";
    const qint64 frameMs = qint64(1000.0 / std::max(1.0, player.fps()));
    const bool controlsOk = parseOk && markersOk && markersCleared && std::llabs(stepForwardMs - frameMs) <= 2 && std::llabs(stepBackMs) <= 2
        && measuredRate > 1.6;
    return shown > 0 && analyzed > 0 && controlsOk ? 0 : 1;
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

// ───────────── اختبار أسوأ الظروف ─────────────

struct Scenario
{
    const char *name;
    std::function<cv::Mat(const cv::Mat &, const std::vector<AnalyzedFace> &)> apply;
};

cv::Mat scaleTo(const cv::Mat &image, int width)
{
    cv::Mat out;
    const double f = double(width) / image.cols;
    cv::resize(image, out, cv::Size(), f, f, f < 1.0 ? cv::INTER_AREA : cv::INTER_CUBIC);
    return out;
}

cv::Mat addNoise(const cv::Mat &image, double sigma, unsigned seed)
{
    cv::Mat noise(image.size(), CV_16SC3);
    cv::theRNG().state = seed;
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(sigma));
    cv::Mat out;
    cv::Mat wide;
    image.convertTo(wide, CV_16SC3);
    cv::add(wide, noise, wide);
    wide.convertTo(out, CV_8UC3);
    return out;
}

cv::Mat jpeg(const cv::Mat &image, int quality)
{
    std::vector<uchar> buffer;
    cv::imencode(".jpg", image, buffer, {cv::IMWRITE_JPEG_QUALITY, quality});
    return cv::imdecode(buffer, cv::IMREAD_COLOR);
}

cv::Mat lighting(const cv::Mat &image, double gain, double gamma)
{
    cv::Mat lut(1, 256, CV_8U);
    for (int i = 0; i < 256; ++i)
        lut.at<uchar>(i) = cv::saturate_cast<uchar>(255.0 * std::pow(i / 255.0, gamma) * gain);
    cv::Mat out;
    cv::LUT(image, lut, out);
    return out;
}

cv::Mat rotate(const cv::Mat &image, double degrees)
{
    const cv::Point2f center(image.cols / 2.0f, image.rows / 2.0f);
    cv::Mat m = cv::getRotationMatrix2D(center, degrees, 1.0);
    const cv::Rect2f bounds = cv::RotatedRect(center, image.size(), float(degrees)).boundingRect2f();
    m.at<double>(0, 2) += bounds.width / 2.0 - center.x;
    m.at<double>(1, 2) += bounds.height / 2.0 - center.y;
    cv::Mat out;
    cv::warpAffine(image, out, m, bounds.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return out;
}

// يغطي جزءاً من كل وجه (كمامة أو نظارة شمسية) اعتماداً على نقاط الوجه في الصورة الأصلية
cv::Mat cover(const cv::Mat &image, const std::vector<AnalyzedFace> &faces, bool mouth)
{
    cv::Mat out = image.clone();
    for (const AnalyzedFace &face : faces) {
        const auto &l = face.detection.landmarks;
        const float eye = float(cv::norm(l[1] - l[0]));
        if (mouth) {
            const float top = l[2].y + eye * 0.12f;
            const cv::Rect2f box = face.detection.box;
            cv::rectangle(out, cv::Rect2f(box.x - eye * 0.1f, top, box.width + eye * 0.2f, box.y + box.height - top + eye * 0.2f),
                          cv::Scalar(205, 200, 190), cv::FILLED);
        } else {
            const cv::Point2f mid = (l[0] + l[1]) / 2.0f;
            cv::rectangle(out, cv::Rect2f(mid.x - eye * 1.05f, mid.y - eye * 0.32f, eye * 2.1f, eye * 0.62f), cv::Scalar(20, 20, 20),
                          cv::FILLED);
        }
    }
    return out;
}

// كاميرا مرتفعة تنظر إلى الأسفل: أعلى الرأس أقرب وأعرض، والذقن أبعد ومنضغط
cv::Mat highAngle(const cv::Mat &image, double strength)
{
    const float w = float(image.cols), h = float(image.rows);
    const float inset = float(w * 0.18 * strength);
    const std::array<cv::Point2f, 4> from{cv::Point2f(0, 0), cv::Point2f(w, 0), cv::Point2f(w, h), cv::Point2f(0, h)};
    const float squash = float(h * (1.0 - 0.32 * strength));
    const std::array<cv::Point2f, 4> to{cv::Point2f(0, 0), cv::Point2f(w, 0), cv::Point2f(w - inset, squash), cv::Point2f(inset, squash)};
    cv::Mat out;
    cv::warpPerspective(image, out, cv::getPerspectiveTransform(from.data(), to.data()), cv::Size(int(w), int(squash)),
                        cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return out;
}

// الصورة مصغّرة داخل لقطة كبيرة بخلفية متنوعة، فيبدو الوجه بعيداً (faceWidth بكسل تقريباً)
cv::Mat farAway(const cv::Mat &image, const std::vector<AnalyzedFace> &faces, float faceWidth, cv::Size canvasSize)
{
    float widest = 0.0f;
    for (const AnalyzedFace &face : faces)
        widest = std::max(widest, face.detection.box.width);
    const double f = widest > 0 ? std::min(1.0, double(faceWidth / widest)) : 0.15;
    cv::Mat shrunk;
    cv::resize(image, shrunk, cv::Size(), f, f, cv::INTER_AREA);
    cv::Mat canvas(canvasSize, CV_8UC3);
    cv::theRNG().state = 21;
    cv::randu(canvas, cv::Scalar::all(40), cv::Scalar::all(200));
    cv::GaussianBlur(canvas, canvas, cv::Size(0, 0), 9);
    const int x = std::max(0, int(canvas.cols * 0.62) - shrunk.cols / 2), y = std::max(0, int(canvas.rows * 0.55) - shrunk.rows / 2);
    const cv::Rect roi = cv::Rect(x, y, shrunk.cols, shrunk.rows) & cv::Rect(0, 0, canvas.cols, canvas.rows);
    shrunk(cv::Rect(0, 0, roi.width, roi.height)).copyTo(canvas(roi));
    return canvas;
}

std::vector<Scenario> scenarios()
{
    using Faces = std::vector<AnalyzedFace>;
    auto motion = [](const cv::Mat &m, int size) {
        cv::Mat kernel = cv::Mat::zeros(size, size, CV_32F);
        kernel.row(size / 2).setTo(1.0f / size);
        cv::Mat out;
        cv::filter2D(m, out, -1, kernel);
        return out;
    };
    auto blur = [](const cv::Mat &m, double sigma) {
        cv::Mat out;
        cv::GaussianBlur(m, out, cv::Size(0, 0), sigma);
        return out;
    };
    return {
        {"original", [](const cv::Mat &m, const Faces &) { return m.clone(); }},
        {"dark", [](const cv::Mat &m, const Faces &) { return lighting(m, 0.35, 1.6); }},
        {"very_dark_noisy", [](const cv::Mat &m, const Faces &) { return addNoise(lighting(m, 0.18, 1.8), 6, 3); }},
        {"overexposed", [](const cv::Mat &m, const Faces &) { return lighting(m, 1.9, 0.6); }},
        {"low_contrast", [](const cv::Mat &m, const Faces &) {
             cv::Mat out;
             m.convertTo(out, -1, 0.35, 120);
             return out;
         }},
        {"tiny", [](const cv::Mat &m, const Faces &) { return scaleTo(m, std::min(m.cols, 150)); }},
        {"small", [](const cv::Mat &m, const Faces &) { return scaleTo(m, std::min(m.cols, 240)); }},
        {"blur", [blur](const cv::Mat &m, const Faces &) { return blur(m, std::max(2.0, m.cols / 220.0)); }},
        {"motion_blur", [motion](const cv::Mat &m, const Faces &) { return motion(m, std::max(9, m.cols / 45) | 1); }},
        {"noise", [](const cv::Mat &m, const Faces &) { return addNoise(m, 28, 5); }},
        {"jpeg_q6", [](const cv::Mat &m, const Faces &) { return jpeg(m, 6); }},
        {"grayscale", [](const cv::Mat &m, const Faces &) {
             cv::Mat gray, out;
             cv::cvtColor(m, gray, cv::COLOR_BGR2GRAY);
             cv::cvtColor(gray, out, cv::COLOR_GRAY2BGR);
             return out;
         }},
        {"color_cast", [](const cv::Mat &m, const Faces &) {
             cv::Mat out;
             cv::multiply(m, cv::Scalar(1.35, 0.85, 0.55), out);
             return out;
         }},
        {"tilt_35", [](const cv::Mat &m, const Faces &) { return rotate(m, 35); }},
        {"rotated_90", [](const cv::Mat &m, const Faces &) {
             cv::Mat out;
             cv::rotate(m, out, cv::ROTATE_90_CLOCKWISE);
             return out;
         }},
        {"upside_down", [](const cv::Mat &m, const Faces &) {
             cv::Mat out;
             cv::rotate(m, out, cv::ROTATE_180);
             return out;
         }},
        {"mirrored", [](const cv::Mat &m, const Faces &) {
             cv::Mat out;
             cv::flip(m, out, 1);
             return out;
         }},
        {"mask", [](const cv::Mat &m, const Faces &f) { return cover(m, f, true); }},
        {"sunglasses", [](const cv::Mat &m, const Faces &f) { return cover(m, f, false); }},
        {"high_angle", [](const cv::Mat &m, const Faces &) { return highAngle(m, 1.0); }},
        {"high_angle_steep", [](const cv::Mat &m, const Faces &) { return highAngle(m, 1.6); }},
        {"high_angle_steep_far", [](const cv::Mat &m, const Faces &f) { return farAway(highAngle(m, 1.6), f, 36, cv::Size(1920, 1080)); }},
        {"far_24px", [](const cv::Mat &m, const Faces &f) { return farAway(m, f, 24, cv::Size(1920, 1080)); }},
        {"far_16px_4k", [](const cv::Mat &m, const Faces &f) { return farAway(m, f, 16, cv::Size(3840, 2160)); }},
        {"high_angle_far", [](const cv::Mat &m, const Faces &f) { return farAway(highAngle(m, 1.0), f, 30, cv::Size(2560, 1440)); }},
        {"high_angle_far_tilt", [](const cv::Mat &m, const Faces &f) {
             return farAway(rotate(highAngle(m, 1.2), 25), f, 34, cv::Size(1920, 1080));
         }},
        {"cctv", [blur](const cv::Mat &m, const Faces &) { return jpeg(addNoise(blur(scaleTo(m, std::min(m.cols, 260)), 0.8), 10, 9), 25); }},
        {"night_cctv", [blur](const cv::Mat &m, const Faces &) {
             return jpeg(addNoise(lighting(blur(scaleTo(m, std::min(m.cols, 320)), 0.7), 0.3, 1.5), 7, 11), 30);
         }},
    };
}

// يسجّل الأشخاص من مجلد (مجلد فرعي لكل شخص)، ثم يطبّق كل ظرف سيئ على صور الاختبار ويتحقق من التعرف.
// اسم صورة الاختبار يحدد من فيها: "obama+biden__x.jpg"، و"unknown__x.jpg" لشخص غير مسجل
int stressTest(const QString &enrollDir, const QString &probeDir, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    const QString dbDir = QDir::temp().filePath(QStringLiteral("malamih-stress"));
    QDir(dbDir).removeRecursively();
    FaceDatabase database;
    QString error;
    if (!database.open(dbDir, &error)) {
        out << "error=" << error << "\n";
        return 1;
    }

    QHash<QString, QString> names; // معرّف الشخص -> اسم المجلد
    for (const QFileInfo &folder : QDir(enrollDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        Person person;
        person.name = folder.fileName();
        const QString id = database.addPerson(person);
        names.insert(id, person.name);
        for (const QFileInfo &file : QDir(folder.filePath()).entryInfoList(QDir::Files, QDir::Name)) {
            const cv::Mat image = ImageIO::loadImage(file.filePath());
            std::vector<AnalyzedFace> faces = engine.analyze(image, true);
            if (faces.empty()) {
                out << "enroll_failed=" << file.fileName() << "\n";
                continue;
            }
            const auto largest = std::max_element(faces.begin(), faces.end(), [](const AnalyzedFace &a, const AnalyzedFace &b) {
                return a.detection.box.area() < b.detection.box.area();
            });
            const FaceItem item = ImageIO::makeFaceItem(image, *largest, 1);
            database.addPhoto(id, ImageIO::faceCrop(item), engine.enrollmentEmbeddings(largest->aligned), largest->quality);
        }
    }
    const GalleryPtr gallery = database.gallery();
    constexpr float Threshold = 0.30f;

    struct Probe
    {
        QString file;
        cv::Mat image;
        QSet<QString> expected;
        std::vector<AnalyzedFace> reference;
    };
    std::vector<Probe> probes;
    for (const QFileInfo &file : QDir(probeDir).entryInfoList(QDir::Files, QDir::Name)) {
        Probe probe;
        probe.file = file.fileName();
        probe.image = ImageIO::loadImage(file.filePath());
        if (probe.image.empty())
            continue;
        for (const QString &name : file.completeBaseName().section(QStringLiteral("__"), 0, 0).split(QLatin1Char('+')))
            if (name != QLatin1String("unknown"))
                probe.expected.insert(name);
        probe.reference = engine.analyze(probe.image, true);
        probes.push_back(std::move(probe));
    }

    // للمقارنة مع المسار القديم: MALAMIH_STRESS_BASELINE=1
    const bool baseline = qEnvironmentVariableIntValue("MALAMIH_STRESS_BASELINE") == 1;
    const QString only = qEnvironmentVariable("MALAMIH_STRESS_ONLY");
    int totalExpected = 0, totalFound = 0, totalWrong = 0;
    QElapsedTimer timer;
    timer.start();
    for (const Scenario &scenario : scenarios()) {
        if (!only.isEmpty() && !only.split(QLatin1Char(',')).contains(QLatin1String(scenario.name)))
            continue;
        int expected = 0, found = 0, wrong = 0;
        double similarity = 0.0;
        int similarityCount = 0;
        QStringList misses;
        for (const Probe &probe : probes) {
            const cv::Mat degraded = scenario.apply(probe.image, probe.reference);
            const std::vector<AnalyzedFace> faces = baseline ? engine.analyze(degraded, true) : engine.analyzeRobust(degraded);
            QSet<QString> seen;
            for (const AnalyzedFace &face : faces) {
                const MatchResult match = FaceDatabase::identify(*gallery, face.embedding, Threshold);
                const QString name = names.value(match.personId);
                if (match.status == MatchStatus::Confirmed) {
                    if (probe.expected.contains(name)) {
                        seen.insert(name);
                        similarity += match.score;
                        ++similarityCount;
                    } else {
                        ++wrong;
                        misses << QStringLiteral("%1:wrong=%2").arg(probe.file, name);
                    }
                }
            }
            expected += int(probe.expected.size());
            found += int(seen.size());
            for (const QString &name : probe.expected) {
                if (seen.contains(name))
                    continue;
                // أعلى تشابه مع الشخص المفقود يبين هل فشل الكشف (لا وجوه) أم التعرف (تشابه منخفض)
                float best = -1.0f;
                for (const AnalyzedFace &face : faces)
                    for (const MatchResult &c : FaceDatabase::candidates(*gallery, face.embedding, 5, Threshold))
                        if (names.value(c.personId) == name)
                            best = std::max(best, c.score);
                misses << QStringLiteral("%1:miss=%2(faces=%3,sim=%4)").arg(probe.file, name).arg(faces.size()).arg(best, 0, 'f', 2);
            }
        }
        totalExpected += expected;
        totalFound += found;
        totalWrong += wrong;
        out << "scenario=" << scenario.name << " found=" << found << "/" << expected << " wrong=" << wrong
            << " mean_similarity=" << (similarityCount ? similarity / similarityCount : 0.0);
        if (!misses.isEmpty())
            out << " misses=" << misses.join(QLatin1Char(','));
        out << "\n";
        out.flush();
    }
    out << "total_found=" << totalFound << "/" << totalExpected << "\n";
    out << "recall=" << (totalExpected ? double(totalFound) / totalExpected : 0.0) << "\n";
    out << "total_wrong=" << totalWrong << "\n";
    out << "elapsed_s=" << timer.elapsed() / 1000.0 << "\n";
    database.close();
    QDir(dbDir).removeRecursively();
    return totalWrong == 0 ? 0 : 1;
}

// يقارن كل صورتين في مجلد ويكتب أدنى تشابه لنفس الشخص وأعلاه لشخصين مختلفين
// (اسم الصورة قبل __ هو اسم الشخص)، لمعايرة أحكام أداة "مقارنة صورتين"
int pairMatrix(const QString &folder, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    struct Item
    {
        QString name, person;
        AnalyzedFace face;
    };
    std::vector<Item> items;
    const QStringList files = QDir(folder).entryList(QDir::Files, QDir::Name);
    for (const QString &file : files) {
        const std::vector<AnalyzedFace> faces = engine.analyzeRobust(ImageIO::loadImage(QDir(folder).filePath(file)));
        if (faces.empty()) {
            out << "no_face=" << file << "\n";
            continue;
        }
        const auto largest = std::max_element(faces.begin(), faces.end(), [](const AnalyzedFace &x, const AnalyzedFace &y) {
            return x.detection.box.area() < y.detection.box.area();
        });
        Item item;
        item.name = file;
        item.person = file.section(QStringLiteral("__"), 0, 0);
        item.face = *largest;
        items.push_back(std::move(item));
    }
    float genuineMin = 2.0f, impostorMax = -2.0f;
    for (size_t i = 0; i < items.size(); ++i) {
        for (size_t j = i + 1; j < items.size(); ++j) {
            const Item &a = items[i], &b = items[j];
            const float score = FaceEngine::cosine(a.face.embedding, b.face.embedding);
            const bool same = a.person == b.person;
            out << (same ? "genuine " : "impostor ") << a.name << " " << b.name << " similarity=" << score << "\n";
            if (same)
                genuineMin = std::min(genuineMin, score);
            else
                impostorMax = std::max(impostorMax, score);
        }
    }
    out << "genuine_min=" << genuineMin << "\nimpostor_max=" << impostorMax << "\n";
    return 0;
}

// يكتب تقرير HTML لوجوه صورة، مع فترات ظهور مصطنعة لاختبار قسم الفيديو أيضاً
int reportTest(const QString &image, const QString &html, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    const cv::Mat bgr = ImageIO::loadImage(image);
    const std::vector<AnalyzedFace> faces = engine.analyze(bgr, true);
    QList<FaceItem> items;
    for (size_t i = 0; i < faces.size(); ++i) {
        FaceItem item = ImageIO::makeFaceItem(bgr, faces[i], int(i) + 1);
        item.match.status = i == 0 ? MatchStatus::Confirmed : MatchStatus::Unknown;
        item.match.personId = QStringLiteral("1");
        item.match.name = QStringLiteral("شخص تجريبي");
        item.match.score = 0.71f;
        item.appearances = {{1000, 4000}, {3000, 9000}, {20000, 26000}};
        item.firstSeenMs = 1000;
        item.lastSeenMs = 26000;
        items << item;
    }
    Report::Source source;
    source.path = image;
    source.video = true;
    source.durationMs = 30000;
    QString error;
    const bool ok = Report::writeHtml(html, items, source, &error);
    out << "faces=" << items.size() << "\nscreen_time_ms=" << Report::screenTime(items.value(0).appearances)
        << "\nok=" << ok << "\n" << error;
    return ok && !items.isEmpty() ? 0 : 1;
}

// نسخة احتياطية ثم تعديل ثم استعادة: يجب أن تعود القاعدة كما كانت وقت النسخ
int backupTest(const QString &directory, QTextStream &out)
{
    QDir(directory).removeRecursively();
    FaceDatabase database;
    QString error;
    if (!database.open(directory, &error)) {
        out << "error=" << error << "\n";
        return 1;
    }
    for (int i = 0; i < 3; ++i) {
        Person person;
        person.name = QStringLiteral("شخص %1").arg(i + 1);
        // الشخص الأول في قائمة المراقبة، فيبقى فيها بعد النسخ والاستعادة
        if (i == 0)
            person.tags = FaceDatabase::withWatchTag(QStringLiteral("موظف"), true);
        database.addPerson(person);
    }
    const QString file = QDir(directory).filePath(QStringLiteral("backup.malamih"));
    const bool saved = database.backup(file, &error);
    const QPair<qint64, qint64> inspected = FaceDatabase::inspectBackup(file);
    for (int i = 0; i < 2; ++i) {
        Person person;
        person.name = QStringLiteral("بعد النسخ %1").arg(i + 1);
        database.addPerson(person);
    }
    const qint64 before = database.personCount();
    // اتصال قراءة مفتوح أثناء الاسترجاع (كما في خيوط البحث): على Windows يمنع استبدال الملف،
    // ويجب أن يرى البيانات المستعادة بعده
    const QString reader = QStringLiteral("malamih-test-reader");
    qint64 readerCount = -1;
    bool restored = false;
    {
        QSqlDatabase readDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), reader);
        readDb.setDatabaseName(QDir(directory).filePath(QStringLiteral("faces.db")));
        readDb.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        readDb.open();
        QSqlQuery count(readDb);
        count.exec(QStringLiteral("SELECT COUNT(*) FROM persons"));
        count.finish();
        restored = database.restore(file, &error);
        if (count.exec(QStringLiteral("SELECT COUNT(*) FROM persons")) && count.next())
            readerCount = count.value(0).toLongLong();
        count.finish();
        readDb.close();
    }
    QSqlDatabase::removeDatabase(reader);
    const qint64 after = database.personCount();
    const bool invalid = !database.restore(QDir(directory).filePath(QStringLiteral("index.codes")));
    out << "saved=" << saved << "\ninspected_persons=" << inspected.first << "\nbefore_restore=" << before
        << "\nrestored=" << restored << "\nafter_restore=" << after << "\nrejects_invalid=" << invalid
        << "\nkept_previous=" << QFile::exists(QDir(directory).filePath(QStringLiteral("faces.db.before-restore")))
        << "\nstill_open=" << (database.personCount() == 3) << "\nreader_sees_restored=" << readerCount;
    // قائمة المراقبة: التصنيف يُضاف ويُزال دون المساس ببقية التصنيفات
    const bool watchTags = FaceDatabase::isWatched(QStringLiteral("موظف، مطلوب")) && !FaceDatabase::isWatched(QStringLiteral("مراقبات"))
        && FaceDatabase::withWatchTag(QStringLiteral("موظف، مراقبة"), false) == QStringLiteral("موظف");
    const int watched = int(database.watchedIds().size());
    // سجل التنبيهات: ظهور الشخص المراقَب وحده في قائمة المراقبة، وكل الظهور في «كل الأشخاص»
    const QList<Person> all = database.findPersons(QString(), 0, 10);
    for (const Person &person : all)
        database.addSighting(person.id, QStringLiteral("clip.mp4"), 4000, 0.7f, QImage());
    const qsizetype logWatched = database.recentSightings(true, QDateTime(), 100).size();
    const qsizetype logAll = database.recentSightings(false, QDateTime(), 100).size();
    const qsizetype logFuture = database.recentSightings(false, QDateTime::currentDateTime().addDays(1), 100).size();
    const bool alertLog = logWatched == 1 && logAll == all.size() && logFuture == 0;
    // الاستيراد من مجلد: اسم الشخص ورقم هويته من اسم المجلد أو الصورة، والبحث عنه بالاسم أو الرقم
    auto parsed = [](const QString &text, bool fromFile) {
        QString name, nationalId;
        ImportDialog::parseName(text, fromFile, &name, &nationalId);
        return name + QLatin1Char('|') + nationalId;
    };
    const bool importNames = parsed(QStringLiteral("أحمد علي (2)"), true) == QStringLiteral("أحمد علي|")
        && parsed(QStringLiteral("أحمد_علي 3"), true) == QStringLiteral("أحمد علي|")
        && parsed(QStringLiteral("أحمد علي - 123456789"), false) == QStringLiteral("أحمد علي|123456789")
        && parsed(QStringLiteral("123456789 - سارة"), true) == QStringLiteral("سارة|123456789")
        && parsed(QStringLiteral("فريق 2024"), false) == QStringLiteral("فريق 2024|")
        && database.findPersonId(all.first().name, QString()) == all.first().id
        && database.findPersonId(QStringLiteral("غير موجود"), QString()).isEmpty();
    out << "\nwatch_tags=" << watchTags << "\nwatched_after_restore=" << watched << "\nalert_log=" << alertLog
        << "\nimport_names=" << importNames << "\n" << error;
    return saved && inspected.first == 3 && before == 5 && restored && after == 3 && readerCount == 3 && invalid && watchTags && watched == 1
                   && alertLog && importNames
               ? 0
               : 1;
}

// معالجات الفيديو: فيديو مولَّد من صورة وجه مدار 90 درجة، يُحلَّل بلا معالجة ثم بتصحيح الاتجاه،
// ومع حد أصغر وجه كبير، ومع تخطي الساكن؛ ثم يُصدَّر مقطع بعد المعالجة وتُفحص أبعاده
int filtersTest(const QString &imagePath, const QString &directory, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    const cv::Mat image = ImageIO::loadImage(imagePath);
    if (image.empty()) {
        out << "error=cannot read image\n";
        return 1;
    }
    const VideoFilters::Settings saved = VideoFilters::current();
    VideoFilters::setCurrent(VideoFilters::Settings());

    // وحدات المعالجة نفسها
    VideoFilters::Settings rotate;
    rotate.quarterTurns = 1;
    const cv::Mat rotated = VideoFilters::apply(image, rotate);
    VideoFilters::Settings tone = VideoFilters::preset(VideoFilters::Preset::Night, rotate);
    const cv::Mat toned = VideoFilters::apply(image, tone);
    const bool units = VideoFilters::apply(image, VideoFilters::Settings()).data == image.data && rotated.cols == image.rows
        && rotated.rows == image.cols && toned.size() == rotated.size() && tone.quarterTurns == 1 && tone.autoLevels
        && VideoFilters::preset(VideoFilters::Preset::Night, tone) == tone && tone.activeCount() == 5
        && !tone.summary().isEmpty() && VideoFilters::Settings().summary().isEmpty();
    out << "units=" << units << "\n";

    // فيديو الوجه المائل: ثانية ونصف
    QDir().mkpath(directory);
    const QString video = QDir(directory).filePath(QStringLiteral("sideways.avi"));
    cv::Mat sideways;
    cv::rotate(image, sideways, cv::ROTATE_90_COUNTERCLOCKWISE);
    cv::Mat padded;
    const int side = std::max(sideways.cols, sideways.rows) * 2;
    cv::copyMakeBorder(sideways, padded, (side - sideways.rows) / 2, (side - sideways.rows + 1) / 2, (side - sideways.cols) / 2,
                       (side - sideways.cols + 1) / 2, cv::BORDER_CONSTANT, cv::Scalar(40, 40, 40));
    cv::resize(padded, padded, cv::Size(480, 480));
    {
        cv::VideoWriter writer(QFile::encodeName(video).toStdString(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 25.0, padded.size());
        if (!writer.isOpened()) {
            out << "error=cannot write video\n";
            VideoFilters::setCurrent(saved);
            return 1;
        }
        for (int i = 0; i < 40; ++i)
            writer.write(padded);
    }

    std::vector<float> reference;
    {
        const std::vector<AnalyzedFace> faces = engine.analyze(image, true);
        if (!faces.empty())
            reference = faces.front().embedding;
    }
    float bestSimilarity = 0.0f;
    auto analyze = [&](const VideoFilters::Settings &filters, int *analyzedFaces) {
        VideoFilters::setCurrent(filters);
        VideoWorker worker(&engine);
        worker.setFrameStep(1);
        QSet<int> people;
        int faces = 0;
        QObject::connect(&worker, &VideoWorker::frameReady, [&](const QImage &, const QList<FaceItem> &items, qint64, double) {
            faces += int(items.size());
            worker.frameShown();
        });
        QObject::connect(&worker, &VideoWorker::trackUpdated, [&](const FaceItem &track) {
            people.insert(track.id);
            if (!reference.empty())
                bestSimilarity = std::max(bestSimilarity, FaceEngine::cosine(reference, track.embedding));
        });
        QObject::connect(&worker, &VideoWorker::identitiesMerged, [&](int, int removedId) { people.remove(removedId); });
        worker.process(video);
        if (analyzedFaces)
            *analyzedFaces = faces;
        return int(people.size());
    };
    // بعد تصحيح الاتجاه يُكشف الوجه في الإطارات كلها تقريباً وبصمته بصمة الصورة الأصلية
    int rawFaces = 0, fixedFaces = 0, stillFaces = 0;
    // الإطارات هنا متطابقة، فتخطي الساكن (المفعّل افتراضياً) يُطفأ لقياس الكشف في كل إطار
    VideoFilters::Settings plain;
    plain.skipStill = false;
    const int raw = analyze(plain, &rawFaces);
    bestSimilarity = 0.0f;
    VideoFilters::Settings every = rotate;
    every.skipStill = false;
    const int fixed = analyze(every, &fixedFaces);
    const float fixedSimilarity = bestSimilarity;
    VideoFilters::Settings tooSmall = every;
    tooSmall.minFaceSize = 400;
    const int ignored = analyze(tooSmall, nullptr);
    VideoFilters::Settings still = rotate;
    still.skipStill = true;
    const int stillPeople = analyze(still, &stillFaces);
    out << "raw_people=" << raw << "\nraw_faces=" << rawFaces << "\nfixed_people=" << fixed << "\nfixed_faces=" << fixedFaces
        << "\nfixed_similarity=" << fixedSimilarity << "\nmin_face_people=" << ignored << "\nskip_still_people=" << stillPeople << "\nskip_still_faces=" << stillFaces << "\n";

    // تصدير مقطع بعد المعالجة: الأبعاد تتبدل مع التدوير
    VideoFilters::Settings portrait;
    portrait.quarterTurns = 1;
    VideoFilters::setCurrent(portrait);
    cv::Mat wide;
    cv::resize(padded, wide, cv::Size(640, 360));
    // (الأبعاد الأصلية 640×360 تصبح 360×640 بعد التدوير)
    const QString wideVideo = QDir(directory).filePath(QStringLiteral("wide.avi"));
    {
        cv::VideoWriter writer(QFile::encodeName(wideVideo).toStdString(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 25.0, wide.size());
        for (int i = 0; i < 50; ++i)
            writer.write(wide);
    }
    const QString exported = QDir(directory).filePath(QStringLiteral("exported.mp4"));
    const QString exportError = VideoExport::exportSegments(wideVideo, {{0, 1500}}, exported, nullptr, true);
    cv::VideoCapture check;
    const bool exportOk = exportError.isEmpty() && ImageIO::openVideo(check, exported)
        && int(check.get(cv::CAP_PROP_FRAME_WIDTH)) == 360 && int(check.get(cv::CAP_PROP_FRAME_HEIGHT)) == 640;
    const cv::Mat frame = VideoExport::frameAt(wideVideo, 500);
    const cv::Mat original = VideoExport::frameAt(wideVideo, 500, false);
    const bool frameOk = frame.cols == 360 && frame.rows == 640 && original.cols == 640;
    out << "export_rotated=" << exportOk << " " << exportError << "\nframe_at=" << frameOk << "\n";

    VideoFilters::setCurrent(saved);
    return units && fixed >= 1 && fixedFaces >= 30 && fixedSimilarity > 0.6f && ignored == 0 && stillPeople >= 1
                   && stillFaces >= 30 && exportOk && frameOk
               ? 0
               : 1;
}


// الكاميرات: صيغ العناوين، حفظ كلمة المرور، القائمة المحفوظة، ثم بث حي حقيقي مع انقطاع وعودة
// إن وُجد ffmpeg يبث الفيديو المعطى كما تبث كاميرا شبكة
int cameraTest(const QString &videoPath, QTextStream &out)
{
    Cameras::Camera hik;
    hik.kind = Cameras::Kind::Network;
    hik.brand = QStringLiteral("hikvision");
    hik.host = QStringLiteral(" 192.168.1.64 ");
    hik.channel = 2;
    hik.substream = true;
    hik.user = QStringLiteral("admin");
    hik.password = QStringLiteral("p@ss:1/x");
    const QString hikUrl = hik.url();
    const QString shown = hik.url(false);
    Cameras::Camera dahua = hik;
    dahua.brand = QStringLiteral("dahua");
    dahua.substream = false;
    Cameras::Camera custom;
    custom.kind = Cameras::Kind::Network;
    custom.brand = QStringLiteral("custom");
    custom.path = QStringLiteral("rtsp://10.0.0.9:8554/live");
    custom.user = QStringLiteral("u");
    custom.password = QStringLiteral("v");
    const bool urls = hikUrl == QLatin1String("rtsp://admin:p%40ss%3A1%2Fx@192.168.1.64:554/Streaming/Channels/202")
        && !shown.contains(QLatin1String("p%40ss")) && shown.contains(QLatin1String("****"))
        && dahua.url(false).endsWith(QLatin1String("/cam/realmonitor?channel=2&subtype=0"))
        && custom.url() == QLatin1String("rtsp://u:v@10.0.0.9:8554/live");
    out << "urls=" << urls << " " << hikUrl << " | " << shown << " | " << dahua.url(false) << " | " << custom.url() << "\n" << Qt::flush;

    const QString stored = Cameras::protect(hik.password);
    const bool secrets = !stored.isEmpty() && !stored.contains(hik.password) && Cameras::unprotect(stored) == hik.password
        && Cameras::protect(QString()).isEmpty() && Cameras::unprotect(QString()).isEmpty();
    out << "secrets=" << secrets << " " << stored.left(2) << "\n" << Qt::flush;

    // القائمة المحفوظة؛ تُستعاد قائمة المستخدم بعد الاختبار
    const QList<Cameras::Camera> userCameras = Cameras::load();
    const QString userLast = Cameras::lastSource();
    hik.id = Cameras::newId();
    hik.name = QStringLiteral("مدخل المبنى");
    Cameras::Camera usb;
    usb.id = Cameras::newId();
    usb.name = QStringLiteral("محوّل الكاميرا التناظرية");
    usb.index = 1;
    usb.width = 720;
    usb.height = 576;
    usb.via = QStringLiteral("analog");
    Cameras::save({hik, usb});
    VideoFilters::Settings night = VideoFilters::preset(VideoFilters::Preset::Night, VideoFilters::Settings());
    night.quarterTurns = 2;
    Cameras::saveFilters(hik.id, night);
    Cameras::Camera found, local, missing;
    const QList<Cameras::Camera> loaded = Cameras::load();
    const bool resolved = Cameras::resolve(hik.id, &found);
    const bool localOk = Cameras::resolve(QStringLiteral("0"), &local) && local.kind == Cameras::Kind::Local && local.index == 0;
    const bool missingOk = !Cameras::resolve(QStringLiteral("c0000000000"), &missing);
    const bool hidden = QSettings().value(QStringLiteral("cameras/list/1/password")).toString() != hik.password;
    const QList<bool> checks = {loaded.size() == 2, resolved, found.password == hik.password, found.url() == hikUrl,
        found.hasFilters, found.filters == night, found.source() == QLatin1String("camera:") + hik.id,
        loaded.size() == 2 && loaded.at(1).width == 720, loaded.size() == 2 && !loaded.at(1).hasFilters, localOk, missingOk, hidden,
        loaded.size() == 2 && loaded.at(1).via == QLatin1String("analog")};
    QString failed;
    for (int i = 0; i < checks.size(); ++i) {
        if (!checks.at(i))
            failed += QString::number(i) + QLatin1Char(' ');
    }
    const bool storage = failed.isEmpty();
    if (!storage)
        out << "storage_failed=" << failed << "count=" << loaded.size() << " url=" << found.url(false)
            << " filters=" << found.hasFilters << "/" << found.filters.quarterTurns << "\n" << Qt::flush;
    Cameras::setLastSource(found.source());
    const bool last = Cameras::lastSource() == found.source();
    out << "storage=" << storage << " last=" << last << "\n" << Qt::flush;
    out << "local_devices=" << Cameras::localDevices().size() << " addresses=" << Cameras::localAddresses().join(QLatin1Char(',')) << "\n" << Qt::flush;
    // تشخيص الكاميرات الموصولة يعمل حتى بلا كاميرات (يكتب القوائم ومحاولات الفتح)
    out << "probe:\n" << Cameras::probeLocal() << "\n" << Qt::flush;

    // مساعدات التوصيل: العنوان المقترح للابتوب، والشبكة، وخطوات كل طريقة
    bool guide = Cameras::suggestedAddress(QStringLiteral("192.168.1.64")) == QLatin1String("192.168.1.200")
        && Cameras::suggestedAddress(QStringLiteral("10.0.5.200")) == QLatin1String("10.0.5.201")
        && Cameras::onLocalNetwork(QStringLiteral("127.0.0.1")) && Cameras::onLocalNetwork(QStringLiteral("camera.local"))
        && !Cameras::onLocalNetwork(QStringLiteral("203.0.113.7")) && Cameras::connections().size() == 8;
    for (const Cameras::Connection &c : Cameras::connections())
        guide = guide && Cameras::connection(c.key).key == c.key && Cameras::steps(c.key).contains(QLatin1String("<ol"));
    out << "guide=" << guide << "\n" << Qt::flush;

    // البحث في الشبكة ينتهي في وقت معقول ولو لم توجد كاميرات
    QElapsedTimer timer;
    timer.start();
    const QList<Cameras::Found> nearby = Cameras::discover();
    const bool discovered = timer.elapsed() < 30000;
    out << "discover=" << discovered << " found=" << nearby.size() << " " << timer.elapsed() << "ms\n";

    // الاكتشاف التلقائي على عنوان لا يرد: فشل مفهوم دون تغيير الكاميرا
    Cameras::Camera nobody;
    nobody.kind = Cameras::Kind::Network;
    nobody.host = QStringLiteral("127.0.0.1");
    QString detectError;
    timer.restart();
    const bool detectFails = !Cameras::detect(nobody, nullptr, &detectError) && !detectError.isEmpty()
        && nobody.brand == QLatin1String("hikvision") && timer.elapsed() < 20000;
    out << "detect_unreachable=" << detectFails << " " << timer.elapsed() << "ms\n";

    // عنوان لا يرد عليه أحد: الفشل سريع وسببه مفهوم
    Cameras::Camera dead;
    dead.kind = Cameras::Kind::Network;
    dead.brand = QStringLiteral("rtsp");
    dead.host = QStringLiteral("127.0.0.1");
    dead.port = 1;
    timer.restart();
    QString deadError;
    const bool deadFails = Cameras::snapshot(dead, &deadError).empty() && !deadError.isEmpty() && timer.elapsed() < 20000;
    out << "unreachable=" << deadFails << " " << timer.elapsed() << "ms " << deadError.section(QLatin1Char('\n'), 0, 0) << "\n" << Qt::flush;

    bool live = true;
    bool detected = true;
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (videoPath != QLatin1String("-") && !ffmpeg.isEmpty()) {
        const QString url = QStringLiteral("http://127.0.0.1:8080/video"); // منفذ تطبيق IP Webcam ومساره
        QProcess server;
        server.setStandardOutputFile(QProcess::nullDevice());
        server.setStandardErrorFile(QProcess::nullDevice());
        const auto startServer = [&] {
            server.start(ffmpeg, {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-re"),
                                  QStringLiteral("-stream_loop"), QStringLiteral("-1"), QStringLiteral("-i"), videoPath,
                                  QStringLiteral("-q:v"), QStringLiteral("5"), QStringLiteral("-f"), QStringLiteral("mjpeg"),
                                  QStringLiteral("-listen"), QStringLiteral("1"), url});
            server.waitForStarted();
        };
        const auto stopServer = [&] {
            server.kill();
            server.waitForFinished(5000);
        };
        Cameras::Camera stream;
        stream.id = Cameras::newId();
        stream.name = QStringLiteral("بث تجريبي");
        stream.kind = Cameras::Kind::Network;
        stream.brand = QStringLiteral("custom");
        stream.path = url;

        // قارئ البث الحي وحده. خادم ffmpeg قد يتأخر في الاستماع على جهاز بطيء، فيُعاد الاتصال حتى 3 مرات
        int read = 0;
        for (int attempt = 1; attempt <= 3 && read == 0; ++attempt) {
            startServer();
            QThread::msleep(800 * attempt);
            Cameras::LiveReader reader(stream);
            QString error;
            if (reader.open(&error)) {
                cv::Mat frame;
                while (read < 40 && reader.read(frame, 5000) && !frame.empty())
                    ++read;
            }
            out << "live_reader_frames=" << read << " fps=" << reader.fps() << " attempt=" << attempt << " " << error << "\n"
                << Qt::flush;
            stopServer();
        }

        // الاكتشاف التلقائي من العنوان وحده يصل إلى صيغة الهاتف
        startServer();
        QThread::msleep(800);
        Cameras::Camera guess;
        guess.kind = Cameras::Kind::Network;
        guess.host = QStringLiteral("127.0.0.1");
        cv::Mat guessed;
        QString guessError;
        // خادم ffmpeg يخدم اتصالاً واحداً ثم يخرج، وفحص المنافذ يستهلك اتصالاً: يُعاد تشغيله حتى ينتهي الاكتشاف
        QFuture<bool> detecting = QtConcurrent::run([&] { return Cameras::detect(guess, &guessed, &guessError); });
        while (!detecting.isFinished()) {
            if (server.waitForFinished(50))
                startServer();
        }
        detected = detecting.result() && guess.brand == QLatin1String("phone") && !guessed.empty();
        out << "detect=" << detected << " " << guess.url(false) << " " << guessError << "\n" << Qt::flush;
        stopServer();

        // التحليل كاملاً من كاميرا محفوظة، مع قطع البث ثم إعادته
        Cameras::save({hik, usb, stream});
        FaceEngine engine;
        if (!loadEngine(engine, out)) {
            Cameras::save(userCameras);
            return 1;
        }
        startServer();
        QThread::msleep(800);
        VideoWorker worker(&engine);
        int frames = 0, framesAfter = 0, lost = 0, restored = 0;
        QString error;
        QObject::connect(&worker, &VideoWorker::frameReady, [&](const QImage &, const QList<FaceItem> &, qint64, double) {
            worker.frameShown();
            if (timer.elapsed() > 120000)
                worker.requestStop(); // لم ينقطع البث أو لم يعد: لا ينتظر الاختبار للأبد
            ++frames;
            if (restored > 0 && ++framesAfter >= 20)
                worker.requestStop();
            else if (frames == 25 && lost == 0)
                stopServer(); // انقطاع الكاميرا
        });
        QObject::connect(&worker, &VideoWorker::connectionLost, [&](int attempt) {
            lost = attempt;
            if (attempt == 1)
                startServer(); // عادت الكاميرا
            else if (attempt > 15)
                worker.requestStop(); // لم تعد: لا ينتظر الاختبار للأبد
        });
        QObject::connect(&worker, &VideoWorker::connectionRestored, [&] { ++restored; });
        QObject::connect(&worker, &VideoWorker::finished, [&](const QString &message) { error = message; });
        timer.restart();
        worker.process(stream.source());
        stopServer();
        const bool snapshotAfter = worker.lastCameraFrame().empty();
        out << "worker_frames=" << frames << " lost_attempts=" << lost << " restored=" << restored
            << " frames_after=" << framesAfter << " error=" << error << " elapsed_ms=" << timer.elapsed() << "\n" << Qt::flush;
        live = detected && read >= 30 && frames >= 45 && lost >= 1 && restored == 1 && framesAfter >= 20 && error.isEmpty() && snapshotAfter;
    } else {
        out << "live=skipped\n";
    }

    Cameras::save(userCameras);
    Cameras::setLastSource(userLast);
    if (userCameras.isEmpty()) {
        QSettings settings;
        settings.remove(QStringLiteral("cameras"));
    }
    return urls && secrets && storage && last && guide && discovered && detectFails && deadFails && live ? 0 : 1;
}


// تسجيل الكاميرا بإطارات مولَّدة في مجلد باسم عربي: مدة مطابقة للوقت الفعلي مع تخطي الانقطاع، وموضع
// التسجيل الجاري، والتسجيل عند الحركة فقط، ورسم مربعات الوجوه، وحذف تسجيلات البرنامج القديمة وحدها
int recordTest(const QString &directory, QTextStream &out)
{
    const QString folder = QDir(directory).filePath(QStringLiteral("تسجيلات الاختبار"));
    QDir(folder).removeRecursively();
    const int previousRetention = CameraRecorder::retentionDays();
    CameraRecorder::setRetentionDays(0);
    std::mt19937 random(7);
    // إطار يتغير كله في كل مرة (حركة)، أو ثابت
    auto movingFrame = [&](cv::Size size) {
        cv::Mat frame(size, CV_8UC3);
        cv::randu(frame, cv::Scalar::all(0), cv::Scalar::all(255));
        return frame;
    };
    const cv::Mat still(cv::Size(640, 480), CV_8UC3, cv::Scalar(90, 90, 90));
    auto feed = [&](CameraRecorder &recorder, int frames, int intervalMs, const std::function<cv::Mat()> &make) {
        for (int i = 0; i < frames; ++i) {
            recorder.push(make());
            QThread::msleep(intervalMs);
        }
    };
    auto frameCount = [](const QString &path, cv::Size *size, cv::Mat *middle) {
        cv::VideoCapture capture;
        if (!ImageIO::openVideo(capture, path))
            return -1;
        int count = 0;
        cv::Mat frame;
        std::vector<cv::Mat> frames;
        while (capture.read(frame)) {
            if (size)
                *size = frame.size();
            frames.push_back(frame.clone());
            ++count;
        }
        if (middle && !frames.empty())
            *middle = frames[frames.size() / 2];
        return count;
    };
    QString error;

    // 1) تسجيل متصل: 3 ثوانٍ بسرعة 10 إطارات، ثم انقطاع 4 ثوانٍ لا يُملأ، ثم ثانية
    CameraRecorder continuous;
    const bool started = continuous.start(folder, QStringLiteral("كاميرا الاختبار"), 10.0, {}, &error);
    feed(continuous, 30, 100, [&] { return movingFrame(cv::Size(641, 481)); });
    const CameraRecorder::Position live = continuous.position();
    QThread::msleep(4000);
    feed(continuous, 10, 100, [&] { return movingFrame(cv::Size(641, 481)); });
    continuous.stop();
    const QStringList continuousFiles = continuous.files();
    cv::Size size;
    const int continuousFrames = continuousFiles.size() == 1 ? frameCount(continuousFiles.first(), &size, nullptr) : -1;
    const bool continuousOk = started && continuousFiles.size() == 1 && continuousFrames >= 34 && continuousFrames <= 48
        && size == cv::Size(640, 480) && QFileInfo(continuousFiles.first()).fileName().startsWith(QStringLiteral("كاميرا الاختبار"));
    const bool positionOk = live.path == (continuousFiles.isEmpty() ? QString() : continuousFiles.first()) && live.ms >= 2000
        && live.ms <= 3200 && continuous.position().ms < 0;
    out << "continuous_frames=" << continuousFrames << "\ncontinuous_size=" << size.width << "x" << size.height
        << "\nlive_position_ms=" << live.ms << "\ncontinuous=" << continuousOk << "\nposition=" << positionOk << "\n";

    // 2) عند الحركة فقط: مشهد ساكن لا يُسجَّل، ثم حركة ثانية تُسجَّل مع ثانيتين قبلها وما بعدها
    CameraRecorder::Options motionOptions;
    motionOptions.motionOnly = true;
    CameraRecorder motion;
    motion.start(folder, QStringLiteral("حركة"), 10.0, motionOptions, &error);
    feed(motion, 40, 100, [&] { return still.clone(); });
    const bool waited = motion.isWaitingForMotion() && motion.position().path.isEmpty();
    feed(motion, 10, 100, [&] { return movingFrame(still.size()); });
    const bool recordingMotion = !motion.isWaitingForMotion();
    feed(motion, 10, 100, [&] { return still.clone(); });
    motion.stop();
    const QStringList motionFiles = motion.files();
    const int motionFrames = motionFiles.size() == 1 ? frameCount(motionFiles.first(), nullptr, nullptr) : -1;
    // نحو ثانيتين قبل الحركة + ثانية حركة + ثانية بعدها، لا الثواني الأربع الساكنة كلها
    const bool motionOk = waited && recordingMotion && motionFiles.size() == 1 && motionFrames >= 25 && motionFrames <= 48;
    out << "motion_frames=" << motionFrames << "\nmotion_waited=" << waited << "\nmotion=" << motionOk << "\n";

    // 3) مربعات الوجوه تُرسم في التسجيل حين تُطلب
    CameraRecorder::Options overlayOptions;
    overlayOptions.overlay = true;
    CameraRecorder overlay;
    overlay.start(folder, QStringLiteral("مربعات"), 10.0, overlayOptions, &error);
    const cv::Mat black(cv::Size(640, 480), CV_8UC3, cv::Scalar::all(0));
    for (int i = 0; i < 15; ++i) {
        overlay.setOverlay({{QRectF(0.25, 0.25, 0.5, 0.5), QStringLiteral("أحمد"), QColor(0, 255, 0)}});
        overlay.push(black.clone());
        QThread::msleep(100);
    }
    overlay.stop();
    cv::Mat middle;
    const QStringList overlayFiles = overlay.files();
    if (overlayFiles.size() == 1)
        frameCount(overlayFiles.first(), nullptr, &middle);
    // الحافة اليسرى للمربع خضراء، ومركزه أسود كما هو
    int edge = 0;
    for (int x = 154; !middle.empty() && x <= 166; ++x)
        edge = std::max<int>(edge, middle.at<cv::Vec3b>(240, x)[1]);
    const bool overlayOk = !middle.empty() && edge > 80 && middle.at<cv::Vec3b>(240, 320)[1] < 40
        && black.at<cv::Vec3b>(240, 160)[1] == 0;
    out << "overlay_edge=" << edge << "\noverlay=" << overlayOk << "\n";

    // 4) الحذف التلقائي: تسجيل قديم للبرنامج يُحذف، وملف قديم آخر في المجلد لا يُمس
    const QString oldRecording = QDir(folder).filePath(QStringLiteral("تسجيل قديم.mp4"));
    const QString otherFile = QDir(folder).filePath(QStringLiteral("ملف المستخدم.mp4"));
    const QDateTime old = QDateTime::currentDateTime().addDays(-40);
    for (const QString &path : {oldRecording, otherFile}) {
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write("old");
        file.close();
        // بعد الكتابة، وإلا يعيد إغلاق الملف وقت التعديل إلى الآن
        file.open(QIODevice::ReadWrite);
        file.setFileTime(old, QFileDevice::FileModificationTime);
        file.close();
    }
    CameraRecorder::rememberFile(oldRecording);
    const int keptWithoutRetention = CameraRecorder::removeExpired();
    CameraRecorder::setRetentionDays(30);
    const int removed = CameraRecorder::removeExpired();
    CameraRecorder::setRetentionDays(previousRetention);
    const bool retentionOk = keptWithoutRetention == 0 && removed == 1 && !QFile::exists(oldRecording) && QFile::exists(otherFile)
        && !continuousFiles.isEmpty() && QFile::exists(continuousFiles.first());
    out << "retention_removed=" << removed << "\nretention=" << retentionOk << "\n" << error;
    return continuousOk && positionOk && motionOk && overlayOk && retentionOk ? 0 : 1;
}


// منطق مراقبة الكاميرا دون كاميرا ولا نماذج: بصمات صناعية تمر على CameraWatch كأنها إطارات محلَّلة
int watchTest(const QString &directory, QTextStream &out)
{
    QDir(directory).removeRecursively();
    FaceDatabase database;
    QString error;
    if (!database.open(directory, &error)) {
        out << "error=" << error << "\n";
        return 1;
    }
    std::mt19937 random(7);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    const auto randomPrint = [&] {
        std::vector<float> print(static_cast<size_t>(FaceEngine::EmbeddingSize));
        for (float &value : print)
            value = gauss(random);
        FaceEngine::normalize(print);
        return print;
    };
    // بصمة الوجه نفسه في إطار آخر: تشابه نحو 0.8
    const auto noisy = [&](const std::vector<float> &base) {
        std::vector<float> print = base;
        for (float &value : print)
            value += gauss(random) * 0.03f;
        FaceEngine::normalize(print);
        return print;
    };
    QImage crop(64, 64, QImage::Format_RGB32);
    crop.fill(Qt::gray);

    Person watchedPerson;
    watchedPerson.name = QStringLiteral("مراقَب");
    watchedPerson.tags = FaceDatabase::withWatchTag(QString(), true);
    const QString watchedId = database.addPerson(watchedPerson);
    Person other;
    other.name = QStringLiteral("معروف");
    const QString otherId = database.addPerson(other);

    const bool previousVisitors = CameraWatch::visitorsEnabled();
    CameraWatch::setVisitorsEnabled(true);
    const float threshold = 0.5f;
    CameraWatch watch(&database);
    watch.reset(QStringLiteral("camera:0"), QStringLiteral("اختبار"), nullptr);
    watch.setWatched(database.watchedIds());
    watch.setThreshold(threshold);
    int alerts = 0;
    QObject::connect(&watch, &CameraWatch::watchAlert, [&alerts] { ++alerts; });

    const auto known = [&](const QString &id, const QString &name, float score) {
        CameraWatch::Face face;
        face.box = QRectF(10, 10, 50, 50);
        face.match.status = MatchStatus::Confirmed;
        face.match.personId = id;
        face.match.name = name;
        face.match.score = score;
        face.quality = 0.8f;
        face.crop = crop;
        return face;
    };
    const auto stranger = [&](const std::vector<float> &print, float quality, int track) {
        CameraWatch::Face face;
        face.box = QRectF(100, 10, 50, 50);
        face.match.status = MatchStatus::Unknown;
        face.embedding = print;
        face.quality = quality;
        face.track = track;
        face.crop = crop;
        return face;
    };
    const auto count = [&](const QString &id) { return database.sightings(id, 100).size(); };

    // إطار واحد بتشابه عادي لا يكفي (وجه مارّ يشبه المراقَب لحظة)، والثاني يؤكده
    watch.process(QImage(), {known(watchedId, watchedPerson.name, 0.55f)});
    const qsizetype afterOne = count(watchedId);
    watch.process(QImage(), {known(watchedId, watchedPerson.name, 0.56f)});
    const qsizetype afterTwo = count(watchedId);
    // ما دام ظاهراً فهو الظهور نفسه
    for (int i = 0; i < 5; ++i)
        watch.process(QImage(), {known(watchedId, watchedPerson.name, 0.6f)});
    const qsizetype afterMore = count(watchedId);
    // تشابه قوي جداً يكفي وحده
    watch.process(QImage(), {known(otherId, other.name, threshold + CameraWatch::StrongMargin + 0.05f)});
    const qsizetype strong = count(otherId);
    const bool confirmOk = afterOne == 0 && afterTwo == 1 && afterMore == 1 && strong == 1 && alerts == 1;
    out << "confirm_one=" << afterOne << "\nconfirm_two=" << afterTwo << "\nconfirm_more=" << afterMore
        << "\nstrong=" << strong << "\nalerts=" << alerts << "\nconfirm=" << confirmOk << "\n";

    // الزوار المجهولون: الوجه نفسه بمسارات مختلفة زائر واحد، ووجه آخر زائر ثانٍ، والمشوش لا يُحفظ
    const std::vector<float> visitorA = randomPrint();
    const std::vector<float> visitorB = randomPrint();
    watch.process(QImage(), {stranger(noisy(visitorA), 0.8f, 1), stranger(noisy(visitorB), 0.8f, 2)});
    watch.process(QImage(), {stranger(noisy(visitorA), 0.8f, 1), stranger(noisy(visitorB), 0.8f, 2)});
    watch.process(QImage(), {stranger(noisy(visitorA), 0.8f, 3)}); // فقد التتبع ثم عاد بمسار جديد
    watch.process(QImage(), {stranger(randomPrint(), 0.1f, 4), stranger(randomPrint(), 0.1f, 4)});
    const QList<Visitor> firstDay = database.visitors(QDateTime(), 1, 100);
    // عودة الزائر الأول في جلسة أخرى (بعد غياب): يُعرف من القاعدة ببصمته فيبقى الرقم نفسه
    CameraWatch later(&database);
    later.reset(QStringLiteral("camera:1"), QStringLiteral("اختبار 2"), nullptr);
    later.setThreshold(threshold);
    later.process(QImage(), {stranger(noisy(visitorA), 0.8f, 1)});
    later.process(QImage(), {stranger(noisy(visitorA), 0.8f, 1)});
    const QList<Visitor> visitors = database.visitors(QDateTime(), 1, 100);
    const QList<Visitor> repeated = database.visitors(QDateTime(), 2, 100);
    const bool clusterOk = firstDay.size() == 2 && visitors.size() == 2 && repeated.size() == 1
        && repeated.first().seenCount == 2 && repeated.first().cameras.size() == 2;
    out << "visitors_first=" << firstDay.size() << "\nvisitors=" << visitors.size() << "\nrepeated=" << repeated.size()
        << "\nrepeated_count=" << (repeated.isEmpty() ? 0 : repeated.first().seenCount) << "\ncluster=" << clusterOk << "\n";

    // تسمية الزائر: ينتقل ظهوره إلى سجل الشخص ويختفي من الزوار
    bool assignOk = false;
    if (!repeated.isEmpty()) {
        const qint64 id = repeated.first().id;
        Person named;
        named.name = QStringLiteral("زائر مسمّى");
        const QString namedId = database.addPerson(named);
        const bool assigned = database.assignVisitor(id, namedId, crop, {database.visitorEmbedding(id)}, 0.8f);
        assignOk = assigned && count(namedId) == 2 && database.visitors(QDateTime(), 1, 100).size() == 1
            && !database.personEmbedding(namedId).empty();
        out << "assigned_sightings=" << count(namedId) << "\n";
    }
    out << "assign=" << assignOk << "\n";
    CameraWatch::setVisitorsEnabled(previousVisitors);
    return confirmOk && clusterOk && assignOk ? 0 : 1;
}


// التقرير اليومي من قاعدة فيها ظهور أمام الكاميرا وزائر مجهول: HTML وPDF
int dailyReportTest(const QString &directory, QTextStream &out)
{
    QDir(directory).removeRecursively();
    FaceDatabase database;
    QString error;
    if (!database.open(QDir(directory).filePath(QStringLiteral("db")), &error)) {
        out << "error=" << error << "\n";
        return 1;
    }
    QImage face(96, 96, QImage::Format_RGB32);
    face.fill(QColor(200, 160, 120));
    Person watched;
    watched.name = QStringLiteral("مراقَب التقرير");
    watched.tags = FaceDatabase::withWatchTag(QString(), true);
    const QString watchedId = database.addPerson(watched);
    Person known;
    known.name = QStringLiteral("معروف التقرير");
    known.phone = QStringLiteral("0599123456");
    known.address = QStringLiteral("رام الله");
    known.notes = QStringLiteral("ملاحظة للفحص");
    const QString knownId = database.addPerson(known);
    std::vector<std::vector<float>> photoPrint(1, std::vector<float>(size_t(FaceEngine::EmbeddingSize), 0.0f));
    photoPrint[0][1] = 1.0f;
    database.addPhoto(knownId, face, photoPrint, 0.8f);
    database.addCameraSighting(watchedId, QStringLiteral("camera:0"), QString(), -1, 0.7f, face, QString());
    database.addCameraSighting(knownId, QStringLiteral("camera:0"), QString(), -1, 0.6f, face, QString());
    database.addCameraSighting(knownId, QStringLiteral("camera:1"), QString(), -1, 0.6f, face, QString());
    std::vector<float> print(size_t(FaceEngine::EmbeddingSize), 0.0f);
    print[0] = 1.0f;
    database.addVisitorSighting(print, 0.8f, 0.5f, QStringLiteral("camera:1"), QString(), -1, face, QString());

    const QString html = QDir(directory).filePath(QStringLiteral("تقرير.html"));
    const QString pdf = QDir(directory).filePath(QStringLiteral("تقرير.pdf"));
    const QSet<QString> ids = database.watchedIds();
    const bool htmlOk = DailyReport::write(html, database, ids, QDate::currentDate(), &error);
    const bool pdfOk = DailyReport::write(pdf, database, ids, QDate::currentDate(), &error);
    QFile file(html);
    const QString text = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    const bool content = text.contains(watched.name) && text.contains(known.name) && text.contains(QStringLiteral("#1"))
        && text.contains(QStringLiteral("class=\"watched\"")) && text.contains(QStringLiteral("data:image/jpeg"));
    QFile pdfFile(pdf);
    const QByteArray pdfHead = pdfFile.open(QIODevice::ReadOnly) ? pdfFile.read(5) : QByteArray();
    const qint64 pdfSize = QFileInfo(pdf).size();
    // ملف الشخص بصيغة PDF
    const QString profile = QDir(directory).filePath(QStringLiteral("ملف.pdf"));
    const bool profileOk = exportPersonPdf(database, knownId, profile) && QFileInfo(profile).size() > 2000;
    out << "html=" << htmlOk << "\nhtml_content=" << content << "\npdf=" << pdfOk << "\npdf_size=" << pdfSize
        << "\nprofile_pdf=" << profileOk << "\n" << error << "\n";
    return htmlOk && content && pdfOk && pdfHead == "%PDF-" && pdfSize > 2000 && profileOk ? 0 : 1;
}


// فحص كرت الشاشة: سرعته مقابل المعالج وتطابق البصمات. على جهاز بلا كرت مناسب ينجح إن عمل المعالج
int gpuTest(const QString &image, QTextStream &out)
{
    const GpuCheck::Result result = GpuCheck::run(FaceEngine::defaultModelsDir(), image == QLatin1String("-") ? QString() : image);
    out << "gpu=" << result.gpu << "\ncpu_ok=" << result.cpuOk << "\ngpu_ok=" << result.gpuOk
        << "\ncpu_ms=" << result.cpuMs << "\ngpu_ms=" << result.gpuMs << "\nfaces=" << result.faces
        << "\ngpu_faces=" << result.gpuFaces << "\nsimilarity=" << result.similarity << "\nreliable=" << result.reliable()
        << "\nerror=" << result.error << "\nsummary=" << result.summary() << "\n";
    if (!result.cpuOk)
        return 1;
    return result.gpu.isEmpty() || result.reliable() ? 0 : 1;
}


// قواعد الكاميرات والصيانة والتنبيهات دون كاميرا ولا نماذج: المناطق والجداول، وتنبيه المجهول المتكرر،
// وحذف الظهور واللقطات القديمة، والنسخ الاحتياطي التلقائي، وأصوات التنبيه، وتصدير مقطع الظهور
int rulesTest(const QString &directory, QTextStream &out)
{
    QDir(directory).removeRecursively();
    QDir().mkpath(directory);
    bool allOk = true;
    const auto check = [&](const char *name, bool ok) {
        out << name << "=" << ok << "\n";
        allOk = allOk && ok;
    };

    // 1) الفترات الزمنية، ومنها ما يعبر منتصف الليل
    const CameraRules::Window night{true, QTime(22, 0), QTime(6, 0)};
    const CameraRules::Window day{true, QTime(8, 0), QTime(17, 0)};
    check("window", night.contains(QTime(23, 30)) && night.contains(QTime(5, 0)) && !night.contains(QTime(12, 0))
                        && day.contains(QTime(9, 0)) && !day.contains(QTime(17, 0)) && CameraRules::Window{true, QTime(3, 0), QTime(3, 0)}.contains(QTime(15, 0))
                        && CameraRules::Window{}.active());

    // 2) المناطق وحفظها
    CameraRules::Rules rules;
    rules.zones = {QRectF(0, 0, 0.5, 1)};
    const QSize frameSize(200, 100);
    const bool includeOk = rules.accepts(QRectF(20, 20, 40, 40), frameSize) && !rules.accepts(QRectF(140, 20, 40, 40), frameSize);
    rules.mode = CameraRules::ZoneMode::Exclude;
    const bool excludeOk = !rules.accepts(QRectF(20, 20, 40, 40), frameSize) && rules.accepts(QRectF(140, 20, 40, 40), frameSize);
    rules.recording = night;
    const QString testSource = QStringLiteral("camera:rules/test 1");
    CameraRules::save(testSource, rules);
    const CameraRules::Rules loaded = CameraRules::load(testSource);
    const bool storeOk = loaded.zones.size() == 1 && std::abs(loaded.zones.first().width() - 0.5) < 0.001
        && loaded.mode == CameraRules::ZoneMode::Exclude && loaded.recording.enabled && loaded.recording.from == QTime(22, 0)
        && !loaded.alerts.enabled;
    CameraRules::save(testSource, {});
    check("zones", includeOk && excludeOk && storeOk && CameraRules::load(testSource).isEmpty());

    // 3) CameraWatch: وجه خارج المنطقة لا يُحسب، والتنبيه خارج ساعاته لا يصدر والظهور يُسجَّل
    FaceDatabase database;
    QString error;
    if (!database.open(QDir(directory).filePath(QStringLiteral("db")), &error)) {
        out << "error=" << error << "\n";
        return 1;
    }
    const QString previousFolder = CameraRecorder::savedFolder();
    CameraRecorder::setSavedFolder(QDir(directory).filePath(QStringLiteral("recordings")));
    const bool previousVisitors = CameraWatch::visitorsEnabled();
    const CameraWatch::StrangerAlerts previousStrangers = CameraWatch::strangerAlerts();
    CameraWatch::setVisitorsEnabled(true);

    Person watchedPerson;
    watchedPerson.name = QStringLiteral("مراقَب");
    watchedPerson.tags = FaceDatabase::withWatchTag(QString(), true);
    const QString watchedId = database.addPerson(watchedPerson);
    QImage frame(frameSize, QImage::Format_RGB32);
    frame.fill(Qt::darkGray);
    QImage crop(64, 64, QImage::Format_RGB32);
    crop.fill(Qt::gray);
    const auto known = [&](const QRectF &box) {
        CameraWatch::Face face;
        face.box = box;
        face.match.status = MatchStatus::Confirmed;
        face.match.personId = watchedId;
        face.match.name = watchedPerson.name;
        face.match.score = 0.55f;
        face.quality = 0.8f;
        face.crop = crop;
        return face;
    };
    const QString zoneSource = QStringLiteral("camera:rules-zone");
    CameraRules::Rules zoneRules;
    zoneRules.zones = {QRectF(0, 0, 0.5, 1)};
    // ساعات تنبيه لا تشمل الآن
    zoneRules.alerts = {true, QTime::currentTime().addSecs(2 * 3600), QTime::currentTime().addSecs(3 * 3600)};
    CameraRules::save(zoneSource, zoneRules);
    CameraWatch watch(&database);
    watch.reset(zoneSource, QStringLiteral("منطقة"), nullptr);
    watch.setWatched(database.watchedIds());
    watch.setThreshold(0.5f);
    int alerts = 0;
    QObject::connect(&watch, &CameraWatch::watchAlert, [&alerts] { ++alerts; });
    for (int i = 0; i < 3; ++i)
        watch.process(frame, {known(QRectF(140, 20, 40, 40))});
    const qsizetype outside = database.sightings(watchedId, 100).size();
    for (int i = 0; i < 2; ++i)
        watch.process(frame, {known(QRectF(20, 20, 40, 40))});
    const qsizetype inside = database.sightings(watchedId, 100).size();
    check("watch_zones", outside == 0 && inside == 1);
    check("alert_schedule", alerts == 0 && !watch.alertsActive());
    CameraRules::save(zoneSource, {});

    // 4) مستوى التنبيه لكل شخص
    const bool levelOk = database.alertLevel(watchedId) == AlertSound::Normal && database.setAlertLevel(watchedId, AlertSound::High)
        && database.alertLevel(watchedId) == AlertSound::High && database.alertLevels().value(watchedId) == AlertSound::High;
    check("alert_level", levelOk);

    // 5) تنبيه المجهول المتكرر: ظهوره مرتين اليوم (أمام كاميرتين) يكفي حين يكون الحد 2
    CameraWatch::StrangerAlerts strangers;
    strangers.dailyCount = 2;
    strangers.night = false;
    CameraWatch::setStrangerAlerts(strangers);
    std::mt19937 random(11);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::vector<float> print(static_cast<size_t>(FaceEngine::EmbeddingSize));
    for (float &value : print)
        value = gauss(random);
    FaceEngine::normalize(print);
    const auto stranger = [&] {
        CameraWatch::Face face;
        face.box = QRectF(20, 20, 40, 40);
        face.match.status = MatchStatus::Unknown;
        face.embedding = print;
        face.quality = 0.8f;
        face.crop = crop;
        return face;
    };
    int strangerAlerts = 0;
    for (int camera = 0; camera < 2; ++camera) {
        CameraWatch visitorWatch(&database);
        visitorWatch.reset(QStringLiteral("camera:rules-visitor-%1").arg(camera), QStringLiteral("زوار"), nullptr);
        visitorWatch.setThreshold(0.5f);
        QObject::connect(&visitorWatch, &CameraWatch::strangerAlert, [&strangerAlerts] { ++strangerAlerts; });
        visitorWatch.process(frame, {stranger()});
        visitorWatch.process(frame, {stranger()});
    }
    out << "stranger_alerts=" << strangerAlerts << "\n";
    check("stranger_alert", strangerAlerts == 1);

    // 6) حذف ظهور الكاميرات القديم: ظهور صورة أو فيديو يبقى
    database.addSighting(watchedId, QDir(directory).filePath(QStringLiteral("photo.jpg")), -1, 0.7f, crop);
    const qsizetype before = database.sightings(watchedId, 100).size();
    const int removedSightings = database.removeCameraSightingsBefore(QDateTime::currentDateTime().addSecs(60));
    const QList<Sighting> remaining = database.sightings(watchedId, 100);
    out << "sightings_before=" << before << "\nsightings_removed=" << removedSightings << "\n";
    check("sighting_retention", before == 2 && removedSightings >= 3 && remaining.size() == 1 && remaining.first().camera.isEmpty()
                                    && database.visitorSightingCount(database.visitors(QDateTime(), 1, 10).value(0).id, QDateTime()) == 0);

    // 7) لقطات الظهور: مجلد يوم قديم يُحذف، ومجلد اليوم وملفات المستخدم تبقى
    const QDir snapshots(CameraRecorder::snapshotsFolder());
    QDir().mkpath(snapshots.filePath(QStringLiteral("2001-02-03")));
    QDir().mkpath(snapshots.filePath(QStringLiteral("ملاحظات")));
    for (const QString &name : {QStringLiteral("2001-02-03/old.jpg"), QStringLiteral("ملاحظات/keep.jpg")}) {
        QFile file(snapshots.filePath(name));
        file.open(QIODevice::WriteOnly);
        file.write("x");
    }
    QThreadPool::globalInstance()->waitForDone(); // لقطات CameraWatch تُحفظ في الخلفية
    const int todaySnapshots = int(QDir(snapshots.filePath(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))))
                                       .entryList({QStringLiteral("*.jpg")}, QDir::Files)
                                       .size());
    const int removedSnapshots = CameraRecorder::removeExpiredSnapshots(QDate::currentDate().addDays(-7));
    out << "snapshots_today=" << todaySnapshots << "\nsnapshots_removed=" << removedSnapshots << "\n";
    check("snapshot_retention", removedSnapshots == 1 && !snapshots.exists(QStringLiteral("2001-02-03"))
                                    && QFile::exists(snapshots.filePath(QStringLiteral("ملاحظات/keep.jpg"))) && todaySnapshots >= 1);

    // 8) النسخ الاحتياطي التلقائي: نسخة صالحة ويبقى آخر keep فقط
    const QString backups = QDir(directory).filePath(QStringLiteral("backups"));
    QDir().mkpath(backups);
    for (const QString &name : {QStringLiteral("ملامح-تلقائي-2001-01-01 00-00-00.malamih"), QStringLiteral("ملامح-تلقائي-2001-01-02 00-00-00.malamih")}) {
        QFile file(QDir(backups).filePath(name));
        file.open(QIODevice::WriteOnly);
        file.write("old");
    }
    QString backupPath;
    QString backupError;
    const bool backedUp = Maintenance::backupNow(database.databaseFile(), backups, 2, &backupPath, &backupError);
    const QStringList kept = Maintenance::autoBackups(backups);
    const QPair<qint64, qint64> counts = FaceDatabase::inspectBackup(backupPath);
    out << "backup_files=" << kept.size() << "\nbackup_persons=" << counts.first << "\n" << backupError;
    check("auto_backup", backedUp && kept.size() == 2 && kept.first() == backupPath && counts.first == database.personCount()
                             && kept.last().contains(QStringLiteral("2001-01-02")));

    // 9) أصوات التنبيه ملفات WAV سليمة
    const QByteArray chime = AlertSound::wav(AlertSound::Sound::Chime);
    const QByteArray alarm = AlertSound::wav(AlertSound::Sound::Alarm);
    check("sounds", chime.startsWith("RIFF") && chime.mid(8, 4) == "WAVE" && chime.size() > 10000 && alarm.size() > chime.size());

    // 10) مقطع حول لحظة الظهور: 1 ثانية قبلها و1 بعدها من فيديو مدته 6 ثوانٍ
    const QString video = QDir(directory).filePath(QStringLiteral("recording.avi"));
    {
        cv::VideoWriter writer(QFile::encodeName(video).toStdString(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 10.0, cv::Size(160, 120));
        for (int i = 0; i < 60; ++i) {
            cv::Mat image(120, 160, CV_8UC3, cv::Scalar(i * 4, 80, 160));
            writer.write(image);
        }
    }
    const QString clip = QDir(directory).filePath(QStringLiteral("clip.mp4"));
    const QString clipError = ClipExport::exportClip(video, 3000, clip, 1000, 1000);
    cv::VideoCapture capture(QFile::encodeName(clip).toStdString());
    const double clipFrames = capture.isOpened() ? capture.get(cv::CAP_PROP_FRAME_COUNT) : 0;
    capture.release();
    const QString missingError = ClipExport::exportClip(QDir(directory).filePath(QStringLiteral("missing.mp4")), 0, clip + QStringLiteral(".x"));
    out << "clip_frames=" << clipFrames << "\n" << clipError;
    check("clip", clipError.isEmpty() && clipFrames >= 15 && clipFrames <= 25 && !missingError.isEmpty());

    CameraRecorder::setSavedFolder(previousFolder);
    CameraWatch::setVisitorsEnabled(previousVisitors);
    CameraWatch::setStrangerAlerts(previousStrangers);
    return allOk ? 0 : 1;
}
} // namespace

// قياس سرعة ثابت الصيغة للمقارنة بين النسخ: الكشف والبصمة لإطار، ثم أول ثوانٍ من فيديو، ثم نقل المحرك
// بين جهازين والتحليل جارٍ في خيط آخر (يجب ألا ينهار ولا يتوقف التحليل)
int bench(const QString &videoPath, int seconds, QTextStream &out)
{
    FaceEngine engine;
    if (!loadEngine(engine, out))
        return 1;
    out << "version=" << QCoreApplication::applicationVersion() << "\n";
    out << "device=" << (engine.gpuName().isEmpty() ? QStringLiteral("cpu") : engine.gpuName()) << "\n";
    out << "gpu_identity=" << FaceEngine::gpuIdentity() << "\n";
    out << "threads=" << Performance::threads(Performance::saved()) << "\n";

    cv::Mat frame;
    cv::VideoCapture capture;
    const bool hasVideo = videoPath != QLatin1String("-") && ImageIO::openVideo(capture, videoPath) && capture.read(frame);
    if (!hasVideo) {
        frame = cv::Mat(480, 640, CV_8UC3, cv::Scalar(70, 90, 110));
        cv::randn(frame, cv::Scalar::all(110), cv::Scalar::all(40));
    }
    const auto measure = [](auto &&work, int repeats) {
        work();
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < repeats; ++i)
            work();
        return timer.nsecsElapsed() / 1e6 / repeats;
    };
    out << "frame=" << frame.cols << "x" << frame.rows << "\n";
    out << "detect_ms=" << measure([&] { engine.detect(frame, false); }, 10) << "\n";
    const cv::Mat aligned(112, 112, CV_8UC3, cv::Scalar(90, 120, 160));
    out << "embed_ms=" << measure([&] { engine.embed(aligned); }, 10) << "\n";
    out << "embed_many_ms=" << measure([&] { engine.embedMany({aligned}); }, 10) << "\n";
    out << "analyze_ms=" << measure([&] { engine.analyze(frame, false); }, 5) << "\n";

    int analyzed = 0;
    double videoSeconds = 0.0;
    if (hasVideo) {
        VideoWorker worker(&engine);
        worker.setFrameStep(0);
        worker.setRange(0, qint64(seconds) * 1000);
        QObject::connect(&worker, &VideoWorker::frameReady, [&](const QImage &, const QList<FaceItem> &, qint64, double) {
            ++analyzed;
            worker.frameShown();
        });
        QElapsedTimer timer;
        timer.start();
        worker.process(videoPath);
        videoSeconds = std::max(0.001, timer.elapsed() / 1000.0);
        out << "video_seconds=" << seconds << "\n";
        out << "video_elapsed_s=" << videoSeconds << "\n";
        out << "video_analyzed_frames=" << analyzed << "\n";
        out << "video_analyzed_per_second=" << analyzed / videoSeconds << "\n";
        out << "video_realtime_factor=" << seconds / videoSeconds << "\n";
    }

    // نقل المحرك والتحليل جارٍ: ثلاث مرات إلى المعالج نفسه (يكفي لاختبار القفل على أي جهاز)
    std::atomic<bool> running{true};
    std::atomic<int> during{0};
    std::thread analysis([&] {
        while (running.load()) {
            engine.detect(frame, false);
            ++during;
        }
    });
    bool switched = true;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 3; ++i)
        switched = engine.switchDevice(FaceEngine::Device::Cpu) && switched;
    const double switchMs = timer.elapsed() / 3.0;
    const int before = during.load();
    while (during.load() < before + 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    running.store(false);
    analysis.join();
    out << "switch_ok=" << switched << "\n";
    out << "switch_ms=" << switchMs << "\n";
    out << "detect_during_switch=" << during.load() << "\n";
    out << "peak_memory_mb=" << peakMemoryMb() << "\n";
    return switched && (!hasVideo || analyzed > 0) ? 0 : 1;
}

int run(const QStringList &args)
{
    if (args.size() < 2)
        return -1;
    const QString command = args.at(1);
    if (command != QLatin1String("--self-test") && command != QLatin1String("--video-test")
        && command != QLatin1String("--db-test") && command != QLatin1String("--enhance-test")
        && command != QLatin1String("--embed-dump") && command != QLatin1String("--stress-test")
        && command != QLatin1String("--pair-matrix") && command != QLatin1String("--report-test")
        && command != QLatin1String("--backup-test") && command != QLatin1String("--filters-test")
        && command != QLatin1String("--camera-test") && command != QLatin1String("--record-test")
        && command != QLatin1String("--watch-test") && command != QLatin1String("--daily-report-test")
        && command != QLatin1String("--gpu-test") && command != QLatin1String("--rules-test")
        && command != QLatin1String("--bench") && command != QLatin1String("--player-test"))
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
    // Malamih --player-test <فيديو> <ثوانٍ> <ملف النتائج>
    if (command == QLatin1String("--player-test") && args.size() == 5)
        return playerTest(args.at(2), args.at(3).toInt(), out);
    // Malamih --embed-dump <فيديو> <تحليل إطار من كل> <ملف النتائج>
    if (command == QLatin1String("--embed-dump") && args.size() == 5)
        return embedDump(args.at(2), args.at(3).toInt(), out);
    // Malamih --db-test <مجلد> <عدد الأشخاص> <صور لكل شخص> <ملف النتائج>
    if (command == QLatin1String("--db-test") && args.size() == 6)
        return databaseTest(args.at(2), args.at(3).toInt(), args.at(4).toInt(), out);
    // Malamih --enhance-test <صورة مرجعية> <صورة رديئة> <حفظ المحسّنة> <ملف النتائج>
    if (command == QLatin1String("--enhance-test") && args.size() == 6)
        return enhanceTest(args.at(2), args.at(3), args.at(4), out);
    // Malamih --stress-test <مجلد التسجيل> <مجلد صور الاختبار> <ملف النتائج>
    if (command == QLatin1String("--stress-test") && args.size() == 5)
        return stressTest(args.at(2), args.at(3), out);
    // Malamih --pair-matrix <مجلد صور> <ملف النتائج>
    // Malamih --backup-test <مجلد مؤقت> <ملف النتائج>
    if (command == QLatin1String("--backup-test") && args.size() == 4)
        return backupTest(args.at(2), out);
    // Malamih --filters-test <صورة وجه> <مجلد مؤقت> <ملف النتائج>
    if (command == QLatin1String("--filters-test") && args.size() == 5)
        return filtersTest(args.at(2), args.at(3), out);
    // Malamih --camera-test <فيديو يُبث كأنه كاميرا شبكة، أو -> <ملف النتائج>
    if (command == QLatin1String("--camera-test") && args.size() == 4)
        return cameraTest(args.at(2), out);
    // Malamih --record-test <مجلد مؤقت> <ملف النتائج>
    if (command == QLatin1String("--record-test") && args.size() == 4)
        return recordTest(args.at(2), out);
    // Malamih --gpu-test <صورة فيها وجوه أو -> <ملف النتائج>
    if (command == QLatin1String("--gpu-test") && args.size() == 4)
        return gpuTest(args.at(2), out);
    // Malamih --daily-report-test <مجلد مؤقت> <ملف النتائج>
    if (command == QLatin1String("--daily-report-test") && args.size() == 4)
        return dailyReportTest(args.at(2), out);
    // Malamih --rules-test <مجلد مؤقت> <ملف النتائج>
    if (command == QLatin1String("--rules-test") && args.size() == 4)
        return rulesTest(args.at(2), out);
    // Malamih --watch-test <مجلد مؤقت> <ملف النتائج>
    if (command == QLatin1String("--watch-test") && args.size() == 4)
        return watchTest(args.at(2), out);
    // Malamih --bench <فيديو أو -> [ثوانٍ من الفيديو، افتراضياً 20] <ملف النتائج>
    if (command == QLatin1String("--bench") && (args.size() == 4 || args.size() == 5))
        return bench(args.at(2), args.size() == 5 ? std::max(1, args.at(3).toInt()) : 20, out);
    // Malamih --report-test <صورة> <تقرير.html> <ملف النتائج>
    if (command == QLatin1String("--report-test") && args.size() == 5)
        return reportTest(args.at(2), args.at(3), out);
    if (command == QLatin1String("--pair-matrix") && args.size() == 4)
        return pairMatrix(args.at(2), out);
    out << "error=invalid arguments\n";
    return 2;
}

} // namespace Diagnostics

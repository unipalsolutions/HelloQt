#include "videoworker.h"

#include "faceengine.h"
#include "imageio.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace {

constexpr int MaxTrackGap = 15;           // إطارات يبقى فيها المسار مفتوحاً دون ظهور الوجه
constexpr int UnconfirmedLifetime = 150;  // كشف لم يُعتمد يُنسى بعد هذا الغياب
constexpr int MinTrackHits = 3;           // لا يُعتمد الوجه قبل ظهوره في عدة إطارات متقاربة
constexpr int EmbedInterval = 5;          // إعادة حساب بصمة الوجه المتتبَّع كل عدة إطارات فقط
constexpr float BetterShotMargin = 0.08f; // لقطة أوضح من أفضل لقطة سابقة تُحلَّل فوراً
// وجه في موضع وجه سابق لكن بصمته بعيدة عنه: شخص آخر (تبدّل اللقطة في حوار مثلاً) فلا يُلحق بمساره
constexpr float TrackBreakSimilarity = 0.22f;
// وجه جديد يُربط بهوية سابقة إذا شابهها بهذا القدر؛ الحد أقل للوجوه الرديئة لأن تشابه
// لقطات الشخص نفسه ينخفض كثيراً مع رداءة الصورة بينما يبقى تشابه الأشخاص المختلفين منخفضاً
constexpr float ReidentifyGood = 0.42f;
constexpr float ReidentifyPoor = 0.32f;
// هويتان لم تظهرا معاً في إطار واحد أبداً وتشابه متوسطاهما بهذا القدر = شخص واحد
constexpr float MergeSimilarity = 0.30f;
constexpr int MergeInterval = 25;            // إطارات محللة بين محاولتي دمج
constexpr int MaxExemplars = 8;              // لقطات مختلفة محفوظة لكل شخص (زوايا وإضاءات)
constexpr float ExemplarRedundancy = 0.9f;   // لقطة بهذا التشابه مع محفوظة لا تضيف جديداً
constexpr qint64 AppearanceGapMs = 2000;     // غياب أطول من هذا يبدأ فترة ظهور جديدة
constexpr int MaxAppearances = 2000;
constexpr float ReidentifyDrift = 0.985f; // تغيّر بصمة الشخص الذي يستدعي بحثاً جديداً في القاعدة
constexpr int UpdateInterval = 10;        // أقصى عدد إطارات بين تحديثين لبطاقة الوجه
constexpr int MaxPendingFrames = 2;
constexpr int MaxDisplaySide = 1920;      // الإطارات الأكبر تُصغَّر للعرض فقط
constexpr int TrackPhotoSize = 192;
constexpr double AutoAnalysisRate = 10.0; // إطارات محللة لكل ثانية من الفيديو في الوضع التلقائي
constexpr double IdleAnalysisRate = 2.0;  // عند خلو المشهد من الوجوه يُفحص أقل، ويعود للمعدل الكامل فور ظهور وجه
constexpr int IdleAfter = 20;             // إطارات محللة متتالية بلا وجوه قبل خفض المعدل

struct Exemplar
{
    std::vector<float> embedding;
    float quality = 0.0f;
};

// شخص ظهر في الفيديو، وقد يعود بعد غياب طويل فيُربط بالهوية نفسها
struct Identity
{
    int id = 0;
    std::vector<float> sum;  // مجموع البصمات مرجّحاً بالجودة
    std::vector<float> mean; // المجموع بعد التطبيع
    std::vector<Exemplar> exemplars;
    std::vector<std::pair<qint64, qint64>> appearances;
    int frames = 0;
    int lastSeen = 0;
    qint64 firstSeenMs = -1;
    qint64 lastSeenMs = -1;
    bool confirmed = false;
    bool mergeChecked = false; // لم تتغير منذ آخر محاولة دمج
    FaceItem best; // أوضح لقطة، مضغوطة في الذاكرة
    qint64 bestMs = -1;
    QRectF sourceBox;
    MatchResult match;
    std::vector<float> identifiedMean; // البصمة عند آخر بحث في القاعدة
    bool matchStale = true;
    bool forceIdentify = true;
    bool changed = false;
    int lastEmitted = INT_MIN / 2;
};

// ظهور متصل لهوية في إطارات متقاربة من اللقطة نفسها
struct Track
{
    int identity = 0;
    QRectF box;
    int firstSeen = 0;
    int lastSeen = 0;
    int hits = 0;
    int lastEmbedded = INT_MIN / 2;
    bool closed = false;
    std::vector<float> embedding; // آخر بصمة تحققنا أنها للشخص نفسه
};

struct Detection
{
    DetectedFace face;
    cv::Mat aligned;
    float quality = 0.0f;
    int track = -1;
    float iou = 0.0f;
    bool needsEmbedding = false;
    std::vector<float> embedding;
};

float overlap(const QRectF &a, const QRectF &b)
{
    const QRectF inter = a.intersected(b);
    const double interArea = inter.width() * inter.height();
    const double total = a.width() * a.height() + b.width() * b.height() - interArea;
    return total > 0.0 ? float(interArea / total) : 0.0f;
}

QRectF toRect(const cv::Rect2f &box)
{
    return QRectF(box.x, box.y, box.width, box.height);
}

float similarity(const Identity &identity, const std::vector<float> &embedding)
{
    float best = identity.mean.empty() ? -1.0f : FaceEngine::cosine(identity.mean, embedding);
    for (const Exemplar &exemplar : identity.exemplars)
        best = std::max(best, FaceEngine::cosine(exemplar.embedding, embedding));
    return best;
}

float reidentifyThreshold(float quality)
{
    const float t = std::clamp((quality - 0.35f) / 0.3f, 0.0f, 1.0f);
    return ReidentifyPoor + t * (ReidentifyGood - ReidentifyPoor);
}

// يحتفظ بلقطات متنوعة: الشبيهة بلقطة محفوظة تحل محلها إن كانت أوضح، والجديدة تُضاف أو تحل محل الأضعف
void addExemplar(Identity &identity, const std::vector<float> &embedding, float quality)
{
    int closest = -1;
    float closestSimilarity = ExemplarRedundancy;
    for (size_t i = 0; i < identity.exemplars.size(); ++i) {
        const float s = FaceEngine::cosine(identity.exemplars[i].embedding, embedding);
        if (s > closestSimilarity) {
            closestSimilarity = s;
            closest = int(i);
        }
    }
    if (closest >= 0) {
        if (quality > identity.exemplars[size_t(closest)].quality)
            identity.exemplars[size_t(closest)] = {embedding, quality};
        return;
    }
    if (identity.exemplars.size() < MaxExemplars) {
        identity.exemplars.push_back({embedding, quality});
        return;
    }
    auto weakest = std::min_element(identity.exemplars.begin(), identity.exemplars.end(),
                                    [](const Exemplar &a, const Exemplar &b) { return a.quality < b.quality; });
    if (quality > weakest->quality)
        *weakest = {embedding, quality};
}

void addAppearance(Identity &identity, qint64 ms)
{
    auto &list = identity.appearances;
    if (!list.empty() && ms >= list.back().first && ms - list.back().second <= AppearanceGapMs) {
        list.back().second = std::max(list.back().second, ms);
        return;
    }
    if (list.size() >= MaxAppearances) {
        // الفترات الكثيرة جداً تُضم المتجاورة منها بدل فقدان آخرها
        list[list.size() - 2].second = list.back().second;
        list.pop_back();
    }
    list.push_back({ms, ms});
}

void mergeAppearances(std::vector<std::pair<qint64, qint64>> &list)
{
    std::sort(list.begin(), list.end());
    std::vector<std::pair<qint64, qint64>> merged;
    for (const auto &range : list) {
        if (!merged.empty() && range.first - merged.back().second <= AppearanceGapMs)
            merged.back().second = std::max(merged.back().second, range.second);
        else
            merged.push_back(range);
    }
    list = std::move(merged);
}

quint64 pairKey(int a, int b)
{
    return (quint64(quint32(std::min(a, b))) << 32) | quint32(std::max(a, b));
}

// تبدّل اللقطة (قطع المشهد) يُعرف من تغيّر توزيع الألوان والإضاءة فجأة بين إطارين محللين
class SceneCutDetector
{
public:
    bool update(const cv::Mat &frame)
    {
        cv::Mat small;
        cv::resize(frame, small, cv::Size(64, 36), 0, 0, cv::INTER_AREA);
        cv::Mat hsv;
        cv::cvtColor(small, hsv, cv::COLOR_BGR2HSV);
        const int channels[] = {0, 1};
        const int sizes[] = {16, 8};
        const float hueRange[] = {0.0f, 180.0f};
        const float satRange[] = {0.0f, 256.0f};
        const float *ranges[] = {hueRange, satRange};
        cv::Mat hist;
        cv::calcHist(&hsv, 1, channels, cv::Mat(), hist, 2, sizes, ranges);
        cv::normalize(hist, hist, 1.0, 0.0, cv::NORM_L1);
        cv::Mat gray;
        cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);

        bool cut = false;
        if (!m_hist.empty()) {
            const double colors = cv::compareHist(m_hist, hist, cv::HISTCMP_BHATTACHARYYA);
            const double pixels = cv::norm(gray, m_gray, cv::NORM_L1) / double(gray.total());
            cut = colors > 0.5 || (colors > 0.3 && pixels > 35.0);
        }
        m_hist = hist;
        m_gray = gray;
        return cut;
    }

private:
    cv::Mat m_hist;
    cv::Mat m_gray;
};

} // namespace

VideoWorker::VideoWorker(const FaceEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
}

void VideoWorker::setGallery(GalleryPtr gallery)
{
    QMutexLocker locker(&m_mutex);
    m_gallery = std::move(gallery);
}

void VideoWorker::setThreshold(float threshold)
{
    QMutexLocker locker(&m_mutex);
    m_threshold = threshold;
}

void VideoWorker::setFrameStep(int step)
{
    m_frameStep = std::max(0, step);
}

void VideoWorker::setRange(qint64 startMs, qint64 endMs)
{
    QMutexLocker locker(&m_mutex);
    m_startMs = std::max<qint64>(0, startMs);
    m_endMs = endMs > m_startMs ? endMs : 0;
}

void VideoWorker::setRegion(const QRectF &region)
{
    QMutexLocker locker(&m_mutex);
    m_region = region.normalized().intersected(QRectF(0, 0, 1, 1));
}

void VideoWorker::setEnhance(bool enhance)
{
    QMutexLocker locker(&m_mutex);
    m_enhance = enhance;
}

void VideoWorker::mergeIdentities(int keepId, int removedId)
{
    QMutexLocker locker(&m_mutex);
    m_manualMerges.append({keepId, removedId});
}

void VideoWorker::requestStop()
{
    m_stop = true;
}

void VideoWorker::setPaused(bool paused)
{
    m_paused = paused;
}

void VideoWorker::frameShown()
{
    --m_pendingFrames;
}

void VideoWorker::process(const QString &source)
{
    m_stop = false;
    m_paused = false;
    m_pendingFrames = 0;
    {
        QMutexLocker locker(&m_mutex);
        m_manualMerges.clear();
    }

    cv::VideoCapture capture;
    const bool camera = source.startsWith(QStringLiteral("camera:"));
    if (camera) {
        capture.open(source.mid(7).toInt());
        // أحدث إطار فقط، حتى لا يتأخر العرض عن الكاميرا عندما يكون التحليل أبطأ منها
        capture.set(cv::CAP_PROP_BUFFERSIZE, 1);
    } else {
        ImageIO::openVideo(capture, source);
    }
    if (!capture.isOpened()) {
        emit finished(camera ? QStringLiteral("تعذر فتح الكاميرا") : QStringLiteral("تعذر فتح ملف الفيديو"));
        return;
    }

    const int frameCount = camera ? 0 : std::max(0, int(capture.get(cv::CAP_PROP_FRAME_COUNT)));
    const double fps = capture.get(cv::CAP_PROP_FPS);
    const bool validFps = fps > 1.0 && fps < 1000.0;
    const double rate = validFps ? fps : 25.0;
    const bool automatic = !camera && m_frameStep.load() <= 0;
    int step = camera ? 1 : m_frameStep.load();
    if (step <= 0)
        step = std::max(1, int(std::lround(rate / AutoAnalysisRate)));
    const int idleStep = std::max(step, int(std::lround(rate / IdleAnalysisRate)));
    int currentStep = step;
    int skip = 0;
    int idleAnalyses = 0;
    QElapsedTimer cameraClock;
    cameraClock.start();

    // الجزء المطلوب من الفيديو: القفز إلى بدايته مباشرة دون قراءة ما قبلها
    qint64 startMs = 0;
    qint64 endMs = 0;
    {
        QMutexLocker locker(&m_mutex);
        startMs = camera ? 0 : m_startMs;
        endMs = camera ? 0 : m_endMs;
    }
    int frameIndex = 0;
    if (startMs > 0) {
        capture.set(cv::CAP_PROP_POS_MSEC, double(startMs));
        frameIndex = std::max(0, int(std::lround(capture.get(cv::CAP_PROP_POS_FRAMES))));
        if (frameIndex == 0 && validFps)
            frameIndex = int(startMs * fps / 1000.0);
    }
    const int startFrame = frameIndex;
    int endFrame = frameCount;
    if (endMs > 0 && validFps)
        endFrame = std::min(frameCount > 0 ? frameCount : INT_MAX, int(std::ceil(endMs * fps / 1000.0)));
    const qint64 durationMs = validFps && frameCount > 0 ? qint64(frameCount * 1000.0 / fps) : 0;
    emit started(step, validFps ? fps : 0.0, durationMs);

    std::map<int, Identity> identities;
    std::vector<Track> tracks;
    std::map<int, int> redirect; // هويات دُمجت في إطار التحليل الحالي
    std::set<quint64> together; // أزواج هويات ظهرت معاً في إطار واحد، فهما شخصان مختلفان حتماً
    SceneCutDetector sceneCuts;
    int nextId = 1;
    int processed = 0;
    double workScale = 1.0;
    cv::Mat frame;

    GalleryPtr gallery;
    quint64 revision = 0;
    float threshold = 0.0f;
    auto emitUpdate = [&](Identity &identity) {
        // البحث في قاعدة كبيرة يُعاد فقط إذا تغيّرت بصمة الشخص فعلاً أو تغيّرت القاعدة أو حد التعرف
        if (identity.matchStale) {
            const bool moved = identity.identifiedMean.empty()
                || FaceEngine::cosine(identity.identifiedMean, identity.mean) < ReidentifyDrift;
            if (identity.forceIdentify || moved) {
                identity.match = gallery ? FaceDatabase::identify(*gallery, identity.mean, threshold) : MatchResult();
                identity.identifiedMean = identity.mean;
                identity.forceIdentify = false;
            }
            identity.matchStale = false;
        }
        FaceItem summary = identity.best;
        summary.id = identity.id;
        summary.frames = identity.frames;
        summary.firstSeenMs = identity.firstSeenMs;
        summary.lastSeenMs = identity.lastSeenMs;
        summary.bestMs = identity.bestMs;
        summary.sourceBox = identity.sourceBox;
        summary.appearances.reserve(qsizetype(identity.appearances.size()));
        for (const auto &range : identity.appearances)
            summary.appearances.append({range.first, range.second});
        summary.embedding = identity.mean;
        summary.match = identity.match;
        identity.changed = false;
        identity.lastEmitted = processed;
        emit trackUpdated(summary);
    };

    // يضم هوية إلى أخرى: بصماتها ولقطاتها وفترات ظهورها، وتُبلَّغ الواجهة لتحذف البطاقة المكررة
    auto merge = [&](int keepId, int removedId) {
        auto keepIt = identities.find(keepId);
        auto removedIt = identities.find(removedId);
        if (keepId == removedId || keepIt == identities.end() || removedIt == identities.end())
            return;
        Identity &keep = keepIt->second;
        Identity &gone = removedIt->second;
        for (size_t i = 0; i < keep.sum.size() && i < gone.sum.size(); ++i)
            keep.sum[i] += gone.sum[i];
        keep.mean = keep.sum;
        FaceEngine::normalize(keep.mean);
        for (const Exemplar &exemplar : gone.exemplars)
            addExemplar(keep, exemplar.embedding, exemplar.quality);
        keep.frames += gone.frames;
        keep.lastSeen = std::max(keep.lastSeen, gone.lastSeen);
        if (gone.firstSeenMs >= 0 && (keep.firstSeenMs < 0 || gone.firstSeenMs < keep.firstSeenMs))
            keep.firstSeenMs = gone.firstSeenMs;
        keep.lastSeenMs = std::max(keep.lastSeenMs, gone.lastSeenMs);
        keep.appearances.insert(keep.appearances.end(), gone.appearances.begin(), gone.appearances.end());
        mergeAppearances(keep.appearances);
        if (!gone.best.packedCrop.isEmpty() && (keep.best.packedCrop.isEmpty() || gone.best.quality > keep.best.quality)) {
            keep.best = gone.best;
            keep.best.id = keep.id;
            keep.bestMs = gone.bestMs;
            keep.sourceBox = gone.sourceBox;
        }
        keep.confirmed = keep.confirmed || gone.confirmed;
        keep.matchStale = keep.forceIdentify = keep.changed = true;
        keep.mergeChecked = false;
        for (Track &track : tracks) {
            if (track.identity == removedId)
                track.identity = keepId;
        }
        std::vector<quint64> moved;
        for (quint64 key : together) {
            const int a = int(key >> 32);
            const int b = int(key & 0xffffffffu);
            if (a == removedId || b == removedId)
                moved.push_back(key);
        }
        for (quint64 key : moved) {
            together.erase(key);
            const int a = int(key >> 32);
            const int b = int(key & 0xffffffffu);
            const int other = a == removedId ? b : a;
            if (other != keepId)
                together.insert(pairKey(keepId, other));
        }
        const bool shown = gone.lastEmitted > INT_MIN / 2;
        identities.erase(removedIt);
        redirect[removedId] = keepId;
        if (shown)
            emit identitiesMerged(keepId, removedId);
        if (keep.confirmed)
            emitUpdate(keep);
    };

    // الدمج التلقائي: هويتان لم تظهرا معاً قط، وبصمتاهما متقاربتان أو طابقتا الشخص نفسه في القاعدة
    auto mergeDuplicates = [&] {
        struct Candidate { float score; int a; int b; };
        std::vector<Candidate> candidates;
        for (auto &[idA, a] : identities) {
            if (!a.confirmed || a.mergeChecked)
                continue;
            a.mergeChecked = true;
            for (const auto &[idB, b] : identities) {
                if (idB == idA || !b.confirmed || together.count(pairKey(idA, idB)))
                    continue;
                const float s = FaceEngine::cosine(a.mean, b.mean);
                const bool samePerson = a.match.status == MatchStatus::Confirmed && b.match.status == MatchStatus::Confirmed
                    && a.match.personId == b.match.personId && s > 0.2f;
                if (s >= MergeSimilarity || samePerson)
                    candidates.push_back({samePerson ? 1.0f + s : s, idA, idB});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &x, const Candidate &y) { return x.score > y.score; });
        for (const Candidate &c : candidates) {
            auto a = identities.find(c.a);
            auto b = identities.find(c.b);
            if (a == identities.end() || b == identities.end() || together.count(pairKey(c.a, c.b)))
                continue;
            // تبقى الهوية الأقدم ظهوراً في الواجهة حتى لا تتبدل البطاقات
            const bool keepA = a->second.lastEmitted > INT_MIN / 2 && (b->second.lastEmitted <= INT_MIN / 2 || c.a < c.b);
            merge(keepA ? c.a : c.b, keepA ? c.b : c.a);
        }
    };

    while (!m_stop) {
        while (m_paused && !m_stop)
            QThread::msleep(25);
        if (!camera && endFrame > startFrame && frameIndex >= endFrame)
            break;
        // الإطارات المتخطاة تُقرأ دون تحويلها إلى صورة، وهذا أسرع بكثير
        if (!camera && skip > 0) {
            if (!capture.grab())
                break;
            ++frameIndex;
            --skip;
            continue;
        }
        if (!capture.read(frame))
            break;
        ++frameIndex;
        skip = currentStep - 1;
        if (frame.empty())
            continue;
        ++processed;
        const qint64 positionMs = camera ? cameraClock.elapsed()
            : validFps ? qint64(double(frameIndex - 1) * 1000.0 / fps)
                       : qint64(capture.get(cv::CAP_PROP_POS_MSEC));

        QRectF region;
        bool enhance = false;
        QList<QPair<int, int>> manualMerges;
        {
            QMutexLocker locker(&m_mutex);
            const quint64 currentRevision = m_gallery ? FaceDatabase::revision(*m_gallery) : 0;
            if (gallery != m_gallery || revision != currentRevision || threshold != m_threshold) {
                for (auto &[id, identity] : identities)
                    identity.matchStale = identity.changed = identity.forceIdentify = true;
            }
            gallery = m_gallery;
            revision = currentRevision;
            threshold = m_threshold;
            region = m_region;
            enhance = m_enhance;
            manualMerges.swap(m_manualMerges);
        }
        for (const auto &[keepId, removedId] : manualMerges) {
            merge(keepId, removedId);
            together.erase(pairKey(keepId, removedId));
        }

        // الإطار المحسّن (إن طُلب) هو الذي يُحلَّل ويُعرض، وقد يكون أكبر من الأصلي
        const cv::Mat work = enhance ? ImageIO::enhance(frame) : frame;
        const double scale = double(work.cols) / double(frame.cols);
        if (scale != workScale) {
            tracks.clear(); // تغيّر مقياس الإحداثيات
            workScale = scale;
        }
        if (sceneCuts.update(frame))
            tracks.clear(); // لقطة جديدة: لا يُفترض أن الوجه في الموضع نفسه هو الشخص نفسه

        // 1) كشف الوجوه داخل منطقة التحليل فقط، وربطها بالمسارات المفتوحة حسب تداخل الموضع
        cv::Rect area(0, 0, work.cols, work.rows);
        if (!region.isEmpty()) {
            const cv::Rect wanted(int(region.x() * work.cols), int(region.y() * work.rows),
                                  int(std::ceil(region.width() * work.cols)), int(std::ceil(region.height() * work.rows)));
            const cv::Rect clipped = wanted & area;
            if (clipped.width >= 24 && clipped.height >= 24)
                area = clipped;
        }
        const bool cropped = area.width != work.cols || area.height != work.rows;
        std::vector<Detection> detections;
        for (DetectedFace face : m_engine->detect(cropped ? work(area) : work, false)) {
            face.box.x += float(area.x);
            face.box.y += float(area.y);
            for (cv::Point2f &point : face.landmarks)
                point += cv::Point2f(float(area.x), float(area.y));
            Detection detection;
            detection.face = face;
            detection.aligned = m_engine->align(work, face);
            detection.quality = m_engine->quality(face, detection.aligned);
            detections.push_back(std::move(detection));
        }
        // في الوضع التلقائي: مشهد بلا وجوه لفترة يُفحص بمعدل أقل حتى يظهر وجه
        idleAnalyses = detections.empty() ? idleAnalyses + 1 : 0;
        if (automatic) {
            const int wanted = idleAnalyses >= IdleAfter ? idleStep : step;
            if (wanted != currentStep) {
                currentStep = wanted;
                skip = std::min(skip, currentStep - 1);
            }
        }

        struct Pair { float iou; size_t detection; size_t track; };
        std::vector<Pair> pairs;
        for (size_t d = 0; d < detections.size(); ++d) {
            for (size_t t = 0; t < tracks.size(); ++t) {
                const float o = overlap(tracks[t].box, toRect(detections[d].face.box));
                if (o > 0.3f)
                    pairs.push_back({o, d, t});
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) { return a.iou > b.iou; });
        std::vector<bool> trackUsed(tracks.size(), false);
        for (const Pair &pair : pairs) {
            if (detections[pair.detection].track >= 0 || trackUsed[pair.track])
                continue;
            detections[pair.detection].track = int(pair.track);
            detections[pair.detection].iou = pair.iou;
            trackUsed[pair.track] = true;
        }

        // 2) البصمة تُحسب للوجوه الجديدة، وللمتتبَّعة كل عدة إطارات، أو عند ربط غير مؤكد
        //    (مسار حديث، تداخل ضعيف، عودة بعد غياب) أو ظهور لقطة أوضح
        std::vector<cv::Mat> toEmbed;
        for (Detection &detection : detections) {
            if (detection.track < 0) {
                detection.needsEmbedding = true;
            } else {
                const Track &track = tracks[size_t(detection.track)];
                const Identity &identity = identities[track.identity];
                detection.needsEmbedding = track.embedding.empty() || track.hits < MinTrackHits
                    || detection.iou < 0.5f || processed - track.lastSeen > 1
                    || processed - track.lastEmbedded >= EmbedInterval
                    || detection.quality > identity.best.quality + BetterShotMargin;
            }
            if (detection.needsEmbedding)
                toEmbed.push_back(detection.aligned);
        }
        const std::vector<std::vector<float>> embeddings = m_engine->embedMany(toEmbed);
        for (size_t d = 0, e = 0; d < detections.size() && e < embeddings.size(); ++d) {
            if (detections[d].needsEmbedding)
                detections[d].embedding = embeddings[e++];
        }

        // 3) التحقق من أن الوجه المربوط بمسار هو الشخص نفسه، وإلا يُفصل ويُعامل كوجه جديد
        for (Detection &detection : detections) {
            if (detection.track < 0 || detection.embedding.empty())
                continue;
            Track &track = tracks[size_t(detection.track)];
            if (track.embedding.empty())
                continue;
            if (FaceEngine::cosine(track.embedding, detection.embedding) >= TrackBreakSimilarity
                || similarity(identities[track.identity], detection.embedding) >= TrackBreakSimilarity)
                continue;
            track.closed = true;
            detection.track = -1;
        }

        // 4) الوجوه غير المتتبَّعة: هوية سابقة تشبهها (عادت بعد غياب أو بزاوية أخرى) أو هوية جديدة
        std::vector<int> present; // هويات ظهرت في هذا الإطار فلا تُربط بوجه آخر
        for (const Detection &detection : detections) {
            if (detection.track >= 0)
                present.push_back(tracks[size_t(detection.track)].identity);
        }
        for (Detection &detection : detections) {
            if (detection.track >= 0 || detection.embedding.empty())
                continue;
            int match = 0;
            float bestSimilarity = reidentifyThreshold(detection.quality);
            for (const auto &[id, identity] : identities) {
                if (std::find(present.begin(), present.end(), id) != present.end())
                    continue;
                const float s = similarity(identity, detection.embedding);
                if (s > bestSimilarity) {
                    bestSimilarity = s;
                    match = id;
                }
            }
            if (match == 0) {
                match = nextId++;
                Identity identity;
                identity.id = match;
                identity.sum.assign(detection.embedding.size(), 0.0f);
                identities.emplace(match, std::move(identity));
            }
            present.push_back(match);
            Track track;
            track.identity = match;
            track.firstSeen = processed;
            tracks.push_back(track);
            detection.track = int(tracks.size()) - 1;
        }
        for (size_t i = 0; i < present.size(); ++i) {
            for (size_t j = i + 1; j < present.size(); ++j)
                together.insert(pairKey(present[i], present[j]));
        }

        // 5) تحديث الهويات: تجميع البصمات مرجّحة بالجودة، فكلما طال ظهور الوجه زادت دقة التعرف
        QList<FaceItem> current;
        const double displayScale = std::min(1.0, double(MaxDisplaySide) / std::max(work.cols, work.rows));
        for (Detection &detection : detections) {
            if (detection.track < 0)
                continue;
            Track &track = tracks[size_t(detection.track)];
            Identity &identity = identities[track.identity];
            track.box = toRect(detection.face.box);
            track.lastSeen = processed;
            ++track.hits;
            ++identity.frames;
            identity.lastSeen = processed;
            if (identity.firstSeenMs < 0)
                identity.firstSeenMs = positionMs;
            identity.lastSeenMs = positionMs;
            addAppearance(identity, positionMs);
            identity.changed = true;

            bool firstConfirmation = false;
            if (!identity.confirmed && track.hits >= MinTrackHits
                && track.hits * 2 >= processed - track.firstSeen + 1) {
                identity.confirmed = true;
                firstConfirmation = true;
            }

            bool betterShot = false;
            if (!detection.embedding.empty()) {
                track.lastEmbedded = processed;
                track.embedding = detection.embedding;
                const float weight = 0.05f + detection.quality * detection.quality;
                for (size_t i = 0; i < identity.sum.size() && i < detection.embedding.size(); ++i)
                    identity.sum[i] += weight * detection.embedding[i];
                identity.mean = identity.sum;
                FaceEngine::normalize(identity.mean);
                addExemplar(identity, detection.embedding, detection.quality);
                identity.matchStale = true;
                identity.mergeChecked = false;

                if (identity.best.packedCrop.isEmpty() || detection.quality >= identity.best.quality) {
                    AnalyzedFace analyzed;
                    analyzed.detection = detection.face;
                    analyzed.aligned = detection.aligned;
                    analyzed.quality = detection.quality;
                    identity.best = ImageIO::makeFaceItem(work, analyzed, identity.id, TrackPhotoSize);
                    ImageIO::pack(identity.best);
                    identity.bestMs = positionMs;
                    const QRectF box = toRect(detection.face.box);
                    identity.sourceBox = QRectF(box.topLeft() / workScale, box.size() / workScale);
                    betterShot = true;
                }
            }
            if (!identity.confirmed)
                continue;

            // تحديث البطاقة عند الاعتماد أو ظهور لقطة أوضح، وإلا كل عدة إطارات فقط
            if (firstConfirmation || betterShot || processed - identity.lastEmitted >= UpdateInterval)
                emitUpdate(identity);

            FaceItem item;
            item.id = identity.id;
            const QRectF box = toRect(detection.face.box);
            item.box = QRectF(box.topLeft() * displayScale, box.size() * displayScale);
            item.quality = detection.quality;
            item.frames = identity.frames;
            item.match = identity.match;
            current.append(item);
        }

        // 6) إغلاق المسارات المنقطعة، ونسيان الكشوفات العابرة، ودمج الهويات المكررة
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [&](const Track &t) { return t.closed || processed - t.lastSeen > MaxTrackGap; }),
                     tracks.end());
        for (auto it = identities.begin(); it != identities.end();) {
            const bool stale = !it->second.confirmed && processed - it->second.lastSeen > UnconfirmedLifetime;
            it = stale ? identities.erase(it) : std::next(it);
        }
        if (processed % MergeInterval == 0) {
            mergeDuplicates();
            // بطاقة دُمجت للتو تُعرض بالهوية الباقية
            for (FaceItem &item : current) {
                for (auto it = redirect.find(item.id); it != redirect.end(); it = redirect.find(item.id))
                    item.id = it->second;
            }
            redirect.clear();
        }

        // لا تُرسل إطارات جديدة إذا كانت الواجهة متأخرة في عرض السابقة
        if (m_pendingFrames < MaxPendingFrames) {
            ++m_pendingFrames;
            cv::Mat display = work;
            if (displayScale < 1.0)
                cv::resize(work, display, cv::Size(), displayScale, displayScale, cv::INTER_AREA);
            double progress = -1.0;
            if (!camera && endFrame > startFrame)
                progress = std::clamp(double(frameIndex - startFrame) / double(endFrame - startFrame), 0.0, 1.0);
            emit frameReady(ImageIO::toQImage(display), current, positionMs, progress);
        }
    }

    // دمج أخير ثم التحديث الأخير لكل وجه بعدد إطاراته ونتيجته النهائية
    for (auto &[id, identity] : identities) {
        if (identity.confirmed && identity.matchStale)
            emitUpdate(identity);
    }
    mergeDuplicates();
    for (auto &[id, identity] : identities) {
        if (identity.confirmed && identity.changed)
            emitUpdate(identity);
    }
    emit finished(QString());
}

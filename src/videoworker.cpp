#include "videoworker.h"

#include "faceengine.h"
#include "imageio.h"

#include <QMutexLocker>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <map>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace {

constexpr int MaxTrackGap = 15;           // إطارات يبقى فيها المسار مفتوحاً دون ظهور الوجه
constexpr int UnconfirmedLifetime = 150;  // كشف لم يُعتمد يُنسى بعد هذا الغياب
constexpr int MinTrackHits = 3;           // لا يُعتمد الوجه قبل ظهوره في عدة إطارات متقاربة
constexpr int EmbedInterval = 5;          // إعادة حساب بصمة الوجه المتتبَّع كل عدة إطارات فقط
constexpr float BetterShotMargin = 0.08f; // لقطة أوضح من أفضل لقطة سابقة تُحلَّل فوراً
constexpr float ReidentifySimilarity = 0.5f;
constexpr int UpdateInterval = 10;        // أقصى عدد إطارات بين تحديثين لبطاقة الوجه
constexpr int MaxPendingFrames = 2;
constexpr int MaxDisplaySide = 1920;      // الإطارات الأكبر تُصغَّر للعرض فقط
constexpr int TrackPhotoSize = 192;
constexpr double AutoAnalysisRate = 10.0; // إطارات محللة لكل ثانية من الفيديو في الوضع التلقائي
// الوجه المرتبط بمسار بتداخل الموضع فقط يُفصل عنه إن لم تشبه بصمته بصمة صاحب المسار،
// كأن ينتقل المشهد إلى شخص آخر يقف في الموضع نفسه
constexpr float TrackKeepSimilarity = 0.30f;
constexpr double SceneCutDistance = 0.45;   // فرق المدرج اللوني بين إطارين متتاليين يدل على قطع المشهد
constexpr float AutoMergeSimilarity = 0.55f; // دمج هويتين متشابهتين جداً في نهاية التحليل
constexpr qint64 AppearanceGapMs = 2000;    // غياب أقصر من هذا لا يبدأ مرة ظهور جديدة

// شخص ظهر في الفيديو، وقد يعود بعد غياب طويل فيُربط بالهوية نفسها
struct Identity
{
    int id = 0;
    std::vector<float> sum;  // مجموع البصمات مرجّحاً بالجودة
    std::vector<float> mean; // المجموع بعد التطبيع
    int frames = 0;
    int lastSeen = 0;
    bool confirmed = false;
    FaceItem best;
    MatchResult match;
    bool matchStale = true;
    bool changed = false;
    int lastEmitted = -1000000;
    qint64 bestMs = -1;
    QRectF sourceBox;
    QList<QPair<qint64, qint64>> appearances;

    void seenAt(qint64 ms)
    {
        if (!appearances.isEmpty() && ms - appearances.last().second <= AppearanceGapMs)
            appearances.last().second = std::max(appearances.last().second, ms);
        else
            appearances.append({ms, ms});
    }
};

// مدرج لوني مصغر لكشف قطع المشهد
cv::Mat sceneSignature(const cv::Mat &frame)
{
    cv::Mat small, hsv, hist;
    cv::resize(frame, small, cv::Size(64, 36), 0, 0, cv::INTER_AREA);
    cv::cvtColor(small, hsv, cv::COLOR_BGR2HSV);
    const int channels[] = {0, 1};
    const int sizes[] = {16, 8};
    const float hueRange[] = {0, 180};
    const float satRange[] = {0, 256};
    const float *ranges[] = {hueRange, satRange};
    cv::calcHist(&hsv, 1, channels, cv::Mat(), hist, 2, sizes, ranges);
    cv::normalize(hist, hist, 1.0, 0.0, cv::NORM_L1);
    return hist;
}

// ظهور متصل لهوية في إطارات متقاربة
struct Track
{
    int identity = 0;
    QRectF box;
    int firstSeen = 0;
    int lastSeen = 0;
    int hits = 0;
    int lastEmbedded = -1000000;
};

struct Detection
{
    DetectedFace face;
    cv::Mat aligned;
    float quality = 0.0f;
    int track = -1;
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

} // namespace

VideoWorker::VideoWorker(const FaceEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_gallery(std::make_shared<Gallery>())
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
    m_endMs = std::max<qint64>(0, endMs);
}

void VideoWorker::setRegion(const QRectF &region)
{
    QMutexLocker locker(&m_mutex);
    m_region = region;
}

void VideoWorker::setEnhance(bool enhance)
{
    m_enhance = enhance;
}

void VideoWorker::mergeIdentities(int keepId, int removedId)
{
    QMutexLocker locker(&m_mutex);
    m_pendingMerges.emplace_back(keepId, removedId);
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
    qint64 startMs = 0;
    qint64 endMs = 0;
    QRectF region;
    {
        QMutexLocker locker(&m_mutex);
        startMs = m_startMs;
        endMs = m_endMs;
        region = m_region;
        m_pendingMerges.clear();
    }

    cv::VideoCapture capture;
    const bool camera = source.startsWith(QStringLiteral("camera:"));
    if (camera) {
        capture.open(source.mid(7).toInt());
        // أحدث إطار فقط، حتى لا يتأخر العرض عن الكاميرا عندما يكون التحليل أبطأ منها
        capture.set(cv::CAP_PROP_BUFFERSIZE, 1);
        startMs = endMs = 0;
    } else {
        ImageIO::openVideo(capture, source);
    }
    if (!capture.isOpened()) {
        emit finished(camera ? QStringLiteral("تعذر فتح الكاميرا") : QStringLiteral("تعذر فتح ملف الفيديو"));
        return;
    }

    const int totalFrames = camera ? 0 : std::max(0, int(capture.get(cv::CAP_PROP_FRAME_COUNT)));
    const double rawFps = capture.get(cv::CAP_PROP_FPS);
    const double fps = rawFps > 1.0 && rawFps < 1000.0 ? rawFps : 25.0;
    int step = camera ? 1 : m_frameStep.load();
    if (step <= 0)
        step = std::max(1, int(std::lround(fps / AutoAnalysisRate)));
    emit started(step, rawFps > 1.0 && rawFps < 1000.0 ? rawFps : 0.0);

    // الجزء المطلوب من الفيديو
    int frameIndex = 0;
    if (startMs > 0) {
        frameIndex = int(std::llround(startMs * fps / 1000.0));
        capture.set(cv::CAP_PROP_POS_FRAMES, frameIndex);
        const double actual = capture.get(cv::CAP_PROP_POS_FRAMES);
        if (actual > 0.0)
            frameIndex = int(actual);
    }
    const int firstFrame = frameIndex;
    int lastFrame = totalFrames;
    if (endMs > startMs)
        lastFrame = totalFrames > 0 ? std::min(totalFrames, int(std::llround(endMs * fps / 1000.0)))
                                    : int(std::llround(endMs * fps / 1000.0));
    const int rangeFrames = lastFrame > firstFrame ? lastFrame - firstFrame : 0;

    std::map<int, Identity> identities;
    std::vector<Track> tracks;
    int nextId = 1;
    int processed = 0;
    cv::Mat frame;
    cv::Mat previousScene;
    QElapsedTimer cameraClock;
    cameraClock.start();

    GalleryPtr gallery;
    float threshold = 0.0f;
    auto summarize = [&](Identity &identity) {
        if (identity.matchStale) {
            identity.match = FaceDatabase::identify(*gallery, identity.mean, threshold);
            identity.matchStale = false;
        }
        FaceItem summary = identity.best;
        summary.id = identity.id;
        summary.frames = identity.frames;
        summary.embedding = identity.mean;
        summary.match = identity.match;
        summary.bestMs = identity.bestMs;
        summary.sourceBox = identity.sourceBox;
        summary.appearances = identity.appearances;
        summary.firstSeenMs = identity.appearances.isEmpty() ? -1 : identity.appearances.first().first;
        summary.lastSeenMs = identity.appearances.isEmpty() ? -1 : identity.appearances.last().second;
        return summary;
    };
    auto emitUpdate = [&](Identity &identity) {
        identity.changed = false;
        identity.lastEmitted = processed;
        emit trackUpdated(summarize(identity));
    };
    // ضم هوية إلى أخرى: البصمات وعدد الإطارات وأوضح لقطة ومرات الظهور
    auto absorb = [&](Identity &keep, Identity &removed) {
        if (keep.sum.size() == removed.sum.size()) {
            for (size_t i = 0; i < keep.sum.size(); ++i)
                keep.sum[i] += removed.sum[i];
            keep.mean = keep.sum;
            FaceEngine::normalize(keep.mean);
        }
        keep.frames += removed.frames;
        keep.lastSeen = std::max(keep.lastSeen, removed.lastSeen);
        keep.confirmed = keep.confirmed || removed.confirmed;
        if (removed.best.quality > keep.best.quality) {
            keep.best = removed.best;
            keep.bestMs = removed.bestMs;
            keep.sourceBox = removed.sourceBox;
        }
        keep.appearances += removed.appearances;
        std::sort(keep.appearances.begin(), keep.appearances.end());
        QList<QPair<qint64, qint64>> merged;
        for (const auto &range : std::as_const(keep.appearances)) {
            if (!merged.isEmpty() && range.first - merged.last().second <= AppearanceGapMs)
                merged.last().second = std::max(merged.last().second, range.second);
            else
                merged.append(range);
        }
        keep.appearances = merged;
        keep.matchStale = keep.changed = true;
        for (Track &track : tracks) {
            if (track.identity == removed.id)
                track.identity = keep.id;
        }
    };

    while (!m_stop) {
        while (m_paused && !m_stop)
            QThread::msleep(25);
        if (lastFrame > 0 && endMs > startMs && frameIndex >= lastFrame)
            break;
        // الإطارات المتخطاة تُقرأ دون فك ترميزها بالكامل، وهذا أسرع بكثير
        if (!camera && (frameIndex - firstFrame) % step != 0) {
            if (!capture.grab())
                break;
            ++frameIndex;
            continue;
        }
        if (!capture.read(frame))
            break;
        const qint64 positionMs = camera ? cameraClock.elapsed() : qint64(std::llround(frameIndex * 1000.0 / fps));
        ++frameIndex;
        if (frame.empty())
            continue;
        ++processed;

        bool enhance = m_enhance.load();
        {
            QMutexLocker locker(&m_mutex);
            if (gallery != m_gallery || threshold != m_threshold) {
                for (auto &[id, identity] : identities)
                    identity.matchStale = identity.changed = true;
            }
            gallery = m_gallery;
            threshold = m_threshold;
            region = m_region;
            // الدمج الذي طلبه المستخدم أثناء التحليل
            for (const auto &[keepId, removedId] : m_pendingMerges) {
                auto keep = identities.find(keepId);
                auto removed = identities.find(removedId);
                if (keep == identities.end() || removed == identities.end() || keepId == removedId)
                    continue;
                absorb(keep->second, removed->second);
                identities.erase(removed);
            }
            m_pendingMerges.clear();
        }

        // قطع المشهد: كل وجه يُتحقق من بصمته بدل الاعتماد على موضعه فقط
        const cv::Mat scene = sceneSignature(frame);
        const bool sceneCut = !previousScene.empty() && cv::compareHist(previousScene, scene, cv::HISTCMP_BHATTACHARYYA) > SceneCutDistance;
        previousScene = scene;

        // منطقة البحث، ثم التحسين إن طُلب؛ والإحداثيات تُعاد إلى الإطار الأصلي
        cv::Rect roi(0, 0, frame.cols, frame.rows);
        if (!region.isEmpty()) {
            roi = cv::Rect(int(region.x() * frame.cols), int(region.y() * frame.rows), int(region.width() * frame.cols),
                           int(region.height() * frame.rows))
                & roi;
            if (roi.width < 16 || roi.height < 16)
                roi = cv::Rect(0, 0, frame.cols, frame.rows);
        }
        cv::Mat work = roi.width == frame.cols && roi.height == frame.rows ? frame : frame(roi).clone();
        cv::Mat original = work;
        if (enhance)
            work = ImageIO::enhance(work);
        const double workScale = double(work.cols) / std::max(1, roi.width);
        auto toSource = [&](const cv::Rect2f &box) {
            return QRectF(roi.x + box.x / workScale, roi.y + box.y / workScale, box.width / workScale, box.height / workScale);
        };

        // 1) كشف الوجوه وربطها بالمسارات المفتوحة حسب تداخل الموضع
        std::vector<Detection> detections;
        for (const DetectedFace &face : m_engine->detect(work, false)) {
            Detection detection;
            detection.face = face;
            detection.aligned = m_engine->align(work, face);
            detection.quality = m_engine->quality(face, detection.aligned);
            detections.push_back(std::move(detection));
        }
        struct Pair { float iou; size_t detection; size_t track; };
        std::vector<Pair> pairs;
        for (size_t d = 0; d < detections.size(); ++d) {
            for (size_t t = 0; t < tracks.size(); ++t) {
                const float o = overlap(tracks[t].box, toSource(detections[d].face.box));
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
            trackUsed[pair.track] = true;
        }

        // 2) البصمة تُحسب للوجوه الجديدة، ولكل وجه متتبَّع كل عدة إطارات أو عند ظهور لقطة أوضح أو قطع المشهد
        std::vector<cv::Mat> toEmbed;
        for (Detection &detection : detections) {
            if (detection.track < 0) {
                detection.needsEmbedding = true;
            } else {
                const Track &track = tracks[size_t(detection.track)];
                const Identity &identity = identities[track.identity];
                detection.needsEmbedding = sceneCut || processed - track.lastEmbedded >= EmbedInterval
                    || detection.quality > identity.best.quality + BetterShotMargin;
            }
            if (detection.needsEmbedding)
                toEmbed.push_back(detection.aligned);
        }
        std::vector<std::vector<float>> embeddings = m_engine->embedMany(toEmbed);
        if (enhance && !embeddings.empty()) {
            // دمج بصمة الوجه من الإطار الأصلي حتى لا يغيّر التحسين ملامحه
            std::vector<cv::Mat> originals;
            for (const Detection &detection : detections) {
                if (!detection.needsEmbedding)
                    continue;
                DetectedFace mapped = detection.face;
                const float k = float(1.0 / workScale);
                mapped.box = cv::Rect2f(mapped.box.x * k, mapped.box.y * k, mapped.box.width * k, mapped.box.height * k);
                for (cv::Point2f &point : mapped.landmarks)
                    point *= k;
                originals.push_back(m_engine->align(original, mapped));
            }
            const std::vector<std::vector<float>> plain = m_engine->embedMany(originals);
            for (size_t i = 0; i < embeddings.size() && i < plain.size(); ++i) {
                for (size_t k = 0; k < embeddings[i].size() && k < plain[i].size(); ++k)
                    embeddings[i][k] += plain[i][k];
                FaceEngine::normalize(embeddings[i]);
            }
        }
        for (size_t d = 0, e = 0; d < detections.size() && e < embeddings.size(); ++d) {
            if (detections[d].needsEmbedding)
                detections[d].embedding = embeddings[e++];
        }

        // وجه ربطه الموضع بمسار لكن بصمته لا تشبه صاحبه: يُفصل ويُعامل كوجه جديد
        for (Detection &detection : detections) {
            if (detection.track < 0 || detection.embedding.empty())
                continue;
            const Identity &identity = identities[tracks[size_t(detection.track)].identity];
            if (!identity.mean.empty() && FaceEngine::cosine(identity.mean, detection.embedding) < TrackKeepSimilarity) {
                tracks[size_t(detection.track)].lastSeen = -1000000; // يُغلق المسار القديم
                detection.track = -1;
            }
        }

        // 3) الوجوه غير المتتبَّعة: هوية سابقة تشبهها (عادت بعد غياب) أو هوية جديدة
        std::vector<int> present; // هويات ظهرت في هذا الإطار فلا تُربط بوجه آخر
        for (const Detection &detection : detections) {
            if (detection.track >= 0)
                present.push_back(tracks[size_t(detection.track)].identity);
        }
        for (Detection &detection : detections) {
            if (detection.track >= 0 || detection.embedding.empty())
                continue;
            int match = 0;
            float bestSimilarity = ReidentifySimilarity;
            for (const auto &[id, identity] : identities) {
                if (std::find(present.begin(), present.end(), id) != present.end())
                    continue;
                const float similarity = FaceEngine::cosine(identity.mean, detection.embedding);
                if (similarity > bestSimilarity) {
                    bestSimilarity = similarity;
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

        // 4) تحديث الهويات: تجميع البصمات مرجّحة بالجودة، فكلما طال ظهور الوجه زادت دقة التعرف
        QList<FaceItem> current;
        const double displayScale = std::min(1.0, double(MaxDisplaySide) / std::max(frame.cols, frame.rows));
        for (Detection &detection : detections) {
            if (detection.track < 0)
                continue;
            Track &track = tracks[size_t(detection.track)];
            Identity &identity = identities[track.identity];
            const QRectF sourceBox = toSource(detection.face.box);
            track.box = sourceBox;
            track.lastSeen = processed;
            ++track.hits;
            ++identity.frames;
            identity.lastSeen = processed;
            identity.changed = true;
            identity.seenAt(positionMs);

            bool firstConfirmation = false;
            if (!identity.confirmed && track.hits >= MinTrackHits
                && track.hits * 2 >= processed - track.firstSeen + 1) {
                identity.confirmed = true;
                firstConfirmation = true;
            }

            bool betterShot = false;
            if (!detection.embedding.empty()) {
                track.lastEmbedded = processed;
                const float weight = 0.05f + detection.quality * detection.quality;
                for (size_t i = 0; i < identity.sum.size() && i < detection.embedding.size(); ++i)
                    identity.sum[i] += weight * detection.embedding[i];
                identity.mean = identity.sum;
                FaceEngine::normalize(identity.mean);
                identity.matchStale = true;

                if (identity.best.crop.isNull() || detection.quality >= identity.best.quality) {
                    AnalyzedFace analyzed;
                    analyzed.detection = detection.face;
                    analyzed.aligned = detection.aligned;
                    analyzed.quality = detection.quality;
                    identity.best = ImageIO::makeFaceItem(work, analyzed, identity.id, TrackPhotoSize);
                    identity.bestMs = positionMs;
                    identity.sourceBox = sourceBox;
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
            item.box = QRectF(sourceBox.topLeft() * displayScale, sourceBox.size() * displayScale);
            item.quality = detection.quality;
            item.frames = identity.frames;
            item.match = identity.match;
            current.append(item);
        }

        // 5) إغلاق المسارات المنقطعة ونسيان الكشوفات العابرة التي لم تُعتمد
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [&](const Track &t) { return processed - t.lastSeen > MaxTrackGap; }),
                     tracks.end());
        for (auto it = identities.begin(); it != identities.end();) {
            const bool stale = !it->second.confirmed && processed - it->second.lastSeen > UnconfirmedLifetime;
            it = stale ? identities.erase(it) : std::next(it);
        }

        // لا تُرسل إطارات جديدة إذا كانت الواجهة متأخرة في عرض السابقة
        if (m_pendingFrames < MaxPendingFrames) {
            ++m_pendingFrames;
            cv::Mat display = frame;
            if (displayScale < 1.0)
                cv::resize(frame, display, cv::Size(), displayScale, displayScale, cv::INTER_AREA);
            emit frameReady(ImageIO::toQImage(display), current, frameIndex - firstFrame, rangeFrames, positionMs);
        }
    }

    // شخص ظُن أنه شخصان (مثلاً بعد تغير الإضاءة أو زاوية الوجه): يُدمج إن تشابهت البصمتان كثيراً
    bool mergedAny = true;
    while (mergedAny) {
        mergedAny = false;
        for (auto a = identities.begin(); a != identities.end() && !mergedAny; ++a) {
            if (!a->second.confirmed)
                continue;
            for (auto b = std::next(a); b != identities.end(); ++b) {
                if (!b->second.confirmed || FaceEngine::cosine(a->second.mean, b->second.mean) < AutoMergeSimilarity)
                    continue;
                // لا يُدمج شخصان ظهرا معاً في الوقت نفسه
                bool together = false;
                for (const auto &ra : std::as_const(a->second.appearances)) {
                    for (const auto &rb : std::as_const(b->second.appearances))
                        together = together || (ra.first <= rb.second && rb.first <= ra.second);
                }
                if (together)
                    continue;
                Identity &keep = a->second.frames >= b->second.frames ? a->second : b->second;
                Identity &removed = &keep == &a->second ? b->second : a->second;
                const int keepId = keep.id;
                const int removedId = removed.id;
                absorb(keep, removed);
                identities.erase(removedId);
                emit identitiesMerged(keepId, removedId);
                mergedAny = true;
                break;
            }
        }
    }

    // التحديث الأخير لكل وجه بعدد إطاراته ونتيجته النهائية
    for (auto &[id, identity] : identities) {
        if (identity.confirmed && identity.changed)
            emitUpdate(identity);
    }
    emit finished(QString());
}

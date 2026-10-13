#include "videoworker.h"

#include "faceengine.h"
#include "imageio.h"

#include <QMutexLocker>
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
};

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
    int step = camera ? 1 : m_frameStep.load();
    if (step <= 0)
        step = std::max(1, int(std::lround((fps > 1.0 && fps < 1000.0 ? fps : 25.0) / AutoAnalysisRate)));
    emit started(step, fps > 1.0 && fps < 1000.0 ? fps : 0.0);

    std::map<int, Identity> identities;
    std::vector<Track> tracks;
    int nextId = 1;
    int frameIndex = 0;
    int processed = 0;
    cv::Mat frame;

    GalleryPtr gallery;
    float threshold = 0.0f;
    auto emitUpdate = [&](Identity &identity) {
        if (identity.matchStale) {
            identity.match = FaceDatabase::identify(*gallery, identity.mean, threshold);
            identity.matchStale = false;
        }
        FaceItem summary = identity.best;
        summary.id = identity.id;
        summary.frames = identity.frames;
        summary.embedding = identity.mean;
        summary.match = identity.match;
        identity.changed = false;
        identity.lastEmitted = processed;
        emit trackUpdated(summary);
    };

    while (!m_stop) {
        while (m_paused && !m_stop)
            QThread::msleep(25);
        // الإطارات المتخطاة تُقرأ دون فك ترميزها بالكامل، وهذا أسرع بكثير
        if (!camera && frameIndex % step != 0) {
            if (!capture.grab())
                break;
            ++frameIndex;
            continue;
        }
        if (!capture.read(frame))
            break;
        ++frameIndex;
        if (frame.empty())
            continue;
        ++processed;

        {
            QMutexLocker locker(&m_mutex);
            if (gallery != m_gallery || threshold != m_threshold) {
                for (auto &[id, identity] : identities)
                    identity.matchStale = identity.changed = true;
            }
            gallery = m_gallery;
            threshold = m_threshold;
        }

        // 1) كشف الوجوه وربطها بالمسارات المفتوحة حسب تداخل الموضع
        std::vector<Detection> detections;
        for (const DetectedFace &face : m_engine->detect(frame, false)) {
            Detection detection;
            detection.face = face;
            detection.aligned = m_engine->align(frame, face);
            detection.quality = m_engine->quality(face, detection.aligned);
            detections.push_back(std::move(detection));
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
            trackUsed[pair.track] = true;
        }

        // 2) البصمة تُحسب للوجوه الجديدة، ولكل وجه متتبَّع كل عدة إطارات أو عند ظهور لقطة أوضح
        std::vector<cv::Mat> toEmbed;
        for (Detection &detection : detections) {
            if (detection.track < 0) {
                detection.needsEmbedding = true;
            } else {
                const Track &track = tracks[size_t(detection.track)];
                const Identity &identity = identities[track.identity];
                detection.needsEmbedding = processed - track.lastEmbedded >= EmbedInterval
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
            track.box = toRect(detection.face.box);
            track.lastSeen = processed;
            ++track.hits;
            ++identity.frames;
            identity.lastSeen = processed;
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
                    identity.best = ImageIO::makeFaceItem(frame, analyzed, identity.id, TrackPhotoSize);
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
            emit frameReady(ImageIO::toQImage(display), current, frameIndex, frameCount);
        }
    }

    // التحديث الأخير لكل وجه بعدد إطاراته ونتيجته النهائية
    for (auto &[id, identity] : identities) {
        if (identity.confirmed && identity.changed)
            emitUpdate(identity);
    }
    emit finished(QString());
}

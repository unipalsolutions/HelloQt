#include "videoworker.h"

#include "faceengine.h"
#include "imageio.h"

#include <QMutexLocker>

#include <algorithm>

#include <opencv2/videoio.hpp>

namespace {

constexpr int MaxTrackGap = 15;      // إطارات يُحتفظ فيها بالمسار دون ظهور الوجه
constexpr int MaxReidentifyGap = 150; // إعادة ربط الوجه بمساره بعد اختفاء مؤقت
constexpr int MaxPendingFrames = 2;
constexpr int MinTrackHits = 3;       // لا يُعرض المسار قبل ظهوره في عدة إطارات متقاربة

struct Track
{
    int id = 0;
    QRectF box;
    std::vector<float> sum;
    int frames = 0;
    int firstSeen = 0;
    int lastSeen = 0;
    bool confirmed = false;
    FaceItem best;
};

float overlap(const QRectF &a, const QRectF &b)
{
    const QRectF inter = a.intersected(b);
    const double interArea = inter.width() * inter.height();
    const double total = a.width() * a.height() + b.width() * b.height() - interArea;
    return total > 0.0 ? float(interArea / total) : 0.0f;
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
    m_frameStep = std::max(1, step);
}

void VideoWorker::requestStop()
{
    m_stop = true;
}

void VideoWorker::frameShown()
{
    --m_pendingFrames;
}

void VideoWorker::process(const QString &source)
{
    m_stop = false;
    m_pendingFrames = 0;

    cv::VideoCapture capture;
    const bool camera = source.startsWith(QStringLiteral("camera:"));
    if (camera)
        capture.open(source.mid(7).toInt());
    else
        ImageIO::openVideo(capture, source);
    if (!capture.isOpened()) {
        emit finished(camera ? QStringLiteral("تعذر فتح الكاميرا") : QStringLiteral("تعذر فتح ملف الفيديو"));
        return;
    }

    const int frameCount = camera ? 0 : int(capture.get(cv::CAP_PROP_FRAME_COUNT));
    std::vector<Track> tracks;
    int nextId = 1;
    int frameIndex = 0;
    int processed = 0;
    cv::Mat frame;

    while (!m_stop && capture.read(frame)) {
        ++frameIndex;
        if (frame.empty())
            continue;
        if (!camera && (frameIndex - 1) % m_frameStep != 0)
            continue;
        ++processed;

        GalleryPtr gallery;
        float threshold;
        {
            QMutexLocker locker(&m_mutex);
            gallery = m_gallery;
            threshold = m_threshold;
        }

        const std::vector<AnalyzedFace> analyzed = m_engine->analyze(frame, false);
        std::vector<bool> used(tracks.size(), false);
        QList<FaceItem> current;

        for (const AnalyzedFace &face : analyzed) {
            FaceItem item = ImageIO::makeFaceItem(frame, face, 0);

            // ربط الوجه بمسار قريب في الموضع، أو بمسار اختفى مؤقتاً ويشبهه في الملامح
            int match = -1;
            float bestOverlap = 0.3f;
            for (size_t t = 0; t < tracks.size(); ++t) {
                if (used[t] || processed - tracks[t].lastSeen > MaxTrackGap)
                    continue;
                const float o = overlap(tracks[t].box, item.box);
                if (o > bestOverlap) {
                    bestOverlap = o;
                    match = int(t);
                }
            }
            if (match < 0) {
                float bestSimilarity = 0.5f;
                for (size_t t = 0; t < tracks.size(); ++t) {
                    if (used[t] || processed - tracks[t].lastSeen > MaxReidentifyGap)
                        continue;
                    std::vector<float> mean = tracks[t].sum;
                    FaceEngine::normalize(mean);
                    const float similarity = FaceEngine::cosine(mean, item.embedding);
                    if (similarity > bestSimilarity) {
                        bestSimilarity = similarity;
                        match = int(t);
                    }
                }
            }
            if (match < 0) {
                Track track;
                track.id = nextId++;
                track.firstSeen = processed;
                track.sum.assign(item.embedding.size(), 0.0f);
                tracks.push_back(track);
                used.push_back(false);
                match = int(tracks.size()) - 1;
            }
            used[match] = true;

            // تجميع البصمات مرجّحة بالجودة: كلما طال ظهور الوجه زادت دقة التعرف
            Track &track = tracks[match];
            const float weight = 0.05f + item.quality * item.quality;
            for (size_t i = 0; i < item.embedding.size() && i < track.sum.size(); ++i)
                track.sum[i] += weight * item.embedding[i];
            ++track.frames;
            track.lastSeen = processed;
            track.box = item.box;
            if (track.best.crop.isNull() || item.quality >= track.best.quality)
                track.best = item;

            // كشف عابر في إطارات متفرقة (مثل يد مرفوعة) لا يُعتمد كوجه
            if (!track.confirmed && track.frames >= MinTrackHits
                && track.frames * 2 >= processed - track.firstSeen + 1)
                track.confirmed = true;
            if (!track.confirmed)
                continue;

            FaceItem summary = track.best;
            summary.id = track.id;
            summary.box = item.box;
            summary.frames = track.frames;
            summary.embedding = track.sum;
            FaceEngine::normalize(summary.embedding);
            summary.match = FaceDatabase::identify(*gallery, summary.embedding, threshold);
            emit trackUpdated(summary);

            item.id = track.id;
            item.frames = track.frames;
            item.match = summary.match;
            item.crop = QImage();
            item.aligned.release();
            current.append(item);
        }

        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [&](const Track &t) { return processed - t.lastSeen > MaxReidentifyGap; }),
                     tracks.end());

        // لا تُرسل إطارات جديدة إذا كانت الواجهة متأخرة في عرض السابقة
        if (m_pendingFrames < MaxPendingFrames) {
            ++m_pendingFrames;
            emit frameReady(ImageIO::toQImage(frame), current, frameIndex, frameCount);
        }
    }

    emit finished(QString());
}

#include "videoworker.h"

#include "cameras.h"
#include "faceengine.h"
#include "imageio.h"
#include "performance.h"
#include "scenecut.h"
#include "videofilters.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace {

constexpr int MaxTrackGap = 15;           // إطارات يبقى فيها المسار مفتوحاً دون ظهور الوجه
constexpr int UnconfirmedLifetime = 150;  // كشف لم يُعتمد يُنسى بعد هذا الغياب
constexpr int MinTrackHits = 3;           // لا يُعتمد الوجه قبل ظهوره في عدة إطارات متقاربة
constexpr int EmbedInterval = 5;          // إعادة حساب بصمة الوجه المتتبَّع كل عدة إطارات فقط
constexpr int QuickEmbedInterval = 20;    // في الفحص السريع: بصمة الوجه المتتبَّع نحو كل 5 ثوانٍ فقط
constexpr float BetterShotMargin = 0.08f; // لقطة أوضح من أفضل لقطة سابقة تُحلَّل فوراً
// الكاميرا: وجه يطابق شخصاً في القاعدة يُعتمد بعد ظهورين بدل MinTrackHits، فمن يمر سريعاً (لقطة قصيرة أو
// تحليل بطيء) يُعرف وينبَّه عليه قبل أن يغيب؛ والكشف الخاطئ لا يطابق شخصاً في القاعدة
constexpr int KnownTrackHits = 2;
// الكاميرا على جهاز بطيء: بين تحليلين ثانية أو أكثر يتحرك فيها المار كثيراً فلا يتداخل موضعاه ويبدأ كل
// ظهور مساراً جديداً لا يبلغ MinTrackHits؛ فتُعد ظهورات الهوية نفسها (المربوطة بالبصمة) في هذه المدة
constexpr qint64 CameraConfirmWindowMs = 10000;
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
constexpr double QuickAnalysisRate = 4.0; // في الفحص السريع للأرشيف
constexpr qint64 QuickPreviewMs = 1000;   // صورة معاينة كل ثانية من الوقت الفعلي في الفحص السريع
constexpr double IdleAnalysisRate = 2.0;  // عند خلو المشهد من الوجوه يُفحص أقل، ويعود للمعدل الكامل فور ظهور وجه
constexpr int IdleAfter = 20;             // إطارات محللة متتالية بلا وجوه قبل خفض المعدل
constexpr double StillDifference = 1.6;   // متوسط تغيّر البكسل (من 255) الذي يُعد دونه الإطار ساكناً
constexpr int MaxStillSkips = 50;
constexpr int DetectionSide = 640;        // الدقة التي يُصغَّر إليها الإطار كاملاً للكشف
constexpr int SmallFaceSide = 24;         // وجه أصغر من هذا بعد التصغير لا يُلتقط بثبات في الإطار كاملاً
constexpr int TileOverlap = 96;           // تداخل أجزاء المسح حتى يقع كل وجه بعيد كاملاً في جزء منها
constexpr int MaxFaceRegions = 6;         // أقصى عدد وجوه صغيرة متتبَّعة يُبحث حولها في التحليل الواحد
constexpr double MinAnalysisRate = 4.0;   // أقل معدل يصل إليه التكيّف مع الجهاز البطيء (إطارات محللة لكل ثانية)
// العرض سبق التحليل بأكثر من هذا إلى جزء لم يُحلَّل: يقفز التحليل إلى موضع العرض لتظهر المربعات عليه،
// ويعود إلى ما تخطاه بعد ذلك
constexpr qint64 FollowLagMs = 800;
constexpr qint64 FollowLeadMs = 300;      // يبدأ التحليل بعد موضع العرض بقليل فتصل نتيجته قبل عرض الإطار
// أثناء التشغيل على جهاز لا يلحق: أبعد مسافة بين إطارين محللين، دون مهلة عرض النتيجة (600) فلا تختفي المربعات بينهما
constexpr qint64 FollowMaxSpacingMs = 500;
// كشف اتجاه الفيديو المائل: عدد الإطارات المفحوصة، وأقل ثقة للوجه فيها، ومجموع الثقة الذي يكفي لإدارته
constexpr int OrientationProbes = 4;
constexpr float OrientationScore = 0.6f;
constexpr double OrientationMinScore = 1.5;
constexpr int ThumbnailHeight = 120;
// الكاميرا تعمل أياماً ويمر أمامها آلاف الأشخاص: أقدم من غاب طويلاً يُنسى (ظهوره سُجِّل في القاعدة)
// فتبقى الذاكرة وسرعة البحث ثابتتين
constexpr size_t MaxCameraIdentities = 300;
constexpr qint64 CameraForgetMs = 10 * 60 * 1000;         // إطار ساكن يُفحص رغم ذلك بعد هذا العدد، احتياطاً للتغير البطيء جداً

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
    std::deque<qint64> recentMs; // الكاميرا: أوقات آخر ظهورات الهوية (MinTrackHits على الأكثر)
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

void mergeAppearances(std::vector<std::pair<qint64, qint64>> &list);

void addAppearance(Identity &identity, qint64 ms)
{
    auto &list = identity.appearances;
    if (!list.empty() && ms >= list.back().first && ms - list.back().second <= AppearanceGapMs) {
        list.back().second = std::max(list.back().second, ms);
        return;
    }
    if (!list.empty() && ms < list.back().first) {
        // تحليل جزء تخطاه سابقاً ليلحق العرض: الفترة تُضم في مكانها
        list.push_back({ms, ms});
        mergeAppearances(list);
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

cv::Mat turned(const cv::Mat &image, int turns)
{
    if (turns % 4 == 0)
        return image;
    cv::Mat result;
    cv::rotate(image, result, turns == 1 ? cv::ROTATE_90_CLOCKWISE : turns == 2 ? cv::ROTATE_180 : cv::ROTATE_90_COUNTERCLOCKWISE);
    return result;
}

// مستطيل في صورة أُديرت turns ربع دورة مع عقارب الساعة (أبعادها width×height) إلى موضعه في الصورة قبل الإدارة
QRectF unturned(const QRectF &box, int turns, double width, double height)
{
    switch (turns) {
    case 1:
        return QRectF(box.y(), width - box.right(), box.height(), box.width());
    case 2:
        return QRectF(width - box.right(), height - box.bottom(), box.width(), box.height());
    case 3:
        return QRectF(height - box.bottom(), box.x(), box.height(), box.width());
    default:
        return box;
    }
}

// منطقة نسبية (من 0 إلى 1) من الإطار إلى موضعها بعد إدارته turns ربع دورة مع عقارب الساعة
QRectF turnedRegion(const QRectF &region, int turns)
{
    switch (turns) {
    case 1:
        return QRectF(1.0 - region.bottom(), region.x(), region.height(), region.width());
    case 2:
        return QRectF(1.0 - region.right(), 1.0 - region.bottom(), region.width(), region.height());
    case 3:
        return QRectF(region.y(), 1.0 - region.right(), region.height(), region.width());
    default:
        return region;
    }
}

// فيديو صُوِّر بالجوال مائلاً أو مقلوباً تظهر وجوهه مستلقية، فلا يكشفها النموذج ولا يتعرف عليها.
// تُفحص بضعة إطارات موزعة على الجزء المطلوب بالاتجاهات الأربعة، ويُعاد عدد أرباع الدورة التي تعدل
// الوجوه (0 إن ظهرت معتدلة أو لم يظهر وجه واضح). يغيّر موضع القراءة، فيُعاد بعده
int uprightTurns(const FaceEngine *engine, cv::VideoCapture &capture, int first, int last, bool fast)
{
    if (last - first < 2)
        return 0;
    const VideoFilters::Settings filters = VideoFilters::current();
    std::vector<cv::Mat> frames;
    for (int i = 0; i < OrientationProbes; ++i) {
        capture.set(cv::CAP_PROP_POS_FRAMES, double(first + qint64(last - first) * (2 * i + 1) / (2 * OrientationProbes)));
        cv::Mat frame;
        if (!capture.read(frame) || frame.empty())
            continue;
        frames.push_back(filters.isIdentity() ? frame : VideoFilters::apply(frame, filters));
    }
    double scores[4] = {0.0, 0.0, 0.0, 0.0};
    int uprightFrames = 0;
    for (const cv::Mat &frame : frames) {
        const std::vector<DetectedFace> faces = engine->detect(frame, false, OrientationScore, fast);
        uprightFrames += faces.empty() ? 0 : 1;
        for (const DetectedFace &face : faces)
            scores[0] += face.score;
    }
    // وجوه معتدلة في أغلب الإطارات المفحوصة: الفيديو معتدل، فلا تُجرَّب الاتجاهات الأخرى (أغلب الفيديوهات)
    if (uprightFrames * 4 >= int(frames.size()) * 3 && !frames.empty())
        return 0;
    for (const cv::Mat &frame : frames) {
        for (int turns = 1; turns < 4; ++turns) {
            for (const DetectedFace &face : engine->detect(turned(frame, turns), false, OrientationScore, fast))
                scores[turns] += face.score;
        }
    }
    const int best = int(std::max_element(scores, scores + 4) - scores);
    // اتجاه آخر يُعتمد فقط إن ظهرت فيه الوجوه أوضح بكثير، فلا تغيّره وجوه مائلة قليلاً في فيديو معتدل
    return best != 0 && scores[best] >= OrientationMinScore && scores[best] >= 2.0 * scores[0] ? best : 0;
}

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

void VideoWorker::setZones(const CameraRules::Rules &rules)
{
    QMutexLocker locker(&m_mutex);
    m_zones = rules;
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

void VideoWorker::setDisplayPosition(qint64 positionMs, bool playing)
{
    m_displayMs = positionMs;
    m_displayPlaying = playing;
}

void VideoWorker::frameShown()
{
    --m_pendingFrames;
}

void VideoWorker::setLiveFrames(std::shared_ptr<Cameras::FrameSlot> slot)
{
    QMutexLocker locker(&m_mutex);
    m_liveFrames = std::move(slot);
}

cv::Mat VideoWorker::lastCameraFrame()
{
    QMutexLocker locker(&m_mutex);
    return m_lastCameraFrame.clone();
}

void VideoWorker::process(const QString &source)
{
    // خطأ غير متوقع في القراءة أو المعالجات أو النماذج (إطار بصيغة غريبة من كاميرا، تعطل كرت الشاشة…)
    // ينهي التحليل برسالة ويُكتب في سجل الأحداث، بدل أن يُغلق البرنامج كله فجأة
    try {
        run(source);
    } catch (const std::exception &e) {
        qCritical("توقف التحليل بخطأ في «%s»: %s", qUtf8Printable(source), e.what());
        emit finished(QStringLiteral("توقف التحليل بسبب خطأ غير متوقع: %1").arg(QString::fromLocal8Bit(e.what())));
    } catch (...) {
        qCritical("توقف التحليل بخطأ غير معروف في «%s»", qUtf8Printable(source));
        emit finished(QStringLiteral("توقف التحليل بسبب خطأ غير متوقع"));
    }
}

void VideoWorker::run(const QString &source)
{
    m_paused = false;
    m_pendingFrames = 0;
    {
        QMutexLocker locker(&m_mutex);
        m_manualMerges.clear();
    }

    cv::VideoCapture capture;
    const bool camera = source.startsWith(QStringLiteral("camera:"));
    // الكاميرا تُقرأ في خيط خاص يحتفظ بأحدث إطار، فلا يتأخر العرض عنها عندما يكون التحليل أبطأ منها
    Cameras::Camera cameraSpec;
    std::unique_ptr<Cameras::LiveReader> live;
    std::shared_ptr<Cameras::FrameSlot> liveFrames;
    {
        QMutexLocker locker(&m_mutex);
        liveFrames = m_liveFrames;
    }
    if (camera) {
        if (!Cameras::resolve(source.mid(7), &cameraSpec)) {
            emit finished(QStringLiteral("هذه الكاميرا لم تعد في قائمة الكاميرات"));
            return;
        }
        live = std::make_unique<Cameras::LiveReader>(cameraSpec, m_recorder, liveFrames);
        QString error;
        if (!live->open(&error)) {
            emit finished(error.isEmpty() ? QStringLiteral("تعذر فتح الكاميرا") : error);
            return;
        }
    } else if (!ImageIO::openVideo(capture, source)) {
        emit finished(QStringLiteral("تعذر فتح ملف الفيديو: قد يكون تالفاً أو ناقصاً أو بصيغة غير مدعومة"));
        return;
    }
    QString endError;

    const int frameCount = camera ? 0 : std::max(0, int(capture.get(cv::CAP_PROP_FRAME_COUNT)));
    const double fps = camera ? live->fps() : capture.get(cv::CAP_PROP_FPS);
    const bool validFps = fps > 1.0 && fps < 1000.0;
    const double rate = validFps ? fps : 25.0;
    const bool automatic = !camera && m_frameStep.load() <= 0;
    const bool quick = !camera && m_quick;
    int step = camera ? 1 : m_frameStep.load();
    if (step <= 0)
        step = std::max(1, int(std::lround(rate / (quick ? QuickAnalysisRate : AutoAnalysisRate))));
    const int idleStep = std::max(step, int(std::lround(rate / IdleAnalysisRate)));
    int currentStep = step;
    int skip = 0;
    int idleAnalyses = 0;
    // التكيّف مع سرعة الجهاز: متوسط زمن تحليل الإطار، ومنه كم إطاراً يمر من الفيديو أثناء تحليل واحد
    const bool adaptive = automatic && m_adaptive && !quick;
    const int slowestStep = std::max(step, int(std::lround(rate / MinAnalysisRate)));
    const int playingSlowestStep = std::max(slowestStep, int(rate * FollowMaxSpacingMs / 1000.0));
    double analysisCostMs = 0.0;
    double embedCostMs = 0.0; // زمن بصمة الوجه الواحد
    double baseCostMs = 0.0;  // زمن الإطار دون البصمات (القراءة والكشف والتتبع)
    double frameEmbedMs = 0.0;
    QElapsedTimer analysisClock;
    const bool deliverFrames = camera || m_deliverFrames;
    // الكشف الخفيف على الجهاز الضعيف ما دام التحليل على المعالج (كرت الشاشة يكفيه الدقيق)
    const bool fastAllowed = Performance::fastDetection() && m_engine->hasFastDetector();
    qint64 lastThumbnailMs = LLONG_MIN / 2;
    QElapsedTimer previewClock;
    previewClock.start();
    qint64 lastPreview = -QuickPreviewMs;
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
    // اللحاق بالعرض: الأجزاء المحللة [بداية، نهاية) بأرقام الإطارات، والجزء الجاري من runStart حتى frameIndex
    // وينتهي عند runLimit (بداية أول جزء محلل بعده، أو نهاية المطلوب)
    const bool follow = adaptive && validFps && endFrame > startFrame;
    m_following = follow;
    std::map<int, int> covered;
    int runStart = frameIndex;
    int runLimit = endFrame;
    int coveredFrames = 0;
    m_displayMs = -1;
    m_displayPlaying = false;
    const qint64 durationMs = validFps && frameCount > 0 ? qint64(frameCount * 1000.0 / fps) : 0;
    // الفيديو المائل يُحلَّل بوجوه معتدلة، ثم تُعاد المواضع إلى اتجاه الإطار المعروض (لا في الفحص السريع للأرشيف)
    int turns = 0;
    if (!camera && !quick && frameCount > 0) {
        turns = uprightTurns(m_engine, capture, startFrame, endFrame > startFrame ? endFrame : frameCount, fastAllowed && m_engine->gpuName().isEmpty());
        if (startMs > 0)
            capture.set(cv::CAP_PROP_POS_MSEC, double(startMs));
        else
            capture.set(cv::CAP_PROP_POS_FRAMES, 0.0);
    }
    emit started(step, validFps ? fps : 0.0, durationMs);

    std::map<int, Identity> identities;
    std::vector<Track> tracks;
    std::map<int, int> redirect; // هويات دُمجت في إطار التحليل الحالي
    std::set<quint64> together; // أزواج هويات ظهرت معاً في إطار واحد، فهما شخصان مختلفان حتماً
    SceneCutDetector sceneCuts;
    int nextId = 1;
    int processed = 0;
    double workScale = 1.0;
    cv::Size filteredSize;
    cv::Mat stillReference; // آخر إطار فُحص فعلاً، مصغراً ورمادياً
    int stillSkips = 0;
    int tileTurn = 0; // جزء الإطار الذي يُمسح بدقته الأصلية في التحليل التالي
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

    const auto isCovered = [&](int index) {
        const auto it = covered.upper_bound(index);
        return it != covered.begin() && std::prev(it)->second > index;
    };
    // يضم الجزء الجاري إلى المحلل (حتى end) ويحدّث عدد إطاراته
    const auto commitRun = [&](int end) {
        end = std::min(end, runLimit);
        if (end <= runStart)
            return;
        int from = runStart;
        int to = end;
        auto it = covered.upper_bound(from);
        if (it != covered.begin() && std::prev(it)->second >= from)
            it = std::prev(it);
        while (it != covered.end() && it->first <= to) {
            from = std::min(from, it->first);
            to = std::max(to, it->second);
            it = covered.erase(it);
        }
        covered[from] = to;
        coveredFrames = 0;
        for (const auto &[first, last] : covered)
            coveredFrames += last - first;
        runStart = end;
    };
    // أول إطار لم يُحلَّل بعد، أو -1 إذا اكتمل المطلوب
    const auto firstGap = [&] {
        int position = startFrame;
        for (const auto &[first, last] : covered) {
            if (first > position)
                return position;
            position = std::max(position, last);
        }
        return position < endFrame ? position : -1;
    };
    // بداية جزء جديد: القراءة من target حتى أول جزء محلل بعده؛ المسارات والإطار الساكن لا تمتد عبر القفزة
    const auto beginRun = [&](int target) {
        capture.set(cv::CAP_PROP_POS_FRAMES, double(target));
        frameIndex = target;
        runStart = target;
        const auto next = covered.upper_bound(target);
        runLimit = next == covered.end() ? endFrame : std::min(endFrame, next->first);
        skip = 0;
        tracks.clear();
        stillReference.release();
        sceneCuts.reset();
    };

    while (!m_stop) {
        while (m_paused && !m_stop)
            QThread::msleep(25);
        if (follow) {
            if (frameIndex >= runLimit) {
                commitRun(frameIndex);
                const int gap = firstGap();
                if (gap < 0)
                    break;
                beginRun(gap);
                continue;
            }
            const qint64 display = m_displayMs;
            if (display >= 0) {
                const int displayFrame = int(double(display) * fps / 1000.0);
                const int lagFrames = int(FollowLagMs * fps / 1000.0);
                // خارج الجزء الجاري بأكثر من المهلة: أمامه، أو خلف بدايته (رجع المستخدم إلى جزء لم يُحلَّل)
                const bool outside = displayFrame > frameIndex + lagFrames || displayFrame < runStart - lagFrames;
                if (outside && displayFrame >= startFrame && displayFrame < endFrame && !isCovered(displayFrame)) {
                    commitRun(frameIndex);
                    const double leadMs = std::min(FollowLeadMs + analysisCostMs, FollowLagMs / 2.0);
                    int target = displayFrame + int(std::ceil(leadMs * fps / 1000.0));
                    if (target >= endFrame || isCovered(target))
                        target = displayFrame;
                    beginRun(target);
                    continue;
                }
            }
        }
        if (!camera && endFrame > startFrame && frameIndex >= endFrame)
            break;
        // الإطارات المتخطاة تُقرأ دون تحويلها إلى صورة، وهذا أسرع بكثير
        if (!camera && skip > 0) {
            if (!capture.grab()) {
                if (follow) {
                    // بقية هذا الجزء لا تُقرأ: تُعد منتهية ويُنتقل إلى غيره
                    commitRun(runLimit);
                    frameIndex = runLimit;
                    continue;
                }
                break;
            }
            ++frameIndex;
            --skip;
            continue;
        }
        if (camera) {
            // الانتظار على دفعات قصيرة حتى يستجيب «إيقاف» فوراً ولا ينتظر مهلة الكاميرا كاملة
            bool got = false;
            for (int waited = 0; waited < 12000 && !m_stop && !got; waited += 250)
                got = live->read(frame, 250);
            if (!got) {
                if (m_stop)
                    break;
                // انقطعت الكاميرا: تُعاد المحاولة حتى تعود (كاميرا الشبكة بلا حد، والموصولة نحو نصف دقيقة)
                live.reset();
                const bool network = cameraSpec.kind == Cameras::Kind::Network;
                for (int attempt = 1; !m_stop && (network || attempt <= 12); ++attempt) {
                    emit connectionLost(attempt);
                    for (int waited = 0; waited < 2000 && !m_stop; waited += 100)
                        QThread::msleep(100);
                    if (m_stop)
                        break;
                    auto reader = std::make_unique<Cameras::LiveReader>(cameraSpec, m_recorder, liveFrames);
                    if (reader->open(nullptr)) {
                        live = std::move(reader);
                        break;
                    }
                }
                if (!live) {
                    if (!m_stop)
                        endError = QStringLiteral("انقطع الاتصال بالكاميرا ولم يعد");
                    break;
                }
                // المشهد بعد العودة قد يختلف، فلا تُكمَل المسارات القديمة
                tracks.clear();
                emit connectionRestored();
                continue;
            }
            QMutexLocker locker(&m_mutex);
            m_lastCameraFrame = frame;
        } else if (!capture.read(frame)) {
            if (follow) {
                commitRun(runLimit);
                frameIndex = runLimit;
                continue;
            }
            break;
        }
        ++frameIndex;
        skip = currentStep - 1;
        if (frame.empty())
            continue;
        ++processed;
        analysisClock.start();
        const qint64 positionMs = camera ? cameraClock.elapsed()
            : validFps ? qint64(double(frameIndex - 1) * 1000.0 / fps)
                       : qint64(capture.get(cv::CAP_PROP_POS_MSEC));

        QRectF region;
        CameraRules::Rules zones;
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
            zones = m_zones;
            enhance = m_enhance;
            manualMerges.swap(m_manualMerges);
        }
        for (const auto &[keepId, removedId] : manualMerges) {
            merge(keepId, removedId);
            together.erase(pairKey(keepId, removedId));
        }

        // معالجات الفيديو (التدوير والإضاءة وغيرها) تسبق كل شيء، فالإطار المعالَج هو الذي يُحلَّل ويُعرض
        const VideoFilters::Settings filters = VideoFilters::current();
        if (!filters.isIdentity())
            frame = VideoFilters::apply(frame, filters);
        if (frame.size() != filteredSize) {
            tracks.clear(); // تغيّر اتجاه الإطار أو أبعاده
            stillReference.release();
            filteredSize = frame.size();
        }

        // الإطار المحسّن (إن طُلب) هو الذي يُحلَّل ويُعرض، وقد يكون أكبر من الأصلي. وفي الفيديو المائل يُحلَّل
        // معتدلاً، ومواضع الوجوه فيه تُعاد إلى اتجاه الإطار بـ shown
        const cv::Mat upright = turned(frame, turns);
        const cv::Mat work = enhance ? ImageIO::enhance(upright) : upright;
        const double scale = double(work.cols) / double(upright.cols);
        const auto shown = [&](const QRectF &box) { return unturned(box, turns, work.cols, work.rows); };
        region = turnedRegion(region, turns);
        if (scale != workScale) {
            tracks.clear(); // تغيّر مقياس الإحداثيات
            workScale = scale;
        }
        if (sceneCuts.update(frame)) {
            tracks.clear(); // لقطة جديدة: لا يُفترض أن الوجه في الموضع نفسه هو الشخص نفسه
            if (!camera && processed > 1)
                emit shotChanged(positionMs);
            else if (camera)
                emit cameraSceneCut();
        }

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

        // تخطي الساكن: كاميرات المراقبة تصوّر مكاناً فارغاً أغلب الوقت، فلا يُبحث عن وجوه في إطار لم
        // يتغير عن آخر إطار فُحص ما دام لا يوجد وجه متتبَّع؛ أي حركة تعيد الفحص فوراً
        bool still = false;
        if (filters.skipStill || quick) {
            cv::Mat reduced, gray;
            reduced = ImageIO::shrink(frame, cv::Size(96, std::max(1, 96 * frame.rows / std::max(1, frame.cols))));
            cv::cvtColor(reduced, gray, cv::COLOR_BGR2GRAY);
            const bool tracking = std::any_of(tracks.begin(), tracks.end(), [&](const Track &t) {
                return !t.closed && processed - t.lastSeen <= MaxTrackGap;
            });
            if (!tracking && !stillReference.empty() && stillReference.size() == gray.size() && stillSkips < MaxStillSkips)
                still = cv::norm(gray, stillReference, cv::NORM_L1) / double(gray.total()) < StillDifference;
            if (still) {
                ++stillSkips;
            } else {
                stillReference = gray;
                stillSkips = 0;
            }
        }

        std::vector<Detection> detections;
        const double minFace = filters.minFaceSize * scale;
        const bool fastDetection = fastAllowed && m_engine->gpuName().isEmpty();
        const cv::Mat scene = cropped ? work(area) : work;
        std::vector<cv::Rect> regions;
        const int sceneSide = std::max(scene.cols, scene.rows);
        const double fullScale = double(DetectionSide) / std::max(1, sceneSide);
        if (!still && !fastDetection && fullScale < 0.7) {
            // في الكشف الدقيق: الإطار الكبير يُصغَّر للكشف فتختفي فيه الوجوه البعيدة (كمشهد شارع)، فيُفحص معه
            // في كل تحليل جزء واحد منه بدقته الأصلية بالتناوب، ويُمسح الإطار كله خلال بضعة تحليلات
            const int side = std::max(800, sceneSide / 3);
            const int stride = side - TileOverlap;
            const int columns = std::max(1, (scene.cols - TileOverlap + stride - 1) / stride);
            const int rows = std::max(1, (scene.rows - TileOverlap + stride - 1) / stride);
            const int tile = tileTurn++ % (columns * rows);
            regions.emplace_back(std::min((tile % columns) * stride, std::max(0, scene.cols - side)),
                                 std::min((tile / columns) * stride, std::max(0, scene.rows - side)), side, side);

            // والوجه الصغير الذي التُقط يُبحث عنه حول موضعه الأخير في كل تحليل حتى لا يضيع بين دورات المسح
            std::vector<const Track *> faint;
            for (const Track &t : tracks) {
                if (!t.closed && processed - t.lastSeen <= MaxTrackGap
                    && std::min(t.box.width(), t.box.height()) * fullScale < SmallFaceSide)
                    faint.push_back(&t);
            }
            std::sort(faint.begin(), faint.end(), [](const Track *a, const Track *b) { return a->lastSeen > b->lastSeen; });
            for (size_t i = 0; i < faint.size() && i < size_t(MaxFaceRegions); ++i) {
                const QRectF &box = faint[i]->box;
                const int around = int(std::max(128.0, 4.0 * std::max(box.width(), box.height())));
                const QPointF center = box.center() - QPointF(area.x, area.y);
                regions.emplace_back(int(center.x()) - around / 2, int(center.y()) - around / 2, around, around);
            }
        }
        for (DetectedFace face : still ? std::vector<DetectedFace>() : m_engine->detect(scene, regions, 0.45f, fastDetection)) {
            if (minFace > 0 && std::min(face.box.width, face.box.height) < minFace)
                continue;
            face.box.x += float(area.x);
            face.box.y += float(area.y);
            for (cv::Point2f &point : face.landmarks)
                point += cv::Point2f(float(area.x), float(area.y));
            if (!zones.accepts(QRectF(face.box.x, face.box.y, face.box.width, face.box.height), QSize(work.cols, work.rows)))
                continue;
            Detection detection;
            detection.face = face;
            detection.aligned = m_engine->align(work, face);
            detection.quality = m_engine->quality(face, detection.aligned);
            detections.push_back(std::move(detection));
        }
        // في الوضع التلقائي: مشهد بلا وجوه لفترة يُفحص بمعدل أقل حتى يظهر وجه
        idleAnalyses = detections.empty() ? idleAnalyses + 1 : 0;
        if (automatic) {
            int wanted = idleAnalyses >= IdleAfter ? idleStep : step;
            if (adaptive && analysisCostMs > 0.0) {
                const int needed = int(std::ceil(analysisCostMs * rate / 1000.0));
                // والعرض يجري يتباعد التحليل أكثر ليبقى معه، وعند توقفه يعود إلى الكثافة المعتادة
                const int slowest = follow && m_displayPlaying ? playingSlowestStep : slowestStep;
                wanted = std::max(wanted, std::clamp(needed, step, slowest));
            }
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

        // المواضع على الإطار بعد المعالجات ودون التحسين، مصغراً كما يعرضه VideoPlayer
        const double displayScale = std::min(1.0, double(MaxDisplaySide) / std::max(frame.cols, frame.rows)) / scale;
        const auto displayBox = [&](const Detection &detection) {
            const QRectF box = shown(toRect(detection.face.box));
            return QRectF(box.topLeft() * displayScale, box.size() * displayScale);
        };

        // الكاميرا: مواضع الوجوه المتتبَّعة تُرسل فور كشفها، فلا ينتظر المربع حساب البصمات (وهو الأبطأ على
        // المعالج) ليلحق الوجه المتحرك؛ النتيجة الكاملة تتبعها في frameReady
        if (camera) {
            QList<FaceItem> moved;
            for (const Detection &detection : detections) {
                if (detection.track < 0)
                    continue;
                const Identity &identity = identities[tracks[size_t(detection.track)].identity];
                if (!identity.confirmed)
                    continue;
                FaceItem item;
                item.id = identity.id;
                item.box = displayBox(detection);
                item.quality = detection.quality;
                item.frames = identity.frames;
                item.match = identity.match;
                moved.append(item);
            }
            if (!moved.isEmpty())
                emit facesMoved(moved);
        }

        // 2) البصمة تُحسب للوجوه الجديدة، وللمتتبَّعة كل عدة إطارات، أو عند ربط غير مؤكد
        //    (مسار حديث، تداخل ضعيف، عودة بعد غياب) أو ظهور لقطة أوضح
        // والعرض يجري: عدد البصمات في الإطار محدود بما يتسع له أبعد تباعد بين إطارين محللين، فلا يتأخر
        // التحليل عن الإطار المعروض إن ظهرت وجوه كثيرة معاً؛ وعلى جهاز بلغ هذا التباعد يُكتفى للوجه المتتبَّع
        // المعتمد بالموضع، وبصمته كل عدة إطارات للتحقق أنه الشخص نفسه
        const bool playing = follow && m_displayPlaying;
        const bool struggling = playing && analysisCostMs * rate / 1000.0 > double(slowestStep);
        for (Detection &detection : detections) {
            if (detection.track < 0) {
                detection.needsEmbedding = true;
                continue;
            }
            const Track &track = tracks[size_t(detection.track)];
            const Identity &identity = identities[track.identity];
            const bool due = processed - track.lastEmbedded >= (quick ? QuickEmbedInterval : EmbedInterval);
            if (struggling) {
                detection.needsEmbedding = track.embedding.empty() || due || (!identity.confirmed && detection.iou < 0.5f);
                continue;
            }
            detection.needsEmbedding = track.embedding.empty() || track.hits < MinTrackHits
                || detection.iou < 0.5f || processed - track.lastSeen > 1 || due
                || detection.quality > identity.best.quality + BetterShotMargin;
        }
        if (playing && embedCostMs > 0.0) {
            const double spacingMs = std::max(double(FollowMaxSpacingMs), currentStep * 1000.0 / rate);
            const int budget = std::max(0, int((spacingMs - baseCostMs) / embedCostMs));
            // الأولوية للوجوه الجديدة (بلا بصمة لا تظهر) وواحد منها على الأقل، ثم الأوضح؛ البقية في الإطارات التالية
            std::vector<Detection *> wanted;
            for (Detection &detection : detections) {
                if (detection.needsEmbedding)
                    wanted.push_back(&detection);
            }
            std::stable_sort(wanted.begin(), wanted.end(), [](const Detection *a, const Detection *b) {
                if ((a->track < 0) != (b->track < 0))
                    return a->track < 0;
                return a->quality > b->quality;
            });
            const bool anyNew = !wanted.empty() && wanted.front()->track < 0;
            for (size_t i = size_t(std::max(budget, anyNew ? 1 : 0)); i < wanted.size(); ++i)
                wanted[i]->needsEmbedding = false;
        }
        std::vector<cv::Mat> toEmbed;
        for (const Detection &detection : detections) {
            if (detection.needsEmbedding)
                toEmbed.push_back(detection.aligned);
        }
        // دون الصورة المعكوسة: الوجه يظهر في إطارات كثيرة بزوايا مختلفة فلا تضيف دقة تُذكر، وتضاعف الزمن
        QElapsedTimer embedClock;
        embedClock.start();
        const std::vector<std::vector<float>> embeddings = m_engine->embedMany(toEmbed, false, !quick);
        frameEmbedMs = double(embedClock.nsecsElapsed()) / 1e6;
        if (!toEmbed.empty()) {
            const double each = frameEmbedMs / double(toEmbed.size());
            embedCostMs = embedCostMs > 0.0 ? embedCostMs * 0.8 + each * 0.2 : each;
        }
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
            if (identity.firstSeenMs < 0 || positionMs < identity.firstSeenMs)
                identity.firstSeenMs = positionMs;
            identity.lastSeenMs = std::max(identity.lastSeenMs, positionMs);
            addAppearance(identity, positionMs);
            identity.changed = true;

            int recentSightings = 0;
            if (camera) {
                identity.recentMs.push_back(positionMs);
                if (int(identity.recentMs.size()) > MinTrackHits)
                    identity.recentMs.pop_front();
                for (const qint64 ms : identity.recentMs)
                    recentSightings += positionMs - ms <= CameraConfirmWindowMs ? 1 : 0;
            }
            bool firstConfirmation = false;
            if (!identity.confirmed
                && ((track.hits >= MinTrackHits && track.hits * 2 >= processed - track.firstSeen + 1)
                    || recentSightings >= MinTrackHits)) {
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
                    const QRectF box = shown(toRect(detection.face.box));
                    identity.sourceBox = QRectF(box.topLeft() / workScale, box.size() / workScale);
                    betterShot = true;
                }
            }
            if (camera && !identity.confirmed && gallery && !detection.embedding.empty()
                && std::max(track.hits, recentSightings) >= KnownTrackHits) {
                const MatchResult known = FaceDatabase::identify(*gallery, identity.mean, threshold);
                if (known.status == MatchStatus::Confirmed) {
                    identity.confirmed = true;
                    firstConfirmation = true;
                    identity.match = known;
                    identity.identifiedMean = identity.mean;
                    identity.matchStale = false;
                    identity.forceIdentify = false;
                }
            }
            if (!identity.confirmed)
                continue;

            // تحديث البطاقة عند الاعتماد أو ظهور لقطة أوضح، وإلا كل عدة إطارات فقط
            if (firstConfirmation || betterShot || processed - identity.lastEmitted >= UpdateInterval)
                emitUpdate(identity);

            FaceItem item;
            item.id = identity.id;
            item.box = displayBox(detection);
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

        if (camera && processed % MergeInterval == 0 && identities.size() > MaxCameraIdentities) {
            std::vector<std::pair<qint64, int>> oldest;
            for (const auto &[id, identity] : identities) {
                const bool tracked = std::any_of(tracks.begin(), tracks.end(), [id = id](const Track &t) { return t.identity == id; });
                if (!tracked && positionMs - identity.lastSeenMs >= CameraForgetMs)
                    oldest.emplace_back(identity.lastSeenMs, id);
            }
            std::sort(oldest.begin(), oldest.end());
            const size_t target = MaxCameraIdentities * 4 / 5;
            QList<int> forgotten;
            for (const auto &[seen, id] : oldest) {
                if (identities.size() <= target)
                    break;
                identities.erase(id);
                forgotten.append(id);
            }
            if (!forgotten.isEmpty()) {
                for (auto it = together.begin(); it != together.end();) {
                    const int a = int(quint32(*it >> 32));
                    const int b = int(quint32(*it));
                    it = forgotten.contains(a) || forgotten.contains(b) ? together.erase(it) : std::next(it);
                }
                emit identitiesForgotten(forgotten);
            }
        }

        double progress = -1.0;
        if (!camera && endFrame > startFrame) {
            const int done = follow ? coveredFrames + frameIndex - runStart : frameIndex - startFrame;
            progress = std::clamp(double(done) / double(endFrame - startFrame), 0.0, 1.0);
        }
        if (!camera && validFps && endFrame > startFrame) {
            QList<QPair<qint64, qint64>> ranges;
            const auto toMs = [&](int index) { return qint64(index * 1000.0 / fps); };
            const auto add = [&](int first, int last) {
                if (last <= first)
                    return;
                if (!ranges.isEmpty() && ranges.back().second >= toMs(first))
                    ranges.back().second = std::max(ranges.back().second, toMs(last));
                else
                    ranges.append({toMs(first), toMs(last)});
            };
            const int runFirst = follow ? runStart : startFrame;
            const int runLast = std::min(frameIndex, endFrame);
            bool runAdded = false;
            if (follow) {
                for (const auto &[first, last] : covered) {
                    if (!runAdded && runFirst <= first) {
                        add(runFirst, runLast);
                        runAdded = true;
                    }
                    add(first, last);
                }
            }
            if (!runAdded)
                add(runFirst, runLast);
            emit analyzedRanges(ranges);
        }
        if (!deliverFrames) {
            // العرض من VideoPlayer: النتيجة وحدها بلا صورة، فلا ينتظر التحليل الواجهة
            emit frameReady(QImage(), current, positionMs, progress);
            const qint64 spacing = std::max<qint64>(1000, durationMs / 600);
            const bool due = quick ? previewClock.elapsed() - lastPreview >= QuickPreviewMs : positionMs - lastThumbnailMs >= spacing;
            if (due) {
                lastThumbnailMs = positionMs;
                lastPreview = previewClock.elapsed();
                const double thumbnailScale = std::min(1.0, double(ThumbnailHeight) / std::max(1, frame.rows));
                const cv::Mat thumbnail = ImageIO::shrink(frame, cv::Size(std::max(1, cvRound(frame.cols * thumbnailScale)),
                                                                          std::max(1, cvRound(frame.rows * thumbnailScale))));
                emit thumbnailReady(positionMs, ImageIO::toQImage(thumbnail));
            }
        } else if (m_pendingFrames < MaxPendingFrames) {
            // لا تُرسل إطارات جديدة إذا كانت الواجهة متأخرة في عرض السابقة
            ++m_pendingFrames;
            const double frameScale = std::min(1.0, double(MaxDisplaySide) / std::max(frame.cols, frame.rows));
            cv::Mat display = frame;
            if (frameScale < 1.0)
                display = ImageIO::shrink(frame, cv::Size(std::max(1, cvRound(frame.cols * frameScale)), std::max(1, cvRound(frame.rows * frameScale))));
            emit frameReady(ImageIO::toQImage(display), current, positionMs, progress);
        }
        const double cost = double(analysisClock.nsecsElapsed()) / 1e6;
        analysisCostMs = analysisCostMs > 0.0 ? analysisCostMs * 0.8 + cost * 0.2 : cost;
        const double base = std::max(0.0, cost - frameEmbedMs);
        baseCostMs = baseCostMs > 0.0 ? baseCostMs * 0.8 + base * 0.2 : base;
        frameEmbedMs = 0.0;
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
    {
        QMutexLocker locker(&m_mutex);
        m_lastCameraFrame.release();
    }
    emit finished(endError);
}

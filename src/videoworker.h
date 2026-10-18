#pragma once

#include "camerarules.h"

#include "facedatabase.h"
#include "facetypes.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QRectF>

#include <atomic>
#include <memory>

#include <opencv2/core.hpp>

class CameraRecorder;
class FaceEngine;
namespace Cameras {
class FrameSlot;
}

// يحلل الفيديو أو الكاميرا في خيط منفصل ويتتبع كل وجه عبر الإطارات
class VideoWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoWorker(const FaceEngine *engine, QObject *parent = nullptr);

    // آمنة للاستدعاء من أي خيط
    void setGallery(GalleryPtr gallery);
    void setThreshold(float threshold);
    void setFrameStep(int step); // 0: تلقائي حسب سرعة إطارات الفيديو
    // جزء الفيديو المطلوب تحليله بالمللي ثانية؛ end = 0 حتى النهاية. يُطبَّق عند البدء
    void setRange(qint64 startMs, qint64 endMs);
    // منطقة من الإطار (إحداثيات نسبية من 0 إلى 1) لا يُبحث عن الوجوه خارجها؛ فارغة = الإطار كله
    void setRegion(const QRectF &region);
    // تحسين كل إطار وتوضيحه قبل التحليل (أبطأ، ويفيد الفيديو الصغير أو المظلم)
    void setEnhance(bool enhance);
    // مناطق الكاميرا: الوجوه خارجها (أو داخل منطقة متجاهلة) لا تُحلَّل
    void setZones(const CameraRules::Rules &rules);
    // دمج هويتين يدوياً أثناء التحليل
    void mergeIdentities(int keepId, int removedId);
    void requestStop();
    // يُستدعى من الواجهة قبل process، فلا يضيع طلب إيقاف يصل قبل أن يبدأ التحليل
    void prepareStart() { m_stop = false; }
    void setPaused(bool paused); // إيقاف مؤقت دون فقدان التقدم أو الأشخاص المكتشفين
    bool isPaused() const { return m_paused; }
    void frameShown();
    // يتسلم كل إطار من الكاميرا ليسجله؛ يُضبط مرة قبل التشغيل ويبقى حياً بعد العامل
    void setRecorder(CameraRecorder *recorder) { m_recorder = recorder; }
    // آخر إطار من الكاميرا كما وصل قبل المعالجات (لمعاينة المعالجات أثناء البث)
    cv::Mat lastCameraFrame();
    // يتسلم كل إطار من الكاميرا للعرض بسرعتها (VideoPlayer)؛ يُضبط قبل التشغيل
    void setLiveFrames(std::shared_ptr<Cameras::FrameSlot> slot);
    // false: frameReady للملفات بلا صورة ولا انتظار للواجهة، لأن العرض من VideoPlayer
    // (الكاميرا ترسل الصورة دائماً للقطات التنبيهات)
    void setDeliverFrames(bool deliver) { m_deliverFrames = deliver; }
    // الوضع التلقائي للملفات: يقل عدد الإطارات المحللة على الجهاز البطيء ليبقى التحليل قريباً من التشغيل
    void setAdaptive(bool adaptive) { m_adaptive = adaptive; }
    // موضع الإطار المعروض الآن في VideoPlayer (أو -1): في الوضع التلقائي يلحق التحليل العرض إن سبقه إلى
    // جزء لم يُحلَّل، ثم يعود إلى ما تخطاه
    void setDisplayPosition(qint64 positionMs, bool playing);
    // التحليل الجاري يقفز إلى موضع العرض إن سبقه (خطوة الإطارات تلقائية والملف معروف الطول)
    bool followsDisplay() const { return m_following; }
    // فحص الأرشيف السريع للملفات: 4 إطارات محللة في الثانية، وبصمة واحدة لكل وجه، وتخطّي الساكن دائماً،
    // وصورة معاينة كل ثانية من الوقت الفعلي بدل كل ثانية من الفيديو
    void setQuickScan(bool quick) { m_quick = quick; }

public slots:
    void process(const QString &source); // مسار ملف، أو "camera:<رقم أو معرّف كاميرا محفوظة>"

signals:
    void started(int frameStep, double fps, qint64 durationMs);
    // progress من 0 إلى 1 ضمن الجزء المطلوب (سالب للكاميرا). مواضع الوجوه على الإطار بعد المعالجات
    // مصغراً إلى 1920 بكسل على الأكثر (كما يعرضه VideoPlayer)، وframe فارغ إن لم يُطلب
    void frameReady(const QImage &frame, const QList<FaceItem> &faces, qint64 positionMs, double progress);
    // صورة مصغرة لشريط الزمن، كل ثانية تقريباً من الجزء المحلل (للملفات حين لا تُرسل الإطارات)
    void thumbnailReady(qint64 positionMs, const QImage &thumbnail);
    // الأجزاء المحللة من الملف حتى الآن [بداية، نهاية) بالمللي ثانية، مرتبة ولا تتداخل
    void analyzedRanges(const QList<QPair<qint64, qint64>> &ranges);
    // الكاميرا: مواضع الوجوه المتتبَّعة في الإطار الجاري قبل اكتمال تحليله (بلا صورة)، ليلحق المربع الوجه
    // المتحرك دون انتظار البصمات؛ يتبعها frameReady للإطار نفسه
    void facesMoved(const QList<FaceItem> &faces);
    void trackUpdated(const FaceItem &track);
    // تغيّرت اللقطة عند هذا الموضع (قطع في المونتاج أو تبدّل الكاميرا)، للملفات فقط
    void shotChanged(qint64 positionMs);
    // تبيّن أن هويتين لشخص واحد فدُمجتا؛ removedId لم يعد موجوداً
    void identitiesMerged(int keepId, int removedId);
    // الكاميرا: أشخاص غابوا طويلاً فنُسوا لتبقى الذاكرة ثابتة؛ تُحذف بطاقاتهم
    void identitiesForgotten(const QList<int> &ids);
    void finished(const QString &error);
    // انقطعت الكاميرا وتجري إعادة الاتصال (المحاولة attempt)، ثم عادت
    void connectionLost(int attempt);
    void connectionRestored();

private:
    void run(const QString &source);
    const FaceEngine *m_engine;
    CameraRecorder *m_recorder = nullptr;
    QMutex m_mutex;
    GalleryPtr m_gallery;
    float m_threshold = 0.30f;
    QRectF m_region;
    CameraRules::Rules m_zones;
    bool m_enhance = false;
    QList<QPair<int, int>> m_manualMerges;
    qint64 m_startMs = 0;
    qint64 m_endMs = 0;
    std::atomic<int> m_frameStep{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int> m_pendingFrames{0};
    std::atomic<bool> m_deliverFrames{true};
    std::atomic<bool> m_adaptive{false};
    std::atomic<bool> m_quick{false};
    std::atomic<qint64> m_displayMs{-1};
    std::atomic<bool> m_displayPlaying{false};
    std::atomic<bool> m_following{false};
    cv::Mat m_lastCameraFrame; // يحميه m_mutex
    std::shared_ptr<Cameras::FrameSlot> m_liveFrames; // يحميه m_mutex
};

#pragma once

#include "facedatabase.h"
#include "facetypes.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QRectF>

#include <atomic>

#include <opencv2/core.hpp>

class FaceEngine;

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
    // دمج هويتين يدوياً أثناء التحليل
    void mergeIdentities(int keepId, int removedId);
    void requestStop();
    // يُستدعى من الواجهة قبل process، فلا يضيع طلب إيقاف يصل قبل أن يبدأ التحليل
    void prepareStart() { m_stop = false; }
    void setPaused(bool paused); // إيقاف مؤقت دون فقدان التقدم أو الأشخاص المكتشفين
    bool isPaused() const { return m_paused; }
    void frameShown();
    // آخر إطار من الكاميرا كما وصل قبل المعالجات (لمعاينة المعالجات أثناء البث)
    cv::Mat lastCameraFrame();

public slots:
    void process(const QString &source); // مسار ملف، أو "camera:<رقم أو معرّف كاميرا محفوظة>"

signals:
    void started(int frameStep, double fps, qint64 durationMs);
    // progress من 0 إلى 1 ضمن الجزء المطلوب (سالب للكاميرا)
    void frameReady(const QImage &frame, const QList<FaceItem> &faces, qint64 positionMs, double progress);
    void trackUpdated(const FaceItem &track);
    // تبيّن أن هويتين لشخص واحد فدُمجتا؛ removedId لم يعد موجوداً
    void identitiesMerged(int keepId, int removedId);
    void finished(const QString &error);
    // انقطعت الكاميرا وتجري إعادة الاتصال (المحاولة attempt)، ثم عادت
    void connectionLost(int attempt);
    void connectionRestored();

private:
    const FaceEngine *m_engine;
    QMutex m_mutex;
    GalleryPtr m_gallery;
    float m_threshold = 0.30f;
    QRectF m_region;
    bool m_enhance = false;
    QList<QPair<int, int>> m_manualMerges;
    qint64 m_startMs = 0;
    qint64 m_endMs = 0;
    std::atomic<int> m_frameStep{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int> m_pendingFrames{0};
    cv::Mat m_lastCameraFrame; // يحميه m_mutex
};

#pragma once

#include "camerarules.h"
#include "facetypes.h"
#include "recorder.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <vector>

class FaceDatabase;

// منطق مراقبة الكاميرا المشترك بين عرض الكاميرا الواحدة ونافذة عدة كاميرات، لكل كاميرا نسخة منه:
//   - لا يُعد الشخص ظاهراً إلا إذا تأكد التعرف عليه في إطارين محلَّلين متقاربين (أو تشابه قوي جداً
//     في إطار واحد)، فلا يطلق وجه مارّ يشبه أحد المراقَبين لحظةً تنبيهاً خاطئاً
//   - كل ظهور يُكتب في سجل الشخص مع لقطة الإطار وموضعه في تسجيل الكاميرا، ويتكرر إن غاب ReappearMs
//   - الوجوه غير المسجلة تُجمع زواراً مجهولين ببصماتها (الظهور المتكرر لوجه واحد تحت رقم واحد)
//   - قواعد الكاميرا: الوجوه خارج مناطقها لا تُحسب، والتنبيه في ساعات جدولها فقط
//   - تنبيه لمجهول تكرر ظهوره كثيراً في يوم أو ظهر ليلاً
class CameraWatch : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 ReappearMs = 5 * 60 * 1000;
    // تأكيدان في إطارين بينهما أقل من هذه المدة
    static constexpr int ConfirmHits = 2;
    static constexpr qint64 ConfirmGapMs = 10 * 1000;
    // تشابه يزيد على حد التعرف بهذا القدر يكفي وحده
    static constexpr float StrongMargin = 0.15f;
    // أقل جودة لوجه مجهول حتى يُحفظ زائراً (الوجه المشوش أو الجانبي جداً لا يُعرف لاحقاً)
    static constexpr float VisitorMinQuality = 0.3f;

    struct Face
    {
        QRectF box; // بإحداثيات الإطار
        MatchResult match;
        std::vector<float> embedding;
        float quality = 0.0f;
        int track = -1; // رقم مسار الوجه إن كان التتبع متاحاً (الكاميرا الواحدة)، وإلا -1
        QImage crop;    // صورة أوضح للوجه إن توفرت، وإلا تُقتطع من الإطار
    };

    explicit CameraWatch(FaceDatabase *database, QObject *parent = nullptr);

    // كاميرا جديدة: source "camera:…"، recorder تسجيلها الجاري إن وُجد (لربط الظهور بموضعه فيه)
    void reset(const QString &source, const QString &cameraName, CameraRecorder *recorder);
    void setWatched(const QSet<QString> &ids) { m_watched = ids; }
    void setThreshold(float threshold) { m_threshold = threshold; }

    // لكل إطار حُلِّل (من خيط الواجهة)
    void process(const QImage &frame, const QList<Face> &faces);

    // مربعات الوجوه للتسجيل بإحداثيات نسبية
    QList<CameraRecorder::OverlayBox> overlay(const QSize &frameSize, const QList<Face> &faces) const;
    bool isWatchedMatch(const MatchResult &match) const;

    // حفظ الزوار المجهولين (إعداد عام، مفعّل افتراضياً)
    static bool visitorsEnabled();
    static void setVisitorsEnabled(bool enabled);

    // تنبيه المجهول المتكرر (إعداد عام): dailyCount ظهور في اليوم نفسه (0 = لا تنبيه)، أو ظهور في ساعات الليل
    struct StrangerAlerts
    {
        int dailyCount = 5;
        bool night = false;
        CameraRules::Window nightWindow{true, QTime(23, 0), QTime(5, 0)};
    };
    static StrangerAlerts strangerAlerts();
    static void setStrangerAlerts(const StrangerAlerts &settings);

    // تُقرأ قواعد الكاميرا عند reset؛ تُستدعى بعد تعديلها والكاميرا تعمل
    void reloadRules();
    const CameraRules::Rules &rules() const { return m_rules; }
    bool alertsActive() const { return m_rules.alerts.active(); }

signals:
    // ظهر شخص من قائمة المراقبة (ظهور مؤكد جديد)
    void watchAlert(const QString &personId, const QString &name, float score, const QImage &crop);
    // ظهور مؤكد جديد لشخص معروف
    void sighted(const QString &personId, const QString &name);
    void visitorSeen(qint64 visitorId, bool isNew);
    // مجهول ظهوره مريب (تكرر كثيراً اليوم أو ظهر ليلاً)؛ reason وصف قصير
    void strangerAlert(qint64 visitorId, const QString &reason, const QImage &crop);

private:
    struct Presence
    {
        int hits = 0;
        qint64 lastHit = 0;
        qint64 lastSeen = -1; // آخر مرة رُئي فيها بعد تأكيد ظهوره
    };

    struct Stranger
    {
        std::vector<float> embedding;
        int track = -1;
        Presence presence;
        qint64 visitorId = -1;
    };

    bool confirm(Presence &presence, qint64 now, bool strong);
    QImage cropOf(const QImage &frame, const Face &face) const;
    void noteStranger(const QImage &frame, const Face &face, qint64 now);
    CameraRecorder::Position recordingPosition() const;
    void checkStranger(qint64 visitorId, const QImage &crop);

    FaceDatabase *m_database;
    QString m_source;
    QString m_cameraName;
    QPointer<CameraRecorder> m_recorder;
    QSet<QString> m_watched;
    float m_threshold = 0.5f;
    QHash<QString, Presence> m_known;
    std::vector<Stranger> m_strangers;
    CameraRules::Rules m_rules;
};

#include "camerawatch.h"

#include "faceengine.h"
#include "facedatabase.h"

#include <QDateTime>
#include <QMutex>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <optional>

namespace {

constexpr size_t MaxStrangers = 200;

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

} // namespace

CameraWatch::CameraWatch(FaceDatabase *database, QObject *parent)
    : QObject(parent)
    , m_database(database)
{
}

namespace {

// يُقرآن مع كل إطار من كل كاميرا، فيُحفظان في الذاكرة بدل فتح ملف الإعدادات كل مرة؛ يتغيران من هنا فقط
QMutex cacheMutex;
int visitorsCache = -1;
std::optional<CameraWatch::StrangerAlerts> strangerCache;

} // namespace

bool CameraWatch::visitorsEnabled()
{
    QMutexLocker locker(&cacheMutex);
    if (visitorsCache < 0)
        visitorsCache = QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("saveVisitors"), true).toBool();
    return visitorsCache == 1;
}

void CameraWatch::setVisitorsEnabled(bool enabled)
{
    QMutexLocker locker(&cacheMutex);
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("saveVisitors"), enabled);
    visitorsCache = enabled;
}

CameraWatch::StrangerAlerts CameraWatch::strangerAlerts()
{
    QMutexLocker locker(&cacheMutex);
    if (strangerCache)
        return *strangerCache;
    QSettings settings(settingsPath(), QSettings::IniFormat);
    StrangerAlerts result;
    result.dailyCount = std::max(0, settings.value(QStringLiteral("strangerDailyCount"), result.dailyCount).toInt());
    result.night = settings.value(QStringLiteral("strangerNight"), result.night).toBool();
    const QTime from = QTime::fromString(settings.value(QStringLiteral("strangerNightFrom")).toString(), QStringLiteral("HH:mm"));
    const QTime to = QTime::fromString(settings.value(QStringLiteral("strangerNightTo")).toString(), QStringLiteral("HH:mm"));
    if (from.isValid())
        result.nightWindow.from = from;
    if (to.isValid())
        result.nightWindow.to = to;
    strangerCache = result;
    return result;
}

void CameraWatch::setStrangerAlerts(const StrangerAlerts &value)
{
    QMutexLocker locker(&cacheMutex);
    strangerCache.reset();
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("strangerDailyCount"), std::max(0, value.dailyCount));
    settings.setValue(QStringLiteral("strangerNight"), value.night);
    settings.setValue(QStringLiteral("strangerNightFrom"), value.nightWindow.from.toString(QStringLiteral("HH:mm")));
    settings.setValue(QStringLiteral("strangerNightTo"), value.nightWindow.to.toString(QStringLiteral("HH:mm")));
}

void CameraWatch::reloadRules()
{
    m_rules = CameraRules::load(m_source);
}

void CameraWatch::reset(const QString &source, const QString &cameraName, CameraRecorder *recorder)
{
    m_source = source;
    m_rules = CameraRules::load(source);
    m_cameraName = cameraName;
    m_recorder = recorder;
    m_known.clear();
    m_strangers.clear();
}

bool CameraWatch::isWatchedMatch(const MatchResult &match) const
{
    return match.status == MatchStatus::Confirmed && m_watched.contains(match.personId);
}

QList<CameraRecorder::OverlayBox> CameraWatch::overlay(const QSize &frameSize, const QList<Face> &faces) const
{
    QList<CameraRecorder::OverlayBox> boxes;
    if (frameSize.width() <= 0 || frameSize.height() <= 0)
        return boxes;
    const double w = frameSize.width(), h = frameSize.height();
    for (const Face &face : faces) {
        const QRectF box(face.box.x() / w, face.box.y() / h, face.box.width() / w, face.box.height() / h);
        boxes.append({box, matchLabel(face.match), isWatchedMatch(face.match) ? QColor(0xff, 0x5c, 0x5c) : matchColor(face.match.status)});
    }
    return boxes;
}

CameraRecorder::Position CameraWatch::recordingPosition() const
{
    // موضع الظهور في التسجيل الجاري، قبله بثانيتين ليظهر الشخص داخلاً
    CameraRecorder::Position position = m_recorder ? m_recorder->position() : CameraRecorder::Position();
    if (position.ms >= 0)
        position.ms = std::max<qint64>(0, position.ms - 2000);
    return position;
}

bool CameraWatch::confirm(Presence &presence, qint64 now, bool strong)
{
    // ما دام ظاهراً (أو غاب أقل من ReappearMs) فهو الظهور نفسه
    if (presence.lastSeen >= 0 && now - presence.lastSeen < ReappearMs) {
        presence.lastSeen = now;
        return false;
    }
    if (now - presence.lastHit > ConfirmGapMs)
        presence.hits = 0;
    ++presence.hits;
    presence.lastHit = now;
    if (presence.hits < ConfirmHits && !strong)
        return false;
    presence.hits = 0;
    presence.lastSeen = now;
    return true;
}

QImage CameraWatch::cropOf(const QImage &frame, const Face &face) const
{
    if (!face.crop.isNull())
        return face.crop;
    if (!face.packedCrop.isEmpty()) {
        const QImage unpacked = QImage::fromData(face.packedCrop, "JPG");
        if (!unpacked.isNull())
            return unpacked;
    }
    const QRectF box = face.box;
    return frame.copy(box.adjusted(-box.width() * 0.3, -box.height() * 0.3, box.width() * 0.3, box.height() * 0.3)
                          .toAlignedRect()
                          .intersected(frame.rect()));
}

void CameraWatch::process(const QImage &frame, const QList<Face> &faces)
{
    if (!m_database)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool visitors = visitorsEnabled();
    const bool alerts = m_rules.alerts.active();
    for (const Face &face : faces) {
        // خارج مناطق الكاميرا (أو داخل منطقة متجاهلة): لا ظهور ولا تنبيه ولا زائر
        if (!m_rules.accepts(face.box, frame.size()))
            continue;
        if (face.match.status == MatchStatus::Unknown) {
            if (visitors)
                noteStranger(frame, face, now);
            continue;
        }
        // «محتمل» قد يكون الشخص نفسه أو غيره: لا يُسجَّل ظهوراً ولا زائراً مجهولاً
        if (face.match.status != MatchStatus::Confirmed || face.match.personId.isEmpty())
            continue;
        const bool strong = face.match.score >= m_threshold + StrongMargin;
        if (!confirm(m_known[face.match.personId], now, strong))
            continue;
        const QImage crop = cropOf(frame, face);
        const QString snapshot = frame.isNull() ? QString() : CameraRecorder::saveSnapshot(frame, face.box, face.match.name);
        const CameraRecorder::Position position = recordingPosition();
        m_database->addCameraSighting(face.match.personId, m_source, position.path, position.ms, face.match.score, crop, snapshot);
        emit sighted(face.match.personId, face.match.name);
        if (alerts && m_watched.contains(face.match.personId))
            emit watchAlert(face.match.personId, face.match.name, face.match.score, crop);
    }

    // من غاب طويلاً يُنسى، فيبقى البحث بين المجهولين سريعاً
    m_strangers.erase(std::remove_if(m_strangers.begin(), m_strangers.end(),
                                     [now](const Stranger &stranger) {
                                         const qint64 last = std::max(stranger.presence.lastSeen, stranger.presence.lastHit);
                                         return now - last > ReappearMs;
                                     }),
                      m_strangers.end());
}

void CameraWatch::noteStranger(const QImage &frame, const Face &face, qint64 now)
{
    if (face.embedding.size() != FaceEngine::EmbeddingSize || face.quality < VisitorMinQuality)
        return;
    std::vector<float> embedding = face.embedding;
    FaceEngine::normalize(embedding);

    // المسار نفسه أولاً (الكاميرا الواحدة تتبع الوجه)، وإلا أقرب مجهول ظهر هنا مؤخراً ببصمته
    Stranger *match = nullptr;
    if (face.track >= 0) {
        for (Stranger &stranger : m_strangers) {
            if (stranger.track == face.track) {
                match = &stranger;
                break;
            }
        }
    }
    if (!match) {
        float best = m_threshold;
        for (Stranger &stranger : m_strangers) {
            const float score = FaceEngine::dot(stranger.embedding.data(), embedding.data(), embedding.size());
            if (score >= best) {
                best = score;
                match = &stranger;
            }
        }
    }
    if (!match) {
        if (m_strangers.size() >= MaxStrangers) {
            const auto oldest = std::min_element(m_strangers.begin(), m_strangers.end(), [](const Stranger &a, const Stranger &b) {
                return std::max(a.presence.lastSeen, a.presence.lastHit) < std::max(b.presence.lastSeen, b.presence.lastHit);
            });
            m_strangers.erase(oldest);
        }
        m_strangers.push_back({embedding, face.track, {}, -1});
        match = &m_strangers.back();
    } else {
        // البصمة تتكيف مع الزاوية والإضاءة ما دام ظاهراً
        for (size_t i = 0; i < embedding.size(); ++i)
            match->embedding[i] = match->embedding[i] * 0.8f + embedding[i] * 0.2f;
        FaceEngine::normalize(match->embedding);
        if (face.track >= 0)
            match->track = face.track;
    }
    if (!confirm(match->presence, now, false))
        return;
    const QImage crop = cropOf(frame, face);
    const QString snapshot = frame.isNull() ? QString() : CameraRecorder::saveSnapshot(frame, face.box, QStringLiteral("مجهول"));
    const CameraRecorder::Position position = recordingPosition();
    bool isNew = false;
    const qint64 id = m_database->addVisitorSighting(match->embedding, face.quality, m_threshold, m_source, position.path, position.ms,
                                                     crop, snapshot, &isNew);
    if (id > 0) {
        match->visitorId = id;
        emit visitorSeen(id, isNew);
        checkStranger(id, crop);
    }
}

void CameraWatch::checkStranger(qint64 visitorId, const QImage &crop)
{
    if (!m_rules.alerts.active())
        return;
    const StrangerAlerts settings = strangerAlerts();
    // تنبيه واحد لكل زائر في اليوم لكل سبب، مشترك بين كل الكاميرات
    static QHash<qint64, QDate> dailyAlerted;
    static QHash<qint64, QDate> nightAlerted;
    const QDateTime now = QDateTime::currentDateTime();
    const QDate today = now.date();
    if (settings.night && settings.nightWindow.contains(now.time())) {
        // ليلة تعبر منتصف الليل تُحسب ليلة واحدة باسم يوم بدايتها
        const QDate night = settings.nightWindow.from > settings.nightWindow.to && now.time() < settings.nightWindow.to ? today.addDays(-1) : today;
        if (nightAlerted.value(visitorId) != night) {
            nightAlerted.insert(visitorId, night);
            emit strangerAlert(visitorId, QStringLiteral("ظهر ليلاً الساعة \u2066%1\u2069").arg(now.toString(QStringLiteral("HH:mm"))), crop);
            return;
        }
    }
    if (settings.dailyCount > 0 && dailyAlerted.value(visitorId) != today) {
        const int count = m_database->visitorSightingCount(visitorId, QDateTime(today, QTime(0, 0)));
        if (count >= settings.dailyCount) {
            dailyAlerted.insert(visitorId, today);
            emit strangerAlert(visitorId, QStringLiteral("ظهر %1 مرات اليوم").arg(count), crop);
        }
    }
}

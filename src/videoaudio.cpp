#include "videoaudio.h"

#include <QDateTime>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef MALAMIH_HAS_AUDIO
#include <QAudioDevice>
#include <QAudioOutput>
#include <QMediaDevices>
#include <QMediaPlayer>
#endif

namespace {

constexpr qint64 MaxDriftMs = 250;      // فرق أكبر بين الصوت والصورة يُصحَّح بالقفز
constexpr qint64 CorrectionGapMs = 1500; // أقل مدة بين تصحيحين

} // namespace

VideoAudio::VideoAudio(QObject *parent)
    : QObject(parent)
{
}

// المشغّل يُنشأ عند أول فيديو لا عند بدء البرنامج، فلا يتأخر فتحه ولا يتأثر بمكتبات الوسائط
void VideoAudio::ensurePlayer()
{
#ifdef MALAMIH_HAS_AUDIO
    // بلا جهاز صوت لا يُنشأ المشغّل أصلاً (بعض إصدارات Qt تتعطل حين تكتب إلى مخرج غير موجود)
    if (m_player || QMediaDevices::defaultAudioOutput().isNull())
        return;
    m_player = new QMediaPlayer(this);
    m_output = new QAudioOutput(this);
    m_output->setVolume(m_volume);
    m_output->setMuted(m_muted);
    m_player->setAudioOutput(m_output);
    connect(m_player, &QMediaPlayer::hasAudioChanged, this, [this](bool available) {
        m_hasAudio = available;
        emit availabilityChanged(available);
    });
#endif
}

VideoAudio::~VideoAudio() = default;

bool VideoAudio::supported()
{
#ifdef MALAMIH_HAS_AUDIO
    return true;
#else
    return false;
#endif
}

void VideoAudio::open(const QString &path)
{
    close();
#ifdef MALAMIH_HAS_AUDIO
    ensurePlayer();
    if (m_player)
        m_player->setSource(QUrl::fromLocalFile(path));
#else
    Q_UNUSED(path)
#endif
}

void VideoAudio::close()
{
#ifdef MALAMIH_HAS_AUDIO
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
#endif
    if (m_hasAudio) {
        m_hasAudio = false;
        emit availabilityChanged(false);
    }
}

void VideoAudio::sync(bool playing, qint64 positionMs, double rate)
{
#ifdef MALAMIH_HAS_AUDIO
    if (!m_hasAudio)
        return;
    // السرعات البعيدة عن الأصلية تُشغَّل بلا صوت كما في المشغلات المعروفة
    const bool audible = playing && rate >= 0.5 && rate <= 2.0;
    if (!audible) {
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        return;
    }
    if (std::abs(m_player->playbackRate() - rate) > 1e-3)
        m_player->setPlaybackRate(rate);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_player->playbackState() != QMediaPlayer::PlayingState) {
        m_player->setPosition(positionMs);
        m_player->play();
        m_lastCorrection = now;
    } else if (std::llabs(m_player->position() - positionMs) > MaxDriftMs && now - m_lastCorrection > CorrectionGapMs) {
        m_player->setPosition(positionMs);
        m_lastCorrection = now;
    }
#else
    Q_UNUSED(playing)
    Q_UNUSED(positionMs)
    Q_UNUSED(rate)
#endif
}

void VideoAudio::setVolume(int volume, bool muted)
{
    m_volume = float(std::clamp(volume, 0, 100)) / 100.0f;
    m_muted = muted;
#ifdef MALAMIH_HAS_AUDIO
    if (m_output) {
        m_output->setVolume(m_volume);
        m_output->setMuted(m_muted);
    }
#endif
}

#pragma once

#include <QObject>
#include <QString>

class QAudioOutput;
class QMediaPlayer;

// صوت الفيديو بجانب VideoPlayer: يُشغَّل الملف نفسه صوتاً فقط ويُضبط على موضع الصورة. يعمل حين
// يُبنى البرنامج مع Qt Multimedia، وإلا فلا صوت ويختفي زره
class VideoAudio : public QObject
{
    Q_OBJECT

public:
    explicit VideoAudio(QObject *parent = nullptr);
    ~VideoAudio() override;

    static bool supported();
    void open(const QString &path);
    void close();
    bool hasAudio() const { return m_hasAudio; }
    // يُستدعى مع كل إطار معروض وعند كل تغيير في التشغيل: الصوت يتبع الصورة لا العكس
    void sync(bool playing, qint64 positionMs, double rate);
    void setVolume(int volume, bool muted); // volume من 0 إلى 100
    // بعد القفز يُصحَّح الموضع مع الإطار التالي مباشرة
    void seeked() { m_lastCorrection = 0; }

signals:
    void availabilityChanged(bool hasAudio);

private:
    void ensurePlayer();

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_output = nullptr;
    bool m_hasAudio = false;
    float m_volume = 0.8f;
    bool m_muted = false;
    qint64 m_lastCorrection = 0; // آخر تصحيح للموضع، فلا يتقطع الصوت بتصحيحات متتالية
};

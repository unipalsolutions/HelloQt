#include "alertsound.h"

#include <QApplication>
#include <QByteArray>
#include <QSettings>
#include <QStandardPaths>
#include <QtEndian>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#include <algorithm>
#include <cmath>
#include <vector>

namespace AlertSound {

namespace {

constexpr int Rate = 22050;
constexpr double TwoPi = 6.283185307179586;

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

struct Tone
{
    double frequency; // 0 = صمت
    double seconds;
};

QByteArray render(const std::vector<Tone> &tones, double volume)
{
    std::vector<qint16> samples;
    for (const Tone &tone : tones) {
        const int count = int(tone.seconds * Rate);
        const int fade = std::min(count / 4, Rate / 100); // بلا طقطقة عند البداية والنهاية
        for (int i = 0; i < count; ++i) {
            double value = 0.0;
            if (tone.frequency > 0) {
                const double t = double(i) / Rate;
                // نغمة مع توافقية خفيفة أوضح على سماعات الحاسوب الصغيرة
                value = std::sin(TwoPi * tone.frequency * t) * 0.8 + std::sin(TwoPi * tone.frequency * 2 * t) * 0.2;
                double envelope = 1.0;
                if (i < fade)
                    envelope = double(i) / fade;
                else if (i > count - fade)
                    envelope = double(count - i) / fade;
                value *= envelope * volume;
            }
            samples.push_back(qint16(std::lround(value * 32000)));
        }
    }
    const quint32 dataBytes = quint32(samples.size() * sizeof(qint16));
    QByteArray wav;
    wav.reserve(int(44 + dataBytes));
    auto put32 = [&wav](quint32 value) {
        const quint32 le = qToLittleEndian(value);
        wav.append(reinterpret_cast<const char *>(&le), 4);
    };
    auto put16 = [&wav](quint16 value) {
        const quint16 le = qToLittleEndian(value);
        wav.append(reinterpret_cast<const char *>(&le), 2);
    };
    wav.append("RIFF", 4);
    put32(36 + dataBytes);
    wav.append("WAVEfmt ", 8);
    put32(16);
    put16(1); // PCM
    put16(1); // قناة واحدة
    put32(Rate);
    put32(Rate * 2);
    put16(2);
    put16(16);
    wav.append("data", 4);
    put32(dataBytes);
    for (qint16 sample : samples)
        put16(quint16(sample));
    return wav;
}

} // namespace

QString levelName(int level)
{
    switch (level) {
    case High:
        return QStringLiteral("عالٍ (إنذار)");
    case Silent:
        return QStringLiteral("صامت");
    case NotifyOnly:
        return QStringLiteral("إشعار فقط");
    default:
        return QStringLiteral("عادي");
    }
}

QString levelDescription(int level)
{
    switch (level) {
    case High:
        return QStringLiteral("صوت إنذار متكرر، وإشعار يبقى ظاهراً مدة أطول");
    case Silent:
        return QStringLiteral("شريط التنبيه وإشعار دون صوت");
    case NotifyOnly:
        return QStringLiteral("إشعار من ويندوز فقط، دون شريط تنبيه ودون صوت");
    default:
        return QStringLiteral("شريط التنبيه وإشعار وصوت تنبيه");
    }
}

QByteArray wav(Sound sound)
{
    switch (sound) {
    case Sound::Alarm: {
        std::vector<Tone> tones;
        for (int i = 0; i < 4; ++i) {
            tones.push_back({1320, 0.22});
            tones.push_back({880, 0.22});
        }
        tones.push_back({0, 0.1});
        return render(tones, 1.0);
    }
    case Sound::Visitor:
        return render({{660, 0.15}, {0, 0.06}, {660, 0.15}}, 0.75);
    case Sound::Chime:
    default:
        return render({{988, 0.16}, {1319, 0.32}}, 0.85);
    }
}

bool enabled()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("alertSound"), true).toBool();
}

void setEnabled(bool on)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("alertSound"), on);
}

void play(Sound sound)
{
    if (!enabled())
        return;
#ifdef _WIN32
    // SND_MEMORY مع SND_ASYNC يحتاج بقاء البيانات حتى ينتهي التشغيل، فتبقى طوال عمل البرنامج
    static const QByteArray chime = wav(Sound::Chime);
    static const QByteArray alarm = wav(Sound::Alarm);
    static const QByteArray visitor = wav(Sound::Visitor);
    const QByteArray &data = sound == Sound::Alarm ? alarm : sound == Sound::Visitor ? visitor : chime;
    if (PlaySoundW(reinterpret_cast<LPCWSTR>(data.constData()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
        return;
#else
    Q_UNUSED(sound);
#endif
    QApplication::beep();
}

} // namespace AlertSound

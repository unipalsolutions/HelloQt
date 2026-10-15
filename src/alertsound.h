#pragma once

#include <QByteArray>
#include <QString>

// مستوى تنبيه الشخص المراقَب وأصوات التنبيه. صوت النظام العادي (beep) قد لا يُسمع أو يكون
// مكتوماً، فتُولَّد أصوات مميزة وتُشغَّل مباشرة على Windows
namespace AlertSound {

enum Level {
    Normal = 0,     // رسالة في النافذة وإشعار وصوت تنبيه
    High = 1,       // صوت إنذار متكرر أعلى، ويبقى الإشعار ظاهراً مدة أطول
    Silent = 2,     // رسالة وإشعار دون صوت
    NotifyOnly = 3, // إشعار من ويندوز فقط، دون شريط التنبيه في النافذة ودون صوت
};

QString levelName(int level);
QString levelDescription(int level);

enum class Sound { Chime, Alarm, Visitor };
void play(Sound sound);

// تشغيل الأصوات (إعداد عام، مفعّل افتراضياً)
bool enabled();
void setEnabled(bool enabled);

// ملف WAV للصوت (لاختباره دون جهاز صوت)
QByteArray wav(Sound sound);

} // namespace AlertSound

#pragma once

#include <QDateTime>
#include <QString>

// سجل أحداث يومي في مجلد بيانات البرنامج (logs): بدء التشغيل وإغلاقه، انقطاع الكاميرات وعودتها،
// التسجيل وتوقفه، التنبيهات، والأخطاء والانهيارات. على جهاز مراقبة يعمل وحده هو الأثر الوحيد لما حدث
namespace AppLog {

// يبدأ السجل ويلتقط رسائل Qt والانهيارات. يعيد وقت التشغيل السابق إن لم يُغلق البرنامج حينها
// إغلاقاً طبيعياً (انهار أو انطفأ الجهاز)، وإلا وقتاً غير صالح
QDateTime start();
// إغلاق طبيعي
void finish();
void write(const QString &text);
QString folder();
// أسطر سجل يوم معين (لتقرير اليوم)
QStringList lines(const QDate &date);

} // namespace AppLog

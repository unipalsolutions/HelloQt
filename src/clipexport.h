#pragma once

#include <QString>

class QWidget;

// مقطع قصير حول لحظة ظهور (قبلها BeforeMs وبعدها AfterMs) في ملف واحد جاهز للتسليم،
// بدل إرسال ملف تسجيل كامل مدته 15 دقيقة
namespace ClipExport {

constexpr qint64 BeforeMs = 10 * 1000;
constexpr qint64 AfterMs = 20 * 1000;

// يسأل عن مكان الحفظ ثم يصدّر مع نافذة تقدم؛ title يدخل في الاسم المقترح للملف
void exportAround(QWidget *parent, const QString &source, qint64 positionMs, const QString &title);

// دون واجهة (للاختبار): يعيد رسالة خطأ أو نصاً فارغاً
QString exportClip(const QString &source, qint64 positionMs, const QString &target, qint64 beforeMs = BeforeMs,
                   qint64 afterMs = AfterMs);

} // namespace ClipExport

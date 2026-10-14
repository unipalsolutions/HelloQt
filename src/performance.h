#pragma once

#include <QString>

// مستوى استهلاك المعالج: الخفيف والمتوازن يتركان أنوية فارغة ويخفضان أولوية البرنامج
// حتى يبقى الجهاز مستجيباً أثناء تحليل الفيديو الطويل
namespace Performance {

enum class Level { Light = 0, Balanced = 1, Max = 2 };

Level saved();
// يتجاوز الإعداد المحفوظ (اختبارات سطر الأوامر تعمل بأقصى سرعة)
void force(Level level);
void save(Level level);
// عدد خيوط المعالجة المسموح بها لهذا المستوى
int threads(Level level);
// يضبط OpenCV ومجمع الخيوط وأولوية العملية؛ نماذج التعرف تأخذ الحد عند تحميلها
void apply(Level level);
QString describe(Level level);
// التحليل على كرت الشاشة إن وُجد كرت مناسب (يُطبَّق عند تحميل النماذج، أي عند فتح البرنامج)
bool gpuEnabled();
void setGpuEnabled(bool enabled);
// فحوص سطر الأوامر تعمل على المعالج دائماً لتبقى نتائجها ثابتة (إلا مع MALAMIH_GPU=1)
void forceCpu();

} // namespace Performance

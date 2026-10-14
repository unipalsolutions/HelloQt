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
// كرت الشاشة لا يُستخدم إلا بعد أن يجتاز فحصاً في عملية منفصلة (تعريف معطوب قد يُسقط البرنامج كله
// بدل أن يعيد خطأ). الفحص مرتبط باسم الكرت ورقم الإصدار، فيُعاد بعد تحديث البرنامج أو تغيير الكرت
bool gpuVerified(const QString &gpu);
void setGpuVerified(const QString &gpu, bool verified);
// هل عمل التشغيل الأخير على كرت الشاشة (إن انهار البرنامج فيه يُلغى اجتياز الفحص)
bool lastRunOnGpu();
void setLastRunOnGpu(bool onGpu);
// فحوص سطر الأوامر تعمل على المعالج دائماً لتبقى نتائجها ثابتة (إلا مع MALAMIH_GPU=1)
void forceCpu();

} // namespace Performance

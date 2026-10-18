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
// التحليل على كرت الشاشة إن وُجد كرت مناسب (يُطبَّق فوراً بتبديل المحرك، انظر FaceEngine::switchDevice)
bool gpuEnabled();
void setGpuEnabled(bool enabled);
// كرت الشاشة لا يُستخدم إلا بعد أن يجتاز فحصاً في عملية منفصلة (تعريف معطوب قد يُسقط البرنامج كله
// بدل أن يعيد خطأ). الفحص مرتبط بهوية الكرت (FaceEngine::gpuIdentity: الاسم والتعريف ونسخة ONNX Runtime)،
// فيُعاد بعد تغيير الكرت أو تحديث تعريفه، لا بعد تحديث البرنامج
bool gpuVerified(const QString &gpu);
void setGpuVerified(const QString &gpu, bool verified);
// هل عمل التشغيل الأخير على كرت الشاشة (إن انهار البرنامج فيه يُلغى اجتياز الفحص)
bool lastRunOnGpu();
void setLastRunOnGpu(bool onGpu);
// فحوص سطر الأوامر تعمل على المعالج دائماً لتبقى نتائجها ثابتة (إلا مع MALAMIH_GPU=1)
void forceCpu();
// دقة كشف الوجوه في الفيديو والكاميرا. السريع نموذج خفيف يفوته بعض الوجوه البعيدة والمائلة، والدقيق النموذج
// الكامل مع فحص أجزاء الإطار الكبير بدقتها الأصلية. التلقائي سريع على الأجهزة بأربعة خيوط أو أقل (لابتوب i3
// مثلاً) ودقيق على غيرها. الصور تبقى على الكشف الدقيق دائماً
enum class Detection { Auto = 0, Fast = 1, Accurate = 2 };
Detection detection();
void setDetection(Detection mode);
QString describe(Detection mode);
// هل يُستخدم الكشف الخفيف حين يعمل التحليل على المعالج (حسب الإعداد أعلاه وعدد الأنوية)
bool fastDetection();
// يتجاوز الإعداد (اختبارات سطر الأوامر على الدقيق لتبقى نتائجها ثابتة، إلا مع MALAMIH_DETECTOR=fast)
void forceFastDetection(bool fast);

} // namespace Performance

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

} // namespace Performance

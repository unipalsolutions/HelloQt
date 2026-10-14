#pragma once

#include <QString>

// فحص كرت الشاشة: يحمّل النماذج مرة على المعالج ومرة على كرت الشاشة، ويقيس سرعة كل منهما
// على الصور نفسها، ويتحقق أن البصمات متطابقة (كرت شاشة بتعريف معطوب قد يعطي بصمات خاطئة
// فيفشل التعرف بصمت). يعمل دون الواجهة، فيُستدعى في الخلفية
namespace GpuCheck {

struct Result
{
    QString gpu;              // اسم الكرت، أو فارغ إن لم يوجد
    QString error;            // سبب تعذر التشغيل على كرت الشاشة
    bool cpuOk = false;
    bool gpuOk = false;       // يعمل على كرت الشاشة
    double cpuMs = 0.0;       // متوسط زمن الإطار (كشف + بصمات)
    double gpuMs = 0.0;
    float similarity = 0.0f;  // أقل تشابه بين بصمات الجهازين للصورة نفسها (1 = متطابقة)
    int faces = 0;            // الوجوه المكشوفة على المعالج / كرت الشاشة
    int gpuFaces = 0;

    // كرت الشاشة يعمل وبصماته موثوقة
    bool reliable() const { return gpuOk && similarity >= 0.98f && faces == gpuFaces; }
    QString summary() const;
};

// image: صورة فيها وجوه (أو فارغ لصورة صناعية تقيس السرعة والتطابق فقط)
Result run(const QString &modelsDir, const QString &image);

} // namespace GpuCheck

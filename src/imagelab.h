#pragma once

#include <opencv2/core.hpp>

class FaceEngine;

// أدوات تحسين الصورة يدوياً: كل قيمة 0 تعني "بلا تغيير"
namespace ImageLab {

struct Settings
{
    int brightness = 0;   // -100..100 (جاما: يفتح الظلال أو يعتّمها)
    int contrast = 0;     // 0..100 موازنة التباين المحلي
    int denoise = 0;      // 0..100 إزالة التشويش مع الحفاظ على الحواف
    int deblur = 0;       // 0..100 إزالة الضبابية (تفكيك التمويه)
    int sharpen = 0;      // 0..100 زيادة الحدة
    int upscale = 1;      // 1..4 تكبير الدقة
    double rotation = 0;  // بالدرجات، موجب = عكس عقارب الساعة
    int quarterTurns = 0; // تدوير 90 درجة
    int keystoneH = 0;    // -100..100 تصحيح المنظور الأفقي
    int keystoneV = 0;    // -100..100 تصحيح المنظور العمودي (تصوير من أعلى أو أسفل)
    bool mirror = false;

    bool isIdentity() const;
};

cv::Mat apply(const cv::Mat &bgr, const Settings &settings);

// قيم مقترحة من قياس الإضاءة والتشويش والوضوح والحجم
Settings suggest(const cv::Mat &bgr);

// زاوية تجعل خط العينين أفقياً لأكبر وجه (يتطلب نماذج التعرف). يعيد false إن لم يجد وجهاً
bool levelFace(const cv::Mat &bgr, const FaceEngine *engine, double *angle);

double estimateNoise(const cv::Mat &bgr);
double estimateSharpness(const cv::Mat &bgr);

} // namespace ImageLab

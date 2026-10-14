#pragma once

#include <QString>

#include <opencv2/core.hpp>

// معالجات تُطبَّق على كل إطار من الفيديو أو الكاميرا قبل كشف الوجوه وعرضها، وعلى الإطارات
// المحفوظة والمقاطع المصدَّرة، فتبقى مواضع الوجوه متطابقة في كل مكان
namespace VideoFilters {

struct Settings
{
    int quarterTurns = 0;     // تدوير 90 درجة مع عقارب الساعة (0..3) للكاميرات المركبة مائلة أو مقلوبة
    bool mirror = false;      // قلب أفقي (كاميرات الويب تعكس الصورة)
    bool flip = false;        // قلب عمودي
    bool deinterlace = false; // إزالة خطوط التشابك في تسجيلات أجهزة المراقبة القديمة
    bool autoLevels = false;  // مدّ الإضاءة تلقائياً لكل إطار (الليل والإضاءة المتغيرة)
    bool grayscale = false;   // رمادي: يوحّد ألوان كاميرات الأشعة تحت الحمراء المختلطة
    int brightness = 0;       // -100..100
    int contrast = 0;         // 0..100 تباين محلي
    int denoise = 0;          // 0..100
    int sharpen = 0;          // 0..100

    // خيارات التحليل
    int minFaceSize = 0;      // تُتجاهل الوجوه الأصغر من هذا (بكسل في الإطار الأصلي)؛ 0 = كل الوجوه
    bool skipStill = false;   // لا يُبحث عن وجوه في الإطارات التي لم يتغير فيها شيء والمشهد خالٍ

    bool isIdentity() const;       // لا يتغير الإطار
    bool changesGeometry() const;  // تتغير أبعاد الإطار أو اتجاهه
    int activeCount() const;       // عدد المعالجات والخيارات المفعّلة (للشارة على الزر)
    // وصف قصير مثل «تدوير 90° · إضاءة +20»؛ withAnalysis = false يقتصر على ما يغيّر الإطار
    QString summary(bool withAnalysis = true) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
};

// الإعدادات الحالية للبرنامج كله، آمنة من أي خيط، وتُحفظ بين الجلسات
Settings current();
void setCurrent(const Settings &settings);

cv::Mat apply(const cv::Mat &bgr, const Settings &settings);
inline cv::Mat apply(const cv::Mat &bgr) { return apply(bgr, current()); }

// إعدادات جاهزة لحالات شائعة
enum class Preset { None, Night, OldDvr };
Settings preset(Preset preset, const Settings &base);

} // namespace VideoFilters

#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

// أيقونات خطية مرسومة بالكود حتى لا يحتاج التطبيق ملفات صور إضافية
namespace Icons {

enum class Name {
    Image,
    Video,
    Camera,
    Stop,
    Pause,
    Play,
    Sparkle,
    Save,
    UserPlus,
    Edit,
    Trash,
    Folder,
    PhotoAdd,
    Search,
    Scan,
    Users,
    Lock,
    Check,
    Alert,
    Info,
    Region,   // مستطيل بزوايا لتحديد منطقة
    Clock,
    Merge,
    Film,
    Next,     // الظهور التالي
    Previous,
    Sliders,  // الإعدادات
    ZoomIn,
    ZoomOut,
    FitWidth,
    Compare,  // وجهان متقابلان
    Collect,  // جمع صور إلى الملف
    Star,
    Export,
    History,
    Copy,     // تكرار
    Record,   // دائرة ممتلئة لتسجيل الكاميرا
    Rewind,   // قفز للخلف
    Forward,  // قفز للأمام
    SkipBack, // الملف السابق
    SkipNext, // الملف التالي
    Volume,
    Muted,
    Fullscreen,
    Panels,   // إظهار اللوحات الجانبية أو إخفاؤها
    Keyboard, // الاختصارات
    Flag,     // علامة على الخط الزمني
    Grid,     // عرض شبكي
    List,     // عرض قائمة
    Reject,   // علامة ×
    More,     // ثلاث نقاط: أدوات إضافية
    Rotate,   // سهم دائري مع عقارب الساعة
    Flip,     // مثلثان متقابلان حول خط: عكس أفقي
    Crop,     // زاويتا القص المتقاطعتان
    Undo,     // سهم راجع: إعادة الضبط
    Level,    // خط أفقي داخل دائرة: تسوية الميلان
};

void paint(QPainter &painter, Name name, const QRectF &rect, const QColor &color);
QIcon icon(Name name, const QColor &color, int size = 24);

} // namespace Icons

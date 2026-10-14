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
};

void paint(QPainter &painter, Name name, const QRectF &rect, const QColor &color);
QIcon icon(Name name, const QColor &color, int size = 24);

} // namespace Icons

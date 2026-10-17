#pragma once

#include <QColor>

class QApplication;

namespace Theme {

// ألوان «ناعمة» داكنة: بني دافئ للخلفيات، أخضر للأفعال الأساسية، وبرتقالي للتمييز
inline const QColor Background(0x15, 0x11, 0x0d);
inline const QColor Sidebar(0x1a, 0x15, 0x11);
inline const QColor Surface(0x21, 0x1b, 0x16);
inline const QColor SurfaceHigh(0x2b, 0x23, 0x1d);
inline const QColor Border(0x3a, 0x2f, 0x26);
inline const QColor Text(0xf4, 0xed, 0xe4);
inline const QColor Muted(0xa9, 0x9a, 0x8a);
inline const QColor Accent(0x3e, 0x8a, 0x63);
inline const QColor Accent2(0x6c, 0xc1, 0x93);
inline const QColor Success(0x4f, 0xbf, 0x87);
inline const QColor Warning(0xf5, 0xa5, 0x24);
inline const QColor Danger(0xd9, 0x54, 0x2b);
// أحمر المراقَبين: أفتح من Danger ليقرأ فوق الصور والفيديو
inline const QColor Watched(0xe8, 0x77, 0x4f);
// برتقالي التحديد (حدود العنصر المختار)
inline const QColor Highlight(0xf5, 0xa5, 0x24);
// خلفية مساحة الفيديو والصورة
inline const QColor Viewport(0x0e, 0x0b, 0x09);

QColor mix(const QColor &a, const QColor &b, double t);
QColor withAlpha(QColor color, int alpha);

void apply(QApplication &app);

} // namespace Theme

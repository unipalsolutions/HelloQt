#pragma once

#include <QColor>

class QApplication;

namespace Theme {

inline const QColor Background(0x0b, 0x10, 0x20);
inline const QColor Sidebar(0x0f, 0x16, 0x29);
inline const QColor Surface(0x14, 0x1c, 0x2f);
inline const QColor SurfaceHigh(0x1b, 0x25, 0x40);
inline const QColor Border(0x24, 0x30, 0x4d);
inline const QColor Text(0xe6, 0xea, 0xf2);
inline const QColor Muted(0x8b, 0x97, 0xb3);
inline const QColor Accent(0x6d, 0x5e, 0xfc);
inline const QColor Accent2(0x22, 0xd3, 0xee);
inline const QColor Success(0x22, 0xc5, 0x5e);
inline const QColor Warning(0xf5, 0x9e, 0x0b);
inline const QColor Danger(0xf4, 0x3f, 0x5e);

QColor mix(const QColor &a, const QColor &b, double t);
QColor withAlpha(QColor color, int alpha);

void apply(QApplication &app);

} // namespace Theme

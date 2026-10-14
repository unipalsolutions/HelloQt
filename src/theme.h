#pragma once

#include <QColor>

class QApplication;

namespace Theme {

inline const QColor Background(0x07, 0x11, 0x1f);
inline const QColor Sidebar(0x0a, 0x16, 0x28);
inline const QColor Surface(0x0e, 0x1d, 0x33);
inline const QColor SurfaceHigh(0x13, 0x28, 0x4a);
inline const QColor Border(0x1c, 0x35, 0x60);
inline const QColor Text(0xe6, 0xf0, 0xff);
inline const QColor Muted(0x8a, 0xa3, 0xc7);
inline const QColor Accent(0x2f, 0x7c, 0xf6);
inline const QColor Accent2(0x38, 0xbd, 0xf8);
inline const QColor Success(0x22, 0xd3, 0xee);
inline const QColor Warning(0x93, 0xc5, 0xfd);
inline const QColor Danger(0x1d, 0x4e, 0xd8);

QColor mix(const QColor &a, const QColor &b, double t);
QColor withAlpha(QColor color, int alpha);

void apply(QApplication &app);

} // namespace Theme

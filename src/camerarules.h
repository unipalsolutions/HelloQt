#pragma once

#include <QDateTime>
#include <QList>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTime>

// قواعد المراقبة لكل كاميرا (تُحفظ بمصدرها "camera:…"):
//   - مناطق داخل الكادر: يُحلَّل ما بداخلها فقط، أو يُتجاهل ما بداخلها (صورة على الجدار، تلفاز، شارع)
//   - جدول التنبيهات: لا تنبيه إلا في ساعات محددة (والظهور يُسجَّل دائماً)
//   - جدول التسجيل: لا يُسجَّل إلا في ساعات محددة (مثلاً ليلاً فقط)
namespace CameraRules {

enum class ZoneMode { Include, Exclude };

struct Window
{
    bool enabled = false;
    QTime from = QTime(22, 0);
    QTime to = QTime(6, 0);
    // الفترة قد تعبر منتصف الليل (من 22:00 إلى 06:00)؛ from == to تعني اليوم كله
    bool contains(const QTime &time) const;
    bool active(const QDateTime &now = QDateTime::currentDateTime()) const { return !enabled || contains(now.time()); }
    QString text() const;
};

struct Rules
{
    QList<QRectF> zones; // بإحداثيات نسبية من 0 إلى 1
    ZoneMode mode = ZoneMode::Include;
    Window alerts;
    Window recording;

    bool hasZones() const { return !zones.isEmpty(); }
    // الوجه (بإحداثيات الإطار) يُحلَّل حسب موقع مركزه
    bool accepts(const QRectF &box, const QSize &frame) const;
    bool acceptsNormalized(const QPointF &center) const;
    bool isEmpty() const { return zones.isEmpty() && !alerts.enabled && !recording.enabled; }
    QString summary() const;
};

Rules load(const QString &source);
void save(const QString &source, const Rules &rules);

} // namespace CameraRules

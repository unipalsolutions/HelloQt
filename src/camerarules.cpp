#include "camerarules.h"

#include <QSettings>
#include <QStandardPaths>

namespace CameraRules {

namespace {

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

// المصدر فيه «:» وقد يكون فيه «/»، فيُحفظ بصيغة لا تفسدها مفاتيح QSettings
QString group(const QString &source)
{
    return QStringLiteral("cameraRules/") + QString::fromLatin1(source.toUtf8().toHex());
}

void writeWindow(QSettings &store, const QString &name, const Window &window)
{
    store.setValue(name + QStringLiteral("Enabled"), window.enabled);
    store.setValue(name + QStringLiteral("From"), window.from.toString(QStringLiteral("HH:mm")));
    store.setValue(name + QStringLiteral("To"), window.to.toString(QStringLiteral("HH:mm")));
}

Window readWindow(QSettings &store, const QString &name, const Window &fallback)
{
    Window window;
    window.enabled = store.value(name + QStringLiteral("Enabled"), false).toBool();
    const QTime from = QTime::fromString(store.value(name + QStringLiteral("From")).toString(), QStringLiteral("HH:mm"));
    const QTime to = QTime::fromString(store.value(name + QStringLiteral("To")).toString(), QStringLiteral("HH:mm"));
    window.from = from.isValid() ? from : fallback.from;
    window.to = to.isValid() ? to : fallback.to;
    return window;
}

} // namespace

bool Window::contains(const QTime &time) const
{
    if (from == to)
        return true;
    if (from < to)
        return time >= from && time < to;
    return time >= from || time < to;
}

QString Window::text() const
{
    if (!enabled)
        return QStringLiteral("دائماً");
    return QStringLiteral("من ⁦%1⁩ إلى ⁦%2⁩")
        .arg(from.toString(QStringLiteral("HH:mm")), to.toString(QStringLiteral("HH:mm")));
}

bool Rules::acceptsNormalized(const QPointF &center) const
{
    if (zones.isEmpty())
        return true;
    bool inside = false;
    for (const QRectF &zone : zones) {
        if (zone.contains(center)) {
            inside = true;
            break;
        }
    }
    return mode == ZoneMode::Include ? inside : !inside;
}

bool Rules::accepts(const QRectF &box, const QSize &frame) const
{
    if (zones.isEmpty() || frame.width() <= 0 || frame.height() <= 0)
        return true;
    const QPointF center = box.center();
    return acceptsNormalized(QPointF(center.x() / frame.width(), center.y() / frame.height()));
}

QString Rules::summary() const
{
    QStringList parts;
    if (!zones.isEmpty())
        parts << (mode == ZoneMode::Include ? QStringLiteral("التحليل داخل %1 منطقة فقط") : QStringLiteral("تجاهل %1 منطقة"))
                     .arg(zones.size());
    if (alerts.enabled)
        parts << QStringLiteral("التنبيهات %1").arg(alerts.text());
    if (recording.enabled)
        parts << QStringLiteral("التسجيل %1").arg(recording.text());
    return parts.isEmpty() ? QStringLiteral("الكادر كله، تنبيه وتسجيل دائماً") : parts.join(QStringLiteral(" · "));
}

Rules load(const QString &source)
{
    Rules rules;
    if (source.isEmpty())
        return rules;
    QSettings store(settingsPath(), QSettings::IniFormat);
    store.beginGroup(group(source));
    rules.mode = store.value(QStringLiteral("zoneMode")).toString() == QLatin1String("exclude") ? ZoneMode::Exclude : ZoneMode::Include;
    const QStringList zones = store.value(QStringLiteral("zones")).toStringList();
    for (const QString &zone : zones) {
        const QStringList values = zone.split(QLatin1Char(','));
        if (values.size() != 4)
            continue;
        const QRectF rect = QRectF(values.at(0).toDouble(), values.at(1).toDouble(), values.at(2).toDouble(), values.at(3).toDouble())
                                .normalized()
                                .intersected(QRectF(0, 0, 1, 1));
        if (rect.width() > 0.005 && rect.height() > 0.005)
            rules.zones.append(rect);
    }
    rules.alerts = readWindow(store, QStringLiteral("alerts"), Window{false, QTime(22, 0), QTime(6, 0)});
    rules.recording = readWindow(store, QStringLiteral("recording"), Window{false, QTime(22, 0), QTime(6, 0)});
    store.endGroup();
    return rules;
}

void save(const QString &source, const Rules &rules)
{
    if (source.isEmpty())
        return;
    QSettings store(settingsPath(), QSettings::IniFormat);
    store.remove(group(source));
    if (rules.isEmpty())
        return;
    store.beginGroup(group(source));
    store.setValue(QStringLiteral("zoneMode"), rules.mode == ZoneMode::Exclude ? QStringLiteral("exclude") : QStringLiteral("include"));
    QStringList zones;
    for (const QRectF &zone : rules.zones)
        zones << QStringLiteral("%1,%2,%3,%4").arg(zone.x(), 0, 'f', 4).arg(zone.y(), 0, 'f', 4).arg(zone.width(), 0, 'f', 4).arg(zone.height(), 0, 'f', 4);
    store.setValue(QStringLiteral("zones"), zones);
    writeWindow(store, QStringLiteral("alerts"), rules.alerts);
    writeWindow(store, QStringLiteral("recording"), rules.recording);
    store.endGroup();
}

} // namespace CameraRules

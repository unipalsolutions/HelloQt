#pragma once

#include <QMap>
#include <QString>

// علامات المستخدم على الفيديو (الموضع بالمللي ثانية ← ملاحظة)، محفوظة لكل ملف في مجلد البرنامج
namespace VideoMarkers {

QMap<qint64, QString> load(const QString &videoPath);
void save(const QString &videoPath, const QMap<qint64, QString> &markers);

} // namespace VideoMarkers

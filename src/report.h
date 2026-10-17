#pragma once

#include "facetypes.h"

#include <QList>
#include <QString>
#include <QStringList>

// تقرير HTML مستقل (الصور مضمّنة فيه) يُفتح في أي متصفح ويُطبع:
// ملخص التحليل، وبطاقة لكل وجه، وفي الفيديو مدة ظهور كل شخص وخط زمني لفترات ظهوره
namespace Report {

struct Source
{
    QString path;          // الصورة أو الفيديو
    bool video = false;
    qint64 durationMs = 0; // مدة الفيديو
};

// مجموع فترات الظهور بعد دمج المتداخل منها
qint64 screenTime(const QList<QPair<qint64, qint64>> &appearances);

bool writeHtml(const QString &path, const QList<FaceItem> &faces, const Source &source, QString *error = nullptr);

// ظهور شخص في ملف من الأرشيف
struct ArchiveSighting
{
    QString path;
    qint64 durationMs = 0;
    QList<QPair<qint64, qint64>> appearances;
};

struct ArchivePerson
{
    FaceItem face; // أوضح لقطة، ونتيجة التعرف في match
    QList<ArchiveSighting> sightings;
};

// تقرير موحّد لمسح الأرشيف: من ظهر، وفي أي ملف، ومتى، ومدة ظهوره
bool writeArchiveHtml(const QString &path, const QList<ArchivePerson> &persons, const QStringList &files, qint64 scannedMs,
                      QString *error = nullptr);
// الجدول نفسه بصيغة CSV (سطر لكل فترة ظهور) لبرامج الجداول
bool writeArchiveCsv(const QString &path, const QList<ArchivePerson> &persons, QString *error = nullptr);

} // namespace Report

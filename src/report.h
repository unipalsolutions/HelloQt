#pragma once

#include "facetypes.h"

#include <QList>
#include <QString>

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

} // namespace Report

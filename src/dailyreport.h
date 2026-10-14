#pragma once

#include <QDate>
#include <QSet>
#include <QString>

class FaceDatabase;

// تقرير يوم واحد من الكاميرات: من ظهر ومتى وأمام أي كاميرا، والتنبيهات، والزوار المجهولون،
// وساعات التسجيل، وأحداث اليوم من سجل الأحداث (الانقطاع والأعطال). يُحفظ HTML (يُفتح في أي متصفح)
// أو PDF للطباعة والإرسال
namespace DailyReport {

bool write(const QString &path, const FaceDatabase &database, const QSet<QString> &watched, const QDate &day,
           QString *error = nullptr);

} // namespace DailyReport

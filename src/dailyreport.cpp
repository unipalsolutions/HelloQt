#include "dailyreport.h"

#include "applog.h"
#include "cameras.h"
#include "facedatabase.h"
#include "recorder.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextDocument>
#include <QTextOption>
#include <QUrl>

#include <algorithm>

namespace DailyReport {

namespace {

constexpr int MaxVisitors = 60;
constexpr int MaxEvents = 200;

struct PersonRow
{
    QString name;
    bool watched = false;
    QDateTime first;
    QDateTime last;
    int count = 0;
    QStringList cameras;
    QImage face;
};

struct VisitorRow
{
    qint64 id = 0;
    QDateTime first;
    QDateTime last;
    int count = 0;
    QStringList cameras;
    QImage face;
};

QString escape(const QString &text)
{
    return text.toHtmlEscaped();
}

QString cameraName(const QString &camera)
{
    return Cameras::displayName(camera);
}

void addCamera(QStringList &cameras, const QString &camera)
{
    if (!cameras.contains(camera))
        cameras << camera;
}

QString hours(qint64 ms)
{
    return QString::number(ms / 3600000.0, 'f', 1);
}

// الصور: في ملف HTML مضمّنة فيه، وفي PDF موارد في المستند نفسه
class Images
{
public:
    explicit Images(QTextDocument *document)
        : m_document(document)
    {
    }

    QString source(const QImage &image)
    {
        if (image.isNull())
            return {};
        const QImage scaled = image.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (m_document) {
            const QUrl url(QStringLiteral("face://%1").arg(++m_next));
            m_document->addResource(QTextDocument::ImageResource, url, scaled);
            return url.toString();
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        scaled.save(&buffer, "JPG", 85);
        return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
    }

    QString tag(const QImage &image)
    {
        const QString src = source(image);
        return src.isEmpty() ? QString() : QStringLiteral("<img src=\"%1\" width=\"48\" height=\"48\">").arg(src);
    }

private:
    QTextDocument *m_document;
    int m_next = 0;
};

QString build(const FaceDatabase &database, const QSet<QString> &watched, const QDate &day, QTextDocument *document)
{
    const QDateTime from(day, QTime(0, 0));
    const QDateTime to(day.addDays(1), QTime(0, 0));
    Images images(document);

    // الأشخاص المعروفون
    QMap<QString, PersonRow> persons;
    QMap<QString, QPair<int, int>> perCamera; // ظهور معروفين، زوار
    int alerts = 0;
    int known = 0;
    for (const SightingEntry &entry : database.cameraSightingsBetween(from, to)) {
        PersonRow &row = persons[entry.personId];
        row.name = entry.name;
        row.watched = watched.contains(entry.personId);
        if (!row.first.isValid())
            row.first = entry.sighting.seen;
        row.last = entry.sighting.seen;
        ++row.count;
        addCamera(row.cameras, cameraName(entry.sighting.camera));
        if (row.face.isNull())
            row.face = entry.sighting.thumbnail;
        ++perCamera[cameraName(entry.sighting.camera)].first;
        ++known;
        alerts += row.watched ? 1 : 0;
    }
    // الزوار المجهولون
    QMap<qint64, VisitorRow> visitors;
    int visitorSightings = 0;
    for (const VisitorSighting &entry : database.visitorSightingsBetween(from, to)) {
        VisitorRow &row = visitors[entry.visitorId];
        row.id = entry.visitorId;
        if (!row.first.isValid())
            row.first = entry.sighting.seen;
        row.last = entry.sighting.seen;
        ++row.count;
        addCamera(row.cameras, cameraName(entry.sighting.camera));
        if (row.face.isNull())
            row.face = entry.sighting.thumbnail;
        ++perCamera[cameraName(entry.sighting.camera)].second;
        ++visitorSightings;
    }
    // التسجيلات
    QMap<QString, QPair<int, qint64>> recorded; // عدد الملفات، المدة
    qint64 recordedMs = 0;
    for (const CameraRecorder::RecordingFile &file : CameraRecorder::recordings(from, to)) {
        const qint64 ms = std::max(file.start, from).msecsTo(std::min(file.end, to));
        auto &entry = recorded[file.camera];
        ++entry.first;
        entry.second += ms;
        recordedMs += ms;
    }

    QString html;
    html += QStringLiteral("<html dir=\"rtl\"><head><meta charset=\"utf-8\"><title>تقرير يوم %1</title><style>"
                           "body { font-family: 'Segoe UI', Tahoma, sans-serif; color: #10233f; margin: 24px; }"
                           "h1 { color: #1d4ed8; margin-bottom: 2px; } h2 { color: #1d4ed8; margin-top: 22px; }"
                           ".muted { color: #64748b; }"
                           "table { border-collapse: collapse; width: 100%; }"
                           "th { background: #e0ecff; text-align: right; padding: 6px; }"
                           "td { border-bottom: 1px solid #dbe5f3; padding: 5px 6px; vertical-align: middle; }"
                           "tr.watched td { background: #ffe4e4; color: #9f1239; font-weight: bold; }"
                           ".num { font-size: 20pt; font-weight: bold; color: #1d4ed8; }"
                           "</style></head><body>")
                .arg(day.toString(QStringLiteral("yyyy/MM/dd")));
    html += QStringLiteral("<h1>تقرير الكاميرات ليوم ⁦%1⁩</h1>").arg(day.toString(QStringLiteral("yyyy/MM/dd")));
    html += QStringLiteral("<p class=\"muted\">أُنشئ ⁦%1⁩ ببرنامج ملامح ⁦%2⁩</p>")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy/MM/dd HH:mm")), QApplication::applicationVersion());

    // الملخص
    html += QStringLiteral("<table cellpadding=\"8\"><tr>");
    const auto stat = [](const QString &label, const QString &value) {
        return QStringLiteral("<td align=\"center\"><span class=\"num\">%1</span><br><span class=\"muted\">%2</span></td>").arg(value, label);
    };
    html += stat(QStringLiteral("أشخاص معروفون"), QString::number(persons.size()));
    html += stat(QStringLiteral("مرات ظهورهم"), QString::number(known));
    html += stat(QStringLiteral("تنبيهات مراقبة"), QString::number(alerts));
    html += stat(QStringLiteral("زوار مجهولون"), QString::number(visitors.size()));
    html += stat(QStringLiteral("ساعات التسجيل"), hours(recordedMs));
    html += QStringLiteral("</tr></table>");

    // الكاميرات
    QStringList cameras = perCamera.keys();
    for (const QString &camera : recorded.keys()) {
        if (!cameras.contains(camera))
            cameras << camera;
    }
    html += QStringLiteral("<h2>الكاميرات</h2>");
    if (cameras.isEmpty()) {
        html += QStringLiteral("<p class=\"muted\">لا ظهور ولا تسجيل في هذا اليوم.</p>");
    } else {
        html += QStringLiteral("<table><tr><th>الكاميرا</th><th>ظهور معروفين</th><th>ظهور زوار مجهولين</th><th>ملفات التسجيل</th><th>ساعات التسجيل</th></tr>");
        for (const QString &camera : std::as_const(cameras)) {
            const auto counts = perCamera.value(camera);
            // أسماء ملفات التسجيل بلا الرموز الممنوعة في أسماء الملفات
            const auto files = recorded.value(recorded.contains(camera) ? camera : CameraRecorder::fileCameraName(camera));
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td></tr>")
                        .arg(escape(camera))
                        .arg(counts.first)
                        .arg(counts.second)
                        .arg(files.first)
                        .arg(hours(files.second));
        }
        html += QStringLiteral("</table>");
    }

    // الأشخاص: المراقَبون أولاً ثم الأكثر ظهوراً
    QList<PersonRow> rows = persons.values();
    std::sort(rows.begin(), rows.end(), [](const PersonRow &a, const PersonRow &b) {
        if (a.watched != b.watched)
            return a.watched;
        if (a.count != b.count)
            return a.count > b.count;
        return a.first < b.first;
    });
    html += QStringLiteral("<h2>من ظهر</h2>");
    if (rows.isEmpty()) {
        html += QStringLiteral("<p class=\"muted\">لم يظهر أحد من المسجلين أمام الكاميرات.</p>");
    } else {
        html += QStringLiteral("<table><tr><th></th><th>الاسم</th><th>أول ظهور</th><th>آخر ظهور</th><th>المرات</th><th>الكاميرات</th></tr>");
        for (const PersonRow &row : std::as_const(rows)) {
            html += QStringLiteral("<tr%1><td width=\"56\">%2</td><td>%3</td><td>⁦%4⁩</td><td>⁦%5⁩</td><td>%6</td><td>%7</td></tr>")
                        .arg(row.watched ? QStringLiteral(" class=\"watched\" bgcolor=\"#ffe4e4\"") : QString(), images.tag(row.face),
                             escape(row.watched ? QStringLiteral("⚠ ") + row.name : row.name),
                             row.first.toString(QStringLiteral("HH:mm:ss")), row.last.toString(QStringLiteral("HH:mm:ss")))
                        .arg(row.count)
                        .arg(escape(row.cameras.join(QStringLiteral("، "))));
        }
        html += QStringLiteral("</table>");
    }

    // الزوار المجهولون: من تكرر ظهوره أولاً
    QList<VisitorRow> unknown = visitors.values();
    std::sort(unknown.begin(), unknown.end(), [](const VisitorRow &a, const VisitorRow &b) {
        return a.count != b.count ? a.count > b.count : a.first < b.first;
    });
    html += QStringLiteral("<h2>الزوار المجهولون</h2>");
    if (unknown.isEmpty()) {
        html += QStringLiteral("<p class=\"muted\">لا زوار مجهولين.</p>");
    } else {
        html += QStringLiteral("<p class=\"muted\">%1 زائراً، %2 مرة ظهور%3</p>")
                    .arg(unknown.size())
                    .arg(visitorSightings)
                    .arg(unknown.size() > MaxVisitors ? QStringLiteral(" (أول %1 فقط)").arg(MaxVisitors) : QString());
        html += QStringLiteral("<table><tr><th></th><th>الزائر</th><th>أول ظهور</th><th>آخر ظهور</th><th>المرات</th><th>الكاميرات</th></tr>");
        for (int i = 0; i < std::min<qsizetype>(unknown.size(), MaxVisitors); ++i) {
            const VisitorRow &row = unknown.at(i);
            html += QStringLiteral("<tr><td width=\"56\">%1</td><td>#%2</td><td>⁦%3⁩</td><td>⁦%4⁩</td><td>%5</td><td>%6</td></tr>")
                        .arg(images.tag(row.face))
                        .arg(row.id)
                        .arg(row.first.toString(QStringLiteral("HH:mm:ss")), row.last.toString(QStringLiteral("HH:mm:ss")))
                        .arg(row.count)
                        .arg(escape(row.cameras.join(QStringLiteral("، "))));
        }
        html += QStringLiteral("</table>");
    }

    // أحداث اليوم من سجل الأحداث
    const QStringList events = AppLog::lines(day);
    html += QStringLiteral("<h2>أحداث اليوم</h2>");
    if (events.isEmpty()) {
        html += QStringLiteral("<p class=\"muted\">لا أحداث مسجلة.</p>");
    } else {
        html += QStringLiteral("<table><tr><th>الوقت</th><th>الحدث</th></tr>");
        const qsizetype first = std::max<qsizetype>(0, events.size() - MaxEvents);
        for (qsizetype i = first; i < events.size(); ++i) {
            const QString &line = events.at(i);
            const bool problem = line.contains(QStringLiteral("[انهيار]")) || line.contains(QStringLiteral("[خطأ]"))
                || line.contains(QStringLiteral("انقطع")) || line.contains(QStringLiteral("منقطعة"));
            html += QStringLiteral("<tr%1><td>⁦%2⁩</td><td>%3</td></tr>")
                        .arg(problem ? QStringLiteral(" class=\"watched\" bgcolor=\"#ffe4e4\"") : QString(), escape(line.left(8)),
                             escape(line.mid(10)));
        }
        html += QStringLiteral("</table>");
        if (first > 0)
            html += QStringLiteral("<p class=\"muted\">آخر %1 حدث فقط من %2</p>").arg(MaxEvents).arg(events.size());
    }
    html += QStringLiteral("</body></html>");
    return html;
}

} // namespace

bool write(const QString &path, const FaceDatabase &database, const QSet<QString> &watched, const QDate &day, QString *error)
{
    if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        QTextDocument document;
        document.setDefaultTextOption(QTextOption(Qt::AlignRight));
        document.setHtml(build(database, watched, day, &document));
        if (!QFileInfo(path).absoluteDir().exists()) {
            if (error)
                *error = QStringLiteral("المجلد غير موجود");
            return false;
        }
        QFile::remove(path);
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageMargins(QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter);
        writer.setTitle(QStringLiteral("تقرير يوم %1").arg(day.toString(Qt::ISODate)));
        writer.setCreator(QStringLiteral("Malamih"));
        document.print(&writer);
        if (!QFile::exists(path)) {
            if (error)
                *error = QStringLiteral("تعذر كتابة ملف PDF");
            return false;
        }
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray data = build(database, watched, day, nullptr).toUtf8();
    if (file.write(data) != data.size() || !file.flush()) {
        if (error)
            *error = QStringLiteral("تعذر حفظ الملف كاملاً؛ تحقق من المساحة الفارغة");
        return false;
    }
    return true;
}

} // namespace DailyReport

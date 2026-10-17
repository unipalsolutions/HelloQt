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
#include <QLocale>
#include <QMap>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextBlock>
#include <QTextCursor>
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
        return src.isEmpty() ? QString() : QStringLiteral("<img src=\"%1\" width=\"40\" height=\"40\">").arg(src);
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

    // جداول QTextDocument (PDF) لا تنعكس مع اتجاه النص، فتُكتب خلاياها فيه من اليسار إلى اليمين
    const bool mirror = document != nullptr;
    const auto tr = [mirror](QStringList cells, const QString &attributes = QString()) {
        if (mirror)
            std::reverse(cells.begin(), cells.end());
        return QStringLiteral("<tr%1>%2</tr>").arg(attributes, cells.join(QString()));
    };
    const auto td = [](const QString &content, bool center = false) {
        return QStringLiteral("<td%1>%2</td>").arg(center ? QStringLiteral(" align=\"center\"") : QString(), content);
    };
    // رأس الجدول داكن ويتكرر أعلى كل صفحة؛ العنوان الذي يبدأ بـ «^» عمود موسَّط
    const auto open = [&tr](const QStringList &titles) {
        QStringList cells;
        for (const QString &title : titles) {
            const bool center = title.startsWith(QLatin1Char('^'));
            cells << QStringLiteral("<th bgcolor=\"#2b231d\" align=\"%1\">%2</th>")
                         .arg(center ? QStringLiteral("center") : QStringLiteral("right"), center ? title.mid(1) : title);
        }
        return QStringLiteral("<table class=\"grid\" width=\"100%\" cellspacing=\"0\" cellpadding=\"6\"><thead>") + tr(cells)
            + QStringLiteral("</thead>");
    };
    // صفوف متناوبة اللون، والمراقَب بلون تحذير
    const auto rowAttributes = [](int index, bool alert) {
        if (alert)
            return QStringLiteral(" class=\"watched\" bgcolor=\"#fdf0ea\"");
        return index % 2 ? QStringLiteral(" bgcolor=\"#faf7f3\"") : QString();
    };
    const auto section = [&tr](const QString &title, const QString &note = QString()) {
        return QStringLiteral("<table class=\"section\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">")
            + tr({QStringLiteral("<td class=\"title\" width=\"70%\" align=\"right\">%1</td>").arg(title),
                  QStringLiteral("<td class=\"note\" width=\"30%\" align=\"left\">%1</td>").arg(note)})
            + QStringLiteral("</table>");
    };
    const auto empty = [](const QString &text) { return QStringLiteral("<p class=\"empty\">%1</p>").arg(text); };
    const auto time = [](const QDateTime &value) { return QStringLiteral("⁦%1⁩").arg(value.toString(QStringLiteral("HH:mm:ss"))); };

    QString html;
    html += QStringLiteral("<html dir=\"rtl\"><head><meta charset=\"utf-8\"><title>تقرير يوم %1</title><style>"
                           "body { font-family: 'Segoe UI', Tahoma, sans-serif; font-size: 10pt; color: #241c16; %2 }"
                           "table.grid { border-collapse: collapse; }"
                           "th { color: #ffffff; font-size: 9pt; font-weight: bold; padding: 7px 8px; }"
                           "table.grid td { border-bottom: 1px solid #ebe3d9; padding: 6px 8px; vertical-align: middle; }"
                           "tr.watched td { color: #b8431f; font-weight: bold; }"
                           "table.section { margin-top: 20px; margin-bottom: 8px; }"
                           "table.section td { border-bottom: 2px solid #3e8a63; padding-bottom: 4px; }"
                           "td.title { color: #2b231d; font-size: 14pt; font-weight: bold; }"
                           "td.note { color: #7a6b5d; font-size: 9pt; }"
                           ".empty { color: #a99a8a; margin: 6px 0; }"
                           ".num { font-size: 22pt; font-weight: bold; }"
                           ".label { color: #5e5044; font-size: 9pt; }"
                           "</style></head><body dir=\"rtl\">")
                .arg(day.toString(QStringLiteral("yyyy/MM/dd")),
                     mirror ? QStringLiteral("margin: 0;") : QStringLiteral("max-width: 980px; margin: 24px auto; padding: 0 16px;"));

    // الترويسة
    html += QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"16\" bgcolor=\"#2b231d\">");
    html += tr({QStringLiteral("<td width=\"65%\" align=\"right\"><span style=\"color:#ffffff; font-size:20pt; font-weight:bold\">تقرير الكاميرات اليومي</span><br>"
                               "<span style=\"color:#e3d3c0; font-size:11pt\">%1 ⁦%2⁩</span></td>")
                    .arg(QLocale(QLocale::Arabic).dayName(day.dayOfWeek()), day.toString(QStringLiteral("yyyy/MM/dd"))),
                QStringLiteral("<td width=\"35%\" align=\"left\" valign=\"bottom\"><span style=\"color:#ffffff; font-size:13pt; font-weight:bold\">ملامح</span><br>"
                               "<span style=\"color:#e3d3c0; font-size:8pt\">أُنشئ ⁦%1⁩ · الإصدار ⁦%2⁩</span></td>")
                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy/MM/dd HH:mm")), QApplication::applicationVersion())});
    html += QStringLiteral("</table>");

    // الملخص: بطاقات
    const auto stat = [](const QString &label, const QString &value, const QString &color, const QString &background) {
        return QStringLiteral("<td align=\"center\" width=\"20%\" bgcolor=\"%4\" style=\"border-top: 3px solid %3\">"
                              "<span class=\"num\" style=\"color:%3\">%1</span><br><span class=\"label\">%2</span></td>")
            .arg(value, label, color, background);
    };
    const QString blue = QStringLiteral("#3e8a63");
    const QString pale = QStringLiteral("#eef6f1");
    html += QStringLiteral("<table width=\"100%\" cellspacing=\"6\" cellpadding=\"10\" style=\"margin-top: 10px\">");
    html += tr({stat(QStringLiteral("أشخاص معروفون"), QString::number(persons.size()), blue, pale),
                stat(QStringLiteral("مرات ظهورهم"), QString::number(known), blue, pale),
                alerts > 0 ? stat(QStringLiteral("تنبيهات مراقبة"), QString::number(alerts), QStringLiteral("#d9542b"), QStringLiteral("#fdf0ea"))
                           : stat(QStringLiteral("تنبيهات مراقبة"), QStringLiteral("0"), QStringLiteral("#3e8a63"), QStringLiteral("#eef6f1")),
                stat(QStringLiteral("زوار مجهولون"), QString::number(visitors.size()), QStringLiteral("#d98a10"), QStringLiteral("#fdf5e6")),
                stat(QStringLiteral("ساعات التسجيل"), hours(recordedMs), QStringLiteral("#9a6b3f"), QStringLiteral("#f6efe7"))});
    html += QStringLiteral("</table>");

    // الكاميرات
    QStringList cameras = perCamera.keys();
    for (const QString &camera : recorded.keys()) {
        if (!cameras.contains(camera))
            cameras << camera;
    }
    html += section(QStringLiteral("الكاميرات"), cameras.isEmpty() ? QString() : QStringLiteral("%1 كاميرا").arg(cameras.size()));
    if (cameras.isEmpty()) {
        html += empty(QStringLiteral("لا ظهور ولا تسجيل في هذا اليوم."));
    } else {
        html += open({QStringLiteral("الكاميرا"), QStringLiteral("^ظهور معروفين"), QStringLiteral("^ظهور زوار مجهولين"),
                      QStringLiteral("^ملفات التسجيل"), QStringLiteral("^ساعات التسجيل")});
        int index = 0;
        for (const QString &camera : std::as_const(cameras)) {
            const auto counts = perCamera.value(camera);
            // أسماء ملفات التسجيل بلا الرموز الممنوعة في أسماء الملفات
            const auto files = recorded.value(recorded.contains(camera) ? camera : CameraRecorder::fileCameraName(camera));
            html += tr({td(QStringLiteral("<b>%1</b>").arg(escape(camera))), td(QString::number(counts.first), true),
                        td(QString::number(counts.second), true), td(QString::number(files.first), true),
                        td(hours(files.second), true)},
                       rowAttributes(index++, false));
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
    html += section(QStringLiteral("من ظهر"), rows.isEmpty() ? QString() : QStringLiteral("%1 شخص").arg(rows.size()));
    if (rows.isEmpty()) {
        html += empty(QStringLiteral("لم يظهر أحد من المسجلين أمام الكاميرات."));
    } else {
        html += open({QString(), QStringLiteral("الاسم"), QStringLiteral("^أول ظهور"), QStringLiteral("^آخر ظهور"), QStringLiteral("^المرات"),
                      QStringLiteral("الكاميرات")});
        int index = 0;
        for (const PersonRow &row : std::as_const(rows)) {
            const QString name = row.watched
                ? QStringLiteral("%1<br><span style=\"font-size:8pt; font-weight:normal; color:#d9542b\">⚠ مراقَب</span>").arg(escape(row.name))
                : escape(row.name);
            html += tr({QStringLiteral("<td width=\"52\">%1</td>").arg(images.tag(row.face)), td(name), td(time(row.first), true),
                        td(time(row.last), true), td(QString::number(row.count), true),
                        td(escape(row.cameras.join(QStringLiteral("، "))))},
                       rowAttributes(index++, row.watched));
        }
        html += QStringLiteral("</table>");
    }

    // الزوار المجهولون: من تكرر ظهوره أولاً
    QList<VisitorRow> unknown = visitors.values();
    std::sort(unknown.begin(), unknown.end(), [](const VisitorRow &a, const VisitorRow &b) {
        return a.count != b.count ? a.count > b.count : a.first < b.first;
    });
    html += section(QStringLiteral("الزوار المجهولون"),
                    unknown.isEmpty() ? QString()
                                      : QStringLiteral("%1 زائراً · %2 مرة ظهور%3")
                                            .arg(unknown.size())
                                            .arg(visitorSightings)
                                            .arg(unknown.size() > MaxVisitors ? QStringLiteral(" · أول %1 فقط").arg(MaxVisitors) : QString()));
    if (unknown.isEmpty()) {
        html += empty(QStringLiteral("لا زوار مجهولين."));
    } else {
        html += open({QString(), QStringLiteral("الزائر"), QStringLiteral("^أول ظهور"), QStringLiteral("^آخر ظهور"), QStringLiteral("^المرات"),
                      QStringLiteral("الكاميرات")});
        for (int i = 0; i < std::min<qsizetype>(unknown.size(), MaxVisitors); ++i) {
            const VisitorRow &row = unknown.at(i);
            html += tr({QStringLiteral("<td width=\"52\">%1</td>").arg(images.tag(row.face)), td(QStringLiteral("<b>⁦#%1⁩</b>").arg(row.id)),
                        td(time(row.first), true), td(time(row.last), true), td(QString::number(row.count), true),
                        td(escape(row.cameras.join(QStringLiteral("، "))))},
                       rowAttributes(i, false));
        }
        html += QStringLiteral("</table>");
    }

    // أحداث اليوم من سجل الأحداث
    const QStringList events = AppLog::lines(day);
    const qsizetype first = std::max<qsizetype>(0, events.size() - MaxEvents);
    html += section(QStringLiteral("أحداث اليوم"),
                    first > 0 ? QStringLiteral("آخر %1 من %2").arg(MaxEvents).arg(events.size())
                              : (events.isEmpty() ? QString() : QStringLiteral("%1 حدث").arg(events.size())));
    if (events.isEmpty()) {
        html += empty(QStringLiteral("لا أحداث مسجلة."));
    } else {
        html += open({QStringLiteral("^الوقت"), QStringLiteral("الحدث")});
        for (qsizetype i = first; i < events.size(); ++i) {
            const QString &line = events.at(i);
            const bool problem = line.contains(QStringLiteral("[انهيار]")) || line.contains(QStringLiteral("[خطأ]"))
                || line.contains(QStringLiteral("انقطع")) || line.contains(QStringLiteral("منقطعة"));
            html += tr({QStringLiteral("<td width=\"80\" align=\"center\">⁦%1⁩</td>").arg(escape(line.left(8))), td(escape(line.mid(10)))},
                       rowAttributes(int(i - first), problem));
        }
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("<p style=\"color:#a99a8a; font-size:8pt; margin-top:18px\">أُنشئ هذا التقرير محلياً ببرنامج ملامح دون إرسال أي بيانات عبر الإنترنت.</p>");
    html += QStringLiteral("</body></html>");
    return html;
}

} // namespace

void makeRightToLeft(QTextDocument *document)
{
    QTextCursor cursor(document);
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        format.setLayoutDirection(Qt::RightToLeft);
        cursor.setPosition(block.position());
        // خلايا الجداول تبقى على محاذاتها المكتوبة (توسيط الأرقام أو يسار الترويسة)؛ ما عداها إلى اليمين
        const Qt::Alignment horizontal = format.alignment() & Qt::AlignHorizontal_Mask;
        const Qt::Alignment vertical = format.alignment() & ~Qt::AlignHorizontal_Mask;
        if (!cursor.currentTable() || !(horizontal & (Qt::AlignHCenter | Qt::AlignLeft)))
            format.setAlignment(vertical | Qt::AlignRight | Qt::AlignAbsolute);
        else if (horizontal & Qt::AlignLeft)
            format.setAlignment(vertical | Qt::AlignLeft | Qt::AlignAbsolute);
        cursor.setBlockFormat(format);
    }
}

bool write(const QString &path, const FaceDatabase &database, const QSet<QString> &watched, const QDate &day, QString *error)
{
    if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        QTextDocument document;
        document.setHtml(build(database, watched, day, &document));
        makeRightToLeft(&document);
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

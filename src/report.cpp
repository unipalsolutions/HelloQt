#include "report.h"

#include "imageio.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTextStream>

#include <algorithm>

namespace Report {

namespace {

QString escape(const QString &text)
{
    return text.toHtmlEscaped();
}

QString statusText(MatchStatus status)
{
    switch (status) {
    case MatchStatus::Confirmed:
        return QStringLiteral("مؤكد");
    case MatchStatus::Possible:
        return QStringLiteral("محتمل");
    case MatchStatus::Unknown:
        break;
    }
    return QStringLiteral("غير مسجل");
}

QString statusClass(MatchStatus status)
{
    switch (status) {
    case MatchStatus::Confirmed:
        return QStringLiteral("ok");
    case MatchStatus::Possible:
        return QStringLiteral("maybe");
    case MatchStatus::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QString imageData(const QImage &image)
{
    if (image.isNull())
        return {};
    const QImage scaled = std::max(image.width(), image.height()) > 200
        ? image.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : image;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    scaled.save(&buffer, "JPG", 85);
    return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
}

// شريط زمني صغير: كل فترة ظهور مستطيل بموضعها من مدة الفيديو
QString timelineBar(const QList<QPair<qint64, qint64>> &appearances, qint64 duration)
{
    if (duration <= 0 || appearances.isEmpty())
        return {};
    QString bar = QStringLiteral("<div class=\"bar\">");
    for (const auto &range : appearances) {
        const double left = 100.0 * std::clamp<qint64>(range.first, 0, duration) / duration;
        const double width = std::max(0.4, 100.0 * (std::clamp<qint64>(range.second, 0, duration) - range.first) / duration);
        bar += QStringLiteral("<span style=\"left:%1%;width:%2%\"></span>").arg(left, 0, 'f', 2).arg(width, 0, 'f', 2);
    }
    return bar + QStringLiteral("</div>");
}

QString formatDuration(qint64 ms)
{
    const qint64 seconds = (ms + 500) / 1000;
    if (seconds < 60)
        return QStringLiteral("%1 ث").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1 د %2 ث").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1 س %2 د").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

const char *const Style = R"(
* { box-sizing: border-box; }
body { font-family: "Segoe UI", Tahoma, Arial, sans-serif; margin: 0; background: #f4f7fb; color: #0f1d33; }
header { background: linear-gradient(135deg, #0a1628, #13284a); color: #e6f0ff; padding: 28px 32px; }
header h1 { margin: 0 0 6px; font-size: 26px; }
header .meta { color: #8aa3c7; font-size: 14px; }
main { padding: 24px 32px 40px; max-width: 1200px; margin: auto; }
.stats { display: flex; gap: 12px; flex-wrap: wrap; margin-bottom: 24px; }
.stat { background: #fff; border-radius: 12px; padding: 14px 18px; min-width: 140px; box-shadow: 0 1px 3px rgba(0,0,0,.08); }
.stat b { display: block; font-size: 24px; color: #2f7cf6; }
.stat span { color: #5b6b85; font-size: 13px; }
h2 { font-size: 19px; margin: 28px 0 12px; }
table { width: 100%; border-collapse: collapse; background: #fff; border-radius: 12px; overflow: hidden; box-shadow: 0 1px 3px rgba(0,0,0,.08); }
th, td { padding: 10px 12px; text-align: right; border-bottom: 1px solid #e6ecf5; font-size: 14px; vertical-align: middle; }
th { background: #eef3fa; color: #3a4b66; font-weight: 600; }
td img { width: 56px; height: 56px; object-fit: cover; border-radius: 10px; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 14px; }
.card { background: #fff; border-radius: 14px; padding: 12px; box-shadow: 0 1px 3px rgba(0,0,0,.08); border-top: 4px solid #94a3b8; }
.card.ok { border-top-color: #22d3ee; } .card.maybe { border-top-color: #3b82f6; }
.card img { width: 100%; aspect-ratio: 1; object-fit: cover; border-radius: 10px; }
.card .name { font-weight: 600; margin: 8px 0 2px; }
.card .info { color: #5b6b85; font-size: 12.5px; line-height: 1.6; }
.pill { display: inline-block; padding: 1px 9px; border-radius: 99px; font-size: 12px; color: #fff; background: #94a3b8; }
.pill.ok { background: #0891b2; } .pill.maybe { background: #3b82f6; }
.bar { direction: ltr; position: relative; height: 10px; background: #e6ecf5; border-radius: 5px; margin-top: 6px; min-width: 160px; }
.bar span { position: absolute; top: 0; bottom: 0; background: #2f7cf6; border-radius: 3px; }
footer { color: #8aa3c7; font-size: 12px; text-align: center; padding: 20px; }
@media print { header { -webkit-print-color-adjust: exact; print-color-adjust: exact; } .card, .stat, table { box-shadow: none; border: 1px solid #dde4ef; } }
)";

} // namespace

qint64 screenTime(const QList<QPair<qint64, qint64>> &appearances)
{
    QList<QPair<qint64, qint64>> ranges = appearances;
    std::sort(ranges.begin(), ranges.end());
    qint64 total = 0, start = -1, end = -1;
    for (const auto &range : ranges) {
        if (start < 0 || range.first > end) {
            if (start >= 0)
                total += end - start;
            start = range.first;
            end = range.second;
        } else {
            end = std::max(end, range.second);
        }
    }
    if (start >= 0)
        total += end - start;
    return total;
}

bool writeHtml(const QString &path, const QList<FaceItem> &faces, const Source &source, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    int confirmed = 0, possible = 0, unknown = 0;
    for (const FaceItem &face : faces) {
        confirmed += face.match.status == MatchStatus::Confirmed ? 1 : 0;
        possible += face.match.status == MatchStatus::Possible ? 1 : 0;
        unknown += face.match.status == MatchStatus::Unknown ? 1 : 0;
    }

    // في الفيديو قد يظهر الشخص المعروف في عدة مسارات (خرج من الإطار ثم عاد)، فتُجمع مساراته
    struct PersonSummary
    {
        QString name;
        QList<QPair<qint64, qint64>> appearances;
        qint64 first = -1;
        float best = 0.0f;
        QImage image;
        float imageQuality = -1.0f;
    };
    QList<QString> order;
    QHash<QString, PersonSummary> persons;
    for (const FaceItem &face : faces) {
        if (face.match.status != MatchStatus::Confirmed || face.match.personId.isEmpty())
            continue;
        if (!persons.contains(face.match.personId))
            order << face.match.personId;
        PersonSummary &summary = persons[face.match.personId];
        summary.name = face.match.name;
        summary.appearances << face.appearances;
        if (face.appearances.isEmpty() && face.firstSeenMs >= 0)
            summary.appearances << qMakePair(face.firstSeenMs, face.lastSeenMs);
        if (face.firstSeenMs >= 0 && (summary.first < 0 || face.firstSeenMs < summary.first))
            summary.first = face.firstSeenMs;
        summary.best = std::max(summary.best, face.match.score);
        if (face.quality > summary.imageQuality) {
            summary.imageQuality = face.quality;
            summary.image = ImageIO::faceCrop(face);
        }
    }

    const QString name = QFileInfo(source.path).fileName();
    out << "<!doctype html>\n<html lang=\"ar\" dir=\"rtl\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        << "<title>" << escape(QStringLiteral("تقرير ملامح — %1").arg(name)) << "</title><style>" << Style << "</style></head><body>\n";
    out << "<header><h1>" << escape(source.video ? QStringLiteral("تقرير تحليل الفيديو") : QStringLiteral("تقرير تحليل الصورة"))
        << "</h1><div class=\"meta\">" << escape(name) << " · "
        << escape(QDateTime::currentDateTime().toString(QStringLiteral("yyyy/MM/dd hh:mm")));
    if (source.video && source.durationMs > 0)
        out << " · " << escape(QStringLiteral("المدة %1").arg(ImageIO::formatTime(source.durationMs)));
    out << "</div></header>\n<main>\n";

    out << "<div class=\"stats\">";
    const auto stat = [&out](int value, const QString &label) {
        out << "<div class=\"stat\"><b>" << value << "</b><span>" << escape(label) << "</span></div>";
    };
    stat(int(faces.size()), source.video ? QStringLiteral("وجه مختلف") : QStringLiteral("وجه"));
    stat(int(persons.size()), QStringLiteral("شخص معروف"));
    stat(confirmed, QStringLiteral("تعرّف مؤكد"));
    stat(possible, QStringLiteral("تعرّف محتمل"));
    stat(unknown, QStringLiteral("غير مسجل"));
    out << "</div>\n";

    if (!persons.isEmpty()) {
        // الأكثر ظهوراً أولاً
        std::sort(order.begin(), order.end(), [&persons](const QString &a, const QString &b) {
            return screenTime(persons[a].appearances) > screenTime(persons[b].appearances);
        });
        out << "<h2>" << escape(QStringLiteral("الأشخاص المعروفون")) << "</h2><table><tr><th></th><th>"
            << escape(QStringLiteral("الاسم")) << "</th><th>" << escape(QStringLiteral("أعلى تشابه")) << "</th>";
        if (source.video)
            out << "<th>" << escape(QStringLiteral("أول ظهور")) << "</th><th>" << escape(QStringLiteral("مدة الظهور"))
                << "</th><th>" << escape(QStringLiteral("مرات الظهور")) << "</th><th>" << escape(QStringLiteral("الخط الزمني")) << "</th>";
        out << "</tr>\n";
        for (const QString &id : std::as_const(order)) {
            const PersonSummary &summary = persons[id];
            out << "<tr><td><img src=\"" << imageData(summary.image) << "\" alt=\"\"></td><td>" << escape(summary.name)
                << "</td><td>" << qRound(summary.best * 100) << "%</td>";
            if (source.video)
                out << "<td>" << (summary.first >= 0 ? ImageIO::formatTime(summary.first) : QString()) << "</td><td>"
                    << escape(formatDuration(screenTime(summary.appearances))) << "</td><td>" << summary.appearances.size()
                    << "</td><td>" << timelineBar(summary.appearances, source.durationMs) << "</td>";
            out << "</tr>\n";
        }
        out << "</table>\n";
    }

    out << "<h2>" << escape(QStringLiteral("كل الوجوه")) << "</h2><div class=\"grid\">\n";
    for (int i = 0; i < faces.size(); ++i) {
        const FaceItem &face = faces.at(i);
        const QString css = statusClass(face.match.status);
        out << "<div class=\"card " << css << "\"><img src=\"" << imageData(ImageIO::faceCrop(face)) << "\" alt=\"\">"
            << "<div class=\"name\">" << (i + 1) << ". " << escape(matchLabel(face.match)) << "</div><div class=\"info\">"
            << "<span class=\"pill " << css << "\">" << escape(statusText(face.match.status)) << "</span> ";
        if (face.match.status != MatchStatus::Unknown)
            out << escape(QStringLiteral("تشابه %1%").arg(qRound(face.match.score * 100))) << " · ";
        out << escape(QStringLiteral("جودة %1%").arg(qRound(face.quality * 100)));
        if (source.video && face.firstSeenMs >= 0) {
            out << "<br>" << escape(QStringLiteral("من %1 إلى %2").arg(ImageIO::formatTime(face.firstSeenMs), ImageIO::formatTime(face.lastSeenMs)));
            if (!face.appearances.isEmpty())
                out << "<br>" << escape(QStringLiteral("ظهر %1 مرة · %2").arg(face.appearances.size()).arg(formatDuration(screenTime(face.appearances))));
            out << timelineBar(face.appearances, source.durationMs);
        }
        out << "</div></div>\n";
    }
    out << "</div>\n</main><footer>" << escape(QStringLiteral("أُنشئ بواسطة ملامح · كل المعالجة تمت على الجهاز نفسه"))
        << "</footer></body></html>\n";
    out.flush();
    if (out.status() != QTextStream::Ok) {
        if (error)
            *error = QStringLiteral("تعذر الكتابة في الملف");
        return false;
    }
    return true;
}

} // namespace Report

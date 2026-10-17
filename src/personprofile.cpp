#include "personprofile.h"

#include "cameras.h"
#include "dailyreport.h"
#include "theme.h"

#include <QDateTime>
#include <QFileInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QTextDocument>
#include <QTime>
#include <QUrl>

#include <algorithm>
#include <cmath>

using namespace Theme;

ProfileStrength profileStrength(const Person &person, const QHash<qint64, float> &consistency)
{
    ProfileStrength s;
    const int count = int(person.photos.size());
    double quality = 0.0;
    for (const FacePhoto &photo : person.photos)
        quality += photo.quality;
    quality = count > 0 ? quality / count : 0.0;

    double meanConsistency = 0.5;
    if (count > 1 && !consistency.isEmpty()) {
        double sum = 0.0;
        for (auto it = consistency.cbegin(); it != consistency.cend(); ++it) {
            sum += it.value();
            if (it.value() < SuspectConsistency)
                s.suspects << it.key();
        }
        meanConsistency = sum / consistency.size();
    }

    const double photosFactor = std::min(1.0, count / 8.0);
    const double consistencyFactor = std::clamp((meanConsistency - 0.2) / 0.4, 0.0, 1.0);
    s.score = count == 0 ? 0.0 : 0.45 * photosFactor + 0.3 * std::clamp(quality / 0.8, 0.0, 1.0) + 0.25 * consistencyFactor;
    if (!s.suspects.isEmpty())
        s.score *= 0.85;
    s.level = s.score >= 0.8 ? QStringLiteral("قوي") : (s.score >= 0.6 ? QStringLiteral("جيد") : (s.score >= 0.35 ? QStringLiteral("متوسط") : QStringLiteral("ضعيف")));

    if (count == 0)
        s.tips << QStringLiteral("لا توجد صور: أضف صورة واضحة للوجه على الأقل");
    else if (count < 4)
        s.tips << QStringLiteral("أضف %1 صور أخرى على الأقل من زوايا وإضاءات مختلفة").arg(4 - count);
    else if (count < 8)
        s.tips << QStringLiteral("صور إضافية متنوعة (نظارة، إضاءة خافتة، جانبية) تزيد الدقة");
    if (count > 0 && quality < 0.5)
        s.tips << QStringLiteral("أغلب الصور ضعيفة الوضوح؛ أضف صوراً أوضح أو اجمعها من فيديو");
    if (!s.suspects.isEmpty())
        s.tips << QStringLiteral("%1 صور قد تكون لشخص آخر (معلَّمة بعلامة تحذير) — راجعها واحذفها إن كانت خطأ").arg(s.suspects.size());
    return s;
}

StrengthMeter::StrengthMeter(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(92, 108);
}

void StrengthMeter::setStrength(const ProfileStrength &strength)
{
    m_strength = strength;
    m_valid = true;
    QString tip = QStringLiteral("قوة الملف: %1 (%2%)").arg(strength.level).arg(qRound(strength.score * 100));
    for (const QString &line : strength.tips)
        tip += QStringLiteral("\n• ") + line;
    setToolTip(tip);
    update();
}

void StrengthMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF ring(10, 4, 72, 72);
    const double thick = 7;
    p.setPen(QPen(Border, thick, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(ring.adjusted(thick / 2, thick / 2, -thick / 2, -thick / 2));
    if (!m_valid)
        return;
    const double v = std::clamp(m_strength.score, 0.0, 1.0);
    const QColor color = v >= 0.6 ? Success : (v >= 0.35 ? Accent2 : QColor(0xfb, 0x71, 0x85));
    QConicalGradient gradient(ring.center(), 90);
    gradient.setColorAt(0.0, color);
    gradient.setColorAt(1.0, mix(color, Accent, 0.7));
    p.setPen(QPen(QBrush(gradient), thick, Qt::SolidLine, Qt::RoundCap));
    if (v > 0.005)
        p.drawArc(ring.adjusted(thick / 2, thick / 2, -thick / 2, -thick / 2), 90 * 16, -int(360 * 16 * v));
    QFont font = p.font();
    font.setPointSizeF(13);
    font.setWeight(QFont::Bold);
    p.setFont(font);
    p.setPen(Text);
    p.drawText(ring, Qt::AlignCenter, QStringLiteral("%1%").arg(qRound(v * 100)));
    font.setPointSizeF(8.5);
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    p.setPen(color);
    p.drawText(QRectF(0, 80, width(), 24), Qt::AlignCenter, QStringLiteral("ملف %1").arg(m_strength.level));
}


bool exportPersonPdf(const FaceDatabase &database, const QString &personId, const QString &path)
{
    const Person person = database.person(personId);
    if (person.id.isEmpty())
        return false;

    QTextDocument document;
    document.setDefaultTextOption(QTextOption(Qt::AlignRight));
    QFont font = document.defaultFont();
    font.setPointSizeF(10.5);
    document.setDefaultFont(font);
    int resource = 0;
    auto addImage = [&](const QImage &image) {
        const QUrl url(QStringLiteral("img://%1").arg(++resource));
        document.addResource(QTextDocument::ImageResource, url, image);
        return url.toString();
    };

    const QHash<qint64, float> consistency = database.photoConsistency(personId);
    const ProfileStrength strength = profileStrength(person, consistency);
    const QString strengthColor = strength.score >= 0.6 ? QStringLiteral("#3e8a63")
                                                        : (strength.score >= 0.35 ? QStringLiteral("#d98a10") : QStringLiteral("#d9542b"));
    // جداول QTextDocument لا تنعكس مع dir=rtl، فتُكتب الخلايا من اليسار إلى اليمين (الأخيرة هي اليمنى)
    const auto section = [](const QString &title, const QString &note = QString()) {
        return QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0' style='margin-top:18px; margin-bottom:8px'><tr>"
                              "<td width='30%' align='left' style='border-bottom:2px solid #3e8a63; padding-bottom:4px; color:#7a6b5d; font-size:9pt'>%2</td>"
                              "<td width='70%' align='right' style='border-bottom:2px solid #3e8a63; padding-bottom:4px; color:#2b231d; font-size:14pt; font-weight:bold'>%1</td>"
                              "</tr></table>")
            .arg(title, note);
    };

    QString html = QStringLiteral("<html><body dir='rtl' style='color:#241c16'>");

    // الترويسة: الصورة والاسم يميناً، والبرنامج والتاريخ يساراً
    html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='14' bgcolor='#2b231d'><tr>"
                           "<td width='150' align='left' valign='bottom'><span style='color:#ffffff; font-size:12pt; font-weight:bold'>ملامح</span><br>"
                           "<span style='color:#e3d3c0; font-size:8pt'>ملف شخص<br>⁦%2⁩</span></td>"
                           "<td align='right' valign='middle'><span style='color:#ffffff; font-size:22pt; font-weight:bold'>%1</span><br>"
                           "<span style='color:#ffffff; background-color:%3; font-size:9pt; font-weight:bold'>&nbsp; ملف %4 · %5% &nbsp;</span></td>")
                .arg(person.name.toHtmlEscaped(), QDateTime::currentDateTime().toString(QStringLiteral("yyyy/MM/dd HH:mm")), strengthColor,
                     strength.level)
                .arg(qRound(strength.score * 100));
    if (person.coverPhoto > 0)
        html += QStringLiteral("<td width='110' align='right'><img src='%1' width='100' height='100'></td>")
                    .arg(addImage(database.photo(person.coverPhoto, 360)));
    html += QStringLiteral("</tr></table>");

    // البيانات: صفوف متناوبة اللون، العنوان يميناً والقيمة بجانبه
    QList<QPair<QString, QString>> fields;
    // number: أرقام وتواريخ تُعزل عن اتجاه النص العربي فلا تنقلب أجزاؤها
    const auto field = [&fields](const QString &key, const QString &value, bool number = false) {
        if (!value.isEmpty())
            fields.append({key, number ? QStringLiteral("⁦%1⁩").arg(value.toHtmlEscaped()) : value.toHtmlEscaped()});
    };
    field(QStringLiteral("رقم الهوية"), person.nationalId);
    field(QStringLiteral("الجنس"), person.gender);
    if (person.birthDate.isValid()) {
        const int age = person.birthDate.daysTo(QDate::currentDate()) / 365;
        field(QStringLiteral("تاريخ الميلاد"), QStringLiteral("%1 (%2 سنة)").arg(person.birthDate.toString(QStringLiteral("yyyy/MM/dd"))).arg(age));
    }
    field(QStringLiteral("الهاتف"), person.phone, true);
    field(QStringLiteral("البريد"), person.email);
    field(QStringLiteral("العنوان"), person.address);
    field(QStringLiteral("التصنيفات"), person.tags);
    field(QStringLiteral("أُضيف"), person.created.toString(QStringLiteral("yyyy/MM/dd HH:mm")), true);
    if (person.lastSeen.isValid())
        field(QStringLiteral("آخر ظهور"),
              QStringLiteral("%1 (%2 مرة)").arg(person.lastSeen.toString(QStringLiteral("yyyy/MM/dd HH:mm"))).arg(person.seenCount));
    field(QStringLiteral("عدد الصور"), QString::number(person.photos.size()));
    html += section(QStringLiteral("البيانات"));
    html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='7'>");
    for (int i = 0; i < fields.size(); ++i) {
        html += QStringLiteral("<tr%3><td align='right' style='border-bottom:1px solid #ebe3d9'>%2</td>"
                               "<td width='140' align='right' style='border-bottom:1px solid #ebe3d9; color:#7a6b5d'>%1</td></tr>")
                    .arg(fields.at(i).first, fields.at(i).second,
                         i % 2 ? QStringLiteral(" bgcolor='#faf7f3'") : QString());
    }
    html += QStringLiteral("</table>");
    if (!person.notes.isEmpty())
        html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='10' style='margin-top:10px'><tr>"
                               "<td bgcolor='#fdf5e6' align='right' style='border-right:3px solid #f5a524'>"
                               "<span style='color:#92400e; font-weight:bold'>ملاحظات</span><br>%1</td></tr></table>")
                    .arg(person.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (!strength.tips.isEmpty()) {
        html += QStringLiteral("<p style='color:#5e5044; font-size:9pt; margin-top:8px'>");
        for (const QString &tip : strength.tips)
            html += QStringLiteral("• %1<br>").arg(tip.toHtmlEscaped());
        html += QStringLiteral("</p>");
    }

    if (!person.photos.isEmpty()) {
        // ستة في كل صف، تبدأ من اليمين: الخلايا تُكتب معكوسة ويُكمَل الصف الأخير بخلايا فارغة على اليسار
        html += section(QStringLiteral("صور الوجه المسجلة"), QStringLiteral("%1 صورة").arg(person.photos.size()));
        html += QStringLiteral("<table width='100%' cellspacing='6' cellpadding='6'>");
        QStringList cells;
        const auto flush = [&] {
            while (cells.size() < 6)
                cells << QStringLiteral("<td width='16%'></td>");
            std::reverse(cells.begin(), cells.end());
            html += QStringLiteral("<tr>") + cells.join(QString()) + QStringLiteral("</tr>");
            cells.clear();
        };
        for (const FacePhoto &photo : person.photos) {
            const bool suspect = consistency.value(photo.id, 1.0f) < SuspectConsistency;
            cells << QStringLiteral("<td align='center' width='16%' bgcolor='%2'><img src='%1' width='84' height='84'><br>"
                                    "<span style='font-size:8pt; color:%3'>جودة %4%%5</span></td>")
                         .arg(addImage(database.photo(photo.id, 240)), suspect ? QStringLiteral("#fdf0ea") : QStringLiteral("#f3eee8"),
                              suspect ? QStringLiteral("#d9542b") : QStringLiteral("#7a6b5d"))
                         .arg(qRound(photo.quality * 100))
                         .arg(suspect ? QStringLiteral("<br>⚠ مشكوك فيها") : QString());
            if (cells.size() == 6)
                flush();
        }
        if (!cells.isEmpty())
            flush();
        html += QStringLiteral("</table>");
    }

    const QList<Sighting> sightings = database.sightings(personId, 40);
    if (!sightings.isEmpty()) {
        html += section(QStringLiteral("سجل الظهور"), QStringLiteral("آخر %1").arg(sightings.size()));
        const QString th = QStringLiteral("<th bgcolor='#2b231d' align='%1' style='color:#ffffff; font-size:9pt'>%2</th>");
        html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='6'><thead><tr>")
            + th.arg(QStringLiteral("center"), QStringLiteral("التشابه")) + th.arg(QStringLiteral("center"), QStringLiteral("الموضع"))
            + th.arg(QStringLiteral("right"), QStringLiteral("المصدر")) + th.arg(QStringLiteral("center"), QStringLiteral("التاريخ"))
            + th.arg(QStringLiteral("right"), QString()) + QStringLiteral("</tr></thead>");
        int index = 0;
        for (const Sighting &sighting : sightings) {
            const QString position = sighting.positionMs >= 0
                ? QTime(0, 0).addMSecs(int(sighting.positionMs)).toString(QStringLiteral("hh:mm:ss"))
                : QStringLiteral("صورة");
            const int score = qRound(sighting.score * 100);
            const QString cell = QStringLiteral("<td align='%1' style='border-bottom:1px solid #ebe3d9'>%2</td>");
            html += QStringLiteral("<tr%1>").arg(index++ % 2 ? QStringLiteral(" bgcolor='#faf7f3'") : QString())
                + cell.arg(QStringLiteral("center"), QStringLiteral("<b style='color:%1'>%2%</b>")
                                                         .arg(score >= 60 ? QStringLiteral("#3e8a63") : QStringLiteral("#d98a10"))
                                                         .arg(score))
                + cell.arg(QStringLiteral("center"), QStringLiteral("⁦%1⁩").arg(position))
                + cell.arg(QStringLiteral("right"), (sighting.source.startsWith(QStringLiteral("camera:"))
                                                          ? Cameras::displayName(sighting.source)
                                                          : QFileInfo(sighting.source).fileName()).toHtmlEscaped())
                + cell.arg(QStringLiteral("center"), QStringLiteral("⁦%1⁩").arg(sighting.seen.toString(QStringLiteral("yyyy/MM/dd HH:mm"))))
                + QStringLiteral("<td width='44' align='right' style='border-bottom:1px solid #ebe3d9'>%1</td></tr>")
                      .arg(sighting.thumbnail.isNull() ? QString()
                                                       : QStringLiteral("<img src='%1' width='34' height='34'>").arg(addImage(sighting.thumbnail)));
        }
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("<p style='color:#a99a8a; font-size:8pt; margin-top:18px'>أُنشئ هذا الملف محلياً ببرنامج ملامح دون إرسال أي بيانات عبر الإنترنت.</p>");
    html += QStringLiteral("</body></html>");
    document.setHtml(html);
    DailyReport::makeRightToLeft(&document);

    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(14, 14, 14, 14), QPageLayout::Millimeter);
    writer.setResolution(150);
    writer.setTitle(person.name);
    writer.setCreator(QStringLiteral("Malamih"));
    document.print(&writer); // يقسّم المحتوى على الصفحات تلقائياً
    return true;
}

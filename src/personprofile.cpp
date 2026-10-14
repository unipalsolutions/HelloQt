#include "personprofile.h"

#include "cameras.h"
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
    auto row = [](const QString &key, const QString &value) {
        if (value.isEmpty())
            return QString();
        // جداول QTextDocument لا تنعكس مع dir=rtl، فتُرتَّب الأعمدة يدوياً من اليمين
        return QStringLiteral("<tr><td align='right' style='padding:3px 0'>%2</td>"
                              "<td align='right' style='color:#64748b; padding:3px 0 3px 18px' width='130'>%1</td></tr>")
            .arg(key, value.toHtmlEscaped());
    };

    QString html = QStringLiteral("<html><body dir='rtl' style='color:#0f172a'>");
    html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0'><tr>"
                           "<td align='right' style='padding-right:16px'><div style='font-size:22pt; font-weight:700'>%1</div>"
                           "<div style='color:#2563eb; font-size:10pt'>ملف شخص — ملامح · %2</div></td>")
                .arg(person.name.toHtmlEscaped(), QDateTime::currentDateTime().toString(QStringLiteral("yyyy/MM/dd hh:mm")));
    if (person.coverPhoto > 0)
        html += QStringLiteral("<td width='130' align='left'><img src='%1' width='120' height='120'></td>")
                    .arg(addImage(database.photo(person.coverPhoto, 360)));
    html += QStringLiteral("</tr></table><hr style='color:#cbd5e1'>");

    html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0'>");
    html += row(QStringLiteral("رقم الهوية"), person.nationalId);
    html += row(QStringLiteral("الجنس"), person.gender);
    if (person.birthDate.isValid()) {
        const int age = person.birthDate.daysTo(QDate::currentDate()) / 365;
        html += row(QStringLiteral("تاريخ الميلاد"), QStringLiteral("%1 (%2 سنة)").arg(person.birthDate.toString(QStringLiteral("yyyy/MM/dd"))).arg(age));
    }
    html += row(QStringLiteral("الهاتف"), person.phone);
    html += row(QStringLiteral("البريد"), person.email);
    html += row(QStringLiteral("العنوان"), person.address);
    html += row(QStringLiteral("التصنيفات"), person.tags);
    html += row(QStringLiteral("أُضيف"), person.created.toString(QStringLiteral("yyyy/MM/dd hh:mm")));
    if (person.lastSeen.isValid())
        html += row(QStringLiteral("آخر ظهور"), QStringLiteral("%1 (%2 مرة)").arg(person.lastSeen.toString(QStringLiteral("yyyy/MM/dd hh:mm"))).arg(person.seenCount));
    html += row(QStringLiteral("قوة الملف"), QStringLiteral("%1 — %2%").arg(strength.level).arg(qRound(strength.score * 100)));
    html += row(QStringLiteral("عدد الصور"), QString::number(person.photos.size()));
    html += QStringLiteral("</table>");
    if (!person.notes.isEmpty())
        html += QStringLiteral("<p style='background:#f1f5f9; padding:8px'>%1</p>")
                    .arg(person.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));

    if (!person.photos.isEmpty()) {
        html += QStringLiteral("<h3 style='color:#1e3a8a'>صور الوجه المسجلة</h3><table cellspacing='6'><tr>");
        int column = 0;
        for (const FacePhoto &photo : person.photos) {
            if (column == 6) {
                html += QStringLiteral("</tr><tr>");
                column = 0;
            }
            const bool suspect = consistency.value(photo.id, 1.0f) < SuspectConsistency;
            html += QStringLiteral("<td align='center'><img src='%1' width='92' height='92'><br>"
                                   "<span style='font-size:8pt; color:%2'>جودة %3%%4</span></td>")
                        .arg(addImage(database.photo(photo.id, 240)), suspect ? QStringLiteral("#e11d48") : QStringLiteral("#64748b"))
                        .arg(qRound(photo.quality * 100))
                        .arg(suspect ? QStringLiteral(" · مشكوك فيها") : QString());
            ++column;
        }
        html += QStringLiteral("</tr></table>");
    }

    const QList<Sighting> sightings = database.sightings(personId, 40);
    if (!sightings.isEmpty()) {
        html += QStringLiteral("<h3 style='color:#1e3a8a'>سجل الظهور</h3>"
                               "<table width='100%' cellspacing='0' cellpadding='4' style='border-collapse:collapse'>"
                               "<tr style='background:#e2e8f0'><th>التشابه</th><th>الموضع</th><th>الملف</th><th align='right'>التاريخ</th><th></th></tr>");
        for (const Sighting &sighting : sightings) {
            const QString position = sighting.positionMs >= 0
                ? QTime(0, 0).addMSecs(int(sighting.positionMs)).toString(QStringLiteral("hh:mm:ss"))
                : QStringLiteral("صورة");
            html += QStringLiteral("<tr><td align='center'>%5%</td><td align='center'>%4</td><td align='center'>%3</td>"
                                   "<td align='right'>%2</td><td width='40' align='right'>%1</td></tr>")
                        .arg(sighting.thumbnail.isNull() ? QString() : QStringLiteral("<img src='%1' width='34' height='34'>").arg(addImage(sighting.thumbnail)),
                             sighting.seen.toString(QStringLiteral("yyyy/MM/dd hh:mm")),
                             (sighting.source.startsWith(QStringLiteral("camera:"))
                                  ? Cameras::displayName(sighting.source)
                                  : QFileInfo(sighting.source).fileName()).toHtmlEscaped(),
                             position,
                             QString::number(qRound(sighting.score * 100)));
        }
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("<p style='color:#94a3b8; font-size:8pt'>أُنشئ هذا الملف محلياً ببرنامج ملامح دون إرسال أي بيانات عبر الإنترنت.</p>");
    html += QStringLiteral("</body></html>");
    document.setHtml(html);

    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(14, 14, 14, 14), QPageLayout::Millimeter);
    writer.setResolution(150);
    writer.setTitle(person.name);
    writer.setCreator(QStringLiteral("Malamih"));
    document.print(&writer); // يقسّم المحتوى على الصفحات تلقائياً
    return true;
}

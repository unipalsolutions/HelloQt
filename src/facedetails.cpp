#include "facedetails.h"

#include "carddelegates.h"
#include "imageio.h"
#include "report.h"
#include "theme.h"
#include "widgets.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int MaxAppearanceChips = 40; // الفترات الأكثر من هذا تُختصر في زر واحد

QPixmap roundedShot(const QImage &image, int size)
{
    const qreal dpr = 2.0;
    QPixmap result(int(size * dpr), int(size * dpr));
    result.fill(Qt::transparent);
    if (image.isNull())
        return result;
    const QImage scaled = image.scaled(result.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, result.width(), result.height()), 14 * dpr, 14 * dpr);
    p.setClipPath(clip);
    p.drawImage(QPointF((result.width() - scaled.width()) / 2.0, (result.height() - scaled.height()) / 2.0), scaled);
    p.end();
    result.setDevicePixelRatio(dpr);
    return result;
}

QString timeRange(qint64 from, qint64 to)
{
    return to > from + 999 ? QStringLiteral("%1–%2").arg(ImageIO::formatTime(from), ImageIO::formatTime(to)) : ImageIO::formatTime(from);
}

} // namespace

FaceDetailsPanel::FaceDetailsPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_content = new QWidget(this);
    outer->addWidget(m_content);
    outer->addStretch();
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(0, 4, 0, 4);
    m_layout->setSpacing(6);
}

// المحتوى كله يُستبدل بجديد، فلا يبقى عنصر من وجه سابق في تخطيط متداخل
void FaceDetailsPanel::clearContent()
{
    auto *outer = static_cast<QVBoxLayout *>(layout());
    auto *content = new QWidget(this);
    outer->replaceWidget(m_content, content);
    m_content->hide();
    m_content->deleteLater();
    m_content = content;
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(0, 4, 0, 4);
    m_layout->setSpacing(6);
}

QLabel *FaceDetailsPanel::sectionLabel(const QString &text)
{
    auto *label = new QLabel(text, m_content);
    label->setStyleSheet(QStringLiteral("color: %1; font-weight: 600; margin-top: 4px;").arg(Theme::Muted.name()));
    return label;
}

void FaceDetailsPanel::showMessage(const QString &text)
{
    m_faceId = 0;
    clearContent();
    auto *label = new QLabel(text, m_content);
    label->setWordWrap(true);
    label->setObjectName(QStringLiteral("muted"));
    m_layout->addWidget(label);
}

void FaceDetailsPanel::showDetails(const Details &d)
{
    m_faceId = d.faceId;
    clearContent();
    const int faceId = d.faceId;

    // أفضل لقطة بجانب الاسم والحالة
    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    auto *shot = new QToolButton(m_content);
    shot->setIcon(QIcon(roundedShot(d.shot, 84)));
    shot->setIconSize(QSize(84, 84));
    shot->setAutoRaise(true);
    shot->setCursor(Qt::PointingHandCursor);
    shot->setToolTip(d.canSeek && d.bestMs >= 0 ? QStringLiteral("أوضح لقطة عند %1: انقر للانتقال إليها").arg(ImageIO::formatTime(d.bestMs))
                                                : QStringLiteral("انقر لتكبير الوجه"));
    shot->setStyleSheet(QStringLiteral("QToolButton { border: 2px solid %1; border-radius: 16px; padding: 1px; }"
                                       "QToolButton:hover { border-color: %2; }")
                            .arg((d.watched ? Theme::Watched : d.color).name(), Theme::Accent2.name()));
    connect(shot, &QToolButton::clicked, this, [this, faceId] { emit shotRequested(faceId); });
    header->addWidget(shot, 0, Qt::AlignTop);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto *title = new QLabel(d.title, m_content);
    title->setWordWrap(true);
    title->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: 600; color: %1;").arg(Theme::Text.name()));
    titles->addWidget(title);
    auto *status = new QLabel(d.status, m_content);
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: %1;").arg(d.color.name()));
    titles->addWidget(status);
    if (d.watched) {
        auto *watched = new QLabel(QStringLiteral("⚠ في قائمة المراقبة"), m_content);
        watched->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(Theme::Watched.name()));
        titles->addWidget(watched);
    }
    // صياغة قصيرة كي لا تُقص النسبة في اللوحة الضيقة، والكاملة في التلميح
    // بلا مرشحين (القاعدة فارغة مثلاً) لا معنى لنسبة تشابه صفرية، فتُعرض الجودة وحدها
    const bool compared = !d.candidates.isEmpty();
    auto *scores = new QLabel(compared ? QStringLiteral("تشابه %1% · جودة %2%").arg(d.score).arg(d.quality)
                                       : QStringLiteral("جودة %1%").arg(d.quality), m_content);
    scores->setObjectName(QStringLiteral("muted"));
    scores->setToolTip(compared ? QStringLiteral("التشابه %1% · جودة الوجه %2%").arg(d.score).arg(d.quality)
                                : QStringLiteral("جودة الوجه %1% · لا أحد في القاعدة يُقارن به").arg(d.quality));
    titles->addWidget(scores);
    if (d.quality < 35) {
        auto *low = new QLabel(QStringLiteral("جودة الوجه منخفضة: النتيجة أقل موثوقية"), m_content);
        low->setWordWrap(true);
        low->setStyleSheet(QStringLiteral("color: #f5a524;"));
        titles->addWidget(low);
    }
    titles->addStretch();
    header->addLayout(titles, 1);
    m_layout->addLayout(header);

    // المرشحون بصورهم: «هذا هو» يضيف الوجه إلى صور المرشح
    if (!d.candidates.isEmpty()) {
        m_layout->addWidget(sectionLabel(QStringLiteral("أقرب المرشحين في القاعدة")));
        for (const Candidate &candidate : d.candidates) {
            auto *row = new QHBoxLayout;
            row->setSpacing(8);
            auto *avatar = new QLabel(m_content);
            avatar->setFixedSize(34, 34);
            avatar->setPixmap(candidate.avatar);
            avatar->setStyleSheet(QStringLiteral("border: 2px solid %1; border-radius: 17px;").arg(matchColor(candidate.status).name()));
            row->addWidget(avatar);
            auto *name = new QLabel(candidate.name, m_content);
            name->setMinimumWidth(10);
            row->addWidget(name, 1);
            auto *percent = new QLabel(QStringLiteral("%1%").arg(candidate.percent), m_content);
            percent->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(matchColor(candidate.status).name()));
            row->addWidget(percent);
            if (candidate.current) {
                auto *mark = new QLabel(QStringLiteral("✓"), m_content);
                mark->setFixedWidth(52);
                mark->setAlignment(Qt::AlignCenter);
                mark->setStyleSheet(QStringLiteral("color: %1; font-weight: 700;").arg(matchColor(MatchStatus::Confirmed).name()));
                row->addWidget(mark);
            } else {
                auto *assign = new QToolButton(m_content);
                assign->setObjectName(QStringLiteral("chip"));
                assign->setText(QStringLiteral("هذا هو"));
                assign->setFixedWidth(52);
                assign->setEnabled(d.canAssign);
                assign->setToolTip(QStringLiteral("إضافة الوجه إلى صور %1 فتتحسن مطابقته في المرات القادمة").arg(candidate.name));
                connect(assign, &QToolButton::clicked, this, [this, faceId, id = candidate.personId] { emit assignRequested(faceId, id); });
                row->addWidget(assign);
            }
            m_layout->addLayout(row);
        }
        auto *other = new QToolButton(m_content);
        other->setObjectName(QStringLiteral("chip"));
        // نص قصير يتسع للوحة الضيقة، والشرح الكامل في التلميح
        other->setText(QStringLiteral("ليس أحدهم؟ اختر غيره…"));
        other->setToolTip(QStringLiteral("اختر شخصاً آخر من القاعدة أو أضف شخصاً جديداً"));
        other->setEnabled(d.canAssign);
        connect(other, &QToolButton::clicked, this, [this, faceId] { emit otherPersonRequested(faceId); });
        m_layout->addWidget(other, 0, Qt::AlignLeading);
    }

    // فترات الظهور: كل فترة زر ينقل إلى بدايتها
    if (d.video && !d.appearances.isEmpty()) {
        m_layout->addWidget(sectionLabel(QStringLiteral("فترات الظهور (%1) · المدة %2")
                                             .arg(d.appearances.size())
                                             .arg(ImageIO::formatTime(Report::screenTime(d.appearances)))));
        auto *chipsHost = new QWidget(m_content);
        auto *chips = new FlowLayout(chipsHost, 4, 4);
        chips->setContentsMargins(0, 0, 0, 0);
        const int shown = int(std::min<qsizetype>(d.appearances.size(), MaxAppearanceChips));
        for (int i = 0; i < shown; ++i) {
            const auto &[from, to] = d.appearances.at(i);
            auto *chip = new QToolButton(chipsHost);
            chip->setObjectName(QStringLiteral("chip"));
            chip->setText(timeRange(from, to));
            chip->setEnabled(d.canSeek);
            chip->setCursor(Qt::PointingHandCursor);
            chip->setToolTip(QStringLiteral("الانتقال إلى هذا الظهور"));
            connect(chip, &QToolButton::clicked, this, [this, from = from] { emit seekRequested(from); });
            chips->addWidget(chip);
        }
        if (d.appearances.size() > shown) {
            auto *more = new QLabel(QStringLiteral("+%1").arg(d.appearances.size() - shown), chipsHost);
            more->setObjectName(QStringLiteral("muted"));
            chips->addWidget(more);
        }
        m_layout->addWidget(chipsHost);
    }

    if (!d.rows.isEmpty()) {
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(3);
        for (int i = 0; i < d.rows.size(); ++i) {
            auto *key = new QLabel(d.rows.at(i).first, m_content);
            key->setObjectName(QStringLiteral("muted"));
            auto *value = new QLabel(d.rows.at(i).second, m_content);
            value->setWordWrap(true);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            grid->addWidget(key, i, 0, Qt::AlignTop);
            grid->addWidget(value, i, 1);
        }
        grid->setColumnStretch(1, 1);
        m_layout->addSpacing(4);
        m_layout->addLayout(grid);
    }
}

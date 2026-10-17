#include "harvestdialog.h"

#include "faceengine.h"
#include "imageio.h"
#include "videofilters.h"
#include "theme.h"
#include "widgets.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <opencv2/videoio.hpp>

#include <algorithm>

namespace {

constexpr int MaxShots = 16;     // أقصى عدد للقطات المقترحة
constexpr int PreselectCount = 6; // تُحدَّد أفضلها مسبقاً

struct Candidate
{
    HarvestShot shot;
    double rank = 0.0;
};

QList<HarvestShot> scanVideo(const FaceEngine *engine, const QString &path, const std::vector<float> &target,
                             const QList<QPair<qint64, qint64>> &ranges, float threshold,
                             const std::shared_ptr<HarvestProgress> &progress)
{
    cv::VideoCapture capture;
    if (!ImageIO::openVideo(capture, path))
        return {};
    const double fps = capture.get(cv::CAP_PROP_FPS) > 1 ? capture.get(cv::CAP_PROP_FPS) : 25.0;
    const qint64 frameCount = qint64(capture.get(cv::CAP_PROP_FRAME_COUNT));
    const qint64 duration = frameCount > 0 ? qint64(frameCount * 1000.0 / fps) : 0;

    // مواضع العينات: فترات ظهور الشخص إن عُرفت، وإلا الفيديو كله (نحو إطارين في الثانية، 900 عينة على الأكثر)
    QList<qint64> times;
    if (!ranges.isEmpty()) {
        for (const auto &range : ranges)
            for (qint64 t = std::max<qint64>(0, range.first); t <= range.second; t += 400)
                times << t;
    } else if (duration > 0) {
        const qint64 step = std::max<qint64>(500, duration / 900);
        for (qint64 t = 0; t < duration; t += step)
            times << t;
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    progress->total = std::max(1, int(times.size()));

    // داخل فترات الظهور نعرف أن الوجه له، فيُقبل تشابه أقل؛ أما في الفيديو كله فنشترط حد التأكيد
    const float accept = ranges.isEmpty() ? threshold : std::max(0.25f, threshold - 0.08f);
    QList<Candidate> candidates;
    const VideoFilters::Settings filters = VideoFilters::current(); // الفيديو المقلوب أو المظلم يُعالج كما في التحليل
    qint64 currentFrame = -1;
    cv::Mat frame;
    for (int i = 0; i < times.size() && !progress->cancel; ++i) {
        const qint64 wanted = qint64(times[i] * fps / 1000.0);
        // القفزات القصيرة أسرع بالقراءة المتتابعة من البحث في الملف
        if (currentFrame < 0 || wanted < currentFrame || wanted - currentFrame > qint64(fps * 3)) {
            capture.set(cv::CAP_PROP_POS_FRAMES, double(wanted));
            currentFrame = wanted;
        }
        bool ok = true;
        while (ok && currentFrame < wanted) {
            ok = capture.grab();
            ++currentFrame;
        }
        ok = ok && capture.read(frame);
        ++currentFrame;
        progress->done = i + 1;
        if (!ok || frame.empty())
            continue;
        frame = VideoFilters::apply(frame, filters);
        const std::vector<AnalyzedFace> faces = engine->analyze(frame, false);
        for (const AnalyzedFace &face : faces) {
            const float similarity = FaceEngine::cosine(target, face.embedding);
            if (similarity < accept || face.quality < 0.3f)
                continue;
            Candidate candidate;
            candidate.shot.face = ImageIO::makeFaceItem(frame, face, candidates.size() + 1, 320);
            candidate.shot.positionMs = times[i];
            candidate.shot.similarity = similarity;
            candidate.rank = 0.6 * face.quality + 0.4 * similarity;
            candidates << candidate;
            ++progress->found;
        }
    }

    // اختيار متنوع: الأوضح أولاً، مع استبعاد اللقطات المتشابهة جداً أو المتقاربة زمنياً
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.rank > b.rank; });
    QList<HarvestShot> picked;
    for (const Candidate &candidate : std::as_const(candidates)) {
        bool diverse = true;
        for (const HarvestShot &shot : std::as_const(picked)) {
            if (std::abs(shot.positionMs - candidate.shot.positionMs) < 1000
                || FaceEngine::cosine(shot.face.embedding, candidate.shot.face.embedding) > 0.88f) {
                diverse = false;
                break;
            }
        }
        if (diverse)
            picked << candidate.shot;
        if (picked.size() >= MaxShots)
            break;
    }
    return picked;
}

constexpr int CheckedRole = Qt::UserRole + 1;

// صورة اللقطة: المحددة بإطار مضيء وعلامة صح، وغير المحددة باهتة
QPixmap shotIcon(const QImage &crop, int size, bool checked)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double radius = size * 0.1;
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
    p.save();
    p.setClipPath(clip);
    const QImage scaled = crop.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    p.drawImage(QPointF((size - scaled.width()) / 2.0, (size - scaled.height()) / 2.0), scaled);
    if (!checked)
        p.fillRect(QRectF(0, 0, size, size), Theme::withAlpha(Theme::Viewport, 140));
    p.restore();
    const double w = size * 0.035;
    if (checked) {
        p.setPen(QPen(Theme::Success, w));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(w / 2, w / 2, size - w, size - w), radius, radius);
    }
    // علامة التحديد في الزاوية
    const QPointF c(size * 0.84, size * 0.16);
    const double r = size * 0.1;
    p.setPen(checked ? Qt::NoPen : QPen(QColor(255, 255, 255, 200), w * 0.7));
    p.setBrush(checked ? Theme::Success : Theme::withAlpha(Theme::Viewport, 150));
    p.drawEllipse(c, r, r);
    if (checked)
        Icons::paint(p, Icons::Name::Check, QRectF(c.x() - r * 0.7, c.y() - r * 0.7, r * 1.4, r * 1.4), Theme::Viewport);
    return pixmap;
}

} // namespace

HarvestDialog::HarvestDialog(const FaceEngine *engine, const QString &videoPath, const std::vector<float> &target,
                             const QList<QPair<qint64, qint64>> &ranges, float threshold, const QString &personName,
                             QWidget *parent)
    : QDialog(parent)
    , m_progress(std::make_shared<HarvestProgress>())
    , m_status(new QLabel(this))
    , m_bar(new QProgressBar(this))
    , m_grid(new QListWidget(this))
    , m_timer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("جمع صور %1 من الفيديو").arg(personName));
    resize(820, 600);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("تعزيز صور %1").arg(personName), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("يبحث البرنامج في \"%1\" %2 ويقترح أوضح اللقطات وأكثرها تنوعاً. "
                                           "إضافة صور من زوايا وإضاءات مختلفة تجعل التعرف على الشخص لاحقاً أسهل وأدق.")
                                .arg(QFileInfo(videoPath).fileName(),
                                     ranges.isEmpty() ? QStringLiteral("كله") : QStringLiteral("في فترات ظهوره")),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_bar->setObjectName(QStringLiteral("thinProgress"));
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(6);
    layout->addWidget(m_bar);
    m_status->setObjectName(QStringLiteral("muted"));
    layout->addWidget(m_status);

    m_grid->setObjectName(QStringLiteral("photoGrid"));
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setIconSize(QSize(120, 120));
    m_grid->setGridSize(QSize(150, 196));
    m_grid->setTextElideMode(Qt::ElideNone);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setWordWrap(true);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(m_grid, 1);
    connect(m_grid, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        setChecked(item, !item->data(CheckedRole).toBool());
        updateSelection();
    });

    auto *buttons = new QHBoxLayout;
    m_selectAll = new FancyButton(Icons::Name::Check, QStringLiteral("تحديد الكل"), FancyButton::Kind::Secondary, this);
    m_selectNone = new FancyButton(Icons::Name::Stop, QStringLiteral("إلغاء التحديد"), FancyButton::Kind::Secondary, this);
    auto setAll = [this](bool checked) {
        for (int i = 0; i < m_grid->count(); ++i)
            setChecked(m_grid->item(i), checked);
        updateSelection();
    };
    connect(m_selectAll, &QAbstractButton::clicked, this, [setAll] { setAll(true); });
    connect(m_selectNone, &QAbstractButton::clicked, this, [setAll] { setAll(false); });
    auto *close = new FancyButton(Icons::Name::Reject, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, this);
    connect(close, &QAbstractButton::clicked, this, &HarvestDialog::reject);
    m_add = new FancyButton(Icons::Name::PhotoAdd, QStringLiteral("إضافة المحدد"), FancyButton::Kind::Primary, this);
    connect(m_add, &QAbstractButton::clicked, this, &QDialog::accept);
    buttons->addWidget(m_selectAll);
    buttons->addWidget(m_selectNone);
    buttons->addStretch();
    buttons->addWidget(close);
    buttons->addWidget(m_add);
    layout->addLayout(buttons);
    m_add->setEnabled(false);
    m_selectAll->setEnabled(false);
    m_selectNone->setEnabled(false);

    m_status->setText(QStringLiteral("جارٍ البحث في الفيديو…"));
    m_timer->setInterval(150);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_bar->setMaximum(m_progress->total);
        m_bar->setValue(m_progress->done);
        m_status->setText(QStringLiteral("جارٍ البحث في الفيديو… %1% · وُجد الشخص في %2 لقطة")
                              .arg(qRound(100.0 * m_progress->done / std::max(1, int(m_progress->total))))
                              .arg(int(m_progress->found)));
    });
    m_timer->start();
    connect(&m_watcher, &QFutureWatcherBase::finished, this, &HarvestDialog::onFinished);
    const std::shared_ptr<HarvestProgress> progress = m_progress;
    m_watcher.setFuture(QtConcurrent::run([engine, videoPath, target, ranges, threshold, progress] {
        return scanVideo(engine, videoPath, target, ranges, threshold, progress);
    }));
}

HarvestDialog::~HarvestDialog()
{
    m_progress->cancel = true;
    m_watcher.waitForFinished();
}

void HarvestDialog::reject()
{
    m_progress->cancel = true;
    QDialog::reject();
}

void HarvestDialog::onFinished()
{
    m_timer->stop();
    m_bar->setMaximum(1);
    m_bar->setValue(1);
    if (m_progress->cancel)
        return;
    m_shots = m_watcher.result();
    for (int i = 0; i < m_shots.size(); ++i) {
        const HarvestShot &shot = m_shots.at(i);
        auto *item = new QListWidgetItem(m_grid);
        item->setText(QStringLiteral("%1\nتشابه %2% · جودة %3%")
                          .arg(ImageIO::formatTime(shot.positionMs))
                          .arg(qRound(shot.similarity * 100))
                          .arg(qRound(shot.face.quality * 100)));
        item->setFlags(Qt::ItemIsEnabled);
        item->setData(Qt::UserRole, i);
        setChecked(item, i < PreselectCount);
    }
    m_selectAll->setEnabled(!m_shots.isEmpty());
    m_selectNone->setEnabled(!m_shots.isEmpty());
    updateSelection();
    if (m_shots.isEmpty())
        m_status->setText(QStringLiteral("لم يُعثر على لقطات واضحة للشخص في هذا الفيديو"));
}

void HarvestDialog::updateSelection()
{
    int checked = 0;
    for (int i = 0; i < m_grid->count(); ++i)
        checked += m_grid->item(i)->data(CheckedRole).toBool() ? 1 : 0;
    m_add->setEnabled(checked > 0);
    m_add->setText(checked > 0 ? QStringLiteral("إضافة %1 صور").arg(checked) : QStringLiteral("إضافة المحدد"));
    if (!m_shots.isEmpty())
        m_status->setText(QStringLiteral("اقتُرحت %1 لقطة متنوعة وحُددت أوضحها · انقر على الصورة لتحديدها أو إلغائها").arg(m_shots.size()));
}

void HarvestDialog::setChecked(QListWidgetItem *item, bool checked)
{
    item->setData(CheckedRole, checked);
    const HarvestShot &shot = m_shots.at(item->data(Qt::UserRole).toInt());
    item->setIcon(QIcon(shotIcon(ImageIO::faceCrop(shot.face), 240, checked)));
}

QList<FaceItem> HarvestDialog::selectedFaces() const
{
    QList<FaceItem> faces;
    for (int i = 0; i < m_grid->count(); ++i) {
        const QListWidgetItem *item = m_grid->item(i);
        if (item->data(CheckedRole).toBool())
            faces << m_shots.at(item->data(Qt::UserRole).toInt()).face;
    }
    return faces;
}

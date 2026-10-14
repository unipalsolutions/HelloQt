#include "shotviewer.h"

#include "enhancedialog.h"
#include "videofilters.h"
#include "videoexport.h"

#include "carddelegates.h"
#include "faceengine.h"
#include "imageio.h"
#include "imageview.h"
#include "theme.h"
#include "timelineview.h"
#include "widgets.h"

#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>

namespace {

double overlap(const QRectF &a, const QRectF &b)
{
    const QRectF i = a.intersected(b);
    const double inter = i.width() * i.height();
    const double uni = a.width() * a.height() + b.width() * b.height() - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

} // namespace

ShotFrame readShotFrame(cv::VideoCapture &capture, qint64 positionMs, double fps, const FaceEngine *engine,
                        const GalleryPtr &gallery, float threshold, bool enhance)
{
    ShotFrame result;
    result.positionMs = positionMs;
    // البحث بالإطار أدق من البحث بالزمن في أغلب الصيغ
    capture.set(cv::CAP_PROP_POS_FRAMES, std::floor(positionMs * fps / 1000.0 + 0.5));
    cv::Mat frame;
    if (!capture.read(frame) || frame.empty()) {
        capture.set(cv::CAP_PROP_POS_MSEC, double(positionMs));
        if (!capture.read(frame) || frame.empty()) {
            result.error = QStringLiteral("تعذر قراءة الإطار عند %1").arg(ImageIO::formatTime(positionMs));
            return result;
        }
    }
    frame = VideoFilters::apply(frame); // نفس معالجات التحليل، فتطابق مواضع الوجوه
    cv::Mat image = frame;
    std::vector<AnalyzedFace> faces;
    if (enhance) {
        image = ImageIO::enhance(frame);
        faces = engine->analyzeEnhanced(frame, image, true);
    } else {
        faces = engine->analyze(frame, true);
    }
    result.scale = frame.cols > 0 ? double(image.cols) / frame.cols : 1.0;
    result.image = ImageIO::toQImage(image);
    for (size_t i = 0; i < faces.size(); ++i) {
        FaceItem item = ImageIO::makeFaceItem(image, faces[i], int(i) + 1);
        if (gallery)
            item.match = FaceDatabase::identify(*gallery, item.embedding, threshold);
        result.faces.append(item);
    }
    return result;
}

ShotViewer::ShotViewer(const FaceEngine *engine, const FaceDatabase *database, const QString &videoPath, const FaceItem &face,
                       float threshold, bool enhance, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_database(database)
    , m_videoPath(videoPath)
    , m_face(face)
    , m_threshold(threshold)
    , m_capture(std::make_shared<cv::VideoCapture>())
{
    setWindowTitle(QStringLiteral("لقطات الوجه في الفيديو — %1").arg(matchLabel(face.match)));
    setAttribute(Qt::WA_DeleteOnClose);

    if (ImageIO::openVideo(*m_capture, videoPath)) {
        const double fps = m_capture->get(cv::CAP_PROP_FPS);
        if (fps > 1.0 && fps < 500.0)
            m_fps = fps;
        const double frames = m_capture->get(cv::CAP_PROP_FRAME_COUNT);
        if (frames > 0.0)
            m_durationMs = qint64(frames / m_fps * 1000.0);
    }
    if (m_durationMs <= 0)
        m_durationMs = std::max<qint64>(face.lastSeenMs + 1000, 1000);

    // ── العرض والتنقل الزمني ──
    m_view = new ImageView(this);
    m_view->setMinimumSize(320, 200);

    auto *previousAppearance = new FancyButton(Icons::Name::Previous, QStringLiteral("الظهور السابق"), FancyButton::Kind::Secondary, this);
    auto *previousFrame = new FancyButton(Icons::Name::Previous, QStringLiteral("إطار"), FancyButton::Kind::Secondary, this);
    auto *nextFrame = new FancyButton(Icons::Name::Next, QStringLiteral("إطار"), FancyButton::Kind::Secondary, this);
    auto *nextAppearance = new FancyButton(Icons::Name::Next, QStringLiteral("الظهور التالي"), FancyButton::Kind::Secondary, this);
    for (FancyButton *button : {previousAppearance, previousFrame, nextFrame, nextAppearance})
        button->setCompact(true);
    previousAppearance->setToolTip(QStringLiteral("الظهور السابق لهذا الوجه (Ctrl+←)"));
    previousFrame->setToolTip(QStringLiteral("الإطار السابق (←)"));
    nextFrame->setToolTip(QStringLiteral("الإطار التالي (→)"));
    nextAppearance->setToolTip(QStringLiteral("الظهور التالي لهذا الوجه (Ctrl+→)"));

    m_timeline = new TimelineView(this);
    m_timeline->setFps(m_fps);
    m_timeline->setDuration(m_durationMs);
    m_timeline->setLabelWidth(110);
    m_timeline->setMinimumHeight(m_timeline->minimumSizeHint().height() + 2);
    {
        TimelineLane lane;
        lane.id = face.id;
        lane.name = matchLabel(face.match);
        lane.color = matchColor(face.match.status);
        lane.avatar = circularPixmap(ImageIO::faceCrop(face), 40);
        lane.segments = face.appearances;
        if (lane.segments.isEmpty() && face.firstSeenMs >= 0)
            lane.segments.append({face.firstSeenMs, std::max(face.firstSeenMs, face.lastSeenMs)});
        m_timeline->setLanes({lane});
        m_timeline->setSelectedLane(face.id);
    }
    m_timeLabel = new QLabel(this);
    m_timeLabel->setObjectName(QStringLiteral("muted"));
    m_timeLabel->setMinimumWidth(130);
    m_timeLabel->setAlignment(Qt::AlignCenter);

    // شريط الزمن من اليسار إلى اليمين مثل مشغلات الفيديو، والأسهم تتبعه
    auto *timelineRow = new QWidget(this);
    timelineRow->setLayoutDirection(Qt::LeftToRight);
    auto *timelineLayout = new QHBoxLayout(timelineRow);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    timelineLayout->setSpacing(6);
    timelineLayout->addStretch();
    timelineLayout->addWidget(previousAppearance);
    timelineLayout->addWidget(previousFrame);
    timelineLayout->addWidget(m_timeLabel);
    timelineLayout->addWidget(nextFrame);
    timelineLayout->addWidget(nextAppearance);
    timelineLayout->addStretch();

    auto *viewCard = new QFrame(this);
    viewCard->setObjectName(QStringLiteral("card"));
    auto *viewLayout = new QVBoxLayout(viewCard);
    viewLayout->setContentsMargins(12, 12, 12, 12);
    viewLayout->setSpacing(10);
    viewLayout->addWidget(m_view, 1);
    viewLayout->addWidget(timelineRow);
    viewLayout->addWidget(m_timeline);

    // ── اللوحة الجانبية ──
    auto *avatar = new QLabel(this);
    avatar->setFixedSize(64, 64);
    avatar->setPixmap(circularPixmap(ImageIO::faceCrop(face), 64));
    auto *name = new QLabel(matchLabel(face.match), this);
    name->setObjectName(QStringLiteral("sectionTitle"));
    name->setWordWrap(true);
    auto *summary = new QLabel(QStringLiteral("ظهر في %1 إطار · %2 مرة").arg(face.frames).arg(std::max<qsizetype>(1, face.appearances.size())), this);
    summary->setObjectName(QStringLiteral("muted"));
    summary->setWordWrap(true);
    auto *nameBox = new QVBoxLayout;
    nameBox->setSpacing(2);
    nameBox->addWidget(name);
    nameBox->addWidget(summary);
    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    header->addWidget(avatar);
    header->addLayout(nameBox, 1);

    auto *bestShot = new FancyButton(Icons::Name::Sparkle, QStringLiteral("أوضح لقطة"), FancyButton::Kind::Primary, this);
    bestShot->setToolTip(QStringLiteral("الانتقال إلى الإطار الذي ظهر فيه الوجه بأفضل جودة"));
    bestShot->setEnabled(face.bestMs >= 0);
    m_enhanceSwitch = new ToggleSwitch(QStringLiteral("تحسين الإطار وتوضيحه"), this);
    m_enhanceSwitch->setChecked(enhance);
    m_enhanceSwitch->setToolTip(QStringLiteral("تكبير الإطار وإزالة التشويش وموازنة الإضاءة قبل كشف الوجوه"));

    m_details = new QLabel(this);
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignRight);

    auto *appearancesTitle = new QLabel(QStringLiteral("مرات الظهور"), this);
    appearancesTitle->setObjectName(QStringLiteral("muted"));
    m_appearances = new QListWidget(this);
    m_appearances->setMinimumHeight(90);
    QList<QPair<qint64, qint64>> appearances = face.appearances;
    if (appearances.isEmpty() && face.firstSeenMs >= 0)
        appearances.append({face.firstSeenMs, std::max(face.firstSeenMs, face.lastSeenMs)});
    for (const auto &range : appearances) {
        const QString text = range.second - range.first < 1000
            ? ImageIO::formatTime(range.first)
            : QStringLiteral("%1 ← %2").arg(ImageIO::formatTime(range.first), ImageIO::formatTime(range.second));
        auto *item = new QListWidgetItem(QStringLiteral("%1 · %2").arg(m_appearances->count() + 1).arg(text), m_appearances);
        item->setData(Qt::UserRole, range.first);
        item->setData(Qt::UserRole + 1, range.second);
        if (face.bestMs >= range.first && face.bestMs <= range.second)
            item->setText(item->text() + QStringLiteral(" ★"));
    }

    auto *saveFrame = new FancyButton(Icons::Name::Image, QStringLiteral("حفظ الإطار"), FancyButton::Kind::Secondary, this);
    auto *advanced = new FancyButton(Icons::Name::Sparkle, QStringLiteral("تحسين متقدم…"), FancyButton::Kind::Secondary, this);
    advanced->setToolTip(QStringLiteral("إزالة التشويش والضبابية، الحدة، تكبير الدقة، الإضاءة، التدوير وتصحيح المنظور، مع معاينة قبل وبعد"));
    connect(advanced, &QAbstractButton::clicked, this, [this, advanced] {
        // القفز إلى موضع في فيديو طويل قد يستغرق ثانية أو أكثر، فيُقرأ الإطار في الخلفية
        advanced->setEnabled(false);
        const qint64 positionMs = m_frame.positionMs;
        auto *watcher = new QFutureWatcher<cv::Mat>(this);
        connect(watcher, &QFutureWatcher<cv::Mat>::finished, this, [this, advanced, watcher, positionMs] {
            const cv::Mat frame = watcher->result();
            watcher->deleteLater();
            advanced->setEnabled(true);
            if (frame.empty())
                return;
            EnhanceDialog dialog(frame, m_engine, QStringLiteral("تحسين الإطار عند %1").arg(ImageIO::formatTime(positionMs)), {}, this);
            dialog.exec();
        });
        watcher->setFuture(QtConcurrent::run([path = m_videoPath, positionMs] { return VideoExport::frameAt(path, positionMs); }));
    });
    m_saveFaceButton = new FancyButton(Icons::Name::Save, QStringLiteral("حفظ الوجه المحدد"), FancyButton::Kind::Secondary, this);
    m_addButton = new FancyButton(Icons::Name::UserPlus, QStringLiteral("إضافة إلى القاعدة"), FancyButton::Kind::Secondary, this);
    m_addButton->setToolTip(QStringLiteral("إضافة الوجه المحدد في هذا الإطار إلى قاعدة البيانات"));
    auto *analyzeFrom = new FancyButton(Icons::Name::Film, QStringLiteral("تحليل الفيديو من هنا"), FancyButton::Kind::Secondary, this);
    analyzeFrom->setToolTip(QStringLiteral("إعادة تحليل الفيديو بدءاً من هذا الموضع"));

    auto *side = new QWidget(this);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(14, 14, 14, 14);
    sideLayout->setSpacing(10);
    sideLayout->addLayout(header);
    sideLayout->addWidget(bestShot);
    sideLayout->addWidget(m_enhanceSwitch);
    sideLayout->addWidget(m_details);
    sideLayout->addWidget(appearancesTitle);
    sideLayout->addWidget(m_appearances, 1);
    sideLayout->addWidget(advanced);
    sideLayout->addWidget(saveFrame);
    sideLayout->addWidget(m_saveFaceButton);
    sideLayout->addWidget(m_addButton);
    sideLayout->addWidget(analyzeFrom);

    // اللوحة تُمرَّر عمودياً على الشاشات القصيرة بدل أن تختفي أجزاء منها
    auto *sideScroll = new QScrollArea(this);
    sideScroll->setObjectName(QStringLiteral("card"));
    sideScroll->setWidget(side);
    sideScroll->setWidgetResizable(true);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setMinimumWidth(240);
    sideScroll->setMaximumWidth(320);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);
    layout->addWidget(viewCard, 1);
    layout->addWidget(sideScroll);

    // حجم يناسب الشاشة الحالية
    const QScreen *screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
    resize(std::min(1200, int(available.width() * 0.9)), std::min(780, int(available.height() * 0.88)));
    setMinimumSize(std::min(620, available.width()), std::min(420, available.height()));

    m_scrubTimer = new QTimer(this);
    m_scrubTimer->setSingleShot(true);
    m_scrubTimer->setInterval(90);
    connect(m_scrubTimer, &QTimer::timeout, this, [this] { seek(m_timeline->position()); });
    connect(m_timeline, &TimelineView::seekRequested, this, [this] { m_scrubTimer->start(); });
    connect(m_timeline, &TimelineView::laneActivated, this, [this](int, qint64 ms) {
        if (ms >= 0)
            seek(ms, true);
    });

    connect(&m_watcher, &QFutureWatcher<ShotFrame>::finished, this, &ShotViewer::onLoaded);
    connect(previousFrame, &QAbstractButton::clicked, this, [this] { stepFrames(-1); });
    connect(nextFrame, &QAbstractButton::clicked, this, [this] { stepFrames(1); });
    connect(previousAppearance, &QAbstractButton::clicked, this, [this] { jumpAppearance(-1); });
    connect(nextAppearance, &QAbstractButton::clicked, this, [this] { jumpAppearance(1); });
    connect(bestShot, &QAbstractButton::clicked, this, [this] { seek(m_face.bestMs, true); });
    connect(m_enhanceSwitch, &QAbstractButton::toggled, this, [this] {
        if (m_frame.positionMs >= 0)
            seek(m_frame.positionMs, true);
    });
    connect(m_appearances, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const qint64 start = item->data(Qt::UserRole).toLongLong();
        const qint64 end = item->data(Qt::UserRole + 1).toLongLong();
        // أوضح لقطة إن كانت ضمن هذه المرة، وإلا منتصفها
        seek(m_face.bestMs >= start && m_face.bestMs <= end ? m_face.bestMs : (start + end) / 2, true);
    });
    connect(m_view, &ImageView::faceClicked, this, &ShotViewer::selectFace);
    connect(saveFrame, &QAbstractButton::clicked, this, &ShotViewer::saveFrame);
    connect(m_saveFaceButton, &QAbstractButton::clicked, this, &ShotViewer::saveFace);
    connect(m_addButton, &QAbstractButton::clicked, this, [this] {
        if (const FaceItem *selected = selectedFace())
            emit addFaceRequested(*selected);
    });
    connect(analyzeFrom, &QAbstractButton::clicked, this, [this] {
        emit analyzeFromRequested(std::max<qint64>(0, m_frame.positionMs >= 0 ? m_frame.positionMs : m_requestedMs));
        close();
    });

    auto shortcut = [this](const QKeySequence &key, auto slot) {
        auto *s = new QShortcut(key, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence(Qt::Key_Right), [this] { stepFrames(1); });
    shortcut(QKeySequence(Qt::Key_Left), [this] { stepFrames(-1); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { jumpAppearance(1); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), [this] { jumpAppearance(-1); });
    shortcut(QKeySequence(Qt::Key_B), [this] { seek(m_face.bestMs, true); });

    updateDetails();
    if (!m_capture->isOpened()) {
        m_details->setText(QStringLiteral("<span style='color:%1'>تعذر فتح الفيديو</span>").arg(Theme::Warning.name()));
        return;
    }
    seek(face.bestMs >= 0 ? face.bestMs : std::max<qint64>(0, face.firstSeenMs), true);
}

void ShotViewer::setThumbnails(const QMap<qint64, QImage> &thumbnails)
{
    m_timeline->setThumbnails(thumbnails);
}

ShotViewer::~ShotViewer()
{
    m_watcher.waitForFinished();
}

void ShotViewer::seek(qint64 positionMs, bool highlightTarget)
{
    if (positionMs < 0 || !m_capture->isOpened())
        return;
    m_requestedMs = std::clamp<qint64>(positionMs, 0, std::max<qint64>(0, m_durationMs - 1));
    m_highlightRequested = highlightTarget;
    if (!m_watcher.isRunning())
        startLoading();
}

void ShotViewer::startLoading()
{
    const qint64 positionMs = m_requestedMs;
    m_highlightTarget = m_highlightRequested;
    m_requestedMs = -1;
    m_view->setBusy(true, QStringLiteral("جارٍ قراءة الإطار وكشف الوجوه"));
    m_timeLabel->setText(ImageIO::formatTime(positionMs));

    const FaceEngine *engine = m_engine;
    const GalleryPtr gallery = m_database ? m_database->gallery() : GalleryPtr();
    const float threshold = m_threshold;
    const bool enhance = m_enhanceSwitch->isChecked();
    const double fps = m_fps;
    std::shared_ptr<cv::VideoCapture> capture = m_capture; // لا يقرؤه إلا خيط واحد في كل مرة
    m_watcher.setFuture(QtConcurrent::run([=] {
        return readShotFrame(*capture, positionMs, fps, engine, gallery, threshold, enhance);
    }));
}

void ShotViewer::onLoaded()
{
    ShotFrame loaded = m_watcher.result();
    if (m_requestedMs >= 0) {
        // وصل طلب أحدث أثناء التحميل: يُعرض هذا الإطار مؤقتاً ثم يُحمَّل المطلوب
        if (loaded.error.isEmpty())
            m_view->setImage(loaded.image);
        startLoading();
        return;
    }
    m_view->setBusy(false);
    if (!loaded.error.isEmpty()) {
        m_details->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::Warning.name(), loaded.error.toHtmlEscaped()));
        return;
    }

    const bool sameFrameSize = !m_frame.image.isNull() && m_frame.image.size() == loaded.image.size();
    m_frame = loaded;
    m_view->setImage(m_frame.image, !sameFrameSize);
    m_view->setFaces(m_frame.faces);

    // تحديد الوجه المطلوب: الأشبه ببصمته، مع تفضيل موضعه المسجل في أوضح لقطة
    int best = -1;
    double bestScore = 0.25;
    const QRectF target(m_face.sourceBox.x() * m_frame.scale, m_face.sourceBox.y() * m_frame.scale,
                        m_face.sourceBox.width() * m_frame.scale, m_face.sourceBox.height() * m_frame.scale);
    const bool atBestShot = m_face.sourceBox.isValid() && std::llabs(m_frame.positionMs - m_face.bestMs) < 200;
    for (const FaceItem &item : std::as_const(m_frame.faces)) {
        double score = m_face.embedding.empty() ? 0.0 : FaceEngine::cosine(item.embedding, m_face.embedding);
        if (atBestShot && overlap(item.box, target) > 0.3)
            score += 1.0;
        if (score > bestScore) {
            bestScore = score;
            best = item.id;
        }
    }
    if (best < 0 && m_frame.faces.size() == 1 && m_highlightTarget)
        best = m_frame.faces.first().id;
    m_selectedId = -1;
    selectFace(best);

    m_timeline->setPosition(m_frame.positionMs);
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(ImageIO::formatTime(m_frame.positionMs), ImageIO::formatTime(m_durationMs)));

    // إبراز مرة الظهور الحالية في القائمة
    for (int i = 0; i < m_appearances->count(); ++i) {
        QListWidgetItem *item = m_appearances->item(i);
        const qint64 start = item->data(Qt::UserRole).toLongLong();
        const qint64 end = item->data(Qt::UserRole + 1).toLongLong();
        if (m_frame.positionMs >= start - 500 && m_frame.positionMs <= end + 500) {
            const QSignalBlocker blocker(m_appearances);
            m_appearances->setCurrentRow(i);
            break;
        }
    }
}

void ShotViewer::stepFrames(int frames)
{
    const qint64 base = m_requestedMs >= 0 ? m_requestedMs : m_frame.positionMs;
    if (base < 0)
        return;
    seek(std::max<qint64>(0, base + qint64(std::llround(frames * 1000.0 / m_fps))));
}

void ShotViewer::jumpAppearance(int direction)
{
    const qint64 current = m_requestedMs >= 0 ? m_requestedMs : m_frame.positionMs;
    qint64 target = -1;
    for (int i = 0; i < m_appearances->count(); ++i) {
        const qint64 start = m_appearances->item(i)->data(Qt::UserRole).toLongLong();
        if (direction > 0 && start > current + 300 && (target < 0 || start < target))
            target = start;
        if (direction < 0 && start < current - 300 && (target < 0 || start > target))
            target = start;
    }
    if (target >= 0)
        seek(target, true);
}

void ShotViewer::selectFace(int id)
{
    m_selectedId = id;
    m_view->setSelectedFace(id);
    updateDetails();
}

const FaceItem *ShotViewer::selectedFace() const
{
    for (const FaceItem &item : m_frame.faces) {
        if (item.id == m_selectedId)
            return &item;
    }
    return nullptr;
}

void ShotViewer::updateDetails()
{
    const FaceItem *selected = selectedFace();
    m_addButton->setEnabled(selected != nullptr);
    m_saveFaceButton->setEnabled(selected != nullptr);

    auto row = [](const QString &key, const QString &value) {
        return QStringLiteral("<span style='color:%1'>%2:</span> <span style='color:%3'>%4</span><br>")
            .arg(Theme::Muted.name(), key, Theme::Text.name(), value.toHtmlEscaped());
    };
    QString html;
    if (m_frame.positionMs >= 0) {
        html += row(QStringLiteral("الموضع"), ImageIO::formatTime(m_frame.positionMs));
        html += row(QStringLiteral("وجوه في الإطار"), QString::number(m_frame.faces.size()));
    }
    if (m_face.bestMs >= 0)
        html += row(QStringLiteral("أوضح لقطة"), ImageIO::formatTime(m_face.bestMs));
    if (selected) {
        html += QStringLiteral("<br><span style='font-weight:600; color:%1'>%2</span><br>")
                    .arg(matchColor(selected->match.status).name(), matchLabel(selected->match).toHtmlEscaped());
        if (selected->match.status != MatchStatus::Unknown)
            html += row(QStringLiteral("التشابه مع القاعدة"), QStringLiteral("%1%").arg(qRound(selected->match.score * 100)));
        if (!m_face.embedding.empty())
            html += row(QStringLiteral("الشبه بالوجه المتتبَّع"),
                        QStringLiteral("%1%").arg(qRound(std::max(0.0f, FaceEngine::cosine(selected->embedding, m_face.embedding)) * 100)));
        html += row(QStringLiteral("جودة الوجه"), QStringLiteral("%1%").arg(qRound(selected->quality * 100)));
    } else if (!m_frame.faces.isEmpty()) {
        html += QStringLiteral("<br><span style='color:%1'>اضغط على وجه في الإطار لتحديده</span>").arg(Theme::Muted.name());
    } else if (m_frame.positionMs >= 0) {
        html += QStringLiteral("<br><span style='color:%1'>لا توجد وجوه واضحة في هذا الإطار؛ جرّب الإطار التالي أو التحسين</span>")
                    .arg(Theme::Warning.name());
    }
    m_details->setText(html);
}

void ShotViewer::saveFrame()
{
    if (m_frame.image.isNull())
        return;
    const QString suggested = QStringLiteral("إطار_%1.png").arg(ImageIO::formatTime(m_frame.positionMs).replace(QLatin1Char(':'), QLatin1Char('-')));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ الإطار"), suggested, QStringLiteral("صور (*.png *.jpg)"));
    if (path.isEmpty())
        return;
    if (m_frame.image.save(path))
        Toast::show(this, QStringLiteral("تم حفظ الإطار"));
    else
        Toast::show(this, QStringLiteral("تعذر حفظ الإطار"), Toast::Kind::Warning);
}

void ShotViewer::saveFace()
{
    const FaceItem *selected = selectedFace();
    if (!selected)
        return;
    const QImage crop = ImageIO::faceCrop(*selected);
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ الوجه"), QStringLiteral("وجه.png"), QStringLiteral("صور (*.png *.jpg)"));
    if (path.isEmpty())
        return;
    if (crop.save(path))
        Toast::show(this, QStringLiteral("تم حفظ الوجه"));
    else
        Toast::show(this, QStringLiteral("تعذر حفظ الوجه"), Toast::Kind::Warning);
}

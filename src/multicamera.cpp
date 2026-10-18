#include "multicamera.h"

#include "alertsound.h"
#include "applog.h"
#include "camerawatch.h"
#include "carddelegates.h"
#include "facedatabase.h"
#include "faceengine.h"
#include "icons.h"
#include "imageio.h"
#include "recorder.h"
#include "theme.h"
#include "videofilters.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr int MaxCameras = 16;
constexpr int TickMs = 40;
constexpr qint64 FacesShownMs = 1500;
constexpr qint64 RetryMs = 4000;
constexpr qint64 AlertFrameMs = 10000;

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

int columnsFor(int count)
{
    if (count <= 1)
        return 1;
    if (count <= 4)
        return 2;
    if (count <= 9)
        return 3;
    return 4;
}

VideoFilters::Settings filtersOf(const Cameras::Camera &camera)
{
    // كما في عرض الكاميرا الواحدة: معالجات الكاميرا المحفوظة معها، وإلا لا شيء
    return camera.hasFilters ? camera.filters : VideoFilters::Settings();
}

QString tileStyle(bool alert)
{
    return QStringLiteral("QFrame#cameraTile { background: %1; border: 2px solid %2; border-radius: 10px; }")
        .arg(Theme::Surface.name(), alert ? Theme::Danger.name() : Theme::Border.name());
}

} // namespace

MultiCameraWindow::MultiCameraWindow(FaceEngine *engine, FaceDatabase *database, std::function<float()> threshold,
                                     QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_engine(engine)
    , m_database(database)
    , m_threshold(std::move(threshold))
{
    setWindowTitle(QStringLiteral("ملامح — عدة كاميرات"));
    setLayoutDirection(Qt::RightToLeft);
    // الحجم الافتراضي لا يتجاوز الشاشة المتاحة كي لا تُقص الكاميرات السفلية على شاشات اللابتوب
    const QScreen *display = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QRect available = display ? display->availableGeometry() : QRect(0, 0, 1280, 800);
    resize(qMin(1280, available.width() * 9 / 10), qMin(800, available.height() * 9 / 10));
    setStyleSheet(QStringLiteral("MultiCameraWindow { background: %1; }").arg(Theme::Background.name()));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *bar = new QHBoxLayout;
    auto *choose = new QPushButton(Icons::icon(Icons::Name::Camera, Theme::Accent2, 18), QStringLiteral("الكاميرات…"));
    choose->setToolTip(QStringLiteral("اختيار الكاميرات المعروضة"));
    connect(choose, &QPushButton::clicked, this, [this] {
        const QStringList sources = chooseCameras();
        if (!sources.isEmpty())
            start(sources);
    });
    m_recordButton = new QPushButton;
    m_recordButton->setToolTip(QStringLiteral("تسجيل كل الكاميرات، كل منها في ملفاتها داخل مجلد التسجيلات"));
    connect(m_recordButton, &QPushButton::clicked, this, &MultiCameraWindow::toggleRecording);
    auto *stopButton = new QPushButton(Icons::icon(Icons::Name::Stop, Theme::Accent2, 18), QStringLiteral("إيقاف"));
    connect(stopButton, &QPushButton::clicked, this, &MultiCameraWindow::close);
    m_status = new QLabel;
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Muted.name()));
    bar->addWidget(choose);
    bar->addWidget(m_recordButton);
    bar->addWidget(stopButton);
    bar->addSpacing(12);
    bar->addWidget(m_status, 1);
    layout->addLayout(bar);

    m_alertBar = new QFrame;
    m_alertBar->setStyleSheet(QStringLiteral("QFrame { background: %1; border: 1px solid %2; border-radius: 10px; }"
                                             "QLabel { border: none; background: transparent; }")
                                  .arg(Theme::mix(Theme::Surface, Theme::Danger, 0.18).name(), Theme::Danger.name()));
    auto *alertLayout = new QHBoxLayout(m_alertBar);
    alertLayout->setContentsMargins(10, 6, 10, 6);
    m_alertAvatar = new QLabel;
    m_alertAvatar->setFixedSize(44, 44);
    m_alert = new QLabel;
    m_alert->setTextFormat(Qt::RichText);
    auto *dismiss = new QPushButton(QStringLiteral("إخفاء"));
    connect(dismiss, &QPushButton::clicked, m_alertBar, &QWidget::hide);
    alertLayout->addWidget(m_alertAvatar);
    alertLayout->addWidget(m_alert, 1);
    alertLayout->addWidget(dismiss);
    m_alertBar->hide();
    layout->addWidget(m_alertBar);

    m_gridHost = new QWidget;
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(8);
    layout->addWidget(m_gridHost, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(TickMs);
    connect(m_timer, &QTimer::timeout, this, &MultiCameraWindow::tick);
    updateRecordButton();
}

MultiCameraWindow::~MultiCameraWindow()
{
    stop();
}

QStringList MultiCameraWindow::savedSources()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("multiCameras")).toStringList();
}

void MultiCameraWindow::setSavedSources(const QStringList &sources)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("multiCameras"), sources);
}

QStringList MultiCameraWindow::chooseCameras()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("اختيار الكاميرات"));
    dialog.setLayoutDirection(Qt::RightToLeft);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(QStringLiteral("اختر الكاميرات التي تُعرض وتُراقب معاً (حتى %1). تُحلَّل بالتناوب، فكلما زاد عددها "
                                           "قلّ عدد الإطارات المحلَّلة من كل منها في الثانية، أما العرض والتسجيل فبسرعتها الكاملة.")
                                .arg(MaxCameras));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *list = new QListWidget;
    layout->addWidget(list, 1);

    const QStringList last = savedSources();
    const QSet<QString> previous(last.begin(), last.end());
    const QList<Cameras::Camera> saved = Cameras::load();
    const QList<Cameras::LocalDevice> devices = Cameras::localDevices();
    const auto add = [&](const QString &source, const QString &text) {
        auto *item = new QListWidgetItem(text, list);
        item->setData(Qt::UserRole, source);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(previous.contains(source) ? Qt::Checked : Qt::Unchecked);
    };
    for (const Cameras::Camera &camera : saved)
        add(camera.source(), QStringLiteral("%1  —  %2").arg(camera.name, camera.description()));
    for (const Cameras::LocalDevice &device : devices) {
        const bool known = std::any_of(saved.begin(), saved.end(), [&](const Cameras::Camera &c) {
            return c.kind == Cameras::Kind::Local && c.index == device.index;
        });
        if (!known)
            add(QStringLiteral("camera:%1").arg(device.index),
                QStringLiteral("%1  —  موصولة بالجهاز").arg(Cameras::localName(device.index, devices)));
    }
    if (list->count() == 0)
        add(QStringLiteral("camera:0"), QStringLiteral("كاميرا الجهاز"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("عرض"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("إلغاء"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(520, 420);

    while (dialog.exec() == QDialog::Accepted) {
        QStringList sources;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->checkState() == Qt::Checked)
                sources << list->item(i)->data(Qt::UserRole).toString();
        }
        if (sources.isEmpty()) {
            QMessageBox::information(&dialog, dialog.windowTitle(), QStringLiteral("اختر كاميرا واحدة على الأقل."));
            continue;
        }
        if (sources.size() > MaxCameras) {
            QMessageBox::information(&dialog, dialog.windowTitle(),
                                     QStringLiteral("يمكن عرض %1 كاميرا معاً على الأكثر.").arg(MaxCameras));
            continue;
        }
        setSavedSources(sources);
        return sources;
    }
    return {};
}

void MultiCameraWindow::setWatched(const QSet<QString> &ids)
{
    m_watched = ids;
    for (const auto &feed : m_feeds)
        feed->watch->setWatched(ids);
}

void MultiCameraWindow::setGallery(std::shared_ptr<Gallery> gallery)
{
    std::lock_guard<std::mutex> lock(m_galleryMutex);
    m_gallery = std::move(gallery);
}

void MultiCameraWindow::start(const QStringList &sources)
{
    stop();
    for (const QString &source : sources) {
        auto feed = std::make_unique<Feed>();
        if (!Cameras::resolve(source.mid(7), &feed->camera))
            continue;
        feed->source = source;
        feed->recorder = new CameraRecorder(this);
        feed->watch = new CameraWatch(m_database, this);
        feed->watch->reset(source, feed->camera.name, feed->recorder);
        feed->rules = std::make_shared<const CameraRules::Rules>(feed->watch->rules());
        feed->watch->setWatched(m_watched);
        Feed *raw = feed.get();
        connect(feed->recorder, &CameraRecorder::failed, this, [this, raw](const QString &error) {
            notice(QStringLiteral("توقف تسجيل «%1»: %2").arg(raw->camera.name, error));
            emit systemAlert(QStringLiteral("توقف التسجيل"), QStringLiteral("«%1»: %2").arg(raw->camera.name, error));
            updateRecordButton();
        });
        connect(feed->watch, &CameraWatch::watchAlert, this,
                [this, raw](const QString &personId, const QString &name, float score, const QImage &crop) {
                    showAlert(*raw, personId, name, score, crop);
                });
        connect(feed->watch, &CameraWatch::strangerAlert, this, [this, raw](qint64 visitorId, const QString &reason, const QImage &) {
            notice(QStringLiteral("زائر مجهول رقم %1 %2 أمام «%3»").arg(visitorId).arg(reason, raw->camera.name));
            emit strangerAlert(visitorId, reason, raw->camera.name);
        });
        m_feeds.push_back(std::move(feed));
    }
    if (m_feeds.empty()) {
        m_status->setText(QStringLiteral("لم تُعثر على الكاميرات المختارة؛ اخترها من جديد"));
        return;
    }
    ++m_generation;
    m_alertCount = 0;
    m_focused = -1;
    m_recording = CameraRecorder::autoRecord();
    buildTiles();
    for (int i = 0; i < int(m_feeds.size()); ++i)
        openFeed(i);
    m_currentThreshold = m_threshold ? m_threshold() : 0.5f;
    m_stopAnalysis = false;
    m_analysis = std::thread([this] { analysisLoop(); });
    m_timer->start();
    updateRecordButton();
    m_status->setText(QStringLiteral("%1 كاميرات — جارٍ الاتصال…").arg(m_feeds.size()));
}

void MultiCameraWindow::stop()
{
    if (m_feeds.empty())
        return;
    m_timer->stop();
    m_stopAnalysis = true;
    if (m_analysis.joinable())
        m_analysis.join();
    ++m_generation; // نتائج التحليل والفتح المتأخرة تُهمل
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const auto &feed : m_feeds) {
        // القارئ يدفع الإطارات إلى المسجّل من خيطه، فيُغلق قبله
        feed->pending.waitForFinished();
        if (feed->opened)
            feed->opened->reader.reset();
        feed->reader.reset();
    }
    for (const auto &feed : m_feeds) {
        feed->recorder->stop();
        delete feed->watch;
        delete feed->recorder;
        delete feed->tile;
    }
    m_feeds.clear();
    m_recording = false;
    QApplication::restoreOverrideCursor();
    updateRecordButton();
    m_status->setText(QStringLiteral("متوقفة"));
    emit stopped();
}

bool MultiCameraWindow::isRecording() const
{
    return std::any_of(m_feeds.begin(), m_feeds.end(), [](const auto &feed) { return feed->recorder->isRecording(); });
}

void MultiCameraWindow::buildTiles()
{
    const int count = int(m_feeds.size());
    const int columns = columnsFor(count);
    for (int i = 0; i < count; ++i) {
        Feed &feed = *m_feeds[size_t(i)];
        if (!feed.tile) {
            feed.tile = new QFrame;
            feed.tile->setObjectName(QStringLiteral("cameraTile"));
            feed.tile->setStyleSheet(tileStyle(false));
            feed.tile->setProperty("alert", false);
            auto *tileLayout = new QVBoxLayout(feed.tile);
            tileLayout->setContentsMargins(6, 6, 6, 6);
            tileLayout->setSpacing(4);
            feed.title = new QLabel;
            feed.title->setTextFormat(Qt::RichText);
            // النص الطويل (سبب الانقطاع) لا يوسّع عموده على حساب بقية الكاميرات
            feed.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            feed.view = new QLabel;
            feed.view->setAlignment(Qt::AlignCenter);
            feed.view->setMinimumSize(160, 90);
            feed.view->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            feed.view->setStyleSheet(QStringLiteral("background: #000; border-radius: 6px; color: %1;").arg(Theme::Muted.name()));
            feed.view->setProperty("feed", i);
            feed.view->setToolTip(QStringLiteral("نقر مزدوج للتكبير أو العودة إلى كل الكاميرات"));
            feed.view->installEventFilter(this);
            tileLayout->addWidget(feed.title);
            tileLayout->addWidget(feed.view, 1);
        }
        m_grid->removeWidget(feed.tile);
        if (m_focused >= 0) {
            feed.tile->setVisible(i == m_focused);
            if (i == m_focused)
                m_grid->addWidget(feed.tile, 0, 0);
        } else {
            feed.tile->setVisible(true);
            m_grid->addWidget(feed.tile, i / columns, i % columns);
        }
        updateTitle(feed);
    }
    for (int c = 0; c < 4; ++c)
        m_grid->setColumnStretch(c, m_focused >= 0 ? (c == 0 ? 1 : 0) : (c < columns ? 1 : 0));
    const int rows = m_focused >= 0 ? 1 : (count + columns - 1) / std::max(1, columns);
    for (int r = 0; r < 4; ++r)
        m_grid->setRowStretch(r, r < rows ? 1 : 0);
}

bool MultiCameraWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        const QVariant index = watched->property("feed");
        if (index.isValid()) {
            m_focused = m_focused >= 0 ? -1 : index.toInt();
            buildTiles();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MultiCameraWindow::openFeed(int index)
{
    Feed &feed = *m_feeds[size_t(index)];
    feed.opening = true;
    ++feed.attempts;
    updateTitle(feed);
    const Cameras::Camera camera = feed.camera;
    CameraRecorder *recorder = feed.recorder;
    auto opened = std::make_shared<Opened>();
    feed.opened = opened;
    feed.pending = QtConcurrent::run([camera, recorder, opened] {
        auto reader = std::make_shared<Cameras::LiveReader>(camera, recorder);
        QString error;
        if (reader->open(&error))
            opened->reader = std::move(reader);
        else
            opened->error = error;
    });
    auto *watcher = new QFutureWatcher<void>(this);
    const quint64 generation = m_generation;
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, index, generation, opened] {
        watcher->deleteLater();
        if (generation != m_generation || index >= int(m_feeds.size()))
            return;
        Feed &feed = *m_feeds[size_t(index)];
        feed.opening = false;
        feed.opened.reset();
        if (!opened->reader) {
            feed.error = opened->error.section(QLatin1Char('\n'), 0, 0);
            noteLost(feed, nowMs());
            feed.retryAt = nowMs() + RetryMs * std::min(feed.attempts, 8);
            feed.view->setWordWrap(true);
            feed.view->setText(QStringLiteral("تعذر الاتصال — إعادة المحاولة تلقائياً\n\n%1").arg(feed.error));
            updateTitle(feed);
            return;
        }
        feed.reader = opened->reader;
        opened->reader.reset();
        feed.error.clear();
        feed.attempts = 0;
        if (feed.offlineAlerted)
            emit systemAlert(QStringLiteral("عادت الكاميرا"), QStringLiteral("«%1» متصلة من جديد").arg(feed.camera.name));
        AppLog::write(QStringLiteral("عدة كاميرات: اتصلت «%1»").arg(feed.camera.name));
        feed.lostAt = 0;
        feed.offlineAlerted = false;
        feed.fpsSince = nowMs();
        feed.frames = 0;
        if (m_recording)
            startRecording(feed);
        updateTitle(feed);
    });
    watcher->setFuture(feed.pending);
}

void MultiCameraWindow::tick()
{
    const qint64 now = nowMs();
    if (m_threshold)
        m_currentThreshold = m_threshold();
    int connected = 0;
    for (const auto &pointer : m_feeds) {
        Feed &feed = *pointer;
        const bool alert = now < feed.alertUntil;
        if (feed.tile->property("alert").toBool() != alert) {
            feed.tile->setProperty("alert", alert);
            feed.tile->setStyleSheet(tileStyle(alert));
        }
        if (!feed.reader) {
            if (feed.lostAt > 0 && !feed.offlineAlerted && now - feed.lostAt >= OfflineAlertMs) {
                feed.offlineAlerted = true;
                emit systemAlert(QStringLiteral("كاميرا منقطعة"),
                                 QStringLiteral("«%1» منقطعة منذ أكثر من %2 دقائق: %3")
                                     .arg(feed.camera.name)
                                     .arg(OfflineAlertMs / 60000)
                                     .arg(feed.error));
            }
            if (!feed.opening && now >= feed.retryAt)
                openFeed(int(&pointer - m_feeds.data()));
            continue;
        }
        ++connected;
        cv::Mat frame;
        if (feed.reader->read(frame, 0)) {
            const VideoFilters::Settings filters = filtersOf(feed.camera);
            if (!filters.isIdentity())
                frame = VideoFilters::apply(frame, filters);
            {
                std::lock_guard<std::mutex> lock(feed.mutex);
                feed.latest = frame;
                ++feed.sequence;
            }
            ++feed.frames;
            if (now - feed.fpsSince >= 2000) {
                feed.shownFps = feed.frames * 1000.0 / double(now - feed.fpsSince);
                feed.frames = 0;
                feed.fpsSince = now;
                updateTitle(feed);
            }
            if (feed.tile->isVisible())
                render(feed, frame);
        } else if (feed.reader->failed()) {
            feed.reader.reset();
            feed.error = QStringLiteral("انقطع الاتصال");
            noteLost(feed, now);
            feed.retryAt = now + RetryMs;
            feed.faces.clear();
            feed.view->setText(QStringLiteral("انقطع الاتصال — إعادة المحاولة تلقائياً"));
            updateTitle(feed);
        }
    }
    QString status = QStringLiteral("%1 من %2 كاميرات متصلة").arg(connected).arg(m_feeds.size());
    if (isRecording())
        status += QStringLiteral(" · جارٍ التسجيل في: %1").arg(CameraRecorder::savedFolder());
    if (m_alertCount > 0)
        status += QStringLiteral(" · تنبيهات: %1").arg(m_alertCount);
    if (now < m_noticeUntil)
        status = m_notice;
    m_status->setText(status);
}

void MultiCameraWindow::render(Feed &feed, const cv::Mat &frame)
{
    if (frame.empty())
        return;
    const QSize area = feed.view->size();
    const double scale = std::min(area.width() / double(frame.cols), area.height() / double(frame.rows));
    if (scale <= 0.0)
        return;
    cv::Mat shown;
    const cv::Size size(std::max(1, int(std::lround(frame.cols * scale))), std::max(1, int(std::lround(frame.rows * scale))));
    cv::resize(frame, shown, size, 0, 0, scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    if (shown.channels() == 1)
        cv::cvtColor(shown, shown, cv::COLOR_GRAY2BGR);
    else if (shown.channels() == 4)
        cv::cvtColor(shown, shown, cv::COLOR_BGRA2BGR);
    QImage image = ImageIO::toQImage(shown);
    if (nowMs() - feed.facesAt < FacesShownMs && feed.facesSize.width() > 0) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const double sx = double(image.width()) / feed.facesSize.width();
        const double sy = double(image.height()) / feed.facesSize.height();
        QFont font = painter.font();
        font.setPixelSize(std::max(11, image.height() / 28));
        font.setBold(true);
        painter.setFont(font);
        for (const Face &face : std::as_const(feed.faces)) {
            const QRectF box(face.box.x() * sx, face.box.y() * sy, face.box.width() * sx, face.box.height() * sy);
            const bool watched = face.match.status == MatchStatus::Confirmed && m_watched.contains(face.match.personId);
            const QColor color = watched ? Theme::Watched : matchColor(face.match.status);
            painter.setPen(QPen(color, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box, 4, 4);
            const QString label = matchLabel(face.match);
            const QRectF text = painter.boundingRect(QRectF(), Qt::AlignCenter, label).adjusted(-5, -2, 5, 2);
            const QRectF tag(box.left(), std::max(0.0, box.top() - text.height() - 2), text.width(), text.height());
            painter.fillRect(tag, Theme::withAlpha(color, 220));
            painter.setPen(Qt::black);
            painter.drawText(tag, Qt::AlignCenter, label);
        }
    }
    feed.view->setPixmap(QPixmap::fromImage(image));
}

void MultiCameraWindow::updateTitle(Feed &feed)
{
    QString state;
    QString color;
    if (feed.reader) {
        color = Theme::Accent2.name();
        state = feed.shownFps > 0.0 ? QStringLiteral("%1 إطاراً/ث").arg(qRound(feed.shownFps)) : QStringLiteral("متصلة");
    } else if (feed.opening) {
        color = Theme::Warning.name();
        state = feed.attempts > 1 ? QStringLiteral("إعادة الاتصال (%1)…").arg(feed.attempts - 1) : QStringLiteral("جارٍ الاتصال…");
    } else {
        color = Theme::Danger.name();
        state = feed.error.isEmpty() ? QStringLiteral("غير متصلة") : feed.error;
        if (state.size() > 48)
            state = state.left(46) + QStringLiteral("…");
    }
    feed.title->setToolTip(feed.error);
    QString text = QStringLiteral("<span style='color:%1'>●</span> <b style='color:%2'>%3</b>"
                                  " <span style='color:%4'>· %5</span>")
                       .arg(color, Theme::Text.name(), feed.camera.name.toHtmlEscaped(), Theme::Muted.name(), state.toHtmlEscaped());
    if (feed.recorder && feed.recorder->isRecording())
        text += feed.recorder->isOffSchedule()        ? QStringLiteral(" <span style='color:#f0a58a'>· التسجيل خارج ساعاته</span>")
            : feed.recorder->isWaitingForMotion() ? QStringLiteral(" <span style='color:#f0a58a'>· بانتظار حركة</span>")
                                                  : QStringLiteral(" <span style='color:#e8774f'>· ⏺ تسجيل</span>");
    feed.title->setText(text);
}

void MultiCameraWindow::analysisLoop()
{
    const quint64 generation = m_generation;
    size_t next = 0;
    while (!m_stopAnalysis) {
        bool worked = false;
        for (size_t step = 0; step < m_feeds.size() && !m_stopAnalysis; ++step) {
            const size_t index = (next + step) % m_feeds.size();
            Feed &feed = *m_feeds[index];
            cv::Mat frame;
            {
                std::lock_guard<std::mutex> lock(feed.mutex);
                if (feed.latest.empty() || feed.sequence == feed.analyzed)
                    continue;
                frame = feed.latest;
                feed.analyzed = feed.sequence;
            }
            next = index + 1;
            std::shared_ptr<Gallery> gallery;
            {
                std::lock_guard<std::mutex> lock(m_galleryMutex);
                gallery = m_gallery;
            }
            const float threshold = m_currentThreshold;
            const std::shared_ptr<const CameraRules::Rules> rules = std::atomic_load(&feed.rules);
            QList<Face> faces;
            try {
                for (const AnalyzedFace &analyzed : m_engine->analyze(frame, false)) {
                    Face face;
                    const cv::Rect2f &box = analyzed.detection.box;
                    face.box = QRectF(box.x, box.y, box.width, box.height);
                    // خارج مناطق الكاميرا: لا يُعرض ولا يُطابق
                    if (rules && !rules->accepts(face.box, QSize(frame.cols, frame.rows)))
                        continue;
                    face.embedding = analyzed.embedding;
                    face.quality = analyzed.quality;
                    if (gallery && !analyzed.embedding.empty())
                        face.match = FaceDatabase::identify(*gallery, analyzed.embedding, threshold);
                    // وجه مشوش أو جانبي جداً لا يكفي وحده لتأكيد الشخص
                    if (face.match.status == MatchStatus::Confirmed && analyzed.quality < 0.2f)
                        face.match.status = MatchStatus::Possible;
                    faces.append(face);
                }
            } catch (const std::exception &) {
            }
            QMetaObject::invokeMethod(
                this, [this, index, generation, frame, faces] { onAnalyzed(int(index), generation, frame, faces); },
                Qt::QueuedConnection);
            worked = true;
            break;
        }
        if (!worked)
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void MultiCameraWindow::onAnalyzed(int index, quint64 generation, const cv::Mat &frame, const QList<Face> &faces)
{
    if (generation != m_generation || index >= int(m_feeds.size()))
        return;
    Feed &feed = *m_feeds[size_t(index)];
    const qint64 now = nowMs();
    feed.faces = faces;
    feed.facesSize = QSize(frame.cols, frame.rows);
    feed.facesAt = now;

    QList<CameraWatch::Face> watched;
    for (const Face &face : faces) {
        CameraWatch::Face item;
        item.box = face.box;
        item.match = face.match;
        item.embedding = face.embedding;
        item.quality = face.quality;
        if (face.match.status != MatchStatus::Possible)
            item.crop = ImageIO::cropFace(frame, cv::Rect2f(float(face.box.x()), float(face.box.y()), float(face.box.width()),
                                                            float(face.box.height())),
                                          256);
        watched.append(item);
    }
    if (feed.recorder->isRecording() && frame.cols > 0 && frame.rows > 0)
        feed.recorder->setOverlay(feed.watch->overlay(QSize(frame.cols, frame.rows), watched));
    feed.watch->setThreshold(m_currentThreshold);
    feed.watch->process(faces.isEmpty() ? QImage() : ImageIO::toQImage(frame), watched);
}

void MultiCameraWindow::noteLost(Feed &feed, qint64 now)
{
    if (feed.lostAt > 0)
        return;
    feed.lostAt = now;
    AppLog::write(QStringLiteral("عدة كاميرات: انقطعت «%1»: %2").arg(feed.camera.name, feed.error));
}

void MultiCameraWindow::showAlert(Feed &feed, const QString &personId, const QString &name, float score, const QImage &crop)
{
    ++m_alertCount;
    const int level = m_database->alertLevel(personId);
    if (level == AlertSound::NotifyOnly) {
        emit watchAlert(name, feed.camera.name, level);
        return;
    }
    feed.alertUntil = nowMs() + AlertFrameMs;
    m_alertAvatar->setPixmap(circularPixmap(crop, 44));
    m_alert->setText(QStringLiteral("<span style='color:%1; font-weight:700; font-size:12pt'>تنبيه مراقبة: ظهر %2</span>"
                                    "<br><span style='color:#e8dccd'>أمام «%3» · التشابه %4% · الساعة ⁦%5⁩</span>")
                         .arg(Theme::Watched.name(), name.toHtmlEscaped(), feed.camera.name.toHtmlEscaped())
                         .arg(qRound(score * 100))
                         .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
    m_alertBar->show();
    if (level == AlertSound::High)
        AlertSound::play(AlertSound::Sound::Alarm);
    else if (level != AlertSound::Silent)
        AlertSound::play(AlertSound::Sound::Chime);
    emit watchAlert(name, feed.camera.name, level);
}

void MultiCameraWindow::toggleRecording()
{
    if (m_feeds.empty())
        return;
    m_recording = !isRecording();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const auto &feed : m_feeds) {
        if (m_recording)
            startRecording(*feed);
        else
            feed->recorder->stop();
        updateTitle(*feed);
    }
    QApplication::restoreOverrideCursor();
    updateRecordButton();
}

void MultiCameraWindow::reloadRules(const QString &source)
{
    for (const auto &feed : m_feeds) {
        if (feed->source != source)
            continue;
        feed->watch->reloadRules();
        std::atomic_store(&feed->rules, std::make_shared<const CameraRules::Rules>(feed->watch->rules()));
        if (feed->recorder->isRecording()) {
            feed->recorder->stop();
            startRecording(*feed);
        }
    }
}

void MultiCameraWindow::startRecording(Feed &feed)
{
    if (feed.recorder->isRecording() || !feed.reader)
        return;
    CameraRecorder::Options options = CameraRecorder::savedOptions();
    options.ownFilters = true;
    options.filters = filtersOf(feed.camera);
    options.schedule = CameraRules::load(feed.camera.source()).recording;
    QString error;
    if (!feed.recorder->start(CameraRecorder::savedFolder(), feed.camera.name, feed.reader->fps(), options, &error))
        notice(QStringLiteral("تعذر تسجيل «%1»: %2").arg(feed.camera.name, error));
    updateRecordButton();
}

void MultiCameraWindow::updateRecordButton()
{
    const bool recording = isRecording();
    m_recordButton->setEnabled(!m_feeds.empty());
    m_recordButton->setText(recording ? QStringLiteral("إيقاف التسجيل") : QStringLiteral("تسجيل الكل"));
    m_recordButton->setIcon(Icons::icon(recording ? Icons::Name::Stop : Icons::Name::Film, Theme::Accent2, 18));
}

void MultiCameraWindow::notice(const QString &text)
{
    m_notice = text;
    m_noticeUntil = nowMs() + 12000;
    m_status->setText(text);
}

void MultiCameraWindow::closeEvent(QCloseEvent *event)
{
    stop();
    QWidget::closeEvent(event);
}

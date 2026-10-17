// النافذة الرئيسية: أدوات تشغيل الفيديو تحت العرض، التنقل بين الأحداث والملفات، العلامات، ملء الشاشة

#include "mainwindow_p.h"
#include "playbackbar.h"
#include "videoaudio.h"
#include "videomarkers.h"

#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QPushButton>
#include <QShortcut>

using namespace MainWindowHelpers;

namespace {

// لا يُعدّ الحدث «تالياً» إن كان عند الموضع الحالي تقريباً، فيتقدم الضغط المتكرر حدثاً حدثاً
constexpr qint64 EventGapMs = 300;

} // namespace

void MainWindow::setupPlaybackBar()
{
    m_audio = new VideoAudio(this);
    m_audio->setVolume(m_playbackBar->volume(), m_playbackBar->isMuted());
    connect(m_audio, &VideoAudio::availabilityChanged, m_playbackBar, &PlaybackBar::setAudioAvailable);
    connect(&m_previewWatcher, &QFutureWatcherBase::finished, this, &MainWindow::onPreviewLoaded);
    // النقر على الفيديو يعطيه التركيز فتعمل الأسهم للتشغيل لا لقائمة الوجوه
    m_view->setFocusPolicy(Qt::ClickFocus);

    PlaybackBar *bar = m_playbackBar;
    connect(bar, &PlaybackBar::playToggled, this, &MainWindow::togglePause);
    connect(bar, &PlaybackBar::jumpRequested, this, &MainWindow::jumpBy);
    connect(bar, &PlaybackBar::frameStepRequested, this, &MainWindow::stepFrame);
    connect(bar, &PlaybackBar::seekRequested, this, &MainWindow::seekVideo);
    connect(bar, &PlaybackBar::rateChanged, this, [this](double rate) {
        m_player->setRate(rate);
        syncAudio();
        statusBar()->showMessage(QStringLiteral("سرعة العرض %1×%2")
                                     .arg(QLocale().toString(rate, 'g', 2),
                                          rate < 0.5 || rate > 2.0 ? QStringLiteral(" — بلا صوت في هذه السرعة") : QString()),
                                 4000);
    });
    connect(bar, &PlaybackBar::volumeChanged, m_audio, &VideoAudio::setVolume);
    connect(bar, &PlaybackBar::fileRequested, this, &MainWindow::navigateVideo);
    connect(bar, &PlaybackBar::eventRequested, this, &MainWindow::jumpToEvent);
    connect(bar, &PlaybackBar::markerRequested, this, [this] { addMarker(m_videoPositionMs); });
    connect(bar, &PlaybackBar::rangeStartHere, this, [this] {
        const qint64 end = m_rangeEnd->time().msecsSinceStartOfDay();
        setTrimRange(m_videoPositionMs, end > m_videoPositionMs ? end : 0);
    });
    connect(bar, &PlaybackBar::rangeEndHere, this, [this] {
        const qint64 start = m_rangeStart->time().msecsSinceStartOfDay();
        if (m_videoPositionMs > 0)
            setTrimRange(start < m_videoPositionMs ? start : 0, m_videoPositionMs);
    });
    connect(bar, &PlaybackBar::rangeCleared, this, [this] { setTrimRange(0, 0); });
    connect(bar, &PlaybackBar::analyzeRangeRequested, this, &MainWindow::reanalyzeVideo);
    connect(bar, &PlaybackBar::exportRangeRequested, this, &MainWindow::exportTrimmedClip);
    connect(bar, &PlaybackBar::fullscreenRequested, this, &MainWindow::toggleVideoFullscreen);
    connect(bar, &PlaybackBar::panelsRequested, this, &MainWindow::togglePanels);
    connect(bar, &PlaybackBar::shortcutsRequested, this, &MainWindow::showShortcuts);
}

void MainWindow::setupPlaybackShortcuts()
{
    // الحروف تعمل فقط حين يُعرض فيديو ملف، ولا تصل إلى حقول الكتابة لأنها تأخذ مفاتيح النص أولاً
    const auto add = [this](const QKeySequence &key, auto slot) {
        connect(new QShortcut(key, this), &QShortcut::activated, this, [this, slot] {
            if (fileVideoActive() || m_videoFullscreen)
                slot();
        });
    };
    add(QKeySequence(Qt::Key_K), [this] { togglePause(); });
    add(QKeySequence(Qt::Key_J), [this] { jumpBy(-10000); });
    add(QKeySequence(Qt::Key_Comma), [this] { stepFrame(-1); });
    add(QKeySequence(Qt::Key_Period), [this] { stepFrame(1); });
    // Shift مع الفاصلة والنقطة يعطي < و >
    add(QKeySequence(Qt::Key_Less), [this] { m_playbackBar->changeRate(-1); });
    add(QKeySequence(Qt::Key_Greater), [this] { m_playbackBar->changeRate(1); });
    add(QKeySequence(Qt::Key_I), [this] { emit m_playbackBar->rangeStartHere(); });
    add(QKeySequence(Qt::Key_O), [this] { emit m_playbackBar->rangeEndHere(); });
    add(QKeySequence(Qt::Key_F), [this] { toggleVideoFullscreen(); });
    add(QKeySequence(Qt::Key_M), [this] { m_playbackBar->toggleMute(); });
    add(QKeySequence(Qt::Key_B), [this] { addMarker(m_videoPositionMs); });
    add(QKeySequence(Qt::Key_N), [this] { jumpToEvent(1); });
    add(QKeySequence(Qt::SHIFT | Qt::Key_N), [this] { jumpToEvent(-1); });
    add(QKeySequence(Qt::Key_P), [this] { togglePanels(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_G), [this] { m_playbackBar->focusGoTo(); });
    connect(new QShortcut(QKeySequence(Qt::Key_F1), this), &QShortcut::activated, this, &MainWindow::showShortcuts);
}

bool MainWindow::fileVideoActive() const
{
    return m_videoMode && m_pages->currentIndex() == 0 && !m_videoSource.isEmpty()
        && !m_videoSource.startsWith(QStringLiteral("camera:"));
}

// الأسهم و Home و End تصل هنا حين لا يستعملها العنصر الذي عليه التركيز (القوائم والحقول تستعملها)
void MainWindow::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (fileVideoActive() && (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier)) {
        const bool shift = modifiers == Qt::ShiftModifier;
        switch (event->key()) {
        case Qt::Key_Left:
            jumpBy(shift ? -30000 : -5000);
            return;
        case Qt::Key_Right:
            jumpBy(shift ? 30000 : 5000);
            return;
        case Qt::Key_Up:
            m_playbackBar->changeVolume(5);
            return;
        case Qt::Key_Down:
            m_playbackBar->changeVolume(-5);
            return;
        case Qt::Key_Home:
            seekVideo(0);
            return;
        case Qt::Key_End: {
            const qint64 duration = m_player->durationMs() > 0 ? m_player->durationMs() : m_videoDurationMs;
            if (duration > 0)
                seekVideo(duration - qint64(1000.0 / std::max(1.0, m_videoFps)));
            return;
        }
        default:
            break;
        }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::seekVideo(qint64 positionMs)
{
    if (!fileVideoActive())
        return;
    const qint64 duration = filePlayerOpen() && m_player->durationMs() > 0 ? m_player->durationMs() : m_videoDurationMs;
    positionMs = std::max<qint64>(0, duration > 0 ? std::min(positionMs, duration - 1) : positionMs);
    // يُحفظ الهدف فوراً، فالضغطات المتتالية تتراكم ولا تنتظر عرض الإطار
    m_videoPositionMs = positionMs;
    loadVideoFrame(positionMs);
}

void MainWindow::jumpBy(qint64 deltaMs)
{
    seekVideo(m_videoPositionMs + deltaMs);
}

void MainWindow::stepFrame(int frames)
{
    if (!fileVideoActive())
        return;
    if (filePlayerOpen()) {
        m_player->stepFrames(frames);
        m_audio->seeked();
        updatePauseButton();
        return;
    }
    seekVideo(m_videoPositionMs + qint64(std::llround(frames * 1000.0 / std::max(1.0, m_videoFps))));
}

void MainWindow::jumpToEvent(int direction)
{
    if (!fileVideoActive())
        return;
    const PlaybackBar::Event kind = m_playbackBar->eventKind();
    QList<qint64> times;
    QString what;
    const auto addAppearances = [&times](const FaceItem &face) {
        for (const auto &segment : face.appearances)
            times.append(segment.first);
        if (face.appearances.isEmpty() && face.firstSeenMs >= 0)
            times.append(face.firstSeenMs);
    };
    switch (kind) {
    case PlaybackBar::Event::Person: {
        const int row = m_faceList->currentRow();
        if (row < 0 || row >= m_faces.size()) {
            Toast::show(this, QStringLiteral("حدد شخصاً من القائمة أو الخط الزمني أولاً"), Toast::Kind::Warning);
            return;
        }
        addAppearances(m_faces.at(row));
        what = QStringLiteral("ظهور %1").arg(matchLabel(m_faces.at(row).match));
        break;
    }
    case PlaybackBar::Event::Known:
        for (const FaceItem &face : std::as_const(m_faces))
            if (face.match.status == MatchStatus::Confirmed)
                addAppearances(face);
        what = QStringLiteral("وجه معروف");
        break;
    case PlaybackBar::Event::Alert:
        for (const FaceItem &face : std::as_const(m_faces))
            if (face.match.status == MatchStatus::Confirmed && m_watched.contains(face.match.personId))
                addAppearances(face);
        what = QStringLiteral("تنبيه مراقبة");
        break;
    case PlaybackBar::Event::Shot:
        times = m_shotChanges;
        what = QStringLiteral("تغيّر اللقطة");
        break;
    case PlaybackBar::Event::Marker:
        times = m_markers.keys();
        what = QStringLiteral("علامة");
        break;
    }
    std::sort(times.begin(), times.end());
    const qint64 now = m_videoPositionMs;
    qint64 target = -1;
    if (direction > 0) {
        const auto it = std::upper_bound(times.cbegin(), times.cend(), now + EventGapMs);
        if (it != times.cend())
            target = *it;
    } else {
        const auto it = std::lower_bound(times.cbegin(), times.cend(), now - EventGapMs);
        if (it != times.cbegin())
            target = *std::prev(it);
    }
    if (target < 0) {
        Toast::show(this, times.isEmpty() ? QStringLiteral("لا يوجد %1 في هذا الفيديو بعد").arg(what)
                                          : (direction > 0 ? QStringLiteral("لا يوجد %1 بعد هذا الموضع").arg(what)
                                                           : QStringLiteral("لا يوجد %1 قبل هذا الموضع").arg(what)),
                    Toast::Kind::Info);
        return;
    }
    seekVideo(target);
    QString text = QStringLiteral("%1 عند %2").arg(what, ImageIO::formatTime(target));
    if (kind == PlaybackBar::Event::Marker && !m_markers.value(target).isEmpty())
        text += QStringLiteral(" — ") + m_markers.value(target);
    statusBar()->showMessage(text, 5000);
}

// فيديوهات مجلد الفيديو الحالي بالترتيب الطبيعي (2 قبل 10) كما في مستكشف الملفات
void MainWindow::updateVideoNavigation()
{
    m_videoSiblings.clear();
    const bool fileVideo = !m_videoSource.isEmpty() && !m_videoSource.startsWith(QStringLiteral("camera:"));
    if (fileVideo) {
        const QFileInfo info(m_videoSource);
        const QStringList files = info.absoluteDir().entryList(QDir::Files | QDir::Readable);
        for (const QString &name : files)
            if (isVideoFile(name))
                m_videoSiblings.append(name);
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(m_videoSiblings.begin(), m_videoSiblings.end(), collator);
    }
    const int index = fileVideo ? int(m_videoSiblings.indexOf(QFileInfo(m_videoSource).fileName())) : -1;
    m_playbackBar->setFileNavigation(index > 0, index >= 0 && index + 1 < m_videoSiblings.size());
}

void MainWindow::navigateVideo(int step)
{
    if (!fileVideoActive() || !m_ready || m_analyzing)
        return;
    const int index = int(m_videoSiblings.indexOf(QFileInfo(m_videoSource).fileName()));
    const int next = index + step;
    if (index < 0 || next < 0 || next >= m_videoSiblings.size()) {
        Toast::show(this, step > 0 ? QStringLiteral("هذا آخر فيديو في المجلد") : QStringLiteral("هذا أول فيديو في المجلد"));
        return;
    }
    const QString path = QFileInfo(m_videoSource).absoluteDir().filePath(m_videoSiblings.at(next));
    {
        // الجزء المحدد خاص بالفيديو السابق
        const QSignalBlocker blockStart(m_rangeStart), blockEnd(m_rangeEnd);
        m_rangeStart->setTime(QTime(0, 0));
        m_rangeEnd->setTime(QTime(0, 0));
    }
    m_lastDirectory = QFileInfo(path).absolutePath();
    if (m_videoRunning) {
        m_pendingVideo = path;
        stopVideo();
        return;
    }
    startVideo(path);
}

void MainWindow::checkAutoNext()
{
    if (!m_playbackBar->autoNext() || m_videoRunning || !fileVideoActive() || !filePlayerOpen() || m_player->isPlaying())
        return;
    const qint64 endMs = m_videoEndMs > 0 ? m_videoEndMs : m_player->durationMs();
    if (endMs <= 0 || m_player->position() + 2 * qint64(1000.0 / m_player->fps()) < endMs)
        return;
    const int index = int(m_videoSiblings.indexOf(QFileInfo(m_videoSource).fileName()));
    if (index < 0 || index + 1 >= m_videoSiblings.size())
        return;
    Toast::show(this, QStringLiteral("التشغيل المتتالي: يُفتح «%1»").arg(m_videoSiblings.at(index + 1)));
    // مهلة قصيرة يرى فيها المستخدم النتيجة، ولا يُنتقل إن تغيّر شيء أثناءها
    QTimer::singleShot(1500, this, [this, source = m_videoSource] {
        if (source == m_videoSource && m_playbackBar->autoNext() && !m_videoRunning && filePlayerOpen() && !m_player->isPlaying())
            navigateVideo(1);
    });
}

void MainWindow::addMarker(qint64 positionMs)
{
    if (!fileVideoActive())
        return;
    const bool wasPlaying = filePlayerOpen() && m_player->isPlaying();
    if (wasPlaying) {
        m_player->pause();
        updatePauseButton();
    }
    bool ok = false;
    const QString note = QInputDialog::getText(this, QStringLiteral("علامة جديدة"),
                                               QStringLiteral("ملاحظة للعلامة عند %1 (اختيارية):").arg(ImageIO::formatTime(positionMs)),
                                               QLineEdit::Normal, QString(), &ok);
    if (ok) {
        m_markers.insert(positionMs, note.trimmed());
        VideoMarkers::save(m_videoSource, m_markers);
        m_timeline->setMarkers(m_markers);
        Toast::show(this, QStringLiteral("أُضيفت علامة عند %1 — تُحفظ مع الفيديو، ويُتنقل بينها بـ N").arg(ImageIO::formatTime(positionMs)));
    }
    if (wasPlaying && filePlayerOpen()) {
        m_player->play();
        updatePauseButton();
    }
}

void MainWindow::editMarker(qint64 markerMs)
{
    if (!m_markers.contains(markerMs))
        return;
    bool ok = false;
    const QString note = QInputDialog::getText(this, QStringLiteral("تعديل العلامة"),
                                               QStringLiteral("ملاحظة العلامة عند %1:").arg(ImageIO::formatTime(markerMs)),
                                               QLineEdit::Normal, m_markers.value(markerMs), &ok);
    if (!ok)
        return;
    m_markers.insert(markerMs, note.trimmed());
    VideoMarkers::save(m_videoSource, m_markers);
    m_timeline->setMarkers(m_markers);
}

void MainWindow::removeMarker(qint64 markerMs)
{
    if (m_markers.remove(markerMs) == 0)
        return;
    VideoMarkers::save(m_videoSource, m_markers);
    m_timeline->setMarkers(m_markers);
    statusBar()->showMessage(QStringLiteral("حُذفت العلامة عند %1").arg(ImageIO::formatTime(markerMs)), 4000);
}

qint64 MainWindow::markerNear(qint64 positionMs) const
{
    // نحو نصف بالمئة من مدة الفيديو، فتُصاب العلامة بالنقر عليها في أي تكبير معتاد
    const qint64 tolerance = std::max<qint64>(500, m_videoDurationMs / 200);
    qint64 best = -1;
    for (auto it = m_markers.cbegin(); it != m_markers.cend(); ++it) {
        if (std::llabs(it.key() - positionMs) <= tolerance && (best < 0 || std::llabs(it.key() - positionMs) < std::llabs(best - positionMs)))
            best = it.key();
    }
    return best;
}

void MainWindow::toggleVideoFullscreen()
{
    if (!m_videoFullscreen) {
        if (!fileVideoActive())
            return;
        m_fullscreenHidden.clear();
        const QList<QWidget *> parts = {m_sidebar, m_recognitionHeader, m_settingsCard, m_statusStrip, m_sideCard, m_timelineCard,
                                        m_progress->parentWidget(), statusBar()};
        for (QWidget *part : parts) {
            if (part && part->isVisible()) {
                m_fullscreenHidden.append(part);
                part->hide();
            }
        }
        m_wasMaximized = isMaximized();
        m_videoFullscreen = true;
        showFullScreen();
        m_view->setFocus();
    } else {
        m_videoFullscreen = false;
        for (const QPointer<QWidget> &part : std::as_const(m_fullscreenHidden))
            if (part)
                part->show();
        m_fullscreenHidden.clear();
        // اللوحات التي أخفاها المستخدم بـ P تبقى مخفية
        if (m_panelsHidden) {
            m_sideCard->hide();
            m_settingsCard->hide();
        }
        if (m_wasMaximized)
            showMaximized();
        else
            showNormal();
    }
    m_playbackBar->setFullscreen(m_videoFullscreen);
}

void MainWindow::togglePanels()
{
    if (m_videoFullscreen)
        return;
    if (!m_panelsHidden && !fileVideoActive())
        return;
    m_panelsHidden = !m_panelsHidden;
    m_sideCard->setVisible(!m_panelsHidden);
    m_settingsCard->setVisible(!m_panelsHidden);
    m_playbackBar->setPanelsHidden(m_panelsHidden);
}

void MainWindow::showShortcuts()
{
    struct Row
    {
        const char *keys;
        const char *action;
    };
    // النصوص هنا بالعربية مباشرة؛ QString::fromUtf8 يحوّلها
    static const Row Video[] = {
        {"مسافة أو K", "تشغيل / إيقاف مؤقت"},
        {"← / →", "رجوع / تقدم 5 ثوانٍ"},
        {"Shift + ← / →", "رجوع / تقدم 30 ثانية"},
        {"J / L", "رجوع / تقدم 10 ثوانٍ"},
        {", / .", "إطار سابق / إطار تالٍ"},
        {"< / >", "أبطأ / أسرع (¼× إلى 8×)"},
        {"Home / End", "بداية الفيديو / نهايته"},
        {"↑ / ↓", "رفع الصوت / خفضه"},
        {"M", "كتم الصوت"},
        {"Ctrl+G", "الانتقال إلى وقت محدد"},
        {"I / O", "بداية الجزء / نهايته عند الموضع الحالي"},
        {"N / Shift+N", "الحدث التالي / السابق (حسب النوع المختار)"},
        {"B", "إضافة علامة بملاحظة"},
        {"PageDown / PageUp", "الفيديو التالي / السابق في المجلد"},
        {"F", "ملء الشاشة"},
        {"P", "إخفاء اللوحات الجانبية / إظهارها"},
        {"Esc", "الخروج من ملء الشاشة، ثم إيقاف التحليل"},
    };
    static const Row General[] = {
        {"Ctrl+O", "فتح صورة"},
        {"Ctrl+Shift+O", "فتح فيديو"},
        {"Ctrl+Shift+C", "تشغيل آخر كاميرا"},
        {"Ctrl+Shift+F", "البحث في مجلد صور"},
        {"Ctrl+V", "لصق صورة وتحليلها"},
        {"Ctrl+S", "حفظ الوجوه والتقرير"},
        {"Ctrl+E", "تحسين الصورة وتوضيحها"},
        {"Ctrl+Shift+E", "استوديو تحسين الصورة"},
        {"Ctrl+R", "تسجيل الكاميرا"},
        {"F5", "إعادة تحليل الصورة"},
        {"L", "إظهار أسماء الوجوه في الصورة"},
        {"Ctrl+= / Ctrl+- / Ctrl+0", "تكبير الصورة / تصغيرها / عرضها كاملة"},
        {"Ctrl+F", "البحث في قاعدة البيانات"},
        {"Ctrl+1 … Ctrl+4", "التنقل بين الصفحات"},
        {"F1", "هذه القائمة"},
    };
    const auto table = [](const QString &title, const Row *rows, std::size_t count) {
        QString html = QStringLiteral("<h3 style='margin:12px 0 6px 0'>%1</h3><table cellspacing='0' cellpadding='5' width='100%'>").arg(title);
        for (std::size_t i = 0; i < count; ++i) {
            html += QStringLiteral("<tr><td width='42%'><span style='background:%1; border-radius:4px; padding:2px 6px'>"
                                   "<b dir='ltr'>&nbsp;%2&nbsp;</b></span></td><td>%3</td></tr>")
                        .arg(Theme::SurfaceHigh.name(), QString::fromUtf8(rows[i].keys).toHtmlEscaped(),
                             QString::fromUtf8(rows[i].action).toHtmlEscaped());
        }
        return html + QStringLiteral("</table>");
    };
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("اختصارات لوحة المفاتيح"));
    dialog.resize(560, 640);
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QLabel(&dialog);
    text->setTextFormat(Qt::RichText);
    text->setWordWrap(true);
    text->setAlignment(Qt::AlignTop);
    text->setText(table(QStringLiteral("تشغيل الفيديو"), Video, std::size(Video))
                  + table(QStringLiteral("عام"), General, std::size(General)));
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidget(text);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("إغلاق"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::requestPreview(qint64 positionMs)
{
    if (!fileVideoActive())
        return;
    m_previewRequestMs = positionMs;
    if (m_previewWatcher.isRunning())
        return; // يُقرأ آخر موضع مطلوب عند انتهاء الحالي
    if (!m_previewCapture || m_previewPath != m_videoSource) {
        m_previewCapture = std::make_shared<cv::VideoCapture>();
        m_previewPath = m_videoSource;
    }
    const qint64 ms = std::exchange(m_previewRequestMs, -1);
    std::shared_ptr<cv::VideoCapture> capture = m_previewCapture;
    const QString path = m_previewPath;
    // نسخة مستقلة عن المشغّل والتحليل، فالمعاينة لا تحرّك موضع العرض
    m_previewWatcher.setFuture(QtConcurrent::run([capture, path, ms] {
        if (!capture->isOpened() && !ImageIO::openVideo(*capture, path))
            return qMakePair(ms, QImage());
        capture->set(cv::CAP_PROP_POS_MSEC, double(ms));
        cv::Mat frame;
        if (!capture->read(frame) || frame.empty())
            return qMakePair(ms, QImage());
        return qMakePair(ms, VideoPlayer::displayImage(frame).scaledToHeight(120, Qt::SmoothTransformation));
    }));
}

void MainWindow::onPreviewLoaded()
{
    const QPair<qint64, QImage> preview = m_previewWatcher.result();
    if (!preview.second.isNull() && m_previewPath == m_videoSource && fileVideoActive())
        m_timeline->setPreviewFrame(preview.first, preview.second);
    if (m_previewRequestMs >= 0)
        requestPreview(m_previewRequestMs);
}

void MainWindow::syncAudio()
{
    if (!m_audio)
        return;
    if (filePlayerOpen())
        m_audio->sync(m_player->isPlaying(), m_player->position(), m_player->rate());
    else
        m_audio->sync(false, 0, 1.0);
}

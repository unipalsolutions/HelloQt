// النافذة الرئيسية: أدوات تتبع حالة العمل (بلا ملف، صورة، فيديو، كاميرا)، شريط الحالة الدائم مع الخطوة
// المقترحة، قائمة «فتح»، شاشة البداية، وحفظ التخطيط بين التشغيلات

#include "mainwindow_p.h"
#include "playbackbar.h"
#include "startscreen.h"
#include "videoaudio.h"

#include <QSplitter>
#include <QStackedWidget>

using namespace MainWindowHelpers;

MainWindow::WorkState MainWindow::workState() const
{
    if (m_videoMode)
        return m_videoSource.startsWith(QStringLiteral("camera:")) ? WorkState::Camera : WorkState::Video;
    if (!m_currentImagePath.isEmpty() || m_view->hasImage() || m_analyzing)
        return WorkState::Image;
    return WorkState::Empty;
}

void MainWindow::updateWorkState()
{
    if (!m_viewStack)
        return;
    const WorkState state = workState();
    const bool empty = state == WorkState::Empty;
    const bool image = state == WorkState::Image;
    const bool video = state == WorkState::Video;
    const bool camera = state == WorkState::Camera;

    // بلا ملف: أزرار الفتح كلها؛ بعده تجتمع في «فتح» وتظهر أدوات الحالة
    for (FancyButton *button : {m_openImageButton, m_openVideoButton, m_recentButton, m_cameraButton, m_folderScanButton})
        button->setVisible(empty);
    m_openMenuButton->setVisible(!empty);
    m_openMenuButton->setEnabled(m_ready);
    m_enhanceImageButton->setVisible(image || (video && !m_videoRunning));
    m_saveFacesButton->setVisible(!empty);
    m_stopButton->setVisible(m_videoRunning);

    // زر أساسي واحد بارز في كل حالة: ما يُتوقع أن يفعله المستخدم الآن
    FancyButton *primary = nullptr;
    if (empty)
        primary = m_openImageButton;
    else if (image && !m_analyzing)
        primary = m_faces.isEmpty() ? m_enhanceImageButton : m_saveFacesButton;
    else if (video && !m_videoRunning)
        primary = m_faces.isEmpty() ? m_openMenuButton : m_saveFacesButton;
    else if (camera && m_videoRunning)
        primary = m_recordButton;
    for (FancyButton *button : {m_openImageButton, m_openMenuButton, m_enhanceImageButton, m_saveFacesButton, m_recordButton})
        button->setKind(button == primary ? FancyButton::Kind::Primary : FancyButton::Kind::Secondary);

    // الإعدادات السريعة حسب الحالة، والنادرة في «إعدادات التحليل»
    m_thoroughSwitch->setVisible(image);
    m_regionGroup->setVisible(video || camera);
    m_reanalyzeButton->setVisible(video);
    m_recordGroup->setVisible(camera);
    m_frameStepGroup->setVisible(video || camera);
    if (!m_videoFullscreen) {
        m_settingsCard->setVisible(!empty && !m_panelsHidden);
        m_sideCard->setVisible(!empty && !m_panelsHidden);
    }
    m_viewStack->setCurrentIndex(empty ? 0 : 1);
    m_startScreen->setReady(m_ready);

    if (m_statusTimer) {
        if (m_videoRunning && !m_statusTimer->isActive())
            m_statusTimer->start();
        else if (!m_videoRunning)
            m_statusTimer->stop();
    }
    updateStatusStrip();
}

void MainWindow::setAdvancedSettingsVisible(bool visible)
{
    m_advancedPanel->setVisible(visible);
    if (m_advancedButton->isChecked() != visible) {
        const QSignalBlocker blocker(m_advancedButton);
        m_advancedButton->setChecked(visible);
    }
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("layout/advanced"), visible);
}

// ───────────── شريط الحالة الدائم ─────────────

QWidget *MainWindow::createStatusStrip(QWidget *parent)
{
    m_statusStrip = new QFrame(parent);
    m_statusStrip->setObjectName(QStringLiteral("statusStrip"));
    m_statusStrip->setStyleSheet(QStringLiteral("#statusStrip { background: %1; border: 1px solid %2; border-radius: 12px; }")
                                     .arg(Theme::Surface.name(), Theme::Border.name()));
    auto *layout = new QHBoxLayout(m_statusStrip);
    layout->setContentsMargins(12, 4, 8, 4);
    layout->setSpacing(10);
    m_statusIcon = new QLabel(m_statusStrip);
    m_statusIcon->setFixedSize(20, 20);
    m_statusTitle = new QLabel(m_statusStrip);
    m_statusTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_statusDetail = label(QString(), QStringLiteral("muted"), m_statusStrip);
    m_statusDetail->setMinimumWidth(10);
    m_statusAction = new FancyButton(Icons::Name::Next, QString(), FancyButton::Kind::Secondary, m_statusStrip);
    m_statusAction->setVisible(false);
    connect(m_statusAction, &QAbstractButton::clicked, this, [this] {
        if (m_statusActionSlot)
            m_statusActionSlot();
    });
    layout->addWidget(m_statusIcon);
    layout->addWidget(m_statusTitle);
    layout->addWidget(m_statusDetail, 1);
    m_statusHint = label(QStringLiteral("الخطوة المقترحة:"), QStringLiteral("muted"), m_statusStrip);
    m_statusHint->setVisible(false);
    layout->addWidget(m_statusHint);
    layout->addWidget(m_statusAction);

    // أثناء التحليل يتحدث المتبقي والسرعة مرة كل ثانية
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(1000);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatusStrip);
    return m_statusStrip;
}

void MainWindow::setStatusAction(Icons::Name icon, const QString &text, const QString &tip, std::function<void()> action)
{
    m_statusActionSlot = std::move(action);
    const bool show = !text.isEmpty() && m_statusActionSlot;
    if (show) {
        m_statusAction->setIconName(icon);
        m_statusAction->setText(text);
        m_statusAction->setToolTip(tip);
    }
    m_statusAction->setVisible(show);
    m_statusHint->setVisible(show);
}

void MainWindow::updateStatusStrip()
{
    if (!m_statusStrip)
        return;
    int known = 0;
    int possible = 0;
    int unknown = 0;
    for (const FaceItem &face : std::as_const(m_faces)) {
        if (face.match.status == MatchStatus::Confirmed)
            ++known;
        else if (face.match.status == MatchStatus::Possible)
            ++possible;
        else
            ++unknown;
    }
    const auto counts = [&] {
        QStringList parts;
        parts << QStringLiteral("معروفون %1").arg(known);
        if (possible > 0)
            parts << QStringLiteral("محتملون %1").arg(possible);
        parts << QStringLiteral("مجهولون %1").arg(unknown);
        return parts.join(QStringLiteral(" · "));
    };
    const auto showTab = [this](int tab) {
        if (QAbstractButton *button = m_faceTabs->button(tab))
            button->click();
        if (m_panelsHidden)
            togglePanels();
    };

    Icons::Name icon = Icons::Name::Info;
    QColor color = Theme::Accent2;
    QString title;
    QString detail;
    setStatusAction(Icons::Name::Next, QString(), QString(), nullptr);

    switch (workState()) {
    case WorkState::Empty:
        title = m_ready ? QStringLiteral("جاهز") : QStringLiteral("جارٍ تحميل نماذج التعرف…");
        detail = m_ready ? QStringLiteral("افتح صورة أو فيديو أو شغّل كاميرا، أو اسحب ملفاً إلى النافذة")
                         : QStringLiteral("يستغرق ذلك لحظات عند أول تشغيل");
        icon = m_ready ? Icons::Name::Check : Icons::Name::Clock;
        color = m_ready ? Theme::Success : Theme::Accent2;
        break;
    case WorkState::Image:
        if (m_analyzing) {
            title = QStringLiteral("جارٍ تحليل الصورة…");
            detail = QStringLiteral("كشف الوجوه ومطابقتها مع القاعدة");
            icon = Icons::Name::Clock;
        } else if (m_faces.isEmpty()) {
            title = QStringLiteral("لم يُعثر على وجوه");
            detail = m_thoroughSwitch->isChecked() ? QStringLiteral("قد تكون الوجوه صغيرة أو مظلمة جداً")
                                                   : QStringLiteral("جرّب «تحليل معمّق للصور» أو تحسين الصورة");
            icon = Icons::Name::Alert;
            color = Theme::Warning;
            setStatusAction(Icons::Name::Sparkle, QStringLiteral("تحسين الصورة"),
                            QStringLiteral("توضيح الصورة ثم تحليلها من جديد"), [this] { openEnhancer(); });
        } else {
            title = QStringLiteral("في الصورة %1 وجه").arg(m_faces.size());
            detail = counts();
            icon = Icons::Name::Check;
            color = Theme::Success;
            if (possible > 0)
                setStatusAction(Icons::Name::Search, QStringLiteral("مراجعة المحتملين"),
                                QStringLiteral("وجوه تشبه أشخاصاً في القاعدة دون يقين: أكّدها أو ارفضها"), [showTab] { showTab(2); });
            else if (unknown > 0)
                setStatusAction(Icons::Name::UserPlus, QStringLiteral("إضافة المجهولين"),
                                QStringLiteral("عرض الوجوه غير المعروفة لإضافتها إلى القاعدة"), [showTab] { showTab(3); });
            else if (m_folderIndex >= 0 && m_folderIndex + 1 < m_folderImages.size())
                setStatusAction(Icons::Name::Next, QStringLiteral("الصورة التالية"),
                                QStringLiteral("الصورة التالية في المجلد (Page Down)"), [this] { navigateImage(1); });
        }
        break;
    case WorkState::Video:
        if (m_videoRunning) {
            title = m_analysisProgress >= 0.0
                ? QStringLiteral("جارٍ تحليل الفيديو — %1%").arg(int(m_analysisProgress * 100.0))
                : QStringLiteral("جارٍ تحليل الفيديو…");
            QStringList parts;
            if (m_analysisRemainingMs >= 0)
                parts << QStringLiteral("المتبقي %1").arg(ImageIO::formatTime(m_analysisRemainingMs));
            if (m_analysisSpeed > 0.0)
                parts << QStringLiteral("السرعة %1×").arg(QLocale().toString(m_analysisSpeed, 'f', 1));
            parts << QStringLiteral("أشخاص %1").arg(m_faces.size()) << counts();
            if (filePlayerOpen() && !m_player->isPlaying())
                parts.prepend(QStringLiteral("العرض متوقف والتحليل مستمر"));
            detail = parts.join(QStringLiteral(" · "));
            icon = Icons::Name::Clock;
            if (filePlayerOpen() && !m_player->isPlaying())
                setStatusAction(Icons::Name::Play, QStringLiteral("تشغيل"), QStringLiteral("متابعة العرض (مسافة)"), [this] { togglePause(); });
        } else {
            title = QStringLiteral("انتهى تحليل الفيديو");
            detail = QStringLiteral("أشخاص مختلفون %1 · %2").arg(m_faces.size()).arg(counts());
            icon = Icons::Name::Check;
            color = Theme::Success;
            const int index = int(m_videoSiblings.indexOf(QFileInfo(m_videoSource).fileName()));
            if (!m_mergeSuggestions.isEmpty())
                setStatusAction(Icons::Name::Merge, QStringLiteral("مراجعة اقتراحات الدمج"),
                                QStringLiteral("وجوه متشابهة غالباً لشخص واحد"), [this] { showMergeSuggestions(); });
            else if (possible > 0)
                setStatusAction(Icons::Name::Search, QStringLiteral("مراجعة المحتملين"),
                                QStringLiteral("وجوه تشبه أشخاصاً في القاعدة دون يقين: أكّدها أو ارفضها"), [showTab] { showTab(2); });
            else if (unknown > 0)
                setStatusAction(Icons::Name::UserPlus, QStringLiteral("إضافة المجهولين"),
                                QStringLiteral("عرض الوجوه غير المعروفة لإضافتها إلى القاعدة"), [showTab] { showTab(3); });
            else if (index >= 0 && index + 1 < m_videoSiblings.size())
                setStatusAction(Icons::Name::SkipNext, QStringLiteral("الفيديو التالي"),
                                QStringLiteral("الفيديو التالي في المجلد (Page Down)"), [this] { navigateVideo(1); });
            else if (!m_faces.isEmpty())
                setStatusAction(Icons::Name::Save, QStringLiteral("حفظ الوجوه"), QStringLiteral("صور الوجوه مع تقرير (Ctrl+S)"),
                                [this] { saveFaces(); });
        }
        break;
    case WorkState::Camera:
        if (m_videoRunning) {
            title = m_videoPaused ? QStringLiteral("الكاميرا «%1» — التحليل متوقف مؤقتاً").arg(m_cameraName)
                                  : QStringLiteral("الكاميرا «%1» تعمل").arg(m_cameraName);
            QStringList parts;
            if (m_recorder->isRecording())
                parts << QStringLiteral("● تسجيل");
            parts << QStringLiteral("أشخاص مختلفون %1").arg(m_faces.size()) << counts();
            detail = parts.join(QStringLiteral(" · "));
            icon = m_videoPaused ? Icons::Name::Pause : Icons::Name::Camera;
            color = m_videoPaused ? Theme::Warning : Theme::Success;
            if (m_videoPaused)
                setStatusAction(Icons::Name::Play, QStringLiteral("استئناف"), QStringLiteral("متابعة التحليل (مسافة)"),
                                [this] { togglePause(); });
            else if (!m_recorder->isRecording())
                setStatusAction(Icons::Name::Record, QStringLiteral("بدء التسجيل"), QStringLiteral("تسجيل البث إلى ملف (Ctrl+R)"),
                                [this] { toggleRecording(); });
        } else {
            title = QStringLiteral("الكاميرا «%1» متوقفة").arg(m_cameraName.isEmpty() ? Cameras::displayName(m_videoSource) : m_cameraName);
            detail = QStringLiteral("أشخاص مختلفون %1 · %2").arg(m_faces.size()).arg(counts());
            icon = Icons::Name::Stop;
            color = Theme::Warning;
            if (m_ready)
                setStatusAction(Icons::Name::Camera, QStringLiteral("تشغيل من جديد"), QStringLiteral("تشغيل الكاميرا نفسها"),
                                [this] { startVideo(m_videoSource); });
        }
        break;
    }
    m_statusIcon->setPixmap(Icons::icon(icon, color, 20).pixmap(20, 20));
    m_statusTitle->setText(title);
    m_statusDetail->setText(detail);
    m_statusDetail->setToolTip(detail);
}

// ───────────── قائمة «فتح» وشاشة البداية ─────────────

void MainWindow::showOpenMenu()
{
    QMenu menu(this);
    const auto icon = [](Icons::Name name) { return Icons::icon(name, Theme::Accent2, 18); };
    const bool idle = m_openImageButton->isEnabled();
    menu.addAction(icon(Icons::Name::Image), QStringLiteral("صورة…\tCtrl+O"), this, &MainWindow::openImage)->setEnabled(idle);
    menu.addAction(icon(Icons::Name::Video), QStringLiteral("فيديو…\tCtrl+Shift+O"), this, &MainWindow::openVideo)->setEnabled(idle);
    menu.addAction(icon(Icons::Name::Camera), QStringLiteral("كاميرا…"), this, &MainWindow::openCamera)->setEnabled(idle);
    menu.addAction(icon(Icons::Name::Folder), QStringLiteral("مجلد صور…\tCtrl+Shift+F"), this, &MainWindow::openFolderScan)
        ->setEnabled(m_ready);
    QMenu *recent = menu.addMenu(icon(Icons::Name::Clock), QStringLiteral("الأخيرة"));
    fillRecentMenu(recent);
    recent->setEnabled(idle);
    menu.addSeparator();
    menu.addAction(icon(Icons::Name::Stop), QStringLiteral("إغلاق والعودة إلى البداية\tCtrl+W"), this, &MainWindow::closeCurrentFile)
        ->setEnabled(idle && !m_analyzing);
    menu.exec(m_openMenuButton->mapToGlobal(QPoint(0, m_openMenuButton->height())));
}

void MainWindow::closeCurrentFile()
{
    if (m_videoRunning || m_analyzing || workState() == WorkState::Empty)
        return;
    if (m_videoFullscreen)
        toggleVideoFullscreen();
    if (m_panelsHidden)
        togglePanels();
    m_player->close();
    m_audio->close();
    m_videoMode = false;
    m_videoSource.clear();
    m_currentImagePath.clear();
    updateFolderNavigation();
    clearWatchAlerts();
    m_faces.clear();
    m_hiddenFaces.clear();
    m_rejectedMatches.clear();
    m_mergeSuggestions.clear();
    m_mergeSuggestButton->setVisible(false);
    m_analyzedFaces.clear();
    m_markers.clear();
    m_shotChanges.clear();
    refreshFaceList();
    m_view->setFaces({});
    m_view->setRegion(QRectF());
    m_videoWorker->setRegion(QRectF());
    m_view->clear();
    m_playbackBar->setVisible(false);
    m_timeline->clear();
    m_timelineCard->setVisible(false);
    setBusy(false);
    statusBar()->clearMessage();
}

// ───────────── حفظ التخطيط ─────────────

void MainWindow::restoreLayout()
{
    const QSettings settings(settingsPath(), QSettings::IniFormat);
    const auto sizes = [&settings](const QString &key) {
        QList<int> values;
        for (const QVariant &value : settings.value(key).toList())
            values << value.toInt();
        return values;
    };
    // الأحجام المحفوظة تُقبل فقط إن كانت معقولة، فلا تختفي لوحة بحجم صفري من تشغيل سابق
    const QList<int> main = sizes(QStringLiteral("layout/main"));
    if (main.size() == 2 && main.at(0) > 200 && main.at(1) > 150)
        m_mainSplitter->setSizes(main);
    const QList<int> media = sizes(QStringLiteral("layout/media"));
    if (media.size() == 2 && media.at(0) > 120 && media.at(1) > 60)
        m_mediaSplitter->setSizes(media);
    setAdvancedSettingsVisible(settings.value(QStringLiteral("layout/advanced"), false).toBool());
    const int page = settings.value(QStringLiteral("layout/page"), 0).toInt();
    if (page > 0 && page < m_pages->count())
        m_navGroup->button(page)->click();
}

void MainWindow::saveLayout() const
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const auto store = [&settings](const QString &key, const QList<int> &values) {
        QVariantList list;
        for (int value : values)
            list << value;
        settings.setValue(key, list);
    };
    // الحجم يُحفظ فقط إن كانت اللوحة ظاهرة، وإلا حُفظ صفر للوحة المخفية
    if (m_sideCard->isVisible())
        store(QStringLiteral("layout/main"), m_mainSplitter->sizes());
    if (m_timelineCard->isVisible())
        store(QStringLiteral("layout/media"), m_mediaSplitter->sizes());
    settings.setValue(QStringLiteral("layout/page"), m_pages->currentIndex());
    if (!m_videoFullscreen)
        settings.setValue(QStringLiteral("layout/geometry"), saveGeometry());
}

bool MainWindow::restoreWindowGeometry()
{
    const QByteArray geometry = QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("layout/geometry")).toByteArray();
    return !geometry.isEmpty() && restoreGeometry(geometry);
}

#include "mainwindow.h"

#include "mainwindow_p.h"
#include "playbackbar.h"
#include "startscreen.h"
#include "videoaudio.h"
#include "videomarkers.h"

#include <QSplitter>
#include <QStackedWidget>

using namespace MainWindowHelpers;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ملامح"));

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_pages = new AnimatedStack(central);
    m_pages->addWidget(createRecognitionPage());
    m_pages->addWidget(createDatabasePage());
    m_matchPage = new MatchPage(&m_engine, &m_database, [this] { return threshold(); }, m_pages);
    m_matchPage->setReady(false);
    m_pages->addWidget(m_matchPage);
    // صفحة تحسين الصور: أي صورة تُفتح أو تُسحب وتُعدَّل والخيارات أمامك
    m_studio = new EnhanceDialog(cv::Mat(), &m_engine, QStringLiteral("تحسين الصور"), QStringLiteral("التعرف على الوجوه فيها"), m_pages);
    m_studio->setEmbedded(true);
    m_studio->onUse = [this](const cv::Mat &image) {
        if (m_analyzing || m_videoRunning) {
            Toast::show(this, QStringLiteral("أوقف التحليل الجاري أولاً"), Toast::Kind::Warning);
            return;
        }
        const QString path = MatchPage::enhancedPath(QStringLiteral("صورة"));
        if (ImageIO::toQImage(image).save(path)) {
            m_navGroup->button(0)->click();
            analyzeImage(path);
        }
    };
    m_pages->addWidget(m_studio);
    connect(m_matchPage, &MatchPage::openPersonRequested, this, &MainWindow::showPerson);
    connect(m_matchPage, &MatchPage::enrollRequested, this, [this](const QString &personId, FaceItem face) {
        if (enrollFace(personId, face))
            Toast::show(this, QStringLiteral("أضيفت الصورة إلى ملف %1").arg(m_database.person(personId).name));
        else
            Toast::show(this, QStringLiteral("تعذرت إضافة الصورة"), Toast::Kind::Warning);
    });
    layout->addWidget(createSidebar());
    layout->addWidget(m_pages, 1);
    setCentralWidget(central);

    setAcceptDrops(true);
    setupShortcuts();

    // قاعدة البيانات سريعة الفتح، أما النماذج فتُحمّل في الخلفية حتى تظهر النافذة فوراً
    m_database.open(FaceDatabase::defaultDirectory(), &m_databaseError);
    FaceEngine *engine = &m_engine;
    connect(&m_loadWatcher, &QFutureWatcher<QString>::finished, this, &MainWindow::onEngineLoaded);
    m_loadWatcher.setFuture(QtConcurrent::run([engine] {
        QString error;
        engine->load(FaceEngine::defaultModelsDir(), &error);
        return error;
    }));
    m_view->setBusy(true, QStringLiteral("جارٍ تحميل نماذج التعرف"));
    statusBar()->showMessage(QStringLiteral("جارٍ تحميل نماذج التعرف…"));
    // رقم الإصدار ظاهر دائماً حتى يُعرف أي نسخة مثبتة عند المستخدم
    auto *version = new QLabel(QStringLiteral("الإصدار \u2066%1\u2069").arg(QApplication::applicationVersion()), this);
    version->setStyleSheet(QStringLiteral("color: %1; padding: 0 8px;").arg(Theme::Muted.name()));
    statusBar()->addPermanentWidget(version);
    m_deviceLabel = new QLabel(this);
    m_deviceLabel->setTextFormat(Qt::PlainText);
    statusBar()->addPermanentWidget(m_deviceLabel);
    m_deviceTimer = new QTimer(this);
    m_deviceTimer->setInterval(1000);
    connect(m_deviceTimer, &QTimer::timeout, this, &MainWindow::updateDeviceLabel);
    connect(&m_deviceWatcher, &QFutureWatcher<QString>::finished, this, [this] {
        const QString error = m_deviceWatcher.result();
        Performance::setLastRunOnGpu(!m_engine.gpuName().isEmpty());
        if (!error.isEmpty()) {
            AppLog::write(QStringLiteral("[تحذير] تعذر نقل التحليل: %1").arg(error));
            Toast::show(this, error, Toast::Kind::Warning);
        } else {
            const QString device = m_engine.gpuName().isEmpty() ? QStringLiteral("المعالج") : m_engine.gpuName();
            AppLog::write(QStringLiteral("انتقل التحليل إلى %1 دون إعادة تشغيل").arg(device));
            Toast::show(this, QStringLiteral("التحليل الآن على %1").arg(device), Toast::Kind::Success);
        }
        updateDeviceLabel();
    });

    connect(&m_database, &FaceDatabase::changed, this, [this] {
        refreshStats();
        refreshMatches();
        m_matchPage->refresh();
    });
    connect(&m_database, &FaceDatabase::personsReset, this, &MainWindow::refreshPersons);
    connect(&m_database, &FaceDatabase::personsReset, this, &MainWindow::refreshWatched);
    connect(&m_database, &FaceDatabase::personUpdated, this, &MainWindow::refreshWatched);
    connect(&m_database, &FaceDatabase::personUpdated, this, [this](const QString &id) {
        m_personModel->refreshPerson(id);
        if (id == selectedPersonId())
            showPersonDetails();
    });
    refreshPersons();
    refreshStats();
    refreshWatched();

    connect(&m_imageWatcher, &QFutureWatcher<ImageAnalysis>::finished, this, &MainWindow::onImageAnalyzed);

    m_videoThread = new QThread(this);
    m_videoWorker = new VideoWorker(&m_engine);
    m_videoWorker->moveToThread(m_videoThread);
    m_videoWorker->setGallery(m_database.gallery());
    // الاسترجاع يفتح القاعدة من جديد بمعرض جديد؛ بدونه يبقى التحليل الحي على أشخاص النسخة القديمة
    connect(&m_database, &FaceDatabase::personsReset, this, [this] {
        m_videoWorker->setGallery(m_database.gallery());
        if (m_multiCamera)
            m_multiCamera->setGallery(m_database.gallery());
    });
    m_videoWorker->setThreshold(threshold());
    // التسجيل يتسلم الإطارات من خيط قراءة الكاميرا مباشرة، فيسجّل كل إطار ولو كان التحليل أبطأ أو متوقفاً
    m_recorder = new CameraRecorder(this);
    m_videoWorker->setRecorder(m_recorder);
    m_maintenance = new Maintenance(&m_database, this);
    connect(m_maintenance, &Maintenance::backupFinished, this, [this](bool ok, const QString &path, const QString &error) {
        if (ok)
            Toast::show(this, QStringLiteral("حُفظت نسخة احتياطية: %1").arg(QFileInfo(path).fileName()));
        else
            emit systemAlert(QStringLiteral("فشلت النسخة الاحتياطية التلقائية"), error);
    });
    m_cameraWatch = new CameraWatch(&m_database, this);
    m_cameraWatch->setWatched(m_watched);
    connect(m_cameraWatch, &CameraWatch::watchAlert, this, [this](const QString &personId, const QString &name, float score, const QImage &crop) {
        FaceItem face;
        face.id = -1;
        for (const FaceItem &item : std::as_const(m_cameraFaces)) {
            if (item.match.personId == personId) {
                face = item;
                break;
            }
        }
        face.match.personId = personId;
        face.match.name = name;
        face.match.score = score;
        face.match.status = MatchStatus::Confirmed;
        showWatchAlert(face, crop);
    });
    connect(m_cameraWatch, &CameraWatch::strangerAlert, this, [this](qint64 visitorId, const QString &reason, const QImage &) {
        showStrangerAlert(visitorId, reason, m_cameraName);
    });
    m_recordTimer = new QTimer(this);
    m_recordTimer->setInterval(1000);
    connect(m_recordTimer, &QTimer::timeout, this, &MainWindow::updateRecordButton);
    connect(m_recorder, &CameraRecorder::fileSaved, this, &MainWindow::addRecentFile);
    connect(m_recorder, &CameraRecorder::failed, this, [this](const QString &error) {
        stopRecording();
        statusBar()->showMessage(error);
        emit systemAlert(QStringLiteral("توقف تسجيل الكاميرا «%1»").arg(m_cameraName), error);
    });
    connect(m_videoThread, &QThread::finished, m_videoWorker, &QObject::deleteLater);
    connect(m_videoWorker, &VideoWorker::started, this, [this](int step, double fps, qint64 durationMs) {
        m_videoClock.start();
        m_videoStep = step;
        m_videoDurationMs = durationMs;
        m_videoFps = fps > 1.0 ? fps : 25.0;
        const bool camera = m_videoSource.startsWith(QStringLiteral("camera:"));
        m_timeline->clear();
        m_laneAvatars.clear();
        m_timeline->setFps(m_videoFps);
        m_timeline->setDuration(camera ? 0 : durationMs);
        m_timeline->setRange(m_videoStartMs, m_videoEndMs);
        m_timeline->setAnalyzedRanges({});
        m_timeline->setMarkers(m_markers);
        m_timeline->setShotChanges(m_shotChanges);
        m_timelineCard->setVisible(!camera && durationMs > 0 && !m_videoFullscreen);
        if (!camera) {
            m_playbackBar->setFps(m_videoFps);
            if (m_playbackBar->isVisible() && durationMs > 0)
                m_playbackBar->setDuration(durationMs);
        }
        if (camera && m_autoRecordSwitch->isChecked())
            startRecording();
    });
    connect(&m_frameWatcher, &QFutureWatcher<ShotFrame>::finished, this, &MainWindow::onVideoFrameLoaded);
    connect(m_videoWorker, &VideoWorker::frameReady, this, &MainWindow::onFrameReady);
    connect(m_videoWorker, &VideoWorker::thumbnailReady, this, [this](qint64 ms, const QImage &thumbnail) {
        if (m_videoRunning && m_timeline->wantsThumbnail(ms))
            m_timeline->addThumbnail(ms, thumbnail);
    });
    m_player = new VideoPlayer(this);
    connect(m_player, &VideoPlayer::frameReady, this, &MainWindow::onPlayerFrame);
    connect(m_player, &VideoPlayer::reachedEnd, this, [this] {
        updatePauseButton();
        checkAutoNext();
    });
    connect(m_videoWorker, &VideoWorker::analyzedRanges, this, [this](const QList<QPair<qint64, qint64>> &ranges) {
        if (!m_timelineCard->isHidden())
            m_timeline->setAnalyzedRanges(ranges);
    });
    connect(m_videoWorker, &VideoWorker::shotChanged, this, [this](qint64 ms) {
        m_shotChanges.append(ms);
        m_timeline->setShotChanges(m_shotChanges);
    });
    setupPlaybackBar();
    connect(m_videoWorker, &VideoWorker::trackUpdated, this, &MainWindow::onTrackUpdated);
    connect(m_videoWorker, &VideoWorker::finished, this, &MainWindow::onVideoFinished);
    connect(m_videoWorker, &VideoWorker::identitiesMerged, this, &MainWindow::onIdentitiesMerged);
    connect(m_videoWorker, &VideoWorker::identitiesForgotten, this, [this](const QList<int> &ids) {
        for (int id : ids)
            m_laneAvatars.remove(id);
        removeFaces(ids);
    });
    connect(m_videoWorker, &VideoWorker::connectionLost, this, [this](int attempt) {
        if (!m_videoRunning)
            return;
        const QString text = QStringLiteral("انقطع الاتصال بالكاميرا «%1» — إعادة المحاولة (%2)").arg(m_cameraName).arg(attempt);
        m_view->setBusy(true, text);
        statusBar()->showMessage(text);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (attempt == 1) {
            m_cameraLostAt = now;
            m_cameraOfflineAlerted = false;
            AppLog::write(QStringLiteral("انقطع الاتصال بالكاميرا «%1»").arg(m_cameraName));
            Toast::show(this, QStringLiteral("انقطع الاتصال بالكاميرا — تُعاد المحاولة تلقائياً"), Toast::Kind::Warning);
        } else if (!m_cameraOfflineAlerted && m_cameraLostAt > 0 && now - m_cameraLostAt >= CameraOfflineAlertMs) {
            m_cameraOfflineAlerted = true;
            emit systemAlert(QStringLiteral("الكاميرا «%1» منقطعة").arg(m_cameraName),
                             QStringLiteral("لا صورة منها منذ %1 دقيقة، وتُعاد المحاولة تلقائياً").arg(CameraOfflineAlertMs / 60000));
        }
    });
    connect(m_videoWorker, &VideoWorker::connectionRestored, this, [this] {
        if (!m_videoRunning)
            return;
        m_view->setBusy(false);
        const qint64 seconds = m_cameraLostAt > 0 ? (QDateTime::currentMSecsSinceEpoch() - m_cameraLostAt) / 1000 : 0;
        AppLog::write(QStringLiteral("عاد الاتصال بالكاميرا «%1» بعد %2 ثانية").arg(m_cameraName).arg(seconds));
        if (m_cameraOfflineAlerted)
            emit systemAlert(QStringLiteral("عادت الكاميرا «%1»").arg(m_cameraName),
                             QStringLiteral("عاد الاتصال بعد انقطاع %1 دقيقة").arg(qMax<qint64>(1, seconds / 60)));
        else
            Toast::show(this, QStringLiteral("عاد الاتصال بالكاميرا «%1»").arg(m_cameraName), Toast::Kind::Success);
        m_cameraLostAt = 0;
        m_cameraOfflineAlerted = false;
    });
    m_videoThread->start();

    loadSettings();
    setBusy(false);
    updateResponsiveLayout();
    setupTray();
    // الأعطال: في السجل دائماً، ورسالة في النافذة إن كانت ظاهرة، وإشعار من ويندوز إن كانت مخفية
    connect(this, &MainWindow::systemAlert, this, [this](const QString &title, const QString &text) {
        AppLog::write(QStringLiteral("[تنبيه] %1 — %2").arg(title, text));
        if (isVisible() && !isMinimized())
            Toast::show(this, QStringLiteral("%1: %2").arg(title, text), Toast::Kind::Warning);
        if (m_tray && !(isVisible() && !isMinimized() && isActiveWindow()))
            m_tray->showMessage(title, text, QSystemTrayIcon::Warning, 15000);
    });
    connect(this, &MainWindow::watchAlert, this, [](const QString &name, const QString &camera) {
        AppLog::write(camera.isEmpty() ? QStringLiteral("تنبيه مراقبة: ظهر %1").arg(name)
                                       : QStringLiteral("تنبيه مراقبة: ظهر %1 أمام «%2»").arg(name, camera));
    });
    m_cameraRetryTimer = new QTimer(this);
    m_cameraRetryTimer->setSingleShot(true);
    m_cameraRetryTimer->setInterval(CameraRestartMs);
    connect(m_cameraRetryTimer, &QTimer::timeout, this, [this] {
        // لم يفتح المستخدم شيئاً آخر منذ توقفها
        if (m_ready && !m_videoRunning && !m_analyzing && m_videoSource.startsWith(QStringLiteral("camera:"))) {
            AppLog::write(QStringLiteral("إعادة تشغيل الكاميرا «%1» بعد انقطاعها").arg(m_cameraName));
            startVideo(m_videoSource);
        }
    });
}

void MainWindow::onEngineLoaded()
{
    m_view->setBusy(false);
    QString error = m_loadWatcher.result();
    if (error.isEmpty())
        error = m_databaseError;
    if (!error.isEmpty()) {
        statusBar()->showMessage(error);
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    m_ready = true;
    m_matchPage->setReady(true);
    setBusy(false);
    const QString device = m_engine.gpuName().isEmpty() ? QStringLiteral("المعالج")
                                                         : QStringLiteral("كرت الشاشة %1").arg(m_engine.gpuName());
    statusBar()->showMessage(QStringLiteral("جاهز — كل المعالجة تتم محلياً على هذا الجهاز (التحليل على %1) · اسحب صورة أو فيديو إلى "
                                            "النافذة أو الصق صورة بـ Ctrl+V")
                                 .arg(device));
    AppLog::write(QStringLiteral("جاهز — التحليل على %1، والقاعدة فيها %2 شخصاً").arg(device).arg(m_database.personCount()));
    m_maintenance->start();
    Performance::setLastRunOnGpu(!m_engine.gpuName().isEmpty());
    updateDeviceLabel();
    m_deviceTimer->start();
    // كرت مفعّل لم يجتز الفحص بعد (كرت جديد أو تعريف محدَّث): يُفحص في الخلفية، والتحليل الآن على المعالج
    // وينتقل إلى الكرت وحده فور اجتيازه
    if (Performance::gpuEnabled() && !FaceEngine::gpuIdentity().isEmpty()
        && !Performance::gpuVerified(FaceEngine::gpuIdentity()))
        verifyGpu(false);
    if (m_previousCrash.isValid()) {
        Toast::show(this,
                    QStringLiteral("لم يُغلق البرنامج إغلاقاً طبيعياً في المرة السابقة (بدأت %1). التفاصيل في «سجل الأحداث» من قائمة "
                                   "الكاميرا ← السجلات والتقارير")
                        .arg(m_previousCrash.toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
                    Toast::Kind::Warning);
    }
    if (!m_pendingFile.isEmpty()) {
        openFile(std::exchange(m_pendingFile, QString()));
    } else if (AppSettings::startCameraOnLaunch() && !Cameras::lastSource().isEmpty()) {
        // بعد إعادة تشغيل الجهاز تعود المراقبة وحدها (والتسجيل إن كان «تسجيل تلقائي» مفعّلاً)
        Cameras::Camera camera;
        if (Cameras::resolve(Cameras::lastSource().mid(7), &camera))
            startVideo(Cameras::lastSource());
    }
}

void MainWindow::setupShortcuts()
{
    const auto add = [this](const QKeySequence &key, auto slot) {
        connect(new QShortcut(key, this), &QShortcut::activated, this, slot);
    };
    add(QKeySequence::Open, [this] {
        if (m_openImageButton->isEnabled())
            openImage();
    });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [this] {
        if (m_openVideoButton->isEnabled())
            openVideo();
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_W), [this] { closeCurrentFile(); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), [this] {
        if (m_cameraButton->isEnabled())
            startLastCamera();
    });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this] {
        if (m_folderScanButton->isEnabled())
            openFolderScan();
    });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), [this] { openArchiveScan(); });
    add(QKeySequence::Save, [this] {
        if (m_saveFacesButton->isEnabled())
            saveFaces();
    });
    add(QKeySequence::Paste, [this] { pasteImage(); });
    add(QKeySequence(Qt::Key_Escape), [this] {
        if (m_videoFullscreen)
            toggleVideoFullscreen(); // Esc يخرج من ملء الشاشة أولاً ولا يوقف التحليل
        else if (m_videoRunning)
            stopVideo();
    });
    add(QKeySequence(Qt::Key_Space), [this] {
        if (m_videoRunning || filePlayerOpen())
            togglePause();
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_R), [this] {
        if (m_recordButton->isVisible())
            toggleRecording();
    });
    add(QKeySequence(Qt::Key_F5), [this] {
        if (!m_currentImagePath.isEmpty() && m_openImageButton->isEnabled())
            analyzeImage(m_currentImagePath);
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_E), [this] { m_enhanceSwitch->toggle(); });
    // التنقل بين صور المجلد والتكبير في صفحة التعرف
    const auto onRecognition = [this] { return m_pages->currentIndex() == 0; };
    // في الفيديو: الفيديو التالي أو السابق في مجلده
    for (const QKeySequence &key : {QKeySequence(Qt::Key_PageDown), QKeySequence(Qt::ALT | Qt::Key_Left)})
        add(key, [this, onRecognition] {
            if (fileVideoActive())
                navigateVideo(1);
            else if (onRecognition())
                navigateImage(1);
        });
    for (const QKeySequence &key : {QKeySequence(Qt::Key_PageUp), QKeySequence(Qt::ALT | Qt::Key_Right)})
        add(key, [this, onRecognition] {
            if (fileVideoActive())
                navigateVideo(-1);
            else if (onRecognition())
                navigateImage(-1);
        });
    add(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this, onRecognition] {
        if (onRecognition())
            m_view->zoomBy(1.25);
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_Minus), [this, onRecognition] {
        if (onRecognition())
            m_view->zoomBy(0.8);
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_0), [this, onRecognition] {
        if (onRecognition())
            m_view->resetZoom();
    });
    // L في الفيديو تقدم 10 ثوانٍ (مع J و K)، وفي الصور إظهار الأسماء
    add(QKeySequence(Qt::Key_L), [this, onRecognition] {
        if (fileVideoActive())
            jumpBy(10000);
        else if (onRecognition())
            m_view->setLabelsVisible(!m_view->labelsVisible());
    });
    setupPlaybackShortcuts();
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] {
        if (m_enhanceImageButton->isEnabled())
            openEnhancer();
    });
    add(QKeySequence::Find, [this] {
        m_navGroup->button(1)->click();
        m_personFilter->setFocus();
        m_personFilter->selectAll();
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_1), [this] { m_navGroup->button(0)->click(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_2), [this] { m_navGroup->button(1)->click(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_3), [this] { m_navGroup->button(2)->click(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_4), [this] { m_navGroup->button(3)->click(); });
}

void MainWindow::loadSettings()
{
    const QSettings settings(settingsPath(), QSettings::IniFormat);
    m_thresholdSlider->setValue(settings.value(QStringLiteral("threshold"), 30).toInt());
    m_thoroughSwitch->setChecked(settings.value(QStringLiteral("thorough"), true).toBool());
    m_enhanceSwitch->setChecked(settings.value(QStringLiteral("enhance"), false).toBool());
    m_frameStepSpin->setValue(settings.value(QStringLiteral("frameStep"), 0).toInt());
    m_lastDirectory = settings.value(QStringLiteral("lastDirectory")).toString();
    m_faceSort->setCurrentIndex(std::clamp(settings.value(QStringLiteral("faceSort"), 0).toInt(), 0, m_faceSort->count() - 1));
    restoreLayout();
}

void MainWindow::saveSettings() const
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("threshold"), m_thresholdSlider->value());
    settings.setValue(QStringLiteral("thorough"), m_thoroughSwitch->isChecked());
    settings.setValue(QStringLiteral("enhance"), m_enhanceSwitch->isChecked());
    settings.setValue(QStringLiteral("frameStep"), m_frameStepSpin->value());
    settings.setValue(QStringLiteral("lastDirectory"), m_lastDirectory);
    settings.setValue(QStringLiteral("faceSort"), m_faceSort->currentIndex());
    saveLayout();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // إغلاق النافذة أثناء تشغيل الكاميرا يبقيها تعمل بجانب الساعة (المراقبة والتسجيل والتنبيهات)
    const bool multi = m_multiCamera && m_multiCamera->isRunning();
    const bool camera = (m_videoRunning && m_videoSource.startsWith(QStringLiteral("camera:"))) || multi;
    if (!m_quitting && camera && m_tray && QSystemTrayIcon::isSystemTrayAvailable() && AppSettings::keepRunningInTray()) {
        event->ignore();
        hide();
        if (!m_trayHintShown) {
            m_trayHintShown = true;
            m_tray->showMessage(QStringLiteral("ملامح يعمل في الخلفية"),
                                QStringLiteral("تستمر مراقبة الكاميرا «%1»%2. انقر الأيقونة بجانب الساعة لإظهار النافذة، "
                                               "أو اختر «خروج» من قائمتها لإغلاق البرنامج.")
                                    .arg(multi ? QStringLiteral("عدة كاميرات") : m_cameraName,
                                         m_recorder->isRecording() || (multi && m_multiCamera->isRecording())
                                             ? QStringLiteral(" وتسجيلها")
                                             : QString()),
                                QSystemTrayIcon::Information, 8000);
        }
        return;
    }
    saveSettings();
    if (m_multiCamera)
        m_multiCamera->stop();
    if (m_tray)
        m_tray->hide();
    QMainWindow::closeEvent(event);
}

bool MainWindow::startHidden() const
{
    return QApplication::arguments().contains(QStringLiteral("--startup")) && m_tray && QSystemTrayIcon::isSystemTrayAvailable();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) {
        event->acceptProposedAction();
        m_startScreen->setDropHighlight(true);
    }
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    m_startScreen->setDropHighlight(false);
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList files;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            files << url.toLocalFile();
    }
    event->acceptProposedAction();
    m_startScreen->setDropHighlight(false);
    if (!m_ready || m_videoRunning || m_analyzing)
        return;

    // في صفحة قاعدة البيانات تُضاف الصور المسحوبة إلى الشخص المحدد
    QStringList images;
    for (const QString &file : files) {
        if (isImageFile(file))
            images << file;
    }
    const QString personId = selectedPersonId();
    if (m_pages->currentIndex() == 1 && !personId.isEmpty() && !images.isEmpty()) {
        addPhotoFiles(personId, images);
        return;
    }
    if (files.isEmpty()) {
        if (event->mimeData()->hasImage())
            pasteImage();
        return;
    }
    if (m_pages->currentIndex() == 3 && !images.isEmpty()) {
        const cv::Mat image = ImageIO::loadImage(images.first());
        if (!image.empty())
            m_studio->setImage(image, QFileInfo(images.first()).completeBaseName());
        return;
    }
    if (m_pages->currentIndex() == 2 && !images.isEmpty()) {
        m_matchPage->openFile(images.first());
        if (images.size() > 1)
            m_matchPage->openFile(images.at(1));
        return;
    }
    m_navGroup->button(0)->click();
    openFile(files.first());
}

void MainWindow::pasteImage()
{
    if (m_pages->currentIndex() == 3) {
        m_studio->pasteImage();
        return;
    }
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        const QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile()) {
            if (m_pages->currentIndex() == 2)
                m_matchPage->openFile(urls.first().toLocalFile());
            else
                openFile(urls.first().toLocalFile());
        }
        return;
    }
    if (!m_openImageButton->isEnabled())
        return;
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("malamih-clipboard.png"));
    if (!image.save(path)) {
        Toast::show(this, QStringLiteral("تعذر قراءة الصورة من الحافظة"), Toast::Kind::Warning);
        return;
    }
    if (m_pages->currentIndex() == 2) {
        m_matchPage->openFile(path);
        return;
    }
    m_navGroup->button(0)->click();
    analyzeImage(path);
}

MainWindow::~MainWindow()
{
    m_loadWatcher.waitForFinished();
    m_deviceWatcher.waitForFinished();
    m_videoWorker->requestStop();
    m_videoThread->quit();
    m_videoThread->wait();
    m_recorder->disconnect(this);
    m_recorder->stop();
    m_imageWatcher.waitForFinished();
    m_frameWatcher.waitForFinished();
    m_previewWatcher.waitForFinished();
    // الصفحات والحوارات تنتظر أعمالها الخلفية في هدمها، وتلك الأعمال تستخدم m_engine و m_database؛
    // تُهدم هنا قبل الأعضاء وإلا هدمها QWidget بعد زوالهما
    const QList<QWidget *> children = findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *child : children)
        child->disconnect(this);
    qDeleteAll(children);
}

QWidget *MainWindow::createSidebar()
{
    auto *sidebar = card(this, QStringLiteral("sidebar"));
    m_sidebar = sidebar;
    sidebar->setFixedWidth(240);
    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(18, 22, 18, 18);
    layout->setSpacing(6);

    auto *brand = new QHBoxLayout;
    brand->setSpacing(12);
    brand->addWidget(new LogoMark(sidebar));
    auto *brandWidget = new QWidget(sidebar);
    auto *brandText = new QVBoxLayout(brandWidget);
    brandText->setContentsMargins(0, 0, 0, 0);
    brandText->setSpacing(0);
    brandText->addWidget(label(QStringLiteral("ملامح"), QStringLiteral("sectionTitle"), brandWidget));
    brandText->addWidget(label(QStringLiteral("التعرف على الوجوه"), QStringLiteral("muted"), brandWidget));
    brand->addWidget(brandWidget, 1);
    layout->addLayout(brand);
    layout->addSpacing(26);

    auto *recognitionNav = new FancyButton(Icons::Name::Scan, QStringLiteral("التعرف"), FancyButton::Kind::Nav, sidebar);
    auto *databaseNav = new FancyButton(Icons::Name::Users, QStringLiteral("قاعدة البيانات"), FancyButton::Kind::Nav, sidebar);
    auto *matchNav = new FancyButton(Icons::Name::Compare, QStringLiteral("المطابقة"), FancyButton::Kind::Nav, sidebar);
    auto *studioNav = new FancyButton(Icons::Name::Sparkle, QStringLiteral("تحسين الصور"), FancyButton::Kind::Nav, sidebar);
    studioNav->setToolTip(QStringLiteral("افتح أي صورة وعدّلها: إزالة التشويش والضبابية، الحدة، تكبير الدقة، الإضاءة، الزاوية والمنظور (Ctrl+4)"));
    m_navButtons = {recognitionNav, databaseNav, matchNav, studioNav};
    auto *group = new QButtonGroup(sidebar);
    m_navGroup = group;
    group->addButton(recognitionNav, 0);
    group->addButton(databaseNav, 1);
    group->addButton(matchNav, 2);
    group->addButton(studioNav, 3);
    recognitionNav->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) { m_pages->slideTo(id); });
    layout->addWidget(recognitionNav);
    layout->addWidget(databaseNav);
    layout->addWidget(matchNav);
    layout->addWidget(studioNav);
    layout->addStretch();

    auto *stats = card(sidebar, QStringLiteral("statCard"));
    auto *statsLayout = new QGridLayout(stats);
    statsLayout->setContentsMargins(14, 12, 14, 12);
    statsLayout->setVerticalSpacing(0);
    m_personCountLabel = label(QStringLiteral("0"), QStringLiteral("statValue"), stats);
    m_photoCountLabel = label(QStringLiteral("0"), QStringLiteral("statValue"), stats);
    statsLayout->addWidget(m_personCountLabel, 0, 0);
    statsLayout->addWidget(m_photoCountLabel, 0, 1);
    statsLayout->addWidget(label(QStringLiteral("شخص"), QStringLiteral("muted"), stats), 1, 0);
    statsLayout->addWidget(label(QStringLiteral("صورة وجه"), QStringLiteral("muted"), stats), 1, 1);
    layout->addWidget(stats);
    layout->addSpacing(10);

    auto *offline = new QWidget(sidebar);
    auto *offlineLayout = new QHBoxLayout(offline);
    offlineLayout->setContentsMargins(0, 0, 0, 0);
    auto *lock = new QLabel(offline);
    lock->setPixmap(Icons::icon(Icons::Name::Lock, Theme::Success, 16).pixmap(16, 16));
    offlineLayout->addWidget(lock);
    auto *offlineText = label(QStringLiteral("يعمل محلياً بالكامل، دون إنترنت"), QStringLiteral("muted"), offline);
    offlineText->setWordWrap(true);
    offlineLayout->addWidget(offlineText, 1);
    layout->addWidget(offline);
    m_sidebarExtras = {brandWidget, stats, offline};
    return sidebar;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateResponsiveLayout();
}

// يكيّف الواجهة مع عرض النافذة حتى لا تختفي أجزاء منها على شاشات اللابتوب الصغيرة
void MainWindow::updateResponsiveLayout()
{
    if (!m_sidebar)
        return;
    const int w = width();
    // 0 كامل، 1 شريط جانبي مصغر، 2 أزرار مصغرة أيضاً؛ والشاشات القصيرة تُخفي النصوص الثانوية
    const int mode = w >= 1480 ? 0 : (w >= 1120 ? 1 : 2);
    const bool shortScreen = height() < 820;
    const int key = mode * 2 + (shortScreen ? 1 : 0);
    if (key == m_layoutMode)
        return;
    m_layoutMode = key;
    const bool compactSidebar = mode >= 1;
    m_sidebar->setFixedWidth(compactSidebar ? 76 : 240);
    m_sidebar->layout()->setContentsMargins(compactSidebar ? QMargins(14, 18, 14, 14) : QMargins(18, 22, 18, 18));
    for (QWidget *extra : std::as_const(m_sidebarExtras))
        extra->setVisible(!compactSidebar);
    for (FancyButton *button : std::as_const(m_navButtons))
        button->setCompact(compactSidebar);
    for (FancyButton *button : std::as_const(m_headerButtons))
        button->setCompact(mode >= 2);
    for (QLabel *subtitle : std::as_const(m_subtitles))
        subtitle->setVisible(mode == 0 && !shortScreen);
    if (m_photosHint)
        m_photosHint->setVisible(mode == 0);
    if (m_matchPage)
        m_matchPage->setCompact(mode >= 2 || shortScreen);
}

QWidget *MainWindow::createRecognitionPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 12);
    layout->setSpacing(12);

    // العنوان والأزرار
    m_openImageButton = new FancyButton(Icons::Name::Image, QStringLiteral("فتح صورة"), FancyButton::Kind::Primary, page);
    m_openVideoButton = new FancyButton(Icons::Name::Video, QStringLiteral("فيديو"), FancyButton::Kind::Secondary, page);
    m_cameraButton = new FancyButton(Icons::Name::Camera, QStringLiteral("الكاميرا"), FancyButton::Kind::Secondary, page);
    m_pauseButton = new FancyButton(Icons::Name::Pause, QStringLiteral("إيقاف مؤقت"), FancyButton::Kind::Secondary, page);
    m_stopButton = new FancyButton(Icons::Name::Stop, QStringLiteral("إيقاف"), FancyButton::Kind::Danger, page);
    m_openImageButton->setToolTip(QStringLiteral("فتح صورة (Ctrl+O) — أو اسحبها إلى النافذة، أو الصقها بـ Ctrl+V"));
    m_openVideoButton->setToolTip(QStringLiteral("فتح فيديو (Ctrl+Shift+O)"));
    m_cameraButton->setToolTip(QStringLiteral("اختيار كاميرا وتشغيلها: كاميرا الجهاز، كاميرا USB أو تناظرية عبر محوّل، "
                                              "أو كاميرا شبكة وجهاز تسجيل\nCtrl+Shift+C يشغّل آخر كاميرا"));
    m_pauseButton->setToolTip(QStringLiteral("إيقاف مؤقت / استئناف (مسافة)"));
    m_stopButton->setToolTip(QStringLiteral("إيقاف التحليل (Esc)"));
    m_recordButton = new FancyButton(Icons::Name::Record, QStringLiteral("تسجيل"), FancyButton::Kind::Secondary, page);
    m_recordButton->setCheckable(true);
    m_recordButton->setToolTip(QStringLiteral("تسجيل بث الكاميرا إلى ملف فيديو MP4 (Ctrl+R)، بكل إطاراته وبعد معالجات الفيديو،\n"
                                              "ويستمر التسجيل أثناء الإيقاف المؤقت للتحليل. يُفتح التسجيل لاحقاً ويُحلَّل كأي فيديو"));
    m_recordButton->setVisible(false);
    connect(m_recordButton, &QAbstractButton::clicked, this, &MainWindow::toggleRecording);
    m_saveFacesButton = new FancyButton(Icons::Name::Save, QStringLiteral("حفظ الوجوه"), FancyButton::Kind::Secondary, page);
    m_saveFacesButton->setToolTip(QStringLiteral("حفظ صور الوجوه المكتشفة مع تقرير مرئي (HTML) وتقرير Excel (CSV) — Ctrl+S"));
    connect(m_openImageButton, &QAbstractButton::clicked, this, &MainWindow::openImage);
    connect(m_openVideoButton, &QAbstractButton::clicked, this, &MainWindow::openVideo);
    connect(m_cameraButton, &QAbstractButton::clicked, this, &MainWindow::openCamera);
    connect(m_pauseButton, &QAbstractButton::clicked, this, &MainWindow::togglePause);
    connect(m_stopButton, &QAbstractButton::clicked, this, &MainWindow::stopVideo);
    connect(m_saveFacesButton, &QAbstractButton::clicked, this, &MainWindow::saveFaces);
    m_enhanceImageButton = new FancyButton(Icons::Name::Sparkle, QStringLiteral("تحسين الصورة"), FancyButton::Kind::Secondary, page);
    m_enhanceImageButton->setToolTip(QStringLiteral("تحسين الصورة الحالية أو إطار الفيديو الحالي يدوياً:\n"
                                                    "إزالة التشويش والضبابية، الحدة، تكبير الدقة، الإضاءة، التدوير وتصحيح المنظور (Ctrl+Shift+E)"));
    connect(m_enhanceImageButton, &QAbstractButton::clicked, this, &MainWindow::openEnhancer);
    m_folderScanButton = new FancyButton(Icons::Name::Folder, QStringLiteral("مجلد صور"), FancyButton::Kind::Secondary, page);
    m_folderScanButton->setToolTip(QStringLiteral("البحث عن الأشخاص في كل صور مجلد (Ctrl+Shift+F)"));
    connect(m_folderScanButton, &QAbstractButton::clicked, this, &MainWindow::openFolderScan);
    m_recentButton = new FancyButton(Icons::Name::Clock, QStringLiteral("الأخيرة"), FancyButton::Kind::Secondary, page);
    m_recentButton->setToolTip(QStringLiteral("آخر الصور والفيديوهات التي فُتحت"));
    connect(m_recentButton, &QAbstractButton::clicked, this, &MainWindow::showRecentMenu);
    // بعد فتح ملف تجتمع أزرار الفتح في زر واحد، فتبقى أدوات الحالة الحالية وحدها ظاهرة
    m_openMenuButton = new FancyButton(Icons::Name::Folder, QStringLiteral("فتح"), FancyButton::Kind::Secondary, page);
    m_openMenuButton->setToolTip(QStringLiteral("فتح صورة أو فيديو أو كاميرا أو مجلد صور، أو أحد الملفات الأخيرة، "
                                                "أو العودة إلى شاشة البداية (Ctrl+W)"));
    m_openMenuButton->setVisible(false);
    connect(m_openMenuButton, &QAbstractButton::clicked, this, &MainWindow::showOpenMenu);
    m_headerButtons = {m_openImageButton, m_openVideoButton, m_recentButton, m_cameraButton, m_folderScanButton, m_openMenuButton,
                       m_pauseButton, m_recordButton, m_stopButton, m_enhanceImageButton, m_saveFacesButton};

    m_recognitionHeader = new QWidget(page);
    auto *header = new QHBoxLayout(m_recognitionHeader);
    header->setContentsMargins(0, 0, 0, 0);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(label(QStringLiteral("التعرف على الوجوه"), QStringLiteral("title"), page));
    auto *subtitle = label(QStringLiteral("افتح صورة أو فيديو، أو شغّل الكاميرا، ليتم كشف الوجوه ومطابقتها مع قاعدة البيانات"),
                           QStringLiteral("subtitle"), page);
    subtitle->setWordWrap(true);
    subtitle->setMinimumWidth(120);
    m_subtitles << subtitle;
    titles->addWidget(subtitle);
    header->addLayout(titles, 1);
    header->setSpacing(8);
    for (FancyButton *button : std::as_const(m_headerButtons))
        header->addWidget(button);
    layout->addWidget(m_recognitionHeader);

    // الإعدادات: مجموعات صغيرة تلتف إلى سطر جديد عندما تضيق النافذة
    // السطر الأول إعدادات الحالة الحالية، و«إعدادات التحليل» النادرة في لوحة تُطوى تحته
    auto *settingsCard = card(page);
    m_settingsCard = settingsCard;
    auto *settingsColumn = new QVBoxLayout(settingsCard);
    settingsColumn->setContentsMargins(16, 6, 16, 6);
    settingsColumn->setSpacing(6);
    auto *settings = new QWidget(settingsCard);
    auto *settingsLayout = new FlowLayout(settings, 18, 8);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsColumn->addWidget(settings);
    m_advancedPanel = new QWidget(settingsCard);
    auto *advancedLayout = new FlowLayout(m_advancedPanel, 18, 8);
    advancedLayout->setContentsMargins(0, 6, 0, 2);
    m_advancedPanel->setObjectName(QStringLiteral("advancedPanel"));
    m_advancedPanel->setStyleSheet(QStringLiteral("#advancedPanel { border-top: 1px solid %1; }").arg(Theme::Border.name()));
    settingsColumn->addWidget(m_advancedPanel);
    auto group = [settings](std::initializer_list<QWidget *> widgets) {
        auto *box = new QWidget(settings);
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        for (QWidget *w : widgets)
            row->addWidget(w);
        return box;
    };

    m_thresholdSlider = new QSlider(Qt::Horizontal, settings);
    m_thresholdSlider->setRange(15, 80);
    m_thresholdSlider->setValue(30);
    m_thresholdSlider->setFixedWidth(150);
    m_thresholdSlider->setCursor(Qt::PointingHandCursor);
    m_thresholdSlider->setToolTip(QStringLiteral("أعلى = أخطاء أقل لكن قد لا يُتعرف على الصور الرديئة جداً.\nأقل = يتعرف أكثر مع احتمال خطأ أكبر."));
    m_thresholdValue = label(QStringLiteral("30%"), QStringLiteral("badge"), settings);
    m_thresholdWarning = new QLabel(settings);
    m_thresholdWarning->setPixmap(Icons::icon(Icons::Name::Alert, Theme::Warning, 18).pixmap(18, 18));
    m_thresholdWarning->setToolTip(QStringLiteral("الحد أقل من 30%: قد يُنسب وجه شخص غير مسجل إلى شخص معروف يشبهه.\n"
                                                  "يُنصح بـ 30% إلى 40%، خاصة مع الفيديو منخفض الجودة."));
    m_thresholdWarning->setVisible(false);
    connect(m_thresholdSlider, &QSlider::valueChanged, this, [this](int value) {
        m_thresholdValue->setText(QStringLiteral("%1%").arg(value));
        m_thresholdWarning->setVisible(value < 30);
        // التحذير يبقى ظاهراً على زر اللوحة وإن كانت مطوية
        if (m_advancedButton)
            m_advancedButton->setIconName(value < 30 ? Icons::Name::Alert : Icons::Name::Sliders);
        m_videoWorker->setThreshold(threshold());
        refreshMatches();
        if (m_matchPage)
            m_matchPage->refresh();
    });

    m_thoroughSwitch = new ToggleSwitch(QStringLiteral("تحليل معمّق للصور"), settings);
    m_thoroughSwitch->setToolTip(QStringLiteral("كشف متعدد المقاييس للصور: أدق مع الوجوه الصغيرة، وأبطأ"));
    m_thoroughSwitch->setChecked(true);

    m_enhanceSwitch = new ToggleSwitch(QStringLiteral("تحسين الصورة وتوضيحها"), settings);
    m_enhanceSwitch->setToolTip(QStringLiteral("للصور والفيديو الصغير أو المظلم أو المشوش (Ctrl+E):\n"
                                               "تكبير، إزالة التشويش، موازنة الإضاءة والتباين، ثم زيادة الحدة.\n"
                                               "يُطبَّق على الفيديو فوراً أثناء التحليل، ويجعله أبطأ."));
    connect(m_enhanceSwitch, &QAbstractButton::toggled, this, [this](bool checked) {
        m_videoWorker->setEnhance(checked);
        if (!m_videoMode && !m_currentImagePath.isEmpty() && m_openImageButton->isEnabled())
            analyzeImage(m_currentImagePath);
    });

    m_frameStepSpin = new QSpinBox(settings);
    m_frameStepSpin->setRange(0, 60);
    m_frameStepSpin->setValue(0);
    m_frameStepSpin->setSpecialValueText(QStringLiteral("تلقائي"));
    m_frameStepSpin->setToolTip(QStringLiteral("تحليل إطار من كل كم إطار في الفيديو.\n"
                                               "تلقائي = نحو 10 إطارات لكل ثانية من الفيديو (سريع ودقيق).\n"
                                               "1 = تحليل كل إطار (الأبطأ). قيمة أكبر = أسرع."));
    connect(m_frameStepSpin, &QSpinBox::valueChanged, this, [this](int value) { m_videoWorker->setFrameStep(value); });

    m_performanceCombo = new QComboBox(settings);
    m_performanceCombo->addItems({QStringLiteral("خفيف"), QStringLiteral("متوازن"), QStringLiteral("أقصى سرعة")});
    m_performanceCombo->setCurrentIndex(int(Performance::saved()));
    auto describePerformance = [this] {
        const auto level = Performance::Level(m_performanceCombo->currentIndex());
        m_performanceCombo->setToolTip(QStringLiteral("استهلاك المعالج أثناء التحليل\n") + Performance::describe(level)
                                       + QStringLiteral("\nإن كان الجهاز يعلق أثناء تحليل الفيديو اختر «خفيف»."));
    };
    describePerformance();
    connect(m_performanceCombo, &QComboBox::currentIndexChanged, this, [this, describePerformance](int index) {
        const auto level = Performance::Level(index);
        Performance::save(level);
        Performance::apply(level);
        describePerformance();
        // نماذج التعرف تأخذ عدد الأنوية عند تحميلها، أما الأولوية فتتغير فوراً
        statusBar()->showMessage(Performance::describe(level)
                                     + QStringLiteral(" · الأولوية تغيّرت الآن، وعدد الأنوية يُطبَّق كاملاً عند إعادة تشغيل البرنامج"),
                                 8000);
    });

    m_regionButton = new FancyButton(Icons::Name::Region, QStringLiteral("تحديد منطقة"), FancyButton::Kind::Secondary, settings);
    m_regionButton->setCheckable(true);
    m_regionButton->setToolTip(QStringLiteral("ارسم مستطيلاً على الإطار ليُبحث عن الوجوه داخله فقط (أسرع ويتجاهل ما حوله)"));
    m_clearRegionButton = new FancyButton(Icons::Name::Stop, QStringLiteral("الإطار كله"), FancyButton::Kind::Secondary, settings);
    m_clearRegionButton->setToolTip(QStringLiteral("إلغاء المنطقة والبحث في الإطار كله"));
    m_reanalyzeButton = new FancyButton(Icons::Name::Film, QStringLiteral("إعادة التحليل"), FancyButton::Kind::Secondary, settings);
    m_reanalyzeButton->setToolTip(QStringLiteral("إعادة تحليل آخر فيديو بالإعدادات والجزء والمنطقة الحالية"));
    connect(m_regionButton, &QAbstractButton::toggled, this, [this](bool on) {
        m_view->setSelectingRegion(on);
        if (on)
            statusBar()->showMessage(QStringLiteral("اسحب على الإطار لتحديد منطقة البحث عن الوجوه"));
    });
    connect(m_clearRegionButton, &QAbstractButton::clicked, this, [this] {
        m_view->setRegion(QRectF());
        m_videoWorker->setRegion(QRectF());
        m_regionButton->setChecked(false);
        updateVideoOptions();
    });
    connect(m_reanalyzeButton, &QAbstractButton::clicked, this, &MainWindow::reanalyzeVideo);
    m_filtersButton = new FancyButton(Icons::Name::Sliders, QStringLiteral("معالجات"), FancyButton::Kind::Secondary, settings);
    connect(m_filtersButton, &QAbstractButton::clicked, this, &MainWindow::openVideoFilters);
    updateFiltersButton();

    settingsLayout->addWidget(m_thoroughSwitch);
    settingsLayout->addWidget(m_enhanceSwitch);
    QWidget *frameStepGroup = group({new QLabel(QStringLiteral("إطار من كل"), settings), m_frameStepSpin});
    QWidget *regionGroup = group({m_filtersButton, m_regionButton, m_clearRegionButton, m_reanalyzeButton});
    m_frameStepGroup = frameStepGroup;
    m_regionGroup = regionGroup;
    auto *recordFolderButton = new FancyButton(Icons::Name::Folder, QStringLiteral("التسجيلات"), FancyButton::Kind::Secondary, settings);
    recordFolderButton->setToolTip(QStringLiteral("مجلد تسجيلات الكاميرا، والتسجيل عند الحركة فقط، ورسم الوجوه، وحذف القديم تلقائياً"));
    connect(recordFolderButton, &QAbstractButton::clicked, this, &MainWindow::showRecordFolderMenu);
    m_autoRecordSwitch = new ToggleSwitch(QStringLiteral("تسجيل تلقائي"), settings);
    m_autoRecordSwitch->setToolTip(QStringLiteral("يبدأ تسجيل الكاميرا وحده كلما شُغّلت"));
    m_autoRecordSwitch->setChecked(CameraRecorder::autoRecord());
    connect(m_autoRecordSwitch, &QAbstractButton::toggled, this, [](bool on) { CameraRecorder::setAutoRecord(on); });
    m_recordGroup = group({new QLabel(QStringLiteral("تسجيل الكاميرا"), settings), m_autoRecordSwitch, recordFolderButton});
    m_videoSettings = {frameStepGroup, regionGroup};
    m_gpuSwitch = new ToggleSwitch(QStringLiteral("كرت الشاشة"), settings);
    const QString gpu = FaceEngine::availableGpu();
    m_gpuSwitch->setChecked(Performance::gpuEnabled() && !gpu.isEmpty());
    m_gpuSwitch->setEnabled(!gpu.isEmpty());
    m_gpuSwitch->setToolTip(gpu.isEmpty() ? QStringLiteral("لا يوجد كرت شاشة يدعم DirectX 12 في هذا الجهاز؛ التحليل على المعالج")
                                          : QStringLiteral("تحليل الوجوه على %1 بدل المعالج: أسرع عادةً بعدة مرات، ويبقى المعالج "
                                                           "متاحاً لبقية البرامج. يُفحص الكرت أولاً، ثم ينتقل التحليل إليه فوراً")
                                                .arg(gpu));
    connect(m_gpuSwitch, &QAbstractButton::toggled, this, [this](bool on) {
        Performance::setGpuEnabled(on);
        if (on && !Performance::gpuVerified(FaceEngine::gpuIdentity()))
            verifyGpu(false);
        else
            applyEngineDevice();
    });
    m_gpuCheckButton = new FancyButton(Icons::Name::Scan, QStringLiteral("فحص"), FancyButton::Kind::Secondary, settings);
    m_gpuCheckButton->setToolTip(QStringLiteral("قياس سرعة التحليل على كرت الشاشة مقابل المعالج، والتحقق أن نتائجه مطابقة للمعالج"));
    connect(m_gpuCheckButton, &QAbstractButton::clicked, this, [this] { verifyGpu(true); });
    auto *gpuCheck = m_gpuCheckButton;
    auto *gpuSwitch = m_gpuSwitch;
    QWidget *resourcesGroup = group({new QLabel(QStringLiteral("استهلاك الموارد"), settings), m_performanceCombo, gpuSwitch, gpuCheck});
    settingsLayout->addWidget(regionGroup);
    settingsLayout->addWidget(m_recordGroup);
    m_recordGroup->setVisible(false);
    m_advancedButton = new FancyButton(Icons::Name::Sliders, QStringLiteral("إعدادات التحليل"), FancyButton::Kind::Secondary, settings);
    m_advancedButton->setCheckable(true);
    m_advancedButton->setToolTip(QStringLiteral("الإعدادات الأقل استخداماً: حد التعرف، تحليل إطار من كل كم إطار، استهلاك المعالج، وكرت الشاشة"));
    connect(m_advancedButton, &QAbstractButton::toggled, this, &MainWindow::setAdvancedSettingsVisible);
    settingsLayout->addWidget(m_advancedButton);
    // المجموعات تنتقل إلى اللوحة المطوية؛ أُنشئت بأب settings فيُعاد تعيين أبيها
    QWidget *thresholdGroup = group({new QLabel(QStringLiteral("حد التعرف"), settings), m_thresholdSlider, m_thresholdValue, m_thresholdWarning});
    for (QWidget *advanced : {thresholdGroup, frameStepGroup, resourcesGroup}) {
        advanced->setParent(m_advancedPanel);
        advancedLayout->addWidget(advanced);
    }
    m_advancedPanel->setVisible(false);
    layout->addWidget(settingsCard);
    layout->addWidget(createStatusStrip(page));

    m_progress = new QProgressBar(page);
    m_progress->setObjectName(QStringLiteral("thinProgress"));
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    auto *progressSlot = new QWidget(page);
    progressSlot->setFixedHeight(4);
    auto *progressLayout = new QVBoxLayout(progressSlot);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->addWidget(m_progress);
    m_progress->setVisible(false);
    layout->addWidget(progressSlot);

    // شريط تنبيه المراقبة: يظهر عندما يُتعرَّف على شخص من قائمة المراقبة
    m_alertBar = new QFrame(page);
    m_alertBar->setObjectName(QStringLiteral("alertBar"));
    m_alertBar->setStyleSheet(QStringLiteral("#alertBar { background: #0f2a55; border: 2px solid %1; border-radius: 12px; }")
                                  .arg(Theme::Accent.name()));
    auto *alertLayout = new QHBoxLayout(m_alertBar);
    alertLayout->setContentsMargins(14, 8, 14, 8);
    alertLayout->setSpacing(12);
    auto *alertIcon = new QLabel(m_alertBar);
    alertIcon->setPixmap(Icons::icon(Icons::Name::Alert, Theme::Accent2, 26).pixmap(26, 26));
    m_alertAvatar = new QLabel(m_alertBar);
    m_alertAvatar->setFixedSize(44, 44);
    m_alertText = new QLabel(m_alertBar);
    m_alertText->setTextFormat(Qt::RichText);
    m_alertText->setWordWrap(true);
    auto *alertShow = new FancyButton(Icons::Name::Search, QStringLiteral("عرض"), FancyButton::Kind::Primary, m_alertBar);
    alertShow->setToolTip(QStringLiteral("تحديد الوجه وعرضه"));
    auto *alertClose = new FancyButton(Icons::Name::Stop, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, m_alertBar);
    connect(alertShow, &QAbstractButton::clicked, this, [this] {
        const int row = m_faceRows.value(m_alertFaceId, -1);
        if (row < 0 || row >= m_faces.size())
            return;
        selectFace(m_alertFaceId);
        if (!m_videoMode)
            m_view->zoomToFace(m_alertFaceId);
        else if (m_faces.at(row).bestMs >= 0)
            loadVideoFrame(m_faces.at(row).bestMs);
    });
    auto *alertLog = new FancyButton(Icons::Name::History, QStringLiteral("السجل"), FancyButton::Kind::Secondary, m_alertBar);
    alertLog->setToolTip(QStringLiteral("كل مرات ظهور الأشخاص المراقَبين"));
    connect(alertLog, &QAbstractButton::clicked, this, &MainWindow::showAlertLog);
    connect(alertClose, &QAbstractButton::clicked, m_alertBar, &QWidget::hide);
    alertLayout->addWidget(alertIcon);
    alertLayout->addWidget(m_alertAvatar);
    alertLayout->addWidget(m_alertText, 1);
    alertLayout->addWidget(alertShow);
    alertLayout->addWidget(alertLog);
    alertLayout->addWidget(alertClose);
    m_alertBar->setVisible(false);
    layout->addWidget(m_alertBar);
    // وميض الإطار بضع مرات لجذب الانتباه
    m_alertPulse = new QTimer(this);
    m_alertPulse->setInterval(350);
    connect(m_alertPulse, &QTimer::timeout, this, [this] {
        const int step = m_alertPulse->property("step").toInt() + 1;
        m_alertPulse->setProperty("step", step);
        const bool bright = step % 2 == 1;
        m_alertBar->setStyleSheet(QStringLiteral("#alertBar { background: %1; border: 2px solid %2; border-radius: 12px; }")
                                      .arg(bright ? QStringLiteral("#123a7a") : QStringLiteral("#0f2a55"),
                                           bright ? Theme::Accent2.name() : Theme::Accent.name()));
        if (step >= 8)
            m_alertPulse->stop();
    });

    // العرض والقائمة الجانبية
    auto *viewCard = card(page, QStringLiteral("viewCard"));
    auto *viewCardLayout = new QVBoxLayout(viewCard);
    viewCardLayout->setContentsMargins(2, 2, 2, 2);
    m_viewStack = new QStackedWidget(viewCard);
    viewCardLayout->addWidget(m_viewStack);
    m_startScreen = new StartScreen(m_viewStack);
    m_viewStack->addWidget(m_startScreen);
    connect(m_startScreen, &StartScreen::openImageRequested, this, &MainWindow::openImage);
    connect(m_startScreen, &StartScreen::openVideoRequested, this, &MainWindow::openVideo);
    connect(m_startScreen, &StartScreen::openCameraRequested, this, &MainWindow::openCamera);
    connect(m_startScreen, &StartScreen::openFolderRequested, this, &MainWindow::openFolderScan);
    connect(m_startScreen, &StartScreen::openArchiveRequested, this, &MainWindow::openArchiveScan);
    connect(m_startScreen, &StartScreen::shortcutsRequested, this, &MainWindow::showShortcuts);
    connect(m_startScreen, &StartScreen::fileRequested, this, [this](const QString &file) {
        m_lastDirectory = QFileInfo(file).absolutePath();
        openFile(file);
    });
    connect(m_startScreen, &StartScreen::cameraRequested, this, &MainWindow::startVideo);
    auto *viewPage = new QWidget(m_viewStack);
    auto *viewLayout = new QVBoxLayout(viewPage);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    viewLayout->setSpacing(0);
    m_viewStack->addWidget(viewPage);
    m_view = new ImageView(viewPage);
    m_view->setMinimumSize(280, 180);
    viewLayout->addWidget(m_view, 1);
    m_playbackBar = new PlaybackBar(viewPage);
    m_playbackBar->setVisible(false);
    viewLayout->addWidget(m_playbackBar);
    m_rangeStart = m_playbackBar->rangeStartEdit();
    m_rangeEnd = m_playbackBar->rangeEndEdit();
    connect(m_view, &ImageView::faceClicked, this, &MainWindow::selectFace);
    connect(m_view, &ImageView::faceDoubleClicked, this, [this](int id) {
        if (m_videoMode)
            openShotViewer(id);
        else
            m_view->zoomToFace(id);
    });
    connect(m_view, &ImageView::navigateRequested, this, &MainWindow::navigateImage);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, &MainWindow::showViewMenu);
    m_view->setToolTip(QString());
    connect(m_view, &ImageView::regionSelected, this, [this](const QRectF &region) {
        m_videoWorker->setRegion(region);
        m_regionButton->setChecked(false);
        updateVideoOptions();
        statusBar()->showMessage(region.isEmpty() ? QStringLiteral("يُبحث عن الوجوه في الإطار كله")
                                                  : QStringLiteral("يُبحث عن الوجوه داخل المنطقة المحددة فقط"));
    });

    // محتوى اللوحة يُمرَّر عمودياً على الشاشات القصيرة جداً بدل أن تتداخل أجزاؤه
    auto *side = card(page);
    m_sideCard = side;
    side->setMinimumWidth(240);
    auto *sideOuter = new QVBoxLayout(side);
    sideOuter->setContentsMargins(0, 0, 0, 0);
    auto *sideScroll = new QScrollArea(side);
    sideScroll->setWidgetResizable(true);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    auto *sideContent = new QWidget(sideScroll);
    sideScroll->setWidget(sideContent);
    sideOuter->addWidget(sideScroll);
    auto *sideLayout = new QVBoxLayout(sideContent);
    sideLayout->setContentsMargins(12, 14, 12, 12);
    sideLayout->setSpacing(8);

    setupFacePanel(side, sideLayout);

    // الخط الزمني أسفل الفيديو بأسلوب برامج المونتاج
    m_timelineCard = card(page);
    auto *timelineLayout = new QVBoxLayout(m_timelineCard);
    timelineLayout->setContentsMargins(10, 6, 10, 8);
    timelineLayout->setSpacing(4);
    auto *timelineHeader = new QHBoxLayout;
    timelineHeader->setSpacing(6);
    timelineHeader->addWidget(label(QStringLiteral("الخط الزمني"), QStringLiteral("sectionTitle"), m_timelineCard));
    auto *timelineHint = label(QStringLiteral("انقر لعرض أي لحظة · انقر اسم شخص للانتقال إلى ظهوره التالي · اسحب المقبضين الأصفرين للاقتطاع · "
                                              "الزر الأيمن للعمليات والعلامات · Ctrl+العجلة للتكبير"),
                               QStringLiteral("muted"), m_timelineCard);
    timelineHint->setMinimumWidth(10);
    timelineHeader->addWidget(timelineHint, 1);
    m_timeline = new TimelineView(m_timelineCard);
    m_laneFilter = new QComboBox(m_timelineCard);
    m_laneFilter->addItems({QStringLiteral("كل الأشخاص"), QStringLiteral("المعروفون"), QStringLiteral("المحتملون"),
                            QStringLiteral("غير المعروفين"), QStringLiteral("المراقَبون")});
    m_laneFilter->setToolTip(QStringLiteral("المسارات الظاهرة في الخط الزمني حسب نتيجة المطابقة"));
    m_laneFilter->setFocusPolicy(Qt::NoFocus);
    m_laneFilter->setCurrentIndex(std::clamp(QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("timeline/lanes"), 0).toInt(),
                                             0, m_laneFilter->count() - 1));
    connect(m_laneFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("timeline/lanes"), index);
        rebuildTimelineLanes();
    });
    timelineHeader->addWidget(m_laneFilter);
    auto timelineButton = [this](Icons::Name icon, const QString &tip, auto slot) {
        auto *button = new FancyButton(icon, tip, FancyButton::Kind::Secondary, m_timelineCard);
        button->setCompact(true);
        button->setToolTip(tip);
        button->setFixedSize(34, 30);
        connect(button, &QAbstractButton::clicked, m_timeline, slot);
        return button;
    };
    timelineHeader->addWidget(timelineButton(Icons::Name::ZoomIn, QStringLiteral("تكبير (Ctrl+العجلة)"), &TimelineView::zoomIn));
    timelineHeader->addWidget(timelineButton(Icons::Name::ZoomOut, QStringLiteral("تصغير"), &TimelineView::zoomOut));
    timelineHeader->addWidget(timelineButton(Icons::Name::FitWidth, QStringLiteral("عرض الفيديو كله"), &TimelineView::fit));
    timelineLayout->addLayout(timelineHeader);
    timelineLayout->addWidget(m_timeline, 1);
    m_timelineCard->setVisible(false);
    connect(m_timeline, &TimelineView::seekRequested, this, &MainWindow::loadVideoFrame);
    connect(m_timeline, &TimelineView::laneClicked, this, [this](int id, qint64 ms) {
        selectFace(id);
        if (ms < 0) {
            m_playbackBar->setEventKind(PlaybackBar::Event::Person);
            jumpToEvent(1);
        }
    });
    connect(m_timeline, &TimelineView::previewRequested, this, &MainWindow::requestPreview);
    connect(m_timeline, &TimelineView::laneActivated, this, [this](int id, qint64 ms) { openShotViewer(id, ms); });
    connect(m_timeline, &TimelineView::rangeEdited, this, &MainWindow::setTrimRange);
    connect(m_timeline, &TimelineView::contextMenuRequested, this, &MainWindow::showTimelineMenu);
    auto syncTrim = [this] {
        if (!m_videoRunning)
            m_timeline->setRange(m_rangeStart->time().msecsSinceStartOfDay(), m_rangeEnd->time().msecsSinceStartOfDay());
    };
    connect(m_rangeStart, &QTimeEdit::timeChanged, this, syncTrim);
    connect(m_rangeEnd, &QTimeEdit::timeChanged, this, syncTrim);
    m_laneTimer = new QTimer(this);
    m_laneTimer->setSingleShot(true);
    m_laneTimer->setInterval(400);
    connect(m_laneTimer, &QTimer::timeout, this, &MainWindow::rebuildTimelineLanes);

    auto *mediaSplitter = new QSplitter(Qt::Vertical, page);
    m_mediaSplitter = mediaSplitter;
    mediaSplitter->setHandleWidth(10);
    mediaSplitter->setChildrenCollapsible(false);
    mediaSplitter->addWidget(viewCard);
    mediaSplitter->addWidget(m_timelineCard);
    mediaSplitter->setStretchFactor(0, 4);
    mediaSplitter->setStretchFactor(1, 1);
    mediaSplitter->setSizes({460, 230});

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    m_mainSplitter = splitter;
    splitter->setHandleWidth(12);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(mediaSplitter);
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 320});
    layout->addWidget(splitter, 1);
    return page;
}

float MainWindow::threshold() const
{
    return m_thresholdSlider->value() / 100.0f;
}

void MainWindow::setBusy(bool busy)
{
    m_analyzing = busy;
    const bool idle = m_ready && !busy && !m_videoRunning;
    m_openImageButton->setEnabled(idle);
    m_openVideoButton->setEnabled(idle);
    m_cameraButton->setEnabled(idle);
    m_folderScanButton->setEnabled(m_ready);
    m_importButton->setEnabled(m_ready);
    m_stopButton->setEnabled(m_videoRunning);
    m_pauseButton->setEnabled(m_videoRunning && !filePlayerOpen());
    m_pauseButton->setVisible(m_videoRunning && !filePlayerOpen());
    m_playbackBar->setAnalysisRunning(m_videoRunning);
    updateRecordButton();
    m_saveFacesButton->setEnabled(!m_faces.isEmpty());
    m_enrollFaceButton->setEnabled(m_ready && m_faceList->currentRow() >= 0);
    updateVideoOptions();
    updateWorkState();
}

void MainWindow::updateVideoOptions()
{
    const bool idle = m_ready && !m_analyzing && !m_videoRunning;
    const bool fileVideo = !m_videoSource.isEmpty() && !m_videoSource.startsWith(QStringLiteral("camera:"));
    m_reanalyzeButton->setEnabled(idle && fileVideo);
    m_regionButton->setEnabled(m_ready && (m_view->hasImage() || m_regionButton->isChecked()));
    m_clearRegionButton->setEnabled(!m_view->region().isEmpty());
    m_enhanceImageButton->setEnabled(!m_analyzing && !m_videoRunning && (!m_videoMode || fileVideo));
}

// ───────────── التعرف ─────────────

void MainWindow::openFile(const QString &path)
{
    if (!m_ready) {
        m_pendingFile = path; // يُفتح تلقائياً بعد تحميل النماذج
        return;
    }
    if (isVideoFile(path))
        startVideo(path);
    else
        analyzeImage(path);
}

void MainWindow::openImage()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("فتح صورة"), m_lastDirectory, ImageFilter);
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    analyzeImage(path);
}

void MainWindow::analyzeImage(const QString &path)
{
    m_player->close();
    m_audio->close();
    if (m_videoFullscreen)
        toggleVideoFullscreen();
    if (m_panelsHidden)
        togglePanels(); // الصور تحتاج قائمة الوجوه
    m_videoMode = false;
    m_playbackBar->setVisible(false);
    m_timelineCard->setVisible(false);
    setVideoSettingsVisible(false);
    if (path != m_currentImagePath)
        m_view->resetZoom();
    m_currentImagePath = path;
    updateFolderNavigation();
    clearWatchAlerts();
    addRecentFile(path);
    m_faces.clear();
    m_rejectedMatches.clear();
    m_mergeSuggestions.clear();
    m_mergeSuggestButton->setVisible(false);
    refreshFaceList();
    m_view->setFaces({});
    setBusy(true);

    // عرض الصورة فوراً مع حركة المسح إلى أن ينتهي التحليل
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage preview = reader.read();
    if (!preview.isNull())
        m_view->setImage(preview, true);
    const bool enhance = m_enhanceSwitch->isChecked();
    m_view->setBusy(true, enhance ? QStringLiteral("جارٍ تحسين الصورة ثم كشف الوجوه")
                                  : QStringLiteral("جارٍ كشف الوجوه والتعرف عليها"));
    statusBar()->showMessage(QStringLiteral("جارٍ تحليل الصورة…"));

    const bool thorough = m_thoroughSwitch->isChecked();
    const FaceEngine *engine = &m_engine;
    m_imageWatcher.setFuture(QtConcurrent::run([engine, path, thorough, enhance] {
        ImageAnalysis result;
        cv::Mat image = ImageIO::loadImage(path);
        if (image.empty()) {
            result.error = QStringLiteral("تعذر قراءة الصورة: %1").arg(QDir::toNativeSeparators(path));
            return result;
        }
        std::vector<AnalyzedFace> faces;
        if (enhance) {
            const cv::Mat enhanced = ImageIO::enhance(image);
            faces = engine->analyzeEnhanced(image, enhanced, thorough);
            image = enhanced;
        } else {
            // التحليل المعمّق يجرّب كل الحيل: الوجوه البعيدة، والزوايا العلوية، والصور المدارة، والإضاءة السيئة
            faces = thorough ? engine->analyzeRobust(image) : engine->analyze(image, false);
        }
        result.image = ImageIO::toQImage(image);
        for (size_t i = 0; i < faces.size(); ++i)
            result.faces.append(ImageIO::makeFaceItem(image, faces[i], int(i) + 1));
        return result;
    }));
}

void MainWindow::onImageAnalyzed()
{
    const ImageAnalysis result = m_imageWatcher.result();
    m_view->setBusy(false);
    if (!result.error.isEmpty()) {
        statusBar()->showMessage(result.error);
        Toast::show(this, result.error, Toast::Kind::Warning);
        setBusy(false);
        return;
    }

    m_faces = result.faces;
    m_view->setImage(result.image, false);
    refreshMatches();
    for (const FaceItem &face : std::as_const(m_faces))
        checkWatchAlert(face);
    recordSightings(m_currentImagePath);
    if (!m_faces.isEmpty())
        m_faceList->setCurrentRow(0);

    int known = 0;
    for (const FaceItem &face : m_faces)
        known += face.match.status == MatchStatus::Confirmed ? 1 : 0;
    const QString summary = m_faces.isEmpty()
        ? QStringLiteral("لم يُعثر على وجوه في الصورة")
        : QStringLiteral("عدد الوجوه: %1 — تم التعرف على %2").arg(m_faces.size()).arg(known);
    statusBar()->showMessage(summary);
    Toast::show(this, summary, m_faces.isEmpty() ? Toast::Kind::Warning : Toast::Kind::Success);
    setBusy(false);
}

void MainWindow::openVideo()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("فتح فيديو"), m_lastDirectory, VideoFilter);
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    startVideo(path);
}

void MainWindow::startVideo(const QString &source)
{
    const bool camera = source.startsWith(QStringLiteral("camera:"));
    // معالجات كاميرا سابقة لا تبقى على المصدر الجديد
    if (!m_cameraId.isEmpty())
        VideoFilters::setCurrent(m_fileFilters, false);
    m_cameraId.clear();
    m_cameraName.clear();
    if (camera) {
        Cameras::setLastSource(source);
        Cameras::Camera info;
        if (Cameras::resolve(source.mid(7), &info)) {
            m_cameraName = info.name;
            if (!info.id.isEmpty()) {
                // لكل كاميرا محفوظة معالجاتها، فكاميرا مقلوبة أو ليلية لا تغيّر إعدادات الملفات
                m_cameraId = info.id;
                m_fileFilters = VideoFilters::current();
                VideoFilters::setCurrent(info.hasFilters ? info.filters : VideoFilters::Settings(), false);
            }
        }
        if (m_cameraName.isEmpty())
            m_cameraName = QStringLiteral("الكاميرا");
    }
    updateFiltersButton();
    if (source != m_videoSource && !m_videoSource.isEmpty()) {
        // منطقة التحليل خاصة بالفيديو السابق
        m_view->setRegion(QRectF());
    }
    m_videoSource = source;
    m_videoMode = true;
    m_videoRunning = true;
    m_folderImages.clear();
    m_folderIndex = -1;
    m_view->setNavigation(false, false, QString());
    setVideoSettingsVisible(true);
    m_playbackBar->setVisible(!camera);
    if (camera && m_videoFullscreen)
        toggleVideoFullscreen();
    m_shotChanges.clear();
    m_markers = camera ? QMap<qint64, QString>() : VideoMarkers::load(source);
    m_timeline->setMarkers(m_markers);
    updateVideoNavigation();
    clearWatchAlerts();
    if (camera)
        m_cameraWatch->reset(source, m_cameraName, m_recorder);
    if (!source.startsWith(QStringLiteral("camera:")))
        addRecentFile(source);
    m_faces.clear();
    m_hiddenFaces.clear();
    m_rejectedMatches.clear();
    m_dismissedMerges.clear();
    m_mergeSuggestions.clear();
    m_mergeSuggestButton->setVisible(false);
    m_analyzedFaces.clear();
    m_uncoveredClock.invalidate();
    m_view->setStatusBadge(QString());
    m_mergedInto.clear();
    refreshFaceList();
    const QRectF region = m_view->region();
    m_view->clear();
    m_view->setRegion(region);
    m_videoStartMs = camera ? 0 : m_rangeStart->time().msecsSinceStartOfDay();
    m_videoEndMs = camera ? 0 : m_rangeEnd->time().msecsSinceStartOfDay();
    if (m_videoEndMs <= m_videoStartMs)
        m_videoEndMs = 0;
    m_videoDurationMs = 0;
    m_videoWorker->setRange(m_videoStartMs, m_videoEndMs);
    m_videoWorker->setRegion(region);
    m_videoWorker->setZones(camera ? m_cameraWatch->rules() : CameraRules::Rules());
    m_videoWorker->setEnhance(m_enhanceSwitch->isChecked());
    m_view->setBusy(true, camera ? QStringLiteral("جارٍ الاتصال بالكاميرا «%1»").arg(m_cameraName)
                                 : QStringLiteral("جارٍ فتح الفيديو"));
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    const QString filters = VideoFilters::current().summary();
    statusBar()->showMessage(filters.isEmpty() ? QStringLiteral("جارٍ التحليل…")
                                               : QStringLiteral("جارٍ التحليل… · معالجات الفيديو: %1").arg(filters));
    setBusy(true);

    m_videoWorker->setFrameStep(m_frameStepSpin->value());
    m_analysisSpeed = 0.0;
    m_analysisRemainingMs = -1;
    m_analysisProgress = -1.0;
    m_videoClock.invalidate();
    m_videoStep = 1;
    m_videoPaused = false;
    m_pausedMs = 0;
    m_pauseButton->setIconName(Icons::Name::Pause);
    m_pauseButton->setText(QStringLiteral("إيقاف مؤقت"));
    m_cameraRetryTimer->stop();
    m_cameraLostAt = 0;
    m_cameraOfflineAlerted = false;
    if (camera)
        AppLog::write(QStringLiteral("تشغيل الكاميرا «%1»").arg(m_cameraName));
    // العرض بسرعة المصدر من VideoPlayer، والتحليل في خيطه على ما يستطيع من الإطارات
    m_videoWorker->setDeliverFrames(false);
    m_videoWorker->setAdaptive(true);
    if (camera) {
        auto slot = std::make_shared<Cameras::FrameSlot>();
        m_videoWorker->setLiveFrames(slot);
        m_player->openLive(slot);
    } else {
        m_videoWorker->setLiveFrames(nullptr);
        if (m_player->openFile(source, m_videoStartMs)) {
            m_player->setStopAt(m_videoEndMs);
            m_player->setRate(m_playbackBar->rate());
            m_playbackBar->setDuration(m_player->durationMs());
            m_playbackBar->setFps(m_player->fps());
            m_playbackBar->setPosition(m_videoStartMs);
            m_player->play();
        }
    }
    if (camera)
        m_audio->close();
    else
        m_audio->open(source);
    updatePauseButton();
    m_videoWorker->prepareStart();
    QMetaObject::invokeMethod(m_videoWorker, "process", Qt::QueuedConnection, Q_ARG(QString, source));
}

void MainWindow::stopVideo()
{
    m_videoWorker->requestStop();
}

void MainWindow::togglePause()
{
    if (filePlayerOpen()) {
        // الملف: الإيقاف للعرض وحده، والتحليل يكمل في الخلفية
        if (m_player->isPlaying()) {
            m_player->pause();
        } else {
            const qint64 endMs = m_videoEndMs > 0 ? m_videoEndMs : m_player->durationMs();
            if (endMs > 0 && m_player->position() + 2 * qint64(1000.0 / m_player->fps()) >= endMs)
                m_player->seek(m_videoStartMs); // انتهى: يعيد التشغيل من البداية
            m_player->play();
        }
        updatePauseButton();
        return;
    }
    if (!m_videoRunning)
        return;
    m_videoPaused = !m_videoPaused;
    m_videoWorker->setPaused(m_videoPaused);
    if (m_videoPaused)
        m_player->pause();
    else
        m_player->play();
    updatePauseButton();
    if (m_videoPaused) {
        m_pauseClock.start();
        statusBar()->showMessage(QStringLiteral("متوقف مؤقتاً — اضغط «استئناف» أو مفتاح المسافة للمتابعة من حيث توقف"));
    } else {
        if (m_pauseClock.isValid())
            m_pausedMs += m_pauseClock.elapsed();
        m_pauseClock.invalidate();
        statusBar()->showMessage(QStringLiteral("جارٍ استئناف التحليل…"));
    }
}

void MainWindow::onFrameReady(const QImage &frame, const QList<FaceItem> &faces, qint64 positionMs, double progress)
{
    if (!frame.isNull())
        m_videoWorker->frameShown();
    ++m_analyzedFrames;
    if (!m_videoRunning)
        return;
    const bool camera = m_videoSource.startsWith(QStringLiteral("camera:"));
    if (camera) {
        // البث يعرضه VideoPlayer بسرعة الكاميرا، والمربعات تنتقل بنعومة إلى آخر نتيجة
        if (!m_player->isOpen()) {
            m_view->setBusy(false);
            m_view->setImage(frame);
        }
        m_view->setFaces(faces);
        noteCameraFaces(frame, faces);
    } else {
        m_analyzedFaces.insert(positionMs, faces);
        // العرض متوقف لا يرسل إطارات: وصول التحليل إلى موضعه يخفي الشارة من هنا
        if (filePlayerOpen())
            updateAnalysisBadge(m_videoPositionMs);
    }
    if (progress >= 0.0) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(int(progress * 1000.0));
    }
    const qint64 endMs = m_videoEndMs > 0 ? m_videoEndMs : m_videoDurationMs;
    QString status = endMs > 0 ? QStringLiteral("الموضع %1 من %2").arg(ImageIO::formatTime(positionMs), ImageIO::formatTime(endMs))
                               : QStringLiteral("الموضع %1").arg(ImageIO::formatTime(positionMs));
    const double seconds = m_videoClock.isValid() ? (m_videoClock.elapsed() - m_pausedMs) / 1000.0 : 0.0;
    // ما حُلِّل من الجزء المطلوب: التحليل قد يقفز ليلحق العرض ثم يعود إلى ما تخطاه، فالموضع وحده لا يكفي
    const qint64 rangeMs = endMs > m_videoStartMs ? endMs - m_videoStartMs : 0;
    const qint64 doneMs = progress >= 0.0 && rangeMs > 0 ? qint64(progress * rangeMs) : positionMs - m_videoStartMs;
    if (seconds > 1.0 && doneMs > 0) {
        // السرعة بالنسبة لزمن التشغيل العادي: 2× تعني أن دقيقة من الفيديو تُحلَّل في نصف دقيقة
        const double speed = doneMs / 1000.0 / seconds;
        status += QStringLiteral(" · السرعة %1×").arg(QLocale().toString(speed, 'f', 1));
        m_analysisSpeed = speed;
        const qint64 leftMs = rangeMs > 0 ? rangeMs - doneMs : endMs - positionMs;
        m_analysisRemainingMs = leftMs > 0 && speed > 0.0 ? qint64(leftMs / speed) : -1;
        if (m_analysisRemainingMs >= 0)
            status += QStringLiteral(" · المتبقي %1").arg(ImageIO::formatTime(m_analysisRemainingMs));
    }
    if (progress >= 0.0)
        m_analysisProgress = progress;
    if (m_videoStep > 1)
        status += QStringLiteral(" · تحليل إطار من كل %1").arg(m_videoStep);
    status += QStringLiteral(" · وجوه في الإطار: %1 · أشخاص مختلفون: %2").arg(faces.size()).arg(m_faces.size());
    if (m_videoPaused)
        status.prepend(QStringLiteral("متوقف مؤقتاً · "));
    else if (filePlayerOpen() && !m_player->isPlaying())
        status.prepend(QStringLiteral("العرض متوقف والتحليل مستمر · "));
    else if (!camera)
        status.prepend(QStringLiteral("التحليل: "));
    statusBar()->showMessage(status);
}

bool MainWindow::filePlayerOpen() const
{
    return m_player && m_player->isOpen() && !m_player->isLive();
}

void MainWindow::updatePauseButton()
{
    const bool playing = filePlayerOpen() ? m_player->isPlaying() : !m_videoPaused;
    m_pauseButton->setIconName(playing ? Icons::Name::Pause : Icons::Name::Play);
    m_pauseButton->setText(playing ? QStringLiteral("إيقاف مؤقت") : QStringLiteral("تشغيل"));
    m_pauseButton->setEnabled(m_videoRunning && !filePlayerOpen());
    m_pauseButton->setVisible(m_videoRunning && !filePlayerOpen());
    m_playbackBar->setPlaying(filePlayerOpen() && m_player->isPlaying());
    // أُوقف العرض أو استؤنف: التحليل يعود إلى كثافته المعتادة أو يتباعد ليبقى معه
    if (m_videoRunning && filePlayerOpen())
        m_videoWorker->setDisplayPosition(m_videoPositionMs, m_player->isPlaying());
    syncAudio();
}

bool MainWindow::hasAnalysisNear(qint64 positionMs) const
{
    auto it = m_analyzedFaces.upperBound(positionMs);
    return it != m_analyzedFaces.begin() && positionMs - std::prev(it).key() <= VideoPlayer::AnalysisToleranceMs;
}

void MainWindow::updateAnalysisBadge(qint64 positionMs)
{
    if (!m_videoRunning || !filePlayerOpen() || hasAnalysisNear(positionMs)) {
        m_uncoveredClock.invalidate();
        m_view->setStatusBadge(QString());
        return;
    }
    if (!m_uncoveredClock.isValid())
        m_uncoveredClock.start();
    if (m_uncoveredClock.elapsed() < 500)
        return;
    const auto next = m_analyzedFaces.upperBound(positionMs);
    const qint64 lagMs = next == m_analyzedFaces.begin() ? -1 : positionMs - std::prev(next).key();
    m_view->setStatusBadge(VideoPlayer::analysisLagText(lagMs, m_videoWorker->followsDisplay(), m_player->isPlaying()));
}

QList<FaceItem> MainWindow::analyzedFacesAt(qint64 positionMs) const
{
    auto next = m_analyzedFaces.upperBound(positionMs);
    if (next == m_analyzedFaces.begin())
        return {};
    const auto previous = std::prev(next);
    if (positionMs - previous.key() > VideoPlayer::AnalysisToleranceMs)
        return {};
    QList<FaceItem> faces = previous.value();
    // بين إطارين محللين: المربع ينتقل خطياً من موضعه في الأول إلى موضعه في الثاني
    const bool between = next != m_analyzedFaces.end() && next.key() - previous.key() <= VideoPlayer::AnalysisToleranceMs;
    const double t = between ? double(positionMs - previous.key()) / double(next.key() - previous.key()) : 0.0;
    QList<FaceItem> result;
    result.reserve(faces.size());
    for (FaceItem &face : faces) {
        if (between) {
            for (const FaceItem &later : next.value()) {
                if (later.id != face.id)
                    continue;
                const QRectF a = face.box, b = later.box;
                face.box = QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                                  a.width() + (b.width() - a.width()) * t, a.height() + (b.height() - a.height()) * t);
                break;
            }
        }
        // الهوية الحالية بعد أي دمج، ونتيجة تعرفها الأحدث (قد تكون سُمّيت أو صُححت بعد تحليل الإطار)
        for (int guard = 0; guard < 64 && m_mergedInto.contains(face.id); ++guard)
            face.id = m_mergedInto.value(face.id);
        if (m_hiddenFaces.contains(face.id))
            continue;
        const int row = m_faceRows.value(face.id, -1);
        if (row >= 0 && row < m_faces.size() && m_faces.at(row).id == face.id)
            face.match = m_faces.at(row).match;
        result.append(face);
    }
    return result;
}

void MainWindow::onPlayerFrame(const QImage &frame, qint64 positionMs)
{
    m_player->frameShown();
    if (!m_videoMode || !m_player->isOpen())
        return;
    m_view->setBusy(false);
    m_view->setImage(frame);
    if (m_player->isLive())
        return;
    m_videoPositionMs = positionMs;
    // التحليل يلحق العرض إن سبقه إلى جزء لم يُحلَّل
    if (m_videoRunning)
        m_videoWorker->setDisplayPosition(positionMs, m_player->isPlaying());
    m_view->setFaces(analyzedFacesAt(positionMs));
    updateAnalysisBadge(positionMs);
    if (!m_timelineCard->isHidden())
        m_timeline->setPosition(positionMs);
    m_playbackBar->setPosition(positionMs);
    m_audio->sync(m_player->isPlaying(), positionMs, m_player->rate());
}

void MainWindow::onTrackUpdated(const FaceItem &track)
{
    if (m_hiddenFaces.contains(track.id))
        return;
    // رفض المستخدم مطابقة هذا الوجه لهذا الشخص: يبقى غير معروف حتى لو أعاد المحلل المطابقة نفسها
    const auto rejected = m_rejectedMatches.constFind(track.id);
    if (rejected != m_rejectedMatches.constEnd() && !track.match.personId.isEmpty() && track.match.personId == *rejected) {
        FaceItem cleared = track;
        cleared.match = MatchResult();
        onTrackUpdated(cleared);
        return;
    }
    if (m_faces.isEmpty())
        m_faceRows.clear();
    const int i = m_faceRows.value(track.id, -1);
    checkWatchAlert(track);
    if (i >= 0 && i < m_faces.size() && m_faces.at(i).id == track.id) {
        m_faces[i] = track;
        updateFaceListItem(m_faceList->item(i), track);
        m_faceList->setRowHidden(i, !faceMatchesFilter(track));
        if (!m_laneTimer->isActive())
            m_laneTimer->start();
        if (!m_faceTabsTimer->isActive())
            m_faceTabsTimer->start();
        // الوجه المحدد يتحدث عدة مرات في الثانية، وتفاصيله تستعلم القاعدة وتعيد بناء النص: مرة كل ثلث ثانية تكفي
        if (m_faceList->currentRow() == i) {
            if (!m_detailsTimer) {
                m_detailsTimer = new QTimer(this);
                m_detailsTimer->setSingleShot(true);
                m_detailsTimer->setInterval(300);
                connect(m_detailsTimer, &QTimer::timeout, this, &MainWindow::showFaceDetails);
            }
            if (!m_detailsTimer->isActive())
                m_detailsTimer->start();
        }
        return;
    }
    m_faceRows.insert(track.id, int(m_faces.size()));
    m_faces.append(track);
    updateFaceListItem(new QListWidgetItem(m_faceList), track);
    m_faceList->setRowHidden(m_faceList->count() - 1, !faceMatchesFilter(track));
    if (!m_laneTimer->isActive())
        m_laneTimer->start();
    updateFaceCount();
    m_saveFacesButton->setEnabled(true);
}

void MainWindow::onVideoFinished(const QString &error)
{
    if (!m_pendingVideo.isEmpty()) {
        // أُوقف التحليل للانتقال إلى فيديو آخر في المجلد
        m_videoRunning = false;
        m_restartFromMs = -1;
        const QString next = std::exchange(m_pendingVideo, QString());
        setBusy(false);
        startVideo(next);
        return;
    }
    if (m_restartFromMs >= 0) {
        // أُوقف التحليل ليبدأ من جديد من موضع آخر
        m_videoRunning = false;
        const qint64 from = std::exchange(m_restartFromMs, -1);
        setBusy(false);
        analyzeVideoFrom(from);
        return;
    }
    m_videoRunning = false;
    m_videoPaused = false;
    if (m_player->isLive())
        m_player->close();
    updatePauseButton();
    // قبل إعادة معالجات الملفات، فلا تُطبَّق على آخر إطارات التسجيل
    const QString recorded = stopRecording();
    if (!m_cameraId.isEmpty()) {
        VideoFilters::setCurrent(m_fileFilters, false);
        m_cameraId.clear();
        updateFiltersButton();
    }
    m_timeline->clearAnalyzedRanges();
    rebuildTimelineLanes();
    m_view->setBusy(false);
    m_progress->setVisible(false);
    m_view->setStatusBadge(QString());
    const bool cameraSource = m_videoSource.startsWith(QStringLiteral("camera:"));
    if (cameraSource)
        AppLog::write(error.isEmpty() ? QStringLiteral("إيقاف الكاميرا «%1»").arg(m_cameraName)
                                      : QStringLiteral("توقفت الكاميرا «%1»: %2").arg(m_cameraName, error));
    if (cameraSource && !error.isEmpty() && m_cameraLostAt > 0) {
        // انقطعت أثناء المراقبة ولم تعد: لا تبقى المراقبة متوقفة على جهاز يعمل وحده
        emit systemAlert(QStringLiteral("توقفت الكاميرا «%1»").arg(m_cameraName),
                         QStringLiteral("%1. تُعاد المحاولة كل %2 ثانية").arg(error).arg(CameraRestartMs / 1000));
        m_cameraRetryTimer->start();
    }
    if (!error.isEmpty()) {
        statusBar()->showMessage(recorded.isEmpty() ? error : error + QStringLiteral(" · ") + recorded);
        Toast::show(this, error, Toast::Kind::Warning);
    } else {
        if (m_faceSort->currentIndex() != 0)
            sortFaces();
        recordSightings(m_videoSource);
        if (!cameraSource)
            updateMergeSuggestions();
        const QString summary = QStringLiteral("انتهى التحليل — أشخاص مختلفون: %1 · انقر نقراً مزدوجاً على وجه لعرض لقطاته")
                                    .arg(m_faces.size());
        statusBar()->showMessage(recorded.isEmpty() ? summary : summary + QStringLiteral(" · ") + recorded);
        Toast::show(this, recorded.isEmpty() ? summary : QStringLiteral("انتهى التحليل وحُفظ تسجيل الكاميرا"), Toast::Kind::Info);
    }
    setBusy(false);
    if (error.isEmpty())
        checkAutoNext();
}

// ───────────── إدارة الوجوه المكتشفة ─────────────

void MainWindow::rebuildFaceRows()
{
    m_faceRows.clear();
    for (int i = 0; i < m_faces.size(); ++i)
        m_faceRows.insert(m_faces.at(i).id, i);
}

void MainWindow::sortFaces()
{
    const int mode = m_faceSort->currentIndex();
    auto order = [mode](const FaceItem &a, const FaceItem &b) {
        switch (mode) {
        case 1:
            return a.frames > b.frames;
        case 2:
            return a.quality > b.quality;
        case 3:
            if (a.match.status != b.match.status)
                return int(a.match.status) > int(b.match.status);
            return a.match.score > b.match.score;
        default:
            break;
        }
        // حسب الظهور: في الفيديو بالزمن، وفي الصورة بالترتيب الأصلي
        if (a.firstSeenMs != b.firstSeenMs)
            return a.firstSeenMs < b.firstSeenMs;
        return a.id < b.id;
    };
    std::stable_sort(m_faces.begin(), m_faces.end(), order);
    refreshFaceList();
}

QList<int> MainWindow::selectedFaceIds() const
{
    QList<int> ids;
    const QList<QListWidgetItem *> items = m_faceList->selectedItems();
    for (const QListWidgetItem *item : items)
        ids << item->data(CardRole::Id).toInt();
    if (ids.isEmpty() && m_faceList->currentItem())
        ids << m_faceList->currentItem()->data(CardRole::Id).toInt();
    return ids;
}

void MainWindow::showFaceContextMenu(const QPoint &pos)
{
    QListWidgetItem *clicked = m_faceList->itemAt(pos);
    if (clicked && !clicked->isSelected()) {
        m_faceList->clearSelection();
        m_faceList->setCurrentItem(clicked);
    }
    const QList<int> ids = selectedFaceIds();
    if (ids.isEmpty())
        return;
    const bool fileVideo = m_videoMode && !m_videoSource.startsWith(QStringLiteral("camera:"));

    QMenu menu(this);
    if (ids.size() == 1) {
        const int id = ids.first();
        QAction *view = menu.addAction(Icons::icon(Icons::Name::Film, Theme::Text, 18), QStringLiteral("عرض اللقطة في الفيديو"),
                                       this, [this, id] { openShotViewer(id); });
        view->setEnabled(fileVideo);
        if (!m_videoMode)
            menu.addAction(Icons::icon(Icons::Name::ZoomIn, Theme::Text, 18), QStringLiteral("تكبير على الوجه في الصورة"), this,
                           [this, id] { m_view->zoomToFace(id); });
        menu.addAction(Icons::icon(Icons::Name::UserPlus, Theme::Text, 18), QStringLiteral("إضافة إلى قاعدة البيانات…"), this, [this, id] {
            const int row = m_faceRows.value(id, -1);
            if (row >= 0)
                addFaceToDatabase(m_faces.at(row));
        })->setEnabled(m_ready);
        if (m_videoMode && !m_videoSource.startsWith(QStringLiteral("camera:")))
            menu.addAction(Icons::icon(Icons::Name::Collect, Theme::Text, 18), QStringLiteral("تعزيز صور الشخص من الفيديو…"), this,
                           [this, id] { harvestFromFace(id); });
        menu.addAction(Icons::icon(Icons::Name::Save, Theme::Text, 18), QStringLiteral("حفظ صورة الوجه…"), this, [this, id] {
            const int row = m_faceRows.value(id, -1);
            if (row < 0)
                return;
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ صورة الوجه"),
                                                              QDir(m_lastDirectory).filePath(QStringLiteral("وجه.png")),
                                                              QStringLiteral("صور (*.png *.jpg)"));
            if (!path.isEmpty() && ImageIO::faceCrop(m_faces.at(row)).save(path))
                Toast::show(this, QStringLiteral("تم حفظ صورة الوجه"));
        });
    } else {
        menu.addAction(Icons::icon(Icons::Name::Merge, Theme::Text, 18),
                       QStringLiteral("دمج الوجوه المحددة (%1) كشخص واحد").arg(ids.size()), this, [this, ids] { mergeFaces(ids); })
            ->setEnabled(m_videoMode);
    }
    menu.addSeparator();
    menu.addAction(Icons::icon(Icons::Name::Trash, Theme::Text, 18),
                   ids.size() == 1 ? QStringLiteral("إزالة من القائمة") : QStringLiteral("إزالة المحدد من القائمة (%1)").arg(ids.size()),
                   this, [this, ids] { removeFaces(ids); });
    menu.exec(m_faceList->viewport()->mapToGlobal(pos));
}

void MainWindow::mergeFaces(const QList<int> &ids)
{
    if (ids.size() < 2)
        return;
    // يبقى الوجه الأكثر ظهوراً، وتُضاف إليه البقية
    int keepRow = -1;
    for (int id : ids) {
        const int row = m_faceRows.value(id, -1);
        if (row >= 0 && (keepRow < 0 || m_faces.at(row).frames > m_faces.at(keepRow).frames))
            keepRow = row;
    }
    if (keepRow < 0)
        return;
    const int keepId = m_faces.at(keepRow).id;
    for (int id : ids) {
        if (id == keepId)
            continue;
        if (m_videoRunning)
            m_videoWorker->mergeIdentities(keepId, id);
        onIdentitiesMerged(keepId, id);
    }
    Toast::show(this, QStringLiteral("دُمجت %1 وجوه كشخص واحد").arg(ids.size()));
}

void MainWindow::onIdentitiesMerged(int keepId, int removedId)
{
    if (keepId != removedId)
        m_mergedInto.insert(removedId, keepId);
    const int keepRow = m_faceRows.value(keepId, -1);
    const int removedRow = m_faceRows.value(removedId, -1);
    if (removedRow < 0 || removedRow >= m_faces.size())
        return;
    if (keepRow >= 0 && keepRow < m_faces.size()) {
        // دمج محلي فوري؛ ومحلل الفيديو يرسل الحالة النهائية للوجه الباقي إن كان ما يزال يعمل
        FaceItem &keep = m_faces[keepRow];
        const FaceItem &removed = m_faces.at(removedRow);
        if (keep.embedding.size() == removed.embedding.size() && !keep.embedding.empty()) {
            const float wa = float(keep.frames), wb = float(removed.frames);
            for (size_t i = 0; i < keep.embedding.size(); ++i)
                keep.embedding[i] = keep.embedding[i] * wa + removed.embedding[i] * wb;
            FaceEngine::normalize(keep.embedding);
        }
        if (removed.quality > keep.quality) {
            keep.quality = removed.quality;
            keep.crop = removed.crop;
            keep.packedCrop = removed.packedCrop;
            keep.aligned = removed.aligned;
            keep.packedAligned = removed.packedAligned;
            keep.bestMs = removed.bestMs;
            keep.sourceBox = removed.sourceBox;
        }
        keep.frames += removed.frames;
        if (removed.firstSeenMs >= 0 && (keep.firstSeenMs < 0 || removed.firstSeenMs < keep.firstSeenMs))
            keep.firstSeenMs = removed.firstSeenMs;
        keep.lastSeenMs = std::max(keep.lastSeenMs, removed.lastSeenMs);
        keep.appearances += removed.appearances;
        std::sort(keep.appearances.begin(), keep.appearances.end());
        if (removed.match.status > keep.match.status
            || (removed.match.status == keep.match.status && removed.match.score > keep.match.score))
            keep.match = removed.match;
        updateFaceListItem(m_faceList->item(keepRow), keep);
    }
    m_hiddenFaces.insert(removedId);
    m_faces.removeAt(removedRow);
    delete m_faceList->takeItem(removedRow);
    rebuildFaceRows();
    applyFaceFilter();
    showFaceDetails();
    // اقتراحات الدمج التي تخص الوجه المدموج لم تعد قائمة
    if (!m_mergeSuggestions.isEmpty()) {
        m_mergeSuggestions.removeIf([removedId](const MergeSuggestion &s) { return s.a == removedId || s.b == removedId; });
        m_mergeSuggestButton->setText(QStringLiteral("غالباً الشخص نفسه: %1 اقتراح").arg(m_mergeSuggestions.size()));
        m_mergeSuggestButton->setVisible(!m_mergeSuggestions.isEmpty());
    }
}

void MainWindow::removeFaces(const QList<int> &ids)
{
    for (int id : ids) {
        const int row = m_faceRows.value(id, -1);
        if (row < 0)
            continue;
        m_hiddenFaces.insert(id);
        m_faces.removeAt(row);
        delete m_faceList->takeItem(row);
        rebuildFaceRows();
        m_mergeSuggestions.removeIf([id](const MergeSuggestion &s) { return s.a == id || s.b == id; });
    }
    m_mergeSuggestButton->setText(QStringLiteral("غالباً الشخص نفسه: %1 اقتراح").arg(m_mergeSuggestions.size()));
    m_mergeSuggestButton->setVisible(!m_mergeSuggestions.isEmpty());
    applyFaceFilter();
    showFaceDetails();
    setBusy(m_analyzing);
}

void MainWindow::openShotViewer(int id, qint64 positionMs)
{
    const int row = m_faceRows.value(id, -1);
    if (row < 0 || row >= m_faces.size() || m_videoSource.isEmpty() || m_videoSource.startsWith(QStringLiteral("camera:")))
        return;
    auto *viewer = new ShotViewer(&m_engine, &m_database, m_videoSource, m_faces.at(row), threshold(), m_enhanceSwitch->isChecked(), this);
    connect(viewer, &ShotViewer::addFaceRequested, this, &MainWindow::addFaceToDatabase);
    // العارض يبقى مفتوحاً وقد يُفتح فيديو آخر بعده، فيُحلَّل فيديو العارض لا الفيديو الحالي
    connect(viewer, &ShotViewer::analyzeFromRequested, this, [this, source = m_videoSource](qint64 positionMs) {
        if (source != m_videoSource) {
            if (m_analyzing || m_videoRunning) {
                Toast::show(this, QStringLiteral("أوقف التحليل الجاري أولاً"), Toast::Kind::Warning);
                return;
            }
            m_videoSource = source;
        }
        analyzeVideoFrom(positionMs);
    });
    viewer->setThumbnails(m_timeline->thumbnails());
    if (positionMs >= 0)
        viewer->showAt(positionMs);
    viewer->show();
}

void MainWindow::rebuildTimelineLanes()
{
    if (!m_videoMode || !!m_timelineCard->isHidden())
        return;
    QList<TimelineLane> lanes;
    lanes.reserve(m_faces.size());
    for (int i = 0; i < m_faces.size(); ++i) {
        if (i < m_faceList->count() && m_faceList->isRowHidden(i))
            continue;
        const FaceItem &face = m_faces.at(i);
        const bool watched = face.match.status == MatchStatus::Confirmed && m_watched.contains(face.match.personId);
        const int filter = m_laneFilter->currentIndex();
        if ((filter == 1 && face.match.status != MatchStatus::Confirmed) || (filter == 2 && face.match.status != MatchStatus::Possible)
            || (filter == 3 && face.match.status != MatchStatus::Unknown) || (filter == 4 && !watched))
            continue;
        TimelineLane lane;
        lane.id = face.id;
        lane.name = matchLabel(face.match);
        // المعروفون بلون حالتهم، وغيرهم بألوان مميزة ثابتة لكل شخص
        lane.color = face.match.status == MatchStatus::Unknown
            ? QColor::fromHsv((face.id * 47 + 190) % 360, 120, 235)
            : matchColor(face.match.status);
        auto cached = m_laneAvatars.constFind(face.id);
        if (cached == m_laneAvatars.constEnd() || cached->first != face.quality) {
            cached = m_laneAvatars.insert(face.id, {face.quality, circularPixmap(ImageIO::faceCrop(face), 40)});
        }
        lane.avatar = cached->second;
        lane.segments = face.appearances;
        if (lane.segments.isEmpty() && face.firstSeenMs >= 0)
            lane.segments.append({face.firstSeenMs, std::max(face.firstSeenMs, face.lastSeenMs)});
        qint64 total = 0;
        for (const auto &segment : std::as_const(lane.segments))
            total += segment.second - segment.first;
        const QString status = face.match.status == MatchStatus::Confirmed
            ? QStringLiteral("معروف %1%").arg(qRound(face.match.score * 100))
            : (face.match.status == MatchStatus::Possible ? QStringLiteral("محتمل %1%").arg(qRound(face.match.score * 100))
                                                           : QStringLiteral("غير معروف"));
        lane.detail = QStringLiteral("%1\n%2%3\nظهر %4 مرة · المدة %5\nانقر الاسم للانتقال إلى ظهوره التالي")
                          .arg(lane.name, status, watched ? QStringLiteral(" · مراقَب") : QString())
                          .arg(lane.segments.size())
                          .arg(ImageIO::formatTime(total));
        lane.watched = watched;
        lanes.append(lane);
    }
    m_timeline->setLanes(lanes);
}

void MainWindow::loadVideoFrame(qint64 positionMs)
{
    if (m_videoSource.isEmpty() || m_videoSource.startsWith(QStringLiteral("camera:")))
        return;
    // المشغّل يعرض الإطار فوراً بنتائج التحليل المحفوظة؛ أثناء التحليل حتى الجزء الذي لم يُحلَّل بعد
    if (filePlayerOpen() && (m_videoRunning || hasAnalysisNear(positionMs))) {
        m_player->seek(positionMs);
        m_timeline->setPosition(positionMs);
        m_playbackBar->setPosition(positionMs);
        m_audio->seeked();
        return;
    }
    if (m_videoRunning)
        return;
    // لا نتيجة محفوظة لهذا الموضع: يُحلَّل الإطار وحده
    if (filePlayerOpen()) {
        m_player->pause();
        m_player->seek(positionMs); // فيتابع التشغيل من هنا
        updatePauseButton();
    }
    if (!m_frameCapture || m_frameCapturePath != m_videoSource) {
        m_frameWatcher.waitForFinished();
        m_frameCapture = std::make_shared<cv::VideoCapture>();
        m_frameCapturePath = m_videoSource;
        if (!ImageIO::openVideo(*m_frameCapture, m_videoSource)) {
            m_frameCapture.reset();
            statusBar()->showMessage(QStringLiteral("تعذر فتح الفيديو"));
            return;
        }
    }
    m_frameRequestMs = positionMs;
    if (m_frameWatcher.isRunning())
        return; // يُحمَّل آخر موضع مطلوب عند انتهاء الحالي
    const qint64 ms = std::exchange(m_frameRequestMs, -1);
    std::shared_ptr<cv::VideoCapture> capture = m_frameCapture;
    const FaceEngine *engine = &m_engine;
    const GalleryPtr gallery = m_database.gallery();
    const float threshold = this->threshold();
    const bool enhance = m_enhanceSwitch->isChecked();
    const double fps = m_videoFps;
    statusBar()->showMessage(QStringLiteral("جارٍ عرض الإطار عند %1…").arg(ImageIO::formatTime(ms)));
    m_frameWatcher.setFuture(QtConcurrent::run([=] { return readShotFrame(*capture, ms, fps, engine, gallery, threshold, enhance); }));
}

void MainWindow::onVideoFrameLoaded()
{
    ShotFrame frame = m_frameWatcher.result();
    if (m_frameRequestMs >= 0) {
        if (frame.error.isEmpty())
            m_view->setImage(frame.image);
        loadVideoFrame(m_frameRequestMs);
        return;
    }
    if (!frame.error.isEmpty()) {
        statusBar()->showMessage(frame.error);
        return;
    }
    // ربط كل وجه في الإطار بالشخص المتتبَّع الأشبه به، فيحدده النقر عليه في القائمة
    QHash<int, float> used;
    int unknown = 0;
    for (FaceItem &face : frame.faces) {
        int bestId = 0;
        float best = 0.35f;
        for (const FaceItem &tracked : std::as_const(m_faces)) {
            const float score = FaceEngine::cosine(face.embedding, tracked.embedding);
            if (score > best) {
                best = score;
                bestId = tracked.id;
            }
        }
        if (bestId != 0 && used.value(bestId, -1.0f) < best) {
            used.insert(bestId, best);
            face.id = bestId;
        } else {
            face.id = -(++unknown);
        }
    }
    m_view->setBusy(false);
    m_view->setImage(frame.image);
    m_view->setFaces(frame.faces);
    m_timeline->setPosition(frame.positionMs);
    m_playbackBar->setPosition(frame.positionMs);
    m_videoPositionMs = frame.positionMs;
    const int row = m_faceList->currentRow();
    if (row >= 0 && row < m_faces.size())
        m_view->setSelectedFace(m_faces.at(row).id);
    statusBar()->showMessage(QStringLiteral("الإطار عند %1 · وجوه فيه: %2 · انقر نقراً مزدوجاً على وجه لعرض لقطاته")
                                 .arg(ImageIO::formatTime(frame.positionMs))
                                 .arg(frame.faces.size()));
    updateVideoOptions();
}

void MainWindow::analyzeVideoFrom(qint64 positionMs)
{
    if (m_videoSource.isEmpty() || m_videoSource.startsWith(QStringLiteral("camera:")))
        return;
    if (m_videoRunning) {
        m_restartFromMs = positionMs;
        m_videoWorker->requestStop();
        return;
    }
    m_rangeStart->setTime(QTime(0, 0).addMSecs(int(positionMs)));
    if (m_rangeEnd->time().msecsSinceStartOfDay() <= positionMs)
        m_rangeEnd->setTime(QTime(0, 0));
    startVideo(m_videoSource);
}

void MainWindow::reanalyzeVideo()
{
    if (!m_videoRunning && !m_videoSource.isEmpty())
        startVideo(m_videoSource);
}

void MainWindow::refreshMatches()
{
    // في الفيديو يعيد محلل الفيديو المطابقة بنفسه، والقوائم الطويلة جداً لا تُعاد مطابقتها كلها هنا
    const GalleryPtr gallery = m_database.gallery();
    if (!gallery || (m_videoMode && (m_videoRunning || m_faces.size() > 300)))
        return;
    for (FaceItem &face : m_faces) {
        face.match = FaceDatabase::identify(*gallery, face.embedding, threshold());
        if (!face.match.personId.isEmpty() && m_rejectedMatches.value(face.id) == face.match.personId)
            face.match = MatchResult();
    }
    refreshFaceList();
    if (!m_videoMode) {
        m_view->setFaces(m_faces);
        for (const FaceItem &face : std::as_const(m_faces))
            checkWatchAlert(face);
    }
}

void MainWindow::refreshFaceList()
{
    const int row = m_faceList->currentRow();
    const int currentId = row >= 0 && row < m_faceList->count() ? m_faceList->item(row)->data(CardRole::Id).toInt() : -1;
    {
        const QSignalBlocker blocker(m_faceList);
        m_faceList->clear();
        for (const FaceItem &face : std::as_const(m_faces)) {
            updateFaceListItem(new QListWidgetItem(m_faceList), face);
            m_faceList->setRowHidden(m_faceList->count() - 1, !faceMatchesFilter(face));
        }
    }
    rebuildFaceRows();
    if (m_laneTimer)
        m_laneTimer->start();
    const int newRow = m_faceRows.value(currentId, -1);
    if (newRow >= 0)
        m_faceList->setCurrentRow(newRow);
    updateFaceCount();
    showFaceDetails();
    setBusy(m_analyzing);
}

void MainWindow::selectFace(int id)
{
    for (int i = 0; i < m_faces.size(); ++i) {
        if (m_faces.at(i).id == id) {
            m_faceList->setCurrentRow(i);
            m_faceList->scrollToItem(m_faceList->item(i));
            return;
        }
    }
}

void MainWindow::addRecentFile(const QString &path)
{
    if (path.isEmpty() || QFileInfo(path).fileName() == QStringLiteral("malamih-clipboard.png"))
        return;
    QSettings settings;
    QStringList files = settings.value(QStringLiteral("recentFiles")).toStringList();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    files.removeAll(absolute);
    files.prepend(absolute);
    while (files.size() > 12)
        files.removeLast();
    settings.setValue(QStringLiteral("recentFiles"), files);
}

void MainWindow::showRecentMenu()
{
    QMenu menu(this);
    fillRecentMenu(&menu);
    menu.exec(m_recentButton->mapToGlobal(QPoint(0, m_recentButton->height())));
}

void MainWindow::fillRecentMenu(QMenu *menu)
{
    QSettings settings;
    const QStringList files = settings.value(QStringLiteral("recentFiles")).toStringList();
    if (files.isEmpty())
        menu->addAction(QStringLiteral("لم يُفتح أي ملف بعد"))->setEnabled(false);
    for (const QString &file : files) {
        const QFileInfo info(file);
        const bool video = isVideoFile(file);
        QAction *action = menu->addAction(Icons::icon(video ? Icons::Name::Video : Icons::Name::Image, Theme::Text, 18),
                                         QStringLiteral("\u2068%1\u2069").arg(info.fileName()), this, [this, file] {
                                             m_lastDirectory = QFileInfo(file).absolutePath();
                                             openFile(file);
                                         });
        action->setToolTip(QDir::toNativeSeparators(file));
        action->setEnabled(info.exists() && m_openImageButton->isEnabled());
    }
    if (!files.isEmpty()) {
        menu->addSeparator();
        menu->addAction(Icons::icon(Icons::Name::Trash, Theme::Text, 18), QStringLiteral("مسح القائمة"), this, [this] {
            QSettings().remove(QStringLiteral("recentFiles"));
            if (m_startScreen->isVisible())
                m_startScreen->refresh();
        });
    }
    menu->setToolTipsVisible(true);
}

void MainWindow::setVideoSettingsVisible(bool)
{
    // ما يظهر يتبع حالة العمل (صورة، فيديو، كاميرا)
    updateWorkState();
}

// يجهّز قائمة صور المجلد الذي فيه الصورة الحالية، بترتيب طبيعي (2 قبل 10) كما في مستكشف الملفات
void MainWindow::updateFolderNavigation()
{
    const QFileInfo info(m_currentImagePath);
    if (m_currentImagePath.isEmpty() || info.fileName() == QStringLiteral("malamih-clipboard.png")) {
        m_folderImages.clear();
        m_folderPath.clear();
        m_folderIndex = -1;
        m_view->setNavigation(false, false, QString());
        return;
    }
    const QString folder = info.absolutePath();
    if (folder != m_folderPath || !m_folderImages.contains(info.fileName())) {
        static const QStringList patterns = {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                                             QStringLiteral("*.bmp"), QStringLiteral("*.webp"), QStringLiteral("*.tif"),
                                             QStringLiteral("*.tiff")};
        m_folderImages = QDir(folder).entryList(patterns, QDir::Files | QDir::Readable);
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(m_folderImages.begin(), m_folderImages.end(), collator);
        m_folderPath = folder;
    }
    m_folderIndex = m_folderImages.indexOf(info.fileName());
    if (m_folderImages.size() < 2 || m_folderIndex < 0) {
        m_view->setNavigation(false, false, QString());
        return;
    }
    // \u2066…\u2069 حتى يبقى الرقمان بترتيبهما داخل النص العربي
    const QString caption = QStringLiteral("\u2066%1 / %2\u2069  ·  \u2068%3\u2069")
                                .arg(m_folderIndex + 1)
                                .arg(m_folderImages.size())
                                .arg(info.fileName());
    m_view->setNavigation(m_folderIndex > 0, m_folderIndex + 1 < m_folderImages.size(), caption);
}

void MainWindow::navigateImage(int step)
{
    if (m_videoMode || m_folderIndex < 0 || !m_openImageButton->isEnabled())
        return;
    const int next = m_folderIndex + step;
    if (next < 0 || next >= m_folderImages.size())
        return;
    analyzeImage(QDir(m_folderPath).filePath(m_folderImages.at(next)));
}

void MainWindow::showViewMenu(const QPoint &pos)
{
    if (!m_view->hasImage())
        return;
    const int faceId = m_view->faceAt(pos);
    QMenu menu(this);
    if (faceId >= 0) {
        selectFace(faceId);
        menu.addAction(Icons::icon(Icons::Name::ZoomIn, Theme::Text, 18), QStringLiteral("تكبير على هذا الوجه"), this,
                       [this, faceId] { m_view->zoomToFace(faceId); });
        menu.addAction(Icons::icon(Icons::Name::UserPlus, Theme::Text, 18), QStringLiteral("إضافة الوجه إلى قاعدة البيانات…"), this,
                       [this, faceId] {
                           const int row = m_faceRows.value(faceId, -1);
                           if (row >= 0)
                               addFaceToDatabase(m_faces.at(row));
                       })
            ->setEnabled(m_ready);
        menu.addSeparator();
    }
    if (m_view->zoom() > 1.0)
        menu.addAction(Icons::icon(Icons::Name::FitWidth, Theme::Text, 18), QStringLiteral("عرض الصورة كاملة (Ctrl+0)"), this,
                       [this] { m_view->resetZoom(); });
    QAction *labels = menu.addAction(QStringLiteral("إظهار أسماء الوجوه (L)"), this,
                                     [this](bool on) { m_view->setLabelsVisible(on); });
    labels->setCheckable(true);
    labels->setChecked(m_view->labelsVisible());
    menu.addSeparator();
    menu.addAction(Icons::icon(Icons::Name::Save, Theme::Text, 18), QStringLiteral("حفظ الصورة مع أسماء الوجوه…"), this,
                   &MainWindow::saveAnnotatedImage);
    menu.addAction(Icons::icon(Icons::Name::Image, Theme::Text, 18), QStringLiteral("نسخ الصورة مع أسماء الوجوه"), this, [this] {
        QApplication::clipboard()->setImage(m_view->annotatedImage());
        Toast::show(this, QStringLiteral("نُسخت الصورة، يمكنك لصقها في أي برنامج"));
    });
    if (!m_videoMode && !m_currentImagePath.isEmpty()) {
        menu.addSeparator();
        menu.addAction(Icons::icon(Icons::Name::Folder, Theme::Text, 18), QStringLiteral("فتح مجلد الصورة"), this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_currentImagePath).absolutePath()));
        });
        menu.addAction(Icons::icon(Icons::Name::Scan, Theme::Text, 18), QStringLiteral("إعادة التحليل (F5)"), this, [this] {
            if (m_openImageButton->isEnabled())
                analyzeImage(m_currentImagePath);
        })->setEnabled(m_openImageButton->isEnabled());
    }
    menu.exec(m_view->mapToGlobal(pos));
}

void MainWindow::saveAnnotatedImage()
{
    const QImage image = m_view->annotatedImage();
    if (image.isNull())
        return;
    const QString base = m_currentImagePath.isEmpty() || m_videoMode ? QStringLiteral("صورة")
                                                                      : QFileInfo(m_currentImagePath).completeBaseName();
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ الصورة مع أسماء الوجوه"),
                                                      QDir(m_lastDirectory).filePath(base + QStringLiteral("-الوجوه.jpg")),
                                                      QStringLiteral("صور (*.jpg *.png)"));
    if (path.isEmpty())
        return;
    if (image.save(path, nullptr, 92))
        Toast::show(this, QStringLiteral("تم حفظ الصورة"));
    else
        Toast::show(this, QStringLiteral("تعذر حفظ الصورة"), Toast::Kind::Warning);
}

// تصحيح بنقرة من قائمة المرشحين: الوجه يُضاف إلى صور الشخص، ثم تُعاد المطابقة تلقائياً
void MainWindow::assignFace(int faceId, const QString &personId)
{
    const int row = m_faceRows.value(faceId, -1);
    if (row < 0 || row >= m_faces.size() || personId.isEmpty())
        return;
    const FaceItem face = m_faces.at(row);
    const QString name = m_database.person(personId).name;
    if (enrollFace(personId, face))
        Toast::show(this, QStringLiteral("أُضيف الوجه إلى صور %1، وستتحسن مطابقته من الآن").arg(name));
    else
        Toast::show(this, QStringLiteral("تعذر إضافة الوجه"), Toast::Kind::Warning);
}

void MainWindow::openFolderScan()
{
    if (!m_ready)
        return;
    auto *dialog = new FolderScanDialog(&m_engine, &m_database, threshold(), m_lastDirectory, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &FolderScanDialog::analyzeRequested, this, [this](const QString &path) {
        m_navGroup->button(0)->click();
        if (m_openImageButton->isEnabled())
            analyzeImage(path);
    });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (!dialog->lastDirectory().isEmpty())
            m_lastDirectory = dialog->lastDirectory();
    });
    dialog->show();
    QTimer::singleShot(0, dialog, &FolderScanDialog::chooseFolder);
}

void MainWindow::openArchiveScan()
{
    if (!m_ready)
        return;
    // نافذة واحدة: المسح الجاري يبقى فيها
    if (m_archiveDialog) {
        m_archiveDialog->show();
        m_archiveDialog->raise();
        m_archiveDialog->activateWindow();
        return;
    }
    auto *dialog = new ArchiveScanDialog(&m_engine, &m_database, threshold(), m_lastDirectory, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_archiveDialog = dialog;
    connect(dialog, &ArchiveScanDialog::openRequested, this, [this](const QString &path) {
        m_navGroup->button(0)->click();
        if (m_openVideoButton->isEnabled())
            openFile(path);
    });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (!dialog->lastDirectory().isEmpty())
            m_lastDirectory = dialog->lastDirectory();
    });
    dialog->show();
}

void MainWindow::saveFaces()
{
    if (m_faces.isEmpty())
        return;
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("اختر مجلد حفظ الوجوه"));
    if (!folder.isEmpty())
        writeFaces(m_faces, folder);
}

void MainWindow::writeFaces(const QList<FaceItem> &faces, const QString &folder)
{
    // مع الصور يُحفظ تقرير CSV (يفتح في Excel) بكل وجه ونتيجته، وأوقات ظهوره في الفيديو
    QFile report(QDir(folder).filePath(QStringLiteral("تقرير_الوجوه.csv")));
    QTextStream csv(&report);
    const bool hasReport = report.open(QIODevice::WriteOnly | QIODevice::Text);
    if (hasReport) {
        csv.setGenerateByteOrderMark(true);
        csv << QStringLiteral("الرقم,الاسم,الحالة,التشابه %,جودة الوجه %,عدد الإطارات,أول ظهور,آخر ظهور,الصورة,مدة الظهور (ث),مرات الظهور\n");
    }
    auto field = [](QString text) {
        // Excel ينفّذ الخلية التي تبدأ بهذه الرموز كمعادلة؛ اسم شخص أو ملف قد يبدأ بها
        if (!text.isEmpty() && QStringLiteral("=+-@\t\r").contains(text.front()))
            text.prepend(QLatin1Char('\''));
        text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + text + QLatin1Char('"');
    };
    int saved = 0;
    for (int i = 0; i < faces.size(); ++i) {
        const FaceItem &face = faces.at(i);
        const QString name = face.match.status == MatchStatus::Unknown ? QStringLiteral("غير_معروف") : face.match.name;
        const QString fileName = QStringLiteral("وجه_%1_%2.png").arg(i + 1).arg(safeFileName(name));
        if (ImageIO::faceCrop(face).save(QDir(folder).filePath(fileName)))
            ++saved;
        if (hasReport) {
            csv << (i + 1) << ',' << field(matchLabel(face.match)) << ',' << field(statusDescription(face.match.status)) << ','
                << qRound(face.match.score * 100) << ',' << qRound(face.quality * 100) << ',' << face.frames << ','
                << (face.firstSeenMs >= 0 ? ImageIO::formatTime(face.firstSeenMs) : QString()) << ','
                << (face.lastSeenMs >= 0 ? ImageIO::formatTime(face.lastSeenMs) : QString()) << ',' << field(fileName) << ','
                << (m_videoMode ? QString::number(Report::screenTime(face.appearances) / 1000.0, 'f', 1) : QString()) << ','
                << (m_videoMode ? QString::number(face.appearances.size()) : QString()) << '\n';
        }
    }
    // وتقرير مرئي يُفتح في المتصفح ويُطبع
    Report::Source source;
    source.video = m_videoMode;
    source.path = m_videoMode ? m_videoSource : m_currentImagePath;
    source.durationMs = m_videoDurationMs;
    const QString htmlPath = QDir(folder).filePath(QStringLiteral("تقرير.html"));
    const bool html = Report::writeHtml(htmlPath, faces, source);
    bool csvSaved = false;
    if (hasReport) {
        csv.flush();
        csvSaved = csv.status() == QTextStream::Ok && report.error() == QFileDevice::NoError;
        report.close();
    }
    const QString reports = html && csvSaved ? QStringLiteral(" مع تقرير مرئي وتقرير CSV")
                            : html           ? QStringLiteral(" مع تقرير مرئي (تعذر حفظ تقرير CSV)")
                            : csvSaved       ? QStringLiteral(" مع تقرير CSV")
                                             : QStringLiteral(" (تعذر حفظ التقارير)");
    Toast::show(this, QStringLiteral("تم حفظ %1 وجه").arg(saved) + reports,
                html && csvSaved ? Toast::Kind::Success : Toast::Kind::Warning);
    if (html)
        QDesktopServices::openUrl(QUrl::fromLocalFile(htmlPath));
}

void MainWindow::addSelectedFaceToDatabase()
{
    const int row = m_faceList->currentRow();
    if (row < 0 || row >= m_faces.size())
        return;
    addFaceToDatabase(m_faces.at(row));
}

// يختار شخصاً من القاعدة أو ينشئ شخصاً جديداً؛ فارغ عند الإلغاء
QString MainWindow::choosePerson(const QString &suggestedId)
{
    PersonPickerDialog picker(&m_database, suggestedId, this);
    if (picker.exec() != QDialog::Accepted)
        return {};
    QString personId = picker.personId();
    if (personId.isEmpty()) {
        PersonDialog dialog(Person(), this);
        if (dialog.exec() != QDialog::Accepted)
            return {};
        personId = m_database.addPerson(dialog.person());
        if (personId.isEmpty())
            Toast::show(this, QStringLiteral("تعذر حفظ الشخص"), Toast::Kind::Warning);
    }
    return personId;
}

void MainWindow::addFaceToDatabase(FaceItem face)
{
    const QString personId = choosePerson(face.match.personId);
    if (personId.isEmpty())
        return;
    if (enrollFace(personId, face))
        Toast::show(this, QStringLiteral("أُضيف الوجه إلى %1").arg(m_database.person(personId).name));
    else
        Toast::show(this, QStringLiteral("تعذر إضافة الوجه"), Toast::Kind::Warning);
}

bool MainWindow::enrollFace(const QString &personId, const FaceItem &face)
{
    const cv::Mat aligned = ImageIO::faceAligned(face);
    if (aligned.empty())
        return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const FaceEngine *engine = &m_engine;
    const std::vector<std::vector<float>> embeddings = runResponsive([engine, aligned] { return engine->enrollmentEmbeddings(aligned); });
    const bool ok = m_database.addPhoto(personId, ImageIO::faceCrop(face), embeddings, face.quality);
    QApplication::restoreOverrideCursor();
    return ok;
}

int MainWindow::enrollFaces(const QString &personId, const QList<FaceItem> &faces)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const FaceEngine *engine = &m_engine;
    int added = 0;
    m_database.beginBatch();
    for (const FaceItem &face : faces) {
        const cv::Mat aligned = ImageIO::faceAligned(face);
        if (aligned.empty())
            continue;
        const auto embeddings = runResponsive([engine, aligned] { return engine->enrollmentEmbeddings(aligned); });
        added += m_database.addPhoto(personId, ImageIO::faceCrop(face), embeddings, face.quality) ? 1 : 0;
    }
    m_database.endBatch();
    QApplication::restoreOverrideCursor();
    return added;
}

// تعزيز صور وجه ظهر في الفيديو الحالي: يجمع أوضح لقطاته من فترات ظهوره
void MainWindow::harvestFromFace(int faceId)
{
    const int row = m_faceRows.value(faceId, -1);
    if (row < 0 || row >= m_faces.size() || m_videoSource.isEmpty() || m_videoSource.startsWith(QStringLiteral("camera:")))
        return;
    const FaceItem face = m_faces.at(row);
    const QString personId = face.match.status == MatchStatus::Confirmed ? face.match.personId : choosePerson(face.match.personId);
    if (personId.isEmpty())
        return;
    const Person person = m_database.person(personId);
    QList<QPair<qint64, qint64>> ranges = face.appearances;
    if (ranges.isEmpty() && face.firstSeenMs >= 0)
        ranges << qMakePair(face.firstSeenMs, face.lastSeenMs);
    HarvestDialog dialog(&m_engine, m_videoSource, face.embedding, ranges, threshold(), person.name, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const int added = enrollFaces(personId, dialog.selectedFaces());
    Toast::show(this, QStringLiteral("أضيفت %1 صور إلى ملف %2").arg(added).arg(person.name),
                added > 0 ? Toast::Kind::Success : Toast::Kind::Warning);
}

// يسجل في ملف كل شخص مؤكد أنه ظهر في هذا الملف (مرة لكل شخص بأعلى تشابه)
void MainWindow::recordSightings(const QString &source)
{
    if (source.isEmpty() || source.startsWith(QStringLiteral("camera:")))
        return;
    QHash<QString, int> best;
    for (int i = 0; i < m_faces.size(); ++i) {
        const FaceItem &face = m_faces.at(i);
        if (face.match.status != MatchStatus::Confirmed || face.match.personId.isEmpty())
            continue;
        const auto it = best.constFind(face.match.personId);
        if (it == best.constEnd() || m_faces.at(*it).match.score < face.match.score)
            best.insert(face.match.personId, i);
    }
    if (best.isEmpty())
        return;
    m_database.beginBatch();
    for (auto it = best.cbegin(); it != best.cend(); ++it) {
        const FaceItem &face = m_faces.at(it.value());
        m_database.addSighting(it.key(), source, m_videoMode ? face.bestMs : -1, face.match.score, ImageIO::faceCrop(face));
    }
    m_database.endBatch();
}

// ───────────── قاعدة البيانات ─────────────

// ───────────── تحسين الصور والخط الزمني ─────────────

void MainWindow::openEnhancer()
{
    if (!m_videoMode && m_currentImagePath.isEmpty()) {
        m_navGroup->button(3)->click();
        return;
    }
    if (m_videoMode) {
        if (!m_videoSource.isEmpty() && !m_videoSource.startsWith(QStringLiteral("camera:")))
            enhanceVideoFrame(std::max<qint64>(0, m_timeline->position()));
        return;
    }
    if (m_currentImagePath.isEmpty())
        return;
    const cv::Mat image = ImageIO::loadImage(m_currentImagePath);
    if (image.empty())
        return;
    EnhanceDialog dialog(image, &m_engine, QStringLiteral("تحسين الصورة — %1").arg(QFileInfo(m_currentImagePath).fileName()),
                         QStringLiteral("تحليل الصورة المحسّنة"), this);
    if (dialog.exec() != QDialog::Accepted || dialog.result().empty())
        return;
    const QString path = MatchPage::enhancedPath(m_currentImagePath);
    if (ImageIO::toQImage(dialog.result()).save(path))
        analyzeImage(path);
}

void MainWindow::enhanceVideoFrame(qint64 positionMs)
{
    const QString source = m_videoSource;
    const cv::Mat frame = runResponsive([source, positionMs] { return VideoExport::frameAt(source, positionMs); });
    if (frame.empty()) {
        Toast::show(this, QStringLiteral("تعذر قراءة الإطار"), Toast::Kind::Warning);
        return;
    }
    EnhanceDialog dialog(frame, &m_engine, QStringLiteral("تحسين الإطار عند %1").arg(ImageIO::formatTime(positionMs)),
                         QStringLiteral("البحث عنه في القاعدة"), this);
    if (dialog.exec() != QDialog::Accepted || dialog.result().empty())
        return;
    const QString path = MatchPage::enhancedPath(QStringLiteral("إطار_%1").arg(positionMs));
    if (ImageIO::toQImage(dialog.result()).save(path)) {
        m_navGroup->button(2)->click();
        m_matchPage->searchImage(path);
    }
}

void MainWindow::openVideoFilters()
{
    const bool fileVideo = m_videoMode && !m_videoSource.isEmpty() && !m_videoSource.startsWith(QStringLiteral("camera:"));
    cv::Mat frame;
    if (fileVideo) {
        const QString source = m_videoSource;
        const qint64 position = m_videoRunning ? m_videoPositionMs : std::max<qint64>(0, m_timeline->position());
        frame = runResponsive([source, position] { return VideoExport::frameAt(source, position, false); });
    } else if (m_videoRunning && m_videoSource.startsWith(QStringLiteral("camera:"))) {
        frame = m_videoWorker->lastCameraFrame();
    }
    const VideoFilters::Settings before = VideoFilters::current();
    const QString cameraId = m_cameraId;
    VideoFiltersDialog dialog(frame, this);
    // كاميرا محفوظة تحتفظ بمعالجاتها لنفسها دون أن تغيّر معالجات الملفات
    dialog.setPersistent(cameraId.isEmpty());
    if (!cameraId.isEmpty())
        dialog.setWindowTitle(QStringLiteral("معالجات الكاميرا «%1»").arg(m_cameraName));
    connect(&dialog, &VideoFiltersDialog::changed, this, &MainWindow::updateFiltersButton);
    dialog.exec();
    const VideoFilters::Settings after = VideoFilters::current();
    if (after == before)
        return;
    if (!cameraId.isEmpty()) {
        Cameras::saveFilters(cameraId, after);
        if (m_cameraId != cameraId) // انتهت الكاميرا أثناء فتح النافذة
            VideoFilters::setCurrent(m_fileFilters, false);
        Toast::show(this, QStringLiteral("حُفظت المعالجات مع الكاميرا «%1»").arg(m_cameraName), Toast::Kind::Info);
        return;
    }
    if (filePlayerOpen() && !m_player->isPlaying()) {
        m_player->seek(m_player->position()); // يُعاد عرض الإطار المتوقف بالمعالجات الجديدة
    }
    if (fileVideo && !m_videoRunning) {
        if (!filePlayerOpen())
            loadVideoFrame(std::max<qint64>(0, m_timeline->position()));
        Toast::show(this, QStringLiteral("تغيّرت معالجات الفيديو · «إعادة التحليل» يطبقها على الفيديو كله"), Toast::Kind::Info);
    } else if (m_videoRunning) {
        Toast::show(this, QStringLiteral("تُطبَّق المعالجات الجديدة على بقية الفيديو"), Toast::Kind::Info);
    }
}

void MainWindow::updateFiltersButton()
{
    const VideoFilters::Settings filters = VideoFilters::current();
    const int count = filters.activeCount();
    m_filtersButton->setText(count > 0 ? QStringLiteral("معالجات (%1)").arg(count) : QStringLiteral("معالجات"));
    const QString summary = filters.summary();
    m_filtersButton->setToolTip(QStringLiteral("معالجات الفيديو وخيارات التحليل: تدوير، مرآة، إضاءة، تباين، تشويش، حدة،\n"
                                               "إزالة التشابك، أصغر وجه، وتخطي الإطارات الساكنة")
                                + (summary.isEmpty() ? QString() : QStringLiteral("\n\nالمفعّل: ") + summary));
}

void MainWindow::setTrimRange(qint64 startMs, qint64 endMs)
{
    const QSignalBlocker blockStart(m_rangeStart), blockEnd(m_rangeEnd);
    m_rangeStart->setTime(QTime(0, 0).addMSecs(int(startMs)));
    m_rangeEnd->setTime(QTime(0, 0).addMSecs(int(std::max<qint64>(0, endMs))));
    m_timeline->setRange(startMs, endMs);
    if (startMs <= 0 && endMs <= 0)
        statusBar()->showMessage(QStringLiteral("أُلغي الاقتطاع: سيُحلل الفيديو كله"), 5000);
    else
        statusBar()->showMessage(QStringLiteral("الجزء المحدد: %1 ← %2 · «إعادة التحليل» يحلله، والزر الأيمن على الخط الزمني يصدّره")
                                     .arg(ImageIO::formatTime(startMs),
                                          endMs > 0 ? ImageIO::formatTime(endMs) : QStringLiteral("النهاية")),
                                 8000);
}

void MainWindow::showTimelineMenu(const QPoint &globalPos, qint64 positionMs, int laneId)
{
    const bool fileVideo = !m_videoSource.isEmpty() && !m_videoSource.startsWith(QStringLiteral("camera:"));
    if (!fileVideo)
        return;
    const qint64 start = m_rangeStart->time().msecsSinceStartOfDay();
    const qint64 end = m_rangeEnd->time().msecsSinceStartOfDay();
    const bool trimmed = start > 0 || end > 0;
    const bool idle = m_ready && !m_analyzing && !m_videoRunning;
    const QString at = ImageIO::formatTime(positionMs);

    QMenu menu(this);
    auto icon = [](Icons::Name name) { return Icons::icon(name, Theme::Accent2, 18); };
    menu.addSection(QStringLiteral("الاقتطاع"));
    menu.addAction(icon(Icons::Name::Previous), QStringLiteral("بداية الجزء هنا (%1)\tI").arg(at), this, [this, positionMs, end] {
        setTrimRange(positionMs, end > positionMs ? end : 0);
    });
    menu.addAction(icon(Icons::Name::Next), QStringLiteral("نهاية الجزء هنا (%1)\tO").arg(at), this, [this, positionMs, start] {
        setTrimRange(start < positionMs ? start : 0, positionMs);
    });
    menu.addAction(icon(Icons::Name::Stop), QStringLiteral("إلغاء الاقتطاع"), this, [this] { setTrimRange(0, 0); })->setEnabled(trimmed);
    menu.addSection(QStringLiteral("العمليات"));
    menu.addAction(icon(Icons::Name::Film), trimmed ? QStringLiteral("تحليل الجزء المحدد") : QStringLiteral("تحليل الفيديو كله"), this,
                   &MainWindow::reanalyzeVideo)
        ->setEnabled(idle);
    menu.addAction(icon(Icons::Name::Play), QStringLiteral("تحليل الفيديو من هنا"), this, [this, positionMs] { analyzeVideoFrom(positionMs); })
        ->setEnabled(m_ready && !m_analyzing);
    menu.addAction(icon(Icons::Name::Export), trimmed ? QStringLiteral("تصدير الجزء المحدد كفيديو…") : QStringLiteral("تصدير الفيديو…"), this,
                   &MainWindow::exportTrimmedClip);
    menu.addAction(icon(Icons::Name::Image), QStringLiteral("حفظ الإطار هنا كصورة…"), this, [this, positionMs] { saveVideoFrame(positionMs); });
    menu.addAction(icon(Icons::Name::Sliders), QStringLiteral("معالجات الفيديو…"), this, &MainWindow::openVideoFilters);
    menu.addAction(icon(Icons::Name::Sparkle), QStringLiteral("تحسين هذا الإطار وتوضيحه…"), this, [this, positionMs] { enhanceVideoFrame(positionMs); })
        ->setEnabled(m_ready);
    menu.addSection(QStringLiteral("العلامات"));
    const qint64 marker = markerNear(positionMs);
    if (marker >= 0) {
        const QString note = m_markers.value(marker);
        const QString title = note.isEmpty() ? ImageIO::formatTime(marker) : QStringLiteral("%1 — %2").arg(ImageIO::formatTime(marker), note);
        menu.addAction(icon(Icons::Name::Flag), QStringLiteral("تعديل ملاحظة العلامة (%1)…").arg(title), this,
                       [this, marker] { editMarker(marker); });
        menu.addAction(icon(Icons::Name::Stop), QStringLiteral("حذف هذه العلامة"), this, [this, marker] { removeMarker(marker); });
    } else {
        menu.addAction(icon(Icons::Name::Flag), QStringLiteral("إضافة علامة هنا (%1)…\tB").arg(at), this,
                       [this, positionMs] { addMarker(positionMs); });
    }

    if (laneId >= 0) {
        const auto face = std::find_if(m_faces.cbegin(), m_faces.cend(), [laneId](const FaceItem &f) { return f.id == laneId; });
        if (face != m_faces.cend()) {
            const QString name = matchLabel(face->match);
            QList<QPair<qint64, qint64>> segments = face->appearances;
            if (segments.isEmpty() && face->firstSeenMs >= 0)
                segments.append({face->firstSeenMs, std::max(face->firstSeenMs + 1000, face->lastSeenMs)});
            menu.addSection(name);
            menu.addAction(icon(Icons::Name::Scan), QStringLiteral("لقطات %1").arg(name), this, [this, laneId, positionMs] {
                openShotViewer(laneId, positionMs);
            });
            menu.addAction(icon(Icons::Name::Export), QStringLiteral("تصدير كل ظهور %1 في مقطع واحد…").arg(name), this, [this, segments, name] {
                // هامش صغير قبل كل ظهور وبعده حتى لا يبدأ المقطع فجأة
                QList<QPair<qint64, qint64>> padded;
                for (const auto &segment : segments) {
                    const qint64 a = std::max<qint64>(0, segment.first - 500);
                    const qint64 b = std::min(m_videoDurationMs > 0 ? m_videoDurationMs : segment.second + 500, segment.second + 500);
                    if (!padded.isEmpty() && a <= padded.last().second)
                        padded.last().second = std::max(padded.last().second, b);
                    else
                        padded.append({a, b});
                }
                exportClip(padded, QStringLiteral("%1_ظهور_%2").arg(QFileInfo(m_videoSource).completeBaseName(), name));
            })->setEnabled(!segments.isEmpty());
        }
    }
    menu.exec(globalPos);
}

void MainWindow::exportTrimmedClip()
{
    if (m_videoSource.isEmpty() || m_videoSource.startsWith(QStringLiteral("camera:")))
        return;
    const qint64 start = m_rangeStart->time().msecsSinceStartOfDay();
    const qint64 end = m_rangeEnd->time().msecsSinceStartOfDay();
    const qint64 last = end > start ? end : m_videoDurationMs > 0 ? m_videoDurationMs : m_player->durationMs();
    if (last <= start)
        return;
    exportClip({{start, last}}, QStringLiteral("%1_%2-%3").arg(QFileInfo(m_videoSource).completeBaseName(),
                                                            ImageIO::formatTime(start).remove(QLatin1Char(':')),
                                                            ImageIO::formatTime(last).remove(QLatin1Char(':'))));
}

void MainWindow::exportClip(const QList<QPair<qint64, qint64>> &segments, const QString &suggestedName)
{
    if (segments.isEmpty())
        return;
    QString name = suggestedName;
    name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("تصدير مقطع فيديو"),
                                                      QDir(m_lastDirectory).filePath(name + QStringLiteral(".mp4")),
                                                      QStringLiteral("فيديو MP4 (*.mp4)"));
    if (path.isEmpty())
        return;
    bool filtered = false;
    if (!VideoFilters::current().isIdentity()) {
        QMessageBox ask(QMessageBox::Question, QStringLiteral("تصدير مقطع"),
                        QStringLiteral("معالجات الفيديو مفعّلة (%1).\nهل يُصدَّر المقطع بعد المعالجة أم كما في الملف الأصلي؟")
                            .arg(VideoFilters::current().summary(false)),
                        QMessageBox::NoButton, this);
        QPushButton *processed = ask.addButton(QStringLiteral("بعد المعالجة"), QMessageBox::AcceptRole);
        ask.addButton(QStringLiteral("الأصلي"), QMessageBox::RejectRole);
        QPushButton *cancel = ask.addButton(QStringLiteral("إلغاء"), QMessageBox::DestructiveRole);
        ask.setDefaultButton(processed);
        ask.exec();
        if (ask.clickedButton() == cancel)
            return;
        filtered = ask.clickedButton() == processed;
    }
    auto progress = std::make_shared<VideoExport::Progress>();
    QProgressDialog dialog(QStringLiteral("جارٍ تصدير المقطع… (الصوت لا يُنسخ)"), QStringLiteral("إلغاء"), 0, 1000, this);
    dialog.setWindowTitle(QStringLiteral("تصدير مقطع"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(0);
    QFutureWatcher<QString> watcher;
    QTimer timer;
    connect(&timer, &QTimer::timeout, &dialog, [&] {
        dialog.setValue(int(1000 * progress->done / std::max<qint64>(1, progress->total)));
    });
    connect(&dialog, &QProgressDialog::canceled, &dialog, [progress] { progress->cancel = true; });
    QEventLoop loop;
    connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    const QString source = m_videoSource;
    watcher.setFuture(QtConcurrent::run([source, segments, path, progress, filtered] {
        return VideoExport::exportSegments(source, segments, path, progress.get(), filtered);
    }));
    timer.start(150);
    loop.exec();
    timer.stop();
    dialog.reset();
    const QString error = watcher.result();
    if (error.isEmpty()) {
        Toast::show(this, QStringLiteral("تم تصدير المقطع: %1").arg(QFileInfo(path).fileName()));
        statusBar()->showMessage(QDir::toNativeSeparators(path), 8000);
    } else {
        Toast::show(this, error, Toast::Kind::Warning);
    }
}

void MainWindow::saveVideoFrame(qint64 positionMs)
{
    const QString source = m_videoSource;
    const cv::Mat frame = runResponsive([source, positionMs] { return VideoExport::frameAt(source, positionMs); });
    if (frame.empty()) {
        Toast::show(this, QStringLiteral("تعذر قراءة الإطار"), Toast::Kind::Warning);
        return;
    }
    const QString suggested = QStringLiteral("إطار_%1.png").arg(ImageIO::formatTime(positionMs).replace(QLatin1Char(':'), QLatin1Char('-')));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ الإطار"), QDir(m_lastDirectory).filePath(suggested),
                                                      QStringLiteral("صور (*.png *.jpg)"));
    if (path.isEmpty())
        return;
    if (ImageIO::toQImage(frame).save(path))
        Toast::show(this, QStringLiteral("تم حفظ الإطار"));
    else
        Toast::show(this, QStringLiteral("تعذر حفظ الإطار"), Toast::Kind::Warning);
}


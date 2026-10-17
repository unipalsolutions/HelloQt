// النافذة الرئيسية: الكاميرا الواحدة وتسجيلها، والتنبيهات، والعمل في الخلفية والإعدادات

#include "mainwindow_p.h"

using namespace MainWindowHelpers;

void MainWindow::verifyGpu(bool interactive)
{
    if (m_gpuChecking)
        return;
    m_gpuChecking = true;
    if (m_gpuCheckButton)
        m_gpuCheckButton->setEnabled(false);
    if (interactive)
        Toast::show(this, QStringLiteral("جارٍ فحص كرت الشاشة والمعالج… (قد يستغرق دقيقة)"), Toast::Kind::Info);
    else
        Toast::show(this, QStringLiteral("جارٍ فحص كرت الشاشة في الخلفية قبل استخدامه…"), Toast::Kind::Info);
    // في عملية منفصلة: إن انهار تعريف الكرت يسقط الفحص وحده ويبقى البرنامج يعمل على المعالج
    GpuCheck::runIsolated(this, [this, interactive](const GpuCheck::Result &result, bool crashed) {
        m_gpuChecking = false;
        if (m_gpuCheckButton)
            m_gpuCheckButton->setEnabled(true);
        const QString gpu = FaceEngine::gpuIdentity();
        const bool reliable = !crashed && result.reliable();
        if (!gpu.isEmpty())
            Performance::setGpuVerified(gpu, reliable);
        AppLog::write(QStringLiteral("فحص كرت الشاشة: %1").arg(crashed ? result.error : result.summary()));
        // كرت لا يصلح يُطفأ مفتاحه، فلا يُعاد فحصه في كل تشغيل؛ تفعيله يدوياً يعيد الفحص
        if (!gpu.isEmpty() && !reliable && Performance::gpuEnabled()) {
            const QSignalBlocker blocker(m_gpuSwitch);
            m_gpuSwitch->setChecked(false);
            Performance::setGpuEnabled(false);
        }
        QString text = crashed ? result.error : result.summary();
        if (!gpu.isEmpty() && !reliable)
            text += QStringLiteral("\n\nأُوقف كرت الشاشة ويبقى التحليل على المعالج.");
        else if (reliable && Performance::gpuEnabled() && m_engine.gpuName().isEmpty())
            text += QStringLiteral("\n\nاجتاز كرت الشاشة الفحص، وينتقل التحليل إليه الآن.");
        else if (reliable && !Performance::gpuEnabled())
            text += QStringLiteral("\n\nكرت الشاشة متوقف في الإعدادات؛ فعّله من مفتاح «كرت الشاشة».");
        if (interactive) {
            if (!gpu.isEmpty() && !reliable)
                QMessageBox::warning(this, QStringLiteral("فحص كرت الشاشة"), text);
            else
                QMessageBox::information(this, QStringLiteral("فحص كرت الشاشة"), text);
        } else {
            Toast::show(this, text.simplified(), reliable ? Toast::Kind::Info : Toast::Kind::Warning);
        }
        applyEngineDevice();
    });
}

void MainWindow::applyEngineDevice()
{
    if (!m_ready || m_deviceWatcher.isRunning())
        return;
    const bool wantGpu = Performance::gpuEnabled() && Performance::gpuVerified(FaceEngine::gpuIdentity());
    if (wantGpu == !m_engine.gpuName().isEmpty())
        return;
    // التحليل الجاري لا يتوقف: يكمل على الجلستين القديمتين حتى لحظة التبديل
    FaceEngine *engine = &m_engine;
    const FaceEngine::Device device = wantGpu ? FaceEngine::Device::Auto : FaceEngine::Device::Cpu;
    if (m_deviceLabel)
        m_deviceLabel->setText(wantGpu ? QStringLiteral("جارٍ نقل التحليل إلى كرت الشاشة…") : QStringLiteral("جارٍ نقل التحليل إلى المعالج…"));
    m_deviceWatcher.setFuture(QtConcurrent::run([engine, device] {
        QString error;
        engine->switchDevice(device, &error);
        return error;
    }));
}

void MainWindow::updateDeviceLabel()
{
    if (!m_deviceLabel || m_deviceWatcher.isRunning())
        return;
    const QString gpu = m_engine.gpuName();
    QString text = gpu.isEmpty() ? QStringLiteral("التحليل: المعالج") : QStringLiteral("التحليل: %1").arg(gpu);
    if (m_videoRunning)
        text += QStringLiteral(" · %1 إطار/ث").arg(m_analyzedFrames);
    m_analyzedFrames = 0;
    // لماذا ليس على الكرت، حين يوجد كرت يُفترض أن يُستخدم (البحث عن الكرت مرة واحدة: يتكرر كل ثانية)
    static const bool hasGpu = !FaceEngine::availableGpu().isEmpty();
    QString why;
    const QString fallback = m_engine.fallbackReason();
    if (gpu.isEmpty() && !fallback.isEmpty())
        why = QStringLiteral("رجع التحليل إلى المعالج بعد عطل في كرت الشاشة: %1").arg(fallback);
    else if (gpu.isEmpty() && hasGpu && Performance::gpuEnabled())
        why = m_gpuChecking ? QStringLiteral("كرت الشاشة قيد الفحص، وينتقل التحليل إليه فور اجتيازه")
                            : QStringLiteral("كرت الشاشة لم يجتز الفحص؛ أعد فحصه من «استهلاك الموارد»");
    else if (gpu.isEmpty() && hasGpu)
        why = QStringLiteral("كرت الشاشة متوقف من مفتاح «كرت الشاشة»");
    const bool warn = !fallback.isEmpty() && gpu.isEmpty();
    m_deviceLabel->setText(warn ? QStringLiteral("⚠ ") + text : text);
    m_deviceLabel->setToolTip(why.isEmpty() ? QStringLiteral("مكان تحليل الوجوه وعدد الإطارات المحللة في الثانية") : why);
    const QString style = QStringLiteral("color: %1; padding: 0 8px;").arg((warn ? Theme::Warning : Theme::Muted).name());
    if (m_deviceLabel->styleSheet() != style)
        m_deviceLabel->setStyleSheet(style);
    if (warn && fallback != m_shownFallback) {
        m_shownFallback = fallback;
        AppLog::write(QStringLiteral("[تحذير] %1").arg(why));
        Toast::show(this, why, Toast::Kind::Warning);
    }
}

void MainWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
    m_tray->setToolTip(QStringLiteral("ملامح — التعرف على الوجوه"));
    auto *menu = new QMenu(this);
    menu->addAction(QStringLiteral("إظهار ملامح"), this, &MainWindow::showFromTray);
    QAction *camera = menu->addAction(QStringLiteral("تشغيل آخر كاميرا"), this, [this] {
        showFromTray();
        if (m_cameraButton->isEnabled())
            startLastCamera();
    });
    menu->addAction(QStringLiteral("عدة كاميرات معاً"), this, [this] { openMultiCamera(); });
    QAction *stop = menu->addAction(QStringLiteral("إيقاف الكاميرا"), this, [this] { stopVideo(); });
    QAction *record = menu->addAction(QStringLiteral("تسجيل"), this, [this] { toggleRecording(); });
    menu->addSeparator();
    addRecordsMenu(menu->addMenu(QStringLiteral("السجلات والتقارير")));
    menu->addAction(QStringLiteral("الإعدادات…"), this, &MainWindow::openSettings);
    menu->addSeparator();
    menu->addAction(QStringLiteral("خروج"), this, &MainWindow::quitApplication);
    connect(menu, &QMenu::aboutToShow, this, [this, camera, stop, record] {
        const bool running = m_videoRunning && m_videoSource.startsWith(QStringLiteral("camera:"));
        camera->setEnabled(m_cameraButton->isEnabled());
        stop->setVisible(running);
        record->setVisible(running);
        record->setText(m_recorder->isRecording() ? QStringLiteral("إيقاف التسجيل") : QStringLiteral("تسجيل الكاميرا"));
    });
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            showFromTray();
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, &MainWindow::showFromTray);
    // تنبيه المراقبة يصل إشعاراً من ويندوز حين تكون النافذة مخفية أو مصغرة
    connect(this, &MainWindow::watchAlert, this, [this](const QString &name, const QString &camera, int level) {
        m_tray->setToolTip(QStringLiteral("ملامح — آخر تنبيه: %1 الساعة %2").arg(name, QTime::currentTime().toString(QStringLiteral("HH:mm"))));
        // «إشعار فقط» لا يظهر في النافذة، فيُرسل الإشعار ولو كانت أمام المستخدم
        if (level != AlertSound::NotifyOnly && isVisible() && !isMinimized() && isActiveWindow())
            return;
        m_tray->showMessage(QStringLiteral("تنبيه مراقبة: ظهر %1").arg(name),
                            camera.isEmpty() ? QStringLiteral("انقر لعرض التفاصيل")
                                             : QStringLiteral("أمام الكاميرا «%1» — انقر لعرضه").arg(camera),
                            QSystemTrayIcon::Warning, level == AlertSound::High ? 60000 : 15000);
        QApplication::alert(this);
    });
    m_tray->show();
}

void MainWindow::activateFromOtherInstance(const QString &message)
{
    if (message == QLatin1String("startup"))
        return;
    showFromTray();
    if (message.startsWith(QLatin1String("open:"))) {
        const QString path = message.mid(5);
        if (m_ready && (m_videoRunning || m_analyzing))
            Toast::show(this, QStringLiteral("البرنامج مشغول الآن؛ افتح الملف بعد انتهاء العمل الحالي"), Toast::Kind::Warning);
        else
            openFile(path);
    } else {
        statusBar()->showMessage(QStringLiteral("ملامح يعمل أصلاً؛ هذه نافذته"), 5000);
    }
}

void MainWindow::showFromTray()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void MainWindow::quitApplication()
{
    m_quitting = true;
    showFromTray(); // النافذة المخفية لا تُغلق فلا يخرج البرنامج
    close();
}

void MainWindow::openSettings()
{
    showFromTray();
    SettingsDialog dialog(&m_database, this);
    connect(&dialog, &SettingsDialog::editRulesRequested, this, &MainWindow::editCameraRules);
    connect(&dialog, &SettingsDialog::backupRequested, this, [this] {
        if (m_maintenance->isBackupRunning())
            Toast::show(this, QStringLiteral("نسخة احتياطية جارية الآن"), Toast::Kind::Info);
        else
            m_maintenance->backupNow();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QSignalBlocker blocker(m_autoRecordSwitch);
    m_autoRecordSwitch->setChecked(CameraRecorder::autoRecord());
    // مدد الاحتفاظ الجديدة تُطبَّق الآن لا بعد ساعة
    m_maintenance->runNow();
    statusBar()->showMessage(QStringLiteral("حُفظت الإعدادات"), 5000);
}

void MainWindow::openCamera()
{
    QMenu menu(this);
    const auto icon = [](Icons::Name name) { return Icons::icon(name, Theme::Accent2, 18); };
    const QList<Cameras::Camera> saved = Cameras::load();
    const QList<Cameras::LocalDevice> devices = Cameras::localDevices();
    const QString last = Cameras::lastSource();
    const auto addCamera = [&](const QString &source, const QString &title, const QString &detail, Icons::Name iconName) {
        QAction *action = menu.addAction(icon(iconName), detail.isEmpty() ? title : QStringLiteral("%1  —  %2").arg(title, detail));
        if (source == last) {
            QFont font = action->font();
            font.setBold(true);
            action->setFont(font);
        }
        connect(action, &QAction::triggered, this, [this, source] { startVideo(source); });
    };
    for (const Cameras::Camera &camera : saved)
        addCamera(camera.source(), camera.name, camera.description(),
                  camera.kind == Cameras::Kind::Network ? Icons::Name::Video : Icons::Name::Camera);
    // الكاميرات الموصولة التي لم تُحفظ تظهر مباشرة بأسمائها
    bool anyLocal = false;
    for (const Cameras::LocalDevice &device : devices) {
        const bool known = std::any_of(saved.begin(), saved.end(), [&](const Cameras::Camera &c) {
            return c.kind == Cameras::Kind::Local && c.index == device.index;
        });
        anyLocal = anyLocal || known;
        if (!known) {
            addCamera(QStringLiteral("camera:%1").arg(device.index), Cameras::localName(device.index, devices),
                      QStringLiteral("موصولة بالجهاز"), Icons::Name::Camera);
            anyLocal = true;
        }
    }
    if (!anyLocal && devices.isEmpty())
        addCamera(QStringLiteral("camera:0"), QStringLiteral("كاميرا الجهاز"), QString(), Icons::Name::Camera);
    menu.addSeparator();
    menu.addAction(icon(Icons::Name::Users), QStringLiteral("عدة كاميرات معاً (شبكة مراقبة)…"), this, [this] { openMultiCamera(); });
    menu.addAction(icon(Icons::Name::Sliders), QStringLiteral("إدارة الكاميرات وإضافة كاميرا شبكة…"), this, [this] { manageCameras(); });
    // قواعد كل كاميرا: المناطق داخل الكادر وساعات التنبيه والتسجيل
    QMenu *rulesMenu = menu.addMenu(icon(Icons::Name::Region), QStringLiteral("مناطق وجداول الكاميرات"));
    const auto addRules = [&](const QString &source, const QString &name) {
        const CameraRules::Rules rules = CameraRules::load(source);
        QAction *action = rulesMenu->addAction(rules.isEmpty() ? name : QStringLiteral("%1  —  %2").arg(name, rules.summary()));
        connect(action, &QAction::triggered, this, [this, source, name] { editCameraRules(source, name); });
    };
    for (const Cameras::Camera &camera : saved)
        addRules(camera.source(), camera.name);
    for (const Cameras::LocalDevice &device : devices) {
        const bool known = std::any_of(saved.begin(), saved.end(), [&](const Cameras::Camera &c) {
            return c.kind == Cameras::Kind::Local && c.index == device.index;
        });
        if (!known)
            addRules(QStringLiteral("camera:%1").arg(device.index), Cameras::localName(device.index, devices));
    }
    if (rulesMenu->isEmpty())
        addRules(QStringLiteral("camera:0"), QStringLiteral("كاميرا الجهاز"));
    menu.addSeparator();
    addRecordsMenu(menu.addMenu(icon(Icons::Name::History), QStringLiteral("السجلات والتقارير")));
    menu.addAction(icon(Icons::Name::Sliders), QStringLiteral("الإعدادات…"), this, &MainWindow::openSettings);
    menu.exec(m_cameraButton->mapToGlobal(QPoint(0, m_cameraButton->height())));
}

void MainWindow::editCameraRules(const QString &source, const QString &name)
{
    const bool running = m_videoRunning && m_videoSource == source;
    CameraRulesDialog dialog(source, name, running ? m_view->image() : QImage(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const CameraRules::Rules rules = dialog.rules();
    AppLog::write(QStringLiteral("قواعد الكاميرا «%1»: %2").arg(name, rules.summary()));
    if (m_multiCamera && m_multiCamera->isRunning())
        m_multiCamera->reloadRules(source);
    if (!running) {
        Toast::show(this, QStringLiteral("حُفظت قواعد «%1»").arg(name));
        return;
    }
    // الكاميرا تعمل: المناطق وساعات التنبيه تسري فوراً، وجدول التسجيل بإعادة بدء التسجيل
    m_cameraWatch->reloadRules();
    m_videoWorker->setZones(m_cameraWatch->rules());
    if (m_recorder->isRecording()) {
        stopRecording();
        startRecording();
    }
    Toast::show(this, QStringLiteral("حُفظت قواعد «%1» وتسري الآن").arg(name));
}

void MainWindow::startLastCamera()
{
    const QString source = Cameras::lastSource();
    Cameras::Camera camera;
    if (!Cameras::resolve(source.mid(7), &camera)) {
        openCamera();
        return;
    }
    startVideo(source);
}

void MainWindow::openMultiCamera()
{
    if (!m_ready) {
        statusBar()->showMessage(QStringLiteral("انتظر حتى يجهز محرك التعرف"), 5000);
        return;
    }
    showFromTray();
    if (!m_multiCamera) {
        m_multiCamera = new MultiCameraWindow(&m_engine, &m_database, [this] { return threshold(); }, this);
        m_multiCamera->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_multiCamera, &MultiCameraWindow::watchAlert, this, &MainWindow::watchAlert);
        connect(m_multiCamera, &MultiCameraWindow::systemAlert, this, &MainWindow::systemAlert);
        connect(m_multiCamera, &MultiCameraWindow::strangerAlert, this, &MainWindow::showStrangerAlert);
    }
    m_multiCamera->setGallery(m_database.gallery());
    m_multiCamera->setWatched(m_watched);
    if (!m_multiCamera->isRunning()) {
        const QStringList sources = m_multiCamera->chooseCameras();
        if (sources.isEmpty())
            return;
        // الكاميرا الموصولة بالجهاز لا تُفتح مرتين: عرض الكاميرا الواحدة يتوقف أولاً
        if (m_videoRunning && m_videoSource.startsWith(QStringLiteral("camera:")))
            stopVideo();
        m_multiCamera->start(sources);
    }
    m_multiCamera->show();
    m_multiCamera->raise();
    m_multiCamera->activateWindow();
}

void MainWindow::manageCameras(const QString &selectId)
{
    CameraDialog dialog(this, selectId);
    dialog.exec();
    if (!dialog.startSource().isEmpty() && m_cameraButton->isEnabled())
        startVideo(dialog.startSource());
}

void MainWindow::toggleRecording()
{
    if (m_recorder->isRecording())
        stopRecording();
    else
        startRecording();
}

void MainWindow::startRecording()
{
    if (!m_videoRunning || !m_videoSource.startsWith(QStringLiteral("camera:")) || m_recorder->isRecording())
        return;
    const QString folder = CameraRecorder::savedFolder();
    QString error;
    // سرعة الكاميرا تُعرف بعد اتصالها؛ قبل ذلك تُعتبر 25 والمدة تُضبط على الوقت الفعلي على أي حال
    CameraRecorder::Options options = CameraRecorder::savedOptions();
    options.schedule = CameraRules::load(m_videoSource).recording;
    if (!m_recorder->start(folder, m_cameraName, m_videoClock.isValid() ? m_videoFps : 0.0, options, &error)) {
        updateRecordButton();
        statusBar()->showMessage(error);
        Toast::show(this, error, Toast::Kind::Warning);
        return;
    }
    m_recordTimer->start();
    updateRecordButton();
    AppLog::write(QStringLiteral("بدء تسجيل الكاميرا «%1»").arg(m_cameraName));
    statusBar()->showMessage(QStringLiteral("جارٍ تسجيل الكاميرا «%1»%2 في: %3")
                                 .arg(m_cameraName, options.motionOnly ? QStringLiteral(" عند الحركة فقط") : QString(),
                                      QDir::toNativeSeparators(folder)));
    Toast::show(this, options.motionOnly ? QStringLiteral("بدأ تسجيل الكاميرا عند الحركة") : QStringLiteral("بدأ تسجيل الكاميرا"),
                Toast::Kind::Info);
}

QString MainWindow::stopRecording()
{
    const bool wasRecording = m_recorder->isRecording();
    m_recorder->stop(); // ينتظر حفظ آخر ملف
    m_recordTimer->stop();
    updateRecordButton();
    if (!wasRecording)
        return {};
    const QStringList files = m_recorder->files();
    AppLog::write(QStringLiteral("إيقاف تسجيل الكاميرا «%1» (%2 ملفات)").arg(m_cameraName).arg(files.size()));
    if (files.isEmpty()) {
        Toast::show(this, QStringLiteral("لم يصل من الكاميرا ما يُسجَّل"), Toast::Kind::Warning);
        return {};
    }
    const QString folder = QDir::toNativeSeparators(QFileInfo(files.last()).absolutePath());
    const QString text = files.size() == 1
        ? QStringLiteral("حُفظ التسجيل: %1").arg(QDir::toNativeSeparators(files.last()))
        : QStringLiteral("حُفظ التسجيل في %1 ملفات في: %2").arg(files.size()).arg(folder);
    statusBar()->showMessage(text + QStringLiteral(" · يُفتح من «الأخيرة» أو «مجلد التسجيلات»"));
    Toast::show(this, QStringLiteral("حُفظ تسجيل الكاميرا"), Toast::Kind::Success);
    return text;
}

void MainWindow::updateRecordButton()
{
    if (!m_recordButton || !m_recorder)
        return;
    const bool camera = m_videoRunning && m_videoSource.startsWith(QStringLiteral("camera:"));
    const bool recording = m_recorder->isRecording();
    m_recordButton->setVisible(camera || recording);
    m_recordButton->setChecked(recording);
    m_recordButton->setIconName(recording ? Icons::Name::Stop : Icons::Name::Record);
    QString text = QStringLiteral("تسجيل");
    if (recording) {
        text = QStringLiteral("إيقاف التسجيل %1").arg(ImageIO::formatTime(m_recorder->elapsedMs()));
        if (m_recorder->isOffSchedule())
            text += QStringLiteral(" · خارج ساعات الجدول");
        else if (m_recorder->isWaitingForMotion())
            text += QStringLiteral(" · بانتظار حركة");
    }
    m_recordButton->setText(text);
}

void MainWindow::showRecordFolderMenu()
{
    const QString folder = CameraRecorder::savedFolder();
    QMenu menu(this);
    menu.addAction(QStringLiteral("فتح مجلد التسجيلات"), this, [folder] {
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    menu.addAction(QStringLiteral("تغيير المجلد…"), this, [this, folder] {
        const QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("مجلد تسجيلات الكاميرا"), folder);
        if (chosen.isEmpty())
            return;
        CameraRecorder::setSavedFolder(chosen);
        statusBar()->showMessage(QStringLiteral("تسجيلات الكاميرا تُحفظ في: %1%2")
                                     .arg(QDir::toNativeSeparators(chosen),
                                          m_recorder->isRecording() ? QStringLiteral(" (من التسجيل القادم)") : QString()));
    });
    menu.addSeparator();
    const CameraRecorder::Options options = CameraRecorder::savedOptions();
    const QString later = m_recorder->isRecording() ? QStringLiteral(" (من التسجيل القادم)") : QString();
    QAction *motion = menu.addAction(QStringLiteral("التسجيل عند الحركة فقط"));
    motion->setCheckable(true);
    motion->setChecked(options.motionOnly);
    motion->setToolTip(QStringLiteral("لا يُسجَّل المشهد الساكن؛ يبدأ التسجيل مع أي حركة أو وجه ويستمر 10 ثوانٍ بعدها"));
    connect(motion, &QAction::toggled, this, [this, later](bool on) {
        CameraRecorder::Options changed = CameraRecorder::savedOptions();
        changed.motionOnly = on;
        CameraRecorder::setSavedOptions(changed);
        statusBar()->showMessage((on ? QStringLiteral("التسجيل عند الحركة فقط") : QStringLiteral("تسجيل كل شيء")) + later);
    });
    QAction *overlay = menu.addAction(QStringLiteral("رسم مربعات الوجوه وأسمائها في التسجيل"));
    overlay->setCheckable(true);
    overlay->setChecked(options.overlay);
    connect(overlay, &QAction::toggled, this, [this, later](bool on) {
        CameraRecorder::Options changed = CameraRecorder::savedOptions();
        changed.overlay = on;
        CameraRecorder::setSavedOptions(changed);
        statusBar()->showMessage((on ? QStringLiteral("تُرسم الوجوه وأسماؤها في التسجيل")
                                     : QStringLiteral("التسجيل نظيف دون مربعات (يصلح لإعادة التحليل)")) + later);
    });
    QMenu *retention = menu.addMenu(QStringLiteral("حذف التسجيلات القديمة تلقائياً"));
    auto *retentionGroup = new QActionGroup(retention);
    const int days = CameraRecorder::retentionDays();
    for (const int option : {0, 3, 7, 14, 30, 90}) {
        QAction *action = retention->addAction(option == 0 ? QStringLiteral("لا تُحذف") : QStringLiteral("الأقدم من %1 يوماً").arg(option));
        action->setCheckable(true);
        action->setChecked(option == days);
        retentionGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, option] {
            CameraRecorder::setRetentionDays(option);
            const int removed = CameraRecorder::removeExpired();
            QString text = option == 0 ? QStringLiteral("لن تُحذف التسجيلات تلقائياً")
                                       : QStringLiteral("تُحذف تسجيلات البرنامج الأقدم من %1 يوماً، وأقدمها حين يقترب القرص من الامتلاء").arg(option);
            if (removed > 0)
                text += QStringLiteral(" · حُذف الآن %1 ملفاً").arg(removed);
            statusBar()->showMessage(text);
        });
    }
    retention->addSeparator();
    retention->addAction(QStringLiteral("لا تُحذف إلا تسجيلات البرنامج، لا ملفات المجلد الأخرى"))->setEnabled(false);
    menu.addSeparator();
    menu.addAction(QDir::toNativeSeparators(folder))->setEnabled(false);
    menu.exec(QCursor::pos());
}

void MainWindow::refreshWatched()
{
    m_watched = m_database.watchedIds();
    if (m_cameraWatch)
        m_cameraWatch->setWatched(m_watched);
    if (m_multiCamera)
        m_multiCamera->setWatched(m_watched);
    m_view->setWatched(m_watched);
    // إضافة شخص إلى المراقبة أثناء عرض صورة أو فيديو تنبّه عنه فوراً إن كان ظاهراً
    for (const FaceItem &face : std::as_const(m_faces))
        checkWatchAlert(face);
    if (m_faceList->currentRow() >= 0)
        showFaceDetails();
}

void MainWindow::clearWatchAlerts()
{
    m_alerted.clear();
    m_alertCount = 0;
    m_alertFaceId = -1;
    m_alertBar->hide();
}

void MainWindow::checkWatchAlert(const FaceItem &face)
{
    // الكاميرا تنبّه من الإطارات نفسها، فيتكرر التنبيه إن عاد الشخص بعد غياب (noteCameraFaces)
    if (m_videoSource.startsWith(QStringLiteral("camera:")) && m_videoMode)
        return;
    if (face.match.status != MatchStatus::Confirmed || !m_watched.contains(face.match.personId)
        || m_alerted.contains(face.match.personId))
        return;
    m_alerted.insert(face.match.personId);
    showWatchAlert(face, ImageIO::faceCrop(face));
}

void MainWindow::showWatchAlert(const FaceItem &face, const QImage &crop)
{
    const bool camera = m_videoMode && m_videoSource.startsWith(QStringLiteral("camera:"));
    const int level = m_database.alertLevel(face.match.personId);
    ++m_alertCount;
    if (level == AlertSound::NotifyOnly) {
        // إشعار من ويندوز فقط، دون شريط ولا صوت
        statusBar()->showMessage(QStringLiteral("تنبيه مراقبة: ظهر %1").arg(face.match.name), 10000);
        emit watchAlert(face.match.name, camera ? m_cameraName : QString(), level);
        return;
    }
    m_alertFaceId = face.id;
    m_alertAvatar->setPixmap(circularPixmap(crop, 44));
    QString where;
    if (camera)
        where = QStringLiteral(" · أمام «%1»").arg(m_cameraName.toHtmlEscaped());
    else if (m_videoMode && face.firstSeenMs >= 0)
        where = QStringLiteral(" عند \u2066%1\u2069").arg(ImageIO::formatTime(face.firstSeenMs));
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    QString text = QStringLiteral("<span style='color:%1; font-weight:700; font-size:12pt'>تنبيه مراقبة: ظهر %2</span>"
                                  "<br><span style='color:#cfe0ff'>التشابه %3%%4 · الساعة \u2066%5\u2069</span>")
                       .arg(Theme::Accent2.name(), face.match.name.toHtmlEscaped())
                       .arg(qRound(face.match.score * 100))
                       .arg(where, time);
    if (m_alertCount > 1)
        text += QStringLiteral("<span style='color:#8aa3c7'> · %1 تنبيهات في %2</span>")
                    .arg(m_alertCount)
                    .arg(camera ? QStringLiteral("هذه الجلسة") : QStringLiteral("هذا الملف"));
    m_alertText->setText(text);
    m_alertBar->show();
    m_alertPulse->setProperty("step", 0);
    m_alertPulse->start();
    if (level == AlertSound::High)
        AlertSound::play(AlertSound::Sound::Alarm);
    else if (level != AlertSound::Silent)
        AlertSound::play(AlertSound::Sound::Chime);
    statusBar()->showMessage(QStringLiteral("تنبيه مراقبة: ظهر %1").arg(face.match.name), 10000);
    emit watchAlert(face.match.name, camera ? m_cameraName : QString(), level);
}

void MainWindow::showStrangerAlert(qint64 visitorId, const QString &reason, const QString &camera)
{
    AlertSound::play(AlertSound::Sound::Visitor);
    // يصل إلى السجل ورسالة النافذة وإشعار ويندوز معاً؛ «الزوار المجهولون» يعرض صوره وظهوره
    emit systemAlert(QStringLiteral("زائر مجهول رقم %1").arg(visitorId),
                     camera.isEmpty() ? reason : QStringLiteral("%1 أمام «%2»").arg(reason, camera));
}

void MainWindow::noteCameraFaces(const QImage &frame, const QList<FaceItem> &faces)
{
    // بصمة الوجه وصورته الأوضح من بطاقته (متوسط إطارات مساره كلها)، فهي أثبت من الإطار وحده
    QList<CameraWatch::Face> watched;
    for (const FaceItem &face : faces) {
        CameraWatch::Face item;
        item.box = face.box;
        item.match = face.match;
        item.quality = face.quality;
        item.track = face.id;
        const int row = m_faceRows.value(face.id, -1);
        if (row >= 0 && row < m_faces.size() && m_faces.at(row).id == face.id) {
            const FaceItem &card = m_faces.at(row);
            item.embedding = card.embedding;
            item.quality = std::max(item.quality, card.quality);
            // فك الصورة مع كل إطار كان يثقل الواجهة؛ تُفك عند الحاجة فقط (CameraWatch::cropOf)
            if (face.match.status != MatchStatus::Possible) {
                item.crop = card.crop;
                if (item.crop.isNull())
                    item.packedCrop = card.packedCrop;
            }
        }
        watched.append(item);
    }
    // مربعات الوجوه للتسجيل، بإحداثيات نسبية لأن الإطار المعروض قد يكون مصغراً
    if (m_recorder->isRecording() && !frame.isNull())
        m_recorder->setOverlay(m_cameraWatch->overlay(frame.size(), watched));
    m_cameraWatch->setThreshold(threshold());
    m_cameraFaces = faces;
    m_cameraWatch->process(frame, watched);
}

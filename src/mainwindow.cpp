#include "mainwindow.h"

#include "alertlog.h"
#include "applog.h"
#include "importdialog.h"
#include "harvestdialog.h"
#include "matchpage.h"
#include "personprofile.h"

#include "cameradialog.h"
#include "cameras.h"
#include "camerawatch.h"
#include "carddelegates.h"
#include "dailyreport.h"
#include "enhancedialog.h"
#include "folderscan.h"
#include "gpucheck.h"
#include "report.h"
#include "icons.h"
#include "imageio.h"
#include "imageview.h"
#include "multicamera.h"
#include "persondialog.h"
#include "recorder.h"
#include "recordingsdialog.h"
#include "shotviewer.h"
#include "timelineview.h"
#include "personmodel.h"
#include "performance.h"
#include "theme.h"
#include "videoexport.h"
#include "videofilters.h"
#include "videofiltersdialog.h"
#include "videoworker.h"
#include "visitorsdialog.h"
#include "widgets.h"

#include <QApplication>
#include <QActionGroup>
#include <QButtonGroup>
#include <QClipboard>
#include <QCollator>
#include <QComboBox>
#include <QMenu>
#include <QScrollArea>
#include <QTimeEdit>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDateEdit>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QListWidget>
#include <QMimeData>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QPainterPath>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

// شخص غاب عن الكاميرا أكثر من هذا ثم عاد يُسجَّل ظهوره من جديد ويُنبَّه عنه مرة أخرى
// انقطاع الكاميرا أطول من هذا يصل إشعاراً (الانقطاع القصير يعود وحده عادةً)
constexpr qint64 CameraOfflineAlertMs = 2 * 60 * 1000;
// الكاميرا الموصولة التي انقطعت ولم تعد يُعاد تشغيلها بعد هذه المدة ما لم يفتح المستخدم شيئاً آخر
constexpr int CameraRestartMs = 30 * 1000;
constexpr int SightingPositionRole = Qt::UserRole + 120;

const QString ImageFilter = QStringLiteral("الصور (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff);;كل الملفات (*)");
const QString VideoFilter = QStringLiteral("الفيديو (*.mp4 *.avi *.mkv *.mov *.wmv *.webm *.m4v *.3gp);;كل الملفات (*)");

const QStringList VideoSuffixes{QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("mkv"), QStringLiteral("mov"),
                                QStringLiteral("wmv"), QStringLiteral("webm"), QStringLiteral("m4v"), QStringLiteral("3gp")};

bool isVideoFile(const QString &path)
{
    return VideoSuffixes.contains(QFileInfo(path).suffix().toLower());
}

bool isImageFile(const QString &path)
{
    return !QImageReader::imageFormat(path).isEmpty();
}

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

// ينفذ عملاً ثقيلاً في الخلفية مع إبقاء الواجهة مستجيبة حتى ينتهي
template<typename Function>
auto runResponsive(Function function, bool allowInput = false)
{
    QFutureWatcher<decltype(function())> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(std::move(function)));
    if (!watcher.isFinished())
        loop.exec(allowInput ? QEventLoop::AllEvents : QEventLoop::ExcludeUserInputEvents);
    return watcher.result();
}

// نافذة تقدم لإضافة صور كثيرة مع إيقاف مؤقت واستئناف وإلغاء
class BatchProgressDialog : public QDialog
{
public:
    BatchProgressDialog(int total, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("إضافة صور"));
        setModal(true);
        setMinimumWidth(460);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(12);
        m_label = new QLabel(this);
        m_label->setWordWrap(true);
        layout->addWidget(m_label);
        m_bar = new QProgressBar(this);
        m_bar->setRange(0, total);
        m_bar->setTextVisible(false);
        m_bar->setObjectName(QStringLiteral("thinProgress"));
        layout->addWidget(m_bar);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        m_pause = new QPushButton(QStringLiteral("إيقاف مؤقت"), this);
        auto *cancel = new QPushButton(QStringLiteral("إلغاء"), this);
        buttons->addWidget(m_pause);
        buttons->addWidget(cancel);
        layout->addLayout(buttons);
        connect(m_pause, &QPushButton::clicked, this, [this] {
            m_paused = !m_paused;
            m_pause->setText(m_paused ? QStringLiteral("استئناف") : QStringLiteral("إيقاف مؤقت"));
            m_label->setText(m_paused ? QStringLiteral("متوقف مؤقتاً — %1").arg(m_text) : m_text);
            if (!m_paused)
                m_resume.quit();
        });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(this, &QDialog::rejected, this, [this] {
            m_canceled = true;
            m_resume.quit();
        });
    }

    void setProgress(int value, const QString &text)
    {
        m_text = text;
        m_bar->setValue(value);
        m_label->setText(text);
    }

    // يعيد false إذا أُلغيت العملية، وينتظر هنا ما دامت متوقفة مؤقتاً
    bool proceed()
    {
        if (m_paused && !m_canceled)
            m_resume.exec();
        return !m_canceled;
    }

private:
    QLabel *m_label;
    QProgressBar *m_bar;
    QPushButton *m_pause;
    QString m_text;
    QEventLoop m_resume;
    bool m_paused = false;
    bool m_canceled = false;
};

// صورة وجه في شبكة صور الشخص، مع نجمة للصورة الرئيسية وعلامة تحذير للصورة المشكوك فيها
QPixmap decoratedPhoto(const QImage &image, int size, bool cover, bool suspect)
{
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size, size), 12, 12);
    p.save();
    p.setClipPath(clip);
    const QImage scaled = image.scaled(size * 2, size * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    p.drawImage(QRectF((size - scaled.width() / 2.0) / 2.0, (size - scaled.height() / 2.0) / 2.0, scaled.width() / 2.0, scaled.height() / 2.0), scaled);
    p.restore();
    if (suspect) {
        p.setPen(QPen(QColor(0xfb, 0x71, 0x85), 3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1.5, 1.5, size - 3, size - 3), 11, 11);
    }
    auto badge = [&](const QPointF &center, const QColor &color, Icons::Name icon) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(7, 17, 31, 220));
        p.drawEllipse(center, 14, 14);
        Icons::paint(p, icon, QRectF(center.x() - 9, center.y() - 9, 18, 18), color);
    };
    if (cover)
        badge(QPointF(size - 18, 18), QColor(0xfa, 0xcc, 0x15), Icons::Name::Star);
    if (suspect)
        badge(QPointF(18, 18), QColor(0xfb, 0x71, 0x85), Icons::Name::Alert);
    return pixmap;
}

QString safeFileName(QString name)
{
    static const QString invalid = QStringLiteral("\\/:*?\"<>|");
    for (QChar c : invalid)
        name.replace(c, QLatin1Char('_'));
    return name;
}

QFrame *card(QWidget *parent, const QString &name = QStringLiteral("card"))
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(name);
    return frame;
}

QLabel *label(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(objectName);
    return l;
}

QFrame *separator(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setObjectName(QStringLiteral("separator"));
    line->setFixedHeight(24);
    return line;
}

// شعار التطبيق: مربع متدرج بأيقونة مسح الوجه
class LogoMark : public QWidget
{
public:
    explicit LogoMark(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(42, 42);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        static const QPixmap logo(QStringLiteral(":/resources/app-256.png"));
        p.drawPixmap(rect(), logo);
    }
};

QString statusDescription(MatchStatus status)
{
    switch (status) {
    case MatchStatus::Confirmed:
        return QStringLiteral("تطابق مؤكد");
    case MatchStatus::Possible:
        return QStringLiteral("تطابق محتمل — يُنصح بالتحقق");
    case MatchStatus::Unknown:
        break;
    }
    return QStringLiteral("غير موجود في قاعدة البيانات");
}

} // namespace

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
        m_timeline->setAnalyzedUntil(m_videoStartMs);
        m_timelineCard->setVisible(!camera && durationMs > 0);
        if (camera && m_autoRecordSwitch->isChecked())
            startRecording();
    });
    connect(&m_frameWatcher, &QFutureWatcher<ShotFrame>::finished, this, &MainWindow::onVideoFrameLoaded);
    connect(m_videoWorker, &VideoWorker::frameReady, this, &MainWindow::onFrameReady);
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
    if (m_previousCrash.isValid()) {
        Toast::show(this,
                    QStringLiteral("لم يُغلق البرنامج إغلاقاً طبيعياً في المرة السابقة (بدأت %1). التفاصيل في «سجل الأحداث» من قائمة "
                                   "الكاميرا ← السجلات والتقارير")
                        .arg(m_previousCrash.toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
                    Toast::Kind::Warning);
    }
    if (!m_pendingFile.isEmpty()) {
        openFile(std::exchange(m_pendingFile, QString()));
    } else if (startCameraOnLaunch() && !Cameras::lastSource().isEmpty()) {
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
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), [this] {
        if (m_cameraButton->isEnabled())
            startLastCamera();
    });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this] {
        if (m_folderScanButton->isEnabled())
            openFolderScan();
    });
    add(QKeySequence::Save, [this] {
        if (m_saveFacesButton->isEnabled())
            saveFaces();
    });
    add(QKeySequence::Paste, [this] { pasteImage(); });
    add(QKeySequence(Qt::Key_Escape), [this] {
        if (m_videoRunning)
            stopVideo();
    });
    add(QKeySequence(Qt::Key_Space), [this] {
        if (m_videoRunning)
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
    for (const QKeySequence &key : {QKeySequence(Qt::Key_PageDown), QKeySequence(Qt::ALT | Qt::Key_Left)})
        add(key, [this, onRecognition] {
            if (onRecognition())
                navigateImage(1);
        });
    for (const QKeySequence &key : {QKeySequence(Qt::Key_PageUp), QKeySequence(Qt::ALT | Qt::Key_Right)})
        add(key, [this, onRecognition] {
            if (onRecognition())
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
    add(QKeySequence(Qt::Key_L), [this, onRecognition] {
        if (onRecognition())
            m_view->setLabelsVisible(!m_view->labelsVisible());
    });
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
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // إغلاق النافذة أثناء تشغيل الكاميرا يبقيها تعمل بجانب الساعة (المراقبة والتسجيل والتنبيهات)
    const bool multi = m_multiCamera && m_multiCamera->isRunning();
    const bool camera = (m_videoRunning && m_videoSource.startsWith(QStringLiteral("camera:"))) || multi;
    if (!m_quitting && camera && m_tray && QSystemTrayIcon::isSystemTrayAvailable() && keepRunningInTray()) {
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

namespace {

const QString RunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");

} // namespace

bool MainWindow::keepRunningInTray()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("keepRunningInTray"), true).toBool();
}

bool MainWindow::startCameraOnLaunch()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("startCameraOnLaunch"), false).toBool();
}

bool MainWindow::launchWithWindows()
{
#ifdef Q_OS_WIN
    return QSettings(RunKey, QSettings::NativeFormat).contains(QStringLiteral("Malamih"));
#else
    return false;
#endif
}

bool MainWindow::startHidden() const
{
    return QApplication::arguments().contains(QStringLiteral("--startup")) && m_tray && QSystemTrayIcon::isSystemTrayAvailable();
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
    addBackgroundMenu(menu);
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
    connect(this, &MainWindow::watchAlert, this, [this](const QString &name, const QString &camera) {
        m_tray->setToolTip(QStringLiteral("ملامح — آخر تنبيه: %1 الساعة %2").arg(name, QTime::currentTime().toString(QStringLiteral("HH:mm"))));
        if (isVisible() && !isMinimized() && isActiveWindow())
            return;
        m_tray->showMessage(QStringLiteral("تنبيه مراقبة: ظهر %1").arg(name),
                            camera.isEmpty() ? QStringLiteral("انقر لعرض التفاصيل")
                                             : QStringLiteral("أمام الكاميرا «%1» — انقر لعرضه").arg(camera),
                            QSystemTrayIcon::Warning, 15000);
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

void MainWindow::addBackgroundMenu(QMenu *parent)
{
    QMenu *menu = parent->addMenu(QStringLiteral("العمل في الخلفية"));
    QAction *visitors = menu->addAction(QStringLiteral("حفظ الزوار المجهولين"));
    visitors->setCheckable(true);
    visitors->setChecked(CameraWatch::visitorsEnabled());
    visitors->setToolTip(QStringLiteral("الوجوه غير المسجلة أمام الكاميرات تُحفظ زواراً (الوجه نفسه تحت رقم واحد) لتسميتهم لاحقاً"));
    connect(visitors, &QAction::toggled, this, [](bool on) { CameraWatch::setVisitorsEnabled(on); });
    QAction *tray = menu->addAction(QStringLiteral("إبقاء الكاميرا تعمل عند إغلاق النافذة"));
    tray->setCheckable(true);
    tray->setChecked(keepRunningInTray());
    tray->setToolTip(QStringLiteral("تختفي النافذة إلى جانب الساعة وتستمر المراقبة والتسجيل والتنبيهات"));
    connect(tray, &QAction::toggled, this, [](bool on) {
        QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("keepRunningInTray"), on);
    });
    QAction *camera = menu->addAction(QStringLiteral("تشغيل آخر كاميرا عند فتح البرنامج"));
    camera->setCheckable(true);
    camera->setChecked(startCameraOnLaunch());
    connect(camera, &QAction::toggled, this, [](bool on) {
        QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("startCameraOnLaunch"), on);
    });
#ifdef Q_OS_WIN
    QAction *startup = menu->addAction(QStringLiteral("فتح البرنامج مع ويندوز"));
    startup->setCheckable(true);
    startup->setChecked(launchWithWindows());
    startup->setToolTip(QStringLiteral("يُفتح بجانب الساعة عند تسجيل الدخول؛ مع «تشغيل آخر كاميرا» تعود المراقبة وحدها بعد إعادة تشغيل الجهاز"));
    connect(startup, &QAction::toggled, this, [this](bool on) {
        QSettings run(RunKey, QSettings::NativeFormat);
        if (on)
            run.setValue(QStringLiteral("Malamih"), QStringLiteral("\"%1\" --startup").arg(QDir::toNativeSeparators(QApplication::applicationFilePath())));
        else
            run.remove(QStringLiteral("Malamih"));
        statusBar()->showMessage(on ? QStringLiteral("سيُفتح البرنامج مع ويندوز") : QStringLiteral("لن يُفتح البرنامج مع ويندوز"));
    });
#endif
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList files;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            files << url.toLocalFile();
    }
    event->acceptProposedAction();
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
    m_videoWorker->requestStop();
    m_videoThread->quit();
    m_videoThread->wait();
    m_recorder->disconnect(this);
    m_recorder->stop();
    m_imageWatcher.waitForFinished();
    m_frameWatcher.waitForFinished();
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
    m_headerButtons = {m_openImageButton, m_openVideoButton, m_recentButton, m_cameraButton, m_folderScanButton,
                       m_pauseButton, m_recordButton, m_stopButton, m_enhanceImageButton, m_saveFacesButton};

    auto *header = new QHBoxLayout;
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
    layout->addLayout(header);

    // الإعدادات: مجموعات صغيرة تلتف إلى سطر جديد عندما تضيق النافذة
    auto *settings = card(page);
    auto *settingsLayout = new FlowLayout(settings, 18, 8);
    settingsLayout->setContentsMargins(16, 8, 16, 8);
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

    // جزء الفيديو المطلوب
    auto timeEdit = [settings](const QString &tip) {
        auto *edit = new QTimeEdit(settings);
        setLtrDisplayFormat(edit, QStringLiteral("HH:mm:ss"));
        edit->setTime(QTime(0, 0));
        edit->setToolTip(tip);
        return edit;
    };
    m_rangeStart = timeEdit(QStringLiteral("بداية الجزء المطلوب تحليله من الفيديو"));
    m_rangeEnd = timeEdit(QStringLiteral("نهاية الجزء المطلوب تحليله؛ 00:00:00 = حتى نهاية الفيديو"));
    m_rangeEnd->setSpecialValueText(QStringLiteral("النهاية"));

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

    settingsLayout->addWidget(group({new QLabel(QStringLiteral("حد التعرف"), settings), m_thresholdSlider, m_thresholdValue, m_thresholdWarning}));
    settingsLayout->addWidget(m_thoroughSwitch);
    settingsLayout->addWidget(m_enhanceSwitch);
    QWidget *frameStepGroup = group({new QLabel(QStringLiteral("إطار من كل"), settings), m_frameStepSpin});
    QWidget *rangeGroup = group({new QLabel(QStringLiteral("الجزء: من"), settings), m_rangeStart,
                                 new QLabel(QStringLiteral("إلى"), settings), m_rangeEnd});
    QWidget *regionGroup = group({m_filtersButton, m_regionButton, m_clearRegionButton, m_reanalyzeButton});
    auto *recordFolderButton = new FancyButton(Icons::Name::Folder, QStringLiteral("التسجيلات"), FancyButton::Kind::Secondary, settings);
    recordFolderButton->setToolTip(QStringLiteral("مجلد تسجيلات الكاميرا، والتسجيل عند الحركة فقط، ورسم الوجوه، وحذف القديم تلقائياً"));
    connect(recordFolderButton, &QAbstractButton::clicked, this, &MainWindow::showRecordFolderMenu);
    m_autoRecordSwitch = new ToggleSwitch(QStringLiteral("تسجيل تلقائي"), settings);
    m_autoRecordSwitch->setToolTip(QStringLiteral("يبدأ تسجيل الكاميرا وحده كلما شُغّلت"));
    m_autoRecordSwitch->setChecked(CameraRecorder::autoRecord());
    connect(m_autoRecordSwitch, &QAbstractButton::toggled, this, [](bool on) { CameraRecorder::setAutoRecord(on); });
    m_recordGroup = group({new QLabel(QStringLiteral("تسجيل الكاميرا"), settings), m_autoRecordSwitch, recordFolderButton});
    m_videoSettings = {frameStepGroup, rangeGroup, regionGroup};
    settingsLayout->addWidget(frameStepGroup);
    auto *gpuSwitch = new ToggleSwitch(QStringLiteral("كرت الشاشة"), settings);
    const QString gpu = FaceEngine::availableGpu();
    gpuSwitch->setChecked(Performance::gpuEnabled() && !gpu.isEmpty());
    gpuSwitch->setEnabled(!gpu.isEmpty());
    gpuSwitch->setToolTip(gpu.isEmpty() ? QStringLiteral("لا يوجد كرت شاشة يدعم DirectX 12 في هذا الجهاز؛ التحليل على المعالج")
                                        : QStringLiteral("تحليل الوجوه على %1 بدل المعالج: أسرع عادةً بعدة مرات، ويبقى المعالج "
                                                         "متاحاً لبقية البرامج. يُطبَّق عند إعادة تشغيل البرنامج")
                                              .arg(gpu));
    connect(gpuSwitch, &QAbstractButton::toggled, this, [this](bool on) {
        Performance::setGpuEnabled(on);
        Toast::show(this, QStringLiteral("يُطبَّق عند إعادة تشغيل البرنامج"), Toast::Kind::Info);
    });
    auto *gpuCheck = new FancyButton(Icons::Name::Scan, QStringLiteral("فحص"), FancyButton::Kind::Secondary, settings);
    gpuCheck->setToolTip(QStringLiteral("قياس سرعة التحليل على كرت الشاشة مقابل المعالج، والتحقق أن نتائجه مطابقة للمعالج"));
    connect(gpuCheck, &QAbstractButton::clicked, this, [this, gpuCheck, gpuSwitch] {
        gpuCheck->setEnabled(false);
        Toast::show(this, QStringLiteral("جارٍ فحص كرت الشاشة والمعالج… (قد يستغرق دقيقة)"), Toast::Kind::Info);
        auto *watcher = new QFutureWatcher<GpuCheck::Result>(this);
        connect(watcher, &QFutureWatcher<GpuCheck::Result>::finished, this, [this, watcher, gpuCheck, gpuSwitch] {
            watcher->deleteLater();
            gpuCheck->setEnabled(true);
            const GpuCheck::Result result = watcher->result();
            AppLog::write(QStringLiteral("فحص كرت الشاشة: %1").arg(result.summary()));
            QString text = result.summary();
            const bool unreliable = !result.gpu.isEmpty() && !result.reliable();
            if (unreliable && Performance::gpuEnabled())
                text += QStringLiteral("\n\nإيقاف كرت الشاشة والعودة إلى المعالج؟ (يُطبَّق عند إعادة تشغيل البرنامج)");
            else if (result.reliable() && !Performance::gpuEnabled())
                text += QStringLiteral("\n\nكرت الشاشة متوقف في الإعدادات؛ فعّله من مفتاح «كرت الشاشة».");
            if (unreliable && Performance::gpuEnabled()) {
                if (QMessageBox::warning(this, QStringLiteral("فحص كرت الشاشة"), text, QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
                    gpuSwitch->setChecked(false);
            } else {
                QMessageBox::information(this, QStringLiteral("فحص كرت الشاشة"), text);
            }
        });
        // يُفحص بنسخ مستقلة من النماذج، فلا يتأثر التحليل الجاري
        watcher->setFuture(QtConcurrent::run([] { return GpuCheck::run(FaceEngine::defaultModelsDir(), QString()); }));
    });
    settingsLayout->addWidget(group({new QLabel(QStringLiteral("استهلاك الموارد"), settings), m_performanceCombo, gpuSwitch, gpuCheck}));
    settingsLayout->addWidget(rangeGroup);
    settingsLayout->addWidget(regionGroup);
    settingsLayout->addWidget(m_recordGroup);
    m_recordGroup->setVisible(false);
    layout->addWidget(settings);

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
        else if (!m_videoRunning && m_faces.at(row).bestMs >= 0)
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
    auto *viewLayout = new QVBoxLayout(viewCard);
    viewLayout->setContentsMargins(2, 2, 2, 2);
    m_view = new ImageView(viewCard);
    m_view->setMinimumSize(280, 180);
    viewLayout->addWidget(m_view);
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
    auto *sideHeader = new QHBoxLayout;
    sideHeader->addWidget(label(QStringLiteral("الوجوه المكتشفة"), QStringLiteral("sectionTitle"), side));
    sideHeader->addStretch();
    m_faceCount = label(QStringLiteral("0"), QStringLiteral("badge"), side);
    sideHeader->addWidget(m_faceCount);
    sideLayout->addLayout(sideHeader);

    // البحث والتصفية والترتيب
    m_faceFilter = new QLineEdit(side);
    m_faceFilter->setPlaceholderText(QStringLiteral("بحث بالاسم…"));
    m_faceFilter->setClearButtonEnabled(true);
    m_faceFilter->addAction(Icons::icon(Icons::Name::Search, Theme::Muted, 18), QLineEdit::LeadingPosition);
    m_faceStatusFilter = new QComboBox(side);
    m_faceStatusFilter->addItems({QStringLiteral("الكل"), QStringLiteral("المعروفون"), QStringLiteral("المحتملون"),
                                  QStringLiteral("غير المعروفين")});
    m_faceStatusFilter->setToolTip(QStringLiteral("عرض الوجوه حسب نتيجة المطابقة"));
    m_faceSort = new QComboBox(side);
    m_faceSort->addItems({QStringLiteral("حسب الظهور"), QStringLiteral("الأكثر ظهوراً"), QStringLiteral("الأوضح"),
                          QStringLiteral("المعروفون أولاً")});
    m_faceSort->setToolTip(QStringLiteral("ترتيب القائمة"));
    connect(m_faceFilter, &QLineEdit::textChanged, this, &MainWindow::applyFaceFilter);
    connect(m_faceStatusFilter, &QComboBox::currentIndexChanged, this, &MainWindow::applyFaceFilter);
    connect(m_faceSort, &QComboBox::currentIndexChanged, this, &MainWindow::sortFaces);
    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    filters->addWidget(m_faceStatusFilter, 1);
    filters->addWidget(m_faceSort, 1);
    sideLayout->addWidget(m_faceFilter);
    sideLayout->addLayout(filters);

    m_faceList = new QListWidget(side);
    m_faceList->setItemDelegate(new FaceCardDelegate(m_faceList));
    m_faceList->setUniformItemSizes(true);
    m_faceList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_faceList->setMouseTracking(true);
    m_faceList->setMinimumHeight(90);
    m_faceList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_faceList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_faceList->setToolTip(QStringLiteral("نقر مزدوج: تكبير الوجه في الصورة، أو عرض لقطاته في الفيديو\n"
                                          "Ctrl أو Shift لتحديد عدة وجوه ثم زر الفأرة الأيمن للدمج أو الحذف"));
    connect(m_faceList, &QListWidget::currentRowChanged, this, &MainWindow::showFaceDetails);
    connect(m_faceList, &QListWidget::customContextMenuRequested, this, &MainWindow::showFaceContextMenu);
    connect(m_faceList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (m_videoMode)
            openShotViewer(item->data(CardRole::Id).toInt());
        else
            m_view->zoomToFace(item->data(CardRole::Id).toInt());
    });

    // التفاصيل تُمرَّر إن طالت حتى تبقى الأزرار ظاهرة على الشاشات القصيرة
    m_faceDetails = new QLabel(side);
    m_faceDetails->setWordWrap(true);
    m_faceDetails->setTextFormat(Qt::RichText);
    m_faceDetails->setAlignment(Qt::AlignTop);
    m_faceDetails->setObjectName(QStringLiteral("muted"));
    m_faceDetails->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(m_faceDetails, &QLabel::linkActivated, this, [this](const QString &link) {
        const int row = m_faceList->currentRow();
        if (row < 0 || row >= m_faces.size() || !m_ready)
            return;
        if (link.startsWith(QStringLiteral("assign:")))
            assignFace(m_faces.at(row).id, link.mid(7));
        else if (link == QStringLiteral("other"))
            addFaceToDatabase(m_faces.at(row));
    });
    auto *detailsScroll = new QScrollArea(side);
    detailsScroll->setWidget(m_faceDetails);
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setFrameShape(QFrame::NoFrame);
    detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailsScroll->setMinimumHeight(110);
    detailsScroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    // القائمة والتفاصيل في مقسم حتى يكبّر المستخدم أيهما شاء
    auto *sideSplitter = new QSplitter(Qt::Vertical, side);
    sideSplitter->setHandleWidth(8);
    sideSplitter->setChildrenCollapsible(false);
    sideSplitter->addWidget(m_faceList);
    sideSplitter->addWidget(detailsScroll);
    sideSplitter->setStretchFactor(0, 1);
    sideSplitter->setStretchFactor(1, 1);
    sideSplitter->setSizes({190, 290});
    sideLayout->addWidget(sideSplitter, 1);

    m_viewShotButton = new FancyButton(Icons::Name::Film, QStringLiteral("اللقطات"), FancyButton::Kind::Secondary, side);
    m_viewShotButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_viewShotButton->setToolTip(QStringLiteral("الرجوع إلى إطارات ظهور الوجه، وأوضح لقطة له، وتحسينها وحفظها"));
    m_viewShotButton->setVisible(false);
    connect(m_viewShotButton, &QAbstractButton::clicked, this, [this] {
        const int row = m_faceList->currentRow();
        if (row >= 0 && row < m_faces.size())
            openShotViewer(m_faces.at(row).id);
    });

    m_enrollFaceButton = new FancyButton(Icons::Name::UserPlus, QStringLiteral("إضافة للقاعدة"), FancyButton::Kind::Primary, side);
    m_enrollFaceButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_enrollFaceButton->setToolTip(QStringLiteral("إضافة الوجه المحدد إلى شخص في قاعدة البيانات، أو إلى شخص جديد"));
    connect(m_enrollFaceButton, &QAbstractButton::clicked, this, &MainWindow::addSelectedFaceToDatabase);
    auto *faceActions = new QHBoxLayout;
    faceActions->setSpacing(8);
    faceActions->addWidget(m_enrollFaceButton, 1);
    faceActions->addWidget(m_viewShotButton, 1);
    sideLayout->addLayout(faceActions);

    // الخط الزمني أسفل الفيديو بأسلوب برامج المونتاج
    m_timelineCard = card(page);
    auto *timelineLayout = new QVBoxLayout(m_timelineCard);
    timelineLayout->setContentsMargins(10, 6, 10, 8);
    timelineLayout->setSpacing(4);
    auto *timelineHeader = new QHBoxLayout;
    timelineHeader->setSpacing(6);
    timelineHeader->addWidget(label(QStringLiteral("الخط الزمني"), QStringLiteral("sectionTitle"), m_timelineCard));
    auto *timelineHint = label(QStringLiteral("انقر لعرض أي لحظة · اسحب المقبضين الأصفرين للاقتطاع · الزر الأيمن للعمليات · Ctrl+العجلة للتكبير"),
                               QStringLiteral("muted"), m_timelineCard);
    timelineHint->setMinimumWidth(10);
    timelineHeader->addWidget(timelineHint, 1);
    m_timeline = new TimelineView(m_timelineCard);
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
    connect(m_timeline, &TimelineView::laneClicked, this, [this](int id, qint64) { selectFace(id); });
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
    mediaSplitter->setHandleWidth(10);
    mediaSplitter->setChildrenCollapsible(false);
    mediaSplitter->addWidget(viewCard);
    mediaSplitter->addWidget(m_timelineCard);
    mediaSplitter->setStretchFactor(0, 4);
    mediaSplitter->setStretchFactor(1, 1);
    mediaSplitter->setSizes({460, 230});

    auto *splitter = new QSplitter(Qt::Horizontal, page);
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

QWidget *MainWindow::createDatabasePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(26, 22, 26, 18);
    layout->setSpacing(14);

    auto *addPersonButton = new FancyButton(Icons::Name::UserPlus, QStringLiteral("شخص جديد"), FancyButton::Kind::Primary, page);
    auto *openFolderButton = new FancyButton(Icons::Name::Folder, QStringLiteral("فتح المجلد"), FancyButton::Kind::Secondary, page);
    connect(addPersonButton, &QAbstractButton::clicked, this, &MainWindow::addPerson);
    connect(openFolderButton, &QAbstractButton::clicked, this, &MainWindow::openDatabaseFolder);
    auto *alertLogButton = new FancyButton(Icons::Name::History, QStringLiteral("السجلات والتقارير"), FancyButton::Kind::Secondary, page);
    alertLogButton->setToolTip(QStringLiteral("سجل التنبيهات، الزوار المجهولون، تسجيلات الكاميرات، تقرير اليوم، سجل الأحداث"));
    connect(alertLogButton, &QAbstractButton::clicked, this, [this, alertLogButton] {
        QMenu menu(this);
        addRecordsMenu(&menu);
        menu.exec(alertLogButton->mapToGlobal(QPoint(0, alertLogButton->height())));
    });
    m_importButton = new FancyButton(Icons::Name::Collect, QStringLiteral("استيراد"), FancyButton::Kind::Secondary, page);
    m_importButton->setToolTip(QStringLiteral("إضافة أشخاص كثيرين دفعة واحدة من مجلد: كل مجلد فرعي شخص وفيه صوره"));
    m_importButton->setEnabled(false);
    connect(m_importButton, &QAbstractButton::clicked, this, &MainWindow::importPersons);
    auto *backupButton = new FancyButton(Icons::Name::Save, QStringLiteral("نسخ احتياطي"), FancyButton::Kind::Secondary, page);
    backupButton->setToolTip(QStringLiteral("حفظ نسخة احتياطية من القاعدة كلها في ملف واحد، أو استعادتها"));
    connect(backupButton, &QAbstractButton::clicked, this, [this, backupButton] {
        QMenu menu(this);
        menu.addAction(Icons::icon(Icons::Name::Save, Theme::Text, 18), QStringLiteral("إنشاء نسخة احتياطية…"), this, &MainWindow::backupDatabase);
        menu.addAction(Icons::icon(Icons::Name::History, Theme::Text, 18), QStringLiteral("استعادة من نسخة احتياطية…"), this, &MainWindow::restoreDatabase);
        menu.exec(backupButton->mapToGlobal(QPoint(0, backupButton->height())));
    });

    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(label(QStringLiteral("قاعدة البيانات"), QStringLiteral("title"), page));
    m_databaseSubtitle = label(QString(), QStringLiteral("subtitle"), page);
    titles->addWidget(m_databaseSubtitle);
    m_databaseSubtitle->setWordWrap(true);
    m_subtitles << m_databaseSubtitle;
    header->addLayout(titles, 1);
    header->addWidget(addPersonButton);
    header->addWidget(m_importButton);
    header->addWidget(alertLogButton);
    header->addWidget(backupButton);
    header->addWidget(openFolderButton);
    m_headerButtons << addPersonButton << m_importButton << alertLogButton << backupButton << openFolderButton;
    layout->addLayout(header);

    // قائمة الأشخاص
    auto *listCard = card(page);
    listCard->setMinimumWidth(220);
    auto *listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(14, 14, 14, 14);
    listLayout->setSpacing(10);
    m_personFilter = new QLineEdit(listCard);
    m_personFilter->setPlaceholderText(QStringLiteral("بحث بالاسم أو رقم الهوية…"));
    m_personFilter->setClearButtonEnabled(true);
    m_personFilter->addAction(Icons::icon(Icons::Name::Search, Theme::Muted, 18), QLineEdit::LeadingPosition);
    // البحث يجري في القاعدة بعد توقف الكتابة قليلاً، لا مع كل حرف
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(250);
    connect(m_personFilter, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_filterTimer, &QTimer::timeout, this, &MainWindow::applyPersonFilter);
    m_personModel = new PersonListModel(&m_database, this);
    m_personList = new QListView(listCard);
    m_personList->setModel(m_personModel);
    m_personList->setItemDelegate(new PersonCardDelegate(m_personList));
    m_personList->setUniformItemSizes(true);
    m_personList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_personList->setMouseTracking(true);
    connect(m_personList->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::showPersonDetails);
    connect(m_personList, &QListView::doubleClicked, this, &MainWindow::editPerson);
    listLayout->addWidget(m_personFilter);
    listLayout->addWidget(m_personList, 1);

    // تفاصيل الشخص
    auto *detailsCard = card(page);
    auto *detailsLayout = new QVBoxLayout(detailsCard);
    detailsLayout->setContentsMargins(22, 20, 22, 18);
    detailsLayout->setSpacing(14);

    auto *profile = new QHBoxLayout;
    profile->setSpacing(18);
    m_personAvatar = new QLabel(detailsCard);
    m_personAvatar->setFixedSize(96, 96);
    profile->addWidget(m_personAvatar, 0, Qt::AlignTop);
    m_personDetails = new QLabel(detailsCard);
    m_personDetails->setTextFormat(Qt::RichText);
    m_personDetails->setWordWrap(true);
    m_personDetails->setAlignment(Qt::AlignTop);
    m_personDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    profile->addWidget(m_personDetails, 1);
    m_strengthMeter = new StrengthMeter(detailsCard);
    profile->addWidget(m_strengthMeter, 0, Qt::AlignTop);
    detailsLayout->addLayout(profile);

    // تنبيه احتمال التكرار: شخص آخر في القاعدة يشبهه بدرجة عالية
    m_duplicateBanner = card(detailsCard, QStringLiteral("statCard"));
    auto *duplicateLayout = new QHBoxLayout(m_duplicateBanner);
    duplicateLayout->setContentsMargins(12, 8, 12, 8);
    auto *duplicateIcon = new QLabel(m_duplicateBanner);
    duplicateIcon->setPixmap(Icons::icon(Icons::Name::Copy, Theme::Accent2, 20).pixmap(20, 20));
    duplicateLayout->addWidget(duplicateIcon);
    m_duplicateText = new QLabel(m_duplicateBanner);
    m_duplicateText->setWordWrap(true);
    duplicateLayout->addWidget(m_duplicateText, 1);
    auto *duplicateOpen = new FancyButton(Icons::Name::Users, QStringLiteral("فتح"), FancyButton::Kind::Secondary, m_duplicateBanner);
    auto *duplicateMerge = new FancyButton(Icons::Name::Merge, QStringLiteral("دمج الملفين"), FancyButton::Kind::Primary, m_duplicateBanner);
    duplicateMerge->setToolTip(QStringLiteral("ينقل صور الشخص الآخر وسجل ظهوره إلى هذا الملف ثم يحذفه"));
    connect(duplicateOpen, &QAbstractButton::clicked, this, [this] { showPerson(m_duplicateId); });
    connect(duplicateMerge, &QAbstractButton::clicked, this, &MainWindow::mergeDuplicate);
    duplicateLayout->addWidget(duplicateOpen);
    duplicateLayout->addWidget(duplicateMerge);
    m_duplicateBanner->setVisible(false);
    detailsLayout->addWidget(m_duplicateBanner);

    m_personActions = new QWidget(detailsCard);
    auto *actions = new FlowLayout(m_personActions, 10, 8);
    actions->setContentsMargins(0, 0, 0, 0);
    auto *editButton = new FancyButton(Icons::Name::Edit, QStringLiteral("تعديل البيانات"), FancyButton::Kind::Secondary, m_personActions);
    auto *addPhotosButton = new FancyButton(Icons::Name::PhotoAdd, QStringLiteral("إضافة صور"), FancyButton::Kind::Primary, m_personActions);
    auto *harvestButton = new FancyButton(Icons::Name::Collect, QStringLiteral("جمع صور من فيديو"), FancyButton::Kind::Secondary, m_personActions);
    harvestButton->setToolTip(QStringLiteral("يبحث عن الشخص في فيديو ويقترح أوضح لقطاته وأكثرها تنوعاً لإضافتها إلى صوره"));
    auto *exportButton = new FancyButton(Icons::Name::Export, QStringLiteral("تصدير PDF"), FancyButton::Kind::Secondary, m_personActions);
    auto *removeButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف الشخص"), FancyButton::Kind::Danger, m_personActions);
    m_watchButton = new FancyButton(Icons::Name::Alert, QStringLiteral("إضافة للمراقبة"), FancyButton::Kind::Secondary, m_personActions);
    m_watchButton->setToolTip(QStringLiteral("قائمة المراقبة: ينبّهك البرنامج بصوت وشريط تنبيه فور ظهور الشخص في صورة أو فيديو أو الكاميرا.\n"
                                             "يضيف الزر التصنيف «مراقبة» إلى بيانات الشخص (والتصنيف «مطلوب» يعمل بالمثل)."));
    connect(m_watchButton, &QAbstractButton::clicked, this, &MainWindow::toggleWatch);
    connect(editButton, &QAbstractButton::clicked, this, &MainWindow::editPerson);
    connect(addPhotosButton, &QAbstractButton::clicked, this, &MainWindow::addPhotos);
    connect(harvestButton, &QAbstractButton::clicked, this, [this] { harvestPhotos(selectedPersonId()); });
    connect(exportButton, &QAbstractButton::clicked, this, &MainWindow::exportPerson);
    connect(removeButton, &QAbstractButton::clicked, this, &MainWindow::removePerson);
    actions->addWidget(addPhotosButton);
    actions->addWidget(harvestButton);
    actions->addWidget(editButton);
    actions->addWidget(exportButton);
    actions->addWidget(m_watchButton);
    actions->addWidget(removeButton);
    m_harvestButton = harvestButton;
    detailsLayout->addWidget(m_personActions);

    auto *photosHeader = new QHBoxLayout;
    m_photosTab = new FancyButton(Icons::Name::Image, QStringLiteral("صور الوجه"), FancyButton::Kind::Nav, detailsCard);
    m_sightingsTab = new FancyButton(Icons::Name::History, QStringLiteral("سجل الظهور"), FancyButton::Kind::Nav, detailsCard);
    for (FancyButton *tab : {m_photosTab, m_sightingsTab}) {
        tab->setAutoExclusive(true);
        tab->setFixedWidth(170);
        photosHeader->addWidget(tab);
    }
    m_photosTab->setChecked(true);
    m_photosHint = label(QStringLiteral("كلما زادت الصور وتنوعت زادت دقة التعرف"), QStringLiteral("muted"), detailsCard);
    photosHeader->addWidget(m_photosHint, 1);
    photosHeader->addStretch();
    m_coverButton = new FancyButton(Icons::Name::Star, QStringLiteral("صورة رئيسية"), FancyButton::Kind::Secondary, detailsCard);
    m_coverButton->setToolTip(QStringLiteral("تعيين الصورة المحددة صورةً رئيسية تمثل الشخص في القوائم"));
    connect(m_coverButton, &QAbstractButton::clicked, this, &MainWindow::setCoverPhoto);
    photosHeader->addWidget(m_coverButton);
    m_removePhotoButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف الصورة"), FancyButton::Kind::Danger, detailsCard);
    connect(m_removePhotoButton, &QAbstractButton::clicked, this, &MainWindow::removePhoto);
    photosHeader->addWidget(m_removePhotoButton);
    m_headerButtons << m_coverButton << m_removePhotoButton;
    detailsLayout->addLayout(photosHeader);

    m_photoList = new QListWidget(detailsCard);
    m_photoList->setObjectName(QStringLiteral("photoGrid"));
    m_photoList->setViewMode(QListView::IconMode);
    m_photoList->setIconSize(QSize(132, 132));
    m_photoList->setGridSize(QSize(156, 172));
    m_photoList->setResizeMode(QListView::Adjust);
    m_photoList->setMovement(QListView::Static);
    m_photoList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_photoList, &QListWidget::currentRowChanged, this, [this] {
        m_removePhotoButton->setEnabled(m_photoList->currentItem());
        m_coverButton->setEnabled(m_photoList->currentItem());
    });
    connect(m_photoList, &QListWidget::itemDoubleClicked, this, &MainWindow::setCoverPhoto);

    m_sightingList = new QListWidget(detailsCard);
    m_sightingList->setItemDelegate(new FaceCardDelegate(m_sightingList));
    m_sightingList->setUniformItemSizes(true);
    m_sightingList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_sightingList->setMouseTracking(true);
    m_sightingList->setToolTip(QStringLiteral("انقر نقراً مزدوجاً لفتح الصورة أو الفيديو الذي ظهر فيه"));
    connect(m_sightingList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const QString source = item->data(CardRole::Id).toString();
        if (!QFileInfo::exists(source)) {
            Toast::show(this, QStringLiteral("الملف غير موجود: %1").arg(QDir::toNativeSeparators(source)), Toast::Kind::Warning);
            return;
        }
        if (!m_ready || m_videoRunning || m_analyzing)
            return;
        m_navGroup->button(0)->click();
        openSighting(source, item->data(SightingPositionRole).toLongLong());
    });
    m_profileStack = new QStackedWidget(detailsCard);
    m_profileStack->addWidget(m_photoList);
    m_profileStack->addWidget(m_sightingList);
    connect(m_photosTab, &QAbstractButton::clicked, this, [this] {
        m_profileStack->setCurrentIndex(0);
        m_coverButton->setVisible(true);
        m_removePhotoButton->setVisible(!selectedPersonId().isEmpty());
    });
    connect(m_sightingsTab, &QAbstractButton::clicked, this, [this] {
        m_profileStack->setCurrentIndex(1);
        m_coverButton->setVisible(false);
        m_removePhotoButton->setVisible(false);
    });
    detailsLayout->addWidget(m_profileStack, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    splitter->setHandleWidth(14);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(listCard);
    splitter->addWidget(detailsCard);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({330, 800});
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
    m_pauseButton->setEnabled(m_videoRunning);
    m_pauseButton->setVisible(m_videoRunning);
    updateRecordButton();
    m_saveFacesButton->setEnabled(!m_faces.isEmpty());
    m_enrollFaceButton->setEnabled(m_ready && m_faceList->currentRow() >= 0);
    updateVideoOptions();
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
    m_videoMode = false;
    m_timelineCard->setVisible(false);
    setVideoSettingsVisible(false);
    if (path != m_currentImagePath)
        m_view->resetZoom();
    m_currentImagePath = path;
    updateFolderNavigation();
    clearWatchAlerts();
    addRecentFile(path);
    m_faces.clear();
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
    menu.addSeparator();
    addRecordsMenu(menu.addMenu(icon(Icons::Name::History), QStringLiteral("السجلات والتقارير")));
    addBackgroundMenu(&menu);
    menu.exec(m_cameraButton->mapToGlobal(QPoint(0, m_cameraButton->height())));
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
    clearWatchAlerts();
    if (camera)
        m_cameraWatch->reset(source, m_cameraName, m_recorder);
    if (!source.startsWith(QStringLiteral("camera:")))
        addRecentFile(source);
    m_faces.clear();
    m_hiddenFaces.clear();
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
    m_videoWorker->prepareStart();
    QMetaObject::invokeMethod(m_videoWorker, "process", Qt::QueuedConnection, Q_ARG(QString, source));
}

void MainWindow::stopVideo()
{
    m_videoWorker->requestStop();
}

void MainWindow::togglePause()
{
    if (!m_videoRunning)
        return;
    m_videoPaused = !m_videoPaused;
    m_videoWorker->setPaused(m_videoPaused);
    m_pauseButton->setIconName(m_videoPaused ? Icons::Name::Play : Icons::Name::Pause);
    m_pauseButton->setText(m_videoPaused ? QStringLiteral("استئناف") : QStringLiteral("إيقاف مؤقت"));
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
    const CameraRecorder::Options options = CameraRecorder::savedOptions();
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
        if (m_recorder->isWaitingForMotion())
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

void MainWindow::onFrameReady(const QImage &frame, const QList<FaceItem> &faces, qint64 positionMs, double progress)
{
    m_videoWorker->frameShown();
    if (!m_videoRunning)
        return;
    m_view->setBusy(false);
    m_view->setImage(frame);
    m_view->setFaces(faces);
    m_videoPositionMs = positionMs;
    if (m_videoSource.startsWith(QStringLiteral("camera:")))
        noteCameraFaces(frame, faces);
    if (!m_timelineCard->isHidden()) {
        m_timeline->setPosition(positionMs);
        m_timeline->setAnalyzedUntil(positionMs);
        if (m_timeline->wantsThumbnail(positionMs))
            m_timeline->addThumbnail(positionMs, frame);
    }
    if (progress >= 0.0) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(int(progress * 1000.0));
    }
    const qint64 endMs = m_videoEndMs > 0 ? m_videoEndMs : m_videoDurationMs;
    QString status = endMs > 0 ? QStringLiteral("الموضع %1 من %2").arg(ImageIO::formatTime(positionMs), ImageIO::formatTime(endMs))
                               : QStringLiteral("الموضع %1").arg(ImageIO::formatTime(positionMs));
    const double seconds = m_videoClock.isValid() ? (m_videoClock.elapsed() - m_pausedMs) / 1000.0 : 0.0;
    if (seconds > 1.0 && positionMs > m_videoStartMs) {
        // السرعة بالنسبة لزمن التشغيل العادي: 2× تعني أن دقيقة من الفيديو تُحلَّل في نصف دقيقة
        const double speed = (positionMs - m_videoStartMs) / 1000.0 / seconds;
        status += QStringLiteral(" · السرعة %1×").arg(QLocale().toString(speed, 'f', 1));
        if (endMs > positionMs && speed > 0.0)
            status += QStringLiteral(" · المتبقي %1").arg(ImageIO::formatTime(qint64((endMs - positionMs) / speed)));
    }
    if (m_videoStep > 1)
        status += QStringLiteral(" · تحليل إطار من كل %1").arg(m_videoStep);
    status += QStringLiteral(" · وجوه في الإطار: %1 · أشخاص مختلفون: %2").arg(faces.size()).arg(m_faces.size());
    if (m_videoPaused)
        status.prepend(QStringLiteral("متوقف مؤقتاً · "));
    statusBar()->showMessage(status);
}

void MainWindow::onTrackUpdated(const FaceItem &track)
{
    if (m_hiddenFaces.contains(track.id))
        return;
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
        if (m_faceList->currentRow() == i)
            showFaceDetails();
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
    // قبل إعادة معالجات الملفات، فلا تُطبَّق على آخر إطارات التسجيل
    const QString recorded = stopRecording();
    if (!m_cameraId.isEmpty()) {
        VideoFilters::setCurrent(m_fileFilters, false);
        m_cameraId.clear();
        updateFiltersButton();
    }
    m_timeline->setAnalyzedUntil(-1);
    rebuildTimelineLanes();
    m_view->setBusy(false);
    m_progress->setVisible(false);
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
        const QString summary = QStringLiteral("انتهى التحليل — أشخاص مختلفون: %1 · انقر نقراً مزدوجاً على وجه لعرض لقطاته")
                                    .arg(m_faces.size());
        statusBar()->showMessage(recorded.isEmpty() ? summary : summary + QStringLiteral(" · ") + recorded);
        Toast::show(this, recorded.isEmpty() ? summary : QStringLiteral("انتهى التحليل وحُفظ تسجيل الكاميرا"), Toast::Kind::Info);
    }
    setBusy(false);
}

// ───────────── إدارة الوجوه المكتشفة ─────────────

void MainWindow::rebuildFaceRows()
{
    m_faceRows.clear();
    for (int i = 0; i < m_faces.size(); ++i)
        m_faceRows.insert(m_faces.at(i).id, i);
}

bool MainWindow::faceMatchesFilter(const FaceItem &face) const
{
    switch (m_faceStatusFilter->currentIndex()) {
    case 1:
        if (face.match.status != MatchStatus::Confirmed)
            return false;
        break;
    case 2:
        if (face.match.status != MatchStatus::Possible)
            return false;
        break;
    case 3:
        if (face.match.status != MatchStatus::Unknown)
            return false;
        break;
    default:
        break;
    }
    const QString text = m_faceFilter->text().trimmed();
    return text.isEmpty() || matchLabel(face.match).contains(text, Qt::CaseInsensitive);
}

void MainWindow::applyFaceFilter()
{
    int shown = 0;
    for (int i = 0; i < m_faces.size() && i < m_faceList->count(); ++i) {
        const bool visible = faceMatchesFilter(m_faces.at(i));
        m_faceList->setRowHidden(i, !visible);
        shown += visible ? 1 : 0;
    }
    m_laneTimer->start();
    const bool filtered = !m_faceFilter->text().trimmed().isEmpty() || m_faceStatusFilter->currentIndex() != 0;
    m_faceCount->setText(filtered ? QStringLiteral("%1 / %2").arg(shown).arg(m_faces.size()) : QString::number(m_faces.size()));
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
    }
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
        lanes.append(lane);
    }
    m_timeline->setLanes(lanes);
}

void MainWindow::loadVideoFrame(qint64 positionMs)
{
    // أثناء التحليل يبقى المؤشر على موضع التحليل
    if (m_videoRunning || m_videoSource.isEmpty() || m_videoSource.startsWith(QStringLiteral("camera:"))) {
        if (m_videoRunning)
            m_timeline->setPosition(m_videoPositionMs);
        return;
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
    for (FaceItem &face : m_faces)
        face.match = FaceDatabase::identify(*gallery, face.embedding, threshold());
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

void MainWindow::updateFaceCount()
{
    if (m_faceFilter->text().trimmed().isEmpty() && m_faceStatusFilter->currentIndex() == 0)
        m_faceCount->setText(QString::number(m_faces.size()));
    else
        applyFaceFilter();
}

void MainWindow::updateFaceListItem(QListWidgetItem *listItem, const FaceItem &face)
{
    if (!listItem)
        return;
    QString subtitle = QStringLiteral("جودة الصورة %1%").arg(qRound(face.quality * 100));
    if (m_videoMode) {
        subtitle += QStringLiteral(" · ظهر في %1 إطار").arg(face.frames);
        if (face.firstSeenMs >= 0)
            subtitle += QStringLiteral(" · ") + ImageIO::formatTime(face.firstSeenMs);
    }
    listItem->setData(CardRole::Id, face.id);
    listItem->setData(CardRole::Title, matchLabel(face.match));
    listItem->setData(CardRole::Subtitle, subtitle);
    listItem->setData(CardRole::Status, int(face.match.status));
    listItem->setData(CardRole::Score, double(face.match.score));
    if (!listItem->data(CardRole::Avatar).isValid() || m_videoMode)
        listItem->setData(CardRole::Avatar, circularPixmap(ImageIO::faceCrop(face), 54));
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

// أقرب المرشحين في القاعدة مع نسبهم، حتى يرى المستخدم البدائل حين يكون التطابق غير مؤكد ويصحح بنقرة
QString MainWindow::candidatesHtml(const FaceItem &face) const
{
    QString html;
    if (!face.embedding.empty() && m_database.personCount() > 0) {
        GalleryPtr gallery = m_database.gallery();
        const QList<MatchResult> candidates = FaceDatabase::candidates(*gallery, face.embedding, 3, threshold());
        if (!candidates.isEmpty()) {
            html += QStringLiteral("<br><br><span style='color:#8aa3c7'>أقرب المرشحين في القاعدة:</span>");
            html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='2'>");
            for (const MatchResult &candidate : candidates) {
                const int percent = std::clamp(qRound(candidate.score * 100), 0, 100);
                // تصحيح بنقرة: إضافة الوجه إلى صور المرشح فتتحسن مطابقته في المرات القادمة
                const bool current = face.match.status == MatchStatus::Confirmed && candidate.personId == face.match.personId;
                const QString action = current
                    ? QStringLiteral("<span style='color:%1'>✓</span>").arg(matchColor(MatchStatus::Confirmed).name())
                    : QStringLiteral("<a href='assign:%1' style='color:%2; text-decoration:none'>هذا هو</a>")
                          .arg(candidate.personId.toHtmlEscaped(), Theme::Accent2.name());
                html += QStringLiteral("<tr><td style='color:%1'>● <span style='color:#e6f0ff'>%2</span></td>"
                                       "<td align='left' width='40' style='color:%1'>%3%</td>"
                                       "<td align='left' width='48'>%4</td></tr>")
                            .arg(matchColor(candidate.status).name(), candidate.name.toHtmlEscaped())
                            .arg(percent)
                            .arg(action);
            }
            html += QStringLiteral("</table>");
            html += QStringLiteral("<a href='other' style='color:%1; text-decoration:none'>ليس أحدهم؟ اختر شخصاً آخر أو أضف شخصاً جديداً…</a>")
                        .arg(Theme::Accent2.name());
        }
    }
    return html;
}

void MainWindow::showFaceDetails()
{
    const int row = m_faceList->currentRow();
    m_enrollFaceButton->setEnabled(m_ready && row >= 0);
    m_viewShotButton->setVisible(m_videoMode && !m_videoSource.startsWith(QStringLiteral("camera:")));
    m_viewShotButton->setEnabled(row >= 0 && row < m_faces.size());
    if (row < 0 || row >= m_faces.size()) {
        m_faceDetails->setText(QStringLiteral("اختر وجهاً من القائمة أو اضغط عليه في الصورة لعرض تفاصيله."));
        m_view->setSelectedFace(-1);
        return;
    }

    const FaceItem &face = m_faces.at(row);
    m_view->setSelectedFace(face.id);
    m_timeline->setSelectedLane(face.id);
    const QString color = matchColor(face.match.status).name();
    QString html = QStringLiteral("<div style='font-size:13pt; font-weight:600; color:%1'>%2</div>"
                                  "<div style='color:%3'>%4</div>")
                       .arg(Theme::Text.name(), matchLabel(face.match).toHtmlEscaped(), color, statusDescription(face.match.status));
    if (face.match.status == MatchStatus::Confirmed && m_watched.contains(face.match.personId))
        html += QStringLiteral("<div style='color:%1; font-weight:600'>⚠ هذا الشخص في قائمة المراقبة</div>").arg(Theme::Accent2.name());
    auto row_ = [](const QString &key, const QString &value) {
        return QStringLiteral("<br><span style='color:#8aa3c7'>%1:</span> <span style='color:#e6f0ff'>%2</span>").arg(key, value.toHtmlEscaped());
    };
    html += QStringLiteral("<br><span style='color:#8aa3c7'>التشابه:</span> <span style='color:#e6f0ff'>%1%</span>"
                           "&nbsp;&nbsp;·&nbsp;&nbsp;<span style='color:#8aa3c7'>جودة الوجه:</span> <span style='color:#e6f0ff'>%2%</span>")
                .arg(qRound(face.match.score * 100))
                .arg(qRound(face.quality * 100));
    if (face.quality < 0.35f)
        html += QStringLiteral("<br><span style='color:#93c5fd'>جودة الوجه منخفضة: النتيجة أقل موثوقية</span>");
    html += candidatesHtml(face);
    if (m_videoMode && face.firstSeenMs >= 0) {
        html += row_(QStringLiteral("أول ظهور"), ImageIO::formatTime(face.firstSeenMs));
        html += row_(QStringLiteral("آخر ظهور"), ImageIO::formatTime(face.lastSeenMs));
        html += row_(QStringLiteral("عدد الإطارات"), QString::number(face.frames));
        if (face.bestMs >= 0)
            html += row_(QStringLiteral("أوضح لقطة"), ImageIO::formatTime(face.bestMs));
        if (face.appearances.size() > 1) {
            QStringList times;
            for (qsizetype i = 0; i < std::min<qsizetype>(face.appearances.size(), 6); ++i)
                times << ImageIO::formatTime(face.appearances.at(i).first);
            if (face.appearances.size() > 6)
                times << QStringLiteral("…");
            html += row_(QStringLiteral("مرات الظهور (%1)").arg(face.appearances.size()), times.join(QStringLiteral("، ")));
        }
    }
    if (!face.match.personId.isEmpty()) {
        const Person person = m_database.person(face.match.personId);
        if (!person.nationalId.isEmpty())
            html += row_(QStringLiteral("رقم الهوية"), person.nationalId);
        if (!person.phone.isEmpty())
            html += row_(QStringLiteral("الهاتف"), person.phone);
        if (!person.tags.isEmpty())
            html += row_(QStringLiteral("التصنيفات"), person.tags);
        if (!person.notes.isEmpty())
            html += row_(QStringLiteral("ملاحظات"), person.notes);
    }
    m_faceDetails->setText(html);
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
    ++m_alertCount;
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
    QApplication::beep();
    statusBar()->showMessage(QStringLiteral("تنبيه مراقبة: ظهر %1").arg(face.match.name), 10000);
    emit watchAlert(face.match.name, camera ? m_cameraName : QString());
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
            if (face.match.status != MatchStatus::Possible)
                item.crop = ImageIO::faceCrop(card);
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

void MainWindow::showAlertLog()
{
    AlertLogDialog dialog(&m_database, this);
    connect(&dialog, &AlertLogDialog::showPersonRequested, this, &MainWindow::showPerson);
    connect(&dialog, &AlertLogDialog::openFileRequested, this, [this](const QString &path, qint64 positionMs) {
        if (!m_ready || m_videoRunning || m_analyzing) {
            Toast::show(this, QStringLiteral("انتظر حتى ينتهي التحليل الحالي"), Toast::Kind::Warning);
            return;
        }
        m_navGroup->button(0)->click();
        m_lastDirectory = QFileInfo(path).absolutePath();
        openSighting(path, positionMs);
    });
    dialog.exec();
}

void MainWindow::addRecordsMenu(QMenu *menu)
{
    const auto icon = [](Icons::Name name) { return Icons::icon(name, Theme::Text, 18); };
    menu->addAction(icon(Icons::Name::Alert), QStringLiteral("سجل التنبيهات…"), this, [this] {
        showFromTray();
        showAlertLog();
    });
    menu->addAction(icon(Icons::Name::Users), QStringLiteral("الزوار المجهولون…"), this, [this] {
        showFromTray();
        showVisitors();
    });
    menu->addAction(icon(Icons::Name::Film), QStringLiteral("تسجيلات الكاميرات (يوم بيوم)…"), this, [this] {
        showFromTray();
        showRecordings();
    });
    menu->addAction(icon(Icons::Name::Export), QStringLiteral("تقرير يومي…"), this, [this] {
        showFromTray();
        showDailyReport();
    });
    menu->addSeparator();
    menu->addAction(icon(Icons::Name::Info), QStringLiteral("سجل الأحداث (الانقطاع والتسجيل والأعطال)…"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppLog::folder()));
    });
}

void MainWindow::showVisitors()
{
    VisitorsDialog dialog(&m_database, this);
    connect(&dialog, &VisitorsDialog::openFileRequested, this, [this](const QString &path, qint64 positionMs) {
        if (!m_ready || m_videoRunning || m_analyzing) {
            Toast::show(this, QStringLiteral("انتظر حتى ينتهي التحليل الحالي (أو أوقف الكاميرا)"), Toast::Kind::Warning);
            return;
        }
        m_navGroup->button(0)->click();
        m_lastDirectory = QFileInfo(path).absolutePath();
        openSighting(path, positionMs);
    });
    connect(&dialog, &VisitorsDialog::assignRequested, this, [this, &dialog](qint64 visitorId) {
        const Visitor visitor = m_database.visitor(visitorId);
        std::vector<float> embedding = m_database.visitorEmbedding(visitorId);
        if (visitor.id <= 0 || embedding.size() != FaceEngine::EmbeddingSize) {
            Toast::show(&dialog, QStringLiteral("تعذر قراءة بصمة الزائر"), Toast::Kind::Warning);
            return;
        }
        const QString personId = choosePerson(QString());
        if (personId.isEmpty())
            return;
        // البصمة متوسط كل إطارات ظهوره، فهي أثبت من صورة واحدة
        if (!m_database.assignVisitor(visitorId, personId, visitor.image, {embedding}, visitor.quality)) {
            Toast::show(&dialog, QStringLiteral("تعذر نقل الزائر إلى الشخص"), Toast::Kind::Warning);
            return;
        }
        Toast::show(&dialog, QStringLiteral("أصبح الزائر #%1 «%2»، وانتقل ظهوره إلى سجله")
                                 .arg(visitorId)
                                 .arg(m_database.person(personId).name));
        dialog.reload();
    });
    dialog.exec();
}

void MainWindow::openSighting(const QString &path, qint64 positionMs)
{
    if (positionMs < 0 || !isVideoFile(path)) {
        openFile(path);
        return;
    }
    // التحليل يبدأ من لحظة الظهور مباشرة، لا من أول الفيديو (تسجيل الكاميرا قد يطول 15 دقيقة)
    m_rangeStart->setTime(QTime(0, 0).addMSecs(int(positionMs)));
    m_rangeEnd->setTime(QTime(0, 0));
    startVideo(path);
    Toast::show(this, QStringLiteral("ظهر الشخص عند \u2066%1\u2069 من الفيديو؛ التحليل يبدأ منها، وامسح «الجزء: من» لتحليله كله")
                          .arg(ImageIO::formatTime(positionMs)),
                Toast::Kind::Info);
}

void MainWindow::toggleWatch()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    Person person = m_database.person(id);
    const bool watch = !FaceDatabase::isWatched(person.tags);
    person.tags = FaceDatabase::withWatchTag(person.tags, watch);
    if (!m_database.updatePerson(person)) {
        Toast::show(this, QStringLiteral("تعذر حفظ التغيير"), Toast::Kind::Warning);
        return;
    }
    Toast::show(this, watch ? QStringLiteral("أُضيف %1 إلى قائمة المراقبة: ستُنبَّه فور ظهوره").arg(person.name)
                            : QStringLiteral("أُزيل %1 من قائمة المراقبة").arg(person.name));
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
    QSettings settings;
    const QStringList files = settings.value(QStringLiteral("recentFiles")).toStringList();
    QMenu menu(this);
    if (files.isEmpty())
        menu.addAction(QStringLiteral("لم يُفتح أي ملف بعد"))->setEnabled(false);
    for (const QString &file : files) {
        const QFileInfo info(file);
        const bool video = isVideoFile(file);
        QAction *action = menu.addAction(Icons::icon(video ? Icons::Name::Video : Icons::Name::Image, Theme::Text, 18),
                                         QStringLiteral("\u2068%1\u2069").arg(info.fileName()), this, [this, file] {
                                             m_lastDirectory = QFileInfo(file).absolutePath();
                                             openFile(file);
                                         });
        action->setToolTip(QDir::toNativeSeparators(file));
        action->setEnabled(info.exists() && m_openImageButton->isEnabled());
    }
    if (!files.isEmpty()) {
        menu.addSeparator();
        menu.addAction(Icons::icon(Icons::Name::Trash, Theme::Text, 18), QStringLiteral("مسح القائمة"), this,
                       [] { QSettings().remove(QStringLiteral("recentFiles")); });
    }
    menu.setToolTipsVisible(true);
    menu.exec(m_recentButton->mapToGlobal(QPoint(0, m_recentButton->height())));
}

void MainWindow::setVideoSettingsVisible(bool visible)
{
    for (QWidget *widget : std::as_const(m_videoSettings))
        widget->setVisible(visible);
    m_recordGroup->setVisible(visible && m_videoSource.startsWith(QStringLiteral("camera:")));
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

void MainWindow::importPersons()
{
    if (!m_ready)
        return;
    ImportDialog dialog(&m_engine, &m_database, threshold(), m_lastDirectory, this);
    connect(&dialog, &ImportDialog::showPersonRequested, this, [this](const QString &id) {
        QTimer::singleShot(0, this, [this, id] { showPerson(id); });
    });
    QTimer::singleShot(0, &dialog, &ImportDialog::chooseFolder);
    dialog.exec();
    if (!dialog.lastDirectory().isEmpty())
        m_lastDirectory = dialog.lastDirectory();
}

void MainWindow::saveFaces()
{
    if (m_faces.isEmpty())
        return;
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("اختر مجلد حفظ الوجوه"));
    if (folder.isEmpty())
        return;

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
    for (int i = 0; i < m_faces.size(); ++i) {
        const FaceItem &face = m_faces.at(i);
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
    const bool html = Report::writeHtml(htmlPath, m_faces, source);
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

// جمع صور شخص من القاعدة بالبحث عنه في فيديو يختاره المستخدم
void MainWindow::harvestPhotos(const QString &personId)
{
    if (personId.isEmpty() || !m_ready)
        return;
    const std::vector<float> target = m_database.personEmbedding(personId);
    if (target.empty()) {
        Toast::show(this, QStringLiteral("أضف صورة واحدة للشخص على الأقل حتى يُبحث عنه في الفيديو"), Toast::Kind::Warning);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("اختر فيديو يظهر فيه الشخص"), m_lastDirectory, VideoFilter);
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    const Person person = m_database.person(personId);
    HarvestDialog dialog(&m_engine, path, target, {}, threshold(), person.name, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const int added = enrollFaces(personId, dialog.selectedFaces());
    Toast::show(this, QStringLiteral("أضيفت %1 صور إلى ملف %2").arg(added).arg(person.name),
                added > 0 ? Toast::Kind::Success : Toast::Kind::Warning);
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

QString MainWindow::selectedPersonId() const
{
    if (!m_personList)
        return {};
    return m_personModel->idAt(m_personList->currentIndex().row());
}

void MainWindow::refreshStats()
{
    m_personCountLabel->setText(QLocale().toString(m_database.personCount()));
    m_photoCountLabel->setText(QLocale().toString(m_database.photoCount()));
    updateDatabaseSubtitle();
}

void MainWindow::updateDatabaseSubtitle()
{
    const QString filter = m_personModel ? m_personModel->filter() : QString();
    if (filter.isEmpty()) {
        m_databaseSubtitle->setText(QStringLiteral("%1 شخص مسجل · %2 صورة وجه · محفوظة على هذا الجهاز فقط")
                                        .arg(QLocale().toString(m_database.personCount()), QLocale().toString(m_database.photoCount())));
        return;
    }
    constexpr qint64 Cap = 10000;
    const qint64 count = m_database.countPersons(filter, Cap + 1);
    m_databaseSubtitle->setText(count > Cap ? QStringLiteral("نتائج البحث: أكثر من %1").arg(QLocale().toString(Cap))
                                            : QStringLiteral("نتائج البحث: %1").arg(QLocale().toString(count)));
}

void MainWindow::refreshPersons()
{
    if (!m_personList)
        return;
    const QString selected = selectedPersonId();
    {
        const QSignalBlocker blocker(m_personList->selectionModel());
        m_personModel->reload();
        const int row = m_personModel->rowOf(selected);
        if (row >= 0)
            m_personList->setCurrentIndex(m_personModel->index(row));
    }
    showPersonDetails();
}

// يفتح ملف شخص في صفحة قاعدة البيانات
void MainWindow::showPerson(const QString &personId)
{
    const Person person = m_database.person(personId);
    if (person.id.isEmpty())
        return;
    m_navGroup->button(1)->click();
    int row = m_personModel->rowOf(personId);
    if (row < 0) {
        m_personFilter->setText(person.name);
        m_filterTimer->stop();
        applyPersonFilter();
        row = m_personModel->rowOf(personId);
    }
    if (row >= 0)
        m_personList->setCurrentIndex(m_personModel->index(row));
}

void MainWindow::applyPersonFilter()
{
    {
        const QSignalBlocker blocker(m_personList->selectionModel());
        m_personModel->setFilter(m_personFilter->text());
    }
    updateDatabaseSubtitle();
    showPersonDetails();
}

void MainWindow::showPersonDetails()
{
    const QString id = selectedPersonId();
    m_photoList->clear();
    m_sightingList->clear();
    m_removePhotoButton->setEnabled(false);
    m_coverButton->setEnabled(false);
    m_personActions->setVisible(!id.isEmpty());
    const bool photosShown = m_profileStack->currentIndex() == 0;
    m_removePhotoButton->setVisible(!id.isEmpty() && photosShown);
    m_coverButton->setVisible(!id.isEmpty() && photosShown);
    m_strengthMeter->setVisible(!id.isEmpty());
    m_photosTab->setVisible(!id.isEmpty());
    m_sightingsTab->setVisible(!id.isEmpty());
    m_profileStack->setVisible(!id.isEmpty());
    m_duplicateBanner->setVisible(false);
    m_duplicateId.clear();

    if (id.isEmpty()) {
        m_personAvatar->setPixmap(QPixmap());
        m_personDetails->setText(QStringLiteral("<div style='font-size:15pt; font-weight:600'>اختر شخصاً</div>"
                                                "<div style='color:#8aa3c7'>اختر شخصاً من القائمة لعرض بياناته وصوره، "
                                                "أو اضغط \"شخص جديد\" لإضافة شخص.</div>"));
        m_photosTab->setText(QStringLiteral("صور الوجه"));
        m_sightingsTab->setText(QStringLiteral("سجل الظهور"));
        return;
    }

    const Person person = m_database.person(id);
    const bool watched = FaceDatabase::isWatched(person.tags);
    m_watchButton->setText(watched ? QStringLiteral("إلغاء المراقبة") : QStringLiteral("إضافة للمراقبة"));
    m_personAvatar->setPixmap(person.coverPhoto <= 0
                                  ? Icons::icon(Icons::Name::Users, Theme::Muted, 96).pixmap(96, 96)
                                  : circularPixmap(m_database.photo(person.coverPhoto, 192), 96));

    auto chip = [](const QString &key, const QString &value) {
        return QStringLiteral("<span style='color:#8aa3c7'>%1</span> <span style='color:#e6f0ff'>%2</span>&nbsp;&nbsp;&nbsp; ")
            .arg(key, value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<div style='font-size:18pt; font-weight:700'>%1</div>").arg(person.name.toHtmlEscaped());
    if (!person.tags.isEmpty()) {
        html += QStringLiteral("<div style='margin:2px 0 4px 0'>");
        for (const QString &tag : person.tags.split(QStringLiteral("، "), Qt::SkipEmptyParts))
            html += QStringLiteral("<span style='background:#13284a; color:#38bdf8; font-weight:600'>&nbsp;%1&nbsp;</span>&nbsp; ")
                        .arg(tag.toHtmlEscaped());
        html += QStringLiteral("</div>");
    }
    html += QStringLiteral("<div>");
    if (!person.nationalId.isEmpty())
        html += chip(QStringLiteral("رقم الهوية:"), person.nationalId);
    if (!person.gender.isEmpty())
        html += chip(QStringLiteral("الجنس:"), person.gender);
    if (person.birthDate.isValid())
        html += chip(QStringLiteral("العمر:"), QStringLiteral("%1 سنة").arg(person.birthDate.daysTo(QDate::currentDate()) / 365));
    if (!person.phone.isEmpty())
        html += chip(QStringLiteral("الهاتف:"), person.phone);
    if (!person.email.isEmpty())
        html += chip(QStringLiteral("البريد:"), person.email);
    if (!person.address.isEmpty())
        html += chip(QStringLiteral("العنوان:"), person.address);
    html += chip(QStringLiteral("أُضيف:"), person.created.toString(QStringLiteral("yyyy/MM/dd")));
    if (person.lastSeen.isValid())
        html += chip(QStringLiteral("آخر ظهور:"), QStringLiteral("%1 (%2 مرة)")
                                                       .arg(person.lastSeen.toString(QStringLiteral("yyyy/MM/dd hh:mm")))
                                                       .arg(person.seenCount));
    html += QStringLiteral("</div>");
    if (!person.notes.isEmpty())
        html += QStringLiteral("<div style='color:#8aa3c7; margin-top:6px'>%1</div>")
                    .arg(person.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));

    // قوة الملف والصور المشكوك فيها
    const QHash<qint64, float> consistency = m_database.photoConsistency(id);
    const ProfileStrength strength = profileStrength(person, consistency);
    m_strengthMeter->setStrength(strength);
    if (!strength.tips.isEmpty())
        html += QStringLiteral("<div style='color:#93c5fd; margin-top:6px'>💡 %1</div>").arg(strength.tips.first().toHtmlEscaped());
    m_personDetails->setText(html);

    for (const FacePhoto &photo : person.photos) {
        auto *item = new QListWidgetItem(m_photoList);
        const bool cover = photo.id == person.coverPhoto;
        const bool suspect = strength.suspects.contains(photo.id);
        item->setIcon(QIcon(decoratedPhoto(m_database.photo(photo.id, m_photoList->iconSize().width()), m_photoList->iconSize().width(), cover, suspect)));
        item->setText(suspect ? QStringLiteral("مشكوك فيها") : QStringLiteral("جودة %1%").arg(qRound(photo.quality * 100)));
        if (suspect)
            item->setForeground(QColor(0xfb, 0x71, 0x85));
        QString tip = QStringLiteral("جودة الوجه: %1%").arg(qRound(photo.quality * 100));
        if (consistency.contains(photo.id))
            tip += QStringLiteral("\nالتشابه مع بقية صوره: %1%").arg(qRound(consistency.value(photo.id) * 100));
        if (photo.added.isValid())
            tip += QStringLiteral("\nأُضيفت: %1").arg(photo.added.toString(QStringLiteral("yyyy/MM/dd hh:mm")));
        if (cover)
            tip += QStringLiteral("\n★ الصورة الرئيسية");
        if (suspect)
            tip += QStringLiteral("\nقد تكون لشخص آخر — راجعها");
        tip += QStringLiteral("\nانقر نقراً مزدوجاً لجعلها الصورة الرئيسية");
        item->setToolTip(tip);
        item->setData(Qt::UserRole, photo.id);
    }
    m_photosTab->setText(QStringLiteral("صور الوجه (%1)").arg(person.photos.size()));

    const QList<Sighting> sightings = m_database.sightings(id, 200);
    for (const Sighting &sighting : sightings) {
        auto *item = new QListWidgetItem(m_sightingList);
        item->setData(CardRole::Id, sighting.source);
        item->setData(SightingPositionRole, sighting.positionMs);
        item->setData(CardRole::Title, sighting.seen.toString(QStringLiteral("yyyy/MM/dd  hh:mm")));
        QString where = sighting.source.startsWith(QStringLiteral("camera:")) ? Cameras::displayName(sighting.source)
                                                                               : QFileInfo(sighting.source).fileName();
        if (!sighting.camera.isEmpty() && !sighting.source.startsWith(QStringLiteral("camera:")))
            where = Cameras::displayName(sighting.camera) + QStringLiteral(" · ") + where;
        if (sighting.positionMs >= 0)
            where += QStringLiteral(" · عند %1").arg(ImageIO::formatTime(sighting.positionMs));
        item->setData(CardRole::Subtitle, where);
        item->setData(CardRole::Status, int(MatchStatus::Confirmed));
        item->setData(CardRole::StatusText, !sighting.camera.isEmpty() || sighting.source.startsWith(QStringLiteral("camera:"))
                                                ? QStringLiteral("كاميرا")
                                                : sighting.positionMs >= 0 ? QStringLiteral("فيديو")
                                                                           : QStringLiteral("صورة"));
        item->setData(CardRole::Score, sighting.score);
        item->setData(CardRole::Avatar, circularPixmap(sighting.thumbnail, 54));
        item->setToolTip(QDir::toNativeSeparators(sighting.source));
    }
    m_sightingsTab->setText(QStringLiteral("سجل الظهور (%1)").arg(person.seenCount));

    // هل في القاعدة شخص آخر يشبهه كثيراً؟ ربما أُضيف الشخص نفسه مرتين
    const std::vector<float> embedding = m_database.personEmbedding(id);
    if (!embedding.empty()) {
        GalleryPtr gallery = m_database.gallery();
        const QList<MatchResult> similar = FaceDatabase::candidates(*gallery, embedding, 1, threshold(), id);
        if (!similar.isEmpty() && similar.first().score >= std::max(0.45f, threshold() + 0.1f)) {
            m_duplicateId = similar.first().personId;
            m_duplicateText->setText(QStringLiteral("قد يكون هذا الشخص مسجلاً مرتين: يشبه <b>%1</b> بنسبة %2%")
                                         .arg(similar.first().name.toHtmlEscaped())
                                         .arg(qRound(similar.first().score * 100)));
            m_duplicateBanner->setVisible(true);
        }
    }
}

void MainWindow::setCoverPhoto()
{
    const QString id = selectedPersonId();
    const QListWidgetItem *item = m_photoList->currentItem();
    if (id.isEmpty() || !item)
        return;
    if (m_database.setCoverPhoto(id, item->data(Qt::UserRole).toLongLong()))
        Toast::show(this, QStringLiteral("تم تعيين الصورة الرئيسية"));
}

void MainWindow::mergeDuplicate()
{
    const QString id = selectedPersonId();
    if (id.isEmpty() || m_duplicateId.isEmpty())
        return;
    const Person keep = m_database.person(id);
    const Person other = m_database.person(m_duplicateId);
    const auto answer = QMessageBox::question(
        this, QStringLiteral("دمج الملفين"),
        QStringLiteral("سيُنقل كل ما في ملف \"%1\" (%2 صور وسجل ظهوره) إلى ملف \"%3\" ثم يُحذف ملفه.\n"
                       "البيانات الفارغة هنا تُكمَّل من الملف الآخر. هل تريد المتابعة؟")
            .arg(other.name)
            .arg(other.photoCount)
            .arg(keep.name));
    if (answer != QMessageBox::Yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_database.mergePersons(id, m_duplicateId);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        Toast::show(this, QStringLiteral("تعذر دمج الملفين"), Toast::Kind::Warning);
        return;
    }
    Toast::show(this, QStringLiteral("تم دمج \"%1\" في \"%2\"").arg(other.name, keep.name));
    showPerson(id);
}

void MainWindow::exportPerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    const Person person = m_database.person(id);
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("تصدير ملف الشخص"),
                                                QDir(m_lastDirectory).filePath(safeFileName(person.name) + QStringLiteral(".pdf")),
                                                QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        path += QStringLiteral(".pdf");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = exportPersonPdf(m_database, id, path);
    QApplication::restoreOverrideCursor();
    if (ok) {
        Toast::show(this, QStringLiteral("تم تصدير ملف %1").arg(person.name));
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else {
        Toast::show(this, QStringLiteral("تعذر إنشاء ملف PDF"), Toast::Kind::Warning);
    }
}

void MainWindow::addPerson()
{
    PersonDialog dialog(Person(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString id = m_database.addPerson(dialog.person());
    if (id.isEmpty()) {
        Toast::show(this, QStringLiteral("تعذر حفظ الشخص"), Toast::Kind::Warning);
        return;
    }
    // الأحدث أولاً، فالشخص الجديد في أول القائمة
    m_personFilter->clear();
    applyPersonFilter();
    m_personModel->reload();
    const int row = m_personModel->rowOf(id);
    if (row >= 0)
        m_personList->setCurrentIndex(m_personModel->index(row));
    Toast::show(this, QStringLiteral("تمت إضافة %1 — أضف الآن صوراً لوجهه").arg(dialog.person().name));
    if (m_ready)
        addPhotosTo(id);
}

void MainWindow::editPerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    PersonDialog dialog(m_database.person(id), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (m_database.updatePerson(dialog.person()))
        Toast::show(this, QStringLiteral("تم حفظ التعديلات"));
    else
        Toast::show(this, QStringLiteral("تعذر حفظ التعديلات"), Toast::Kind::Warning);
}

void MainWindow::removePerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    const Person person = m_database.person(id);
    const auto answer = QMessageBox::question(this, QStringLiteral("حذف الشخص"),
                                              QStringLiteral("هل تريد حذف \"%1\" وكل صوره من قاعدة البيانات؟").arg(person.name));
    if (answer != QMessageBox::Yes)
        return;
    if (m_database.removePerson(id))
        Toast::show(this, QStringLiteral("تم حذف %1").arg(person.name), Toast::Kind::Info);
    else
        Toast::show(this, QStringLiteral("تعذر حذف الشخص"), Toast::Kind::Warning);
}

void MainWindow::addPhotos()
{
    const QString id = selectedPersonId();
    if (!id.isEmpty() && m_ready)
        addPhotosTo(id);
}

void MainWindow::addPhotosTo(const QString &personId)
{
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("اختر صوراً لوجه الشخص"), m_lastDirectory, ImageFilter);
    if (files.isEmpty())
        return;
    m_lastDirectory = QFileInfo(files.first()).absolutePath();
    addPhotoFiles(personId, files);
}

void MainWindow::addPhotoFiles(const QString &personId, const QStringList &files)
{
    struct Enrollment
    {
        int faces = 0;
        FaceItem face;
        std::vector<std::vector<float>> embeddings;
    };

    BatchProgressDialog progress(int(files.size()), this);
    progress.show();

    int added = 0;
    QStringList noFace;
    QStringList multipleFaces;
    const FaceEngine *engine = &m_engine;
    for (int i = 0; i < files.size() && progress.proceed(); ++i) {
        const QString file = files.at(i);
        progress.setProgress(i, QStringLiteral("جارٍ تحليل %1 (%2 من %3)…").arg(QFileInfo(file).fileName()).arg(i + 1).arg(files.size()));

        // التحليل في الخلفية حتى تبقى الواجهة وأزرار الإيقاف المؤقت والإلغاء مستجيبة
        const Enrollment result = runResponsive([engine, file] {
            Enrollment enrollment;
            const cv::Mat image = ImageIO::loadImage(file);
            std::vector<AnalyzedFace> faces = image.empty() ? std::vector<AnalyzedFace>() : engine->analyze(image, true);
            if (faces.empty() && !image.empty())
                faces = engine->analyzeRobust(image);
            enrollment.faces = int(faces.size());
            if (faces.empty())
                return enrollment;
            // عند وجود أكثر من وجه يُعتمد الأكبر
            const auto largest = std::max_element(faces.begin(), faces.end(), [](const AnalyzedFace &a, const AnalyzedFace &b) {
                return a.detection.box.area() < b.detection.box.area();
            });
            enrollment.face = ImageIO::makeFaceItem(image, *largest, 0);
            enrollment.embeddings = engine->enrollmentEmbeddings(largest->aligned);
            return enrollment;
        }, true);
        if (result.faces == 0) {
            noFace << QFileInfo(file).fileName();
            continue;
        }
        if (result.faces > 1)
            multipleFaces << QFileInfo(file).fileName();
        if (m_database.addPhoto(personId, result.face.crop, result.embeddings, result.face.quality))
            ++added;
    }
    progress.hide();

    if (noFace.isEmpty() && multipleFaces.isEmpty()) {
        Toast::show(this, QStringLiteral("أُضيفت %1 صورة").arg(added));
        return;
    }
    QString message = QStringLiteral("أُضيفت %1 صورة.").arg(added);
    if (!noFace.isEmpty())
        message += QStringLiteral("\n\nلم يُعثر على وجه في:\n") + noFace.join(QLatin1Char('\n'));
    if (!multipleFaces.isEmpty())
        message += QStringLiteral("\n\nفيها أكثر من وجه (اعتُمد الوجه الأكبر):\n") + multipleFaces.join(QLatin1Char('\n'));
    QMessageBox::information(this, QStringLiteral("إضافة صور"), message);
}

void MainWindow::removePhoto()
{
    const QString id = selectedPersonId();
    const QListWidgetItem *item = m_photoList->currentItem();
    if (id.isEmpty() || !item)
        return;
    if (QMessageBox::question(this, QStringLiteral("حذف الصورة"), QStringLiteral("هل تريد حذف هذه الصورة؟")) != QMessageBox::Yes)
        return;
    if (m_database.removePhoto(id, item->data(Qt::UserRole).toLongLong()))
        Toast::show(this, QStringLiteral("تم حذف الصورة"), Toast::Kind::Info);
    else
        Toast::show(this, QStringLiteral("تعذر حذف الصورة"), Toast::Kind::Warning);
}

void MainWindow::openDatabaseFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_database.directory()));
}

void MainWindow::backupDatabase()
{
    const QString name = QStringLiteral("ملامح-نسخة-احتياطية-%1.malamih").arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ نسخة احتياطية"), QDir(m_lastDirectory).filePath(name),
                                                      QStringLiteral("نسخ ملامح الاحتياطية (*.malamih)"));
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_database.backup(path, &error);
    QApplication::restoreOverrideCursor();
    if (ok)
        Toast::show(this, QStringLiteral("حُفظت النسخة الاحتياطية (%1 شخص، %2 ميغابايت)")
                              .arg(m_database.personCount())
                              .arg(QFileInfo(path).size() / 1048576.0, 0, 'f', 1));
    else
        QMessageBox::warning(this, QStringLiteral("نسخة احتياطية"), QStringLiteral("تعذر إنشاء النسخة الاحتياطية:\n%1").arg(error));
}

void MainWindow::restoreDatabase()
{
    if (m_analyzing || m_videoRunning) {
        Toast::show(this, QStringLiteral("أوقف التحليل الجاري أولاً"), Toast::Kind::Warning);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("استعادة نسخة احتياطية"), m_lastDirectory,
                                                      QStringLiteral("نسخ ملامح الاحتياطية (*.malamih *.db);;كل الملفات (*)"));
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    const QPair<qint64, qint64> counts = FaceDatabase::inspectBackup(path);
    if (counts.first < 0) {
        QMessageBox::warning(this, QStringLiteral("استعادة"), QStringLiteral("هذا الملف ليس نسخة احتياطية صالحة من ملامح."));
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("استعادة نسخة احتياطية"),
        QStringLiteral("ستُستبدل القاعدة الحالية (%1 شخص، %2 صورة) بالنسخة الاحتياطية (%3 شخص، %4 صورة).\n\n"
                       "تُحفظ القاعدة الحالية بجانبها باسم faces.db.before-restore تحسباً. هل تريد المتابعة؟")
            .arg(m_database.personCount())
            .arg(m_database.photoCount())
            .arg(counts.first)
            .arg(counts.second));
    if (answer != QMessageBox::Yes)
        return;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_database.restore(path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("استعادة"), QStringLiteral("تعذرت الاستعادة، وبقيت القاعدة كما كانت:\n%1").arg(error));
        return;
    }
    refreshMatches();
    Toast::show(this, QStringLiteral("استُعيدت القاعدة: %1 شخص").arg(m_database.personCount()));
}

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
    if (fileVideo && !m_videoRunning) {
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
                   [this, start, end] {
                       const qint64 last = end > 0 ? end : m_videoDurationMs;
                       exportClip({{start, last}}, QStringLiteral("%1_%2-%3").arg(QFileInfo(m_videoSource).completeBaseName(),
                                                                               ImageIO::formatTime(start).remove(QLatin1Char(':')),
                                                                               ImageIO::formatTime(last).remove(QLatin1Char(':'))));
                   });
    menu.addAction(icon(Icons::Name::Image), QStringLiteral("حفظ الإطار هنا كصورة…"), this, [this, positionMs] { saveVideoFrame(positionMs); });
    menu.addAction(icon(Icons::Name::Sliders), QStringLiteral("معالجات الفيديو…"), this, &MainWindow::openVideoFilters);
    menu.addAction(icon(Icons::Name::Sparkle), QStringLiteral("تحسين هذا الإطار وتوضيحه…"), this, [this, positionMs] { enhanceVideoFrame(positionMs); })
        ->setEnabled(m_ready);

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

void MainWindow::showRecordings()
{
    RecordingsDialog dialog(&m_database, m_watched, this);
    connect(&dialog, &RecordingsDialog::openFileRequested, this, [this](const QString &path, qint64 positionMs) {
        if (!m_ready || m_videoRunning || m_analyzing) {
            Toast::show(this, QStringLiteral("انتظر حتى ينتهي التحليل الحالي (أو أوقف الكاميرا)"), Toast::Kind::Warning);
            return;
        }
        m_navGroup->button(0)->click();
        m_lastDirectory = QFileInfo(path).absolutePath();
        openSighting(path, positionMs);
    });
    dialog.exec();
}

void MainWindow::showDailyReport()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("تقرير يومي"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    auto *hint = new QLabel(QStringLiteral("تقرير يوم من الكاميرات: من ظهر ومتى وأمام أي كاميرا، وتنبيهات المراقبة، والزوار المجهولون، "
                                           "وساعات التسجيل، وأحداث اليوم (الانقطاع والأعطال). يُحفظ PDF للطباعة والإرسال أو HTML يُفتح في أي متصفح."),
                            &dialog);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("muted"));
    layout->addWidget(hint);
    auto *row = new QHBoxLayout;
    auto *date = new QDateEdit(QDate::currentDate(), &dialog);
    date->setCalendarPopup(true);
    date->setDisplayFormat(QStringLiteral("yyyy/MM/dd"));
    date->setMaximumDate(QDate::currentDate());
    row->addWidget(new QLabel(QStringLiteral("اليوم:"), &dialog));
    row->addWidget(date);
    row->addStretch(1);
    layout->addLayout(row);
    auto *buttons = new QHBoxLayout;
    auto *pdf = new FancyButton(Icons::Name::Export, QStringLiteral("حفظ PDF…"), FancyButton::Kind::Primary, &dialog);
    auto *html = new FancyButton(Icons::Name::Export, QStringLiteral("حفظ HTML…"), FancyButton::Kind::Secondary, &dialog);
    auto *close = new FancyButton(Icons::Name::Stop, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, &dialog);
    buttons->addWidget(pdf);
    buttons->addWidget(html);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    const auto save = [this, &dialog, date](const QString &extension) {
        const QDate day = date->date();
        const QString initial = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                    .filePath(QStringLiteral("تقرير-الكاميرات-%1.%2").arg(day.toString(Qt::ISODate), extension));
        const QString path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("حفظ التقرير"), initial,
                                                          extension == QLatin1String("pdf") ? QStringLiteral("PDF (*.pdf)")
                                                                                            : QStringLiteral("HTML (*.html)"));
        if (path.isEmpty())
            return;
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = DailyReport::write(path, m_database, m_watched, day, &error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            Toast::show(&dialog, QStringLiteral("تعذر حفظ التقرير: %1").arg(error), Toast::Kind::Warning);
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        dialog.accept();
    };
    connect(pdf, &QAbstractButton::clicked, &dialog, [save] { save(QStringLiteral("pdf")); });
    connect(html, &QAbstractButton::clicked, &dialog, [save] { save(QStringLiteral("html")); });
    connect(close, &QAbstractButton::clicked, &dialog, &QDialog::reject);
    dialog.exec();
}

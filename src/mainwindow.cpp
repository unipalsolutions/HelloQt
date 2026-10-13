#include "mainwindow.h"

#include "carddelegates.h"
#include "icons.h"
#include "imageio.h"
#include "imageview.h"
#include "persondialog.h"
#include "shotviewer.h"
#include "timelineview.h"
#include "personmodel.h"
#include "theme.h"
#include "videoworker.h"
#include "widgets.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QMenu>
#include <QScrollArea>
#include <QTimeEdit>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
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
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

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

    connect(&m_database, &FaceDatabase::changed, this, [this] {
        refreshStats();
        refreshMatches();
    });
    connect(&m_database, &FaceDatabase::personsReset, this, &MainWindow::refreshPersons);
    connect(&m_database, &FaceDatabase::personUpdated, this, [this](const QString &id) {
        m_personModel->refreshPerson(id);
        if (id == selectedPersonId())
            showPersonDetails();
    });
    refreshPersons();
    refreshStats();

    connect(&m_imageWatcher, &QFutureWatcher<ImageAnalysis>::finished, this, &MainWindow::onImageAnalyzed);

    m_videoThread = new QThread(this);
    m_videoWorker = new VideoWorker(&m_engine);
    m_videoWorker->moveToThread(m_videoThread);
    m_videoWorker->setGallery(m_database.gallery());
    m_videoWorker->setThreshold(threshold());
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
    });
    connect(&m_frameWatcher, &QFutureWatcher<ShotFrame>::finished, this, &MainWindow::onVideoFrameLoaded);
    connect(m_videoWorker, &VideoWorker::frameReady, this, &MainWindow::onFrameReady);
    connect(m_videoWorker, &VideoWorker::trackUpdated, this, &MainWindow::onTrackUpdated);
    connect(m_videoWorker, &VideoWorker::finished, this, &MainWindow::onVideoFinished);
    connect(m_videoWorker, &VideoWorker::identitiesMerged, this, &MainWindow::onIdentitiesMerged);
    m_videoThread->start();

    loadSettings();
    setBusy(false);
    updateResponsiveLayout();
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
    setBusy(false);
    statusBar()->showMessage(QStringLiteral("جاهز — كل المعالجة تتم محلياً على هذا الجهاز · اسحب صورة أو فيديو إلى النافذة أو الصق صورة بـ Ctrl+V"));
    if (!m_pendingFile.isEmpty())
        openFile(std::exchange(m_pendingFile, QString()));
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
            openCamera();
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
    add(QKeySequence(Qt::Key_F5), [this] {
        if (!m_currentImagePath.isEmpty() && m_openImageButton->isEnabled())
            analyzeImage(m_currentImagePath);
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_E), [this] { m_enhanceSwitch->toggle(); });
    add(QKeySequence::Find, [this] {
        m_navGroup->button(1)->click();
        m_personFilter->setFocus();
        m_personFilter->selectAll();
    });
    add(QKeySequence(Qt::CTRL | Qt::Key_1), [this] { m_navGroup->button(0)->click(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_2), [this] { m_navGroup->button(1)->click(); });
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
    saveSettings();
    QMainWindow::closeEvent(event);
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
    m_navGroup->button(0)->click();
    openFile(files.first());
}

void MainWindow::pasteImage()
{
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        const QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile())
            openFile(urls.first().toLocalFile());
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
    m_navGroup->button(0)->click();
    analyzeImage(path);
}

MainWindow::~MainWindow()
{
    m_loadWatcher.waitForFinished();
    m_videoWorker->requestStop();
    m_videoThread->quit();
    m_videoThread->wait();
    m_imageWatcher.waitForFinished();
    m_frameWatcher.waitForFinished();
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
    m_navButtons = {recognitionNav, databaseNav};
    auto *group = new QButtonGroup(sidebar);
    m_navGroup = group;
    group->addButton(recognitionNav, 0);
    group->addButton(databaseNav, 1);
    recognitionNav->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) { m_pages->slideTo(id); });
    layout->addWidget(recognitionNav);
    layout->addWidget(databaseNav);
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
    m_cameraButton->setToolTip(QStringLiteral("تشغيل الكاميرا (Ctrl+Shift+C)"));
    m_pauseButton->setToolTip(QStringLiteral("إيقاف مؤقت / استئناف (مسافة)"));
    m_stopButton->setToolTip(QStringLiteral("إيقاف التحليل (Esc)"));
    m_saveFacesButton = new FancyButton(Icons::Name::Save, QStringLiteral("حفظ الوجوه"), FancyButton::Kind::Secondary, page);
    m_saveFacesButton->setToolTip(QStringLiteral("حفظ صور الوجوه المكتشفة مع تقرير (Ctrl+S)"));
    connect(m_openImageButton, &QAbstractButton::clicked, this, &MainWindow::openImage);
    connect(m_openVideoButton, &QAbstractButton::clicked, this, &MainWindow::openVideo);
    connect(m_cameraButton, &QAbstractButton::clicked, this, &MainWindow::openCamera);
    connect(m_pauseButton, &QAbstractButton::clicked, this, &MainWindow::togglePause);
    connect(m_stopButton, &QAbstractButton::clicked, this, &MainWindow::stopVideo);
    connect(m_saveFacesButton, &QAbstractButton::clicked, this, &MainWindow::saveFaces);
    m_headerButtons = {m_openImageButton, m_openVideoButton, m_cameraButton, m_pauseButton, m_stopButton, m_saveFacesButton};

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

    // جزء الفيديو المطلوب
    auto timeEdit = [settings](const QString &tip) {
        auto *edit = new QTimeEdit(settings);
        edit->setDisplayFormat(QStringLiteral("HH:mm:ss"));
        edit->setTime(QTime(0, 0));
        edit->setToolTip(tip);
        edit->setLayoutDirection(Qt::LeftToRight);
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

    settingsLayout->addWidget(group({new QLabel(QStringLiteral("حد التعرف"), settings), m_thresholdSlider, m_thresholdValue, m_thresholdWarning}));
    settingsLayout->addWidget(m_thoroughSwitch);
    settingsLayout->addWidget(m_enhanceSwitch);
    settingsLayout->addWidget(group({new QLabel(QStringLiteral("إطار من كل"), settings), m_frameStepSpin}));
    settingsLayout->addWidget(group({new QLabel(QStringLiteral("الجزء: من"), settings), m_rangeStart,
                                     new QLabel(QStringLiteral("إلى"), settings), m_rangeEnd}));
    settingsLayout->addWidget(group({m_regionButton, m_clearRegionButton, m_reanalyzeButton}));
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
    });
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
    m_faceList->setToolTip(QStringLiteral("انقر نقراً مزدوجاً لعرض لقطات الوجه في الفيديو · Ctrl أو Shift لتحديد عدة وجوه ثم زر الفأرة الأيمن للدمج أو الحذف"));
    connect(m_faceList, &QListWidget::currentRowChanged, this, &MainWindow::showFaceDetails);
    connect(m_faceList, &QListWidget::customContextMenuRequested, this, &MainWindow::showFaceContextMenu);
    connect(m_faceList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (m_videoMode)
            openShotViewer(item->data(CardRole::Id).toInt());
    });
    sideLayout->addWidget(m_faceList, 1);

    // التفاصيل تُمرَّر إن طالت حتى تبقى الأزرار ظاهرة على الشاشات القصيرة
    m_faceDetails = new QLabel(side);
    m_faceDetails->setWordWrap(true);
    m_faceDetails->setTextFormat(Qt::RichText);
    m_faceDetails->setAlignment(Qt::AlignTop);
    m_faceDetails->setObjectName(QStringLiteral("muted"));
    auto *detailsScroll = new QScrollArea(side);
    detailsScroll->setWidget(m_faceDetails);
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setFrameShape(QFrame::NoFrame);
    detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailsScroll->setMinimumHeight(56);
    detailsScroll->setMaximumHeight(190);
    detailsScroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    sideLayout->addWidget(detailsScroll);

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
    auto *timelineHint = label(QStringLiteral("انقر لعرض أي لحظة · نقر مزدوج على مقطع لعرض لقطاته · Ctrl+العجلة للتكبير"),
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
    header->addWidget(openFolderButton);
    m_headerButtons << addPersonButton << openFolderButton;
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
    detailsLayout->addLayout(profile);

    m_personActions = new QWidget(detailsCard);
    auto *actions = new FlowLayout(m_personActions, 10, 8);
    actions->setContentsMargins(0, 0, 0, 0);
    auto *editButton = new FancyButton(Icons::Name::Edit, QStringLiteral("تعديل البيانات"), FancyButton::Kind::Secondary, m_personActions);
    auto *addPhotosButton = new FancyButton(Icons::Name::PhotoAdd, QStringLiteral("إضافة صور"), FancyButton::Kind::Primary, m_personActions);
    auto *removeButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف الشخص"), FancyButton::Kind::Danger, m_personActions);
    connect(editButton, &QAbstractButton::clicked, this, &MainWindow::editPerson);
    connect(addPhotosButton, &QAbstractButton::clicked, this, &MainWindow::addPhotos);
    connect(removeButton, &QAbstractButton::clicked, this, &MainWindow::removePerson);
    actions->addWidget(addPhotosButton);
    actions->addWidget(editButton);
    actions->addWidget(removeButton);
    detailsLayout->addWidget(m_personActions);

    auto *photosHeader = new QHBoxLayout;
    auto *photosTitle = label(QStringLiteral("صور الوجه المسجلة"), QStringLiteral("sectionTitle"), detailsCard);
    photosHeader->addWidget(photosTitle);
    m_photosHint = label(QStringLiteral("كلما زادت الصور وتنوعت زادت دقة التعرف"), QStringLiteral("muted"), detailsCard);
    photosHeader->addWidget(m_photosHint, 1);
    photosHeader->addStretch();
    m_removePhotoButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف الصورة"), FancyButton::Kind::Danger, detailsCard);
    connect(m_removePhotoButton, &QAbstractButton::clicked, this, &MainWindow::removePhoto);
    photosHeader->addWidget(m_removePhotoButton);
    detailsLayout->addLayout(photosHeader);

    m_photoList = new QListWidget(detailsCard);
    m_photoList->setObjectName(QStringLiteral("photoGrid"));
    m_photoList->setViewMode(QListView::IconMode);
    m_photoList->setIconSize(QSize(132, 132));
    m_photoList->setGridSize(QSize(156, 172));
    m_photoList->setResizeMode(QListView::Adjust);
    m_photoList->setMovement(QListView::Static);
    m_photoList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_photoList, &QListWidget::currentRowChanged, this, [this] { m_removePhotoButton->setEnabled(m_photoList->currentItem()); });
    detailsLayout->addWidget(m_photoList, 1);

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
    m_stopButton->setEnabled(m_videoRunning);
    m_pauseButton->setEnabled(m_videoRunning);
    m_pauseButton->setVisible(m_videoRunning);
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
    m_currentImagePath = path;
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
            faces = engine->analyze(image, thorough);
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
    startVideo(QStringLiteral("camera:0"));
}

void MainWindow::startVideo(const QString &source)
{
    const bool camera = source.startsWith(QStringLiteral("camera:"));
    if (source != m_videoSource && !m_videoSource.isEmpty()) {
        // منطقة التحليل خاصة بالفيديو السابق
        m_view->setRegion(QRectF());
    }
    m_videoSource = source;
    m_videoMode = true;
    m_videoRunning = true;
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
    m_view->setBusy(true, source.startsWith(QStringLiteral("camera:")) ? QStringLiteral("جارٍ تشغيل الكاميرا")
                                                                       : QStringLiteral("جارٍ فتح الفيديو"));
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    statusBar()->showMessage(QStringLiteral("جارٍ التحليل…"));
    setBusy(true);

    m_videoWorker->setFrameStep(m_frameStepSpin->value());
    m_videoClock.invalidate();
    m_videoStep = 1;
    m_videoPaused = false;
    m_pausedMs = 0;
    m_pauseButton->setIconName(Icons::Name::Pause);
    m_pauseButton->setText(QStringLiteral("إيقاف مؤقت"));
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

void MainWindow::onFrameReady(const QImage &frame, const QList<FaceItem> &faces, qint64 positionMs, double progress)
{
    m_videoWorker->frameShown();
    if (!m_videoRunning)
        return;
    m_view->setBusy(false);
    m_view->setImage(frame);
    m_view->setFaces(faces);
    m_videoPositionMs = positionMs;
    if (m_timelineCard->isVisible()) {
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
    m_timeline->setAnalyzedUntil(-1);
    rebuildTimelineLanes();
    m_view->setBusy(false);
    m_progress->setVisible(false);
    if (!error.isEmpty()) {
        statusBar()->showMessage(error);
        Toast::show(this, error, Toast::Kind::Warning);
    } else {
        if (m_faceSort->currentIndex() != 0)
            sortFaces();
        const QString summary = QStringLiteral("انتهى التحليل — أشخاص مختلفون: %1 · انقر نقراً مزدوجاً على وجه لعرض لقطاته")
                                    .arg(m_faces.size());
        statusBar()->showMessage(summary);
        Toast::show(this, summary, Toast::Kind::Info);
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
        menu.addAction(Icons::icon(Icons::Name::UserPlus, Theme::Text, 18), QStringLiteral("إضافة إلى قاعدة البيانات…"), this, [this, id] {
            const int row = m_faceRows.value(id, -1);
            if (row >= 0)
                addFaceToDatabase(m_faces.at(row));
        })->setEnabled(m_ready);
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
    connect(viewer, &ShotViewer::analyzeFromRequested, this, &MainWindow::analyzeVideoFrom);
    viewer->setThumbnails(m_timeline->thumbnails());
    if (positionMs >= 0)
        viewer->showAt(positionMs);
    viewer->show();
}

void MainWindow::rebuildTimelineLanes()
{
    if (!m_videoMode || !m_timelineCard->isVisible())
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
    if (!m_videoMode)
        m_view->setFaces(m_faces);
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
    auto row_ = [](const QString &key, const QString &value) {
        return QStringLiteral("<br><span style='color:#8aa3c7'>%1:</span> <span style='color:#e6f0ff'>%2</span>").arg(key, value.toHtmlEscaped());
    };
    html += row_(QStringLiteral("درجة التشابه"), QStringLiteral("%1%").arg(qRound(face.match.score * 100)));
    html += row_(QStringLiteral("جودة الوجه"), QStringLiteral("%1%").arg(qRound(face.quality * 100)));
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
        if (!person.notes.isEmpty())
            html += row_(QStringLiteral("ملاحظات"), person.notes);
    }
    if (face.quality < 0.35f)
        html += QStringLiteral("<br><span style='color:#93c5fd'>جودة الوجه منخفضة: النتيجة أقل موثوقية</span>");
    m_faceDetails->setText(html);
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
        csv << QStringLiteral("الرقم,الاسم,الحالة,التشابه %,جودة الوجه %,عدد الإطارات,أول ظهور,آخر ظهور,الصورة\n");
    }
    auto field = [](QString text) {
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
                << (face.lastSeenMs >= 0 ? ImageIO::formatTime(face.lastSeenMs) : QString()) << ',' << field(fileName) << '\n';
        }
    }
    Toast::show(this, QStringLiteral("تم حفظ %1 وجه مع تقرير CSV").arg(saved));
}

void MainWindow::addSelectedFaceToDatabase()
{
    const int row = m_faceList->currentRow();
    if (row < 0 || row >= m_faces.size())
        return;
    addFaceToDatabase(m_faces.at(row));
}

void MainWindow::addFaceToDatabase(const FaceItem &face)
{
    PersonPickerDialog picker(&m_database, face.match.personId, this);
    if (picker.exec() != QDialog::Accepted)
        return;

    QString personId = picker.personId();
    if (personId.isEmpty()) {
        PersonDialog dialog(Person(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        personId = m_database.addPerson(dialog.person());
        if (personId.isEmpty()) {
            Toast::show(this, QStringLiteral("تعذر حفظ الشخص"), Toast::Kind::Warning);
            return;
        }
    }

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
    m_removePhotoButton->setEnabled(false);
    m_personActions->setVisible(!id.isEmpty());
    m_removePhotoButton->setVisible(!id.isEmpty());

    if (id.isEmpty()) {
        m_personAvatar->setPixmap(QPixmap());
        m_personDetails->setText(QStringLiteral("<div style='font-size:15pt; font-weight:600'>اختر شخصاً</div>"
                                                "<div style='color:#8aa3c7'>اختر شخصاً من القائمة لعرض بياناته وصوره، "
                                                "أو اضغط \"شخص جديد\" لإضافة شخص.</div>"));
        return;
    }

    const Person person = m_database.person(id);
    m_personAvatar->setPixmap(person.coverPhoto <= 0
                                  ? Icons::icon(Icons::Name::Users, Theme::Muted, 96).pixmap(96, 96)
                                  : circularPixmap(m_database.photo(person.coverPhoto, 192), 96));

    auto chip = [](const QString &key, const QString &value) {
        return QStringLiteral("<span style='color:#8aa3c7'>%1</span> <span style='color:#e6f0ff'>%2</span>&nbsp;&nbsp;&nbsp;")
            .arg(key, value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<div style='font-size:18pt; font-weight:700'>%1</div><div>").arg(person.name.toHtmlEscaped());
    if (!person.nationalId.isEmpty())
        html += chip(QStringLiteral("رقم الهوية:"), person.nationalId);
    if (!person.phone.isEmpty())
        html += chip(QStringLiteral("الهاتف:"), person.phone);
    html += chip(QStringLiteral("أُضيف:"), person.created.toString(QStringLiteral("yyyy/MM/dd  hh:mm")));
    html += QStringLiteral("</div>");
    if (!person.notes.isEmpty())
        html += QStringLiteral("<div style='color:#8aa3c7; margin-top:6px'>%1</div>")
                    .arg(person.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    m_personDetails->setText(html);

    for (const FacePhoto &photo : person.photos) {
        auto *item = new QListWidgetItem(m_photoList);
        item->setIcon(QIcon(QPixmap::fromImage(m_database.photo(photo.id, m_photoList->iconSize().width()))));
        item->setText(QStringLiteral("جودة %1%").arg(qRound(photo.quality * 100)));
        item->setData(Qt::UserRole, photo.id);
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
            const std::vector<AnalyzedFace> faces = image.empty() ? std::vector<AnalyzedFace>() : engine->analyze(image, true);
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

#include "mainwindow.h"

#include "carddelegates.h"
#include "icons.h"
#include "imageio.h"
#include "imageview.h"
#include "persondialog.h"
#include "theme.h"
#include "videoworker.h"
#include "widgets.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

const QString ImageFilter = QStringLiteral("الصور (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff);;كل الملفات (*)");
const QString VideoFilter = QStringLiteral("الفيديو (*.mp4 *.avi *.mkv *.mov *.wmv *.webm *.m4v *.3gp);;كل الملفات (*)");

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
        QLinearGradient gradient(0, 0, width(), height());
        gradient.setColorAt(0, Theme::Accent);
        gradient.setColorAt(1, Theme::Accent2);
        p.setPen(Qt::NoPen);
        p.setBrush(gradient);
        p.drawRoundedRect(rect(), 12, 12);
        Icons::paint(p, Icons::Name::Scan, QRectF(9, 9, 24, 24), Qt::white);
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
    setWindowTitle(QStringLiteral("التعرف على الوجوه"));

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

    QString error;
    if (!m_engine.load(FaceEngine::defaultModelsDir(), &error)) {
        statusBar()->showMessage(error);
        QMessageBox::critical(this, windowTitle(), error);
    } else if (!m_database.open(FaceDatabase::defaultDirectory(), &error)) {
        statusBar()->showMessage(error);
        QMessageBox::critical(this, windowTitle(), error);
    } else {
        m_ready = true;
        statusBar()->showMessage(QStringLiteral("جاهز — كل المعالجة تتم محلياً على هذا الجهاز"));
    }

    connect(&m_database, &FaceDatabase::changed, this, [this] {
        refreshPersons();
        refreshMatches();
        if (m_videoWorker)
            m_videoWorker->setGallery(m_database.gallery());
    });
    refreshPersons();

    connect(&m_imageWatcher, &QFutureWatcher<ImageAnalysis>::finished, this, &MainWindow::onImageAnalyzed);

    m_videoThread = new QThread(this);
    m_videoWorker = new VideoWorker(&m_engine);
    m_videoWorker->moveToThread(m_videoThread);
    m_videoWorker->setGallery(m_database.gallery());
    m_videoWorker->setThreshold(threshold());
    connect(m_videoThread, &QThread::finished, m_videoWorker, &QObject::deleteLater);
    connect(m_videoWorker, &VideoWorker::started, this, [this](int step, double) {
        m_videoClock.start();
        m_videoStep = step;
    });
    connect(m_videoWorker, &VideoWorker::frameReady, this, &MainWindow::onFrameReady);
    connect(m_videoWorker, &VideoWorker::trackUpdated, this, &MainWindow::onTrackUpdated);
    connect(m_videoWorker, &VideoWorker::finished, this, &MainWindow::onVideoFinished);
    m_videoThread->start();

    setBusy(false);
}

MainWindow::~MainWindow()
{
    m_videoWorker->requestStop();
    m_videoThread->quit();
    m_videoThread->wait();
    m_imageWatcher.waitForFinished();
}

QWidget *MainWindow::createSidebar()
{
    auto *sidebar = card(this, QStringLiteral("sidebar"));
    sidebar->setFixedWidth(240);
    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(18, 22, 18, 18);
    layout->setSpacing(6);

    auto *brand = new QHBoxLayout;
    brand->setSpacing(12);
    brand->addWidget(new LogoMark(sidebar));
    auto *brandText = new QVBoxLayout;
    brandText->setSpacing(0);
    auto *name = label(QStringLiteral("HelloQt"), QStringLiteral("sectionTitle"), sidebar);
    brandText->addWidget(name);
    brandText->addWidget(label(QStringLiteral("التعرف على الوجوه"), QStringLiteral("muted"), sidebar));
    brand->addLayout(brandText, 1);
    layout->addLayout(brand);
    layout->addSpacing(26);

    auto *recognitionNav = new FancyButton(Icons::Name::Scan, QStringLiteral("التعرف"), FancyButton::Kind::Nav, sidebar);
    auto *databaseNav = new FancyButton(Icons::Name::Users, QStringLiteral("قاعدة البيانات"), FancyButton::Kind::Nav, sidebar);
    auto *group = new QButtonGroup(sidebar);
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

    auto *offline = new QHBoxLayout;
    auto *lock = new QLabel(sidebar);
    lock->setPixmap(Icons::icon(Icons::Name::Lock, Theme::Success, 16).pixmap(16, 16));
    offline->addWidget(lock);
    auto *offlineText = label(QStringLiteral("يعمل محلياً بالكامل، دون إنترنت"), QStringLiteral("muted"), sidebar);
    offlineText->setWordWrap(true);
    offline->addWidget(offlineText, 1);
    layout->addLayout(offline);
    return sidebar;
}

QWidget *MainWindow::createRecognitionPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(26, 22, 26, 18);
    layout->setSpacing(14);

    // العنوان والأزرار
    m_openImageButton = new FancyButton(Icons::Name::Image, QStringLiteral("فتح صورة"), FancyButton::Kind::Primary, page);
    m_openVideoButton = new FancyButton(Icons::Name::Video, QStringLiteral("فيديو"), FancyButton::Kind::Secondary, page);
    m_cameraButton = new FancyButton(Icons::Name::Camera, QStringLiteral("الكاميرا"), FancyButton::Kind::Secondary, page);
    m_stopButton = new FancyButton(Icons::Name::Stop, QStringLiteral("إيقاف"), FancyButton::Kind::Danger, page);
    m_saveFacesButton = new FancyButton(Icons::Name::Save, QStringLiteral("حفظ الوجوه"), FancyButton::Kind::Secondary, page);
    connect(m_openImageButton, &QAbstractButton::clicked, this, &MainWindow::openImage);
    connect(m_openVideoButton, &QAbstractButton::clicked, this, &MainWindow::openVideo);
    connect(m_cameraButton, &QAbstractButton::clicked, this, &MainWindow::openCamera);
    connect(m_stopButton, &QAbstractButton::clicked, this, &MainWindow::stopVideo);
    connect(m_saveFacesButton, &QAbstractButton::clicked, this, &MainWindow::saveFaces);

    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(label(QStringLiteral("التعرف على الوجوه"), QStringLiteral("title"), page));
    titles->addWidget(label(QStringLiteral("افتح صورة أو فيديو، أو شغّل الكاميرا، ليتم كشف الوجوه ومطابقتها مع قاعدة البيانات"),
                            QStringLiteral("subtitle"), page));
    header->addLayout(titles, 1);
    header->setSpacing(10);
    header->addWidget(m_openImageButton);
    header->addWidget(m_openVideoButton);
    header->addWidget(m_cameraButton);
    header->addWidget(m_stopButton);
    header->addWidget(m_saveFacesButton);
    layout->addLayout(header);

    // الإعدادات
    auto *settings = card(page);
    auto *settingsLayout = new QHBoxLayout(settings);
    settingsLayout->setContentsMargins(18, 10, 18, 10);
    settingsLayout->setSpacing(14);

    m_thresholdSlider = new QSlider(Qt::Horizontal, settings);
    m_thresholdSlider->setRange(15, 80);
    m_thresholdSlider->setValue(30);
    m_thresholdSlider->setFixedWidth(180);
    m_thresholdSlider->setCursor(Qt::PointingHandCursor);
    m_thresholdSlider->setToolTip(QStringLiteral("أعلى = أخطاء أقل لكن قد لا يُتعرف على الصور الرديئة جداً.\nأقل = يتعرف أكثر مع احتمال خطأ أكبر."));
    m_thresholdValue = label(QStringLiteral("30%"), QStringLiteral("badge"), settings);
    connect(m_thresholdSlider, &QSlider::valueChanged, this, [this](int value) {
        m_thresholdValue->setText(QStringLiteral("%1%").arg(value));
        m_videoWorker->setThreshold(threshold());
        refreshMatches();
    });

    m_thoroughSwitch = new ToggleSwitch(QStringLiteral("تحليل معمّق للصور (أدق وأبطأ)"), settings);
    m_thoroughSwitch->setChecked(true);

    m_frameStepSpin = new QSpinBox(settings);
    m_frameStepSpin->setRange(0, 60);
    m_frameStepSpin->setValue(0);
    m_frameStepSpin->setSpecialValueText(QStringLiteral("تلقائي"));
    m_frameStepSpin->setToolTip(QStringLiteral("تلقائي = نحو 10 إطارات لكل ثانية من الفيديو (سريع ودقيق).\n"
                                               "1 = تحليل كل إطار (الأبطأ). قيمة أكبر = أسرع."));
    connect(m_frameStepSpin, &QSpinBox::valueChanged, this, [this](int value) { m_videoWorker->setFrameStep(value); });

    settingsLayout->addWidget(new QLabel(QStringLiteral("حد التعرف"), settings));
    settingsLayout->addWidget(m_thresholdSlider);
    settingsLayout->addWidget(m_thresholdValue);
    settingsLayout->addWidget(separator(settings));
    settingsLayout->addWidget(m_thoroughSwitch);
    settingsLayout->addWidget(separator(settings));
    settingsLayout->addWidget(new QLabel(QStringLiteral("الفيديو: تحليل إطار من كل"), settings));
    settingsLayout->addWidget(m_frameStepSpin);
    settingsLayout->addStretch();
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
    viewLayout->addWidget(m_view);
    connect(m_view, &ImageView::faceClicked, this, &MainWindow::selectFace);

    auto *side = card(page);
    side->setMinimumWidth(300);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(14, 16, 14, 14);
    sideLayout->setSpacing(10);
    auto *sideHeader = new QHBoxLayout;
    sideHeader->addWidget(label(QStringLiteral("الوجوه المكتشفة"), QStringLiteral("sectionTitle"), side));
    sideHeader->addStretch();
    m_faceCount = label(QStringLiteral("0"), QStringLiteral("badge"), side);
    sideHeader->addWidget(m_faceCount);
    sideLayout->addLayout(sideHeader);

    m_faceList = new QListWidget(side);
    m_faceList->setItemDelegate(new FaceCardDelegate(m_faceList));
    m_faceList->setUniformItemSizes(true);
    m_faceList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_faceList->setMouseTracking(true);
    connect(m_faceList, &QListWidget::currentRowChanged, this, &MainWindow::showFaceDetails);
    sideLayout->addWidget(m_faceList, 1);

    m_faceDetails = new QLabel(side);
    m_faceDetails->setWordWrap(true);
    m_faceDetails->setTextFormat(Qt::RichText);
    m_faceDetails->setMinimumHeight(96);
    m_faceDetails->setAlignment(Qt::AlignTop);
    m_faceDetails->setObjectName(QStringLiteral("muted"));
    sideLayout->addWidget(m_faceDetails);

    m_enrollFaceButton = new FancyButton(Icons::Name::UserPlus, QStringLiteral("إضافة الوجه إلى قاعدة البيانات"),
                                         FancyButton::Kind::Primary, side);
    m_enrollFaceButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_enrollFaceButton, &QAbstractButton::clicked, this, &MainWindow::addSelectedFaceToDatabase);
    sideLayout->addWidget(m_enrollFaceButton);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    splitter->setHandleWidth(14);
    splitter->addWidget(viewCard);
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 330});
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
    header->addLayout(titles, 1);
    header->addWidget(addPersonButton);
    header->addWidget(openFolderButton);
    layout->addLayout(header);

    // قائمة الأشخاص
    auto *listCard = card(page);
    listCard->setMinimumWidth(290);
    auto *listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(14, 14, 14, 14);
    listLayout->setSpacing(10);
    m_personFilter = new QLineEdit(listCard);
    m_personFilter->setPlaceholderText(QStringLiteral("بحث بالاسم أو رقم الهوية…"));
    m_personFilter->setClearButtonEnabled(true);
    m_personFilter->addAction(Icons::icon(Icons::Name::Search, Theme::Muted, 18), QLineEdit::LeadingPosition);
    connect(m_personFilter, &QLineEdit::textChanged, this, &MainWindow::applyPersonFilter);
    m_personList = new QListWidget(listCard);
    m_personList->setItemDelegate(new PersonCardDelegate(m_personList));
    m_personList->setUniformItemSizes(true);
    m_personList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_personList->setMouseTracking(true);
    connect(m_personList, &QListWidget::currentRowChanged, this, &MainWindow::showPersonDetails);
    connect(m_personList, &QListWidget::itemDoubleClicked, this, &MainWindow::editPerson);
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
    auto *actions = new QHBoxLayout(m_personActions);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(10);
    auto *editButton = new FancyButton(Icons::Name::Edit, QStringLiteral("تعديل البيانات"), FancyButton::Kind::Secondary, m_personActions);
    auto *addPhotosButton = new FancyButton(Icons::Name::PhotoAdd, QStringLiteral("إضافة صور"), FancyButton::Kind::Primary, m_personActions);
    auto *removeButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف الشخص"), FancyButton::Kind::Danger, m_personActions);
    connect(editButton, &QAbstractButton::clicked, this, &MainWindow::editPerson);
    connect(addPhotosButton, &QAbstractButton::clicked, this, &MainWindow::addPhotos);
    connect(removeButton, &QAbstractButton::clicked, this, &MainWindow::removePerson);
    actions->addWidget(addPhotosButton);
    actions->addWidget(editButton);
    actions->addStretch();
    actions->addWidget(removeButton);
    detailsLayout->addWidget(m_personActions);

    auto *photosHeader = new QHBoxLayout;
    auto *photosTitle = label(QStringLiteral("صور الوجه المسجلة"), QStringLiteral("sectionTitle"), detailsCard);
    photosHeader->addWidget(photosTitle);
    photosHeader->addWidget(label(QStringLiteral("كلما زادت الصور وتنوعت زادت دقة التعرف"), QStringLiteral("muted"), detailsCard), 1);
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
    m_saveFacesButton->setEnabled(!m_faces.isEmpty());
    m_enrollFaceButton->setEnabled(m_ready && m_faceList->currentRow() >= 0);
}

// ───────────── التعرف ─────────────

void MainWindow::openFile(const QString &path)
{
    static const QStringList videoSuffixes{QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("mkv"),
                                           QStringLiteral("mov"), QStringLiteral("wmv"), QStringLiteral("webm"),
                                           QStringLiteral("m4v"), QStringLiteral("3gp")};
    if (!m_ready)
        return;
    if (videoSuffixes.contains(QFileInfo(path).suffix().toLower()))
        startVideo(path);
    else
        analyzeImage(path);
}

void MainWindow::openImage()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("فتح صورة"), QString(), ImageFilter);
    if (!path.isEmpty())
        analyzeImage(path);
}

void MainWindow::analyzeImage(const QString &path)
{
    m_videoMode = false;
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
    m_view->setBusy(true, QStringLiteral("جارٍ كشف الوجوه والتعرف عليها"));
    statusBar()->showMessage(QStringLiteral("جارٍ تحليل الصورة…"));

    const bool thorough = m_thoroughSwitch->isChecked();
    const FaceEngine *engine = &m_engine;
    m_imageWatcher.setFuture(QtConcurrent::run([engine, path, thorough] {
        ImageAnalysis result;
        const cv::Mat image = ImageIO::loadImage(path);
        if (image.empty()) {
            result.error = QStringLiteral("تعذر قراءة الصورة: %1").arg(QDir::toNativeSeparators(path));
            return result;
        }
        result.image = ImageIO::toQImage(image);
        const std::vector<AnalyzedFace> faces = engine->analyze(image, thorough);
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
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("فتح فيديو"), QString(), VideoFilter);
    if (!path.isEmpty())
        startVideo(path);
}

void MainWindow::openCamera()
{
    startVideo(QStringLiteral("camera:0"));
}

void MainWindow::startVideo(const QString &source)
{
    m_videoMode = true;
    m_videoRunning = true;
    m_faces.clear();
    refreshFaceList();
    m_view->clear();
    m_view->setBusy(true, source.startsWith(QStringLiteral("camera:")) ? QStringLiteral("جارٍ تشغيل الكاميرا")
                                                                       : QStringLiteral("جارٍ فتح الفيديو"));
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    statusBar()->showMessage(QStringLiteral("جارٍ التحليل…"));
    setBusy(true);

    m_videoWorker->setFrameStep(m_frameStepSpin->value());
    m_videoClock.invalidate();
    m_videoStep = 1;
    QMetaObject::invokeMethod(m_videoWorker, "process", Qt::QueuedConnection, Q_ARG(QString, source));
}

void MainWindow::stopVideo()
{
    m_videoWorker->requestStop();
}

void MainWindow::onFrameReady(const QImage &frame, const QList<FaceItem> &faces, int frameIndex, int frameCount)
{
    m_videoWorker->frameShown();
    if (!m_videoRunning)
        return;
    m_view->setBusy(false);
    m_view->setImage(frame);
    m_view->setFaces(faces);
    if (frameCount > 0) {
        m_progress->setRange(0, frameCount);
        m_progress->setValue(std::min(frameIndex, frameCount));
    }
    QString status = frameCount > 0 ? QStringLiteral("الإطار %1 من %2").arg(frameIndex).arg(frameCount)
                                    : QStringLiteral("الإطار %1").arg(frameIndex);
    const double seconds = m_videoClock.isValid() ? m_videoClock.elapsed() / 1000.0 : 0.0;
    if (seconds > 1.0) {
        const double speed = frameIndex / seconds;
        status += QStringLiteral(" · السرعة %1 إطار/ث").arg(speed, 0, 'f', 1);
        if (frameCount > frameIndex && speed > 0.0) {
            const int remaining = int((frameCount - frameIndex) / speed);
            status += QStringLiteral(" · المتبقي %1").arg(remaining >= 3600
                ? QStringLiteral("%1:%2:%3").arg(remaining / 3600).arg(remaining / 60 % 60, 2, 10, QLatin1Char('0')).arg(remaining % 60, 2, 10, QLatin1Char('0'))
                : QStringLiteral("%1:%2").arg(remaining / 60).arg(remaining % 60, 2, 10, QLatin1Char('0')));
        }
    }
    if (m_videoStep > 1)
        status += QStringLiteral(" · تحليل إطار من كل %1").arg(m_videoStep);
    status += QStringLiteral(" · وجوه في الإطار: %1 · أشخاص مختلفون: %2").arg(faces.size()).arg(m_faces.size());
    statusBar()->showMessage(status);
}

void MainWindow::onTrackUpdated(const FaceItem &track)
{
    for (int i = 0; i < m_faces.size(); ++i) {
        if (m_faces.at(i).id == track.id) {
            m_faces[i] = track;
            updateFaceListItem(m_faceList->item(i), track);
            if (m_faceList->currentRow() == i)
                showFaceDetails();
            return;
        }
    }
    m_faces.append(track);
    updateFaceListItem(new QListWidgetItem(m_faceList), track);
    updateFaceCount();
    m_saveFacesButton->setEnabled(true);
}

void MainWindow::onVideoFinished(const QString &error)
{
    m_videoRunning = false;
    m_view->setBusy(false);
    m_progress->setVisible(false);
    if (!error.isEmpty()) {
        statusBar()->showMessage(error);
        Toast::show(this, error, Toast::Kind::Warning);
    } else {
        const QString summary = QStringLiteral("انتهى التحليل — أشخاص مختلفون: %1").arg(m_faces.size());
        statusBar()->showMessage(summary);
        Toast::show(this, summary, Toast::Kind::Info);
    }
    setBusy(false);
}

void MainWindow::refreshMatches()
{
    const GalleryPtr gallery = m_database.gallery();
    for (FaceItem &face : m_faces)
        face.match = FaceDatabase::identify(*gallery, face.embedding, threshold());
    refreshFaceList();
    if (!m_videoMode)
        m_view->setFaces(m_faces);
}

void MainWindow::refreshFaceList()
{
    const int row = m_faceList->currentRow();
    m_faceList->clear();
    for (const FaceItem &face : m_faces)
        updateFaceListItem(new QListWidgetItem(m_faceList), face);
    if (row >= 0 && row < m_faceList->count())
        m_faceList->setCurrentRow(row);
    updateFaceCount();
    showFaceDetails();
    setBusy(m_analyzing);
}

void MainWindow::updateFaceCount()
{
    m_faceCount->setText(QString::number(m_faces.size()));
}

void MainWindow::updateFaceListItem(QListWidgetItem *listItem, const FaceItem &face)
{
    if (!listItem)
        return;
    QString subtitle = QStringLiteral("جودة الصورة %1%").arg(qRound(face.quality * 100));
    if (m_videoMode)
        subtitle += QStringLiteral(" · ظهر في %1 إطار").arg(face.frames);
    listItem->setData(CardRole::Id, face.id);
    listItem->setData(CardRole::Title, matchLabel(face.match));
    listItem->setData(CardRole::Subtitle, subtitle);
    listItem->setData(CardRole::Status, int(face.match.status));
    listItem->setData(CardRole::Score, double(face.match.score));
    if (!listItem->data(CardRole::Avatar).isValid() || m_videoMode)
        listItem->setData(CardRole::Avatar, circularPixmap(face.crop, 54));
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
    if (row < 0 || row >= m_faces.size()) {
        m_faceDetails->setText(QStringLiteral("اختر وجهاً من القائمة أو اضغط عليه في الصورة لعرض تفاصيله."));
        m_view->setSelectedFace(-1);
        return;
    }

    const FaceItem &face = m_faces.at(row);
    m_view->setSelectedFace(face.id);
    const QString color = matchColor(face.match.status).name();
    QString html = QStringLiteral("<div style='font-size:13pt; font-weight:600; color:%1'>%2</div>"
                                  "<div style='color:%3'>%4</div>")
                       .arg(Theme::Text.name(), matchLabel(face.match).toHtmlEscaped(), color, statusDescription(face.match.status));
    auto row_ = [](const QString &key, const QString &value) {
        return QStringLiteral("<br><span style='color:#8b97b3'>%1:</span> <span style='color:#e6eaf2'>%2</span>").arg(key, value.toHtmlEscaped());
    };
    html += row_(QStringLiteral("درجة التشابه"), QStringLiteral("%1%").arg(qRound(face.match.score * 100)));
    html += row_(QStringLiteral("جودة الوجه"), QStringLiteral("%1%").arg(qRound(face.quality * 100)));
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
        html += QStringLiteral("<br><span style='color:#f59e0b'>جودة الوجه منخفضة: النتيجة أقل موثوقية</span>");
    m_faceDetails->setText(html);
}

void MainWindow::saveFaces()
{
    if (m_faces.isEmpty())
        return;
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("اختر مجلد حفظ الوجوه"));
    if (folder.isEmpty())
        return;

    int saved = 0;
    for (int i = 0; i < m_faces.size(); ++i) {
        const FaceItem &face = m_faces.at(i);
        const QString name = face.match.status == MatchStatus::Unknown ? QStringLiteral("غير_معروف") : face.match.name;
        const QString file = QDir(folder).filePath(QStringLiteral("وجه_%1_%2.png").arg(i + 1).arg(safeFileName(name)));
        if (face.crop.save(file))
            ++saved;
    }
    Toast::show(this, QStringLiteral("تم حفظ %1 وجه").arg(saved));
}

void MainWindow::addSelectedFaceToDatabase()
{
    const int row = m_faceList->currentRow();
    if (row < 0 || row >= m_faces.size())
        return;
    const FaceItem face = m_faces.at(row);

    PersonPickerDialog picker(m_database.persons(), face.match.personId, this);
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
    if (face.aligned.empty())
        return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<std::vector<float>> embeddings = m_engine.enrollmentEmbeddings(face.aligned);
    const bool ok = m_database.addPhoto(personId, face.crop, embeddings, face.quality);
    QApplication::restoreOverrideCursor();
    return ok;
}

// ───────────── قاعدة البيانات ─────────────

QString MainWindow::selectedPersonId() const
{
    if (!m_personList)
        return {};
    const QListWidgetItem *item = m_personList->currentItem();
    return item ? item->data(CardRole::Id).toString() : QString();
}

void MainWindow::refreshStats()
{
    int photos = 0;
    const QList<Person> persons = m_database.persons();
    for (const Person &person : persons)
        photos += int(person.photos.size());
    m_personCountLabel->setText(QString::number(persons.size()));
    m_photoCountLabel->setText(QString::number(photos));
    m_databaseSubtitle->setText(QStringLiteral("%1 شخص مسجل · %2 صورة وجه · محفوظة على هذا الجهاز فقط")
                                    .arg(persons.size())
                                    .arg(photos));
}

void MainWindow::refreshPersons()
{
    if (!m_personList)
        return;
    refreshStats();
    const QString selected = selectedPersonId();

    // الصور الرمزية تُحمَّل عند ظهور البطاقة فقط، فتبقى القائمة سريعة مع آلاف الأشخاص
    const QSignalBlocker blocker(m_personList);
    m_personList->setUpdatesEnabled(false);
    m_personList->clear();
    for (const Person &person : m_database.persons()) {
        auto *item = new QListWidgetItem(m_personList);
        item->setData(CardRole::Id, person.id);
        item->setData(CardRole::Title, person.name);
        QString subtitle = QStringLiteral("%1 صورة").arg(person.photos.size());
        if (!person.nationalId.isEmpty())
            subtitle = QStringLiteral("رقم الهوية %1 · ").arg(person.nationalId) + subtitle;
        item->setData(CardRole::Subtitle, subtitle);
        item->setData(CardRole::Status, -1);
        item->setData(CardRole::SearchText, person.name + QLatin1Char(' ') + person.nationalId + QLatin1Char(' ') + person.phone);
        if (!person.photos.isEmpty())
            item->setData(CardRole::AvatarPath, m_database.photoPath(person.id, person.photos.first().file));
        if (person.id == selected)
            m_personList->setCurrentItem(item);
    }
    applyPersonFilter();
    m_personList->setUpdatesEnabled(true);
    showPersonDetails();
}

void MainWindow::applyPersonFilter()
{
    const QString filter = m_personFilter->text().trimmed();
    m_personList->setUpdatesEnabled(false);
    for (int i = 0; i < m_personList->count(); ++i) {
        QListWidgetItem *item = m_personList->item(i);
        item->setHidden(!filter.isEmpty() && !item->data(CardRole::SearchText).toString().contains(filter, Qt::CaseInsensitive));
    }
    m_personList->setUpdatesEnabled(true);
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
                                                "<div style='color:#8b97b3'>اختر شخصاً من القائمة لعرض بياناته وصوره، "
                                                "أو اضغط \"شخص جديد\" لإضافة شخص.</div>"));
        return;
    }

    const Person person = m_database.person(id);
    m_personAvatar->setPixmap(person.photos.isEmpty()
                                  ? Icons::icon(Icons::Name::Users, Theme::Muted, 96).pixmap(96, 96)
                                  : circularPixmapFromFile(m_database.photoPath(person.id, person.photos.first().file), 96));

    auto chip = [](const QString &key, const QString &value) {
        return QStringLiteral("<span style='color:#8b97b3'>%1</span> <span style='color:#e6eaf2'>%2</span>&nbsp;&nbsp;&nbsp;")
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
        html += QStringLiteral("<div style='color:#8b97b3; margin-top:6px'>%1</div>")
                    .arg(person.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    m_personDetails->setText(html);

    for (const FacePhoto &photo : person.photos) {
        auto *item = new QListWidgetItem(m_photoList);
        QImageReader reader(m_database.photoPath(person.id, photo.file));
        const QSize original = reader.size();
        if (original.isValid())
            reader.setScaledSize(original.scaled(m_photoList->iconSize(), Qt::KeepAspectRatio).boundedTo(original));
        item->setIcon(QIcon(QPixmap::fromImage(reader.read())));
        item->setText(QStringLiteral("جودة %1%").arg(qRound(photo.quality * 100)));
        item->setData(Qt::UserRole, photo.file);
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
    m_personFilter->clear();
    for (int i = 0; i < m_personList->count(); ++i) {
        if (m_personList->item(i)->data(CardRole::Id).toString() == id)
            m_personList->setCurrentRow(i);
    }
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
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("اختر صوراً لوجه الشخص"), QString(), ImageFilter);
    if (files.isEmpty())
        return;

    QProgressDialog progress(QStringLiteral("جارٍ تحليل الصور…"), QStringLiteral("إلغاء"), 0, int(files.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    int added = 0;
    QStringList noFace;
    QStringList multipleFaces;
    for (int i = 0; i < files.size() && !progress.wasCanceled(); ++i) {
        progress.setValue(i);
        progress.setLabelText(QStringLiteral("جارٍ تحليل %1…").arg(QFileInfo(files.at(i)).fileName()));
        QCoreApplication::processEvents();

        const cv::Mat image = ImageIO::loadImage(files.at(i));
        const std::vector<AnalyzedFace> faces = image.empty() ? std::vector<AnalyzedFace>() : m_engine.analyze(image, true);
        if (faces.empty()) {
            noFace << QFileInfo(files.at(i)).fileName();
            continue;
        }
        // عند وجود أكثر من وجه يُعتمد الأكبر
        const auto largest = std::max_element(faces.begin(), faces.end(), [](const AnalyzedFace &a, const AnalyzedFace &b) {
            return a.detection.box.area() < b.detection.box.area();
        });
        if (faces.size() > 1)
            multipleFaces << QFileInfo(files.at(i)).fileName();
        if (enrollFace(personId, ImageIO::makeFaceItem(image, *largest, 0)))
            ++added;
    }
    progress.setValue(int(files.size()));

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
    if (m_database.removePhoto(id, item->data(Qt::UserRole).toString()))
        Toast::show(this, QStringLiteral("تم حذف الصورة"), Toast::Kind::Info);
    else
        Toast::show(this, QStringLiteral("تعذر حذف الصورة"), Toast::Kind::Warning);
}

void MainWindow::openDatabaseFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_database.directory()));
}

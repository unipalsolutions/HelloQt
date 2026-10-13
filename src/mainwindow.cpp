#include "mainwindow.h"

#include "imageio.h"
#include "imageview.h"
#include "persondialog.h"
#include "videoworker.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
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

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("التعرف على الوجوه"));

    auto *tabs = new QTabWidget(this);
    tabs->addTab(createRecognitionTab(), QStringLiteral("التعرف على الوجوه"));
    tabs->addTab(createDatabaseTab(), QStringLiteral("قاعدة البيانات"));
    setCentralWidget(tabs);

    m_progress = new QProgressBar(this);
    m_progress->setMaximumWidth(240);
    m_progress->setVisible(false);
    statusBar()->addPermanentWidget(m_progress);

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

QWidget *MainWindow::createRecognitionTab()
{
    auto *page = new QWidget(this);

    m_openImageButton = new QPushButton(QStringLiteral("فتح صورة"), page);
    m_openVideoButton = new QPushButton(QStringLiteral("فتح فيديو"), page);
    m_cameraButton = new QPushButton(QStringLiteral("الكاميرا"), page);
    m_stopButton = new QPushButton(QStringLiteral("إيقاف"), page);
    m_saveFacesButton = new QPushButton(QStringLiteral("حفظ الوجوه المستخرجة"), page);
    connect(m_openImageButton, &QPushButton::clicked, this, &MainWindow::openImage);
    connect(m_openVideoButton, &QPushButton::clicked, this, &MainWindow::openVideo);
    connect(m_cameraButton, &QPushButton::clicked, this, &MainWindow::openCamera);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopVideo);
    connect(m_saveFacesButton, &QPushButton::clicked, this, &MainWindow::saveFaces);

    m_thresholdSpin = new QDoubleSpinBox(page);
    m_thresholdSpin->setRange(0.15, 0.80);
    m_thresholdSpin->setSingleStep(0.01);
    m_thresholdSpin->setValue(0.30);
    m_thresholdSpin->setToolTip(QStringLiteral("أعلى = أخطاء أقل لكن قد لا يُتعرف على صور رديئة. أقل = يتعرف أكثر مع احتمال خطأ أكبر."));
    connect(m_thresholdSpin, &QDoubleSpinBox::valueChanged, this, [this] {
        m_videoWorker->setThreshold(threshold());
        refreshMatches();
    });

    m_thoroughCheck = new QCheckBox(QStringLiteral("تحليل معمّق للصور (أدق وأبطأ)"), page);
    m_thoroughCheck->setChecked(true);

    m_frameStepSpin = new QSpinBox(page);
    m_frameStepSpin->setRange(1, 30);
    m_frameStepSpin->setValue(1);
    m_frameStepSpin->setToolTip(QStringLiteral("1 = تحليل كل إطار (أدق). قيمة أكبر = أسرع."));
    connect(m_frameStepSpin, &QSpinBox::valueChanged, this, [this](int value) { m_videoWorker->setFrameStep(value); });

    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_openImageButton);
    toolbar->addWidget(m_openVideoButton);
    toolbar->addWidget(m_cameraButton);
    toolbar->addWidget(m_stopButton);
    toolbar->addSpacing(16);
    toolbar->addWidget(new QLabel(QStringLiteral("حد التعرف:"), page));
    toolbar->addWidget(m_thresholdSpin);
    toolbar->addWidget(m_thoroughCheck);
    toolbar->addWidget(new QLabel(QStringLiteral("فيديو: تحليل إطار من كل"), page));
    toolbar->addWidget(m_frameStepSpin);
    toolbar->addStretch();
    toolbar->addWidget(m_saveFacesButton);

    m_view = new ImageView(page);
    connect(m_view, &ImageView::faceClicked, this, &MainWindow::selectFace);

    m_faceList = new QListWidget(page);
    m_faceList->setIconSize(QSize(72, 72));
    m_faceList->setSpacing(2);
    connect(m_faceList, &QListWidget::currentRowChanged, this, &MainWindow::showFaceDetails);

    m_faceDetails = new QLabel(page);
    m_faceDetails->setWordWrap(true);
    m_faceDetails->setTextFormat(Qt::RichText);
    m_faceDetails->setMinimumHeight(90);
    m_faceDetails->setAlignment(Qt::AlignTop);

    m_enrollFaceButton = new QPushButton(QStringLiteral("إضافة الوجه المحدد إلى قاعدة البيانات"), page);
    connect(m_enrollFaceButton, &QPushButton::clicked, this, &MainWindow::addSelectedFaceToDatabase);

    auto *sidePanel = new QWidget(page);
    auto *sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->addWidget(new QLabel(QStringLiteral("الوجوه المستخرجة:"), sidePanel));
    sideLayout->addWidget(m_faceList, 1);
    sideLayout->addWidget(m_faceDetails);
    sideLayout->addWidget(m_enrollFaceButton);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    splitter->addWidget(m_view);
    splitter->addWidget(sidePanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 320});

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(toolbar);
    layout->addWidget(splitter, 1);
    return page;
}

QWidget *MainWindow::createDatabaseTab()
{
    auto *page = new QWidget(this);

    auto *addPersonButton = new QPushButton(QStringLiteral("شخص جديد"), page);
    m_editPersonButton = new QPushButton(QStringLiteral("تعديل البيانات"), page);
    m_removePersonButton = new QPushButton(QStringLiteral("حذف الشخص"), page);
    m_addPhotosButton = new QPushButton(QStringLiteral("إضافة صور"), page);
    m_removePhotoButton = new QPushButton(QStringLiteral("حذف الصورة المحددة"), page);
    auto *openFolderButton = new QPushButton(QStringLiteral("فتح مجلد القاعدة"), page);
    connect(addPersonButton, &QPushButton::clicked, this, &MainWindow::addPerson);
    connect(m_editPersonButton, &QPushButton::clicked, this, &MainWindow::editPerson);
    connect(m_removePersonButton, &QPushButton::clicked, this, &MainWindow::removePerson);
    connect(m_addPhotosButton, &QPushButton::clicked, this, &MainWindow::addPhotos);
    connect(m_removePhotoButton, &QPushButton::clicked, this, &MainWindow::removePhoto);
    connect(openFolderButton, &QPushButton::clicked, this, &MainWindow::openDatabaseFolder);

    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(addPersonButton);
    toolbar->addWidget(m_editPersonButton);
    toolbar->addWidget(m_removePersonButton);
    toolbar->addSpacing(16);
    toolbar->addWidget(m_addPhotosButton);
    toolbar->addWidget(m_removePhotoButton);
    toolbar->addStretch();
    toolbar->addWidget(openFolderButton);

    m_personFilter = new QLineEdit(page);
    m_personFilter->setPlaceholderText(QStringLiteral("بحث بالاسم أو رقم الهوية…"));
    m_personFilter->setClearButtonEnabled(true);
    connect(m_personFilter, &QLineEdit::textChanged, this, &MainWindow::refreshPersons);

    m_personList = new QListWidget(page);
    m_personList->setIconSize(QSize(56, 56));
    connect(m_personList, &QListWidget::currentRowChanged, this, &MainWindow::showPersonDetails);
    connect(m_personList, &QListWidget::itemDoubleClicked, this, &MainWindow::editPerson);

    auto *listPanel = new QWidget(page);
    auto *listLayout = new QVBoxLayout(listPanel);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->addWidget(m_personFilter);
    listLayout->addWidget(m_personList, 1);

    m_personDetails = new QLabel(page);
    m_personDetails->setTextFormat(Qt::RichText);
    m_personDetails->setWordWrap(true);
    m_personDetails->setAlignment(Qt::AlignTop);
    m_personDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_photoList = new QListWidget(page);
    m_photoList->setViewMode(QListView::IconMode);
    m_photoList->setIconSize(QSize(128, 128));
    m_photoList->setResizeMode(QListView::Adjust);
    m_photoList->setMovement(QListView::Static);
    m_photoList->setSpacing(6);
    connect(m_photoList, &QListWidget::currentRowChanged, this, [this] { m_removePhotoButton->setEnabled(m_photoList->currentItem()); });

    auto *detailsPanel = new QWidget(page);
    auto *detailsLayout = new QVBoxLayout(detailsPanel);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->addWidget(m_personDetails);
    detailsLayout->addWidget(new QLabel(QStringLiteral("صور الوجه المسجلة (كلما زادت الصور وتنوعت زادت دقة التعرف):"), detailsPanel));
    detailsLayout->addWidget(m_photoList, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    splitter->addWidget(listPanel);
    splitter->addWidget(detailsPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(toolbar);
    layout->addWidget(splitter, 1);
    return page;
}

float MainWindow::threshold() const
{
    return float(m_thresholdSpin->value());
}

void MainWindow::setBusy(bool busy)
{
    const bool idle = m_ready && !busy && !m_videoRunning;
    m_openImageButton->setEnabled(idle);
    m_openVideoButton->setEnabled(idle);
    m_cameraButton->setEnabled(idle);
    m_stopButton->setEnabled(m_videoRunning);
    m_saveFacesButton->setEnabled(!m_faces.isEmpty());
    m_enrollFaceButton->setEnabled(m_ready && m_faceList->currentRow() >= 0);
    m_addPhotosButton->setEnabled(m_ready && !selectedPersonId().isEmpty());
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
    setBusy(true);
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    statusBar()->showMessage(QStringLiteral("جارٍ تحليل الصورة…"));

    const bool thorough = m_thoroughCheck->isChecked();
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
    m_progress->setVisible(false);
    if (!result.error.isEmpty()) {
        statusBar()->showMessage(result.error);
        QMessageBox::warning(this, windowTitle(), result.error);
        setBusy(false);
        return;
    }

    m_faces = result.faces;
    m_view->setImage(result.image);
    refreshMatches();
    if (!m_faces.isEmpty())
        m_faceList->setCurrentRow(0);
    statusBar()->showMessage(m_faces.isEmpty() ? QStringLiteral("لم يُعثر على وجوه في الصورة")
                                               : QStringLiteral("عدد الوجوه: %1").arg(m_faces.size()));
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
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    statusBar()->showMessage(QStringLiteral("جارٍ التحليل…"));
    setBusy(true);

    m_videoWorker->setFrameStep(m_frameStepSpin->value());
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
    m_view->setImage(frame);
    m_view->setFaces(faces);
    if (frameCount > 0) {
        m_progress->setRange(0, frameCount);
        m_progress->setValue(std::min(frameIndex, frameCount));
    }
    statusBar()->showMessage(QStringLiteral("الإطار %1 — وجوه في الإطار: %2 — أشخاص مختلفون حتى الآن: %3")
                                 .arg(frameIndex)
                                 .arg(faces.size())
                                 .arg(m_faces.size()));
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
    auto *item = new QListWidgetItem(m_faceList);
    updateFaceListItem(item, track);
    m_saveFacesButton->setEnabled(true);
}

void MainWindow::onVideoFinished(const QString &error)
{
    m_videoRunning = false;
    m_progress->setVisible(false);
    if (!error.isEmpty()) {
        statusBar()->showMessage(error);
        QMessageBox::warning(this, windowTitle(), error);
    } else {
        statusBar()->showMessage(QStringLiteral("انتهى التحليل — أشخاص مختلفون: %1").arg(m_faces.size()));
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
    showFaceDetails();
    setBusy(false);
}

void MainWindow::updateFaceListItem(QListWidgetItem *listItem, const FaceItem &face)
{
    if (!listItem)
        return;
    QString text = matchLabel(face.match);
    QString info = QStringLiteral("تشابه %1% · جودة %2%")
                       .arg(qRound(face.match.score * 100))
                       .arg(qRound(face.quality * 100));
    if (m_videoMode)
        info += QStringLiteral(" · %1 إطار").arg(face.frames);
    listItem->setText(text + QLatin1Char('\n') + info);
    listItem->setForeground(matchColor(face.match.status));
    listItem->setIcon(QIcon(QPixmap::fromImage(face.crop.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
    listItem->setData(Qt::UserRole, face.id);
}

void MainWindow::selectFace(int id)
{
    for (int i = 0; i < m_faces.size(); ++i) {
        if (m_faces.at(i).id == id) {
            m_faceList->setCurrentRow(i);
            return;
        }
    }
}

void MainWindow::showFaceDetails()
{
    const int row = m_faceList->currentRow();
    m_enrollFaceButton->setEnabled(m_ready && row >= 0);
    if (row < 0 || row >= m_faces.size()) {
        m_faceDetails->setText(QStringLiteral("اختر وجهاً من القائمة أو اضغط عليه في الصورة."));
        m_view->setSelectedFace(-1);
        return;
    }

    const FaceItem &face = m_faces.at(row);
    m_view->setSelectedFace(face.id);
    QString status;
    switch (face.match.status) {
    case MatchStatus::Confirmed:
        status = QStringLiteral("تطابق مؤكد");
        break;
    case MatchStatus::Possible:
        status = QStringLiteral("تطابق محتمل — يُنصح بالتحقق");
        break;
    case MatchStatus::Unknown:
        status = QStringLiteral("غير موجود في قاعدة البيانات");
        break;
    }

    QString html = QStringLiteral("<b style='font-size:14pt'>%1</b><br>%2<br>درجة التشابه: %3%<br>جودة الوجه: %4%")
                       .arg(matchLabel(face.match).toHtmlEscaped(), status)
                       .arg(qRound(face.match.score * 100))
                       .arg(qRound(face.quality * 100));
    if (!face.match.personId.isEmpty()) {
        const Person person = m_database.person(face.match.personId);
        if (!person.nationalId.isEmpty())
            html += QStringLiteral("<br>رقم الهوية: %1").arg(person.nationalId.toHtmlEscaped());
        if (!person.phone.isEmpty())
            html += QStringLiteral("<br>الهاتف: %1").arg(person.phone.toHtmlEscaped());
        if (!person.notes.isEmpty())
            html += QStringLiteral("<br>ملاحظات: %1").arg(person.notes.toHtmlEscaped());
    }
    if (face.quality < 0.35f)
        html += QStringLiteral("<br><span style='color:#d68a00'>جودة الوجه منخفضة: النتيجة أقل موثوقية</span>");
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
    statusBar()->showMessage(QStringLiteral("تم حفظ %1 وجه في %2").arg(saved).arg(QDir::toNativeSeparators(folder)));
}

void MainWindow::addSelectedFaceToDatabase()
{
    const int row = m_faceList->currentRow();
    if (row < 0 || row >= m_faces.size())
        return;
    const FaceItem face = m_faces.at(row);

    const QString newPerson = QStringLiteral("＋ شخص جديد…");
    QStringList choices{newPerson};
    QStringList ids{QString()};
    for (const Person &person : m_database.persons()) {
        choices << (person.nationalId.isEmpty() ? person.name : QStringLiteral("%1 (%2)").arg(person.name, person.nationalId));
        ids << person.id;
    }
    int current = 0;
    if (!face.match.personId.isEmpty())
        current = std::max(0, int(ids.indexOf(face.match.personId)));

    bool ok = false;
    const QString choice = QInputDialog::getItem(this, QStringLiteral("إضافة الوجه"),
                                                 QStringLiteral("أضف هذا الوجه إلى:"), choices, current, false, &ok);
    if (!ok)
        return;

    QString personId = ids.value(choices.indexOf(choice));
    if (personId.isEmpty()) {
        PersonDialog dialog(Person(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        personId = m_database.addPerson(dialog.person());
        if (personId.isEmpty()) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("تعذر حفظ الشخص"));
            return;
        }
    }

    if (enrollFace(personId, face))
        statusBar()->showMessage(QStringLiteral("أُضيف الوجه إلى %1").arg(m_database.person(personId).name));
    else
        QMessageBox::warning(this, windowTitle(), QStringLiteral("تعذر إضافة الوجه"));
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
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void MainWindow::refreshPersons()
{
    if (!m_personList)
        return;
    const QString selected = selectedPersonId();
    const QString filter = m_personFilter->text().trimmed();

    m_personList->clear();
    for (const Person &person : m_database.persons()) {
        if (!filter.isEmpty() && !person.name.contains(filter, Qt::CaseInsensitive)
            && !person.nationalId.contains(filter, Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(m_personList);
        item->setText(QStringLiteral("%1\nعدد الصور: %2").arg(person.name).arg(person.photos.size()));
        item->setData(Qt::UserRole, person.id);
        if (!person.photos.isEmpty()) {
            const QPixmap pixmap(m_database.photoPath(person.id, person.photos.first().file));
            item->setIcon(QIcon(pixmap.scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
        if (person.id == selected)
            m_personList->setCurrentItem(item);
    }
    showPersonDetails();
}

void MainWindow::showPersonDetails()
{
    const QString id = selectedPersonId();
    m_editPersonButton->setEnabled(!id.isEmpty());
    m_removePersonButton->setEnabled(!id.isEmpty());
    m_addPhotosButton->setEnabled(m_ready && !id.isEmpty());
    m_photoList->clear();
    m_removePhotoButton->setEnabled(false);

    if (id.isEmpty()) {
        m_personDetails->setText(QStringLiteral("عدد الأشخاص في القاعدة: %1<br>اختر شخصاً لعرض بياناته، أو أضف شخصاً جديداً.")
                                     .arg(m_database.persons().size()));
        return;
    }

    const Person person = m_database.person(id);
    QString html = QStringLiteral("<b style='font-size:15pt'>%1</b>").arg(person.name.toHtmlEscaped());
    if (!person.nationalId.isEmpty())
        html += QStringLiteral("<br>رقم الهوية: %1").arg(person.nationalId.toHtmlEscaped());
    if (!person.phone.isEmpty())
        html += QStringLiteral("<br>الهاتف: %1").arg(person.phone.toHtmlEscaped());
    if (!person.notes.isEmpty())
        html += QStringLiteral("<br>ملاحظات: %1").arg(person.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    html += QStringLiteral("<br>تاريخ الإضافة: %1").arg(QLocale().toString(person.created, QLocale::ShortFormat));
    m_personDetails->setText(html);

    for (const FacePhoto &photo : person.photos) {
        auto *item = new QListWidgetItem(m_photoList);
        item->setIcon(QIcon(QPixmap(m_database.photoPath(person.id, photo.file))));
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
        QMessageBox::warning(this, windowTitle(), QStringLiteral("تعذر حفظ الشخص"));
        return;
    }
    m_personFilter->clear();
    for (int i = 0; i < m_personList->count(); ++i) {
        if (m_personList->item(i)->data(Qt::UserRole).toString() == id)
            m_personList->setCurrentRow(i);
    }
    if (m_ready)
        addPhotosTo(id);
}

void MainWindow::editPerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    PersonDialog dialog(m_database.person(id), this);
    if (dialog.exec() == QDialog::Accepted && !m_database.updatePerson(dialog.person()))
        QMessageBox::warning(this, windowTitle(), QStringLiteral("تعذر حفظ التعديلات"));
}

void MainWindow::removePerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    const Person person = m_database.person(id);
    const auto answer = QMessageBox::question(this, QStringLiteral("حذف الشخص"),
                                              QStringLiteral("هل تريد حذف \"%1\" وكل صوره من قاعدة البيانات؟").arg(person.name));
    if (answer == QMessageBox::Yes && !m_database.removePerson(id))
        QMessageBox::warning(this, windowTitle(), QStringLiteral("تعذر حذف الشخص"));
}

void MainWindow::addPhotos()
{
    const QString id = selectedPersonId();
    if (!id.isEmpty())
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
    if (!m_database.removePhoto(id, item->data(Qt::UserRole).toString()))
        QMessageBox::warning(this, windowTitle(), QStringLiteral("تعذر حذف الصورة"));
}

void MainWindow::openDatabaseFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_database.directory()));
}

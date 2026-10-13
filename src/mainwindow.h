#pragma once

#include "facedatabase.h"
#include "faceengine.h"
#include "facetypes.h"

#include <QFutureWatcher>
#include <QList>
#include <QMainWindow>

class ImageView;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QThread;
class VideoWorker;

struct ImageAnalysis
{
    QImage image;
    QList<FaceItem> faces;
    QString error;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString &path); // صورة أو فيديو

private:
    QWidget *createRecognitionTab();
    QWidget *createDatabaseTab();

    // التعرف
    void openImage();
    void analyzeImage(const QString &path);
    void openVideo();
    void openCamera();
    void startVideo(const QString &source);
    void stopVideo();
    void onImageAnalyzed();
    void onFrameReady(const QImage &frame, const QList<FaceItem> &faces, int frameIndex, int frameCount);
    void onTrackUpdated(const FaceItem &track);
    void onVideoFinished(const QString &error);
    void saveFaces();
    void addSelectedFaceToDatabase();
    void selectFace(int id);
    void showFaceDetails();
    void refreshMatches();
    void refreshFaceList();
    void updateFaceListItem(QListWidgetItem *listItem, const FaceItem &face);
    void setBusy(bool busy);
    float threshold() const;

    // قاعدة البيانات
    void refreshPersons();
    void showPersonDetails();
    void addPerson();
    void editPerson();
    void removePerson();
    void addPhotos();
    void addPhotosTo(const QString &personId);
    void removePhoto();
    void openDatabaseFolder();
    QString selectedPersonId() const;
    bool enrollFace(const QString &personId, const FaceItem &face);

    FaceEngine m_engine;
    FaceDatabase m_database;
    bool m_ready = false;

    QList<FaceItem> m_faces; // وجوه الصورة الحالية، أو مسارات الوجوه في الفيديو
    bool m_videoMode = false;
    bool m_videoRunning = false;
    QFutureWatcher<ImageAnalysis> m_imageWatcher;
    QThread *m_videoThread = nullptr;
    VideoWorker *m_videoWorker = nullptr;

    ImageView *m_view = nullptr;
    QListWidget *m_faceList = nullptr;
    QLabel *m_faceDetails = nullptr;
    QPushButton *m_openImageButton = nullptr;
    QPushButton *m_openVideoButton = nullptr;
    QPushButton *m_cameraButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_saveFacesButton = nullptr;
    QPushButton *m_enrollFaceButton = nullptr;
    QDoubleSpinBox *m_thresholdSpin = nullptr;
    QCheckBox *m_thoroughCheck = nullptr;
    QSpinBox *m_frameStepSpin = nullptr;
    QProgressBar *m_progress = nullptr;

    QLineEdit *m_personFilter = nullptr;
    QListWidget *m_personList = nullptr;
    QLabel *m_personDetails = nullptr;
    QListWidget *m_photoList = nullptr;
    QPushButton *m_editPersonButton = nullptr;
    QPushButton *m_removePersonButton = nullptr;
    QPushButton *m_addPhotosButton = nullptr;
    QPushButton *m_removePhotoButton = nullptr;
};

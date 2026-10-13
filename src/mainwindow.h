#pragma once

#include "facedatabase.h"
#include "faceengine.h"
#include "facetypes.h"

#include <QFutureWatcher>
#include <QList>
#include <QMainWindow>

class AnimatedStack;
class FancyButton;
class ImageView;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QSlider;
class QSpinBox;
class QThread;
class ToggleSwitch;
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
    QWidget *createSidebar();
    QWidget *createRecognitionPage();
    QWidget *createDatabasePage();

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
    void updateFaceCount();
    void setBusy(bool busy);
    float threshold() const;

    // قاعدة البيانات
    void refreshPersons();
    void refreshStats();
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
    bool m_analyzing = false;
    QFutureWatcher<ImageAnalysis> m_imageWatcher;
    QThread *m_videoThread = nullptr;
    VideoWorker *m_videoWorker = nullptr;

    AnimatedStack *m_pages = nullptr;
    QLabel *m_personCountLabel = nullptr;
    QLabel *m_photoCountLabel = nullptr;

    ImageView *m_view = nullptr;
    QListWidget *m_faceList = nullptr;
    QLabel *m_faceCount = nullptr;
    QLabel *m_faceDetails = nullptr;
    FancyButton *m_openImageButton = nullptr;
    FancyButton *m_openVideoButton = nullptr;
    FancyButton *m_cameraButton = nullptr;
    FancyButton *m_stopButton = nullptr;
    FancyButton *m_saveFacesButton = nullptr;
    FancyButton *m_enrollFaceButton = nullptr;
    QSlider *m_thresholdSlider = nullptr;
    QLabel *m_thresholdValue = nullptr;
    ToggleSwitch *m_thoroughSwitch = nullptr;
    QSpinBox *m_frameStepSpin = nullptr;
    QProgressBar *m_progress = nullptr;

    QLabel *m_databaseSubtitle = nullptr;
    QLineEdit *m_personFilter = nullptr;
    QListWidget *m_personList = nullptr;
    QLabel *m_personAvatar = nullptr;
    QLabel *m_personDetails = nullptr;
    QListWidget *m_photoList = nullptr;
    QWidget *m_personActions = nullptr;
    FancyButton *m_removePhotoButton = nullptr;
};

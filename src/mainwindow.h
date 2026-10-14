#pragma once

#include "facedatabase.h"
#include "faceengine.h"
#include "facetypes.h"
#include "shotviewer.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QList>
#include <QMainWindow>
#include <QRectF>
#include <QSet>

class AnimatedStack;
class QButtonGroup;
class QComboBox;
class QTimeEdit;
class FancyButton;
class ImageView;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QListView;
class PersonListModel;
class QTimer;
class QProgressBar;
class QSlider;
class QSpinBox;
class QThread;
class ToggleSwitch;
class TimelineView;
class VideoWorker;
class MatchPage;
class QFrame;
class QStackedWidget;
class StrengthMeter;

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

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void onEngineLoaded();
    void setupShortcuts();
    void loadSettings();
    void saveSettings() const;
    void pasteImage();
    QWidget *createSidebar();
    QWidget *createRecognitionPage();
    QWidget *createDatabasePage();
    void updateResponsiveLayout();

    // التعرف
    void openImage();
    void analyzeImage(const QString &path);
    void openVideo();
    void openCamera();
    void startVideo(const QString &source);
    void stopVideo();
    void togglePause();
    void onImageAnalyzed();
    void onFrameReady(const QImage &frame, const QList<FaceItem> &faces, qint64 positionMs, double progress);
    void onTrackUpdated(const FaceItem &track);
    void onVideoFinished(const QString &error);
    void saveFaces();
    void updateFolderNavigation();
    void navigateImage(int step);
    void showViewMenu(const QPoint &pos);
    void saveAnnotatedImage();
    void assignFace(int faceId, const QString &personId);
    void setVideoSettingsVisible(bool visible);
    void refreshWatched();
    void checkWatchAlert(const FaceItem &face);
    void clearWatchAlerts();
    void toggleWatch();
    void addRecentFile(const QString &path);
    void showRecentMenu();
    void addSelectedFaceToDatabase();
    void addFaceToDatabase(const FaceItem &face);
    void onIdentitiesMerged(int keepId, int removedId);
    void showFaceContextMenu(const QPoint &pos);
    QList<int> selectedFaceIds() const;
    void mergeFaces(const QList<int> &ids);
    void removeFaces(const QList<int> &ids);
    void openShotViewer(int id, qint64 positionMs = -1);
    void rebuildTimelineLanes();
    void loadVideoFrame(qint64 positionMs);
    void onVideoFrameLoaded();
    void analyzeVideoFrom(qint64 positionMs);
    void reanalyzeVideo();
    void openEnhancer();
    void openFolderScan();
    void enhanceVideoFrame(qint64 positionMs);
    void showTimelineMenu(const QPoint &globalPos, qint64 positionMs, int laneId);
    void exportClip(const QList<QPair<qint64, qint64>> &segments, const QString &suggestedName);
    void saveVideoFrame(qint64 positionMs);
    void setTrimRange(qint64 startMs, qint64 endMs);
    void sortFaces();
    void applyFaceFilter();
    bool faceMatchesFilter(const FaceItem &face) const;
    void rebuildFaceRows();
    void updateVideoOptions();
    void selectFace(int id);
    void showFaceDetails();
    QString candidatesHtml(const FaceItem &face) const;
    void refreshMatches();
    void refreshFaceList();
    void updateFaceListItem(QListWidgetItem *listItem, const FaceItem &face);
    void updateFaceCount();
    void setBusy(bool busy);
    float threshold() const;

    // قاعدة البيانات
    void refreshPersons();
    void applyPersonFilter();
    void showPerson(const QString &personId);
    void updateDatabaseSubtitle();
    void refreshStats();
    void showPersonDetails();
    void addPerson();
    void editPerson();
    void removePerson();
    void addPhotos();
    void addPhotosTo(const QString &personId);
    void addPhotoFiles(const QString &personId, const QStringList &files);
    void removePhoto();
    void openDatabaseFolder();
    void backupDatabase();
    void restoreDatabase();
    QString selectedPersonId() const;
    bool enrollFace(const QString &personId, const FaceItem &face);
    int enrollFaces(const QString &personId, const QList<FaceItem> &faces);
    QString choosePerson(const QString &suggestedId);
    void harvestPhotos(const QString &personId);
    void harvestFromFace(int faceId);
    void recordSightings(const QString &source);
    void setCoverPhoto();
    void mergeDuplicate();
    void exportPerson();

    FaceEngine m_engine;
    MatchPage *m_matchPage = nullptr;
    FaceDatabase m_database;
    bool m_ready = false;
    QFutureWatcher<QString> m_loadWatcher; // تحميل النماذج في الخلفية حتى تظهر النافذة فوراً
    QString m_databaseError;
    QString m_pendingFile;
    QString m_currentImagePath;
    QString m_lastDirectory;
    QStringList m_folderImages; // صور مجلد الصورة الحالية للتنقل بينها
    QString m_folderPath;
    int m_folderIndex = -1;

    QList<FaceItem> m_faces; // وجوه الصورة الحالية، أو مسارات الوجوه في الفيديو
    QHash<int, int> m_faceRows; // رقم الوجه ← موضعه، حتى يبقى التحديث سريعاً مع آلاف الأشخاص
    bool m_videoMode = false;
    bool m_videoRunning = false;
    bool m_analyzing = false;
    QFutureWatcher<ImageAnalysis> m_imageWatcher;
    QThread *m_videoThread = nullptr;
    VideoWorker *m_videoWorker = nullptr;
    QElapsedTimer m_videoClock;
    int m_videoStep = 1;
    qint64 m_videoDurationMs = 0;
    qint64 m_videoStartMs = 0; // الجزء المحلَّل من الفيديو
    qint64 m_videoEndMs = 0;
    qint64 m_videoPositionMs = 0;
    QString m_videoSource;   // آخر فيديو أو كاميرا حُلِّل، لإعادة التحليل والرجوع للقطات
    qint64 m_restartFromMs = -1; // إعادة التحليل من موضع بعد توقف التحليل الجاري
    QSet<int> m_hiddenFaces;     // وجوه حذفها المستخدم أثناء التحليل فلا تعود للظهور
    double m_videoFps = 25.0;
    // قراءة إطار من الفيديو عند النقر على الخط الزمني بعد انتهاء التحليل
    std::shared_ptr<cv::VideoCapture> m_frameCapture;
    QString m_frameCapturePath;
    QFutureWatcher<ShotFrame> m_frameWatcher;
    qint64 m_frameRequestMs = -1;
    TimelineView *m_timeline = nullptr;
    QWidget *m_timelineCard = nullptr;
    QTimer *m_laneTimer = nullptr;
    QHash<int, QPair<float, QPixmap>> m_laneAvatars; // صورة كل مسار مع جودة الوجه الذي صُنعت منه
    bool m_videoPaused = false;
    QElapsedTimer m_pauseClock;
    qint64 m_pausedMs = 0;

    AnimatedStack *m_pages = nullptr;
    QWidget *m_sidebar = nullptr;
    QList<QWidget *> m_sidebarExtras; // تختفي في الشريط الجانبي المصغر
    QList<FancyButton *> m_navButtons;
    QList<FancyButton *> m_headerButtons;
    QList<QLabel *> m_subtitles;
    QLabel *m_photosHint = nullptr;
    int m_layoutMode = -1;
    QButtonGroup *m_navGroup = nullptr;
    QLabel *m_personCountLabel = nullptr;
    QLabel *m_photoCountLabel = nullptr;

    ImageView *m_view = nullptr;
    QListWidget *m_faceList = nullptr;
    QLabel *m_faceCount = nullptr;
    QLabel *m_faceDetails = nullptr;
    FancyButton *m_openImageButton = nullptr;
    FancyButton *m_openVideoButton = nullptr;
    FancyButton *m_cameraButton = nullptr;
    FancyButton *m_pauseButton = nullptr;
    FancyButton *m_stopButton = nullptr;
    FancyButton *m_saveFacesButton = nullptr;
    FancyButton *m_enrollFaceButton = nullptr;
    QSlider *m_thresholdSlider = nullptr;
    QLabel *m_thresholdValue = nullptr;
    ToggleSwitch *m_thoroughSwitch = nullptr;
    ToggleSwitch *m_enhanceSwitch = nullptr;
    QSpinBox *m_frameStepSpin = nullptr;
    QComboBox *m_performanceCombo = nullptr;
    FancyButton *m_enhanceImageButton = nullptr;
    FancyButton *m_folderScanButton = nullptr;
    class EnhanceDialog *m_studio = nullptr; // صفحة تحسين الصور
    QProgressBar *m_progress = nullptr;
    QLabel *m_thresholdWarning = nullptr;
    QTimeEdit *m_rangeStart = nullptr;
    QTimeEdit *m_rangeEnd = nullptr;
    FancyButton *m_regionButton = nullptr;
    FancyButton *m_clearRegionButton = nullptr;
    FancyButton *m_reanalyzeButton = nullptr;
    QLineEdit *m_faceFilter = nullptr;
    QComboBox *m_faceStatusFilter = nullptr;
    QComboBox *m_faceSort = nullptr;
    FancyButton *m_viewShotButton = nullptr;
    QList<QWidget *> m_videoSettings;
    // قائمة المراقبة: تنبيه فور ظهور شخص مراقَب
    QSet<QString> m_watched;
    QSet<QString> m_alerted; // من نُبِّه عنه في الصورة أو الفيديو الحالي
    QFrame *m_alertBar = nullptr;
    QLabel *m_alertAvatar = nullptr;
    QLabel *m_alertText = nullptr;
    int m_alertFaceId = -1;
    int m_alertCount = 0;
    QTimer *m_alertPulse = nullptr;
    FancyButton *m_watchButton = nullptr;
    FancyButton *m_recentButton = nullptr; // إعدادات تخص الفيديو فقط فتختفي عند عرض صورة

    QLabel *m_databaseSubtitle = nullptr;
    QLineEdit *m_personFilter = nullptr;
    QListView *m_personList = nullptr;
    PersonListModel *m_personModel = nullptr;
    QTimer *m_filterTimer = nullptr;
    QLabel *m_personAvatar = nullptr;
    QLabel *m_personDetails = nullptr;
    QListWidget *m_photoList = nullptr;
    QWidget *m_personActions = nullptr;
    FancyButton *m_removePhotoButton = nullptr;
    FancyButton *m_coverButton = nullptr;
    FancyButton *m_harvestButton = nullptr;
    FancyButton *m_photosTab = nullptr;
    FancyButton *m_sightingsTab = nullptr;
    QStackedWidget *m_profileStack = nullptr;
    QListWidget *m_sightingList = nullptr;
    StrengthMeter *m_strengthMeter = nullptr;
    QFrame *m_duplicateBanner = nullptr;
    QLabel *m_duplicateText = nullptr;
    QString m_duplicateId;
};

#pragma once

#include "facedatabase.h"
#include "faceengine.h"
#include "facetypes.h"
#include "shotviewer.h"
#include "videofilters.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QRectF>
#include <QSet>

class AnimatedStack;
class QButtonGroup;
class QComboBox;
class QTimeEdit;
class QMenu;
class QSystemTrayIcon;
class FancyButton;
class Maintenance;
class ImageView;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QListView;
class PersonListModel;
class QTimer;
class CameraRecorder;
class CameraWatch;
class MultiCameraWindow;
class QProgressBar;
class QSlider;
class QSpinBox;
class QThread;
class ToggleSwitch;
class TimelineView;
class VideoPlayer;
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
    // فُتح البرنامج مع ويندوز: يبقى بجانب الساعة دون نافذة إن أمكن
    bool startHidden() const;
    // طلب من نسخة ثانية فُتحت: "show" أو "open:<مسار>" أو "startup" (لا شيء، البرنامج يعمل أصلاً)
    void activateFromOtherInstance(const QString &message);
    // لم يُغلق البرنامج إغلاقاً طبيعياً في التشغيل السابق (يُعرض تنبيه بعد تحميل النماذج)
    void setPreviousCrash(const QDateTime &started) { m_previousCrash = started; }


signals:
    // تنبيه مراقبة (لإشعار ويندوز حين تكون النافذة مخفية)؛ camera فارغ للملفات
    // level مستوى تنبيه الشخص (AlertSound::Level)
    void watchAlert(const QString &name, const QString &camera, int level);
    // عطل يحتاج انتباه المستخدم (كاميرا منقطعة طويلاً، توقف التسجيل)؛ يُكتب في السجل ويصل إشعاراً
    void systemAlert(const QString &title, const QString &text);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void onEngineLoaded();
    // فحص كرت الشاشة في عملية منفصلة؛ interactive: نتيجته في رسالة، وإلا إشعار قصير
    void verifyGpu(bool interactive);
    // ينقل المحرك إلى الكرت أو المعالج حسب الإعدادات ونتيجة الفحص، دون إعادة تشغيل البرنامج
    void applyEngineDevice();
    // مكان التحليل وسرعته في شريط الحالة، مع تنبيه إن رجع إلى المعالج
    void updateDeviceLabel();
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
    void openCamera();          // قائمة الكاميرات المحفوظة والموصولة
    void startLastCamera();     // آخر كاميرا استُخدمت (Ctrl+Shift+C)
    // المناطق وساعات التنبيه والتسجيل لكاميرا
    void editCameraRules(const QString &source, const QString &name);
    // مجهول ظهوره مريب (تكرر كثيراً اليوم أو ظهر ليلاً)
    void showStrangerAlert(qint64 visitorId, const QString &reason, const QString &camera);
    void openMultiCamera();     // عرض عدة كاميرات معاً ومراقبتها
    void manageCameras(const QString &selectId = QString());
    void startVideo(const QString &source);
    void stopVideo();
    void togglePause();
    // تسجيل الكاميرا الجارية إلى ملفات فيديو
    void toggleRecording();
    void startRecording();
    QString stopRecording(); // رسالة الحفظ، أو فارغ إن لم يكن التسجيل جارياً
    void updateRecordButton();
    void showRecordFolderMenu();
    void setupTray();
    void showFromTray();
    void openSettings();
    void quitApplication();
    void onImageAnalyzed();
    void onFrameReady(const QImage &frame, const QList<FaceItem> &faces, qint64 positionMs, double progress);
    void onTrackUpdated(const FaceItem &track);
    void onVideoFinished(const QString &error);
    void onPlayerFrame(const QImage &frame, qint64 positionMs);
    // وجوه الموضع المعروض من نتائج التحليل المحفوظة، بين أقرب إطارين محللين
    QList<FaceItem> analyzedFacesAt(qint64 positionMs) const;
    bool hasAnalysisNear(qint64 positionMs) const;
    bool filePlayerOpen() const;
    void updatePauseButton();
    void saveFaces();
    void updateFolderNavigation();
    void navigateImage(int step);
    void showViewMenu(const QPoint &pos);
    void saveAnnotatedImage();
    void assignFace(int faceId, const QString &personId);
    void setVideoSettingsVisible(bool visible);
    void refreshWatched();
    void checkWatchAlert(const FaceItem &face);
    void showWatchAlert(const FaceItem &face, const QImage &crop);
    // الكاميرا: ظهور شخص معروف بعد غياب يُسجَّل (مع موضعه في التسجيل وصورة الإطار) وينبَّه عنه إن كان مراقَباً
    void noteCameraFaces(const QImage &frame, const QList<FaceItem> &faces);
    void clearWatchAlerts();
    void toggleWatch();
    void showAlertLog();
    void showVisitors();
    void showRecordings();
    void showDailyReport();
    // السجلات والتقارير: التنبيهات، الزوار المجهولون، التسجيلات، تقرير اليوم، سجل الأحداث
    void addRecordsMenu(QMenu *parent);
    // يفتح الملف الذي ظهر فيه شخص، والفيديو من لحظة ظهوره
    void openSighting(const QString &path, qint64 positionMs);
    void importPersons();
    void addRecentFile(const QString &path);
    void showRecentMenu();
    void addSelectedFaceToDatabase();
    void addFaceToDatabase(FaceItem face); // نسخة لأن الحوار يعيد بناء m_faces أثناء انتظاره
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
    void openVideoFilters();
    void updateFiltersButton();
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
    QDateTime m_previousCrash;
    qint64 m_cameraLostAt = 0;      // بداية انقطاع الكاميرا الجارية
    bool m_cameraOfflineAlerted = false;
    QTimer *m_cameraRetryTimer = nullptr; // إعادة تشغيل الكاميرا الموصولة إن انقطعت ولم تعد
    QFutureWatcher<QString> m_loadWatcher; // تحميل النماذج في الخلفية حتى تظهر النافذة فوراً
    QFutureWatcher<QString> m_deviceWatcher; // نقل المحرك بين المعالج والكرت في الخلفية
    QLabel *m_deviceLabel = nullptr;
    QTimer *m_deviceTimer = nullptr;
    QTimer *m_detailsTimer = nullptr; // تحديث تفاصيل الوجه المحدد أثناء التحليل بتمهّل
    int m_analyzedFrames = 0; // الإطارات المحللة منذ آخر تحديث لسرعة التحليل
    QString m_shownFallback;  // سبب الرجوع إلى المعالج الذي نُبّه إليه
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
    QString m_cameraName;    // اسم الكاميرا الجارية
    QString m_cameraId;      // كاميرا محفوظة تحمل معالجاتها الخاصة (فارغ للملفات والكاميرا غير المحفوظة)
    VideoFilters::Settings m_fileFilters; // معالجات الملفات، تعود بعد إيقاف كاميرا بمعالجاتها الخاصة
    qint64 m_restartFromMs = -1; // إعادة التحليل من موضع بعد توقف التحليل الجاري
    QSet<int> m_hiddenFaces;     // وجوه حذفها المستخدم أثناء التحليل فلا تعود للظهور
    // العرض بسرعة الفيديو الأصلية منفصلاً عن التحليل، ونتائج التحليل لكل موضع محلل لتُعرض عليه
    VideoPlayer *m_player = nullptr;
    QMap<qint64, QList<FaceItem>> m_analyzedFaces;
    QHash<int, int> m_mergedInto; // هوية دُمجت ← الهوية الباقية
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
    CameraRecorder *m_recorder = nullptr;
    QTimer *m_recordTimer = nullptr; // يحدّث مدة التسجيل على الزر
    FancyButton *m_recordButton = nullptr;
    QWidget *m_recordGroup = nullptr; // مجلد التسجيلات والتسجيل التلقائي، للكاميرا فقط
    ToggleSwitch *m_autoRecordSwitch = nullptr;
    ToggleSwitch *m_gpuSwitch = nullptr;
    FancyButton *m_gpuCheckButton = nullptr;
    bool m_gpuChecking = false;
    QSystemTrayIcon *m_tray = nullptr;
    QPointer<MultiCameraWindow> m_multiCamera;
    bool m_quitting = false;
    bool m_trayHintShown = false;
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
    FancyButton *m_importButton = nullptr;
    class EnhanceDialog *m_studio = nullptr; // صفحة تحسين الصور
    QProgressBar *m_progress = nullptr;
    QLabel *m_thresholdWarning = nullptr;
    QTimeEdit *m_rangeStart = nullptr;
    QTimeEdit *m_rangeEnd = nullptr;
    FancyButton *m_regionButton = nullptr;
    FancyButton *m_clearRegionButton = nullptr;
    FancyButton *m_reanalyzeButton = nullptr;
    FancyButton *m_filtersButton = nullptr;
    QLineEdit *m_faceFilter = nullptr;
    QComboBox *m_faceStatusFilter = nullptr;
    QComboBox *m_faceSort = nullptr;
    FancyButton *m_viewShotButton = nullptr;
    QList<QWidget *> m_videoSettings;
    // قائمة المراقبة: تنبيه فور ظهور شخص مراقَب
    QSet<QString> m_watched;
    QSet<QString> m_alerted; // من نُبِّه عنه في الصورة أو الفيديو الحالي
    CameraWatch *m_cameraWatch = nullptr; // ظهور الأشخاص والزوار المجهولين أمام الكاميرا الجارية
    Maintenance *m_maintenance = nullptr; // حذف القديم والنسخ الاحتياطي التلقائي
    QList<FaceItem> m_cameraFaces;         // وجوه آخر إطار من الكاميرا (لربط التنبيه بوجهه)
    QFrame *m_alertBar = nullptr;
    QLabel *m_alertAvatar = nullptr;
    QLabel *m_alertText = nullptr;
    int m_alertFaceId = -1;
    int m_alertCount = 0;
    QTimer *m_alertPulse = nullptr;
    FancyButton *m_watchButton = nullptr;
    FancyButton *m_alertLevelButton = nullptr;
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

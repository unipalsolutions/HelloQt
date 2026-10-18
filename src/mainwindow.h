#pragma once

#include "facedatabase.h"
#include "faceengine.h"
#include "facetypes.h"
#include "icons.h"
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

#include <functional>

class AnimatedStack;
class ElidedLabel;
class StartScreen;
class QSplitter;
class QStackedWidget;
class FaceCardDelegate;
class FaceDetailsPanel;
class QToolButton;
class QVBoxLayout;
class QButtonGroup;
class QComboBox;
class QTimeEdit;
class QMenu;
class QSystemTrayIcon;
class FancyButton;
class ArchiveScanDialog;
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
class SegmentedControl;
class SideDrawer;
class Stepper;
class FloatingBar;
class QHBoxLayout;
class ProgressRing;
class CollapsibleSection;
class QThread;
class ToggleSwitch;
class TimelineView;
class VideoPlayer;
class VideoWorker;
class MatchPage;
class QFrame;
class QStackedWidget;
class StrengthMeter;
class PlaybackBar;
class VideoAudio;

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
    bool restoreWindowGeometry(); // آخر حجم وموضع للنافذة؛ false إن لم يُحفظ شيء بعد
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
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

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
    // ما يعرضه شريط الأزرار والإعدادات: لا شيء مفتوح، صورة، فيديو ملف، كاميرا
    enum class WorkState { Empty, Image, Video, Camera };
    WorkState workState() const;
    void updateWorkState();
    QWidget *createStatusStrip(QWidget *parent);
    void updateStatusStrip();
    void setStatusAction(Icons::Name icon, const QString &text, const QString &tip, std::function<void()> action);
    void showOpenMenu();
    void fillRecentMenu(QMenu *menu);
    void closeCurrentFile(); // يعود إلى شاشة البداية
    void setAdvancedSettingsVisible(bool visible);
    void restoreLayout();
    void saveLayout() const;

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
    void onPlayerFrame(const QImage &frame, qint64 positionMs, const QImage &fitted);
    // وجوه الموضع المعروض من نتائج التحليل المحفوظة، بين أقرب إطارين محللين
    QList<FaceItem> analyzedFacesAt(qint64 positionMs) const;
    bool cameraBoxesHeld() const;
    bool hasAnalysisNear(qint64 positionMs) const;
    // شارة على الإطار حين يُعرض موضع لم يصله التحليل، فلا يُظن أن البرنامج لم يجد وجوهاً
    void updateAnalysisBadge(qint64 positionMs);
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
    void addFaceToKnownPerson(int faceId);
    void addEnrollActions(QMenu *menu, int faceId);
    void addFaceToDatabase(FaceItem face); // نسخة لأن الحوار يعيد بناء m_faces أثناء انتظاره
    void onIdentitiesMerged(int keepId, int removedId);
    void showFaceContextMenu(const QPoint &pos);
    QList<int> selectedFaceIds() const;
    void mergeFaces(const QList<int> &ids);
    void removeFaces(const QList<int> &ids);
    // لوحة الوجوه المكتشفة (mainwindow_faces.cpp)
    void setupFacePanel(QWidget *side, QVBoxLayout *layout);
    int faceTab(const FaceItem &face) const; // 1 معروف، 2 محتمل، 3 مجهول، 4 مراقَب
    void updateFaceTabs();
    void updateBulkBar();
    void setFaceView(bool grid, int scalePercent);
    int faceActions(const FaceItem &face) const;
    void onFaceAction(int id, int action);
    void confirmFaceMatch(int id);
    void rejectFaceMatch(int id);
    void toggleFaceWatch(int id);
    void addFacesToPerson(const QList<int> &ids);
    void exportFaces(const QList<int> &ids);
    void writeFaces(const QList<FaceItem> &faces, const QString &folder);
    void updateMergeSuggestions();
    void showMergeSuggestions();
    QPixmap personAvatar(const QString &personId);
    void openShotViewer(int id, qint64 positionMs = -1);
    void rebuildTimelineLanes();
    void loadVideoFrame(qint64 positionMs);
    void onVideoFrameLoaded();
    void analyzeVideoFrom(qint64 positionMs);
    void reanalyzeVideo();
    void openEnhancer();
    void openFolderScan();
    void openArchiveScan();
    void enhanceVideoFrame(qint64 positionMs);
    void showTimelineMenu(const QPoint &globalPos, qint64 positionMs, int laneId);
    void exportClip(const QList<QPair<qint64, qint64>> &segments, const QString &suggestedName);
    QString saveFolder() const; // مجلد الحفظ المقترح لما يُستخرج من الفيديو
    void saveVideoFrame(qint64 positionMs);
    void setTrimRange(qint64 startMs, qint64 endMs);
    void exportTrimmedClip();

    // أدوات التشغيل تحت الفيديو (mainwindow_playback.cpp)
    void setupPlaybackBar();
    void setupPlaybackShortcuts();
    bool fileVideoActive() const; // فيديو ملف معروض في صفحة التعرف
    void seekVideo(qint64 positionMs);
    void jumpBy(qint64 deltaMs);
    void stepFrame(int frames);
    void jumpToEvent(int direction);
    void navigateVideo(int step);
    void updateVideoNavigation();
    void checkAutoNext();
    void addMarker(qint64 positionMs);
    void editMarker(qint64 markerMs);
    void removeMarker(qint64 markerMs);
    qint64 markerNear(qint64 positionMs) const; // سالب إن لم توجد علامة قريبة
    void toggleVideoFullscreen();
    void togglePanels();
    void showShortcuts();
    void requestPreview(qint64 positionMs);
    void onPreviewLoaded();
    void syncAudio();
    void sortFaces();
    void applyFaceFilter();
    bool faceMatchesFilter(const FaceItem &face) const;
    void rebuildFaceRows();
    void updateVideoOptions();
    void openVideoFilters();
    void updateFiltersButton();
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
    qint64 m_boxesHeldUntil = 0;    // بعد تبدّل المشهد المعروض: نتائج المشهد السابق لا تُرسم حتى هذا الوقت
    int m_displayCuts = 0;          // تبدّلات المشهد في البث المعروض
    int m_workerCuts = 0;           // ما بلغه التحليل منها
    QString m_videoError;           // سبب توقف تحليل ملف الفيديو، فارغ إن انتهى طبيعياً
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
    QHash<int, QString> m_rejectedMatches; // وجه ← شخص رفض المستخدم مطابقته فلا يعود إليه
    struct MergeSuggestion
    {
        int a = 0;
        int b = 0;
        float score = 0.0f;
    };
    QList<MergeSuggestion> m_mergeSuggestions;
    QSet<quint64> m_dismissedMerges;       // أزواج قال المستخدم إنهما شخصان مختلفان
    QHash<QString, QPixmap> m_personAvatars; // صور المرشحين المصغرة
    // العرض بسرعة الفيديو الأصلية منفصلاً عن التحليل، ونتائج التحليل لكل موضع محلل لتُعرض عليه
    VideoPlayer *m_player = nullptr;
    QMap<qint64, QList<FaceItem>> m_analyzedFaces;
    // منذ متى والموضع المعروض بلا نتيجة تحليل، فلا تومض الشارة في الفجوات القصيرة
    QElapsedTimer m_uncoveredClock;
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
    bool m_tightHeight = false;
    QButtonGroup *m_navGroup = nullptr;
    QLabel *m_personCountLabel = nullptr;
    QLabel *m_photoCountLabel = nullptr;

    ImageView *m_view = nullptr;
    QListWidget *m_faceList = nullptr;
    QLabel *m_faceCount = nullptr;
    FaceDetailsPanel *m_faceDetails = nullptr;
    FaceCardDelegate *m_faceDelegate = nullptr;
    FancyButton *m_openImageButton = nullptr;
    FancyButton *m_openVideoButton = nullptr;
    FancyButton *m_cameraButton = nullptr;
    FancyButton *m_pauseButton = nullptr;
    FancyButton *m_stopButton = nullptr;
    FancyButton *m_saveFacesButton = nullptr;
    FancyButton *m_enrollFaceButton = nullptr;
    QSlider *m_thresholdSlider = nullptr;
    ToggleSwitch *m_thoroughSwitch = nullptr;
    ToggleSwitch *m_enhanceSwitch = nullptr;
    Stepper *m_frameStepSpin = nullptr;
    SegmentedControl *m_performanceCombo = nullptr;
    FancyButton *m_enhanceImageButton = nullptr;
    FancyButton *m_folderScanButton = nullptr;
    QPointer<ArchiveScanDialog> m_archiveDialog;
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
    SegmentedControl *m_faceTabs = nullptr;    // الكل، معروفون، محتملون، مجهولون، مراقَبون
    QTimer *m_faceTabsTimer = nullptr;         // تحديث عدادات التبويبات مرة كل ربع ثانية على الأكثر أثناء التحليل
    FancyButton *m_gridViewButton = nullptr;
    FancyButton *m_listViewButton = nullptr;
    QSlider *m_cardSizeSlider = nullptr;
    QWidget *m_bulkBar = nullptr;              // العمليات على عدة وجوه محددة
    QLabel *m_bulkLabel = nullptr;
    FancyButton *m_bulkMergeButton = nullptr;
    FancyButton *m_mergeSuggestButton = nullptr; // «غالباً الشخص نفسه»
    QComboBox *m_faceSort = nullptr;
    FancyButton *m_viewShotButton = nullptr;
    PlaybackBar *m_playbackBar = nullptr;
    VideoAudio *m_audio = nullptr;
    QWidget *m_recognitionHeader = nullptr;
    QLabel *m_pageTitle = nullptr;              // «التعرف على الوجوه» بلا ملف
    QLabel *m_fileTitle = nullptr;              // اسم الملف المفتوح ومعلوماته بدل العنوان
    QWidget *m_toolsSeparator = nullptr;
    SideDrawer *m_settingsDrawer = nullptr;     // درج «إعدادات التحليل»
    QWidget *m_sideCard = nullptr;
    SegmentedControl *m_laneFilter = nullptr;          // تصفية مسارات الخط الزمني حسب الحالة
    QMap<qint64, QString> m_markers;            // علامات الفيديو الحالي وملاحظاتها
    QList<qint64> m_shotChanges;                // مواضع تغيّر اللقطة التي وجدها التحليل
    QStringList m_videoSiblings;                // فيديوهات مجلد الفيديو الحالي بالترتيب الطبيعي
    QString m_pendingVideo;                     // فيديو يُفتح بعد توقف التحليل الجاري
    bool m_videoFullscreen = false;
    bool m_wasMaximized = false;
    bool m_panelsHidden = false;
    QStackedWidget *m_viewStack = nullptr;      // شاشة البداية أو العرض
    StartScreen *m_startScreen = nullptr;
    FancyButton *m_openMenuButton = nullptr;    // «فتح» يجمع أزرار الفتح بعد فتح ملف
    QWidget *m_frameStepGroup = nullptr;
    FancyButton *m_advancedButton = nullptr;    // إظهار «إعدادات التحليل» النادرة أو طيّها
    QWidget *m_statusStrip = nullptr;           // ماذا يجري، كم بقي، والخطوة المقترحة
    FloatingBar *m_zoomBar = nullptr;
    FloatingBar *m_liveBar = nullptr;
    QHBoxLayout *m_headerLayout = nullptr;
    bool m_liveControlsFloating = false;
    CollapsibleSection *m_detailsSection = nullptr;
    ProgressRing *m_statusRing = nullptr;
    QLabel *m_statusTitle = nullptr;
    ElidedLabel *m_statusDetail = nullptr;
    QLabel *m_statusHint = nullptr;
    FancyButton *m_statusAction = nullptr;
    std::function<void()> m_statusActionSlot;
    QTimer *m_statusTimer = nullptr;
    double m_analysisSpeed = 0.0;               // سرعة تحليل الفيديو بالنسبة لزمنه الحقيقي
    qint64 m_analysisRemainingMs = -1;
    double m_analysisProgress = -1.0;
    QSplitter *m_mainSplitter = nullptr;
    QSplitter *m_mediaSplitter = nullptr;
    QList<QPointer<QWidget>> m_fullscreenHidden; // ما أُخفي لملء الشاشة فيعود عند الخروج
    // معاينة الإطار عند المرور على الخط الزمني: آخر طلب فقط
    std::shared_ptr<cv::VideoCapture> m_previewCapture;
    QString m_previewPath;
    QFutureWatcher<QPair<qint64, QImage>> m_previewWatcher;
    qint64 m_previewRequestMs = -1;
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
    QFrame *m_profileTabs = nullptr;
    QWidget *m_personEmpty = nullptr;
    QWidget *m_personProfile = nullptr;
    QLabel *m_personEmptyText = nullptr;
    QStackedWidget *m_profileStack = nullptr;
    QListWidget *m_sightingList = nullptr;
    StrengthMeter *m_strengthMeter = nullptr;
    QFrame *m_duplicateBanner = nullptr;
    QLabel *m_duplicateText = nullptr;
    QString m_duplicateId;
};

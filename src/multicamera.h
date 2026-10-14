#pragma once

#include "cameras.h"
#include "facetypes.h"

#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <opencv2/core.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class CameraRecorder;
class FaceDatabase;
class FaceEngine;
class Gallery;
class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;

// مراقبة عدة كاميرات معاً في شبكة: لكل كاميرا قارئ بث ومسجّل خاص بها، وخيط تحليل واحد يمر عليها
// بالتناوب بمحرك التعرف نفسه، فيبقى الحمل ثابتاً مهما زاد عدد الكاميرات (تقل سرعة تحليل كل منها فقط).
// ظهور الأشخاص المعروفين يُسجَّل لكل كاميرا مع موضعه في تسجيلها، والمراقَبون يُنبَّه عنهم باسم الكاميرا
class MultiCameraWindow : public QWidget
{
    Q_OBJECT

public:
    // يُكتب الظهور من جديد إن عاد الشخص أمام الكاميرا نفسها بعد غياب هذه المدة
    static constexpr qint64 ReappearMs = 5 * 60 * 1000;

    MultiCameraWindow(FaceEngine *engine, FaceDatabase *database, std::function<float()> threshold, QWidget *parent = nullptr);
    ~MultiCameraWindow() override;

    // الكاميرات المختارة آخر مرة (مصادر "camera:…")
    static QStringList savedSources();
    static void setSavedSources(const QStringList &sources);

    // نافذة اختيار الكاميرات؛ تعيد المختار أو قائمة فارغة إن ألغى المستخدم
    QStringList chooseCameras();
    void start(const QStringList &sources);
    void stop();
    bool isRunning() const { return !m_feeds.empty(); }
    bool isRecording() const;
    void setWatched(const QSet<QString> &ids) { m_watched = ids; }
    void setGallery(std::shared_ptr<Gallery> gallery);

signals:
    void watchAlert(const QString &name, const QString &camera);
    void stopped();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Face
    {
        QRectF box; // بإحداثيات الإطار المحلَّل
        MatchResult match;
    };

    struct Opened
    {
        std::shared_ptr<Cameras::LiveReader> reader;
        QString error;
    };

    struct Feed
    {
        Cameras::Camera camera;
        QString source;
        std::shared_ptr<Cameras::LiveReader> reader;
        CameraRecorder *recorder = nullptr;
        bool opening = false;
        QFuture<void> pending;           // فتح الكاميرا جارٍ في الخلفية
        std::shared_ptr<Opened> opened;
        int attempts = 0;
        qint64 retryAt = 0;
        QString error;

        // يتشاركه خيط الواجهة (يضع أحدث إطار) وخيط التحليل (يأخذه)
        std::mutex mutex;
        cv::Mat latest;
        quint64 sequence = 0;
        quint64 analyzed = 0;

        // خيط الواجهة فقط
        QFrame *tile = nullptr;
        QLabel *title = nullptr;
        QLabel *view = nullptr;
        QList<Face> faces;
        QSize facesSize;
        qint64 facesAt = 0;
        QHash<QString, qint64> seen; // آخر ظهور لكل شخص أمام هذه الكاميرا
        qint64 alertUntil = 0;       // إطار أحمر حول البلاطة بعد التنبيه
        int frames = 0;
        qint64 fpsSince = 0;
        double shownFps = 0.0;
    };

    void buildTiles();
    void openFeed(int index);
    void tick();
    void render(Feed &feed, const cv::Mat &frame);
    void updateTitle(Feed &feed);
    void analysisLoop();
    void onAnalyzed(int index, quint64 generation, const cv::Mat &frame, const QList<Face> &faces);
    void toggleRecording();
    void startRecording(Feed &feed);
    void updateRecordButton();
    void showAlert(Feed &feed, const Face &face, const QImage &crop);
    // رسالة مؤقتة في شريط الحالة (خطأ تسجيل)
    void notice(const QString &text);

    FaceEngine *m_engine;
    FaceDatabase *m_database;
    std::function<float()> m_threshold;
    QSet<QString> m_watched;

    std::vector<std::unique_ptr<Feed>> m_feeds;
    quint64 m_generation = 0;
    std::thread m_analysis;
    std::atomic<bool> m_stopAnalysis{false};
    std::atomic<float> m_currentThreshold{0.5f};
    std::mutex m_galleryMutex;
    std::shared_ptr<Gallery> m_gallery;
    bool m_recording = false;
    int m_focused = -1; // كاميرا مكبّرة بنقر مزدوج، أو -1 لعرض الكل
    int m_alertCount = 0;
    QString m_notice;
    qint64 m_noticeUntil = 0;

    QTimer *m_timer = nullptr;
    QGridLayout *m_grid = nullptr;
    QWidget *m_gridHost = nullptr;
    QPushButton *m_recordButton = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_alert = nullptr;
    QLabel *m_alertAvatar = nullptr;
    QFrame *m_alertBar = nullptr;
};

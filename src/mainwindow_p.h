#pragma once

// ما تتشاركه ملفات النافذة الرئيسية المقسمة (mainwindow*.cpp): الملفات المضمَّنة والأدوات الصغيرة

#include "mainwindow.h"

#include "alertlog.h"
#include "alertsound.h"
#include "applog.h"
#include "appsettings.h"
#include "importdialog.h"
#include "harvestdialog.h"
#include "matchpage.h"
#include "personprofile.h"

#include "cameradialog.h"
#include "camerarulesdialog.h"
#include "cameras.h"
#include "camerawatch.h"
#include "carddelegates.h"
#include "controls.h"
#include "dailyreport.h"
#include "enhancedialog.h"
#include "archivedialog.h"
#include "folderscan.h"
#include "gpucheck.h"
#include "report.h"
#include "icons.h"
#include "imageio.h"
#include "imageview.h"
#include "maintenance.h"
#include "multicamera.h"
#include "persondialog.h"
#include "recorder.h"
#include "recordingsdialog.h"
#include "settingsdialog.h"
#include "shotviewer.h"
#include "timelineview.h"
#include "personmodel.h"
#include "performance.h"
#include "theme.h"
#include "videoexport.h"
#include "videofilters.h"
#include "videofiltersdialog.h"
#include "videoplayer.h"
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

namespace MainWindowHelpers {

// شخص غاب عن الكاميرا أكثر من هذا ثم عاد يُسجَّل ظهوره من جديد ويُنبَّه عنه مرة أخرى
// انقطاع الكاميرا أطول من هذا يصل إشعاراً (الانقطاع القصير يعود وحده عادةً)
constexpr qint64 CameraOfflineAlertMs = 2 * 60 * 1000;
// الكاميرا الموصولة التي انقطعت ولم تعد يُعاد تشغيلها بعد هذه المدة ما لم يفتح المستخدم شيئاً آخر
constexpr int CameraRestartMs = 30 * 1000;
// أقصى ما يكمل فيه مربع الوجه حركته المتوقعة حين يسبق العرض تحليل الإطار التالي
constexpr qint64 MaxBoxExtrapolationMs = 250;
constexpr int SightingPositionRole = Qt::UserRole + 120;

const QString ImageFilter = QStringLiteral("الصور (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff);;كل الملفات (*)");
const QString VideoFilter = QStringLiteral("الفيديو (*.mp4 *.avi *.mkv *.mov *.wmv *.webm *.m4v *.3gp);;كل الملفات (*)");

const QStringList VideoSuffixes{QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("mkv"), QStringLiteral("mov"),
                                QStringLiteral("wmv"), QStringLiteral("webm"), QStringLiteral("m4v"), QStringLiteral("3gp")};

inline bool isVideoFile(const QString &path)
{
    return VideoSuffixes.contains(QFileInfo(path).suffix().toLower());
}

inline bool isImageFile(const QString &path)
{
    return !QImageReader::imageFormat(path).isEmpty();
}

inline QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

// ينفذ عملاً ثقيلاً في الخلفية مع إبقاء الواجهة مستجيبة حتى ينتهي
template<typename Function>
inline auto runResponsive(Function function, bool allowInput = false)
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
inline QPixmap decoratedPhoto(const QImage &image, int size, bool cover, bool suspect)
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

inline QString safeFileName(QString name)
{
    static const QString invalid = QStringLiteral("\\/:*?\"<>|");
    for (QChar c : invalid)
        name.replace(c, QLatin1Char('_'));
    return name;
}

inline QFrame *card(QWidget *parent, const QString &name = QStringLiteral("card"))
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(name);
    return frame;
}

inline QLabel *label(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(objectName);
    return l;
}

inline QFrame *separator(QWidget *parent)
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

inline QString statusDescription(MatchStatus status)
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

} // namespace MainWindowHelpers

#include "clipexport.h"

#include "videoexport.h"
#include "widgets.h"

#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <opencv2/videoio.hpp>

#include <algorithm>

namespace ClipExport {

QString exportClip(const QString &source, qint64 positionMs, const QString &target, qint64 beforeMs, qint64 afterMs)
{
    if (!QFileInfo::exists(source))
        return QStringLiteral("التسجيل غير موجود: ما زال يُسجَّل (يُصدَّر بعد انتهاء جزئه كل 15 دقيقة أو إيقاف التسجيل)، أو حُذف");
    const qint64 start = std::max<qint64>(0, positionMs - beforeMs);
    qint64 end = std::max<qint64>(0, positionMs) + afterMs;
    // لا يتجاوز نهاية الملف
    {
        cv::VideoCapture capture(QFile::encodeName(source).toStdString());
        if (capture.isOpened()) {
            const double fps = capture.get(cv::CAP_PROP_FPS);
            const double frames = capture.get(cv::CAP_PROP_FRAME_COUNT);
            if (fps > 0 && frames > 0)
                end = std::min<qint64>(end, qint64(frames * 1000.0 / fps));
        }
    }
    if (end <= start)
        end = start + afterMs;
    VideoExport::Progress progress;
    return VideoExport::exportSegments(source, {{start, end}}, target, &progress, false);
}

void exportAround(QWidget *parent, const QString &source, qint64 positionMs, const QString &title)
{
    if (!QFileInfo::exists(source)) {
        Toast::show(parent, QStringLiteral("التسجيل غير موجود: ما زال يُسجَّل (يُصدَّر بعد انتهاء جزئه كل 15 دقيقة أو إيقاف التسجيل)، أو حُذف"),
                    Toast::Kind::Warning);
        return;
    }
    QString name = QStringLiteral("مقطع %1").arg(title.simplified());
    name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(parent, QStringLiteral("تصدير مقطع الظهور"),
                                                      QDir(folder).filePath(name + QStringLiteral(".mp4")),
                                                      QStringLiteral("فيديو MP4 (*.mp4)"));
    if (path.isEmpty())
        return;
    QProgressDialog dialog(QStringLiteral("جارٍ تصدير %1 ثانية حول لحظة الظهور…").arg((BeforeMs + AfterMs) / 1000), QString(), 0, 0, parent);
    dialog.setWindowTitle(QStringLiteral("تصدير مقطع"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(300);
    QFutureWatcher<QString> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([source, positionMs, path] { return exportClip(source, positionMs, path); }));
    loop.exec();
    dialog.reset();
    const QString error = watcher.result();
    if (error.isEmpty())
        Toast::show(parent, QStringLiteral("حُفظ المقطع: %1").arg(QFileInfo(path).fileName()));
    else
        Toast::show(parent, error, Toast::Kind::Warning);
}

} // namespace ClipExport

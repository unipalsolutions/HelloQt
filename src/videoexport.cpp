#include "videoexport.h"

#include "imageio.h"
#include "videofilters.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>

namespace VideoExport {

QString exportSegments(const QString &source, QList<QPair<qint64, qint64>> segments, const QString &target, Progress *progress,
                       bool filtered)
{
    cv::VideoCapture capture;
    if (!ImageIO::openVideo(capture, source))
        return QStringLiteral("تعذر فتح الفيديو");
    double fps = capture.get(cv::CAP_PROP_FPS);
    if (!(fps > 1.0 && fps < 240.0))
        fps = 25.0;
    const cv::Size sourceSize(int(capture.get(cv::CAP_PROP_FRAME_WIDTH)), int(capture.get(cv::CAP_PROP_FRAME_HEIGHT)));
    if (sourceSize.width <= 0 || sourceSize.height <= 0)
        return QStringLiteral("تعذر قراءة أبعاد الفيديو");
    const VideoFilters::Settings filters = filtered ? VideoFilters::current() : VideoFilters::Settings();
    // التدوير يبدّل العرض والارتفاع
    const cv::Size size = filters.quarterTurns % 2 == 1 ? cv::Size(sourceSize.height, sourceSize.width) : sourceSize;

    std::sort(segments.begin(), segments.end());
    qint64 total = 0;
    for (const auto &segment : segments)
        total += std::max<qint64>(0, segment.second - segment.first);
    if (total <= 0)
        return QStringLiteral("الجزء المحدد فارغ");
    if (progress)
        progress->total = total;

    // OpenCV لا يكتب إلى المسارات العربية على Windows، فيُكتب ملف مؤقت ثم يُنقل
    const QString temp = QDir::temp().filePath(QStringLiteral("malamih_%1.mp4").arg(QUuid::createUuid().toString(QUuid::Id128)));
    cv::VideoWriter writer;
    if (!writer.open(QFile::encodeName(temp).toStdString(), cv::CAP_FFMPEG, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, size)
        && !writer.open(QFile::encodeName(temp).toStdString(), cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, size))
        return QStringLiteral("تعذر إنشاء ملف الفيديو");

    qint64 written = 0;
    cv::Mat frame;
    for (const auto &segment : std::as_const(segments)) {
        capture.set(cv::CAP_PROP_POS_MSEC, double(segment.first));
        while (capture.read(frame)) {
            if (progress && progress->cancel) {
                writer.release();
                QFile::remove(temp);
                return QStringLiteral("أُلغي التصدير");
            }
            const qint64 position = qint64(capture.get(cv::CAP_PROP_POS_MSEC));
            if (position < segment.first - 1000 / fps)
                continue; // القفز قد يقف قبل الموضع المطلوب بقليل
            if (position > segment.second)
                break;
            if (frame.size() != sourceSize)
                cv::resize(frame, frame, sourceSize);
            if (!filters.isIdentity())
                frame = VideoFilters::apply(frame, filters);
            writer.write(frame);
            if (progress)
                progress->done = written + std::clamp<qint64>(position - segment.first, 0, segment.second - segment.first);
        }
        written += segment.second - segment.first;
    }
    writer.release();

    QFile::remove(target);
    if (!QFile::rename(temp, target)) {
        // نقل بين قرصين مختلفين: نسخ ثم حذف
        const bool copied = QFile::copy(temp, target);
        QFile::remove(temp);
        if (!copied)
            return QStringLiteral("تعذر حفظ الملف في المكان المختار");
    }
    return {};
}

cv::Mat frameAt(const QString &source, qint64 positionMs, bool filtered)
{
    cv::VideoCapture capture;
    cv::Mat frame;
    if (!ImageIO::openVideo(capture, source))
        return frame;
    capture.set(cv::CAP_PROP_POS_MSEC, double(positionMs));
    capture.read(frame);
    return filtered ? VideoFilters::apply(frame) : frame;
}

} // namespace VideoExport

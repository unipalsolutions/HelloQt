#include "imageio.h"

#include <QDir>
#include <QFile>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>

namespace ImageIO {

cv::Mat loadImage(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    if (data.isEmpty())
        return {};
    const cv::Mat buffer(1, int(data.size()), CV_8UC1, const_cast<char *>(data.constData()));
    return cv::imdecode(buffer, cv::IMREAD_COLOR);
}

QImage toQImage(const cv::Mat &bgr)
{
    if (bgr.empty())
        return {};
    return QImage(bgr.data, bgr.cols, bgr.rows, int(bgr.step), QImage::Format_BGR888).copy();
}

QImage cropFace(const cv::Mat &bgr, const cv::Rect2f &box, int maxSize)
{
    const float side = std::max(box.width, box.height) * 1.6f;
    const cv::Point2f center(box.x + box.width / 2.0f, box.y + box.height / 2.0f);
    cv::Rect rect(cvRound(center.x - side / 2.0f), cvRound(center.y - side / 2.0f), cvRound(side), cvRound(side));
    rect &= cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (rect.empty())
        return {};

    cv::Mat crop = bgr(rect);
    const int longest = std::max(crop.cols, crop.rows);
    if (longest > maxSize) {
        cv::Mat scaled;
        const double scale = double(maxSize) / longest;
        cv::resize(crop, scaled, cv::Size(), scale, scale, cv::INTER_AREA);
        return toQImage(scaled);
    }
    return toQImage(crop);
}

bool openVideo(cv::VideoCapture &capture, const QString &path)
{
#ifdef _WIN32
    // المسار القصير (8.3) بأحرف لاتينية يتجاوز مشكلة المسارات العربية في OpenCV
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD length = GetShortPathNameW(native.c_str(), nullptr, 0);
    if (length > 0) {
        std::wstring shortPath(length, L'\0');
        const DWORD written = GetShortPathNameW(native.c_str(), shortPath.data(), length);
        if (written > 0 && written < length) {
            shortPath.resize(written);
            if (capture.open(QString::fromStdWString(shortPath).toStdString(), cv::CAP_FFMPEG))
                return true;
        }
    }
#endif
    if (capture.open(path.toUtf8().toStdString(), cv::CAP_FFMPEG))
        return true;
    return capture.open(QFile::encodeName(path).toStdString(), cv::CAP_ANY);
}

FaceItem makeFaceItem(const cv::Mat &frame, const AnalyzedFace &face, int id)
{
    FaceItem item;
    item.id = id;
    const cv::Rect2f &b = face.detection.box;
    item.box = QRectF(b.x, b.y, b.width, b.height);
    item.crop = cropFace(frame, b);
    item.aligned = face.aligned;
    item.embedding = face.embedding;
    item.quality = face.quality;
    item.detectionScore = face.detection.score;
    return item;
}

} // namespace ImageIO

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
#include <cmath>
#include <vector>

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

namespace {

// تقدير مستوى التشويش (طريقة Immerkær): متوسط استجابة مرشح لابلاس مزدوج على التدرج الرمادي
double estimateNoise(const cv::Mat &bgr)
{
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    const cv::Mat kernel = (cv::Mat_<float>(3, 3) << 1, -2, 1, -2, 4, -2, 1, -2, 1);
    cv::Mat response;
    cv::filter2D(gray, response, CV_32F, kernel);
    const double sum = cv::sum(cv::abs(response))[0];
    return std::sqrt(CV_PI / 2.0) * sum / (6.0 * std::max(1, gray.cols - 2) * std::max(1, gray.rows - 2));
}

} // namespace

cv::Mat enhance(const cv::Mat &bgr)
{
    if (bgr.empty())
        return {};
    cv::Mat image = bgr.clone();

    // تصحيح جاما للصور المظلمة أولاً، حتى يُقاس التشويش الذي يظهر بعد الإضاءة ويُزال
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    const double brightness = cv::mean(gray)[0];
    if (brightness < 90.0) {
        cv::Mat lut(1, 256, CV_8U);
        const double gamma = std::clamp(brightness / 120.0, 0.45, 0.9);
        for (int i = 0; i < 256; ++i)
            lut.at<uchar>(i) = cv::saturate_cast<uchar>(std::pow(i / 255.0, gamma) * 255.0);
        cv::LUT(image, lut, image);
    }
    const double noise = estimateNoise(image);

    // إزالة التشويش بالدقة الأصلية قبل التكبير، وبقوة تتناسب مع مستوى التشويش، مع الحفاظ على الحواف
    if (noise > 2.0) {
        if (noise > 8.0)
            cv::medianBlur(image, image, 3);
        cv::Mat denoised;
        const double strength = std::clamp(noise * 3.0, 15.0, 90.0);
        cv::bilateralFilter(image, denoised, noise > 8.0 ? 9 : 5, strength, 5);
        image = denoised;
    }

    // الصور الصغيرة تُكبَّر حتى تظهر الوجوه البعيدة بتفاصيل كافية
    const int longSide = std::max(image.cols, image.rows);
    if (longSide < 1000) {
        const double scale = std::min(3.0, 1400.0 / longSide);
        cv::resize(image, image, cv::Size(), scale, scale, cv::INTER_CUBIC);
    }

    // موازنة الإضاءة والتباين محلياً على قناة السطوع فقط حتى لا تتغير الألوان
    cv::Mat lab;
    cv::cvtColor(image, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels;
    cv::split(lab, channels);
    cv::Mat &light = channels[0];
    cv::createCLAHE(noise > 8.0 ? 1.3 : 1.8, cv::Size(8, 8))->apply(light, light);

    // زيادة الحدة على الحواف فقط (قناع حدة موجَّه) حتى لا يتضخم التشويش في المساحات الملساء
    cv::Mat lightF, blurred, detail;
    light.convertTo(lightF, CV_32F);
    const double radius = std::max(1.0, std::max(image.cols, image.rows) / 900.0);
    cv::GaussianBlur(lightF, blurred, cv::Size(0, 0), radius);
    detail = lightF - blurred;
    cv::Mat gx, gy, edges;
    cv::Sobel(blurred, gx, CV_32F, 1, 0);
    cv::Sobel(blurred, gy, CV_32F, 0, 1);
    cv::magnitude(gx, gy, edges);
    cv::GaussianBlur(edges, edges, cv::Size(0, 0), radius * 2.0);
    double maxEdge = 0.0;
    cv::minMaxLoc(edges, nullptr, &maxEdge);
    if (maxEdge > 0.0)
        edges = cv::min(edges * (3.0 / maxEdge), 1.0);
    const double amount = std::clamp(1.2 - noise / 8.0, 0.25, 1.0);
    lightF += detail.mul(edges) * amount;
    lightF.convertTo(light, CV_8U);
    cv::merge(channels, lab);

    cv::Mat result;
    cv::cvtColor(lab, result, cv::COLOR_Lab2BGR);
    return result;
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

QImage faceCrop(const FaceItem &face)
{
    if (!face.crop.isNull())
        return face.crop;
    return toQImage(face.aligned);
}

QString formatTime(qint64 ms)
{
    const qint64 seconds = std::max<qint64>(0, ms) / 1000;
    if (seconds >= 3600)
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

FaceItem makeFaceItem(const cv::Mat &frame, const AnalyzedFace &face, int id, int photoSize)
{
    FaceItem item;
    item.id = id;
    const cv::Rect2f &b = face.detection.box;
    item.box = QRectF(b.x, b.y, b.width, b.height);
    item.crop = cropFace(frame, b, photoSize);
    item.aligned = face.aligned;
    item.embedding = face.embedding;
    item.quality = face.quality;
    item.detectionScore = face.detection.score;
    return item;
}

} // namespace ImageIO

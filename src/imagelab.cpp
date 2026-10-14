#include "imagelab.h"

#include "faceengine.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace ImageLab {

bool Settings::isIdentity() const
{
    return brightness == 0 && contrast == 0 && denoise == 0 && deblur == 0 && sharpen == 0 && upscale <= 1
        && std::abs(rotation) < 0.05 && quarterTurns % 4 == 0 && keystoneH == 0 && keystoneV == 0 && !mirror && !hasCrop();
}

bool Settings::hasCrop() const
{
    return crop.x > 1e-4 || crop.y > 1e-4 || crop.width < 1 - 1e-4 || crop.height < 1 - 1e-4;
}

Settings Settings::geometryOnly() const
{
    Settings s;
    s.rotation = rotation;
    s.quarterTurns = quarterTurns;
    s.keystoneH = keystoneH;
    s.keystoneV = keystoneV;
    s.mirror = mirror;
    s.crop = crop;
    return s;
}

double estimateNoise(const cv::Mat &bgr)
{
    // الفرق بين الصورة ونسختها المنعّمة في المناطق المستوية تقريباً
    cv::Mat gray, smooth, diff;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::medianBlur(gray, smooth, 3);
    cv::absdiff(gray, smooth, diff);
    return cv::mean(diff)[0];
}

double estimateSharpness(const cv::Mat &bgr)
{
    cv::Mat gray, lap;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Laplacian(gray, lap, CV_32F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    return stddev[0] * stddev[0];
}

namespace {

cv::Mat geometry(const cv::Mat &input, const Settings &s)
{
    cv::Mat image = input;
    if (s.mirror)
        cv::flip(image, image, 1);
    switch (((s.quarterTurns % 4) + 4) % 4) {
    case 1:
        cv::rotate(image, image, cv::ROTATE_90_COUNTERCLOCKWISE);
        break;
    case 2:
        cv::rotate(image, image, cv::ROTATE_180);
        break;
    case 3:
        cv::rotate(image, image, cv::ROTATE_90_CLOCKWISE);
        break;
    default:
        break;
    }
    const float w = float(image.cols), h = float(image.rows);
    if (s.keystoneH != 0 || s.keystoneV != 0) {
        // يضيّق الحافة الأبعد عن الكاميرا فيعيد الخطوط المتوازية إلى استقامتها
        const float kv = std::clamp(s.keystoneV, -100, 100) / 100.0f * 0.3f;
        const float kh = std::clamp(s.keystoneH, -100, 100) / 100.0f * 0.3f;
        std::array<cv::Point2f, 4> dst{cv::Point2f(0, 0), cv::Point2f(w, 0), cv::Point2f(w, h), cv::Point2f(0, h)};
        if (kv > 0) {
            dst[0].x += kv * w;
            dst[1].x -= kv * w;
        } else {
            dst[3].x -= kv * w;
            dst[2].x += kv * w;
        }
        if (kh > 0) {
            dst[0].y += kh * h;
            dst[3].y -= kh * h;
        } else {
            dst[1].y -= kh * h;
            dst[2].y += kh * h;
        }
        const std::array<cv::Point2f, 4> src{cv::Point2f(0, 0), cv::Point2f(w, 0), cv::Point2f(w, h), cv::Point2f(0, h)};
        // المعكوس: الصورة الملتقطة بميلان تُمدّ حتى تملأ الإطار
        const cv::Mat transform = cv::getPerspectiveTransform(dst.data(), src.data());
        cv::Mat warped;
        cv::warpPerspective(image, warped, transform, image.size(), cv::INTER_CUBIC, cv::BORDER_REPLICATE);
        image = warped;
    }
    if (std::abs(s.rotation) >= 0.05) {
        // تدوير مع تكبير بسيط حتى لا تظهر حواف فارغة
        const double radians = std::abs(s.rotation) * CV_PI / 180.0;
        const double zoom = std::cos(radians) + std::sin(radians) * std::max(w / h, h / w);
        const cv::Mat transform = cv::getRotationMatrix2D(cv::Point2f(w / 2, h / 2), s.rotation, zoom);
        cv::Mat rotated;
        cv::warpAffine(image, rotated, transform, image.size(), cv::INTER_CUBIC, cv::BORDER_REPLICATE);
        image = rotated;
    }
    if (s.hasCrop()) {
        const cv::Rect area(int(std::lround(s.crop.x * image.cols)), int(std::lround(s.crop.y * image.rows)),
                            std::max(1, int(std::lround(s.crop.width * image.cols))),
                            std::max(1, int(std::lround(s.crop.height * image.rows))));
        const cv::Rect clipped = area & cv::Rect(0, 0, image.cols, image.rows);
        // نسخة مستقلة حتى لا تعدّل خطوات التحسين اللاحقة الصورة الأصلية
        if (!clipped.empty())
            image = image(clipped).clone();
    }
    return image;
}

// Richardson-Lucy بنواة جاوس على قناة السطوع: يستعيد تفاصيل ضاعت بالتمويه
cv::Mat deconvolve(const cv::Mat &bgr, int strength)
{
    cv::Mat ycrcb;
    cv::cvtColor(bgr, ycrcb, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> channels;
    cv::split(ycrcb, channels);
    cv::Mat observed;
    channels[0].convertTo(observed, CV_32F, 1.0 / 255.0);
    observed += 1e-3f;
    const double sigma = 0.6 + strength / 100.0 * 2.0;
    const int iterations = 4 + strength / 10;
    cv::Mat estimate = observed.clone(), blurred, ratio;
    for (int i = 0; i < iterations; ++i) {
        cv::GaussianBlur(estimate, blurred, cv::Size(0, 0), sigma);
        cv::divide(observed, blurred + 1e-3f, ratio);
        cv::GaussianBlur(ratio, ratio, cv::Size(0, 0), sigma);
        estimate = estimate.mul(ratio);
        cv::min(estimate, 1.5f, estimate);
    }
    estimate.convertTo(channels[0], CV_8U, 255.0);
    cv::merge(channels, ycrcb);
    cv::Mat result;
    cv::cvtColor(ycrcb, result, cv::COLOR_YCrCb2BGR);
    return result;
}

} // namespace

cv::Mat apply(const cv::Mat &bgr, const Settings &s)
{
    if (bgr.empty())
        return {};
    cv::Mat image = geometry(bgr, s);
    if (image.data == bgr.data)
        image = bgr.clone();

    if (s.denoise > 0) {
        const double strength = s.denoise / 100.0;
        if (strength > 0.6)
            cv::medianBlur(image, image, 3);
        cv::Mat denoised;
        cv::bilateralFilter(image, denoised, strength > 0.5 ? 9 : 5, 10.0 + strength * 80.0, 3.0 + strength * 4.0);
        image = denoised;
    }
    if (s.upscale > 1) {
        const double scale = std::clamp(s.upscale, 1, 4);
        cv::resize(image, image, cv::Size(), scale, scale, cv::INTER_CUBIC);
    }
    if (s.deblur > 0)
        image = deconvolve(image, s.deblur);
    if (s.brightness != 0) {
        const double gamma = std::pow(2.0, -s.brightness / 100.0 * 1.3); // أقل من 1 = أفتح
        cv::Mat lut(1, 256, CV_8U);
        for (int i = 0; i < 256; ++i)
            lut.at<uchar>(i) = cv::saturate_cast<uchar>(std::pow(i / 255.0, gamma) * 255.0);
        cv::LUT(image, lut, image);
    }
    if (s.contrast > 0) {
        cv::Mat lab;
        cv::cvtColor(image, lab, cv::COLOR_BGR2Lab);
        std::vector<cv::Mat> channels;
        cv::split(lab, channels);
        cv::createCLAHE(0.5 + s.contrast / 100.0 * 3.5, cv::Size(8, 8))->apply(channels[0], channels[0]);
        cv::merge(channels, lab);
        cv::cvtColor(lab, image, cv::COLOR_Lab2BGR);
    }
    if (s.sharpen > 0) {
        cv::Mat blurred;
        const double radius = 1.0 + image.cols / 1500.0;
        cv::GaussianBlur(image, blurred, cv::Size(0, 0), radius);
        const double amount = s.sharpen / 100.0 * 1.8;
        cv::addWeighted(image, 1.0 + amount, blurred, -amount, 0, image);
    }
    return image;
}

Settings suggest(const cv::Mat &bgr)
{
    Settings s;
    if (bgr.empty())
        return s;
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Scalar mean, stddev;
    cv::meanStdDev(gray, mean, stddev);
    if (mean[0] < 85)
        s.brightness = int(std::clamp((110 - mean[0]) * 1.2, 15.0, 70.0));
    else if (mean[0] > 185)
        s.brightness = -int(std::clamp((mean[0] - 160) * 1.2, 10.0, 50.0));
    if (stddev[0] < 50)
        s.contrast = int(std::clamp((60 - stddev[0]) * 2.0, 20.0, 70.0));
    else
        s.contrast = 15;
    const double noise = estimateNoise(bgr);
    if (noise > 2.5)
        s.denoise = int(std::clamp(noise * 9.0, 20.0, 80.0));
    const int longSide = std::max(bgr.cols, bgr.rows);
    if (longSide < 700)
        s.upscale = longSide < 400 ? 3 : 2;
    const double sharpness = estimateSharpness(bgr);
    if (sharpness < 150)
        s.deblur = int(std::clamp((150 - sharpness) / 3.0, 15.0, 55.0));
    s.sharpen = s.denoise > 50 ? 20 : 35;
    return s;
}

bool levelFace(const cv::Mat &bgr, const FaceEngine *engine, double *angle)
{
    if (!engine || !engine->isLoaded() || bgr.empty())
        return false;
    const std::vector<DetectedFace> faces = engine->detect(bgr, true);
    if (faces.empty())
        return false;
    const auto largest = std::max_element(faces.begin(), faces.end(), [](const DetectedFace &a, const DetectedFace &b) {
        return a.box.area() < b.box.area();
    });
    const cv::Point2f left = largest->landmarks[0], right = largest->landmarks[1];
    *angle = std::atan2(right.y - left.y, right.x - left.x) * 180.0 / CV_PI;
    return true;
}

bool faceCrop(const cv::Mat &bgr, const FaceEngine *engine, cv::Rect2d *crop)
{
    if (!engine || !engine->isLoaded() || bgr.empty())
        return false;
    const std::vector<DetectedFace> faces = engine->detect(bgr, true);
    if (faces.empty())
        return false;
    const auto largest = std::max_element(faces.begin(), faces.end(), [](const DetectedFace &a, const DetectedFace &b) {
        return a.box.area() < b.box.area();
    });
    // الوجه في الثلث العلوي مع الكتفين، كصور الهوية
    const double w = bgr.cols, h = bgr.rows;
    double width = std::min(largest->box.width * 2.2, w);
    double height = std::min(width * 4.0 / 3.0, h);
    width = std::min(width, height * 3.0 / 4.0);
    const double cx = largest->box.x + largest->box.width / 2.0;
    const double cy = largest->box.y + largest->box.height * 0.62;
    const double x = std::clamp(cx - width / 2.0, 0.0, w - width);
    const double y = std::clamp(cy - height * 0.45, 0.0, h - height);
    *crop = cv::Rect2d(x / w, y / h, width / w, height / h);
    return true;
}

} // namespace ImageLab

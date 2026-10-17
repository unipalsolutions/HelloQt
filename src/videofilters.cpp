#include "videofilters.h"

#include "imagelab.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QStringList>

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace VideoFilters {

namespace {

QMutex &mutex()
{
    static QMutex instance;
    return instance;
}

Settings load()
{
    QSettings store;
    store.beginGroup(QStringLiteral("videoFilters"));
    return read(store);
}

Settings &storage()
{
    static Settings instance = load();
    return instance;
}

// يمدّ مدى الإضاءة بين أغمق 1% وأفتح 1% من البكسلات إلى المدى الكامل
void stretchLevels(cv::Mat &image)
{
    cv::Mat gray, small;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, small, cv::Size(160, std::max(1, 160 * gray.rows / std::max(1, gray.cols))), 0, 0, cv::INTER_AREA);
    int histogram[256] = {};
    for (int y = 0; y < small.rows; ++y)
        for (int x = 0; x < small.cols; ++x)
            ++histogram[small.at<uchar>(y, x)];
    const int total = small.rows * small.cols;
    const int cut = std::max(1, total / 100);
    int low = 0, high = 255, sum = 0;
    for (; low < 255 && (sum += histogram[low]) < cut; ++low) {}
    sum = 0;
    for (; high > 0 && (sum += histogram[high]) < cut; --high) {}
    if (high - low < 8)
        return;
    const double alpha = std::min(4.0, 255.0 / double(high - low));
    image.convertTo(image, -1, alpha, -low * alpha);
}

} // namespace

bool Settings::isIdentity() const
{
    return quarterTurns % 4 == 0 && !mirror && !flip && !deinterlace && !autoLevels && !grayscale && brightness == 0
        && contrast == 0 && denoise == 0 && sharpen == 0;
}

bool Settings::changesGeometry() const
{
    return quarterTurns % 4 != 0 || mirror || flip;
}

int Settings::activeCount() const
{
    return int(quarterTurns % 4 != 0) + int(mirror) + int(flip) + int(deinterlace) + int(autoLevels) + int(grayscale)
        + int(brightness != 0) + int(contrast != 0) + int(denoise != 0) + int(sharpen != 0) + int(minFaceSize > 0)
        + int(!skipStill); // تخطي الساكن مفعّل افتراضياً، فإيقافه هو الذي يُعد
}

QString Settings::summary(bool withAnalysis) const
{
    QStringList parts;
    if (quarterTurns % 4 != 0)
        parts << QStringLiteral("تدوير %1°").arg(quarterTurns % 4 * 90);
    if (mirror)
        parts << QStringLiteral("مرآة");
    if (flip)
        parts << QStringLiteral("قلب عمودي");
    if (deinterlace)
        parts << QStringLiteral("إزالة التشابك");
    if (autoLevels)
        parts << QStringLiteral("إضاءة تلقائية");
    if (grayscale)
        parts << QStringLiteral("رمادي");
    if (brightness != 0)
        parts << QStringLiteral("إضاءة %1%2").arg(brightness > 0 ? QStringLiteral("+") : QString()).arg(brightness);
    if (contrast != 0)
        parts << QStringLiteral("تباين %1").arg(contrast);
    if (denoise != 0)
        parts << QStringLiteral("إزالة تشويش %1").arg(denoise);
    if (sharpen != 0)
        parts << QStringLiteral("حدة %1").arg(sharpen);
    if (withAnalysis && minFaceSize > 0)
        parts << QStringLiteral("وجوه من %1 بكسل").arg(minFaceSize);
    if (withAnalysis && !skipStill)
        parts << QStringLiteral("فحص كل الإطارات");
    return parts.join(QStringLiteral(" · "));
}

bool Settings::operator==(const Settings &o) const
{
    return quarterTurns % 4 == o.quarterTurns % 4 && mirror == o.mirror && flip == o.flip && deinterlace == o.deinterlace
        && autoLevels == o.autoLevels && grayscale == o.grayscale && brightness == o.brightness && contrast == o.contrast
        && denoise == o.denoise && sharpen == o.sharpen && minFaceSize == o.minFaceSize && skipStill == o.skipStill;
}

Settings current()
{
    QMutexLocker locker(&mutex());
    return storage();
}

Settings read(const QSettings &store)
{
    Settings s;
    s.quarterTurns = std::clamp(store.value(QStringLiteral("quarterTurns"), 0).toInt(), 0, 3);
    s.mirror = store.value(QStringLiteral("mirror"), false).toBool();
    s.flip = store.value(QStringLiteral("flip"), false).toBool();
    s.deinterlace = store.value(QStringLiteral("deinterlace"), false).toBool();
    s.autoLevels = store.value(QStringLiteral("autoLevels"), false).toBool();
    s.grayscale = store.value(QStringLiteral("grayscale"), false).toBool();
    s.brightness = std::clamp(store.value(QStringLiteral("brightness"), 0).toInt(), -100, 100);
    s.contrast = std::clamp(store.value(QStringLiteral("contrast"), 0).toInt(), 0, 100);
    s.denoise = std::clamp(store.value(QStringLiteral("denoise"), 0).toInt(), 0, 100);
    s.sharpen = std::clamp(store.value(QStringLiteral("sharpen"), 0).toInt(), 0, 100);
    s.minFaceSize = std::clamp(store.value(QStringLiteral("minFaceSize"), 0).toInt(), 0, 400);
    s.skipStill = store.value(QStringLiteral("skipStill"), true).toBool();
    return s;
}

void write(QSettings &store, const Settings &s)
{
    store.setValue(QStringLiteral("quarterTurns"), s.quarterTurns % 4);
    store.setValue(QStringLiteral("mirror"), s.mirror);
    store.setValue(QStringLiteral("flip"), s.flip);
    store.setValue(QStringLiteral("deinterlace"), s.deinterlace);
    store.setValue(QStringLiteral("autoLevels"), s.autoLevels);
    store.setValue(QStringLiteral("grayscale"), s.grayscale);
    store.setValue(QStringLiteral("brightness"), s.brightness);
    store.setValue(QStringLiteral("contrast"), s.contrast);
    store.setValue(QStringLiteral("denoise"), s.denoise);
    store.setValue(QStringLiteral("sharpen"), s.sharpen);
    store.setValue(QStringLiteral("minFaceSize"), s.minFaceSize);
    store.setValue(QStringLiteral("skipStill"), s.skipStill);
}

void setCurrent(const Settings &s, bool persist)
{
    {
        QMutexLocker locker(&mutex());
        if (storage() == s)
            return;
        storage() = s;
    }
    if (!persist)
        return;
    QSettings store;
    store.beginGroup(QStringLiteral("videoFilters"));
    write(store, s);
}

cv::Mat apply(const cv::Mat &bgr, const Settings &s)
{
    if (bgr.empty() || s.isIdentity())
        return bgr;
    cv::Mat image = bgr;
    // التشابك أولاً وعلى الإطار الأصلي، لأن خطوطه أفقية في اتجاه التسجيل
    if (s.deinterlace && image.rows >= 4) {
        cv::Mat half;
        cv::resize(image, half, cv::Size(image.cols, image.rows / 2), 0, 0, cv::INTER_AREA);
        cv::Mat full; // لا يُكتب في image لأنها تشارك الإطار الأصلي بياناته
        cv::resize(half, full, bgr.size(), 0, 0, cv::INTER_LINEAR);
        image = full;
    }
    const int turns = s.quarterTurns % 4;
    if (turns != 0) {
        cv::Mat rotated;
        cv::rotate(image, rotated, turns == 1 ? cv::ROTATE_90_CLOCKWISE : turns == 2 ? cv::ROTATE_180 : cv::ROTATE_90_COUNTERCLOCKWISE);
        image = rotated;
    }
    if (s.mirror || s.flip) {
        cv::Mat flipped;
        cv::flip(image, flipped, s.mirror && s.flip ? -1 : s.mirror ? 1 : 0);
        image = flipped;
    }
    if (image.data == bgr.data)
        image = bgr.clone(); // لا تُعدَّل بيانات الإطار الأصلي
    if (s.grayscale) {
        cv::Mat gray;
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        cv::cvtColor(gray, image, cv::COLOR_GRAY2BGR);
    }
    if (s.autoLevels)
        stretchLevels(image);
    if (s.brightness != 0 || s.contrast != 0 || s.denoise != 0 || s.sharpen != 0) {
        ImageLab::Settings tone;
        tone.brightness = s.brightness;
        tone.contrast = s.contrast;
        tone.denoise = s.denoise;
        tone.sharpen = s.sharpen;
        image = ImageLab::apply(image, tone);
    }
    return image;
}

Settings preset(Preset preset, const Settings &base)
{
    Settings s = base;
    // الإعداد الجاهز يضبط تحسين الصورة فقط، ويحافظ على الاتجاه وخيارات التحليل
    s.deinterlace = s.autoLevels = s.grayscale = false;
    s.brightness = s.contrast = s.denoise = s.sharpen = 0;
    switch (preset) {
    case Preset::None:
        break;
    case Preset::Night:
        s.autoLevels = true;
        s.brightness = 35;
        s.contrast = 40;
        s.denoise = 45;
        break;
    case Preset::OldDvr:
        s.deinterlace = true;
        s.denoise = 30;
        s.sharpen = 35;
        s.contrast = 25;
        break;
    }
    return s;
}

} // namespace VideoFilters

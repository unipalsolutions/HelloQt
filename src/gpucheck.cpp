#include "gpucheck.h"

#include "faceengine.h"
#include "imageio.h"

#include <QElapsedTimer>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <random>

namespace GpuCheck {

namespace {

constexpr int Rounds = 5;

// إطار صناعي ثابت بحجم إطار كاميرا: تدرجات ودوائر وضوضاء، فتعمل النماذج عليه كما على صورة حقيقية
cv::Mat syntheticFrame()
{
    cv::Mat frame(720, 1280, CV_8UC3);
    std::mt19937 random(11);
    std::uniform_int_distribution<int> noise(0, 40);
    for (int y = 0; y < frame.rows; ++y) {
        auto *row = frame.ptr<cv::Vec3b>(y);
        for (int x = 0; x < frame.cols; ++x)
            row[x] = cv::Vec3b(uchar((x / 5 + noise(random)) % 256), uchar((y / 3 + noise(random)) % 256), uchar((x + y) / 8 % 256));
    }
    for (int i = 0; i < 12; ++i)
        cv::circle(frame, cv::Point(100 + i * 95, 200 + (i % 3) * 140), 40 + i * 3, cv::Scalar(30 * i % 256, 200, 120), -1);
    return frame;
}

// مقاطع ثابتة من الإطار بحجم مدخل نموذج البصمات، لمقارنة البصمات وإن لم يكن في الصورة وجه
std::vector<cv::Mat> patches(const cv::Mat &frame)
{
    std::vector<cv::Mat> list;
    for (int i = 0; i < 4; ++i) {
        const int size = std::min(frame.cols, frame.rows) / 2;
        const cv::Rect rect((frame.cols - size) * i / 3, (frame.rows - size) * (3 - i) / 3, size, size);
        cv::Mat patch;
        cv::resize(frame(rect), patch, cv::Size(112, 112));
        list.push_back(patch);
    }
    return list;
}

struct Measure
{
    double ms = 0.0;
    int faces = 0;
    std::vector<std::vector<float>> facePrints; // بصمات الوجوه المكشوفة
    std::vector<std::vector<float>> crops;  // بصمات المقاطع الثابتة
};

Measure measure(const FaceEngine &engine, const cv::Mat &frame, const std::vector<cv::Mat> &crops)
{
    Measure result;
    // التشغيل الأول يهيئ كرت الشاشة (أبطأ كثيراً) فلا يُحسب
    engine.analyze(frame, false);
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < Rounds; ++i)
        result.faces = int(engine.analyze(frame, false).size());
    result.ms = double(timer.nsecsElapsed()) / 1e6 / Rounds;
    for (const AnalyzedFace &face : engine.analyze(frame, false))
        result.facePrints.push_back(face.embedding);
    for (const cv::Mat &crop : crops)
        result.crops.push_back(engine.embed(crop));
    return result;
}

} // namespace

QString Result::summary() const
{
    if (gpu.isEmpty())
        return QStringLiteral("لا يوجد كرت شاشة مناسب؛ التحليل على المعالج (%1 ملي ثانية للإطار)").arg(cpuMs, 0, 'f', 0);
    if (!gpuOk)
        return QStringLiteral("كرت الشاشة %1 لا يعمل مع البرنامج: %2. التحليل على المعالج").arg(gpu, error);
    QString text = QStringLiteral("%1: %2 ملي ثانية للإطار مقابل %3 على المعالج (%4 مرة)")
                       .arg(gpu)
                       .arg(gpuMs, 0, 'f', 0)
                       .arg(cpuMs, 0, 'f', 0)
                       .arg(gpuMs > 0 ? cpuMs / gpuMs : 0.0, 0, 'f', 1);
    if (reliable())
        text += QStringLiteral(" — البصمات متطابقة (%1%)، فكرت الشاشة موثوق").arg(qRound(similarity * 100));
    else
        text += QStringLiteral(" — تحذير: نتائجه تختلف عن المعالج (تطابق البصمات %1%، الوجوه %2 مقابل %3)؛ يُنصح بإيقافه أو تحديث تعريفه")
                    .arg(qRound(similarity * 100))
                    .arg(gpuFaces)
                    .arg(faces);
    return text;
}

Result run(const QString &modelsDir, const QString &image)
{
    Result result;
    result.gpu = FaceEngine::availableGpu();
    cv::Mat frame;
    if (!image.isEmpty())
        frame = ImageIO::loadImage(image);
    if (frame.empty())
        frame = syntheticFrame();
    const std::vector<cv::Mat> crops = patches(frame);

    Measure cpu;
    {
        FaceEngine engine;
        QString error;
        result.cpuOk = engine.load(modelsDir, &error, FaceEngine::Device::Cpu);
        if (!result.cpuOk) {
            result.error = error;
            return result;
        }
        cpu = measure(engine, frame, crops);
        result.cpuMs = cpu.ms;
        result.faces = cpu.faces;
    }
    if (result.gpu.isEmpty())
        return result;

    FaceEngine engine;
    QString error;
    result.gpuOk = engine.load(modelsDir, &error, FaceEngine::Device::Gpu);
    if (!result.gpuOk) {
        result.error = error;
        return result;
    }
    try {
        const Measure gpu = measure(engine, frame, crops);
        result.gpuMs = gpu.ms;
        result.gpuFaces = gpu.faces;
        const auto similarity = [](const std::vector<float> &a, const std::vector<float> &b) {
            return a.size() == b.size() && !a.empty() ? FaceEngine::dot(a.data(), b.data(), a.size()) : 0.0f;
        };
        result.similarity = gpu.crops.size() == cpu.crops.size() ? 1.0f : 0.0f;
        for (size_t i = 0; i < std::min(gpu.crops.size(), cpu.crops.size()); ++i)
            result.similarity = std::min(result.similarity, similarity(cpu.crops[i], gpu.crops[i]));
        // ترتيب الوجوه قد يختلف بين الجهازين: كل وجه يُقارن بأقرب وجه له
        for (const std::vector<float> &face : cpu.facePrints) {
            float best = 0.0f;
            for (const std::vector<float> &other : gpu.facePrints)
                best = std::max(best, similarity(face, other));
            result.similarity = std::min(result.similarity, best);
        }
    } catch (const std::exception &e) {
        result.gpuOk = false;
        result.error = QString::fromLocal8Bit(e.what());
    }
    return result;
}

} // namespace GpuCheck

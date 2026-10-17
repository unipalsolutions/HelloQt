#include "faceengine.h"

#include "performance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#endif

#ifdef MALAMIH_DIRECTML
// من onnxruntime.dll في نسخة DirectML (dml_provider_factory.h دون الحاجة لترويسات DirectX)
extern "C" OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions *options, int device_id);
#endif

namespace {

const char *const DetectorFile = "det_10g.onnx";
const char *const RecognizerFile = "w600k_r50.onnx";

constexpr int AlignedSize = 112;

// مواضع النقاط الخمس المرجعية في مدخل ArcFace بحجم 112x112
const std::array<cv::Point2f, 5> ArcFaceTemplate = {{
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f},
}};

struct GpuAdapter
{
    int index = -1;
    QString name;
    QString driver; // إصدار تعريف الكرت، مثل 31.0.15.5222
};

// كرت شاشة حقيقي يدعم DirectX 12؛ مع كرتين (مدمج ومنفصل) يُختار صاحب الذاكرة الأكبر.
// المحوّل البرمجي (WARP و Microsoft Basic Render Driver في الأجهزة الافتراضية) أبطأ من المعالج فيُستبعد
GpuAdapter findGpu()
{
    GpuAdapter best;
#if defined(_WIN32) && defined(MALAMIH_DIRECTML)
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return best;
    SIZE_T bestMemory = 0;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        const bool ok = SUCCEEDED(adapter->GetDesc1(&desc));
        LARGE_INTEGER umd{};
        const bool hasDriver = ok && SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
        adapter->Release();
        if (!ok || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId == 0x1414)
            continue;
        if (best.index < 0 || desc.DedicatedVideoMemory > bestMemory) {
            best.index = int(i);
            best.name = QString::fromWCharArray(desc.Description).trimmed();
            best.driver = hasDriver ? QStringLiteral("%1.%2.%3.%4")
                                          .arg(HIWORD(umd.HighPart)).arg(LOWORD(umd.HighPart))
                                          .arg(HIWORD(umd.LowPart)).arg(LOWORD(umd.LowPart))
                                    : QString();
            bestMemory = desc.DedicatedVideoMemory;
        }
    }
    factory->Release();
#endif
    return best;
}

// ما يُربط به فحص الكرت: الكرت نفسه وتعريفه ونسخة ONNX Runtime، فلا يتغير بتحديث البرنامج
QString identity(const GpuAdapter &adapter)
{
    if (adapter.name.isEmpty())
        return QString();
    return QStringLiteral("%1|%2|ORT %3").arg(adapter.name, adapter.driver, QString::fromLatin1(OrtGetApiBase()->GetVersionString()));
}

std::basic_string<ORTCHAR_T> toOrtPath(const QString &path)
{
#ifdef _WIN32
    return QDir::toNativeSeparators(path).toStdWString();
#else
    return QFile::encodeName(path).toStdString();
#endif
}

// BGR uint8 بترتيب HWC إلى RGB float بترتيب NCHW مع (x - mean) * scale
std::vector<float> toBlob(const cv::Mat &bgr, float mean, float scale)
{
    const int h = bgr.rows;
    const int w = bgr.cols;
    std::vector<float> blob(size_t(3) * h * w);
    float *r = blob.data();
    float *g = r + size_t(h) * w;
    float *b = g + size_t(h) * w;
    for (int y = 0; y < h; ++y) {
        const auto *row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            b[i] = (row[x][0] - mean) * scale;
            g[i] = (row[x][1] - mean) * scale;
            r[i] = (row[x][2] - mean) * scale;
        }
    }
    return blob;
}

float iou(const cv::Rect2f &a, const cv::Rect2f &b)
{
    const float inter = (a & b).area();
    const float total = a.area() + b.area() - inter;
    return total > 0.0f ? inter / total : 0.0f;
}

std::vector<DetectedFace> nms(std::vector<DetectedFace> faces, float threshold)
{
    std::sort(faces.begin(), faces.end(),
              [](const DetectedFace &a, const DetectedFace &b) { return a.score > b.score; });
    std::vector<DetectedFace> kept;
    for (const DetectedFace &face : faces) {
        const bool overlaps = std::any_of(kept.begin(), kept.end(), [&](const DetectedFace &k) {
            return iou(k.box, face.box) > threshold;
        });
        if (!overlaps)
            kept.push_back(face);
    }
    return kept;
}

bool isPoorlyLit(const cv::Mat &bgr)
{
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Scalar mean, stddev;
    cv::meanStdDev(gray, mean, stddev);
    return mean[0] < 80.0 || mean[0] > 200.0 || stddev[0] < 35.0;
}

// تحسين التباين المحلي (CLAHE) على قناة الإضاءة فقط
cv::Mat enhance(const cv::Mat &bgr)
{
    cv::Mat lab;
    cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels;
    cv::split(lab, channels);
    cv::createCLAHE(2.5, cv::Size(8, 8))->apply(channels[0], channels[0]);
    cv::merge(channels, lab);
    cv::Mat result;
    cv::cvtColor(lab, result, cv::COLOR_Lab2BGR);
    return result;
}

// تحويل تشابه (دوران + تكبير + إزاحة) بأقل مربعات من src إلى dst
cv::Mat similarityTransform(const std::array<cv::Point2f, 5> &src, const std::array<cv::Point2f, 5> &dst)
{
    cv::Point2d srcMean, dstMean;
    for (size_t i = 0; i < src.size(); ++i) {
        srcMean += cv::Point2d(src[i]);
        dstMean += cv::Point2d(dst[i]);
    }
    srcMean /= double(src.size());
    dstMean /= double(dst.size());

    double numA = 0.0, numB = 0.0, den = 0.0;
    for (size_t i = 0; i < src.size(); ++i) {
        const cv::Point2d s = cv::Point2d(src[i]) - srcMean;
        const cv::Point2d t = cv::Point2d(dst[i]) - dstMean;
        numA += s.x * t.x + s.y * t.y;
        numB += s.x * t.y - s.y * t.x;
        den += s.x * s.x + s.y * s.y;
    }
    if (den <= 0.0)
        den = 1.0;
    const double a = numA / den;
    const double b = numB / den;
    return (cv::Mat_<double>(2, 3) << a, -b, dstMean.x - (a * srcMean.x - b * srcMean.y),
                                      b, a, dstMean.y - (b * srcMean.x + a * srcMean.y));
}

} // namespace

struct FaceEngine::Impl
{
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "Malamih"};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    // mutable: إن تعطل كرت الشاشة أثناء العمل تُستبدل الجلستان بجلستين على المعالج
    mutable std::unique_ptr<Ort::Session> detector;
    mutable std::unique_ptr<Ort::Session> recognizer;
    QString detectorPath;
    QString recognizerPath;
    std::string detectorInput;
    std::vector<std::string> detectorOutputs;
    std::string recognizerInput;
    std::string recognizerOutput;
    QString gpu; // اسم كرت الشاشة الذي تعمل عليه النماذج، فارغ للمعالج
    // DirectML لا يقبل تشغيلين متزامنين للجلسة نفسها، والمعالج يقبل
    mutable std::mutex runMutex;
    mutable std::atomic<bool> onGpu{false};
    // تبديل الجلستين أثناء العمل (المعالج ⇄ الكرت) يقفله حصرياً، وكل تشغيل للنموذج يقفله مشتركاً؛
    // وswitching يوقف التشغيلات الجديدة حتى لا يبقى التبديل منتظراً خلف خيوط لا تتوقف
    mutable std::shared_mutex sessionMutex;
    std::atomic<bool> switching{false};
    mutable QString fallbackReason; // لماذا رجع التحليل إلى المعالج، فارغ إن لم يرجع

    // يشغّل النموذج، وعلى كرت الشاشة يمنع التشغيل المتزامن؛ وإن فشل الكرت (تعطل التعريف، نفاد ذاكرته،
    // فصل الشاشة) ينتقل المحرك كله إلى المعالج ويعيد المحاولة بدل أن يتوقف التحليل أو ينهار البرنامج
    template<typename Run>
    std::vector<Ort::Value> guardedRun(Run run) const
    {
        while (switching.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::shared_lock<std::shared_mutex> sessions(sessionMutex);
        if (!onGpu.load())
            return run();
        std::lock_guard<std::mutex> lock(runMutex);
        if (onGpu.load()) {
            try {
                return run();
            } catch (const std::exception &e) {
                switchToCpu(QString::fromLocal8Bit(e.what()));
            }
        }
        return run();
    }
    void switchToCpu(const QString &reason) const;

    std::vector<DetectedFace> detectAt(const cv::Mat &bgr, int inputSize, float threshold) const;
    bool recognizerBatch = false; // هل يقبل نموذج البصمة أكثر من وجه في التشغيل الواحد

    std::vector<std::vector<float>> recognize(const std::vector<cv::Mat> &faces, std::vector<float> *norms = nullptr) const;
};

struct FaceEngine::Sessions
{
    std::unique_ptr<Ort::Session> detector;
    std::unique_ptr<Ort::Session> recognizer;
    QString gpu;
};

std::vector<DetectedFace> FaceEngine::Impl::detectAt(const cv::Mat &bgr, int inputSize, float threshold) const
{
    // تصغير/تكبير مع الحفاظ على النسبة. المدخل يُقرَّب إلى مضاعفات 32 فقط بدل مربع كامل،
    // فإطار 16:9 بحجم 640 يصبح 640x384 بدل 640x640 (حساب أقل بنحو 40%)
    const float ratio = float(bgr.rows) / float(bgr.cols);
    int width = inputSize;
    int height = std::max(1, int(inputSize * ratio));
    if (ratio > 1.0f) {
        height = inputSize;
        width = std::max(1, int(inputSize / ratio));
    }
    const float scale = float(height) / float(bgr.rows);
    const int inputWidth = (width + 31) / 32 * 32;
    const int inputHeight = (height + 31) / 32 * 32;

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(width, height), 0, 0, scale > 1.0f ? cv::INTER_CUBIC : cv::INTER_AREA);
    cv::Mat canvas(inputHeight, inputWidth, CV_8UC3, cv::Scalar::all(0));
    resized.copyTo(canvas(cv::Rect(0, 0, width, height)));

    std::vector<float> blob = toBlob(canvas, 127.5f, 1.0f / 128.0f);
    const std::array<int64_t, 4> shape{1, 3, inputHeight, inputWidth};
    Ort::Value input = Ort::Value::CreateTensor<float>(memory, blob.data(), blob.size(), shape.data(), shape.size());

    const char *inputName = detectorInput.c_str();
    std::vector<const char *> outputNames;
    for (const std::string &name : detectorOutputs)
        outputNames.push_back(name.c_str());
    std::vector<Ort::Value> outputs = guardedRun([&] {
        return detector->Run(Ort::RunOptions{nullptr}, &inputName, &input, 1, outputNames.data(), outputNames.size());
    });

    // مخرجات SCRFD: درجات ثم صناديق ثم نقاط لكل خطوة (8، 16، 32) مع مرساتين لكل خلية
    const int strides[3] = {8, 16, 32};
    std::vector<DetectedFace> faces;
    for (int level = 0; level < 3; ++level) {
        const int stride = strides[level];
        const int grid = inputWidth / stride;
        const size_t count = size_t(grid) * (inputHeight / stride) * 2;
        const Ort::Value &scoreOut = outputs[level];
        const Ort::Value &boxOut = outputs[level + 3];
        const Ort::Value &kpsOut = outputs[level + 6];
        if (scoreOut.GetTensorTypeAndShapeInfo().GetElementCount() != count
            || boxOut.GetTensorTypeAndShapeInfo().GetElementCount() != count * 4
            || kpsOut.GetTensorTypeAndShapeInfo().GetElementCount() != count * 10)
            throw std::runtime_error("Unexpected face detector output layout");

    const float *scores = scoreOut.GetTensorData<float>();
        const float *boxes = boxOut.GetTensorData<float>();
        const float *kps = kpsOut.GetTensorData<float>();
        for (size_t k = 0; k < count; ++k) {
            if (scores[k] < threshold)
                continue;
            const size_t cell = k / 2;
            const float cx = float(cell % grid) * stride;
            const float cy = float(cell / grid) * stride;
            const float *b = boxes + k * 4;
            const float x1 = (cx - b[0] * stride) / scale;
            const float y1 = (cy - b[1] * stride) / scale;
            const float x2 = (cx + b[2] * stride) / scale;
            const float y2 = (cy + b[3] * stride) / scale;

            DetectedFace face;
            face.score = scores[k];
            face.box = cv::Rect2f(x1, y1, x2 - x1, y2 - y1);
            for (int j = 0; j < 5; ++j) {
                face.landmarks[j] = cv::Point2f((cx + kps[k * 10 + j * 2] * stride) / scale,
                                                (cy + kps[k * 10 + j * 2 + 1] * stride) / scale);
            }
            faces.push_back(face);
        }
    }
    return faces;
}

std::vector<std::vector<float>> FaceEngine::Impl::recognize(const std::vector<cv::Mat> &faces, std::vector<float> *norms) const
{
    std::vector<std::vector<float>> result;
    if (faces.empty())
        return result;
    // النموذج يقبل دفعة من الوجوه في تشغيل واحد، وهذا أسرع من تشغيل منفصل لكل وجه
    const size_t step = size_t(3) * AlignedSize * AlignedSize;
    const size_t batch = recognizerBatch ? faces.size() : 1;
    for (size_t start = 0; start < faces.size(); start += batch) {
        const size_t count = std::min(batch, faces.size() - start);
        std::vector<float> blob(step * count);
        for (size_t i = 0; i < count; ++i) {
            const std::vector<float> one = toBlob(faces[start + i], 127.5f, 1.0f / 127.5f);
            std::copy(one.begin(), one.end(), blob.begin() + std::ptrdiff_t(step * i));
        }
        const std::array<int64_t, 4> shape{int64_t(count), 3, AlignedSize, AlignedSize};
        Ort::Value input = Ort::Value::CreateTensor<float>(memory, blob.data(), blob.size(), shape.data(), shape.size());

        const char *inputName = recognizerInput.c_str();
        const char *outputName = recognizerOutput.c_str();
        std::vector<Ort::Value> outputs = guardedRun([&] {
            return recognizer->Run(Ort::RunOptions{nullptr}, &inputName, &input, 1, &outputName, 1);
        });
        const float *data = outputs[0].GetTensorData<float>();
        const size_t size = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount() / count;
        for (size_t i = 0; i < count; ++i) {
            std::vector<float> embedding(data + i * size, data + (i + 1) * size);
            // طول البصمة قبل التطبيع يرتفع كلما رأى النموذج وجهاً سليماً واضحاً
            if (norms) {
                double sum = 0.0;
                for (float x : embedding)
                    sum += double(x) * x;
                norms->push_back(float(std::sqrt(sum)));
            }
            FaceEngine::normalize(embedding);
            result.push_back(std::move(embedding));
        }
    }
    return result;
}

void FaceEngine::Impl::switchToCpu(const QString &reason) const
{
    // يُستدعى وrunMutex مقفل، فلا يعمل أي خيط آخر على الجلستين أثناء استبدالهما
    qWarning("تعذر التحليل على كرت الشاشة (%s)؛ انتقل التحليل إلى المعالج", qUtf8Printable(reason));
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(Performance::threads(Performance::saved()));
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    auto cpuDetector = std::make_unique<Ort::Session>(env, toOrtPath(detectorPath).c_str(), options);
    auto cpuRecognizer = std::make_unique<Ort::Session>(env, toOrtPath(recognizerPath).c_str(), options);
    detector = std::move(cpuDetector);
    recognizer = std::move(cpuRecognizer);
    fallbackReason = reason;
    onGpu.store(false);
}

FaceEngine::FaceEngine()
    : d(std::make_unique<Impl>())
{
}

FaceEngine::~FaceEngine() = default;

QString FaceEngine::defaultModelsDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/models");
}

namespace {

Ort::SessionOptions cpuOptions()
{
    Ort::SessionOptions options;
    // يُحترم مستوى استهلاك الموارد المختار حتى لا يتجمد الجهاز أثناء التحليل
    options.SetIntraOpNumThreads(Performance::threads(Performance::saved()));
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return options;
}

} // namespace

// جلستا الكشف والبصمة على الجهاز المطلوب؛ Auto: كرت اجتاز الفحص في عملية منفصلة (انظر Performance::gpuVerified)
// ثم المعالج إن تعذر. لا تلمس الجلستين العاملتين، فيصلح بناؤهما والتحليل جارٍ
bool FaceEngine::createSessions(Device device, Sessions *out, QString *error) const
{
    try {
#ifdef MALAMIH_DIRECTML
        GpuAdapter adapter;
        if (device == Device::Gpu || (device == Device::Auto && Performance::gpuEnabled())) {
            adapter = findGpu();
            if (device == Device::Auto && !Performance::gpuVerified(identity(adapter)))
                adapter = GpuAdapter();
        }
        QString gpuError = adapter.index < 0 ? QStringLiteral("لا يوجد كرت شاشة يدعم DirectX 12") : QString();
        if (adapter.index >= 0) {
            try {
                Ort::SessionOptions gpuOptions = cpuOptions();
                // شرطان من DirectML
                gpuOptions.DisableMemPattern();
                gpuOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(gpuOptions, adapter.index));
                out->detector = std::make_unique<Ort::Session>(d->env, toOrtPath(d->detectorPath).c_str(), gpuOptions);
                out->recognizer = std::make_unique<Ort::Session>(d->env, toOrtPath(d->recognizerPath).c_str(), gpuOptions);
                out->gpu = adapter.name.isEmpty() ? QStringLiteral("كرت الشاشة") : adapter.name;
                return true;
            } catch (const std::exception &e) {
                out->detector.reset();
                out->recognizer.reset();
                gpuError = QString::fromLocal8Bit(e.what());
            }
        }
        if (device == Device::Gpu) {
            if (error)
                *error = QStringLiteral("تعذر التشغيل على كرت الشاشة: %1").arg(gpuError);
            return false;
        }
#else
        if (device == Device::Gpu) {
            if (error)
                *error = QStringLiteral("هذه النسخة تعمل على المعالج فقط");
            return false;
        }
#endif
        const Ort::SessionOptions options = cpuOptions();
        out->detector = std::make_unique<Ort::Session>(d->env, toOrtPath(d->detectorPath).c_str(), options);
        out->recognizer = std::make_unique<Ort::Session>(d->env, toOrtPath(d->recognizerPath).c_str(), options);
        out->gpu.clear();
        return true;
    } catch (const std::exception &e) {
        out->detector.reset();
        out->recognizer.reset();
        if (error)
            *error = QStringLiteral("تعذر تحميل النماذج: %1").arg(QString::fromLocal8Bit(e.what()));
        return false;
    }
}

bool FaceEngine::load(const QString &modelsDir, QString *error, Device device)
{
    const QString detectorPath = QDir(modelsDir).filePath(QString::fromLatin1(DetectorFile));
    // نموذج تعرّف بديل لأدوات التشخيص فقط (مقارنة نماذج على نفس الصور)
    const QString alternative = qEnvironmentVariable("MALAMIH_RECOGNIZER");
    const QString recognizerPath = alternative.isEmpty()
        ? QDir(modelsDir).filePath(QString::fromLatin1(RecognizerFile)) : alternative;
    for (const QString &path : {detectorPath, recognizerPath}) {
        if (!QFileInfo::exists(path)) {
            if (error)
                *error = QStringLiteral("ملف النموذج غير موجود: %1").arg(QDir::toNativeSeparators(path));
            return false;
        }
    }

    d->gpu.clear();
    d->onGpu.store(false);
    d->fallbackReason.clear();
    d->detectorPath = detectorPath;
    d->recognizerPath = recognizerPath;
    d->detector.reset();
    d->recognizer.reset();
    Sessions sessions;
    if (!createSessions(device, &sessions, error))
        return false;
    try {
        Ort::AllocatorWithDefaultOptions allocator;
        d->detectorInput = sessions.detector->GetInputNameAllocated(0, allocator).get();
        d->detectorOutputs.clear();
        for (size_t i = 0; i < sessions.detector->GetOutputCount(); ++i)
            d->detectorOutputs.emplace_back(sessions.detector->GetOutputNameAllocated(i, allocator).get());
        d->recognizerInput = sessions.recognizer->GetInputNameAllocated(0, allocator).get();
        d->recognizerOutput = sessions.recognizer->GetOutputNameAllocated(0, allocator).get();
        const std::vector<int64_t> recognizerShape =
            sessions.recognizer->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        d->recognizerBatch = !recognizerShape.empty() && recognizerShape[0] < 0;

        if (d->detectorOutputs.size() != 9)
            throw std::runtime_error("Unsupported face detector model");
    } catch (const std::exception &e) {
        if (error)
            *error = QStringLiteral("تعذر تحميل النماذج: %1").arg(QString::fromLocal8Bit(e.what()));
        return false;
    }
    d->detector = std::move(sessions.detector);
    d->recognizer = std::move(sessions.recognizer);
    d->gpu = sessions.gpu;
    d->onGpu.store(!d->gpu.isEmpty());
    return true;
}

bool FaceEngine::switchDevice(Device device, QString *error)
{
    if (!isLoaded()) {
        if (error)
            *error = QStringLiteral("النماذج لم تُحمّل بعد");
        return false;
    }
    // البناء (ثوانٍ على الكرت) والتحليل جارٍ على الجلستين القديمتين، ثم استبدال لحظي بينهما
    Sessions sessions;
    if (!createSessions(device, &sessions, error))
        return false;
    d->switching.store(true);
    {
        std::unique_lock<std::shared_mutex> lock(d->sessionMutex);
        std::swap(d->detector, sessions.detector);
        std::swap(d->recognizer, sessions.recognizer);
        d->gpu = sessions.gpu;
        d->fallbackReason.clear();
        d->onGpu.store(!d->gpu.isEmpty());
    }
    d->switching.store(false);
    // الجلستان القديمتان تُهدمان هنا خارج القفل
    return true;
}

bool FaceEngine::isLoaded() const
{
    return d->detector && d->recognizer;
}

QString FaceEngine::gpuName() const
{
    // فارغ أيضاً إن انتقل التحليل إلى المعالج بعد تعطل كرت الشاشة
    std::shared_lock<std::shared_mutex> lock(d->sessionMutex);
    return d->onGpu.load() ? d->gpu : QString();
}

QString FaceEngine::fallbackReason() const
{
    std::shared_lock<std::shared_mutex> lock(d->sessionMutex);
    return d->onGpu.load() ? QString() : d->fallbackReason;
}

QString FaceEngine::availableGpu()
{
    return findGpu().name;
}

QString FaceEngine::gpuIdentity()
{
    return identity(findGpu());
}

std::vector<DetectedFace> FaceEngine::detect(const cv::Mat &bgr, bool thorough, float threshold) const
{
    if (!isLoaded() || bgr.empty())
        return {};

    std::vector<DetectedFace> all;
    auto collect = [&all](std::vector<DetectedFace> faces, cv::Point2f offset = {}) {
        for (DetectedFace &face : faces) {
            face.box.x += offset.x;
            face.box.y += offset.y;
            for (cv::Point2f &p : face.landmarks)
                p += offset;
            all.push_back(face);
        }
    };

    collect(d->detectAt(bgr, 640, threshold));

    const bool poorlyLit = isPoorlyLit(bgr);
    cv::Mat enhanced;
    if (poorlyLit) {
        enhanced = enhance(bgr);
        collect(d->detectAt(enhanced, 640, threshold));
    }

    if (thorough) {
        // دقة أعلى تكشف الوجوه الصغيرة والبعيدة، وتكبّر الصور الصغيرة
        collect(d->detectAt(bgr, 1280, threshold));
        if (poorlyLit)
            collect(d->detectAt(enhanced, 1280, threshold));

        // الصور الكبيرة جداً تُقسَّم إلى أربعة أجزاء متداخلة حتى لا تضيع الوجوه الصغيرة
        if (std::max(bgr.cols, bgr.rows) > 1920) {
            const int tileW = int(bgr.cols * 0.6);
            const int tileH = int(bgr.rows * 0.6);
            for (int x : {0, bgr.cols - tileW}) {
                for (int y : {0, bgr.rows - tileH}) {
                    const cv::Rect tile(x, y, tileW, tileH);
                    collect(d->detectAt(bgr(tile), 1280, threshold), cv::Point2f(float(x), float(y)));
                }
            }
        }
    }

    // استبعاد الكشوفات المقطوعة عند حافة الإطار: نقاط الوجه خارج الصورة أو ربع الصندوق خارجها
    const cv::Rect2f frame(0.0f, 0.0f, float(bgr.cols), float(bgr.rows));
    auto invalid = [&frame](const DetectedFace &f) {
        if (f.box.width < 6 || f.box.height < 6)
            return true;
        if ((f.box & frame).area() < f.box.area() * 0.75f)
            return true;
        return std::any_of(f.landmarks.begin(), f.landmarks.end(),
                           [&frame](const cv::Point2f &p) { return !frame.contains(p); });
    };
    std::vector<DetectedFace> faces = nms(std::move(all), 0.4f);
    faces.erase(std::remove_if(faces.begin(), faces.end(), invalid), faces.end());
    return faces;
}

cv::Mat FaceEngine::align(const cv::Mat &bgr, const DetectedFace &face) const
{
    const cv::Mat transform = similarityTransform(face.landmarks, ArcFaceTemplate);
    const double scale = std::hypot(transform.at<double>(0, 0), transform.at<double>(1, 0));
    cv::Mat aligned;
    cv::warpAffine(bgr, aligned, transform, cv::Size(AlignedSize, AlignedSize),
                   scale > 1.0 ? cv::INTER_CUBIC : cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar::all(0));
    return aligned;
}

std::vector<float> FaceEngine::embed(const cv::Mat &aligned) const
{
    std::vector<std::vector<float>> result = embedMany({aligned});
    return result.empty() ? std::vector<float>() : std::move(result.front());
}

std::vector<std::vector<float>> FaceEngine::embedMany(const std::vector<cv::Mat> &aligned, bool mirrored) const
{
    if (!isLoaded() || aligned.empty())
        return {};

    // بصمة كل وجه = متوسط بصمته وبصمة صورته المعكوسة (وصورته المحسّنة إن كانت مظلمة)،
    // وهذا أثبت أمام الزوايا والإضاءة. كل النسخ تُحسب معاً في دفعة واحدة
    std::vector<cv::Mat> inputs;
    std::vector<size_t> owner;
    for (size_t i = 0; i < aligned.size(); ++i) {
        if (aligned[i].empty())
            continue;
        inputs.push_back(aligned[i]);
        owner.push_back(i);
        if (mirrored) {
            cv::Mat flipped;
            cv::flip(aligned[i], flipped, 1);
            inputs.push_back(flipped);
            owner.push_back(i);
        }
        if (isPoorlyLit(aligned[i])) {
            inputs.push_back(enhance(aligned[i]));
            owner.push_back(i);
        }
    }

    std::vector<std::vector<float>> result(aligned.size());
    const std::vector<std::vector<float>> embeddings = d->recognize(inputs);
    for (size_t k = 0; k < embeddings.size(); ++k) {
        std::vector<float> &sum = result[owner[k]];
        if (sum.empty())
            sum.assign(embeddings[k].size(), 0.0f);
        for (size_t j = 0; j < sum.size() && j < embeddings[k].size(); ++j)
            sum[j] += embeddings[k][j];
    }
    for (std::vector<float> &embedding : result)
        normalize(embedding);
    return result;
}

std::vector<std::vector<float>> FaceEngine::enrollmentEmbeddings(const cv::Mat &aligned) const
{
    // محاكاة لقطات منخفضة الدقة ومشوشة حتى تتطابق الصور الرديئة مع صور التسجيل
    std::vector<cv::Mat> versions{aligned};
    for (int size : {24, 40}) {
        cv::Mat scaled, restored;
        cv::resize(aligned, scaled, cv::Size(size, size), 0, 0, cv::INTER_AREA);
        cv::resize(scaled, restored, aligned.size(), 0, 0, cv::INTER_CUBIC);
        versions.push_back(restored);
    }
    cv::Mat blurred;
    cv::GaussianBlur(aligned, blurred, cv::Size(0, 0), 1.8);
    versions.push_back(blurred);
    // ضبابية حركة أفقية (كاميرا تهتز أو شخص يتحرك)
    cv::Mat kernel = cv::Mat::zeros(9, 9, CV_32F);
    kernel.row(4).setTo(1.0f / 9.0f);
    cv::Mat moving;
    cv::filter2D(aligned, moving, -1, kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
    versions.push_back(moving);
    return embedMany(versions);
}

float FaceEngine::quality(const DetectedFace &face, const cv::Mat &aligned) const
{
    const float sizeScore = std::clamp(std::min(face.box.width, face.box.height) / 80.0f, 0.0f, 1.0f);

    cv::Mat gray, laplacian;
    cv::cvtColor(aligned, gray, cv::COLOR_BGR2GRAY);
    cv::Laplacian(gray, laplacian, CV_32F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(laplacian, mean, stddev);
    const float sharpness = std::clamp(float(stddev[0] * stddev[0]) / 300.0f, 0.0f, 1.0f);

    // وجه أمامي: الأنف في منتصف المسافة بين العينين
    const auto &l = face.landmarks;
    const float eyeDistance = std::max(1.0f, float(cv::norm(l[1] - l[0])));
    const float noseOffset = std::abs(l[2].x - (l[0].x + l[1].x) / 2.0f) / eyeDistance;
    const float frontal = std::clamp(1.0f - noseOffset * 1.5f, 0.0f, 1.0f);

    return std::clamp(0.35f * sizeScore + 0.25f * sharpness + 0.2f * frontal + 0.2f * std::min(1.0f, face.score),
                      0.0f, 1.0f);
}

std::vector<AnalyzedFace> FaceEngine::analyze(const cv::Mat &bgr, bool thorough) const
{
    std::vector<AnalyzedFace> result;
    std::vector<cv::Mat> aligned;
    for (const DetectedFace &face : detect(bgr, thorough)) {
        AnalyzedFace analyzed;
        analyzed.detection = face;
        analyzed.aligned = align(bgr, face);
        analyzed.quality = quality(face, analyzed.aligned);
        aligned.push_back(analyzed.aligned);
        result.push_back(std::move(analyzed));
    }
    // الصور المزدحمة تُحسب بصماتها على دفعات حتى لا تستهلك ذاكرة كبيرة دفعة واحدة
    constexpr size_t Chunk = 16;
    for (size_t start = 0; start < aligned.size(); start += Chunk) {
        const size_t end = std::min(aligned.size(), start + Chunk);
        std::vector<std::vector<float>> embeddings =
            embedMany(std::vector<cv::Mat>(aligned.begin() + std::ptrdiff_t(start), aligned.begin() + std::ptrdiff_t(end)));
        for (size_t i = start; i < end; ++i)
            result[i].embedding = std::move(embeddings[i - start]);
    }
    return result;
}

float FaceEngine::dot(const float *a, const float *b, size_t size)
{
    // ثمانية مجاميع مستقلة تسمح للمترجم بالتنفيذ المتوازي (SIMD)
    float sum[8] = {};
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        for (int k = 0; k < 8; ++k)
            sum[k] += a[i + k] * b[i + k];
    }
    float total = ((sum[0] + sum[1]) + (sum[2] + sum[3])) + ((sum[4] + sum[5]) + (sum[6] + sum[7]));
    for (; i < size; ++i)
        total += a[i] * b[i];
    return total;
}

float FaceEngine::cosine(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.empty() || a.size() != b.size())
        return 0.0f;
    return dot(a.data(), b.data(), a.size());
}

void FaceEngine::normalize(std::vector<float> &v)
{
    double norm = 0.0;
    for (float x : v)
        norm += double(x) * x;
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        for (float &x : v)
            x = float(x / norm);
    }
}

std::vector<AnalyzedFace> FaceEngine::analyzeEnhanced(const cv::Mat &original, const cv::Mat &enhanced, bool thorough) const
{
    std::vector<AnalyzedFace> faces = analyze(enhanced, thorough);
    if (faces.empty() || original.empty())
        return faces;

    const float sx = float(original.cols) / float(enhanced.cols);
    const float sy = float(original.rows) / float(enhanced.rows);
    std::vector<cv::Mat> aligned;
    aligned.reserve(faces.size());
    for (const AnalyzedFace &face : faces) {
        DetectedFace mapped = face.detection;
        mapped.box = cv::Rect2f(mapped.box.x * sx, mapped.box.y * sy, mapped.box.width * sx, mapped.box.height * sy);
        for (cv::Point2f &point : mapped.landmarks)
            point = cv::Point2f(point.x * sx, point.y * sy);
        aligned.push_back(align(original, mapped));
    }
    const std::vector<std::vector<float>> originals = embedMany(aligned);
    for (size_t i = 0; i < faces.size() && i < originals.size(); ++i) {
        std::vector<float> &embedding = faces[i].embedding;
        for (size_t k = 0; k < embedding.size() && k < originals[i].size(); ++k)
            embedding[k] += originals[i][k];
        normalize(embedding);
    }
    return faces;
}

namespace {

// ينقل كشفاً من صورة محوّلة إلى إحداثيات الصورة الأصلية (inverse: من المحوّلة إلى الأصلية)
DetectedFace mapBack(const DetectedFace &face, const cv::Matx23d &inverse)
{
    auto map = [&inverse](const cv::Point2f &p) {
        return cv::Point2f(float(inverse(0, 0) * p.x + inverse(0, 1) * p.y + inverse(0, 2)),
                           float(inverse(1, 0) * p.x + inverse(1, 1) * p.y + inverse(1, 2)));
    };
    DetectedFace out = face;
    for (cv::Point2f &p : out.landmarks)
        p = map(p);
    const cv::Point2f corners[4] = {map({face.box.x, face.box.y}), map({face.box.x + face.box.width, face.box.y}),
                                    map({face.box.x, face.box.y + face.box.height}),
                                    map({face.box.x + face.box.width, face.box.y + face.box.height})};
    float x1 = corners[0].x, y1 = corners[0].y, x2 = x1, y2 = y1;
    for (const cv::Point2f &c : corners) {
        x1 = std::min(x1, c.x);
        y1 = std::min(y1, c.y);
        x2 = std::max(x2, c.x);
        y2 = std::max(y2, c.y);
    }
    // الصندوق المحيط بصندوق مُدار أكبر من الوجه، فيُصغَّر حول مركزه
    const float rotatedArea = (x2 - x1) * (y2 - y1);
    const float shrink = rotatedArea > 0 ? std::sqrt(std::max(0.5f, std::min(1.0f, face.box.area() * float(std::abs(cv::determinant(cv::Matx22d(inverse(0, 0), inverse(0, 1), inverse(1, 0), inverse(1, 1))))) / rotatedArea))) : 1.0f;
    const cv::Point2f center((x1 + x2) / 2, (y1 + y2) / 2);
    const float w = (x2 - x1) * shrink, h = (y2 - y1) * shrink;
    out.box = cv::Rect2f(center.x - w / 2, center.y - h / 2, w, h);
    return out;
}

// شكل منطقي لنقاط الوجه: العينان فوق الفم، والأنف بينهما، والمسافات متناسبة مع الصندوق
bool plausibleFace(const DetectedFace &face)
{
    const auto &l = face.landmarks;
    const cv::Point2f eyes = (l[0] + l[1]) / 2.0f, mouth = (l[3] + l[4]) / 2.0f;
    const float eyeDistance = float(cv::norm(l[1] - l[0]));
    const float eyeToMouth = float(cv::norm(mouth - eyes));
    const float size = std::max(face.box.width, face.box.height);
    if (eyeDistance < size * 0.18f || eyeDistance > size * 0.8f)
        return false;
    if (eyeToMouth < eyeDistance * 0.45f || eyeToMouth > eyeDistance * 2.2f)
        return false;
    // الأنف بين العينين والفم على المحور العمودي للوجه
    const cv::Point2f axis = (mouth - eyes) / std::max(1e-3f, eyeToMouth);
    const float along = (l[2] - eyes).dot(axis) / eyeToMouth;
    return along > 0.15f && along < 0.95f;
}

bool insideFrame(const DetectedFace &f, const cv::Rect2f &frame)
{
    if (f.box.width < 6 || f.box.height < 6)
        return false;
    if ((f.box & frame).area() < f.box.area() * 0.75f)
        return false;
    return std::all_of(f.landmarks.begin(), f.landmarks.end(), [&frame](const cv::Point2f &p) { return frame.contains(p); });
}

} // namespace

std::vector<AnalyzedFace> FaceEngine::analyzeRobust(const cv::Mat &bgr) const
{
    if (!isLoaded() || bgr.empty())
        return {};
    const cv::Rect2f frame(0.0f, 0.0f, float(bgr.cols), float(bgr.rows));
    std::vector<DetectedFace> all = detect(bgr, true);
    const bool poorlyLit = isPoorlyLit(bgr);
    cv::Mat lit = bgr;
    if (poorlyLit) {
        // الصورة المحروقة تُغمَّق أولاً حتى تعود تفاصيل الوجه قبل تحسين التباين
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        if (cv::mean(gray)[0] > 170.0) {
            cv::Mat lut(1, 256, CV_8U);
            for (int i = 0; i < 256; ++i)
                lut.at<uchar>(i) = cv::saturate_cast<uchar>(255.0 * std::pow(i / 255.0, 2.2));
            cv::LUT(bgr, lut, lit);
        }
        lit = enhance(lit);
    }

    // الكشوفات الضعيفة (وجه من زاوية حادة أو بعيد) تُقبل فقط إن اتفق عليها تمريران مختلفان
    constexpr float Strong = 0.45f, Weak = 0.28f;
    struct WeakFace
    {
        DetectedFace face;
        int pass;
    };
    std::vector<WeakFace> weak;
    int pass = 0;
    auto collect = [&](const std::vector<DetectedFace> &faces, const cv::Matx23d &inverse) {
        ++pass;
        for (const DetectedFace &face : faces) {
            DetectedFace mapped = mapBack(face, inverse);
            if (!insideFrame(mapped, frame) || !plausibleFace(mapped))
                continue;
            if (mapped.score >= Strong)
                all.push_back(mapped);
            else
                weak.push_back({mapped, pass});
        }
    };
    collect(d->detectAt(lit, 1280, Weak), cv::Matx23d(1, 0, 0, 0, 1, 0));

    // 1) الوجوه البعيدة في الصور الكبيرة: مربعات متداخلة بدقتها الأصلية تُكبَّر 1.5 مرة قبل الكشف
    if (std::max(bgr.cols, bgr.rows) > 1280) {
        const int tile = 640, step = 540;
        for (int y = 0;; y += step) {
            for (int x = 0;; x += step) {
                const cv::Rect r = cv::Rect(x, y, tile, tile) & cv::Rect(0, 0, bgr.cols, bgr.rows);
                if (r.width >= tile / 3 && r.height >= tile / 3)
                    collect(d->detectAt(lit(r), tile * 3 / 2, Weak), cv::Matx23d(1, 0, r.x, 0, 1, r.y));
                if (x + tile >= bgr.cols)
                    break;
            }
            if (y + tile >= bgr.rows)
                break;
        }
    }

    // 2) الكاميرا المرتفعة تضغط الوجه رأسياً؛ مدّ الصورة رأسياً يعيد نسبه فيسهل كشفه
    {
        constexpr double Stretch = 1.5;
        cv::Mat tall;
        cv::resize(lit, tall, cv::Size(), 1.0, Stretch, cv::INTER_LINEAR);
        collect(d->detectAt(tall, 1280, Weak), cv::Matx23d(1, 0, 0, 0, 1.0 / Stretch, 0));
    }

    // 3) صورة مقلوبة أو مدارة أو رأس مائل جداً: يُجرَّب فقط إن لم يوجد أي وجه
    auto rotatedPass = [&](double degrees, float threshold) {
        const cv::Point2f center(bgr.cols / 2.0f, bgr.rows / 2.0f);
        cv::Mat forward = cv::getRotationMatrix2D(center, degrees, 1.0);
        const cv::Rect2f bounds = cv::RotatedRect(center, bgr.size(), float(degrees)).boundingRect2f();
        forward.at<double>(0, 2) += bounds.width / 2.0 - center.x;
        forward.at<double>(1, 2) += bounds.height / 2.0 - center.y;
        cv::Mat rotated;
        cv::warpAffine(lit, rotated, forward, bounds.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        cv::Mat inverse;
        cv::invertAffineTransform(forward, inverse);
        collect(d->detectAt(rotated, 960, threshold), cv::Matx23d(inverse));
    };
    auto acceptWeak = [&] {
        for (size_t i = 0; i < weak.size(); ++i) {
            for (size_t j = 0; j < weak.size(); ++j) {
                if (weak[i].pass != weak[j].pass && iou(weak[i].face.box, weak[j].face.box) > 0.35f) {
                    all.push_back(weak[i].face);
                    break;
                }
            }
        }
        weak.clear();
    };
    acceptWeak();
    // إن لم يوجد وجه واضح فقد تكون الصورة مدارة أو مقلوبة أو الرأس مائلاً جداً
    auto clearFace = [&all] {
        return std::any_of(all.begin(), all.end(), [](const DetectedFace &f) { return f.score >= 0.6f; });
    };
    if (!clearFace()) {
        for (double degrees : {180.0, 90.0, -90.0})
            rotatedPass(degrees, Weak);
        acceptWeak();
    }
    if (!clearFace()) {
        for (double degrees : {30.0, -30.0, 55.0, -55.0})
            rotatedPass(degrees, Weak);
        acceptWeak();
    }

    std::vector<DetectedFace> faces = nms(std::move(all), 0.4f);
    faces.erase(std::remove_if(faces.begin(), faces.end(), [&frame](const DetectedFace &f) { return !insideFrame(f, frame); }),
                faces.end());

    // 5) اتجاه كل وجه: الكاشف قد يجد وجهاً مقلوباً أو مداراً لكن بنقاط خاطئة (العينان مكان الفم)،
    // فتُدار منطقته وحدها في الاتجاهات الأربعة
    for (DetectedFace &face : faces) {
        const float side = std::max(face.box.width, face.box.height) * 2.0f;
        const cv::Point2f center(face.box.x + face.box.width / 2, face.box.y + face.box.height / 2);
        const cv::Rect region = cv::Rect(cv::Rect2f(center.x - side / 2, center.y - side / 2, side, side))
                                & cv::Rect(0, 0, bgr.cols, bgr.rows);
        if (region.width < 12 || region.height < 12)
            continue;
        const cv::Mat crop = lit(region);
        const int input = std::clamp(int(std::max(region.width, region.height) * 1.5), 160, 480);
        auto bestIn = [&](const std::vector<DetectedFace> &found, const cv::Matx23d &inverse, DetectedFace &best) {
            float bestScore = -1.0f;
            for (const DetectedFace &f : found) {
                DetectedFace mapped = mapBack(f, inverse);
                mapped.box.x += float(region.x);
                mapped.box.y += float(region.y);
                for (cv::Point2f &p : mapped.landmarks)
                    p += cv::Point2f(float(region.x), float(region.y));
                // صندوق الكشف الخاطئ قد ينزاح عن الوجه الحقيقي، فيكفي قرب المركز وتقارب الحجم
                const cv::Point2f c(mapped.box.x + mapped.box.width / 2, mapped.box.y + mapped.box.height / 2);
                const float size = std::max(face.box.width, face.box.height);
                const float ratio = std::max(mapped.box.width, mapped.box.height) / std::max(1.0f, size);
                if (cv::norm(c - center) < size * 0.6f && ratio > 0.5f && ratio < 2.0f && mapped.score > bestScore) {
                    bestScore = mapped.score;
                    best = mapped;
                }
            }
            return bestScore;
        };
        // درجة الكاشف لا تكفي: يعطي الوجه المقلوب درجة عالية أيضاً. لذلك يُكشف الوجه في كل اتجاه،
        // ويُعتمد الاتجاه الذي يرى فيه نموذج التعرف وجهاً أوضح (أطول بصمة قبل التطبيع)
        std::vector<DetectedFace> options{face};
        for (const cv::RotateFlags flag : {cv::ROTATE_180, cv::ROTATE_90_CLOCKWISE, cv::ROTATE_90_COUNTERCLOCKWISE}) {
            cv::Mat rotated;
            cv::rotate(crop, rotated, flag);
            const double w = crop.cols, h = crop.rows;
            // من إحداثيات الصورة المدارة إلى إحداثيات المنطقة
            const cv::Matx23d inverse = flag == cv::ROTATE_180 ? cv::Matx23d(-1, 0, w, 0, -1, h)
                                      : flag == cv::ROTATE_90_CLOCKWISE ? cv::Matx23d(0, 1, 0, -1, 0, h)
                                                                        : cv::Matx23d(0, -1, w, 1, 0, 0);
            DetectedFace candidate;
            if (bestIn(d->detectAt(rotated, input, 0.3f), inverse, candidate) > 0.0f)
                options.push_back(candidate);
        }
        if (options.size() < 2)
            continue;
        std::vector<cv::Mat> crops;
        for (const DetectedFace &option : options)
            crops.push_back(align(lit, option));
        std::vector<float> norms;
        d->recognize(crops, &norms);
        if (norms.size() != options.size())
            continue;
        size_t best = 0;
        for (size_t i = 1; i < norms.size(); ++i)
            if (norms[i] > norms[best])
                best = i;
        // الاتجاه الحالي يُستبدل فقط إن كان واضح الخطأ: بصمته أقصر بكثير،
        // أو اتجاه آخر كشفه أوضح بفارق كبير وبصمته ليست أقصر
        if (best != 0 && norms[best] > norms[0] * 1.25f) {
            face = options[best];
            continue;
        }
        size_t clearest = 0;
        for (size_t i = 1; i < options.size(); ++i)
            if (options[i].score > options[clearest].score)
                clearest = i;
        if (clearest != 0 && options[clearest].score > options[0].score + 0.08f && norms[clearest] >= norms[0] * 0.97f)
            face = options[clearest];
    }

    std::vector<AnalyzedFace> result;
    std::vector<cv::Mat> aligned, alignedLit;
    for (const DetectedFace &face : faces) {
        AnalyzedFace analyzed;
        analyzed.detection = face;
        analyzed.aligned = align(bgr, face);
        analyzed.quality = quality(face, analyzed.aligned);
        aligned.push_back(analyzed.aligned);
        if (poorlyLit)
            alignedLit.push_back(align(lit, face));
        result.push_back(std::move(analyzed));
    }
    constexpr size_t Chunk = 16;
    for (size_t start = 0; start < aligned.size(); start += Chunk) {
        const size_t end = std::min(aligned.size(), start + Chunk);
        const auto slice = [&](const std::vector<cv::Mat> &v) {
            return std::vector<cv::Mat>(v.begin() + std::ptrdiff_t(start), v.begin() + std::ptrdiff_t(end));
        };
        std::vector<std::vector<float>> embeddings = embedMany(slice(aligned));
        // في الإضاءة السيئة تُدمج بصمة النسخة المحسّنة مع الأصلية
        if (poorlyLit) {
            const std::vector<std::vector<float>> extra = embedMany(slice(alignedLit));
            for (size_t i = 0; i < embeddings.size() && i < extra.size(); ++i) {
                for (size_t k = 0; k < embeddings[i].size() && k < extra[i].size(); ++k)
                    embeddings[i][k] += extra[i][k];
                normalize(embeddings[i]);
            }
        }
        for (size_t i = start; i < end; ++i)
            result[i].embedding = std::move(embeddings[i - start]);
    }
    return result;
}

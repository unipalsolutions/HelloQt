#pragma once

#include <QString>

#include <opencv2/core.hpp>

#include <array>
#include <memory>
#include <vector>

struct DetectedFace
{
    cv::Rect2f box;
    std::array<cv::Point2f, 5> landmarks; // العينان، الأنف، زاويتا الفم
    float score = 0.0f;
};

struct AnalyzedFace
{
    DetectedFace detection;
    cv::Mat aligned;              // وجه مُحاذى 112x112 بصيغة BGR
    std::vector<float> embedding; // بصمة الوجه (512 قيمة، طولها 1)
    float quality = 0.0f;         // من 0 إلى 1
};

// يكشف الوجوه ويستخرج بصماتها محلياً بالكامل باستخدام ONNX Runtime
class FaceEngine
{
public:
    FaceEngine();
    ~FaceEngine();

    static constexpr size_t EmbeddingSize = 512;

    static QString defaultModelsDir();

    bool load(const QString &modelsDir, QString *error = nullptr);
    bool isLoaded() const;

    // thorough: كشف متعدد المقاييس مع تحسين الإضاءة وتقسيم الصور الكبيرة (أدق وأبطأ)
    std::vector<DetectedFace> detect(const cv::Mat &bgr, bool thorough, float threshold = 0.45f) const;
    cv::Mat align(const cv::Mat &bgr, const DetectedFace &face) const;
    std::vector<float> embed(const cv::Mat &aligned) const;
    std::vector<std::vector<float>> embedMany(const std::vector<cv::Mat> &aligned) const;
    // بصمات إضافية لنسخ مُضعَّفة من الصورة حتى تتطابق الصور الرديئة لاحقاً
    std::vector<std::vector<float>> enrollmentEmbeddings(const cv::Mat &aligned) const;
    float quality(const DetectedFace &face, const cv::Mat &aligned) const;

    std::vector<AnalyzedFace> analyze(const cv::Mat &bgr, bool thorough) const;
    // يكشف الوجوه في النسخة المحسّنة (تظهر فيها الوجوه الصغيرة والمظلمة أوضح)، ثم يدمج بصمة كل وجه
    // من النسختين المحسّنة والأصلية حتى لا يغيّر التحسين ملامح الوجه التي يعتمد عليها التعرف
    // أقصى ما يمكن لصورة واحدة في أسوأ الظروف (يُستخدم للصور الثابتة)
    std::vector<AnalyzedFace> analyzeRobust(const cv::Mat &bgr) const;
    std::vector<AnalyzedFace> analyzeEnhanced(const cv::Mat &original, const cv::Mat &enhanced, bool thorough) const;

    static float dot(const float *a, const float *b, size_t size);
    static float cosine(const std::vector<float> &a, const std::vector<float> &b);
    static void normalize(std::vector<float> &v);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

// تبدّل اللقطة (قطع المشهد) يُعرف من تغيّر توزيع الألوان والإضاءة وترتيب الصورة فجأة بين إطارين
class SceneCutDetector
{
public:
    bool update(const cv::Mat &frame)
    {
        cv::Mat scaled;
        cv::resize(frame, scaled, cv::Size(64, 36), 0, 0, cv::INTER_AREA);
        cv::Mat hsv;
        cv::cvtColor(scaled, hsv, cv::COLOR_BGR2HSV);
        const int channels[] = {0, 1};
        const int sizes[] = {16, 8};
        const float hueRange[] = {0.0f, 180.0f};
        const float satRange[] = {0.0f, 256.0f};
        const float *ranges[] = {hueRange, satRange};
        cv::Mat hist;
        cv::calcHist(&hsv, 1, channels, cv::Mat(), hist, 2, sizes, ranges);
        cv::normalize(hist, hist, 1.0, 0.0, cv::NORM_L1);
        cv::Mat gray;
        cv::cvtColor(scaled, gray, cv::COLOR_BGR2GRAY);

        bool cut = false;
        if (!m_hist.empty()) {
            const double colors = cv::compareHist(m_hist, hist, cv::HISTCMP_BHATTACHARYYA);
            const double pixels = cv::norm(gray, m_gray, cv::NORM_L1) / double(gray.total());
            // تشابه البنية (ارتباط الصورتين بعد طرح متوسط الإضاءة): بين مشهدين معتمين متقاربي الألوان لا يكفي
            // اللون ولا فرق الإضاءة، لكن ترتيب الأشياء في الصورة يتغير كلياً. على مقطع الاختبار: كل القطعات
            // دون 0.35، وأدنى قيمة داخل اللقطة الواحدة 0.6
            cv::Mat current, previous, structure;
            gray.convertTo(current, CV_32F);
            m_gray.convertTo(previous, CV_32F);
            cv::matchTemplate(current, previous, structure, cv::TM_CCOEFF_NORMED);
            cut = colors > 0.5 || (colors > 0.3 && pixels > 35.0) || (colors > 0.25 && structure.at<float>(0, 0) < 0.35f);
        }
        m_hist = hist;
        m_gray = gray;
        return cut;
    }

    // بعد القفز إلى موضع آخر: الإطار التالي ليس استمراراً لما قبله
    void reset()
    {
        m_hist.release();
        m_gray.release();
    }

private:
    cv::Mat m_hist;
    cv::Mat m_gray;
};

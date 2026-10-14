#pragma once

#include "imagelab.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>

#include <opencv2/core.hpp>

class BeforeAfterView;
class FaceEngine;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;

// تحسين صورة يدوياً مع معاينة قبل/بعد: إضاءة، تباين، إزالة التشويش والضبابية، حدة، تكبير،
// تدوير وتسوية ميلان الوجه، وتصحيح المنظور للصور الملتقطة من زاوية مرتفعة
class EnhanceDialog : public QDialog
{
    Q_OBJECT

public:
    // useText: نص زر الاستخدام (مثل "تحليل الصورة المحسّنة")؛ فارغ = حفظ ونسخ فقط
    EnhanceDialog(const cv::Mat &image, const FaceEngine *engine, const QString &title, const QString &useText,
                  QWidget *parent = nullptr);
    ~EnhanceDialog() override;

    // الصورة بالدقة الكاملة بعد التحسين (تُحسب عند القبول)
    cv::Mat result() const { return m_result; }

private:
    struct Control
    {
        QSlider *slider = nullptr;
        QLabel *value = nullptr;
    };

    Control addSlider(class QGridLayout *grid, int row, const QString &name, const QString &tip, int minimum, int maximum);
    ImageLab::Settings settings() const;
    void setSettings(const ImageLab::Settings &settings);
    void schedulePreview();
    void renderPreview();
    cv::Mat renderFull();
    void save();
    void copy();
    void levelFace();

    cv::Mat m_source;
    cv::Mat m_preview; // نسخة مصغرة للمعاينة السريعة
    cv::Mat m_result;
    const FaceEngine *m_engine;
    BeforeAfterView *m_view;
    QLabel *m_info;
    Control m_brightness, m_contrast, m_denoise, m_deblur, m_sharpen, m_rotation, m_keystoneV, m_keystoneH;
    QComboBox *m_upscale;
    int m_quarterTurns = 0;
    bool m_mirror = false;
    bool m_updating = false;
    QTimer *m_debounce;
    QFutureWatcher<QImage> m_watcher;
    bool m_pending = false;
};

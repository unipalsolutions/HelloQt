#pragma once

#include "videofilters.h"

#include <QDialog>

#include <opencv2/core.hpp>

class QComboBox;
class QLabel;
class QSlider;
class QSpinBox;
class QTimer;
class ToggleSwitch;

// معالجات الفيديو وخيارات تحليله مع معاينة قبل/بعد على الإطار الحالي. كل تغيير يُطبَّق فوراً،
// حتى على الكاميرا أو الفيديو الجاري تحليله
class VideoFiltersDialog : public QDialog
{
    Q_OBJECT

public:
    // frame: الإطار الحالي كما في الملف (قبل المعالجات)، وقد يكون فارغاً فلا تظهر المعاينة
    explicit VideoFiltersDialog(const cv::Mat &frame, QWidget *parent = nullptr);

signals:
    void changed(); // تغيّرت VideoFilters::current()

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QSlider *addSlider(class QGridLayout *grid, int row, const QString &name, const QString &tip, int minimum, int maximum,
                       QLabel **value);
    VideoFilters::Settings settings() const;
    void setSettings(const VideoFilters::Settings &settings);
    void commit();
    void renderPreview();

    cv::Mat m_frame; // مصغّر للمعاينة السريعة
    QLabel *m_before = nullptr;
    QLabel *m_after = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_speed = nullptr;
    QTimer *m_debounce = nullptr;
    QComboBox *m_preset = nullptr;
    QSlider *m_brightness = nullptr, *m_contrast = nullptr, *m_denoise = nullptr, *m_sharpen = nullptr;
    QLabel *m_brightnessValue = nullptr, *m_contrastValue = nullptr, *m_denoiseValue = nullptr, *m_sharpenValue = nullptr;
    QLabel *m_rotationValue = nullptr;
    ToggleSwitch *m_mirror = nullptr, *m_flip = nullptr, *m_deinterlace = nullptr, *m_autoLevels = nullptr,
                 *m_grayscale = nullptr, *m_skipStill = nullptr;
    QSpinBox *m_minFace = nullptr;
    int m_quarterTurns = 0;
    bool m_updating = false;
};

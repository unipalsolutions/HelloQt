#pragma once

#include "faceengine.h"
#include "facetypes.h"

#include <QImage>
#include <QString>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace ImageIO {

// تقرأ الصورة عبر Qt حتى تعمل المسارات العربية على Windows
cv::Mat loadImage(const QString &path);
QImage toQImage(const cv::Mat &bgr);
QImage cropFace(const cv::Mat &bgr, const cv::Rect2f &box, int maxSize = 512);
bool openVideo(cv::VideoCapture &capture, const QString &path);
FaceItem makeFaceItem(const cv::Mat &frame, const AnalyzedFace &face, int id);

} // namespace ImageIO

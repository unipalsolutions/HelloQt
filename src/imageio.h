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
// تحسين الصورة وتوضيحها: تكبير الصغيرة، إزالة التشويش، موازنة الإضاءة والتباين، ثم زيادة الحدة
cv::Mat enhance(const cv::Mat &bgr);
QImage toQImage(const cv::Mat &bgr);
QImage cropFace(const cv::Mat &bgr, const cv::Rect2f &box, int maxSize = 512);
bool openVideo(cv::VideoCapture &capture, const QString &path);
// صورة الوجه المحفوظة للعرض والحفظ
QImage faceCrop(const FaceItem &face);
// زمن بصيغة 1:05 أو 1:02:05
QString formatTime(qint64 ms);
FaceItem makeFaceItem(const cv::Mat &frame, const AnalyzedFace &face, int id, int photoSize = 512);

} // namespace ImageIO

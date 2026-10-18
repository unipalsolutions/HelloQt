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
// تصغير سريع إلى size: متوسط المساحة بنسبة صحيحة ثم خطي إلى الحجم بالضبط. متوسط المساحة بنسبة كسرية
// (من 2560 إلى 1920 مثلاً) أبطأ بأربع مرات، وكان مع كل إطار من كاميرات الشبكة عالية الدقة يثقل الجهاز كله.
// التصغير الكبير جداً (صورة مصغرة للحركة) يأخذ عينة كل بضع بكسلات أولاً ثم يتوسطها، فلا يمر على الإطار كله
cv::Mat shrink(const cv::Mat &image, cv::Size size);
QImage cropFace(const cv::Mat &bgr, const cv::Rect2f &box, int maxSize = 512);
bool openVideo(cv::VideoCapture &capture, const QString &path);
// ضغط صورتي الوجه في الذاكرة، وقراءتهما سواء كانتا مضغوطتين أم لا
void pack(FaceItem &face);
QImage faceCrop(const FaceItem &face);
cv::Mat faceAligned(const FaceItem &face);
QString formatTime(qint64 ms);
// عدد المرات بصيغته العربية: مرة، مرتين، 3 مرات، 11 مرة
QString timesText(int count);

FaceItem makeFaceItem(const cv::Mat &frame, const AnalyzedFace &face, int id, int photoSize = 512);

} // namespace ImageIO

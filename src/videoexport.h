#pragma once

#include <QList>
#include <QPair>
#include <QString>

#include <opencv2/core.hpp>

#include <atomic>

namespace VideoExport {

struct Progress
{
    std::atomic<qint64> done{0};  // مللي ثانية مكتوبة
    std::atomic<qint64> total{1};
    std::atomic<bool> cancel{false};
};

// ينسخ فترات من الفيديو (بالترتيب) إلى ملف MP4 جديد. الصوت لا يُنسخ.
// يعيد رسالة خطأ، أو نصاً فارغاً عند النجاح
QString exportSegments(const QString &source, QList<QPair<qint64, qint64>> segments, const QString &target, Progress *progress);

// إطار واحد عند موضع معيّن
cv::Mat frameAt(const QString &source, qint64 positionMs);

} // namespace VideoExport

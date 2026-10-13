#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QPair>
#include <QMetaType>
#include <QRectF>
#include <QString>

#include <opencv2/core.hpp>

#include <vector>

enum class MatchStatus { Unknown, Possible, Confirmed };

struct MatchResult
{
    QString personId;
    QString name;
    float score = 0.0f;
    MatchStatus status = MatchStatus::Unknown;
};

// وجه مستخرج من صورة، أو مسار وجه واحد عبر إطارات الفيديو
struct FaceItem
{
    int id = 0;
    QRectF box;                   // بإحداثيات الإطار
    QImage crop;                  // الوجه مع هامش، للعرض والحفظ
    cv::Mat aligned;              // الوجه المُحاذى 112x112
    std::vector<float> embedding; // في الفيديو: متوسط بصمات كل الإطارات
    float quality = 0.0f;
    float detectionScore = 0.0f;
    int frames = 1;
    MatchResult match;

    // في الفيديو فقط (بالملي ثانية من بداية الفيديو)
    qint64 firstSeenMs = -1;
    qint64 lastSeenMs = -1;
    qint64 bestMs = -1;                      // موضع أوضح لقطة
    QRectF sourceBox;                        // موضع الوجه في أوضح لقطة بإحداثيات الإطار الأصلي
    QList<QPair<qint64, qint64>> appearances; // مرات الظهور المتصلة: البداية والنهاية
};

Q_DECLARE_METATYPE(FaceItem)

inline QString matchLabel(const MatchResult &match)
{
    switch (match.status) {
    case MatchStatus::Confirmed:
        return match.name;
    case MatchStatus::Possible:
        return QStringLiteral("%1؟").arg(match.name);
    case MatchStatus::Unknown:
        break;
    }
    return QStringLiteral("غير معروف");
}

inline QColor matchColor(MatchStatus status)
{
    switch (status) {
    case MatchStatus::Confirmed:
        return QColor(0x22, 0xd3, 0xee);
    case MatchStatus::Possible:
        return QColor(0x3b, 0x82, 0xf6);
    case MatchStatus::Unknown:
        break;
    }
    return QColor(0x94, 0xa3, 0xb8);
}

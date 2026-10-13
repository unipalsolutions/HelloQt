#pragma once

#include <QColor>
#include <QImage>
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
        return QColor(0x22, 0xc5, 0x5e);
    case MatchStatus::Possible:
        return QColor(0xf5, 0x9e, 0x0b);
    case MatchStatus::Unknown:
        break;
    }
    return QColor(0xf4, 0x3f, 0x5e);
}

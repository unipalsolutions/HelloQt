#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QMetaType>
#include <QPair>
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
    qint64 firstSeenMs = -1; // في الفيديو: أول وآخر ظهور بالمللي ثانية من بدايته
    qint64 lastSeenMs = -1;
    qint64 bestMs = -1;                       // في الفيديو: موضع أوضح لقطة للوجه
    QRectF sourceBox;                         // موضع الوجه في تلك اللقطة بإحداثيات الفيديو الأصلية
    QList<QPair<qint64, qint64>> appearances; // فترات ظهوره في الفيديو (بداية، نهاية)
    MatchResult match;
    // في الفيديو تُحفظ الصورتان مضغوطتين (نحو 10KB بدل 150KB) حتى تبقى الذاكرة صغيرة
    // مع آلاف الأشخاص في الفيديوهات الطويلة؛ ImageIO::faceCrop و faceAligned تعيدانهما
    QByteArray packedCrop;
    QByteArray packedAligned;
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
        return QColor(0x4f, 0xbf, 0x87);
    case MatchStatus::Possible:
        return QColor(0xf5, 0xa5, 0x24);
    case MatchStatus::Unknown:
        break;
    }
    return QColor(0xa8, 0xa2, 0x9e);
}

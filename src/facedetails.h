#pragma once

#include "facetypes.h"

#include <QPixmap>
#include <QWidget>

class QLabel;
class QToolButton;
class QVBoxLayout;
class FlowLayout;

// لوحة تفاصيل الوجه المحدد: أفضل لقطة واسمه وحالته، المرشحون من القاعدة بصورهم مع تصحيح بنقرة،
// فترات الظهور أزراراً تنقل إلى موضعها، ثم بيانات الشخص. اللوحة تعرض فقط، والنافذة تنفذ الطلبات
class FaceDetailsPanel : public QWidget
{
    Q_OBJECT

public:
    struct Candidate
    {
        QString personId;
        QString name;
        int percent = 0;
        MatchStatus status = MatchStatus::Unknown;
        QPixmap avatar;
        bool current = false; // التطابق الحالي للوجه
    };

    struct Details
    {
        int faceId = 0;
        QImage shot;
        QString title;
        QString status;
        QColor color;
        bool watched = false;
        int score = 0;
        int quality = 0;
        bool video = false;
        bool canSeek = false; // ملف فيديو يمكن الانتقال داخله
        qint64 bestMs = -1;
        QList<QPair<qint64, qint64>> appearances;
        QList<Candidate> candidates;
        bool canAssign = false;
        QList<QPair<QString, QString>> rows; // معلومات إضافية: العنوان والقيمة
    };

    explicit FaceDetailsPanel(QWidget *parent = nullptr);

    void showDetails(const Details &details);
    void showMessage(const QString &text);
    int faceId() const { return m_faceId; }

signals:
    void assignRequested(int faceId, const QString &personId);
    void otherPersonRequested(int faceId);
    void seekRequested(qint64 ms);
    void shotRequested(int faceId); // النقر على أفضل لقطة

private:
    void clearContent();
    QLabel *sectionLabel(const QString &text);

    QVBoxLayout *m_layout = nullptr;
    QWidget *m_content = nullptr;
    int m_faceId = 0;
};

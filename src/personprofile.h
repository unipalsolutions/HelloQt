#pragma once

#include "facedatabase.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

// قوة ملف الشخص: كلما زادت صوره وتنوعت ووضحت وتماسكت زادت دقة التعرف عليه
struct ProfileStrength
{
    double score = 0.0;     // 0..1
    QString level;          // ضعيف، متوسط، جيد، قوي
    QStringList tips;       // ما ينقص الملف
    QList<qint64> suspects; // صور قد تكون لشخص آخر
};

// الصورة التي يقل تشابهها مع بقية صور الشخص عن هذا الحد يُشك أنها لشخص آخر
constexpr float SuspectConsistency = 0.3f;

ProfileStrength profileStrength(const Person &person, const QHash<qint64, float> &consistency);

// مؤشر دائري لقوة الملف
class StrengthMeter : public QWidget
{
public:
    explicit StrengthMeter(QWidget *parent = nullptr);
    void setStrength(const ProfileStrength &strength);
    QSize sizeHint() const override { return {92, 108}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    ProfileStrength m_strength;
    bool m_valid = false;
};

// يصدّر ملف الشخص (بياناته وصوره وسجل ظهوره) إلى PDF
bool exportPersonPdf(const FaceDatabase &database, const QString &personId, const QString &path);

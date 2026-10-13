#pragma once

#include <QPixmap>
#include <QStyledItemDelegate>

namespace CardRole {
enum {
    Id = Qt::UserRole,
    Title = Qt::UserRole + 10,
    Subtitle,
    Status, // MatchStatus، أو -1 لبطاقة شخص
    Score,
    Avatar,
    AvatarPath, // مسار صورة تُحمَّل عند الرسم فقط، للقوائم الطويلة
    SearchText, // نص البحث (الاسم ورقم الهوية والهاتف)
};
}

QPixmap circularPixmap(const QImage &image, int size);
// صورة دائرية من ملف مع ذاكرة مؤقتة، فلا تُقرأ من القرص إلا البطاقات الظاهرة
QPixmap circularPixmapFromFile(const QString &path, int size);

// بطاقة وجه مستخرج: صورة دائرية، حالة التطابق، وشريط التشابه
class FaceCardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

// بطاقة شخص في قاعدة البيانات
class PersonCardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

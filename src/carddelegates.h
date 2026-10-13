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
};
}

QPixmap circularPixmap(const QImage &image, int size);

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

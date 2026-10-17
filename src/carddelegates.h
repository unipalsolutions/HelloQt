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
    StatusText, // نص بديل لشارة الحالة (اختياري)
    Watched,    // من قائمة المراقبة
    Actions,    // FaceAction: الأزرار السريعة الظاهرة عند المرور على البطاقة
};
}

// الأزرار السريعة على بطاقة الوجه
namespace FaceAction {
enum : int {
    Confirm = 1,  // تأكيد التطابق المحتمل
    Reject = 2,   // ليس هذا الشخص
    Add = 4,      // إضافة للقاعدة
    Shots = 8,    // اللقطات
    First = 16,   // أول ظهور
    Watch = 32,   // إضافة للمراقبة أو إلغاؤها
};
}

QPixmap circularPixmap(const QImage &image, int size);

class QAbstractItemView;

// بطاقة وجه مستخرج: صورة دائرية بأيقونة الحالة، الاسم ونسبة التطابق، وعدد الظهور ومدته.
// في القائمة البطاقة عريضة، وفي الشبكة مربعة والصورة فوق الاسم. عند المرور تظهر الأزرار السريعة
// التي يحددها CardRole::Actions، والنقر عليها يرسل actionTriggered دون فتح البطاقة
class FaceCardDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit FaceCardDelegate(QAbstractItemView *view);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    void setGrid(bool grid) { m_grid = grid; }
    bool isGrid() const { return m_grid; }
    void setScale(double scale) { m_scale = scale; }
    double scale() const { return m_scale; }

    static QString actionText(int action, const QModelIndex &index);

signals:
    void actionTriggered(int id, int action);

private:
    QList<QPair<int, QRectF>> actionRects(const QStyleOptionViewItem &option, const QModelIndex &index) const;
    void paintList(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    void paintGrid(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    void paintActions(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;

    QAbstractItemView *m_view = nullptr;
    bool m_grid = false;
    double m_scale = 1.0;
    QPoint m_mouse{-1, -1}; // موضع الفأرة في المنظر لإبراز الزر تحتها
};

// بطاقة شخص في قاعدة البيانات
class PersonCardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

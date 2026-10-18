#pragma once

#include "icons.h"

#include <QWidget>

class QAbstractButton;
class QLabel;
class QSpacerItem;
class QVBoxLayout;

// شاشة البداية في صفحة التعرف قبل فتح أي ملف: بطاقات كبيرة للصورة والفيديو والكاميرا والمجلد والأرشيف،
// آخر الملفات بصور مصغرة والكاميرات بنقرة، ومنطقة سحب وإفلات، وأهم الاختصارات
class StartScreen : public QWidget
{
    Q_OBJECT

public:
    explicit StartScreen(QWidget *parent = nullptr);

    // يقرأ آخر الملفات والكاميرات من جديد
    void refresh();
    // قبل تحميل النماذج تتعطل الأزرار وتظهر رسالة الانتظار
    void setReady(bool ready);
    // يُرسم إطار منطقة الإفلات بارزاً أثناء سحب ملف فوق النافذة
    void setDropHighlight(bool on);

signals:
    void openImageRequested();
    void openVideoRequested();
    void openCameraRequested();
    void openFolderRequested();
    void openArchiveRequested();
    void fileRequested(const QString &path);
    void cameraRequested(const QString &source);
    void shortcutsRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QAbstractButton *tile(Icons::Name icon, const QString &title, const QString &hint);
    QWidget *panel(Icons::Name icon, const QString &title, QVBoxLayout **list, QLabel **count);
    void clearList(QVBoxLayout *layout);

    QList<QAbstractButton *> m_tiles;
    QVBoxLayout *m_recentLayout = nullptr;
    QVBoxLayout *m_cameraLayout = nullptr;
    QLabel *m_recentCount = nullptr;
    QLabel *m_cameraCount = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_logo = nullptr;
    QLabel *m_hero = nullptr;
    QVBoxLayout *m_outer = nullptr;
    QList<QSpacerItem *> m_gaps;
    bool m_ready = false;
    bool m_dropHighlight = false;
};

#pragma once

#include "icons.h"

#include <QWidget>

class QLabel;
class QToolButton;
class QVBoxLayout;

// شاشة البداية في صفحة التعرف قبل فتح أي ملف: أزرار كبيرة للصورة والفيديو والكاميرا والمجلد،
// آخر الملفات والكاميرات بنقرة، ومنطقة سحب وإفلات، وأهم الاختصارات
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
    void fileRequested(const QString &path);
    void cameraRequested(const QString &source);
    void shortcutsRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QToolButton *tile(Icons::Name icon, const QString &title, const QString &hint);
    QToolButton *listButton(Icons::Name icon, const QString &text, const QString &tip);
    void clearList(QVBoxLayout *layout);

    QList<QToolButton *> m_tiles;
    QVBoxLayout *m_recentLayout = nullptr;
    QVBoxLayout *m_cameraLayout = nullptr;
    QLabel *m_status = nullptr;
    bool m_ready = false;
    bool m_dropHighlight = false;
};

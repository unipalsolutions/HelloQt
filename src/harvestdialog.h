#pragma once

#include "facetypes.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QList>
#include <QPair>

#include <atomic>
#include <memory>
#include <vector>

class FaceEngine;
class FancyButton;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QTimer;

// لقطة مرشحة لإضافتها إلى صور الشخص
struct HarvestShot
{
    FaceItem face;
    qint64 positionMs = 0;
    float similarity = 0.0f;
};

struct HarvestProgress
{
    std::atomic<int> done{0};
    std::atomic<int> total{1};
    std::atomic<int> found{0};
    std::atomic<bool> cancel{false};
};

// يبحث عن شخص في فيديو (كله أو فترات ظهوره فقط) ويقترح أوضح لقطاته وأكثرها تنوعاً
// (زوايا وإضاءات مختلفة) لإضافتها إلى صوره، فيصبح التعرف عليه لاحقاً أسهل وأدق
class HarvestDialog : public QDialog
{
    Q_OBJECT

public:
    HarvestDialog(const FaceEngine *engine, const QString &videoPath, const std::vector<float> &target,
                  const QList<QPair<qint64, qint64>> &ranges, float threshold, const QString &personName,
                  QWidget *parent = nullptr);
    ~HarvestDialog() override;

    QList<FaceItem> selectedFaces() const;

protected:
    void reject() override;

private:
    void onFinished();
    void updateSelection();
    void setChecked(QListWidgetItem *item, bool checked);

    std::shared_ptr<HarvestProgress> m_progress;
    QFutureWatcher<QList<HarvestShot>> m_watcher;
    QList<HarvestShot> m_shots;
    QLabel *m_status;
    QProgressBar *m_bar;
    QListWidget *m_grid;
    FancyButton *m_add;
    FancyButton *m_selectAll;
    FancyButton *m_selectNone;
    QTimer *m_timer;
};

#pragma once

#include "facetypes.h"

#include <QFutureWatcher>
#include <QImage>
#include <QList>
#include <QString>
#include <QWidget>

#include <array>
#include <functional>

class FaceDatabase;
class FaceEngine;
class FaceSlot;
class FancyButton;
class QLabel;
class QListWidget;
class SimilarityGauge;
class AnimatedStack;

struct MatchImage
{
    QString path;
    QImage image;
    QList<FaceItem> faces;
    QString error;
};

// صفحة المطابقة: مقارنة وجهين من صورتين مع نسبة التشابه والحكم، والبحث في القاعدة بصورة
// لعرض أقرب الأشخاص إليها بنسبهم مع إمكانية إضافة الصورة لملف أحدهم لتقوية التعرف عليه
class MatchPage : public QWidget
{
    Q_OBJECT

public:
    MatchPage(const FaceEngine *engine, FaceDatabase *database, std::function<float()> threshold, QWidget *parent = nullptr);
    ~MatchPage() override;

    void setReady(bool ready);
    // تغيّر حد التعرف أو القاعدة: يعيد حساب النتائج
    void refresh();
    // يفتح صورة في أول خانة فارغة من القسم الظاهر (للصق والسحب على النافذة)
    void openFile(const QString &path);
    void setCompact(bool compact);

signals:
    void openPersonRequested(const QString &personId);
    void enrollRequested(const QString &personId, const FaceItem &face);

private:
    enum Slot { First, Second, Query, SlotCount };

    void browse(int slot);
    void load(int slot, const QString &path);
    void onLoaded(int slot);
    void selectFace(int slot, int index);
    void updateComparison();
    void updateSearch();
    void clearComparison();
    const FaceItem *selectedFace(int slot) const;

    const FaceEngine *m_engine;
    FaceDatabase *m_database;
    std::function<float()> m_threshold;
    bool m_ready = false;
    QString m_lastDirectory;

    std::array<FaceSlot *, SlotCount> m_slots{};
    std::array<QFutureWatcher<MatchImage> *, SlotCount> m_watchers{};
    std::array<MatchImage, SlotCount> m_images;
    std::array<int, SlotCount> m_selected{{-1, -1, -1}};

    AnimatedStack *m_stack = nullptr;
    QList<FancyButton *> m_tabs;
    QLabel *m_subtitle = nullptr;
    SimilarityGauge *m_gauge = nullptr;
    QLabel *m_avatarA = nullptr;
    QLabel *m_avatarB = nullptr;
    QLabel *m_verdict = nullptr;
    QLabel *m_verdictDetail = nullptr;
    QListWidget *m_results = nullptr;
    QLabel *m_resultsHint = nullptr;
    FancyButton *m_openPerson = nullptr;
    FancyButton *m_enroll = nullptr;
};

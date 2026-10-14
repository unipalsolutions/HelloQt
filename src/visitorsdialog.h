#pragma once

#include "facedatabase.h"

#include <QDialog>
#include <QList>

class FancyButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;

// الزوار المجهولون: وجوه غير مسجلة ظهرت أمام الكاميرات، كل وجه تحت رقم واحد مهما تكرر ظهوره،
// مع مرات ظهوره ولقطاتها ومواضعها في التسجيلات. يُسمّى الزائر بإضافته إلى شخص جديد أو موجود
// فينتقل ظهوره كله إلى سجل ذلك الشخص
class VisitorsDialog : public QDialog
{
    Q_OBJECT

public:
    VisitorsDialog(FaceDatabase *database, QWidget *parent = nullptr);

    void reload();

signals:
    void openFileRequested(const QString &path, qint64 positionMs);
    // تسمية الزائر (تختار النافذة الرئيسية الشخص)
    void assignRequested(qint64 visitorId);

private:
    QDateTime since() const;
    qint64 currentVisitor() const;
    const Sighting *currentSighting() const;
    void showSightings();
    void updateButtons();
    void openSighting();

    FaceDatabase *m_database;
    QComboBox *m_period;
    QCheckBox *m_repeated;
    QCheckBox *m_enabled;
    QLabel *m_summary;
    QListWidget *m_list;
    QLabel *m_details;
    QListWidget *m_sightings;
    FancyButton *m_assign;
    FancyButton *m_remove;
    FancyButton *m_open;
    FancyButton *m_snapshot;
    QList<Visitor> m_visitors;
    QList<Sighting> m_current;
};

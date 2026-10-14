#pragma once

#include "facedatabase.h"

#include <QDialog>
#include <QList>

class FancyButton;
class QComboBox;
class QLabel;
class QListWidget;

// سجل التنبيهات: كل مرات ظهور الأشخاص المراقَبين (أو كل الأشخاص) في الصور والفيديوهات والكاميرا،
// الأحدث أولاً، مع فتح الملف الذي ظهروا فيه أو ملف الشخص، وتصدير السجل إلى CSV
class AlertLogDialog : public QDialog
{
    Q_OBJECT

public:
    AlertLogDialog(const FaceDatabase *database, QWidget *parent = nullptr);

signals:
    void openFileRequested(const QString &path, qint64 positionMs);
    void showPersonRequested(const QString &personId);

private:
    QDateTime since() const;
    void reload();
    void updateButtons();
    void openCurrent();
    void exportCsv();
    const SightingEntry *currentEntry() const;

    const FaceDatabase *m_database;
    QComboBox *m_scope;
    QComboBox *m_period;
    QLabel *m_summary;
    QListWidget *m_list;
    FancyButton *m_open;
    FancyButton *m_person;
    FancyButton *m_export;
    QList<SightingEntry> m_entries;
};

#pragma once

#include "facedatabase.h"

#include <QAbstractListModel>
#include <QCache>
#include <QPixmap>

// قائمة الأشخاص تُقرأ من القاعدة صفحةً صفحة عند التمرير، فتبقى فورية حتى مع ملايين الأشخاص.
// الأحدث أولاً، والصور الرمزية تُحمَّل للبطاقات الظاهرة فقط.
class PersonListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit PersonListModel(const FaceDatabase *database, QObject *parent = nullptr);

    void setFilter(const QString &filter);
    QString filter() const { return m_filter; }
    void reload();
    void refreshPerson(const QString &id);
    int rowOf(const QString &id) const;
    QString idAt(int row) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

private:
    const FaceDatabase *m_database;
    QString m_filter;
    QList<Person> m_rows;
    bool m_more = true;
    mutable QCache<qint64, QPixmap> m_avatars;
};

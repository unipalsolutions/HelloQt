#include "personmodel.h"

#include "carddelegates.h"

namespace {

constexpr int PageSize = 100;
constexpr int AvatarSize = 46;

} // namespace

PersonListModel::PersonListModel(const FaceDatabase *database, QObject *parent)
    : QAbstractListModel(parent)
    , m_database(database)
    , m_avatars(4000)
{
}

void PersonListModel::setFilter(const QString &filter)
{
    if (filter.trimmed() == m_filter)
        return;
    m_filter = filter.trimmed();
    reload();
}

void PersonListModel::reload()
{
    beginResetModel();
    m_rows.clear();
    m_more = true;
    endResetModel();
    if (canFetchMore(QModelIndex()))
        fetchMore(QModelIndex());
}

void PersonListModel::refreshPerson(const QString &id)
{
    const int row = rowOf(id);
    if (row < 0)
        return;
    const Person updated = m_database->person(id);
    if (updated.id.isEmpty())
        return;
    Person &person = m_rows[row];
    person.name = updated.name;
    person.nationalId = updated.nationalId;
    person.phone = updated.phone;
    person.photoCount = updated.photoCount;
    person.coverPhoto = updated.coverPhoto;
    emit dataChanged(index(row), index(row));
}

int PersonListModel::rowOf(const QString &id) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id == id)
            return i;
    }
    return -1;
}

QString PersonListModel::idAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).id : QString();
}

int PersonListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

bool PersonListModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && m_more;
}

void PersonListModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid() || !m_more)
        return;
    const qint64 before = m_rows.isEmpty() ? 0 : m_rows.last().id.toLongLong();
    const QList<Person> page = m_database->findPersons(m_filter, before, PageSize);
    m_more = page.size() == PageSize;
    if (page.isEmpty())
        return;
    beginInsertRows(QModelIndex(), int(m_rows.size()), int(m_rows.size() + page.size() - 1));
    m_rows.append(page);
    endInsertRows();
}

QVariant PersonListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Person &person = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return person.nationalId.isEmpty() ? person.name : QStringLiteral("%1 (%2)").arg(person.name, person.nationalId);
    case CardRole::Id:
        return person.id;
    case CardRole::Title:
        return person.name;
    case CardRole::Subtitle: {
        QString subtitle = QStringLiteral("%1 صورة").arg(person.photoCount);
        if (!person.nationalId.isEmpty())
            subtitle = QStringLiteral("رقم الهوية %1 · ").arg(person.nationalId) + subtitle;
        return subtitle;
    }
    case CardRole::Status:
        return -1;
    case CardRole::Avatar: {
        if (person.coverPhoto <= 0)
            return QPixmap();
        if (const QPixmap *cached = m_avatars.object(person.coverPhoto))
            return *cached;
        const QPixmap avatar = circularPixmap(m_database->photo(person.coverPhoto, AvatarSize * 2), AvatarSize);
        m_avatars.insert(person.coverPhoto, new QPixmap(avatar));
        return avatar;
    }
    default:
        return {};
    }
}

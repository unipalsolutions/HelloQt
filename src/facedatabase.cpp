#include "facedatabase.h"

#include "faceengine.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cstring>

namespace {

const char *const DatabaseFile = "persons.json";
const char *const ModelName = "insightface-buffalo_l";

QString encodeEmbedding(const std::vector<float> &embedding)
{
    const QByteArray raw(reinterpret_cast<const char *>(embedding.data()), int(embedding.size() * sizeof(float)));
    return QString::fromLatin1(raw.toBase64());
}

std::vector<float> decodeEmbedding(const QString &text)
{
    const QByteArray raw = QByteArray::fromBase64(text.toLatin1());
    std::vector<float> embedding(size_t(raw.size()) / sizeof(float));
    memcpy(embedding.data(), raw.constData(), embedding.size() * sizeof(float));
    return embedding;
}

QJsonObject toJson(const Person &person)
{
    QJsonArray photos;
    for (const FacePhoto &photo : person.photos) {
        QJsonArray embeddings;
        for (const std::vector<float> &embedding : photo.embeddings)
            embeddings.append(encodeEmbedding(embedding));
        photos.append(QJsonObject{
            {QStringLiteral("file"), photo.file},
            {QStringLiteral("quality"), double(photo.quality)},
            {QStringLiteral("embeddings"), embeddings},
        });
    }
    return QJsonObject{
        {QStringLiteral("id"), person.id},
        {QStringLiteral("name"), person.name},
        {QStringLiteral("nationalId"), person.nationalId},
        {QStringLiteral("phone"), person.phone},
        {QStringLiteral("notes"), person.notes},
        {QStringLiteral("created"), person.created.toString(Qt::ISODate)},
        {QStringLiteral("photos"), photos},
    };
}

Person fromJson(const QJsonObject &object)
{
    Person person;
    person.id = object.value(QStringLiteral("id")).toString();
    person.name = object.value(QStringLiteral("name")).toString();
    person.nationalId = object.value(QStringLiteral("nationalId")).toString();
    person.phone = object.value(QStringLiteral("phone")).toString();
    person.notes = object.value(QStringLiteral("notes")).toString();
    person.created = QDateTime::fromString(object.value(QStringLiteral("created")).toString(), Qt::ISODate);
    for (const QJsonValue &value : object.value(QStringLiteral("photos")).toArray()) {
        const QJsonObject photoObject = value.toObject();
        FacePhoto photo;
        photo.file = photoObject.value(QStringLiteral("file")).toString();
        photo.quality = float(photoObject.value(QStringLiteral("quality")).toDouble());
        for (const QJsonValue &embedding : photoObject.value(QStringLiteral("embeddings")).toArray())
            photo.embeddings.push_back(decodeEmbedding(embedding.toString()));
        person.photos.append(photo);
    }
    return person;
}

} // namespace

FaceDatabase::FaceDatabase(QObject *parent)
    : QObject(parent)
    , m_gallery(std::make_shared<Gallery>())
{
}

QString FaceDatabase::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/database");
}

bool FaceDatabase::open(const QString &directory, QString *error)
{
    m_directory = directory;
    m_persons.clear();
    if (!QDir().mkpath(QDir(directory).filePath(QStringLiteral("photos")))) {
        if (error)
            *error = QStringLiteral("تعذر إنشاء مجلد قاعدة البيانات: %1").arg(QDir::toNativeSeparators(directory));
        return false;
    }

    QFile file(QDir(directory).filePath(QString::fromLatin1(DatabaseFile)));
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("تعذر قراءة ملف قاعدة البيانات");
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            if (error)
                *error = QStringLiteral("ملف قاعدة البيانات تالف: %1").arg(parseError.errorString());
            return false;
        }
        for (const QJsonValue &value : document.object().value(QStringLiteral("persons")).toArray())
            m_persons.append(fromJson(value.toObject()));
    }

    commit();
    return true;
}

QString FaceDatabase::directory() const
{
    return m_directory;
}

QList<Person> FaceDatabase::persons() const
{
    return m_persons;
}

Person FaceDatabase::person(const QString &id) const
{
    const int index = indexOf(id);
    return index >= 0 ? m_persons.at(index) : Person();
}

QString FaceDatabase::addPerson(Person person)
{
    person.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    person.created = QDateTime::currentDateTime();
    person.photos.clear();
    m_persons.append(person);
    if (!save()) {
        m_persons.removeLast();
        return {};
    }
    commit();
    return person.id;
}

bool FaceDatabase::updatePerson(const Person &person)
{
    const int index = indexOf(person.id);
    if (index < 0)
        return false;
    const Person previous = m_persons.at(index);
    Person &target = m_persons[index];
    target.name = person.name;
    target.nationalId = person.nationalId;
    target.phone = person.phone;
    target.notes = person.notes;
    if (!save()) {
        m_persons[index] = previous;
        return false;
    }
    commit();
    return true;
}

bool FaceDatabase::removePerson(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    const Person removed = m_persons.takeAt(index);
    if (!save()) {
        m_persons.insert(index, removed);
        return false;
    }
    QDir(QDir(m_directory).filePath(QStringLiteral("photos/") + id)).removeRecursively();
    commit();
    return true;
}

bool FaceDatabase::addPhoto(const QString &personId, const QImage &crop,
                            const std::vector<std::vector<float>> &embeddings, float quality)
{
    const int index = indexOf(personId);
    if (index < 0 || embeddings.empty())
        return false;

    const QString folder = QDir(m_directory).filePath(QStringLiteral("photos/") + personId);
    QDir().mkpath(folder);
    FacePhoto photo;
    photo.file = QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".jpg");
    photo.quality = quality;
    photo.embeddings = embeddings;
    if (!crop.save(QDir(folder).filePath(photo.file), "JPG", 95))
        return false;

    m_persons[index].photos.append(photo);
    if (!save()) {
        m_persons[index].photos.removeLast();
        QFile::remove(QDir(folder).filePath(photo.file));
        return false;
    }
    commit();
    return true;
}

bool FaceDatabase::removePhoto(const QString &personId, const QString &file)
{
    const int index = indexOf(personId);
    if (index < 0)
        return false;
    QList<FacePhoto> &photos = m_persons[index].photos;
    for (int i = 0; i < photos.size(); ++i) {
        if (photos.at(i).file != file)
            continue;
        const FacePhoto removed = photos.takeAt(i);
        if (!save()) {
            photos.insert(i, removed);
            return false;
        }
        QFile::remove(photoPath(personId, file));
        commit();
        return true;
    }
    return false;
}

QString FaceDatabase::photoPath(const QString &personId, const QString &file) const
{
    return QDir(m_directory).filePath(QStringLiteral("photos/%1/%2").arg(personId, file));
}

GalleryPtr FaceDatabase::gallery() const
{
    return m_gallery;
}

MatchResult FaceDatabase::identify(const Gallery &gallery, const std::vector<float> &embedding, float threshold)
{
    MatchResult result;
    float best = -1.0f;
    float second = -1.0f;
    const GalleryEntry *bestEntry = nullptr;

    for (const GalleryEntry &entry : gallery) {
        float score = FaceEngine::cosine(entry.mean, embedding);
        for (const std::vector<float> &t : entry.templates)
            score = std::max(score, FaceEngine::cosine(t, embedding));
        if (score > best) {
            second = best;
            best = score;
            bestEntry = &entry;
        } else if (score > second) {
            second = score;
        }
    }

    if (!bestEntry)
        return result;

    result.score = std::max(0.0f, best);
    // مطابقة قريبة جداً من شخص آخر تُعد غير محسومة
    const bool clearWinner = best - second >= 0.05f;
    if (best >= threshold && clearWinner)
        result.status = MatchStatus::Confirmed;
    else if (best >= threshold - 0.1f)
        result.status = MatchStatus::Possible;

    if (result.status != MatchStatus::Unknown) {
        result.personId = bestEntry->personId;
        result.name = bestEntry->name;
    }
    return result;
}

bool FaceDatabase::save()
{
    QJsonArray persons;
    for (const Person &person : m_persons)
        persons.append(toJson(person));
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("model"), QString::fromLatin1(ModelName)},
        {QStringLiteral("persons"), persons},
    };

    QSaveFile file(QDir(m_directory).filePath(QString::fromLatin1(DatabaseFile)));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return file.commit();
}

void FaceDatabase::commit()
{
    auto gallery = std::make_shared<Gallery>();
    for (const Person &person : m_persons) {
        GalleryEntry entry;
        entry.personId = person.id;
        entry.name = person.name;
        for (const FacePhoto &photo : person.photos) {
            for (const std::vector<float> &embedding : photo.embeddings) {
                if (embedding.empty())
                    continue;
                entry.templates.push_back(embedding);
                if (entry.mean.empty())
                    entry.mean.assign(embedding.size(), 0.0f);
                if (entry.mean.size() == embedding.size()) {
                    for (size_t i = 0; i < embedding.size(); ++i)
                        entry.mean[i] += embedding[i];
                }
            }
        }
        if (entry.templates.empty())
            continue;
        FaceEngine::normalize(entry.mean);
        gallery->push_back(std::move(entry));
    }
    m_gallery = gallery;
    emit changed();
}

int FaceDatabase::indexOf(const QString &id) const
{
    for (int i = 0; i < m_persons.size(); ++i) {
        if (m_persons.at(i).id == id)
            return i;
    }
    return -1;
}

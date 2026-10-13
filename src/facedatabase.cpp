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
#include <numeric>

namespace {

const char *const DatabaseFile = "persons.json";
const char *const EmbeddingsFile = "embeddings.bin";
const char *const ModelName = "insightface-buffalo_l";
constexpr int DatabaseVersion = 2;
constexpr quint32 EmbeddingsMagic = 0x4d455148; // "HQEM"
constexpr quint32 EmbeddingsVersion = 1;
constexpr int StoredPhotoSize = 256; // أكبر بُعد لصورة الوجه المحفوظة
constexpr size_t PrefilterPersons = 256; // فوقها تُصفّى القاعدة بمتوسط كل شخص أولاً
constexpr size_t PrefilterCandidates = 64;

// الإصدار 1 كان يحفظ البصمات نصاً داخل persons.json
EmbeddingBlock decodeLegacyEmbeddings(const QJsonArray &array)
{
    auto block = std::make_shared<std::vector<float>>();
    for (const QJsonValue &value : array) {
        const QByteArray raw = QByteArray::fromBase64(value.toString().toLatin1());
        const size_t count = size_t(raw.size()) / sizeof(float);
        const size_t offset = block->size();
        block->resize(offset + count);
        memcpy(block->data() + offset, raw.constData(), count * sizeof(float));
    }
    return block->empty() ? nullptr : block;
}

QJsonObject toJson(const Person &person)
{
    QJsonArray photos;
    for (const FacePhoto &photo : person.photos) {
        photos.append(QJsonObject{
            {QStringLiteral("file"), photo.file},
            {QStringLiteral("quality"), double(photo.quality)},
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

Person fromJson(const QJsonObject &object, bool *legacy)
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
        if (photoObject.contains(QStringLiteral("embeddings"))) {
            photo.embeddings = decodeLegacyEmbeddings(photoObject.value(QStringLiteral("embeddings")).toArray());
            *legacy = true;
        }
        person.photos.append(photo);
    }
    return person;
}

std::shared_ptr<const GalleryEntry> makeEntry(const Person &person)
{
    auto entry = std::make_shared<GalleryEntry>();
    entry->personId = person.id;
    entry->name = person.name;
    size_t count = 0;
    for (const FacePhoto &photo : person.photos) {
        if (photo.embeddings && !photo.embeddings->empty() && photo.embeddings->size() % FaceEngine::EmbeddingSize == 0)
            entry->templates.push_back(photo.embeddings);
    }
    if (entry->templates.empty())
        return nullptr;

    constexpr size_t dimension = FaceEngine::EmbeddingSize;
    entry->mean.assign(dimension, 0.0f);
    for (const EmbeddingBlock &block : entry->templates) {
        for (size_t offset = 0; offset + dimension <= block->size(); offset += dimension) {
            for (size_t i = 0; i < dimension; ++i)
                entry->mean[i] += (*block)[offset + i];
            ++count;
        }
    }
    if (count == 0)
        return nullptr;
    FaceEngine::normalize(entry->mean);
    return entry;
}

// أعلى تشابه بين البصمة وكل بصمات الشخص ومتوسطها
float personScore(const GalleryEntry &entry, const float *embedding, size_t dimension)
{
    float score = FaceEngine::dot(entry.mean.data(), embedding, dimension);
    for (const EmbeddingBlock &block : entry.templates) {
        for (size_t offset = 0; offset + dimension <= block->size(); offset += dimension)
            score = std::max(score, FaceEngine::dot(block->data() + offset, embedding, dimension));
    }
    return score;
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
    m_entries.clear();
    m_json.clear();
    if (!QDir().mkpath(QDir(directory).filePath(QStringLiteral("photos")))) {
        if (error)
            *error = QStringLiteral("تعذر إنشاء مجلد قاعدة البيانات: %1").arg(QDir::toNativeSeparators(directory));
        return false;
    }

    bool legacy = false;
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
        const QJsonArray persons = document.object().value(QStringLiteral("persons")).toArray();
        m_persons.reserve(persons.size());
        for (const QJsonValue &value : persons)
            m_persons.append(fromJson(value.toObject(), &legacy));
    }

    if (legacy) {
        // ترحيل قاعدة الإصدار 1: نقل البصمات إلى ملفات ثنائية ثم تصغير persons.json
        bool migrated = true;
        for (const Person &person : std::as_const(m_persons))
            migrated = saveEmbeddings(person) && migrated;
        if (migrated)
            save();
    } else {
        for (Person &person : m_persons)
            loadEmbeddings(person);
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
    commit(person.id);
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
    m_json.remove(person.id);
    if (!save()) {
        m_persons[index] = previous;
        m_json.remove(person.id);
        return false;
    }
    commit(person.id);
    return true;
}

bool FaceDatabase::removePerson(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    const Person removed = m_persons.takeAt(index);
    m_index.clear();
    m_json.remove(id);
    if (!save()) {
        m_persons.insert(index, removed);
        return false;
    }
    QDir(QDir(m_directory).filePath(QStringLiteral("photos/") + id)).removeRecursively();
    commit(id);
    return true;
}

bool FaceDatabase::addPhoto(const QString &personId, const QImage &crop,
                            const std::vector<std::vector<float>> &embeddings, float quality)
{
    const int index = indexOf(personId);
    if (index < 0)
        return false;

    auto block = std::make_shared<std::vector<float>>();
    for (const std::vector<float> &embedding : embeddings) {
        if (embedding.size() == FaceEngine::EmbeddingSize)
            block->insert(block->end(), embedding.begin(), embedding.end());
    }
    if (block->empty())
        return false;

    const QString folder = QDir(m_directory).filePath(QStringLiteral("photos/") + personId);
    QDir().mkpath(folder);
    FacePhoto photo;
    photo.file = QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".jpg");
    photo.quality = quality;
    photo.embeddings = block;
    // صورة الوجه للعرض فقط (البصمات محفوظة منفصلة)، فتكفي دقة متوسطة بحجم صغير
    const QImage stored = std::max(crop.width(), crop.height()) > StoredPhotoSize
        ? crop.scaled(StoredPhotoSize, StoredPhotoSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : crop;
    if (!stored.save(QDir(folder).filePath(photo.file), "JPG", 90))
        return false;

    m_persons[index].photos.append(photo);
    m_json.remove(personId);
    if (!saveEmbeddings(m_persons.at(index)) || !save()) {
        m_persons[index].photos.removeLast();
        m_json.remove(personId);
        QFile::remove(QDir(folder).filePath(photo.file));
        return false;
    }
    commit(personId);
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
        m_json.remove(personId);
        if (!save()) {
            photos.insert(i, removed);
            m_json.remove(personId);
            return false;
        }
        // إن تعذرت إعادة كتابة البصمات فالصورة المحذوفة تُتجاهل عند الفتح لأنها ليست في persons.json
        saveEmbeddings(m_persons.at(index));
        QFile::remove(photoPath(personId, file));
        commit(personId);
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
    const size_t dimension = gallery.dimension;
    if (gallery.entries.empty() || embedding.size() != dimension)
        return result;

    // في القواعد الكبيرة تُقارن كل البصمات فقط للأشخاص الأقرب بالمتوسط
    std::vector<size_t> candidates(gallery.entries.size());
    std::iota(candidates.begin(), candidates.end(), size_t(0));
    if (candidates.size() > PrefilterPersons) {
        std::vector<float> meanScores(candidates.size());
        for (size_t i = 0; i < candidates.size(); ++i)
            meanScores[i] = FaceEngine::dot(gallery.means.data() + i * dimension, embedding.data(), dimension);
        std::nth_element(candidates.begin(), candidates.begin() + PrefilterCandidates, candidates.end(),
                         [&](size_t a, size_t b) { return meanScores[a] > meanScores[b]; });
        candidates.resize(PrefilterCandidates);
    }

    float best = -1.0f;
    float second = -1.0f;
    const GalleryEntry *bestEntry = nullptr;
    for (size_t i : candidates) {
        const GalleryEntry &entry = *gallery.entries[i];
        const float score = personScore(entry, embedding.data(), dimension);
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
    // كل شخص يُحوَّل إلى JSON مرة واحدة ويُحفظ مؤقتاً، فالحفظ يبقى سريعاً مع آلاف الأشخاص
    QByteArray data = QByteArrayLiteral("{\"version\":") + QByteArray::number(DatabaseVersion)
        + QByteArrayLiteral(",\"model\":\"") + ModelName + QByteArrayLiteral("\",\"persons\":[");
    for (int i = 0; i < m_persons.size(); ++i) {
        const Person &person = m_persons.at(i);
        auto cached = m_json.constFind(person.id);
        if (cached == m_json.constEnd())
            cached = m_json.insert(person.id, QJsonDocument(toJson(person)).toJson(QJsonDocument::Compact));
        if (i > 0)
            data += ',';
        data += *cached;
    }
    data += "]}";

    QSaveFile file(QDir(m_directory).filePath(QString::fromLatin1(DatabaseFile)));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(data);
    return file.commit();
}

// الصيغة: "HQEM"، الإصدار، عدد الصور، ثم لكل صورة: طول الاسم، الاسم، عدد القيم، القيم
bool FaceDatabase::saveEmbeddings(const Person &person) const
{
    const QString folder = QDir(m_directory).filePath(QStringLiteral("photos/") + person.id);
    QDir().mkpath(folder);
    QByteArray data;
    auto append = [&data](quint32 value) { data.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
    append(EmbeddingsMagic);
    append(EmbeddingsVersion);
    append(quint32(person.photos.size()));
    for (const FacePhoto &photo : person.photos) {
        const QByteArray name = photo.file.toUtf8();
        append(quint32(name.size()));
        data.append(name);
        const quint32 count = photo.embeddings ? quint32(photo.embeddings->size()) : 0;
        append(count);
        if (count > 0)
            data.append(reinterpret_cast<const char *>(photo.embeddings->data()), int(count * sizeof(float)));
    }

    QSaveFile file(QDir(folder).filePath(QString::fromLatin1(EmbeddingsFile)));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(data);
    return file.commit();
}

void FaceDatabase::loadEmbeddings(Person &person) const
{
    QFile file(QDir(QDir(m_directory).filePath(QStringLiteral("photos/") + person.id))
                   .filePath(QString::fromLatin1(EmbeddingsFile)));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = file.readAll();
    qsizetype position = 0;
    auto read = [&](quint32 &value) {
        if (position + qsizetype(sizeof(value)) > data.size())
            return false;
        memcpy(&value, data.constData() + position, sizeof(value));
        position += sizeof(value);
        return true;
    };

    quint32 magic = 0, version = 0, photos = 0;
    if (!read(magic) || !read(version) || !read(photos) || magic != EmbeddingsMagic || version != EmbeddingsVersion)
        return;
    QHash<QString, EmbeddingBlock> blocks;
    for (quint32 i = 0; i < photos; ++i) {
        quint32 nameSize = 0, count = 0;
        if (!read(nameSize) || position + qsizetype(nameSize) > data.size())
            return;
        const QString name = QString::fromUtf8(data.constData() + position, qsizetype(nameSize));
        position += nameSize;
        if (!read(count) || position + qsizetype(count) * qsizetype(sizeof(float)) > data.size())
            return;
        auto block = std::make_shared<std::vector<float>>(count);
        memcpy(block->data(), data.constData() + position, count * sizeof(float));
        position += qsizetype(count) * qsizetype(sizeof(float));
        blocks.insert(name, block);
    }
    // تُعتمد فقط الصور المسجلة في persons.json
    for (FacePhoto &photo : person.photos)
        photo.embeddings = blocks.value(photo.file);
}

// يُعاد بناء مدخل الشخص المتغير فقط، وباقي الأشخاص يُشارَكون مع المعرض السابق دون نسخ
void FaceDatabase::commit(const QString &changedId)
{
    m_index.clear();
    if (changedId.isEmpty())
        m_entries.clear();
    else
        m_entries.remove(changedId);

    auto gallery = std::make_shared<Gallery>();
    gallery->entries.reserve(size_t(m_persons.size()));
    for (int i = 0; i < m_persons.size(); ++i) {
        const Person &person = m_persons.at(i);
        m_index.insert(person.id, i);
        auto cached = m_entries.constFind(person.id);
        std::shared_ptr<const GalleryEntry> entry = cached != m_entries.constEnd() ? *cached : makeEntry(person);
        m_entries.insert(person.id, entry);
        if (!entry || (gallery->dimension != 0 && entry->mean.size() != gallery->dimension))
            continue;
        gallery->dimension = entry->mean.size();
        gallery->entries.push_back(entry);
        gallery->means.insert(gallery->means.end(), entry->mean.begin(), entry->mean.end());
    }
    m_gallery = gallery;
    emit changed();
}

int FaceDatabase::indexOf(const QString &id) const
{
    if (!m_index.isEmpty())
        return m_index.value(id, -1);
    for (int i = 0; i < m_persons.size(); ++i) {
        if (m_persons.at(i).id == id)
            return i;
    }
    return -1;
}

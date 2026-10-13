#pragma once

#include "facetypes.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

using EmbeddingBlock = std::shared_ptr<const std::vector<float>>; // بصمات متتالية، كل واحدة بطول البصمة

struct FacePhoto
{
    QString file;
    float quality = 0.0f;
    EmbeddingBlock embeddings; // بصمات الصورة ونسخها المُضعَّفة، مشتركة مع المعرض دون نسخ
};

struct Person
{
    QString id;
    QString name;
    QString nationalId;
    QString phone;
    QString notes;
    QDateTime created;
    QList<FacePhoto> photos;
};

struct GalleryEntry
{
    QString personId;
    QString name;
    std::vector<EmbeddingBlock> templates;
    std::vector<float> mean;
};

// نسخة ثابتة من القاعدة للمطابقة، يمكن استخدامها من أي خيط
struct Gallery
{
    std::vector<std::shared_ptr<const GalleryEntry>> entries;
    std::vector<float> means; // متوسطات الأشخاص متتالية، لتصفية سريعة في القواعد الكبيرة
    size_t dimension = 0;
};

using GalleryPtr = std::shared_ptr<const Gallery>;

// قاعدة بيانات محلية بداخل مجلد القاعدة:
//   persons.json                    بيانات الأشخاص وأسماء صورهم (صغير وسريع الحفظ)
//   photos/<id>/<صورة>.jpg          صور الوجوه
//   photos/<id>/embeddings.bin      بصمات وجوه الشخص، يُعاد كتابته عند تغيّر صوره فقط
class FaceDatabase : public QObject
{
    Q_OBJECT

public:
    explicit FaceDatabase(QObject *parent = nullptr);

    static QString defaultDirectory();

    bool open(const QString &directory, QString *error = nullptr);
    QString directory() const;

    QList<Person> persons() const;
    Person person(const QString &id) const;
    QString addPerson(Person person);
    bool updatePerson(const Person &person);
    bool removePerson(const QString &id);
    bool addPhoto(const QString &personId, const QImage &crop,
                  const std::vector<std::vector<float>> &embeddings, float quality);
    bool removePhoto(const QString &personId, const QString &file);
    QString photoPath(const QString &personId, const QString &file) const;

    GalleryPtr gallery() const;

    // يعيد أفضل شخص مطابق؛ threshold هو حد التأكيد، وما دونه بـ 0.1 "محتمل"
    static MatchResult identify(const Gallery &gallery, const std::vector<float> &embedding, float threshold);

signals:
    void changed();

private:
    bool save();
    bool saveEmbeddings(const Person &person) const;
    void loadEmbeddings(Person &person) const;
    void commit(const QString &changedId = QString());
    int indexOf(const QString &id) const;

    QString m_directory;
    QList<Person> m_persons;
    QHash<QString, int> m_index;
    QHash<QString, std::shared_ptr<const GalleryEntry>> m_entries;
    QHash<QString, QByteArray> m_json; // نص JSON لكل شخص، يُعاد إنشاؤه عند تعديله فقط
    GalleryPtr m_gallery;
};

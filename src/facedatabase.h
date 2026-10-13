#pragma once

#include "facetypes.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

struct FacePhoto
{
    QString file;
    float quality = 0.0f;
    std::vector<std::vector<float>> embeddings;
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
    std::vector<std::vector<float>> templates;
    std::vector<float> mean;
};

using Gallery = std::vector<GalleryEntry>;
using GalleryPtr = std::shared_ptr<const Gallery>;

// قاعدة بيانات محلية: ملف persons.json ومجلد photos بداخل مجلد القاعدة
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
    void commit();
    int indexOf(const QString &id) const;

    QString m_directory;
    QList<Person> m_persons;
    GalleryPtr m_gallery;
};

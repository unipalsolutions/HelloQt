#include "videomarkers.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace VideoMarkers {

namespace {

// اسم الملف من المسار الكامل، فلكل فيديو علاماته ولو تكرر اسمه في مجلدات مختلفة
QString storePath(const QString &videoPath)
{
    const QString absolute = QFileInfo(videoPath).absoluteFilePath();
    const QByteArray key = QCryptographicHash::hash(absolute.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/markers/")
        + QString::fromLatin1(key) + QStringLiteral(".json");
}

} // namespace

QMap<qint64, QString> load(const QString &videoPath)
{
    QMap<qint64, QString> markers;
    QFile file(storePath(videoPath));
    if (videoPath.isEmpty() || !file.open(QIODevice::ReadOnly))
        return markers;
    const QJsonArray list = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("markers")).toArray();
    for (const QJsonValue &value : list) {
        const QJsonObject marker = value.toObject();
        const qint64 ms = qint64(marker.value(QStringLiteral("ms")).toDouble(-1));
        if (ms >= 0)
            markers.insert(ms, marker.value(QStringLiteral("note")).toString());
    }
    return markers;
}

void save(const QString &videoPath, const QMap<qint64, QString> &markers)
{
    if (videoPath.isEmpty())
        return;
    const QString path = storePath(videoPath);
    if (markers.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray list;
    for (auto it = markers.cbegin(); it != markers.cend(); ++it)
        list.append(QJsonObject{{QStringLiteral("ms"), double(it.key())}, {QStringLiteral("note"), it.value()}});
    const QJsonObject root{{QStringLiteral("video"), QFileInfo(videoPath).absoluteFilePath()}, {QStringLiteral("markers"), list}};
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

} // namespace VideoMarkers

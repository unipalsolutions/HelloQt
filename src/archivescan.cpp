#include "archivescan.h"

#include "appsettings.h"
#include "faceengine.h"
#include "imageio.h"
#include "report.h"
#include "videoworker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opencv2/videoio.hpp>

namespace {

constexpr int StateVersion = 1;
// وجهان من جزأين مختلفين بهذا التشابه = شخص واحد. أعلى من حد الدمج داخل الفيديو الواحد (0.30)
// لأن هنا لا يُعرف أن الشخصين ظهرا معاً في إطار واحد فيُمنع دمجهما
constexpr float CrossChunkSimilarity = 0.40f;
constexpr qint64 AppearanceGapMs = 2000;
constexpr qint64 MinCheckpointMs = 1000; // جزء أُوقف قبل هذا من بدايته يُعاد من أوله
constexpr qint64 SpeedWarmupMs = 8000;   // لا يُعتمد قياس السرعة قبل هذا

QString encodeFloats(const std::vector<float> &values)
{
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(values.data()), qsizetype(values.size() * sizeof(float))).toBase64());
}

std::vector<float> decodeFloats(const QString &text)
{
    const QByteArray bytes = QByteArray::fromBase64(text.toLatin1());
    std::vector<float> values(size_t(bytes.size()) / sizeof(float));
    std::memcpy(values.data(), bytes.constData(), values.size() * sizeof(float));
    return values;
}

QJsonObject faceToJson(const FaceItem &face)
{
    QJsonObject o;
    o[QStringLiteral("id")] = face.id;
    o[QStringLiteral("q")] = double(face.quality);
    o[QStringLiteral("fr")] = face.frames;
    o[QStringLiteral("f")] = double(face.firstSeenMs);
    o[QStringLiteral("l")] = double(face.lastSeenMs);
    o[QStringLiteral("b")] = double(face.bestMs);
    o[QStringLiteral("sb")] = QJsonArray{face.sourceBox.x(), face.sourceBox.y(), face.sourceBox.width(), face.sourceBox.height()};
    QJsonArray appearances;
    for (const auto &range : face.appearances)
        appearances.append(QJsonArray{double(range.first), double(range.second)});
    o[QStringLiteral("ap")] = appearances;
    o[QStringLiteral("e")] = encodeFloats(face.embedding);
    o[QStringLiteral("c")] = QString::fromLatin1(face.packedCrop.toBase64());
    o[QStringLiteral("a")] = QString::fromLatin1(face.packedAligned.toBase64());
    o[QStringLiteral("mp")] = face.match.personId;
    o[QStringLiteral("mn")] = face.match.name;
    o[QStringLiteral("ms")] = double(face.match.score);
    o[QStringLiteral("mt")] = int(face.match.status);
    return o;
}

FaceItem faceFromJson(const QJsonObject &o)
{
    FaceItem face;
    face.id = o.value(QStringLiteral("id")).toInt();
    face.quality = float(o.value(QStringLiteral("q")).toDouble());
    face.frames = o.value(QStringLiteral("fr")).toInt(1);
    face.firstSeenMs = qint64(o.value(QStringLiteral("f")).toDouble(-1));
    face.lastSeenMs = qint64(o.value(QStringLiteral("l")).toDouble(-1));
    face.bestMs = qint64(o.value(QStringLiteral("b")).toDouble(-1));
    const QJsonArray box = o.value(QStringLiteral("sb")).toArray();
    if (box.size() == 4)
        face.sourceBox = QRectF(box.at(0).toDouble(), box.at(1).toDouble(), box.at(2).toDouble(), box.at(3).toDouble());
    for (const QJsonValue &value : o.value(QStringLiteral("ap")).toArray()) {
        const QJsonArray range = value.toArray();
        if (range.size() == 2)
            face.appearances.append({qint64(range.at(0).toDouble()), qint64(range.at(1).toDouble())});
    }
    face.embedding = decodeFloats(o.value(QStringLiteral("e")).toString());
    face.packedCrop = QByteArray::fromBase64(o.value(QStringLiteral("c")).toString().toLatin1());
    face.packedAligned = QByteArray::fromBase64(o.value(QStringLiteral("a")).toString().toLatin1());
    face.match.personId = o.value(QStringLiteral("mp")).toString();
    face.match.name = o.value(QStringLiteral("mn")).toString();
    face.match.score = float(o.value(QStringLiteral("ms")).toDouble());
    face.match.status = MatchStatus(std::clamp(o.value(QStringLiteral("mt")).toInt(), 0, 2));
    return face;
}

void mergeRanges(QList<QPair<qint64, qint64>> &ranges)
{
    std::sort(ranges.begin(), ranges.end());
    QList<QPair<qint64, qint64>> merged;
    for (const auto &range : std::as_const(ranges)) {
        if (!merged.isEmpty() && range.first - merged.last().second <= AppearanceGapMs)
            merged.last().second = std::max(merged.last().second, range.second);
        else
            merged.append(range);
    }
    ranges = merged;
}

qint64 probeDuration(const QString &path)
{
    cv::VideoCapture capture;
    if (!ImageIO::openVideo(capture, path))
        return -1;
    const double fps = capture.get(cv::CAP_PROP_FPS);
    const double frames = capture.get(cv::CAP_PROP_FRAME_COUNT);
    if (fps > 1.0 && fps < 1000.0 && frames > 0)
        return qint64(frames * 1000.0 / fps);
    return 0;
}

} // namespace

namespace Archive {

QList<Person> people(const QList<Chunk> &chunks, int fileCount, const GalleryPtr &gallery, float threshold)
{
    struct Entry
    {
        const FaceItem *face;
        int chunk;
        int file;
        MatchResult match;
    };
    std::vector<Entry> entries;
    for (int c = 0; c < chunks.size(); ++c) {
        const Chunk &chunk = chunks.at(c);
        if (chunk.file < 0 || chunk.file >= fileCount)
            continue;
        for (const FaceItem &face : chunk.faces) {
            // القاعدة قد تغيرت منذ حُلل الجزء (شخص أُضيف أثناء مسح طويل): يُعاد التعرف بالبصمة المحفوظة
            const MatchResult match = gallery && !face.embedding.empty() ? FaceDatabase::identify(*gallery, face.embedding, threshold) : face.match;
            entries.push_back({&face, c, chunk.file, match});
        }
    }
    // الأوضح أولاً، فتكون أول لقطة في كل مجموعة أفضلها وتُبنى المجموعات المجهولة على البصمات الأثبت
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.face->quality > b.face->quality; });

    struct Group
    {
        std::vector<int> members;
        std::vector<float> sum;
        std::vector<float> mean;
        QSet<int> chunks;
        MatchResult match;
    };
    std::vector<Group> groups;
    QHash<QString, int> known;
    for (int i = 0; i < int(entries.size()); ++i) {
        const Entry &entry = entries[size_t(i)];
        int target = -1;
        if (entry.match.status == MatchStatus::Confirmed && !entry.match.personId.isEmpty()) {
            target = known.value(entry.match.personId, -1);
            if (target < 0) {
                target = int(groups.size());
                known.insert(entry.match.personId, target);
                groups.push_back({});
                groups.back().match = entry.match;
            } else if (entry.match.score > groups[size_t(target)].match.score) {
                groups[size_t(target)].match = entry.match;
            }
        } else if (!entry.face->embedding.empty()) {
            // وجه من جزء آخر يشبه مجموعة مجهولة: الشخص نفسه عاد في ملف أو جزء آخر. وجهان من الجزء نفسه
            // لم يدمجهما التحليل، فهما شخصان مختلفان
            float best = CrossChunkSimilarity;
            for (int g = 0; g < int(groups.size()); ++g) {
                const Group &group = groups[size_t(g)];
                if (group.match.status == MatchStatus::Confirmed || group.chunks.contains(entry.chunk) || group.mean.empty())
                    continue;
                const float s = FaceEngine::cosine(group.mean, entry.face->embedding);
                if (s > best) {
                    best = s;
                    target = g;
                }
            }
            if (target < 0) {
                target = int(groups.size());
                groups.push_back({});
            }
            Group &group = groups[size_t(target)];
            if (entry.match.status == MatchStatus::Possible && entry.match.score > group.match.score)
                group.match = entry.match;
        } else {
            continue;
        }
        Group &group = groups[size_t(target)];
        group.members.push_back(i);
        group.chunks.insert(entry.chunk);
        const std::vector<float> &embedding = entry.face->embedding;
        if (!embedding.empty()) {
            if (group.sum.empty())
                group.sum.assign(embedding.size(), 0.0f);
            const float weight = 0.05f + entry.face->quality * entry.face->quality;
            for (size_t k = 0; k < group.sum.size() && k < embedding.size(); ++k)
                group.sum[k] += weight * embedding[k];
            group.mean = group.sum;
            FaceEngine::normalize(group.mean);
        }
    }

    QList<Person> result;
    for (const Group &group : groups) {
        if (group.members.empty())
            continue;
        Person person;
        person.match = group.match;
        const Entry &best = entries[size_t(group.members.front())];
        person.face = *best.face;
        person.face.match = group.match;
        person.bestFile = best.file;
        person.bestFileMs = best.face->bestMs;
        QHash<int, int> byFile;
        for (int member : group.members) {
            const Entry &entry = entries[size_t(member)];
            int index = byFile.value(entry.file, -1);
            if (index < 0) {
                index = int(person.sightings.size());
                byFile.insert(entry.file, index);
                Sighting sighting;
                sighting.file = entry.file;
                person.sightings.append(sighting);
            }
            Sighting &sighting = person.sightings[index];
            sighting.appearances << entry.face->appearances;
            if (entry.face->appearances.isEmpty() && entry.face->firstSeenMs >= 0)
                sighting.appearances << qMakePair(entry.face->firstSeenMs, entry.face->lastSeenMs);
        }
        for (Sighting &sighting : person.sightings) {
            mergeRanges(sighting.appearances);
            if (!sighting.appearances.isEmpty()) {
                sighting.firstMs = sighting.appearances.first().first;
                sighting.lastMs = sighting.appearances.last().second;
            }
            sighting.screenMs = Report::screenTime(sighting.appearances);
            person.screenMs += sighting.screenMs;
        }
        std::sort(person.sightings.begin(), person.sightings.end(), [](const Sighting &a, const Sighting &b) {
            return a.file != b.file ? a.file < b.file : a.firstMs < b.firstMs;
        });
        result.append(person);
    }
    // المعروفون أولاً ثم المحتملون ثم المجهولون، والأطول ظهوراً أولاً في كل منها
    std::sort(result.begin(), result.end(), [](const Person &a, const Person &b) {
        if (a.match.status != b.match.status)
            return int(a.match.status) > int(b.match.status);
        return a.screenMs > b.screenMs;
    });
    return result;
}

QList<Report::ArchivePerson> toReport(const QList<Person> &persons, const QList<File> &files)
{
    QList<Report::ArchivePerson> result;
    for (const Person &person : persons) {
        Report::ArchivePerson entry;
        entry.face = person.face;
        for (const Sighting &sighting : person.sightings) {
            if (sighting.file < 0 || sighting.file >= files.size())
                continue;
            entry.sightings.append({files.at(sighting.file).path, files.at(sighting.file).durationMs, sighting.appearances});
        }
        result.append(entry);
    }
    return result;
}

} // namespace Archive

ArchiveScan::ArchiveScan(const FaceEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_statePath(defaultStatePath())
{
    const QSettings settings(AppSettings::settingsPath(), QSettings::IniFormat);
    m_savedSpeed = settings.value(QStringLiteral("archive/speed"), 0.0).toDouble();
}

ArchiveScan::~ArchiveScan()
{
    if (m_running) {
        stop();
        for (Slot &slot : m_slots) {
            slot.thread->quit();
            slot.thread->wait();
        }
        // نتائج الخيوط الأخيرة ما زالت في طابور الأحداث: تُسلَّم الآن ليُحفظ موضع كل خيط قبل الإغلاق
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    }
    shutdownSlots();
}

QString ArchiveScan::defaultStatePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/archive-scan.json");
}

bool ArchiveScan::load()
{
    if (m_running)
        return false;
    QFile file(m_statePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != StateVersion)
        return false;
    m_files.clear();
    m_chunks.clear();
    for (const QJsonValue &value : root.value(QStringLiteral("files")).toArray()) {
        const QJsonObject o = value.toObject();
        Archive::File f;
        f.path = o.value(QStringLiteral("path")).toString();
        f.durationMs = qint64(o.value(QStringLiteral("duration")).toDouble());
        f.error = o.value(QStringLiteral("error")).toString();
        m_files.append(f);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("chunks")).toArray()) {
        const QJsonObject o = value.toObject();
        Archive::Chunk chunk;
        chunk.file = o.value(QStringLiteral("file")).toInt();
        chunk.startMs = qint64(o.value(QStringLiteral("start")).toDouble());
        chunk.endMs = qint64(o.value(QStringLiteral("end")).toDouble());
        chunk.done = o.value(QStringLiteral("done")).toBool();
        for (const QJsonValue &face : o.value(QStringLiteral("faces")).toArray())
            chunk.faces.append(faceFromJson(face.toObject()));
        if (chunk.file >= 0 && chunk.file < m_files.size())
            m_chunks.append(chunk);
    }
    emit filesChanged();
    emit progressChanged();
    return !m_files.isEmpty();
}

bool ArchiveScan::save() const
{
    if (m_files.isEmpty()) {
        QFile::remove(m_statePath);
        return true;
    }
    QJsonArray files;
    for (const Archive::File &f : m_files)
        files.append(QJsonObject{{QStringLiteral("path"), f.path}, {QStringLiteral("duration"), double(f.durationMs)},
                                 {QStringLiteral("error"), f.error}});
    QJsonArray chunks;
    for (const Archive::Chunk &chunk : m_chunks) {
        QJsonArray faces;
        for (const FaceItem &face : chunk.faces)
            faces.append(faceToJson(face));
        chunks.append(QJsonObject{{QStringLiteral("file"), chunk.file}, {QStringLiteral("start"), double(chunk.startMs)},
                                  {QStringLiteral("end"), double(chunk.endMs)}, {QStringLiteral("done"), chunk.done},
                                  {QStringLiteral("faces"), faces}});
    }
    const QJsonObject root{{QStringLiteral("version"), StateVersion}, {QStringLiteral("files"), files}, {QStringLiteral("chunks"), chunks}};
    QDir().mkpath(QFileInfo(m_statePath).absolutePath());
    // الكتابة في ملف مؤقت ثم استبداله: انقطاع الكهرباء أثناء الحفظ لا يفسد الحالة السابقة
    QSaveFile file(m_statePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit())
        return false;
    const double measured = speed();
    if (measured > 0.0) {
        m_savedSpeed = measured;
        QSettings(AppSettings::settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("archive/speed"), measured);
    }
    return true;
}

void ArchiveScan::setGallery(GalleryPtr gallery)
{
    m_gallery = std::move(gallery);
    for (Slot &slot : m_slots)
        slot.worker->setGallery(m_gallery);
}

void ArchiveScan::setThreshold(float threshold)
{
    m_threshold = threshold;
    for (Slot &slot : m_slots)
        slot.worker->setThreshold(threshold);
}

int ArchiveScan::autoParallel(bool gpu)
{
    const int cores = std::max(1, QThread::idealThreadCount());
    // على الكرت: خيطان يكفيان ليُفك الفيديو في أحدهما بينما يحلل الكرت إطار الآخر.
    // على المعالج: النماذج تستعمل كل الأنوية أصلاً فلا يفيد خيط ثانٍ إلا مع أنوية كثيرة
    // يُفك فيها الفيديو العالي الدقة بالتوازي (قيس: خيطان على أربع أنوية أبطأ من خيط واحد)
    if (gpu)
        return 2;
    return cores >= 8 ? std::min(4, cores / 4) : 1;
}

int ArchiveScan::addFiles(const QStringList &paths)
{
    QSet<QString> existing;
    for (const Archive::File &f : std::as_const(m_files))
        existing.insert(QFileInfo(f.path).absoluteFilePath());
    int added = 0;
    for (const QString &path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (existing.contains(absolute))
            continue;
        const qint64 duration = probeDuration(absolute);
        if (duration < 0)
            continue;
        existing.insert(absolute);
        Archive::File f;
        f.path = absolute;
        f.durationMs = duration;
        const int index = int(m_files.size());
        m_files.append(f);
        // مدة مجهولة: جزء واحد حتى نهاية الملف
        if (duration <= 0) {
            m_chunks.append({index, 0, 0, false, {}});
        } else {
            for (qint64 start = 0; start < duration; start += m_chunkMs) {
                // الجزء الأخير القصير جداً يُضم إلى ما قبله
                const qint64 end = duration - (start + m_chunkMs) < m_chunkMs / 4 ? duration : start + m_chunkMs;
                m_chunks.append({index, start, end, false, {}});
                if (end == duration)
                    break;
            }
        }
        ++added;
    }
    if (added > 0) {
        save();
        emit filesChanged();
        emit progressChanged();
    }
    return added;
}

void ArchiveScan::removeFile(int file)
{
    if (m_running || file < 0 || file >= m_files.size())
        return;
    m_files.removeAt(file);
    for (int i = int(m_chunks.size()) - 1; i >= 0; --i) {
        if (m_chunks[i].file == file)
            m_chunks.removeAt(i);
        else if (m_chunks[i].file > file)
            --m_chunks[i].file;
    }
    save();
    emit filesChanged();
    emit progressChanged();
}

void ArchiveScan::clear()
{
    if (m_running)
        return;
    m_files.clear();
    m_chunks.clear();
    save();
    emit filesChanged();
    emit progressChanged();
}

bool ArchiveScan::hasPending() const
{
    return std::any_of(m_chunks.begin(), m_chunks.end(), [](const Archive::Chunk &c) { return !c.done; });
}

void ArchiveScan::start(int parallel)
{
    if (m_running || !hasPending())
        return;
    parallel = std::clamp(parallel, 1, 8);
    shutdownSlots();
    m_slots.resize(size_t(parallel));
    for (int i = 0; i < parallel; ++i) {
        Slot &slot = m_slots[size_t(i)];
        slot.thread = new QThread(this);
        slot.worker = new VideoWorker(m_engine);
        slot.worker->moveToThread(slot.thread);
        slot.worker->setGallery(m_gallery);
        slot.worker->setThreshold(m_threshold);
        slot.worker->setDeliverFrames(false);
        slot.worker->setQuickScan(true);
        slot.worker->setFrameStep(0);
        connect(slot.thread, &QThread::finished, slot.worker, &QObject::deleteLater);
        connect(slot.worker, &VideoWorker::trackUpdated, this, [this, i](const FaceItem &track) {
            m_slots[size_t(i)].faces.insert(track.id, track);
        });
        connect(slot.worker, &VideoWorker::identitiesMerged, this, [this, i](int, int removedId) {
            m_slots[size_t(i)].faces.remove(removedId);
        });
        connect(slot.worker, &VideoWorker::frameReady, this, [this, i](const QImage &, const QList<FaceItem> &, qint64 positionMs, double) {
            m_slots[size_t(i)].positionMs = positionMs;
        });
        connect(slot.worker, &VideoWorker::thumbnailReady, this, [this, i](qint64 positionMs, const QImage &image) {
            const Slot &s = m_slots[size_t(i)];
            if (s.chunk >= 0)
                emit previewReady(i, image, m_files.at(m_chunks.at(s.chunk).file).path, positionMs);
        });
        connect(slot.worker, &VideoWorker::finished, this, [this, i](const QString &error) { onWorkerFinished(i, error); });
        slot.thread->start();
    }
    m_running = true;
    m_stopping = false;
    m_doneAtStart = doneMs();
    m_clock.start();
    dispatch();
    emit progressChanged();
}

void ArchiveScan::stop()
{
    if (!m_running || m_stopping)
        return;
    m_stopping = true;
    bool busy = false;
    for (Slot &slot : m_slots) {
        if (slot.chunk >= 0) {
            slot.stopping = true;
            slot.worker->requestStop();
            busy = true;
        }
    }
    if (!busy) {
        m_running = false;
        save();
        emit progressChanged();
        emit finished();
    }
}

void ArchiveScan::dispatch()
{
    if (m_stopping)
        return;
    QSet<int> taken;
    for (const Slot &slot : m_slots) {
        if (slot.chunk >= 0)
            taken.insert(slot.chunk);
    }
    for (int i = 0; i < int(m_slots.size()); ++i) {
        if (m_slots[size_t(i)].chunk >= 0)
            continue;
        int next = -1;
        for (int c = 0; c < m_chunks.size(); ++c) {
            if (!m_chunks[c].done && !taken.contains(c) && m_files.at(m_chunks[c].file).error.isEmpty()) {
                next = c;
                break;
            }
        }
        if (next < 0)
            break;
        taken.insert(next);
        runChunk(i, next);
    }
    if (activeWorkers() == 0) {
        m_running = false;
        save();
        emit progressChanged();
        emit finished();
    }
}

void ArchiveScan::runChunk(int slotIndex, int chunk)
{
    Slot &slot = m_slots[size_t(slotIndex)];
    const Archive::Chunk &c = m_chunks.at(chunk);
    slot.chunk = chunk;
    slot.positionMs = c.startMs;
    slot.stopping = false;
    slot.faces.clear();
    slot.worker->setRange(c.startMs, c.endMs);
    slot.worker->prepareStart();
    QMetaObject::invokeMethod(slot.worker, "process", Qt::QueuedConnection, Q_ARG(QString, m_files.at(c.file).path));
}

void ArchiveScan::onWorkerFinished(int slotIndex, const QString &error)
{
    Slot &slot = m_slots[size_t(slotIndex)];
    const int index = slot.chunk;
    if (index < 0)
        return;
    slot.chunk = -1;
    Archive::Chunk &chunk = m_chunks[index];
    const QList<FaceItem> faces = slot.faces.values();
    slot.faces.clear();
    if (!error.isEmpty() && !slot.stopping) {
        // ملف تالف أو لا يُقرأ: يُعلَّم ولا يوقف بقية القائمة
        m_files[chunk.file].error = error;
        for (Archive::Chunk &other : m_chunks) {
            if (other.file == chunk.file && !other.done)
                other.done = true;
        }
    } else if (slot.stopping) {
        // أُوقف في منتصف الجزء: ما حُلل منه يُحفظ جزءاً منتهياً، والباقي جزء جديد يُتابع منه لاحقاً
        const qint64 reached = slot.positionMs;
        if (reached - chunk.startMs >= MinCheckpointMs && (chunk.endMs <= 0 || reached < chunk.endMs)) {
            Archive::Chunk rest{chunk.file, reached, chunk.endMs, false, {}};
            chunk.endMs = reached;
            chunk.done = true;
            chunk.faces = faces;
            m_chunks.append(rest);
        }
    } else {
        chunk.done = true;
        chunk.faces = faces;
        // مدة لم تُعرف عند الإضافة: آخر موضع وصل إليه التحليل
        if (chunk.endMs <= 0) {
            chunk.endMs = std::max(chunk.startMs, slot.positionMs);
            m_files[chunk.file].durationMs = std::max(m_files[chunk.file].durationMs, chunk.endMs);
        }
    }
    slot.stopping = false;
    save();
    emit chunkFinished();
    emit progressChanged();

    if (m_stopping) {
        if (activeWorkers() == 0) {
            m_running = false;
            m_stopping = false;
            save();
            emit progressChanged();
            emit finished();
        }
        return;
    }
    dispatch();
}

void ArchiveScan::shutdownSlots()
{
    for (Slot &slot : m_slots) {
        // بعد توقف الخيط يكون العامل قد حُذف معه (deleteLater عند finished)
        if (slot.thread->isRunning()) {
            slot.worker->requestStop();
            slot.thread->quit();
            slot.thread->wait();
        }
        delete slot.thread;
    }
    m_slots.clear();
}

int ArchiveScan::activeWorkers() const
{
    return int(std::count_if(m_slots.begin(), m_slots.end(), [](const Slot &s) { return s.chunk >= 0; }));
}

ArchiveScan::FileState ArchiveScan::fileState(int file) const
{
    if (file < 0 || file >= m_files.size())
        return FileState::Waiting;
    if (!m_files.at(file).error.isEmpty())
        return FileState::Failed;
    for (const Slot &slot : m_slots) {
        if (slot.chunk >= 0 && m_chunks.at(slot.chunk).file == file)
            return FileState::Running;
    }
    for (const Archive::Chunk &chunk : m_chunks) {
        if (chunk.file == file && !chunk.done)
            return FileState::Waiting;
    }
    return FileState::Done;
}

qint64 ArchiveScan::chunkDoneMs(int index) const
{
    const Archive::Chunk &chunk = m_chunks.at(index);
    const qint64 length = std::max<qint64>(0, chunk.endMs - chunk.startMs);
    if (chunk.done)
        return length;
    for (const Slot &slot : m_slots) {
        if (slot.chunk == index)
            return std::clamp<qint64>(slot.positionMs - chunk.startMs, 0, length);
    }
    return 0;
}

double ArchiveScan::fileProgress(int file) const
{
    qint64 total = 0, done = 0;
    for (int c = 0; c < m_chunks.size(); ++c) {
        if (m_chunks[c].file != file)
            continue;
        total += std::max<qint64>(0, m_chunks[c].endMs - m_chunks[c].startMs);
        done += chunkDoneMs(c);
    }
    return total > 0 ? double(done) / double(total) : (fileState(file) == FileState::Done ? 1.0 : 0.0);
}

QList<Archive::Chunk> ArchiveScan::snapshot() const
{
    QList<Archive::Chunk> result;
    for (const Archive::Chunk &chunk : m_chunks) {
        if (chunk.done && !chunk.faces.isEmpty())
            result.append(chunk);
    }
    for (const Slot &slot : m_slots) {
        if (slot.chunk < 0 || slot.faces.isEmpty())
            continue;
        Archive::Chunk running = m_chunks.at(slot.chunk);
        running.faces = slot.faces.values();
        result.append(running);
    }
    return result;
}

qint64 ArchiveScan::totalMs() const
{
    qint64 total = 0;
    for (const Archive::Chunk &chunk : m_chunks) {
        if (m_files.at(chunk.file).error.isEmpty())
            total += std::max<qint64>(0, chunk.endMs - chunk.startMs);
    }
    return total;
}

qint64 ArchiveScan::doneMs() const
{
    qint64 done = 0;
    for (int c = 0; c < m_chunks.size(); ++c) {
        if (m_files.at(m_chunks[c].file).error.isEmpty())
            done += chunkDoneMs(c);
    }
    return done;
}

double ArchiveScan::speed() const
{
    if (m_running && m_clock.isValid() && m_clock.elapsed() >= SpeedWarmupMs) {
        const qint64 advanced = doneMs() - m_doneAtStart;
        if (advanced > 0)
            return double(advanced) / double(m_clock.elapsed());
    }
    return m_savedSpeed;
}

qint64 ArchiveScan::remainingMs() const
{
    const double rate = speed();
    if (rate <= 0.0)
        return -1;
    return qint64(double(std::max<qint64>(0, totalMs() - doneMs())) / rate);
}

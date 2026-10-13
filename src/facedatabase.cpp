#include "facedatabase.h"

#include "faceengine.h"

#include <QBuffer>
#include <QCache>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QReadWriteLock>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QThreadStorage>
#include <QUuid>
#include <QtAlgorithms>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>

namespace {

constexpr size_t Dimension = FaceEngine::EmbeddingSize;
constexpr size_t CodeWords = 8; // بصمة ثنائية من 512 بت لكل شخص
constexpr int StoredPhotoSize = 192; // أكبر بُعد لصورة الوجه المحفوظة
constexpr int StoredPhotoQuality = 88;
constexpr qsizetype TemplateBytes = qsizetype(sizeof(float) + Dimension);

constexpr quint32 CodesMagic = 0x58494c4d; // "MLIX"
constexpr quint32 MeansMagic = 0x4e4d4c4d; // "MLMN"
constexpr quint32 IndexVersion = 1;
constexpr qint64 CodesHeader = 32;
constexpr qint64 MeansHeader = 16;
constexpr qint64 CodeRecord = qint64(sizeof(qint64) + CodeWords * sizeof(quint64));
constexpr qint64 MeanRecord = qint64(sizeof(float) + Dimension);

constexpr size_t ExactRerankLimit = 4096; // حتى هذا العدد يُعاد ترتيب كل الأشخاص بدقة دون تصفية
constexpr size_t TemplateCandidates = 24; // الأقرب بالمتوسط تُقارن بكل بصماتهم
constexpr size_t ParallelScanLimit = 200000;

constexpr double TwoPi = 6.283185307179586;

const char *const DatabaseFile = "faces.db";
const char *const CodesFile = "index.codes";
const char *const MeansFile = "index.means";
const char *const LegacyFile = "persons.json";

// ───── ضغط البصمات: كل بصمة = معامل (float) ثم 512 قيمة int8 ─────

QByteArray packTemplates(const std::vector<std::vector<float>> &embeddings)
{
    QByteArray data;
    for (const std::vector<float> &embedding : embeddings) {
        if (embedding.size() != Dimension)
            continue;
        float peak = 0.0f;
        for (float v : embedding)
            peak = std::max(peak, std::abs(v));
        const float scale = peak > 0.0f ? peak / 127.0f : 1.0f;
        const qsizetype offset = data.size();
        data.resize(offset + TemplateBytes);
        memcpy(data.data() + offset, &scale, sizeof(scale));
        auto *values = reinterpret_cast<qint8 *>(data.data() + offset + sizeof(scale));
        for (size_t i = 0; i < Dimension; ++i)
            values[i] = qint8(std::lround(std::clamp(embedding[i] / scale, -127.0f, 127.0f)));
    }
    return data;
}

void unpackTemplates(const QByteArray &data, std::vector<float> &out)
{
    for (qsizetype offset = 0; offset + TemplateBytes <= data.size(); offset += TemplateBytes) {
        float scale = 0.0f;
        memcpy(&scale, data.constData() + offset, sizeof(scale));
        const auto *values = reinterpret_cast<const qint8 *>(data.constData() + offset + sizeof(scale));
        const size_t start = out.size();
        out.resize(start + Dimension);
        for (size_t i = 0; i < Dimension; ++i)
            out[start + i] = float(values[i]) * scale;
    }
}

// متوسط كل بصمات الشخص بعد التطبيع
std::vector<float> meanOf(const std::vector<float> &templates)
{
    std::vector<float> mean;
    if (templates.size() < Dimension)
        return mean;
    mean.assign(Dimension, 0.0f);
    for (size_t offset = 0; offset + Dimension <= templates.size(); offset += Dimension) {
        for (size_t i = 0; i < Dimension; ++i)
            mean[i] += templates[offset + i];
    }
    FaceEngine::normalize(mean);
    return mean;
}

// ───── البصمة الثنائية (SimHash): إشارة إسقاط البصمة على 512 اتجاهاً عشوائياً ثابتاً ─────
// احتمال اختلاف كل بت = الزاوية بين البصمتين / π، فمسافة هامنج تقرّب التشابه بسرعة كبيرة

const std::vector<float> &projections()
{
    static const std::vector<float> matrix = [] {
        // مولّد حتمي مكتوب هنا حتى تبقى الاتجاهات نفسها على كل المنصات والمترجمات
        std::vector<float> m(CodeWords * 64 * Dimension);
        quint64 state = 0x6d616c616d6968ULL;
        auto next = [&state] {
            state += 0x9e3779b97f4a7c15ULL;
            quint64 z = state;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        };
        for (size_t i = 0; i < m.size(); i += 2) {
            const double u1 = (double(next() >> 11) + 1.0) / 9007199254740993.0;
            const double u2 = double(next() >> 11) / 9007199254740992.0;
            const double radius = std::sqrt(-2.0 * std::log(u1));
            m[i] = float(radius * std::cos(TwoPi * u2));
            if (i + 1 < m.size())
                m[i + 1] = float(radius * std::sin(TwoPi * u2));
        }
        return m;
    }();
    return matrix;
}

std::array<quint64, CodeWords> codeOf(const float *embedding)
{
    const std::vector<float> &matrix = projections();
    std::array<quint64, CodeWords> code{};
    for (size_t bit = 0; bit < CodeWords * 64; ++bit) {
        if (FaceEngine::dot(matrix.data() + bit * Dimension, embedding, Dimension) > 0.0f)
            code[bit / 64] |= quint64(1) << (bit % 64);
    }
    return code;
}

// ───── اتصال SQLite للقراءة خاص بكل خيط (اتصالات Qt لا تُشارك بين الخيوط) ─────

struct ThreadConnections
{
    QStringList names;
    ~ThreadConnections()
    {
        for (const QString &name : std::as_const(names))
            QSqlDatabase::removeDatabase(name);
    }
};

QThreadStorage<ThreadConnections *> s_connections;

QSqlDatabase readConnection(const QString &path)
{
    if (!s_connections.hasLocalData())
        s_connections.setLocalData(new ThreadConnections);
    const QString name = QStringLiteral("malamih-read-%1-%2")
                             .arg(quintptr(QThread::currentThread()), 0, 16)
                             .arg(qHash(path), 0, 16);
    if (!QSqlDatabase::contains(name)) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        db.open();
        s_connections.localData()->names.append(name);
    }
    return QSqlDatabase::database(name, false);
}

QString likePattern(const QString &filter)
{
    QString escaped = filter;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('%'), QLatin1String("\\%"));
    escaped.replace(QLatin1Char('_'), QLatin1String("\\_"));
    return QLatin1Char('%') + escaped + QLatin1Char('%');
}

} // namespace

// ───────────── فهرس المطابقة ─────────────

class Gallery
{
public:
    explicit Gallery(QString databasePath)
        : m_databasePath(std::move(databasePath))
    {
        m_cache.setMaxCost(64 * 1024); // بالكيلوبايت
    }

    // يقرأ الفهرس من القرص؛ يعيد false إذا لم يطابق القاعدة فيجب إعادة بنائه
    bool load(const QString &directory, quint64 generation)
    {
        QWriteLocker locker(&m_lock);
        QMutexLocker fileLocker(&m_fileMutex);
        clear();
        m_codes.setFileName(QDir(directory).filePath(QString::fromLatin1(CodesFile)));
        m_means.setFileName(QDir(directory).filePath(QString::fromLatin1(MeansFile)));
        // ملف المتوسطات يُقرأ بقفزات عشوائية صغيرة، فالقراءة دون ذاكرة Qt الوسيطة أسرع بكثير
        if (!m_codes.open(QIODevice::ReadWrite) || !m_means.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
            return false;

        quint32 header[4] = {};
        quint64 stored[2] = {};
        if (m_codes.read(reinterpret_cast<char *>(header), sizeof(header)) != qint64(sizeof(header))
            || m_codes.read(reinterpret_cast<char *>(stored), sizeof(stored)) != qint64(sizeof(stored))
            || header[0] != CodesMagic || header[1] != IndexVersion || header[2] != CodeWords * 64
            || stored[0] != generation || generation == 0)
            return false;
        const quint64 slotCount = stored[1];
        if (m_codes.size() < CodesHeader + qint64(slotCount) * CodeRecord
            || m_means.size() < MeansHeader + qint64(slotCount) * MeanRecord)
            return false;

        // قراءة كتلة واحدة كبيرة أسرع بكثير من قراءة كل سجل وحده
        const QByteArray data = m_codes.read(qint64(slotCount) * CodeRecord);
        if (data.size() != qsizetype(slotCount) * CodeRecord)
            return false;
        m_ids.resize(slotCount);
        m_bits.resize(slotCount * CodeWords);
        for (quint64 slot = 0; slot < slotCount; ++slot) {
            const char *record = data.constData() + slot * CodeRecord;
            memcpy(&m_ids[slot], record, sizeof(qint64));
            memcpy(&m_bits[slot * CodeWords], record + sizeof(qint64), CodeWords * sizeof(quint64));
            if (m_ids[slot] > 0) {
                m_slots.insert(m_ids[slot], slot);
                ++m_alive;
            } else {
                m_free.push_back(slot);
            }
        }
        m_generation = generation;
        return true;
    }

    // ملفات فارغة، تمهيداً لإعادة البناء
    bool reset(const QString &directory)
    {
        QWriteLocker locker(&m_lock);
        QMutexLocker fileLocker(&m_fileMutex);
        clear();
        m_codes.setFileName(QDir(directory).filePath(QString::fromLatin1(CodesFile)));
        m_means.setFileName(QDir(directory).filePath(QString::fromLatin1(MeansFile)));
        if (!m_codes.open(QIODevice::ReadWrite | QIODevice::Truncate)
            || !m_means.open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Unbuffered))
            return false;
        const quint32 means[4] = {MeansMagic, IndexVersion, quint32(Dimension), 0};
        m_means.write(reinterpret_cast<const char *>(means), sizeof(means));
        m_generation = 1; // أي قيمة غير صفرية؛ writeHeader(0) يعلّم الفهرس كغير مكتمل
        writeHeader(0);
        return true;
    }

    // قبل أي تعديل: يُعلَّم الفهرس كغير مكتمل حتى تُحفظ القاعدة، فإن انقطع البرنامج يُعاد بناؤه
    void markDirty()
    {
        QMutexLocker fileLocker(&m_fileMutex);
        if (m_generation != 0) {
            writeHeader(0);
            m_generation = 0;
        }
    }

    void setGeneration(quint64 generation)
    {
        QReadLocker locker(&m_lock);
        QMutexLocker fileLocker(&m_fileMutex);
        m_generation = generation;
        writeHeader(generation);
        m_codes.flush();
        m_means.flush();
    }

    void upsert(qint64 personId, const std::vector<float> &mean, std::vector<float> templates, const QString &name)
    {
        if (mean.size() != Dimension)
            return remove(personId);
        const std::array<quint64, CodeWords> code = codeOf(mean.data());
        float peak = 0.0f;
        for (float v : mean)
            peak = std::max(peak, std::abs(v));
        QByteArray meanRecord(MeanRecord, Qt::Uninitialized);
        const float scale = peak > 0.0f ? peak / 127.0f : 1.0f;
        memcpy(meanRecord.data(), &scale, sizeof(scale));
        auto *values = reinterpret_cast<qint8 *>(meanRecord.data() + sizeof(scale));
        for (size_t i = 0; i < Dimension; ++i)
            values[i] = qint8(std::lround(std::clamp(mean[i] / scale, -127.0f, 127.0f)));

        QWriteLocker locker(&m_lock);
        quint64 slot = 0;
        auto existing = m_slots.constFind(personId);
        if (existing != m_slots.constEnd()) {
            slot = *existing;
        } else if (!m_free.empty()) {
            slot = m_free.back();
            m_free.pop_back();
            ++m_alive;
        } else {
            slot = m_ids.size();
            m_ids.push_back(0);
            m_bits.resize(m_bits.size() + CodeWords);
            ++m_alive;
        }
        m_ids[slot] = personId;
        std::copy(code.begin(), code.end(), m_bits.begin() + qsizetype(slot * CodeWords));
        m_slots.insert(personId, slot);
        writeSlot(slot, &meanRecord);
        if (slot + 1 == m_ids.size()) {
            QMutexLocker fileLocker(&m_fileMutex);
            writeHeader(m_generation);
        }
        locker.unlock();

        // بدون البصمات الكاملة (أثناء إعادة البناء) تُقرأ من القاعدة عند الحاجة
        ++m_revision;
        QMutexLocker cacheLocker(&m_cacheMutex);
        if (templates.empty()) {
            m_cache.remove(personId);
            return;
        }
        const int cost = std::max(1, int(templates.size() * sizeof(float) / 1024));
        m_cache.insert(personId, new Entry{std::move(templates), name}, cost);
    }

    void remove(qint64 personId)
    {
        {
            QWriteLocker locker(&m_lock);
            auto it = m_slots.find(personId);
            if (it != m_slots.end()) {
                const quint64 slot = *it;
                m_slots.erase(it);
                m_ids[slot] = 0;
                m_free.push_back(slot);
                --m_alive;
                writeSlot(slot, nullptr);
            }
        }
        forget(personId);
    }

    void forget(qint64 personId)
    {
        ++m_revision;
        QMutexLocker cacheLocker(&m_cacheMutex);
        m_cache.remove(personId);
    }

    qint64 size() const
    {
        QReadLocker locker(&m_lock);
        return qint64(m_alive);
    }

    MatchResult search(const std::vector<float> &embedding, float threshold);
    quint64 revision() const { return m_revision; }

private:
    struct Entry
    {
        std::vector<float> templates;
        QString name;
    };

    void clear()
    {
        m_codes.close();
        m_means.close();
        m_ids.clear();
        m_bits.clear();
        m_slots.clear();
        m_free.clear();
        m_alive = 0;
        QMutexLocker cacheLocker(&m_cacheMutex);
        m_cache.clear();
    }

    // يُستدعى مع m_fileMutex
    void writeHeader(quint64 generation)
    {
        const quint32 header[4] = {CodesMagic, IndexVersion, quint32(CodeWords * 64), 0};
        const quint64 stored[2] = {generation, quint64(m_ids.size())};
        m_codes.seek(0);
        m_codes.write(reinterpret_cast<const char *>(header), sizeof(header));
        m_codes.write(reinterpret_cast<const char *>(stored), sizeof(stored));
    }

    // يُستدعى مع قفل الكتابة
    void writeSlot(quint64 slot, const QByteArray *meanRecord)
    {
        QMutexLocker fileLocker(&m_fileMutex);
        m_codes.seek(CodesHeader + qint64(slot) * CodeRecord);
        m_codes.write(reinterpret_cast<const char *>(&m_ids[slot]), sizeof(qint64));
        m_codes.write(reinterpret_cast<const char *>(&m_bits[slot * CodeWords]), CodeWords * sizeof(quint64));
        if (meanRecord) {
            m_means.seek(MeansHeader + qint64(slot) * MeanRecord);
            m_means.write(*meanRecord);
        }
    }

    // بصمات المرشحين وأسماؤهم: من الذاكرة المؤقتة، والباقي باستعلامين فقط مهما كان عددهم
    QHash<qint64, Entry> loadEntries(const std::vector<qint64> &ids)
    {
        QHash<qint64, Entry> entries;
        QStringList missing;
        {
            QMutexLocker cacheLocker(&m_cacheMutex);
            for (qint64 id : ids) {
                if (const Entry *cached = m_cache.object(id))
                    entries.insert(id, *cached);
                else
                    missing.append(QString::number(id));
            }
        }
        if (missing.isEmpty())
            return entries;
        // الأرقام من الفهرس نفسه (أعداد صحيحة)، فإدراجها في نص الاستعلام آمن
        const QString list = missing.join(QLatin1Char(','));
        QSqlDatabase db = readConnection(m_databasePath);
        QSqlQuery query(db);
        query.setForwardOnly(true);
        QHash<qint64, Entry> loaded;
        if (query.exec(QStringLiteral("SELECT id, name FROM persons WHERE id IN (%1)").arg(list))) {
            while (query.next())
                loaded[query.value(0).toLongLong()].name = query.value(1).toString();
        }
        if (query.exec(QStringLiteral("SELECT person_id, templates FROM photos WHERE person_id IN (%1)").arg(list))) {
            while (query.next()) {
                auto it = loaded.find(query.value(0).toLongLong());
                if (it != loaded.end())
                    unpackTemplates(query.value(1).toByteArray(), it->templates);
            }
        }
        QMutexLocker cacheLocker(&m_cacheMutex);
        for (auto it = loaded.cbegin(); it != loaded.cend(); ++it) {
            const int cost = std::max(1, int(it->templates.size() * sizeof(float) / 1024));
            m_cache.insert(it.key(), new Entry(*it), cost);
            entries.insert(it.key(), *it);
        }
        return entries;
    }

    QString m_databasePath;
    mutable QReadWriteLock m_lock;   // يحمي المصفوفات في الذاكرة
    QMutex m_fileMutex;              // يحمي ملفي الفهرس
    QFile m_codes;
    QFile m_means;
    std::vector<qint64> m_ids;       // رقم الشخص في كل خانة (0 = فارغة)
    std::vector<quint64> m_bits;     // البصمات الثنائية متتالية
    QHash<qint64, quint64> m_slots;  // رقم الشخص ← خانته
    std::vector<quint64> m_free;
    size_t m_alive = 0;
    quint64 m_generation = 0;
    QMutex m_cacheMutex;
    QCache<qint64, Entry> m_cache;   // بصمات الأشخاص الأقرب مؤخراً، لتجنب قراءتها من القاعدة
    std::atomic<quint64> m_revision{1};
};

MatchResult Gallery::search(const std::vector<float> &embedding, float threshold)
{
    MatchResult result;
    if (embedding.size() != Dimension)
        return result;

    struct Candidate
    {
        quint64 slot;
        qint64 personId;
        float score;
    };
    std::vector<Candidate> candidates;
    {
        QReadLocker locker(&m_lock);
        const size_t slotCount = m_ids.size();
        if (m_alive == 0)
            return result;

        // 1) تصفية سريعة بمسافة هامنج بين البصمات الثنائية
        std::vector<quint64> chosen;
        if (m_alive <= ExactRerankLimit) {
            for (quint64 slot = 0; slot < slotCount; ++slot) {
                if (m_ids[slot] > 0)
                    chosen.push_back(slot);
            }
        } else {
            const std::array<quint64, CodeWords> code = codeOf(embedding.data());
            std::vector<quint16> distances(slotCount);
            auto scan = [&](size_t begin, size_t end) {
                for (size_t slot = begin; slot < end; ++slot) {
                    const quint64 *bits = &m_bits[slot * CodeWords];
                    uint d = 0;
                    for (size_t w = 0; w < CodeWords; ++w)
                        d += qPopulationCount(bits[w] ^ code[w]);
                    distances[slot] = m_ids[slot] > 0 ? quint16(d) : quint16(0xffff);
                }
            };
            const size_t threads = slotCount >= ParallelScanLimit ? std::max(1u, std::min(8u, std::thread::hardware_concurrency())) : 1;
            if (threads > 1) {
                std::vector<std::thread> workers;
                const size_t chunk = (slotCount + threads - 1) / threads;
                for (size_t t = 0; t < threads; ++t)
                    workers.emplace_back(scan, std::min(slotCount, t * chunk), std::min(slotCount, (t + 1) * chunk));
                for (std::thread &worker : workers)
                    worker.join();
            } else {
                scan(0, slotCount);
            }

            // أقرب K بعدّ المسافات (قيمها 0..512) دون فرز الملايين
            const size_t wanted = std::clamp<size_t>(m_alive / 256, 2048, 8192);
            std::array<size_t, CodeWords * 64 + 1> histogram{};
            for (quint16 d : distances) {
                if (d <= CodeWords * 64)
                    ++histogram[d];
            }
            size_t cutoff = 0, total = 0;
            while (cutoff < histogram.size() && total + histogram[cutoff] < wanted)
                total += histogram[cutoff++];
            size_t atCutoff = wanted - total;
            for (quint64 slot = 0; slot < slotCount; ++slot) {
                const quint16 d = distances[slot];
                if (d < cutoff || (d == cutoff && atCutoff > 0 && atCutoff--))
                    chosen.push_back(slot);
            }
        }

        // 2) ترتيب المرشحين بدقة بمتوسط بصمات كل منهم (يُقرأ من القرص بالترتيب)
        QByteArray record(MeanRecord, Qt::Uninitialized);
        QMutexLocker fileLocker(&m_fileMutex);
        candidates.reserve(chosen.size());
        for (quint64 slot : chosen) {
            if (!m_means.seek(MeansHeader + qint64(slot) * MeanRecord) || m_means.read(record.data(), MeanRecord) != MeanRecord)
                continue;
            float scale = 0.0f;
            memcpy(&scale, record.constData(), sizeof(scale));
            const auto *values = reinterpret_cast<const qint8 *>(record.constData() + sizeof(scale));
            float dot = 0.0f;
            for (size_t i = 0; i < Dimension; ++i)
                dot += float(values[i]) * embedding[i];
            candidates.push_back({slot, m_ids[slot], dot * scale});
        }
    }

    const size_t keep = std::min(TemplateCandidates, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + qsizetype(keep), candidates.end(),
                      [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    candidates.resize(keep);

    // 3) الأقرب يُقارنون بكل بصماتهم المحفوظة؛ النتيجة أعلى تشابه مع أي منها أو مع المتوسط
    std::vector<qint64> ids;
    for (const Candidate &candidate : candidates)
        ids.push_back(candidate.personId);
    const QHash<qint64, Entry> entries = loadEntries(ids);
    float best = -1.0f;
    float second = -1.0f;
    qint64 bestId = 0;
    QString bestName;
    for (const Candidate &candidate : candidates) {
        const auto found = entries.constFind(candidate.personId);
        if (found == entries.constEnd())
            continue;
        const Entry &entry = *found;
        float score = candidate.score;
        for (size_t offset = 0; offset + Dimension <= entry.templates.size(); offset += Dimension)
            score = std::max(score, FaceEngine::dot(entry.templates.data() + offset, embedding.data(), Dimension));
        if (score > best) {
            second = best;
            best = score;
            bestId = candidate.personId;
            bestName = entry.name;
        } else if (score > second) {
            second = score;
        }
    }
    if (bestId == 0)
        return result;

    result.score = std::max(0.0f, best);
    // مطابقة قريبة جداً من شخص آخر تُعد غير محسومة
    const bool clearWinner = best - second >= 0.05f;
    if (best >= threshold && clearWinner)
        result.status = MatchStatus::Confirmed;
    else if (best >= threshold - 0.1f)
        result.status = MatchStatus::Possible;
    if (result.status != MatchStatus::Unknown) {
        result.personId = QString::number(bestId);
        result.name = bestName;
    }
    return result;
}

// ───────────── قاعدة البيانات ─────────────

FaceDatabase::FaceDatabase(QObject *parent)
    : QObject(parent)
{
}

FaceDatabase::~FaceDatabase()
{
    close();
}

QString FaceDatabase::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/database");
}

void FaceDatabase::close()
{
    m_gallery.reset();
    if (m_db.isValid()) {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool FaceDatabase::exec(const QString &sql, const QVariantList &values) const
{
    QSqlQuery query(m_db);
    if (!query.prepare(sql))
        return false;
    for (const QVariant &value : values)
        query.addBindValue(value);
    return query.exec();
}

bool FaceDatabase::open(const QString &directory, QString *error)
{
    close();
    m_directory = directory;
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        close();
        return false;
    };
    if (!QDir().mkpath(directory))
        return fail(QStringLiteral("تعذر إنشاء مجلد قاعدة البيانات: %1").arg(QDir::toNativeSeparators(directory)));

    const QString path = QDir(directory).filePath(QString::fromLatin1(DatabaseFile));
    const bool fresh = !QFile::exists(path);
    m_connection = QStringLiteral("malamih-%1").arg(quintptr(this), 0, 16);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.setDatabaseName(path);
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open())
        return fail(QStringLiteral("تعذر فتح قاعدة البيانات: %1").arg(m_db.lastError().text()));

    // WAL: القراءة من خيوط أخرى مستمرة أثناء الكتابة، والحفظ سريع دون انتظار القرص في كل عملية
    const QStringList setup{
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("PRAGMA synchronous=NORMAL"),
        QStringLiteral("PRAGMA cache_size=-65536"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS persons(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL,"
                       " national_id TEXT, phone TEXT, notes TEXT, created INTEGER,"
                       " photo_count INTEGER NOT NULL DEFAULT 0, cover_photo INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS persons_national_id ON persons(national_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS persons_phone ON persons(phone)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS photos(id INTEGER PRIMARY KEY AUTOINCREMENT, person_id INTEGER NOT NULL,"
                       " quality REAL, added INTEGER, image BLOB, templates BLOB)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS photos_person ON photos(person_id)"),
        QStringLiteral("INSERT OR IGNORE INTO meta(key, value) VALUES('model', 'insightface-buffalo_l')"),
        QStringLiteral("INSERT OR IGNORE INTO meta(key, value) VALUES('generation', '1')"),
    };
    for (const QString &sql : setup) {
        if (!exec(sql))
            return fail(QStringLiteral("تعذر تهيئة قاعدة البيانات: %1").arg(m_db.lastError().text()));
    }

    if (fresh && QFile::exists(QDir(directory).filePath(QString::fromLatin1(LegacyFile))) && !migrateLegacy(error)) {
        close();
        return false;
    }

    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM persons), (SELECT COUNT(*) FROM photos),"
                              " (SELECT value FROM meta WHERE key = 'generation')"));
    quint64 generation = 1;
    if (query.next()) {
        m_personCount = query.value(0).toLongLong();
        m_photoCount = query.value(1).toLongLong();
        generation = query.value(2).toULongLong();
    }
    query.finish();

    m_gallery = std::make_shared<Gallery>(path);
    if (!m_gallery->load(directory, generation)) {
        // الفهرس مفقود أو قديم: يُبنى من جديد من بصمات القاعدة
        if (!m_gallery->reset(directory))
            return fail(QStringLiteral("تعذر إنشاء فهرس المطابقة"));
        QSqlQuery photos(m_db);
        photos.setForwardOnly(true);
        photos.exec(QStringLiteral("SELECT p.person_id, p.templates, s.name FROM photos p JOIN persons s ON s.id = p.person_id"
                                   " ORDER BY p.person_id"));
        qint64 current = 0;
        QString name;
        std::vector<float> templates;
        auto flush = [&] {
            if (current > 0 && !templates.empty()) {
                const std::vector<float> mean = meanOf(templates);
                m_gallery->upsert(current, mean, {}, name);
            }
            templates.clear();
        };
        while (photos.next()) {
            const qint64 personId = photos.value(0).toLongLong();
            if (personId != current) {
                flush();
                current = personId;
                name = photos.value(2).toString();
            }
            unpackTemplates(photos.value(1).toByteArray(), templates);
        }
        flush();
        // البناء لا يغيّر القاعدة، فيُعتمد برقم إصدارها الحالي
        m_gallery->setGeneration(generation);
    }
    emit personsReset();
    emit changed();
    return true;
}

QString FaceDatabase::directory() const
{
    return m_directory;
}

namespace {

Person personFromQuery(const QSqlQuery &query)
{
    Person person;
    person.id = query.value(0).toString();
    person.name = query.value(1).toString();
    person.nationalId = query.value(2).toString();
    person.phone = query.value(3).toString();
    person.notes = query.value(4).toString();
    person.created = QDateTime::fromSecsSinceEpoch(query.value(5).toLongLong());
    person.photoCount = query.value(6).toInt();
    person.coverPhoto = query.value(7).toLongLong();
    return person;
}

const QString PersonColumns = QStringLiteral("id, name, national_id, phone, notes, created, photo_count, cover_photo");

} // namespace

Person FaceDatabase::person(const QString &id) const
{
    Person person;
    if (!m_db.isOpen())
        return person;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT %1 FROM persons WHERE id = ?").arg(PersonColumns));
    query.addBindValue(id.toLongLong());
    if (!query.exec() || !query.next())
        return person;
    person = personFromQuery(query);
    query.prepare(QStringLiteral("SELECT id, quality FROM photos WHERE person_id = ? ORDER BY id"));
    query.addBindValue(id.toLongLong());
    if (query.exec()) {
        while (query.next())
            person.photos.append(FacePhoto{query.value(0).toLongLong(), query.value(1).toFloat()});
    }
    return person;
}

namespace {

// شرط البحث: الأرقام تُبحث كبداية لرقم الهوية أو الهاتف عبر الفهرس (فورية مع الملايين)،
// والنص يُبحث داخل الاسم
QString filterCondition(const QString &text, QVariantList &values)
{
    if (text.isEmpty())
        return QStringLiteral("1");
    bool digits = true;
    for (QChar c : text)
        digits = digits && (c.isDigit() || c == QLatin1Char('+'));
    if (digits) {
        const QString upper = text + QChar(0xffff);
        values << text << upper << text << upper;
        return QStringLiteral("((national_id >= ? AND national_id < ?) OR (phone >= ? AND phone < ?))");
    }
    values << likePattern(text);
    return QStringLiteral("name LIKE ? ESCAPE '\\'");
}

} // namespace

QList<Person> FaceDatabase::findPersons(const QString &filter, qint64 beforeId, int limit) const
{
    QList<Person> persons;
    if (!m_db.isOpen())
        return persons;
    QVariantList values{beforeId > 0 ? beforeId : std::numeric_limits<qint64>::max()};
    const QString condition = filterCondition(filter.trimmed(), values);
    values << limit;
    QSqlQuery query(m_db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral("SELECT %1 FROM persons WHERE id < ? AND %2 ORDER BY id DESC LIMIT ?").arg(PersonColumns, condition));
    for (const QVariant &value : std::as_const(values))
        query.addBindValue(value);
    if (query.exec()) {
        while (query.next())
            persons.append(personFromQuery(query));
    }
    return persons;
}

qint64 FaceDatabase::countPersons(const QString &filter, qint64 cap) const
{
    const QString text = filter.trimmed();
    if (text.isEmpty())
        return std::min(m_personCount, cap);
    QVariantList values;
    const QString condition = filterCondition(text, values);
    values << cap;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM (SELECT 1 FROM persons WHERE %1 LIMIT ?)").arg(condition));
    for (const QVariant &value : std::as_const(values))
        query.addBindValue(value);
    return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
}

void FaceDatabase::beginBatch()
{
    if (m_batchDepth++ == 0 && m_db.isOpen()) {
        m_db.transaction();
        if (m_gallery)
            m_gallery->markDirty();
    }
}

bool FaceDatabase::endBatch()
{
    if (m_batchDepth == 0 || --m_batchDepth > 0)
        return true;
    if (!m_db.isOpen())
        return false;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key = 'generation'"));
    query.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'generation'"));
    const quint64 generation = query.next() ? query.value(0).toULongLong() : 0;
    query.finish();
    const bool ok = m_db.commit();
    if (ok && m_gallery && generation > 0)
        m_gallery->setGeneration(generation);
    if (m_batchReset)
        emit personsReset();
    if (m_batchChanged)
        emit changed();
    m_batchReset = m_batchChanged = false;
    return ok;
}

// كل عملية كتابة معاملة مستقلة إلا داخل beginBatch/endBatch
void FaceDatabase::notify(const QString &personId, bool reset)
{
    if (m_batchDepth > 0) {
        m_batchReset = m_batchReset || reset;
        m_batchChanged = true;
        return;
    }
    if (reset)
        emit personsReset();
    else if (!personId.isEmpty())
        emit personUpdated(personId);
    emit changed();
}

QString FaceDatabase::addPerson(Person person)
{
    if (!m_db.isOpen())
        return {};
    beginBatch();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO persons(name, national_id, phone, notes, created) VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(person.name);
    query.addBindValue(person.nationalId);
    query.addBindValue(person.phone);
    query.addBindValue(person.notes);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    const bool ok = query.exec();
    const QString id = ok ? query.lastInsertId().toString() : QString();
    if (ok) {
        ++m_personCount;
        notify(id, true);
    }
    endBatch();
    return id;
}

bool FaceDatabase::updatePerson(const Person &person)
{
    const qint64 id = person.id.toLongLong();
    if (!m_db.isOpen() || id <= 0)
        return false;
    beginBatch();
    const bool ok = exec(QStringLiteral("UPDATE persons SET name = ?, national_id = ?, phone = ?, notes = ? WHERE id = ?"),
                         {person.name, person.nationalId, person.phone, person.notes, id});
    if (ok) {
        m_gallery->forget(id); // الاسم المخزن مؤقتاً للمطابقة
        notify(person.id, false);
    }
    return endBatch() && ok;
}

bool FaceDatabase::removePerson(const QString &personId)
{
    const qint64 id = personId.toLongLong();
    if (!m_db.isOpen() || id <= 0)
        return false;
    beginBatch();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM photos WHERE person_id = ?"));
    query.addBindValue(id);
    const qint64 photos = query.exec() && query.next() ? query.value(0).toLongLong() : 0;
    query.finish();
    const bool ok = exec(QStringLiteral("DELETE FROM photos WHERE person_id = ?"), {id})
        && exec(QStringLiteral("DELETE FROM persons WHERE id = ?"), {id});
    if (ok) {
        m_gallery->remove(id);
        --m_personCount;
        m_photoCount -= photos;
        notify(personId, true);
    } else {
        m_db.rollback();
        m_batchDepth = 0;
        return false;
    }
    return endBatch();
}

// يعيد حساب متوسط بصمات الشخص ويحدّث فهرس المطابقة
bool FaceDatabase::refreshPerson(qint64 personId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT name FROM persons WHERE id = ?"));
    query.addBindValue(personId);
    if (!query.exec() || !query.next())
        return false;
    const QString name = query.value(0).toString();
    query.prepare(QStringLiteral("SELECT templates FROM photos WHERE person_id = ?"));
    query.addBindValue(personId);
    if (!query.exec())
        return false;
    std::vector<float> templates;
    while (query.next())
        unpackTemplates(query.value(0).toByteArray(), templates);
    if (templates.empty()) {
        m_gallery->remove(personId);
        return true;
    }
    const std::vector<float> mean = meanOf(templates);
    m_gallery->upsert(personId, mean, std::move(templates), name);
    return true;
}

bool FaceDatabase::addPhoto(const QString &personId, const QImage &crop,
                            const std::vector<std::vector<float>> &embeddings, float quality)
{
    const qint64 id = personId.toLongLong();
    if (!m_db.isOpen() || id <= 0)
        return false;
    const QByteArray templates = packTemplates(embeddings);
    if (templates.isEmpty())
        return false;

    // صورة الوجه للعرض فقط (البصمات محفوظة منفصلة)، فتكفي دقة متوسطة بحجم صغير
    QByteArray image;
    if (!crop.isNull()) {
        const QImage stored = std::max(crop.width(), crop.height()) > StoredPhotoSize
            ? crop.scaled(StoredPhotoSize, StoredPhotoSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : crop;
        QBuffer buffer(&image);
        buffer.open(QIODevice::WriteOnly);
        stored.save(&buffer, "JPG", StoredPhotoQuality);
    }

    beginBatch();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO photos(person_id, quality, added, image, templates) VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(id);
    query.addBindValue(double(quality));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(image);
    query.addBindValue(templates);
    bool ok = query.exec();
    const qint64 photoId = ok ? query.lastInsertId().toLongLong() : 0;
    ok = ok
        && exec(QStringLiteral("UPDATE persons SET photo_count = photo_count + 1,"
                               " cover_photo = CASE WHEN cover_photo = 0 THEN ? ELSE cover_photo END WHERE id = ?"),
                {photoId, id})
        && refreshPerson(id);
    if (!ok) {
        m_db.rollback();
        m_batchDepth = 0;
        refreshPerson(id);
        return false;
    }
    ++m_photoCount;
    notify(personId, false);
    return endBatch();
}

bool FaceDatabase::removePhoto(const QString &personId, qint64 photoId)
{
    const qint64 id = personId.toLongLong();
    if (!m_db.isOpen() || id <= 0)
        return false;
    beginBatch();
    const bool ok = exec(QStringLiteral("DELETE FROM photos WHERE id = ? AND person_id = ?"), {photoId, id})
        && exec(QStringLiteral("UPDATE persons SET photo_count = (SELECT COUNT(*) FROM photos WHERE person_id = ?1),"
                               " cover_photo = COALESCE((SELECT MIN(id) FROM photos WHERE person_id = ?1), 0) WHERE id = ?1"),
                {id})
        && refreshPerson(id);
    if (!ok) {
        m_db.rollback();
        m_batchDepth = 0;
        refreshPerson(id);
        return false;
    }
    --m_photoCount;
    notify(personId, false);
    return endBatch();
}

QImage FaceDatabase::photo(qint64 photoId, int maxSize) const
{
    if (!m_db.isOpen() || photoId <= 0)
        return {};
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT image FROM photos WHERE id = ?"));
    query.addBindValue(photoId);
    if (!query.exec() || !query.next())
        return {};
    QByteArray data = query.value(0).toByteArray();
    QBuffer buffer(&data);
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    if (maxSize > 0 && size.isValid() && std::max(size.width(), size.height()) > maxSize)
        reader.setScaledSize(size.scaled(maxSize, maxSize, Qt::KeepAspectRatio));
    return reader.read();
}

quint64 FaceDatabase::revision(const Gallery &gallery)
{
    return gallery.revision();
}

GalleryPtr FaceDatabase::gallery() const
{
    return m_gallery;
}

MatchResult FaceDatabase::identify(Gallery &gallery, const std::vector<float> &embedding, float threshold)
{
    return gallery.search(embedding, threshold);
}

// ───────────── ترحيل القواعد القديمة (persons.json) ─────────────

namespace {

struct LegacyPhoto
{
    QString file;
    float quality = 0.0f;
    std::vector<float> embeddings;
};

void appendLegacyEmbeddings(const QJsonArray &array, std::vector<float> &out)
{
    for (const QJsonValue &value : array) {
        const QByteArray raw = QByteArray::fromBase64(value.toString().toLatin1());
        const size_t count = size_t(raw.size()) / sizeof(float);
        const size_t offset = out.size();
        out.resize(offset + count);
        memcpy(out.data() + offset, raw.constData(), count * sizeof(float));
    }
}

// الإصدار 2 حفظ البصمات في photos/<id>/embeddings.bin
QHash<QString, std::vector<float>> readLegacyEmbeddings(const QString &path)
{
    QHash<QString, std::vector<float>> blocks;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return blocks;
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
    if (!read(magic) || !read(version) || !read(photos) || magic != 0x4d455148 || version != 1)
        return blocks;
    for (quint32 i = 0; i < photos; ++i) {
        quint32 nameSize = 0, count = 0;
        if (!read(nameSize) || position + qsizetype(nameSize) > data.size())
            break;
        const QString name = QString::fromUtf8(data.constData() + position, qsizetype(nameSize));
        position += nameSize;
        if (!read(count) || position + qsizetype(count) * qsizetype(sizeof(float)) > data.size())
            break;
        std::vector<float> block(count);
        memcpy(block.data(), data.constData() + position, count * sizeof(float));
        position += qsizetype(count) * qsizetype(sizeof(float));
        blocks.insert(name, std::move(block));
    }
    return blocks;
}

} // namespace

bool FaceDatabase::migrateLegacy(QString *error)
{
    const QDir dir(m_directory);
    QFile file(dir.filePath(QString::fromLatin1(LegacyFile)));
    if (!file.open(QIODevice::ReadOnly))
        return true;
    const QJsonArray persons = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("persons")).toArray();
    file.close();

    m_db.transaction();
    for (const QJsonValue &value : persons) {
        const QJsonObject object = value.toObject();
        const QString oldId = object.value(QStringLiteral("id")).toString();
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("INSERT INTO persons(name, national_id, phone, notes, created) VALUES(?, ?, ?, ?, ?)"));
        query.addBindValue(object.value(QStringLiteral("name")).toString());
        query.addBindValue(object.value(QStringLiteral("nationalId")).toString());
        query.addBindValue(object.value(QStringLiteral("phone")).toString());
        query.addBindValue(object.value(QStringLiteral("notes")).toString());
        const QDateTime created = QDateTime::fromString(object.value(QStringLiteral("created")).toString(), Qt::ISODate);
        query.addBindValue(created.isValid() ? created.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch());
        if (!query.exec())
            continue;
        const qint64 newId = query.lastInsertId().toLongLong();

        const QString folder = dir.filePath(QStringLiteral("photos/") + oldId);
        const QHash<QString, std::vector<float>> blocks = readLegacyEmbeddings(QDir(folder).filePath(QStringLiteral("embeddings.bin")));
        qint64 cover = 0;
        int count = 0;
        for (const QJsonValue &photoValue : object.value(QStringLiteral("photos")).toArray()) {
            const QJsonObject photo = photoValue.toObject();
            const QString name = photo.value(QStringLiteral("file")).toString();
            std::vector<float> flat = blocks.value(name);
            if (photo.contains(QStringLiteral("embeddings")))
                appendLegacyEmbeddings(photo.value(QStringLiteral("embeddings")).toArray(), flat);
            std::vector<std::vector<float>> embeddings;
            for (size_t offset = 0; offset + Dimension <= flat.size(); offset += Dimension)
                embeddings.emplace_back(flat.begin() + qsizetype(offset), flat.begin() + qsizetype(offset + Dimension));
            if (embeddings.empty())
                continue;
            QFile imageFile(QDir(folder).filePath(name));
            const QByteArray image = imageFile.open(QIODevice::ReadOnly) ? imageFile.readAll() : QByteArray();
            QSqlQuery insert(m_db);
            insert.prepare(QStringLiteral("INSERT INTO photos(person_id, quality, added, image, templates) VALUES(?, ?, ?, ?, ?)"));
            insert.addBindValue(newId);
            insert.addBindValue(photo.value(QStringLiteral("quality")).toDouble());
            insert.addBindValue(QDateTime::currentSecsSinceEpoch());
            insert.addBindValue(image);
            insert.addBindValue(packTemplates(embeddings));
            if (insert.exec()) {
                ++count;
                if (cover == 0)
                    cover = insert.lastInsertId().toLongLong();
            }
        }
        exec(QStringLiteral("UPDATE persons SET photo_count = ?, cover_photo = ? WHERE id = ?"), {count, cover, newId});
    }
    exec(QStringLiteral("UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key = 'generation'"));
    if (!m_db.commit()) {
        if (error)
            *error = QStringLiteral("تعذر ترحيل قاعدة البيانات القديمة: %1").arg(m_db.lastError().text());
        return false;
    }
    // الملفات القديمة تبقى نسخة احتياطية، ولا تُرحَّل مرة ثانية
    QFile::rename(dir.filePath(QString::fromLatin1(LegacyFile)), dir.filePath(QStringLiteral("persons.json.migrated")));
    return true;
}

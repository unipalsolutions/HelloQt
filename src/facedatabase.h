#pragma once

#include "facetypes.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <memory>
#include <vector>

struct FacePhoto
{
    qint64 id = 0;
    float quality = 0.0f;
    QDateTime added;
};

// مرة تعرَّف فيها البرنامج على الشخص في صورة أو فيديو
struct Sighting
{
    qint64 id = 0;
    QDateTime seen;
    QString source;       // مسار الصورة أو الفيديو
    qint64 positionMs = -1; // موضعه في الفيديو
    float score = 0.0f;
    QImage thumbnail;
    QString camera;   // "camera:…" إن كان الظهور أمام كاميرا (والمصدر ملف تسجيلها إن كانت تُسجَّل)
    QString snapshot; // صورة الإطار كاملاً لحظة الظهور (للكاميرا)
};

// ظهور مع صاحبه (لسجل التنبيهات الذي يجمع ظهور عدة أشخاص)
struct SightingEntry
{
    Sighting sighting;
    QString personId;
    QString name;
    QString tags;
};

struct Person
{
    QString id;
    QString name;
    QString nationalId;
    QString phone;
    QString notes;
    QString gender;   // "ذكر" أو "أنثى" أو فارغ
    QDate birthDate;
    QString address;
    QString email;
    QString tags;     // تصنيفات مفصولة بفواصل، مثل: موظف، زائر، مطلوب
    QDateTime created;
    QDateTime lastSeen; // آخر مرة تعرَّف عليه البرنامج
    int seenCount = 0;
    int photoCount = 0;
    qint64 coverPhoto = 0;   // صورة الوجه التي تمثل الشخص في القوائم
    QList<FacePhoto> photos; // تُملأ في person() فقط، لا في نتائج البحث
};

// فهرس المطابقة: يبقى في الذاكرة لكل شخص بصمة ثنائية مختصرة (64 بايت) فقط، ومتوسط بصماته
// مضغوطاً في ملف على القرص. البحث يصفّي الملايين بمسافة هامنج ثم يعيد ترتيب الأقرب بدقة.
// آمن للاستخدام من عدة خيوط في الوقت نفسه.
class Gallery;
using GalleryPtr = std::shared_ptr<Gallery>;

// قاعدة بيانات محلية بداخل مجلد القاعدة:
//   faces.db      SQLite: الأشخاص وصور وجوههم وبصماتها
//   index.codes   البصمات الثنائية المختصرة لكل شخص (تُقرأ كاملة عند الفتح)
//   index.means   متوسط بصمات كل شخص مضغوطاً (يُقرأ منه عند البحث فقط)
// ملفا الفهرس يُعاد بناؤهما تلقائياً من faces.db إذا فُقدا أو لم يطابقاه.
class FaceDatabase : public QObject
{
    Q_OBJECT

public:
    explicit FaceDatabase(QObject *parent = nullptr);
    ~FaceDatabase() override;

    static QString defaultDirectory();

    bool open(const QString &directory, QString *error = nullptr);
    void close();
    QString directory() const;

    // نسخة احتياطية كاملة في ملف واحد (الأشخاص وصورهم وبصماتهم وسجل الظهور)، تُنشأ والقاعدة مفتوحة
    bool backup(const QString &target, QString *error = nullptr) const;
    // يستبدل القاعدة بنسخة احتياطية بعد التحقق منها؛ القاعدة الحالية تُحفظ بجانبها باسم faces.db.before-restore
    bool restore(const QString &source, QString *error = nullptr);
    // عدد الأشخاص والصور في ملف نسخة احتياطية، أو -1 إن لم يكن نسخة صالحة
    static QPair<qint64, qint64> inspectBackup(const QString &path);

    qint64 personCount() const { return m_personCount; }
    qint64 photoCount() const { return m_photoCount; }

    Person person(const QString &id) const;
    // صفحة من الأشخاص الأحدث أولاً؛ beforeId للصفحة التالية (0 للأولى)
    QList<Person> findPersons(const QString &filter, qint64 beforeId, int limit) const;
    // عدد نتائج البحث حتى الحد cap (البحث في الملايين لا يحتاج عدداً دقيقاً)
    qint64 countPersons(const QString &filter, qint64 cap) const;

    // قائمة المراقبة: الأشخاص الذين في تصنيفاتهم «مراقبة» أو «مطلوب» يُنبَّه المستخدم فور ظهورهم
    static bool isWatched(const QString &tags);
    static QString withWatchTag(const QString &tags, bool watched);
    QSet<QString> watchedIds() const;

    // الشخص المسجل برقم الهوية هذا، أو بالاسم نفسه تماماً إن لم يُعطَ رقم هوية
    QString findPersonId(const QString &name, const QString &nationalId) const;
    QString addPerson(Person person);
    bool updatePerson(const Person &person);
    bool removePerson(const QString &id);
    bool addPhoto(const QString &personId, const QImage &crop,
                  const std::vector<std::vector<float>> &embeddings, float quality);
    bool removePhoto(const QString &personId, qint64 photoId);
    QImage photo(qint64 photoId, int maxSize = 0) const;
    bool setCoverPhoto(const QString &personId, qint64 photoId);
    // ينقل صور removedId وسجل ظهوره إلى keepId ثم يحذفه
    bool mergePersons(const QString &keepId, const QString &removedId);
    // متوسط بصمات الشخص (فارغ إن لم تكن له صور)
    std::vector<float> personEmbedding(const QString &personId) const;
    // تشابه كل صورة مع بقية صور الشخص (0..1)؛ القيمة المنخفضة تعني صورة قد تكون لشخص آخر
    QHash<qint64, float> photoConsistency(const QString &personId) const;

    // سجل الظهور
    bool addSighting(const QString &personId, const QString &source, qint64 positionMs, float score, const QImage &face);
    // ظهور أمام الكاميرا: يُضاف دائماً سجلاً جديداً (لا يُدمج مع ظهور سابق في المصدر نفسه)؛
    // recording ملف التسجيل الجاري وpositionMs موضع الظهور فيه، أو فارغ و -1 إن لم تكن تُسجَّل
    bool addCameraSighting(const QString &personId, const QString &camera, const QString &recording, qint64 positionMs,
                           float score, const QImage &face, const QString &snapshot);
    QList<Sighting> sightings(const QString &personId, int limit = 50) const;
    // أحدث الظهور لكل الأشخاص منذ since (أو كله إن كان غير صالح)؛ watchedOnly لقائمة المراقبة فقط
    QList<SightingEntry> recentSightings(bool watchedOnly, const QDateTime &since, int limit) const;

    // يجمع عمليات كثيرة في معاملة واحدة (للاستيراد الكبير)، والإشعارات تُرسل مرة واحدة في النهاية
    void beginBatch();
    bool endBatch();

    GalleryPtr gallery() const;

    // يعيد أفضل شخص مطابق؛ threshold هو حد التأكيد، وما دونه بـ 0.1 "محتمل"
    static MatchResult identify(Gallery &gallery, const std::vector<float> &embedding, float threshold);
    // أقرب count أشخاص مرتبين بالتشابه (لعرض المرشحين ونسبهم)؛ excludeId يُستبعد من النتائج
    static QList<MatchResult> candidates(Gallery &gallery, const std::vector<float> &embedding, int count, float threshold,
                                         const QString &excludeId = QString());
    // يزيد مع كل تغيير في الأشخاص أو بصماتهم، فيعرف من يخزّن نتائج المطابقة متى يعيد حسابها
    static quint64 revision(const Gallery &gallery);

signals:
    void changed();                       // أي تغيير (الإحصاءات ونتائج المطابقة)
    void personsReset();                  // إضافة أو حذف أشخاص
    void personUpdated(const QString &id); // تغيّرت بيانات شخص أو صوره

private:
    bool exec(const QString &sql, const QVariantList &values = {}) const;
    bool refreshPerson(qint64 personId);
    bool migrateLegacy(QString *error);
    bool upgradeSchema();
    void notify(const QString &personId, bool reset);
    void beginStep();
    void rollbackStep();

    QString m_directory;
    QString m_connection;
    QSqlDatabase m_db;
    GalleryPtr m_gallery;
    qint64 m_personCount = 0;
    qint64 m_photoCount = 0;
    int m_batchDepth = 0;
    bool m_batchReset = false;
    bool m_batchChanged = false;
    QSet<QString> m_batchUpdated; // أشخاص تغيروا داخل الدفعة الحالية
};

#include "importdialog.h"

#include "facedatabase.h"
#include "faceengine.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace {

const QStringList ImagePatterns = {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                                   QStringLiteral("*.bmp"), QStringLiteral("*.webp"), QStringLiteral("*.tif"),
                                   QStringLiteral("*.tiff"), QStringLiteral("*.jfif")};
constexpr int CommitEvery = 100; // صور في كل معاملة، فلا يضيع كل شيء إن أُغلق البرنامج فجأة

enum Column { NameColumn, IdColumn, PhotosColumn, StatusColumn };

struct Job
{
    int person;
    QStringList files;
};

struct Candidate
{
    FaceItem item;
    cv::Mat aligned;
    std::vector<float> embedding;
    double area = 0.0;
};

void runImport(const FaceEngine *engine, QList<Job> jobs, float mismatch, std::shared_ptr<ImportState> state)
{
    for (const Job &job : jobs) {
        // أولاً كل وجوه كل صور الشخص
        QList<std::vector<Candidate>> images;
        for (const QString &file : job.files) {
            while (state->paused && !state->cancel)
                QThread::msleep(100);
            if (state->cancel)
                return;
            std::vector<Candidate> candidates;
            const cv::Mat image = ImageIO::loadImage(file);
            std::vector<AnalyzedFace> faces = image.empty() ? std::vector<AnalyzedFace>() : engine->analyze(image, true);
            if (faces.empty() && !image.empty())
                faces = engine->analyzeRobust(image);
            for (const AnalyzedFace &face : faces)
                candidates.push_back({ImageIO::makeFaceItem(image, face, 0), face.aligned, face.embedding, double(face.detection.box.area())});
            images << candidates;
            ++state->done;
        }

        // الوجه المرجعي هو الأشبه ببقية الصور، فلا تُفسد صورة دخيلة أو جماعية ملف الشخص
        // حتى لو جاءت أولاً في الترتيب
        const Candidate *reference = nullptr;
        double bestScore = -1.0;
        for (int i = 0; i < images.size(); ++i) {
            for (const Candidate &candidate : images.at(i)) {
                double score = 0.0;
                for (int j = 0; j < images.size(); ++j) {
                    if (j == i)
                        continue;
                    float best = 0.0f;
                    for (const Candidate &other : images.at(j))
                        best = std::max(best, FaceEngine::cosine(candidate.embedding, other.embedding));
                    score += best;
                }
                // عند التساوي (صورة واحدة مثلاً) يُفضَّل الوجه الأكبر
                score += candidate.area * 1e-9;
                if (score > bestScore) {
                    bestScore = score;
                    reference = &candidate;
                }
            }
        }

        QList<ImportResult> results;
        for (int i = 0; i < images.size(); ++i) {
            ImportResult result;
            result.person = job.person;
            result.file = job.files.at(i);
            result.faces = int(images.at(i).size());
            const Candidate *chosen = nullptr;
            float similarity = -1.0f;
            for (const Candidate &candidate : images.at(i)) {
                const float value = &candidate == reference ? 1.0f : FaceEngine::cosine(candidate.embedding, reference->embedding);
                if (value > similarity) {
                    similarity = value;
                    chosen = &candidate;
                }
            }
            if (!chosen) {
                result.status = ImportResult::Status::NoFace;
            } else if (similarity < mismatch) {
                result.status = ImportResult::Status::Mismatch;
            } else {
                result.status = ImportResult::Status::Added;
                result.face = chosen->item;
                result.embeddings = engine->enrollmentEmbeddings(chosen->aligned);
            }
            results << result;
        }
        QMutexLocker locker(&state->mutex);
        state->pending << results;
    }
}

} // namespace

void ImportDialog::parseName(const QString &text, bool fromFile, QString *name, QString *nationalId)
{
    QString raw = text;
    raw.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (fromFile) {
        // «أحمد علي 2» و«أحمد علي (3)» صور للشخص نفسه
        raw.remove(QRegularExpression(QStringLiteral("[\\s.\\-]*[(\\[]?\\d{1,3}[)\\]]?\\s*$")));
    }
    QStringList nameParts;
    QString id;
    for (const QString &part : raw.split(QRegularExpression(QStringLiteral("\\s*[-–—]\\s*")), Qt::SkipEmptyParts)) {
        const QString trimmed = part.trimmed();
        if (id.isEmpty() && QRegularExpression(QStringLiteral("^\\d{5,}$")).match(trimmed).hasMatch())
            id = trimmed;
        else if (!trimmed.isEmpty())
            nameParts << trimmed;
    }
    QString joined = nameParts.join(QLatin1Char(' ')).simplified();
    if (joined.isEmpty() && fromFile && id.isEmpty())
        joined = text.simplified(); // اسم من أرقام قليلة فقط: يبقى كما هو
    if (joined.isEmpty())
        joined = id;
    *name = joined;
    *nationalId = id;
}

ImportDialog::ImportDialog(const FaceEngine *engine, FaceDatabase *database, float threshold, const QString &directory,
                           QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_database(database)
    , m_threshold(threshold)
    , m_directory(directory)
    , m_state(std::make_shared<ImportState>())
    , m_folderLabel(new QLabel(this))
    , m_summary(new QLabel(this))
    , m_bar(new QProgressBar(this))
    , m_tree(new QTreeWidget(this))
    , m_tags(new QLineEdit(this))
    , m_timer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("استيراد أشخاص من مجلد"));
    resize(900, 680);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("استيراد أشخاص من مجلد"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("كل <b>مجلد فرعي</b> يصبح شخصاً باسم المجلد وتُضاف إليه صور الوجوه التي فيه، "
                                           "والصور الموضوعة مباشرة في المجلد يُؤخذ اسم صاحبها من <b>اسم الملف</b> "
                                           "(صورتا «أحمد علي» و«أحمد علي 2» لشخص واحد). "
                                           "لإضافة رقم الهوية اكتبه بعد شرطة: «أحمد علي - 123456789»."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    layout->addWidget(hint);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(10);
    m_choose = new FancyButton(Icons::Name::Folder, QStringLiteral("اختيار مجلد…"), FancyButton::Kind::Secondary, this);
    connect(m_choose, &QAbstractButton::clicked, this, &ImportDialog::chooseFolder);
    m_mergeExisting = new ToggleSwitch(QStringLiteral("إضافة الصور لملفات المسجلين بالاسم نفسه"), this);
    m_mergeExisting->setChecked(true);
    m_mergeExisting->setToolTip(QStringLiteral("إن كان الشخص مسجلاً (بالاسم نفسه أو رقم الهوية) تُضاف صوره إلى ملفه بدل تخطيه"));
    connect(m_mergeExisting, &QAbstractButton::toggled, this, [this] {
        for (int i = 0; i < m_persons.size(); ++i)
            updateItem(i);
        updateSummary();
    });
    m_tags->setPlaceholderText(QStringLiteral("تصنيفات للأشخاص الجدد (اختياري)، مثل: موظف"));
    m_tags->setClearButtonEnabled(true);
    m_tags->setMinimumWidth(240);
    controls->addWidget(m_choose);
    controls->addWidget(m_tags, 1);
    controls->addWidget(m_mergeExisting);
    layout->addLayout(controls);

    m_folderLabel->setObjectName(QStringLiteral("muted"));
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_folderLabel);

    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({QStringLiteral("الشخص"), QStringLiteral("رقم الهوية"), QStringLiteral("الصور"), QStringLiteral("الحالة")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(false);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(IdColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(PhotosColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(StatusColumn, QHeaderView::Interactive);
    m_tree->header()->resizeSection(StatusColumn, 300);
    layout->addWidget(m_tree, 1);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        const int index = m_items.indexOf(item);
        if (column != NameColumn || index < 0 || m_watcher.isRunning() || m_finished)
            return;
        m_persons[index].include = item->checkState(NameColumn) == Qt::Checked;
        updateSummary();
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const int index = m_items.indexOf(item);
        if (index < 0)
            return;
        const ImportPerson &person = m_persons.at(index);
        const QString id = !person.personId.isEmpty() ? person.personId : person.existingId;
        if (!id.isEmpty() && !m_watcher.isRunning()) {
            emit showPersonRequested(id);
            accept();
        }
    });

    m_bar->setObjectName(QStringLiteral("thinProgress"));
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(6);
    m_bar->setValue(0);
    layout->addWidget(m_bar);
    m_summary->setObjectName(QStringLiteral("muted"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *buttons = new QHBoxLayout;
    m_start = new FancyButton(Icons::Name::UserPlus, QStringLiteral("بدء الاستيراد"), FancyButton::Kind::Primary, this);
    connect(m_start, &QAbstractButton::clicked, this, &ImportDialog::start);
    m_pause = new FancyButton(Icons::Name::Stop, QStringLiteral("إيقاف مؤقت"), FancyButton::Kind::Secondary, this);
    connect(m_pause, &QAbstractButton::clicked, this, [this] {
        m_state->paused = !m_state->paused;
        m_pause->setText(m_state->paused ? QStringLiteral("استئناف") : QStringLiteral("إيقاف مؤقت"));
        updateSummary();
    });
    m_close = new FancyButton(Icons::Name::Reject, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, this);
    connect(m_close, &QAbstractButton::clicked, this, &ImportDialog::reject);
    buttons->addWidget(m_start);
    buttons->addWidget(m_pause);
    buttons->addStretch();
    buttons->addWidget(m_close);
    layout->addLayout(buttons);

    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout, this, &ImportDialog::drain);
    connect(&m_watcher, &QFutureWatcherBase::finished, this, &ImportDialog::onFinished);
    setRunning(false);
    m_start->setEnabled(false);
    m_summary->setText(QStringLiteral("اختر المجلد الذي فيه صور الأشخاص"));
}

ImportDialog::~ImportDialog()
{
    m_state->cancel = true;
    m_watcher.waitForFinished();
    onFinished(); // يحفظ ما أُضيف ويغلق المعاملة إن أُغلقت النافذة أثناء الاستيراد
}

void ImportDialog::reject()
{
    if (m_watcher.isRunning()) {
        const bool paused = m_state->paused;
        m_state->paused = true;
        const auto answer = QMessageBox::question(this, QStringLiteral("إيقاف الاستيراد"),
                                                  QStringLiteral("هل تريد إيقاف الاستيراد؟ يبقى ما أُضيف حتى الآن في القاعدة."));
        m_state->paused = paused;
        if (answer != QMessageBox::Yes)
            return;
        m_state->cancel = true;
        m_watcher.waitForFinished();
        onFinished();
    }
    QDialog::reject();
}

void ImportDialog::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("مجلد صور الأشخاص"), m_directory);
    if (!folder.isEmpty())
        load(folder);
}

void ImportDialog::load(const QString &folder)
{
    m_directory = folder;
    m_finished = false;
    m_folderLabel->setText(QDir::toNativeSeparators(folder));
    m_persons.clear();
    QHash<QString, int> byKey;
    const QDir root(folder);
    QDirIterator it(folder, ImagePatterns, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString relative = root.relativeFilePath(file);
        const int slash = relative.indexOf(QLatin1Char('/'));
        QString name, nationalId;
        if (slash > 0)
            parseName(relative.left(slash), false, &name, &nationalId);
        else
            parseName(QFileInfo(file).completeBaseName(), true, &name, &nationalId);
        if (name.isEmpty())
            continue;
        const QString key = nationalId.isEmpty() ? name.toCaseFolded() : QStringLiteral("#") + nationalId;
        int index = byKey.value(key, -1);
        if (index < 0) {
            index = int(m_persons.size());
            byKey.insert(key, index);
            ImportPerson person;
            person.name = name;
            person.nationalId = nationalId;
            person.existingId = m_database->findPersonId(name, nationalId);
            m_persons << person;
        }
        m_persons[index].files << file;
    }
    std::sort(m_persons.begin(), m_persons.end(), [](const ImportPerson &a, const ImportPerson &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    for (ImportPerson &person : m_persons)
        person.files.sort(Qt::CaseInsensitive);

    const QSignalBlocker blocker(m_tree);
    m_tree->clear();
    m_items.clear();
    for (int i = 0; i < m_persons.size(); ++i) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(NameColumn, Qt::Checked);
        item->setText(NameColumn, m_persons.at(i).name);
        item->setText(IdColumn, m_persons.at(i).nationalId);
        item->setTextAlignment(PhotosColumn, Qt::AlignCenter);
        m_items << item;
        updateItem(i);
    }
    m_bar->setMaximum(1);
    m_bar->setValue(0);
    m_start->setEnabled(!m_persons.isEmpty());
    m_start->setText(QStringLiteral("بدء الاستيراد"));
    updateSummary();
}

int ImportDialog::totalFiles() const
{
    int total = 0;
    for (const ImportPerson &person : m_persons) {
        if (person.include && (person.existingId.isEmpty() || m_mergeExisting->isChecked()))
            total += int(person.files.size());
    }
    return total;
}

void ImportDialog::updateItem(int index)
{
    const ImportPerson &person = m_persons.at(index);
    QTreeWidgetItem *item = m_items.at(index);
    const bool skipped = !person.existingId.isEmpty() && !m_mergeExisting->isChecked();
    QString status;
    QColor color = Theme::Muted;
    if (m_watcher.isRunning() || m_finished) {
        if (!person.include || (skipped && person.personId.isEmpty())) {
            status = QStringLiteral("لم يُستورد");
        } else {
            const int processed = person.added + person.noFace + person.mismatched;
            status = person.added > 0 ? QStringLiteral("أُضيفت %1 من %2").arg(person.added).arg(person.files.size())
                                      : QStringLiteral("لم يُضف");
            if (person.noFace > 0)
                status += QStringLiteral(" · بلا وجه %1").arg(person.noFace);
            if (person.mismatched > 0)
                status += QStringLiteral(" · لا تشبهه %1").arg(person.mismatched);
            if (!person.duplicateOf.isEmpty())
                status += QStringLiteral(" · يشبه المسجل «%1»").arg(person.duplicateOf);
            if (processed == 0 && !m_finished)
                status = QStringLiteral("في الانتظار");
            const bool clean = person.added > 0 && person.noFace == 0 && person.mismatched == 0 && person.duplicateOf.isEmpty();
            color = processed == 0 ? Theme::Muted : clean ? Theme::Accent2 : Theme::Warning;
        }
    } else if (skipped) {
        status = QStringLiteral("مسجل من قبل — سيُتخطّى");
    } else if (!person.existingId.isEmpty()) {
        status = QStringLiteral("مسجل من قبل — تُضاف الصور إلى ملفه");
        color = Theme::Accent;
    } else {
        status = QStringLiteral("شخص جديد");
        color = Theme::Text;
    }
    const QSignalBlocker blocker(m_tree);
    item->setText(PhotosColumn, QString::number(person.files.size()));
    item->setText(StatusColumn, status);
    item->setForeground(StatusColumn, color);
    item->setToolTip(NameColumn, person.files.join(QLatin1Char('\n')));
}

void ImportDialog::updateSummary()
{
    if (m_persons.isEmpty()) {
        if (!m_directory.isEmpty() && !m_folderLabel->text().isEmpty())
            m_summary->setText(QStringLiteral("لم يُعثر على صور في هذا المجلد"));
        return;
    }
    int persons = 0, existing = 0, added = 0, noFace = 0, mismatched = 0, created = 0, duplicates = 0;
    for (const ImportPerson &person : std::as_const(m_persons)) {
        if (!person.include || (!person.existingId.isEmpty() && !m_mergeExisting->isChecked()))
            continue;
        ++persons;
        existing += person.existingId.isEmpty() ? 0 : 1;
        added += person.added;
        noFace += person.noFace;
        mismatched += person.mismatched;
        created += !person.personId.isEmpty() && person.existingId.isEmpty() ? 1 : 0;
        duplicates += person.duplicateOf.isEmpty() ? 0 : 1;
    }
    if (!m_watcher.isRunning() && !m_finished) {
        m_summary->setText(QStringLiteral("سيُستورد %1 شخص (%2 منهم مسجلون من قبل) من %3 صورة")
                               .arg(persons).arg(existing).arg(totalFiles()));
        return;
    }
    QString text = m_finished ? QStringLiteral("انتهى الاستيراد: ") : m_state->paused ? QStringLiteral("متوقف مؤقتاً — ")
                                                                                       : QStringLiteral("جارٍ الاستيراد (%1 من %2) — ")
                                                                                             .arg(m_state->done.load())
                                                                                             .arg(totalFiles());
    text += QStringLiteral("%1 شخص جديد، %2 صورة وجه").arg(created).arg(added);
    if (noFace > 0)
        text += QStringLiteral("، %1 صورة بلا وجه").arg(noFace);
    if (mismatched > 0)
        text += QStringLiteral("، %1 صورة لا تشبه بقية صور صاحبها فلم تُضف").arg(mismatched);
    if (duplicates > 0)
        text += QStringLiteral("، %1 شخص يشبه شخصاً مسجلاً باسم آخر — راجعهم").arg(duplicates);
    if (m_finished)
        text += QStringLiteral(". انقر نقراً مزدوجاً على شخص لفتح ملفه.");
    m_summary->setText(text);
}

void ImportDialog::setRunning(bool running)
{
    m_choose->setEnabled(!running);
    m_tags->setEnabled(!running);
    m_mergeExisting->setEnabled(!running);
    m_start->setVisible(!running);
    m_pause->setVisible(running);
    m_close->setText(running ? QStringLiteral("إيقاف") : QStringLiteral("إغلاق"));
}

void ImportDialog::start()
{
    if (m_watcher.isRunning() || m_persons.isEmpty())
        return;
    if (m_finished) {
        chooseFolder();
        return;
    }
    QList<Job> jobs;
    for (int i = 0; i < m_persons.size(); ++i) {
        const ImportPerson &person = m_persons.at(i);
        if (person.include && (person.existingId.isEmpty() || m_mergeExisting->isChecked()))
            jobs << Job{i, person.files};
    }
    if (jobs.isEmpty())
        return;
    m_state = std::make_shared<ImportState>();
    m_bar->setMaximum(totalFiles());
    m_bar->setValue(0);
    m_pause->setText(QStringLiteral("إيقاف مؤقت"));
    setRunning(true);
    m_sinceCommit = 0;
    m_database->beginBatch();
    const FaceEngine *engine = m_engine;
    // صورة أبعد عن بقية صور الشخص من هذا الحد تُعد لشخص آخر (صورة جماعية أو وُضعت في المجلد خطأً)
    const float mismatch = std::max(0.12f, m_threshold * 0.6f);
    const std::shared_ptr<ImportState> state = m_state;
    m_watcher.setFuture(QtConcurrent::run([engine, jobs, mismatch, state] { runImport(engine, jobs, mismatch, state); }));
    m_timer->start();
    for (int i = 0; i < m_persons.size(); ++i)
        updateItem(i);
    updateSummary();
}

void ImportDialog::drain()
{
    QList<ImportResult> results;
    {
        QMutexLocker locker(&m_state->mutex);
        results.swap(m_state->pending);
    }
    for (const ImportResult &result : std::as_const(results)) {
        ImportPerson &person = m_persons[result.person];
        if (result.status == ImportResult::Status::NoFace) {
            ++person.noFace;
        } else if (result.status == ImportResult::Status::Mismatch) {
            ++person.mismatched;
        } else {
            if (person.personId.isEmpty()) {
                if (!person.existingId.isEmpty()) {
                    person.personId = person.existingId;
                } else {
                    // قبل إنشاء الملف: هل وجهه مسجل باسم آخر؟
                    if (const GalleryPtr gallery = m_database->gallery(); gallery && !result.embeddings.empty()) {
                        const MatchResult match = FaceDatabase::identify(*gallery, result.embeddings.front(),
                                                                         std::max(m_threshold + 0.1f, 0.45f));
                        if (match.status == MatchStatus::Confirmed)
                            person.duplicateOf = match.name;
                    }
                    Person created;
                    created.name = person.name;
                    created.nationalId = person.nationalId;
                    created.tags = m_tags->text().trimmed();
                    person.personId = m_database->addPerson(created);
                }
            }
            if (!person.personId.isEmpty()
                && m_database->addPhoto(person.personId, result.face.crop, result.embeddings, result.face.quality)) {
                ++person.added;
                if (++m_sinceCommit >= CommitEvery) {
                    m_database->endBatch();
                    m_database->beginBatch();
                    m_sinceCommit = 0;
                }
            }
        }
        updateItem(result.person);
    }
    if (!results.isEmpty())
        m_tree->scrollToItem(m_items.at(results.last().person));
    m_bar->setValue(m_state->done);
    updateSummary();
}

void ImportDialog::onFinished()
{
    if (!m_timer->isActive())
        return;
    m_timer->stop();
    drain();
    m_database->endBatch();
    m_finished = true;
    setRunning(false);
    m_start->setText(QStringLiteral("استيراد مجلد آخر…"));
    for (int i = 0; i < m_persons.size(); ++i)
        updateItem(i);
    updateSummary();
}

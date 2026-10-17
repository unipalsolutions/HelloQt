#include "archivedialog.h"

#include "appsettings.h"
#include "facedatabase.h"
#include "faceengine.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace {

const QStringList VideoSuffixes = {QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("mkv"), QStringLiteral("mov"),
                                   QStringLiteral("wmv"), QStringLiteral("m4v"), QStringLiteral("webm"), QStringLiteral("mpg"),
                                   QStringLiteral("mpeg"), QStringLiteral("flv"), QStringLiteral("ts"), QStringLiteral("3gp")};
const QString ParallelKey = QStringLiteral("archive/parallel");
constexpr int PersonRole = Qt::UserRole;
constexpr int PreviewWidth = 224;
constexpr int PreviewHeight = 126;
constexpr qint64 ThirtyHoursMs = 30LL * 3600 * 1000;

QString videoFilter()
{
    QStringList patterns;
    for (const QString &suffix : VideoSuffixes)
        patterns << QStringLiteral("*.") + suffix;
    return QStringLiteral("فيديو (%1);;كل الملفات (*)").arg(patterns.join(QLatin1Char(' ')));
}

// مدة بالساعات والدقائق للتقديرات الطويلة
QString longDuration(qint64 ms)
{
    const qint64 minutes = std::max<qint64>(0, ms) / 60000;
    if (minutes < 1)
        return QStringLiteral("أقل من دقيقة");
    if (minutes < 60)
        return QStringLiteral("%1 دقيقة").arg(minutes);
    const qint64 hours = minutes / 60;
    return minutes % 60 == 0 ? QStringLiteral("%1 ساعة").arg(hours)
                             : QStringLiteral("%1 ساعة و%2 دقيقة").arg(hours).arg(minutes % 60);
}

QPixmap personIcon(const QImage &crop, MatchStatus status, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double radius = size * 0.12;
    const double w = size * 0.035;
    QPainterPath clip;
    clip.addRoundedRect(QRectF(w, w, size - 2 * w, size - 2 * w), radius, radius);
    p.save();
    p.setClipPath(clip);
    if (!crop.isNull()) {
        const QImage scaled = crop.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawImage(QPointF((size - scaled.width()) / 2.0, (size - scaled.height()) / 2.0), scaled);
    }
    p.restore();
    p.setPen(QPen(matchColor(status), w));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(w / 2, w / 2, size - w, size - w), radius, radius);
    return pixmap;
}

QLabel *previewLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setFixedSize(PreviewWidth, PreviewHeight + 22);
    label->setAlignment(Qt::AlignCenter);
    label->setObjectName(QStringLiteral("muted"));
    label->setStyleSheet(QStringLiteral("QLabel { background: %1; border: 1px solid %2; border-radius: 8px; }")
                             .arg(Theme::Surface.name(), Theme::Border.name()));
    label->setText(QStringLiteral("بانتظار جزء…"));
    return label;
}

} // namespace

ArchiveScanDialog::ArchiveScanDialog(const FaceEngine *engine, FaceDatabase *database, float threshold, const QString &directory,
                                     QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_database(database)
    , m_threshold(threshold)
    , m_directory(directory)
    , m_scan(new ArchiveScan(engine, this))
    , m_files(new QTreeWidget(this))
    , m_parallel(new QComboBox(this))
    , m_previews(new QWidget(this))
    , m_bar(new QProgressBar(this))
    , m_status(new QLabel(this))
    , m_estimate(new QLabel(this))
    , m_peopleTitle(new QLabel(this))
    , m_grid(new QListWidget(this))
    , m_tick(new QTimer(this))
    , m_peopleTimer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("مسح أرشيف التسجيلات"));
    resize(1100, 760);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("مسح أرشيف التسجيلات"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("أضف ملفات فيديو أو مجلدات كاملة، فتُحلل بالفحص السريع دون تشغيلها على الشاشة، "
                                           "ويُعرض من ظهر وفي أي ملف ومتى. يُحفظ الموضع باستمرار: إن أُغلق البرنامج يتابع المسح من حيث توقف."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *controls = new FlowLayout;
    m_addFiles = new FancyButton(Icons::Name::Video, QStringLiteral("إضافة ملفات…"), FancyButton::Kind::Secondary, this);
    m_addFolder = new FancyButton(Icons::Name::Folder, QStringLiteral("إضافة مجلد…"), FancyButton::Kind::Secondary, this);
    m_addFolder->setToolTip(QStringLiteral("يضيف كل ملفات الفيديو في المجلد ومجلداته الفرعية"));
    m_remove = new FancyButton(Icons::Name::Trash, QStringLiteral("إزالة المحدد"), FancyButton::Kind::Secondary, this);
    m_clear = new FancyButton(Icons::Name::Trash, QStringLiteral("إفراغ القائمة"), FancyButton::Kind::Secondary, this);
    m_run = new FancyButton(Icons::Name::Play, QStringLiteral("بدء المسح"), FancyButton::Kind::Primary, this);
    connect(m_addFiles, &QAbstractButton::clicked, this, &ArchiveScanDialog::addFiles);
    connect(m_addFolder, &QAbstractButton::clicked, this, &ArchiveScanDialog::addFolder);
    connect(m_remove, &QAbstractButton::clicked, this, &ArchiveScanDialog::removeSelected);
    connect(m_clear, &QAbstractButton::clicked, this, &ArchiveScanDialog::clearAll);
    connect(m_run, &QAbstractButton::clicked, this, &ArchiveScanDialog::toggleRun);

    const bool gpu = engine && !engine->gpuName().isEmpty();
    m_parallel->addItem(QStringLiteral("تلقائي (%1)").arg(ArchiveScan::autoParallel(gpu)), 0);
    for (int n = 1; n <= 4; ++n)
        m_parallel->addItem(QString::number(n), n);
    m_parallel->setToolTip(gpu ? QStringLiteral("عدد الأجزاء التي تُحلل معاً. على كرت الشاشة يُفك فيديو بينما يُحلل غيره فيسرع المسح")
                               : QStringLiteral("عدد الأجزاء التي تُحلل معاً. النماذج على المعالج تستخدم كل الأنوية أصلاً، "
                                                "فالزيادة تفيد فقط مع أنوية كثيرة وفيديو عالي الدقة"));
    const int saved = QSettings(AppSettings::settingsPath(), QSettings::IniFormat).value(ParallelKey, 0).toInt();
    m_parallel->setCurrentIndex(std::max(0, m_parallel->findData(saved)));
    connect(m_parallel, &QComboBox::currentIndexChanged, this, [this] {
        QSettings(AppSettings::settingsPath(), QSettings::IniFormat).setValue(ParallelKey, m_parallel->currentData().toInt());
    });
    auto *parallelBox = new QWidget(this);
    auto *parallelRow = new QHBoxLayout(parallelBox);
    parallelRow->setContentsMargins(0, 0, 0, 0);
    parallelRow->addWidget(new QLabel(QStringLiteral("التحليل المتوازي:"), parallelBox));
    parallelRow->addWidget(m_parallel);
    for (QWidget *widget : {static_cast<QWidget *>(m_addFiles), static_cast<QWidget *>(m_addFolder), static_cast<QWidget *>(m_remove),
                            static_cast<QWidget *>(m_clear), parallelBox, static_cast<QWidget *>(m_run)})
        controls->addWidget(widget);
    layout->addLayout(controls);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);

    auto *top = new QWidget(splitter);
    auto *topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    m_files->setHeaderLabels({QStringLiteral("الملف"), QStringLiteral("المدة"), QStringLiteral("الحالة")});
    m_files->setRootIsDecorated(false);
    m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_files->setAlternatingRowColors(true);
    m_files->header()->setStretchLastSection(false);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_files->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_files->setMinimumHeight(140);
    connect(m_files, &QTreeWidget::itemSelectionChanged, this, [this] {
        m_remove->setEnabled(!m_scan->isRunning() && !m_files->selectedItems().isEmpty());
    });
    connect(m_files, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        emit openRequested(item->data(0, Qt::UserRole).toString());
    });
    topLayout->addWidget(m_files, 1);
    auto *previewLayout = new QVBoxLayout(m_previews);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(6);
    previewLayout->addStretch();
    m_previews->setVisible(false);
    topLayout->addWidget(m_previews);
    splitter->addWidget(top);

    auto *bottom = new QWidget(splitter);
    auto *bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 6, 0, 0);
    m_bar->setObjectName(QStringLiteral("thinProgress"));
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(6);
    m_bar->setRange(0, 1000);
    bottomLayout->addWidget(m_bar);
    m_status->setWordWrap(true);
    bottomLayout->addWidget(m_status);
    m_estimate->setObjectName(QStringLiteral("muted"));
    m_estimate->setWordWrap(true);
    bottomLayout->addWidget(m_estimate);
    m_peopleTitle->setObjectName(QStringLiteral("sectionTitle"));
    bottomLayout->addWidget(m_peopleTitle);
    m_grid->setObjectName(QStringLiteral("photoGrid"));
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setIconSize(QSize(96, 96));
    m_grid->setGridSize(QSize(150, 172));
    m_grid->setTextElideMode(Qt::ElideRight);
    m_grid->setWordWrap(true);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const int index = item->data(PersonRole).toInt();
        if (index < 0 || index >= m_people.size())
            return;
        const Archive::Person &person = m_people.at(index);
        if (person.bestFile >= 0 && person.bestFile < m_scan->files().size())
            emit openRequested(m_scan->files().at(person.bestFile).path);
    });
    bottomLayout->addWidget(m_grid, 1);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    auto *buttons = new QHBoxLayout;
    m_html = new FancyButton(Icons::Name::Export, QStringLiteral("التقرير الموحّد (HTML)…"), FancyButton::Kind::Secondary, this);
    m_html->setToolTip(QStringLiteral("من ظهر، وفي أي ملف، ومتى، ومدة ظهوره، مع شريط زمني لكل ملف"));
    m_csv = new FancyButton(Icons::Name::Export, QStringLiteral("جدول Excel (CSV)…"), FancyButton::Kind::Secondary, this);
    connect(m_html, &QAbstractButton::clicked, this, [this] { saveReport(false); });
    connect(m_csv, &QAbstractButton::clicked, this, [this] { saveReport(true); });
    auto *close = new FancyButton(Icons::Name::Reject, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, this);
    connect(close, &QAbstractButton::clicked, this, &ArchiveScanDialog::reject);
    buttons->addWidget(m_html);
    buttons->addWidget(m_csv);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    reloadGallery();
    if (m_database) {
        connect(m_database, &FaceDatabase::personsReset, this, &ArchiveScanDialog::reloadGallery);
        connect(m_database, &FaceDatabase::personUpdated, this, &ArchiveScanDialog::reloadGallery);
    }
    connect(m_scan, &ArchiveScan::filesChanged, this, &ArchiveScanDialog::refreshFiles);
    connect(m_scan, &ArchiveScan::progressChanged, this, &ArchiveScanDialog::refreshStatus);
    connect(m_scan, &ArchiveScan::previewReady, this, &ArchiveScanDialog::showPreview);
    connect(m_scan, &ArchiveScan::chunkFinished, this, &ArchiveScanDialog::schedulePeople);
    connect(m_scan, &ArchiveScan::finished, this, [this] {
        m_tick->stop();
        refreshFiles();
        refreshStatus();
        computePeople();
        if (!m_closing && !m_scan->hasPending() && !m_scan->files().isEmpty())
            Toast::show(this, QStringLiteral("انتهى مسح الأرشيف"));
    });
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, [this] {
        refreshFiles();
        refreshStatus();
    });
    // تُجمع الوجوه بعد انتهاء الأجزاء وكل بضع ثوانٍ أثناء المسح، لا مع كل إطار
    m_peopleTimer->setSingleShot(true);
    m_peopleTimer->setInterval(1500);
    connect(m_peopleTimer, &QTimer::timeout, this, &ArchiveScanDialog::computePeople);
    connect(&m_peopleWatcher, &QFutureWatcherBase::finished, this, &ArchiveScanDialog::showPeople);

    m_scan->load();
    refreshFiles();
    refreshStatus();
    computePeople();
}

ArchiveScanDialog::~ArchiveScanDialog()
{
    m_peopleWatcher.waitForFinished();
    // هدم m_scan (ابن هذه النافذة) يوقف الخيوط ويحفظ موضعها
}

void ArchiveScanDialog::reject()
{
    stopAndWait();
    QDialog::reject();
}

void ArchiveScanDialog::closeEvent(QCloseEvent *event)
{
    stopAndWait();
    QDialog::closeEvent(event);
}

void ArchiveScanDialog::stopAndWait()
{
    if (!m_scan->isRunning())
        return;
    m_closing = true;
    m_status->setText(QStringLiteral("جارٍ حفظ موضع المسح…"));
    setEnabled(false);
    QEventLoop loop;
    connect(m_scan, &ArchiveScan::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    m_scan->stop();
    if (m_scan->isRunning())
        loop.exec();
    setEnabled(true);
}

void ArchiveScanDialog::addPaths(const QStringList &paths)
{
    QStringList files;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QStringList found;
            QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString file = it.next();
                if (VideoSuffixes.contains(QFileInfo(file).suffix().toLower()))
                    found << file;
            }
            found.sort(Qt::CaseInsensitive);
            files << found;
        } else if (info.isFile()) {
            files << path;
        }
    }
    if (files.isEmpty()) {
        Toast::show(this, QStringLiteral("لا توجد ملفات فيديو"), Toast::Kind::Warning);
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int added = m_scan->addFiles(files);
    QApplication::restoreOverrideCursor();
    if (added < files.size())
        Toast::show(this, QStringLiteral("أُضيف %1 من %2 ملف (المكرر وما تعذر فتحه لا يُضاف)").arg(added).arg(files.size()),
                    added > 0 ? Toast::Kind::Info : Toast::Kind::Warning);
}

void ArchiveScanDialog::addFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("إضافة ملفات فيديو"), m_directory, videoFilter());
    if (paths.isEmpty())
        return;
    m_directory = QFileInfo(paths.first()).absolutePath();
    addPaths(paths);
}

void ArchiveScanDialog::addFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("إضافة مجلد تسجيلات"), m_directory);
    if (folder.isEmpty())
        return;
    m_directory = folder;
    addPaths({folder});
}

void ArchiveScanDialog::removeSelected()
{
    QList<int> rows;
    for (QTreeWidgetItem *item : m_files->selectedItems())
        rows << m_files->indexOfTopLevelItem(item);
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (const int row : std::as_const(rows))
        m_scan->removeFile(row);
    computePeople();
}

void ArchiveScanDialog::clearAll()
{
    m_scan->clear();
    computePeople();
}

int ArchiveScanDialog::parallel() const
{
    const int chosen = m_parallel->currentData().toInt();
    return chosen > 0 ? chosen : ArchiveScan::autoParallel(m_engine && !m_engine->gpuName().isEmpty());
}

void ArchiveScanDialog::toggleRun()
{
    if (m_scan->isRunning()) {
        m_scan->stop();
        refreshStatus();
        return;
    }
    const int count = parallel();
    // معاينة لكل خيط
    for (QLabel *label : std::as_const(m_previewLabels))
        delete label;
    m_previewLabels.clear();
    auto *previewLayout = static_cast<QVBoxLayout *>(m_previews->layout());
    for (int i = 0; i < count; ++i) {
        QLabel *label = previewLabel(m_previews);
        previewLayout->insertWidget(i, label);
        m_previewLabels << label;
    }
    m_previews->setVisible(true);
    m_scan->setThreshold(m_threshold);
    m_scan->start(count);
    m_tick->start();
    refreshFiles();
    refreshStatus();
}

void ArchiveScanDialog::refreshFiles()
{
    const QList<Archive::File> &files = m_scan->files();
    while (m_files->topLevelItemCount() > files.size())
        delete m_files->takeTopLevelItem(m_files->topLevelItemCount() - 1);
    for (int i = 0; i < files.size(); ++i) {
        QTreeWidgetItem *item = m_files->topLevelItem(i);
        if (!item)
            item = new QTreeWidgetItem(m_files);
        const Archive::File &file = files.at(i);
        if (item->data(0, Qt::UserRole).toString() != file.path) {
            // ⁨…⁩ حتى لا يقلب اسم الملف اللاتيني ترتيب السطر
            item->setText(0, QStringLiteral("⁨%1⁩").arg(QFileInfo(file.path).fileName()));
            item->setToolTip(0, QDir::toNativeSeparators(file.path));
            item->setData(0, Qt::UserRole, file.path);
            item->setIcon(0, Icons::icon(Icons::Name::Video, Theme::Accent2, 16));
        }
        item->setText(1, file.durationMs > 0 ? ImageIO::formatTime(file.durationMs) : QStringLiteral("—"));
        QString state;
        QColor color = Theme::Muted;
        switch (m_scan->fileState(i)) {
        case ArchiveScan::FileState::Waiting: {
            const double progress = m_scan->fileProgress(i);
            state = progress > 0.0 ? QStringLiteral("متوقف عند %1%").arg(int(progress * 100)) : QStringLiteral("بانتظار");
            break;
        }
        case ArchiveScan::FileState::Running:
            state = QStringLiteral("جارٍ %1%").arg(int(m_scan->fileProgress(i) * 100));
            color = Theme::Accent2;
            break;
        case ArchiveScan::FileState::Done:
            state = QStringLiteral("تم");
            color = Theme::Success;
            break;
        case ArchiveScan::FileState::Failed:
            state = QStringLiteral("خطأ");
            color = Theme::Danger.lighter(160);
            item->setToolTip(2, file.error);
            break;
        }
        item->setText(2, state);
        item->setForeground(2, color);
    }
}

void ArchiveScanDialog::refreshStatus()
{
    const bool running = m_scan->isRunning();
    const bool any = !m_scan->files().isEmpty();
    const bool pending = m_scan->hasPending();
    const qint64 total = m_scan->totalMs();
    const qint64 done = m_scan->doneMs();
    m_bar->setValue(total > 0 ? int(1000 * double(done) / double(total)) : 0);

    m_addFiles->setEnabled(!running);
    m_addFolder->setEnabled(!running);
    m_clear->setEnabled(!running && any);
    m_remove->setEnabled(!running && !m_files->selectedItems().isEmpty());
    m_parallel->setEnabled(!running);
    m_run->setEnabled(running ? !m_scan->isStopping() : pending);
    if (running) {
        m_run->setText(m_scan->isStopping() ? QStringLiteral("جارٍ الإيقاف…") : QStringLiteral("إيقاف"));
        m_run->setIconName(Icons::Name::Pause);
        m_run->setKind(FancyButton::Kind::Secondary);
    } else {
        m_run->setText(done > 0 && pending ? QStringLiteral("متابعة المسح") : QStringLiteral("بدء المسح"));
        m_run->setIconName(Icons::Name::Play);
        m_run->setKind(FancyButton::Kind::Primary);
    }
    if (!running) {
        m_previews->setVisible(false);
    }
    const bool hasPeople = !m_people.isEmpty();
    m_html->setEnabled(hasPeople && !running);
    m_csv->setEnabled(hasPeople && !running);

    const double speed = m_scan->speed();
    if (!any) {
        m_status->setText(QStringLiteral("أضف ملفات أو مجلداً للبدء"));
    } else {
        QString text = QStringLiteral("تم %1 من %2 (%3%)")
                           .arg(ImageIO::formatTime(done), ImageIO::formatTime(total))
                           .arg(total > 0 ? int(100 * double(done) / double(total)) : 0);
        if (running && !m_scan->isStopping()) {
            if (speed > 0.0)
                text += QStringLiteral(" · المتبقي نحو %1 · السرعة %2× · %3 خيط")
                            .arg(longDuration(m_scan->remainingMs()))
                            .arg(speed, 0, 'f', 1)
                            .arg(m_scan->activeWorkers());
            else
                text += QStringLiteral(" · جارٍ قياس السرعة…");
        } else if (m_scan->isStopping()) {
            text += QStringLiteral(" · جارٍ حفظ الموضع…");
        } else if (pending && done > 0) {
            text += QStringLiteral(" · مسح غير مكتمل محفوظ: اضغط «متابعة المسح» ليكمل من حيث توقف");
        } else if (!pending) {
            text += QStringLiteral(" · اكتمل المسح");
        } else if (speed > 0.0) {
            text += QStringLiteral(" · الوقت المتوقع نحو %1").arg(longDuration(qint64((total - done) / speed)));
        }
        m_status->setText(text);
    }
    // تقدير الأرشيف الطويل من السرعة المقيسة على هذا الجهاز
    if (speed > 0.0)
        m_estimate->setText(QStringLiteral("بالسرعة المقيسة على هذا الجهاز: 30 ساعة من التسجيلات تحتاج نحو %1")
                                .arg(longDuration(qint64(ThirtyHoursMs / speed))));
    else
        m_estimate->setText(QStringLiteral("يُعرض تقدير مدة المسح بعد ثوانٍ من البدء"));
}

void ArchiveScanDialog::showPreview(int slot, const QImage &image, const QString &path, qint64 positionMs)
{
    if (slot < 0 || slot >= m_previewLabels.size())
        return;
    QPixmap pixmap(PreviewWidth, PreviewHeight + 22);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QImage scaled = image.scaled(PreviewWidth - 4, PreviewHeight - 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    p.drawImage(QPointF((PreviewWidth - scaled.width()) / 2.0, 2 + (PreviewHeight - 4 - scaled.height()) / 2.0), scaled);
    p.setPen(Theme::Muted);
    const QString caption = QStringLiteral("%1 · %2").arg(ImageIO::formatTime(positionMs), QFileInfo(path).fileName());
    p.drawText(QRect(6, PreviewHeight, PreviewWidth - 12, 20), Qt::AlignCenter,
               p.fontMetrics().elidedText(caption, Qt::ElideMiddle, PreviewWidth - 12));
    p.end();
    m_previewLabels.at(slot)->setPixmap(pixmap);
}

void ArchiveScanDialog::schedulePeople()
{
    if (!m_peopleTimer->isActive())
        m_peopleTimer->start();
}

void ArchiveScanDialog::computePeople()
{
    if (m_peopleWatcher.isRunning()) {
        m_peopleDirty = true;
        return;
    }
    m_peopleDirty = false;
    const QList<Archive::Chunk> chunks = m_scan->snapshot();
    const int fileCount = int(m_scan->files().size());
    const GalleryPtr gallery = m_database ? m_database->gallery() : GalleryPtr();
    const float threshold = m_threshold;
    m_peopleWatcher.setFuture(QtConcurrent::run([chunks, fileCount, gallery, threshold] {
        return Archive::people(chunks, fileCount, gallery, threshold);
    }));
}

void ArchiveScanDialog::showPeople()
{
    m_people = m_peopleWatcher.result();
    if (m_peopleDirty) {
        computePeople();
    }
    const QList<Archive::File> &files = m_scan->files();
    m_grid->setUpdatesEnabled(false);
    m_grid->clear();
    int known = 0;
    int unknownNumber = 0;
    for (int i = 0; i < m_people.size(); ++i) {
        const Archive::Person &person = m_people.at(i);
        const bool confirmed = person.match.status == MatchStatus::Confirmed;
        known += confirmed ? 1 : 0;
        const QString name = person.match.status == MatchStatus::Unknown ? QStringLiteral("مجهول %1").arg(++unknownNumber)
                                                                          : matchLabel(person.match);
        auto *item = new QListWidgetItem(m_grid);
        item->setIcon(QIcon(personIcon(ImageIO::faceCrop(person.face), person.match.status, 192)));
        item->setText(QStringLiteral("%1\n%2 · %3 ملف")
                          .arg(name, ImageIO::formatTime(person.screenMs))
                          .arg(person.sightings.size()));
        QStringList lines{name};
        for (const Archive::Sighting &sighting : person.sightings) {
            if (sighting.file < 0 || sighting.file >= files.size())
                continue;
            QStringList ranges;
            for (int r = 0; r < std::min<int>(6, sighting.appearances.size()); ++r)
                ranges << QStringLiteral("%1–%2").arg(ImageIO::formatTime(sighting.appearances.at(r).first),
                                                      ImageIO::formatTime(sighting.appearances.at(r).second));
            if (sighting.appearances.size() > 6)
                ranges << QStringLiteral("…");
            lines << QStringLiteral("⁨%1⁩: %2").arg(QFileInfo(files.at(sighting.file).path).fileName(), ranges.join(QStringLiteral("، ")));
        }
        lines << QStringLiteral("انقر مرتين لفتح الملف الذي فيه أوضح لقطة");
        item->setToolTip(lines.join(QLatin1Char('\n')));
        item->setData(PersonRole, i);
    }
    m_grid->setUpdatesEnabled(true);
    m_peopleTitle->setText(m_people.isEmpty() ? QStringLiteral("من ظهر")
                                              : QStringLiteral("من ظهر: %1 شخص · %2 معروف").arg(m_people.size()).arg(known));
    refreshStatus();
}

void ArchiveScanDialog::reloadGallery()
{
    if (m_database)
        m_scan->setGallery(m_database->gallery());
    schedulePeople();
}

void ArchiveScanDialog::saveReport(bool csv)
{
    if (m_people.isEmpty())
        return;
    const QString suggested = QDir(m_directory).filePath(csv ? QStringLiteral("تقرير الأرشيف.csv") : QStringLiteral("تقرير الأرشيف.html"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ تقرير الأرشيف"), suggested,
                                                      csv ? QStringLiteral("CSV (*.csv)") : QStringLiteral("HTML (*.html)"));
    if (path.isEmpty())
        return;
    QStringList paths;
    for (const Archive::File &file : m_scan->files())
        paths << file.path;
    const QList<Report::ArchivePerson> persons = Archive::toReport(m_people, m_scan->files());
    QString error;
    const bool ok = csv ? Report::writeArchiveCsv(path, persons, &error)
                        : Report::writeArchiveHtml(path, persons, paths, m_scan->doneMs(), &error);
    if (!ok) {
        Toast::show(this, QStringLiteral("تعذر حفظ التقرير: %1").arg(error), Toast::Kind::Warning);
        return;
    }
    m_directory = QFileInfo(path).absolutePath();
    Toast::show(this, QStringLiteral("حُفظ التقرير"));
    if (!csv)
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

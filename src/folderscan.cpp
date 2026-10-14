#include "folderscan.h"

#include "facedatabase.h"
#include "faceengine.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace {

const QStringList ImagePatterns = {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                                   QStringLiteral("*.bmp"), QStringLiteral("*.webp"), QStringLiteral("*.tif"),
                                   QStringLiteral("*.tiff"), QStringLiteral("*.jfif")};
const QString AllFaces = QStringLiteral("#all");
const QString KnownFaces = QStringLiteral("#known");
const QString UnknownFaces = QStringLiteral("#unknown");
constexpr int HitRole = Qt::UserRole;

void scanFolder(const FaceEngine *engine, GalleryPtr gallery, float threshold, QString folder, bool recursive,
                bool thorough, std::shared_ptr<FolderScanState> state)
{
    QStringList files;
    QDirIterator it(folder, ImagePatterns, QDir::Files,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext() && !state->cancel)
        files << it.next();
    files.sort(Qt::CaseInsensitive);
    state->total = int(files.size());

    for (int i = 0; i < files.size() && !state->cancel; ++i) {
        const cv::Mat image = ImageIO::loadImage(files.at(i));
        QList<FolderHit> hits;
        if (!image.empty()) {
            const std::vector<AnalyzedFace> faces = thorough ? engine->analyzeRobust(image) : engine->analyze(image, false);
            for (const AnalyzedFace &face : faces) {
                FolderHit hit;
                hit.path = files.at(i);
                hit.crop = ImageIO::cropFace(image, face.detection.box, 200);
                hit.quality = face.quality;
                if (gallery)
                    hit.match = FaceDatabase::identify(*gallery, face.embedding, threshold);
                hits << hit;
            }
        }
        state->faces += int(hits.size());
        if (!hits.isEmpty()) {
            QMutexLocker locker(&state->mutex);
            state->pending << hits;
        }
        state->done = i + 1;
    }
}

// صورة الوجه داخل إطار بلون حالة التعرف
QPixmap hitIcon(const QImage &crop, MatchStatus status, int size)
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
    const QImage scaled = crop.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    p.drawImage(QPointF((size - scaled.width()) / 2.0, (size - scaled.height()) / 2.0), scaled);
    p.restore();
    p.setPen(QPen(matchColor(status), w));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(w / 2, w / 2, size - w, size - w), radius, radius);
    return pixmap;
}

} // namespace

FolderScanDialog::FolderScanDialog(const FaceEngine *engine, FaceDatabase *database, float threshold,
                                   const QString &directory, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_database(database)
    , m_threshold(threshold)
    , m_directory(directory)
    , m_state(std::make_shared<FolderScanState>())
    , m_folderLabel(new QLabel(this))
    , m_status(new QLabel(this))
    , m_bar(new QProgressBar(this))
    , m_filter(new QComboBox(this))
    , m_grid(new QListWidget(this))
    , m_timer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("البحث في مجلد صور"));
    resize(960, 680);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("البحث في مجلد صور"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("يكشف البرنامج الوجوه في كل صور المجلد ويتعرف عليها من القاعدة. "
                                           "اختر شخصاً من القائمة لعرض كل صوره، ثم انسخها إلى مجلد أو سجّلها في سجل ظهوره."),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *controls = new QHBoxLayout;
    m_choose = new FancyButton(Icons::Name::Folder, QStringLiteral("اختيار مجلد…"), FancyButton::Kind::Primary, this);
    connect(m_choose, &QAbstractButton::clicked, this, &FolderScanDialog::chooseFolder);
    m_stop = new FancyButton(Icons::Name::Stop, QStringLiteral("إيقاف"), FancyButton::Kind::Secondary, this);
    m_stop->setEnabled(false);
    connect(m_stop, &QAbstractButton::clicked, this, &FolderScanDialog::stop);
    m_recursive = new ToggleSwitch(QStringLiteral("يشمل المجلدات الفرعية"), this);
    m_recursive->setChecked(true);
    m_thorough = new ToggleSwitch(QStringLiteral("تحليل معمّق (أبطأ)"), this);
    m_thorough->setToolTip(QStringLiteral("يجد الوجوه الصغيرة والمائلة، لكنه أبطأ بعدة مرات"));
    controls->addWidget(m_choose);
    controls->addWidget(m_stop);
    controls->addSpacing(12);
    controls->addWidget(m_recursive);
    controls->addWidget(m_thorough);
    controls->addStretch();
    layout->addLayout(controls);

    m_folderLabel->setObjectName(QStringLiteral("muted"));
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_folderLabel);
    m_bar->setObjectName(QStringLiteral("thinProgress"));
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(6);
    m_bar->setValue(0);
    layout->addWidget(m_bar);

    auto *filterRow = new QHBoxLayout;
    auto *filterLabel = new QLabel(QStringLiteral("عرض:"), this);
    m_filter->setMinimumWidth(260);
    connect(m_filter, &QComboBox::currentIndexChanged, this, &FolderScanDialog::applyFilter);
    filterRow->addWidget(filterLabel);
    filterRow->addWidget(m_filter);
    filterRow->addSpacing(12);
    filterRow->addWidget(m_status, 1);
    m_status->setObjectName(QStringLiteral("muted"));
    layout->addLayout(filterRow);
    rebuildFilter();

    m_grid->setObjectName(QStringLiteral("photoGrid"));
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setIconSize(QSize(112, 112));
    m_grid->setGridSize(QSize(164, 190));
    m_grid->setTextElideMode(Qt::ElideRight);
    m_grid->setWordWrap(true);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_grid, 1);
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_hits.at(item->data(HitRole).toInt()).path));
    });
    connect(m_grid, &QWidget::customContextMenuRequested, this, &FolderScanDialog::showMenu);

    auto *buttons = new QHBoxLayout;
    m_copy = new FancyButton(Icons::Name::Export, QStringLiteral("نسخ الصور المعروضة إلى مجلد…"), FancyButton::Kind::Secondary, this);
    connect(m_copy, &QAbstractButton::clicked, this, &FolderScanDialog::copyVisible);
    m_record = new FancyButton(Icons::Name::History, QStringLiteral("تسجيل في سجل الظهور"), FancyButton::Kind::Secondary, this);
    m_record->setToolTip(QStringLiteral("يضيف الصور التي تأكد فيها التعرف إلى سجل ظهور كل شخص"));
    connect(m_record, &QAbstractButton::clicked, this, &FolderScanDialog::recordSightings);
    auto *close = new FancyButton(Icons::Name::Stop, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, this);
    connect(close, &QAbstractButton::clicked, this, &FolderScanDialog::reject);
    buttons->addWidget(m_copy);
    buttons->addWidget(m_record);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    m_copy->setEnabled(false);
    m_record->setEnabled(false);

    m_status->setText(QStringLiteral("اختر مجلداً للبدء"));
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &FolderScanDialog::drain);
    connect(&m_watcher, &QFutureWatcherBase::finished, this, &FolderScanDialog::onFinished);
}

FolderScanDialog::~FolderScanDialog()
{
    m_state->cancel = true;
    m_watcher.waitForFinished();
}

void FolderScanDialog::reject()
{
    stop();
    QDialog::reject();
}

void FolderScanDialog::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("اختيار مجلد الصور"), m_directory);
    if (!folder.isEmpty())
        start(folder);
}

void FolderScanDialog::start(const QString &folder)
{
    if (m_watcher.isRunning())
        return;
    m_folder = folder;
    m_directory = folder;
    m_folderLabel->setText(QDir::toNativeSeparators(folder));
    m_hits.clear();
    m_personCounts.clear();
    m_personNames.clear();
    m_grid->clear();
    rebuildFilter();
    m_state = std::make_shared<FolderScanState>();
    m_choose->setEnabled(false);
    m_recursive->setEnabled(false);
    m_thorough->setEnabled(false);
    m_stop->setEnabled(true);
    m_copy->setEnabled(false);
    m_record->setEnabled(false);
    m_bar->setMaximum(0); // متحرك إلى أن يُعرف عدد الصور
    m_status->setText(QStringLiteral("جارٍ جمع الصور…"));

    const FaceEngine *engine = m_engine;
    const GalleryPtr gallery = m_database ? m_database->gallery() : GalleryPtr();
    const float threshold = m_threshold;
    const bool recursive = m_recursive->isChecked();
    const bool thorough = m_thorough->isChecked();
    const std::shared_ptr<FolderScanState> state = m_state;
    m_watcher.setFuture(QtConcurrent::run([engine, gallery, threshold, folder, recursive, thorough, state] {
        scanFolder(engine, gallery, threshold, folder, recursive, thorough, state);
    }));
    m_timer->start();
}

void FolderScanDialog::stop()
{
    m_state->cancel = true;
}

void FolderScanDialog::drain()
{
    QList<FolderHit> hits;
    {
        QMutexLocker locker(&m_state->mutex);
        hits.swap(m_state->pending);
    }
    if (!hits.isEmpty()) {
        const int before = m_personCounts.size();
        for (const FolderHit &hit : std::as_const(hits))
            addHit(hit);
        if (m_personCounts.size() != before)
            rebuildFilter();
        else {
            // تحديث الأعداد في القائمة دون إعادة بنائها حتى لا يضيع اختيار المستخدم
            for (int i = 0; i < m_filter->count(); ++i) {
                const QString id = m_filter->itemData(i).toString();
                if (m_personCounts.contains(id))
                    m_filter->setItemText(i, QStringLiteral("%1 (%2)").arg(m_personNames.value(id)).arg(m_personCounts.value(id)));
            }
        }
    }
    const int total = m_state->total;
    if (total > 0) {
        m_bar->setMaximum(total);
        m_bar->setValue(m_state->done);
    }
    updateStatus();
}

void FolderScanDialog::addHit(const FolderHit &hit)
{
    const int index = int(m_hits.size());
    m_hits << hit;
    if (hit.match.status == MatchStatus::Confirmed && !hit.match.personId.isEmpty()) {
        m_personCounts[hit.match.personId] += 1;
        m_personNames.insert(hit.match.personId, hit.match.name);
    }
    auto *item = new QListWidgetItem(m_grid);
    item->setIcon(QIcon(hitIcon(hit.crop, hit.match.status, 224)));
    // سطر للاسم وسطر للنسبة واسم الملف، فلا يقطع أحدهما الآخر
    const QString detail = hit.match.status == MatchStatus::Unknown
        ? QFileInfo(hit.path).fileName()
        : QStringLiteral("%1% · %2").arg(qRound(hit.match.score * 100)).arg(QFileInfo(hit.path).fileName());
    item->setText(QStringLiteral("%1\n%2").arg(matchLabel(hit.match), detail));
    item->setToolTip(QDir::toNativeSeparators(hit.path));
    item->setData(HitRole, index);
    item->setHidden(!hitVisible(hit));
}

void FolderScanDialog::rebuildFilter()
{
    const QString current = m_filter->currentData().toString();
    const QSignalBlocker blocker(m_filter);
    m_filter->clear();
    m_filter->addItem(QStringLiteral("كل الوجوه"), AllFaces);
    m_filter->addItem(QStringLiteral("المعروفون فقط"), KnownFaces);
    m_filter->addItem(QStringLiteral("غير المعروفين"), UnknownFaces);
    QStringList ids = m_personCounts.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        return m_personCounts.value(a) > m_personCounts.value(b);
    });
    if (!ids.isEmpty())
        m_filter->insertSeparator(m_filter->count());
    for (const QString &id : std::as_const(ids))
        m_filter->addItem(QStringLiteral("%1 (%2)").arg(m_personNames.value(id)).arg(m_personCounts.value(id)), id);
    const int index = m_filter->findData(current);
    m_filter->setCurrentIndex(index >= 0 ? index : 0);
}

bool FolderScanDialog::hitVisible(const FolderHit &hit) const
{
    const QString filter = m_filter->currentData().toString();
    if (filter.isEmpty() || filter == AllFaces)
        return true;
    if (filter == KnownFaces)
        return hit.match.status == MatchStatus::Confirmed;
    if (filter == UnknownFaces)
        return hit.match.status != MatchStatus::Confirmed;
    return hit.match.status == MatchStatus::Confirmed && hit.match.personId == filter;
}

void FolderScanDialog::applyFilter()
{
    m_grid->setUpdatesEnabled(false);
    for (int i = 0; i < m_grid->count(); ++i) {
        QListWidgetItem *item = m_grid->item(i);
        item->setHidden(!hitVisible(m_hits.at(item->data(HitRole).toInt())));
    }
    m_grid->setUpdatesEnabled(true);
    updateStatus();
}

QStringList FolderScanDialog::visiblePaths() const
{
    QStringList paths;
    QSet<QString> seen;
    for (int i = 0; i < m_grid->count(); ++i) {
        const QListWidgetItem *item = m_grid->item(i);
        if (item->isHidden())
            continue;
        const QString &path = m_hits.at(item->data(HitRole).toInt()).path;
        if (!seen.contains(path)) {
            seen.insert(path);
            paths << path;
        }
    }
    return paths;
}

void FolderScanDialog::updateStatus()
{
    const bool running = m_watcher.isRunning();
    int shown = 0;
    for (int i = 0; i < m_grid->count(); ++i)
        shown += m_grid->item(i)->isHidden() ? 0 : 1;
    const QString summary = QStringLiteral("%1 صورة · %2 وجه · %3 شخص معروف · المعروض %4")
                                .arg(int(m_state->done))
                                .arg(int(m_state->faces))
                                .arg(m_personCounts.size())
                                .arg(shown);
    if (running && m_state->total > 0)
        m_status->setText(QStringLiteral("جارٍ البحث… %1 من %2 · %3")
                              .arg(int(m_state->done))
                              .arg(int(m_state->total))
                              .arg(summary));
    else if (!running && !m_folder.isEmpty())
        m_status->setText(summary);
    const bool any = !running && shown > 0;
    m_copy->setEnabled(any);
    m_record->setEnabled(!running && !m_personCounts.isEmpty() && m_database);
}

void FolderScanDialog::onFinished()
{
    m_timer->stop();
    drain();
    m_bar->setMaximum(1);
    m_bar->setValue(1);
    m_choose->setEnabled(true);
    m_recursive->setEnabled(true);
    m_thorough->setEnabled(true);
    m_stop->setEnabled(false);
    updateStatus();
    if (m_state->total == 0)
        m_status->setText(QStringLiteral("لا توجد صور في هذا المجلد"));
    else if (m_state->cancel)
        m_status->setText(QStringLiteral("أُوقف البحث · ") + m_status->text());
}

void FolderScanDialog::copyVisible()
{
    const QStringList paths = visiblePaths();
    if (paths.isEmpty())
        return;
    const QString target = QFileDialog::getExistingDirectory(this, QStringLiteral("نسخ %1 صورة إلى").arg(paths.size()), m_directory);
    if (target.isEmpty())
        return;
    int copied = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        QString destination = QDir(target).filePath(info.fileName());
        // صور بنفس الاسم من مجلدات فرعية مختلفة لا تكتب فوق بعضها
        for (int n = 2; QFileInfo::exists(destination); ++n)
            destination = QDir(target).filePath(QStringLiteral("%1 (%2).%3").arg(info.completeBaseName()).arg(n).arg(info.suffix()));
        copied += QFile::copy(path, destination) ? 1 : 0;
    }
    Toast::show(this, QStringLiteral("نُسخت %1 من %2 صورة").arg(copied).arg(paths.size()),
                copied == paths.size() ? Toast::Kind::Success : Toast::Kind::Warning);
}

void FolderScanDialog::recordSightings()
{
    if (!m_database)
        return;
    // أفضل وجه لكل شخص في كل صورة
    QHash<QString, int> best;
    for (int i = 0; i < m_hits.size(); ++i) {
        const FolderHit &hit = m_hits.at(i);
        if (hit.match.status != MatchStatus::Confirmed || hit.match.personId.isEmpty())
            continue;
        const QString key = hit.match.personId + QLatin1Char('\n') + hit.path;
        const auto it = best.constFind(key);
        if (it == best.constEnd() || m_hits.at(*it).match.score < hit.match.score)
            best.insert(key, i);
    }
    m_database->beginBatch();
    for (const int index : std::as_const(best)) {
        const FolderHit &hit = m_hits.at(index);
        m_database->addSighting(hit.match.personId, hit.path, -1, hit.match.score, hit.crop);
    }
    m_database->endBatch();
    m_record->setEnabled(false);
    Toast::show(this, QStringLiteral("سُجّلت %1 مرة ظهور لـ %2 شخص").arg(best.size()).arg(m_personCounts.size()));
}

void FolderScanDialog::showMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_grid->itemAt(pos);
    if (!item)
        return;
    const FolderHit hit = m_hits.at(item->data(HitRole).toInt());
    QMenu menu(this);
    menu.addAction(QStringLiteral("فتح الصورة"), this, [hit] { QDesktopServices::openUrl(QUrl::fromLocalFile(hit.path)); });
    menu.addAction(QStringLiteral("تحليلها في صفحة التعرف"), this, [this, hit] {
        emit analyzeRequested(hit.path);
    });
    menu.addAction(QStringLiteral("فتح المجلد الذي فيه الصورة"), this, [hit] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(hit.path).absolutePath()));
    });
    if (hit.match.status == MatchStatus::Confirmed) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("عرض كل صور %1").arg(hit.match.name), this, [this, hit] {
            const int index = m_filter->findData(hit.match.personId);
            if (index >= 0)
                m_filter->setCurrentIndex(index);
        });
    }
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
}

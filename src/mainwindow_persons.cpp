// النافذة الرئيسية: صفحة قاعدة البيانات (الأشخاص وصورهم وملفاتهم)

#include "mainwindow_p.h"

using namespace MainWindowHelpers;

QWidget *MainWindow::createDatabasePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(26, 22, 26, 18);
    layout->setSpacing(14);

    auto *addPersonButton = new FancyButton(Icons::Name::UserPlus, QStringLiteral("شخص جديد"), FancyButton::Kind::Primary, page);
    auto *openFolderButton = new FancyButton(Icons::Name::Folder, QStringLiteral("فتح المجلد"), FancyButton::Kind::Secondary, page);
    connect(addPersonButton, &QAbstractButton::clicked, this, &MainWindow::addPerson);
    connect(openFolderButton, &QAbstractButton::clicked, this, &MainWindow::openDatabaseFolder);
    auto *alertLogButton = new FancyButton(Icons::Name::History, QStringLiteral("السجلات والتقارير"), FancyButton::Kind::Secondary, page);
    alertLogButton->setToolTip(QStringLiteral("سجل التنبيهات، الزوار المجهولون، تسجيلات الكاميرات، تقرير اليوم، سجل الأحداث"));
    connect(alertLogButton, &QAbstractButton::clicked, this, [this, alertLogButton] {
        QMenu menu(this);
        addRecordsMenu(&menu);
        menu.exec(alertLogButton->mapToGlobal(QPoint(0, alertLogButton->height())));
    });
    m_importButton = new FancyButton(Icons::Name::Collect, QStringLiteral("استيراد"), FancyButton::Kind::Secondary, page);
    m_importButton->setToolTip(QStringLiteral("إضافة أشخاص كثيرين دفعة واحدة من مجلد: كل مجلد فرعي شخص وفيه صوره"));
    m_importButton->setEnabled(false);
    connect(m_importButton, &QAbstractButton::clicked, this, &MainWindow::importPersons);
    auto *backupButton = new FancyButton(Icons::Name::Save, QStringLiteral("نسخ احتياطي"), FancyButton::Kind::Secondary, page);
    backupButton->setToolTip(QStringLiteral("حفظ نسخة احتياطية من القاعدة كلها في ملف واحد، أو استعادتها"));
    connect(backupButton, &QAbstractButton::clicked, this, [this, backupButton] {
        QMenu menu(this);
        menu.addAction(Icons::icon(Icons::Name::Save, Theme::Text, 18), QStringLiteral("إنشاء نسخة احتياطية…"), this, &MainWindow::backupDatabase);
        menu.addAction(Icons::icon(Icons::Name::History, Theme::Text, 18), QStringLiteral("استعادة من نسخة احتياطية…"), this, &MainWindow::restoreDatabase);
        menu.exec(backupButton->mapToGlobal(QPoint(0, backupButton->height())));
    });

    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(label(QStringLiteral("قاعدة البيانات"), QStringLiteral("title"), page));
    m_databaseSubtitle = label(QString(), QStringLiteral("subtitle"), page);
    titles->addWidget(m_databaseSubtitle);
    m_databaseSubtitle->setWordWrap(true);
    m_subtitles << m_databaseSubtitle;
    header->addLayout(titles, 1);
    header->addWidget(addPersonButton);
    header->addWidget(m_importButton);
    header->addWidget(alertLogButton);
    header->addWidget(backupButton);
    header->addWidget(openFolderButton);
    m_headerButtons << addPersonButton << m_importButton << alertLogButton << backupButton << openFolderButton;
    layout->addLayout(header);

    // قائمة الأشخاص
    auto *listCard = card(page);
    listCard->setMinimumWidth(220);
    auto *listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(14, 14, 14, 14);
    listLayout->setSpacing(10);
    m_personFilter = new QLineEdit(listCard);
    m_personFilter->setPlaceholderText(QStringLiteral("بحث بالاسم أو رقم الهوية…"));
    m_personFilter->setClearButtonEnabled(true);
    m_personFilter->addAction(Icons::icon(Icons::Name::Search, Theme::Muted, 18), QLineEdit::LeadingPosition);
    // البحث يجري في القاعدة بعد توقف الكتابة قليلاً، لا مع كل حرف
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(250);
    connect(m_personFilter, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_filterTimer, &QTimer::timeout, this, &MainWindow::applyPersonFilter);
    m_personModel = new PersonListModel(&m_database, this);
    m_personList = new QListView(listCard);
    m_personList->setModel(m_personModel);
    m_personList->setItemDelegate(new PersonCardDelegate(m_personList));
    m_personList->setUniformItemSizes(true);
    m_personList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_personList->setMouseTracking(true);
    connect(m_personList->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::showPersonDetails);
    connect(m_personList, &QListView::doubleClicked, this, &MainWindow::editPerson);
    listLayout->addWidget(m_personFilter);
    listLayout->addWidget(m_personList, 1);

    // تفاصيل الشخص
    auto *detailsCard = card(page);
    auto *detailsLayout = new QVBoxLayout(detailsCard);
    detailsLayout->setContentsMargins(22, 20, 22, 18);
    detailsLayout->setSpacing(14);

    // حالة «لم يُختر أحد»: أيقونة في دائرة مضيئة ونص قصير وزر لإضافة شخص
    m_personEmpty = new QWidget(detailsCard);
    auto *emptyLayout = new QVBoxLayout(m_personEmpty);
    emptyLayout->setSpacing(10);
    emptyLayout->addStretch(3);
    auto *emptyIcon = new QLabel(m_personEmpty);
    {
        const qreal dpr = devicePixelRatioF();
        QPixmap badge(QSize(88, 88) * dpr);
        badge.setDevicePixelRatio(dpr);
        badge.fill(Qt::transparent);
        QPainter painter(&badge);
        painter.setRenderHint(QPainter::Antialiasing);
        QRadialGradient glow(QPointF(44, 44), 44);
        glow.setColorAt(0.0, Theme::withAlpha(Theme::Accent, 90));
        glow.setColorAt(1.0, Theme::withAlpha(Theme::Accent, 25));
        painter.setPen(QPen(Theme::withAlpha(Theme::Accent2, 60), 1.0));
        painter.setBrush(glow);
        painter.drawEllipse(QRectF(1, 1, 86, 86));
        Icons::paint(painter, Icons::Name::Users, QRectF(26, 26, 36, 36), Theme::Accent2);
        painter.end();
        emptyIcon->setPixmap(badge);
    }
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon);
    auto *emptyTitle = label(QStringLiteral("اختر شخصاً لعرض ملفه"), QStringLiteral("sectionTitle"), m_personEmpty);
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    m_personEmptyText = label(QString(), QStringLiteral("muted"), m_personEmpty);
    m_personEmptyText->setAlignment(Qt::AlignCenter);
    m_personEmptyText->setWordWrap(true);
    m_personEmptyText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    emptyLayout->addWidget(m_personEmptyText);
    auto *emptyAdd = new FancyButton(Icons::Name::UserPlus, QStringLiteral("شخص جديد"), FancyButton::Kind::Primary, m_personEmpty);
    connect(emptyAdd, &QAbstractButton::clicked, this, &MainWindow::addPerson);
    emptyLayout->addSpacing(6);
    emptyLayout->addWidget(emptyAdd, 0, Qt::AlignCenter);
    emptyLayout->addStretch(4);
    detailsLayout->addWidget(m_personEmpty, 1);

    m_personProfile = new QWidget(detailsCard);
    auto *profile = new QHBoxLayout(m_personProfile);
    profile->setContentsMargins(0, 0, 0, 0);
    profile->setSpacing(18);
    m_personAvatar = new QLabel(detailsCard);
    m_personAvatar->setFixedSize(96, 96);
    profile->addWidget(m_personAvatar, 0, Qt::AlignTop);
    m_personDetails = new QLabel(detailsCard);
    m_personDetails->setTextFormat(Qt::RichText);
    m_personDetails->setWordWrap(true);
    m_personDetails->setAlignment(Qt::AlignTop);
    m_personDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    profile->addWidget(m_personDetails, 1);
    m_strengthMeter = new StrengthMeter(detailsCard);
    profile->addWidget(m_strengthMeter, 0, Qt::AlignTop);
    detailsLayout->addWidget(m_personProfile);

    // تنبيه احتمال التكرار: شخص آخر في القاعدة يشبهه بدرجة عالية
    m_duplicateBanner = card(detailsCard, QStringLiteral("statCard"));
    auto *duplicateLayout = new QHBoxLayout(m_duplicateBanner);
    duplicateLayout->setContentsMargins(12, 8, 12, 8);
    auto *duplicateIcon = new QLabel(m_duplicateBanner);
    duplicateIcon->setPixmap(Icons::icon(Icons::Name::Copy, Theme::Accent2, 20).pixmap(20, 20));
    duplicateLayout->addWidget(duplicateIcon);
    m_duplicateText = new QLabel(m_duplicateBanner);
    m_duplicateText->setWordWrap(true);
    duplicateLayout->addWidget(m_duplicateText, 1);
    auto *duplicateOpen = new FancyButton(Icons::Name::Users, QStringLiteral("فتح"), FancyButton::Kind::Secondary, m_duplicateBanner);
    auto *duplicateMerge = new FancyButton(Icons::Name::Merge, QStringLiteral("دمج الملفين"), FancyButton::Kind::Primary, m_duplicateBanner);
    duplicateMerge->setToolTip(QStringLiteral("ينقل صور الشخص الآخر وسجل ظهوره إلى هذا الملف ثم يحذفه"));
    connect(duplicateOpen, &QAbstractButton::clicked, this, [this] { showPerson(m_duplicateId); });
    connect(duplicateMerge, &QAbstractButton::clicked, this, &MainWindow::mergeDuplicate);
    duplicateLayout->addWidget(duplicateOpen);
    duplicateLayout->addWidget(duplicateMerge);
    m_duplicateBanner->setVisible(false);
    detailsLayout->addWidget(m_duplicateBanner);

    m_personActions = new QWidget(detailsCard);
    auto *actions = new FlowLayout(m_personActions, 10, 8);
    actions->setContentsMargins(0, 0, 0, 0);
    auto *editButton = new FancyButton(Icons::Name::Edit, QStringLiteral("تعديل البيانات"), FancyButton::Kind::Secondary, m_personActions);
    auto *addPhotosButton = new FancyButton(Icons::Name::PhotoAdd, QStringLiteral("إضافة صور"), FancyButton::Kind::Primary, m_personActions);
    auto *harvestButton = new FancyButton(Icons::Name::Collect, QStringLiteral("جمع صور من فيديو"), FancyButton::Kind::Secondary, m_personActions);
    harvestButton->setToolTip(QStringLiteral("يبحث عن الشخص في فيديو ويقترح أوضح لقطاته وأكثرها تنوعاً لإضافتها إلى صوره"));
    auto *exportButton = new FancyButton(Icons::Name::Export, QStringLiteral("تصدير PDF"), FancyButton::Kind::Secondary, m_personActions);
    auto *removeButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف الشخص"), FancyButton::Kind::Danger, m_personActions);
    m_watchButton = new FancyButton(Icons::Name::Alert, QStringLiteral("إضافة للمراقبة"), FancyButton::Kind::Secondary, m_personActions);
    m_watchButton->setToolTip(QStringLiteral("قائمة المراقبة: ينبّهك البرنامج بصوت وشريط تنبيه فور ظهور الشخص في صورة أو فيديو أو الكاميرا.\n"
                                             "يضيف الزر التصنيف «مراقبة» إلى بيانات الشخص (والتصنيف «مطلوب» يعمل بالمثل)."));
    connect(m_watchButton, &QAbstractButton::clicked, this, &MainWindow::toggleWatch);
    m_alertLevelButton = new FancyButton(Icons::Name::Alert, QStringLiteral("التنبيه: عادي"), FancyButton::Kind::Secondary, m_personActions);
    m_alertLevelButton->setToolTip(QStringLiteral("كيف يُنبَّه عن هذا الشخص: عادي، أو إنذار عالٍ، أو صامت، أو إشعار فقط"));
    m_alertLevelButton->setVisible(false);
    connect(m_alertLevelButton, &QAbstractButton::clicked, this, [this] {
        const QString id = selectedPersonId();
        if (id.isEmpty())
            return;
        const int current = m_database.alertLevel(id);
        QMenu menu(this);
        for (const int level : {AlertSound::Normal, AlertSound::High, AlertSound::Silent, AlertSound::NotifyOnly}) {
            QAction *action = menu.addAction(QStringLiteral("%1 — %2").arg(AlertSound::levelName(level), AlertSound::levelDescription(level)));
            action->setCheckable(true);
            action->setChecked(level == current);
            connect(action, &QAction::triggered, this, [this, id, level] {
                if (!m_database.setAlertLevel(id, level)) {
                    Toast::show(this, QStringLiteral("تعذر حفظ التغيير"), Toast::Kind::Warning);
                    return;
                }
                m_alertLevelButton->setText(QStringLiteral("التنبيه: %1").arg(AlertSound::levelName(level)));
                if (level == AlertSound::High)
                    AlertSound::play(AlertSound::Sound::Alarm);
                else if (level == AlertSound::Normal)
                    AlertSound::play(AlertSound::Sound::Chime);
            });
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("تجربة صوت التنبيه العادي"), this, [] { AlertSound::play(AlertSound::Sound::Chime); });
        menu.addAction(QStringLiteral("تجربة صوت الإنذار"), this, [] { AlertSound::play(AlertSound::Sound::Alarm); });
        menu.exec(m_alertLevelButton->mapToGlobal(QPoint(0, m_alertLevelButton->height())));
    });
    connect(editButton, &QAbstractButton::clicked, this, &MainWindow::editPerson);
    connect(addPhotosButton, &QAbstractButton::clicked, this, &MainWindow::addPhotos);
    connect(harvestButton, &QAbstractButton::clicked, this, [this] { harvestPhotos(selectedPersonId()); });
    connect(exportButton, &QAbstractButton::clicked, this, &MainWindow::exportPerson);
    connect(removeButton, &QAbstractButton::clicked, this, &MainWindow::removePerson);
    actions->addWidget(addPhotosButton);
    actions->addWidget(harvestButton);
    actions->addWidget(editButton);
    actions->addWidget(exportButton);
    actions->addWidget(m_watchButton);
    actions->addWidget(m_alertLevelButton);
    actions->addWidget(removeButton);
    m_harvestButton = harvestButton;
    detailsLayout->addWidget(m_personActions);

    auto *photosHeader = new QHBoxLayout;
    m_photosTab = new FancyButton(Icons::Name::Image, QStringLiteral("صور الوجه"), FancyButton::Kind::Tab, detailsCard);
    m_sightingsTab = new FancyButton(Icons::Name::History, QStringLiteral("سجل الظهور"), FancyButton::Kind::Tab, detailsCard);
    m_profileTabs = makeTabStrip({m_photosTab, m_sightingsTab}, detailsCard);
    photosHeader->addWidget(m_profileTabs);
    m_photosTab->setChecked(true);
    m_photosHint = label(QStringLiteral("كلما زادت الصور وتنوعت زادت دقة التعرف"), QStringLiteral("muted"), detailsCard);
    photosHeader->addWidget(m_photosHint, 1);
    photosHeader->addStretch();
    m_coverButton = new FancyButton(Icons::Name::Star, QStringLiteral("صورة رئيسية"), FancyButton::Kind::Secondary, detailsCard);
    m_coverButton->setToolTip(QStringLiteral("تعيين الصورة المحددة صورةً رئيسية تمثل الشخص في القوائم"));
    connect(m_coverButton, &QAbstractButton::clicked, this, &MainWindow::setCoverPhoto);
    photosHeader->addWidget(m_coverButton);
    m_removePhotoButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف الصورة"), FancyButton::Kind::Danger, detailsCard);
    connect(m_removePhotoButton, &QAbstractButton::clicked, this, &MainWindow::removePhoto);
    photosHeader->addWidget(m_removePhotoButton);
    m_headerButtons << m_coverButton << m_removePhotoButton;
    detailsLayout->addLayout(photosHeader);

    m_photoList = new QListWidget(detailsCard);
    m_photoList->setObjectName(QStringLiteral("photoGrid"));
    m_photoList->setViewMode(QListView::IconMode);
    m_photoList->setIconSize(QSize(132, 132));
    m_photoList->setGridSize(QSize(156, 172));
    m_photoList->setResizeMode(QListView::Adjust);
    m_photoList->setMovement(QListView::Static);
    m_photoList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_photoList, &QListWidget::currentRowChanged, this, [this] {
        m_removePhotoButton->setEnabled(m_photoList->currentItem());
        m_coverButton->setEnabled(m_photoList->currentItem());
    });
    connect(m_photoList, &QListWidget::itemDoubleClicked, this, &MainWindow::setCoverPhoto);

    m_sightingList = new QListWidget(detailsCard);
    m_sightingList->setItemDelegate(new FaceCardDelegate(m_sightingList));
    m_sightingList->setUniformItemSizes(true);
    m_sightingList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_sightingList->setMouseTracking(true);
    m_sightingList->setToolTip(QStringLiteral("انقر نقراً مزدوجاً لفتح الصورة أو الفيديو الذي ظهر فيه"));
    connect(m_sightingList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const QString source = item->data(CardRole::Id).toString();
        if (!QFileInfo::exists(source)) {
            Toast::show(this, QStringLiteral("الملف غير موجود: %1").arg(QDir::toNativeSeparators(source)), Toast::Kind::Warning);
            return;
        }
        if (!m_ready || m_videoRunning || m_analyzing)
            return;
        m_navGroup->button(0)->click();
        openSighting(source, item->data(SightingPositionRole).toLongLong());
    });
    m_profileStack = new QStackedWidget(detailsCard);
    m_profileStack->addWidget(m_photoList);
    m_profileStack->addWidget(m_sightingList);
    connect(m_photosTab, &QAbstractButton::clicked, this, [this] {
        m_profileStack->setCurrentIndex(0);
        m_coverButton->setVisible(true);
        m_removePhotoButton->setVisible(!selectedPersonId().isEmpty());
    });
    connect(m_sightingsTab, &QAbstractButton::clicked, this, [this] {
        m_profileStack->setCurrentIndex(1);
        m_coverButton->setVisible(false);
        m_removePhotoButton->setVisible(false);
    });
    detailsLayout->addWidget(m_profileStack, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    splitter->setHandleWidth(14);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(listCard);
    splitter->addWidget(detailsCard);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({330, 800});
    layout->addWidget(splitter, 1);
    return page;
}

void MainWindow::toggleWatch()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    Person person = m_database.person(id);
    const bool watch = !FaceDatabase::isWatched(person.tags);
    person.tags = FaceDatabase::withWatchTag(person.tags, watch);
    if (!m_database.updatePerson(person)) {
        Toast::show(this, QStringLiteral("تعذر حفظ التغيير"), Toast::Kind::Warning);
        return;
    }
    Toast::show(this, watch ? QStringLiteral("أُضيف %1 إلى قائمة المراقبة: ستُنبَّه فور ظهوره").arg(person.name)
                            : QStringLiteral("أُزيل %1 من قائمة المراقبة").arg(person.name));
}

void MainWindow::importPersons()
{
    if (!m_ready)
        return;
    ImportDialog dialog(&m_engine, &m_database, threshold(), m_lastDirectory, this);
    connect(&dialog, &ImportDialog::showPersonRequested, this, [this](const QString &id) {
        QTimer::singleShot(0, this, [this, id] { showPerson(id); });
    });
    QTimer::singleShot(0, &dialog, &ImportDialog::chooseFolder);
    dialog.exec();
    if (!dialog.lastDirectory().isEmpty())
        m_lastDirectory = dialog.lastDirectory();
}

// جمع صور شخص من القاعدة بالبحث عنه في فيديو يختاره المستخدم
void MainWindow::harvestPhotos(const QString &personId)
{
    if (personId.isEmpty() || !m_ready)
        return;
    const std::vector<float> target = m_database.personEmbedding(personId);
    if (target.empty()) {
        Toast::show(this, QStringLiteral("أضف صورة واحدة للشخص على الأقل حتى يُبحث عنه في الفيديو"), Toast::Kind::Warning);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("اختر فيديو يظهر فيه الشخص"), m_lastDirectory, VideoFilter);
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    const Person person = m_database.person(personId);
    HarvestDialog dialog(&m_engine, path, target, {}, threshold(), person.name, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const int added = enrollFaces(personId, dialog.selectedFaces());
    Toast::show(this, QStringLiteral("أضيفت %1 صور إلى ملف %2").arg(added).arg(person.name),
                added > 0 ? Toast::Kind::Success : Toast::Kind::Warning);
}

QString MainWindow::selectedPersonId() const
{
    if (!m_personList)
        return {};
    return m_personModel->idAt(m_personList->currentIndex().row());
}

void MainWindow::refreshStats()
{
    m_personCountLabel->setText(QLocale().toString(m_database.personCount()));
    m_photoCountLabel->setText(QLocale().toString(m_database.photoCount()));
    updateDatabaseSubtitle();
}

void MainWindow::updateDatabaseSubtitle()
{
    const QString filter = m_personModel ? m_personModel->filter() : QString();
    if (filter.isEmpty()) {
        m_databaseSubtitle->setText(QStringLiteral("%1 شخص مسجل · %2 صورة وجه · محفوظة على هذا الجهاز فقط")
                                        .arg(QLocale().toString(m_database.personCount()), QLocale().toString(m_database.photoCount())));
        return;
    }
    constexpr qint64 Cap = 10000;
    const qint64 count = m_database.countPersons(filter, Cap + 1);
    m_databaseSubtitle->setText(count > Cap ? QStringLiteral("نتائج البحث: أكثر من %1").arg(QLocale().toString(Cap))
                                            : QStringLiteral("نتائج البحث: %1").arg(QLocale().toString(count)));
}

void MainWindow::refreshPersons()
{
    if (!m_personList)
        return;
    const QString selected = selectedPersonId();
    {
        const QSignalBlocker blocker(m_personList->selectionModel());
        m_personModel->reload();
        const int row = m_personModel->rowOf(selected);
        if (row >= 0)
            m_personList->setCurrentIndex(m_personModel->index(row));
    }
    showPersonDetails();
}

// يفتح ملف شخص في صفحة قاعدة البيانات
void MainWindow::showPerson(const QString &personId)
{
    const Person person = m_database.person(personId);
    if (person.id.isEmpty())
        return;
    m_navGroup->button(1)->click();
    int row = m_personModel->rowOf(personId);
    if (row < 0) {
        m_personFilter->setText(person.name);
        m_filterTimer->stop();
        applyPersonFilter();
        row = m_personModel->rowOf(personId);
    }
    if (row >= 0)
        m_personList->setCurrentIndex(m_personModel->index(row));
}

void MainWindow::applyPersonFilter()
{
    {
        const QSignalBlocker blocker(m_personList->selectionModel());
        m_personModel->setFilter(m_personFilter->text());
    }
    updateDatabaseSubtitle();
    showPersonDetails();
}

void MainWindow::showPersonDetails()
{
    const QString id = selectedPersonId();
    m_photoList->clear();
    m_sightingList->clear();
    m_removePhotoButton->setEnabled(false);
    m_coverButton->setEnabled(false);
    m_personActions->setVisible(!id.isEmpty());
    const bool photosShown = m_profileStack->currentIndex() == 0;
    m_removePhotoButton->setVisible(!id.isEmpty() && photosShown);
    m_coverButton->setVisible(!id.isEmpty() && photosShown);
    m_strengthMeter->setVisible(!id.isEmpty());
    m_profileTabs->setVisible(!id.isEmpty());
    m_profileStack->setVisible(!id.isEmpty());
    m_duplicateBanner->setVisible(false);
    m_duplicateId.clear();

    m_personEmpty->setVisible(id.isEmpty());
    m_personProfile->setVisible(!id.isEmpty());
    if (id.isEmpty()) {
        m_personAvatar->setPixmap(QPixmap());
        const qint64 count = m_database.personCount();
        m_personEmptyText->setText(count > 0 ? QStringLiteral("في القاعدة \u2066%1\u2069 أشخاص. اختر أحدهم من القائمة لعرض بياناته وصوره وسجل ظهوره،\nأو أضف شخصاً جديداً.").arg(count)
                                             : QStringLiteral("القاعدة فارغة. أضف أول شخص مع صورة واضحة لوجهه ليبدأ التعرف عليه."));
        m_photosTab->setText(QStringLiteral("صور الوجه"));
        m_sightingsTab->setText(QStringLiteral("سجل الظهور"));
        return;
    }

    const Person person = m_database.person(id);
    const bool watched = FaceDatabase::isWatched(person.tags);
    m_watchButton->setText(watched ? QStringLiteral("إلغاء المراقبة") : QStringLiteral("إضافة للمراقبة"));
    m_alertLevelButton->setVisible(watched);
    if (watched)
        m_alertLevelButton->setText(QStringLiteral("التنبيه: %1").arg(AlertSound::levelName(m_database.alertLevel(id))));
    m_personAvatar->setPixmap(person.coverPhoto <= 0
                                  ? Icons::icon(Icons::Name::Users, Theme::Muted, 96).pixmap(96, 96)
                                  : circularPixmap(m_database.photo(person.coverPhoto, 192), 96));

    auto chip = [](const QString &key, const QString &value) {
        return QStringLiteral("<span style='color:#a99a8a'>%1</span> <span style='color:#f4ede4'>%2</span>&nbsp;&nbsp;&nbsp; ")
            .arg(key, value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<div style='font-size:18pt; font-weight:700'>%1</div>").arg(person.name.toHtmlEscaped());
    if (!person.tags.isEmpty()) {
        html += QStringLiteral("<div style='margin:2px 0 4px 0'>");
        for (const QString &tag : person.tags.split(QStringLiteral("، "), Qt::SkipEmptyParts))
            html += QStringLiteral("<span style='background:#2b231d; color:#6cc193; font-weight:600'>&nbsp;%1&nbsp;</span>&nbsp; ")
                        .arg(tag.toHtmlEscaped());
        html += QStringLiteral("</div>");
    }
    html += QStringLiteral("<div>");
    if (!person.nationalId.isEmpty())
        html += chip(QStringLiteral("رقم الهوية:"), person.nationalId);
    if (!person.gender.isEmpty())
        html += chip(QStringLiteral("الجنس:"), person.gender);
    if (person.birthDate.isValid())
        html += chip(QStringLiteral("العمر:"), QStringLiteral("%1 سنة").arg(person.birthDate.daysTo(QDate::currentDate()) / 365));
    if (!person.phone.isEmpty())
        html += chip(QStringLiteral("الهاتف:"), person.phone);
    if (!person.email.isEmpty())
        html += chip(QStringLiteral("البريد:"), person.email);
    if (!person.address.isEmpty())
        html += chip(QStringLiteral("العنوان:"), person.address);
    html += chip(QStringLiteral("أُضيف:"), person.created.toString(QStringLiteral("yyyy/MM/dd")));
    if (person.lastSeen.isValid())
        html += chip(QStringLiteral("آخر ظهور:"), QStringLiteral("%1 (%2 مرة)")
                                                       .arg(person.lastSeen.toString(QStringLiteral("yyyy/MM/dd hh:mm")))
                                                       .arg(person.seenCount));
    html += QStringLiteral("</div>");
    if (!person.notes.isEmpty())
        html += QStringLiteral("<div style='color:#a99a8a; margin-top:6px'>%1</div>")
                    .arg(person.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));

    // قوة الملف والصور المشكوك فيها
    const QHash<qint64, float> consistency = m_database.photoConsistency(id);
    const ProfileStrength strength = profileStrength(person, consistency);
    m_strengthMeter->setStrength(strength);
    if (!strength.tips.isEmpty())
        html += QStringLiteral("<div style='color:#f5a524; margin-top:6px'>💡 %1</div>").arg(strength.tips.first().toHtmlEscaped());
    m_personDetails->setText(html);

    for (const FacePhoto &photo : person.photos) {
        auto *item = new QListWidgetItem(m_photoList);
        const bool cover = photo.id == person.coverPhoto;
        const bool suspect = strength.suspects.contains(photo.id);
        item->setIcon(QIcon(decoratedPhoto(m_database.photo(photo.id, m_photoList->iconSize().width()), m_photoList->iconSize().width(), cover, suspect)));
        item->setText(suspect ? QStringLiteral("مشكوك فيها") : QStringLiteral("جودة %1%").arg(qRound(photo.quality * 100)));
        if (suspect)
            item->setForeground(QColor(0xfb, 0x71, 0x85));
        QString tip = QStringLiteral("جودة الوجه: %1%").arg(qRound(photo.quality * 100));
        if (consistency.contains(photo.id))
            tip += QStringLiteral("\nالتشابه مع بقية صوره: %1%").arg(qRound(consistency.value(photo.id) * 100));
        if (photo.added.isValid())
            tip += QStringLiteral("\nأُضيفت: %1").arg(photo.added.toString(QStringLiteral("yyyy/MM/dd hh:mm")));
        if (cover)
            tip += QStringLiteral("\n★ الصورة الرئيسية");
        if (suspect)
            tip += QStringLiteral("\nقد تكون لشخص آخر — راجعها");
        tip += QStringLiteral("\nانقر نقراً مزدوجاً لجعلها الصورة الرئيسية");
        item->setToolTip(tip);
        item->setData(Qt::UserRole, photo.id);
    }
    m_photosTab->setText(QStringLiteral("صور الوجه (%1)").arg(person.photos.size()));

    const QList<Sighting> sightings = m_database.sightings(id, 200);
    for (const Sighting &sighting : sightings) {
        auto *item = new QListWidgetItem(m_sightingList);
        item->setData(CardRole::Id, sighting.source);
        item->setData(SightingPositionRole, sighting.positionMs);
        item->setData(CardRole::Title, sighting.seen.toString(QStringLiteral("yyyy/MM/dd  hh:mm")));
        QString where = sighting.source.startsWith(QStringLiteral("camera:")) ? Cameras::displayName(sighting.source)
                                                                               : QFileInfo(sighting.source).fileName();
        if (!sighting.camera.isEmpty() && !sighting.source.startsWith(QStringLiteral("camera:")))
            where = Cameras::displayName(sighting.camera) + QStringLiteral(" · ") + where;
        if (sighting.positionMs >= 0)
            where += QStringLiteral(" · عند %1").arg(ImageIO::formatTime(sighting.positionMs));
        item->setData(CardRole::Subtitle, where);
        item->setData(CardRole::Status, int(MatchStatus::Confirmed));
        item->setData(CardRole::StatusText, !sighting.camera.isEmpty() || sighting.source.startsWith(QStringLiteral("camera:"))
                                                ? QStringLiteral("كاميرا")
                                                : sighting.positionMs >= 0 ? QStringLiteral("فيديو")
                                                                           : QStringLiteral("صورة"));
        item->setData(CardRole::Score, sighting.score);
        item->setData(CardRole::Avatar, circularPixmap(sighting.thumbnail, 54));
        item->setToolTip(QDir::toNativeSeparators(sighting.source));
    }
    m_sightingsTab->setText(QStringLiteral("سجل الظهور (%1)").arg(person.seenCount));

    // هل في القاعدة شخص آخر يشبهه كثيراً؟ ربما أُضيف الشخص نفسه مرتين
    const std::vector<float> embedding = m_database.personEmbedding(id);
    if (!embedding.empty()) {
        GalleryPtr gallery = m_database.gallery();
        const QList<MatchResult> similar = FaceDatabase::candidates(*gallery, embedding, 1, threshold(), id);
        if (!similar.isEmpty() && similar.first().score >= std::max(0.45f, threshold() + 0.1f)) {
            m_duplicateId = similar.first().personId;
            m_duplicateText->setText(QStringLiteral("قد يكون هذا الشخص مسجلاً مرتين: يشبه <b>%1</b> بنسبة %2%")
                                         .arg(similar.first().name.toHtmlEscaped())
                                         .arg(qRound(similar.first().score * 100)));
            m_duplicateBanner->setVisible(true);
        }
    }
}

void MainWindow::setCoverPhoto()
{
    const QString id = selectedPersonId();
    const QListWidgetItem *item = m_photoList->currentItem();
    if (id.isEmpty() || !item)
        return;
    if (m_database.setCoverPhoto(id, item->data(Qt::UserRole).toLongLong()))
        Toast::show(this, QStringLiteral("تم تعيين الصورة الرئيسية"));
}

void MainWindow::mergeDuplicate()
{
    const QString id = selectedPersonId();
    if (id.isEmpty() || m_duplicateId.isEmpty())
        return;
    const Person keep = m_database.person(id);
    const Person other = m_database.person(m_duplicateId);
    const auto answer = QMessageBox::question(
        this, QStringLiteral("دمج الملفين"),
        QStringLiteral("سيُنقل كل ما في ملف \"%1\" (%2 صور وسجل ظهوره) إلى ملف \"%3\" ثم يُحذف ملفه.\n"
                       "البيانات الفارغة هنا تُكمَّل من الملف الآخر. هل تريد المتابعة؟")
            .arg(other.name)
            .arg(other.photoCount)
            .arg(keep.name));
    if (answer != QMessageBox::Yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_database.mergePersons(id, m_duplicateId);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        Toast::show(this, QStringLiteral("تعذر دمج الملفين"), Toast::Kind::Warning);
        return;
    }
    Toast::show(this, QStringLiteral("تم دمج \"%1\" في \"%2\"").arg(other.name, keep.name));
    showPerson(id);
}

void MainWindow::exportPerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    const Person person = m_database.person(id);
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("تصدير ملف الشخص"),
                                                QDir(m_lastDirectory).filePath(safeFileName(person.name) + QStringLiteral(".pdf")),
                                                QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        path += QStringLiteral(".pdf");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = exportPersonPdf(m_database, id, path);
    QApplication::restoreOverrideCursor();
    if (ok) {
        Toast::show(this, QStringLiteral("تم تصدير ملف %1").arg(person.name));
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else {
        Toast::show(this, QStringLiteral("تعذر إنشاء ملف PDF"), Toast::Kind::Warning);
    }
}

void MainWindow::addPerson()
{
    PersonDialog dialog(Person(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString id = m_database.addPerson(dialog.person());
    if (id.isEmpty()) {
        Toast::show(this, QStringLiteral("تعذر حفظ الشخص"), Toast::Kind::Warning);
        return;
    }
    // الأحدث أولاً، فالشخص الجديد في أول القائمة
    m_personFilter->clear();
    applyPersonFilter();
    m_personModel->reload();
    const int row = m_personModel->rowOf(id);
    if (row >= 0)
        m_personList->setCurrentIndex(m_personModel->index(row));
    Toast::show(this, QStringLiteral("تمت إضافة %1 — أضف الآن صوراً لوجهه").arg(dialog.person().name));
    if (m_ready)
        addPhotosTo(id);
}

void MainWindow::editPerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    PersonDialog dialog(m_database.person(id), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (m_database.updatePerson(dialog.person()))
        Toast::show(this, QStringLiteral("تم حفظ التعديلات"));
    else
        Toast::show(this, QStringLiteral("تعذر حفظ التعديلات"), Toast::Kind::Warning);
}

void MainWindow::removePerson()
{
    const QString id = selectedPersonId();
    if (id.isEmpty())
        return;
    const Person person = m_database.person(id);
    const auto answer = QMessageBox::question(this, QStringLiteral("حذف الشخص"),
                                              QStringLiteral("هل تريد حذف \"%1\" وكل صوره من قاعدة البيانات؟").arg(person.name));
    if (answer != QMessageBox::Yes)
        return;
    if (m_database.removePerson(id))
        Toast::show(this, QStringLiteral("تم حذف %1").arg(person.name), Toast::Kind::Info);
    else
        Toast::show(this, QStringLiteral("تعذر حذف الشخص"), Toast::Kind::Warning);
}

void MainWindow::addPhotos()
{
    const QString id = selectedPersonId();
    if (!id.isEmpty() && m_ready)
        addPhotosTo(id);
}

void MainWindow::addPhotosTo(const QString &personId)
{
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("اختر صوراً لوجه الشخص"), m_lastDirectory, ImageFilter);
    if (files.isEmpty())
        return;
    m_lastDirectory = QFileInfo(files.first()).absolutePath();
    addPhotoFiles(personId, files);
}

void MainWindow::addPhotoFiles(const QString &personId, const QStringList &files)
{
    struct Enrollment
    {
        int faces = 0;
        FaceItem face;
        std::vector<std::vector<float>> embeddings;
    };

    BatchProgressDialog progress(int(files.size()), this);
    progress.show();

    int added = 0;
    QStringList noFace;
    QStringList multipleFaces;
    const FaceEngine *engine = &m_engine;
    for (int i = 0; i < files.size() && progress.proceed(); ++i) {
        const QString file = files.at(i);
        progress.setProgress(i, QStringLiteral("جارٍ تحليل %1 (%2 من %3)…").arg(QFileInfo(file).fileName()).arg(i + 1).arg(files.size()));

        // التحليل في الخلفية حتى تبقى الواجهة وأزرار الإيقاف المؤقت والإلغاء مستجيبة
        const Enrollment result = runResponsive([engine, file] {
            Enrollment enrollment;
            const cv::Mat image = ImageIO::loadImage(file);
            std::vector<AnalyzedFace> faces = image.empty() ? std::vector<AnalyzedFace>() : engine->analyze(image, true);
            if (faces.empty() && !image.empty())
                faces = engine->analyzeRobust(image);
            enrollment.faces = int(faces.size());
            if (faces.empty())
                return enrollment;
            // عند وجود أكثر من وجه يُعتمد الأكبر
            const auto largest = std::max_element(faces.begin(), faces.end(), [](const AnalyzedFace &a, const AnalyzedFace &b) {
                return a.detection.box.area() < b.detection.box.area();
            });
            enrollment.face = ImageIO::makeFaceItem(image, *largest, 0);
            enrollment.embeddings = engine->enrollmentEmbeddings(largest->aligned);
            return enrollment;
        }, true);
        if (result.faces == 0) {
            noFace << QFileInfo(file).fileName();
            continue;
        }
        if (result.faces > 1)
            multipleFaces << QFileInfo(file).fileName();
        if (m_database.addPhoto(personId, result.face.crop, result.embeddings, result.face.quality))
            ++added;
    }
    progress.hide();

    if (noFace.isEmpty() && multipleFaces.isEmpty()) {
        Toast::show(this, QStringLiteral("أُضيفت %1 صورة").arg(added));
        return;
    }
    QString message = QStringLiteral("أُضيفت %1 صورة.").arg(added);
    if (!noFace.isEmpty())
        message += QStringLiteral("\n\nلم يُعثر على وجه في:\n") + noFace.join(QLatin1Char('\n'));
    if (!multipleFaces.isEmpty())
        message += QStringLiteral("\n\nفيها أكثر من وجه (اعتُمد الوجه الأكبر):\n") + multipleFaces.join(QLatin1Char('\n'));
    QMessageBox::information(this, QStringLiteral("إضافة صور"), message);
}

void MainWindow::removePhoto()
{
    const QString id = selectedPersonId();
    const QListWidgetItem *item = m_photoList->currentItem();
    if (id.isEmpty() || !item)
        return;
    if (QMessageBox::question(this, QStringLiteral("حذف الصورة"), QStringLiteral("هل تريد حذف هذه الصورة؟")) != QMessageBox::Yes)
        return;
    if (m_database.removePhoto(id, item->data(Qt::UserRole).toLongLong()))
        Toast::show(this, QStringLiteral("تم حذف الصورة"), Toast::Kind::Info);
    else
        Toast::show(this, QStringLiteral("تعذر حذف الصورة"), Toast::Kind::Warning);
}

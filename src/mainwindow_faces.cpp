// النافذة الرئيسية: لوحة الوجوه المكتشفة؛ التبويبات بعدّاداتها، القائمة أو الشبكة، الأزرار السريعة،
// العمليات الجماعية، لوحة التفاصيل، واقتراحات الدمج

#include "facedetails.h"
#include "mainwindow_p.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QToolButton>

using namespace MainWindowHelpers;

namespace {

// تشابه متوسطي بصمتين يكفي لاقتراح أنهما الشخص نفسه؛ أقل من حد الدمج التلقائي في محلل الفيديو (0.30)
constexpr float SuggestSimilarity = 0.24f;
constexpr int MaxSuggestionFaces = 600; // الأكثر ظهوراً فقط، فالمقارنة بين كل زوجين تبقى سريعة
constexpr int MaxSuggestions = 30;

quint64 pairKey(int a, int b)
{
    return (quint64(quint32(std::min(a, b))) << 32) | quint32(std::max(a, b));
}

bool overlaps(const QList<QPair<qint64, qint64>> &a, const QList<QPair<qint64, qint64>> &b)
{
    for (const auto &x : a)
        for (const auto &y : b)
            if (x.first <= y.second && y.first <= x.second)
                return true;
    return false;
}

} // namespace

void MainWindow::setupFacePanel(QWidget *side, QVBoxLayout *sideLayout)
{
    QSettings settings(settingsPath(), QSettings::IniFormat);

    // العنوان والعدد، وبجانبهما العرض قائمة أو شبكة
    auto *sideHeader = new QHBoxLayout;
    sideHeader->setSpacing(4);
    sideHeader->addWidget(label(QStringLiteral("الوجوه المكتشفة"), QStringLiteral("sectionTitle"), side));
    sideHeader->addStretch();
    auto *viewGroup = new QButtonGroup(this);
    auto viewButton = [&](Icons::Name icon, const QString &tip) {
        auto *button = new QToolButton(side);
        button->setObjectName(QStringLiteral("tab"));
        button->setCheckable(true);
        button->setIcon(Icons::icon(icon, Theme::Text, 16));
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        viewGroup->addButton(button);
        sideHeader->addWidget(button);
        return button;
    };
    auto *listButton = viewButton(Icons::Name::List, QStringLiteral("عرض قائمة"));
    auto *gridButton = viewButton(Icons::Name::Grid, QStringLiteral("عرض شبكي"));
    sideHeader->addSpacing(4);
    m_faceCount = label(QStringLiteral("0"), QStringLiteral("badge"), side);
    sideHeader->addWidget(m_faceCount);
    sideLayout->addLayout(sideHeader);

    // البحث والترتيب
    m_faceFilter = new QLineEdit(side);
    m_faceFilter->setPlaceholderText(QStringLiteral("بحث بالاسم…"));
    m_faceFilter->setClearButtonEnabled(true);
    m_faceFilter->addAction(Icons::icon(Icons::Name::Search, Theme::Muted, 18), QLineEdit::LeadingPosition);
    m_faceSort = new SelectBox(side);
    m_faceSort->addItems({QStringLiteral("حسب الظهور"), QStringLiteral("الأكثر ظهوراً"), QStringLiteral("الأوضح"),
                          QStringLiteral("المعروفون أولاً")});
    m_faceSort->setToolTip(QStringLiteral("ترتيب القائمة"));
    connect(m_faceFilter, &QLineEdit::textChanged, this, &MainWindow::applyFaceFilter);
    connect(m_faceSort, &QComboBox::currentIndexChanged, this, &MainWindow::sortFaces);
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(6);
    searchRow->addWidget(m_faceFilter, 3);
    searchRow->addWidget(m_faceSort, 2);
    sideLayout->addLayout(searchRow);

    // تبويبات الحالة بعدّاداتها: أزرار متلاصقة تنزلق بينها الكبسولة
    const QStringList tips{QStringLiteral("كل الوجوه"), QStringLiteral("طابقوا شخصاً في القاعدة بثقة"),
                           QStringLiteral("يشبهون شخصاً في القاعدة دون تأكيد: راجعهم وأكّد أو ارفض"),
                           QStringLiteral("ليسوا في القاعدة"), QStringLiteral("من قائمة المراقبة")};
    m_faceTabs = new SegmentedControl({QStringLiteral("الكل"), QStringLiteral("معروف"), QStringLiteral("محتمل"),
                                       QStringLiteral("مجهول"), QStringLiteral("مراقَب")},
                                      side);
    m_faceTabs->setExpanding(true);
    // لا تفرض التبويبات عرضاً أكبر من اللوحة؛ النص الطويل يُختصر
    m_faceTabs->setMinimumWidth(180);
    for (int i = 0; i < tips.size(); ++i)
        m_faceTabs->setItemToolTip(i, tips.at(i));
    sideLayout->addWidget(m_faceTabs);
    auto *tabsHost = new QWidget(side);
    auto *tabs = new QHBoxLayout(tabsHost);
    tabs->setContentsMargins(0, 0, 0, 0);
    tabs->setSpacing(6);
    tabs->addWidget(label(QStringLiteral("حجم البطاقات"), QStringLiteral("muted"), tabsHost));
    // حجم البطاقات تحت التبويبات
    m_cardSizeSlider = new QSlider(Qt::Horizontal, tabsHost);
    m_cardSizeSlider->setRange(80, 160);
    m_cardSizeSlider->setSingleStep(10);
    m_cardSizeSlider->setPageStep(20);
    m_cardSizeSlider->setMinimumWidth(64);
    m_cardSizeSlider->setToolTip(QStringLiteral("حجم البطاقات"));
    m_cardSizeSlider->setFocusPolicy(Qt::NoFocus);
    tabs->addWidget(m_cardSizeSlider);
    const int savedTab = std::clamp(settings.value(QStringLiteral("faces/tab"), 0).toInt(), 0, int(tips.size()) - 1);
    m_faceTabs->setCurrentIndex(savedTab);
    connect(m_faceTabs, &SegmentedControl::currentChanged, this, [this](int id) {
        QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("faces/tab"), id);
        applyFaceFilter();
    });
    sideLayout->addWidget(tabsHost);

    // «غالباً الشخص نفسه»: يظهر حين توجد اقتراحات دمج
    m_mergeSuggestButton = new FancyButton(Icons::Name::Merge, QString(), FancyButton::Kind::Secondary, side);
    m_mergeSuggestButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_mergeSuggestButton->setToolTip(QStringLiteral("وجوه متشابهة لم تظهر معاً في وقت واحد: غالباً الشخص نفسه. راجعها وادمجها بنقرة"));
    m_mergeSuggestButton->setVisible(false);
    connect(m_mergeSuggestButton, &QAbstractButton::clicked, this, &MainWindow::showMergeSuggestions);
    sideLayout->addWidget(m_mergeSuggestButton);

    m_faceList = new QListWidget(side);
    m_faceDelegate = new FaceCardDelegate(m_faceList);
    m_faceList->setItemDelegate(m_faceDelegate);
    m_faceList->setUniformItemSizes(true);
    m_faceList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_faceList->setMouseTracking(true);
    m_faceList->setMinimumHeight(90);
    m_faceList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_faceList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_faceList->setToolTip(QStringLiteral("نقر مزدوج: تكبير الوجه في الصورة، أو عرض لقطاته في الفيديو\n"
                                          "Ctrl أو Shift لتحديد عدة وجوه للدمج أو الإضافة لشخص أو التصدير"));
    connect(m_faceDelegate, &FaceCardDelegate::actionTriggered, this, &MainWindow::onFaceAction);
    connect(m_faceList, &QListWidget::currentRowChanged, this, &MainWindow::showFaceDetails);
    connect(m_faceList, &QListWidget::itemSelectionChanged, this, &MainWindow::updateBulkBar);
    connect(m_faceList, &QListWidget::customContextMenuRequested, this, &MainWindow::showFaceContextMenu);
    connect(m_faceList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (m_videoMode)
            openShotViewer(item->data(CardRole::Id).toInt());
        else
            m_view->zoomToFace(item->data(CardRole::Id).toInt());
    });

    const bool grid = settings.value(QStringLiteral("faces/grid"), false).toBool();
    (grid ? gridButton : listButton)->setChecked(true);
    m_cardSizeSlider->setValue(settings.value(QStringLiteral("faces/size"), 110).toInt());
    connect(listButton, &QToolButton::clicked, this, [this] { setFaceView(false, m_cardSizeSlider->value()); });
    connect(gridButton, &QToolButton::clicked, this, [this] { setFaceView(true, m_cardSizeSlider->value()); });
    connect(m_cardSizeSlider, &QSlider::valueChanged, this, [this](int value) { setFaceView(m_faceDelegate->isGrid(), value); });
    setFaceView(grid, m_cardSizeSlider->value());

    // العمليات الجماعية: تظهر حين يُحدد أكثر من وجه
    m_bulkBar = new QFrame(side);
    m_bulkBar->setObjectName(QStringLiteral("bulkBar"));
    m_bulkBar->setStyleSheet(QStringLiteral("QFrame#bulkBar { background: %2; border: 1px solid %1; border-radius: 10px; }")
                                 .arg(Theme::Accent.name(), Theme::mix(Theme::Surface, Theme::Accent, 0.15).name()));
    auto *bulk = new QHBoxLayout(m_bulkBar);
    bulk->setContentsMargins(8, 4, 8, 4);
    bulk->setSpacing(4);
    m_bulkLabel = new QLabel(m_bulkBar);
    m_bulkLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
    bulk->addWidget(m_bulkLabel, 1);
    auto bulkButton = [&](Icons::Name icon, const QString &tip, auto slot) {
        auto *button = new FancyButton(icon, tip, FancyButton::Kind::Secondary, m_bulkBar);
        button->setCompact(true);
        button->setToolTip(tip);
        button->setFixedSize(34, 30);
        connect(button, &QAbstractButton::clicked, this, slot);
        bulk->addWidget(button);
        return button;
    };
    m_bulkMergeButton = bulkButton(Icons::Name::Merge, QStringLiteral("دمج المحدد كشخص واحد"), [this] { mergeFaces(selectedFaceIds()); });
    bulkButton(Icons::Name::UserPlus, QStringLiteral("إضافة المحدد إلى شخص في القاعدة…"), [this] { addFacesToPerson(selectedFaceIds()); });
    bulkButton(Icons::Name::Export, QStringLiteral("تصدير صور المحدد مع تقرير…"), [this] { exportFaces(selectedFaceIds()); });
    bulkButton(Icons::Name::Trash, QStringLiteral("إزالة المحدد من القائمة"), [this] { removeFaces(selectedFaceIds()); });
    bulkButton(Icons::Name::Reject, QStringLiteral("إلغاء التحديد"), [this] {
        const int row = m_faceList->currentRow();
        m_faceList->clearSelection();
        if (row >= 0)
            m_faceList->item(row)->setSelected(true);
    });
    m_bulkBar->setVisible(false);
    sideLayout->addWidget(m_bulkBar);

    // التفاصيل تُمرَّر إن طالت حتى تبقى الأزرار ظاهرة على الشاشات القصيرة
    m_faceDetails = new FaceDetailsPanel(side);
    connect(m_faceDetails, &FaceDetailsPanel::assignRequested, this, [this](int faceId, const QString &personId) {
        if (m_ready)
            assignFace(faceId, personId);
    });
    connect(m_faceDetails, &FaceDetailsPanel::otherPersonRequested, this, [this](int faceId) {
        const int row = m_faceRows.value(faceId, -1);
        if (m_ready && row >= 0 && row < m_faces.size())
            addFaceToDatabase(m_faces.at(row));
    });
    connect(m_faceDetails, &FaceDetailsPanel::seekRequested, this, &MainWindow::loadVideoFrame);
    connect(m_faceDetails, &FaceDetailsPanel::shotRequested, this, [this](int faceId) {
        const int row = m_faceRows.value(faceId, -1);
        if (row < 0 || row >= m_faces.size())
            return;
        if (!m_videoMode)
            m_view->zoomToFace(faceId);
        else if (m_faces.at(row).bestMs >= 0)
            loadVideoFrame(m_faces.at(row).bestMs);
    });
    auto *detailsScroll = new QScrollArea(side);
    detailsScroll->setWidget(m_faceDetails);
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setFrameShape(QFrame::NoFrame);
    detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailsScroll->setMinimumHeight(80);
    detailsScroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    // القائمة والتفاصيل في مقسم حتى يكبّر المستخدم أيهما شاء
    auto *sideSplitter = new QSplitter(Qt::Vertical, side);
    sideSplitter->setHandleWidth(8);
    sideSplitter->setChildrenCollapsible(false);
    // التفاصيل في قسم يُطوى، فتأخذ القائمة المساحة كلها حين لا يحتاجها المستخدم
    m_detailsSection = new CollapsibleSection(QStringLiteral("تفاصيل الوجه"), side, QStringLiteral("faces/detailsExpanded"));
    m_detailsSection->contentLayout()->setContentsMargins(8, 0, 8, 8);
    m_detailsSection->contentLayout()->addWidget(detailsScroll);
    sideSplitter->addWidget(m_faceList);
    sideSplitter->addWidget(m_detailsSection);
    sideSplitter->setStretchFactor(0, 3);
    sideSplitter->setStretchFactor(1, 2);
    sideSplitter->setSizes({300, 200});
    sideLayout->addWidget(sideSplitter, 1);

    m_viewShotButton = new FancyButton(Icons::Name::Film, QStringLiteral("اللقطات"), FancyButton::Kind::Secondary, side);
    m_viewShotButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_viewShotButton->setToolTip(QStringLiteral("الرجوع إلى إطارات ظهور الوجه، وأوضح لقطة له، وتحسينها وحفظها"));
    m_viewShotButton->setVisible(false);
    connect(m_viewShotButton, &QAbstractButton::clicked, this, [this] {
        const int row = m_faceList->currentRow();
        if (row >= 0 && row < m_faces.size())
            openShotViewer(m_faces.at(row).id);
    });

    m_enrollFaceButton = new FancyButton(Icons::Name::UserPlus, QStringLiteral("إضافة للقاعدة"), FancyButton::Kind::Primary, side);
    m_enrollFaceButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_enrollFaceButton->setToolTip(QStringLiteral("إضافة الوجه المحدد إلى شخص في قاعدة البيانات، أو إلى شخص جديد"));
    connect(m_enrollFaceButton, &QAbstractButton::clicked, this, &MainWindow::addSelectedFaceToDatabase);
    auto *faceActions = new QHBoxLayout;
    faceActions->setSpacing(8);
    faceActions->addWidget(m_enrollFaceButton, 1);
    faceActions->addWidget(m_viewShotButton, 1);
    sideLayout->addLayout(faceActions);

    m_faceTabsTimer = new QTimer(this);
    m_faceTabsTimer->setSingleShot(true);
    m_faceTabsTimer->setInterval(250);
    connect(m_faceTabsTimer, &QTimer::timeout, this, &MainWindow::updateFaceTabs);
    updateFaceTabs();
}

void MainWindow::setFaceView(bool grid, int scalePercent)
{
    m_faceDelegate->setGrid(grid);
    m_faceDelegate->setScale(scalePercent / 100.0);
    m_faceList->setViewMode(grid ? QListView::IconMode : QListView::ListMode);
    m_faceList->setFlow(grid ? QListView::LeftToRight : QListView::TopToBottom);
    m_faceList->setWrapping(grid);
    m_faceList->setResizeMode(QListView::Adjust);
    m_faceList->setMovement(QListView::Static);
    m_faceList->setDragEnabled(false);
    m_faceList->setSpacing(0);
    // الحجم الموحد محفوظ في القائمة، فيُعاد حسابه بعد تغيّر حجم البطاقة
    m_faceList->setUniformItemSizes(false);
    m_faceList->setUniformItemSizes(true);
    m_faceList->doItemsLayout();
    m_faceList->viewport()->update();
    // بطاقة كاملة واحدة على الأقل، فلا تُقص النسبة حين يضيق المكان (كظهور شريط التنبيه)
    const int cardHeight = m_faceDelegate->sizeHint(QStyleOptionViewItem(), QModelIndex()).height();
    m_faceList->setMinimumHeight(std::max(90, cardHeight + 2 * m_faceList->frameWidth() + 4));
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("faces/grid"), grid);
    settings.setValue(QStringLiteral("faces/size"), scalePercent);
}

int MainWindow::faceTab(const FaceItem &face) const
{
    if (face.match.status == MatchStatus::Confirmed)
        return m_watched.contains(face.match.personId) ? 4 : 1;
    return face.match.status == MatchStatus::Possible ? 2 : 3;
}

bool MainWindow::faceMatchesFilter(const FaceItem &face) const
{
    const int tab = m_faceTabs ? m_faceTabs->currentIndex() : 0;
    if (tab > 0) {
        const int kind = faceTab(face);
        // المراقَبون معروفون أيضاً، فيظهرون في التبويبين
        if (kind != tab && !(tab == 1 && kind == 4))
            return false;
    }
    const QString text = m_faceFilter->text().trimmed();
    return text.isEmpty() || matchLabel(face.match).contains(text, Qt::CaseInsensitive);
}

void MainWindow::applyFaceFilter()
{
    int shown = 0;
    for (int i = 0; i < m_faces.size() && i < m_faceList->count(); ++i) {
        const bool visible = faceMatchesFilter(m_faces.at(i));
        m_faceList->setRowHidden(i, !visible);
        shown += visible ? 1 : 0;
    }
    // لا تبقى تفاصيل وجه أخفاه التبويب أو البحث
    if (m_faceList->currentRow() >= 0 && m_faceList->isRowHidden(m_faceList->currentRow()))
        m_faceList->setCurrentRow(-1);
    m_laneTimer->start();
    const bool filtered = !m_faceFilter->text().trimmed().isEmpty() || m_faceTabs->currentIndex() > 0;
    m_faceCount->setText(filtered ? QStringLiteral("%1 / %2").arg(shown).arg(m_faces.size()) : QString::number(m_faces.size()));
    updateFaceTabs();
}

void MainWindow::updateFaceCount()
{
    if (m_faceFilter->text().trimmed().isEmpty() && m_faceTabs->currentIndex() <= 0)
        m_faceCount->setText(QString::number(m_faces.size()));
    else
        applyFaceFilter();
    if (!m_faceTabsTimer->isActive())
        m_faceTabsTimer->start();
}

void MainWindow::updateFaceTabs()
{
    int counts[5] = {int(m_faces.size()), 0, 0, 0, 0};
    for (const FaceItem &face : std::as_const(m_faces)) {
        const int kind = faceTab(face);
        ++counts[kind];
        if (kind == 4)
            ++counts[1];
    }
    static const QStringList names{QStringLiteral("الكل"), QStringLiteral("معروف"), QStringLiteral("محتمل"),
                                   QStringLiteral("مجهول"), QStringLiteral("مراقَب")};
    for (int i = 0; i < m_faceTabs->count(); ++i)
        m_faceTabs->setItemText(i, counts[i] > 0 ? QStringLiteral("%1 %2").arg(names.at(i), QLocale().toString(counts[i])) : names.at(i));
}

void MainWindow::updateBulkBar()
{
    const int selected = int(m_faceList->selectedItems().size());
    m_bulkBar->setVisible(selected > 1);
    if (selected > 1) {
        m_bulkLabel->setText(QStringLiteral("محدد: %1").arg(selected));
        m_bulkMergeButton->setEnabled(m_videoMode);
    }
}

int MainWindow::faceActions(const FaceItem &face) const
{
    const bool fileVideo = m_videoMode && !m_videoSource.startsWith(QStringLiteral("camera:"));
    int actions = 0;
    if (m_ready && face.match.status == MatchStatus::Possible && !face.match.personId.isEmpty())
        actions |= FaceAction::Confirm;
    if (!face.match.personId.isEmpty() && face.match.status != MatchStatus::Unknown)
        actions |= FaceAction::Reject;
    if (m_ready)
        actions |= FaceAction::Add;
    if (fileVideo)
        actions |= FaceAction::Shots;
    if (fileVideo && face.firstSeenMs >= 0)
        actions |= FaceAction::First;
    if (face.match.status == MatchStatus::Confirmed)
        actions |= FaceAction::Watch;
    return actions;
}

void MainWindow::updateFaceListItem(QListWidgetItem *listItem, const FaceItem &face)
{
    if (!listItem)
        return;
    QString subtitle;
    if (m_videoMode) {
        const int times = int(std::max<qsizetype>(1, face.appearances.size()));
        subtitle = QStringLiteral("ظهر ") + ImageIO::timesText(times);
        const qint64 screen = Report::screenTime(face.appearances);
        if (screen > 0)
            subtitle += QStringLiteral(" · ") + ImageIO::formatTime(screen);
        if (face.firstSeenMs >= 0)
            subtitle += QStringLiteral(" · من ") + ImageIO::formatTime(face.firstSeenMs);
    } else {
        subtitle = QStringLiteral("جودة الصورة %1%").arg(qRound(face.quality * 100));
    }
    listItem->setData(CardRole::Id, face.id);
    listItem->setData(CardRole::Title, matchLabel(face.match));
    listItem->setData(CardRole::Subtitle, subtitle);
    listItem->setData(CardRole::Status, int(face.match.status));
    listItem->setData(CardRole::Score, double(face.match.score));
    listItem->setData(CardRole::Watched, face.match.status == MatchStatus::Confirmed && m_watched.contains(face.match.personId));
    listItem->setData(CardRole::Actions, faceActions(face));
    // الصورة بحجم أكبر البطاقات حتى تبقى واضحة عند التكبير
    if (!listItem->data(CardRole::Avatar).isValid() || m_videoMode)
        listItem->setData(CardRole::Avatar, circularPixmap(ImageIO::faceCrop(face), 116));
}

void MainWindow::onFaceAction(int id, int action)
{
    const int row = m_faceRows.value(id, -1);
    if (row < 0 || row >= m_faces.size())
        return;
    switch (action) {
    case FaceAction::Confirm:
        confirmFaceMatch(id);
        break;
    case FaceAction::Reject:
        rejectFaceMatch(id);
        break;
    case FaceAction::Add:
        addFaceToDatabase(m_faces.at(row));
        break;
    case FaceAction::Shots:
        openShotViewer(id);
        break;
    case FaceAction::First:
        m_faceList->setCurrentRow(row);
        loadVideoFrame(m_faces.at(row).firstSeenMs);
        break;
    case FaceAction::Watch:
        toggleFaceWatch(id);
        break;
    default:
        break;
    }
}

// تأكيد التطابق المحتمل: الوجه يُضاف إلى صور الشخص فيصير مؤكداً الآن وتتحسن مطابقته لاحقاً
void MainWindow::confirmFaceMatch(int id)
{
    const int row = m_faceRows.value(id, -1);
    if (!m_ready || row < 0 || row >= m_faces.size() || m_faces.at(row).match.personId.isEmpty())
        return;
    const FaceItem face = m_faces.at(row);
    if (!enrollFace(face.match.personId, face)) {
        Toast::show(this, QStringLiteral("تعذر إضافة الوجه"), Toast::Kind::Warning);
        return;
    }
    const int current = m_faceRows.value(id, -1); // الإضافة قد تعيد بناء القائمة
    if (current >= 0 && current < m_faces.size()) {
        m_faces[current].match.status = MatchStatus::Confirmed;
        m_rejectedMatches.remove(id);
        updateFaceListItem(m_faceList->item(current), m_faces.at(current));
        m_faceList->setRowHidden(current, !faceMatchesFilter(m_faces.at(current)));
        checkWatchAlert(m_faces.at(current));
    }
    updateFaceTabs();
    m_laneTimer->start();
    showFaceDetails();
    Toast::show(this, QStringLiteral("تأكد أنه %1، وأُضيف الوجه إلى صوره").arg(face.match.name));
}

// رفض التطابق: يبقى الوجه غير معروف حتى نهاية هذا الملف، ولا تُغيَّر القاعدة
void MainWindow::rejectFaceMatch(int id)
{
    const int row = m_faceRows.value(id, -1);
    if (row < 0 || row >= m_faces.size() || m_faces.at(row).match.personId.isEmpty())
        return;
    const QString name = m_faces.at(row).match.name;
    m_rejectedMatches.insert(id, m_faces.at(row).match.personId);
    m_faces[row].match = MatchResult();
    updateFaceListItem(m_faceList->item(row), m_faces.at(row));
    m_faceList->setRowHidden(row, !faceMatchesFilter(m_faces.at(row)));
    updateFaceTabs();
    m_laneTimer->start();
    showFaceDetails();
    Toast::show(this, QStringLiteral("رُفض التطابق مع %1: الوجه غير معروف الآن").arg(name));
}

void MainWindow::toggleFaceWatch(int id)
{
    const int row = m_faceRows.value(id, -1);
    if (row < 0 || row >= m_faces.size() || m_faces.at(row).match.status != MatchStatus::Confirmed)
        return;
    Person person = m_database.person(m_faces.at(row).match.personId);
    if (person.id.isEmpty())
        return;
    const bool watch = !FaceDatabase::isWatched(person.tags);
    person.tags = FaceDatabase::withWatchTag(person.tags, watch);
    if (!m_database.updatePerson(person)) {
        Toast::show(this, QStringLiteral("تعذر حفظ التغيير"), Toast::Kind::Warning);
        return;
    }
    // تحديث القاعدة يعيد قائمة المراقبة عبر refreshWatched، والبطاقات تتبعها هنا
    for (int i = 0; i < m_faces.size() && i < m_faceList->count(); ++i) {
        if (m_faces.at(i).match.personId == person.id) {
            updateFaceListItem(m_faceList->item(i), m_faces.at(i));
            m_faceList->setRowHidden(i, !faceMatchesFilter(m_faces.at(i)));
        }
    }
    updateFaceTabs();
    m_laneTimer->start();
    Toast::show(this, watch ? QStringLiteral("أُضيف %1 إلى قائمة المراقبة: ستُنبَّه فور ظهوره").arg(person.name)
                            : QStringLiteral("أُزيل %1 من قائمة المراقبة").arg(person.name));
}

void MainWindow::addFacesToPerson(const QList<int> &ids)
{
    if (!m_ready || ids.isEmpty())
        return;
    QList<FaceItem> faces;
    QString suggested;
    for (int id : ids) {
        const int row = m_faceRows.value(id, -1);
        if (row < 0 || row >= m_faces.size())
            continue;
        faces << m_faces.at(row);
        if (suggested.isEmpty() && m_faces.at(row).match.status == MatchStatus::Confirmed)
            suggested = m_faces.at(row).match.personId;
    }
    if (faces.isEmpty())
        return;
    const QString personId = choosePerson(suggested);
    if (personId.isEmpty())
        return;
    const int added = enrollFaces(personId, faces);
    Toast::show(this, QStringLiteral("أُضيف %1 من %2 وجه إلى %3").arg(added).arg(faces.size()).arg(m_database.person(personId).name),
                added == faces.size() ? Toast::Kind::Success : Toast::Kind::Warning);
}

void MainWindow::exportFaces(const QList<int> &ids)
{
    QList<FaceItem> faces;
    for (int id : ids) {
        const int row = m_faceRows.value(id, -1);
        if (row >= 0 && row < m_faces.size())
            faces << m_faces.at(row);
    }
    if (faces.isEmpty())
        return;
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("اختر مجلد تصدير الوجوه المحددة"), m_lastDirectory);
    if (!folder.isEmpty())
        writeFaces(faces, folder);
}

QPixmap MainWindow::personAvatar(const QString &personId)
{
    auto cached = m_personAvatars.constFind(personId);
    if (cached != m_personAvatars.constEnd())
        return *cached;
    const Person person = m_database.person(personId);
    qint64 photoId = person.coverPhoto;
    if (photoId <= 0 && !person.photos.isEmpty())
        photoId = person.photos.first().id;
    const QPixmap avatar = photoId > 0 ? circularPixmap(m_database.photo(photoId, 96), 30) : QPixmap();
    // صور القاعدة قد تتغير؛ الذاكرة المؤقتة صغيرة وتُفرَّغ كلما كبرت
    if (m_personAvatars.size() > 200)
        m_personAvatars.clear();
    m_personAvatars.insert(personId, avatar);
    return avatar;
}

void MainWindow::showFaceDetails()
{
    const int row = m_faceList->currentRow();
    const bool fileVideo = m_videoMode && !m_videoSource.startsWith(QStringLiteral("camera:"));
    m_enrollFaceButton->setEnabled(m_ready && row >= 0);
    m_viewShotButton->setVisible(fileVideo);
    m_viewShotButton->setEnabled(row >= 0 && row < m_faces.size());
    if (row < 0 || row >= m_faces.size()) {
        m_enrollFaceButton->setText(QStringLiteral("إضافة للقاعدة"));
        m_faceDetails->showMessage(QStringLiteral("اختر وجهاً من القائمة أو اضغط عليه في الصورة لعرض تفاصيله."));
        m_view->setSelectedFace(-1);
        return;
    }

    const FaceItem &face = m_faces.at(row);
    // للوجه المعروف يضيف الزر صورته إلى ملفه مباشرة؛ الاسم يُختصر حتى لا يعرّض اللوحة
    const bool known = face.match.status == MatchStatus::Confirmed;
    const QString shortName = face.match.name.size() > 16 ? face.match.name.left(15) + QStringLiteral("…") : face.match.name;
    m_enrollFaceButton->setText(known ? QStringLiteral("إضافة لملف %1").arg(shortName) : QStringLiteral("إضافة للقاعدة"));
    m_enrollFaceButton->setToolTip(known ? QStringLiteral("إضافة صورة هذا الوجه إلى ملف %1 في القاعدة\n"
                                                          "لإضافته إلى شخص آخر: زر الفأرة الأيمن على الوجه في الصورة")
                                                .arg(face.match.name)
                                         : QStringLiteral("إضافة الوجه المحدد إلى شخص في قاعدة البيانات، أو إلى شخص جديد"));
    m_view->setSelectedFace(face.id);
    m_timeline->setSelectedLane(face.id);

    FaceDetailsPanel::Details d;
    d.faceId = face.id;
    d.shot = ImageIO::faceCrop(face);
    d.title = matchLabel(face.match);
    d.status = statusDescription(face.match.status);
    d.color = matchColor(face.match.status);
    d.watched = face.match.status == MatchStatus::Confirmed && m_watched.contains(face.match.personId);
    d.score = qRound(face.match.score * 100);
    d.quality = qRound(face.quality * 100);
    d.video = m_videoMode;
    d.canSeek = fileVideo;
    d.bestMs = face.bestMs;
    d.appearances = face.appearances;
    d.canAssign = m_ready;

    // أقرب المرشحين في القاعدة بصورهم، حتى يرى المستخدم البدائل ويصحح بنقرة
    if (!face.embedding.empty() && m_database.personCount() > 0) {
        const GalleryPtr gallery = m_database.gallery();
        const QList<MatchResult> candidates = FaceDatabase::candidates(*gallery, face.embedding, 3, threshold());
        for (const MatchResult &match : candidates) {
            FaceDetailsPanel::Candidate candidate;
            candidate.personId = match.personId;
            candidate.name = match.name;
            candidate.percent = std::clamp(qRound(match.score * 100), 0, 100);
            candidate.status = match.status;
            candidate.avatar = personAvatar(match.personId);
            candidate.current = face.match.status == MatchStatus::Confirmed && match.personId == face.match.personId;
            d.candidates << candidate;
        }
    }

    if (m_videoMode && face.firstSeenMs >= 0) {
        d.rows << qMakePair(QStringLiteral("أول ظهور"), ImageIO::formatTime(face.firstSeenMs));
        d.rows << qMakePair(QStringLiteral("آخر ظهور"), ImageIO::formatTime(face.lastSeenMs));
        d.rows << qMakePair(QStringLiteral("عدد الإطارات"), QString::number(face.frames));
        if (face.bestMs >= 0)
            d.rows << qMakePair(QStringLiteral("أوضح لقطة"), ImageIO::formatTime(face.bestMs));
    }
    if (!face.match.personId.isEmpty() && face.match.status != MatchStatus::Unknown) {
        const Person person = m_database.person(face.match.personId);
        if (!person.nationalId.isEmpty())
            d.rows << qMakePair(QStringLiteral("رقم الهوية"), person.nationalId);
        if (!person.phone.isEmpty())
            d.rows << qMakePair(QStringLiteral("الهاتف"), person.phone);
        if (!person.tags.isEmpty())
            d.rows << qMakePair(QStringLiteral("التصنيفات"), person.tags);
        if (!person.notes.isEmpty())
            d.rows << qMakePair(QStringLiteral("ملاحظات"), person.notes);
    }
    m_faceDetails->showDetails(d);
}

// اقتراحات الدمج: وجهان بصمتاهما متقاربتان ولم يظهرا في وقت واحد، أو طابقا الشخص نفسه في القاعدة
void MainWindow::updateMergeSuggestions()
{
    m_mergeSuggestions.clear();
    if (m_videoMode && !m_videoRunning && m_faces.size() > 1) {
        QList<int> rows;
        rows.reserve(m_faces.size());
        for (int i = 0; i < m_faces.size(); ++i)
            if (!m_faces.at(i).embedding.empty())
                rows << i;
        if (rows.size() > MaxSuggestionFaces) {
            std::partial_sort(rows.begin(), rows.begin() + MaxSuggestionFaces, rows.end(),
                              [this](int a, int b) { return m_faces.at(a).frames > m_faces.at(b).frames; });
            rows.resize(MaxSuggestionFaces);
        }
        for (int i = 0; i < rows.size(); ++i) {
            const FaceItem &a = m_faces.at(rows.at(i));
            for (int j = i + 1; j < rows.size(); ++j) {
                const FaceItem &b = m_faces.at(rows.at(j));
                if (a.embedding.size() != b.embedding.size() || m_dismissedMerges.contains(pairKey(a.id, b.id)))
                    continue;
                const bool bothKnown = a.match.status == MatchStatus::Confirmed && b.match.status == MatchStatus::Confirmed;
                if (bothKnown && a.match.personId != b.match.personId)
                    continue;
                const float s = FaceEngine::cosine(a.embedding, b.embedding);
                const bool samePerson = bothKnown && a.match.personId == b.match.personId;
                if ((s < SuggestSimilarity && !samePerson) || overlaps(a.appearances, b.appearances))
                    continue;
                m_mergeSuggestions.append({a.id, b.id, samePerson ? std::max(s, 0.99f) : s});
            }
        }
        std::sort(m_mergeSuggestions.begin(), m_mergeSuggestions.end(),
                  [](const MergeSuggestion &x, const MergeSuggestion &y) { return x.score > y.score; });
        if (m_mergeSuggestions.size() > MaxSuggestions)
            m_mergeSuggestions.resize(MaxSuggestions);
    }
    m_mergeSuggestButton->setText(QStringLiteral("غالباً الشخص نفسه: %1 اقتراح").arg(m_mergeSuggestions.size()));
    m_mergeSuggestButton->setVisible(!m_mergeSuggestions.isEmpty());
}

void MainWindow::showMergeSuggestions()
{
    updateMergeSuggestions();
    if (m_mergeSuggestions.isEmpty()) {
        Toast::show(this, QStringLiteral("لا توجد اقتراحات دمج الآن"));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("غالباً الشخص نفسه"));
    dialog.setMinimumWidth(460);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setSpacing(8);
    auto *hint = new QLabel(QStringLiteral("وجوه متشابهة لم تظهر في وقت واحد. ادمج ما هو للشخص نفسه، أو اختر «مختلفان» فلا يُقترح ثانية."),
                            &dialog);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("muted"));
    layout->addWidget(hint);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *host = new QWidget(scroll);
    auto *rows = new QVBoxLayout(host);
    rows->setSpacing(6);
    scroll->setWidget(host);
    layout->addWidget(scroll, 1);

    auto faceWidget = [this, host](int id) {
        auto *widget = new QWidget(host);
        auto *column = new QVBoxLayout(widget);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);
        const int row = m_faceRows.value(id, -1);
        auto *avatar = new QLabel(widget);
        avatar->setAlignment(Qt::AlignCenter);
        auto *name = new QLabel(widget);
        name->setAlignment(Qt::AlignCenter);
        name->setMinimumWidth(10);
        if (row >= 0 && row < m_faces.size()) {
            const FaceItem &face = m_faces.at(row);
            avatar->setPixmap(circularPixmap(ImageIO::faceCrop(face), 56));
            name->setText(QStringLiteral("%1\n%2").arg(matchLabel(face.match),
                                                       face.firstSeenMs >= 0 ? ImageIO::formatTime(face.firstSeenMs) : QString()));
        }
        column->addWidget(avatar);
        column->addWidget(name);
        return widget;
    };
    int merged = 0;
    const QList<MergeSuggestion> suggestions = m_mergeSuggestions;
    for (const MergeSuggestion &suggestion : suggestions) {
        auto *frame = new QFrame(host);
        frame->setObjectName(QStringLiteral("suggestion"));
        frame->setStyleSheet(QStringLiteral("QFrame#suggestion { border: 1px solid %1; border-radius: 12px; }").arg(Theme::Border.name()));
        auto *row = new QHBoxLayout(frame);
        row->setContentsMargins(10, 6, 10, 6);
        row->addWidget(faceWidget(suggestion.a), 1);
        auto *score = new QLabel(QStringLiteral("%1%").arg(qRound(std::min(suggestion.score, 1.0f) * 100)), frame);
        score->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(Theme::Accent2.name()));
        score->setToolTip(QStringLiteral("تشابه البصمتين"));
        row->addWidget(score);
        row->addWidget(faceWidget(suggestion.b), 1);
        auto *buttons = new QVBoxLayout;
        auto *merge = new QPushButton(QStringLiteral("دمج"), frame);
        auto *different = new QPushButton(QStringLiteral("مختلفان"), frame);
        buttons->addWidget(merge);
        buttons->addWidget(different);
        row->addLayout(buttons);
        connect(merge, &QPushButton::clicked, frame, [this, frame, suggestion, &merged] {
            if (m_faceRows.contains(suggestion.a) && m_faceRows.contains(suggestion.b)) {
                mergeFaces({suggestion.a, suggestion.b});
                ++merged;
            }
            frame->setEnabled(false);
        });
        connect(different, &QPushButton::clicked, frame, [this, frame, suggestion] {
            m_dismissedMerges.insert(pairKey(suggestion.a, suggestion.b));
            frame->setEnabled(false);
        });
        rows->addWidget(frame);
    }
    rows->addStretch();
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("إغلاق"));
    auto *mergeAll = box->addButton(QStringLiteral("دمج الكل"), QDialogButtonBox::ActionRole);
    connect(mergeAll, &QPushButton::clicked, &dialog, [this, &dialog, suggestions, &merged] {
        // الوجه المدموج يختفي، فيُتبع إلى الوجه الذي دُمج فيه قبل دمج الزوج التالي
        auto resolve = [this](int id) {
            while (m_mergedInto.contains(id) && m_mergedInto.value(id) != id)
                id = m_mergedInto.value(id);
            return id;
        };
        for (const MergeSuggestion &suggestion : suggestions) {
            if (m_dismissedMerges.contains(pairKey(suggestion.a, suggestion.b)))
                continue;
            const int a = resolve(suggestion.a);
            const int b = resolve(suggestion.b);
            if (a != b && m_faceRows.contains(a) && m_faceRows.contains(b)) {
                mergeFaces({a, b});
                ++merged;
            }
        }
        dialog.accept();
    });
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(box);
    dialog.resize(520, 560);
    dialog.exec();
    updateMergeSuggestions();
    if (merged > 0)
        statusBar()->showMessage(QStringLiteral("دُمج %1 زوج من الوجوه").arg(merged));
}

#include "cameradialog.h"

#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFuture>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

struct TestResult
{
    QImage image;
    QString error;
    QString info;
};

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QWidget *formRow(const QString &caption, QWidget *field, QWidget *parent, QLabel **captionLabel = nullptr)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *label = new QLabel(caption, row);
    label->setFixedWidth(110);
    layout->addWidget(label);
    layout->addWidget(field, 1);
    if (captionLabel)
        *captionLabel = label;
    return row;
}

const QList<QPair<int, int>> &resolutions()
{
    static const QList<QPair<int, int>> list = {{0, 0}, {640, 480}, {1280, 720}, {1920, 1080}};
    return list;
}

} // namespace

CameraDialog::CameraDialog(QWidget *parent, const QString &selectId)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("الكاميرات"));
    resize(1120, 700);
    m_cameras = Cameras::load();
    m_devices = Cameras::localDevices();

    auto *layout = new QHBoxLayout(this);
    layout->setSpacing(18);

    // قائمة الكاميرات
    auto *listColumn = new QVBoxLayout;
    listColumn->addWidget(sectionTitle(QStringLiteral("الكاميرات المحفوظة"), this));
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("plainList"));
    m_list->setMinimumWidth(240);
    m_list->setIconSize(QSize(22, 22));
    listColumn->addWidget(m_list, 1);
    auto *addButton = new FancyButton(Icons::Name::UserPlus, QStringLiteral("إضافة كاميرا"), FancyButton::Kind::Primary, this);
    m_removeButton = new FancyButton(Icons::Name::Trash, QStringLiteral("حذف"), FancyButton::Kind::Danger, this);
    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(addButton, 1);
    listButtons->addWidget(m_removeButton);
    listColumn->addLayout(listButtons);
    layout->addLayout(listColumn, 0);

    connect(addButton, &QAbstractButton::clicked, this, [this] {
        const QString via = chooseConnection();
        if (!via.isEmpty())
            addCamera(via);
    });
    connect(m_removeButton, &QAbstractButton::clicked, this, &CameraDialog::removeCamera);
    connect(m_list, &QListWidget::currentRowChanged, this, &CameraDialog::select);

    // الإعدادات
    m_form = new QWidget(this);
    auto *form = new QVBoxLayout(m_form);
    form->setContentsMargins(0, 0, 0, 0);
    form->addWidget(sectionTitle(QStringLiteral("الإعدادات"), m_form));
    m_name = new QLineEdit(m_form);
    m_name->setPlaceholderText(QStringLiteral("مثل: البوابة الرئيسية"));
    form->addWidget(formRow(QStringLiteral("الاسم"), m_name, m_form));

    m_localBox = new QWidget(m_form);
    auto *local = new QVBoxLayout(m_localBox);
    local->setContentsMargins(0, 0, 0, 0);
    m_device = new QComboBox(m_localBox);
    auto *refresh = new FancyButton(Icons::Name::Scan, QStringLiteral("تحديث"), FancyButton::Kind::Secondary, m_localBox);
    refresh->setToolTip(QStringLiteral("البحث من جديد بعد توصيل كاميرا أو محوّل"));
    auto *deviceField = new QWidget(m_localBox);
    auto *deviceLayout = new QHBoxLayout(deviceField);
    deviceLayout->setContentsMargins(0, 0, 0, 0);
    deviceLayout->addWidget(m_device, 1);
    deviceLayout->addWidget(refresh);
    auto *probe = new FancyButton(Icons::Name::Check, QStringLiteral("تشخيص"), FancyButton::Kind::Secondary, m_localBox);
    probe->setToolTip(QStringLiteral("يجرّب كل كاميرا موصولة بكل طريقة التقاط ويعرض النتيجة، لمعرفة سبب كاميرا لا تفتح"));
    deviceLayout->addWidget(probe);
    connect(probe, &QAbstractButton::clicked, this, &CameraDialog::probeCameras);
#ifdef Q_OS_WIN
    auto *privacy = new FancyButton(Icons::Name::Lock, QStringLiteral("أذونات الكاميرا"), FancyButton::Kind::Secondary, m_localBox);
    privacy->setToolTip(QStringLiteral("صفحة ويندوز التي تسمح للبرامج باستخدام الكاميرا"));
    deviceLayout->addWidget(privacy);
    connect(privacy, &QAbstractButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:privacy-webcam"))); });
#endif
    local->addWidget(formRow(QStringLiteral("الجهاز"), deviceField, m_localBox));
    m_resolution = new QComboBox(m_localBox);
    for (const auto &[w, h] : resolutions())
        m_resolution->addItem(w == 0 ? QStringLiteral("تلقائية (دقة الكاميرا)") : QStringLiteral("%1×%2").arg(w).arg(h));
    m_resolution->setToolTip(QStringLiteral("الدقة الأعلى أوضح للوجوه البعيدة وأبطأ في التحليل"));
    local->addWidget(formRow(QStringLiteral("الدقة"), m_resolution, m_localBox));
    m_backend = new QComboBox(m_localBox);
    m_backend->addItems({QStringLiteral("تلقائية"), QStringLiteral("DirectShow"), QStringLiteral("Media Foundation")});
    m_backend->setToolTip(QStringLiteral("بعض محوّلات الفيديو وبطاقات الالتقاط لا تعمل إلا بإحدى الطريقتين"));
    QWidget *backendRow = formRow(QStringLiteral("طريقة الالتقاط"), m_backend, m_localBox);
#ifndef Q_OS_WIN
    backendRow->setVisible(false);
#endif
    local->addWidget(backendRow);
    form->addWidget(m_localBox);
    connect(refresh, &QAbstractButton::clicked, this, &CameraDialog::refreshDevices);

    m_networkBox = new QWidget(m_form);
    auto *network = new QVBoxLayout(m_networkBox);
    network->setContentsMargins(0, 0, 0, 0);
    m_brand = new QComboBox(m_networkBox);
    for (const Cameras::Brand &b : Cameras::brands())
        m_brand->addItem(b.name, b.key);
    network->addWidget(formRow(QStringLiteral("نوع الكاميرا"), m_brand, m_networkBox));
    m_brandHint = new QLabel(m_networkBox);
    m_brandHint->setObjectName(QStringLiteral("muted"));
    m_brandHint->setWordWrap(true);
    network->addWidget(m_brandHint);
    m_host = new QLineEdit(m_networkBox);
    m_host->setPlaceholderText(QStringLiteral("192.168.1.64"));
    m_host->setLayoutDirection(Qt::LeftToRight);
    m_port = new QSpinBox(m_networkBox);
    m_port->setRange(1, 65535);
    m_port->setLayoutDirection(Qt::LeftToRight);
    auto *hostField = new QWidget(m_networkBox);
    auto *hostLayout = new QHBoxLayout(hostField);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->addWidget(m_host, 1);
    hostLayout->addWidget(new QLabel(QStringLiteral("المنفذ"), hostField));
    hostLayout->addWidget(m_port);
    network->addWidget(formRow(QStringLiteral("العنوان (IP)"), hostField, m_networkBox));
    m_hostCheck = new QLabel(m_networkBox);
    m_hostCheck->setWordWrap(true);
    m_hostCheck->setTextFormat(Qt::RichText);
    m_hostCheck->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Warning.name()));
    m_hostCheck->setVisible(false);
    network->addWidget(m_hostCheck);
    m_findButton = new FancyButton(Icons::Name::Scan, QStringLiteral("البحث في الشبكة"), FancyButton::Kind::Secondary, m_networkBox);
    m_findButton->setToolTip(QStringLiteral("يجد الكاميرات وأجهزة التسجيل الموصولة بشبكة اللابتوب أو بسلكه مباشرة"));
    m_detectButton = new FancyButton(Icons::Name::Check, QStringLiteral("اكتشاف تلقائي"), FancyButton::Kind::Secondary, m_networkBox);
    m_detectButton->setToolTip(QStringLiteral("يجرّب أنواع الكاميرات المعروفة على العنوان والمستخدم حتى تظهر الصورة"));
    m_netSettingsButton = new FancyButton(Icons::Name::Video, QStringLiteral("إعدادات الشبكة"), FancyButton::Kind::Secondary, m_networkBox);
    m_netSettingsButton->setToolTip(QStringLiteral("فتح اتصالات الشبكة في ويندوز لضبط عنوان اللابتوب للسلك المباشر"));
#ifndef Q_OS_WIN
    m_netSettingsButton->setVisible(false);
#endif
    auto *helpers = new QHBoxLayout;
    helpers->setContentsMargins(110, 0, 0, 0);
    helpers->addWidget(m_findButton);
    helpers->addWidget(m_detectButton);
    helpers->addWidget(m_netSettingsButton);
    helpers->addStretch();
    network->addLayout(helpers);
    connect(m_findButton, &QAbstractButton::clicked, this, &CameraDialog::findCameras);
    connect(m_detectButton, &QAbstractButton::clicked, this, &CameraDialog::detectCamera);
    connect(m_netSettingsButton, &QAbstractButton::clicked, this, [] {
        QProcess::startDetached(QStringLiteral("control.exe"), {QStringLiteral("ncpa.cpl")});
    });
    m_channel = new QSpinBox(m_networkBox);
    m_channel->setRange(1, 128);
    m_channel->setToolTip(QStringLiteral("رقم الكاميرا في جهاز التسجيل؛ 1 للكاميرا المنفردة"));
    m_channelRow = formRow(QStringLiteral("القناة"), m_channel, m_networkBox);
    network->addWidget(m_channelRow);
    m_stream = new QComboBox(m_networkBox);
    m_stream->addItems({QStringLiteral("رئيسي (دقة كاملة)"), QStringLiteral("فرعي (أخف وأسرع)")});
    m_streamRow = formRow(QStringLiteral("البث"), m_stream, m_networkBox);
    network->addWidget(m_streamRow);
    m_path = new QLineEdit(m_networkBox);
    m_path->setLayoutDirection(Qt::LeftToRight);
    m_pathRow = formRow(QStringLiteral("المسار"), m_path, m_networkBox, &m_pathLabel);
    network->addWidget(m_pathRow);
    m_user = new QLineEdit(m_networkBox);
    m_user->setLayoutDirection(Qt::LeftToRight);
    m_user->setPlaceholderText(QStringLiteral("admin"));
    network->addWidget(formRow(QStringLiteral("المستخدم"), m_user, m_networkBox));
    m_password = new QLineEdit(m_networkBox);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setLayoutDirection(Qt::LeftToRight);
    network->addWidget(formRow(QStringLiteral("كلمة المرور"), m_password, m_networkBox));
    m_url = new QLabel(m_networkBox);
    m_url->setObjectName(QStringLiteral("muted"));
    m_url->setWordWrap(true);
    m_url->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_url->setLayoutDirection(Qt::LeftToRight);
    m_url->setAlignment(Qt::AlignLeft);
    network->addWidget(formRow(QStringLiteral("عنوان البث"), m_url, m_networkBox));
    form->addWidget(m_networkBox);

    m_help = new QLabel(m_form);
    m_help->setWordWrap(true);
    m_help->setTextFormat(Qt::RichText);
    m_help->setObjectName(QStringLiteral("muted"));
    m_help->setAlignment(Qt::AlignTop);
    auto *helpScroll = new QScrollArea(m_form);
    helpScroll->setWidget(m_help);
    helpScroll->setWidgetResizable(true);
    helpScroll->setFrameShape(QFrame::NoFrame);
    form->addWidget(helpScroll, 1);
    layout->addWidget(m_form, 1);

    // الاختبار والتشغيل
    auto *previewColumn = new QVBoxLayout;
    previewColumn->addWidget(sectionTitle(QStringLiteral("اختبار الاتصال"), this));
    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFixedSize(400, 225);
    m_preview->setStyleSheet(QStringLiteral("background: #0e0b09; border-radius: 10px; color: #a99a8a;"));
    m_preview->setText(QStringLiteral("اضغط «اختبار» لرؤية صورة من الكاميرا"));
    m_preview->setWordWrap(true);
    previewColumn->addWidget(m_preview);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setFixedWidth(400);
    previewColumn->addWidget(m_status);
    m_testButton = new FancyButton(Icons::Name::Scan, QStringLiteral("اختبار"), FancyButton::Kind::Secondary, this);
    m_startButton = new FancyButton(Icons::Name::Play, QStringLiteral("تشغيل الكاميرا"), FancyButton::Kind::Primary, this);
    auto *close = new FancyButton(Icons::Name::Check, QStringLiteral("حفظ وإغلاق"), FancyButton::Kind::Secondary, this);
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_startButton, 1);
    actions->addWidget(m_testButton);
    previewColumn->addLayout(actions);
    previewColumn->addStretch();
    previewColumn->addWidget(close);
    layout->addLayout(previewColumn, 0);

    connect(m_testButton, &QAbstractButton::clicked, this, &CameraDialog::testConnection);
    connect(m_startButton, &QAbstractButton::clicked, this, [this] {
        if (Cameras::Camera *camera = current()) {
            storeForm();
            m_startSource = camera->source();
            accept();
        }
    });
    connect(close, &QAbstractButton::clicked, this, &QDialog::accept);

    auto edited = [this] {
        if (m_loading)
            return;
        storeForm();
        updateUrl();
        if (m_current >= 0 && m_list->item(m_current))
            m_list->item(m_current)->setText(m_cameras.at(m_current).name);
    };
    connect(m_name, &QLineEdit::textEdited, this, edited);
    for (QLineEdit *edit : {m_host, m_path, m_user, m_password})
        connect(edit, &QLineEdit::textEdited, this, edited);
    for (QSpinBox *spin : {m_port, m_channel})
        connect(spin, &QSpinBox::valueChanged, this, edited);
    for (QComboBox *combo : {m_device, m_resolution, m_backend, m_stream})
        connect(combo, &QComboBox::currentIndexChanged, this, edited);
    connect(m_brand, &QComboBox::currentIndexChanged, this, [this, edited] {
        if (m_loading)
            return;
        // المنفذ الافتراضي يتبع نوع الكاميرا ما لم يغيّره المستخدم
        const Cameras::Brand &b = Cameras::brand(m_brand->currentData().toString());
        if (m_port->value() == 554 || m_port->value() == 80 || m_port->value() == 8080) {
            const QSignalBlocker blocker(m_port);
            m_port->setValue(b.port);
        }
        edited();
        loadForm();
    });

    rebuildList();
    int row = 0;
    for (int i = 0; i < m_cameras.size(); ++i) {
        if (m_cameras.at(i).id == selectId)
            row = i;
    }
    if (!m_cameras.isEmpty())
        m_list->setCurrentRow(row);
    else
        select(-1);
}

void CameraDialog::done(int result)
{
    storeForm();
    Cameras::save(m_cameras);
    QDialog::done(result);
}

Cameras::Camera *CameraDialog::current()
{
    return m_current >= 0 && m_current < m_cameras.size() ? &m_cameras[m_current] : nullptr;
}

void CameraDialog::rebuildList()
{
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    for (const Cameras::Camera &c : m_cameras) {
        auto *item = new QListWidgetItem(
            Icons::icon(c.kind == Cameras::Kind::Network ? Icons::Name::Video : Icons::Name::Camera, Theme::Accent), c.name, m_list);
        item->setToolTip(c.description());
    }
}

void CameraDialog::select(int row)
{
    if (!m_loading)
        storeForm();
    m_current = row >= 0 && row < m_cameras.size() ? row : -1;
    ++m_testToken;
    m_preview->clear();
    m_preview->setText(QStringLiteral("اضغط «اختبار» لرؤية صورة من الكاميرا"));
    m_status->clear();
    loadForm();
}

QString CameraDialog::chooseConnection()
{
    QDialog chooser(this);
    chooser.setWindowTitle(QStringLiteral("إضافة كاميرا"));
    auto *layout = new QVBoxLayout(&chooser);
    layout->setSpacing(8);
    auto *question = new QLabel(QStringLiteral("<b>كيف الكاميرا موصولة؟</b>"), &chooser);
    question->setStyleSheet(QStringLiteral("font-size: 16px;"));
    layout->addWidget(question);
    auto *hint = new QLabel(QStringLiteral("اختر الأقرب لحالتك، وسيعرض البرنامج خطوات التوصيل خطوة بخطوة."), &chooser);
    hint->setObjectName(QStringLiteral("muted"));
    layout->addWidget(hint);
    QString chosen;
    auto *columns = new QHBoxLayout;
    columns->setSpacing(16);
    for (const Cameras::Kind kind : {Cameras::Kind::Local, Cameras::Kind::Network}) {
        auto *column = new QVBoxLayout;
        column->addWidget(sectionTitle(kind == Cameras::Kind::Local ? QStringLiteral("بسلك في اللابتوب مباشرة")
                                                                    : QStringLiteral("عبر الشبكة"),
                                       &chooser));
        for (const Cameras::Connection &c : Cameras::connections()) {
            if (c.kind != kind)
                continue;
            auto *card = new QPushButton(&chooser);
            card->setCursor(Qt::PointingHandCursor);
            card->setMinimumSize(330, 66);
            card->setStyleSheet(QStringLiteral("QPushButton { text-align: right; border-radius: 10px; padding: 0; }"));
            auto *cardLayout = new QHBoxLayout(card);
            cardLayout->setContentsMargins(12, 8, 12, 8);
            auto *icon = new QLabel(card);
            icon->setPixmap(Icons::icon(kind == Cameras::Kind::Local ? Icons::Name::Camera : Icons::Name::Video, Theme::Accent)
                                .pixmap(26, 26));
            cardLayout->addWidget(icon);
            auto *texts = new QVBoxLayout;
            texts->setSpacing(2);
            auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(c.title), card);
            auto *description = new QLabel(c.description, card);
            description->setObjectName(QStringLiteral("muted"));
            description->setWordWrap(true);
            texts->addWidget(title);
            texts->addWidget(description);
            cardLayout->addLayout(texts, 1);
            for (QLabel *label : {icon, title, description})
                label->setAttribute(Qt::WA_TransparentForMouseEvents);
            connect(card, &QPushButton::clicked, &chooser, [&chooser, &chosen, key = c.key] {
                chosen = key;
                chooser.accept();
            });
            column->addWidget(card);
        }
        column->addStretch();
        columns->addLayout(column, 1);
    }
    layout->addLayout(columns);
    auto *cancel = new FancyButton(Icons::Name::Check, QStringLiteral("إلغاء"), FancyButton::Kind::Secondary, &chooser);
    connect(cancel, &QAbstractButton::clicked, &chooser, &QDialog::reject);
    layout->addWidget(cancel, 0, Qt::AlignLeft);
    return chooser.exec() == QDialog::Accepted ? chosen : QString();
}

void CameraDialog::addCamera(const QString &via)
{
    storeForm();
    const Cameras::Connection &how = Cameras::connection(via);
    Cameras::Camera c;
    c.id = Cameras::newId();
    c.kind = how.kind;
    c.via = how.key;
    if (c.kind == Cameras::Kind::Local) {
        m_devices = Cameras::localDevices();
        // أول كاميرا موصولة لم تُحفظ بعد
        int index = m_devices.isEmpty() ? 0 : m_devices.first().index;
        for (const Cameras::LocalDevice &d : m_devices) {
            const bool used = std::any_of(m_cameras.begin(), m_cameras.end(), [&d](const Cameras::Camera &x) {
                return x.kind == Cameras::Kind::Local && x.index == d.index;
            });
            if (!used) {
                index = d.index;
                break;
            }
        }
        c.index = index;
        c.name = Cameras::localName(index, m_devices);
    } else {
        c.brand = how.brand;
        c.port = Cameras::brand(c.brand).port;
        if (via == QLatin1String("recorder")) {
            c.name = QStringLiteral("جهاز التسجيل - قناة 1");
        } else if (via == QLatin1String("phone_wifi")) {
            c.name = QStringLiteral("الهاتف");
        } else {
            int count = 1;
            for (const Cameras::Camera &x : m_cameras)
                count += x.kind == Cameras::Kind::Network ? 1 : 0;
            c.name = QStringLiteral("كاميرا شبكة %1").arg(count);
        }
    }
    m_cameras.append(c);
    rebuildList();
    m_list->setCurrentRow(int(m_cameras.size()) - 1);
    m_name->setFocus();
    m_name->selectAll();
}

void CameraDialog::removeCamera()
{
    const Cameras::Camera *camera = current();
    if (!camera)
        return;
    if (QMessageBox::question(this, QStringLiteral("حذف الكاميرا"), QStringLiteral("حذف «%1» من قائمة الكاميرات؟").arg(camera->name))
        != QMessageBox::Yes)
        return;
    m_loading = true;
    m_cameras.removeAt(m_current);
    m_current = -1;
    rebuildList();
    m_loading = false;
    if (!m_cameras.isEmpty())
        m_list->setCurrentRow(0);
    else
        select(-1);
}

void CameraDialog::refreshDevices()
{
    m_devices = Cameras::localDevices();
    loadForm();
    m_status->setText(m_devices.isEmpty() ? QStringLiteral("لم يُعثر على كاميرا موصولة بأسمائها؛ جرّب الأرقام 0 إلى 3")
                                          : QStringLiteral("الكاميرات الموصولة: %1").arg(m_devices.size()));
}

void CameraDialog::loadForm()
{
    const Cameras::Camera *camera = current();
    m_loading = true;
    m_form->setEnabled(camera);
    m_removeButton->setEnabled(camera);
    setBusy(false);
    const bool network = camera && camera->kind == Cameras::Kind::Network;
    m_localBox->setVisible(camera && !network);
    m_networkBox->setVisible(network);
    if (camera) {
        m_name->setText(camera->name);
        // الكاميرات الموصولة بأسمائها، والأرقام الأولى إن لم تُقرأ الأسماء
        m_device->clear();
        QList<int> indices;
        for (const Cameras::LocalDevice &d : m_devices)
            indices.append(d.index);
        if (indices.isEmpty())
            indices = {0, 1, 2, 3};
        if (!indices.contains(camera->index))
            indices.append(camera->index);
        for (int index : indices)
            m_device->addItem(QStringLiteral("%1 · %2").arg(index).arg(Cameras::localName(index, m_devices)), index);
        m_device->setCurrentIndex(std::max(0, m_device->findData(camera->index)));
        int resolution = 0;
        for (int i = 0; i < resolutions().size(); ++i) {
            if (resolutions().at(i).first == camera->width && resolutions().at(i).second == camera->height)
                resolution = i;
        }
        m_resolution->setCurrentIndex(resolution);
        m_backend->setCurrentIndex(int(camera->backend));

        m_brand->setCurrentIndex(std::max(0, m_brand->findData(camera->brand)));
        const Cameras::Brand &b = Cameras::brand(camera->brand);
        m_brandHint->setText(b.hint);
        m_brandHint->setVisible(!b.hint.isEmpty());
        m_host->setText(camera->host);
        m_port->setValue(camera->port);
        m_channel->setValue(camera->channel);
        m_stream->setCurrentIndex(camera->substream ? 1 : 0);
        m_path->setText(camera->path);
        m_user->setText(camera->user);
        m_password->setText(camera->password);
        const bool custom = b.pattern.isEmpty();
        m_host->parentWidget()->parentWidget()->setVisible(!custom);
        m_channelRow->setVisible(b.channels);
        m_streamRow->setVisible(b.streams);
        m_pathRow->setVisible(custom || b.pattern.contains(QStringLiteral("{path}")));
        m_pathLabel->setText(custom ? QStringLiteral("العنوان الكامل") : QStringLiteral("المسار"));
        m_path->setPlaceholderText(custom ? QStringLiteral("rtsp://192.168.1.20:554/live") : QStringLiteral("live/ch1"));
    } else {
        m_name->clear();
    }
    m_loading = false;
    updateUrl();
    updateHelp();
}

void CameraDialog::storeForm()
{
    Cameras::Camera *camera = current();
    if (!camera || m_loading)
        return;
    camera->name = m_name->text().trimmed().isEmpty() ? camera->name : m_name->text().trimmed();
    if (camera->kind == Cameras::Kind::Local) {
        const int index = m_device->currentData().isValid() ? m_device->currentData().toInt() : camera->index;
        // الاسم يتبع الجهاز المختار ما لم يكتب المستخدم اسماً خاصاً
        if (index != camera->index && camera->name == Cameras::localName(camera->index, m_devices)) {
            camera->name = Cameras::localName(index, m_devices);
            const QSignalBlocker blocker(m_name);
            m_name->setText(camera->name);
        }
        camera->index = index;
        const auto [w, h] = resolutions().value(m_resolution->currentIndex());
        camera->width = w;
        camera->height = h;
        camera->backend = Cameras::Backend(std::clamp(m_backend->currentIndex(), 0, 2));
    } else {
        camera->brand = m_brand->currentData().toString();
        camera->host = m_host->text().trimmed();
        camera->port = m_port->value();
        camera->channel = m_channel->value();
        camera->substream = m_stream->currentIndex() == 1;
        camera->path = m_path->text().trimmed();
        camera->user = m_user->text().trimmed();
        camera->password = m_password->text();
    }
    if (m_list->item(m_current))
        m_list->item(m_current)->setToolTip(camera->description());
}

void CameraDialog::updateUrl()
{
    const Cameras::Camera *camera = current();
    const bool network = camera && camera->kind == Cameras::Kind::Network;
    m_url->setText(network ? camera->url(false) : QString());
    // كاميرا على نطاق آخر لا يصلها اللابتوب: يُقال ذلك قبل الاختبار مع العنوان المطلوب
    const QString host = network ? camera->host.trimmed() : QString();
    const bool away = !host.isEmpty() && !Cameras::onLocalNetwork(host);
    m_hostCheck->setVisible(away);
    if (away)
        m_hostCheck->setText(QStringLiteral("⚠ هذا العنوان ليس على شبكة اللابتوب. إن كانت الكاميرا موصولة بسلك مباشر فاجعل عنوان "
                                            "اللابتوب <b>%1</b> والقناع 255.255.255.0 من «إعدادات الشبكة».")
                                 .arg(Cameras::suggestedAddress(host)));
}

void CameraDialog::updateHelp()
{
    const Cameras::Camera *camera = current();
    if (!camera) {
        m_help->setText(QStringLiteral("<b>لا توجد كاميرات محفوظة بعد.</b><br><br>اضغط «إضافة كاميرا» واختر كيف الكاميرا موصولة: "
                                       "بسلك USB، أو كاميرا مراقبة بمحوّل، أو عبر الشبكة، أو جهاز تسجيل، أو الهاتف. "
                                       "يعرض البرنامج بعدها خطوات التوصيل خطوة بخطوة."));
        return;
    }
    QString via = camera->via;
    if (via.isEmpty())
        via = camera->kind == Cameras::Kind::Network ? QStringLiteral("router") : QStringLiteral("usb");
    m_help->setText(Cameras::steps(via));
}

void CameraDialog::setBusy(bool busy)
{
    const bool enabled = !busy && current();
    m_testButton->setEnabled(enabled);
    m_findButton->setEnabled(enabled);
    m_detectButton->setEnabled(enabled);
    // الاختبار يُبقي الكاميرا الموصولة مفتوحة، فلا تُشغَّل قبل أن يتركها
    m_startButton->setEnabled(enabled);
}

void CameraDialog::probeCameras()
{
    storeForm();
    const int token = ++m_testToken;
    setBusy(true);
    m_status->setStyleSheet(QString());
    m_status->setText(QStringLiteral("جارٍ تجربة كل كاميرا بكل طريقة… (حتى دقيقة)"));
    QtConcurrent::run([] { return Cameras::probeLocal(); }).then(this, [this, token](const QString &report) {
        setBusy(false);
        if (token != m_testToken)
            return;
        m_status->setText(QString());
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("تشخيص الكاميرات"));
        dialog.resize(760, 420);
        auto *layout = new QVBoxLayout(&dialog);
        auto *hint = new QLabel(QStringLiteral("كل سطر محاولة لفتح كاميرا بطريقة التقاط. انسخ النتيجة وأرسلها للدعم إن لم تفتح الكاميرا."), &dialog);
        hint->setWordWrap(true);
        hint->setObjectName(QStringLiteral("muted"));
        auto *text = new QPlainTextEdit(report, &dialog);
        text->setReadOnly(true);
        text->setLayoutDirection(Qt::LeftToRight);
        auto *copy = new FancyButton(Icons::Name::Check, QStringLiteral("نسخ النتيجة"), FancyButton::Kind::Primary, &dialog);
        connect(copy, &QAbstractButton::clicked, &dialog, [report, copy] {
            QGuiApplication::clipboard()->setText(report);
            copy->setText(QStringLiteral("نُسخت"));
        });
        layout->addWidget(hint);
        layout->addWidget(text, 1);
        layout->addWidget(copy, 0, Qt::AlignLeft);
        dialog.exec();
    });
}

void CameraDialog::findCameras()
{
    storeForm();
    if (!current())
        return;
    const int token = ++m_testToken;
    setBusy(true);
    m_status->setStyleSheet(QString());
    m_status->setText(QStringLiteral("جارٍ البحث عن الكاميرات في الشبكة… (حتى 10 ثوانٍ)"));
    QtConcurrent::run([] { return Cameras::discover(); }).then(this, [this, token](const QList<Cameras::Found> &found) {
        setBusy(false);
        if (token != m_testToken)
            return;
        if (found.isEmpty()) {
            showPreview(QImage(),
                        QStringLiteral("لم يُعثر على كاميرا. تأكد أن الكاميرا تعمل (ضوءها مضاء) وأن السلك موصول، وأن اللابتوب "
                                       "على شبكة الكاميرا نفسها. بعض الكاميرات لا تعلن عن نفسها: اكتب عنوانها من تطبيقها أو "
                                       "من شاشة جهاز التسجيل."),
                        false);
            return;
        }
        QMenu menu(this);
        for (const Cameras::Found &f : found) {
            QString text = f.host;
            if (!f.model.isEmpty())
                text += QStringLiteral(" · ") + f.model;
            else if (f.brand != QLatin1String("rtsp"))
                text += QStringLiteral(" · ") + Cameras::brand(f.brand).name;
            if (!f.sameNetwork)
                text += QStringLiteral("  (يحتاج ضبط عنوان اللابتوب)");
            QAction *action = menu.addAction(Icons::icon(Icons::Name::Video, Theme::Text), text);
            action->setData(int(&f - found.constData()));
        }
        m_status->setText(QStringLiteral("وُجد %1. اختر الكاميرا من القائمة.").arg(found.size()));
        QAction *picked = menu.exec(m_findButton->mapToGlobal(QPoint(0, m_findButton->height())));
        Cameras::Camera *camera = current();
        if (!picked || !camera || camera->kind != Cameras::Kind::Network)
            return;
        const Cameras::Found f = found.at(picked->data().toInt());
        camera->host = f.host;
        // الشركة تُضبط إن عُرفت؛ وإلا يبقى النوع ويكشفه «اكتشاف تلقائي»
        if (f.brand != QLatin1String("rtsp")) {
            camera->brand = f.brand;
            camera->port = f.port;
        }
        loadForm();
        if (!f.sameNetwork)
            showPreview(QImage(),
                        QStringLiteral("الكاميرا على %1 وهو نطاق غير نطاق اللابتوب. اضغط «إعدادات الشبكة» واجعل عنوان اللابتوب "
                                       "%2 والقناع 255.255.255.0، ثم اكتب مستخدم الكاميرا وكلمة مرورها واضغط «اكتشاف تلقائي».")
                            .arg(f.host, Cameras::suggestedAddress(f.host)),
                        false);
        else
            showPreview(QImage(),
                        QStringLiteral("تم اختيار %1. اكتب اسم المستخدم وكلمة المرور، ثم اضغط «اكتشاف تلقائي» أو «اختبار».").arg(f.host),
                        true);
    });
}

void CameraDialog::detectCamera()
{
    storeForm();
    const Cameras::Camera *camera = current();
    if (!camera || camera->kind != Cameras::Kind::Network)
        return;
    const int token = ++m_testToken;
    setBusy(true);
    m_preview->clear();
    m_preview->setText(QStringLiteral("جارٍ تجربة أنواع الكاميرات…"));
    m_status->setStyleSheet(QString());
    m_status->clear();
    const Cameras::Camera spec = *camera;
    QPointer<CameraDialog> self(this);
    struct Detected
    {
        bool ok = false;
        Cameras::Camera camera;
        QImage image;
        QString error;
    };
    QtConcurrent::run([spec, self, token] {
        Detected d;
        d.camera = spec;
        cv::Mat frame;
        d.ok = Cameras::detect(d.camera, &frame, &d.error, [self, token](const QString &text) {
            QMetaObject::invokeMethod(
                qApp,
                [self, token, text] {
                    if (self && token == self->m_testToken)
                        self->m_status->setText(text);
                },
                Qt::QueuedConnection);
        });
        if (!frame.empty())
            d.image = ImageIO::toQImage(frame);
        return d;
    }).then(this, [this, token](const Detected &d) {
        setBusy(false);
        if (token != m_testToken)
            return;
        Cameras::Camera *camera = current();
        if (!d.ok || !camera || camera->id != d.camera.id) {
            showPreview(QImage(), d.error, false);
            return;
        }
        camera->brand = d.camera.brand;
        camera->port = d.camera.port;
        camera->path = d.camera.path;
        camera->substream = d.camera.substream;
        loadForm();
        showPreview(d.image,
                    QStringLiteral("✓ تم التعرف على الكاميرا: %1 · %2×%3. اضغط «تشغيل الكاميرا».")
                        .arg(Cameras::brand(d.camera.brand).name)
                        .arg(d.image.width())
                        .arg(d.image.height()),
                    true);
    });
}

void CameraDialog::showPreview(const QImage &image, const QString &text, bool ok)
{
    if (!image.isNull())
        m_preview->setPixmap(QPixmap::fromImage(image.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    else
        m_preview->setText(ok ? QString() : QStringLiteral("لا توجد صورة"));
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg((ok ? Theme::Success : Theme::Warning).name()));
    m_status->setText(text);
}

void CameraDialog::testConnection()
{
    storeForm();
    const Cameras::Camera *camera = current();
    if (!camera)
        return;
    const int token = ++m_testToken;
    setBusy(true);
    m_preview->clear();
    m_preview->setText(QStringLiteral("جارٍ الاتصال بالكاميرا…"));
    m_status->setStyleSheet(QString());
    m_status->setText(camera->kind == Cameras::Kind::Network ? camera->url(false) : QString());
    const Cameras::Camera spec = *camera;
    QPointer<CameraDialog> self(this);
    QtConcurrent::run([spec] {
        TestResult result;
        const cv::Mat frame = Cameras::snapshot(spec, &result.error, &result.info);
        if (!frame.empty())
            result.image = ImageIO::toQImage(frame);
        return result;
    }).then(this, [this, token](const TestResult &result) {
        setBusy(false);
        if (token != m_testToken)
            return;
        const Cameras::Camera *camera = current();
        if (result.image.isNull())
            showPreview(QImage(),
                        result.error
                            + (camera && camera->kind == Cameras::Kind::Network && !camera->host.trimmed().isEmpty()
                                   ? QStringLiteral("\n\nلا تعرف الإعدادات الصحيحة؟ اضغط «اكتشاف تلقائي».")
                                   : QString()),
                        false);
        else
            showPreview(result.image, QStringLiteral("✓ الكاميرا تعمل · %1").arg(result.info), true);
    });
}

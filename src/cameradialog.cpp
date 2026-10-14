#include "cameradialog.h"

#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "widgets.h"

#include <QComboBox>
#include <QFuture>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QScrollArea>
#include <QSpinBox>
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

QString localHelp()
{
    return QStringLiteral(
        "<b>كيف أوصل الكاميرا باللابتوب مباشرة؟</b>"
        "<ul style='margin-right:-20px'>"
        "<li><b>كاميرا USB</b> (ويب أو مؤتمرات): تُوصل بمنفذ USB وتظهر فوراً في القائمة.</li>"
        "<li><b>كاميرا مراقبة تناظرية</b> (سلك محوري BNC): محوّل «USB Video Capture» مثل EasyCAP، "
        "مع وصلة BNC إلى RCA ومحوّل كهرباء 12 فولت للكاميرا. فعّل «إزالة التشابك» من «معالجات».</li>"
        "<li><b>كاميرات AHD / TVI / CVI</b> (تناظرية عالية الدقة): المحوّل الرخيص لا يقرؤها؛ حوّل الكاميرا إلى "
        "وضع CVBS من قائمتها (زر أو سلك التحكم)، أو استخدم محوّل AHD إلى HDMI ثم بطاقة التقاط HDMI.</li>"
        "<li><b>كاميرا فيديو أو احترافية أو مخرج HDMI لجهاز تسجيل DVR</b>: بطاقة التقاط HDMI إلى USB "
        "(مثل Cam Link). كاميرات SDI تحتاج محوّل SDI إلى HDMI قبلها.</li>"
        "<li><b>الهاتف بسلك USB</b>: تطبيق DroidCam أو Camo يجعله كاميرا ويب على الجهاز.</li>"
        "</ul>"
        "كل هذه تظهر للنظام كاميرا برقم. إن لم تعمل الصورة جرّب «طريقة الالتقاط» الأخرى أو دقة أقل، "
        "وأغلق أي برنامج آخر يستخدم الكاميرا.");
}

QString networkHelp()
{
    const QStringList mine = Cameras::localAddresses();
    return QStringLiteral(
               "<b>كاميرات الشبكة وأجهزة التسجيل</b>"
               "<ul style='margin-right:-20px'>"
               "<li><b>سلك مباشر من الكاميرا إلى اللابتوب</b>: كاميرا PoE تحتاج «حاقن PoE» صغيراً يعطيها الكهرباء. "
               "اضبط عنوان اللابتوب يدوياً على نطاق الكاميرا: إن كانت 192.168.1.64 فاجعل اللابتوب 192.168.1.10 "
               "والقناع 255.255.255.0.</li>"
               "<li><b>عبر الراوتر أو Wi-Fi</b>: الكاميرا واللابتوب على الشبكة نفسها؛ عنوان الكاميرا من تطبيقها أو من صفحة الراوتر.</li>"
               "<li><b>جهاز تسجيل DVR / NVR</b>: عنوان الجهاز نفسه، والقناة رقم الكاميرا فيه. الكاميرات التناظرية "
               "الموصولة بالجهاز تصل بهذه الطريقة أيضاً.</li>"
               "<li><b>البث الفرعي</b> أخف وأسرع عبر Wi-Fi، والرئيسي أوضح للوجوه البعيدة.</li>"
               "</ul>")
        + QStringLiteral("عناوين هذا اللابتوب الآن: <b>%1</b>")
              .arg(mine.isEmpty() ? QStringLiteral("غير متصل بأي شبكة") : mine.join(QStringLiteral("، ")));
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

    auto *addMenu = new QMenu(this);
    addMenu->addAction(Icons::icon(Icons::Name::Camera, Theme::Text), QStringLiteral("كاميرا موصولة بالجهاز (USB، تناظرية، HDMI)"),
                       this, [this] { addCamera(Cameras::Kind::Local); });
    addMenu->addAction(Icons::icon(Icons::Name::Video, Theme::Text), QStringLiteral("كاميرا شبكة أو جهاز تسجيل (IP / RTSP)"),
                       this, [this] { addCamera(Cameras::Kind::Network); });
    connect(addButton, &QAbstractButton::clicked, this, [addButton, addMenu] {
        addMenu->exec(addButton->mapToGlobal(QPoint(0, addButton->height())));
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
    m_preview->setStyleSheet(QStringLiteral("background: #050d19; border-radius: 10px; color: #8aa0bd;"));
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
    };
    connect(m_name, &QLineEdit::textEdited, this, [this, edited] {
        edited();
        if (m_current >= 0 && m_list->item(m_current))
            m_list->item(m_current)->setText(m_cameras.at(m_current).name);
    });
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

void CameraDialog::addCamera(Cameras::Kind kind)
{
    storeForm();
    Cameras::Camera c;
    c.id = Cameras::newId();
    c.kind = kind;
    if (kind == Cameras::Kind::Local) {
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
        int count = 1;
        for (const Cameras::Camera &x : m_cameras)
            count += x.kind == Cameras::Kind::Network ? 1 : 0;
        c.name = QStringLiteral("كاميرا شبكة %1").arg(count);
        c.port = Cameras::brand(c.brand).port;
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
    m_testButton->setEnabled(camera);
    m_startButton->setEnabled(camera);
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
        camera->index = m_device->currentData().isValid() ? m_device->currentData().toInt() : camera->index;
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
    m_url->setText(camera && camera->kind == Cameras::Kind::Network ? camera->url(false) : QString());
}

void CameraDialog::updateHelp()
{
    const Cameras::Camera *camera = current();
    if (!camera) {
        m_help->setText(QStringLiteral("<b>لا توجد كاميرات محفوظة بعد.</b><br>اضغط «إضافة كاميرا» واختر نوعها.<br><br>") + localHelp()
                        + QStringLiteral("<br><br>") + networkHelp());
        return;
    }
    m_help->setText(camera->kind == Cameras::Kind::Network ? networkHelp() : localHelp());
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
    m_testButton->setEnabled(false);
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
        m_testButton->setEnabled(current());
        if (token != m_testToken)
            return;
        if (result.image.isNull())
            showPreview(QImage(), result.error, false);
        else
            showPreview(result.image, QStringLiteral("✓ الكاميرا تعمل · %1").arg(result.info), true);
    });
}

#include "persondialog.h"
#include "widgets.h"

#include "personmodel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QTimer>

PersonDialog::PersonDialog(const Person &person, QWidget *parent)
    : QDialog(parent)
    , m_person(person)
    , m_name(new QLineEdit(person.name, this))
    , m_nationalId(new QLineEdit(person.nationalId, this))
    , m_phone(new QLineEdit(person.phone, this))
    , m_gender(new QComboBox(this))
    , m_hasBirthDate(new QCheckBox(this))
    , m_birthDate(new QDateEdit(this))
    , m_email(new QLineEdit(person.email, this))
    , m_address(new QLineEdit(person.address, this))
    , m_tags(new QLineEdit(person.tags, this))
    , m_notes(new QPlainTextEdit(person.notes, this))
{
    setWindowTitle(person.id.isEmpty() ? QStringLiteral("شخص جديد") : QStringLiteral("تعديل بيانات الشخص"));
    resize(460, 480);

    m_gender->addItems({QStringLiteral("غير محدد"), QStringLiteral("ذكر"), QStringLiteral("أنثى")});
    m_gender->setCurrentIndex(std::max(0, m_gender->findText(person.gender)));
    setLtrDisplayFormat(m_birthDate, QStringLiteral("yyyy/MM/dd"));
    m_birthDate->setCalendarPopup(true);
    m_birthDate->setMaximumDate(QDate::currentDate());
    m_birthDate->setDate(person.birthDate.isValid() ? person.birthDate : QDate(1990, 1, 1));
    m_hasBirthDate->setChecked(person.birthDate.isValid());
    m_birthDate->setEnabled(m_hasBirthDate->isChecked());
    connect(m_hasBirthDate, &QCheckBox::toggled, m_birthDate, &QWidget::setEnabled);
    auto *birthRow = new QHBoxLayout;
    birthRow->addWidget(m_hasBirthDate);
    birthRow->addWidget(m_birthDate, 1);
    m_email->setPlaceholderText(QStringLiteral("name@example.com"));
    m_tags->setPlaceholderText(QStringLiteral("مثال: موظف، زائر، مطلوب"));
    m_tags->setToolTip(QStringLiteral("تصنيفات مفصولة بفواصل، يمكن البحث بها في القاعدة"));
    auto *completer = new QCompleter({QStringLiteral("موظف"), QStringLiteral("زائر"), QStringLiteral("عميل"), QStringLiteral("مطلوب"),
                                      QStringLiteral("ممنوع من الدخول"), QStringLiteral("VIP"), QStringLiteral("مفقود")},
                                     this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_tags->setCompleter(completer);
    m_notes->setMinimumHeight(80);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("الاسم:"), m_name);
    form->addRow(QStringLiteral("رقم الهوية:"), m_nationalId);
    form->addRow(QStringLiteral("الجنس:"), m_gender);
    form->addRow(QStringLiteral("تاريخ الميلاد:"), birthRow);
    form->addRow(QStringLiteral("الهاتف:"), m_phone);
    form->addRow(QStringLiteral("البريد:"), m_email);
    form->addRow(QStringLiteral("العنوان:"), m_address);
    form->addRow(QStringLiteral("التصنيفات:"), m_tags);
    form->addRow(QStringLiteral("ملاحظات:"), m_notes);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("حفظ"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("إلغاء"));
    connect(buttons, &QDialogButtonBox::accepted, this, &PersonDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PersonDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

Person PersonDialog::person() const
{
    Person person = m_person;
    person.name = m_name->text().trimmed();
    person.nationalId = m_nationalId->text().trimmed();
    person.phone = m_phone->text().trimmed();
    person.gender = m_gender->currentIndex() > 0 ? m_gender->currentText() : QString();
    person.birthDate = m_hasBirthDate->isChecked() ? m_birthDate->date() : QDate();
    person.email = m_email->text().trimmed();
    person.address = m_address->text().trimmed();
    QStringList tags;
    for (QString tag : m_tags->text().split(QRegularExpression(QStringLiteral("[,،]")), Qt::SkipEmptyParts)) {
        tag = tag.trimmed();
        if (!tag.isEmpty() && !tags.contains(tag))
            tags << tag;
    }
    person.tags = tags.join(QStringLiteral("، "));
    person.notes = m_notes->toPlainText().trimmed();
    return person;
}

void PersonDialog::accept()
{
    if (m_name->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("يرجى إدخال الاسم"));
        m_name->setFocus();
        return;
    }
    QDialog::accept();
}

PersonPickerDialog::PersonPickerDialog(const FaceDatabase *database, const QString &suggestedId, QWidget *parent)
    : QDialog(parent)
    , m_search(new QLineEdit(this))
    , m_list(new QListView(this))
    , m_model(new PersonListModel(database, this))
{
    setWindowTitle(QStringLiteral("إضافة الوجه"));
    resize(440, 500);
    m_search->setPlaceholderText(QStringLiteral("ابحث بالاسم أو رقم الهوية أو الهاتف…"));
    m_search->setClearButtonEnabled(true);
    m_list->setUniformItemSizes(true);
    m_list->setModel(m_model);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("إضافة"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("إلغاء"));
    QPushButton *newPerson = buttons->addButton(QStringLiteral("＋ شخص جديد…"), QDialogButtonBox::ActionRole);
    connect(newPerson, &QPushButton::clicked, this, [this] {
        m_newPerson = true;
        accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListView::doubleClicked, this, &QDialog::accept);

    // البحث في القاعدة بعد توقف الكتابة قليلاً، لا مع كل حرف
    auto *debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(250);
    connect(m_search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    auto selectFirst = [this] {
        if (m_model->rowCount() > 0)
            m_list->setCurrentIndex(m_model->index(0));
    };
    connect(debounce, &QTimer::timeout, this, [this, selectFirst] {
        m_model->setFilter(m_search->text());
        selectFirst();
    });
    auto updateOk = [this, buttons] { buttons->button(QDialogButtonBox::Ok)->setEnabled(m_list->currentIndex().isValid()); };
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, updateOk);
    connect(m_model, &QAbstractItemModel::modelReset, this, updateOk);

    // الشخص المقترح (أقرب مطابقة) يظهر أولاً
    const Person suggested = suggestedId.isEmpty() ? Person() : database->person(suggestedId);
    if (!suggested.id.isEmpty()) {
        const QSignalBlocker blocker(m_search);
        m_search->setText(suggested.nationalId.isEmpty() ? suggested.name : suggested.nationalId);
        m_model->setFilter(m_search->text());
        const int row = m_model->rowOf(suggested.id);
        m_list->setCurrentIndex(m_model->index(std::max(0, row)));
    } else {
        m_model->reload();
        selectFirst();
    }
    updateOk();

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("أضف هذا الوجه إلى:"), this));
    layout->addWidget(m_search);
    layout->addWidget(m_list, 1);
    layout->addWidget(buttons);
    m_search->setFocus();
    m_search->selectAll();
}

QString PersonPickerDialog::personId() const
{
    return m_newPerson ? QString() : m_model->idAt(m_list->currentIndex().row());
}

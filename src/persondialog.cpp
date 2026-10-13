#include "persondialog.h"

#include "personmodel.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>

PersonDialog::PersonDialog(const Person &person, QWidget *parent)
    : QDialog(parent)
    , m_person(person)
    , m_name(new QLineEdit(person.name, this))
    , m_nationalId(new QLineEdit(person.nationalId, this))
    , m_phone(new QLineEdit(person.phone, this))
    , m_notes(new QPlainTextEdit(person.notes, this))
{
    setWindowTitle(person.id.isEmpty() ? QStringLiteral("شخص جديد") : QStringLiteral("تعديل بيانات الشخص"));
    resize(420, 320);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("الاسم:"), m_name);
    form->addRow(QStringLiteral("رقم الهوية:"), m_nationalId);
    form->addRow(QStringLiteral("الهاتف:"), m_phone);
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

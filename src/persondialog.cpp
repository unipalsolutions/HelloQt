#include "persondialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>

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

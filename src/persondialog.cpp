#include "persondialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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

PersonPickerDialog::PersonPickerDialog(const QList<Person> &persons, const QString &suggestedId, QWidget *parent)
    : QDialog(parent)
    , m_search(new QLineEdit(this))
    , m_list(new QListWidget(this))
{
    setWindowTitle(QStringLiteral("إضافة الوجه"));
    resize(420, 480);
    m_search->setPlaceholderText(QStringLiteral("ابحث بالاسم أو رقم الهوية…"));
    m_search->setClearButtonEnabled(true);
    m_list->setUniformItemSizes(true);

    auto *newPerson = new QListWidgetItem(QStringLiteral("＋ شخص جديد…"), m_list);
    newPerson->setData(Qt::UserRole + 1, QString());
    m_list->setCurrentItem(newPerson);
    m_list->setUpdatesEnabled(false);
    for (const Person &person : persons) {
        auto *item = new QListWidgetItem(
            person.nationalId.isEmpty() ? person.name : QStringLiteral("%1 (%2)").arg(person.name, person.nationalId), m_list);
        item->setData(Qt::UserRole, person.id);
        item->setData(Qt::UserRole + 1, person.name + QLatin1Char(' ') + person.nationalId + QLatin1Char(' ') + person.phone);
        if (person.id == suggestedId)
            m_list->setCurrentItem(item);
    }
    m_list->setUpdatesEnabled(true);
    m_list->scrollToItem(m_list->currentItem(), QAbstractItemView::PositionAtCenter);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("إضافة"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("إلغاء"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(m_search, &QLineEdit::textChanged, this, &PersonPickerDialog::applyFilter);
    connect(m_list, &QListWidget::currentItemChanged, this,
            [buttons](QListWidgetItem *current) { buttons->button(QDialogButtonBox::Ok)->setEnabled(current); });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("أضف هذا الوجه إلى:"), this));
    layout->addWidget(m_search);
    layout->addWidget(m_list, 1);
    layout->addWidget(buttons);
    m_search->setFocus();
}

QString PersonPickerDialog::personId() const
{
    const QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void PersonPickerDialog::applyFilter()
{
    const QString filter = m_search->text().trimmed();
    QListWidgetItem *firstVisible = nullptr;
    m_list->setUpdatesEnabled(false);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const bool newPerson = i == 0;
        const bool visible = newPerson || filter.isEmpty()
            || item->data(Qt::UserRole + 1).toString().contains(filter, Qt::CaseInsensitive);
        item->setHidden(!visible);
        if (visible && !newPerson && !firstVisible)
            firstVisible = item;
    }
    m_list->setUpdatesEnabled(true);
    if (!filter.isEmpty() && firstVisible)
        m_list->setCurrentItem(firstVisible);
}

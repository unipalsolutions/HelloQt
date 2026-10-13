#pragma once

#include "facedatabase.h"

#include <QDialog>

class QLineEdit;
class QListWidget;
class QPlainTextEdit;

class PersonDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PersonDialog(const Person &person, QWidget *parent = nullptr);

    Person person() const;

    void accept() override;

private:
    Person m_person;
    QLineEdit *m_name;
    QLineEdit *m_nationalId;
    QLineEdit *m_phone;
    QPlainTextEdit *m_notes;
};

// اختيار شخص بالبحث، يبقى سريعاً حتى مع آلاف الأشخاص
class PersonPickerDialog : public QDialog
{
    Q_OBJECT

public:
    PersonPickerDialog(const QList<Person> &persons, const QString &suggestedId, QWidget *parent = nullptr);

    // فارغ يعني "شخص جديد"
    QString personId() const;

private:
    void applyFilter();

    QLineEdit *m_search;
    QListWidget *m_list;
};

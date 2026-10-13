#pragma once

#include "facedatabase.h"

#include <QDialog>

class QLineEdit;
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

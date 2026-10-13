#pragma once

#include "facedatabase.h"

#include <QDialog>

class PersonListModel;
class QLineEdit;
class QListView;
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

// اختيار شخص بالبحث في القاعدة مباشرة، يبقى سريعاً حتى مع ملايين الأشخاص
class PersonPickerDialog : public QDialog
{
    Q_OBJECT

public:
    PersonPickerDialog(const FaceDatabase *database, const QString &suggestedId, QWidget *parent = nullptr);

    // فارغ يعني "شخص جديد"
    QString personId() const;

private:
    QLineEdit *m_search;
    QListView *m_list;
    PersonListModel *m_model;
    bool m_newPerson = false;
};

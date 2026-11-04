#pragma once
#include "application/PeopleService.h"
#include <QWidget>
#include <functional>
class QTableWidget;
class QPushButton;
class QLineEdit;
class QComboBox;
class QLabel;
class PeoplePage : public QWidget {
public:
    PeoplePage(PeopleService &people, const Session &session, QWidget *parent = nullptr);
    void reload();
    std::function<void()> onExpired;
    std::function<void()> onChanged;
private:
    PeopleService &people;
    const Session &session;
    QTableWidget *table;
    QPushButton *create, *edit, *remove = nullptr, *retry;
    QLineEdit *search;
    QComboBox *filter;
    QLabel *state, *summary, *feedback, *emptyTitle, *emptyBody;
    QVector<Person> rows;
    int loadGeneration = 0;
    qint64 selectedId = 0, changedId = 0;
    QString savedMessage;
    const Person *selected() const;
    void updateActions();
    void savePerson(const std::optional<Person> &expected);
    void inactivatePerson();
};

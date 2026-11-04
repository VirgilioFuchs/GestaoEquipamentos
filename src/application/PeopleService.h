#pragma once
#include "application/AuthService.h"
#include "domain/Catalog.h"
#include <QVector>

class PeopleService {
public:
    PeopleService(LocalDatabase &database, AuthService &auth);
    QVector<Person> people(CatalogFilter filter = CatalogFilter::Active, const QString &search = {}) const;
    QVector<Person> activePeople() const;
    QVector<Person> activeHomonyms(const QString &name) const;
    qint64 create(const Session &session, const PersonDraft &draft);
    void update(const Session &session, const Person &expected, const PersonDraft &draft);
    void inactivate(const Session &session, qint64 personId);

private:
    LocalDatabase &database;
    AuthService &auth;
};

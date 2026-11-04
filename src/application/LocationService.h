#pragma once
#include "application/AuthService.h"
#include "domain/Catalog.h"
#include <QVector>

class LocationService {
public:
    LocationService(LocalDatabase &database, AuthService &auth);
    QVector<Location> locations(CatalogFilter filter = CatalogFilter::Active, const QString &search = {}) const;
    QStringList activeLocationNames() const;
    qint64 create(const Session &session, const QString &name);
    int notebooksAt(qint64 locationId) const;
    void softDelete(const Session &session, qint64 locationId);
    void rename(const Session &session, const Location &expected, const QString &name);

private:
    LocalDatabase &database;
    AuthService &auth;
};

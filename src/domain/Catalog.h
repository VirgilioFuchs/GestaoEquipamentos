#pragma once
#include <QString>
#include "domain/Notebook.h"

enum class CatalogFilter {
    Active,
    Inactive,
    All
};
struct Person {
    qint64 id = 0;
    QString name;
    ResponsibleRole role = ResponsibleRole::Unknown;
    QString subject;
    bool active = true;
};
struct PersonDraft {
    QString name;
    ResponsibleRole role = ResponsibleRole::Unknown;
    QString subject;
};
struct Location {
    qint64 id;
    QString name;
    bool deleted;
    int notebookCount;
};

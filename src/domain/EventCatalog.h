#pragma once
#include <QString>
#include <QVector>
struct EventKind {
    QString action, label, group;
};
const QVector<EventKind> &eventCatalog();
QString eventLabel(const QString &action);
QString eventGroup(const QString &action);

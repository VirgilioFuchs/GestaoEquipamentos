#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <optional>

struct HistoryEntry {
    qint64 id = 0;
    QDateTime happenedAt;
    QString entityType;
    QString assetTag;
    QString notebookName;
    QString action;
    QString details;
    QString actorUsername;
    QJsonObject changes;
    std::optional<qint64> notebookId, loanId;
    QString responsibleName, responsibleRole, responsibleSubject, checkoutLocation, returnLocation;
};

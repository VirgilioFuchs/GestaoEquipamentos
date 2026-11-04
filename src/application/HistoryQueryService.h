#pragma once
#include "domain/HistoryFilter.h"
#include <QTimeZone>
class LocalDatabase;
// All operations execute synchronously on the owning (GUI) thread.
class HistoryQueryService {
public:
    static constexpr int DefaultLimit = 500;
    explicit HistoryQueryService(LocalDatabase &database) : database(database) {}
    HistoryQueryService(const HistoryQueryService &) = delete;
    HistoryQueryService &operator=(const HistoryQueryService &) = delete;
    HistoryPage search(const HistoryFilter &, const QDateTime &, const QTimeZone &, int limit = DefaultLimit,
                       QStringList *queryPlan = nullptr) const;
    HistoryFilterOptions filterOptions() const;

private:
    LocalDatabase &database;
};

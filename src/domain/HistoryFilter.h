#pragma once
#include "HistoryEntry.h"
#include <QDate>
#include <QStringList>
#include <QVector>
#include <optional>

enum class PeriodPreset {
    Today,
    Last7Days,
    Last30Days,
    All,
    Custom
};
inline QString periodLabel(PeriodPreset period) {
    switch (period) {
    case PeriodPreset::Today:
        return QStringLiteral("Hoje");
    case PeriodPreset::Last7Days:
        return QStringLiteral("Últimos 7 dias");
    case PeriodPreset::Last30Days:
        return QStringLiteral("Últimos 30 dias");
    case PeriodPreset::All:
        return QStringLiteral("Todo o período");
    case PeriodPreset::Custom:
        return QStringLiteral("Personalizado");
    }
    return {};
}
struct PeriodRange {
    std::optional<QDateTime> startUtc, endUtc;
};
struct HistoryFilter {
    PeriodPreset period = PeriodPreset::Today;
    QDate customFrom, customTo;
    std::optional<qint64> notebookId, accountId, personId, locationId;
    QString personText, legacyLocationText, action;
    int activeCount() const {
        return int(period != PeriodPreset::All) + int(notebookId.has_value()) + int(accountId.has_value()) +
               int(personId.has_value() || !personText.trimmed().isEmpty()) +
               int(locationId.has_value() || !legacyLocationText.trimmed().isEmpty()) +
               int(!action.trimmed().isEmpty());
    }
    bool operator==(const HistoryFilter &o) const {
        return period == o.period &&
               (period != PeriodPreset::Custom || (customFrom == o.customFrom && customTo == o.customTo)) &&
               notebookId == o.notebookId && accountId == o.accountId && personId == o.personId &&
               locationId == o.locationId && personText == o.personText && legacyLocationText == o.legacyLocationText &&
               action == o.action;
    }
};
struct HistoryPage {
    QVector<HistoryEntry> entries;
    bool truncated = false;
};
struct FilterOption {
    qint64 id = 0;
    QString label;
    bool inactive = false;
};
struct HistoryFilterOptions {
    QVector<FilterOption> notebooks, accounts, activeLocations, deletedLocations;
    QStringList legacyLocationTexts, actions;
};

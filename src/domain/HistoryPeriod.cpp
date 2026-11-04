#include "HistoryPeriod.h"
#include <stdexcept>

PeriodRange resolvePeriod(PeriodPreset preset, QDate from, QDate to, const QDateTime &nowUtc, const QTimeZone &zone) {
    if (preset == PeriodPreset::All) return {};
    const QDate today = nowUtc.toTimeZone(zone).date();
    if (preset != PeriodPreset::Custom) {
        to = today;
        from = today.addDays(preset == PeriodPreset::Last7Days ? -6 : preset == PeriodPreset::Last30Days ? -29 : 0);
    }
    if (!from.isValid() || !to.isValid() || from > to)
        throw std::invalid_argument("A data inicial deve ser igual ou anterior à data final.");
    return {from.startOfDay(zone).toUTC(), to.addDays(1).startOfDay(zone).toUTC()};
}
QString searchFold(const QString &text) {
    QString result;
    const QString normalized = text.normalized(QString::NormalizationForm_D);
    for (const QChar c : normalized) if (c.category() != QChar::Mark_NonSpacing) result.append(c);
    return result.toCaseFolded().simplified();
}

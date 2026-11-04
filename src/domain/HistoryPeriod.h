#pragma once
#include "HistoryFilter.h"
#include <QTimeZone>
PeriodRange resolvePeriod(PeriodPreset preset, QDate from, QDate to, const QDateTime &nowUtc, const QTimeZone &zone);
QString searchFold(const QString &text);

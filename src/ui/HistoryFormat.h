#pragma once
#include "domain/HistoryEntry.h"

QString describeChanges(const QJsonObject &changes);
QString historyDetails(const HistoryEntry &entry);
QString historyAssetTagLabel(const HistoryEntry &entry);
QString historyNotebookNameLabel(const HistoryEntry &entry);
// Colunas da página Logs: sempre o snapshot gravado na época, nunca o cadastro atual.
QString notebookText(const HistoryEntry &entry);
QString responsibleText(const HistoryEntry &entry);
QString locationText(const HistoryEntry &entry);
QString actorText(const HistoryEntry &entry);

#pragma once
#include "application/AuthService.h"
#include "domain/HistoryEntry.h"
#include "domain/Notebook.h"
#include "storage/LocalDatabase.h"
#include <QVector>
#include <optional>

// All operations execute synchronously on the owning (GUI) thread.
class NotebookStore {
public:
    NotebookStore(LocalDatabase &database, AuthService &auth);
    NotebookStore(const NotebookStore &) = delete;
    NotebookStore &operator=(const NotebookStore &) = delete;
    QVector<Notebook> notebooks() const;
    QVector<HistoryEntry> history() const;
    LocalDatabase &localDatabase() const { return database; }
    // Todo local e toda pessoa por id de cadastro ativo; nenhum caminho aceita texto livre.
    qint64 add(const Session &session, const QString &assetTag, const QString &name, qint64 locationId);
    // Retirada: snapshot da pessoa, local de uso e origem gravados com um único nowUtc().
    void checkout(const Session &session, qint64 notebookId, qint64 personId, qint64 locationId);
    // A pessoa que devolve pode ser diferente da que retirou; encerra só o loan aberto (mesmo nowUtc() do
    // evento).
    void checkin(const Session &session, qint64 notebookId, qint64 personId, qint64 locationId);
    // Origem do loan aberto, somente se o local não foi excluído.
    std::optional<qint64> suggestedReturnLocation(qint64 notebookId) const;
    // U2: só locais ativos; recusa notebook em manutenção. Evento com location_id/from_location_id.
    void updateLocation(const Session &session, qint64 notebookId, qint64 locationId);
    // Nunca altera o local. Observação (até 200, aparada) só ao enviar; gravada em changes.maintenance_note.
    void setMaintenance(const Session &session, qint64 id, bool enabled, const QString &note = QString());
    // expected: cópia do notebook selecionado; divergência com o banco recusa a edição como seleção obsoleta.
    // LocationId nullopt mantém o local atual (inclusive excluído ou texto legado).
    void update(const Session &session, const Notebook &expected, const QString &assetTag, const QString &name,
                std::optional<qint64> locationId);
    void inactivate(const Session &session, qint64 notebookId);
    // Espelho de inactivate(): mesma permissão; só notebooks inativos podem ser reativados.
    void reactivate(const Session &session, qint64 notebookId);

private:
    struct Responsible;
    Responsible readActivePerson(qint64 personId);
    LocalDatabase &database;
    AuthService &auth;
    QSqlDatabase &db;
    void audit(const Actor &actor, qint64 id, const QString &action, const QString &details, const QJsonObject &changes,
               const QString &at);
};

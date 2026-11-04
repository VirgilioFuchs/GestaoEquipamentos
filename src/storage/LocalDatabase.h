#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QStringList>
#include <functional>
#include <optional>

struct Actor {
    qint64 accountId;
    QString username;
};

// changes: {"<campo>": {"de": <anterior>, "para": <novo>}} com campos de changeFieldAllowlist() e valores escalares ou null.
struct EventRecord {
    QString entityType;
    std::optional<qint64> entityId;
    std::optional<qint64> notebookId;
    QString action;
    QString details;
    std::optional<Actor> actor;
    QJsonObject changes;
    QString happenedAt;
    // v6: vínculos estruturados para filtros da fase 4 (NULL quando vazios).
    std::optional<qint64> loanId;
    std::optional<qint64> locationId;
    std::optional<qint64> fromLocationId;
};

enum class DataProfile { Operational, Demo };

// All operations execute synchronously on the owning (GUI) thread.
class LocalDatabase {
public:
    static constexpr int CurrentSchemaVersion = 7;
    static void applyApplicationIdentity();
    // Mesmo caminho para o aplicativo e para o GestaoEquipamentosAdmin; exige applyApplicationIdentity().
    static QString profilePath(DataProfile profile);
    explicit LocalDatabase(const QString &path);
    ~LocalDatabase();
    LocalDatabase(const LocalDatabase &) = delete;
    LocalDatabase &operator=(const LocalDatabase &) = delete;
    QSqlDatabase &connection();
    void transaction(const std::function<void()> &operation);
    bool inTransaction() const;
    qint64 appendEvent(const EventRecord &event);
    QString databaseUuid() const;
    QString lastBackupPath() const;
    static QString nowUtc();
    static QString catalogKey(const QString &name);
    static QJsonObject change(const QString &field, const QJsonValue &from, const QJsonValue &to);
    static const QStringList &changeFieldAllowlist();
private:
    QSqlDatabase db;
    QString name;
    QString backupPath;
    bool active = false;
};

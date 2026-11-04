#include "LocalDatabase.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVector>
#include <stdexcept>

namespace {
[[noreturn]] void fail(const QString &message) {
    throw std::runtime_error(message.toStdString());
}
void run(QSqlQuery &q) {
    if (!q.exec())
        fail(q.lastError().text());
}
void sql(const QSqlDatabase &db, const QString &statement) {
    QSqlQuery q(db);
    if (!q.exec(statement))
        fail(q.lastError().text());
}
int schemaVersion(const QSqlDatabase &db) {
    QSqlQuery q(db);
    if (!q.exec("PRAGMA user_version") || !q.next())
        fail("Não foi possível ler a versão do banco.");
    return q.value(0).toInt();
}
const QStringList &secretFieldNames() {
    static const QStringList names{"password", "password_verifier", "secret", "proof", "code", "uri", "token"};
    return names;
}
void validateChanges(const QJsonObject &changes) {
    const auto &allowed = LocalDatabase::changeFieldAllowlist();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        if (secretFieldNames().contains(it.key(), Qt::CaseInsensitive) || !allowed.contains(it.key()))
            fail("Campo não permitido no histórico.");
        if (!it.value().isObject())
            fail("Formato de alteração inválido no histórico.");
        const auto entry = it.value().toObject();
        if (entry.size() != 2 || !entry.contains("de") || !entry.contains("para"))
            fail("Formato de alteração inválido no histórico.");
        for (const auto &side : {entry.value("de"), entry.value("para")})
            if (!side.isNull() && !side.isBool() && !side.isDouble() && !side.isString())
                fail("Formato de alteração inválido no histórico.");
    }
}
QString backupBeforeUpgrade(QSqlDatabase &db, const QString &path, int targetVersion) {
    const QFileInfo file(path);
    const auto folder = file.absoluteDir().filePath("backups");
    if (!QDir().mkpath(folder))
        fail("Não foi possível criar a cópia de segurança antes de atualizar o banco. Nada foi alterado.");
    const auto stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmssZ");
    const auto base = QDir(folder).filePath(file.fileName() + ".pre-v" + QString::number(targetVersion) + "-" + stamp);
    auto target = base + ".bak";
    for (int n = 1; QFileInfo::exists(target); ++n)
        target = base + "-" + QString::number(n) + ".bak";
    QSqlQuery q(db);
    q.prepare("VACUUM INTO ?");
    q.addBindValue(target);
    if (!q.exec())
        fail("Não foi possível criar a cópia de segurança antes de atualizar o banco. Nada foi alterado.");
    return target;
}
void migrateToV2(QSqlDatabase &db) {
    sql(db, "CREATE TABLE IF NOT EXISTS notebooks(id INTEGER PRIMARY KEY, asset_tag TEXT NOT NULL COLLATE NOCASE "
            "UNIQUE, name TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'available' CHECK(status IN "
            "('available','loaned','maintenance')))");
    sql(db, "CREATE TABLE IF NOT EXISTS loans(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES "
            "notebooks(id), borrower TEXT NOT NULL, checked_out_at TEXT NOT NULL, returned_at TEXT)");
    sql(db, "CREATE UNIQUE INDEX IF NOT EXISTS one_open_loan ON loans(notebook_id) WHERE returned_at IS NULL");
    sql(db, "CREATE INDEX IF NOT EXISTS loans_by_notebook ON loans(notebook_id,id)");
    sql(db, "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES "
            "notebooks(id), action TEXT NOT NULL, details TEXT NOT NULL, happened_at TEXT NOT NULL)");
    sql(db, "ALTER TABLE notebooks ADD COLUMN location TEXT");
    sql(db, "ALTER TABLE loans ADD COLUMN borrower_role TEXT CHECK(borrower_role IN ('professor','assistant'))");
    sql(db, "ALTER TABLE loans ADD COLUMN returned_by TEXT");
    sql(db, "ALTER TABLE loans ADD COLUMN returned_by_role TEXT CHECK(returned_by_role IN ('professor','assistant'))");
}
void migrateToV3(QSqlDatabase &db, int fromVersion) {
    Q_UNUSED(fromVersion);
    sql(db,
        "CREATE TABLE accounts(id INTEGER PRIMARY KEY, username TEXT NOT NULL COLLATE NOCASE UNIQUE, password_verifier "
        "TEXT NOT NULL, "
        "role TEXT NOT NULL CHECK(role IN ('principal','admin','common')), active INTEGER NOT NULL DEFAULT 1 "
        "CHECK(active IN (0,1)), "
        "admin_slot INTEGER UNIQUE CHECK(admin_slot BETWEEN 1 AND 3), must_change_password INTEGER NOT NULL DEFAULT 0 "
        "CHECK(must_change_password IN (0,1)), "
        "credential_version INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, created_by INTEGER REFERENCES "
        "accounts(id), "
        "CHECK((role<>'common' AND active=1) = (admin_slot IS NOT NULL)), CHECK(role<>'principal' OR admin_slot=1))");
    sql(db, "CREATE TABLE app_config(id INTEGER PRIMARY KEY CHECK(id=1), database_uuid TEXT NOT NULL, "
            "principal_account_id INTEGER UNIQUE REFERENCES accounts(id), created_at TEXT NOT NULL)");
    QSqlQuery config(db);
    config.prepare("INSERT INTO app_config(id,database_uuid,principal_account_id,created_at) VALUES(1,?,NULL,?)");
    config.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
    config.addBindValue(LocalDatabase::nowUtc());
    run(config);
    sql(db, "CREATE TABLE events_v3(id INTEGER PRIMARY KEY, entity_type TEXT NOT NULL CHECK(entity_type IN "
            "('notebook','account','recovery','system')), "
            "entity_id INTEGER, notebook_id INTEGER REFERENCES notebooks(id), action TEXT NOT NULL, details TEXT NOT "
            "NULL, happened_at TEXT NOT NULL, "
            "actor_account_id INTEGER REFERENCES accounts(id), actor_username TEXT, changes TEXT, "
            "CHECK(entity_type<>'notebook' OR notebook_id IS NOT NULL), CHECK((actor_account_id IS NULL) = "
            "(actor_username IS NULL)))");
    sql(db, "INSERT INTO events_v3(id,entity_type,entity_id,notebook_id,action,details,happened_at) "
            "SELECT id,'notebook',notebook_id,notebook_id,action,details,happened_at FROM events ORDER BY id");
    sql(db, "DROP TABLE events");
    sql(db, "ALTER TABLE events_v3 RENAME TO events");
    sql(db, "CREATE TRIGGER events_append_only_update BEFORE UPDATE ON events BEGIN SELECT RAISE(ABORT,'Eventos do "
            "histórico não podem ser alterados.'); END");
    sql(db, "CREATE TRIGGER events_append_only_delete BEFORE DELETE ON events BEGIN SELECT RAISE(ABORT,'Eventos do "
            "histórico não podem ser excluídos.'); END");
    sql(db, "CREATE TRIGGER accounts_no_delete BEFORE DELETE ON accounts BEGIN SELECT RAISE(ABORT,'Contas não podem "
            "ser excluídas.'); END");
    sql(db, "CREATE TRIGGER principal_immutable BEFORE UPDATE OF principal_account_id ON app_config "
            "WHEN OLD.principal_account_id IS NOT NULL AND NEW.principal_account_id IS NOT OLD.principal_account_id "
            "BEGIN SELECT RAISE(ABORT,'A conta principal não pode ser substituída.'); END");
    sql(db, "CREATE TRIGGER principal_protected BEFORE UPDATE OF active, role ON accounts "
            "WHEN OLD.role='principal' AND (NEW.active IS NOT 1 OR NEW.role IS NOT 'principal') "
            "BEGIN SELECT RAISE(ABORT,'A conta principal não pode ser inativada nem mudar de perfil.'); END");
}
// O segredo TOTP fica no cofre do SO; aqui só a chave do cofre, a geração e o último contador aceito.
void migrateToV4(QSqlDatabase &db) {
    sql(db, "CREATE TABLE recovery_config(id INTEGER PRIMARY KEY CHECK(id=1), generation INTEGER NOT NULL DEFAULT 0 "
            "CHECK(generation>=0), "
            "vault_key TEXT, totp_last_counter INTEGER, enrolled_at TEXT, failed_attempts INTEGER NOT NULL DEFAULT 0 "
            "CHECK(failed_attempts>=0), "
            "cooldown_until TEXT, CHECK((vault_key IS NULL) = (enrolled_at IS NULL)))");
    sql(db, "INSERT INTO recovery_config(id,generation) VALUES(1,0)");
    sql(db,
        "CREATE TABLE recovery_codes(id INTEGER PRIMARY KEY, generation INTEGER NOT NULL, code_verifier TEXT NOT NULL, "
        "created_at TEXT NOT NULL, used_at TEXT, UNIQUE(generation, code_verifier))");
    sql(db, "CREATE TRIGGER recovery_codes_no_delete BEFORE DELETE ON recovery_codes "
            "BEGIN SELECT RAISE(ABORT,'Códigos de recuperação não podem ser excluídos.'); END");
    sql(db, "CREATE TRIGGER recovery_codes_used_once BEFORE UPDATE ON recovery_codes "
            "WHEN OLD.used_at IS NOT NULL OR NEW.used_at IS NULL OR NEW.id IS NOT OLD.id OR NEW.generation IS NOT "
            "OLD.generation "
            "OR NEW.code_verifier IS NOT OLD.code_verifier OR NEW.created_at IS NOT OLD.created_at "
            "BEGIN SELECT RAISE(ABORT,'Código de recuperação já utilizado.'); END");
}
// Notebook inativo fica congelado; a única alteração aceita é a reativação (active 0→1) sem mudar nenhum outro campo.
void createInactiveNotebookTrigger(QSqlDatabase &db) {
    sql(db, "DROP TRIGGER IF EXISTS notebooks_inactive_frozen");
    sql(db,
        "CREATE TRIGGER notebooks_inactive_frozen BEFORE UPDATE ON notebooks WHEN OLD.active=0 AND NOT (NEW.active=1 "
        "AND NEW.id IS OLD.id AND NEW.asset_tag IS OLD.asset_tag AND NEW.name IS OLD.name AND NEW.status IS OLD.status "
        "AND NEW.location IS OLD.location AND NEW.location_id IS OLD.location_id) "
        "BEGIN SELECT RAISE(ABORT,'Notebook inativo não pode ser movimentado.'); END");
}
// Devolve os eventos "Local importado"; o chamador grava depois do último passo (o INSERT usa as colunas da versão
// final).
QVector<EventRecord> migrateToV5(QSqlDatabase &db) {
    sql(db, "CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 120), "
            "name_key TEXT NOT NULL, "
            "role TEXT NOT NULL CHECK(role IN ('professor','assistant')), subject TEXT CHECK(subject IS NULL OR "
            "(role='professor' AND length(subject) BETWEEN 1 AND 120)), "
            "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)), created_at TEXT NOT NULL)");
    sql(db, "CREATE INDEX people_by_name_key ON people(name_key)");
    sql(db, "CREATE TABLE locations(id INTEGER PRIMARY KEY, name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 120), "
            "name_key TEXT NOT NULL, created_at TEXT NOT NULL, deleted_at TEXT)");
    sql(db, "CREATE UNIQUE INDEX locations_active_name_key ON locations(name_key) WHERE deleted_at IS NULL");
    sql(db, "ALTER TABLE notebooks ADD COLUMN location_id INTEGER REFERENCES locations(id)");
    sql(db, "ALTER TABLE notebooks ADD COLUMN active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1))");
    for (const auto &column : {"borrower_person_id INTEGER REFERENCES people(id)", "borrower_subject TEXT",
                               "checkout_location_id INTEGER REFERENCES locations(id)", "checkout_location TEXT",
                               "returned_by_person_id INTEGER REFERENCES people(id)", "returned_by_subject TEXT",
                               "return_location_id INTEGER REFERENCES locations(id)", "return_location TEXT"})
        sql(db, "ALTER TABLE loans ADD COLUMN " + QString::fromLatin1(column));

    // Preserve every existing explicit index/trigger, including installations with additional indexes.
    QStringList eventObjects;
    {
        QSqlQuery q(db);
        if (!q.exec("SELECT sql FROM sqlite_master WHERE tbl_name='events' AND type IN ('index','trigger') AND sql IS "
                    "NOT NULL ORDER BY type,name"))
            fail(q.lastError().text());
        while (q.next())
            eventObjects << q.value(0).toString();
    }
    sql(db, "CREATE TABLE events_v5(id INTEGER PRIMARY KEY, entity_type TEXT NOT NULL CHECK(entity_type IN "
            "('notebook','account','recovery','system','person','location')), "
            "entity_id INTEGER, notebook_id INTEGER REFERENCES notebooks(id), action TEXT NOT NULL, details TEXT NOT "
            "NULL, happened_at TEXT NOT NULL, "
            "actor_account_id INTEGER REFERENCES accounts(id), actor_username TEXT, changes TEXT, notebook_asset_tag "
            "TEXT, notebook_name TEXT, "
            "CHECK(entity_type<>'notebook' OR notebook_id IS NOT NULL), CHECK((actor_account_id IS NULL) = "
            "(actor_username IS NULL)), "
            "CHECK(entity_type NOT IN ('person','location') OR entity_id IS NOT NULL))");
    sql(db, "INSERT INTO "
            "events_v5(id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_username,"
            "changes) "
            "SELECT "
            "id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_username,changes "
            "FROM events ORDER BY id");
    sql(db, "DROP TABLE events");
    sql(db, "ALTER TABLE events_v5 RENAME TO events");
    for (const auto &statement : eventObjects)
        sql(db, statement);

    for (const auto &table : {"people", "locations", "notebooks", "loans"}) {
        const auto name = QString::fromLatin1(table);
        sql(db, "CREATE TRIGGER " + name + "_no_delete BEFORE DELETE ON " + name +
                    " BEGIN SELECT RAISE(ABORT,'Registros históricos não podem ser excluídos.'); END");
    }
    sql(db, "CREATE TRIGGER people_inactive_frozen BEFORE UPDATE ON people WHEN OLD.active=0 BEGIN SELECT "
            "RAISE(ABORT,'Pessoa inativa não pode ser alterada.'); END");
    sql(db, "CREATE TRIGGER locations_deleted_frozen BEFORE UPDATE ON locations WHEN OLD.deleted_at IS NOT NULL BEGIN "
            "SELECT RAISE(ABORT,'Local excluído não pode ser alterado.'); END");
    createInactiveNotebookTrigger(db);
    sql(db,
        "CREATE TRIGGER loans_history_immutable BEFORE UPDATE ON loans WHEN OLD.returned_at IS NOT NULL "
        "OR NEW.notebook_id IS NOT OLD.notebook_id OR NEW.borrower IS NOT OLD.borrower OR NEW.borrower_role IS NOT "
        "OLD.borrower_role "
        "OR NEW.borrower_person_id IS NOT OLD.borrower_person_id OR NEW.borrower_subject IS NOT OLD.borrower_subject "
        "OR NEW.checked_out_at IS NOT OLD.checked_out_at OR NEW.checkout_location_id IS NOT OLD.checkout_location_id "
        "OR NEW.checkout_location IS NOT OLD.checkout_location BEGIN SELECT RAISE(ABORT,'Empréstimos anteriores não "
        "podem ser alterados.'); END");

    struct ImportedLocation {
        qint64 id;
        QString name;
        int count = 0;
    };
    QVector<ImportedLocation> imported;
    QHash<QString, int> byKey;
    QVector<QPair<qint64, QString>> notebooks;
    {
        QSqlQuery q(db);
        if (!q.exec("SELECT id,location FROM notebooks ORDER BY id"))
            fail(q.lastError().text());
        while (q.next())
            notebooks.append({q.value(0).toLongLong(), q.value(1).toString()});
    }
    for (const auto &notebook : notebooks) {
        const auto key = LocalDatabase::catalogKey(notebook.second);
        if (key.isEmpty())
            continue;
        if (!byKey.contains(key)) {
            const auto label = notebook.second.trimmed();
            QSqlQuery insert(db);
            insert.prepare("INSERT INTO locations(name,name_key,created_at) VALUES(?,?,?)");
            insert.addBindValue(label);
            insert.addBindValue(key);
            insert.addBindValue(LocalDatabase::nowUtc());
            run(insert);
            byKey.insert(key, imported.size());
            imported.append({insert.lastInsertId().toLongLong(), label, 0});
        }
        auto &location = imported[byKey.value(key)];
        QSqlQuery update(db);
        update.prepare("UPDATE notebooks SET location_id=? WHERE id=?");
        update.addBindValue(location.id);
        update.addBindValue(notebook.first);
        run(update);
        ++location.count;
    }
    QVector<EventRecord> events;
    for (const auto &location : imported)
        events.append({"location", location.id, std::nullopt, "Local importado",
                       "Local importado na atualização do banco: " + location.name +
                           ". Notebooks vinculados: " + QString::number(location.count) + ".",
                       std::nullopt, LocalDatabase::change("name", QJsonValue::Null, location.name),
                       LocalDatabase::nowUtc()});
    return events;
}
// v6 (03-01-D1 option-a): aditivo, sem backfill; linhas legadas ficam com as colunas novas NULL.
void migrateToV6(QSqlDatabase &db) {
    sql(db, "ALTER TABLE loans ADD COLUMN origin_location_id INTEGER REFERENCES locations(id)");
    sql(db, "ALTER TABLE loans ADD COLUMN origin_location TEXT");
    sql(db, "ALTER TABLE events ADD COLUMN loan_id INTEGER REFERENCES loans(id)");
    sql(db, "ALTER TABLE events ADD COLUMN location_id INTEGER REFERENCES locations(id)");
    sql(db, "ALTER TABLE events ADD COLUMN from_location_id INTEGER REFERENCES locations(id)");
    sql(db, "DROP TRIGGER loans_history_immutable");
    sql(db,
        "CREATE TRIGGER loans_history_immutable BEFORE UPDATE ON loans WHEN OLD.returned_at IS NOT NULL "
        "OR NEW.notebook_id IS NOT OLD.notebook_id OR NEW.borrower IS NOT OLD.borrower OR NEW.borrower_role IS NOT "
        "OLD.borrower_role "
        "OR NEW.borrower_person_id IS NOT OLD.borrower_person_id OR NEW.borrower_subject IS NOT OLD.borrower_subject "
        "OR NEW.checked_out_at IS NOT OLD.checked_out_at OR NEW.checkout_location_id IS NOT OLD.checkout_location_id "
        "OR NEW.checkout_location IS NOT OLD.checkout_location OR NEW.origin_location_id IS NOT OLD.origin_location_id "
        "OR NEW.origin_location IS NOT OLD.origin_location BEGIN SELECT RAISE(ABORT,'Empréstimos anteriores não podem "
        "ser alterados.'); END");
}
void migrateToV7(QSqlDatabase &db) {
    sql(db, "CREATE INDEX events_by_happened_at ON events(happened_at,id)");
    sql(db, "CREATE INDEX events_by_notebook_time ON events(notebook_id,happened_at)");
    sql(db, "CREATE INDEX events_by_loan ON events(loan_id)");
    sql(db, "CREATE INDEX events_by_location ON events(location_id)");
    sql(db, "CREATE INDEX events_by_from_location ON events(from_location_id)");
    sql(db, "CREATE INDEX loans_by_checkout ON loans(notebook_id,checked_out_at)");
    sql(db, "CREATE INDEX loans_by_return ON loans(notebook_id,returned_at)");
    sql(db, "CREATE INDEX loans_by_borrower_person ON loans(borrower_person_id)");
    sql(db, "CREATE INDEX loans_by_returner_person ON loans(returned_by_person_id)");
}
} // namespace

LocalDatabase::LocalDatabase(const QString &path) : name(QUuid::createUuid().toString()) {
    db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(path);
    try {
        if (!db.open())
            fail(db.lastError().text());
        sql(db, "PRAGMA foreign_keys=ON");
        sql(db, "PRAGMA busy_timeout=5000");
        const int found = schemaVersion(db);
        if (found > CurrentSchemaVersion)
            fail("Banco criado por uma versão mais recente do aplicativo.");
        sql(db, "PRAGMA journal_mode=WAL");
        if (found >= 1 && found < CurrentSchemaVersion)
            backupPath = backupBeforeUpgrade(db, path, CurrentSchemaVersion);
        transaction([&] {
            const int schema = schemaVersion(db);
            if (schema > CurrentSchemaVersion)
                fail("Banco criado por uma versão mais recente do aplicativo.");
            if (schema >= 5) {
                // Bancos v5 criados antes da reativação ainda têm o trigger que recusava qualquer UPDATE em inativo.
                QSqlQuery q(db);
                if (!q.exec("SELECT sql FROM sqlite_master WHERE type='trigger' AND name='notebooks_inactive_frozen'"))
                    fail(q.lastError().text());
                if (!q.next() || !q.value(0).toString().contains("NEW.active=1")) {
                    q.finish();
                    createInactiveNotebookTrigger(db);
                }
            }
            if (schema == CurrentSchemaVersion)
                return;
            if (schema < 2)
                migrateToV2(db);
            if (schema < 3)
                migrateToV3(db, schema);
            if (schema < 4)
                migrateToV4(db);
            const auto imported = schema < 5 ? migrateToV5(db) : QVector<EventRecord>{};
            if (schema < 6)
                migrateToV6(db);
            migrateToV7(db);
            for (const auto &event : imported)
                appendEvent(event);
            // Um único evento por abertura, da versão lida até a atual, mesmo atravessando passos intermediários.
            if (schema >= 1) {
                EventRecord event{"system",
                                  std::nullopt,
                                  std::nullopt,
                                  "Atualização do banco",
                                  "Esquema atualizado da versão " + QString::number(schema) + " para a versão " +
                                      QString::number(CurrentSchemaVersion) + ".",
                                  std::nullopt,
                                  change("schema_version", schema, CurrentSchemaVersion),
                                  nowUtc()};
                appendEvent(event);
            }
            static_assert(CurrentSchemaVersion == 7, "Atualize a cadeia de migrações e o user_version gravado.");
            sql(db, "PRAGMA user_version=7");
        });
    } catch (...) {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        throw;
    }
}
LocalDatabase::~LocalDatabase() {
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}
QSqlDatabase &LocalDatabase::connection() {
    return db;
}
void LocalDatabase::transaction(const std::function<void()> &operation) {
    if (active)
        fail("Já existe uma transação em andamento.");
    sql(db, "BEGIN IMMEDIATE");
    active = true;
    try {
        operation();
        if (!db.commit())
            fail(db.lastError().text());
        active = false;
    } catch (...) {
        db.rollback();
        active = false;
        throw;
    }
}
bool LocalDatabase::inTransaction() const {
    return active;
}
qint64 LocalDatabase::appendEvent(const EventRecord &event) {
    if (!active)
        fail("Eventos só podem ser gravados dentro de uma transação.");
    validateChanges(event.changes);
    if (event.actor && event.actor->username.trimmed().isEmpty())
        fail("Conta operadora inválida no histórico.");
    QVariant assetTag, notebookName;
    if (event.notebookId) {
        QSqlQuery notebook(db);
        notebook.prepare("SELECT asset_tag,name FROM notebooks WHERE id=?");
        notebook.addBindValue(*event.notebookId);
        run(notebook);
        if (!notebook.next())
            fail("Notebook não encontrado ao registrar o histórico.");
        assetTag = notebook.value(0);
        notebookName = notebook.value(1);
    }
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO "
        "events(entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_username,changes,"
        "notebook_asset_tag,notebook_name,loan_id,location_id,from_location_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    q.addBindValue(event.entityType);
    q.addBindValue(event.entityId ? QVariant(*event.entityId) : QVariant());
    q.addBindValue(event.notebookId ? QVariant(*event.notebookId) : QVariant());
    q.addBindValue(event.action);
    q.addBindValue(event.details);
    q.addBindValue(event.happenedAt.isEmpty() ? nowUtc() : event.happenedAt);
    q.addBindValue(event.actor ? QVariant(event.actor->accountId) : QVariant());
    q.addBindValue(event.actor ? QVariant(event.actor->username) : QVariant());
    q.addBindValue(event.changes.isEmpty()
                       ? QVariant()
                       : QVariant(QString::fromUtf8(QJsonDocument(event.changes).toJson(QJsonDocument::Compact))));
    q.addBindValue(assetTag);
    q.addBindValue(notebookName);
    for (const auto &link : {event.loanId, event.locationId, event.fromLocationId})
        q.addBindValue(link ? QVariant::fromValue(*link) : QVariant(QMetaType::fromType<qint64>()));
    run(q);
    return q.lastInsertId().toLongLong();
}
QString LocalDatabase::databaseUuid() const {
    QSqlQuery q(db);
    if (!q.exec("SELECT database_uuid FROM app_config WHERE id=1") || !q.next())
        fail("Não foi possível ler a identificação do banco.");
    return q.value(0).toString();
}
QString LocalDatabase::lastBackupPath() const {
    return backupPath;
}
void LocalDatabase::applyApplicationIdentity() {
    QCoreApplication::setOrganizationName("GestaoEquipamentos");
    QCoreApplication::setApplicationName("GestaoEquipamentos");
}
QString LocalDatabase::profilePath(DataProfile profile) {
    if (QCoreApplication::organizationName() != "GestaoEquipamentos" ||
        QCoreApplication::applicationName() != "GestaoEquipamentos")
        fail("Identidade do aplicativo não configurada antes de resolver o banco.");
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory))
        fail("Não foi possível criar o diretório de dados.");
    return directory + (profile == DataProfile::Demo ? "/demo.sqlite" : "/notebooks.sqlite");
}
QString LocalDatabase::nowUtc() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QString LocalDatabase::catalogKey(const QString &name) {
    return name.trimmed().toCaseFolded();
}
QJsonObject LocalDatabase::change(const QString &field, const QJsonValue &from, const QJsonValue &to) {
    return QJsonObject{{field, QJsonObject{{"de", from}, {"para", to}}}};
}
const QStringList &LocalDatabase::changeFieldAllowlist() {
    static const QStringList fields{"username",
                                    "role",
                                    "active",
                                    "must_change_password",
                                    "initial_password_mode",
                                    "admin_slot",
                                    "asset_tag",
                                    "name",
                                    "location",
                                    "status",
                                    "method",
                                    "failed_attempts",
                                    "cooldown_until",
                                    "generation",
                                    "totp_enrolled",
                                    "schema_version",
                                    "person_role",
                                    "subject",
                                    "notebook_active",
                                    "location_deleted",
                                    "location_id",
                                    "maintenance_note"};
    return fields;
}

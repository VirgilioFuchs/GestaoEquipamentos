#pragma once
#include "application/AccountService.h"
#include "application/AuthService.h"
#include "application/RecoveryService.h"
#include "application/LocationService.h"
#include "application/PeopleService.h"
#include "security/SecretVault.h"
#include "storage/NotebookStore.h"
#include <QHash>
#include <QSqlQuery>
#include <QSqlError>
#include <QUrl>
#include <QUrlQuery>

class FakeSessionClock : public SessionClock {
public:
    QDateTime nowUtc() const override { return now; }
    qint64 monotonicMs() const override { return elapsedMs; }
    void advance(qint64 seconds) { now = now.addSecs(seconds); elapsedMs += seconds * 1000; }
    // Cada relógio isolado: o UTC pode ser ajustado pelo usuário (inclusive para trás) sem mover o monotônico.
    void advanceWall(qint64 seconds) { now = now.addSecs(seconds); }
    void advanceMonotonic(qint64 ms) { elapsedMs += ms; }
private:
    QDateTime now = QDateTime::fromString("2026-01-05T12:00:00.000Z", Qt::ISODateWithMs);
    qint64 elapsedMs = 0;
};

// Cofre em memória com a mesma semântica do cofre do SO: ler entrada inexistente falha.
class MemorySecretVault : public SecretVault {
public:
    void write(const QString &key, const QByteArray &secret) override { entries.insert(key, secret); }
    QByteArray read(const QString &key) override {
        const auto it = entries.constFind(key);
        if (it == entries.constEnd()) throw VaultError("Entrada não encontrada no cofre.");
        return *it;
    }
    void remove(const QString &key) override { entries.remove(key); }
    QHash<QString, QByteArray> entries;
};

// Corrupt: a releitura devolve bytes diferentes dos gravados.
class FailingSecretVault : public MemorySecretVault {
public:
    enum class Stage { None, Write, Read, Corrupt };
    void write(const QString &key, const QByteArray &secret) override {
        if (failOn == Stage::Write) throw VaultError("Falha simulada na gravação.");
        MemorySecretVault::write(key, secret);
    }
    QByteArray read(const QString &key) override {
        if (failOn == Stage::Read) throw VaultError("Falha simulada na leitura.");
        QByteArray value = MemorySecretVault::read(key);
        if (failOn == Stage::Corrupt) value[0] = char(value[0] ^ 0x01);
        return value;
    }
    Stage failOn = Stage::None;
};

inline QByteArray fromBase32(const QString &text) {
    static const QString alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QByteArray out; quint32 buffer = 0; int bits = 0;
    for (const QChar c : text) {
        const int value = alphabet.indexOf(c);
        if (value < 0) return {};
        buffer = (buffer << 5) | quint32(value); bits += 5;
        if (bits >= 8) { out.append(char((buffer >> (bits - 8)) & 0xff)); bits -= 8; }
    }
    return out;
}
// O teste lê o segredo pelo mesmo canal que o celular: a URI otpauth mostrada à principal.
inline QByteArray secretOf(const EnrollmentDraft &draft) {
    return fromBase32(QUrlQuery(QUrl(draft.uri())).queryItemValue("secret"));
}

inline const QString TestPrincipal = QStringLiteral("principal");
inline const QByteArray TestPassword = QByteArrayLiteral("senha-de-teste-segura");

// Banco SQLite real com a principal de teste configurada e conectada (signIn = false só abre o banco).
// Sem cofre informado, a recuperação usa memoryVault.
struct TestContext {
    explicit TestContext(const QString &path, bool signIn = true, SecretVault *vault = nullptr)
        : database(path), auth(database, hasher, clock), store(database, auth), accounts(database, auth),
          recovery(database, auth, vault ? *vault : memoryVault, clock), locations(database, auth), people(database, auth) {
        if (!signIn) return;
        if (!auth.hasPrincipal()) auth.bootstrapPrincipal(TestPrincipal, auth.preparePassword(TestPassword));
        session = auth.login(TestPrincipal, TestPassword);
    }
    // A principal conectada cria a conta comum pelo AccountService; devolve a sessão de login dessa conta.
    Session createCommonAndLogin(const QString &username, const QByteArray &password, InitialPasswordMode mode = InitialPasswordMode::Temporary) {
        accounts.createCommonAccount(session, username, auth.preparePassword(password), mode);
        return auth.login(username, password);
    }
    FakeSessionClock clock;
    PasswordHasher hasher{PasswordHasher::testing()};
    LocalDatabase database;
    AuthService auth;
    NotebookStore store;
    AccountService accounts;
    MemorySecretVault memoryVault;
    RecoveryService recovery;
    LocationService locations;
    PeopleService people;
    Session session;
};

// DDL congelada dos esquemas legados; fixtures não dependem da versão atual.
namespace LegacyFixtures {
inline bool execAll(const QString &path, const QStringList &statements) {
    bool ok = true;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "fixture"); db.setDatabaseName(path);
        ok = db.open(); QSqlQuery q(db);
        for (const auto &statement : statements) if (ok && !q.exec(statement)) { qWarning() << statement << q.lastError().text(); ok = false; }
    }
    QSqlDatabase::removeDatabase("fixture");
    return ok;
}
inline bool createV2(const QString &path) {
    return execAll(path, {
        "CREATE TABLE notebooks(id INTEGER PRIMARY KEY, asset_tag TEXT NOT NULL COLLATE NOCASE UNIQUE, name TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'available' CHECK(status IN ('available','loaned','maintenance')), location TEXT)",
        "CREATE TABLE loans(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES notebooks(id), borrower TEXT NOT NULL, checked_out_at TEXT NOT NULL, returned_at TEXT, borrower_role TEXT CHECK(borrower_role IN ('professor','assistant')), returned_by TEXT, returned_by_role TEXT CHECK(returned_by_role IN ('professor','assistant')))",
        "CREATE UNIQUE INDEX one_open_loan ON loans(notebook_id) WHERE returned_at IS NULL",
        "CREATE INDEX loans_by_notebook ON loans(notebook_id,id)",
        "CREATE TABLE events(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES notebooks(id), action TEXT NOT NULL, details TEXT NOT NULL, happened_at TEXT NOT NULL)",
        "INSERT INTO notebooks VALUES(1,'NB-7','Dell Latitude','loaned','Sala 12')",
        "INSERT INTO loans VALUES(1,1,'Ana','2026-01-01T09:00:00.000Z','2026-01-01T10:00:00.000Z','professor','Carlos','assistant'),(2,1,'Bia','2026-01-02T08:30:00.123Z',NULL,'assistant',NULL,NULL)",
        "INSERT INTO events VALUES(1,1,'Retirada','Retirado por: Ana (Professor). Local: Sala 12','2026-01-01T09:00:00.000Z'),(2,1,'Devolução','Devolvido por: Carlos (Auxiliar). Local: Armário 1','2026-01-01T10:00:00.000Z')",
        "PRAGMA user_version=2"});
}
inline bool createV3(const QString &path) {
    if (!createV2(path)) return false;
    if (!execAll(path, {
        "CREATE TABLE accounts(id INTEGER PRIMARY KEY, username TEXT NOT NULL COLLATE NOCASE UNIQUE, password_verifier TEXT NOT NULL, role TEXT NOT NULL CHECK(role IN ('principal','admin','common')), active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)), admin_slot INTEGER UNIQUE CHECK(admin_slot BETWEEN 1 AND 3), must_change_password INTEGER NOT NULL DEFAULT 0 CHECK(must_change_password IN (0,1)), credential_version INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, created_by INTEGER REFERENCES accounts(id), CHECK((role<>'common' AND active=1) = (admin_slot IS NOT NULL)), CHECK(role<>'principal' OR admin_slot=1))",
        "CREATE TABLE app_config(id INTEGER PRIMARY KEY CHECK(id=1), database_uuid TEXT NOT NULL, principal_account_id INTEGER UNIQUE REFERENCES accounts(id), created_at TEXT NOT NULL)",
        "CREATE TABLE events_v3(id INTEGER PRIMARY KEY, entity_type TEXT NOT NULL CHECK(entity_type IN ('notebook','account','recovery','system')), entity_id INTEGER, notebook_id INTEGER REFERENCES notebooks(id), action TEXT NOT NULL, details TEXT NOT NULL, happened_at TEXT NOT NULL, actor_account_id INTEGER REFERENCES accounts(id), actor_username TEXT, changes TEXT, CHECK(entity_type<>'notebook' OR notebook_id IS NOT NULL), CHECK((actor_account_id IS NULL) = (actor_username IS NULL)))",
        "INSERT INTO events_v3(id,entity_type,entity_id,notebook_id,action,details,happened_at) SELECT id,'notebook',notebook_id,notebook_id,action,details,happened_at FROM events ORDER BY id",
        "DROP TABLE events",
        "ALTER TABLE events_v3 RENAME TO events",
        "CREATE TRIGGER events_append_only_update BEFORE UPDATE ON events BEGIN SELECT RAISE(ABORT,'Eventos do histórico não podem ser alterados.'); END",
        "CREATE TRIGGER events_append_only_delete BEFORE DELETE ON events BEGIN SELECT RAISE(ABORT,'Eventos do histórico não podem ser excluídos.'); END",
        "CREATE TRIGGER accounts_no_delete BEFORE DELETE ON accounts BEGIN SELECT RAISE(ABORT,'Contas não podem ser excluídas.'); END",
        "CREATE TRIGGER principal_immutable BEFORE UPDATE OF principal_account_id ON app_config WHEN OLD.principal_account_id IS NOT NULL AND NEW.principal_account_id IS NOT OLD.principal_account_id BEGIN SELECT RAISE(ABORT,'A conta principal não pode ser substituída.'); END",
        "CREATE TRIGGER principal_protected BEFORE UPDATE OF active, role ON accounts WHEN OLD.role='principal' AND (NEW.active IS NOT 1 OR NEW.role IS NOT 'principal') BEGIN SELECT RAISE(ABORT,'A conta principal não pode ser inativada nem mudar de perfil.'); END",
        "INSERT INTO app_config VALUES(1,'fixture-v3-uuid',NULL,'2026-01-01T00:00:00.000Z')",
        "UPDATE notebooks SET status='available'",
        "UPDATE loans SET returned_at='2026-01-02T10:00:00.000Z',returned_by='Carlos',returned_by_role='assistant' WHERE id=2",
        "PRAGMA user_version=3"})) return false;
    bool ok;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "fixture-account"); db.setDatabaseName(path);
        ok = db.open(); QSqlQuery q(db);
        q.prepare("INSERT INTO accounts(id,username,password_verifier,role,admin_slot,created_at) VALUES(1,'principal',?,'principal',1,'2026-01-01T00:00:00.000Z')");
        q.addBindValue(PasswordHasher(PasswordHasher::testing()).hash(TestPassword));
        ok = ok && q.exec();
    }
    QSqlDatabase::removeDatabase("fixture-account");
    return ok && execAll(path, {
        "UPDATE app_config SET principal_account_id=1",
        R"(INSERT INTO events(entity_type,entity_id,action,details,happened_at,actor_account_id,actor_username,changes) VALUES('account',1,'Conta principal criada','Principal','2026-01-02T11:00:00.000Z',1,'principal','{"username":{"de":null,"para":"principal"}}'))"});
}
inline bool createV4(const QString &path) {
    if (!createV3(path)) return false;
    return execAll(path, {
        "CREATE TABLE recovery_config(id INTEGER PRIMARY KEY CHECK(id=1), generation INTEGER NOT NULL DEFAULT 0 CHECK(generation>=0), vault_key TEXT, totp_last_counter INTEGER, enrolled_at TEXT, failed_attempts INTEGER NOT NULL DEFAULT 0 CHECK(failed_attempts>=0), cooldown_until TEXT, CHECK((vault_key IS NULL) = (enrolled_at IS NULL)))",
        "INSERT INTO recovery_config(id,generation) VALUES(1,0)",
        "CREATE TABLE recovery_codes(id INTEGER PRIMARY KEY, generation INTEGER NOT NULL, code_verifier TEXT NOT NULL, created_at TEXT NOT NULL, used_at TEXT, UNIQUE(generation, code_verifier))",
        "CREATE TRIGGER recovery_codes_no_delete BEFORE DELETE ON recovery_codes BEGIN SELECT RAISE(ABORT,'Códigos de recuperação não podem ser excluídos.'); END",
        "CREATE TRIGGER recovery_codes_used_once BEFORE UPDATE ON recovery_codes WHEN OLD.used_at IS NOT NULL OR NEW.used_at IS NULL OR NEW.id IS NOT OLD.id OR NEW.generation IS NOT OLD.generation OR NEW.code_verifier IS NOT OLD.code_verifier OR NEW.created_at IS NOT OLD.created_at BEGIN SELECT RAISE(ABORT,'Código de recuperação já utilizado.'); END",
        "INSERT INTO notebooks(id,asset_tag,name,status,location) VALUES(2,'NB-8','HP','available','sala 12 ')",
        "PRAGMA user_version=4"});
}
// Literal da DDL de migrateToV5 (fase 2) sobre o fixture v4: pessoa, local, loan aberto com snapshot e loans encerrados legados.
inline bool createV5(const QString &path) {
    if (!createV4(path)) return false;
    return execAll(path, {
        "CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 120), name_key TEXT NOT NULL, role TEXT NOT NULL CHECK(role IN ('professor','assistant')), subject TEXT CHECK(subject IS NULL OR (role='professor' AND length(subject) BETWEEN 1 AND 120)), active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)), created_at TEXT NOT NULL)",
        "CREATE INDEX people_by_name_key ON people(name_key)",
        "CREATE TABLE locations(id INTEGER PRIMARY KEY, name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 120), name_key TEXT NOT NULL, created_at TEXT NOT NULL, deleted_at TEXT)",
        "CREATE UNIQUE INDEX locations_active_name_key ON locations(name_key) WHERE deleted_at IS NULL",
        "ALTER TABLE notebooks ADD COLUMN location_id INTEGER REFERENCES locations(id)",
        "ALTER TABLE notebooks ADD COLUMN active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1))",
        "ALTER TABLE loans ADD COLUMN borrower_person_id INTEGER REFERENCES people(id)",
        "ALTER TABLE loans ADD COLUMN borrower_subject TEXT",
        "ALTER TABLE loans ADD COLUMN checkout_location_id INTEGER REFERENCES locations(id)",
        "ALTER TABLE loans ADD COLUMN checkout_location TEXT",
        "ALTER TABLE loans ADD COLUMN returned_by_person_id INTEGER REFERENCES people(id)",
        "ALTER TABLE loans ADD COLUMN returned_by_subject TEXT",
        "ALTER TABLE loans ADD COLUMN return_location_id INTEGER REFERENCES locations(id)",
        "ALTER TABLE loans ADD COLUMN return_location TEXT",
        "CREATE TABLE events_v5(id INTEGER PRIMARY KEY, entity_type TEXT NOT NULL CHECK(entity_type IN ('notebook','account','recovery','system','person','location')), entity_id INTEGER, notebook_id INTEGER REFERENCES notebooks(id), action TEXT NOT NULL, details TEXT NOT NULL, happened_at TEXT NOT NULL, actor_account_id INTEGER REFERENCES accounts(id), actor_username TEXT, changes TEXT, notebook_asset_tag TEXT, notebook_name TEXT, CHECK(entity_type<>'notebook' OR notebook_id IS NOT NULL), CHECK((actor_account_id IS NULL) = (actor_username IS NULL)), CHECK(entity_type NOT IN ('person','location') OR entity_id IS NOT NULL))",
        "INSERT INTO events_v5(id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_username,changes) SELECT id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_username,changes FROM events ORDER BY id",
        "DROP TABLE events",
        "ALTER TABLE events_v5 RENAME TO events",
        "CREATE TRIGGER events_append_only_delete BEFORE DELETE ON events BEGIN SELECT RAISE(ABORT,'Eventos do histórico não podem ser excluídos.'); END",
        "CREATE TRIGGER events_append_only_update BEFORE UPDATE ON events BEGIN SELECT RAISE(ABORT,'Eventos do histórico não podem ser alterados.'); END",
        "CREATE TRIGGER people_no_delete BEFORE DELETE ON people BEGIN SELECT RAISE(ABORT,'Registros históricos não podem ser excluídos.'); END",
        "CREATE TRIGGER locations_no_delete BEFORE DELETE ON locations BEGIN SELECT RAISE(ABORT,'Registros históricos não podem ser excluídos.'); END",
        "CREATE TRIGGER notebooks_no_delete BEFORE DELETE ON notebooks BEGIN SELECT RAISE(ABORT,'Registros históricos não podem ser excluídos.'); END",
        "CREATE TRIGGER loans_no_delete BEFORE DELETE ON loans BEGIN SELECT RAISE(ABORT,'Registros históricos não podem ser excluídos.'); END",
        "CREATE TRIGGER people_inactive_frozen BEFORE UPDATE ON people WHEN OLD.active=0 BEGIN SELECT RAISE(ABORT,'Pessoa inativa não pode ser alterada.'); END",
        "CREATE TRIGGER locations_deleted_frozen BEFORE UPDATE ON locations WHEN OLD.deleted_at IS NOT NULL BEGIN SELECT RAISE(ABORT,'Local excluído não pode ser alterado.'); END",
        "CREATE TRIGGER notebooks_inactive_frozen BEFORE UPDATE ON notebooks WHEN OLD.active=0 AND NOT (NEW.active=1 AND NEW.id IS OLD.id AND NEW.asset_tag IS OLD.asset_tag AND NEW.name IS OLD.name AND NEW.status IS OLD.status AND NEW.location IS OLD.location AND NEW.location_id IS OLD.location_id) BEGIN SELECT RAISE(ABORT,'Notebook inativo não pode ser movimentado.'); END",
        "CREATE TRIGGER loans_history_immutable BEFORE UPDATE ON loans WHEN OLD.returned_at IS NOT NULL OR NEW.notebook_id IS NOT OLD.notebook_id OR NEW.borrower IS NOT OLD.borrower OR NEW.borrower_role IS NOT OLD.borrower_role OR NEW.borrower_person_id IS NOT OLD.borrower_person_id OR NEW.borrower_subject IS NOT OLD.borrower_subject OR NEW.checked_out_at IS NOT OLD.checked_out_at OR NEW.checkout_location_id IS NOT OLD.checkout_location_id OR NEW.checkout_location IS NOT OLD.checkout_location BEGIN SELECT RAISE(ABORT,'Empréstimos anteriores não podem ser alterados.'); END",
        "INSERT INTO locations(id,name,name_key,created_at) VALUES(1,'Sala 12','sala 12','2026-01-03T08:00:00.000Z')",
        "UPDATE notebooks SET location_id=1",
        "INSERT INTO people(id,name,name_key,role,subject,created_at) VALUES(1,'Ana Souza','ana souza','professor','Matemática','2026-01-03T08:00:00.000Z')",
        "UPDATE notebooks SET status='loaned' WHERE id=2",
        "INSERT INTO loans(id,notebook_id,borrower,checked_out_at,borrower_role,borrower_person_id,borrower_subject,checkout_location_id,checkout_location) VALUES(3,2,'Ana Souza','2026-01-04T09:00:00.000Z','professor',1,'Matemática',1,'Sala 12')",
        R"(INSERT INTO events(entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_username,changes,notebook_asset_tag,notebook_name) VALUES('notebook',2,2,'Retirada','Retirado por: Ana Souza (Professor · Matemática). Local: Sala 12','2026-01-04T09:00:00.000Z',1,'principal','{"status":{"de":"available","para":"loaned"}}','NB-8','HP'))",
        "PRAGMA user_version=5"});
}
} // namespace LegacyFixtures

// Catálogos criados pela principal do contexto (mesmas regras do serviço).
inline qint64 createLocation(TestContext &context, const QString &name) { return context.locations.create(context.session, name); }
inline qint64 createPerson(TestContext &context, const QString &name, ResponsibleRole role, const QString &subject = {}) {
    return context.people.create(context.session, PersonDraft{name, role, subject});
}

// 03-03: o serviço só aceita IDs. Testes que apenas montam dados usam estes atalhos por rótulo:
// local e pessoa são gravados direto no banco (sem evento, como o antigo texto livre) e o serviço recebe os IDs.
// Rótulo vazio vira id 0 (o serviço recusa); local excluído homônimo é devolvido para o serviço recusar.
namespace Fixture {
inline qint64 location(TestContext &c, const QString &label) {
    const auto text = label.trimmed();
    if (text.isEmpty()) return 0;
    QSqlQuery q(c.database.connection());
    q.prepare("SELECT id FROM locations WHERE name_key=? ORDER BY deleted_at IS NULL DESC,id DESC"); q.addBindValue(LocalDatabase::catalogKey(text));
    if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
    if (q.next()) return q.value(0).toLongLong();
    q.prepare("INSERT INTO locations(name,name_key,created_at) VALUES(?,?,?)");
    q.addBindValue(text); q.addBindValue(LocalDatabase::catalogKey(text)); q.addBindValue(LocalDatabase::nowUtc());
    if (!q.exec()) return 0; // rótulo inválido (ex.: acima de 120 caracteres): o serviço recusa o id 0
    return q.lastInsertId().toLongLong();
}
inline qint64 person(TestContext &c, const QString &name, ResponsibleRole role) {
    const auto text = name.trimmed();
    if (text.isEmpty() || (role != ResponsibleRole::Professor && role != ResponsibleRole::Assistant)) return 0;
    const QString roleKey = role == ResponsibleRole::Professor ? "professor" : "assistant";
    QSqlQuery q(c.database.connection());
    q.prepare("SELECT id FROM people WHERE name=? AND role=? AND subject IS NULL AND active=1 ORDER BY id"); q.addBindValue(text); q.addBindValue(roleKey);
    if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
    if (q.next()) return q.value(0).toLongLong();
    q.prepare("INSERT INTO people(name,name_key,role,created_at) VALUES(?,?,?,?)");
    q.addBindValue(text); q.addBindValue(LocalDatabase::catalogKey(text)); q.addBindValue(roleKey); q.addBindValue(LocalDatabase::nowUtc());
    if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
    return q.lastInsertId().toLongLong();
}
inline qint64 add(TestContext &c, const Session &s, const QString &tag, const QString &name, const QString &place) {
    return c.store.add(s, tag, name, location(c, place));
}
inline void checkout(TestContext &c, const Session &s, qint64 id, const QString &who, ResponsibleRole role, const QString &place) {
    c.store.checkout(s, id, person(c, who, role), location(c, place));
}
inline void checkin(TestContext &c, const Session &s, qint64 id, const QString &who, ResponsibleRole role, const QString &place) {
    c.store.checkin(s, id, person(c, who, role), location(c, place));
}
inline void updateLocation(TestContext &c, const Session &s, qint64 id, const QString &place) {
    c.store.updateLocation(s, id, location(c, place));
}
// Mesmo rótulo do local atual (chave de catálogo) mantém o local, como na edição da fase 2.
inline void update(TestContext &c, const Session &s, const Notebook &expected, const QString &tag, const QString &name, const QString &place) {
    const bool keep = LocalDatabase::catalogKey(place) == LocalDatabase::catalogKey(expected.location);
    c.store.update(s, expected, tag, name, keep ? std::nullopt : std::optional<qint64>(location(c, place)));
}
} // namespace Fixture

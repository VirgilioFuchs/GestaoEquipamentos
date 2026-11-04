#include "AccountService.h"
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

namespace {
void run(QSqlQuery &q) {
    if (!q.exec())
        throw std::runtime_error(q.lastError().text().toStdString());
}
QJsonObject merge(std::initializer_list<QJsonObject> parts) {
    QJsonObject all;
    for (const auto &part : parts)
        for (auto it = part.begin(); it != part.end(); ++it)
            all.insert(it.key(), it.value());
    return all;
}
QString modeValue(InitialPasswordMode mode) {
    return mode == InitialPasswordMode::Temporary ? QStringLiteral("temporária") : QStringLiteral("assistida");
}
const QString Duplicate = QStringLiteral("Já existe uma conta com esse nome de usuário.");

struct Target {
    QString username;
    int mustChange;
};
// Exige conta comum e ativa; principal e administradores ficam fora desta gestão.
Target activeCommonTarget(QSqlDatabase &db, qint64 accountId) {
    QSqlQuery q(db);
    q.prepare("SELECT username, role, active, must_change_password FROM accounts WHERE id=?");
    q.addBindValue(accountId);
    run(q);
    if (!q.next())
        throw AuthError("account_not_found", "Conta não encontrada. Atualize a lista e tente novamente.");
    if (q.value(1).toString() != QLatin1String("common"))
        throw AuthError("not_common_account", "Esta ação está disponível somente para contas comuns.");
    if (q.value(2).toInt() != 1)
        throw AuthError("inactive_account", "Esta conta já está inativa.");
    return {q.value(0).toString(), q.value(3).toInt()};
}

const QString AdminLimit = QStringLiteral(
    "Limite de três administradores ativos atingido. Inative outro administrador para liberar uma vaga.");

struct AdminTarget {
    QString username;
    int mustChange;
    int slot;
};
// Exige administrador ativo; a principal nunca é alvo destas operações.
AdminTarget activeAdministratorTarget(QSqlDatabase &db, qint64 accountId) {
    QSqlQuery q(db);
    q.prepare("SELECT username, role, active, must_change_password, admin_slot FROM accounts WHERE id=?");
    q.addBindValue(accountId);
    run(q);
    if (!q.next())
        throw AuthError("account_not_found", "Conta não encontrada. Atualize a lista e tente novamente.");
    if (q.value(1).toString() == QLatin1String("principal"))
        throw AuthError("principal_protected", "A conta principal não pode ser alterada por esta ação.");
    if (q.value(1).toString() != QLatin1String("admin"))
        throw AuthError("not_administrator", "Esta ação está disponível somente para administradores.");
    if (q.value(2).toInt() != 1)
        throw AuthError("inactive_account", "Esta conta já está inativa.");
    return {q.value(0).toString(), q.value(3).toInt(), q.value(4).toInt()};
}
} // namespace

AccountService::AccountService(LocalDatabase &database, AuthService &auth) : database(database), auth(auth) {}

QVector<AccountSummary> AccountService::commonAccounts(const Session &session, AccountFilter filter,
                                                       const QString &search) {
    QVector<AccountSummary> rows;
    const QString needle = search.trimmed().toLower();
    database.transaction([&] {
        auth.authorize(session, Permission::ManageCommonAccounts);
        QString statement = "SELECT id, username, role, active, must_change_password FROM accounts WHERE role='common'";
        if (filter == AccountFilter::Active)
            statement += " AND active=1";
        if (filter == AccountFilter::Inactive)
            statement += " AND active=0";
        if (!needle.isEmpty())
            statement += " AND instr(username, ?) > 0";
        QSqlQuery q(database.connection());
        q.prepare(statement + " ORDER BY username");
        if (!needle.isEmpty())
            q.addBindValue(needle);
        run(q);
        while (q.next())
            rows.append({q.value(0).toLongLong(), q.value(1).toString(), accountRoleFromValue(q.value(2).toString()),
                         q.value(3).toInt() == 1, q.value(4).toInt() != 0});
    });
    return rows;
}

qint64 AccountService::createCommonAccount(const Session &session, const QString &username,
                                           const PreparedPassword &password, InitialPasswordMode mode) {
    const QString canonical = AuthService::canonicalUsername(username);
    if (password.verifier.isEmpty())
        throw std::logic_error("Conta sem verificador de senha.");
    const int mustChange = mode == InitialPasswordMode::Temporary ? 1 : 0;
    qint64 accountId = 0;
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ManageCommonAccounts);
        QSqlQuery exists(database.connection());
        exists.prepare("SELECT COUNT(*) FROM accounts WHERE username=?");
        exists.addBindValue(canonical);
        run(exists);
        if (exists.next() && exists.value(0).toLongLong() > 0)
            throw AuthError("duplicate_username", Duplicate);
        const QString now = LocalDatabase::nowUtc();
        QSqlQuery insert(database.connection());
        insert.prepare("INSERT INTO "
                       "accounts(username,password_verifier,role,active,admin_slot,must_change_password,credential_"
                       "version,created_at,created_by) "
                       "VALUES(?,?,'common',1,NULL,?,1,?,?)");
        insert.addBindValue(canonical);
        insert.addBindValue(password.verifier);
        insert.addBindValue(mustChange);
        insert.addBindValue(now);
        insert.addBindValue(actor.accountId);
        if (!insert.exec()) {
            if (insert.lastError().text().contains("UNIQUE", Qt::CaseInsensitive))
                throw AuthError("duplicate_username", Duplicate);
            throw std::runtime_error(insert.lastError().text().toStdString());
        }
        accountId = insert.lastInsertId().toLongLong();
        database.appendEvent(
            {"account", accountId, std::nullopt, "Conta criada", "Conta comum " + canonical + " criada.", actor,
             merge({LocalDatabase::change("username", QJsonValue::Null, canonical),
                    LocalDatabase::change("role", QJsonValue::Null, QStringLiteral("common")),
                    LocalDatabase::change("active", QJsonValue::Null, 1),
                    LocalDatabase::change("must_change_password", QJsonValue::Null, mustChange),
                    LocalDatabase::change("initial_password_mode", QJsonValue::Null, modeValue(mode))}),
             now});
    });
    return accountId;
}

void AccountService::resetCommonPassword(const Session &session, qint64 accountId, const PreparedPassword &password,
                                         InitialPasswordMode mode) {
    if (password.verifier.isEmpty())
        throw std::logic_error("Conta sem verificador de senha.");
    const int mustChange = mode == InitialPasswordMode::Temporary ? 1 : 0;
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ManageCommonAccounts);
        const Target target = activeCommonTarget(database.connection(), accountId);
        QSqlQuery update(database.connection());
        update.prepare("UPDATE accounts SET password_verifier=?, must_change_password=?, "
                       "credential_version=credential_version+1 WHERE id=?");
        update.addBindValue(password.verifier);
        update.addBindValue(mustChange);
        update.addBindValue(accountId);
        run(update);
        database.appendEvent(
            {"account", accountId, std::nullopt, "Senha redefinida",
             "Senha da conta comum " + target.username + " redefinida.", actor,
             merge({LocalDatabase::change("must_change_password", target.mustChange, mustChange),
                    LocalDatabase::change("initial_password_mode", QJsonValue::Null, modeValue(mode))}),
             LocalDatabase::nowUtc()});
    });
}

void AccountService::inactivateCommonAccount(const Session &session, qint64 accountId) {
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ManageCommonAccounts);
        const Target target = activeCommonTarget(database.connection(), accountId);
        QSqlQuery update(database.connection());
        update.prepare("UPDATE accounts SET active=0, credential_version=credential_version+1 WHERE id=?");
        update.addBindValue(accountId);
        run(update);
        database.appendEvent({"account", accountId, std::nullopt, "Conta inativada",
                              "Conta comum " + target.username + " inativada.", actor,
                              LocalDatabase::change("active", 1, 0), LocalDatabase::nowUtc()});
    });
}

QVector<AccountSummary> AccountService::administrators(const Session &session) {
    QVector<AccountSummary> rows;
    database.transaction([&] {
        auth.authorize(session, Permission::ManageAdministrators);
        QSqlQuery q(database.connection());
        q.prepare(
            "SELECT id, username, role, active, must_change_password FROM accounts WHERE role IN ('principal','admin') "
            "ORDER BY role<>'principal', active DESC, username");
        run(q);
        while (q.next())
            rows.append({q.value(0).toLongLong(), q.value(1).toString(), accountRoleFromValue(q.value(2).toString()),
                         q.value(3).toInt() == 1, q.value(4).toInt() != 0});
    });
    return rows;
}

int AccountService::activeAdministratorCount(const Session &session) {
    int count = 0;
    database.transaction([&] {
        auth.authorize(session, Permission::ManageAdministrators);
        QSqlQuery q(database.connection());
        q.prepare("SELECT COUNT(*) FROM accounts WHERE role IN ('principal','admin') AND active=1");
        run(q);
        if (q.next())
            count = q.value(0).toInt();
    });
    return count;
}

qint64 AccountService::createAdministrator(const Session &session, const QString &username,
                                           const PreparedPassword &password, InitialPasswordMode mode) {
    const QString canonical = AuthService::canonicalUsername(username);
    if (password.verifier.isEmpty())
        throw std::logic_error("Conta sem verificador de senha.");
    const int mustChange = mode == InitialPasswordMode::Temporary ? 1 : 0;
    qint64 accountId = 0;
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ManageAdministrators);
        QSqlQuery exists(database.connection());
        exists.prepare("SELECT COUNT(*) FROM accounts WHERE username=?");
        exists.addBindValue(canonical);
        run(exists);
        if (exists.next() && exists.value(0).toLongLong() > 0)
            throw AuthError("duplicate_username", Duplicate);
        QSqlQuery taken(database.connection());
        taken.prepare("SELECT admin_slot FROM accounts WHERE admin_slot IS NOT NULL");
        run(taken);
        QSet<int> used;
        while (taken.next())
            used.insert(taken.value(0).toInt());
        int slot = 0;
        for (int candidate = 2; candidate <= 3 && slot == 0; ++candidate)
            if (!used.contains(candidate))
                slot = candidate;
        if (slot == 0)
            throw AuthError("admin_limit", AdminLimit);
        const QString now = LocalDatabase::nowUtc();
        QSqlQuery insert(database.connection());
        insert.prepare("INSERT INTO "
                       "accounts(username,password_verifier,role,active,admin_slot,must_change_password,credential_"
                       "version,created_at,created_by) "
                       "VALUES(?,?,'admin',1,?,?,1,?,?)");
        insert.addBindValue(canonical);
        insert.addBindValue(password.verifier);
        insert.addBindValue(slot);
        insert.addBindValue(mustChange);
        insert.addBindValue(now);
        insert.addBindValue(actor.accountId);
        if (!insert.exec()) {
            const QString error = insert.lastError().text();
            if (error.contains("admin_slot", Qt::CaseInsensitive) || error.contains("CHECK", Qt::CaseInsensitive))
                throw AuthError("admin_limit", AdminLimit);
            if (error.contains("UNIQUE", Qt::CaseInsensitive))
                throw AuthError("duplicate_username", Duplicate);
            throw std::runtime_error(error.toStdString());
        }
        accountId = insert.lastInsertId().toLongLong();
        database.appendEvent({"account", accountId, std::nullopt, "Administrador criado",
                              "Administrador " + canonical + " criado.", actor,
                              merge({LocalDatabase::change("username", QJsonValue::Null, canonical),
                                     LocalDatabase::change("role", QJsonValue::Null, QStringLiteral("admin")),
                                     LocalDatabase::change("active", QJsonValue::Null, 1),
                                     LocalDatabase::change("must_change_password", QJsonValue::Null, mustChange),
                                     LocalDatabase::change("initial_password_mode", QJsonValue::Null, modeValue(mode)),
                                     LocalDatabase::change("admin_slot", QJsonValue::Null, slot)}),
                              now});
    });
    return accountId;
}

void AccountService::resetAdministratorPassword(const Session &session, qint64 accountId,
                                                const PreparedPassword &password, InitialPasswordMode mode) {
    if (password.verifier.isEmpty())
        throw std::logic_error("Conta sem verificador de senha.");
    const int mustChange = mode == InitialPasswordMode::Temporary ? 1 : 0;
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ManageAdministrators);
        const AdminTarget target = activeAdministratorTarget(database.connection(), accountId);
        QSqlQuery update(database.connection());
        update.prepare("UPDATE accounts SET password_verifier=?, must_change_password=?, "
                       "credential_version=credential_version+1 WHERE id=?");
        update.addBindValue(password.verifier);
        update.addBindValue(mustChange);
        update.addBindValue(accountId);
        run(update);
        database.appendEvent(
            {"account", accountId, std::nullopt, "Senha de administrador redefinida",
             "Senha do administrador " + target.username + " redefinida.", actor,
             merge({LocalDatabase::change("must_change_password", target.mustChange, mustChange),
                    LocalDatabase::change("initial_password_mode", QJsonValue::Null, modeValue(mode))}),
             LocalDatabase::nowUtc()});
    });
}

void AccountService::inactivateAdministrator(const Session &session, qint64 accountId) {
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ManageAdministrators);
        const AdminTarget target = activeAdministratorTarget(database.connection(), accountId);
        QSqlQuery update(database.connection());
        update.prepare(
            "UPDATE accounts SET active=0, admin_slot=NULL, credential_version=credential_version+1 WHERE id=?");
        update.addBindValue(accountId);
        run(update);
        database.appendEvent({"account", accountId, std::nullopt, "Administrador inativado",
                              "Administrador " + target.username + " inativado.", actor,
                              merge({LocalDatabase::change("active", 1, 0),
                                     LocalDatabase::change("admin_slot", target.slot, QJsonValue::Null)}),
                              LocalDatabase::nowUtc()});
    });
}

#include "AuthService.h"
#include "security/Crypto.h"
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>

namespace {
const QString AlreadyInitialized =
    QStringLiteral("A conta principal já foi configurada. Use a recuperação autenticada.");
const QString InvalidCredentials =
    QStringLiteral("Não foi possível entrar. Confira o nome de usuário e a senha e tente novamente.");
const QString SessionExpired = QStringLiteral("Sessão administrativa expirada. Entre novamente para continuar.");
[[noreturn]] void deny(AuthorizationError::Reason reason, const QString &message) {
    throw AuthorizationError(reason, message);
}
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
void checkPasswordPolicy(const QByteArray &utf8) {
    const auto codePoints = QString::fromUtf8(utf8).toUcs4().size();
    if (codePoints < 8 || codePoints > 128)
        throw AuthError("invalid_password", "A senha deve ter de 8 a 128 caracteres.");
}
} // namespace

LoginAttempt::LoginAttempt(const PasswordHasher &hasher, QString verifier, QByteArray password, qint64 accountId,
                           qint64 credentialVersion)
    : hasher(hasher), verifier(std::move(verifier)), password(std::move(password)), accountId(accountId),
      credentialVersion(credentialVersion) {}

void LoginAttempt::verify() {
    verified = hasher.verify(verifier, password);
    crypto::wipe(password);
}

PasswordChangeAttempt::PasswordChangeAttempt(const PasswordHasher &hasher, QString currentVerifier, qint64 accountId,
                                             qint64 credentialVersion)
    : hasher(hasher), currentVerifier(std::move(currentVerifier)), accountId(accountId),
      credentialVersion(credentialVersion) {}

void PasswordChangeAttempt::prepare(const QByteArray &newPassword) {
    prepared.reset();
    checkPasswordPolicy(newPassword);
    if (hasher.verify(currentVerifier, newPassword))
        throw AuthError("same_password", "A nova senha deve ser diferente da senha temporária.");
    prepared = PreparedPassword(hasher.hash(newPassword));
}

AuthService::AuthService(LocalDatabase &database, const PasswordHasher &hasher, const SessionClock &clock,
                         qint64 adminDurationMs)
    : database(database), hasher(hasher), clock(clock), adminDurationMs(adminDurationMs) {}

const QString &AuthService::dummy() const {
    if (dummyVerifier.isEmpty())
        dummyVerifier = hasher.hash(crypto::randomBytes(32).toHex());
    return dummyVerifier;
}

QString AuthService::canonicalUsername(const QString &username) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9._-]{3,64}$"));
    const QString canonical = username.trimmed().toLower();
    if (!pattern.match(canonical).hasMatch())
        throw AuthError(
            "invalid_username",
            "O nome de usuário deve ter de 3 a 64 caracteres: letras sem acento, números, ponto, hífen ou sublinhado.");
    return canonical;
}

PreparedPassword AuthService::preparePassword(const QByteArray &utf8) const {
    checkPasswordPolicy(utf8);
    return PreparedPassword(hasher.hash(utf8));
}

qint64 AuthService::bootstrapPrincipal(const QString &username, const PreparedPassword &password,
                                       const QString &origin) {
    const QString canonical = canonicalUsername(username);
    qint64 accountId = 0;
    database.transaction([&] {
        auto &db = database.connection();
        QSqlQuery check(db);
        check.prepare(
            "SELECT (SELECT principal_account_id FROM app_config WHERE id=1), (SELECT COUNT(*) FROM accounts)");
        run(check);
        if (!check.next() || !check.value(0).isNull() || check.value(1).toLongLong() > 0)
            throw AuthError("already_initialized", AlreadyInitialized);
        const QString now = LocalDatabase::nowUtc();
        QSqlQuery insert(db);
        insert.prepare("INSERT INTO "
                       "accounts(username,password_verifier,role,active,admin_slot,must_change_password,credential_"
                       "version,created_at) "
                       "VALUES(?,?,'principal',1,1,0,1,?)");
        insert.addBindValue(canonical);
        insert.addBindValue(password.verifier);
        insert.addBindValue(now);
        run(insert);
        accountId = insert.lastInsertId().toLongLong();
        QSqlQuery config(db);
        config.prepare("UPDATE app_config SET principal_account_id=? WHERE id=1");
        config.addBindValue(accountId);
        run(config);
        database.appendEvent({"account", accountId, std::nullopt, "Conta principal criada",
                              "Conta principal " + canonical + " criada pelo " + origin + ".",
                              Actor{accountId, canonical},
                              merge({LocalDatabase::change("username", QJsonValue::Null, canonical),
                                     LocalDatabase::change("role", QJsonValue::Null, QStringLiteral("principal")),
                                     LocalDatabase::change("admin_slot", QJsonValue::Null, 1)}),
                              now});
    });
    return accountId;
}

bool AuthService::hasPrincipal() const {
    QSqlQuery q(database.connection());
    if (!q.exec("SELECT principal_account_id FROM app_config WHERE id=1") || !q.next())
        return false;
    return !q.value(0).isNull();
}

std::optional<Actor> AuthService::authenticate(const QString &username, const QByteArray &utf8) const {
    QString canonical;
    try {
        canonical = canonicalUsername(username);
    } catch (const AuthError &) {
    }
    std::optional<Actor> found;
    QString verifier = dummy();
    if (!canonical.isEmpty()) {
        QSqlQuery q(database.connection());
        q.prepare("SELECT id, username, password_verifier FROM accounts WHERE username=? AND active=1");
        q.addBindValue(canonical);
        if (q.exec() && q.next()) {
            found = Actor{q.value(0).toLongLong(), q.value(1).toString()};
            verifier = q.value(2).toString();
        }
    }
    const bool ok = hasher.verify(verifier, utf8);
    if (!found || !ok)
        return std::nullopt;
    return found;
}

std::shared_ptr<LoginAttempt> AuthService::prepareLogin(const QString &username, QByteArray password) const {
    QString canonical;
    try {
        canonical = canonicalUsername(username);
    } catch (const AuthError &) {
    }
    QString verifier = dummy();
    qint64 accountId = 0, credentialVersion = 0;
    if (!canonical.isEmpty()) {
        QSqlQuery q(database.connection());
        q.prepare("SELECT id, password_verifier, credential_version FROM accounts WHERE username=?");
        q.addBindValue(canonical);
        run(q);
        if (q.next()) {
            accountId = q.value(0).toLongLong();
            verifier = q.value(1).toString();
            credentialVersion = q.value(2).toLongLong();
        }
    }
    std::shared_ptr<LoginAttempt> attempt(new LoginAttempt(hasher, verifier, password, accountId, credentialVersion));
    crypto::wipe(password);
    return attempt;
}

Session AuthService::completeLogin(const LoginAttempt &attempt) {
    if (!attempt.verified || attempt.accountId == 0)
        throw AuthError("invalid_credentials", InvalidCredentials);
    QSqlQuery q(database.connection());
    q.prepare("SELECT username, role, active, credential_version, must_change_password FROM accounts WHERE id=?");
    q.addBindValue(attempt.accountId);
    run(q);
    if (!q.next() || q.value(2).toInt() != 1 || q.value(3).toLongLong() != attempt.credentialVersion)
        throw AuthError("invalid_credentials", InvalidCredentials);
    Session session;
    session.token = crypto::randomBytes(32);
    session.id = attempt.accountId;
    session.name = q.value(0).toString();
    session.accountRole = accountRoleFromValue(q.value(1).toString());
    session.mustChange = q.value(4).toInt() != 0;
    session.issued = clock.nowUtc();
    sessions.insert(session.token, {session.id, attempt.credentialVersion, session.accountRole, session.issued,
                                    clock.monotonicMs(), session.issued});
    return session;
}

Session AuthService::login(const QString &username, const QByteArray &password) {
    auto attempt = prepareLogin(username, password);
    attempt->verify();
    return completeLogin(*attempt);
}

void AuthService::logout(Session &session) {
    sessions.remove(session.token);
    expiredTokens.remove(session.token);
    session = Session();
}

PasswordChangeAttempt AuthService::prepareRequiredPasswordChange(const Session &session) const {
    const auto state = sessions.constFind(session.token);
    if (session.token.isEmpty() || state == sessions.constEnd() || state->accountId != session.id)
        deny(AuthorizationError::Reason::NotAuthenticated, "Entre novamente para continuar.");
    QSqlQuery q(database.connection());
    q.prepare("SELECT password_verifier, credential_version, must_change_password FROM accounts WHERE id=?");
    q.addBindValue(state->accountId);
    run(q);
    if (!q.next())
        deny(AuthorizationError::Reason::NotAuthenticated, "Entre novamente para continuar.");
    if (q.value(2).toInt() == 0)
        throw AuthError("change_not_required", "Esta conta não precisa definir uma nova senha.");
    return PasswordChangeAttempt(hasher, q.value(0).toString(), state->accountId, q.value(1).toLongLong());
}

void AuthService::completeRequiredPasswordChange(Session &session, const PasswordChangeAttempt &attempt) {
    if (!attempt.prepared)
        throw std::logic_error("A nova senha não foi preparada.");
    if (attempt.accountId != session.id)
        deny(AuthorizationError::Reason::NotAuthenticated, "Entre novamente para continuar.");
    qint64 newVersion = 0;
    database.transaction([&] {
        const Actor actor = authorize(session, Permission::ChangeOwnPassword);
        QSqlQuery current(database.connection());
        current.prepare("SELECT credential_version, must_change_password FROM accounts WHERE id=?");
        current.addBindValue(actor.accountId);
        run(current);
        if (!current.next() || current.value(0).toLongLong() != attempt.credentialVersion)
            deny(AuthorizationError::Reason::Revoked,
                 "Sua sessão foi encerrada porque a conta foi alterada. Entre novamente.");
        if (current.value(1).toInt() == 0)
            throw AuthError("change_not_required", "Esta conta não precisa definir uma nova senha.");
        newVersion = attempt.credentialVersion + 1;
        QSqlQuery update(database.connection());
        update.prepare("UPDATE accounts SET password_verifier=?, must_change_password=0, credential_version=? WHERE "
                       "id=? AND credential_version=?");
        update.addBindValue(attempt.prepared->verifier);
        update.addBindValue(newVersion);
        update.addBindValue(actor.accountId);
        update.addBindValue(attempt.credentialVersion);
        run(update);
        if (update.numRowsAffected() != 1)
            throw std::runtime_error("A conta foi alterada durante a troca de senha.");
        database.appendEvent({"account", actor.accountId, std::nullopt, "Senha definida",
                              "Senha pessoal definida por " + actor.username + ".", actor,
                              LocalDatabase::change("must_change_password", 1, 0), LocalDatabase::nowUtc()});
    });
    SessionState state = sessions.take(session.token);
    state.credentialVersion = newVersion;
    session.token = crypto::randomBytes(32);
    session.mustChange = false;
    sessions.insert(session.token, state);
}

Actor AuthService::authorize(const Session &session, Permission permission) {
    if (!database.inTransaction())
        throw std::runtime_error("A autorização deve ocorrer dentro da transação da operação.");
    using Reason = AuthorizationError::Reason;
    const auto state = sessions.find(session.token);
    if (!session.token.isEmpty() && expiredTokens.contains(session.token))
        deny(Reason::Expired, SessionExpired);
    if (session.token.isEmpty() || state == sessions.end() || state->accountId != session.id)
        deny(Reason::NotAuthenticated, "Entre novamente para continuar.");
    // Vale para toda Permission, inclusive inventário, e vem antes das regras da conta.
    if (expired(*state)) {
        sessions.erase(state);
        expiredTokens.insert(session.token);
        deny(Reason::Expired, SessionExpired);
    }
    QSqlQuery q(database.connection());
    q.prepare("SELECT username, role, active, credential_version, must_change_password FROM accounts WHERE id=?");
    q.addBindValue(state->accountId);
    run(q);
    if (!q.next())
        deny(Reason::NotAuthenticated, "Entre novamente para continuar.");
    if (q.value(2).toInt() != 1)
        deny(Reason::Inactive, "Esta conta está inativa. Entre com outra conta ativa.");
    if (q.value(3).toLongLong() != state->credentialVersion)
        deny(Reason::Revoked, "Sua sessão foi encerrada porque a conta foi alterada. Entre novamente.");
    if (q.value(4).toInt() != 0 && permission != Permission::ChangeOwnPassword)
        deny(Reason::PasswordChangeRequired, "Defina sua senha pessoal para continuar.");
    const AccountRole role = accountRoleFromValue(q.value(1).toString());
    if (permission == Permission::InactivateCatalog && role != AccountRole::Principal &&
        role != AccountRole::Administrator)
        deny(Reason::Forbidden, "Somente administradores podem inativar pessoas e notebooks.");
    if (permission == Permission::ManageCommonAccounts && role == AccountRole::Common)
        deny(Reason::Forbidden, "Esta ação exige uma conta administradora ativa.");
    if ((permission == Permission::ManageAdministrators || permission == Permission::ConfigureRecovery) &&
        role != AccountRole::Principal)
        deny(Reason::Forbidden, "Esta ação é exclusiva da conta principal.");
    state->role = role;
    state->lastObservedUtc = clock.nowUtc();
    return Actor{state->accountId, q.value(0).toString()};
}

// O primeiro relógio que indicar o prazo encerra a sessão; UTC anterior ao login também, pois o relógio foi recuado.
bool AuthService::expired(const SessionState &state) const {
    if (state.role == AccountRole::Common)
        return false;
    const QDateTime now = clock.nowUtc();
    return clock.monotonicMs() - state.issuedMonotonicMs >= adminDurationMs ||
           now >= state.issuedUtc.addMSecs(adminDurationMs) || now < state.issuedUtc;
}

QDateTime AuthService::sessionDeadlineUtc(const Session &session) const {
    const auto state = sessions.constFind(session.token);
    if (session.token.isEmpty() || state == sessions.constEnd() || state->role == AccountRole::Common)
        return {};
    return state->issuedUtc.addMSecs(adminDurationMs);
}

bool AuthService::sessionExpired(const Session &session) const {
    if (session.token.isEmpty())
        return false;
    if (expiredTokens.contains(session.token))
        return true;
    const auto state = sessions.constFind(session.token);
    return state != sessions.constEnd() && expired(*state);
}

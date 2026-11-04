#pragma once
#include "application/Session.h"
#include "application/SessionClock.h"
#include "security/PasswordHasher.h"
#include "storage/LocalDatabase.h"
#include <QHash>
#include <QSet>
#include <QString>
#include <memory>
#include <optional>
#include <stdexcept>

class AuthError : public std::runtime_error {
public:
    AuthError(const QString &code, const QString &message) : std::runtime_error(message.toStdString()), errorCode(code) {}
    QString code() const { return errorCode; }
private:
    QString errorCode;
};

// Prazo absoluto, contado do login, das sessões de administrador e da principal; não é renovado por atividade.
constexpr qint64 AdminSessionDuration = 15 * 60 * 1000;

enum class Permission { OperateInventory, ChangeOwnPassword, ManageCommonAccounts, ManageAdministrators, ConfigureRecovery, InactivateCatalog };

class AuthorizationError : public std::runtime_error {
public:
    enum class Reason { NotAuthenticated, Inactive, Revoked, PasswordChangeRequired, Forbidden, Expired };
    AuthorizationError(Reason reason, const QString &message) : std::runtime_error(message.toStdString()), why(reason) {}
    Reason reason() const { return why; }
private:
    Reason why;
};

// Verificador já derivado; criado somente por AuthService::preparePassword.
class PreparedPassword {
public:
    PreparedPassword(const PreparedPassword &) = default;
    PreparedPassword &operator=(const PreparedPassword &) = default;
private:
    friend class AuthService;
    friend class AccountService;
    friend class PasswordChangeAttempt;
    friend class RecoveryService;
    explicit PreparedPassword(QString verifier) : verifier(std::move(verifier)) {}
    QString verifier;
};

// prepare() não usa SQL e pode rodar em outra thread; a gravação fica em completeRequiredPasswordChange.
class PasswordChangeAttempt {
public:
    void prepare(const QByteArray &newPassword);
private:
    friend class AuthService;
    PasswordChangeAttempt(const PasswordHasher &hasher, QString currentVerifier, qint64 accountId, qint64 credentialVersion);
    const PasswordHasher &hasher;
    QString currentVerifier;
    qint64 accountId;
    qint64 credentialVersion;
    std::optional<PreparedPassword> prepared;
};

// verify() não usa SQL e pode rodar em outra thread; o resultado só é definido por ela.
class LoginAttempt {
public:
    void verify();
private:
    friend class AuthService;
    LoginAttempt(const PasswordHasher &hasher, QString verifier, QByteArray password, qint64 accountId, qint64 credentialVersion);
    const PasswordHasher &hasher;
    QString verifier;
    QByteArray password;
    qint64 accountId;
    qint64 credentialVersion;
    bool verified = false;
};

class AuthService {
public:
    AuthService(LocalDatabase &database, const PasswordHasher &hasher, const SessionClock &clock = SessionClock::system(), qint64 adminDurationMs = AdminSessionDuration);
    static QString canonicalUsername(const QString &username);
    // Sem SQL: pode rodar fora da thread proprietária da conexão.
    PreparedPassword preparePassword(const QByteArray &utf8) const;
    qint64 bootstrapPrincipal(const QString &username, const PreparedPassword &password);
    bool hasPrincipal() const;
    std::optional<Actor> authenticate(const QString &username, const QByteArray &utf8) const;
    std::shared_ptr<LoginAttempt> prepareLogin(const QString &username, QByteArray password) const;
    Session completeLogin(const LoginAttempt &attempt);
    Session login(const QString &username, const QByteArray &password);
    void logout(Session &session);
    // Troca obrigatória: prepare lê o verificador atual; complete grava, troca o token e libera a Session.
    PasswordChangeAttempt prepareRequiredPasswordChange(const Session &session) const;
    void completeRequiredPasswordChange(Session &session, const PasswordChangeAttempt &attempt);
    // Deve ser a primeira instrução dentro de LocalDatabase::transaction.
    Actor authorize(const Session &session, Permission permission);
    // Nulo para conta comum ou sessão desconhecida.
    QDateTime sessionDeadlineUtc(const Session &session) const;
    // Sem SQL: a mesma regra de prazo de authorize, para a interface bloquear antes da próxima ação.
    bool sessionExpired(const Session &session) const;
    const SessionClock &sessionClock() const { return clock; }
private:
    struct SessionState {
        qint64 accountId;
        qint64 credentialVersion;
        AccountRole role;
        QDateTime issuedUtc;
        qint64 issuedMonotonicMs;
        QDateTime lastObservedUtc;
    };
    LocalDatabase &database;
    const PasswordHasher &hasher;
    const SessionClock &clock;
    const qint64 adminDurationMs;
    mutable QString dummyVerifier;
    QHash<QByteArray, SessionState> sessions;
    QSet<QByteArray> expiredTokens;
    const QString &dummy() const;
    bool expired(const SessionState &state) const;
};

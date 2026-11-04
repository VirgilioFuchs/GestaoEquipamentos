#pragma once
#include "application/AuthService.h"
#include "domain/Account.h"
#include <QVector>

// Gestão de contas comuns. Cada chamada autoriza a Session dentro da própria transação, na thread proprietária da conexão;
// as senhas chegam já derivadas por AuthService::preparePassword.
class AccountService {
public:
    AccountService(LocalDatabase &database, AuthService &auth);
    AccountService(const AccountService &) = delete;
    AccountService &operator=(const AccountService &) = delete;
    QVector<AccountSummary> commonAccounts(const Session &session, AccountFilter filter, const QString &search);
    qint64 createCommonAccount(const Session &session, const QString &username, const PreparedPassword &password, InitialPasswordMode mode);
    void resetCommonPassword(const Session &session, qint64 accountId, const PreparedPassword &password, InitialPasswordMode mode);
    void inactivateCommonAccount(const Session &session, qint64 accountId);
    // Exclusivas da principal. A vaga (admin_slot 2 ou 3) é escolhida e conferida dentro da transação.
    QVector<AccountSummary> administrators(const Session &session);
    int activeAdministratorCount(const Session &session);
    qint64 createAdministrator(const Session &session, const QString &username, const PreparedPassword &password, InitialPasswordMode mode);
    void resetAdministratorPassword(const Session &session, qint64 accountId, const PreparedPassword &password, InitialPasswordMode mode);
    void inactivateAdministrator(const Session &session, qint64 accountId);
private:
    LocalDatabase &database;
    AuthService &auth;
};

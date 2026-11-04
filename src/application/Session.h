#pragma once
#include "domain/Account.h"
#include <QByteArray>
#include <QDateTime>
#include <QString>

// Emitida somente por AuthService; o token só é reconhecido pela instância emissora.
class Session {
public:
    Session() = default;
    bool isValid() const { return !token.isEmpty(); }
    qint64 accountId() const { return id; }
    QString username() const { return name; }
    AccountRole role() const { return accountRole; }
    bool mustChangePassword() const { return mustChange; }
    QDateTime issuedAtUtc() const { return issued; }
private:
    friend class AuthService;
    QByteArray token;
    qint64 id = 0;
    QString name;
    AccountRole accountRole = AccountRole::Common;
    bool mustChange = false;
    QDateTime issued;
};

#pragma once
#include <QString>

enum class AccountRole { Common, Administrator, Principal };

inline QString accountRoleLabel(AccountRole role) {
    switch (role) {
    case AccountRole::Principal: return QStringLiteral("Principal");
    case AccountRole::Administrator: return QStringLiteral("Administrador");
    default: return QStringLiteral("Comum");
    }
}

inline AccountRole accountRoleFromValue(const QString &value) {
    if (value == QLatin1String("principal")) return AccountRole::Principal;
    if (value == QLatin1String("admin")) return AccountRole::Administrator;
    return AccountRole::Common;
}

struct AccountSummary {
    qint64 id = 0;
    QString username;
    AccountRole role = AccountRole::Common;
    bool active = true;
    bool mustChangePassword = false;
};

enum class AccountFilter { Active, Inactive, All };
enum class InitialPasswordMode { Temporary, Assisted };

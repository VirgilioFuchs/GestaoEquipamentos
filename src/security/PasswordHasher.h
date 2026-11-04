#pragma once
#include <QByteArray>
#include <QString>
#include <cstddef>

// Argon2id no formato PHC (crypto_pwhash_str). Sem estado mutável: seguro em qualquer thread.
class PasswordHasher {
public:
    struct Cost { unsigned long long opsLimit; size_t memLimit; };
    static Cost interactive();
    static Cost testing();
    explicit PasswordHasher(Cost cost = interactive());
    QString hash(const QByteArray &utf8) const;
    bool verify(const QString &verifier, const QByteArray &utf8) const;
private:
    Cost cost;
};

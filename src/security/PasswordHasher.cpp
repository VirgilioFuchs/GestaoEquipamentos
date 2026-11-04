#include "PasswordHasher.h"
#include "Crypto.h"
#include <sodium.h>
#include <stdexcept>

PasswordHasher::Cost PasswordHasher::interactive() {
    return {crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE};
}
PasswordHasher::Cost PasswordHasher::testing() {
    return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN};
}
PasswordHasher::PasswordHasher(Cost cost) : cost(cost) {
    crypto::initialize();
}

QString PasswordHasher::hash(const QByteArray &utf8) const {
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out, utf8.constData(), static_cast<unsigned long long>(utf8.size()), cost.opsLimit,
                          cost.memLimit) != 0)
        throw std::runtime_error("Memória insuficiente para proteger a senha.");
    return QString::fromLatin1(out);
}
bool PasswordHasher::verify(const QString &verifier, const QByteArray &utf8) const {
    const QByteArray stored = verifier.toLatin1();
    if (stored.isEmpty() || stored.size() >= crypto_pwhash_STRBYTES)
        return false;
    return crypto_pwhash_str_verify(stored.constData(), utf8.constData(),
                                    static_cast<unsigned long long>(utf8.size())) == 0;
}

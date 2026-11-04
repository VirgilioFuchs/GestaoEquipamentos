#include "Crypto.h"
#include <sodium.h>
#include <stdexcept>

namespace crypto {
void initialize() {
    if (sodium_init() < 0)
        throw std::runtime_error("Não foi possível inicializar a biblioteca de criptografia.");
}
QByteArray randomBytes(int count) {
    initialize();
    QByteArray bytes(count, Qt::Uninitialized);
    randombytes_buf(bytes.data(), static_cast<size_t>(bytes.size()));
    return bytes;
}
bool constantTimeEquals(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size())
        return false;
    return sodium_memcmp(a.constData(), b.constData(), static_cast<size_t>(a.size())) == 0;
}
void wipe(QByteArray &bytes) {
    if (!bytes.isEmpty())
        sodium_memzero(bytes.data(), static_cast<size_t>(bytes.size()));
    bytes.clear();
}
} // namespace crypto

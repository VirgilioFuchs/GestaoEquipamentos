#include "RecoveryCodes.h"
#include "Crypto.h"
#include <sodium.h>

namespace recovery_codes {
namespace {
constexpr char Alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr int Symbols = 16;
}

QStringList generate() {
    QStringList codes;
    for (int i = 0; i < SetSize; ++i) {
        QByteArray random = crypto::randomBytes(Symbols * 5 / 8);
        QString code;
        quint32 buffer = 0; int bits = 0;
        for (const char byte : random) {
            buffer = (buffer << 8) | quint8(byte); bits += 8;
            while (bits >= 5) { code.append(QLatin1Char(Alphabet[(buffer >> (bits - 5)) & 31])); bits -= 5; }
        }
        crypto::wipe(random);
        codes << code.left(4) + '-' + code.mid(4, 4) + '-' + code.mid(8, 4) + '-' + code.mid(12, 4);
    }
    return codes;
}

QString normalize(const QString &code) {
    QString out;
    for (const QChar c : code) if (c != ' ' && c != '-') out.append(c.toUpper());
    return out;
}

QString verifier(const QString &databaseUuid, qint64 generation, const QString &normalized) {
    crypto::initialize();
    const QByteArray input = databaseUuid.toUtf8() + '\n' + QByteArray::number(generation) + '\n' + normalized.toUtf8();
    QByteArray hash(crypto_generichash_BYTES, Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char *>(hash.data()), size_t(hash.size()),
                       reinterpret_cast<const unsigned char *>(input.constData()), size_t(input.size()), nullptr, 0);
    return QString::fromLatin1(hash.toHex());
}
}

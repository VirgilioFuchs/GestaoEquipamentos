#include "Totp.h"
#include "Crypto.h"
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QUrl>

namespace totp {
QString code(const QByteArray &secret, qint64 counter) {
    QByteArray message(8, '\0');
    auto value = static_cast<quint64>(counter);
    for (int i = 7; i >= 0; --i) { message[i] = static_cast<char>(value & 0xff); value >>= 8; }
    const QByteArray mac = QMessageAuthenticationCode::hash(message, secret, QCryptographicHash::Sha1);
    const int offset = mac[mac.size() - 1] & 0x0f;
    const quint32 binary = (quint32(quint8(mac[offset]) & 0x7f) << 24) | (quint32(quint8(mac[offset + 1])) << 16)
                         | (quint32(quint8(mac[offset + 2])) << 8) | quint32(quint8(mac[offset + 3]));
    return QStringLiteral("%1").arg(binary % 1000000u, Digits, 10, QLatin1Char('0'));
}

qint64 counterAt(const QDateTime &utc) {
    const qint64 seconds = utc.toSecsSinceEpoch();
    return seconds >= 0 ? seconds / StepSeconds : -((-seconds + StepSeconds - 1) / StepSeconds);
}

std::optional<qint64> match(const QByteArray &secret, const QString &typed, const QDateTime &utc, std::optional<qint64> lastCounter) {
    QString digits = typed;
    digits.remove(QLatin1Char(' '));
    if (digits.size() != Digits) return std::nullopt;
    for (const QChar c : digits) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return std::nullopt;
    const QByteArray given = digits.toLatin1();
    const qint64 now = counterAt(utc);
    std::optional<qint64> found;
    for (qint64 counter = now - 1; counter <= now + 1; ++counter) {
        if (lastCounter && counter <= *lastCounter) continue;
        if (crypto::constantTimeEquals(code(secret, counter).toLatin1(), given) && !found) found = counter;
    }
    return found;
}

QString base32(const QByteArray &bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QString out;
    quint32 buffer = 0;
    int bits = 0;
    for (const char byte : bytes) {
        buffer = (buffer << 8) | quint8(byte);
        bits += 8;
        while (bits >= 5) { out += QLatin1Char(alphabet[(buffer >> (bits - 5)) & 0x1f]); bits -= 5; }
    }
    if (bits > 0) out += QLatin1Char(alphabet[(buffer << (5 - bits)) & 0x1f]);
    return out;
}

QString otpauthUri(const QString &account, const QByteArray &secret) {
    return "otpauth://totp/GestaoEquipamentos:" + QString::fromLatin1(QUrl::toPercentEncoding(account))
         + "?secret=" + base32(secret) + "&issuer=GestaoEquipamentos&algorithm=SHA1&digits=6&period=30";
}
}

#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <optional>

// TOTP RFC 6238 compatível com o Google Authenticator: HMAC-SHA1, 6 dígitos, passo de 30 s.
namespace totp {
constexpr int Digits = 6;
constexpr qint64 StepSeconds = 30;
QString code(const QByteArray &secret, qint64 counter);
qint64 counterAt(const QDateTime &utc);
// Aceita o passo atual ±1; o contador encontrado precisa ser maior que lastCounter (passo já usado é recusado).
std::optional<qint64> match(const QByteArray &secret, const QString &code, const QDateTime &utc, std::optional<qint64> lastCounter);
// RFC 4648 sem padding.
QString base32(const QByteArray &bytes);
QString otpauthUri(const QString &account, const QByteArray &secret);
}

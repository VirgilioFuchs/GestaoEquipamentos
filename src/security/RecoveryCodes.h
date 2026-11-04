#pragma once
#include <QString>
#include <QStringList>

// Códigos de uso único: 16 símbolos Base32 sem I, L, O e U (80 bits), exibidos como XXXX-XXXX-XXXX-XXXX.
// O banco guarda só o verificador, ligado ao UUID do banco e à geração do vínculo.
namespace recovery_codes {
constexpr int SetSize = 10;
QStringList generate();
QString normalize(const QString &code);
QString verifier(const QString &databaseUuid, qint64 generation, const QString &normalized);
}

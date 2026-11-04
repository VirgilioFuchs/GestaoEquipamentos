#pragma once
#include "application/AuthService.h"
#include "application/RecoveryService.h"
#include <QByteArray>
#include <functional>
#include <optional>

// Protocolo do GestaoEquipamentosAdmin: objetos JSON de uma linha no stdin; cada resposta é uma linha no stdout.
// bootstrap:      {protocol, action, username, password} → resposta final.
// recover_verify: {protocol, action, username, method ("authenticator" | "backup_code"), proof} → recusa final,
//                 ou "verified" e espera {action: "recover_commit", password} por até CommitTimeoutMs.
// Sem a segunda mensagem (fim da entrada ou prazo), nada é alterado e a prova continua válida.
namespace AdminProtocol {
constexpr int Version = 1;
constexpr int MaxRequestBytes = 16384;
constexpr int CommitTimeoutMs = 5 * 60 * 1000;
// Devolve a próxima linha (sem o '\n'), ou nullopt no fim da entrada ou quando o prazo acaba.
using ReadLine = std::function<std::optional<QByteArray>(int timeoutMs)>;
using WriteLine = std::function<void(const QByteArray &json)>;
// Conduz uma conversa completa e devolve o código de saída do processo.
int serve(const ReadLine &read, const WriteLine &write, AuthService &auth, RecoveryService &recovery);
}

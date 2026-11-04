#pragma once
#include "application/AuthService.h"
#include "application/SessionClock.h"
#include "security/SecretVault.h"
#include "storage/LocalDatabase.h"
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>

struct RecoveryStatus {
    bool configured = false;
    QDateTime enrolledAtUtc;
    qint64 generation = 0;
    // Vínculo removido porque um código de recuperação foi usado; exige novo vínculo.
    bool needsRebinding = false;
};

enum class RecoveryMethod {
    Authenticator,
    BackupCode
};
constexpr int MaxRecoveryFailures = 5;
constexpr int RecoveryCooldownSeconds = 300;
// Prazo entre verifyRecovery e completeRecovery (o helper espera a nova senha por 5 minutos).
constexpr int RecoveryConfirmSeconds = 360;

// Prova conferida por verifyRecovery e ainda não consumida; existe só em memória.
class VerifiedRecovery {
public:
    ~VerifiedRecovery();
    VerifiedRecovery(const VerifiedRecovery &) = delete;
    VerifiedRecovery &operator=(const VerifiedRecovery &) = delete;

private:
    friend class RecoveryService;
    VerifiedRecovery(QString username, RecoveryMethod method, QByteArray proof, QDateTime verifiedAtUtc);
    QString username;
    RecoveryMethod method;
    QByteArray proof;
    QDateTime verifiedAtUtc;
};

// Segredo candidato já gravado e relido no cofre; o vínculo só fica ativo em confirmEnrollment.
class EnrollmentDraft {
public:
    ~EnrollmentDraft();
    EnrollmentDraft(const EnrollmentDraft &) = delete;
    EnrollmentDraft &operator=(const EnrollmentDraft &) = delete;
    // Contém o segredo em Base32: exibir somente como QR/texto na tela, nunca registrar.
    QString uri() const;

private:
    friend class RecoveryService;
    EnrollmentDraft(QByteArray secret, QString vaultKey, QString account, qint64 baseGeneration);
    void close();
    QByteArray secret;
    QString vaultKey;
    QString account;
    qint64 baseGeneration;
    bool open = true;
};

class RecoveryService {
public:
    RecoveryService(LocalDatabase &database, AuthService &auth, SecretVault &vault,
                    const SessionClock &clock = SessionClock::system());
    std::unique_ptr<EnrollmentDraft> beginEnrollment(const Session &session);
    // Devolve os códigos de recuperação da nova geração: exibir uma única vez, nunca gravar em texto.
    QStringList confirmEnrollment(const Session &session, EnrollmentDraft &draft, const QString &code);
    void cancelEnrollment(EnrollmentDraft &draft);
    RecoveryStatus status(const Session &session);
    int remainingBackupCodes(const Session &session);
    // Sem sessão; roda na thread proprietária da conexão. A senha já vem derivada por AuthService::preparePassword.
    void recoverPrincipal(const QString &username, RecoveryMethod method, const QString &proof,
                          const PreparedPassword &newPassword);
    // Confere a prova sem consumi-la nem alterar a senha; recusa e cooldown contam e geram evento como em
    // recoverPrincipal.
    std::unique_ptr<VerifiedRecovery> verifyRecovery(const QString &username, RecoveryMethod method,
                                                     const QString &proof);
    // Confere a prova de novo e conclui como recoverPrincipal, na mesma transação; o TOTP vale pela hora da
    // conferência.
    void completeRecovery(const VerifiedRecovery &verified, const PreparedPassword &newPassword);

private:
    // Sem newPassword, só confere: prova aceita não altera nada.
    void attemptRecovery(const QString &username, RecoveryMethod method, const QString &proof,
                         const std::optional<QDateTime> &proofTimeUtc, const PreparedPassword *newPassword);
    void discardCandidate(EnrollmentDraft &draft);
    LocalDatabase &database;
    AuthService &auth;
    SecretVault &vault;
    const SessionClock &clock;
};

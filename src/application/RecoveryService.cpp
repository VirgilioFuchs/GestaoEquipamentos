#include "RecoveryService.h"
#include "security/Crypto.h"
#include "security/RecoveryCodes.h"
#include "security/Totp.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {
const QString Unavailable = QStringLiteral("Não foi possível preparar a recuperação. Tente novamente. Se o problema continuar, procure o responsável pela instalação.");
const QString InvalidCode = QStringLiteral("Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método.");
const QString Changed = QStringLiteral("A recuperação foi configurada em outra janela. Inicie a configuração novamente.");
const QString Cooldown = QStringLiteral("Muitas tentativas. Aguarde alguns minutos e tente novamente.");
const QString AuthenticatorUnavailable = QStringLiteral("Não foi possível usar o código do Authenticator neste computador. Use um código de recuperação.");
constexpr int SecretBytes = 20;

[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
void run(QSqlQuery &q) { if (!q.exec()) fail(q.lastError().text()); }
}

EnrollmentDraft::EnrollmentDraft(QByteArray secret, QString vaultKey, QString account, qint64 baseGeneration)
    : secret(std::move(secret)), vaultKey(std::move(vaultKey)), account(std::move(account)), baseGeneration(baseGeneration) {}
EnrollmentDraft::~EnrollmentDraft() { crypto::wipe(secret); }
QString EnrollmentDraft::uri() const { return open ? totp::otpauthUri(account, secret) : QString(); }
void EnrollmentDraft::close() { crypto::wipe(secret); open = false; }

RecoveryService::RecoveryService(LocalDatabase &database, AuthService &auth, SecretVault &vault, const SessionClock &clock)
    : database(database), auth(auth), vault(vault), clock(clock) {}

std::unique_ptr<EnrollmentDraft> RecoveryService::beginEnrollment(const Session &session) {
    QString uuid, username;
    qint64 principalId = 0, generation = 0;
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ConfigureRecovery);
        QSqlQuery q(database.connection());
        q.prepare("SELECT a.database_uuid, a.principal_account_id, r.generation FROM app_config a, recovery_config r WHERE a.id=1 AND r.id=1");
        run(q);
        if (!q.next()) fail("Configuração de recuperação ausente.");
        uuid = q.value(0).toString(); principalId = q.value(1).toLongLong(); generation = q.value(2).toLongLong();
        username = actor.username;
    });
    // O sufixo separa candidatos simultâneos da mesma geração; a chave ativa nunca é reutilizada.
    const QString key = QStringLiteral("recovery/%1/%2/%3/%4").arg(uuid).arg(principalId).arg(generation + 1)
                            .arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    QByteArray secret = crypto::randomBytes(SecretBytes);
    try {
        vault.write(key, secret);
        QByteArray stored = vault.read(key);
        const bool same = crypto::constantTimeEquals(stored, secret);
        crypto::wipe(stored);
        if (!same) throw VaultError("O cofre devolveu um valor diferente do gravado.");
    } catch (const VaultError &) {
        try { vault.remove(key); } catch (const VaultError &) {}
        crypto::wipe(secret);
        throw AuthError("recovery_unavailable", Unavailable);
    }
    return std::unique_ptr<EnrollmentDraft>(new EnrollmentDraft(std::move(secret), key, username, generation));
}

QStringList RecoveryService::confirmEnrollment(const Session &session, EnrollmentDraft &draft, const QString &code) {
    if (!draft.open) throw AuthError("invalid_code", InvalidCode);
    const auto counter = totp::match(draft.secret, code, clock.nowUtc(), std::nullopt);
    if (!counter) throw AuthError("invalid_code", InvalidCode);
    QString previousKey;
    QStringList codes;
    bool stale = false;
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::ConfigureRecovery);
        auto &db = database.connection();
        QSqlQuery current(db);
        current.prepare("SELECT generation, vault_key FROM recovery_config WHERE id=1");
        run(current);
        if (!current.next()) fail("Configuração de recuperação ausente.");
        if (current.value(0).toLongLong() != draft.baseGeneration) { stale = true; return; }
        previousKey = current.value(1).toString();
        const qint64 next = draft.baseGeneration + 1;
        QSqlQuery update(db);
        update.prepare("UPDATE recovery_config SET vault_key=?, generation=?, totp_last_counter=?, enrolled_at=?, failed_attempts=0, cooldown_until=NULL "
                       "WHERE id=1 AND generation=?");
        update.addBindValue(draft.vaultKey); update.addBindValue(next); update.addBindValue(*counter);
        update.addBindValue(clock.nowUtc().toString(Qt::ISODateWithMs)); update.addBindValue(draft.baseGeneration);
        run(update);
        if (update.numRowsAffected() != 1) fail("Configuração de recuperação ausente.");
        const QString uuid = database.databaseUuid();
        const QString created = clock.nowUtc().toString(Qt::ISODateWithMs);
        codes = recovery_codes::generate();
        for (const QString &plain : codes) {
            QSqlQuery insert(db);
            insert.prepare("INSERT INTO recovery_codes(generation,code_verifier,created_at) VALUES(?,?,?)");
            insert.addBindValue(next); insert.addBindValue(recovery_codes::verifier(uuid, next, recovery_codes::normalize(plain)));
            insert.addBindValue(created);
            run(insert);
        }
        QJsonObject changes = LocalDatabase::change("generation", draft.baseGeneration, next);
        changes.insert("method", QJsonObject{{"de", previousKey.isEmpty() ? QJsonValue() : QJsonValue("totp")}, {"para", "totp"}});
        database.appendEvent({"recovery", 1, std::nullopt, "Recuperação vinculada",
                              "Google Authenticator vinculado à conta principal.", actor, changes, LocalDatabase::nowUtc()});
    });
    if (stale) {
        discardCandidate(draft);
        throw AuthError("recovery_changed", Changed);
    }
    draft.close();
    if (!previousKey.isEmpty() && previousKey != draft.vaultKey) {
        try { vault.remove(previousKey); } catch (const VaultError &) {}
    }
    return codes;
}

void RecoveryService::cancelEnrollment(EnrollmentDraft &draft) {
    if (draft.open) discardCandidate(draft);
}

void RecoveryService::discardCandidate(EnrollmentDraft &draft) {
    draft.close();
    try { vault.remove(draft.vaultKey); } catch (const VaultError &) {}
}

RecoveryStatus RecoveryService::status(const Session &session) {
    RecoveryStatus result;
    database.transaction([&] {
        auth.authorize(session, Permission::ConfigureRecovery);
        QSqlQuery q(database.connection());
        q.prepare("SELECT vault_key, enrolled_at, generation FROM recovery_config WHERE id=1");
        run(q);
        if (!q.next()) fail("Configuração de recuperação ausente.");
        result.configured = !q.value(0).isNull();
        if (result.configured) result.enrolledAtUtc = QDateTime::fromString(q.value(1).toString(), Qt::ISODateWithMs);
        result.generation = q.value(2).toLongLong();
        result.needsRebinding = !result.configured && result.generation > 0;
    });
    return result;
}

int RecoveryService::remainingBackupCodes(const Session &session) {
    int remaining = 0;
    database.transaction([&] {
        auth.authorize(session, Permission::ConfigureRecovery);
        QSqlQuery q(database.connection());
        q.prepare("SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NULL AND generation=(SELECT generation FROM recovery_config WHERE id=1)");
        run(q);
        if (q.next()) remaining = q.value(0).toInt();
    });
    return remaining;
}

VerifiedRecovery::VerifiedRecovery(QString username, RecoveryMethod method, QByteArray proof, QDateTime verifiedAtUtc)
    : username(std::move(username)), method(method), proof(std::move(proof)), verifiedAtUtc(std::move(verifiedAtUtc)) {}
VerifiedRecovery::~VerifiedRecovery() { crypto::wipe(proof); }

void RecoveryService::recoverPrincipal(const QString &username, RecoveryMethod method, const QString &proof, const PreparedPassword &newPassword) {
    attemptRecovery(username, method, proof, std::nullopt, &newPassword);
}

std::unique_ptr<VerifiedRecovery> RecoveryService::verifyRecovery(const QString &username, RecoveryMethod method, const QString &proof) {
    const QDateTime verifiedAt = clock.nowUtc();
    attemptRecovery(username, method, proof, verifiedAt, nullptr);
    return std::unique_ptr<VerifiedRecovery>(new VerifiedRecovery(username, method, proof.toUtf8(), verifiedAt));
}

void RecoveryService::completeRecovery(const VerifiedRecovery &verified, const PreparedPassword &newPassword) {
    if (verified.verifiedAtUtc.secsTo(clock.nowUtc()) > RecoveryConfirmSeconds) throw AuthError("invalid_code", InvalidCode);
    attemptRecovery(verified.username, verified.method, QString::fromUtf8(verified.proof), verified.verifiedAtUtc, &newPassword);
}

void RecoveryService::attemptRecovery(const QString &username, RecoveryMethod method, const QString &proof,
                                      const std::optional<QDateTime> &proofTimeUtc, const PreparedPassword *newPassword) {
    QString canonical;
    try { canonical = AuthService::canonicalUsername(username); } catch (const AuthError &) {}
    QString readKey;
    QByteArray secret;
    if (method == RecoveryMethod::Authenticator) {
        database.transaction([&] {
            QSqlQuery q(database.connection());
            q.prepare("SELECT vault_key FROM recovery_config WHERE id=1");
            run(q);
            if (q.next()) readKey = q.value(0).toString();
        });
        if (readKey.isEmpty()) throw AuthError("authenticator_unavailable", AuthenticatorUnavailable);
        try { secret = vault.read(readKey); }
        catch (const VaultError &) { throw AuthError("authenticator_unavailable", AuthenticatorUnavailable); }
    }
    enum class Outcome { Recovered, Verified, Refused, Cooldown } outcome = Outcome::Refused;
    QString removedKey;
    try {
        database.transaction([&] {
            auto &db = database.connection();
            QSqlQuery state(db);
            state.prepare("SELECT r.generation, r.vault_key, r.totp_last_counter, r.failed_attempts, r.cooldown_until, a.database_uuid, p.id, p.username "
                          "FROM recovery_config r JOIN app_config a ON a.id=1 LEFT JOIN accounts p ON p.id=a.principal_account_id AND p.active=1 WHERE r.id=1");
            run(state);
            if (!state.next()) fail("Configuração de recuperação ausente.");
            const QDateTime now = clock.nowUtc();
            const QString cooldown = state.value(4).toString();
            if (!cooldown.isEmpty() && QDateTime::fromString(cooldown, Qt::ISODateWithMs) > now) { outcome = Outcome::Cooldown; return; }
            const qint64 generation = state.value(0).toLongLong();
            const QString activeKey = state.value(1).toString();
            const int failures = state.value(3).toInt();
            const bool principalMatches = !state.value(6).isNull() && !canonical.isEmpty() && canonical == state.value(7).toString();
            std::optional<qint64> counter;
            qint64 codeId = 0;
            if (method == RecoveryMethod::Authenticator) {
                if (!activeKey.isEmpty() && activeKey == readKey) {
                    const auto last = state.value(2).isNull() ? std::nullopt : std::optional<qint64>(state.value(2).toLongLong());
                    counter = totp::match(secret, proof, proofTimeUtc.value_or(now), last);
                }
            } else {
                const QByteArray wanted = recovery_codes::verifier(state.value(5).toString(), generation, recovery_codes::normalize(proof)).toLatin1();
                QSqlQuery codes(db);
                codes.prepare("SELECT id, code_verifier FROM recovery_codes WHERE generation=? AND used_at IS NULL");
                codes.addBindValue(generation);
                run(codes);
                while (codes.next())
                    if (crypto::constantTimeEquals(codes.value(1).toString().toLatin1(), wanted)) codeId = codes.value(0).toLongLong();
            }
            const bool proven = method == RecoveryMethod::Authenticator ? counter.has_value() : codeId != 0;
            if (!principalMatches || !proven) {
                QSqlQuery update(db);
                if (failures + 1 >= MaxRecoveryFailures) {
                    const QString until = now.addSecs(RecoveryCooldownSeconds).toString(Qt::ISODateWithMs);
                    update.prepare("UPDATE recovery_config SET failed_attempts=0, cooldown_until=? WHERE id=1");
                    update.addBindValue(until);
                    run(update);
                    QJsonObject changes = LocalDatabase::change("failed_attempts", failures, 0);
                    changes.insert("cooldown_until", QJsonObject{{"de", cooldown.isEmpty() ? QJsonValue() : QJsonValue(cooldown)}, {"para", until}});
                    database.appendEvent({"recovery", 1, std::nullopt, "Recuperação bloqueada temporariamente",
                                          "Cinco tentativas recusadas; novas tentativas bloqueadas por 5 minutos.", std::nullopt, changes, LocalDatabase::nowUtc()});
                } else {
                    update.prepare("UPDATE recovery_config SET failed_attempts=? WHERE id=1");
                    update.addBindValue(failures + 1);
                    run(update);
                    database.appendEvent({"recovery", 1, std::nullopt, "Tentativa de recuperação recusada", "Prova de recuperação recusada.",
                                          std::nullopt, LocalDatabase::change("failed_attempts", failures, failures + 1), LocalDatabase::nowUtc()});
                }
                outcome = Outcome::Refused;
                return;
            }
            if (!newPassword) { outcome = Outcome::Verified; return; }
            const Actor principal{state.value(6).toLongLong(), state.value(7).toString()};
            QSqlQuery account(db);
            account.prepare("UPDATE accounts SET password_verifier=?, must_change_password=0, credential_version=credential_version+1 WHERE id=?");
            account.addBindValue(newPassword->verifier); account.addBindValue(principal.accountId);
            run(account);
            if (account.numRowsAffected() != 1) fail("Conta principal ausente.");
            QJsonObject changes = LocalDatabase::change("method", QJsonValue(),
                                                        method == RecoveryMethod::Authenticator ? QStringLiteral("authenticator") : QStringLiteral("backup_code"));
            QSqlQuery config(db);
            if (method == RecoveryMethod::Authenticator) {
                config.prepare("UPDATE recovery_config SET totp_last_counter=?, failed_attempts=0, cooldown_until=NULL WHERE id=1 AND vault_key=?");
                config.addBindValue(*counter); config.addBindValue(activeKey);
                run(config);
                if (config.numRowsAffected() != 1) fail("Configuração de recuperação alterada.");
            } else {
                QSqlQuery consume(db);
                consume.prepare("UPDATE recovery_codes SET used_at=? WHERE id=? AND used_at IS NULL");
                consume.addBindValue(now.toString(Qt::ISODateWithMs)); consume.addBindValue(codeId);
                run(consume);
                if (consume.numRowsAffected() != 1) fail("Código de recuperação já utilizado.");
                // Decisão 2026-10-21 #5: a principal precisou de código, então o segredo TOTP deixa de valer.
                config.prepare("UPDATE recovery_config SET failed_attempts=0, cooldown_until=NULL, vault_key=NULL, enrolled_at=NULL, totp_last_counter=NULL WHERE id=1");
                run(config);
                if (!activeKey.isEmpty()) {
                    removedKey = activeKey;
                    changes.insert("totp_enrolled", QJsonObject{{"de", 1}, {"para", 0}});
                }
            }
            database.appendEvent({"recovery", 1, std::nullopt, "Conta principal recuperada",
                                  "Senha da conta principal redefinida pela recuperação.", principal, changes, LocalDatabase::nowUtc()});
            outcome = Outcome::Recovered;
        });
    } catch (...) {
        crypto::wipe(secret);
        throw;
    }
    crypto::wipe(secret);
    if (outcome == Outcome::Cooldown) throw AuthError("recovery_cooldown", Cooldown);
    if (outcome == Outcome::Refused) throw AuthError("invalid_code", InvalidCode);
    // O cofre não participa da transação; uma chave que sobrar fica inalcançável porque vault_key é NULL.
    if (!removedKey.isEmpty()) {
        try { vault.remove(removedKey); } catch (const VaultError &) {}
    }
}

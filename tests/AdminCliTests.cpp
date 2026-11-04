#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QStandardPaths>
#include "TestSupport.h"
#include "application/AdminProtocol.h"
#include "security/Totp.h"

namespace {
const QByteArray Password = QByteArrayLiteral("frase secreta do helper 42");
const QByteArray NewPassword = QByteArrayLiteral("nova frase da principal 77");
const QString InvalidCode = "Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método.";
struct Run { int exitCode; QByteArray out; QByteArray err; };
Run runHelper(const QStringList &arguments, const QByteArray &input) {
    QProcess helper;
    helper.start(QStringLiteral(GESTAOEQUIPAMENTOS_ADMIN_EXE), arguments);
    if (!helper.waitForStarted(10000)) return {-1, {}, "não iniciou"};
    helper.write(input);
    helper.closeWriteChannel();
    if (!helper.waitForFinished(60000)) { helper.kill(); return {-1, {}, "tempo esgotado"}; }
    return {helper.exitCode(), helper.readAllStandardOutput(), helper.readAllStandardError()};
}
QByteArray bootstrapRequest(const QString &username) {
    return QJsonDocument(QJsonObject{{"protocol", 1}, {"action", "bootstrap"}, {"username", username},
                                     {"password", QString::fromUtf8(Password)}}).toJson(QJsonDocument::Compact);
}
QByteArray verifyRequest(const QString &method, const QString &proof) {
    return QJsonDocument(QJsonObject{{"protocol", 1}, {"action", "recover_verify"}, {"username", "principal"}, {"method", method},
                                     {"proof", proof}}).toJson(QJsonDocument::Compact);
}
QByteArray commitRequest(const QByteArray &password) {
    return QJsonDocument(QJsonObject{{"action", "recover_commit"}, {"password", QString::fromUtf8(password)}}).toJson(QJsonDocument::Compact);
}
// Uma conversa com o helper: cada mensagem é uma linha e a resposta chega antes da próxima mensagem.
struct Conversation {
    Conversation() { helper.start(QStringLiteral(GESTAOEQUIPAMENTOS_ADMIN_EXE), {"--test-profile"}); }
    QJsonObject send(const QByteArray &line) { helper.write(line + '\n'); return reply(); }
    QJsonObject reply() {
        while (!helper.canReadLine())
            if (!helper.waitForReadyRead(60000)) return {};
        const QByteArray line = helper.readLine();
        transcript += line;
        return QJsonDocument::fromJson(line).object();
    }
    // A entrada continua aberta: o helper precisa terminar sozinho.
    int exited() {
        if (!helper.waitForFinished(60000)) { helper.kill(); return -1; }
        transcript += helper.readAll();
        return helper.exitCode();
    }
    QProcess helper;
    QByteArray transcript;
};
QString replyCode(const QJsonObject &reply) { return reply.value("code").toString(); }
QString replyMessage(const QJsonObject &reply) { return reply.value("message").toString(); }
QString currentTotp(const QByteArray &secret) { return totp::code(secret, totp::counterAt(QDateTime::currentDateTimeUtc())); }
template <class F> QString refusal(F &&action) {
    try { action(); } catch (const AuthError &e) { return QString::fromUtf8(e.what()); }
    return QStringLiteral("aceito");
}

// Mesmo banco do helper, com relógio real (o TOTP do helper usa a hora do sistema).
struct InProcess {
    InProcess(const QString &path, SecretVault &vault) : database(path), auth(database, hasher), recovery(database, auth, vault) {}
    struct Binding { QStringList codes; QByteArray secret; };
    Binding enroll(const QByteArray &password) {
        const Session session = auth.login("principal", password);
        auto draft = recovery.beginEnrollment(session);
        Binding out{{}, secretOf(*draft)};
        out.codes = recovery.confirmEnrollment(session, *draft, currentTotp(out.secret));
        return out;
    }
    QString scalar(const QString &query) {
        QSqlQuery q(database.connection());
        if (!q.exec(query) || !q.next()) return QStringLiteral("ERRO");
        return q.value(0).isNull() ? QStringLiteral("<null>") : q.value(0).toString();
    }
    PasswordHasher hasher{PasswordHasher::testing()};
    LocalDatabase database;
    AuthService auth;
    RecoveryService recovery;
};
}

class AdminCliTests : public QObject {
    Q_OBJECT
    QString path;
    void removeProfileDatabase() {
        for (const auto &suffix : {"", "-wal", "-shm"}) QFile::remove(path + suffix);
    }
    // Nada mudou desde o vínculo, e o código conferido continua recuperando a conta.
    void expectUntouchedAndStillUsable(MemorySecretVault &vault, const QString &backupCode) {
        InProcess app(path, vault);
        QVERIFY(app.auth.authenticate("principal", Password).has_value());
        QCOMPARE(app.scalar("SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringLiteral("0"));
        QVERIFY(app.scalar("SELECT vault_key FROM recovery_config") != QStringLiteral("<null>"));
        QCOMPARE(app.scalar("SELECT COUNT(*) FROM events WHERE entity_type='recovery' AND action<>'Recuperação vinculada'"), QStringLiteral("0"));
        app.recovery.recoverPrincipal("principal", RecoveryMethod::BackupCode, backupCode, app.auth.preparePassword(NewPassword));
        QVERIFY(app.auth.authenticate("principal", NewPassword).has_value());
    }
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        LocalDatabase::applyApplicationIdentity();
        path = LocalDatabase::profilePath(DataProfile::Operational);
        QVERIFY(path.contains("qttest", Qt::CaseInsensitive));
    }
    void init() { removeProfileDatabase(); }
    void cleanupTestCase() { removeProfileDatabase(); }

    void bootstrapViaHelper() {
        const auto run = runHelper({"--test-profile"}, bootstrapRequest("Principal"));
        QVERIFY2(run.exitCode == 0, run.out + run.err);
        QVERIFY(!run.out.contains(Password));
        const auto reply = QJsonDocument::fromJson(run.out).object();
        QCOMPARE(reply.value("status").toString(), QStringLiteral("ok"));
        LocalDatabase db(path);
        PasswordHasher hasher(PasswordHasher::testing());
        AuthService auth(db, hasher);
        const auto actor = auth.authenticate("principal", Password);
        QVERIFY(actor.has_value());
        QCOMPARE(actor->username, QStringLiteral("principal"));
    }

    void helperRefusals() {
        QCOMPARE(runHelper({"--test-profile"}, bootstrapRequest("principal")).exitCode, 0);
        const auto second = runHelper({"--test-profile"}, bootstrapRequest("outra.principal"));
        QCOMPARE(second.exitCode, 1);
        QCOMPARE(QJsonDocument::fromJson(second.out).object().value("code").toString(), QStringLiteral("already_initialized"));
        QVERIFY(!second.out.contains(Password));
        const auto withPath = runHelper({"--test-profile", path}, bootstrapRequest("principal"));
        QCOMPARE(withPath.exitCode, 2);
        QVERIFY(!withPath.out.contains(Password));
        LocalDatabase db(path);
        QSqlQuery count(db.connection());
        QVERIFY(count.exec("SELECT COUNT(*) FROM accounts") && count.next());
        QCOMPARE(count.value(0).toInt(), 1);
    }

    void recoverWithBackupCodeViaHelper() {
        QCOMPARE(runHelper({"--test-profile"}, bootstrapRequest("Principal")).exitCode, 0);
        MemorySecretVault vault;
        QStringList codes;
        { InProcess app(path, vault); codes = app.enroll(Password).codes; }
        QCOMPARE(codes.size(), 10);

        Conversation talk;
        QCOMPARE(replyCode(talk.send(verifyRequest("backup_code", codes[0]))), QStringLiteral("verified"));
        {
            InProcess app(path, vault);
            QVERIFY(app.auth.authenticate("principal", Password).has_value());
            QCOMPARE(app.scalar("SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringLiteral("0"));
            QVERIFY(app.scalar("SELECT vault_key FROM recovery_config") != QStringLiteral("<null>"));
        }
        const auto done = talk.send(commitRequest(NewPassword));
        QCOMPARE(replyMessage(done), QStringLiteral("Senha redefinida. Entre com a nova senha. Vincule o Google Authenticator novamente no aplicativo."));
        QCOMPARE(talk.exited(), 0);
        QVERIFY(!talk.transcript.contains(NewPassword) && !talk.transcript.contains(codes[0].toUtf8()));

        InProcess app(path, vault);
        QVERIFY(app.auth.authenticate("principal", NewPassword).has_value());
        QVERIFY(!app.auth.authenticate("principal", Password).has_value());
        QCOMPARE(app.scalar("SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringLiteral("1"));
        QCOMPARE(app.scalar("SELECT vault_key FROM recovery_config"), QStringLiteral("<null>"));
        QCOMPARE(app.scalar("SELECT actor_username FROM events WHERE action='Conta principal recuperada'"), QStringLiteral("principal"));
        QCOMPARE(refusal([&] { app.recovery.recoverPrincipal("principal", RecoveryMethod::BackupCode, codes[0], app.auth.preparePassword("outra senha qualquer")); }),
                 InvalidCode);

        app.recovery.recoverPrincipal("principal", RecoveryMethod::BackupCode, codes[1], app.auth.preparePassword(Password));
        Conversation reused;
        QCOMPARE(replyMessage(reused.send(verifyRequest("backup_code", codes[1]))), InvalidCode);
        QCOMPARE(reused.exited(), 1);
        QVERIFY(app.auth.authenticate("principal", Password).has_value());
    }

    void invalidCodeRefusedAtVerify() {
        QCOMPARE(runHelper({"--test-profile"}, bootstrapRequest("Principal")).exitCode, 0);
        MemorySecretVault vault;
        { InProcess app(path, vault); app.enroll(Password); }
        Conversation talk;
        const auto refused = talk.send(verifyRequest("backup_code", "ZZZZ-ZZZZ-ZZZZ-ZZZZ"));
        QCOMPARE(replyMessage(refused), InvalidCode);
        QCOMPARE(talk.exited(), 1);
        InProcess app(path, vault);
        QVERIFY(app.auth.authenticate("principal", Password).has_value());
        QCOMPARE(app.scalar("SELECT failed_attempts FROM recovery_config"), QStringLiteral("1"));
        QCOMPARE(app.scalar("SELECT COUNT(*) FROM events WHERE action='Tentativa de recuperação recusada' AND actor_username IS NULL"),
                 QStringLiteral("1"));
        QCOMPARE(app.scalar("SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringLiteral("0"));
    }

    void endOfInputAfterVerifyChangesNothing() {
        QCOMPARE(runHelper({"--test-profile"}, bootstrapRequest("Principal")).exitCode, 0);
        MemorySecretVault vault;
        QStringList codes;
        { InProcess app(path, vault); codes = app.enroll(Password).codes; }
        Conversation talk;
        QCOMPARE(replyCode(talk.send(verifyRequest("backup_code", codes[0]))), QStringLiteral("verified"));
        talk.helper.closeWriteChannel();
        QCOMPARE(replyCode(talk.reply()), QStringLiteral("cancelled"));
        QCOMPARE(talk.exited(), 1);
        expectUntouchedAndStillUsable(vault, codes[0]);
    }

    void timeoutAfterVerifyChangesNothing() {
        QCOMPARE(runHelper({"--test-profile"}, bootstrapRequest("Principal")).exitCode, 0);
        MemorySecretVault vault;
        QStringList codes;
        {
            InProcess app(path, vault);
            codes = app.enroll(Password).codes;
            QList<int> waits;
            QList<QByteArray> replies;
            const int exitCode = AdminProtocol::serve([&](int timeoutMs) -> std::optional<QByteArray> {
                waits << timeoutMs;
                if (waits.size() == 1) return verifyRequest("backup_code", codes[0]);
                return std::nullopt;
            }, [&](const QByteArray &json) { replies << json; }, app.auth, app.recovery);
            QCOMPARE(exitCode, 1);
            QCOMPARE(waits.value(1), AdminProtocol::CommitTimeoutMs);
            QCOMPARE(replies.size(), 2);
            QCOMPARE(replyCode(QJsonDocument::fromJson(replies[1]).object()), QStringLiteral("cancelled"));
        }
        expectUntouchedAndStillUsable(vault, codes[0]);
    }

    void passwordPolicyAtCommitChangesNothing() {
        QCOMPARE(runHelper({"--test-profile"}, bootstrapRequest("Principal")).exitCode, 0);
        MemorySecretVault vault;
        QStringList codes;
        { InProcess app(path, vault); codes = app.enroll(Password).codes; }
        Conversation talk;
        QCOMPARE(replyCode(talk.send(verifyRequest("backup_code", codes[0]))), QStringLiteral("verified"));
        const auto refused = talk.send(commitRequest("curta"));
        QCOMPARE(replyCode(refused), QStringLiteral("invalid_password"));
        QCOMPARE(replyMessage(refused), QStringLiteral("A senha deve ter de 8 a 128 caracteres."));
        QCOMPARE(talk.exited(), 1);
        expectUntouchedAndStillUsable(vault, codes[0]);
    }

    void recoverWithTotpViaHelper() {
        QCOMPARE(runHelper({"--test-profile"}, bootstrapRequest("Principal")).exitCode, 0);
        KeychainSecretVault vault(vaultServiceName(DataProfile::Operational));
        InProcess app(path, vault);
        InProcess::Binding binding;
        try { binding = app.enroll(Password); }
        catch (const AuthError &) {
            if (qgetenv("GESTAOEQUIPAMENTOS_REQUIRE_VAULT") == "1") QFAIL("Cofre do sistema indisponível com GESTAOEQUIPAMENTOS_REQUIRE_VAULT=1.");
            QSKIP("Cofre do sistema indisponível neste ambiente.");
        }
        const QString key = app.scalar("SELECT vault_key FROM recovery_config");
        auto cleanup = qScopeGuard([&] { try { vault.remove(key); } catch (const VaultError &) {} });
        // O passo atual foi consumido no vínculo; o próximo ainda está na janela aceita.
        const QString code = totp::code(binding.secret, totp::counterAt(QDateTime::currentDateTimeUtc()) + 1);
        Conversation talk;
        QCOMPARE(replyCode(talk.send(verifyRequest("authenticator", code))), QStringLiteral("verified"));
        QCOMPARE(replyMessage(talk.send(commitRequest(NewPassword))), QStringLiteral("Senha redefinida. Entre com a nova senha."));
        QCOMPARE(talk.exited(), 0);
        QVERIFY(!talk.transcript.contains(code.toUtf8()) && !talk.transcript.contains(NewPassword));
        QVERIFY(app.auth.authenticate("principal", NewPassword).has_value());
        QCOMPARE(app.scalar("SELECT vault_key FROM recovery_config"), key);
        QCOMPARE(vault.read(key), binding.secret);
        Conversation replay;
        QCOMPARE(replyMessage(replay.send(verifyRequest("authenticator", code))), InvalidCode);
        QCOMPARE(replay.exited(), 1);
    }
};

QTEST_GUILESS_MAIN(AdminCliTests)
#include "AdminCliTests.moc"

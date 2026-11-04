#include <QtTest>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUrlQuery>
#include "TestSupport.h"
#include "application/RecoveryService.h"
#include "security/RecoveryCodes.h"
#include "security/Totp.h"

namespace {
const QString Unavailable = "Não foi possível preparar a recuperação. Tente novamente. Se o problema continuar, procure o responsável pela instalação.";
const QString InvalidCode = "Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método.";

struct RecoveryContext : TestContext {
    RecoveryContext(const QString &path, SecretVault &vault) : TestContext(path, true, &vault) {}
};

QString currentCode(TestContext &c, const EnrollmentDraft &draft, qint64 offset = 0) {
    return totp::code(secretOf(draft), totp::counterAt(c.clock.nowUtc()) + offset);
}
QStringList rows(LocalDatabase &db, const QString &query) {
    QStringList out; QSqlQuery q(db.connection());
    if (!q.exec(query)) return {"ERRO"};
    while (q.next()) {
        QStringList columns;
        for (int i = 0; i < q.record().count(); ++i) columns << (q.value(i).isNull() ? QStringLiteral("<null>") : q.value(i).toString());
        out << columns.join('|');
    }
    return out;
}
const QString RecoveryRow = "SELECT generation,vault_key,totp_last_counter,enrolled_at,failed_attempts FROM recovery_config";
const QString RecoveryEvents = "SELECT action,actor_username,changes FROM events WHERE entity_type='recovery' ORDER BY id";
const QStringList NotConfigured{"0|<null>|<null>|<null>|0"};
const QString Changed = "A recuperação foi configurada em outra janela. Inicie a configuração novamente.";

template <class F> AuthorizationError::Reason denial(F &&action) {
    try { action(); } catch (const AuthorizationError &e) { return e.reason(); }
    throw std::logic_error("A ação deveria ter sido recusada.");
}
template <class F> QString refusal(F &&action) {
    try { action(); } catch (const AuthError &e) { return QString::fromUtf8(e.what()); }
    throw std::logic_error("A ação deveria ter sido recusada.");
}
QString otherKey(const MemorySecretVault &vault, const QString &known) {
    for (const auto &key : vault.entries.keys()) if (key != known) return key;
    return {};
}
struct Enrolled { QStringList codes; QByteArray secret; QString key; };
Enrolled enroll(TestContext &c) {
    auto draft = c.recovery.beginEnrollment(c.session);
    Enrolled out; out.secret = secretOf(*draft);
    out.codes = c.recovery.confirmEnrollment(c.session, *draft, currentCode(c, *draft));
    out.key = rows(c.database, "SELECT vault_key FROM recovery_config").value(0);
    return out;
}
const QByteArray NewPassword = "nova senha da principal";
const QString Cooldown = "Muitas tentativas. Aguarde alguns minutos e tente novamente.";
const QString AuthenticatorUnavailable = "Não foi possível usar o código do Authenticator neste computador. Use um código de recuperação.";
const QString WrongCode = "ZZZZ-ZZZZ-ZZZZ-ZZZZ";
QString nextTotp(TestContext &c, const QByteArray &secret) {
    c.clock.advance(30);
    return totp::code(secret, totp::counterAt(c.clock.nowUtc()));
}
QString recoveryState(LocalDatabase &db) {
    return rows(db, "SELECT failed_attempts, cooldown_until, (SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL), "
                    "(SELECT COUNT(*) FROM events WHERE entity_type='recovery') FROM recovery_config").value(0);
}
const QString RecoveredEvents = "SELECT action,actor_username,changes FROM events WHERE entity_type='recovery' AND action='Conta principal recuperada'";
}

class RecoveryTests : public QObject {
    Q_OBJECT
private slots:
    void rfc6238Vectors() {
        const QByteArray secret = "12345678901234567890";
        const QList<QPair<qint64, QString>> vectors{{59, "287082"}, {1111111109, "081804"}, {1111111111, "050471"},
                                                    {1234567890, "005924"}, {2000000000, "279037"}, {20000000000, "353130"}};
        for (const auto &[seconds, expected] : vectors) {
            const auto at = QDateTime::fromSecsSinceEpoch(seconds).toUTC();
            QCOMPARE(totp::code(secret, totp::counterAt(at)), expected);
            QCOMPARE(totp::match(secret, expected, at, std::nullopt), std::optional<qint64>(totp::counterAt(at)));
        }
        const auto at = QDateTime::fromSecsSinceEpoch(1111111111).toUTC(); const qint64 now = totp::counterAt(at);
        QCOMPARE(totp::match(secret, totp::code(secret, now - 1), at, std::nullopt), std::optional<qint64>(now - 1));
        QCOMPARE(totp::match(secret, totp::code(secret, now + 1), at, std::nullopt), std::optional<qint64>(now + 1));
        QVERIFY(!totp::match(secret, totp::code(secret, now - 2), at, std::nullopt));
        QVERIFY(!totp::match(secret, totp::code(secret, now), at, now));
        QVERIFY(!totp::match(secret, totp::code(secret, now - 1), at, now - 1));
        QCOMPARE(totp::base32("12345678901234567890"), QString("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"));
        QCOMPARE(totp::base32("f"), QString("MY"));
        QCOMPARE(totp::otpauthUri("ana silva", "12345678901234567890"),
                 QString("otpauth://totp/GestaoEquipamentos:ana%20silva?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=GestaoEquipamentos&algorithm=SHA1&digits=6&period=30"));
    }
    void enrollmentHappyPath() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        QVERIFY(!c.recovery.status(c.session).configured);
        auto draft = c.recovery.beginEnrollment(c.session);
        QVERIFY(draft->uri().startsWith("otpauth://totp/GestaoEquipamentos:principal?secret="));
        QCOMPARE(vault.entries.size(), 1);
        const QString key = vault.entries.keys().first();
        QVERIFY2(key.startsWith(QString("recovery/%1/%2/1/").arg(c.database.databaseUuid()).arg(c.session.accountId())), qPrintable(key));
        const QByteArray secret = secretOf(*draft); QCOMPARE(secret.size(), 20); QCOMPARE(vault.entries.value(key), secret);
        QVERIFY(!c.recovery.status(c.session).configured);
        QCOMPARE(rows(c.database, RecoveryRow), QStringList{"0|<null>|<null>|<null>|0"});

        const qint64 counter = totp::counterAt(c.clock.nowUtc());
        c.recovery.confirmEnrollment(c.session, *draft, currentCode(c, *draft));
        const auto status = c.recovery.status(c.session);
        QVERIFY(status.configured); QCOMPARE(status.generation, qint64(1)); QCOMPARE(status.enrolledAtUtc, c.clock.nowUtc());
        QCOMPARE(rows(c.database, RecoveryRow),
                 QStringList{QString("1|%1|%2|%3|0").arg(key).arg(counter).arg(c.clock.nowUtc().toString(Qt::ISODateWithMs))});
        QCOMPARE(rows(c.database, RecoveryEvents),
                 QStringList{R"(Recuperação vinculada|principal|{"generation":{"de":0,"para":1},"method":{"de":null,"para":"totp"}})"});
        QCOMPARE(vault.entries.keys(), QStringList{key});
        QVERIFY(draft->uri().isEmpty());
    }
    void onlyPrincipalEnrolls() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        c.accounts.createAdministrator(c.session, "gestor", c.auth.preparePassword("senha-gestor"), InitialPasswordMode::Assisted);
        const Session admin = c.auth.login("gestor", "senha-gestor");
        const Session common = c.createCommonAndLogin("operador", "senha-operador", InitialPasswordMode::Assisted);
        auto draft = c.recovery.beginEnrollment(c.session);
        for (const Session *other : {&admin, &common}) {
            QCOMPARE(denial([&] { c.recovery.beginEnrollment(*other); }), AuthorizationError::Reason::Forbidden);
            QCOMPARE(denial([&] { c.recovery.confirmEnrollment(*other, *draft, currentCode(c, *draft)); }), AuthorizationError::Reason::Forbidden);
            QCOMPARE(denial([&] { c.recovery.status(*other); }), AuthorizationError::Reason::Forbidden);
        }
        QCOMPARE(vault.entries.size(), 1);
        QCOMPARE(rows(c.database, RecoveryRow), NotConfigured); QVERIFY(rows(c.database, RecoveryEvents).isEmpty());
        c.clock.advance(AdminSessionDuration / 1000);
        QCOMPARE(denial([&] { c.recovery.confirmEnrollment(c.session, *draft, currentCode(c, *draft)); }), AuthorizationError::Reason::Expired);
        QCOMPARE(denial([&] { c.recovery.beginEnrollment(c.session); }), AuthorizationError::Reason::Expired);
        QCOMPARE(rows(c.database, RecoveryRow), NotConfigured); QVERIFY(rows(c.database, RecoveryEvents).isEmpty());
    }
    void vaultWriteOrReadFailure() {
        using Stage = FailingSecretVault::Stage;
        for (const Stage stage : {Stage::Write, Stage::Read, Stage::Corrupt}) {
            QTemporaryDir temp; FailingSecretVault vault; vault.failOn = stage; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
            QCOMPARE(refusal([&] { c.recovery.beginEnrollment(c.session); }), Unavailable);
            QVERIFY(vault.entries.isEmpty());
            QCOMPARE(rows(c.database, RecoveryRow), NotConfigured); QVERIFY(rows(c.database, RecoveryEvents).isEmpty());
            QVERIFY(!c.recovery.status(c.session).configured);
        }
    }
    void cancelKeepsPreviousBinding() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        auto first = c.recovery.beginEnrollment(c.session);
        const QByteArray firstSecret = secretOf(*first); const QString activeKey = vault.entries.keys().first();
        c.recovery.confirmEnrollment(c.session, *first, currentCode(c, *first));
        const auto before = rows(c.database, RecoveryRow);
        c.clock.advance(60);
        auto second = c.recovery.beginEnrollment(c.session);
        const QString candidate = otherKey(vault, activeKey);
        QCOMPARE(vault.entries.size(), 2); QVERIFY2(candidate.contains("/2/"), qPrintable(candidate));
        c.recovery.cancelEnrollment(*second);
        QCOMPARE(vault.entries.keys(), QStringList{activeKey}); QCOMPARE(vault.read(activeKey), firstSecret);
        QCOMPARE(rows(c.database, RecoveryRow), before); QCOMPARE(c.recovery.status(c.session).generation, qint64(1));
        QVERIFY(second->uri().isEmpty());
        QCOMPARE(refusal([&] { c.recovery.confirmEnrollment(c.session, *second, totp::code(firstSecret, totp::counterAt(c.clock.nowUtc()))); }), InvalidCode);
        QCOMPARE(rows(c.database, RecoveryEvents).size(), 1);
    }
    void wrongCodeDoesNotActivate() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        auto draft = c.recovery.beginEnrollment(c.session);
        const QByteArray secret = secretOf(*draft); const qint64 now = totp::counterAt(c.clock.nowUtc());
        const QStringList accepted{totp::code(secret, now - 1), totp::code(secret, now), totp::code(secret, now + 1)};
        QString wrong;
        for (int n = 0; wrong.isEmpty() || accepted.contains(wrong); ++n) wrong = QString("%1").arg(n, 6, 10, QLatin1Char('0'));
        QStringList refused{wrong, "12345", "abcdef", ""};
        if (!accepted.contains(totp::code(secret, now - 2))) refused << totp::code(secret, now - 2);
        for (const auto &code : refused) {
            QCOMPARE(refusal([&] { c.recovery.confirmEnrollment(c.session, *draft, code); }), InvalidCode);
            QVERIFY(!c.recovery.status(c.session).configured);
            QCOMPARE(rows(c.database, RecoveryRow), NotConfigured); QVERIFY(rows(c.database, RecoveryEvents).isEmpty());
        }
        c.recovery.confirmEnrollment(c.session, *draft, totp::code(secret, now));
        const qint64 last = rows(c.database, "SELECT totp_last_counter FROM recovery_config").first().toLongLong();
        QCOMPARE(last, now);
        QVERIFY(!totp::match(secret, totp::code(secret, now), c.clock.nowUtc(), last));
        QVERIFY(!totp::match(secret, totp::code(secret, now - 1), c.clock.nowUtc(), last));
        QCOMPARE(refusal([&] { c.recovery.confirmEnrollment(c.session, *draft, totp::code(secret, now)); }), InvalidCode);
        QCOMPARE(rows(c.database, RecoveryEvents).size(), 1);
    }
    void generationRaceRejected() {
        QTemporaryDir temp; MemorySecretVault vault; const auto path = temp.filePath("notebooks.sqlite");
        RecoveryContext a(path, vault); RecoveryContext b(path, vault);
        auto draftA = a.recovery.beginEnrollment(a.session); const QString keyA = vault.entries.keys().first();
        auto draftB = b.recovery.beginEnrollment(b.session); const QString keyB = otherKey(vault, keyA);
        QCOMPARE(vault.entries.size(), 2); QVERIFY(keyA.contains("/1/")); QVERIFY(keyB.contains("/1/"));
        const QByteArray secretA = secretOf(*draftA);
        a.recovery.confirmEnrollment(a.session, *draftA, currentCode(a, *draftA));
        QCOMPARE(refusal([&] { b.recovery.confirmEnrollment(b.session, *draftB, currentCode(b, *draftB)); }), Changed);
        QCOMPARE(vault.entries.keys(), QStringList{keyA}); QCOMPARE(vault.read(keyA), secretA);
        QCOMPARE(rows(b.database, "SELECT generation,vault_key FROM recovery_config"), QStringList{"1|" + keyA});
        QCOMPARE(rows(b.database, RecoveryEvents).size(), 1);
        QVERIFY(draftB->uri().isEmpty());
    }
    void secretNeverPersisted() {
        QTemporaryDir temp; const auto path = temp.filePath("notebooks.sqlite"); MemorySecretVault vault;
        QByteArray secret; QByteArray encoded;
        const auto leaks = [&] {
            QStringList found;
            for (const auto &file : {path, path + "-wal"}) {
                QFile f(file);
                if (!f.open(QIODevice::ReadOnly)) continue;
                const QByteArray bytes = f.readAll();
                if (bytes.contains(secret) || bytes.contains(encoded) || bytes.contains(encoded.toLower()) || bytes.contains("otpauth")) found << file;
            }
            return found;
        };
        {
            RecoveryContext c(path, vault);
            auto draft = c.recovery.beginEnrollment(c.session);
            secret = secretOf(*draft); encoded = totp::base32(secret).toLatin1(); QCOMPARE(secret.size(), 20);
            c.recovery.confirmEnrollment(c.session, *draft, currentCode(c, *draft));
            QVERIFY(QFileInfo::exists(path + "-wal"));
            QCOMPARE(leaks(), QStringList{});
            QSqlQuery tables(c.database.connection());
            QVERIFY(tables.exec("SELECT name FROM sqlite_master WHERE type='table'"));
            int scanned = 0;
            while (tables.next()) {
                QSqlQuery q(c.database.connection());
                QVERIFY(q.exec("SELECT * FROM \"" + tables.value(0).toString() + "\""));
                while (q.next()) for (int i = 0; i < q.record().count(); ++i) {
                    const QVariant value = q.value(i); ++scanned;
                    QVERIFY(!value.toByteArray().contains(secret));
                    QVERIFY(!value.toString().contains(QString::fromLatin1(encoded), Qt::CaseInsensitive));
                }
            }
            QVERIFY(scanned > 0);
            const auto changes = rows(c.database, "SELECT changes FROM events WHERE entity_type='recovery'");
            QCOMPARE(changes.size(), 1);
            QCOMPARE(QJsonDocument::fromJson(changes.first().toUtf8()).object().keys(), (QStringList{"generation", "method"}));
        }
        QCOMPARE(leaks(), QStringList{});
    }
    void recoverWithBackupCode() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        const Enrolled e = enroll(c);
        QCOMPARE(e.codes.size(), recovery_codes::SetSize);
        const QRegularExpression format("^[0-9A-HJKMNP-TV-Z]{4}(-[0-9A-HJKMNP-TV-Z]{4}){3}$");
        for (const auto &code : e.codes) QVERIFY2(format.match(code).hasMatch(), qPrintable(code));
        QCOMPARE(QSet<QString>(e.codes.begin(), e.codes.end()).size(), e.codes.size());
        QCOMPARE(rows(c.database, "SELECT COUNT(*) FROM recovery_codes WHERE generation=1 AND used_at IS NULL"), QStringList{"10"});

        c.recovery.recoverPrincipal(" Principal ", RecoveryMethod::BackupCode, e.codes[0].toLower().replace('-', ' '), c.auth.preparePassword(NewPassword));
        QVERIFY_THROWS_EXCEPTION(AuthError, c.auth.login(TestPrincipal, TestPassword));
        const Session fresh = c.auth.login(TestPrincipal, NewPassword);
        QCOMPARE(denial([&] { c.recovery.status(c.session); }), AuthorizationError::Reason::Revoked);
        const auto status = c.recovery.status(fresh);
        QVERIFY(!status.configured); QVERIFY(status.needsRebinding); QCOMPARE(status.generation, qint64(1));
        QCOMPARE(rows(c.database, RecoveryRow), QStringList{"1|<null>|<null>|<null>|0"});
        QVERIFY(vault.entries.isEmpty());
        QCOMPARE(c.recovery.remainingBackupCodes(fresh), 9);
        QCOMPARE(rows(c.database, RecoveredEvents),
                 QStringList{R"(Conta principal recuperada|principal|{"method":{"de":null,"para":"backup_code"},"totp_enrolled":{"de":1,"para":0}})"});
    }
    void backupCodeSingleUse() {
        QTemporaryDir temp; const auto path = temp.filePath("notebooks.sqlite"); MemorySecretVault vault;
        QString used;
        {
            RecoveryContext c(path, vault);
            used = enroll(c).codes[3];
            c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, used, c.auth.preparePassword(NewPassword));
            QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, used, c.auth.preparePassword("outra senha qualquer")); }), InvalidCode);
        }
        TestContext reopened(path, false, &vault);
        QCOMPARE(refusal([&] { reopened.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, used, reopened.auth.preparePassword("outra senha qualquer")); }), InvalidCode);
        TestContext second(path, false, &vault);
        QCOMPARE(refusal([&] { second.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, used, second.auth.preparePassword("outra senha qualquer")); }), InvalidCode);
        QCOMPARE(rows(second.database, "SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringList{"1"});
        QCOMPARE(rows(second.database, RecoveredEvents).size(), 1);
        second.auth.login(TestPrincipal, NewPassword);
    }
    void recoverWithTotp() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        const Enrolled e = enroll(c);
        const QString enrolledAt = rows(c.database, "SELECT enrolled_at FROM recovery_config").value(0);
        const QString code = nextTotp(c, e.secret);
        c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, code, c.auth.preparePassword(NewPassword));
        QVERIFY_THROWS_EXCEPTION(AuthError, c.auth.login(TestPrincipal, TestPassword));
        const Session fresh = c.auth.login(TestPrincipal, NewPassword);
        QCOMPARE(rows(c.database, "SELECT vault_key, enrolled_at, totp_last_counter FROM recovery_config"),
                 QStringList{QString("%1|%2|%3").arg(e.key, enrolledAt).arg(totp::counterAt(c.clock.nowUtc()))});
        QCOMPARE(vault.entries.keys(), QStringList{e.key}); QCOMPARE(vault.entries.value(e.key), e.secret);
        QVERIFY(c.recovery.status(fresh).configured); QVERIFY(!c.recovery.status(fresh).needsRebinding);
        QCOMPARE(c.recovery.remainingBackupCodes(fresh), 10);
        QCOMPARE(rows(c.database, RecoveredEvents), QStringList{R"(Conta principal recuperada|principal|{"method":{"de":null,"para":"authenticator"}})"});
    }
    void verifiedTotpSurvivesTypingDelay() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        const Enrolled e = enroll(c);
        const QString code = nextTotp(c, e.secret);
        const QString before = recoveryState(c.database);
        const auto verified = c.recovery.verifyRecovery(TestPrincipal, RecoveryMethod::Authenticator, code);
        QCOMPARE(recoveryState(c.database), before);
        c.auth.login(TestPrincipal, TestPassword);
        // O código já saiu da janela pelo relógio, mas foi conferido a tempo.
        c.clock.advance(120);
        c.recovery.completeRecovery(*verified, c.auth.preparePassword(NewPassword));
        c.auth.login(TestPrincipal, NewPassword);
        QCOMPARE(rows(c.database, "SELECT vault_key FROM recovery_config"), QStringList{e.key});
        QCOMPARE(refusal([&] { c.recovery.completeRecovery(*verified, c.auth.preparePassword("senha de replay")); }), InvalidCode);

        const auto stale = c.recovery.verifyRecovery(TestPrincipal, RecoveryMethod::Authenticator, nextTotp(c, e.secret));
        const QString beforeStale = recoveryState(c.database);
        c.clock.advance(RecoveryConfirmSeconds + 1);
        QCOMPARE(refusal([&] { c.recovery.completeRecovery(*stale, c.auth.preparePassword("senha atrasada")); }), InvalidCode);
        QCOMPARE(recoveryState(c.database), beforeStale);
        c.auth.login(TestPrincipal, NewPassword);
    }
    void backupCodeRecoveryInvalidatesTotp() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        {
            const Enrolled e = enroll(c);
            QSqlQuery block(c.database.connection());
            QVERIFY(block.exec("CREATE TRIGGER block_events BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'bloqueado'); END"));
            QVERIFY_THROWS_EXCEPTION(std::exception, c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[0], c.auth.preparePassword(NewPassword)));
            QVERIFY(block.exec("DROP TRIGGER block_events"));
            QCOMPARE(rows(c.database, "SELECT vault_key FROM recovery_config"), QStringList{e.key});
            QCOMPARE(rows(c.database, "SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringList{"0"});
            QCOMPARE(vault.entries.keys(), QStringList{e.key});
            c.auth.login(TestPrincipal, TestPassword);

            c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[0], c.auth.preparePassword(NewPassword));
            QCOMPARE(rows(c.database, "SELECT vault_key, enrolled_at, totp_last_counter FROM recovery_config"), QStringList{"<null>|<null>|<null>"});
            QCOMPARE(rows(c.database, RecoveredEvents),
                     QStringList{R"(Conta principal recuperada|principal|{"method":{"de":null,"para":"backup_code"},"totp_enrolled":{"de":1,"para":0}})"});
            QVERIFY(vault.entries.isEmpty());
            vault.entries.insert(e.key, e.secret);
            const QString before = recoveryState(c.database);
            QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, nextTotp(c, e.secret), c.auth.preparePassword("senha do segredo antigo")); }),
                     AuthenticatorUnavailable);
            QCOMPARE(recoveryState(c.database), before);
            vault.entries.clear();
            const Session fresh = c.auth.login(TestPrincipal, NewPassword);
            QVERIFY(c.recovery.status(fresh).needsRebinding); QCOMPARE(c.recovery.remainingBackupCodes(fresh), 9);
            c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[1], c.auth.preparePassword("segunda senha recuperada"));
            QCOMPARE(rows(c.database, RecoveredEvents).last(), QString(R"(Conta principal recuperada|principal|{"method":{"de":null,"para":"backup_code"}})"));
            c.session = c.auth.login(TestPrincipal, "segunda senha recuperada");
            const Enrolled again = enroll(c);
            const auto status = c.recovery.status(c.session);
            QCOMPARE(status.generation, qint64(2)); QVERIFY(status.configured); QVERIFY(!status.needsRebinding);
            QCOMPARE(c.recovery.remainingBackupCodes(c.session), 10);
            QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[2], c.auth.preparePassword(NewPassword)); }), InvalidCode);
            QVERIFY(!again.codes.contains(e.codes[2]));
        }
    }
    void totpReplayRejected() {
        QTemporaryDir temp; const auto path = temp.filePath("notebooks.sqlite"); MemorySecretVault vault;
        QString accepted, previous;
        {
            RecoveryContext c(path, vault);
            const Enrolled e = enroll(c);
            accepted = nextTotp(c, e.secret);
            previous = totp::code(e.secret, totp::counterAt(c.clock.nowUtc()) - 1);
            c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, accepted, c.auth.preparePassword(NewPassword));
            QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, accepted, c.auth.preparePassword("senha de replay")); }), InvalidCode);
        }
        for (int instance = 0; instance < 2; ++instance) {
            TestContext other(path, false, &vault); other.clock.advance(30);
            QCOMPARE(refusal([&] { other.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, accepted, other.auth.preparePassword("senha de replay")); }), InvalidCode);
            QCOMPARE(refusal([&] { other.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, previous, other.auth.preparePassword("senha de replay")); }), InvalidCode);
        }
        TestContext check(path, false, &vault);
        QCOMPARE(rows(check.database, RecoveredEvents).size(), 1);
        check.auth.login(TestPrincipal, NewPassword);
    }
    void concurrentBackupCode() {
        QTemporaryDir temp; const auto path = temp.filePath("notebooks.sqlite"); MemorySecretVault vault;
        RecoveryContext a(path, vault); RecoveryContext b(path, vault);
        const Enrolled e = enroll(a);
        a.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[5], a.auth.preparePassword(NewPassword));
        QCOMPARE(refusal([&] { b.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[5], b.auth.preparePassword("senha da outra janela")); }), InvalidCode);
        QCOMPARE(rows(b.database, "SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringList{"1"});
        QSqlQuery reset(b.database.connection());
        QVERIFY(!reset.exec("UPDATE recovery_codes SET used_at=NULL WHERE used_at IS NOT NULL"));
        QCOMPARE(rows(b.database, "SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringList{"1"});
        b.auth.login(TestPrincipal, NewPassword);
    }
    void wrongUsernameGeneric() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        const Enrolled e = enroll(c);
        c.createCommonAndLogin("operador", "senha do operador", InitialPasswordMode::Assisted);
        for (const QString &name : {QString("operador"), QString("inexistente"), QString("x"), QString()})
            QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(name, RecoveryMethod::BackupCode, e.codes[0], c.auth.preparePassword(NewPassword)); }), InvalidCode);
        QCOMPARE(rows(c.database, "SELECT COUNT(*) FROM recovery_codes WHERE used_at IS NOT NULL"), QStringList{"0"});
        QCOMPARE(c.recovery.remainingBackupCodes(c.session), 10);
        c.auth.login(TestPrincipal, TestPassword);
    }
    void cooldownAfterFiveFailures() {
        QTemporaryDir temp; const auto path = temp.filePath("notebooks.sqlite"); MemorySecretVault vault;
        const QByteArray typedPassword = "senha nova digitada";
        QString until, valid;
        {
            RecoveryContext c(path, vault);
            const Enrolled e = enroll(c); valid = e.codes[0];
            const QStringList names{TestPrincipal, "intruso.digitado", TestPrincipal, TestPrincipal, TestPrincipal};
            for (const auto &name : names)
                QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(name, RecoveryMethod::BackupCode, WrongCode, c.auth.preparePassword(typedPassword)); }), InvalidCode);
            until = c.clock.nowUtc().addSecs(RecoveryCooldownSeconds).toString(Qt::ISODateWithMs);
            QStringList expected;
            for (int n = 0; n < 4; ++n)
                expected << QString(R"(Tentativa de recuperação recusada|<null>|<null>|Prova de recuperação recusada.|{"failed_attempts":{"de":%1,"para":%2}})").arg(n).arg(n + 1);
            expected << QString(R"(Recuperação bloqueada temporariamente|<null>|<null>|Cinco tentativas recusadas; novas tentativas bloqueadas por 5 minutos.|{"cooldown_until":{"de":null,"para":"%1"},"failed_attempts":{"de":4,"para":0}})").arg(until);
            const QString attempts = "SELECT action,actor_account_id,actor_username,details,changes FROM events WHERE entity_type='recovery' AND action<>'Recuperação vinculada' ORDER BY id";
            QCOMPARE(rows(c.database, attempts), expected);
            for (const auto &line : rows(c.database, "SELECT details || changes FROM events"))
                for (const QString &leak : {QString("ZZZZ"), QString("intruso"), QString::fromUtf8(typedPassword), QString("$argon2id")})
                    QVERIFY2(!line.contains(leak, Qt::CaseInsensitive), qPrintable(line));
            const QString before = recoveryState(c.database);
            QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, valid, c.auth.preparePassword(NewPassword)); }), Cooldown);
            QCOMPARE(recoveryState(c.database), before);
        }
        TestContext reopened(path, false, &vault);
        QCOMPARE(rows(reopened.database, "SELECT failed_attempts, cooldown_until FROM recovery_config"), QStringList{"0|" + until});
        reopened.clock.advance(RecoveryCooldownSeconds - 1);
        QCOMPARE(refusal([&] { reopened.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, valid, reopened.auth.preparePassword(NewPassword)); }), Cooldown);
        reopened.clock.advance(1);
        reopened.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, valid, reopened.auth.preparePassword(NewPassword));
        QCOMPARE(rows(reopened.database, "SELECT failed_attempts, cooldown_until FROM recovery_config"), QStringList{"0|<null>"});
        reopened.auth.login(TestPrincipal, NewPassword);
    }
    void invalidNewPasswordDoesNotConsume() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        enroll(c);
        const QString before = recoveryState(c.database);
        QVERIFY_THROWS_EXCEPTION(AuthError, c.auth.preparePassword("curta77"));
        QVERIFY_THROWS_EXCEPTION(AuthError, c.auth.preparePassword(QByteArray(129, 'a')));
        QCOMPARE(recoveryState(c.database), before);
        QCOMPARE(c.recovery.remainingBackupCodes(c.session), 10);
    }
    void recoveryRevokesSessions() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        const Enrolled e = enroll(c);
        const Session other = c.auth.login(TestPrincipal, TestPassword);
        c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, nextTotp(c, e.secret), c.auth.preparePassword(NewPassword));
        for (const Session *open : std::initializer_list<const Session *>{&c.session, &other})
            QCOMPARE(denial([&] { c.recovery.status(*open); }), AuthorizationError::Reason::Revoked);
    }
    void vaultLostBackupStillWorks() {
        QTemporaryDir temp; MemorySecretVault vault; RecoveryContext c(temp.filePath("notebooks.sqlite"), vault);
        const Enrolled e = enroll(c);
        vault.entries.clear();
        const QString before = recoveryState(c.database);
        QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator, nextTotp(c, e.secret), c.auth.preparePassword(NewPassword)); }),
                 AuthenticatorUnavailable);
        QCOMPARE(recoveryState(c.database), before);
        c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[9], c.auth.preparePassword(NewPassword));
        c.auth.login(TestPrincipal, NewPassword);
    }
    void codesNeverPersisted() {
        QTemporaryDir temp; const auto path = temp.filePath("notebooks.sqlite"); MemorySecretVault vault;
        QStringList forms;
        {
            RecoveryContext c(path, vault);
            const Enrolled e = enroll(c);
            for (const auto &code : e.codes) forms << code << recovery_codes::normalize(code);
            c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[0], c.auth.preparePassword(NewPassword));
            QCOMPARE(refusal([&] { c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, e.codes[1] + "X", c.auth.preparePassword(NewPassword)); }), InvalidCode);
            QVERIFY(QFileInfo::exists(path + "-wal"));
            for (const auto &file : {path, path + "-wal"}) {
                QFile f(file); QVERIFY(f.open(QIODevice::ReadOnly));
                const QByteArray bytes = f.readAll();
                for (const auto &form : forms) QVERIFY2(!bytes.contains(form.toLatin1()), qPrintable(file));
            }
            QSqlQuery tables(c.database.connection());
            QVERIFY(tables.exec("SELECT name FROM sqlite_master WHERE type='table'"));
            while (tables.next()) {
                QSqlQuery q(c.database.connection());
                QVERIFY(q.exec("SELECT * FROM \"" + tables.value(0).toString() + "\""));
                while (q.next()) for (int i = 0; i < q.record().count(); ++i)
                    for (const auto &form : forms) QVERIFY(!q.value(i).toString().contains(form, Qt::CaseInsensitive));
            }
            for (const auto &changes : rows(c.database, "SELECT changes FROM events WHERE changes IS NOT NULL"))
                for (const auto &key : QJsonDocument::fromJson(changes.toUtf8()).object().keys())
                    QVERIFY2(LocalDatabase::changeFieldAllowlist().contains(key), qPrintable(key));
        }
    }
};

QTEST_GUILESS_MAIN(RecoveryTests)
#include "RecoveryTests.moc"

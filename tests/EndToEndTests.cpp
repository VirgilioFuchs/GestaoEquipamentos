#include <QtTest>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include "TestSupport.h"
#include "security/Totp.h"

namespace {
const QString InvalidCode = "Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método.";
const QString AuthenticatorUnavailable =
    "Não foi possível usar o código do Authenticator neste computador. Use um código de recuperação.";

QStringList rows(LocalDatabase &db, const QString &query) {
    QStringList out;
    QSqlQuery q(db.connection());
    if (!q.exec(query))
        return {"ERRO"};
    while (q.next()) {
        QStringList columns;
        for (int i = 0; i < q.record().count(); ++i)
            columns << (q.value(i).isNull() ? QStringLiteral("<null>") : q.value(i).toString());
        out << columns.join('|');
    }
    return out;
}
template <class F>
QString refusal(F &&action) {
    try {
        action();
    } catch (const AuthError &e) {
        return QString::fromUtf8(e.what());
    }
    return QStringLiteral("aceito");
}
struct Binding {
    QStringList codes;
    QByteArray secret;
};
Binding enroll(TestContext &c, const Session &session) {
    auto draft = c.recovery.beginEnrollment(session);
    Binding out{{}, secretOf(*draft)};
    out.codes =
        c.recovery.confirmEnrollment(session, *draft, totp::code(out.secret, totp::counterAt(c.clock.nowUtc())));
    return out;
}
// Procura cada segredo nos bytes do banco e do WAL; devolve os encontrados.
QStringList leaks(const QString &path, const QList<QByteArray> &needles) {
    QStringList found;
    for (const QString &file : {path, path + "-wal"}) {
        QFile f(file);
        if (!f.exists())
            continue;
        if (!f.open(QIODevice::ReadOnly))
            return {"sem leitura: " + file};
        const QByteArray bytes = f.readAll();
        for (const auto &needle : needles)
            if (!needle.isEmpty() && bytes.contains(needle))
                found << QString::fromUtf8(needle.left(4)) + "… em " + QFileInfo(file).fileName();
    }
    return found;
}
} // namespace

class EndToEndTests : public QObject {
    Q_OBJECT
private slots:
    void fullPhaseScenario() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("notebooks.sqlite");
        const QByteArray Temporary = "senha temporaria do operador", Personal = "senha pessoal do operador",
                         AdminPassword = "senha do administrador um", Recovered = "senha recuperada da principal";
        MemorySecretVault vault;
        QList<QByteArray> secrets{TestPassword, Temporary, Personal, AdminPassword, Recovered, "otpauth"};
        {
            TestContext c(path, false, &vault);
            c.auth.bootstrapPrincipal(TestPrincipal, c.auth.preparePassword(TestPassword));
            c.session = c.auth.login(TestPrincipal, TestPassword);
            Session common = c.createCommonAndLogin("operador.um", Temporary);
            QVERIFY(common.mustChangePassword());
            PasswordChangeAttempt attempt = c.auth.prepareRequiredPasswordChange(common);
            attempt.prepare(Personal);
            c.auth.completeRequiredPasswordChange(common, attempt);
            Fixture::add(c, c.session, "NB-E2E", "Dell Latitude", "Armário 1");
            Fixture::checkout(c, common, c.store.notebooks().first().id, "Ana", ResponsibleRole::Professor, "Sala 4");
            const qint64 admin = c.accounts.createAdministrator(
                c.session, "admin.um", c.auth.preparePassword(AdminPassword), InitialPasswordMode::Temporary);
            c.accounts.inactivateAdministrator(c.session, admin);
            c.accounts.inactivateCommonAccount(
                c.session,
                rows(c.database, "SELECT id FROM accounts WHERE username='operador.um'").value(0).toLongLong());
            const Binding binding = enroll(c, c.session);
            QCOMPARE(binding.codes.size(), 10);
            c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, binding.codes[0],
                                        c.auth.preparePassword(Recovered));
            c.auth.login(TestPrincipal, Recovered);
            secrets << binding.secret << totp::base32(binding.secret).toLatin1();
            for (const auto &code : binding.codes)
                secrets << code.toLatin1() << QString(code).remove('-').toLatin1();

            QCOMPARE(
                rows(c.database, "SELECT action, actor_username FROM events WHERE entity_type<>'system' ORDER BY id"),
                (QStringList{"Conta principal criada|principal", "Conta criada|principal", "Senha definida|operador.um",
                             "Cadastro|principal", "Retirada|operador.um", "Administrador criado|principal",
                             "Administrador inativado|principal", "Conta inativada|principal",
                             "Recuperação vinculada|principal", "Conta principal recuperada|principal"}));
            QCOMPARE(rows(c.database, "SELECT COUNT(*) FROM events WHERE entity_type<>'system' AND (actor_account_id "
                                      "IS NULL OR actor_username IS NULL)"),
                     QStringList{"0"});
            QStringList accountChanges;
            for (const auto &entry : c.store.history())
                if (entry.action == "Conta inativada" || entry.action == "Conta principal recuperada")
                    accountChanges << entry.action + "|" + entry.actorUsername + "|" +
                                          QString::fromUtf8(
                                              QJsonDocument(entry.changes).toJson(QJsonDocument::Compact));
            QCOMPARE(
                accountChanges,
                (QStringList{
                    R"(Conta principal recuperada|principal|{"method":{"de":null,"para":"backup_code"},"totp_enrolled":{"de":1,"para":0}})",
                    R"(Conta inativada|principal|{"active":{"de":1,"para":0}})"}));
            QVERIFY(QFileInfo::exists(path + "-wal"));
            const QStringList open = leaks(path, secrets);
            QVERIFY2(open.isEmpty(), qPrintable(open.join(", ")));
        }
        const QStringList closed = leaks(path, secrets);
        QVERIFY2(closed.isEmpty(), qPrintable(closed.join(", ")));
    }

    void restoreFromCopyWithoutVault() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString original = temp.filePath("notebooks.sqlite");
        MemorySecretVault vault;
        Binding binding;
        {
            TestContext c(original, true, &vault);
            binding = enroll(c, c.session);
            Fixture::add(c, c.session, "NB-R1", "Dell", "Armário 1");
            Fixture::checkout(c, c.session, c.store.notebooks().first().id, "Ana", ResponsibleRole::Professor,
                              "Sala 2");
        }
        QVERIFY(QDir(temp.path()).mkpath("restaurado"));
        const QString copy = temp.filePath("restaurado/notebooks.sqlite");
        for (const QString suffix : {"", "-wal"})
            if (QFileInfo::exists(original + suffix))
                QVERIFY(QFile::copy(original + suffix, copy + suffix));

        MemorySecretVault empty;
        TestContext c(copy, false, &empty);
        c.clock.advance(30);
        QCOMPARE(refusal([&] {
                     c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator,
                                                 totp::code(binding.secret, totp::counterAt(c.clock.nowUtc())),
                                                 c.auth.preparePassword("senha pelo celular"));
                 }),
                 AuthenticatorUnavailable);
        c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, binding.codes[0],
                                    c.auth.preparePassword("senha apos restaurar"));
        Session session = c.auth.login(TestPrincipal, "senha apos restaurar");
        QVERIFY(c.recovery.status(session).needsRebinding);
        QCOMPARE(c.store.notebooks().first().borrower, QString("Ana"));

        const Binding rebound = enroll(c, session);
        QCOMPARE(c.recovery.status(session).generation, qint64(2));
        QVERIFY(!c.recovery.status(session).needsRebinding);
        QCOMPARE(rebound.codes.size(), 10);
        for (const auto &code : rebound.codes)
            QVERIFY(!binding.codes.contains(code));
        QCOMPARE(refusal([&] {
                     c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, binding.codes[1],
                                                 c.auth.preparePassword("senha com codigo antigo"));
                 }),
                 InvalidCode);

        c.clock.advance(30);
        c.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::Authenticator,
                                    totp::code(rebound.secret, totp::counterAt(c.clock.nowUtc())),
                                    c.auth.preparePassword("senha pelo novo vinculo"));
        session = c.auth.login(TestPrincipal, "senha pelo novo vinculo");
        QVERIFY(c.recovery.status(session).configured);
        QCOMPARE(c.recovery.remainingBackupCodes(session), 10);
        QCOMPARE(empty.entries.size(), 1);
    }
};

QTEST_GUILESS_MAIN(EndToEndTests)
#include "EndToEndTests.moc"

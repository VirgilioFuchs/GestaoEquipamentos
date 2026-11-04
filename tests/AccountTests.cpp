#include <QtTest>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "TestSupport.h"

namespace {
const QByteArray Temporary = QByteArrayLiteral("senha temporaria do operador");
const QByteArray Personal = QByteArrayLiteral("frase pessoal do operador um");
const QByteArray Reset = QByteArrayLiteral("nova senha temporaria redefinida");
template <typename F>
QString refusal(F &&operation, AuthorizationError::Reason expected) {
    try {
        operation();
    } catch (const AuthorizationError &error) {
        return error.reason() == expected ? QString()
                                          : QStringLiteral("motivo inesperado: ") + QString::fromUtf8(error.what());
    } catch (const std::exception &error) {
        return QStringLiteral("outra exceção: ") + QString::fromUtf8(error.what());
    }
    return QStringLiteral("operação aceita");
}
template <typename F>
QString errorCode(F &&operation) {
    try {
        operation();
    } catch (const AuthError &error) {
        return error.code() + "|" + QString::fromUtf8(error.what());
    } catch (const std::exception &error) {
        return QStringLiteral("outra exceção: ") + QString::fromUtf8(error.what());
    }
    return QStringLiteral("operação aceita");
}
bool execAux(const QString &path, const QString &statement) {
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "aux");
        db.setDatabaseName(path);
        QSqlQuery q(db);
        ok = db.open() && q.exec(statement);
    }
    QSqlDatabase::removeDatabase("aux");
    return ok;
}
QJsonObject json(const char *text) {
    return QJsonDocument::fromJson(QByteArray(text)).object();
}
} // namespace

class AccountTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString freshPath(const QString &name) { return dir.filePath(name + ".sqlite"); }
    static qint64 scalar(LocalDatabase &db, const QString &statement) {
        QSqlQuery q(db.connection());
        if (!q.exec(statement) || !q.next())
            return -1;
        return q.value(0).toLongLong();
    }
    // Tudo o que o histórico guarda em texto: details e changes de cada evento.
    static QString eventText(LocalDatabase &db) {
        QSqlQuery q(db.connection());
        if (!q.exec("SELECT details||' '||COALESCE(changes,'') FROM events"))
            return QStringLiteral("ERRO");
        QStringList rows;
        while (q.next())
            rows << q.value(0).toString();
        return rows.join('\n');
    }
    static void verifyNoSecrets(LocalDatabase &db, const QList<QByteArray> &passwords) {
        const QString text = eventText(db);
        for (const auto &password : passwords)
            QVERIFY2(!text.contains(QString::fromUtf8(password)), "senha gravada no histórico");
        QVERIFY(!text.contains("$argon2id"));
        QVERIFY(!text.contains("\"password", Qt::CaseInsensitive));
    }
    static qint64 accountId(LocalDatabase &db, const QString &username) {
        return scalar(db, "SELECT id FROM accounts WHERE username='" + username + "'");
    }
    static QString verifier(LocalDatabase &db, qint64 id) {
        QSqlQuery q(db.connection());
        q.prepare("SELECT password_verifier||'|'||active||'|'||credential_version FROM accounts WHERE id=?");
        q.addBindValue(id);
        return q.exec() && q.next() ? q.value(0).toString() : QString();
    }
    // Administradores ainda não têm fluxo próprio nesta fase; a vaga 2 é preenchida diretamente no SQLite real.
    static void addAdministrator(TestContext &ctx, const QString &username, const QByteArray &password) {
        QSqlQuery fixture(ctx.database.connection());
        fixture.prepare("INSERT INTO accounts(username,password_verifier,role,active,admin_slot,created_at) "
                        "VALUES(?,?,'admin',1,2,?)");
        fixture.addBindValue(username);
        fixture.addBindValue(ctx.hasher.hash(password));
        fixture.addBindValue(LocalDatabase::nowUtc());
        QVERIFY2(fixture.exec(), qPrintable(fixture.lastError().text()));
    }
private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }

    void createTemporaryRequiresChange() {
        TestContext ctx(freshPath("temporary"));
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const qint64 notebook = ctx.store.notebooks().first().id;
        Session operatorSession = ctx.createCommonAndLogin("operador.um", Temporary);
        QVERIFY(operatorSession.isValid());
        QVERIFY(operatorSession.mustChangePassword());
        QCOMPARE(operatorSession.role(), AccountRole::Common);
        QCOMPARE(scalar(ctx.database,
                        "SELECT COUNT(*) FROM accounts WHERE username='operador.um' AND role='common' AND active=1 "
                        "AND must_change_password=1 AND credential_version=1 AND admin_slot IS NULL AND created_by IS "
                        "NOT NULL"),
                 1);

        const qint64 events = scalar(ctx.database, "SELECT COUNT(*) FROM events");
        QCOMPARE(refusal(
                     [&] {
                         Fixture::checkout(ctx, operatorSession, notebook, "Ana", ResponsibleRole::Professor,
                                           "Sala 12");
                     },
                     AuthorizationError::Reason::PasswordChangeRequired),
                 QString());
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), events);
        QCOMPARE(ctx.store.notebooks().first().status, Status::Available);

        const Session restricted = operatorSession;
        PasswordChangeAttempt attempt = ctx.auth.prepareRequiredPasswordChange(operatorSession);
        attempt.prepare(Personal);
        ctx.auth.completeRequiredPasswordChange(operatorSession, attempt);
        QVERIFY(!operatorSession.mustChangePassword());
        QCOMPARE(
            scalar(ctx.database,
                   "SELECT credential_version FROM accounts WHERE username='operador.um' AND must_change_password=0"),
            2);
        QCOMPARE(ctx.store.history().first().action, QStringLiteral("Senha definida"));
        QCOMPARE(ctx.store.history().first().actorUsername, QStringLiteral("operador.um"));

        Fixture::checkout(ctx, operatorSession, notebook, "Ana", ResponsibleRole::Professor, "Sala 12");
        QCOMPARE(ctx.store.notebooks().first().status, Status::Loaned);
        QCOMPARE(ctx.store.history().first().actorUsername, QStringLiteral("operador.um"));
        QCOMPARE(refusal(
                     [&] {
                         Fixture::checkin(ctx, restricted, notebook, "Ana", ResponsibleRole::Professor, "Armário 1");
                     },
                     AuthorizationError::Reason::NotAuthenticated),
                 QString());

        QVERIFY(!ctx.auth.authenticate("operador.um", Temporary).has_value());
        QVERIFY(ctx.auth.authenticate("operador.um", Personal).has_value());
        verifyNoSecrets(ctx.database, {Temporary, Personal, TestPassword});
    }

    void commonCannotManageAccounts() {
        TestContext ctx(freshPath("common-forbidden"));
        const Session common = ctx.createCommonAndLogin("operador.um", Personal, InitialPasswordMode::Assisted);
        const qint64 self = accountId(ctx.database, "operador.um");
        const qint64 accounts = scalar(ctx.database, "SELECT COUNT(*) FROM accounts");
        const qint64 events = scalar(ctx.database, "SELECT COUNT(*) FROM events");
        const QString before = verifier(ctx.database, self);
        const auto forbidden = AuthorizationError::Reason::Forbidden;
        QCOMPARE(refusal(
                     [&] {
                         ctx.accounts.createCommonAccount(common, "operador.dois", ctx.auth.preparePassword(Temporary),
                                                          InitialPasswordMode::Temporary);
                     },
                     forbidden),
                 QString());
        QCOMPARE(refusal(
                     [&] {
                         ctx.accounts.resetCommonPassword(common, self, ctx.auth.preparePassword(Reset),
                                                          InitialPasswordMode::Assisted);
                     },
                     forbidden),
                 QString());
        QCOMPARE(refusal(
                     [&] {
                         ctx.accounts.inactivateCommonAccount(common, self);
                     },
                     forbidden),
                 QString());
        QCOMPARE(refusal(
                     [&] {
                         ctx.accounts.commonAccounts(common, AccountFilter::All, {});
                     },
                     forbidden),
                 QString());
        try {
            ctx.accounts.commonAccounts(common, AccountFilter::All, {});
        } catch (const AuthorizationError &e) {
            QCOMPARE(QString::fromUtf8(e.what()), QStringLiteral("Esta ação exige uma conta administradora ativa."));
        }
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts"), accounts);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), events);
        QCOMPARE(verifier(ctx.database, self), before);
    }

    void assistedCreation() {
        TestContext ctx(freshPath("assisted"));
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const Session common = ctx.createCommonAndLogin("operador.um", Personal, InitialPasswordMode::Assisted);
        QVERIFY(!common.mustChangePassword());
        QCOMPARE(scalar(ctx.database, "SELECT must_change_password FROM accounts WHERE username='operador.um'"), 0);
        Fixture::checkout(ctx, common, ctx.store.notebooks().first().id, "Ana", ResponsibleRole::Professor, "Sala 12");
        QCOMPARE(ctx.store.history().first().actorUsername, QStringLiteral("operador.um"));
        const auto listed = ctx.accounts.commonAccounts(ctx.session, AccountFilter::Active, "OPERADOR");
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.first().username, QStringLiteral("operador.um"));
        QCOMPARE(listed.first().role, AccountRole::Common);
        QVERIFY(ctx.accounts.commonAccounts(ctx.session, AccountFilter::Inactive, {}).isEmpty());
        QVERIFY(ctx.accounts.commonAccounts(ctx.session, AccountFilter::All, "principal").isEmpty());
    }

    void adminRevalidatedAtCommit() {
        const auto path = freshPath("revalidated");
        TestContext ctx(path);
        addAdministrator(ctx, "admin.um", Personal);
        const Session admin = ctx.auth.login("admin.um", Personal);
        const qint64 created = ctx.accounts.createCommonAccount(
            admin, "operador.dois", ctx.auth.preparePassword(Temporary), InitialPasswordMode::Temporary);
        QCOMPARE(scalar(ctx.database, "SELECT created_by FROM accounts WHERE id=" + QString::number(created)),
                 accountId(ctx.database, "admin.um"));
        QCOMPARE(ctx.store.history().first().actorUsername, QStringLiteral("admin.um"));
        const PreparedPassword prepared = ctx.auth.preparePassword(Temporary);
        QVERIFY(execAux(path, "UPDATE accounts SET active=0, admin_slot=NULL WHERE username='admin.um'"));
        const qint64 events = scalar(ctx.database, "SELECT COUNT(*) FROM events");
        QCOMPARE(refusal(
                     [&] {
                         ctx.accounts.createCommonAccount(admin, "operador.um", prepared,
                                                          InitialPasswordMode::Temporary);
                     },
                     AuthorizationError::Reason::Inactive),
                 QString());
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='operador.um'"), 0);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), events);
    }

    void duplicateUsernameRejected() {
        TestContext ctx(freshPath("duplicate"));
        ctx.accounts.createCommonAccount(ctx.session, "operador.um", ctx.auth.preparePassword(Temporary),
                                         InitialPasswordMode::Temporary);
        const QString expected = QStringLiteral("duplicate_username|Já existe uma conta com esse nome de usuário.");
        QCOMPARE(errorCode([&] {
                     ctx.accounts.createCommonAccount(ctx.session, " Operador.Um ", ctx.auth.preparePassword(Temporary),
                                                      InitialPasswordMode::Temporary);
                 }),
                 expected);
        QCOMPARE(errorCode([&] {
                     ctx.accounts.createCommonAccount(ctx.session, TestPrincipal, ctx.auth.preparePassword(Temporary),
                                                      InitialPasswordMode::Assisted);
                 }),
                 expected);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='operador.um'"), 1);

        const auto shared = freshPath("duplicate-shared");
        TestContext first(shared);
        TestContext second(shared);
        first.accounts.createCommonAccount(first.session, "operador.dois", first.auth.preparePassword(Temporary),
                                           InitialPasswordMode::Temporary);
        QCOMPARE(errorCode([&] {
                     second.accounts.createCommonAccount(second.session, "operador.dois",
                                                         second.auth.preparePassword(Personal),
                                                         InitialPasswordMode::Assisted);
                 }),
                 expected);
        QCOMPARE(scalar(first.database, "SELECT COUNT(*) FROM accounts WHERE username='operador.dois'"), 1);
        QCOMPARE(scalar(first.database, "SELECT COUNT(*) FROM events WHERE action='Conta criada'"), 1);
    }

    void newPasswordMustDiffer() {
        TestContext ctx(freshPath("differ"));
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        Session common = ctx.createCommonAndLogin("operador.um", Temporary);
        const qint64 id = accountId(ctx.database, "operador.um");
        const QString before = verifier(ctx.database, id);
        const qint64 events = scalar(ctx.database, "SELECT COUNT(*) FROM events");
        PasswordChangeAttempt attempt = ctx.auth.prepareRequiredPasswordChange(common);
        QCOMPARE(errorCode([&] {
                     attempt.prepare(Temporary);
                 }),
                 QStringLiteral("same_password|A nova senha deve ser diferente da senha temporária."));
        QVERIFY(errorCode([&] {
                    attempt.prepare("curta");
                }).startsWith("invalid_password|"));
        try {
            ctx.auth.completeRequiredPasswordChange(common, attempt);
            QFAIL("troca gravada sem senha preparada");
        } catch (const std::logic_error &) {
        }
        QCOMPARE(verifier(ctx.database, id), before);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), events);
        QVERIFY(common.mustChangePassword());
        QCOMPARE(refusal(
                     [&] {
                         Fixture::checkout(ctx, common, ctx.store.notebooks().first().id, "Ana",
                                           ResponsibleRole::Professor, "Sala 12");
                     },
                     AuthorizationError::Reason::PasswordChangeRequired),
                 QString());
    }

    void resetCommonPassword() {
        TestContext ctx(freshPath("reset"));
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const qint64 notebook = ctx.store.notebooks().first().id;
        const Session previous = ctx.createCommonAndLogin("operador.um", Personal, InitialPasswordMode::Assisted);
        const qint64 id = accountId(ctx.database, "operador.um");
        ctx.accounts.resetCommonPassword(ctx.session, id, ctx.auth.preparePassword(Reset),
                                         InitialPasswordMode::Temporary);
        QCOMPARE(refusal(
                     [&] {
                         Fixture::checkout(ctx, previous, notebook, "Ana", ResponsibleRole::Professor, "Sala 12");
                     },
                     AuthorizationError::Reason::Revoked),
                 QString());
        QVERIFY(!ctx.auth.authenticate("operador.um", Personal).has_value());
        Session renewed = ctx.auth.login("operador.um", Reset);
        QVERIFY(renewed.mustChangePassword());
        QCOMPARE(scalar(ctx.database, "SELECT credential_version FROM accounts WHERE id=" + QString::number(id)), 2);
        ctx.accounts.resetCommonPassword(ctx.session, id, ctx.auth.preparePassword(Temporary),
                                         InitialPasswordMode::Assisted);
        QCOMPARE(scalar(ctx.database, "SELECT must_change_password FROM accounts WHERE id=" + QString::number(id)), 0);
        const Session assisted = ctx.auth.login("operador.um", Temporary);
        QVERIFY(!assisted.mustChangePassword());
        Fixture::checkout(ctx, assisted, notebook, "Ana", ResponsibleRole::Professor, "Sala 12");
        QCOMPARE(ctx.store.notebooks().first().status, Status::Loaned);
    }

    void inactivateCommonPreservesAuthorship() {
        const auto path = freshPath("inactivate");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const qint64 notebook = ctx.store.notebooks().first().id;
        const Session common = ctx.createCommonAndLogin("operador.um", Personal, InitialPasswordMode::Assisted);
        Fixture::checkout(ctx, common, notebook, "Ana", ResponsibleRole::Professor, "Sala 12");
        const qint64 id = accountId(ctx.database, "operador.um");
        ctx.accounts.inactivateCommonAccount(ctx.session, id);
        bool authored = false;
        for (const auto &entry : ctx.store.history())
            authored = authored || (entry.action == "Retirada" && entry.actorUsername == "operador.um");
        QVERIFY(authored);
        QCOMPARE(ctx.store.history().first().action, QStringLiteral("Conta inativada"));
        QCOMPARE(ctx.store.history().first().actorUsername, TestPrincipal);
        QCOMPARE(
            errorCode([&] {
                ctx.auth.login("operador.um", Personal);
            }),
            QStringLiteral(
                "invalid_credentials|Não foi possível entrar. Confira o nome de usuário e a senha e tente novamente."));
        QCOMPARE(refusal(
                     [&] {
                         Fixture::checkin(ctx, common, notebook, "Ana", ResponsibleRole::Professor, "Armário 1");
                     },
                     AuthorizationError::Reason::Inactive),
                 QString());
        QVERIFY(!execAux(path, "DELETE FROM accounts WHERE username='operador.um'"));
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='operador.um' AND active=0"), 1);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events WHERE actor_username='operador.um'"), 1);
        QCOMPARE(errorCode([&] {
                     ctx.accounts.inactivateCommonAccount(ctx.session, id);
                 }),
                 QStringLiteral("inactive_account|Esta conta já está inativa."));
        const auto inactive = ctx.accounts.commonAccounts(ctx.session, AccountFilter::Inactive, {});
        QCOMPARE(inactive.size(), 1);
        QVERIFY(!inactive.first().active);
        QVERIFY(ctx.accounts.commonAccounts(ctx.session, AccountFilter::Active, {}).isEmpty());
    }

    void targetMustBeCommon() {
        TestContext ctx(freshPath("target"));
        addAdministrator(ctx, "admin.um", Personal);
        const qint64 principal = accountId(ctx.database, TestPrincipal);
        const qint64 admin = accountId(ctx.database, "admin.um");
        const qint64 events = scalar(ctx.database, "SELECT COUNT(*) FROM events");
        const QString principalBefore = verifier(ctx.database, principal), adminBefore = verifier(ctx.database, admin);
        const QString expected =
            QStringLiteral("not_common_account|Esta ação está disponível somente para contas comuns.");
        for (const qint64 target : {principal, admin}) {
            QCOMPARE(errorCode([&] {
                         ctx.accounts.resetCommonPassword(ctx.session, target, ctx.auth.preparePassword(Reset),
                                                          InitialPasswordMode::Temporary);
                     }),
                     expected);
            QCOMPARE(errorCode([&] {
                         ctx.accounts.inactivateCommonAccount(ctx.session, target);
                     }),
                     expected);
        }
        QVERIFY(errorCode([&] {
                    ctx.accounts.inactivateCommonAccount(ctx.session, 9999);
                }).startsWith("account_not_found|"));
        QCOMPARE(verifier(ctx.database, principal), principalBefore);
        QCOMPARE(verifier(ctx.database, admin), adminBefore);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), events);
    }

    void accountEventsRecordChanges() {
        TestContext ctx(freshPath("changes"));
        Session common = ctx.createCommonAndLogin("operador.um", Temporary);
        const qint64 id = accountId(ctx.database, "operador.um");
        auto latest = [&] {
            return ctx.store.history().first();
        };
        QCOMPARE(latest().action, QStringLiteral("Conta criada"));
        QCOMPARE(latest().entityType, QStringLiteral("account"));
        QCOMPARE(
            latest().changes,
            json(
                R"({"username":{"de":null,"para":"operador.um"},"role":{"de":null,"para":"common"},"active":{"de":null,"para":1},)"
                R"("must_change_password":{"de":null,"para":1},"initial_password_mode":{"de":null,"para":"temporária"}})"));
        PasswordChangeAttempt attempt = ctx.auth.prepareRequiredPasswordChange(common);
        attempt.prepare(Personal);
        ctx.auth.completeRequiredPasswordChange(common, attempt);
        QCOMPARE(latest().action, QStringLiteral("Senha definida"));
        QCOMPARE(latest().changes, json(R"({"must_change_password":{"de":1,"para":0}})"));
        ctx.accounts.resetCommonPassword(ctx.session, id, ctx.auth.preparePassword(Reset),
                                         InitialPasswordMode::Temporary);
        QCOMPARE(latest().action, QStringLiteral("Senha redefinida"));
        QCOMPARE(
            latest().changes,
            json(
                R"({"must_change_password":{"de":0,"para":1},"initial_password_mode":{"de":null,"para":"temporária"}})"));
        ctx.accounts.inactivateCommonAccount(ctx.session, id);
        QCOMPARE(latest().action, QStringLiteral("Conta inativada"));
        QCOMPARE(latest().changes, json(R"({"active":{"de":1,"para":0}})"));
        verifyNoSecrets(ctx.database, {Temporary, Personal, Reset, TestPassword});
    }
};

QTEST_GUILESS_MAIN(AccountTests)
#include "AccountTests.moc"

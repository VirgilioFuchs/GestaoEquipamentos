#include <QtTest>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "TestSupport.h"

namespace {
const QByteArray AdminPassword = QByteArrayLiteral("senha do administrador um");
const QByteArray OtherPassword = QByteArrayLiteral("senha do administrador dois");
const QByteArray ResetPassword = QByteArrayLiteral("senha redefinida do admin");
const QString LimitMessage = QStringLiteral("Limite de três administradores ativos atingido. Inative outro administrador para liberar uma vaga.");
template <typename F> QString refusal(F &&operation, AuthorizationError::Reason expected) {
    try { operation(); } catch (const AuthorizationError &error) {
        return error.reason() == expected ? QString() : QStringLiteral("motivo inesperado: ") + QString::fromUtf8(error.what());
    } catch (const std::exception &error) { return QStringLiteral("outra exceção: ") + QString::fromUtf8(error.what()); }
    return QStringLiteral("operação aceita");
}
template <typename F> QString errorCode(F &&operation) {
    try { operation(); } catch (const AuthError &error) { return error.code() + "|" + QString::fromUtf8(error.what()); }
    catch (const std::exception &error) { return QStringLiteral("outra exceção: ") + QString::fromUtf8(error.what()); }
    return QStringLiteral("operação aceita");
}
bool execAux(const QString &path, const QString &statement) {
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "aux"); db.setDatabaseName(path);
        QSqlQuery q(db);
        ok = db.open() && q.exec(statement);
    }
    QSqlDatabase::removeDatabase("aux");
    return ok;
}
QJsonObject json(const char *text) { return QJsonDocument::fromJson(QByteArray(text)).object(); }
}

class AdminTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString freshPath(const QString &name) { return dir.filePath(name + ".sqlite"); }
    static qint64 scalar(LocalDatabase &db, const QString &statement) {
        QSqlQuery q(db.connection());
        if (!q.exec(statement) || !q.next()) return -1;
        return q.value(0).toLongLong();
    }
    static qint64 accountId(LocalDatabase &db, const QString &username) {
        return scalar(db, "SELECT id FROM accounts WHERE username='" + username + "'");
    }
    static qint64 eventCount(LocalDatabase &db) { return scalar(db, "SELECT COUNT(*) FROM events"); }
    static qint64 createAdmin(TestContext &ctx, const QString &username, const QByteArray &password) {
        return ctx.accounts.createAdministrator(ctx.session, username, ctx.auth.preparePassword(password), InitialPasswordMode::Assisted);
    }
private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }

    void adminLimitThree() {
        TestContext ctx(freshPath("limit"));
        QCOMPARE(ctx.accounts.activeAdministratorCount(ctx.session), 1);
        const qint64 first = createAdmin(ctx, "admin.um", AdminPassword);
        createAdmin(ctx, "admin.dois", OtherPassword);
        QCOMPARE(ctx.accounts.activeAdministratorCount(ctx.session), 3);
        QCOMPARE(scalar(ctx.database, "SELECT admin_slot FROM accounts WHERE username='admin.um'"), 2);
        QCOMPARE(scalar(ctx.database, "SELECT admin_slot FROM accounts WHERE username='admin.dois'"), 3);
        const qint64 events = eventCount(ctx.database);
        QCOMPARE(errorCode([&] { createAdmin(ctx, "admin.tres", AdminPassword); }), "admin_limit|" + LimitMessage);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='admin.tres'"), 0);
        QCOMPARE(eventCount(ctx.database), events);

        ctx.accounts.resetAdministratorPassword(ctx.session, first, ctx.auth.preparePassword(ResetPassword), InitialPasswordMode::Temporary);
        QCOMPARE(ctx.accounts.activeAdministratorCount(ctx.session), 3);
        QCOMPARE(scalar(ctx.database, "SELECT admin_slot FROM accounts WHERE username='admin.um'"), 2);
        QVERIFY(errorCode([&] { createAdmin(ctx, "admin.tres", AdminPassword); }).startsWith("admin_limit|"));

        ctx.accounts.inactivateAdministrator(ctx.session, first);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='admin.um' AND active=0 AND admin_slot IS NULL"), 1);
        QCOMPARE(ctx.accounts.activeAdministratorCount(ctx.session), 2);
        createAdmin(ctx, "admin.tres", AdminPassword);
        QCOMPARE(scalar(ctx.database, "SELECT admin_slot FROM accounts WHERE username='admin.tres'"), 2);
        QCOMPARE(ctx.accounts.activeAdministratorCount(ctx.session), 3);
        const auto listed = ctx.accounts.administrators(ctx.session);
        QCOMPARE(listed.size(), 4);
        QCOMPARE(listed.first().role, AccountRole::Principal);
        QCOMPARE(listed.last().username, QStringLiteral("admin.um"));
        QVERIFY(!listed.last().active);
    }

    void adminSessionExpiresAbsolute() {
        TestContext ctx(freshPath("expiry"));
        const QDateTime issued = ctx.session.issuedAtUtc();
        QCOMPARE(ctx.auth.sessionDeadlineUtc(ctx.session), issued.addSecs(15 * 60));
        Fixture::add(ctx,ctx.session, "NB-1", "Dell", "Armário 1");
        const qint64 first = ctx.store.notebooks().first().id;
        ctx.clock.advance(300);
        Fixture::checkout(ctx,ctx.session, first, "Ana", ResponsibleRole::Professor, "Sala 12");
        ctx.clock.advance(300);
        Fixture::checkin(ctx,ctx.session, first, "Ana", ResponsibleRole::Professor, "Armário 1");
        ctx.clock.advance(299);
        Fixture::add(ctx,ctx.session, "NB-2", "Lenovo", "Armário 2");
        qint64 second = 0;
        for (const auto &notebook : ctx.store.notebooks()) if (notebook.assetTag == "NB-2") second = notebook.id;
        Fixture::checkout(ctx,ctx.session, second, "Bruno", ResponsibleRole::Assistant, "Sala 3");
        Fixture::checkin(ctx,ctx.session, second, "Bruno", ResponsibleRole::Assistant, "Armário 2");
        Fixture::updateLocation(ctx,ctx.session, first, "Sala 7");
        ctx.store.setMaintenance(ctx.session, first, true);
        QVERIFY(!ctx.auth.sessionExpired(ctx.session));
        QCOMPARE(ctx.auth.sessionDeadlineUtc(ctx.session), issued.addSecs(15 * 60));

        ctx.clock.advance(1);
        QVERIFY(ctx.auth.sessionExpired(ctx.session));
        const qint64 events = eventCount(ctx.database);
        const auto expired = AuthorizationError::Reason::Expired;
        QCOMPARE(refusal([&] { Fixture::add(ctx,ctx.session, "NB-3", "HP", "Armário 3"); }, expired), QString());
        QCOMPARE(refusal([&] { Fixture::checkout(ctx,ctx.session, second, "Ana", ResponsibleRole::Professor, "Sala 12"); }, expired), QString());
        QCOMPARE(refusal([&] { Fixture::checkin(ctx,ctx.session, second, "Ana", ResponsibleRole::Professor, "Armário 2"); }, expired), QString());
        QCOMPARE(refusal([&] { Fixture::updateLocation(ctx,ctx.session, second, "Sala 9"); }, expired), QString());
        QCOMPARE(refusal([&] { ctx.store.setMaintenance(ctx.session, first, false); }, expired), QString());
        QCOMPARE(refusal([&] { ctx.accounts.createCommonAccount(ctx.session, "operador.um", ctx.auth.preparePassword(AdminPassword), InitialPasswordMode::Assisted); }, expired), QString());
        try { Fixture::add(ctx,ctx.session, "NB-3", "HP", "Armário 3"); QFAIL("sessão expirada cadastrou notebook"); }
        catch (const AuthorizationError &e) { QCOMPARE(QString::fromUtf8(e.what()), QStringLiteral("Sessão administrativa expirada. Entre novamente para continuar.")); }
        QCOMPARE(eventCount(ctx.database), events);
        QCOMPARE(ctx.store.notebooks().size(), 2);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='operador.um'"), 0);

        Session renewed = ctx.auth.login(TestPrincipal, TestPassword);
        ctx.store.setMaintenance(renewed, first, false);
        QCOMPARE(eventCount(ctx.database), events + 1);
    }

    void concurrentAdminCreation() {
        const auto shared = freshPath("concurrent");
        TestContext first(shared);
        createAdmin(first, "admin.um", AdminPassword);
        TestContext second(shared);
        QCOMPARE(second.accounts.activeAdministratorCount(second.session), 2);
        const QString a = errorCode([&] { createAdmin(first, "admin.dois", OtherPassword); });
        const QString b = errorCode([&] { createAdmin(second, "admin.tres", OtherPassword); });
        QCOMPARE(a, QStringLiteral("operação aceita"));
        QCOMPARE(b, "admin_limit|" + LimitMessage);
        QCOMPARE(scalar(first.database, "SELECT COUNT(*) FROM accounts WHERE role IN ('principal','admin') AND active=1"), 3);
        QCOMPARE(scalar(first.database, "SELECT COUNT(*) FROM accounts WHERE username='admin.tres'"), 0);
        QCOMPARE(scalar(first.database, "SELECT COUNT(*) FROM events WHERE action='Administrador criado'"), 2);
        const QString insert = "INSERT INTO accounts(username,password_verifier,role,active,admin_slot,created_at) VALUES('admin.quatro','x','admin',1,%1,'2026-01-05T12:00:00.000Z')";
        for (const QString slot : {QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"), QStringLiteral("NULL")})
            QVERIFY2(!execAux(shared, insert.arg(slot)), qPrintable("quarto administrador aceito com admin_slot " + slot));
        QCOMPARE(scalar(first.database, "SELECT COUNT(*) FROM accounts WHERE username='admin.quatro'"), 0);
    }

    void onlyPrincipalManagesAdmins() {
        TestContext ctx(freshPath("only-principal"));
        createAdmin(ctx, "admin.um", AdminPassword);
        const qint64 other = createAdmin(ctx, "admin.dois", OtherPassword);
        const Session admin = ctx.auth.login("admin.um", AdminPassword);
        const qint64 events = eventCount(ctx.database);
        const auto forbidden = AuthorizationError::Reason::Forbidden;
        QCOMPARE(refusal([&] { ctx.accounts.administrators(admin); }, forbidden), QString());
        QCOMPARE(refusal([&] { ctx.accounts.activeAdministratorCount(admin); }, forbidden), QString());
        QCOMPARE(refusal([&] { ctx.accounts.createAdministrator(admin, "admin.tres", ctx.auth.preparePassword(AdminPassword), InitialPasswordMode::Assisted); }, forbidden), QString());
        QCOMPARE(refusal([&] { ctx.accounts.resetAdministratorPassword(admin, other, ctx.auth.preparePassword(ResetPassword), InitialPasswordMode::Temporary); }, forbidden), QString());
        QCOMPARE(refusal([&] { ctx.accounts.inactivateAdministrator(admin, other); }, forbidden), QString());
        try { ctx.accounts.inactivateAdministrator(admin, other); }
        catch (const AuthorizationError &e) { QCOMPARE(QString::fromUtf8(e.what()), QStringLiteral("Esta ação é exclusiva da conta principal.")); }
        QCOMPARE(eventCount(ctx.database), events);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='admin.dois' AND active=1 AND admin_slot=3"), 1);
        ctx.accounts.createCommonAccount(admin, "operador.um", ctx.auth.preparePassword(AdminPassword), InitialPasswordMode::Assisted);
        QCOMPARE(ctx.store.history().first().actorUsername, QStringLiteral("admin.um"));
    }

    void principalCannotBeChanged() {
        const auto path = freshPath("principal");
        TestContext ctx(path);
        const qint64 principal = accountId(ctx.database, TestPrincipal);
        ctx.accounts.createCommonAccount(ctx.session, "operador.um", ctx.auth.preparePassword(AdminPassword), InitialPasswordMode::Assisted);
        const qint64 common = accountId(ctx.database, "operador.um");
        const qint64 events = eventCount(ctx.database);
        const QString protectedCode = QStringLiteral("principal_protected|A conta principal não pode ser alterada por esta ação.");
        QCOMPARE(errorCode([&] { ctx.accounts.inactivateAdministrator(ctx.session, principal); }), protectedCode);
        QCOMPARE(errorCode([&] { ctx.accounts.resetAdministratorPassword(ctx.session, principal, ctx.auth.preparePassword(ResetPassword), InitialPasswordMode::Temporary); }), protectedCode);
        QVERIFY(errorCode([&] { ctx.accounts.inactivateAdministrator(ctx.session, common); }).startsWith("not_administrator|"));
        QVERIFY(!execAux(path, "UPDATE accounts SET role='admin' WHERE role='principal'"));
        QVERIFY(!execAux(path, "UPDATE accounts SET active=0, admin_slot=NULL WHERE role='principal'"));
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE id=" + QString::number(principal) + " AND role='principal' AND active=1 AND admin_slot=1"), 1);
        QCOMPARE(eventCount(ctx.database), events);
        QVERIFY(ctx.auth.authenticate(TestPrincipal, TestPassword).has_value());
    }

    void twoClocks() {
        const auto expired = AuthorizationError::Reason::Expired;
        {
            TestContext ctx(freshPath("clock-wall"));
            ctx.clock.advanceWall(15 * 60 - 1);
            Fixture::add(ctx,ctx.session, "NB-1", "Dell", "Armário 1");
            ctx.clock.advanceWall(1);
            QVERIFY(ctx.auth.sessionExpired(ctx.session));
            QCOMPARE(refusal([&] { Fixture::add(ctx,ctx.session, "NB-2", "HP", "Armário 2"); }, expired), QString());
        }
        {
            TestContext ctx(freshPath("clock-monotonic"));
            ctx.clock.advanceMonotonic(AdminSessionDuration - 1);
            Fixture::add(ctx,ctx.session, "NB-1", "Dell", "Armário 1");
            ctx.clock.advanceMonotonic(1);
            QVERIFY(ctx.auth.sessionExpired(ctx.session));
            QCOMPARE(refusal([&] { Fixture::add(ctx,ctx.session, "NB-2", "HP", "Armário 2"); }, expired), QString());
        }
        {
            TestContext ctx(freshPath("clock-back"));
            ctx.clock.advanceWall(-60);
            QVERIFY(ctx.auth.sessionExpired(ctx.session));
            QCOMPARE(refusal([&] { Fixture::add(ctx,ctx.session, "NB-1", "Dell", "Armário 1"); }, expired), QString());
            QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM notebooks"), 0);
        }
    }

    void commonSessionNoExpiry() {
        TestContext ctx(freshPath("common"));
        Fixture::add(ctx,ctx.session, "NB-1", "Dell", "Armário 1");
        const qint64 notebook = ctx.store.notebooks().first().id;
        const Session common = ctx.createCommonAndLogin("operador.um", AdminPassword, InitialPasswordMode::Assisted);
        QVERIFY(!ctx.auth.sessionDeadlineUtc(common).isValid());
        ctx.clock.advance(8 * 3600);
        QVERIFY(!ctx.auth.sessionExpired(common));
        Fixture::checkout(ctx,common, notebook, "Ana", ResponsibleRole::Professor, "Sala 12");
        Fixture::checkin(ctx,common, notebook, "Ana", ResponsibleRole::Professor, "Armário 1");
        QCOMPARE(ctx.store.history().first().actorUsername, QStringLiteral("operador.um"));
        QCOMPARE(refusal([&] { Fixture::checkout(ctx,ctx.session, notebook, "Ana", ResponsibleRole::Professor, "Sala 12"); },
                         AuthorizationError::Reason::Expired), QString());
    }

    void inactivatedAdminSessionRefused() {
        TestContext ctx(freshPath("inactivated"));
        const qint64 id = createAdmin(ctx, "admin.um", AdminPassword);
        const Session admin = ctx.auth.login("admin.um", AdminPassword);
        Fixture::add(ctx,admin, "NB-1", "Dell", "Armário 1");
        ctx.accounts.inactivateAdministrator(ctx.session, id);
        const qint64 events = eventCount(ctx.database);
        const auto inactive = AuthorizationError::Reason::Inactive;
        QCOMPARE(refusal([&] { Fixture::add(ctx,admin, "NB-2", "HP", "Armário 2"); }, inactive), QString());
        QCOMPARE(refusal([&] { ctx.accounts.createCommonAccount(admin, "operador.um", ctx.auth.preparePassword(AdminPassword), InitialPasswordMode::Assisted); }, inactive), QString());
        QCOMPARE(eventCount(ctx.database), events);
        QVERIFY(errorCode([&] { ctx.auth.login("admin.um", AdminPassword); }).startsWith("invalid_credentials|"));
    }

    void adminEventsRecordChanges() {
        TestContext ctx(freshPath("changes"));
        auto latest = [&] { return ctx.store.history().first(); };
        const qint64 id = createAdmin(ctx, "admin.um", AdminPassword);
        QCOMPARE(latest().action, QStringLiteral("Administrador criado"));
        QCOMPARE(latest().actorUsername, TestPrincipal);
        QCOMPARE(latest().changes, json(R"({"username":{"de":null,"para":"admin.um"},"role":{"de":null,"para":"admin"},"active":{"de":null,"para":1},)"
                                        R"("must_change_password":{"de":null,"para":0},"initial_password_mode":{"de":null,"para":"assistida"},)"
                                        R"("admin_slot":{"de":null,"para":2}})"));
        ctx.accounts.resetAdministratorPassword(ctx.session, id, ctx.auth.preparePassword(ResetPassword), InitialPasswordMode::Temporary);
        QCOMPARE(latest().action, QStringLiteral("Senha de administrador redefinida"));
        QCOMPARE(latest().changes, json(R"({"must_change_password":{"de":0,"para":1},"initial_password_mode":{"de":null,"para":"temporária"}})"));
        QVERIFY(!latest().changes.contains("admin_slot"));
        QCOMPARE(scalar(ctx.database, "SELECT admin_slot FROM accounts WHERE id=" + QString::number(id)), 2);
        ctx.accounts.inactivateAdministrator(ctx.session, id);
        QCOMPARE(latest().action, QStringLiteral("Administrador inativado"));
        QCOMPARE(latest().changes, json(R"({"active":{"de":1,"para":0},"admin_slot":{"de":2,"para":null}})"));
        QSqlQuery q(ctx.database.connection());
        QVERIFY(q.exec("SELECT details||' '||COALESCE(changes,'') FROM events"));
        QStringList text;
        while (q.next()) text << q.value(0).toString();
        const QString all = text.join('\n');
        for (const auto &secret : {AdminPassword, ResetPassword, TestPassword}) QVERIFY2(!all.contains(QString::fromUtf8(secret)), "senha gravada no histórico");
        QVERIFY(!all.contains("$argon2id"));
    }
};

QTEST_GUILESS_MAIN(AdminTests)
#include "AdminTests.moc"

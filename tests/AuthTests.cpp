#include <QtTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSqlError>
#include <QSqlQuery>
#include "application/AdminProtocol.h"
#include "application/AuthService.h"
#include "application/DemoSeed.h"
#include <QJsonObject>
#include "TestSupport.h"

namespace {
const QByteArray Password = QByteArrayLiteral("senha longa da principal 1");
template <typename F>
QString errorCode(F &&operation) {
    try {
        operation();
    } catch (const AuthError &error) {
        return error.code();
    } catch (...) {
        return QStringLiteral("other");
    }
    return {};
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
const QString GenericLoginFailure = QStringLiteral(
    "invalid_credentials|Não foi possível entrar. Confira o nome de usuário e a senha e tente novamente.");
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
} // namespace

class AuthTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    PasswordHasher fastHasher{PasswordHasher::testing()};
    QString freshPath(const QString &name) { return dir.filePath(name + ".sqlite"); }
    static qint64 scalar(LocalDatabase &db, const QString &statement) {
        QSqlQuery q(db.connection());
        if (!q.exec(statement) || !q.next())
            return -1;
        return q.value(0).toLongLong();
    }
    static QStringList inventoryState(LocalDatabase &db) {
        QStringList rows;
        QSqlQuery q(db.connection());
        if (!q.exec("SELECT id||'|'||status||'|'||COALESCE(location,'') FROM notebooks UNION ALL SELECT 'loans "
                    "'||COUNT(*) FROM loans UNION ALL SELECT 'events '||COUNT(*) FROM events"))
            return {"ERRO"};
        while (q.next())
            rows << q.value(0).toString();
        return rows;
    }
    void addCommonAccount(TestContext &ctx, const QString &username) {
        QSqlQuery fixture(ctx.database.connection());
        fixture.prepare("INSERT INTO accounts(username,password_verifier,role,active,admin_slot,created_at) "
                        "VALUES(?,?,'common',1,NULL,?)");
        fixture.addBindValue(username);
        fixture.addBindValue(fastHasher.hash(TestPassword));
        fixture.addBindValue(LocalDatabase::nowUtc());
        QVERIFY2(fixture.exec(), qPrintable(fixture.lastError().text()));
    }
private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }

    void bootstrapCreatesPrincipal() {
        const auto path = freshPath("bootstrap");
        qint64 id = 0;
        {
            LocalDatabase db(path);
            AuthService auth(db, fastHasher);
            QVERIFY(!auth.hasPrincipal());
            id = auth.bootstrapPrincipal("  Principal ", auth.preparePassword(Password));
            QVERIFY(auth.hasPrincipal());
            QCOMPARE(scalar(db, "SELECT COUNT(*) FROM accounts WHERE role='principal' AND admin_slot=1 AND active=1"),
                     1);
            QCOMPARE(scalar(db, "SELECT principal_account_id FROM app_config WHERE id=1"), id);
            const auto actor = auth.authenticate("principal", Password);
            QVERIFY(actor.has_value());
            QCOMPARE(actor->accountId, id);
            QCOMPARE(actor->username, QStringLiteral("principal"));
        }
        TestContext ctx(path, false);
        const auto entry = ctx.store.history().first();
        QCOMPARE(entry.action, QStringLiteral("Conta principal criada"));
        QCOMPARE(entry.actorUsername, QStringLiteral("principal"));
        const auto expected =
            QJsonDocument::fromJson(
                R"({"username":{"de":null,"para":"principal"},"role":{"de":null,"para":"principal"},"admin_slot":{"de":null,"para":1}})")
                .object();
        QCOMPARE(entry.changes, expected);
    }

    void kdfInteractiveTiming() {
        PasswordHasher interactive;
        QElapsedTimer timer;
        timer.start();
        const auto verifier = interactive.hash(Password);
        qInfo().noquote() << "Argon2id interactive:" << timer.elapsed() << "ms; prefixo"
                          << verifier.section('$', 0, 3) + "$";
        QVERIFY(interactive.verify(verifier, Password));
    }

    void secondBootstrapRefused() {
        LocalDatabase db(freshPath("second"));
        AuthService auth(db, fastHasher);
        auth.bootstrapPrincipal("principal", auth.preparePassword(Password));
        const auto accounts = scalar(db, "SELECT COUNT(*) FROM accounts");
        const auto events = scalar(db, "SELECT COUNT(*) FROM events");
        try {
            auth.bootstrapPrincipal("outra.principal", auth.preparePassword(Password));
            QFAIL("segundo bootstrap aceito");
        } catch (const AuthError &error) {
            QCOMPARE(error.code(), QStringLiteral("already_initialized"));
            QCOMPARE(QString::fromUtf8(error.what()),
                     QStringLiteral("A conta principal já foi configurada. Use a recuperação autenticada."));
        }
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM accounts"), accounts);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM events"), events);
    }

    void principalImmutableByTrigger() {
        const auto path = freshPath("immutable");
        {
            LocalDatabase db(path);
            AuthService auth(db, fastHasher);
            auth.bootstrapPrincipal("principal", auth.preparePassword(Password));
        }
        QVERIFY(!execAux(path, "UPDATE accounts SET role='admin' WHERE role='principal'"));
        QVERIFY(!execAux(path, "UPDATE accounts SET active=0 WHERE role='principal'"));
        QVERIFY(!execAux(path, "UPDATE app_config SET principal_account_id=NULL WHERE id=1"));
        LocalDatabase db(path);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM accounts WHERE role='principal' AND active=1"), 1);
        QVERIFY(scalar(db, "SELECT principal_account_id FROM app_config WHERE id=1") > 0);
    }

    void bootstrapRollsBackOnEventFailure() {
        LocalDatabase db(freshPath("rollback"));
        QSqlQuery trigger(db.connection());
        QVERIFY(trigger.exec("CREATE TRIGGER fail_audit BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'simulated "
                             "audit failure'); END"));
        AuthService auth(db, fastHasher);
        const auto prepared = auth.preparePassword(Password);
        QVERIFY_THROWS_EXCEPTION(std::exception, auth.bootstrapPrincipal("principal", prepared));
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM accounts"), 0);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM app_config WHERE principal_account_id IS NULL"), 1);
        QVERIFY(!auth.hasPrincipal());
    }

    void passwordNeverStored() {
        const auto path = freshPath("secret-scan");
        LocalDatabase db(path);
        AuthService auth(db, fastHasher);
        auth.bootstrapPrincipal("principal", auth.preparePassword(Password));
        QSqlQuery tables(db.connection());
        QVERIFY(tables.exec("SELECT name FROM sqlite_master WHERE type='table'"));
        int scannedColumns = 0;
        while (tables.next()) {
            const auto table = tables.value(0).toString();
            QSqlQuery columns(db.connection());
            QVERIFY(columns.exec("PRAGMA table_info(\"" + table + "\")"));
            while (columns.next()) {
                const auto column = columns.value(1).toString();
                QSqlQuery hit(db.connection());
                hit.prepare("SELECT COUNT(*) FROM \"" + table + "\" WHERE instr(CAST(\"" + column +
                            "\" AS TEXT), ?) > 0");
                hit.addBindValue(QString::fromUtf8(Password));
                QVERIFY2(hit.exec() && hit.next(), qPrintable(hit.lastError().text()));
                QVERIFY2(hit.value(0).toInt() == 0, qPrintable(table + "." + column));
                ++scannedColumns;
            }
        }
        QVERIFY(scannedColumns > 10);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM events WHERE instr(COALESCE(changes,''), '$argon2id$') > 0"), 0);
    }

    void interactiveVerifierFormat() {
        PasswordHasher interactive;
        const auto first = interactive.hash(Password);
        const auto second = interactive.hash(Password);
        QVERIFY(first.startsWith("$argon2id$v=19$m=65536,t=2,p=1$"));
        QVERIFY(first != second);
        const QByteArray unicode = QStringLiteral("ação, coração e café ☕🙂 longa").toUtf8();
        QVERIFY(interactive.verify(interactive.hash(unicode), unicode));
        QVERIFY(!interactive.verify(first, unicode));
    }

    void authenticateGeneric() {
        const auto path = freshPath("generic");
        LocalDatabase db(path);
        AuthService auth(db, fastHasher);
        auth.bootstrapPrincipal("principal", auth.preparePassword(Password));
        QSqlQuery fixture(db.connection());
        fixture.prepare("INSERT INTO accounts(username,password_verifier,role,active,admin_slot,created_at) "
                        "VALUES('comum',?,'common',1,NULL,?)");
        fixture.addBindValue(fastHasher.hash(Password));
        fixture.addBindValue(LocalDatabase::nowUtc());
        QVERIFY2(fixture.exec(), qPrintable(fixture.lastError().text()));
        QVERIFY(auth.authenticate("comum", Password).has_value());
        QVERIFY(execAux(path, "UPDATE accounts SET active=0 WHERE username='comum'"));
        QVERIFY(!auth.authenticate("principal", "senha errada mas comprida").has_value());
        QVERIFY(!auth.authenticate("principal", Password + " ").has_value());
        QVERIFY(!auth.authenticate("ninguem", Password).has_value());
        QVERIFY(!auth.authenticate("comum", Password).has_value());
        QVERIFY(!auth.authenticate("x", Password).has_value());
        const auto actor = auth.authenticate("  PRINCIPAL ", Password);
        QVERIFY(actor.has_value());
        QCOMPARE(actor->username, QStringLiteral("principal"));
    }

    void passwordPolicy() {
        LocalDatabase db(freshPath("policy"));
        AuthService auth(db, fastHasher);
        QCOMPARE(errorCode([&] {
                     auth.preparePassword(QByteArray(7, 'a'));
                 }),
                 QStringLiteral("invalid_password"));
        QCOMPARE(errorCode([&] {
                     auth.preparePassword(QByteArray(129, 'a'));
                 }),
                 QStringLiteral("invalid_password"));
        QCOMPARE(errorCode([&] {
                     auth.preparePassword(QByteArray(8, 'a'));
                 }),
                 QString());
        QCOMPARE(errorCode([&] {
                     auth.preparePassword(QByteArray(128, 'a'));
                 }),
                 QString());
        QCOMPARE(errorCode([&] {
                     auth.preparePassword(QStringLiteral("🙂").repeated(8).toUtf8());
                 }),
                 QString());
        QCOMPARE(errorCode([&] {
                     auth.preparePassword(QStringLiteral("🙂").repeated(7).toUtf8());
                 }),
                 QStringLiteral("invalid_password"));
        QCOMPARE(errorCode([] {
                     AuthService::canonicalUsername("ab");
                 }),
                 QStringLiteral("invalid_username"));
        QCOMPARE(errorCode([] {
                     AuthService::canonicalUsername("nome com espaço");
                 }),
                 QStringLiteral("invalid_username"));
        QCOMPARE(AuthService::canonicalUsername("  Operador.Um "), QStringLiteral("operador.um"));
    }

    void adminProtocolRejectsInvalid() {
        LocalDatabase db(freshPath("protocol"));
        AuthService auth(db, fastHasher);
        MemorySecretVault vault;
        RecoveryService recovery(db, auth, vault);
        const QString marker = QStringLiteral("marcador-secreto-123456");
        auto request = [&](QJsonObject object) {
            return QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        struct Served {
            int exitCode;
            QList<QByteArray> replies;
            int unread;
        };
        auto serve = [&](QList<QByteArray> lines) {
            Served served{};
            served.exitCode = AdminProtocol::serve(
                [&](int) -> std::optional<QByteArray> {
                    if (lines.isEmpty())
                        return std::nullopt;
                    return lines.takeFirst();
                },
                [&](const QByteArray &json) {
                    served.replies << json;
                },
                auth, recovery);
            served.unread = int(lines.size());
            return served;
        };
        auto field = [](const QByteArray &json, const char *name) {
            return QJsonDocument::fromJson(json).object().value(name).toString();
        };
        const QJsonObject valid{
            {"protocol", 1}, {"action", "bootstrap"}, {"username", "principal"}, {"password", marker}};
        QJsonObject oversized = valid;
        oversized["password"] = marker + QString(17000, 'x');
        QJsonObject protocol2 = valid;
        protocol2["protocol"] = 2;
        QJsonObject unknownAction = valid;
        unknownAction["action"] = "reset";
        QJsonObject extraField = valid;
        extraField["db"] = "outro.sqlite";
        QJsonObject numericField = valid;
        numericField["username"] = 5;
        const QJsonObject verify{{"protocol", 1},
                                 {"action", "recover_verify"},
                                 {"username", "principal"},
                                 {"method", "backup_code"},
                                 {"proof", marker}};
        QJsonObject unknownMethod = verify;
        unknownMethod["method"] = "email";
        QJsonObject numericProof = verify;
        numericProof["proof"] = 123456;
        QJsonObject missingProof = verify;
        missingProof.remove("proof");
        QJsonObject verifyExtra = verify;
        verifyExtra["db"] = "outro.sqlite";
        QJsonObject singleShot = verify;
        singleShot["password"] = marker;
        QJsonObject removedAction = singleShot;
        removedAction["action"] = "recover";
        const QJsonObject commitFirst{{"action", "recover_commit"}, {"password", marker}};
        const QList<QByteArray> requests{request(oversized),     QByteArray("{\"protocol\":1,") + marker.toUtf8(),
                                         request(protocol2),     request(unknownAction),
                                         request(extraField),    request(numericField),
                                         request(unknownMethod), request(numericProof),
                                         request(missingProof),  request(verifyExtra),
                                         request(singleShot),    request(removedAction),
                                         request(commitFirst)};
        for (const auto &input : requests) {
            const auto served = serve({input});
            QCOMPARE(served.exitCode, 2);
            QCOMPARE(served.replies.size(), 1);
            QVERIFY(!served.replies[0].contains(marker.toUtf8()));
            QVERIFY(!served.replies[0].contains("outro.sqlite"));
        }
        QCOMPARE(field(serve({request(protocol2)}).replies.value(0), "code"), QStringLiteral("unsupported_protocol"));
        QCOMPARE(serve({}).exitCode, 2);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM accounts"), 0);
        QJsonObject shortPassword = valid;
        shortPassword["password"] = "curta";
        QCOMPARE(serve({request(shortPassword)}).exitCode, 1);
        QCOMPARE(serve({request(valid)}).exitCode, 0);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM accounts"), 1);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM events WHERE entity_type='recovery'"), 0);
        // Prova errada termina na conferência: a segunda linha nunca é lida.
        const auto wrong = serve({request(verify), request(commitFirst)});
        QCOMPARE(wrong.exitCode, 1);
        QCOMPARE(wrong.replies.size(), 1);
        QCOMPARE(wrong.unread, 1);
        QVERIFY(!wrong.replies[0].contains(marker.toUtf8()));
        QCOMPARE(
            field(wrong.replies[0], "message"),
            QStringLiteral("Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método."));
        QCOMPARE(scalar(db, "SELECT failed_attempts FROM recovery_config"), 1);
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM events WHERE entity_type='recovery' AND action='Tentativa de "
                            "recuperação recusada'"),
                 1);
    }

    void guardsOutsideButtons() {
        using Reason = AuthorizationError::Reason;
        const auto path = freshPath("guards");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const qint64 id = ctx.store.notebooks().first().id;
        addCommonAccount(ctx, "inativa");
        addCommonAccount(ctx, "alterada");

        AuthService otherInstance(ctx.database, ctx.hasher, ctx.clock);
        const Session foreign = otherInstance.login(TestPrincipal, TestPassword);
        Session ended = ctx.auth.login(TestPrincipal, TestPassword);
        const Session endedCopy = ended;
        ctx.auth.logout(ended);
        QVERIFY(!ended.isValid());
        const Session inactive = ctx.auth.login("inativa", TestPassword);
        QVERIFY(execAux(path, "UPDATE accounts SET active=0 WHERE username='inativa'"));
        const Session revoked = ctx.auth.login("alterada", TestPassword);
        QVERIFY(execAux(path, "UPDATE accounts SET credential_version=credential_version+1 WHERE username='alterada'"));

        const QList<QPair<Session, Reason>> cases{{Session(), Reason::NotAuthenticated},
                                                  {foreign, Reason::NotAuthenticated},
                                                  {endedCopy, Reason::NotAuthenticated},
                                                  {inactive, Reason::Inactive},
                                                  {revoked, Reason::Revoked}};
        const auto before = inventoryState(ctx.database);
        const auto historyBefore = ctx.store.history().size();
        for (const auto &item : cases) {
            const Session &session = item.first;
            const Reason reason = item.second;
            QCOMPARE(refusal(
                         [&] {
                             Fixture::add(ctx, session, "NB-2", "HP", "Armário 2");
                         },
                         reason),
                     QString());
            QCOMPARE(refusal(
                         [&] {
                             Fixture::checkout(ctx, session, id, "Ana", ResponsibleRole::Professor, "Sala 12");
                         },
                         reason),
                     QString());
            QCOMPARE(refusal(
                         [&] {
                             Fixture::checkin(ctx, session, id, "Carlos", ResponsibleRole::Assistant, "Armário 1");
                         },
                         reason),
                     QString());
            QCOMPARE(refusal(
                         [&] {
                             Fixture::updateLocation(ctx, session, id, "Sala 3");
                         },
                         reason),
                     QString());
            QCOMPARE(refusal(
                         [&] {
                             ctx.store.setMaintenance(session, id, true);
                         },
                         reason),
                     QString());
            QCOMPARE(inventoryState(ctx.database), before);
            QCOMPARE(ctx.store.history().size(), historyBefore);
        }
        Fixture::updateLocation(ctx, ctx.session, id, "Sala 3");
        QCOMPARE(ctx.store.notebooks().first().location, QStringLiteral("Sala 3"));
    }

    void genericLoginFailure() {
        const auto path = freshPath("login");
        TestContext ctx(path);
        addCommonAccount(ctx, "inativa");
        QVERIFY(execAux(path, "UPDATE accounts SET active=0 WHERE username='inativa'"));
        auto outcome = [&](const QString &username, const QByteArray &password) {
            try {
                ctx.auth.login(username, password);
                return QStringLiteral("aceito");
            } catch (const AuthError &error) {
                return error.code() + "|" + QString::fromUtf8(error.what());
            }
        };
        QCOMPARE(outcome("ninguem", TestPassword), GenericLoginFailure);
        QCOMPARE(outcome(TestPrincipal, "senha errada mas comprida"), GenericLoginFailure);
        QCOMPARE(outcome(TestPrincipal, TestPassword + " "), GenericLoginFailure);
        QCOMPARE(outcome(TestPrincipal, " " + TestPassword), GenericLoginFailure);
        QCOMPARE(outcome("inativa", TestPassword), GenericLoginFailure);
        QCOMPARE(outcome("x", TestPassword), GenericLoginFailure);
        const auto unverified = ctx.auth.prepareLogin(TestPrincipal, TestPassword);
        QCOMPARE(errorCode([&] {
                     ctx.auth.completeLogin(*unverified);
                 }),
                 QStringLiteral("invalid_credentials"));
        const auto session = ctx.auth.login("  PRINCIPAL ", TestPassword);
        QVERIFY(session.isValid());
        QCOMPARE(session.username(), TestPrincipal);
        QCOMPARE(session.role(), AccountRole::Principal);
        QVERIFY(!session.mustChangePassword());
        QCOMPARE(session.issuedAtUtc(), ctx.clock.nowUtc());
    }

    void inventoryEventsAuthored() {
        TestContext ctx(freshPath("authored"));
        const auto lastBefore = scalar(ctx.database, "SELECT MAX(id) FROM events");
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const qint64 id = ctx.store.notebooks().first().id;
        Fixture::checkout(ctx, ctx.session, id, "Ana", ResponsibleRole::Professor, "Sala 12");
        Fixture::checkin(ctx, ctx.session, id, "Carlos", ResponsibleRole::Assistant, "Armário 1");
        Fixture::updateLocation(ctx, ctx.session, id, "Sala 3");
        ctx.store.setMaintenance(ctx.session, id, true);
        QSqlQuery q(ctx.database.connection());
        q.prepare("SELECT action, actor_account_id, actor_username, changes FROM events WHERE id>? ORDER BY id");
        q.addBindValue(lastBefore);
        QVERIFY(q.exec());
        QStringList actions;
        while (q.next()) {
            actions << q.value(0).toString();
            QCOMPARE(q.value(1).toLongLong(), ctx.session.accountId());
            QCOMPARE(q.value(2).toString(), TestPrincipal);
            const auto changes = QJsonDocument::fromJson(q.value(3).toString().toUtf8()).object();
            QVERIFY2(!changes.isEmpty(), qPrintable(q.value(0).toString()));
            for (auto it = changes.begin(); it != changes.end(); ++it) {
                QVERIFY(it.value().isObject());
                QCOMPARE(it.value().toObject().keys(), (QStringList{"de", "para"}));
            }
        }
        QCOMPARE(actions, (QStringList{"Cadastro", "Retirada", "Devolução", "Localização", "Manutenção"}));
    }

    void seedDemoRefusesOperational() {
        TestContext ctx(freshPath("demo-new"), false);
        try {
            seedDemo(ctx.auth, ctx.store, ctx.accounts, DataProfile::Operational, ctx.locations, ctx.people);
            QFAIL("seedDemo aceitou o perfil operacional");
        } catch (const std::runtime_error &e) {
            QCOMPARE(QString::fromUtf8(e.what()),
                     QStringLiteral("Dados de demonstração só podem ser criados no banco de demonstração."));
        }
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts"), 0);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), 0);
        const Session session =
            seedDemo(ctx.auth, ctx.store, ctx.accounts, DataProfile::Demo, ctx.locations, ctx.people);
        QCOMPARE(session.username(), QStringLiteral("demo.principal"));
        QCOMPARE(session.role(), AccountRole::Principal);
        QCOMPARE(ctx.store.notebooks().size(), 5);
        QCOMPARE(ctx.people.activePeople().size(), 4);
        QCOMPARE(ctx.people.activeHomonyms("ana souza").size(), 2);
        QCOMPARE(ctx.people.activeHomonyms("ana souza").first().subject, QString("Matemática"));
        int perStatus[3] = {0, 0, 0};
        QString note;
        for (const auto &n : ctx.store.notebooks()) {
            ++perStatus[static_cast<int>(n.status)];
            if (n.status == Status::Maintenance)
                note = n.maintenanceNote;
        }
        QCOMPARE(perStatus[0], 2);
        QCOMPARE(perStatus[1], 2);
        QCOMPARE(perStatus[2], 1);
        QCOMPARE(note, QString("Bateria não carrega"));
        QCOMPARE(scalar(ctx.database,
                        "SELECT COUNT(*) FROM loans WHERE returned_at IS NOT NULL AND returned_by_person_id "
                        "IS NOT NULL AND returned_by_person_id<>borrower_person_id"),
                 2);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM notebooks WHERE active=0 AND asset_tag='NB-005'"), 1);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM people WHERE active=0"), 1);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM locations WHERE name='Sala 7' AND deleted_at IS NOT NULL"),
                 1);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events e JOIN accounts a ON a.id=e.actor_account_id WHERE "
                                      "a.username='demo.operador' AND e.action IN ('Retirada','Devolução')"),
                 2);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM notebooks WHERE location_id IS NULL"), 0);
        QCOMPARE(ctx.locations.activeLocationNames(),
                 QStringList({"Armário 1", "Biblioteca", "Coordenação", "Laboratório", "Sala 12", "Suporte Técnico"}));
        QCOMPARE(ctx.store.history().first().actorUsername, QStringLiteral("demo.principal"));
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts WHERE username='demo.operador' AND role='common' "
                                      "AND must_change_password=0"),
                 1);
        const auto events = scalar(ctx.database, "SELECT COUNT(*) FROM events");
        seedDemo(ctx.auth, ctx.store, ctx.accounts, DataProfile::Demo, ctx.locations, ctx.people);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts"), 2);
        QCOMPARE(ctx.store.notebooks().size(), 5);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), events);
    }

    void seedDemoOverLegacyDemo() {
        const auto path = freshPath("demo-legacy");
        const QStringList legacy{
            "CREATE TABLE notebooks(id INTEGER PRIMARY KEY, asset_tag TEXT NOT NULL COLLATE NOCASE UNIQUE, name TEXT "
            "NOT NULL, status TEXT NOT NULL DEFAULT 'available' CHECK(status IN ('available','loaned','maintenance')), "
            "location TEXT)",
            "CREATE TABLE loans(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES notebooks(id), "
            "borrower TEXT NOT NULL, checked_out_at TEXT NOT NULL, returned_at TEXT, borrower_role TEXT "
            "CHECK(borrower_role IN ('professor','assistant')), returned_by TEXT, returned_by_role TEXT "
            "CHECK(returned_by_role IN ('professor','assistant')))",
            "CREATE UNIQUE INDEX one_open_loan ON loans(notebook_id) WHERE returned_at IS NULL",
            "CREATE INDEX loans_by_notebook ON loans(notebook_id,id)",
            "CREATE TABLE events(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES notebooks(id), action "
            "TEXT NOT NULL, details TEXT NOT NULL, happened_at TEXT NOT NULL)",
            "INSERT INTO notebooks VALUES(1,'NB-001','Dell Latitude 5420','available','Armário 1'),(2,'NB-002','Lenovo "
            "ThinkPad E14','maintenance','Armário 1'),(3,'NB-003','HP ProBook 440','available','Armário "
            "1'),(4,'NB-004','Dell Inspiron 15','loaned','Sala 12')",
            "INSERT INTO loans VALUES(1,4,'Marina Costa','2026-01-01T09:00:00.000Z',NULL,'professor',NULL,NULL)",
            "INSERT INTO events VALUES(1,1,'Cadastro','Notebook cadastrado: Dell Latitude 5420. Local: Armário "
            "1','2026-01-01T08:00:00.000Z'),(2,4,'Retirada','Retirado por: Marina Costa (Professor). Local: Sala "
            "12','2026-01-01T09:00:00.000Z')",
            "PRAGMA user_version=2"};
        for (const auto &statement : legacy)
            QVERIFY2(execAux(path, statement), qPrintable(statement));
        TestContext ctx(path, false);
        QCOMPARE(scalar(ctx.database, "PRAGMA user_version"), qint64(LocalDatabase::CurrentSchemaVersion));
        const Session session =
            seedDemo(ctx.auth, ctx.store, ctx.accounts, DataProfile::Demo, ctx.locations, ctx.people);
        QCOMPARE(session.username(), QStringLiteral("demo.principal"));
        QCOMPARE(ctx.store.notebooks().size(), 5);
        const auto accounts = scalar(ctx.database, "SELECT COUNT(*) FROM accounts");
        const auto events = scalar(ctx.database, "SELECT COUNT(*) FROM events");
        QCOMPARE(accounts, 2);
        seedDemo(ctx.auth, ctx.store, ctx.accounts, DataProfile::Demo, ctx.locations, ctx.people);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM accounts"), accounts);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM events"), events);
        QCOMPARE(ctx.store.notebooks().size(), 5);
    }

    void seedDemoDataset150IsExactAndIdempotent() {
        TestContext ctx(freshPath("demo-150"), false);
        seedDemoDataset150(ctx.auth, ctx.store, ctx.accounts, DataProfile::Demo, ctx.locations, ctx.people);
        QCOMPARE(ctx.people.people(CatalogFilter::All).size(), 50);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM people WHERE role='professor'"), 25);
        QCOMPARE(scalar(ctx.database, "SELECT COUNT(*) FROM people WHERE role='assistant'"), 25);
        QCOMPARE(ctx.locations.locations(CatalogFilter::All).size(), 50);
        QCOMPARE(ctx.store.notebooks().size(), 50);
        const auto eventsAfterFirstSeed = ctx.store.history().size();
        seedDemoDataset150(ctx.auth, ctx.store, ctx.accounts, DataProfile::Demo, ctx.locations, ctx.people);
        QCOMPARE(ctx.people.people(CatalogFilter::All).size(), 50);
        QCOMPARE(ctx.locations.locations(CatalogFilter::All).size(), 50);
        QCOMPARE(ctx.store.notebooks().size(), 50);
        QCOMPARE(ctx.store.history().size(), eventsAfterFirstSeed);
    }

    void authorizeOutsideTransaction() {
        TestContext ctx(freshPath("outside"));
        try {
            ctx.auth.authorize(ctx.session, Permission::OperateInventory);
            QFAIL("authorize aceito fora de transação");
        } catch (const AuthorizationError &) {
            QFAIL("deveria recusar por estar fora da transação, não por autorização");
        } catch (const std::runtime_error &) {
        }
        Actor actor{0, {}};
        ctx.database.transaction([&] {
            actor = ctx.auth.authorize(ctx.session, Permission::OperateInventory);
        });
        QCOMPARE(actor.accountId, ctx.session.accountId());
        QCOMPARE(actor.username, TestPrincipal);
    }
};

QTEST_GUILESS_MAIN(AuthTests)
#include "AuthTests.moc"

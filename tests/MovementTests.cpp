#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QSqlRecord>
#include "TestSupport.h"

// Fase 3: movimentações por IDs (retirada, devolução, local, manutenção, cadastro/edição).
class MovementTests : public QObject {
    Q_OBJECT
    template<typename Action> static QString errorOf(Action action) {
        try { action(); } catch (const std::exception &e) { return QString::fromUtf8(e.what()); }
        return {};
    }
    static QSqlRecord row(TestContext &ctx, const QString &sql) {
        QSqlQuery q(ctx.database.connection());
        if (!q.exec(sql) || !q.next()) return {};
        return q.record();
    }
    static QString scalar(TestContext &ctx, const QString &sql) {
        QSqlQuery q(ctx.database.connection());
        if (!q.exec(sql) || !q.next()) return QStringLiteral("ERRO");
        return q.value(0).isNull() ? QStringLiteral("<null>") : q.value(0).toString();
    }
    static int count(TestContext &ctx, const QString &table) { return scalar(ctx, "SELECT COUNT(*) FROM " + table).toInt(); }
    static Notebook notebook(TestContext &ctx, qint64 id) {
        for (const auto &n : ctx.store.notebooks()) if (n.id == id) return n;
        return {};
    }
    static qint64 addNotebook(TestContext &ctx, const QString &tag, qint64 locationId) {
        return ctx.store.add(ctx.session, tag, "Dell " + tag, locationId);
    }
    static Session common(TestContext &ctx) {
        return ctx.createCommonAndLogin("operador", "senha-operador-segura", InitialPasswordMode::Assisted);
    }
    static void failEvents(const QString &path, bool enabled) {
        QVERIFY(LegacyFixtures::execAll(path, {enabled ? "CREATE TRIGGER fail_audit BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'simulated audit failure'); END"
                                                       : "DROP TRIGGER fail_audit"}));
    }
    static QString rowText(TestContext &ctx, const QString &sql) {
        const auto record = row(ctx, sql); QStringList values;
        for (int i = 0; i < record.count(); ++i) values << (record.value(i).isNull() ? QStringLiteral("<null>") : record.value(i).toString());
        return values.join('|');
    }
private slots:
    // ---------------- 03-01: retirada ----------------
    void checkoutByIdsRecordsSnapshotAndOrigin() {
        QTemporaryDir temp; const auto path = temp.filePath("checkout.sqlite"); qint64 nb, ana, support, room, operatorId;
        {
            TestContext ctx(path);
            support = createLocation(ctx, "Suporte Técnico"); room = createLocation(ctx, "Sala 12");
            nb = addNotebook(ctx, "NB-001", support);
            ana = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, "Matemática");
            const auto operatorSession = common(ctx); operatorId = operatorSession.accountId();
            ctx.store.checkout(operatorSession, nb, ana, room);
        }
        TestContext ctx(path);
        const auto n = notebook(ctx, nb);
        QCOMPARE(n.status, Status::Loaned); QCOMPARE(n.location, QString("Sala 12")); QCOMPARE(n.locationId, room);
        QCOMPARE(n.borrowerPersonId, ana); QCOMPARE(n.borrower, QString("Ana Souza")); QCOMPARE(n.borrowerSubject, QString("Matemática"));
        const auto loan = row(ctx, "SELECT id,origin_location_id,origin_location,checkout_location_id,checkout_location,borrower_person_id FROM loans");
        QCOMPARE(loan.value(1).toLongLong(), support); QCOMPARE(loan.value(2).toString(), QString("Suporte Técnico"));
        QCOMPARE(loan.value(3).toLongLong(), room); QCOMPARE(loan.value(4).toString(), QString("Sala 12")); QCOMPARE(loan.value(5).toLongLong(), ana);
        const auto event = row(ctx, "SELECT loan_id,location_id,from_location_id,actor_account_id,actor_username,details FROM events WHERE action='Retirada'");
        QCOMPARE(event.value(0).toLongLong(), loan.value(0).toLongLong()); QCOMPARE(event.value(1).toLongLong(), room);
        QCOMPARE(event.value(2).toLongLong(), support); QCOMPARE(event.value(3).toLongLong(), operatorId);
        QCOMPARE(event.value(4).toString(), QString("operador"));
        QCOMPARE(event.value(5).toString(), QString("Retirado por: Ana Souza (Professor · Matemática). Local: Sala 12"));
    }
    void checkoutSharesTimestampWithEvent() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("instant.sqlite"));
        const auto from = createLocation(ctx, "Armário 1"), to = createLocation(ctx, "Sala 3");
        const auto nb = addNotebook(ctx, "NB-1", from); const auto bia = createPerson(ctx, "Bia", ResponsibleRole::Assistant);
        ctx.store.checkout(ctx.session, nb, bia, to);
        QCOMPARE(scalar(ctx, "SELECT l.checked_out_at=e.happened_at FROM loans l JOIN events e ON e.loan_id=l.id AND e.action='Retirada'"), QString("1"));
        const auto changes = QJsonDocument::fromJson(scalar(ctx, "SELECT changes FROM events WHERE action='Retirada'").toUtf8()).object();
        QCOMPARE(changes.value("location_id").toObject().value("de").toInteger(), from);
        QCOMPARE(changes.value("location_id").toObject().value("para").toInteger(), to);
        QCOMPARE(changes.value("location").toObject().value("de").toString(), QString("Armário 1"));
        QCOMPARE(changes.value("status").toObject().value("para").toString(), QString("loaned"));
    }
    void checkoutRequiresIds() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("ids.sqlite"));
        const auto place = createLocation(ctx, "Sala"); const auto nb = addNotebook(ctx, "NB-1", place);
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor); const auto events = count(ctx, "events");
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, nb, 0, place); }), QString("Selecione a pessoa responsável pela retirada."));
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, nb, ana, 0); }), QString("Selecione o local de uso."));
        QCOMPARE(count(ctx, "loans"), 0); QCOMPARE(count(ctx, "events"), events);
    }
    void checkoutRefusesInactivePerson() {
        QTemporaryDir temp; const auto path = temp.filePath("inactive-person.sqlite");
        TestContext a(path); TestContext b(path);
        const auto place = createLocation(a, "Sala"); const auto nb = addNotebook(a, "NB-1", place);
        const auto ana = createPerson(a, "Ana", ResponsibleRole::Professor);
        QVERIFY(!a.people.activePeople().isEmpty()); // A leu a pessoa ativa
        b.people.inactivate(b.session, ana);          // B inativa antes da confirmação de A
        const auto events = count(a, "events");
        QCOMPARE(errorOf([&] { a.store.checkout(a.session, nb, ana, place); }), QString("Esta pessoa está inativa e não pode ser selecionada."));
        QCOMPARE(errorOf([&] { a.store.checkout(a.session, nb, 999, place); }), QString("Pessoa não encontrada."));
        QCOMPARE(count(a, "loans"), 0); QCOMPARE(count(a, "events"), events); QCOMPARE(notebook(a, nb).status, Status::Available);
    }
    void checkoutRefusesDeletedLocation() {
        QTemporaryDir temp; const auto path = temp.filePath("deleted-location.sqlite");
        TestContext a(path); TestContext b(path);
        const auto home = createLocation(a, "Armário"); const auto room = createLocation(a, "Sala 9");
        const auto nb = addNotebook(a, "NB-1", home); const auto ana = createPerson(a, "Ana", ResponsibleRole::Professor);
        QCOMPARE(a.locations.locations(CatalogFilter::Active).size(), 2);
        b.locations.softDelete(b.session, room);
        const auto events = count(a, "events");
        QCOMPARE(errorOf([&] { a.store.checkout(a.session, nb, ana, room); }),
                 QString("O local Sala 9 foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais."));
        QCOMPARE(errorOf([&] { a.store.checkout(a.session, nb, ana, 999); }), QString("Local não encontrado."));
        QCOMPARE(count(a, "loans"), 0); QCOMPARE(count(a, "events"), events); QCOMPARE(notebook(a, nb).locationId, home);
    }
    void checkoutRefusesUnavailableOrInactiveNotebook() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("unavailable.sqlite"));
        const auto place = createLocation(ctx, "Sala"); const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        const auto lent = addNotebook(ctx, "NB-1", place), fixing = addNotebook(ctx, "NB-2", place), retired = addNotebook(ctx, "NB-3", place);
        ctx.store.checkout(ctx.session, lent, ana, place);
        ctx.store.setMaintenance(ctx.session, fixing, true);
        ctx.store.inactivate(ctx.session, retired);
        const auto events = count(ctx, "events");
        const QString unavailable = "Somente notebooks disponíveis podem ser retirados. Atualize a seleção.";
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, lent, ana, place); }), unavailable);
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, fixing, ana, place); }), unavailable);
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, retired, ana, place); }), QString("Notebook inativo não pode ser movimentado."));
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, 999, ana, place); }), QString("Notebook não encontrado."));
        QCOMPARE(count(ctx, "events"), events); QCOMPARE(count(ctx, "loans"), 1);
    }
    void checkoutHomonymsKeepDistinctIds() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("homonyms.sqlite"));
        const auto place = createLocation(ctx, "Sala");
        const auto first = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, "Matemática");
        const auto second = createPerson(ctx, "Ana Souza", ResponsibleRole::Assistant);
        const auto a = addNotebook(ctx, "NB-1", place), b = addNotebook(ctx, "NB-2", place);
        ctx.store.checkout(ctx.session, a, first, place);
        const auto before = scalar(ctx, "SELECT borrower_person_id||'|'||borrower_role FROM loans WHERE notebook_id=" + QString::number(a));
        ctx.store.checkout(ctx.session, b, second, place);
        QCOMPARE(notebook(ctx, b).borrowerPersonId, second); QCOMPARE(notebook(ctx, b).borrowerRole, ResponsibleRole::Assistant);
        QCOMPARE(scalar(ctx, "SELECT borrower_person_id||'|'||borrower_role FROM loans WHERE notebook_id=" + QString::number(a)), before);
        QCOMPARE(notebook(ctx, a).borrowerPersonId, first);
    }
    void checkoutSnapshotSurvivesPersonEdit() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("snapshot.sqlite"));
        const auto place = createLocation(ctx, "Sala"); const auto nb = addNotebook(ctx, "NB-1", place);
        const auto ana = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, "Matemática");
        ctx.store.checkout(ctx.session, nb, ana, place);
        Person expected{ana, "Ana Souza", ResponsibleRole::Professor, "Matemática", true};
        ctx.people.update(ctx.session, expected, PersonDraft{"Ana Lima", ResponsibleRole::Assistant, {}});
        const auto n = notebook(ctx, nb);
        QCOMPARE(n.borrower, QString("Ana Souza")); QCOMPARE(n.borrowerRole, ResponsibleRole::Professor); QCOMPARE(n.borrowerSubject, QString("Matemática"));
        QCOMPARE(scalar(ctx, "SELECT borrower||'|'||borrower_role||'|'||borrower_subject FROM loans"), QString("Ana Souza|professor|Matemática"));
        QCOMPARE(scalar(ctx, "SELECT details FROM events WHERE action='Retirada'"), QString("Retirado por: Ana Souza (Professor · Matemática). Local: Sala"));
    }
    void checkoutFromDeletedLocationKeepsOrigin() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("deleted-origin.sqlite"));
        const auto old = createLocation(ctx, "Depósito antigo"), room = createLocation(ctx, "Sala 4");
        const auto nb = addNotebook(ctx, "NB-1", old); ctx.locations.softDelete(ctx.session, old);
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        ctx.store.checkout(ctx.session, nb, ana, room);
        QCOMPARE(scalar(ctx, "SELECT origin_location_id||'|'||origin_location FROM loans"), QString::number(old) + "|Depósito antigo");
        QCOMPARE(scalar(ctx, "SELECT from_location_id FROM events WHERE action='Retirada'"), QString::number(old));
    }
    void checkoutAuthorization() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("auth.sqlite"));
        const auto place = createLocation(ctx, "Sala"); const auto nb = addNotebook(ctx, "NB-1", place);
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        const auto operatorSession = common(ctx);
        const auto events = count(ctx, "events");
        QVERIFY_EXCEPTION_THROWN(ctx.store.checkout(Session(), nb, ana, place), AuthorizationError);
        ctx.clock.advance(15 * 60);
        try { ctx.store.checkout(ctx.session, nb, ana, place); QFAIL("Sessão expirada deveria ser recusada."); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(), AuthorizationError::Reason::Expired); }
        QCOMPARE(count(ctx, "events"), events); QCOMPARE(count(ctx, "loans"), 0);
        ctx.store.checkout(operatorSession, nb, ana, place);
        QCOMPARE(notebook(ctx, nb).status, Status::Loaned);
    }
    void checkoutRollbackOnEventFailure() {
        QTemporaryDir temp; const auto path = temp.filePath("rollback.sqlite"); TestContext ctx(path);
        const auto home = createLocation(ctx, "Armário"), room = createLocation(ctx, "Sala");
        const auto nb = addNotebook(ctx, "NB-1", home); const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        failEvents(path, true);
        QVERIFY_EXCEPTION_THROWN(ctx.store.checkout(ctx.session, nb, ana, room), std::runtime_error);
        const auto n = notebook(ctx, nb);
        QCOMPARE(n.status, Status::Available); QCOMPARE(n.locationId, home); QCOMPARE(count(ctx, "loans"), 0);
        failEvents(path, false);
    }
    // ---------------- 03-02: matriz, local por id e manutenção ----------------
    void transitionsRefusedOutsideMatrix() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("matrix.sqlite"));
        const auto home = createLocation(ctx, "Armário"), room = createLocation(ctx, "Sala");
        const auto nb = addNotebook(ctx, "NB-1", home); const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        const auto events = count(ctx, "events");
        QCOMPARE(errorOf([&] { ctx.store.setMaintenance(ctx.session, nb, false); }), QString("Este notebook não está em manutenção."));
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, nb, ana, room); }), QString("Somente notebooks emprestados podem ser devolvidos. Atualize a seleção."));
        ctx.store.checkout(ctx.session, nb, ana, room);
        QCOMPARE(errorOf([&] { ctx.store.setMaintenance(ctx.session, nb, true); }), QString("Transição inválida. Devolva o aparelho antes de colocá-lo em manutenção."));
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, nb, ana, home); }), QString("Somente notebooks disponíveis podem ser retirados. Atualize a seleção."));
        QCOMPARE(count(ctx, "events"), events + 1);
        ctx.store.checkin(ctx.session, nb, ana,Fixture::location(ctx,"Armário")); ctx.store.setMaintenance(ctx.session, nb, true);
        QCOMPARE(errorOf([&] { ctx.store.checkout(ctx.session, nb, ana, room); }), QString("Somente notebooks disponíveis podem ser retirados. Atualize a seleção."));
        QCOMPARE(errorOf([&] { ctx.store.setMaintenance(ctx.session, nb, true); }), QString("Transição inválida. Devolva o aparelho antes de colocá-lo em manutenção."));
        QCOMPARE(notebook(ctx, nb).status, Status::Maintenance);
    }
    void updateLocationById() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("location.sqlite"));
        const auto support = createLocation(ctx, "Suporte Técnico"), room = createLocation(ctx, "Sala 12"), lab = createLocation(ctx, "Laboratório");
        const auto nb = addNotebook(ctx, "NB-001", support);
        ctx.store.updateLocation(common(ctx), nb, room);
        auto n = notebook(ctx, nb); QCOMPARE(n.location, QString("Sala 12")); QCOMPARE(n.locationId, room);
        const auto event = row(ctx, "SELECT details,location_id,from_location_id,changes FROM events WHERE action='Localização'");
        QCOMPARE(event.value(0).toString(), QString("Local alterado: Suporte Técnico → Sala 12"));
        QCOMPARE(event.value(1).toLongLong(), room); QCOMPARE(event.value(2).toLongLong(), support);
        const auto changes = QJsonDocument::fromJson(event.value(3).toString().toUtf8()).object();
        QCOMPARE(changes.value("location").toObject().value("para").toString(), QString("Sala 12"));
        QCOMPARE(changes.value("location_id").toObject().value("de").toInteger(), support);
        // Emprestado também pode mudar de local; o loan aberto não muda.
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        ctx.store.checkout(ctx.session, nb, ana, support);
        const auto loanBefore = scalar(ctx, "SELECT checkout_location_id||'|'||origin_location_id||'|'||checkout_location FROM loans");
        ctx.store.updateLocation(ctx.session, nb, lab);
        QCOMPARE(notebook(ctx, nb).location, QString("Laboratório")); QCOMPARE(notebook(ctx, nb).status, Status::Loaned);
        QCOMPARE(scalar(ctx, "SELECT checkout_location_id||'|'||origin_location_id||'|'||checkout_location FROM loans"), loanBefore);
    }
    void updateLocationRefusals() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("location-refusals.sqlite"));
        const auto home = createLocation(ctx, "Armário"), room = createLocation(ctx, "Sala"), gone = createLocation(ctx, "Depósito");
        const auto nb = addNotebook(ctx, "NB-1", home); ctx.locations.softDelete(ctx.session, gone);
        const auto events = count(ctx, "events");
        QCOMPARE(errorOf([&] { ctx.store.updateLocation(ctx.session, nb, home); }), QString("Informe um local diferente do atual."));
        QCOMPARE(errorOf([&] { ctx.store.updateLocation(ctx.session, nb, qint64(0)); }), QString("Selecione o novo local."));
        QCOMPARE(errorOf([&] { ctx.store.updateLocation(ctx.session, nb, gone); }), QString("O local Depósito foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais."));
        QCOMPARE(errorOf([&] { ctx.store.updateLocation(ctx.session, nb, qint64(9999)); }), QString("Local não encontrado."));
        QVERIFY_EXCEPTION_THROWN(ctx.store.updateLocation(Session(), nb, room), AuthorizationError);
        ctx.store.setMaintenance(ctx.session, nb, true);
        QCOMPARE(errorOf([&] { ctx.store.updateLocation(ctx.session, nb, room); }), QString("Conclua a manutenção antes de atualizar o local."));
        ctx.store.setMaintenance(ctx.session, nb, false); ctx.store.inactivate(ctx.session, nb);
        const auto afterSetup = count(ctx, "events");
        QCOMPARE(errorOf([&] { ctx.store.updateLocation(ctx.session, nb, room); }), QString("Notebook inativo não pode ser movimentado."));
        QCOMPARE(count(ctx, "events"), afterSetup); QCOMPARE(afterSetup, events + 3);
        QCOMPARE(notebook(ctx, nb).locationId, home);
        // Notebook em local excluído pode ir para um local ativo; from_location_id é o id excluído.
        const auto nb2 = addNotebook(ctx, "NB-2", room); ctx.locations.softDelete(ctx.session, room);
        ctx.store.updateLocation(ctx.session, nb2, home);
        QCOMPARE(scalar(ctx, "SELECT from_location_id FROM events WHERE action='Localização' ORDER BY id DESC LIMIT 1"), QString::number(room));
    }
    void maintenanceNoteRecorded() {
        QTemporaryDir temp; const auto path = temp.filePath("note.sqlite"); qint64 nb, other;
        {
            TestContext ctx(path); const auto home = createLocation(ctx, "Armário");
            nb = addNotebook(ctx, "NB-1", home); other = addNotebook(ctx, "NB-2", home);
            ctx.store.setMaintenance(ctx.session, nb, true, "  Tela piscando  ");
            ctx.store.setMaintenance(ctx.session, other, true, "   ");
            const auto changes = QJsonDocument::fromJson(scalar(ctx, QString("SELECT changes FROM events WHERE action='Manutenção' AND notebook_id=%1").arg(nb)).toUtf8()).object();
            QVERIFY(changes.value("maintenance_note").toObject().value("de").isNull());
            QCOMPARE(changes.value("maintenance_note").toObject().value("para").toString(), QString("Tela piscando"));
            QVERIFY(!scalar(ctx, QString("SELECT changes FROM events WHERE action='Manutenção' AND notebook_id=%1").arg(other)).contains("maintenance_note"));
        }
        TestContext ctx(path);
        const auto n = notebook(ctx, nb);
        QCOMPARE(n.maintenanceNote, QString("Tela piscando"));
        QCOMPARE(n.maintenanceSince, QDateTime::fromString(scalar(ctx, QString("SELECT happened_at FROM events WHERE action='Manutenção' AND notebook_id=%1").arg(nb)), Qt::ISODateWithMs));
        QVERIFY(n.maintenanceSince.isValid()); QVERIFY(notebook(ctx, other).maintenanceNote.isEmpty());
    }
    void maintenanceKeepsLocation() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("keeps.sqlite"));
        const auto home = createLocation(ctx, "Armário"); const auto nb = addNotebook(ctx, "NB-1", home);
        const QString sql = QString("SELECT location||'|'||location_id FROM notebooks WHERE id=%1").arg(nb);
        const auto before = scalar(ctx, sql);
        ctx.store.setMaintenance(ctx.session, nb, true, "Bateria"); QCOMPARE(scalar(ctx, sql), before);
        ctx.store.setMaintenance(ctx.session, nb, false); QCOMPARE(scalar(ctx, sql), before);
        QCOMPARE(scalar(ctx, "SELECT COUNT(*) FROM events WHERE action IN ('Manutenção','Disponibilização') AND location_id IS NULL AND from_location_id IS NULL"), QString("2"));
    }
    void maintenanceNoteLimits() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("limits.sqlite"));
        const auto home = createLocation(ctx, "Armário"); const auto nb = addNotebook(ctx, "NB-1", home);
        const auto events = count(ctx, "events");
        QCOMPARE(errorOf([&] { ctx.store.setMaintenance(ctx.session, nb, true, QString(201, 'x')); }), QString("Observação excede o limite de caracteres."));
        QCOMPARE(count(ctx, "events"), events);
        ctx.store.setMaintenance(ctx.session, nb, true, QString(200, 'x'));
        QCOMPARE(notebook(ctx, nb).maintenanceNote.size(), 200);
        QCOMPARE(errorOf([&] { ctx.store.setMaintenance(ctx.session, nb, false, "x"); }), QString("A observação só pode ser informada ao enviar à manutenção."));
        QCOMPARE(count(ctx, "events"), events + 1); QCOMPARE(notebook(ctx, nb).status, Status::Maintenance);
    }
    // ---------------- 03-03: devolução, sugestão de local e cadastro por id ----------------
    void checkinByDifferentPerson() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("checkin.sqlite"));
        const auto home = createLocation(ctx, "Armário 1"), room = createLocation(ctx, "Sala 12");
        const auto nb = addNotebook(ctx, "NB-1", home);
        const auto ana = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, "Matemática");
        const auto carlos = createPerson(ctx, "Carlos Lima", ResponsibleRole::Assistant);
        ctx.store.checkout(ctx.session, nb, ana, room);
        ctx.store.checkin(ctx.session, nb, carlos, home);
        const auto n = notebook(ctx, nb);
        QCOMPARE(n.status, Status::Available); QCOMPARE(n.locationId, home); QCOMPARE(n.location, QString("Armário 1"));
        QCOMPARE(n.returnedByPersonId, carlos); QCOMPARE(n.returnedBy, QString("Carlos Lima")); QCOMPARE(n.returnedByRole, ResponsibleRole::Assistant);
        QCOMPARE(rowText(ctx, "SELECT borrower_person_id,returned_by_person_id,returned_by,returned_by_role,returned_by_subject,return_location_id,return_location FROM loans"),
                 QString::number(ana) + "|" + QString::number(carlos) + "|Carlos Lima|assistant|<null>|" + QString::number(home) + "|Armário 1");
        const auto event = row(ctx, "SELECT loan_id,location_id,from_location_id,details FROM events WHERE action='Devolução'");
        QCOMPARE(event.value(0).toString(), scalar(ctx, "SELECT id FROM loans"));
        QCOMPARE(event.value(1).toLongLong(), home); QCOMPARE(event.value(2).toLongLong(), room);
        QCOMPARE(event.value(3).toString(), QString("Devolvido por: Carlos Lima (Auxiliar). Local: Armário 1"));
    }
    void checkinSharesTimestampWithEvent() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("checkin-instant.sqlite"));
        const auto home = createLocation(ctx, "Armário 1"), room = createLocation(ctx, "Sala 3");
        const auto nb = addNotebook(ctx, "NB-1", home); const auto bia = createPerson(ctx, "Bia", ResponsibleRole::Assistant);
        ctx.store.checkout(ctx.session, nb, bia, room); ctx.clock.advance(5); ctx.store.checkin(ctx.session, nb, bia, home);
        QCOMPARE(scalar(ctx, "SELECT l.returned_at=e.happened_at FROM loans l JOIN events e ON e.loan_id=l.id AND e.action='Devolução'"), QString("1"));
        const auto changes = QJsonDocument::fromJson(scalar(ctx, "SELECT changes FROM events WHERE action='Devolução'").toUtf8()).object();
        QCOMPARE(changes.value("status").toObject().value("para").toString(), QString("available"));
        QCOMPARE(changes.value("location_id").toObject().value("de").toInteger(), room);
        QCOMPARE(changes.value("location_id").toObject().value("para").toInteger(), home);
        QCOMPARE(changes.value("person_role").toObject().value("para").toString(), QString("assistant"));
    }
    void checkinRefusals() {
        QTemporaryDir temp; const auto path = temp.filePath("checkin-refusals.sqlite");
        TestContext ctx(path); TestContext other(path);
        const auto home = createLocation(ctx, "Armário"), room = createLocation(ctx, "Sala 5"), gone = createLocation(ctx, "Depósito");
        const auto nb = addNotebook(ctx, "NB-1", home), idle = addNotebook(ctx, "NB-2", home), retired = addNotebook(ctx, "NB-3", home);
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor), bruno = createPerson(ctx, "Bruno", ResponsibleRole::Assistant);
        ctx.store.checkout(ctx.session, nb, ana, room); ctx.store.inactivate(ctx.session, retired);
        other.people.inactivate(other.session, bruno); other.locations.softDelete(other.session, gone);
        const auto events = count(ctx, "events"); const auto loan = rowText(ctx, "SELECT * FROM loans");
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, nb, 0, home); }), QString("Selecione a pessoa responsável pela devolução."));
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, nb, ana, 0); }), QString("Selecione o local após a devolução."));
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, nb, bruno, home); }), QString("Esta pessoa está inativa e não pode ser selecionada."));
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, nb, ana, gone); }), QString("O local Depósito foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais."));
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, idle, ana, home); }), QString("Somente notebooks emprestados podem ser devolvidos. Atualize a seleção."));
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, retired, ana, home); }), QString("Notebook inativo não pode ser movimentado."));
        QCOMPARE(errorOf([&] { ctx.store.checkin(ctx.session, 999, ana, home); }), QString("Notebook não encontrado."));
        QVERIFY_EXCEPTION_THROWN(ctx.store.checkin(Session(), nb, ana, home), AuthorizationError);
        ctx.clock.advance(15 * 60);
        try { ctx.store.checkin(ctx.session, nb, ana, home); QFAIL("Sessão expirada deveria ser recusada."); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(), AuthorizationError::Reason::Expired); }
        QCOMPARE(count(ctx, "events"), events); QCOMPARE(rowText(ctx, "SELECT * FROM loans"), loan);
        QCOMPARE(notebook(ctx, nb).status, Status::Loaned); QCOMPARE(notebook(ctx, nb).locationId, room);
    }
    void suggestedReturnLocationRules() {
        QTemporaryDir temp; const auto path = temp.filePath("suggested.sqlite"); TestContext ctx(path);
        const auto home = createLocation(ctx, "Armário 1"), room = createLocation(ctx, "Sala 12");
        const auto a = addNotebook(ctx, "NB-1", home), b = addNotebook(ctx, "NB-2", home), c = addNotebook(ctx, "NB-3", home);
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        QVERIFY(!ctx.store.suggestedReturnLocation(a)); // disponível: sem sugestão
        ctx.store.checkout(ctx.session, a, ana, room);
        QCOMPARE(ctx.store.suggestedReturnLocation(a), std::optional<qint64>(home)); // origem do empréstimo aberto
        ctx.store.checkout(ctx.session, b, ana, room); ctx.locations.softDelete(ctx.session, home);
        QVERIFY(!ctx.store.suggestedReturnLocation(b)); // origem excluída: sem sugestão
        const auto other = createLocation(ctx, "Sala 3");
        ctx.store.checkout(ctx.session, c, ana, other);
        // Simula um empréstimo legado (pré-v6) sem origem: o trigger S3 é recriado logo depois.
        const auto guard = scalar(ctx, "SELECT sql FROM sqlite_master WHERE type='trigger' AND name='loans_history_immutable'");
        QVERIFY(LegacyFixtures::execAll(path, {"DROP TRIGGER loans_history_immutable",
                                               "UPDATE loans SET origin_location_id=NULL,origin_location=NULL WHERE notebook_id=" + QString::number(c), guard}));
        QVERIFY(!ctx.store.suggestedReturnLocation(c)); // empréstimo legado sem origem
        ctx.store.checkin(ctx.session, a, ana, room);
        QVERIFY(!ctx.store.suggestedReturnLocation(a)); // devolvido: sem sugestão
        QVERIFY(!ctx.store.suggestedReturnLocation(999));
    }
    void loanHistoryPreservedAcrossCycles() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("cycles.sqlite"));
        const auto home = createLocation(ctx, "Armário 1"), room = createLocation(ctx, "Sala 12"), lab = createLocation(ctx, "Laboratório");
        const auto nb = addNotebook(ctx, "NB-1", home);
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor), carlos = createPerson(ctx, "Carlos", ResponsibleRole::Assistant);
        ctx.store.checkout(ctx.session, nb, ana, room); ctx.store.checkin(ctx.session, nb, carlos, home);
        const QString first = "SELECT * FROM loans ORDER BY id LIMIT 1"; const auto closed = rowText(ctx, first);
        ctx.store.checkout(ctx.session, nb, carlos, lab);
        QCOMPARE(rowText(ctx, first), closed);
        ctx.store.checkin(ctx.session, nb, ana, room);
        QCOMPARE(rowText(ctx, first), closed); QCOMPARE(count(ctx, "loans"), 2);
        QCOMPARE(scalar(ctx, "SELECT COUNT(*) FROM loans WHERE returned_at IS NULL"), QString("0"));
        QCOMPARE(scalar(ctx, "SELECT COUNT(*) FROM events WHERE entity_type='notebook' AND action IN ('Retirada','Devolução')"), QString("4"));
        QCOMPARE(scalar(ctx, "SELECT COUNT(DISTINCT loan_id) FROM events WHERE action IN ('Retirada','Devolução')"), QString("2"));
        QVERIFY(!LegacyFixtures::execAll(temp.filePath("cycles.sqlite"), {"UPDATE loans SET returned_by='X' WHERE id=(SELECT MIN(id) FROM loans)"})); // S3: empréstimo encerrado é imutável
        QCOMPARE(rowText(ctx, first), closed);
    }
    void commonAccountFullCycle() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("common.sqlite"));
        const auto home = createLocation(ctx, "Armário 1"), room = createLocation(ctx, "Sala 12");
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        const auto operatorSession = common(ctx); const auto before = count(ctx, "events");
        const auto nb = ctx.store.add(operatorSession, "NB-9", "Dell 9", home);
        ctx.store.checkout(operatorSession, nb, ana, room);
        ctx.store.checkin(operatorSession, nb, ana, home);
        ctx.store.updateLocation(operatorSession, nb, room);
        ctx.store.setMaintenance(operatorSession, nb, true, "Tela piscando");
        QCOMPARE(count(ctx, "events"), before + 5);
        QCOMPARE(scalar(ctx, "SELECT group_concat(action,'|') FROM (SELECT action FROM events WHERE entity_type='notebook' ORDER BY id)"),
                 QString("Cadastro|Retirada|Devolução|Localização|Manutenção"));
        QCOMPARE(scalar(ctx, "SELECT COUNT(*) FROM events WHERE entity_type='notebook' AND actor_account_id=" + QString::number(operatorSession.accountId())), QString("5"));
        QCOMPARE(notebook(ctx, nb).status, Status::Maintenance);
    }
    void addAndUpdateByLocationId() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("add-update.sqlite"));
        const auto home = createLocation(ctx, "Armário 1"), room = createLocation(ctx, "Sala 12"), gone = createLocation(ctx, "Depósito");
        ctx.locations.softDelete(ctx.session, gone); const auto events = count(ctx, "events");
        QCOMPARE(errorOf([&] { ctx.store.add(ctx.session, "NB-0", "Dell", 0); }), QString("Selecione o local atual."));
        QCOMPARE(errorOf([&] { ctx.store.add(ctx.session, "NB-0", "Dell", gone); }), QString("O local Depósito foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais."));
        QCOMPARE(count(ctx, "events"), events); QCOMPARE(count(ctx, "notebooks"), 0);
        const auto nb = ctx.store.add(ctx.session, "NB-1", "Dell", home);
        QCOMPARE(notebook(ctx, nb).locationId, home);
        QCOMPARE(scalar(ctx, "SELECT location_id FROM events WHERE action='Cadastro'"), QString::number(home));
        auto current = notebook(ctx, nb);
        ctx.store.update(ctx.session, current, "NB-1", "Dell 5420", std::nullopt); // mantém o local
        QCOMPARE(notebook(ctx, nb).locationId, home);
        current = notebook(ctx, nb);
        ctx.store.update(ctx.session, current, "NB-1", "Dell 5420", room);
        QCOMPARE(notebook(ctx, nb).locationId, room); QCOMPARE(notebook(ctx, nb).location, QString("Sala 12"));
        const auto event = row(ctx, "SELECT location_id,from_location_id FROM events WHERE action='Notebook atualizado' ORDER BY id DESC LIMIT 1");
        QCOMPARE(event.value(0).toLongLong(), room); QCOMPARE(event.value(1).toLongLong(), home);
        current = notebook(ctx, nb);
        QCOMPARE(errorOf([&] { ctx.store.update(ctx.session, current, "NB-1", "Dell 5420", gone); }), QString("O local Depósito foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais."));
        ctx.locations.softDelete(ctx.session, room); current = notebook(ctx, nb);
        ctx.store.update(ctx.session, current, "NB-1", "Dell 5430", std::nullopt); // local excluído é mantido
        QCOMPARE(notebook(ctx, nb).locationId, room); QCOMPARE(notebook(ctx, nb).name, QString("Dell 5430"));
    }
};
QTEST_GUILESS_MAIN(MovementTests)
#include "MovementTests.moc"

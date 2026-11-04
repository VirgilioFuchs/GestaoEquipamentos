#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QSqlRecord>
#include "TestSupport.h"

class CatalogTests : public QObject {
    Q_OBJECT
    static int count(TestContext &ctx, const QString &table) {
        QSqlQuery query(ctx.database.connection());
        if (!query.exec("SELECT COUNT(*) FROM " + table) || !query.next()) return -1;
        return query.value(0).toInt();
    }
    static QString createError(TestContext &ctx, const Session &session, const QString &name) {
        try { ctx.locations.create(session,name); }
        catch (const std::exception &e) { return QString::fromUtf8(e.what()); }
        return {};
    }
    template<typename Action> static QString errorOf(Action action) {
        try { action(); } catch (const std::exception &e) { return QString::fromUtf8(e.what()); }
        return {};
    }
    static Location location(TestContext &ctx, qint64 id) {
        for (const auto &row : ctx.locations.locations(CatalogFilter::All)) if (row.id==id) return row;
        return {};
    }
    static Person person(TestContext &ctx, qint64 id) {
        for (const auto &row : ctx.people.people(CatalogFilter::All)) if (row.id==id) return row;
        return {};
    }
    static Notebook notebook(TestContext &ctx, qint64 id) {
        for (const auto &row : ctx.store.notebooks()) if (row.id==id) return row;
        return {};
    }
    static qint64 addNotebook(TestContext &ctx, const QString &tag, const QString &name, const QString &place) {
        return Fixture::add(ctx,ctx.session,tag,name,place);
    }
    static Session adminSession(TestContext &ctx) {
        ctx.accounts.createAdministrator(ctx.session,"administradora",ctx.auth.preparePassword("senha-admin-segura"),InitialPasswordMode::Assisted);
        return ctx.auth.login("administradora","senha-admin-segura");
    }
    static int loans(TestContext &ctx, qint64 id) {
        QSqlQuery query(ctx.database.connection()); query.prepare("SELECT COUNT(*) FROM loans WHERE notebook_id=?"); query.addBindValue(id);
        if (!query.exec() || !query.next()) return -1;
        return query.value(0).toInt();
    }
    static const QString &duplicate() { static const QString text("Este patrimônio já está cadastrado. Informe outro patrimônio."); return text; }
    static const QString &stale() { static const QString text("Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente."); return text; }
    static QVariantList row(TestContext &ctx, const QString &sql) {
        QSqlQuery query(ctx.database.connection()); QVariantList values;
        if (!query.exec(sql) || !query.next()) return values;
        for (int i=0;i<query.record().count();++i) values.append(query.value(i));
        return values;
    }
    static HistoryEntry eventNamed(const QVector<HistoryEntry> &history, const QString &action) {
        for (const auto &entry : history) if (entry.action==action) return entry;
        return {};
    }
private slots:
    void checkoutPersonSnapshot() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("by-person.sqlite"));
        const auto room=ctx.locations.create(ctx.session,"Sala 12"); ctx.locations.create(ctx.session,"Biblioteca");
        const auto ana=ctx.people.create(ctx.session,{"Ana Souza",ResponsibleRole::Professor,"Matemática"});
        const auto id=addNotebook(ctx,"NB-001","Dell","Biblioteca");
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        ctx.store.checkout(common,id,ana,Fixture::location(ctx," sala 12 "));
        const auto loan=row(ctx,"SELECT borrower,borrower_role,borrower_person_id,borrower_subject,checkout_location,checkout_location_id FROM loans");
        QCOMPARE(loan.value(0).toString(),QString("Ana Souza")); QCOMPARE(loan.value(1).toString(),QString("professor"));
        QCOMPARE(loan.value(2).toLongLong(),ana); QCOMPARE(loan.value(3).toString(),QString("Matemática"));
        QCOMPARE(loan.value(4).toString(),QString("Sala 12")); QCOMPARE(loan.value(5).toLongLong(),room);
        const auto event=ctx.store.history().first();
        QCOMPARE(event.action,QString("Retirada")); QCOMPARE(event.actorUsername,QString("operador"));
        QCOMPARE(event.details,QString("Retirado por: Ana Souza (Professor · Matemática). Local: Sala 12"));
        QCOMPARE(event.changes.value("person_role").toObject().value("para").toString(),QString("professor"));
        QCOMPARE(event.changes.value("subject").toObject().value("para").toString(),QString("Matemática"));
        QCOMPARE(event.changes.value("status").toObject().value("para").toString(),QString("loaned"));
        QCOMPARE(event.changes.value("location_id").toObject().value("para").toInteger(),room);
        const auto current=notebook(ctx,id);
        QCOMPARE(current.status,Status::Loaned); QCOMPARE(current.borrower,QString("Ana Souza")); QCOMPARE(current.borrowerSubject,QString("Matemática"));
        QCOMPARE(current.borrowerRole,ResponsibleRole::Professor); QCOMPARE(current.locationId,room);
        ctx.people.update(ctx.session,person(ctx,ana),{"Ana Souza",ResponsibleRole::Assistant,{}});
        QCOMPARE(row(ctx,"SELECT borrower_subject,borrower_role FROM loans").value(0).toString(),QString("Matemática"));
        QCOMPARE(eventNamed(ctx.store.history(),"Retirada").details,event.details);
        QCOMPARE(notebook(ctx,id).borrowerSubject,QString("Matemática"));
    }
    void checkinPersonSnapshot() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("checkin-person.sqlite"));
        ctx.locations.create(ctx.session,"Sala 12"); const auto library=ctx.locations.create(ctx.session,"Biblioteca");
        const auto beatriz=ctx.people.create(ctx.session,{"Beatriz Rocha",ResponsibleRole::Professor,{}});
        const auto id=addNotebook(ctx,"NB-001","Dell","Sala 12");
        Fixture::checkout(ctx,ctx.session,id,"Ana",ResponsibleRole::Professor,"Sala 12");
        ctx.store.checkin(ctx.session,id,beatriz,Fixture::location(ctx,"Biblioteca"));
        const auto loan=row(ctx,"SELECT returned_by,returned_by_role,returned_by_person_id,returned_by_subject,return_location,return_location_id,borrower_person_id FROM loans");
        QCOMPARE(loan.value(0).toString(),QString("Beatriz Rocha")); QCOMPARE(loan.value(1).toString(),QString("professor"));
        QCOMPARE(loan.value(2).toLongLong(),beatriz); QVERIFY(loan.value(3).isNull());
        QCOMPARE(loan.value(4).toString(),QString("Biblioteca")); QCOMPARE(loan.value(5).toLongLong(),library); QVERIFY(loan.value(6).toLongLong()!=beatriz); // retirada por outra pessoa (fixture)
        const auto event=ctx.store.history().first();
        QCOMPARE(event.action,QString("Devolução")); QVERIFY(!event.details.contains(" · "));
        QCOMPARE(event.details,QString("Devolvido por: Beatriz Rocha (Professor). Local: Biblioteca"));
        QVERIFY(!event.changes.contains("subject")); QCOMPARE(event.changes.value("person_role").toObject().value("para").toString(),QString("professor"));
        const auto current=notebook(ctx,id); QCOMPARE(current.status,Status::Available); QCOMPARE(current.returnedBy,QString("Beatriz Rocha")); QVERIFY(current.returnedBySubject.isEmpty());
        QCOMPARE(errorOf([&]{ctx.store.checkin(ctx.session,id,beatriz,Fixture::location(ctx,"Biblioteca"));}),QString("Somente notebooks emprestados podem ser devolvidos. Atualize a seleção."));
    }
    void inactivePersonRefused() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("inactive-person.sqlite"));
        ctx.locations.create(ctx.session,"Sala 12");
        const auto carlos=ctx.people.create(ctx.session,{"Carlos Lima",ResponsibleRole::Assistant,{}});
        const auto ana=ctx.people.create(ctx.session,{"Ana Souza",ResponsibleRole::Professor,"Matemática"});
        const auto free=addNotebook(ctx,"NB-001","Dell","Sala 12"), lent=addNotebook(ctx,"NB-002","HP","Sala 12"), frozen=addNotebook(ctx,"NB-003","Lenovo","Sala 12");
        Fixture::checkout(ctx,ctx.session,lent,"Bia",ResponsibleRole::Assistant,"Sala 12");
        const auto admin=adminSession(ctx); ctx.people.inactivate(admin,carlos); ctx.store.inactivate(admin,frozen);
        const int events=count(ctx,"events"), loanRows=count(ctx,"loans");
        const auto snapshot=ctx.store.notebooks();
        const QString inactive("Esta pessoa está inativa e não pode ser selecionada.");
        QCOMPARE(errorOf([&]{ctx.store.checkout(ctx.session,free,carlos,Fixture::location(ctx,"Sala 12"));}),inactive);
        QCOMPARE(errorOf([&]{ctx.store.checkin(ctx.session,lent,carlos,Fixture::location(ctx,"Sala 12"));}),inactive);
        QCOMPARE(errorOf([&]{ctx.store.checkout(ctx.session,free,9999,Fixture::location(ctx,"Sala 12"));}),QString("Pessoa não encontrada."));
        QCOMPARE(errorOf([&]{ctx.store.checkin(ctx.session,lent,9999,Fixture::location(ctx,"Sala 12"));}),QString("Pessoa não encontrada."));
        QCOMPARE(errorOf([&]{ctx.store.checkout(ctx.session,frozen,ana,Fixture::location(ctx,"Sala 12"));}),QString("Notebook inativo não pode ser movimentado."));
        QVERIFY_EXCEPTION_THROWN(ctx.store.checkout(Session(),free,ana,Fixture::location(ctx,"Sala 12")),AuthorizationError);
        QVERIFY_EXCEPTION_THROWN(ctx.store.checkin(Session(),lent,ana,Fixture::location(ctx,"Sala 12")),AuthorizationError);
        ctx.clock.advance(15*60);
        try { ctx.store.checkout(ctx.session,free,ana,Fixture::location(ctx,"Sala 12")); QFAIL("Sessão expirada aceita"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Expired); }
        try { ctx.store.checkin(ctx.session,lent,ana,Fixture::location(ctx,"Sala 12")); QFAIL("Sessão expirada aceita"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Expired); }
        QCOMPARE(count(ctx,"events"),events); QCOMPARE(count(ctx,"loans"),loanRows);
        const auto after=ctx.store.notebooks(); QCOMPARE(after.size(),snapshot.size());
        for (int i=0;i<after.size();++i) { QCOMPARE(after[i].status,snapshot[i].status); QCOMPARE(after[i].location,snapshot[i].location); QCOMPARE(after[i].borrower,snapshot[i].borrower); }
    }
    void loanAndEventShareTimestamp() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("timestamp.sqlite"));
        ctx.locations.create(ctx.session,"Sala 12");
        const auto ana=ctx.people.create(ctx.session,{"Ana Souza",ResponsibleRole::Professor,"Matemática"});
        const auto id=addNotebook(ctx,"NB-001","Dell","Sala 12");
        ctx.store.checkout(ctx.session,id,ana,Fixture::location(ctx,"Sala 12")); ctx.store.checkin(ctx.session,id,ana,Fixture::location(ctx,"Sala 12"));
        const auto loan=row(ctx,"SELECT checked_out_at,returned_at FROM loans");
        QCOMPARE(row(ctx,"SELECT happened_at FROM events WHERE action='Retirada'").value(0).toString(),loan.value(0).toString());
        QCOMPARE(row(ctx,"SELECT happened_at FROM events WHERE action='Devolução'").value(0).toString(),loan.value(1).toString());
        QVERIFY(loan.value(0).toString().endsWith('Z')); QVERIFY(loan.value(1).toString().endsWith('Z'));
    }
    void historyKeepsFactsAfterEditsAndReopen() {
        QTemporaryDir temp; const auto path=temp.filePath("facts.sqlite"); qint64 id=0;
        QVector<HistoryEntry> before;
        {
            TestContext ctx(path);
            const auto room=ctx.locations.create(ctx.session,"Sala 12"), library=ctx.locations.create(ctx.session,"Biblioteca");
            const auto ana=ctx.people.create(ctx.session,{"Ana Souza",ResponsibleRole::Professor,"Matemática"});
            id=addNotebook(ctx,"NB-001","Dell","Sala 12");
            ctx.store.checkout(ctx.session,id,ana,Fixture::location(ctx,"Sala 12")); ctx.store.checkin(ctx.session,id,ana,Fixture::location(ctx,"Biblioteca"));
            before=ctx.store.history();
            ctx.people.update(ctx.session,person(ctx,ana),{"Ana Souza",ResponsibleRole::Assistant,{}});
            ctx.locations.rename(ctx.session,location(ctx,room),"Sala 12B");
            ctx.locations.softDelete(ctx.session,library);
            Fixture::update(ctx,ctx.session,notebook(ctx,id),"NB-101","Dell 5420",notebook(ctx,id).location);
        }
        TestContext reopened(path);
        const auto loan=row(reopened,"SELECT borrower,borrower_role,borrower_subject,checkout_location,returned_by,returned_by_role,returned_by_subject,return_location FROM loans");
        QCOMPARE(loan.value(0).toString(),QString("Ana Souza")); QCOMPARE(loan.value(1).toString(),QString("professor")); QCOMPARE(loan.value(2).toString(),QString("Matemática"));
        QCOMPARE(loan.value(3).toString(),QString("Sala 12")); QCOMPARE(loan.value(4).toString(),QString("Ana Souza")); QCOMPARE(loan.value(5).toString(),QString("professor"));
        QCOMPARE(loan.value(6).toString(),QString("Matemática")); QCOMPARE(loan.value(7).toString(),QString("Biblioteca"));
        const auto after=reopened.store.history();
        for (const auto &old : before) for (const auto &current : after) if (old.id==current.id) {
            QCOMPARE(current.details,old.details); QCOMPARE(current.changes,old.changes); QCOMPARE(current.assetTag,old.assetTag); QCOMPARE(current.notebookName,old.notebookName); QCOMPARE(current.happenedAt,old.happenedAt);
        }
        const auto checkout=eventNamed(after,"Retirada"), checkin=eventNamed(after,"Devolução");
        QCOMPARE(checkout.details,QString("Retirado por: Ana Souza (Professor · Matemática). Local: Sala 12"));
        QCOMPARE(checkin.details,QString("Devolvido por: Ana Souza (Professor · Matemática). Local: Biblioteca"));
        for (const auto &entry : {checkout,checkin}) { QCOMPARE(entry.assetTag,QString("NB-001")); QCOMPARE(entry.notebookName,QString("Dell")); }
        auto personChanges=LocalDatabase::change("person_role","professor","assistant");
        personChanges.insert("subject",LocalDatabase::change("subject","Matemática",QJsonValue::Null).value("subject"));
        QCOMPARE(eventNamed(after,"Pessoa atualizada").changes,personChanges);
        QCOMPARE(eventNamed(after,"Local atualizado").changes.value("name").toObject().value("para").toString(),QString("Sala 12B"));
        QCOMPARE(eventNamed(after,"Local excluído").changes,LocalDatabase::change("location_deleted",0,1));
        auto notebookChanges=LocalDatabase::change("asset_tag","NB-001","NB-101"); notebookChanges.insert("name",LocalDatabase::change("name","Dell","Dell 5420").value("name"));
        const auto edit=eventNamed(after,"Notebook atualizado"); QCOMPARE(edit.changes,notebookChanges); QCOMPARE(edit.assetTag,QString("NB-101"));
        const auto current=notebook(reopened,id); QCOMPARE(current.assetTag,QString("NB-101")); QCOMPARE(current.returnedBySubject,QString("Matemática")); QVERIFY(current.locationDeleted);
    }
    void legacyLoansNeverLinkedByName() {
        QTemporaryDir temp; const auto path=temp.filePath("legacy-link.sqlite"); QVERIFY(LegacyFixtures::createV2(path));
        TestContext ctx(path);
        const auto legacyDetails=row(ctx,"SELECT details FROM events WHERE action='Retirada' ORDER BY id LIMIT 1").value(0).toString();
        QCOMPARE(legacyDetails,QString("Retirado por: Ana (Professor). Local: Sala 12"));
        const auto ana=ctx.people.create(ctx.session,{"Ana",ResponsibleRole::Professor,"Matemática"});
        ctx.people.create(ctx.session,{"Bia",ResponsibleRole::Assistant,{}});
        const auto id=addNotebook(ctx,"NB-900","Novo","Sala 12");
        ctx.store.checkout(ctx.session,id,ana,Fixture::location(ctx,"Sala 12"));
        QCOMPARE(row(ctx,"SELECT COUNT(*) FROM loans WHERE notebook_id=1 AND (borrower_person_id IS NOT NULL OR borrower_subject IS NOT NULL OR returned_by_person_id IS NOT NULL OR returned_by_subject IS NOT NULL)").value(0).toInt(),0);
        QCOMPARE(row(ctx,"SELECT COUNT(*) FROM loans WHERE notebook_id=1").value(0).toInt(),2);
        QCOMPARE(row(ctx,"SELECT details FROM events WHERE action='Retirada' ORDER BY id LIMIT 1").value(0).toString(),legacyDetails);
        QCOMPARE(row(ctx,"SELECT borrower_person_id FROM loans WHERE notebook_id="+QString::number(id)).value(0).toLongLong(),ana);
        TestContext reopened(path);
        QCOMPARE(row(reopened,"SELECT COUNT(*) FROM loans WHERE notebook_id=1 AND borrower_person_id IS NULL AND borrower_subject IS NULL").value(0).toInt(),2);
    }
    void editNotebookKeepsHistory() {
        QTemporaryDir temp; const auto path=temp.filePath("edit-history.sqlite"); TestContext ctx(path);
        ctx.locations.create(ctx.session,"Sala 12");
        const auto id=addNotebook(ctx,"NB-001","Dell","Sala 12");
        Fixture::checkout(ctx,ctx.session,id,"Ana",ResponsibleRole::Professor,"Sala 12");
        Fixture::checkin(ctx,ctx.session,id,"Carlos",ResponsibleRole::Assistant,"Sala 12");
        const auto before=notebook(ctx,id); const int loanCount=loans(ctx,id);
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        Fixture::update(ctx,common,before,"NB-101","Dell 5420","Sala 12");
        const auto after=notebook(ctx,id);
        QCOMPARE(after.id,before.id); QCOMPARE(after.status,before.status); QCOMPARE(after.checkedOutAt,before.checkedOutAt); QCOMPARE(after.returnedAt,before.returnedAt);
        QCOMPARE(after.borrower,before.borrower); QCOMPARE(after.returnedBy,before.returnedBy); QCOMPARE(after.locationId,before.locationId); QVERIFY(after.active);
        QCOMPARE(after.assetTag,QString("NB-101")); QCOMPARE(after.name,QString("Dell 5420")); QCOMPARE(loans(ctx,id),loanCount);
        const auto history=ctx.store.history();
        const auto event=history.first(); QCOMPARE(event.action,QString("Notebook atualizado")); QCOMPARE(event.actorUsername,QString("operador"));
        QCOMPARE(event.assetTag,QString("NB-101")); QCOMPARE(event.notebookName,QString("Dell 5420"));
        QCOMPARE(event.details,QString("Cadastro do notebook atualizado: NB-101 · Dell 5420."));
        auto changes=LocalDatabase::change("asset_tag","NB-001","NB-101"); changes.insert("name",LocalDatabase::change("name","Dell","Dell 5420").value("name"));
        QCOMPARE(event.changes,changes);
        int moves=0;
        for (const auto &entry : history) if (entry.action=="Retirada" || entry.action=="Devolução" || entry.action=="Cadastro") {
            ++moves; QCOMPARE(entry.assetTag,QString("NB-001")); QCOMPARE(entry.notebookName,QString("Dell"));
        }
        QCOMPARE(moves,3);
    }
    void notebookAssetUniqueIncludingInactive() {
        QTemporaryDir temp; const auto path=temp.filePath("asset-unique.sqlite"); TestContext ctx(path);
        const auto first=addNotebook(ctx,"NB-001","Dell","Sala 12"); const auto second=addNotebook(ctx,"NB-002","HP","Sala 12");
        const int events=count(ctx,"events");
        QCOMPARE(errorOf([&]{Fixture::add(ctx,ctx.session,"nb-001","Outro","Sala 12");}),duplicate());
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,notebook(ctx,second),"nb-001","HP","Sala 12");}),duplicate());
        ctx.store.inactivate(ctx.session,first); const int afterInactive=count(ctx,"events"); QCOMPARE(afterInactive,events+1);
        QCOMPARE(errorOf([&]{Fixture::add(ctx,ctx.session,"NB-001","Outro","Sala 12");}),duplicate());
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,notebook(ctx,second)," Nb-001 ","HP","Sala 12");}),duplicate());
        { TestContext other(path); QCOMPARE(errorOf([&]{Fixture::add(other,other.session,"nB-001","Outro","Sala 12");}),duplicate());
          QCOMPARE(errorOf([&]{Fixture::update(other,other.session,notebook(other,second),"NB-001","HP","Sala 12");}),duplicate()); }
        QCOMPARE(count(ctx,"events"),afterInactive); QCOMPARE(count(ctx,"notebooks"),2);
        Fixture::update(ctx,ctx.session,notebook(ctx,second),"nb-002","HP","Sala 12");
        QCOMPARE(notebook(ctx,second).assetTag,QString("nb-002"));
    }
    void editKeepsDeletedLocation() {
        QTemporaryDir temp; const auto path=temp.filePath("edit-deleted.sqlite"); TestContext ctx(path);
        const auto room=ctx.locations.create(ctx.session,"Sala 12"); const auto other=ctx.locations.create(ctx.session,"Sala 13"); ctx.locations.create(ctx.session,"Biblioteca");
        const auto id=addNotebook(ctx,"NB-001","Dell","Sala 12");
        ctx.locations.softDelete(ctx.session,room); ctx.locations.softDelete(ctx.session,other);
        Fixture::update(ctx,ctx.session,notebook(ctx,id),"NB-001","Dell 5420","Sala 12");
        QCOMPARE(notebook(ctx,id).locationId,room); QVERIFY(notebook(ctx,id).locationDeleted);
        QCOMPARE(ctx.store.history().first().changes,LocalDatabase::change("name","Dell","Dell 5420"));
        const int events=count(ctx,"events");
        QVERIFY(errorOf([&]{Fixture::update(ctx,ctx.session,notebook(ctx,id),"NB-001","Dell 5420","Sala 13");}).contains("foi excluído"));
        QCOMPARE(count(ctx,"events"),events); QCOMPARE(notebook(ctx,id).locationId,room);
        Fixture::update(ctx,ctx.session,notebook(ctx,id),"NB-001","Dell 5420","Biblioteca");
        QCOMPARE(notebook(ctx,id).location,QString("Biblioteca")); QVERIFY(!notebook(ctx,id).locationDeleted);
        const auto changes=ctx.store.history().first().changes;
        QCOMPARE(changes.value("location").toObject(),LocalDatabase::change("location","Sala 12","Biblioteca").value("location").toObject());
        QVERIFY(!changes.contains("asset_tag")); QVERIFY(!changes.contains("name"));
        // Notebook legado sem local informado pode ser editado sem informar local.
        QSqlQuery query(ctx.database.connection());
        QVERIFY(query.exec("INSERT INTO notebooks(asset_tag,name,location,location_id) VALUES('NB-LEG','Legado',NULL,NULL)"));
        qint64 legacy=0; for (const auto &row : ctx.store.notebooks()) if (row.assetTag=="NB-LEG") legacy=row.id;
        QVERIFY(notebook(ctx,legacy).location.isEmpty());
        Fixture::update(ctx,ctx.session,notebook(ctx,legacy),"NB-LEG","Legado corrigido","");
        QCOMPARE(notebook(ctx,legacy).name,QString("Legado corrigido")); QVERIFY(notebook(ctx,legacy).location.isEmpty());
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,notebook(ctx,id),"NB-001","Dell 5420","  ");}),QString("Selecione o local atual."));
    }
    void editNotebookGuards() {
        QTemporaryDir temp; const auto path=temp.filePath("edit-guards.sqlite"); TestContext ctx(path);
        const auto id=addNotebook(ctx,"NB-001","Dell","Sala 12"); const auto expected=notebook(ctx,id); const int events=count(ctx,"events");
        QVERIFY_EXCEPTION_THROWN(Fixture::update(ctx,Session(),expected,"NB-009","Dell","Sala 12"),AuthorizationError);
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,expected,"NB-001","Dell"," sala 12 ");}),QString("Nenhuma alteração para salvar."));
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,expected,"","Dell","Sala 12");}),QString("Patrimônio é obrigatório."));
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,expected,QString(61,'x'),"Dell","Sala 12");}),QString("Patrimônio excede o limite de caracteres."));
        QCOMPARE(count(ctx,"events"),events);
        { TestContext second(path); Fixture::update(second,second.session,notebook(second,id),"NB-001","Dell renomeado","Sala 12"); }
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,expected,"NB-002","Dell","Sala 12");}),stale());
        QCOMPARE(notebook(ctx,id).assetTag,QString("NB-001"));
        ctx.store.inactivate(ctx.session,id);
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,notebook(ctx,id),"NB-003","Outro","Sala 12");}),QString("Cadastros inativos não podem ser editados."));
        Notebook missing{}; missing.id=999;
        QCOMPARE(errorOf([&]{Fixture::update(ctx,ctx.session,missing,"NB-004","X","Sala 12");}),QString("Notebook não encontrado."));
    }
    void inactivateNotebookRules() {
        QTemporaryDir temp; const auto path=temp.filePath("inactivate-notebook.sqlite"); TestContext ctx(path);
        const auto free=addNotebook(ctx,"NB-001","Dell","Sala 12"); const auto loaned=addNotebook(ctx,"NB-002","HP","Sala 12"); const auto repair=addNotebook(ctx,"NB-003","Lenovo","Sala 12");
        const auto spare=addNotebook(ctx,"NB-004","Acer","Sala 12");
        Fixture::checkout(ctx,ctx.session,loaned,"Ana",ResponsibleRole::Professor,"Sala 12"); ctx.store.setMaintenance(ctx.session,repair,true);
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        const auto admin=adminSession(ctx); const int events=count(ctx,"events");
        QCOMPARE(errorOf([&]{ctx.store.inactivate(admin,loaned);}),QString("Somente notebooks disponíveis podem ser inativados."));
        QCOMPARE(errorOf([&]{ctx.store.inactivate(admin,repair);}),QString("Somente notebooks disponíveis podem ser inativados."));
        try { ctx.store.inactivate(common,free); QFAIL("Conta comum inativou notebook"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Forbidden); QCOMPARE(QString::fromUtf8(e.what()),QString("Somente administradores podem inativar pessoas e notebooks.")); }
        QCOMPARE(count(ctx,"events"),events); QVERIFY(notebook(ctx,free).active);
        ctx.store.inactivate(admin,free);
        const auto event=ctx.store.history().first(); QCOMPARE(event.action,QString("Notebook inativado")); QCOMPARE(event.actorUsername,QString("administradora"));
        QCOMPARE(event.changes,LocalDatabase::change("notebook_active",1,0)); QCOMPARE(event.details,QString("Notebook inativado: NB-001 · Dell."));
        QVERIFY(!notebook(ctx,free).active); QCOMPARE(notebook(ctx,free).status,Status::Available);
        const int afterInactive=count(ctx,"events");
        QCOMPARE(errorOf([&]{ctx.store.inactivate(ctx.session,free);}),QString("Este notebook já está inativo."));
        QCOMPARE(errorOf([&]{Fixture::checkout(ctx,ctx.session,free,"Ana",ResponsibleRole::Professor,"Sala 12");}),QString("Notebook inativo não pode ser movimentado."));
        QCOMPARE(errorOf([&]{ctx.store.setMaintenance(ctx.session,free,true);}),QString("Notebook inativo não pode ser movimentado."));
        QCOMPARE(errorOf([&]{Fixture::updateLocation(ctx,ctx.session,free,"Biblioteca");}),QString("Notebook inativo não pode ser movimentado."));
        QCOMPARE(errorOf([&]{ctx.store.inactivate(ctx.session,999);}),QString("Notebook não encontrado."));
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","notebook-frozen"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(!query.exec(QString("UPDATE notebooks SET status='maintenance' WHERE id=%1").arg(free)));
            QVERIFY(query.lastError().text().contains("Notebook inativo não pode ser movimentado."));
        }
        QSqlDatabase::removeDatabase("notebook-frozen");
        QCOMPARE(count(ctx,"events"),afterInactive);
        ctx.clock.advance(15*60);
        try { ctx.store.inactivate(ctx.session,spare); QFAIL("Sessão expirada inativou notebook"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Expired); }
        QVERIFY(notebook(ctx,spare).active); QCOMPARE(count(ctx,"events"),afterInactive);
        TestContext reopened(path); QVERIFY(!notebook(reopened,free).active); QVERIFY(notebook(reopened,loaned).active);
    }
    void inactivateRacesWithCheckout() {
        QTemporaryDir temp; const auto path=temp.filePath("inactivate-race.sqlite"); TestContext a(path);
        const auto id=addNotebook(a,"NB-001","Dell","Sala 12"); QCOMPARE(notebook(a,id).status,Status::Available);
        { TestContext b(path); Fixture::checkout(b,b.session,id,"Ana",ResponsibleRole::Professor,"Sala 12"); }
        const int events=count(a,"events");
        QCOMPARE(errorOf([&]{a.store.inactivate(a.session,id);}),QString("Somente notebooks disponíveis podem ser inativados."));
        QCOMPARE(count(a,"events"),events); QVERIFY(notebook(a,id).active); QCOMPARE(notebook(a,id).status,Status::Loaned);
        QCOMPARE(notebook(a,id).borrower,QString("Ana")); QVERIFY(!notebook(a,id).returnedAt.isValid()); QCOMPARE(loans(a,id),1);
    }
    void reactivateNotebookRules() {
        QTemporaryDir temp; const auto path=temp.filePath("reactivate-notebook.sqlite"); TestContext ctx(path);
        const auto id=addNotebook(ctx,"NB-001","Dell","Sala 12"); const auto other=addNotebook(ctx,"NB-002","HP","Sala 12");
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        const auto admin=adminSession(ctx);
        QCOMPARE(errorOf([&]{ctx.store.reactivate(admin,id);}),QString("Este notebook já está ativo."));
        QCOMPARE(errorOf([&]{ctx.store.reactivate(admin,999);}),QString("Notebook não encontrado."));
        ctx.store.inactivate(admin,id); ctx.store.inactivate(admin,other); const int events=count(ctx,"events");
        try { ctx.store.reactivate(common,id); QFAIL("Conta comum reativou notebook"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Forbidden); }
        QCOMPARE(count(ctx,"events"),events); QVERIFY(!notebook(ctx,id).active);
        ctx.store.reactivate(admin,id);
        QCOMPARE(count(ctx,"events"),events+1);
        const auto event=ctx.store.history().first(); QCOMPARE(event.action,QString("Notebook reativado")); QCOMPARE(event.actorUsername,QString("administradora"));
        QCOMPARE(event.changes,LocalDatabase::change("notebook_active",0,1)); QCOMPARE(event.details,QString("Notebook reativado: NB-001 · Dell."));
        QVERIFY(notebook(ctx,id).active); QCOMPARE(notebook(ctx,id).status,Status::Available);
        // Reativado volta a operar normalmente e pode ser inativado de novo.
        Fixture::checkout(ctx,ctx.session,id,"Ana",ResponsibleRole::Professor,"Sala 12"); QCOMPARE(notebook(ctx,id).status,Status::Loaned);
        Fixture::checkin(ctx,ctx.session,id,"Ana",ResponsibleRole::Professor,"Sala 12");
        ctx.store.inactivate(admin,id); QVERIFY(!notebook(ctx,id).active);
        const int afterCycle=count(ctx,"events");
        ctx.clock.advance(15*60);
        try { ctx.store.reactivate(ctx.session,other); QFAIL("Sessão expirada reativou notebook"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Expired); }
        QVERIFY(!notebook(ctx,other).active); QCOMPARE(count(ctx,"events"),afterCycle);
        TestContext reopened(path); QVERIFY(!notebook(reopened,id).active); QVERIFY(!notebook(reopened,other).active);
        reopened.store.reactivate(reopened.session,other); QVERIFY(notebook(reopened,other).active);
    }
    void reactivateUpgradesLegacyFrozenTrigger() {
        QTemporaryDir temp; const auto path=temp.filePath("reactivate-legacy-trigger.sqlite"); qint64 id=0;
        { TestContext ctx(path); id=addNotebook(ctx,"NB-001","Dell","Sala 12"); ctx.store.inactivate(ctx.session,id); }
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","legacy-trigger"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec("DROP TRIGGER notebooks_inactive_frozen"));
            QVERIFY(query.exec("CREATE TRIGGER notebooks_inactive_frozen BEFORE UPDATE ON notebooks WHEN OLD.active=0 BEGIN SELECT RAISE(ABORT,'Notebook inativo não pode ser movimentado.'); END"));
        }
        QSqlDatabase::removeDatabase("legacy-trigger");
        TestContext reopened(path); const int events=count(reopened,"events");
        reopened.store.reactivate(reopened.session,id); QVERIFY(notebook(reopened,id).active); QCOMPARE(count(reopened,"events"),events+1);
        reopened.store.inactivate(reopened.session,id);
        QCOMPARE(errorOf([&]{Fixture::updateLocation(reopened,reopened.session,id,"Biblioteca");}),QString("Notebook inativo não pode ser movimentado."));
        QCOMPARE(row(reopened,"PRAGMA user_version").value(0).toInt(),LocalDatabase::CurrentSchemaVersion);
    }
    void notebookRollbackOnEventFailure() {
        QTemporaryDir temp; const auto path=temp.filePath("notebook-rollback.sqlite"); TestContext ctx(path);
        const auto id=addNotebook(ctx,"NB-001","Dell","Sala 12"); const auto expected=notebook(ctx,id); const int events=count(ctx,"events");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","notebook-event-failure"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec("CREATE TRIGGER fail_notebook_event BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'falha simulada'); END"));
        }
        QSqlDatabase::removeDatabase("notebook-event-failure");
        QVERIFY_EXCEPTION_THROWN(Fixture::update(ctx,ctx.session,expected,"NB-101","Dell 5420","Biblioteca"),std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(ctx.store.inactivate(ctx.session,id),std::runtime_error);
        QCOMPARE(count(ctx,"events"),events);
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","notebook-reactivate-setup"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec(QString("UPDATE notebooks SET active=0 WHERE id=%1").arg(id)));
        }
        QSqlDatabase::removeDatabase("notebook-reactivate-setup");
        QVERIFY_EXCEPTION_THROWN(ctx.store.reactivate(ctx.session,id),std::runtime_error);
        QCOMPARE(count(ctx,"events"),events); QVERIFY(!notebook(ctx,id).active);
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","notebook-reactivate-restore"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec(QString("UPDATE notebooks SET active=1 WHERE id=%1").arg(id)));
        }
        QSqlDatabase::removeDatabase("notebook-reactivate-restore");
        TestContext reopened(path); const auto row=notebook(reopened,id);
        QCOMPARE(row.assetTag,QString("NB-001")); QCOMPARE(row.name,QString("Dell")); QCOMPARE(row.location,QString("Sala 12")); QVERIFY(row.active);
    }
    void peopleAreNotAccounts() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("separate.sqlite")); const int accounts=count(ctx,"accounts");
        ctx.people.create(ctx.session,{"principal",ResponsibleRole::Professor,{}});
        ctx.people.create(ctx.session,{"operador.x",ResponsibleRole::Assistant,{}});
        QCOMPARE(count(ctx,"accounts"),accounts);
        QVERIFY_EXCEPTION_THROWN(ctx.auth.login("operador.x","qualquer-senha"),AuthError);
        QCOMPARE(ctx.auth.login(TestPrincipal,TestPassword).accountId(),ctx.session.accountId());
    }
    void homonymsKeepDistinctIds() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("homonyms.sqlite"));
        const auto first=ctx.people.create(ctx.session,{"Ana Souza",ResponsibleRole::Professor,{}});
        const auto second=ctx.people.create(ctx.session,{"Ana Souza",ResponsibleRole::Assistant,{}});
        QVERIFY(first!=second); const auto rows=ctx.people.activeHomonyms(" ana souza "); QCOMPARE(rows.size(),2);
        QCOMPARE(rows[0].id,first); QCOMPARE(rows[1].id,second); ctx.people.inactivate(ctx.session,first);
        QCOMPARE(ctx.people.activeHomonyms("ANA SOUZA").first().id,second); QCOMPARE(ctx.people.activeHomonyms("Ana").size(),0);
    }
    void professorSubjectOptional() {
        QTemporaryDir temp; const auto path=temp.filePath("subjects.sqlite"); TestContext ctx(path);
        const auto empty=ctx.people.create(ctx.session,{"Ana",ResponsibleRole::Professor,"  "});
        const auto longId=ctx.people.create(ctx.session,{"Bia",ResponsibleRole::Professor,QString(120,'x')});
        const auto assistant=ctx.people.create(ctx.session,{"Carlos",ResponsibleRole::Assistant,"Física"});
        QCOMPARE(person(ctx,longId).subject.size(),120); QVERIFY(person(ctx,empty).subject.isEmpty()); QVERIFY(person(ctx,assistant).subject.isEmpty());
        const int events=count(ctx,"events");
        QCOMPARE(errorOf([&]{ctx.people.create(ctx.session,{"Longa",ResponsibleRole::Professor,QString(121,'x')});}),QString("Matéria excede o limite de caracteres."));
        QCOMPARE(errorOf([&]{ctx.people.create(ctx.session,{"  ",ResponsibleRole::Professor,{}});}),QString("Nome é obrigatório."));
        QCOMPARE(errorOf([&]{ctx.people.create(ctx.session,{QString(121,'x'),ResponsibleRole::Professor,{}});}),QString("Nome excede o limite de caracteres."));
        QCOMPARE(errorOf([&]{ctx.people.create(ctx.session,{"Sem função",ResponsibleRole::Unknown,{}});}),QString("Selecione Professor ou Auxiliar."));
        QCOMPARE(count(ctx,"events"),events);
        QSqlQuery query(ctx.database.connection()); QVERIFY(query.exec("SELECT COUNT(*) FROM people WHERE subject IS NULL")); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(),2);
        TestContext reopened(path); QCOMPARE(person(reopened,longId).subject,QString(120,'x'));
        QCOMPARE(reopened.people.people(CatalogFilter::Active,"auxiliar").first().id,assistant);
        QCOMPARE(reopened.people.people(CatalogFilter::Active,QString(120,'x')).first().id,longId);
    }
    void roleChangeClearsSubject() {
        QTemporaryDir temp; const auto path=temp.filePath("role.sqlite"); TestContext ctx(path);
        const auto id=ctx.people.create(ctx.session,{"Ana",ResponsibleRole::Professor,"Matemática"});
        ctx.people.update(ctx.session,person(ctx,id),{"Ana",ResponsibleRole::Assistant,"Matemática"});
        QVERIFY(person(ctx,id).subject.isEmpty());
        auto changes=LocalDatabase::change("person_role","professor","assistant");
        changes.insert("subject",LocalDatabase::change("subject","Matemática",QJsonValue::Null).value("subject"));
        QCOMPARE(ctx.store.history().first().changes,changes);
        ctx.people.update(ctx.session,person(ctx,id),{"Ana",ResponsibleRole::Professor,{}});
        QCOMPARE(ctx.store.history().first().changes,LocalDatabase::change("person_role","assistant","professor"));
        TestContext reopened(path); QVERIFY(person(reopened,id).subject.isEmpty());
        QSqlQuery query(reopened.database.connection()); QVERIFY(query.exec("SELECT subject FROM people")); QVERIFY(query.next()); QVERIFY(query.value(0).isNull());
    }
    void updatePersonGuards() {
        QTemporaryDir temp; const auto path=temp.filePath("guards.sqlite"); TestContext ctx(path);
        const auto id=ctx.people.create(ctx.session,{"Ana",ResponsibleRole::Professor,{}}); const auto expected=person(ctx,id);
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        const PersonDraft changed{"Ana nova",ResponsibleRole::Professor,"Física"}; const int baseline=count(ctx,"events");
        QVERIFY_EXCEPTION_THROWN(ctx.people.create(Session(),changed),AuthorizationError);
        QVERIFY_EXCEPTION_THROWN(ctx.people.update(Session(),expected,changed),AuthorizationError);
        QCOMPARE(count(ctx,"people"),1); QCOMPARE(count(ctx,"events"),baseline); QCOMPARE(person(ctx,id).name,expected.name);
        QCOMPARE(errorOf([&]{ctx.people.update(common,expected,{"Ana",ResponsibleRole::Professor,{}});}),QString("Nenhuma alteração para salvar."));
        QCOMPARE(errorOf([&]{ctx.people.update(common,expected,{"Ana",ResponsibleRole::Unknown,{}});}),QString("Selecione Professor ou Auxiliar."));
        { TestContext second(path); second.people.update(second.session,person(second,id),changed); }
        QCOMPARE(errorOf([&]{ctx.people.update(common,expected,{"Outra",ResponsibleRole::Assistant,{}});}),QString("Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente."));
        ctx.people.update(common,person(ctx,id),{"Ana corrigida",ResponsibleRole::Assistant,{}});
        QCOMPARE(ctx.store.history().first().actorUsername,QString("operador"));
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","people-inactive-account"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec("UPDATE accounts SET active=0 WHERE username='operador'"));
        }
        QSqlDatabase::removeDatabase("people-inactive-account"); const int events=count(ctx,"events");
        QVERIFY_EXCEPTION_THROWN(ctx.people.create(common,changed),AuthorizationError);
        QVERIFY_EXCEPTION_THROWN(ctx.people.update(common,person(ctx,id),changed),AuthorizationError);
        QCOMPARE(count(ctx,"events"),events); QCOMPARE(count(ctx,"people"),1); QCOMPARE(person(ctx,id).name,QString("Ana corrigida"));
        ctx.people.inactivate(ctx.session,id);
        QCOMPARE(errorOf([&]{ctx.people.update(ctx.session,person(ctx,id),changed);}),QString("Cadastros inativos não podem ser editados."));
        Person missing; missing.id=999;
        QCOMPARE(errorOf([&]{ctx.people.update(ctx.session,missing,changed);}),QString("Pessoa não encontrada."));
    }
    void inactivatePersonRequiresAdmin() {
        QTemporaryDir temp; const auto path=temp.filePath("inactivate.sqlite"); TestContext ctx(path);
        const auto first=ctx.people.create(ctx.session,{"Ana",ResponsibleRole::Professor,{}});
        const auto second=ctx.people.create(ctx.session,{"Bia",ResponsibleRole::Assistant,{}});
        const auto third=ctx.people.create(ctx.session,{"Carlos",ResponsibleRole::Assistant,{}});
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        const int events=count(ctx,"events");
        try { ctx.people.inactivate(common,first); QFAIL("Conta comum inativou pessoa"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Forbidden); QCOMPARE(QString::fromUtf8(e.what()),QString("Somente administradores podem inativar pessoas e notebooks.")); }
        QCOMPARE(count(ctx,"events"),events); QVERIFY(person(ctx,first).active);
        ctx.accounts.createAdministrator(ctx.session,"administradora",ctx.auth.preparePassword("senha-admin-segura"),InitialPasswordMode::Assisted);
        const auto admin=ctx.auth.login("administradora","senha-admin-segura"); ctx.people.inactivate(admin,first);
        QCOMPARE(ctx.store.history().first().actorUsername,QString("administradora"));
        QCOMPARE(ctx.store.history().first().action,QString("Pessoa inativada"));
        QCOMPARE(ctx.store.history().first().changes,LocalDatabase::change("active",1,0));
        ctx.people.inactivate(ctx.session,second);
        QCOMPARE(errorOf([&]{ctx.people.inactivate(ctx.session,first);}),QString("Esta pessoa já está inativa."));
        QCOMPARE(ctx.people.activePeople().size(),1); QCOMPARE(ctx.people.people(CatalogFilter::Inactive).size(),2);
        ctx.clock.advanceWall(15*60); ctx.clock.advanceMonotonic(15*60*1000); const int beforeExpired=count(ctx,"events");
        try { ctx.people.inactivate(ctx.session,third); QFAIL("Sessão expirada inativou pessoa"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Expired); }
        QVERIFY(person(ctx,third).active); QCOMPARE(count(ctx,"events"),beforeExpired);
        TestContext reopened(path); QVERIFY(!person(reopened,first).active); QVERIFY(!person(reopened,second).active); QVERIFY(person(reopened,third).active);
    }
    void personRollbackOnEventFailure() {
        QTemporaryDir temp; const auto path=temp.filePath("person-rollback.sqlite"); TestContext ctx(path);
        const auto id=ctx.people.create(ctx.session,{"Ana",ResponsibleRole::Professor,"Matemática"}); const auto expected=person(ctx,id); const int events=count(ctx,"events");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","people-event-failure"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec("CREATE TRIGGER fail_person_event BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'falha simulada'); END"));
        }
        QSqlDatabase::removeDatabase("people-event-failure");
        QVERIFY_EXCEPTION_THROWN(ctx.people.create(ctx.session,{"Bia",ResponsibleRole::Assistant,{}}),std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(ctx.people.update(ctx.session,expected,{"Ana nova",ResponsibleRole::Assistant,{}}),std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(ctx.people.inactivate(ctx.session,id),std::runtime_error);
        QCOMPARE(count(ctx,"events"),events); QCOMPARE(count(ctx,"people"),1);
        TestContext reopened(path); QCOMPARE(person(reopened,id).name,expected.name); QCOMPARE(person(reopened,id).subject,expected.subject); QVERIFY(person(reopened,id).active);
    }
    void createPersonAppearsInLogs() {
        QTemporaryDir temp; const auto path=temp.filePath("person.sqlite"); TestContext ctx(path);
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        const int accounts=count(ctx,"accounts");
        const auto id=ctx.people.create(common,{" Ana Souza ",ResponsibleRole::Professor," Matemática "});
        const auto rows=ctx.people.people(); QCOMPARE(rows.size(),1); QCOMPARE(rows.first().id,id);
        QCOMPARE(rows.first().name,QString("Ana Souza")); QCOMPARE(rows.first().subject,QString("Matemática"));
        QCOMPARE(count(ctx,"accounts"),accounts);
        const auto event=ctx.store.history().first(); QCOMPARE(event.action,QString("Pessoa cadastrada"));
        QCOMPARE(event.entityType,QString("person")); QCOMPARE(event.actorUsername,QString("operador"));
        QVERIFY(event.assetTag.isEmpty()); QVERIFY(event.notebookName.isEmpty());
        TestContext reopened(path); QCOMPARE(reopened.people.activePeople().first().subject,QString("Matemática"));
    }
    void locationChangesAuthorizeAndRollbackTogether() {
        QTemporaryDir temp; const auto path=temp.filePath("atomic.sqlite"); TestContext ctx(path);
        const auto id=ctx.locations.create(ctx.session,"Sala 12"); const auto expected=location(ctx,id); const int events=count(ctx,"events");
        QVERIFY_EXCEPTION_THROWN(ctx.locations.rename(Session(),expected,"Outra"),AuthorizationError);
        QVERIFY_EXCEPTION_THROWN(ctx.locations.softDelete(Session(),id),AuthorizationError);
        QSqlQuery query(ctx.database.connection()); QVERIFY(query.exec("CREATE TRIGGER fail_catalog_change BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'falha simulada'); END"));
        QVERIFY_EXCEPTION_THROWN(ctx.locations.rename(ctx.session,expected,"Outra"),std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(ctx.locations.softDelete(ctx.session,id),std::runtime_error);
        QCOMPARE(location(ctx,id).name,expected.name); QVERIFY(!location(ctx,id).deleted); QCOMPARE(count(ctx,"events"),events);
        TestContext reopened(path); QCOMPARE(location(reopened,id).name,expected.name); QVERIFY(!location(reopened,id).deleted);
        ctx.clock.advance(15*60);
        QVERIFY_EXCEPTION_THROWN(ctx.locations.rename(ctx.session,expected,"Outra"),AuthorizationError);
        QVERIFY_EXCEPTION_THROWN(ctx.locations.softDelete(ctx.session,id),AuthorizationError);
        QCOMPARE(count(ctx,"events"),events);
    }
    void renameLocation() {
        QTemporaryDir temp; const auto path=temp.filePath("rename.sqlite"); TestContext ctx(path);
        const auto id=ctx.locations.create(ctx.session,"Sala 12"); ctx.locations.create(ctx.session,"Biblioteca");
        const auto expected=location(ctx,id);
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        QCOMPARE(errorOf([&]{ctx.locations.rename(common,expected," biblioteca ");}),QString("Já existe um local ativo com esse nome."));
        ctx.locations.rename(common,expected,"SALA 12");
        QCOMPARE(ctx.store.history().first().changes,LocalDatabase::change("name","Sala 12","SALA 12"));
        QCOMPARE(ctx.store.history().first().actorUsername,QString("operador"));
        QCOMPARE(errorOf([&]{ctx.locations.rename(common,location(ctx,id),"SALA 12");}),QString("Nenhuma alteração para salvar."));
        TestContext second(path); second.locations.rename(second.session,location(second,id),"Sala nova");
        QCOMPARE(errorOf([&]{ctx.locations.rename(common,expected,"Outro nome");}),QString("Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente."));
        ctx.locations.softDelete(common,id);
        QCOMPARE(errorOf([&]{ctx.locations.rename(common,location(ctx,id),"Renomeado");}),QString("Locais excluídos não podem ser editados."));
    }
    void deletedLocationRefusedForNewMoves() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("moves.sqlite"));
        const auto room=ctx.locations.create(ctx.session,"Sala 12");
        Fixture::add(ctx,ctx.session,"NB-1","Um","Biblioteca"); const auto available=ctx.store.notebooks().first().id;
        Fixture::add(ctx,ctx.session,"NB-2","Dois","Biblioteca"); const auto loaned=ctx.store.notebooks().first().id;
        Fixture::checkout(ctx,ctx.session,loaned,"Ana",ResponsibleRole::Professor,"Sala 12");
        ctx.locations.softDelete(ctx.session,room); const int events=count(ctx,"events"), loans=count(ctx,"loans");
        const QString error="O local Sala 12 foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais.";
        QCOMPARE(errorOf([&]{Fixture::checkout(ctx,ctx.session,available,"Ana",ResponsibleRole::Professor," sala 12");}),error);
        QCOMPARE(errorOf([&]{Fixture::checkin(ctx,ctx.session,loaned,"Bia",ResponsibleRole::Assistant," sala 12");}),error);
        QCOMPARE(errorOf([&]{Fixture::updateLocation(ctx,ctx.session,available," sala 12");}),error);
        QCOMPARE(count(ctx,"events"),events); QCOMPARE(count(ctx,"loans"),loans);
        QCOMPARE(ctx.store.notebooks().first().status,Status::Loaned);
        const auto newId=ctx.locations.create(ctx.session,"Sala 12"); QVERIFY(newId!=room);
        Fixture::checkout(ctx,ctx.session,available,"Ana",ResponsibleRole::Professor,"sala 12");
        QSqlQuery query(ctx.database.connection()); QVERIFY(query.exec("SELECT checkout_location_id,checkout_location FROM loans ORDER BY id DESC")); QVERIFY(query.next());
        QCOMPARE(query.value(0).toLongLong(),newId); QCOMPARE(query.value(1).toString(),QString("Sala 12"));
    }
    void deletedBetweenOpenAndConfirm() {
        QTemporaryDir temp; const auto path=temp.filePath("concurrent.sqlite"); TestContext first(path);
        const auto id=first.locations.create(first.session,"Laboratório"); Fixture::add(first,first.session,"NB-1","Um","Pátio");
        const auto notebook=first.store.notebooks().first().id; QVERIFY(first.locations.activeLocationNames().contains("Laboratório"));
        TestContext second(path); second.locations.softDelete(second.session,id);
        const int events=count(first,"events");
        QCOMPARE(errorOf([&]{Fixture::checkout(first,first.session,notebook,"Ana",ResponsibleRole::Professor,"Laboratório");}),QString("O local Laboratório foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais."));
        QCOMPARE(count(first,"events"),events); QCOMPARE(count(first,"loans"),0);
    }
    void renameDoesNotRewriteHistory() {
        QTemporaryDir temp; const auto path=temp.filePath("history.sqlite"); TestContext ctx(path);
        const auto room=ctx.locations.create(ctx.session,"Sala 12"), library=ctx.locations.create(ctx.session,"Biblioteca");
        Fixture::add(ctx,ctx.session,"NB-1","Um","Sala 12"); const auto id=ctx.store.notebooks().first().id;
        Fixture::checkout(ctx,ctx.session,id,"Ana",ResponsibleRole::Professor,"Sala 12"); Fixture::checkin(ctx,ctx.session,id,"Bia",ResponsibleRole::Assistant,"Biblioteca");
        const auto before=ctx.store.history();
        ctx.locations.rename(ctx.session,location(ctx,room),"Sala nova"); ctx.locations.rename(ctx.session,location(ctx,library),"Biblioteca nova");
        ctx.locations.softDelete(ctx.session,room);
        const auto after=ctx.store.history();
        for (const auto &old : before) for (const auto &current : after) if (old.id==current.id) { QCOMPARE(current.details,old.details); QCOMPARE(current.changes,old.changes); }
        QCOMPARE(ctx.store.notebooks().first().location,QString("Biblioteca nova"));
        TestContext reopened(path); QSqlQuery query(reopened.database.connection()); QVERIFY(query.exec("SELECT checkout_location,return_location,checkout_location_id,return_location_id FROM loans")); QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(),QString("Sala 12")); QCOMPARE(query.value(1).toString(),QString("Biblioteca")); QCOMPARE(query.value(2).toLongLong(),room); QCOMPARE(query.value(3).toLongLong(),library);
    }
    void locationEventsRecordLocationId() {
        QTemporaryDir temp; TestContext ctx(temp.filePath("event-id.sqlite"));
        const auto room=ctx.locations.create(ctx.session,"Sala 12"), library=ctx.locations.create(ctx.session,"Biblioteca");
        Fixture::add(ctx,ctx.session,"NB-1","Um","sala 12"); const auto id=ctx.store.notebooks().first().id;
        const auto added=ctx.store.history().first(); QCOMPARE(added.changes.value("location").toObject(),LocalDatabase::change("location",QJsonValue::Null,"Sala 12").value("location").toObject());
        QCOMPARE(added.changes.value("location_id").toObject(),LocalDatabase::change("location_id",QJsonValue::Null,QJsonValue(room)).value("location_id").toObject());
        Fixture::updateLocation(ctx,ctx.session,id,"Biblioteca");
        QCOMPARE(ctx.store.history().first().changes,([&]{auto changes=LocalDatabase::change("location","Sala 12","Biblioteca"); changes.insert("location_id",LocalDatabase::change("location_id",QJsonValue(room),QJsonValue(library)).value("location_id")); return changes;})());
        Fixture::checkout(ctx,ctx.session,id,"Ana",ResponsibleRole::Professor,"Pátio");
        QCOMPARE(ctx.store.history().first().changes.value("location_id").toObject(),LocalDatabase::change("location_id",QJsonValue(library),QJsonValue(Fixture::location(ctx,"Pátio"))).value("location_id").toObject());
        ctx.locations.rename(ctx.session,location(ctx,room),"Sala nova");
        QSqlQuery query(ctx.database.connection()); QVERIFY(query.exec("SELECT entity_id FROM events WHERE action='Local atualizado'")); QVERIFY(query.next()); QCOMPARE(query.value(0).toLongLong(),room);
        for (const auto &event : ctx.store.history()) if (event.id==added.id) QCOMPARE(event.changes,added.changes);
    }
    void deleteLocationInUse() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto path=temp.filePath("delete.sqlite"); TestContext ctx(path);
        const auto room=ctx.locations.create(ctx.session,"Sala 12"); ctx.locations.create(ctx.session,"Biblioteca");
        Fixture::add(ctx,ctx.session,"NB-1","Um","sala 12"); Fixture::add(ctx,ctx.session,"NB-2","Dois","Sala 12");
        Fixture::add(ctx,ctx.session,"NB-3","Três","Biblioteca");
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        QCOMPARE(ctx.locations.notebooksAt(room),2); ctx.locations.softDelete(common,room);
        int linked=0;
        for (const auto &notebook : ctx.store.notebooks()) if (notebook.locationId==room) {
            ++linked; QVERIFY(notebook.locationDeleted); QCOMPARE(notebook.location,QString("Sala 12"));
        }
        QCOMPARE(linked,2); QVERIFY(!ctx.locations.activeLocationNames().contains("Sala 12"));
        QCOMPARE(ctx.locations.locations(CatalogFilter::Inactive).first().id,room);
        const auto event=ctx.store.history().first(); QCOMPARE(event.action,QString("Local excluído")); QCOMPARE(event.actorUsername,QString("operador"));
        QCOMPARE(event.changes,LocalDatabase::change("location_deleted",0,1));
        QSqlQuery query(ctx.database.connection()); QVERIFY(query.exec("SELECT deleted_at FROM locations WHERE id="+QString::number(room))); QVERIFY(query.next()); QVERIFY(query.value(0).toString().endsWith('Z'));
        try { ctx.locations.softDelete(common,room); QFAIL("Segunda exclusão aceita"); }
        catch (const std::runtime_error &e) { QCOMPARE(QString::fromUtf8(e.what()),QString("Este local já foi excluído.")); }
        const int events=count(ctx,"events");
        try { Fixture::add(ctx,common,"NB-4","Quatro","Sala 12"); QFAIL("Local excluído aceito"); }
        catch (const std::runtime_error &e) { QCOMPARE(QString::fromUtf8(e.what()),QString("O local Sala 12 foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais.")); }
        QCOMPARE(count(ctx,"events"),events); QCOMPARE(count(ctx,"notebooks"),3);
        QCOMPARE(createError(ctx,common,"Sala 12"),QString());
        TestContext reopened(path); QCOMPARE(reopened.locations.notebooksAt(room),2);
        // Um nome liberado ganha outra identidade sem alterar os vínculos anteriores.
        QVERIFY(reopened.locations.locations().first().id != room);
    }
    void locationNameUniqueAmongActive() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto path=temp.filePath("unique.sqlite");
        TestContext ctx(path); const auto id=ctx.locations.create(ctx.session,"Sala 12"); QVERIFY(id>0);
        const int events=count(ctx,"events");
        QCOMPARE(createError(ctx,ctx.session," sala 12 "),QString("Já existe um local ativo com esse nome."));
        TestContext second(path);
        QCOMPARE(createError(second,second.session," sala 12"),QString("Já existe um local ativo com esse nome."));
        QCOMPARE(count(ctx,"locations"),1); QCOMPARE(count(ctx,"events"),events);
        QSqlQuery event(ctx.database.connection()); QVERIFY(event.exec("SELECT entity_type,entity_id,actor_username,changes,happened_at FROM events WHERE action='Local cadastrado'"));
        QVERIFY(event.next()); QCOMPARE(event.value(0).toString(),QString("location")); QCOMPARE(event.value(1).toLongLong(),id);
        QCOMPARE(event.value(2).toString(),TestPrincipal);
        QCOMPARE(QJsonDocument::fromJson(event.value(3).toByteArray()).object(),LocalDatabase::change("name",QJsonValue::Null,"Sala 12"));
        QVERIFY(event.value(4).toString().endsWith('Z'));
    }
    void locationRequiresAuthenticatedSession() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto path=temp.filePath("session.sqlite"); TestContext ctx(path);
        const int baseline=count(ctx,"events");
        QVERIFY_EXCEPTION_THROWN(ctx.locations.create(Session(),"Sem sessão"),AuthorizationError);
        QCOMPARE(count(ctx,"locations"),0); QCOMPARE(count(ctx,"events"),baseline);
        const auto common=ctx.createCommonAndLogin("operador","senha-operador-segura",InitialPasswordMode::Assisted);
        ctx.locations.create(common,"Biblioteca"); QCOMPARE(count(ctx,"locations"),1);
        const int afterCommon=count(ctx,"events");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","catalog-inactive"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec("UPDATE accounts SET active=0 WHERE username='operador'"));
        }
        QSqlDatabase::removeDatabase("catalog-inactive");
        QVERIFY_EXCEPTION_THROWN(ctx.locations.create(common,"Conta inativa"),AuthorizationError);
        ctx.clock.advance(15*60);
        try { ctx.locations.create(ctx.session,"Expirado"); QFAIL("Sessão expirada aceita"); }
        catch (const AuthorizationError &e) { QCOMPARE(e.reason(),AuthorizationError::Reason::Expired); }
        QCOMPARE(count(ctx,"locations"),1); QCOMPARE(count(ctx,"events"),afterCommon);
    }
    void locationRollbackOnEventFailure() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto path=temp.filePath("rollback.sqlite"); TestContext ctx(path);
        const int baseline=count(ctx,"events");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","catalog-event-failure"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec("CREATE TRIGGER fail_location_event BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'evento indisponível'); END"));
        }
        QSqlDatabase::removeDatabase("catalog-event-failure");
        QVERIFY_EXCEPTION_THROWN(ctx.locations.create(ctx.session,"Biblioteca"),std::runtime_error);
        QCOMPARE(count(ctx,"locations"),0); QCOMPARE(count(ctx,"events"),baseline);
        TestContext reopened(path,false); QCOMPARE(count(reopened,"locations"),0);
    }
    void locationValidation() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto path=temp.filePath("validation.sqlite");
        qint64 id=0; const QString accented(120,QChar(0x00e1));
        {
            TestContext ctx(path); const int baseline=count(ctx,"events");
            QCOMPARE(createError(ctx,ctx.session,"   "),QString("Nome do local é obrigatório."));
            QCOMPARE(createError(ctx,ctx.session,QString(121,'x')),QString("Nome do local excede o limite de caracteres."));
            QCOMPARE(count(ctx,"events"),baseline); QCOMPARE(count(ctx,"locations"),0);
            id=ctx.locations.create(ctx.session,accented);
        }
        TestContext reopened(path); const auto rows=reopened.locations.locations();
        QCOMPARE(rows.size(),1); QCOMPARE(rows.first().id,id); QCOMPARE(rows.first().name,accented);
    }
    void locationsListAndSearch() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); TestContext ctx(temp.filePath("search.sqlite"));
        for (const auto &name : {"Sala 12","Biblioteca","Laboratório"}) ctx.locations.create(ctx.session,name);
        const auto filtered=ctx.locations.locations(CatalogFilter::Active,"sala");
        QCOMPARE(filtered.size(),1); QCOMPARE(filtered.first().name,QString("Sala 12"));
        const auto rows=ctx.locations.locations(); QCOMPARE(rows.size(),3);
        QCOMPARE(rows[0].name,QString("Biblioteca")); QCOMPARE(rows[1].name,QString("Laboratório")); QCOMPARE(rows[2].name,QString("Sala 12"));
        QCOMPARE(rows[0].notebookCount,0); QVERIFY(!rows[0].deleted);
        QVERIFY(ctx.locations.locations(CatalogFilter::Inactive).isEmpty());
        QCOMPARE(ctx.locations.activeLocationNames(),QStringList({"Biblioteca","Laboratório","Sala 12"}));
    }
};
QTEST_GUILESS_MAIN(CatalogTests)
#include "CatalogTests.moc"

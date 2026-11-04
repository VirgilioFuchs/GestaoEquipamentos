#include "application/HistoryQueryService.h"
#include "domain/HistoryPeriod.h"
#include "domain/EventCatalog.h"
#include "storage/LocalDatabase.h"
#include "TestSupport.h"
#include <QTemporaryDir>
#include <QtTest>
#include <QElapsedTimer>
#include <algorithm>

class HistoryQueryTests : public QObject {
    Q_OBJECT
private slots:
    void filterByAccountExcludesLegacyActor() {
        QTemporaryDir dir; TestContext c(dir.filePath("account.sqlite"));
        c.database.transaction([&] { c.database.appendEvent({"system",std::nullopt,std::nullopt,"Legado","",std::nullopt,{},"2025-01-01T00:00:00.000Z"}); });
        HistoryQueryService service(c.database); HistoryFilter f; f.period=PeriodPreset::All; f.accountId=c.session.accountId();
        const auto page=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); QVERIFY(!page.entries.isEmpty());
        for (const auto &entry : page.entries) { QVERIFY(entry.action!="Legado"); QCOMPARE(entry.actorUsername,TestPrincipal); }
        f.accountId.reset(); bool legacy=false; for (const auto &entry : service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")).entries) if (entry.action=="Legado") { legacy=true; QVERIFY(entry.actorUsername.isEmpty()); } QVERIFY(legacy);
    }
    void movementEventsCorrelateWithLoans() {
        QTemporaryDir dir; TestContext c(dir.filePath("movement.sqlite")); const auto l=createLocation(c,"Sala");
        const auto p=createPerson(c,"Ana",ResponsibleRole::Professor,QString::fromUtf8("Matemática")); const auto n=c.store.add(c.session,"NB-1","Notebook",l);
        c.store.checkout(c.session,n,p,l); c.store.checkin(c.session,n,p,l);
        HistoryQueryService service(c.database); HistoryFilter f; f.period=PeriodPreset::All; f.personId=p;
        const auto result=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); QCOMPARE(result.entries.size(),2);
        for (const auto &e : result.entries) { QVERIFY(e.loanId.has_value()); QCOMPARE(e.responsibleName,QString("Ana")); QCOMPARE(e.responsibleSubject,QString::fromUtf8("Matemática"));
            QSqlQuery q(c.database.connection()); q.prepare("SELECT checked_out_at,returned_at FROM loans WHERE id=?"); q.addBindValue(*e.loanId); QVERIFY(q.exec()); QVERIFY(q.next());
            QCOMPARE(e.happenedAt.toUTC().toString(Qt::ISODateWithMs),q.value(e.action=="Retirada" ? 0 : 1).toString()); }
    }
    void combinedFiltersAreConjunctive() {
        QTemporaryDir dir; TestContext c(dir.filePath("combined.sqlite"));
        const auto loc=createLocation(c,"Sala"), person=createPerson(c,"Ana",ResponsibleRole::Professor);
        const auto n=c.store.add(c.session,"NB-1","Notebook",loc); c.store.checkout(c.session,n,person,loc);
        HistoryFilter f; f.period=PeriodPreset::All; f.notebookId=n; f.personId=person; f.accountId=c.session.accountId(); f.locationId=loc; f.action="Retirada";
        HistoryQueryService service(c.database); const auto now=QDateTime::currentDateTimeUtc(); const QTimeZone zone("UTC");
        QCOMPARE(service.search(f,now,zone).entries.size(),1);
        auto different=f; different.action=QString::fromUtf8("Devolução"); QVERIFY(service.search(different,now,zone).entries.isEmpty());
        different=f; different.accountId=999999; QVERIFY(service.search(different,now,zone).entries.isEmpty());
        different=f; different.personId=999999; QVERIFY(service.search(different,now,zone).entries.isEmpty());
        different=f; different.locationId=999999; QVERIFY(service.search(different,now,zone).entries.isEmpty());
        different=f; different.notebookId=999999; QVERIFY(service.search(different,now,zone).entries.isEmpty());
    }
    void filterOptionsMarkersAndOrder() {
        QTemporaryDir dir; TestContext c(dir.filePath("options.sqlite"));
        const auto loc=createLocation(c,"Sala"); c.locations.softDelete(c.session,loc);
        const auto n=Fixture::add(c,c.session,"NB-1","Notebook","Biblioteca"); c.store.inactivate(c.session,n);
        const auto account=c.accounts.createCommonAccount(c.session,"operador",c.auth.preparePassword(TestPassword),InitialPasswordMode::Assisted);
        c.accounts.inactivateCommonAccount(c.session,account);
        HistoryQueryService service(c.database); const auto options=service.filterOptions();
        QVERIFY(std::any_of(options.deletedLocations.cbegin(),options.deletedLocations.cend(),[loc](const FilterOption &o){ return o.id==loc && o.label.endsWith(QString::fromUtf8(" (excluído)")); }));
        QVERIFY(std::any_of(options.notebooks.cbegin(),options.notebooks.cend(),[n](const FilterOption &o){ return o.id==n && o.label.endsWith(" (inativo)"); }));
        QVERIFY(std::any_of(options.accounts.cbegin(),options.accounts.cend(),[account](const FilterOption &o){ return o.id==account && o.label.endsWith(" (inativa)"); }));
        QVERIFY(options.actions.indexOf("Cadastro") < options.actions.indexOf("Pessoa cadastrada") || !options.actions.contains("Pessoa cadastrada"));
    }
    void historyQueryAt100k() {
        QTemporaryDir dir; TestContext c(dir.filePath("performance.sqlite"));
        const auto loc=createLocation(c,"Sala"), person=createPerson(c,"Ana",ResponsibleRole::Professor);
        const auto n=c.store.add(c.session,"NB-1","Notebook",loc);
        QSqlQuery q(c.database.connection());
        c.database.transaction([&] {
            QVERIFY(q.exec("WITH RECURSIVE seq(x) AS (VALUES(2) UNION ALL SELECT x+1 FROM seq WHERE x<300) INSERT INTO notebooks(id,asset_tag,name) SELECT x,printf('NB-%04d',x),'Notebook' FROM seq"));
            QVERIFY(q.exec(QString("WITH RECURSIVE seq(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM seq WHERE x<40000) INSERT INTO loans(notebook_id,borrower,checked_out_at,returned_at,borrower_person_id) SELECT (x%%300)+1,'Ana',strftime('%%Y-%%m-%%dT%%H:%%M:%%S.000Z','2024-01-01',printf('+%%d seconds',x*600)),strftime('%%Y-%%m-%%dT%%H:%%M:%%S.000Z','2024-01-01',printf('+%%d seconds',x*600+1)),%1 FROM seq").arg(person).replace("%%","%")));
            QVERIFY(q.exec(QString("WITH RECURSIVE seq(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM seq WHERE x<100000) INSERT INTO events(entity_type,entity_id,notebook_id,action,details,happened_at) SELECT 'notebook',(x%%300)+1,(x%%300)+1,'Retirada','',strftime('%%Y-%%m-%%dT%%H:%%M:%%S.000Z','2024-01-01',printf('+%%d seconds',x*600)) FROM seq").replace("%%","%")));
        });
        HistoryQueryService service(c.database); const auto now=QDateTime::fromString("2025-01-01T12:00:00.000Z",Qt::ISODateWithMs); const QTimeZone zone("UTC");
        for (int i=0;i<3;++i) { HistoryFilter f; if (i==1) { f.period=PeriodPreset::All; f.personId=person; } if (i==2) { f.period=PeriodPreset::All; f.notebookId=n; }
            QStringList plan; service.search(f,now,zone,HistoryQueryService::DefaultLimit,&plan); const auto text=plan.join(" | "); qInfo() << "EXPLAIN" << text;
            for (const auto &line : plan) QVERIFY(!line.startsWith("SCAN e ") && line!="SCAN e"); QVERIFY(text.contains("INDEX"));
            QElapsedTimer timer; timer.start(); const auto result=service.search(f,now,zone); const auto ms=timer.elapsed(); qInfo() << "historyQueryAt100k" << i << ms << "ms"; QVERIFY(!result.entries.isEmpty()); QVERIFY(ms<3000); }
    }
    void locationByIdIncludesDeletedAndLegacyLinks() {
        QTemporaryDir dir; TestContext c(dir.filePath("local.sqlite"));
        const auto sala=createLocation(c,"Sala 12"), other=createLocation(c,"Biblioteca");
        const auto n=c.store.add(c.session,"NB-12","Notebook",sala);
        const auto person=createPerson(c,"Ana",ResponsibleRole::Professor);
        c.store.checkout(c.session,n,person,sala); c.store.updateLocation(c.session,n,other); c.locations.softDelete(c.session,sala);
        QSqlQuery q(c.database.connection()); q.prepare("INSERT INTO events(entity_type,entity_id,notebook_id,action,details,happened_at,changes) VALUES('notebook',?,?,'Localização','v5','2025-01-01T00:00:00.000Z',?)");
        q.addBindValue(n); q.addBindValue(n); q.addBindValue(QString("{\"location_id\":{\"de\":%1,\"para\":%2}}").arg(sala).arg(other)); QVERIFY(q.exec());
        HistoryFilter f; f.period=PeriodPreset::All; f.locationId=sala; HistoryQueryService service(c.database);
        HistoryPage page; try { page=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); } catch (const std::exception &e) { QFAIL(e.what()); }
        bool checkout=false, change=false, legacy=false, deleted=false;
        for (const auto &entry : page.entries) { checkout |= entry.action=="Retirada"; change |= entry.action==QString::fromUtf8("Localização") && entry.details!="v5"; legacy |= entry.details=="v5"; deleted |= entry.action==QString::fromUtf8("Local excluído"); }
        QVERIFY(checkout); QVERIFY(change); QVERIFY(legacy); QVERIFY(deleted);
    }
    void locationLegacyTextAndDetails() {
        QTemporaryDir dir; LocalDatabase db(dir.filePath("legacy.sqlite"));
        db.transaction([&] { db.appendEvent({"system",std::nullopt,std::nullopt,"Antigo",QString::fromUtf8("Local: Sala_1"),std::nullopt,{},"2026-01-01T00:00:00.000Z"});
            db.appendEvent({"system",std::nullopt,std::nullopt,"Antigo","Local: Sala11",std::nullopt,{},"2026-01-01T00:01:00.000Z"}); });
        HistoryFilter f; f.period=PeriodPreset::All; f.legacyLocationText="Sala_1"; HistoryQueryService service(db);
        HistoryPage page; try { page=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); } catch (const std::exception &e) { QFAIL(e.what()); }
        QCOMPARE(page.entries.size(),1); QVERIFY(page.entries.first().details.contains("Sala_1"));
    }
    void legacyLocationMatchesSnapshotsAndJson() {
        QTemporaryDir dir; TestContext c(dir.filePath("legacy.sqlite")); const auto n=Fixture::add(c,c.session,"NB-1","Notebook","Atual");
        QSqlQuery q(c.database.connection());
        QVERIFY(q.exec(QString("INSERT INTO loans(notebook_id,borrower,checked_out_at,returned_at,checkout_location) VALUES(%1,'José','2025-01-01T10:00:00.000Z','2025-01-01T11:00:00.000Z','Sala Antiga')").arg(n)));
        QVERIFY(q.exec(QString("INSERT INTO events(entity_type,entity_id,notebook_id,action,details,happened_at) VALUES('notebook',%1,%1,'Retirada','histórico','2025-01-01T10:00:00.000Z')").arg(n)));
        q.prepare("INSERT INTO events(entity_type,entity_id,notebook_id,action,details,happened_at,changes) VALUES('notebook',?,?, 'Localização','','2025-01-01T12:00:00.000Z',?)");
        q.addBindValue(n); q.addBindValue(n); q.addBindValue(QStringLiteral("{\"location\":{\"de\":\"Sala Antiga\",\"para\":\"Pátio\"}}")); QVERIFY(q.exec());
        HistoryQueryService service(c.database); HistoryFilter f; f.period=PeriodPreset::All; f.legacyLocationText="sala antiga";
        QCOMPARE(service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")).entries.size(),2);
        QVERIFY(service.filterOptions().legacyLocationTexts.contains("Sala Antiga"));
    }
    void resultCapAt500() {
        QTemporaryDir dir; LocalDatabase db(dir.filePath("cap.sqlite"));
        db.transaction([&] { for (int i=0;i<501;++i) db.appendEvent({"system",std::nullopt,std::nullopt,"Teste",QString::number(i),std::nullopt,{},"2026-01-01T00:00:00.000Z"}); });
        HistoryFilter f; f.period=PeriodPreset::All; HistoryQueryService service(db); const auto page=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC"));
        QCOMPARE(page.entries.size(),500); QVERIFY(page.truncated); QCOMPARE(page.entries.first().details,QString("500"));
        const auto precise=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC"),501); QCOMPARE(precise.entries.size(),501); QVERIFY(!precise.truncated);
        HistoryFilter exact; exact.period=PeriodPreset::Custom; exact.customFrom=QDate(2026,1,2); exact.customTo=QDate(2026,1,2);
        db.transaction([&] { for (int i=0;i<500;++i) db.appendEvent({"system",std::nullopt,std::nullopt,"Teste","",std::nullopt,{},"2026-01-02T00:00:00.000Z"}); });
        const auto exactly=service.search(exact,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); QCOMPARE(exactly.entries.size(),500); QVERIFY(!exactly.truncated);
    }
    void likeWildcardsAreLiteral() {
        QTemporaryDir dir; LocalDatabase db(dir.filePath("wildcards.sqlite"));
        db.transaction([&] { db.appendEvent({"system",std::nullopt,std::nullopt,"Antigo","Sala 100%",std::nullopt,{},"2025-01-01T00:00:00.000Z"});
            db.appendEvent({"system",std::nullopt,std::nullopt,"Antigo","Sala 1000",std::nullopt,{},"2025-01-01T00:00:01.000Z"}); });
        HistoryFilter f; f.period=PeriodPreset::All; f.legacyLocationText="100%"; HistoryQueryService service(db);
        const auto result=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); QCOMPARE(result.entries.size(),1); QCOMPARE(result.entries[0].details,QString("Sala 100%"));
    }
    void jsonFunctionsAvailable() {
        QTemporaryDir dir; LocalDatabase db(dir.filePath("json.sqlite")); QSqlQuery q(db.connection());
        QVERIFY(q.exec("SELECT json_extract('{\"a\":{\"de\":1}}','$.a.de')")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),1);
    }
    void personByIdReachesV5EventsWithoutLoanId() {
        QTemporaryDir dir; const auto path=dir.filePath("v5.sqlite"); QVERIFY(LegacyFixtures::createV5(path));
        LocalDatabase db(path); HistoryQueryService service(db); HistoryFilter f; f.period=PeriodPreset::All; f.personId=1;
        const auto p=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); QCOMPARE(p.entries.size(),1);
        QCOMPARE(p.entries[0].responsibleName,QString("Ana Souza")); QCOMPARE(p.entries[0].responsibleSubject,QString::fromUtf8("Matemática"));
        f.personId.reset(); f.locationId=1; const auto locations=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC"));
        bool found=false; for (const auto &e : locations.entries) if (e.action=="Retirada" && e.responsibleName=="Ana Souza") found=true; QVERIFY(found);
    }
    void everyWrittenActionHasLabel() {
        const QStringList actions={"Retirada","Devolução","Localização","Manutenção","Disponibilização","Cadastro","Notebook atualizado","Notebook inativado","Notebook reativado","Pessoa cadastrada","Pessoa atualizada","Pessoa inativada","Local importado","Local cadastrado","Local atualizado","Local excluído","Conta principal criada","Conta criada","Senha definida","Senha redefinida","Conta inativada","Administrador criado","Senha de administrador redefinida","Administrador inativado","Recuperação vinculada","Recuperação bloqueada temporariamente","Tentativa de recuperação recusada","Conta principal recuperada","Atualização do banco"};
        for (const QString &action : actions)
            QVERIFY(eventGroup(action)!=QString::fromUtf8("Outros registros"));
        QCOMPARE(eventGroup("desconhecido"),QString("Outros registros")); QCOMPARE(eventLabel("desconhecido"),QString("desconhecido"));
    }
    void personIdAndTextRejected() {
        QTemporaryDir dir; LocalDatabase db(dir.filePath("history.sqlite")); HistoryQueryService service(db);
        HistoryFilter f; f.period=PeriodPreset::All; f.personId=1; f.personText="Ana";
        QVERIFY_EXCEPTION_THROWN(service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")),std::invalid_argument);
    }
    void personByIdMatchesCheckoutOrReturn() {
        QTemporaryDir dir; TestContext c(dir.filePath("history.sqlite"));
        const auto n=Fixture::add(c,c.session,"NB-1","Notebook","Sala");
        const auto a=createPerson(c,"Ana",ResponsibleRole::Professor,"Matemática");
        const auto b=createPerson(c,"Ana",ResponsibleRole::Assistant);
        const auto loc=Fixture::location(c,"Sala"); c.store.checkout(c.session,n,a,loc); c.store.checkin(c.session,n,b,loc);
        HistoryQueryService service(c.database); HistoryFilter f; f.period=PeriodPreset::All; f.personId=a;
        HistoryPage p; try { p=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); } catch (const std::exception &e) { QFAIL(e.what()); } QCOMPARE(p.entries.size(),1); QCOMPARE(p.entries[0].action,QString("Retirada"));
        f.personId=b; p=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); QCOMPARE(p.entries.size(),1); QCOMPARE(p.entries[0].action,QString("Devolução"));
    }
    void personByTextFoldsCaseAndAccents() {
        QTemporaryDir dir; TestContext c(dir.filePath("history.sqlite"));
        const auto n=Fixture::add(c,c.session,"NB-1","Notebook","Sala");
        Fixture::checkout(c,c.session,n,QString::fromUtf8("José Lima"),ResponsibleRole::Professor,"Sala");
        Fixture::checkin(c,c.session,n,QString::fromUtf8("JOSÉ LIMA"),ResponsibleRole::Assistant,"Sala");
        QSqlQuery q(c.database.connection());
        QVERIFY(q.exec(QString("INSERT INTO loans(notebook_id,borrower,checked_out_at,returned_at,returned_by) VALUES(%1,'José Lima','2025-01-01T10:00:00.000Z','2025-01-01T11:00:00.000Z','JOSÉ LIMA')").arg(n)));
        QVERIFY(q.exec(QString("INSERT INTO events(entity_type,entity_id,notebook_id,action,details,happened_at) VALUES('notebook',%1,%1,'Retirada','','2025-01-01T10:00:00.000Z'),('notebook',%1,%1,'Devolução','','2025-01-01T11:00:00.000Z')").arg(n)));
        HistoryFilter f; f.period=PeriodPreset::All; f.personText="jose";
        HistoryQueryService service(c.database); HistoryPage p; try { p=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); } catch (const std::exception &e) { QFAIL(e.what()); } QCOMPARE(p.entries.size(),4);
        f.personText="zzz"; QVERIFY(service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")).entries.isEmpty());
    }
    void personByTextNoMatchIsEmpty() {
        QTemporaryDir dir; TestContext c(dir.filePath("history.sqlite"));
        const auto loc=createLocation(c,"Sala"), person=createPerson(c,"Ana",ResponsibleRole::Professor);
        const auto n=c.store.add(c.session,"NB-1","Notebook",loc); c.store.checkout(c.session,n,person,loc);
        HistoryFilter f; f.period=PeriodPreset::All; f.personText="zzz"; HistoryQueryService service(c.database);
        const auto page=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC"));
        QVERIFY(page.entries.isEmpty()); QVERIFY(!page.truncated);
    }
    void filterByEventType() {
        QTemporaryDir dir; TestContext c(dir.filePath("history.sqlite"));
        const auto sala=createLocation(c,"Sala"), biblioteca=createLocation(c,"Biblioteca");
        const auto n=c.store.add(c.session,"NB-1","Notebook",sala); c.store.updateLocation(c.session,n,biblioteca);
        c.store.setMaintenance(c.session,n,true);
        HistoryFilter f; f.period=PeriodPreset::All; f.action=QString::fromUtf8("Localização"); HistoryQueryService service(c.database);
        const auto page=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); QCOMPARE(page.entries.size(),1);
        QCOMPARE(page.entries.first().action,f.action);
        QCOMPARE(eventLabel(page.entries.first().action),QString::fromUtf8("Atualização de local"));
    }
    void filterByAccountAndEventType() {
        QTemporaryDir dir; TestContext c(dir.filePath("history.sqlite")); const auto n=Fixture::add(c,c.session,"NB-1","Notebook","Sala");
        Fixture::updateLocation(c,c.session,n,"Biblioteca"); HistoryQueryService service(c.database);
        HistoryFilter f; f.period=PeriodPreset::All; f.accountId=c.session.accountId(); f.action=QString::fromUtf8("Localização");
        HistoryPage p; try { p=service.search(f,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")); } catch (const std::exception &e) { QFAIL(e.what()); } QCOMPARE(p.entries.size(),1); QCOMPARE(p.entries[0].action,f.action);
    }
    void periodPresetsUtcBoundaries() {
        const QTimeZone zone("America/Sao_Paulo");
        if (!zone.isValid()) QSKIP("Dados do fuso America/Sao_Paulo indisponíveis");
        const auto now=QDateTime::fromString("2026-10-22T01:30:00.000Z",Qt::ISODateWithMs);
        for (const auto &item : {qMakePair(PeriodPreset::Today,QString("2026-10-21T03:00:00.000Z")),qMakePair(PeriodPreset::Last7Days,QString("2026-10-15T03:00:00.000Z")),qMakePair(PeriodPreset::Last30Days,QString("2026-09-22T03:00:00.000Z"))}) {
            const auto range=resolvePeriod(item.first,{}, {},now,zone);
            QCOMPARE(range.startUtc->toString(Qt::ISODateWithMs),item.second);
            QCOMPARE(range.endUtc->toString(Qt::ISODateWithMs),QString("2026-10-22T03:00:00.000Z"));
        }
        const auto all=resolvePeriod(PeriodPreset::All,{}, {},now,zone); QVERIFY(!all.startUtc && !all.endUtc);
    }
    void periodCustomInclusive() {
        const QTimeZone zone("America/Sao_Paulo"); if (!zone.isValid()) QSKIP("Fuso indisponível");
        const auto range=resolvePeriod(PeriodPreset::Custom,QDate(2026,10,1),QDate(2026,10,3),QDateTime::currentDateTimeUtc(),zone);
        QCOMPARE(range.startUtc->toString(Qt::ISODateWithMs),QString("2026-10-01T03:00:00.000Z"));
        QCOMPARE(range.endUtc->toString(Qt::ISODateWithMs),QString("2026-10-04T03:00:00.000Z"));
        QVERIFY_EXCEPTION_THROWN(resolvePeriod(PeriodPreset::Custom,QDate(2026,10,3),QDate(2026,10,1),QDateTime::currentDateTimeUtc(),zone),std::invalid_argument);
    }
    void periodDstDays() {
        const QTimeZone zone("America/New_York"); if (!zone.isValid()) QSKIP("Fuso indisponível");
        for (const auto &item : {qMakePair(QDate(2026,3,8),23),qMakePair(QDate(2026,11,1),25)}) {
            const auto range=resolvePeriod(PeriodPreset::Custom,item.first,item.first,QDateTime::currentDateTimeUtc(),zone);
            QCOMPARE(range.startUtc->secsTo(*range.endUtc),qint64(item.second*3600));
        }
    }
    void periodBoundaryMilliseconds() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); LocalDatabase db(dir.filePath("history.sqlite"));
        db.transaction([&] { db.appendEvent({"system",std::nullopt,std::nullopt,"Antes","",std::nullopt,{},"2026-10-21T02:59:59.999Z"}); db.appendEvent({"system",std::nullopt,std::nullopt,"Dentro","",std::nullopt,{},"2026-10-21T03:00:00.000Z"}); });
        const QTimeZone zone("America/Sao_Paulo"); if (!zone.isValid()) QSKIP("Fuso indisponível");
        HistoryQueryService service(db); const auto page=service.search({},QDateTime::fromString("2026-10-22T01:30:00.000Z",Qt::ISODateWithMs),zone);
        QCOMPARE(page.entries.size(),1); QCOMPARE(page.entries.first().action,QString("Dentro"));
    }
    void filterByNotebookIncludesInactiveAndRenamed() {
        QTemporaryDir dir; TestContext ctx(dir.filePath("filter.sqlite"));
        const auto target=Fixture::add(ctx,ctx.session,"NB-001","Original","Sala");
        Fixture::add(ctx,ctx.session,"NB-002","Outro","Sala");
        const auto notebooks=ctx.store.notebooks();
        const auto it=std::find_if(notebooks.cbegin(),notebooks.cend(),[target](const Notebook &n) { return n.id==target; });
        QVERIFY(it != notebooks.cend()); const auto original=*it;
        ctx.store.update(ctx.session,original,"NB-001","Renomeado",std::nullopt);
        ctx.store.inactivate(ctx.session,original.id);
        HistoryQueryService service(ctx.database); const auto options=service.filterOptions();
        bool found=false; for (const auto &option : options.notebooks) if (option.id==original.id) { found=true; QVERIFY(option.inactive); QVERIFY(option.label.endsWith(" (inativo)")); }
        QVERIFY(found); HistoryFilter filter; filter.period=PeriodPreset::All; filter.notebookId=original.id;
        const auto entries=service.search(filter,QDateTime::currentDateTimeUtc(),QTimeZone("UTC")).entries;
        QVERIFY(entries.size() >= 3); bool old=false; for (const auto &entry : entries) { QVERIFY(entry.assetTag != "NB-002"); old |= entry.notebookName == "Original"; } QVERIFY(old);
    }
};
QTEST_GUILESS_MAIN(HistoryQueryTests)
#include "HistoryQueryTests.moc"

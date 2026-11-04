#include "DemoSeed.h"
#include <stdexcept>
#include <QStringList>

namespace {
const QString DemoPrincipal = QStringLiteral("demo.principal");
const QByteArray DemoPassword = QByteArrayLiteral("demonstracao-gestaoequipamentos");
const QString DemoOperator = QStringLiteral("demo.operador");
const QByteArray DemoOperatorPassword = QByteArrayLiteral("operador-demonstracao-gestaoequipamentos");
}

Session seedDemo(AuthService &auth, NotebookStore &store, AccountService &accounts, DataProfile profile, LocationService &locations, PeopleService &people) {
    if (profile != DataProfile::Demo) throw std::runtime_error("Dados de demonstração só podem ser criados no banco de demonstração.");
    if (!auth.hasPrincipal()) auth.bootstrapPrincipal(DemoPrincipal, auth.preparePassword(DemoPassword));
    Session session = auth.login(DemoPrincipal, DemoPassword);
    if (people.people(CatalogFilter::All).isEmpty()) {
        people.create(session,{"Ana Souza",ResponsibleRole::Professor,"Matemática"});
        people.create(session,{"Beatriz Rocha",ResponsibleRole::Professor,{}});
        people.create(session,{"Carlos Lima",ResponsibleRole::Assistant,{}});
    }
    if (locations.locations(CatalogFilter::All).isEmpty())
        for (const auto &name : {"Sala 12","Biblioteca","Laboratório","Coordenação","Armário 1"}) locations.create(session,name);
    // Fase 3: local da manutenção; também acrescentado a perfis demo criados antes.
    auto locationId = [&](const QString &name) { for (const auto &place : locations.locations(CatalogFilter::All)) if (place.name == name) return place.id; return qint64(0); };
    if (!locationId("Suporte Técnico")) locations.create(session,"Suporte Técnico");
    // Pessoas por nome e função (o seed é idempotente e só cria cada uma uma vez).
    auto personId = [&](const QString &name) { for (const auto &person : people.activeHomonyms(name)) return person.id; return qint64(0); };
    if (store.notebooks().isEmpty()) {
        // Fase 3: tudo por IDs de cadastro (03-PD-01); um notebook por guia.
        const auto armario = locationId("Armário 1"), sala = locationId("Sala 12"), laboratorio = locationId("Laboratório");
        store.add(session, "NB-001", "Dell Latitude 5420", armario);
        const auto lenovo = store.add(session, "NB-002", "Lenovo ThinkPad E14", armario);
        const auto hp = store.add(session, "NB-003", "HP ProBook 440", locationId("Suporte Técnico"));
        const auto inspiron = store.add(session, "NB-004", "Dell Inspiron 15", armario);
        if (const auto beatriz = personId("Beatriz Rocha")) store.checkout(session, inspiron, beatriz, laboratorio);
        store.setMaintenance(session, hp, true, "Bateria não carrega");
        if (const auto carlos = personId("Carlos Lima"), beatriz = personId("Beatriz Rocha"); carlos && beatriz) {
            store.checkout(session, lenovo, carlos, sala);
            store.checkin(session, lenovo, beatriz, armario); // devolução por outra pessoa (D-04)
        }
    }
    // Retirada por pessoa cadastrada (snapshot com matéria) uma única vez: NB-001 nunca emprestado (também em perfis demo antigos).
    qint64 ana = 0;
    for (const auto &person : people.activeHomonyms("Ana Souza")) if (person.role == ResponsibleRole::Professor && !ana) ana = person.id;
    for (const auto &notebook : store.notebooks())
        if (ana && notebook.assetTag == "NB-001" && notebook.active && notebook.status == Status::Available && notebook.borrower.isEmpty())
            store.checkout(session, notebook.id, ana, locationId("Sala 12"));
    bool hasOperator = false;
    for (const auto &account : accounts.commonAccounts(session, AccountFilter::All, DemoOperator)) hasOperator = hasOperator || account.username == DemoOperator;
    if (!hasOperator) accounts.createCommonAccount(session, DemoOperator, auth.preparePassword(DemoOperatorPassword), InitialPasswordMode::Assisted);
    // Fase 4: variedade para os filtros de Logs (homônimo, pessoa, local e notebook inativos, movimentação por conta comum), uma única vez.
    bool hasPositivo = false;
    for (const auto &notebook : store.notebooks()) hasPositivo = hasPositivo || notebook.assetTag == "NB-005";
    if (const auto armario = locationId("Armário 1"); !hasPositivo && armario) {
        qint64 anaAssistant = 0;
        for (const auto &person : people.activeHomonyms("Ana Souza")) if (person.role == ResponsibleRole::Assistant && !anaAssistant) anaAssistant = person.id;
        if (!anaAssistant) anaAssistant = people.create(session,{"Ana Souza",ResponsibleRole::Assistant,{}});
        const auto daniel = people.create(session,{"Daniel Costa",ResponsibleRole::Professor,"Ciências"});
        const auto sala7 = locations.create(session,"Sala 7");
        const auto positivo = store.add(session, "NB-005", "Positivo Master N", armario);
        const Session operador = auth.login(DemoOperator, DemoOperatorPassword);
        store.checkout(operador, positivo, anaAssistant, sala7);
        store.checkin(operador, positivo, daniel, armario);
        session = auth.login(DemoPrincipal, DemoPassword);
        locations.softDelete(session, sala7);
        people.inactivate(session, daniel);
        store.inactivate(session, positivo);
    }
    return session;
}

Session seedDemoDataset150(AuthService &auth, NotebookStore &store, AccountService &accounts, DataProfile profile, LocationService &locations, PeopleService &people) {
    if (profile != DataProfile::Demo) throw std::runtime_error("Dados de demonstração só podem ser criados no banco de demonstração.");
    const auto session = seedDemo(auth,store,accounts,profile,locations,people);

    auto professors = people.people(CatalogFilter::All);
    int professorCount = 0, assistantCount = 0;
    for (const auto &person : professors) {
        if (person.role == ResponsibleRole::Professor) ++professorCount;
        else if (person.role == ResponsibleRole::Assistant) ++assistantCount;
    }
    if (professorCount>25 || assistantCount>25 || locations.locations(CatalogFilter::All).size()>50 || store.notebooks().size()>50)
        throw std::runtime_error("O banco demo já excede um dos limites da carga de 150 dados; nenhum cadastro foi removido.");
    const QStringList subjects{QStringLiteral("Matemática"),QStringLiteral("Português"),QStringLiteral("Ciências"),QStringLiteral("História"),QStringLiteral("Geografia")};
    for (int i=professorCount; i<25; ++i)
        people.create(session,{QStringLiteral("Professor de Teste %1").arg(i+1,2,10,QChar('0')),ResponsibleRole::Professor,subjects.at(i%subjects.size())});
    for (int i=assistantCount; i<25; ++i)
        people.create(session,{QStringLiteral("Auxiliar de Teste %1").arg(i+1,2,10,QChar('0')),ResponsibleRole::Assistant,{}});

    auto places = locations.locations(CatalogFilter::All);
    for (int i=places.size(); i<50; ++i)
        locations.create(session,QStringLiteral("Local de Teste %1").arg(i+1,2,10,QChar('0')));
    places = locations.locations(CatalogFilter::Active);
    if (places.isEmpty()) throw std::runtime_error("A carga de demonstração precisa de ao menos um local ativo.");

    auto equipment = store.notebooks();
    for (int i=equipment.size(); i<50; ++i) {
        const auto number=i+1;
        store.add(session,QStringLiteral("TEST-NB-%1").arg(number,3,10,QChar('0')),
                  QStringLiteral("Notebook de Teste %1").arg(number,2,10,QChar('0')),
                  places.at(i%places.size()).id);
    }
    return session;
}

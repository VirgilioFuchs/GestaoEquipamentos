#include "PeopleService.h"
#include <QSqlError>
#include <QSqlQuery>

namespace {
void run(QSqlQuery &query) {
    if (!query.exec()) throw std::runtime_error(query.lastError().text().toStdString());
}
QString roleValue(ResponsibleRole role) { return role == ResponsibleRole::Professor ? QStringLiteral("professor") : QStringLiteral("assistant"); }
PersonDraft validated(const PersonDraft &draft) {
    PersonDraft result{draft.name.trimmed(),draft.role,{}};
    if (result.name.isEmpty()) throw std::runtime_error("Nome é obrigatório.");
    if (result.name.size()>120) throw std::runtime_error("Nome excede o limite de caracteres.");
    if (result.role!=ResponsibleRole::Professor && result.role!=ResponsibleRole::Assistant) throw std::runtime_error("Selecione Professor ou Auxiliar.");
    if (result.role==ResponsibleRole::Professor) {
        result.subject=draft.subject.trimmed();
        if (result.subject.size()>120) throw std::runtime_error("Matéria excede o limite de caracteres.");
    }
    return result;
}
QJsonValue subjectValue(const QString &subject) { return subject.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(subject); }
}

PeopleService::PeopleService(LocalDatabase &database, AuthService &auth) : database(database), auth(auth) {}

QVector<Person> PeopleService::people(CatalogFilter filter, const QString &search) const {
    QString sql="SELECT id,name,role,subject,active FROM people WHERE 1=1";
    if (filter==CatalogFilter::Active) sql+=" AND active=1";
    if (filter==CatalogFilter::Inactive) sql+=" AND active=0";
    QSqlQuery query(database.connection()); query.prepare(sql+" ORDER BY name_key,id"); run(query);
    QVector<Person> rows; const auto needle=LocalDatabase::catalogKey(search);
    while (query.next()) {
        Person person{query.value(0).toLongLong(),query.value(1).toString(),query.value(2).toString()=="professor" ? ResponsibleRole::Professor : ResponsibleRole::Assistant,query.value(3).toString(),query.value(4).toBool()};
        if (needle.isEmpty() || needle==QString::number(person.id) || LocalDatabase::catalogKey(person.name+" "+roleLabel(person.role)+" "+person.subject).contains(needle)) rows.append(person);
    }
    return rows;
}
QVector<Person> PeopleService::activePeople() const { return people(CatalogFilter::Active); }
QVector<Person> PeopleService::activeHomonyms(const QString &name) const {
    QSqlQuery query(database.connection());
    query.prepare("SELECT id,name,role,subject,active FROM people WHERE active=1 AND name_key=? ORDER BY id");
    query.addBindValue(LocalDatabase::catalogKey(name)); run(query); QVector<Person> rows;
    while (query.next()) rows.append({query.value(0).toLongLong(),query.value(1).toString(),query.value(2).toString()=="professor" ? ResponsibleRole::Professor : ResponsibleRole::Assistant,query.value(3).toString(),true});
    return rows;
}

void PeopleService::update(const Session &session, const Person &expected, const PersonDraft &draft) {
    const auto clean=validated(draft);
    database.transaction([&] {
        const Actor actor=auth.authorize(session, Permission::OperateInventory);
        QSqlQuery query(database.connection()); query.prepare("SELECT name,role,subject,active FROM people WHERE id=?");
        query.addBindValue(expected.id); run(query);
        if (!query.next()) throw std::runtime_error("Pessoa não encontrada.");
        if (!query.value(3).toBool()) throw std::runtime_error("Cadastros inativos não podem ser editados.");
        const auto previousName=query.value(0).toString(), previousRole=query.value(1).toString(), previousSubject=query.value(2).toString(); query.finish();
        if (previousName!=expected.name || previousRole!=roleValue(expected.role) || previousSubject!=expected.subject)
            throw std::runtime_error("Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente.");
        QJsonObject changes;
        if (previousName!=clean.name) changes.insert("name",LocalDatabase::change("name",previousName,clean.name).value("name"));
        if (previousRole!=roleValue(clean.role)) changes.insert("person_role",LocalDatabase::change("person_role",previousRole,roleValue(clean.role)).value("person_role"));
        if (previousSubject!=clean.subject) changes.insert("subject",LocalDatabase::change("subject",subjectValue(previousSubject),subjectValue(clean.subject)).value("subject"));
        if (changes.isEmpty()) throw std::runtime_error("Nenhuma alteração para salvar.");
        query.prepare("UPDATE people SET name=?,name_key=?,role=?,subject=? WHERE id=? AND active=1");
        query.addBindValue(clean.name); query.addBindValue(LocalDatabase::catalogKey(clean.name)); query.addBindValue(roleValue(clean.role));
        query.addBindValue(clean.subject.isEmpty() ? QVariant() : QVariant(clean.subject)); query.addBindValue(expected.id); run(query);
        if (query.numRowsAffected()!=1) throw std::runtime_error("Cadastros inativos não podem ser editados.");
        database.appendEvent({"person",expected.id,std::nullopt,"Pessoa atualizada","Cadastro de pessoa atualizado: "+clean.name+".",actor,changes,LocalDatabase::nowUtc()});
    });
}

void PeopleService::inactivate(const Session &session, qint64 personId) {
    database.transaction([&] {
        const Actor actor=auth.authorize(session, Permission::InactivateCatalog);
        QSqlQuery query(database.connection()); query.prepare("SELECT name,role FROM people WHERE id=?"); query.addBindValue(personId); run(query);
        if (!query.next()) throw std::runtime_error("Pessoa não encontrada.");
        const auto name=query.value(0).toString(); const auto role=query.value(1).toString()=="professor" ? ResponsibleRole::Professor : ResponsibleRole::Assistant; query.finish();
        query.prepare("UPDATE people SET active=0 WHERE id=? AND active=1"); query.addBindValue(personId); run(query);
        if (query.numRowsAffected()!=1) throw std::runtime_error("Esta pessoa já está inativa.");
        database.appendEvent({"person",personId,std::nullopt,"Pessoa inativada",QString("Pessoa inativada: %1 (%2).").arg(name,roleLabel(role)),actor,LocalDatabase::change("active",1,0),LocalDatabase::nowUtc()});
    });
}

qint64 PeopleService::create(const Session &session, const PersonDraft &draft) {
    const auto clean=validated(draft); qint64 id=0;
    database.transaction([&] {
        const Actor actor=auth.authorize(session, Permission::OperateInventory);
        const auto now=LocalDatabase::nowUtc(); QSqlQuery query(database.connection());
        query.prepare("INSERT INTO people(name,name_key,role,subject,active,created_at) VALUES(?,?,?,?,1,?)");
        query.addBindValue(clean.name); query.addBindValue(LocalDatabase::catalogKey(clean.name)); query.addBindValue(roleValue(clean.role));
        query.addBindValue(clean.subject.isEmpty() ? QVariant() : QVariant(clean.subject)); query.addBindValue(now); run(query); id=query.lastInsertId().toLongLong();
        auto changes=LocalDatabase::change("name",QJsonValue::Null,clean.name);
        changes.insert("person_role",LocalDatabase::change("person_role",QJsonValue::Null,roleValue(clean.role)).value("person_role"));
        changes.insert("active",LocalDatabase::change("active",QJsonValue::Null,1).value("active"));
        if (!clean.subject.isEmpty()) changes.insert("subject",LocalDatabase::change("subject",QJsonValue::Null,subjectValue(clean.subject)).value("subject"));
        database.appendEvent({"person",id,std::nullopt,"Pessoa cadastrada",QString("Pessoa cadastrada: %1 (%2).").arg(clean.name,roleLabel(clean.role)+(clean.subject.isEmpty() ? QString() : " · "+clean.subject)),actor,changes,now});
    });
    return id;
}

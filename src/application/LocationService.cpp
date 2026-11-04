#include "LocationService.h"
#include <QSqlError>
#include <QSqlQuery>

namespace {
const QString Duplicate = QStringLiteral("Já existe um local ativo com esse nome.");
void run(QSqlQuery &query) {
    if (!query.exec()) throw std::runtime_error(query.lastError().text().toStdString());
}
}

LocationService::LocationService(LocalDatabase &database, AuthService &auth) : database(database), auth(auth) {}

QVector<Location> LocationService::locations(CatalogFilter filter, const QString &search) const {
    QString statement = "SELECT l.id,l.name,l.deleted_at,(SELECT COUNT(*) FROM notebooks n WHERE n.location_id=l.id) FROM locations l WHERE 1=1";
    if (filter == CatalogFilter::Active) statement += " AND l.deleted_at IS NULL";
    if (filter == CatalogFilter::Inactive) statement += " AND l.deleted_at IS NOT NULL";
    const QString needle = LocalDatabase::catalogKey(search);
    if (!needle.isEmpty()) statement += " AND (instr(l.name_key,?)>0 OR CAST(l.id AS TEXT)=?)";
    QSqlQuery query(database.connection());
    query.prepare(statement + " ORDER BY l.name_key,l.id");
    if (!needle.isEmpty()) { query.addBindValue(needle); query.addBindValue(needle); }
    run(query);
    QVector<Location> rows;
    while (query.next()) rows.append({query.value(0).toLongLong(),query.value(1).toString(),!query.value(2).isNull(),query.value(3).toInt()});
    return rows;
}

QStringList LocationService::activeLocationNames() const {
    QStringList names;
    for (const auto &location : locations(CatalogFilter::Active)) names.append(location.name);
    return names;
}

int LocationService::notebooksAt(qint64 locationId) const {
    QSqlQuery query(database.connection());
    query.prepare("SELECT COUNT(*) FROM notebooks WHERE location_id=?");
    query.addBindValue(locationId); run(query); query.next();
    return query.value(0).toInt();
}

void LocationService::softDelete(const Session &session, qint64 locationId) {
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::OperateInventory);
        QSqlQuery query(database.connection());
        query.prepare("SELECT name,deleted_at FROM locations WHERE id=?");
        query.addBindValue(locationId); run(query);
        if (!query.next()) throw std::runtime_error("Local não encontrado.");
        if (!query.value(1).isNull()) throw std::runtime_error("Este local já foi excluído.");
        const auto name = query.value(0).toString(); query.finish();
        const int count = notebooksAt(locationId);
        const auto now = LocalDatabase::nowUtc();
        query.prepare("UPDATE locations SET deleted_at=? WHERE id=? AND deleted_at IS NULL");
        query.addBindValue(now); query.addBindValue(locationId); run(query);
        if (query.numRowsAffected() != 1) throw std::runtime_error("Este local já foi excluído.");
        database.appendEvent({"location",locationId,std::nullopt,"Local excluído",
                              QString("Local excluído: %1. Notebooks neste local: %2.").arg(name).arg(count),actor,
                              LocalDatabase::change("location_deleted",0,1),now});
    });
}

void LocationService::rename(const Session &session, const Location &expected, const QString &name) {
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty()) throw std::runtime_error("Nome do local é obrigatório.");
    if (trimmed.size() > 120) throw std::runtime_error("Nome do local excede o limite de caracteres.");
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::OperateInventory);
        QSqlQuery query(database.connection()); query.prepare("SELECT name,deleted_at FROM locations WHERE id=?");
        query.addBindValue(expected.id); run(query);
        if (!query.next()) throw std::runtime_error("Local não encontrado.");
        if (!query.value(1).isNull()) throw std::runtime_error("Locais excluídos não podem ser editados.");
        const auto previous = query.value(0).toString(); query.finish();
        if (previous != expected.name) throw std::runtime_error("Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente.");
        if (previous == trimmed) throw std::runtime_error("Nenhuma alteração para salvar.");
        const auto key = LocalDatabase::catalogKey(trimmed);
        query.prepare("SELECT id FROM locations WHERE name_key=? AND deleted_at IS NULL AND id<>?");
        query.addBindValue(key); query.addBindValue(expected.id); run(query);
        if (query.next()) throw std::runtime_error(Duplicate.toStdString());
        query.finish(); query.prepare("UPDATE locations SET name=?,name_key=? WHERE id=? AND deleted_at IS NULL");
        query.addBindValue(trimmed); query.addBindValue(key); query.addBindValue(expected.id);
        if (!query.exec()) {
            if (query.lastError().text().contains("UNIQUE",Qt::CaseInsensitive)) throw std::runtime_error(Duplicate.toStdString());
            throw std::runtime_error(query.lastError().text().toStdString());
        }
        if (query.numRowsAffected()!=1) throw std::runtime_error("Locais excluídos não podem ser editados.");
        database.appendEvent({"location",expected.id,std::nullopt,"Local atualizado","Local atualizado: " + previous + " → " + trimmed + ".",actor,
                              LocalDatabase::change("name",previous,trimmed),LocalDatabase::nowUtc()});
    });
}

qint64 LocationService::create(const Session &session, const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) throw std::runtime_error("Nome do local é obrigatório.");
    if (trimmed.size() > 120) throw std::runtime_error("Nome do local excede o limite de caracteres.");
    qint64 id = 0;
    database.transaction([&] {
        const Actor actor = auth.authorize(session, Permission::OperateInventory);
        const QString key = LocalDatabase::catalogKey(trimmed);
        QSqlQuery existing(database.connection());
        existing.prepare("SELECT id FROM locations WHERE name_key=? AND deleted_at IS NULL");
        existing.addBindValue(key); run(existing);
        if (existing.next()) throw std::runtime_error(Duplicate.toStdString());
        const QString now = LocalDatabase::nowUtc();
        QSqlQuery insert(database.connection());
        insert.prepare("INSERT INTO locations(name,name_key,created_at) VALUES(?,?,?)");
        insert.addBindValue(trimmed); insert.addBindValue(key); insert.addBindValue(now);
        if (!insert.exec()) {
            if (insert.lastError().text().contains("UNIQUE",Qt::CaseInsensitive)) throw std::runtime_error(Duplicate.toStdString());
            throw std::runtime_error(insert.lastError().text().toStdString());
        }
        id = insert.lastInsertId().toLongLong();
        database.appendEvent({"location",id,std::nullopt,"Local cadastrado","Local cadastrado: " + trimmed + ".",actor,
                              LocalDatabase::change("name",QJsonValue::Null,trimmed),now});
    });
    return id;
}

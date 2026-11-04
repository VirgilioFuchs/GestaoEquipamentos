#include "NotebookStore.h"
#include "application/HistoryQueryService.h"
#include <algorithm>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QSqlError>
#include <initializer_list>
#include <stdexcept>

namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
void run(QSqlQuery &q) { if (!q.exec()) fail(q.lastError().text()); }
QString roleValue(ResponsibleRole role) {
    switch (role) {
    case ResponsibleRole::Professor: return "professor";
    case ResponsibleRole::Assistant: return "assistant";
    default: fail("Selecione a função do responsável.");
    }
}
ResponsibleRole readRole(const QString &value) {
    if (value == "professor") return ResponsibleRole::Professor;
    if (value == "assistant") return ResponsibleRole::Assistant;
    return ResponsibleRole::Unknown;
}
QString required(QString value, const QString &field, int maxLength) {
    value = value.trimmed();
    if (value.isEmpty()) fail(field + QStringLiteral(" é obrigatório."));
    if (value.size() > maxLength) fail(field + QStringLiteral(" excede o limite de caracteres."));
    return value;
}
struct ResolvedLocation {
    std::optional<qint64> id;
    QString label;
};
ResolvedLocation storedLocation(const QSqlDatabase &db, qint64 id) {
    QSqlQuery query(db); query.prepare("SELECT n.location_id,COALESCE(l.name,n.location) FROM notebooks n LEFT JOIN locations l ON l.id=n.location_id WHERE n.id=?");
    query.addBindValue(id); run(query);
    if (!query.next()) fail("Notebook não encontrado.");
    return {query.value(0).isNull() ? std::nullopt : std::optional<qint64>(query.value(0).toLongLong()),query.value(1).toString()};
}
const QString DuplicateAssetTag = QStringLiteral("Este patrimônio já está cadastrado. Informe outro patrimônio.");
const QString StaleSelection = QStringLiteral("Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente.");
const QString InactiveFrozen = QStringLiteral("Notebook inativo não pode ser movimentado.");
// Movimentações recusam notebook inativo antes de qualquer outra regra; o trigger notebooks_inactive_frozen é a segunda barreira.
void ensureActive(const QSqlDatabase &db, qint64 id) {
    QSqlQuery q(db); q.prepare("SELECT active FROM notebooks WHERE id=?"); q.addBindValue(id); run(q);
    if (!q.next()) fail("Notebook não encontrado.");
    if (q.value(0).toInt() != 1) fail(InactiveFrozen);
}
QJsonObject merged(std::initializer_list<QJsonObject> parts) {
    QJsonObject all;
    for (const auto &part : parts) for (auto it = part.begin(); it != part.end(); ++it) all.insert(it.key(), it.value());
    return all;
}
QJsonValue jsonLocationId(const ResolvedLocation &location) {
    return location.id ? QJsonValue(*location.id) : QJsonValue(QJsonValue::Null);
}
QJsonObject locationChange(const ResolvedLocation &previous, const ResolvedLocation &place) {
    if (previous.id == place.id && previous.label == place.label) return {};
    return merged({LocalDatabase::change("location",previous.label.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(previous.label),place.label),
                   LocalDatabase::change("location_id",jsonLocationId(previous),jsonLocationId(place))});
}
// Leitura de catálogo dentro da transação (revalida o que o diálogo mostrou).
struct ActiveLocation { qint64 id; QString name; };
ActiveLocation readActiveLocation(const QSqlDatabase &db, qint64 locationId) {
    QSqlQuery q(db); q.prepare("SELECT name,deleted_at FROM locations WHERE id=?"); q.addBindValue(locationId); run(q);
    if (!q.next()) fail("Local não encontrado.");
    const auto name = q.value(0).toString();
    if (!q.value(1).isNull()) fail("O local " + name + " foi excluído. Escolha um local ativo ou cadastre-o novamente em Locais.");
    return {locationId, name};
}
// Sem UPDATE: diferencia notebook inativo de status incompatível depois que o UPDATE condicional não afetou linha.
[[noreturn]] void refuseTransition(const QSqlDatabase &db, qint64 id, const QString &message) {
    QSqlQuery q(db); q.prepare("SELECT active FROM notebooks WHERE id=?"); q.addBindValue(id); run(q);
    if (!q.next()) fail("Notebook não encontrado.");
    if (q.value(0).toInt() != 1) fail(InactiveFrozen);
    fail(message);
}
}
NotebookStore::NotebookStore(LocalDatabase &database, AuthService &auth) : database(database), auth(auth), db(database.connection()) {}
void NotebookStore::audit(const Actor &actor, qint64 id, const QString &action, const QString &details, const QJsonObject &changes, const QString &at) {
    database.appendEvent({"notebook", id, id, action, details, actor, changes, at});
}
qint64 NotebookStore::add(const Session &session, const QString &assetTag, const QString &name, qint64 locationId) {
    const auto tag = required(assetTag, "Patrimônio", 60);
    const auto label = required(name, "Nome do notebook", 120);
    if (locationId <= 0) fail("Selecione o local atual.");
    qint64 id = 0;
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::OperateInventory);
        const auto place = readActiveLocation(db, locationId);
        QSqlQuery q(db); q.prepare("SELECT id FROM notebooks WHERE asset_tag=?"); q.addBindValue(tag); run(q);
        if (q.next()) fail(DuplicateAssetTag);
        q.prepare("INSERT INTO notebooks(asset_tag,name,location,location_id) VALUES(?,?,?,?)"); q.addBindValue(tag); q.addBindValue(label); q.addBindValue(place.name); q.addBindValue(place.id); run(q);
        id = q.lastInsertId().toLongLong();
        const auto changes = merged({LocalDatabase::change("asset_tag", QJsonValue::Null, tag), LocalDatabase::change("name", QJsonValue::Null, label),
                                     LocalDatabase::change("location", QJsonValue::Null, place.name),LocalDatabase::change("location_id",QJsonValue::Null,place.id)});
        EventRecord event{"notebook", id, id, "Cadastro", "Notebook cadastrado: " + label + ". Local: " + place.name, actor, changes, LocalDatabase::nowUtc()};
        event.locationId = place.id;
        database.appendEvent(event);
    });
    return id;
}
// Snapshot de pessoa cadastrada lido dentro da transação da movimentação.
struct NotebookStore::Responsible {
    QString name;
    ResponsibleRole role = ResponsibleRole::Unknown;
    QString subject;
    qint64 personId = 0;
};
namespace {
QVariant optionalId(const std::optional<qint64> &id) { return id ? QVariant::fromValue(*id) : QVariant(QMetaType::fromType<qint64>()); }
QVariant optionalText(const QString &text) { return text.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(text); }
QString responsibleLabel(ResponsibleRole role, const QString &subject) {
    return subject.isEmpty() ? roleLabel(role) : roleLabel(role) + QStringLiteral(" · ") + subject;
}
QJsonObject snapshotChanges(const QString &role, const QString &subject) {
    auto changes = LocalDatabase::change("person_role", QJsonValue::Null, role);
    if (!subject.isEmpty()) changes = merged({changes, LocalDatabase::change("subject", QJsonValue::Null, subject)});
    return changes;
}
}
NotebookStore::Responsible NotebookStore::readActivePerson(qint64 personId) {
    QSqlQuery q(db); q.prepare("SELECT name,role,subject,active FROM people WHERE id=?"); q.addBindValue(personId); run(q);
    if (!q.next()) fail("Pessoa não encontrada.");
    if (q.value(3).toInt() != 1) fail("Esta pessoa está inativa e não pode ser selecionada.");
    return {q.value(0).toString(), readRole(q.value(1).toString()), q.value(2).isNull() ? QString() : q.value(2).toString(), personId};
}
void NotebookStore::checkout(const Session &session, qint64 notebookId, qint64 personId, qint64 locationId) {
    if (personId <= 0) fail("Selecione a pessoa responsável pela retirada.");
    if (locationId <= 0) fail("Selecione o local de uso.");
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::OperateInventory);
        const auto who = readActivePerson(personId);
        const auto place = readActiveLocation(db, locationId);
        const auto origin = storedLocation(db, notebookId); // D-06: lido antes do UPDATE
        QSqlQuery q(db);
        q.prepare("UPDATE notebooks SET status='loaned',location=?,location_id=? WHERE id=? AND status='available' AND active=1");
        q.addBindValue(place.name); q.addBindValue(place.id); q.addBindValue(notebookId); run(q);
        if (q.numRowsAffected() != 1) refuseTransition(db, notebookId, "Somente notebooks disponíveis podem ser retirados. Atualize a seleção.");
        const QString now = LocalDatabase::nowUtc();
        const auto function = roleValue(who.role);
        q.prepare("INSERT INTO loans(notebook_id,borrower,checked_out_at,borrower_role,checkout_location_id,checkout_location,borrower_person_id,borrower_subject,origin_location_id,origin_location) VALUES(?,?,?,?,?,?,?,?,?,?)");
        q.addBindValue(notebookId); q.addBindValue(who.name); q.addBindValue(now); q.addBindValue(function);
        q.addBindValue(place.id); q.addBindValue(place.name); q.addBindValue(who.personId); q.addBindValue(optionalText(who.subject));
        q.addBindValue(optionalId(origin.id)); q.addBindValue(optionalText(origin.label)); run(q);
        const auto loanId = q.lastInsertId().toLongLong();
        const ResolvedLocation target{place.id, place.name};
        EventRecord event{"notebook", notebookId, notebookId, "Retirada",
                          "Retirado por: " + who.name + " (" + responsibleLabel(who.role, who.subject) + "). Local: " + place.name, actor,
                          merged({LocalDatabase::change("status", "available", "loaned"), locationChange(origin, target), snapshotChanges(function, who.subject)}), now};
        event.loanId = loanId; event.locationId = place.id; event.fromLocationId = origin.id;
        database.appendEvent(event);
    });
}
void NotebookStore::checkin(const Session &session, qint64 notebookId, qint64 personId, qint64 locationId) {
    if (personId <= 0) fail("Selecione a pessoa responsável pela devolução.");
    if (locationId <= 0) fail("Selecione o local após a devolução.");
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::OperateInventory);
        const auto who = readActivePerson(personId);
        const auto place = readActiveLocation(db, locationId);
        QSqlQuery q(db); q.prepare("SELECT status,active FROM notebooks WHERE id=?"); q.addBindValue(notebookId); run(q);
        if (!q.next()) fail("Notebook não encontrado.");
        if (q.value(1).toInt() != 1) fail(InactiveFrozen);
        const QString NotLoaned = QStringLiteral("Somente notebooks emprestados podem ser devolvidos. Atualize a seleção.");
        if (q.value(0).toString() != "loaned") fail(NotLoaned);
        q.prepare("SELECT id FROM loans WHERE notebook_id=? AND returned_at IS NULL"); q.addBindValue(notebookId); run(q);
        if (!q.next()) fail(NotLoaned);
        const auto loanId = q.value(0).toLongLong();
        if (q.next()) fail(NotLoaned);
        const auto previous = storedLocation(db, notebookId);
        const QString now = LocalDatabase::nowUtc(); // R1: o mesmo instante no loan e no evento
        const auto function = roleValue(who.role);
        q.prepare("UPDATE loans SET returned_at=?,returned_by=?,returned_by_role=?,returned_by_person_id=?,returned_by_subject=?,return_location_id=?,return_location=? WHERE id=? AND returned_at IS NULL");
        q.addBindValue(now); q.addBindValue(who.name); q.addBindValue(function); q.addBindValue(who.personId); q.addBindValue(optionalText(who.subject));
        q.addBindValue(place.id); q.addBindValue(place.name); q.addBindValue(loanId); run(q);
        if (q.numRowsAffected() != 1) fail(NotLoaned);
        q.prepare("UPDATE notebooks SET status='available',location=?,location_id=? WHERE id=? AND status='loaned' AND active=1");
        q.addBindValue(place.name); q.addBindValue(place.id); q.addBindValue(notebookId); run(q);
        if (q.numRowsAffected() != 1) refuseTransition(db, notebookId, NotLoaned);
        const ResolvedLocation target{place.id, place.name};
        EventRecord event{"notebook", notebookId, notebookId, "Devolução",
                          "Devolvido por: " + who.name + " (" + responsibleLabel(who.role, who.subject) + "). Local: " + place.name, actor,
                          merged({LocalDatabase::change("status", "loaned", "available"), locationChange(previous, target), snapshotChanges(function, who.subject)}), now};
        event.loanId = loanId; event.locationId = place.id; event.fromLocationId = previous.id;
        database.appendEvent(event);
    });
}
std::optional<qint64> NotebookStore::suggestedReturnLocation(qint64 notebookId) const {
    QSqlQuery q(db);
    q.prepare("SELECT l.origin_location_id FROM loans l JOIN notebooks n ON n.id=l.notebook_id JOIN locations p ON p.id=l.origin_location_id "
              "WHERE l.notebook_id=? AND l.returned_at IS NULL AND n.status='loaned' AND p.deleted_at IS NULL");
    q.addBindValue(notebookId); run(q);
    if (!q.next()) return std::nullopt;
    return q.value(0).toLongLong();
}
void NotebookStore::updateLocation(const Session &session, qint64 notebookId, qint64 locationId) {
    if (locationId <= 0) fail("Selecione o novo local.");
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::OperateInventory);
        const auto place = readActiveLocation(db, locationId);
        QSqlQuery q(db); q.prepare("SELECT status,active FROM notebooks WHERE id=?"); q.addBindValue(notebookId); run(q);
        if (!q.next()) fail("Notebook não encontrado.");
        if (q.value(1).toInt() != 1) fail(InactiveFrozen);
        if (q.value(0).toString() == "maintenance") fail("Conclua a manutenção antes de atualizar o local.");
        const auto previous = storedLocation(db, notebookId);
        if (previous.id == place.id) fail("Informe um local diferente do atual.");
        q.prepare("UPDATE notebooks SET location=?,location_id=? WHERE id=? AND active=1 AND status IN ('available','loaned')");
        q.addBindValue(place.name); q.addBindValue(place.id); q.addBindValue(notebookId); run(q);
        if (q.numRowsAffected() != 1) refuseTransition(db, notebookId, "Conclua a manutenção antes de atualizar o local.");
        const ResolvedLocation target{place.id, place.name};
        EventRecord event{"notebook", notebookId, notebookId, "Localização",
                          "Local alterado: " + (previous.label.isEmpty() ? QStringLiteral("Não informado") : previous.label) + " → " + place.name,
                          actor, locationChange(previous, target), LocalDatabase::nowUtc()};
        event.locationId = place.id; event.fromLocationId = previous.id;
        database.appendEvent(event);
    });
}
void NotebookStore::setMaintenance(const Session &session, qint64 id, bool enabled, const QString &note) {
    const QString text = note.trimmed();
    if (!enabled && !text.isEmpty()) fail("A observação só pode ser informada ao enviar à manutenção.");
    if (text.size() > 200) fail("Observação excede o limite de caracteres.");
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::OperateInventory);
        ensureActive(db, id);
        const QString from = enabled ? "available" : "maintenance", to = enabled ? "maintenance" : "available";
        QSqlQuery q(db); q.prepare("UPDATE notebooks SET status=? WHERE id=? AND active = 1 AND status=?");
        q.addBindValue(to); q.addBindValue(id); q.addBindValue(from); run(q);
        if (q.numRowsAffected() != 1) fail(enabled ? "Transição inválida. Devolva o aparelho antes de colocá-lo em manutenção." : "Este notebook não está em manutenção.");
        auto changes = LocalDatabase::change("status", from, to);
        if (!text.isEmpty()) changes = merged({changes, LocalDatabase::change("maintenance_note", QJsonValue::Null, text)});
        audit(actor, id, enabled ? "Manutenção" : "Disponibilização", enabled ? "Enviado para manutenção" : "Retornou da manutenção",
              changes, LocalDatabase::nowUtc());
    });
}
void NotebookStore::update(const Session &session, const Notebook &expected, const QString &assetTag, const QString &name, std::optional<qint64> locationId) {
    const auto tag = required(assetTag, "Patrimônio", 60);
    const auto label = required(name, "Nome do notebook", 120);
    if (locationId && *locationId <= 0) fail("Selecione o local atual.");
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::OperateInventory);
        QSqlQuery q(db);
        q.prepare("SELECT n.asset_tag,n.name,COALESCE(l.name,n.location,''),n.location_id,n.active FROM notebooks n LEFT JOIN locations l ON l.id=n.location_id WHERE n.id=?");
        q.addBindValue(expected.id); run(q);
        if (!q.next()) fail("Notebook não encontrado.");
        const auto currentTag = q.value(0).toString(), currentName = q.value(1).toString(), currentPlace = q.value(2).toString();
        const ResolvedLocation previous{q.value(3).isNull() ? std::nullopt : std::optional<qint64>(q.value(3).toLongLong()), currentPlace};
        if (q.value(4).toInt() != 1) fail("Cadastros inativos não podem ser editados.");
        if (currentTag != expected.assetTag || currentName != expected.name || currentPlace != expected.location) fail(StaleSelection);
        // D-07: nullopt (ou o mesmo id) mantém location/location_id, inclusive local excluído ou texto legado.
        std::optional<ActiveLocation> place;
        if (locationId && previous.id != *locationId) place = readActiveLocation(db, *locationId);
        q.prepare("SELECT id FROM notebooks WHERE asset_tag=? AND id<>?"); q.addBindValue(tag); q.addBindValue(expected.id); run(q);
        if (q.next()) fail(DuplicateAssetTag);
        QJsonObject changes;
        if (tag != currentTag) changes = merged({changes, LocalDatabase::change("asset_tag", currentTag, tag)});
        if (label != currentName) changes = merged({changes, LocalDatabase::change("name", currentName, label)});
        if (place) changes = merged({changes, locationChange(previous, {place->id, place->name})});
        if (changes.isEmpty()) fail("Nenhuma alteração para salvar.");
        if (!place) {
            q.prepare("UPDATE notebooks SET asset_tag=?,name=? WHERE id=? AND active = 1");
            q.addBindValue(tag); q.addBindValue(label); q.addBindValue(expected.id);
        } else {
            q.prepare("UPDATE notebooks SET asset_tag=?,name=?,location=?,location_id=? WHERE id=? AND active = 1");
            q.addBindValue(tag); q.addBindValue(label); q.addBindValue(place->name); q.addBindValue(place->id); q.addBindValue(expected.id);
        }
        if (!q.exec()) {
            if (q.lastError().text().contains("UNIQUE", Qt::CaseInsensitive)) fail(DuplicateAssetTag);
            fail(q.lastError().text());
        }
        if (q.numRowsAffected() != 1) fail(StaleSelection);
        EventRecord event{"notebook", expected.id, expected.id, "Notebook atualizado", "Cadastro do notebook atualizado: " + tag + " · " + label + ".", actor, changes, LocalDatabase::nowUtc()};
        if (place) { event.locationId = place->id; event.fromLocationId = previous.id; }
        database.appendEvent(event);
    });
}
void NotebookStore::inactivate(const Session &session, qint64 notebookId) {
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::InactivateCatalog);
        QSqlQuery q(db); q.prepare("UPDATE notebooks SET active = 0 WHERE id = ? AND active = 1 AND status = 'available'");
        q.addBindValue(notebookId); run(q);
        if (q.numRowsAffected() != 1) {
            q.prepare("SELECT active FROM notebooks WHERE id=?"); q.addBindValue(notebookId); run(q);
            if (!q.next()) fail("Notebook não encontrado.");
            if (q.value(0).toInt() != 1) fail("Este notebook já está inativo.");
            fail("Somente notebooks disponíveis podem ser inativados.");
        }
        q.prepare("SELECT asset_tag,name FROM notebooks WHERE id=?"); q.addBindValue(notebookId); run(q); q.next();
        audit(actor, notebookId, "Notebook inativado", "Notebook inativado: " + q.value(0).toString() + " · " + q.value(1).toString() + ".",
              LocalDatabase::change("notebook_active", 1, 0), LocalDatabase::nowUtc());
    });
}
void NotebookStore::reactivate(const Session &session, qint64 notebookId) {
    database.transaction([&] {
        const auto actor = auth.authorize(session, Permission::InactivateCatalog);
        QSqlQuery q(db); q.prepare("UPDATE notebooks SET active = 1 WHERE id = ? AND active = 0");
        q.addBindValue(notebookId); run(q);
        if (q.numRowsAffected() != 1) {
            q.prepare("SELECT active FROM notebooks WHERE id=?"); q.addBindValue(notebookId); run(q);
            if (!q.next()) fail("Notebook não encontrado.");
            fail("Este notebook já está ativo.");
        }
        q.prepare("SELECT asset_tag,name FROM notebooks WHERE id=?"); q.addBindValue(notebookId); run(q); q.next();
        audit(actor, notebookId, "Notebook reativado", "Notebook reativado: " + q.value(0).toString() + " · " + q.value(1).toString() + ".",
              LocalDatabase::change("notebook_active", 0, 1), LocalDatabase::nowUtc());
    });
}
QVector<Notebook> NotebookStore::notebooks() const {
    QSqlQuery q(db);
    if (!q.exec("SELECT n.id,n.asset_tag,n.name,n.status,COALESCE(l.borrower,''),l.checked_out_at,l.returned_at,COALESCE(p.name,n.location),l.returned_by,l.borrower_role,l.returned_by_role,n.location_id,p.deleted_at,n.active,l.borrower_subject,l.returned_by_subject,l.borrower_person_id,m.happened_at,m.changes,l.returned_by_person_id FROM notebooks n LEFT JOIN locations p ON p.id=n.location_id LEFT JOIN loans l ON l.id=(SELECT MAX(id) FROM loans WHERE notebook_id=n.id) LEFT JOIN events m ON n.status='maintenance' AND m.id=(SELECT MAX(id) FROM events WHERE notebook_id=n.id AND action='Manutenção') ORDER BY n.id DESC")) fail(q.lastError().text());
    QVector<Notebook> rows;
    while (q.next()) {
        const auto state = q.value(3).toString();
        rows.append({q.value(0).toLongLong(),q.value(1).toString(),q.value(2).toString(),state == "loaned" ? Status::Loaned : state == "maintenance" ? Status::Maintenance : Status::Available,q.value(4).toString(),QDateTime::fromString(q.value(5).toString(),Qt::ISODateWithMs),QDateTime::fromString(q.value(6).toString(),Qt::ISODateWithMs)});
        auto &notebook = rows.last();
        notebook.location = q.value(7).toString(); notebook.returnedBy = q.value(8).toString();
        notebook.locationId = q.value(11).toLongLong(); notebook.locationDeleted = !q.value(12).isNull(); notebook.active = q.value(13).toInt() == 1;
        notebook.borrowerSubject = q.value(14).toString(); notebook.returnedBySubject = q.value(15).toString();
        notebook.borrowerRole = readRole(q.value(9).toString()); notebook.returnedByRole = readRole(q.value(10).toString());
        notebook.borrowerPersonId = q.value(16).toLongLong();
        notebook.returnedByPersonId = q.value(19).toLongLong();
        if (notebook.status == Status::Maintenance && !q.value(17).isNull()) {
            notebook.maintenanceSince = QDateTime::fromString(q.value(17).toString(), Qt::ISODateWithMs);
            notebook.maintenanceNote = QJsonDocument::fromJson(q.value(18).toString().toUtf8()).object().value("maintenance_note").toObject().value("para").toString();
        }
    }
    return rows;
}
QVector<HistoryEntry> NotebookStore::history() const {
    HistoryFilter filter; filter.period = PeriodPreset::All;
    auto rows = HistoryQueryService(database).search(filter, QDateTime::currentDateTimeUtc(), QTimeZone("UTC"), 0).entries;
    std::sort(rows.begin(), rows.end(), [](const HistoryEntry &a, const HistoryEntry &b) { return a.id > b.id; });
    return rows;
}

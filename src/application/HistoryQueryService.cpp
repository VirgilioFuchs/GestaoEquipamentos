#include "HistoryQueryService.h"
#include "domain/HistoryPeriod.h"
#include "domain/EventCatalog.h"
#include "storage/LocalDatabase.h"
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <stdexcept>
#include <algorithm>

namespace {
void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QStringList legacyLocations(QSqlDatabase db) {
    QSqlQuery q(db);
    const QString sql="SELECT checkout_location FROM loans WHERE checkout_location_id IS NULL AND checkout_location IS NOT NULL UNION SELECT return_location FROM loans WHERE return_location_id IS NULL AND return_location IS NOT NULL UNION SELECT json_extract(changes,'$.location.de') FROM events WHERE from_location_id IS NULL AND json_valid(changes) UNION SELECT json_extract(changes,'$.location.para') FROM events WHERE location_id IS NULL AND json_valid(changes)";
    if (!q.exec(sql)) fail(q.lastError().text());
    QStringList result; while (q.next()) if (!q.value(0).isNull() && !q.value(0).toString().trimmed().isEmpty()) result << q.value(0).toString();
    return result;
}
QString escapeLike(QString text) { text.replace("\\","\\\\"); text.replace("%","\\%"); text.replace("_","\\_"); return text; }
}
HistoryPage HistoryQueryService::search(const HistoryFilter &filter, const QDateTime &now, const QTimeZone &zone, int limit, QStringList *queryPlan) const {
    if (filter.locationId && !filter.legacyLocationText.trimmed().isEmpty())
        throw std::invalid_argument("Escolha um local cadastrado ou legado, não os dois.");
    if (filter.personId && !filter.personText.trimmed().isEmpty())
        throw std::invalid_argument("Escolha uma pessoa cadastrada ou digite um nome, não os dois.");
    const auto range = resolvePeriod(filter.period, filter.customFrom, filter.customTo, now, zone);
    QStringList where;
    QVariantList binds;
    if (range.startUtc) { where << "e.happened_at >= ?"; binds << range.startUtc->toString(Qt::ISODateWithMs); }
    if (range.endUtc) { where << "e.happened_at < ?"; binds << range.endUtc->toString(Qt::ISODateWithMs); }
    if (filter.notebookId) { where << "e.notebook_id = ?"; binds << *filter.notebookId; }
    if (filter.accountId) { where << "e.actor_account_id = ?"; binds << *filter.accountId; }
    if (!filter.action.trimmed().isEmpty()) { where << "e.action = ?"; binds << filter.action; }
    if (filter.locationId) {
        where << "(e.location_id=? OR e.from_location_id=? OR (e.entity_type='location' AND e.entity_id=?) OR (e.location_id IS NULL AND e.from_location_id IS NULL AND json_valid(e.changes) AND (json_extract(e.changes,'$.location_id.de')=? OR json_extract(e.changes,'$.location_id.para')=?)) OR e.id IN (SELECT e2.id FROM loans x JOIN events e2 ON e2.loan_id IS NULL AND e2.notebook_id=x.notebook_id AND e2.happened_at=x.checked_out_at AND e2.action='Retirada' WHERE x.checkout_location_id=? UNION ALL SELECT e2.id FROM loans x JOIN events e2 ON e2.loan_id IS NULL AND e2.notebook_id=x.notebook_id AND e2.happened_at=x.returned_at AND e2.action='Devolução' WHERE x.return_location_id=?))";
        for (int i=0;i<7;++i) binds << *filter.locationId;
    }
    if (!filter.legacyLocationText.trimmed().isEmpty()) {
        QVariantList locations; const auto needle=searchFold(filter.legacyLocationText);
        for (const auto &value : legacyLocations(database.connection())) if (searchFold(value)==needle) locations << value;
        QStringList clauses;
        if (!locations.isEmpty()) {
            const QString marks=QStringList(locations.size(),QStringLiteral("?")).join(',');
            clauses << QString("(json_valid(e.changes) AND ((e.from_location_id IS NULL AND json_extract(e.changes,'$.location.de') IN (%1)) OR (e.location_id IS NULL AND json_extract(e.changes,'$.location.para') IN (%1))))").arg(marks);
            binds.append(locations); binds.append(locations);
            clauses << QString("e.id IN (SELECT e2.id FROM loans x JOIN events e2 ON e2.notebook_id=x.notebook_id AND e2.happened_at=x.checked_out_at AND e2.action='Retirada' WHERE x.checkout_location_id IS NULL AND x.checkout_location IN (%1) UNION ALL SELECT e2.id FROM loans x JOIN events e2 ON e2.notebook_id=x.notebook_id AND e2.happened_at=x.returned_at AND e2.action='Devolução' WHERE x.return_location_id IS NULL AND x.return_location IN (%1))").arg(marks);
            binds.append(locations); binds.append(locations);
        }
        clauses << "((e.changes IS NULL OR e.changes='{}') AND e.details LIKE ? ESCAPE '\\')";
        binds << (QStringLiteral("%")+escapeLike(filter.legacyLocationText.trimmed())+QStringLiteral("%"));
        where << "("+clauses.join(" OR ")+")";
    }
    if (filter.personId || !filter.personText.trimmed().isEmpty()) {
        QVariantList values;
        if (filter.personId) values << *filter.personId;
        else {
            QSqlQuery names(database.connection());
            if (!names.exec("SELECT borrower FROM loans WHERE borrower IS NOT NULL UNION SELECT returned_by FROM loans WHERE returned_by IS NOT NULL")) fail(names.lastError().text());
            const QString needle=searchFold(filter.personText);
            while (names.next()) if (searchFold(names.value(0).toString()).contains(needle)) values << names.value(0);
            if (values.isEmpty()) return {};
        }
        const QString marks=QStringList(values.size(), QStringLiteral("?")).join(',');
        const QString borrower=filter.personId ? "borrower_person_id" : "borrower";
        const QString returner=filter.personId ? "returned_by_person_id" : "returned_by";
        where << QString("e.id IN (SELECT e2.id FROM events e2 JOIN loans l ON l.id=e2.loan_id WHERE e2.action='Retirada' AND l.%1 IN (%3) UNION ALL SELECT e2.id FROM events e2 JOIN loans l ON l.id=e2.loan_id WHERE e2.action='Devolução' AND l.%2 IN (%3) UNION ALL SELECT e3.id FROM loans x JOIN events e3 ON e3.loan_id IS NULL AND e3.notebook_id=x.notebook_id AND e3.happened_at=x.checked_out_at AND e3.action='Retirada' WHERE x.%1 IN (%3) UNION ALL SELECT e3.id FROM loans x JOIN events e3 ON e3.loan_id IS NULL AND e3.notebook_id=x.notebook_id AND e3.happened_at=x.returned_at AND e3.action='Devolução' WHERE x.%2 IN (%3))").arg(borrower,returner,marks);
        for (int i=0;i<4;++i) binds.append(values);
    }
    QString sql = "SELECT e.id,e.happened_at,e.entity_type,COALESCE(e.notebook_asset_tag,''),COALESCE(e.notebook_name,''),e.action,e.details,e.actor_username,e.changes,e.notebook_id,e.loan_id,CASE WHEN e.action='Retirada' THEN l.borrower WHEN e.action='Devolução' THEN l.returned_by END,CASE WHEN e.action='Retirada' THEN l.borrower_role WHEN e.action='Devolução' THEN l.returned_by_role END,CASE WHEN e.action='Retirada' THEN l.borrower_subject WHEN e.action='Devolução' THEN l.returned_by_subject END,l.checkout_location,l.return_location FROM events e LEFT JOIN loans l ON l.id=COALESCE(e.loan_id,CASE WHEN e.action='Retirada' THEN (SELECT x.id FROM loans x WHERE x.notebook_id=e.notebook_id AND x.checked_out_at=e.happened_at ORDER BY x.id LIMIT 1) WHEN e.action='Devolução' THEN (SELECT x.id FROM loans x WHERE x.notebook_id=e.notebook_id AND x.returned_at=e.happened_at ORDER BY x.id LIMIT 1) END)";
    if (!where.isEmpty()) sql += " WHERE " + where.join(" AND ");
    sql += " ORDER BY e.happened_at DESC,e.id DESC";
    if (limit > 0) { sql += " LIMIT ?"; binds << qint64(limit) + 1; }
    if (queryPlan) {
        queryPlan->clear(); QSqlQuery explain(database.connection());
        if (!explain.prepare("EXPLAIN QUERY PLAN " + sql)) fail(explain.lastError().text());
        for (const auto &value : binds) explain.addBindValue(value);
        if (!explain.exec()) fail(explain.lastError().text());
        while (explain.next()) *queryPlan << explain.value(3).toString();
    }
    QSqlQuery q(database.connection());
    if (!q.prepare(sql)) fail(q.lastError().text());
    for (const auto &value : binds) q.addBindValue(value);
    if (!q.exec()) fail(q.lastError().text());
    HistoryPage result;
    while (q.next()) {
        if (limit > 0 && result.entries.size() == limit) { result.truncated = true; break; }
        HistoryEntry entry;
        entry.id = q.value(0).toLongLong(); entry.happenedAt = QDateTime::fromString(q.value(1).toString(), Qt::ISODateWithMs);
        entry.entityType = q.value(2).toString(); entry.assetTag = q.value(3).toString(); entry.notebookName = q.value(4).toString();
        entry.action = q.value(5).toString(); entry.details = q.value(6).toString(); entry.actorUsername = q.value(7).toString();
        if (!q.value(8).isNull()) entry.changes = QJsonDocument::fromJson(q.value(8).toString().toUtf8()).object();
        if (!q.value(9).isNull()) entry.notebookId=q.value(9).toLongLong();
        if (!q.value(10).isNull()) entry.loanId=q.value(10).toLongLong();
        entry.responsibleName=q.value(11).toString(); entry.responsibleRole=q.value(12).toString(); entry.responsibleSubject=q.value(13).toString();
        entry.checkoutLocation=q.value(14).toString(); entry.returnLocation=q.value(15).toString();
        result.entries.append(entry);
    }
    return result;
}
HistoryFilterOptions HistoryQueryService::filterOptions() const {
    QSqlQuery q(database.connection());
    if (!q.exec("SELECT id,asset_tag,name,active FROM notebooks ORDER BY asset_tag COLLATE NOCASE,id")) fail(q.lastError().text());
    HistoryFilterOptions result;
    while (q.next()) {
        const bool inactive = !q.value(3).toBool();
        result.notebooks.append({q.value(0).toLongLong(), q.value(1).toString() + QStringLiteral(" · ") + q.value(2).toString() + (inactive ? QStringLiteral(" (inativo)") : QString()), inactive});
    }
    if (!q.exec("SELECT id,username,role,active FROM accounts ORDER BY username COLLATE NOCASE,id")) fail(q.lastError().text());
    while (q.next()) {
        const QString role=q.value(2).toString(); const QString label=role=="principal" ? "Principal" : role=="admin" ? "Administrador" : "Comum";
        const bool inactive=!q.value(3).toBool();
        result.accounts.append({q.value(0).toLongLong(),q.value(1).toString()+QStringLiteral(" · ")+label+(inactive ? QStringLiteral(" (inativa)") : QString()),inactive});
    }
    if (!q.exec("SELECT DISTINCT action FROM events")) fail(q.lastError().text());
    while (q.next()) result.actions << q.value(0).toString();
    std::sort(result.actions.begin(),result.actions.end(),[](const QString &a,const QString &b) {
        auto position=[](const QString &value) { const auto &kinds=eventCatalog(); for (int i=0;i<kinds.size();++i) if (kinds[i].action==value) return i; return int(kinds.size()); };
        const int x=position(a),y=position(b); return x==y ? a.localeAwareCompare(b)<0 : x<y;
    });
    if (!q.exec("SELECT id,name,deleted_at FROM locations ORDER BY name_key,id")) fail(q.lastError().text());
    QStringList existing;
    while (q.next()) {
        const bool deleted=!q.value(2).isNull(); const auto name=q.value(1).toString(); existing << searchFold(name);
        FilterOption option{q.value(0).toLongLong(),name+(deleted ? QStringLiteral(" (excluído)") : QString()),deleted};
        (deleted ? result.deletedLocations : result.activeLocations).append(option);
    }
    for (const auto &value : legacyLocations(database.connection()))
        if (!existing.contains(searchFold(value)) && !result.legacyLocationTexts.contains(value)) result.legacyLocationTexts << value;
    result.legacyLocationTexts.sort(Qt::CaseInsensitive);
    return result;
}

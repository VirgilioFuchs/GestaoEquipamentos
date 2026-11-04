#include <QtTest>
#include <QTemporaryDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <stdexcept>
#include "TestSupport.h"
namespace {
const QString BackupFailure =
    "Não foi possível criar a cópia de segurança antes de atualizar o banco. Nada foi alterado.";
QStringList dump(const QString &path, const QString &query) {
    QStringList rows;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "inspect");
        db.setDatabaseName(path);
        QSqlQuery q(db);
        if (!db.open() || !q.exec(query))
            rows << "ERRO: " + q.lastError().text();
        while (q.next()) {
            QStringList columns;
            for (int c = 0; c < q.record().count(); ++c)
                columns << (q.value(c).isNull() ? QStringLiteral("<null>") : q.value(c).toString());
            rows << columns.join('|');
        }
    }
    QSqlDatabase::removeDatabase("inspect");
    return rows;
}
const QString LegacyEvents = "SELECT id,notebook_id,action,details,happened_at FROM events WHERE id<=2 ORDER BY id";
const QString AllLoans =
    "SELECT id,notebook_id,borrower,checked_out_at,returned_at,borrower_role,returned_by,returned_by_role FROM loans "
    "ORDER BY id";
const QString AllAccounts = "SELECT * FROM accounts ORDER BY id";
const QString AllEvents =
    "SELECT id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_username,changes "
    "FROM events ORDER BY id";
const QString RecoveryRow = "SELECT id,generation,vault_key,totp_last_counter,enrolled_at FROM recovery_config";
using LegacyFixtures::createV2;
using LegacyFixtures::createV3;
using LegacyFixtures::createV4;
using LegacyFixtures::createV5;
using LegacyFixtures::execAll;
bool createV1(const QString &path) {
    return execAll(
        path,
        {"CREATE TABLE notebooks(id INTEGER PRIMARY KEY,asset_tag TEXT NOT NULL COLLATE NOCASE UNIQUE,name TEXT NOT "
         "NULL,status TEXT NOT NULL DEFAULT 'available' CHECK(status IN ('available','loaned','maintenance')))",
         "CREATE TABLE loans(id INTEGER PRIMARY KEY,notebook_id INTEGER NOT NULL REFERENCES notebooks(id),borrower "
         "TEXT NOT NULL,checked_out_at TEXT NOT NULL,returned_at TEXT)",
         "CREATE TABLE events(id INTEGER PRIMARY KEY,notebook_id INTEGER NOT NULL REFERENCES notebooks(id),action TEXT "
         "NOT NULL,details TEXT NOT NULL,happened_at TEXT NOT NULL)",
         "INSERT INTO notebooks VALUES(1,'OLD-1','Dell','loaned')",
         "INSERT INTO loans VALUES(1,1,'Ana','2026-01-01T10:00:00.123Z',NULL)",
         "INSERT INTO events VALUES(1,1,'Retirada','Ana','2026-01-01T10:00:00.123Z')", "PRAGMA user_version=1"});
}
// Fixture congelada: não depende da versão atual produzida pelo aplicativo.
bool createV6(const QString &path) {
    if (!createV5(path))
        return false;
    return execAll(
        path, {"ALTER TABLE loans ADD COLUMN origin_location_id INTEGER REFERENCES locations(id)",
               "ALTER TABLE loans ADD COLUMN origin_location TEXT",
               "ALTER TABLE events ADD COLUMN loan_id INTEGER REFERENCES loans(id)",
               "ALTER TABLE events ADD COLUMN location_id INTEGER REFERENCES locations(id)",
               "ALTER TABLE events ADD COLUMN from_location_id INTEGER REFERENCES locations(id)",
               "DROP TRIGGER loans_history_immutable",
               "CREATE TRIGGER loans_history_immutable BEFORE UPDATE ON loans WHEN OLD.returned_at IS NOT NULL OR "
               "NEW.notebook_id IS NOT OLD.notebook_id OR NEW.borrower IS NOT OLD.borrower OR NEW.borrower_role IS NOT "
               "OLD.borrower_role OR NEW.borrower_person_id IS NOT OLD.borrower_person_id OR NEW.borrower_subject IS "
               "NOT OLD.borrower_subject OR NEW.checked_out_at IS NOT OLD.checked_out_at OR NEW.checkout_location_id "
               "IS NOT OLD.checkout_location_id OR NEW.checkout_location IS NOT OLD.checkout_location OR "
               "NEW.origin_location_id IS NOT OLD.origin_location_id OR NEW.origin_location IS NOT OLD.origin_location "
               "BEGIN SELECT RAISE(ABORT,'Empréstimos anteriores não podem ser alterados.'); END",
               "PRAGMA user_version=6"});
}
EventRecord systemEvent(const QJsonObject &changes) {
    EventRecord event;
    event.entityType = "system";
    event.action = "Teste";
    event.details = "Teste";
    event.changes = changes;
    return event;
}
} // namespace
class StoreTests : public QObject {
    Q_OBJECT
    void verifyUpgrade(int version) {
        QTemporaryDir temp;
        const auto path = temp.filePath("upgrade.sqlite");
        QVERIFY(version == 1   ? createV1(path)
                : version == 2 ? createV2(path)
                : version == 3 ? createV3(path)
                               : createV4(path));
        const QString loansQuery =
            version == 1 ? "SELECT id,notebook_id,borrower,checked_out_at,returned_at FROM loans ORDER BY id"
                         : AllLoans;
        const QString notebooksQuery = version == 1
                                           ? "SELECT id,asset_tag,name,status FROM notebooks ORDER BY id"
                                           : "SELECT id,asset_tag,name,status,location FROM notebooks ORDER BY id";
        const QString eventsQuery =
            version == 1 ? "SELECT id,notebook_id,action,details,happened_at FROM events WHERE id=1" : LegacyEvents;
        const auto eventsBefore = dump(path, eventsQuery);
        const auto loansBefore = dump(path, loansQuery);
        const auto notebooksBefore = dump(path, notebooksQuery);
        const auto accountsBefore = version >= 3 ? dump(path, AllAccounts) : QStringList{};
        const auto configBefore = version >= 3 ? dump(path, "SELECT * FROM app_config") : QStringList{};
        const auto recoveryBefore = version == 4 ? dump(path, "SELECT * FROM recovery_config") : QStringList{};
        QString backup;
        {
            LocalDatabase database(path);
            backup = database.lastBackupPath();
        }
        QVERIFY(backup.contains(".pre-v" + QString::number(LocalDatabase::CurrentSchemaVersion) + "-"));
        QCOMPARE(dump(backup, "PRAGMA user_version"), QStringList{QString::number(version)});
        QCOMPARE(dump(backup, loansQuery), loansBefore);
        QCOMPARE(dump(backup, eventsQuery), eventsBefore);
        for (int pass = 0; pass < 2; ++pass) {
            QCOMPARE(dump(path, "PRAGMA user_version"),
                     QStringList{QString::number(LocalDatabase::CurrentSchemaVersion)});
            QCOMPARE(dump(path, eventsQuery), eventsBefore);
            QCOMPARE(dump(path, loansQuery), loansBefore);
            QCOMPARE(dump(path, notebooksQuery), notebooksBefore);
            if (version >= 3) {
                QCOMPARE(dump(path, AllAccounts), accountsBefore);
                QCOMPARE(dump(path, "SELECT * FROM app_config"), configBefore);
            }
            if (version == 4)
                QCOMPARE(dump(path, "SELECT * FROM recovery_config"), recoveryBefore);
            QCOMPARE(
                dump(path, "SELECT changes FROM events WHERE entity_type='system'"),
                QStringList{QString::fromUtf8(
                    QJsonDocument(LocalDatabase::change("schema_version", version, LocalDatabase::CurrentSchemaVersion))
                        .toJson(QJsonDocument::Compact))});
            QCOMPARE(dump(path, "SELECT COUNT(*) FROM events WHERE action='Local importado' AND id>(SELECT id FROM "
                                "events WHERE entity_type='system')"),
                     QStringList{"0"});
            QCOMPARE(dump(path, "PRAGMA foreign_key_check"), QStringList{});
            QCOMPARE(dump(path, "PRAGMA integrity_check"), QStringList{"ok"});
            QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.bak"}, QDir::Files).size(), 1);
            LocalDatabase reopened(path);
            QVERIFY(reopened.lastBackupPath().isEmpty());
        }
    }
private slots:
    void migrationToV7FromV1ToV6() {
        for (int version = 1; version <= 6; ++version) {
            QTemporaryDir temp;
            const auto path = temp.filePath("upgrade.sqlite");
            QVERIFY(version == 1   ? createV1(path)
                    : version == 2 ? createV2(path)
                    : version == 3 ? createV3(path)
                    : version == 4 ? createV4(path)
                    : version == 5 ? createV5(path)
                                   : createV6(path));
            const auto notebooks = dump(path, "SELECT id,asset_tag,name,status FROM notebooks ORDER BY id");
            const auto loans =
                dump(path, "SELECT id,notebook_id,borrower,checked_out_at,returned_at FROM loans ORDER BY id");
            const auto events = dump(path, "SELECT id,notebook_id,action,details,happened_at FROM events ORDER BY id");
            const auto people = version >= 5 ? dump(path, "SELECT * FROM people ORDER BY id") : QStringList{};
            const auto locations = version >= 5 ? dump(path, "SELECT * FROM locations ORDER BY id") : QStringList{};
            const auto fullLoans = version == 6 ? dump(path, "SELECT * FROM loans ORDER BY id") : QStringList{};
            const auto fullEvents = version == 6 ? dump(path, "SELECT * FROM events ORDER BY id") : QStringList{};
            const auto fullNotebooks = version == 6 ? dump(path, "SELECT * FROM notebooks ORDER BY id") : QStringList{};
            const int eventCount = events.size();
            QString backup;
            {
                LocalDatabase db(path);
                backup = db.lastBackupPath();
            }
            QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"7"});
            QCOMPARE(dump(path, "SELECT id,asset_tag,name,status FROM notebooks ORDER BY id"), notebooks);
            QCOMPARE(dump(path, "SELECT id,notebook_id,borrower,checked_out_at,returned_at FROM loans ORDER BY id"),
                     loans);
            QCOMPARE(
                dump(path,
                     QString("SELECT id,notebook_id,action,details,happened_at FROM events WHERE id<=%1 ORDER BY id")
                         .arg(eventCount)),
                events);
            if (version >= 5) {
                QCOMPARE(dump(path, "SELECT * FROM people ORDER BY id"), people);
                QCOMPARE(dump(path, "SELECT * FROM locations ORDER BY id"), locations);
            }
            if (version == 6) {
                QCOMPARE(dump(path, "SELECT * FROM loans ORDER BY id"), fullLoans);
                QCOMPARE(dump(path, "SELECT * FROM notebooks ORDER BY id"), fullNotebooks);
                QCOMPARE(dump(path, QString("SELECT * FROM events WHERE id<=%1 ORDER BY id").arg(eventCount)),
                         fullEvents);
            }
            QCOMPARE(dump(path, "SELECT changes FROM events WHERE action='Atualização do banco'"),
                     QStringList{QString::fromUtf8(QJsonDocument(LocalDatabase::change("schema_version", version, 7))
                                                       .toJson(QJsonDocument::Compact))});
            QCOMPARE(dump(path, "PRAGMA integrity_check"), QStringList{"ok"});
            QCOMPARE(dump(backup, "PRAGMA user_version"), QStringList{QString::number(version)});
            const auto after = dump(path, "SELECT * FROM events ORDER BY id");
            {
                LocalDatabase db(path);
                QVERIFY(db.lastBackupPath().isEmpty());
            }
            QCOMPARE(dump(path, "SELECT * FROM events ORDER BY id"), after);
            QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.bak"}, QDir::Files).size(), 1);
        }
    }
    void backupBeforeV7() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v6.sqlite");
        QVERIFY(createV6(path));
        const auto before = dump(path, "SELECT * FROM events ORDER BY id");
        QString backup;
        {
            LocalDatabase db(path);
            backup = db.lastBackupPath();
        }
        QVERIFY(backup.contains(".pre-v7-"));
        QCOMPARE(dump(backup, "PRAGMA user_version"), QStringList{"6"});
        QCOMPARE(dump(backup, "SELECT * FROM events ORDER BY id"), before);
        QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.pre-v7-*.bak"}, QDir::Files).size(), 1);
    }
    void migrationV7FailureRollsBack() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v6.sqlite");
        QVERIFY(createV6(path));
        QVERIFY(execAll(path, {"CREATE INDEX events_by_happened_at ON events(id)"}));
        const auto before = dump(path, "SELECT * FROM events ORDER BY id");
        QVERIFY_EXCEPTION_THROWN(LocalDatabase db(path), std::runtime_error);
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"6"});
        QCOMPARE(dump(path, "SELECT * FROM events ORDER BY id"), before);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM sqlite_master WHERE name='loans_by_checkout'"), QStringList{"0"});
    }
    void indexesPresentOnNewAndMigrated() {
        const QString query = "SELECT name FROM sqlite_master WHERE type='index' AND name IN "
                              "('events_by_happened_at','events_by_notebook_time','events_by_loan','events_by_location'"
                              ",'events_by_from_location','loans_by_checkout','loans_by_return','loans_by_borrower_"
                              "person','loans_by_returner_person') ORDER BY name";
        QTemporaryDir temp;
        const auto fresh = temp.filePath("fresh.sqlite"), old = temp.filePath("old.sqlite");
        QVERIFY(createV6(old));
        { LocalDatabase db(fresh); }
        { LocalDatabase db(old); }
        QCOMPARE(dump(fresh, query).size(), 9);
        QCOMPARE(dump(old, query), dump(fresh, query));
    }
    void historyUsesSnapshots() {
        QTemporaryDir temp;
        const auto path = temp.filePath("history-snapshots.sqlite");
        QVERIFY(createV2(path));
        qint64 firstEvent;
        {
            TestContext cs(path);
            for (const auto &entry : cs.store.history())
                if (entry.id <= 2) {
                    QVERIFY(entry.assetTag.isEmpty());
                    QVERIFY(entry.notebookName.isEmpty());
                }
            Fixture::updateLocation(cs, cs.session, 1, "Biblioteca");
            const auto recorded = cs.store.history().first();
            firstEvent = recorded.id;
            QCOMPARE(recorded.assetTag, QString("NB-7"));
            QCOMPARE(recorded.notebookName, QString("Dell Latitude"));
            QVERIFY(execAll(path, {"UPDATE notebooks SET name='Dell 5420',asset_tag='NB-RENAMED' WHERE id=1"}));
            QCOMPARE(cs.store.history().first().notebookName, QString("Dell Latitude"));
            QCOMPARE(cs.store.history().first().assetTag, QString("NB-7"));
            Fixture::updateLocation(cs, cs.session, 1, "Sala 3");
            QCOMPARE(cs.store.history().first().notebookName, QString("Dell 5420"));
            QCOMPARE(cs.store.history().first().assetTag, QString("NB-RENAMED"));
        }
        TestContext reopened(path);
        for (const auto &entry : reopened.store.history()) {
            if (entry.id == firstEvent) {
                QCOMPARE(entry.assetTag, QString("NB-7"));
                QCOMPARE(entry.notebookName, QString("Dell Latitude"));
            }
            if (entry.id <= 2) {
                QVERIFY(entry.assetTag.isEmpty());
                QVERIFY(entry.notebookName.isEmpty());
            }
        }
    }
    void migrationFromV1ToV5() { verifyUpgrade(1); }
    void migrationFromV2ToV5() { verifyUpgrade(2); }
    void migrationFromV3ToV5() { verifyUpgrade(3); }
    void migrationFromV4ToV5() { verifyUpgrade(4); }
    void migrationFromEmptyToV5() {
        QTemporaryDir temp;
        const auto path = temp.filePath("new.sqlite");
        LocalDatabase database(path);
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{QString::number(LocalDatabase::CurrentSchemaVersion)});
        QVERIFY(database.lastBackupPath().isEmpty());
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM events"), QStringList{"0"});
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM people"), QStringList{"0"});
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM locations"), QStringList{"0"});
        QVERIFY(!QFileInfo::exists(temp.filePath("backups")));
    }
    void backupBeforeV5() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v4.sqlite");
        QVERIFY(createV4(path));
        const auto eventsBefore = dump(path, AllEvents);
        const auto loansBefore = dump(path, AllLoans);
        LocalDatabase database(path);
        const auto backup = database.lastBackupPath();
        QCOMPARE(dump(backup, "PRAGMA user_version"), QStringList{"4"});
        QCOMPARE(dump(backup, AllEvents), eventsBefore);
        QCOMPARE(dump(backup, AllLoans), loansBefore);
        QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.pre-v7-*.bak"}, QDir::Files).size(), 1);
    }
    void migrationV5FailureRollsBack() {
        QTemporaryDir temp;
        const auto path = temp.filePath("failure.sqlite");
        QVERIFY(createV4(path));
        // Collision late in the migration exercises rollback after ALTER, event rebuild and trigger creation.
        QVERIFY(execAll(
            path, {"CREATE TRIGGER loans_no_delete BEFORE DELETE ON loans BEGIN SELECT RAISE(ABORT,'fixture'); END"}));
        const auto eventsBefore = dump(path, AllEvents);
        const auto loansBefore = dump(path, AllLoans);
        const auto notebooksBefore = dump(path, "SELECT * FROM notebooks ORDER BY id");
        QVERIFY_EXCEPTION_THROWN(LocalDatabase database(path), std::runtime_error);
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"4"});
        QCOMPARE(dump(path, AllEvents), eventsBefore);
        QCOMPARE(dump(path, AllLoans), loansBefore);
        QCOMPARE(dump(path, "SELECT * FROM notebooks ORDER BY id"), notebooksBefore);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('people','locations','events_v5')"),
                 QStringList{"0"});
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM pragma_table_info('events') WHERE name='notebook_asset_tag'"),
                 QStringList{"0"});
        const auto other = temp.filePath("collision.sqlite");
        QVERIFY(createV4(other));
        QVERIFY(execAll(other, {"CREATE TABLE people(id INTEGER PRIMARY KEY)"}));
        QVERIFY_EXCEPTION_THROWN(LocalDatabase database(other), std::runtime_error);
        QCOMPARE(dump(other, "PRAGMA user_version"), QStringList{"4"});
        QCOMPARE(dump(other, "SELECT COUNT(*) FROM sqlite_master WHERE name='locations'"), QStringList{"0"});
    }
    void futureVersion8Refused() {
        QTemporaryDir temp;
        const auto path = temp.filePath("future.sqlite");
        QVERIFY(createV4(path));
        QVERIFY(execAll(path, {"PRAGMA user_version=8"}));
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto bytes = original.readAll();
        original.close();
        try {
            LocalDatabase database(path);
            QFAIL("Versão futura deveria ser recusada.");
        } catch (const std::runtime_error &error) {
            QCOMPARE(QString::fromUtf8(error.what()),
                     QString("Banco criado por uma versão mais recente do aplicativo."));
        }
        QFile after(path);
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), bytes);
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"8"});
        QVERIFY(!QFileInfo::exists(temp.filePath("backups")));
    }
    void locationImportDedupe() {
        QTemporaryDir temp;
        const auto path = temp.filePath("locations.sqlite");
        QVERIFY(createV4(path));
        QVERIFY(
            execAll(path, {"INSERT INTO notebooks(id,asset_tag,name,status,location) VALUES(3,'NB-9','A','available',' "
                           " Biblioteca'),(4,'NB-10','B','available',''),(5,'NB-11','C','available',NULL)"}));
        const auto texts = dump(path, "SELECT location FROM notebooks ORDER BY id");
        LocalDatabase database(path);
        QCOMPARE(dump(path, "SELECT name,name_key FROM locations ORDER BY id"),
                 (QStringList{"Sala 12|sala 12", "Biblioteca|biblioteca"}));
        QCOMPARE(dump(path, "SELECT location_id FROM notebooks ORDER BY id"),
                 (QStringList{"1", "1", "2", "<null>", "<null>"}));
        QCOMPARE(dump(path, "SELECT location FROM notebooks ORDER BY id"), texts);
        QCOMPARE(dump(path, "SELECT actor_account_id,actor_username,details FROM events WHERE action='Local importado' "
                            "ORDER BY id"),
                 (QStringList{
                     "<null>|<null>|Local importado na atualização do banco: Sala 12. Notebooks vinculados: 2.",
                     "<null>|<null>|Local importado na atualização do banco: Biblioteca. Notebooks vinculados: 1."}));
        QCOMPARE(LocalDatabase::catalogKey("  ÁREA  "), QString("área"));
    }
    void historyRowsUntouchedByV5() {
        QTemporaryDir temp;
        const auto path = temp.filePath("history.sqlite");
        QVERIFY(createV4(path));
        const auto events = dump(path, AllEvents);
        const auto loans = dump(path, AllLoans);
        LocalDatabase database(path);
        QCOMPARE(dump(path, "SELECT "
                            "id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,actor_"
                            "username,changes FROM events WHERE id<=3 ORDER BY id"),
                 events);
        QCOMPARE(dump(path, AllLoans), loans);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM people"), QStringList{"0"});
        QCOMPARE(dump(path, "SELECT "
                            "borrower_person_id,borrower_subject,checkout_location_id,checkout_location,returned_by_"
                            "person_id,returned_by_subject,return_location_id,return_location FROM loans ORDER BY id"),
                 (QStringList{"<null>|<null>|<null>|<null>|<null>|<null>|<null>|<null>",
                              "<null>|<null>|<null>|<null>|<null>|<null>|<null>|<null>"}));
    }
    void eventsAppendOnlyAfterV5() {
        QTemporaryDir temp;
        const auto path = temp.filePath("events.sqlite");
        QVERIFY(createV4(path));
        LocalDatabase database(path);
        const auto before = dump(path, "SELECT * FROM events ORDER BY id");
        QVERIFY(!execAll(path, {"UPDATE events SET details='alterado'"}));
        QVERIFY(!execAll(path, {"DELETE FROM events"}));
        QCOMPARE(dump(path, "SELECT * FROM events ORDER BY id"), before);
    }
    void eventsIndexesRecreatedAfterV5() {
        const QString objects =
            "SELECT type,name FROM sqlite_master WHERE tbl_name='events' AND type IN ('index','trigger') AND sql IS "
            "NOT NULL AND name NOT LIKE 'events_by_%' ORDER BY type,name";
        QTemporaryDir temp;
        const auto path = temp.filePath("indexes.sqlite");
        QVERIFY(createV4(path));
        const auto before = dump(path, objects);
        LocalDatabase upgraded(path);
        QCOMPARE(dump(path, objects), before);
        const auto fresh = temp.filePath("fresh.sqlite");
        LocalDatabase database(fresh);
        QCOMPARE(dump(fresh, objects), before);
        const auto custom = temp.filePath("custom.sqlite");
        QVERIFY(createV4(custom));
        QVERIFY(execAll(custom, {"CREATE INDEX custom_events_notebook ON events(notebook_id,id)"}));
        const auto customBefore = dump(custom, objects);
        QVERIFY(dump(custom, "EXPLAIN QUERY PLAN SELECT * FROM events WHERE notebook_id=1 ORDER BY id")
                    .join(' ')
                    .contains("custom_events_notebook"));
        LocalDatabase customUpgrade(custom);
        QCOMPARE(dump(custom, objects), customBefore);
        QVERIFY(dump(custom, "EXPLAIN QUERY PLAN SELECT * FROM events WHERE notebook_id=1 ORDER BY id")
                    .join(' ')
                    .contains("custom_events_notebook"));
    }
    void newEventsCarryNotebookSnapshot() {
        QTemporaryDir temp;
        const auto path = temp.filePath("snapshot.sqlite");
        QVERIFY(createV4(path));
        TestContext cs(path);
        QCOMPARE(dump(path, "SELECT notebook_asset_tag,notebook_name FROM events WHERE id<=3 ORDER BY id"),
                 (QStringList{"<null>|<null>", "<null>|<null>", "<null>|<null>"}));
        Fixture::updateLocation(cs, cs.session, 1, "Biblioteca");
        QCOMPARE(dump(path, "SELECT notebook_asset_tag,notebook_name FROM events WHERE action='Localização'"),
                 QStringList{"NB-7|Dell Latitude"});
    }
    void upgradeV4ShowsImportedLocation() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v4.sqlite");
        QVERIFY(createV4(path));
        const auto originalEvents = dump(path, AllEvents);
        const auto originalLoans = dump(path, AllLoans);
        {
            TestContext cs(path);
            const auto entries = cs.store.history();
            QCOMPARE(entries[0].entityType, QString("system"));
            QCOMPARE(entries[0].changes,
                     LocalDatabase::change("schema_version", 4, LocalDatabase::CurrentSchemaVersion));
            QCOMPARE(entries[1].action, QString("Local importado"));
            QCOMPARE(entries[1].entityType, QString("location"));
            QVERIFY(entries[1].actorUsername.isEmpty());
            QCOMPARE(entries[1].details,
                     QString("Local importado na atualização do banco: Sala 12. Notebooks vinculados: 2."));
            QCOMPARE(dump(path, "SELECT id,name FROM locations"), QStringList{"1|Sala 12"});
            QCOMPARE(dump(path, "SELECT location_id FROM notebooks ORDER BY id"), (QStringList{"1", "1"}));
            QCOMPARE(dump(path, "PRAGMA user_version"),
                     QStringList{QString::number(LocalDatabase::CurrentSchemaVersion)});
            QCOMPARE(dump(path, "SELECT "
                                "id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,"
                                "actor_username,changes FROM events WHERE id<=3 ORDER BY id"),
                     originalEvents);
            QCOMPARE(dump(path, AllLoans), originalLoans);
            QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.pre-v7-*.bak"}, QDir::Files).size(), 1);
        }
        const auto after = dump(path, "SELECT * FROM events ORDER BY id");
        {
            LocalDatabase reopened(path);
            QVERIFY(reopened.lastBackupPath().isEmpty());
        }
        QCOMPARE(dump(path, "SELECT * FROM events ORDER BY id"), after);
    }
    void independentResponsibilities_data() {
        QTest::addColumn<ResponsibleRole>("outRole");
        QTest::addColumn<ResponsibleRole>("inRole");
        QTest::newRow("professor-auxiliar") << ResponsibleRole::Professor << ResponsibleRole::Assistant;
        QTest::newRow("auxiliar-professor") << ResponsibleRole::Assistant << ResponsibleRole::Professor;
        QTest::newRow("professor-professor") << ResponsibleRole::Professor << ResponsibleRole::Professor;
        QTest::newRow("auxiliar-auxiliar") << ResponsibleRole::Assistant << ResponsibleRole::Assistant;
    }
    void independentResponsibilities() {
        QFETCH(ResponsibleRole, outRole);
        QFETCH(ResponsibleRole, inRole);
        QTemporaryDir temp;
        const auto path = temp.filePath("roles.sqlite");
        qint64 id;
        {
            TestContext cs(path);
            auto &s = cs.store;
            Fixture::add(cs, cs.session, "NB-1", "Dell", "Estoque");
            id = s.notebooks().first().id;
            Fixture::checkout(cs, cs.session, id, " Ana ", outRole, " Sala 12 ");
            Fixture::checkin(cs, cs.session, id, " Carlos ", inRole, " Biblioteca ");
        }
        {
            TestContext cs(path);
            auto &s = cs.store;
            const auto n = s.notebooks().first();
            QCOMPARE(n.status, Status::Available);
            QCOMPARE(n.borrower, QString("Ana"));
            QCOMPARE(n.borrowerRole, outRole);
            QCOMPARE(n.returnedBy, QString("Carlos"));
            QCOMPARE(n.returnedByRole, inRole);
            QCOMPARE(n.location, QString("Biblioteca"));
            QVERIFY(n.checkedOutAt.isValid());
            QVERIFY(n.returnedAt >= n.checkedOutAt);
            QCOMPARE(s.history().first().details,
                     "Devolvido por: Carlos (" + roleLabel(inRole) + "). Local: Biblioteca");
            Fixture::updateLocation(cs, cs.session, id, " Sala 2 ");
            QCOMPARE(s.notebooks().first().location, QString("Sala 2"));
            QCOMPARE(s.history().first().details, QString("Local alterado: Biblioteca → Sala 2"));
            Fixture::checkout(cs, cs.session, id, "Bia", outRole, "Sala 3");
            QVERIFY(!s.notebooks().first().returnedAt.isValid());
            QVERIFY(s.notebooks().first().returnedBy.isEmpty());
        }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "preserved-loans");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("SELECT borrower,returned_by,borrower_role,returned_by_role FROM loans ORDER BY id"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QString("Ana"));
            QCOMPARE(q.value(1).toString(), QString("Carlos"));
            QVERIFY(!q.value(2).isNull());
            QVERIFY(!q.value(3).isNull());
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QString("Bia"));
            QVERIFY(q.value(1).isNull());
            QVERIFY(!q.next());
        }
        QSqlDatabase::removeDatabase("preserved-loans");
    }
    void migrationPreservesLegacyData() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v1.sqlite");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE notebooks(id INTEGER PRIMARY KEY,asset_tag TEXT NOT NULL COLLATE NOCASE "
                           "UNIQUE,name TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'available' CHECK(status IN "
                           "('available','loaned','maintenance')))"));
            QVERIFY(q.exec("CREATE TABLE loans(id INTEGER PRIMARY KEY,notebook_id INTEGER NOT NULL REFERENCES "
                           "notebooks(id),borrower TEXT NOT NULL,checked_out_at TEXT NOT NULL,returned_at TEXT)"));
            QVERIFY(q.exec("CREATE TABLE events(id INTEGER PRIMARY KEY,notebook_id INTEGER NOT NULL REFERENCES "
                           "notebooks(id),action TEXT NOT NULL,details TEXT NOT NULL,happened_at TEXT NOT NULL)"));
            QVERIFY(q.exec("INSERT INTO notebooks VALUES(1,'OLD-1','Dell','loaned'),(2,'OLD-2','HP','available')"));
            QVERIFY(q.exec("INSERT INTO loans "
                           "VALUES(1,1,'Ana','2026-01-01T10:00:00.000Z',NULL),(2,2,'Bia','2026-01-01T09:00:00.000Z','"
                           "2026-01-01T11:00:00.000Z')"));
            QVERIFY(q.exec("INSERT INTO events VALUES(1,1,'Retirada','Ana','2026-01-01T10:00:00.000Z')"));
            QVERIFY(q.exec("PRAGMA user_version=1"));
        }
        QSqlDatabase::removeDatabase("legacy");
        {
            TestContext cs(path);
            auto &s = cs.store;
            const auto rows = s.notebooks();
            QCOMPARE(rows.size(), 2);
            QCOMPARE(rows[0].borrower, QString("Bia"));
            QCOMPARE(rows[0].returnedAt.toUTC().toString(Qt::ISODateWithMs), QString("2026-01-01T11:00:00.000Z"));
            QVERIFY(rows[0].returnedBy.isEmpty());
            QCOMPARE(rows[0].returnedByRole, ResponsibleRole::Unknown);
            QVERIFY(rows[1].location.isEmpty());
            QCOMPARE(rows[1].borrowerRole, ResponsibleRole::Unknown);
            QCOMPARE(s.history().last().details, QString("Ana"));
            QList<QJsonObject> upgrades;
            for (const auto &entry : s.history())
                if (entry.entityType == "system")
                    upgrades << entry.changes;
            QCOMPARE(upgrades, QList<QJsonObject>{
                                   LocalDatabase::change("schema_version", 1, LocalDatabase::CurrentSchemaVersion)});
            Fixture::checkin(cs, cs.session, 1, "Carlos", ResponsibleRole::Assistant, "Estoque");
            QCOMPARE(s.notebooks()[1].borrower, QString("Ana"));
            QCOMPARE(s.notebooks()[1].borrowerRole, ResponsibleRole::Unknown);
        }
        {
            TestContext cs(path, false);
            auto &s = cs.store;
            QCOMPARE(s.notebooks()[1].returnedBy, QString("Carlos"));
            QCOMPARE(s.history().size(), 4);
        }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "version-check");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("PRAGMA user_version"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), LocalDatabase::CurrentSchemaVersion);
            q.finish();
            QVERIFY(q.exec("PRAGMA user_version=8"));
        }
        QSqlDatabase::removeDatabase("version-check");
        QVERIFY_EXCEPTION_THROWN(TestContext c(path), std::runtime_error);
    }
    void newFieldsValidation() {
        TestContext cs(":memory:");
        auto &s = cs.store;
        QVERIFY_EXCEPTION_THROWN(Fixture::add(cs, cs.session, "NB-1", "Dell", " "), std::runtime_error);
        Fixture::add(cs, cs.session, "NB-1", "Dell", "Estoque");
        const auto id = s.notebooks().first().id;
        QVERIFY_EXCEPTION_THROWN(Fixture::checkout(cs, cs.session, id, "Ana", ResponsibleRole::Unknown, "Sala"),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::checkout(cs, cs.session, id, "Ana", static_cast<ResponsibleRole>(99), "Sala"),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::checkout(cs, cs.session, id, "Ana", ResponsibleRole::Professor, " "),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::updateLocation(cs, cs.session, id, QString(121, 'x')), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::updateLocation(cs, cs.session, id, "Estoque"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::updateLocation(cs, cs.session, 999, "Sala"), std::runtime_error);
        QCOMPARE(s.history().size(), 2);
        Fixture::checkout(cs, cs.session, id, "Ana", ResponsibleRole::Professor, "Sala");
        QVERIFY_EXCEPTION_THROWN(Fixture::checkin(cs, cs.session, id, " ", ResponsibleRole::Assistant, "Estoque"),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::checkin(cs, cs.session, id, "Carlos", ResponsibleRole::Unknown, "Estoque"),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::checkin(cs, cs.session, id, "Carlos", ResponsibleRole::Assistant, " "),
                                 std::runtime_error);
        QCOMPARE(s.notebooks().first().status, Status::Loaned);
        QCOMPARE(s.history().size(), 3);
    }
    void lifecycle() {
        TestContext cs(":memory:");
        auto &s = cs.store;
        Fixture::add(cs, cs.session, "NB-001", "Dell", "Armário 1");
        auto n = s.notebooks().first();
        QCOMPARE(n.status, Status::Available);
        Fixture::checkout(cs, cs.session, n.id, " Virgílio ", ResponsibleRole::Professor, "Sala 12");
        n = s.notebooks().first();
        QCOMPARE(n.borrower, QString("Virgílio"));
        QVERIFY(n.checkedOutAt.isValid());
        QVERIFY(!n.returnedAt.isValid());
        QCOMPARE(s.history().first().actorUsername, QString("principal"));
        QVERIFY_EXCEPTION_THROWN(
            Fixture::checkout(cs, cs.session, n.id, "Outra pessoa", ResponsibleRole::Professor, "Sala 12"),
            std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(s.setMaintenance(cs.session, n.id, true), std::runtime_error);
        QCOMPARE(s.history().size(), 3);
        Fixture::checkin(cs, cs.session, n.id, "Carlos", ResponsibleRole::Assistant, "Armário 1");
        n = s.notebooks().first();
        QCOMPARE(n.status, Status::Available);
        QVERIFY(n.returnedAt.isValid());
        QVERIFY(n.returnedAt >= n.checkedOutAt);
        QVERIFY_EXCEPTION_THROWN(
            Fixture::checkin(cs, cs.session, n.id, "Carlos", ResponsibleRole::Assistant, "Armário 1"),
            std::runtime_error);
        s.setMaintenance(cs.session, n.id, true);
        QCOMPARE(s.notebooks().first().status, Status::Maintenance);
        QVERIFY_EXCEPTION_THROWN(Fixture::checkout(cs, cs.session, n.id, "Ana", ResponsibleRole::Professor, "Sala 12"),
                                 std::runtime_error);
        s.setMaintenance(cs.session, n.id, false);
        Fixture::checkout(cs, cs.session, n.id, "Ana", ResponsibleRole::Professor, "Sala 12");
        n = s.notebooks().first();
        QCOMPARE(n.borrower, QString("Ana"));
        QVERIFY(!n.returnedAt.isValid());
        QCOMPARE(s.history().size(), 7);
    }
    void validationAndRollback() {
        TestContext cs(":memory:");
        auto &s = cs.store;
        Fixture::add(cs, cs.session, "NB-001", "Dell", "Armário 1");
        QVERIFY_EXCEPTION_THROWN(Fixture::add(cs, cs.session, "nb-001", "Outro", "Armário 1"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::add(cs, cs.session, "  ", "Dell", "Armário 1"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::add(cs, cs.session, "NB-002", "  ", "Armário 1"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            Fixture::checkout(cs, cs.session, s.notebooks().first().id, "  ", ResponsibleRole::Professor, "Sala 12"),
            std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Fixture::checkout(cs, cs.session, 999, "Ana", ResponsibleRole::Professor, "Sala 12"),
                                 std::runtime_error);
        QCOMPARE(s.notebooks().size(), 1);
        QCOMPARE(s.history().size(), 2);
    }
    void rollbackOnAuditFailure() {
        QTemporaryDir temp;
        const auto path = temp.filePath("test.sqlite");
        TestContext cs(path);
        auto &s = cs.store;
        Fixture::add(cs, cs.session, "NB-001", "Dell", "Armário 1");
        const auto id = s.notebooks().first().id;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "fault-injection");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER fail_audit BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'simulated "
                           "audit failure'); END"));
        }
        QSqlDatabase::removeDatabase("fault-injection");
        QVERIFY_EXCEPTION_THROWN(Fixture::checkout(cs, cs.session, id, "Ana", ResponsibleRole::Professor, "Sala 12"),
                                 std::runtime_error);
        QCOMPARE(s.notebooks().first().status, Status::Available);
        QVERIFY(!s.notebooks().first().checkedOutAt.isValid());
        QCOMPARE(s.history().size(), 2);
        QCOMPARE(s.notebooks().first().location, QString("Armário 1"));
        QVERIFY_EXCEPTION_THROWN(Fixture::updateLocation(cs, cs.session, id, "Sala 2"), std::runtime_error);
        QCOMPARE(s.notebooks().first().location, QString("Armário 1"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "fault-reset");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("DROP TRIGGER fail_audit"));
        }
        QSqlDatabase::removeDatabase("fault-reset");
        Fixture::checkout(cs, cs.session, id, "Ana", ResponsibleRole::Professor, "Sala 12");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "fault-return");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER fail_audit BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'simulated "
                           "audit failure'); END"));
        }
        QSqlDatabase::removeDatabase("fault-return");
        QVERIFY_EXCEPTION_THROWN(Fixture::checkin(cs, cs.session, id, "Carlos", ResponsibleRole::Assistant, "Estoque"),
                                 std::runtime_error);
        const auto n = s.notebooks().first();
        QCOMPARE(n.status, Status::Loaned);
        QCOMPARE(n.location, QString("Sala 12"));
        QVERIFY(!n.returnedAt.isValid());
        QVERIFY(n.returnedBy.isEmpty());
        QCOMPARE(n.returnedByRole, ResponsibleRole::Unknown);
        QCOMPARE(s.history().size(), 3);
    }
    void persistenceAndSecondConnection() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("test.sqlite");
        qint64 id;
        {
            TestContext cs(path);
            auto &s = cs.store;
            Fixture::add(cs, cs.session, "NB-001", "Dell", "Armário 1");
            id = s.notebooks().first().id;
            Fixture::checkout(cs, cs.session, id, "Ana", ResponsibleRole::Professor, "Sala 12");
        }
        {
            TestContext ca(path);
            auto &a = ca.store;
            TestContext cb(path);
            auto &b = cb.store;
            QCOMPARE(b.notebooks().first().status, Status::Loaned);
            Fixture::checkin(ca, ca.session, id, "Carlos", ResponsibleRole::Assistant, "Armário 1");
            Fixture::checkout(cb, cb.session, id, "Bia", ResponsibleRole::Professor, "Sala 12");
            QVERIFY_EXCEPTION_THROWN(
                Fixture::checkout(ca, ca.session, id, "Ana", ResponsibleRole::Professor, "Sala 12"),
                std::runtime_error);
            QCOMPARE(a.notebooks().first().borrower, QString("Bia"));
            QCOMPARE(a.history().size(), 5);
        }
    }
    void migrationFromEmptyToCurrent() {
        QTemporaryDir temp;
        const auto path = temp.filePath("novo.sqlite");
        QString uuid;
        {
            LocalDatabase db(path);
            QVERIFY(db.lastBackupPath().isEmpty());
            uuid = db.databaseUuid();
            QVERIFY(!uuid.isEmpty());
        }
        {
            TestContext cs(path, false);
            auto &s = cs.store;
            QVERIFY(s.history().isEmpty());
        }
        {
            LocalDatabase db(path);
            QCOMPARE(db.databaseUuid(), uuid);
        }
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{QString::number(LocalDatabase::CurrentSchemaVersion)});
        QCOMPARE(dump(path, "SELECT COUNT(*),COUNT(principal_account_id) FROM app_config"), QStringList{"1|0"});
        QCOMPARE(dump(path, RecoveryRow), QStringList{"1|0|<null>|<null>|<null>"});
        QVERIFY(!QFileInfo::exists(temp.filePath("backups")));
        QVERIFY(QDir(temp.path()).entryList({"*.bak"}, QDir::Files).isEmpty());
    }
    void migrationFromV2ToCurrent() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v2.sqlite");
        QVERIFY(createV2(path));
        const auto notebooksBefore = dump(path, "SELECT id,asset_tag,name,status,location FROM notebooks ORDER BY id");
        const auto loansBefore = dump(path, AllLoans);
        const auto eventsBefore = dump(path, LegacyEvents);
        QCOMPARE(eventsBefore.size(), 2);
        QCOMPARE(loansBefore.size(), 2);
        {
            TestContext cs(path, false);
            auto &s = cs.store;
            const auto h = s.history();
            QCOMPARE(h.size(), 4);
            QCOMPARE(h[0].entityType, QString("system"));
            QCOMPARE(h[0].action, QString("Atualização do banco"));
            QCOMPARE(h[0].details, QString("Esquema atualizado da versão 2 para a versão 7."));
            QVERIFY(h[0].actorUsername.isEmpty());
            QVERIFY(h[0].assetTag.isEmpty());
            QVERIFY(h[0].notebookName.isEmpty());
            QCOMPARE(h[0].changes, LocalDatabase::change("schema_version", 2, LocalDatabase::CurrentSchemaVersion));
            QVERIFY(h[2].assetTag.isEmpty());
            QVERIFY(h[2].notebookName.isEmpty());
            QVERIFY(h[2].actorUsername.isEmpty());
            QVERIFY(h[2].changes.isEmpty());
            QCOMPARE(h[3].details, QString("Retirado por: Ana (Professor). Local: Sala 12"));
        }
        for (int pass = 0; pass < 2; ++pass) {
            QCOMPARE(dump(path, "PRAGMA user_version"),
                     QStringList{QString::number(LocalDatabase::CurrentSchemaVersion)});
            QCOMPARE(dump(path, "SELECT id,asset_tag,name,status,location FROM notebooks ORDER BY id"),
                     notebooksBefore);
            QCOMPARE(dump(path, AllLoans), loansBefore);
            QCOMPARE(dump(path, LegacyEvents), eventsBefore);
            QCOMPARE(dump(path, "SELECT entity_type,entity_id,actor_account_id,actor_username,changes FROM events "
                                "WHERE id<=2 ORDER BY id"),
                     (QStringList{"notebook|1|<null>|<null>|<null>", "notebook|1|<null>|<null>|<null>"}));
            QCOMPARE(dump(path, "SELECT notebook_id,actor_account_id,actor_username,changes FROM events WHERE "
                                "entity_type='system'"),
                     QStringList{R"(<null>|<null>|<null>|{"schema_version":{"de":2,"para":7}})"});
            QCOMPARE(dump(path, "PRAGMA foreign_key_check"), QStringList{});
            QCOMPARE(dump(path, "PRAGMA integrity_check"), QStringList{"ok"});
            QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.bak"}, QDir::Files).size(), 1);
            LocalDatabase reopened(path);
            QVERIFY(reopened.lastBackupPath().isEmpty());
        }
        {
            TestContext cs(path);
            auto &s = cs.store;
            QCOMPARE(s.history().size(), 5);
            Fixture::checkin(cs, cs.session, 1, "Carlos", ResponsibleRole::Assistant, "Armário 1");
        }
        QCOMPARE(dump(path, "SELECT borrower,returned_by FROM loans ORDER BY id"),
                 (QStringList{"Ana|Carlos", "Bia|Carlos"}));
    }
    void migrationFromV3ToV4() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v3.sqlite");
        QVERIFY(createV3(path));
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'recovery%'"), QStringList{"0"});
        QVERIFY(dump(path, "SELECT COUNT(*) FROM events WHERE actor_account_id IS NOT NULL AND changes IS NOT NULL")
                    .first()
                    .toInt() > 0);
        const auto accountsBefore = dump(path, AllAccounts);
        const auto eventsBefore = dump(path, AllEvents);
        const auto loansBefore = dump(path, AllLoans);
        const auto notebooksBefore = dump(path, "SELECT id,asset_tag,name,status,location FROM notebooks ORDER BY id");
        const auto configBefore = dump(path, "SELECT * FROM app_config");
        const auto lastEvent = dump(path, "SELECT MAX(id) FROM events").first();
        QString backup;
        {
            LocalDatabase db(path);
            backup = db.lastBackupPath();
        }
        QVERIFY2(QRegularExpression(R"(^v3\.sqlite\.pre-v7-\d{8}T\d{6}Z\.bak$)")
                     .match(QFileInfo(backup).fileName())
                     .hasMatch(),
                 qPrintable(backup));
        QCOMPARE(dump(backup, "PRAGMA user_version"), QStringList{"3"});
        QCOMPARE(dump(backup, AllAccounts), accountsBefore);
        QCOMPARE(dump(backup, AllEvents), eventsBefore);
        QCOMPARE(dump(backup, AllLoans), loansBefore);
        for (int pass = 0; pass < 2; ++pass) {
            QCOMPARE(dump(path, "PRAGMA user_version"),
                     QStringList{QString::number(LocalDatabase::CurrentSchemaVersion)});
            QCOMPARE(dump(path, AllAccounts), accountsBefore);
            QCOMPARE(dump(path, AllLoans), loansBefore);
            QCOMPARE(dump(path, "SELECT id,asset_tag,name,status,location FROM notebooks ORDER BY id"),
                     notebooksBefore);
            QCOMPARE(dump(path, "SELECT * FROM app_config"), configBefore);
            QCOMPARE(dump(path, "SELECT "
                                "id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,"
                                "actor_username,changes FROM events WHERE id<=" +
                                    lastEvent + " ORDER BY id"),
                     eventsBefore);
            QCOMPARE(
                dump(path, "SELECT entity_type,actor_account_id,changes,details FROM events WHERE entity_type='system' "
                           "AND id>" +
                               lastEvent),
                QStringList{
                    R"(system|<null>|{"schema_version":{"de":3,"para":7}}|Esquema atualizado da versão 3 para a versão 7.)"});
            QCOMPARE(dump(path, RecoveryRow), QStringList{"1|0|<null>|<null>|<null>"});
            QCOMPARE(dump(path, "PRAGMA foreign_key_check"), QStringList{});
            QCOMPARE(dump(path, "PRAGMA integrity_check"), QStringList{"ok"});
            QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.bak"}, QDir::Files).size(), 1);
            LocalDatabase reopened(path);
            QVERIFY(reopened.lastBackupPath().isEmpty());
        }
        {
            TestContext cs(path);
            QCOMPARE(cs.store.notebooks().first().returnedBy, QString("Carlos"));
        }
    }
    void migrationV3FailureRollsBack() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v3.sqlite");
        QVERIFY(createV3(path));
        QVERIFY(execAll(path, {"CREATE TABLE recovery_codes(id INTEGER PRIMARY KEY)"}));
        const auto accountsBefore = dump(path, AllAccounts);
        const auto eventsBefore = dump(path, AllEvents);
        const auto loansBefore = dump(path, AllLoans);
        QVERIFY_EXCEPTION_THROWN(TestContext c(path), std::runtime_error);
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"3"});
        QCOMPARE(dump(path, AllAccounts), accountsBefore);
        QCOMPARE(dump(path, AllEvents), eventsBefore);
        QCOMPARE(dump(path, AllLoans), loansBefore);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM sqlite_master WHERE name='recovery_config'"), QStringList{"0"});
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM pragma_table_info('recovery_codes')"), QStringList{"1"});
    }
    void backupBeforeUpgrade() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v2.sqlite");
        QVERIFY(createV2(path));
        const auto loansBefore = dump(path, AllLoans);
        const auto eventsBefore = dump(path, LegacyEvents);
        QString backup;
        {
            LocalDatabase db(path);
            backup = db.lastBackupPath();
        }
        QVERIFY(QFileInfo::exists(backup));
        QCOMPARE(QFileInfo(backup).absolutePath(), QFileInfo(temp.filePath("backups")).absoluteFilePath());
        QVERIFY2(QRegularExpression(R"(^v2\.sqlite\.pre-v7-\d{8}T\d{6}Z\.bak$)")
                     .match(QFileInfo(backup).fileName())
                     .hasMatch(),
                 qPrintable(backup));
        QCOMPARE(dump(backup, "PRAGMA user_version"), QStringList{"2"});
        QCOMPARE(dump(backup, LegacyEvents), eventsBefore);
        QCOMPARE(dump(backup, AllLoans), loansBefore);
        QCOMPARE(dump(backup, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('accounts','app_config')"),
                 QStringList{"0"});

        QTemporaryDir blocked;
        const auto other = blocked.filePath("v2.sqlite");
        QVERIFY(createV2(other));
        {
            QFile file(blocked.filePath("backups"));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        try {
            LocalDatabase db(other);
            QFAIL("A atualização deveria ser recusada sem cópia de segurança.");
        } catch (const std::runtime_error &e) {
            QCOMPARE(QString::fromUtf8(e.what()), BackupFailure);
        }
        QVERIFY_EXCEPTION_THROWN(TestContext c(other), std::runtime_error);
        QCOMPARE(dump(other, "PRAGMA user_version"), QStringList{"2"});
        QCOMPARE(dump(other, LegacyEvents), eventsBefore);
        QCOMPARE(dump(other, AllLoans), loansBefore);
        QCOMPARE(dump(other, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('accounts','app_config')"),
                 QStringList{"0"});
    }
    void migrationFailureRollsBack() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v2.sqlite");
        QVERIFY(createV2(path));
        QVERIFY(execAll(path, {"CREATE TABLE events_v3(id INTEGER PRIMARY KEY)"}));
        const auto loansBefore = dump(path, AllLoans);
        const auto eventsBefore = dump(path, "SELECT * FROM events ORDER BY id");
        QVERIFY_EXCEPTION_THROWN(TestContext c(path), std::runtime_error);
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"2"});
        QCOMPARE(dump(path, "SELECT * FROM events ORDER BY id"), eventsBefore);
        QCOMPARE(dump(path, AllLoans), loansBefore);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('accounts','app_config')"),
                 QStringList{"0"});
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM pragma_table_info('events') WHERE name='entity_type'"),
                 QStringList{"0"});
    }
    void eventsAreAppendOnly() {
        QTemporaryDir temp;
        const auto path = temp.filePath("events.sqlite");
        TestContext cs(path);
        auto &s = cs.store;
        Fixture::add(cs, cs.session, "NB-1", "Dell", "Armário 1");
        const auto before = dump(path, "SELECT * FROM events");
        QVERIFY(!execAll(path, {"UPDATE events SET details='alterado'"}));
        QVERIFY(!execAll(path, {"UPDATE events SET happened_at='2000-01-01T00:00:00.000Z'"}));
        QVERIFY(!execAll(path, {"DELETE FROM events"}));
        QCOMPARE(dump(path, "SELECT * FROM events"), before);
        QCOMPARE(s.history().size(), 2);
    }
    void accountTriggersProtectPrincipal() {
        QTemporaryDir temp;
        const auto path = temp.filePath("accounts.sqlite");
        { LocalDatabase db(path); }
        const QString principal =
            "INSERT INTO accounts(id,username,password_verifier,role,active,admin_slot,created_at) "
            "VALUES(1,'ana','v','principal',1,1,'2026-01-01T00:00:00.000Z')";
        QVERIFY(execAll(path, {principal, "UPDATE app_config SET principal_account_id=1"}));
        QVERIFY(!execAll(path, {"UPDATE app_config SET principal_account_id=NULL"}));
        QVERIFY(!execAll(path, {"UPDATE accounts SET role='admin' WHERE id=1"}));
        QVERIFY(!execAll(path, {"UPDATE accounts SET active=0,admin_slot=NULL WHERE id=1"}));
        QVERIFY(!execAll(path, {"DELETE FROM accounts WHERE id=1"}));
        QVERIFY(!execAll(path, {"INSERT INTO accounts(username,password_verifier,role,active,admin_slot,created_at) "
                                "VALUES('bia','v','common',1,2,'2026-01-01T00:00:00.000Z')"}));
        QVERIFY(!execAll(path, {"INSERT INTO accounts(username,password_verifier,role,active,admin_slot,created_at) "
                                "VALUES('bia','v','admin',1,4,'2026-01-01T00:00:00.000Z')"}));
        QVERIFY(!execAll(path, {"INSERT INTO accounts(username,password_verifier,role,active,admin_slot,created_at) "
                                "VALUES('bia','v','admin',1,1,'2026-01-01T00:00:00.000Z')"}));
        QCOMPARE(dump(path, "SELECT username,role,active,admin_slot FROM accounts"), QStringList{"ana|principal|1|1"});
        QCOMPARE(dump(path, "SELECT principal_account_id FROM app_config"), QStringList{"1"});
    }
    void changesContractRejectsSecrets() {
        QTemporaryDir temp;
        const auto path = temp.filePath("contract.sqlite");
        LocalDatabase db(path);
        const auto attempt = [&](const QJsonObject &changes) {
            db.transaction([&] {
                db.appendEvent(systemEvent(changes));
            });
        };
        for (const auto &secret : {"password", "password_verifier", "secret", "proof", "code", "uri", "token"}) {
            QVERIFY(!LocalDatabase::changeFieldAllowlist().contains(secret, Qt::CaseInsensitive));
            QVERIFY_EXCEPTION_THROWN(attempt(LocalDatabase::change(secret, QJsonValue::Null, "valor")),
                                     std::runtime_error);
        }
        QVERIFY_EXCEPTION_THROWN(attempt(QJsonObject{{"location", "Sala 12"}}), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            attempt(QJsonObject{{"location", QJsonObject{{"de", "A"}, {"para", "B"}, {"extra", "C"}}}}),
            std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            attempt(QJsonObject{{"location", QJsonObject{{"de", QJsonObject{{"x", 1}}}, {"para", "B"}}}}),
            std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(attempt(QJsonObject{{"location", QJsonObject{{"de", QJsonArray{1}}, {"para", "B"}}}}),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(attempt(QJsonObject{{"location", QJsonObject{{"para", "B"}}}}), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(attempt(LocalDatabase::change("unknown_field", "A", "B")), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(db.appendEvent(systemEvent(LocalDatabase::change("location", "A", "B"))),
                                 std::runtime_error);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM events"), QStringList{"0"});
        attempt(LocalDatabase::change("location", QJsonValue::Null, "Sala 12"));
        attempt({});
        QCOMPARE(dump(path, "SELECT changes FROM events ORDER BY id"),
                 (QStringList{R"({"location":{"de":null,"para":"Sala 12"}})", "<null>"}));
    }
    void inventoryChangesRecorded() {
        QTemporaryDir temp;
        const auto path = temp.filePath("changes.sqlite");
        TestContext cs(path);
        auto &s = cs.store;
        Fixture::add(cs, cs.session, "NB-1", "Dell", "Armário 1");
        const auto id = s.notebooks().first().id;
        Fixture::updateLocation(cs, cs.session, id, "Sala 12");
        s.setMaintenance(cs.session, id, true);
        s.setMaintenance(cs.session, id, false);
        Fixture::checkout(cs, cs.session, id, "Ana", ResponsibleRole::Professor, "Sala 3");
        Fixture::checkin(cs, cs.session, id, "Carlos", ResponsibleRole::Assistant, "Sala 3");
        QCOMPARE(
            dump(path, "SELECT action,changes FROM events WHERE entity_type='notebook' ORDER BY id"),
            (QStringList{
                R"(Cadastro|{"asset_tag":{"de":null,"para":"NB-1"},"location":{"de":null,"para":"Armário 1"},"location_id":{"de":null,"para":1},"name":{"de":null,"para":"Dell"}})",
                R"(Localização|{"location":{"de":"Armário 1","para":"Sala 12"},"location_id":{"de":1,"para":2}})",
                R"(Manutenção|{"status":{"de":"available","para":"maintenance"}})",
                R"(Disponibilização|{"status":{"de":"maintenance","para":"available"}})",
                R"(Retirada|{"location":{"de":"Sala 12","para":"Sala 3"},"location_id":{"de":2,"para":3},"person_role":{"de":null,"para":"professor"},"status":{"de":"available","para":"loaned"}})",
                R"(Devolução|{"person_role":{"de":null,"para":"assistant"},"status":{"de":"loaned","para":"available"}})"}));
        auto expected = LocalDatabase::change("location", "Armário 1", "Sala 12");
        expected.insert("location_id", LocalDatabase::change("location_id", 1, 2).value("location_id"));
        QCOMPARE(s.history()[4].changes, expected);
        QCOMPARE(s.history()[4].details, QString("Local alterado: Armário 1 → Sala 12"));
    }
    void rollbackOnAuditFailureForAddAndMaintenance() {
        QTemporaryDir temp;
        const auto path = temp.filePath("rollback.sqlite");
        TestContext cs(path);
        auto &s = cs.store;
        Fixture::add(cs, cs.session, "NB-1", "Dell", "Armário 1");
        const auto id = s.notebooks().first().id;
        QVERIFY(execAll(path, {"CREATE TRIGGER fail_audit BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'simulated "
                               "audit failure'); END"}));
        QVERIFY_EXCEPTION_THROWN(Fixture::add(cs, cs.session, "NB-2", "HP", "Armário 2"), std::runtime_error);
        QCOMPARE(s.notebooks().size(), 1);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM notebooks WHERE asset_tag='NB-2'"), QStringList{"0"});
        QVERIFY_EXCEPTION_THROWN(s.setMaintenance(cs.session, id, true), std::runtime_error);
        QCOMPARE(s.notebooks().first().status, Status::Available);
        QCOMPARE(s.history().size(), 2);
    }
    void utcFormatPreserved() {
        QTemporaryDir temp;
        const auto path = temp.filePath("utc.sqlite");
        TestContext cs(path);
        auto &s = cs.store;
        Fixture::add(cs, cs.session, "NB-1", "Dell", "Armário 1");
        const auto raw = dump(path, "SELECT happened_at FROM events WHERE entity_type='notebook'").value(0);
        QVERIFY2(QRegularExpression(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$)").match(raw).hasMatch(),
                 qPrintable(raw));
        const auto parsed = QDateTime::fromString(raw, Qt::ISODateWithMs);
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.timeSpec(), Qt::UTC);
        QCOMPARE(s.history().first().happenedAt, parsed);
        QVERIFY(LocalDatabase::nowUtc().endsWith('Z'));
    }
    // ---- Esquema v6 (03-01-D1 option-a) ----
    void migrationFromV5ToV6() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v5.sqlite");
        QVERIFY(createV5(path));
        const QString loans = "SELECT * FROM loans ORDER BY id";
        const QString events = "SELECT * FROM events ORDER BY id";
        const QString notebooks = "SELECT * FROM notebooks ORDER BY id";
        const QString people = "SELECT * FROM people ORDER BY id";
        const QString places = "SELECT * FROM locations ORDER BY id";
        const auto loansBefore = dump(path, loans), eventsBefore = dump(path, events),
                   notebooksBefore = dump(path, notebooks), peopleBefore = dump(path, people),
                   placesBefore = dump(path, places);
        const auto lastEvent = dump(path, "SELECT MAX(id) FROM events").first();
        {
            LocalDatabase database(path);
            QVERIFY(!database.lastBackupPath().isEmpty());
        }
        for (int pass = 0; pass < 2; ++pass) {
            QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"7"});
            QCOMPARE(dump(path,
                          "SELECT "
                          "id,notebook_id,borrower,checked_out_at,returned_at,borrower_role,returned_by,returned_by_"
                          "role,borrower_person_id,borrower_subject,checkout_location_id,checkout_location,returned_by_"
                          "person_id,returned_by_subject,return_location_id,return_location FROM loans ORDER BY id"),
                     loansBefore);
            QCOMPARE(dump(path, "SELECT "
                                "id,entity_type,entity_id,notebook_id,action,details,happened_at,actor_account_id,"
                                "actor_username,changes,notebook_asset_tag,notebook_name FROM events WHERE id<=" +
                                    lastEvent + " ORDER BY id"),
                     eventsBefore);
            QCOMPARE(dump(path, notebooks), notebooksBefore);
            QCOMPARE(dump(path, people), peopleBefore);
            QCOMPARE(dump(path, places), placesBefore);
            QCOMPARE(
                dump(path,
                     "SELECT COUNT(*) FROM loans WHERE origin_location_id IS NOT NULL OR origin_location IS NOT NULL"),
                QStringList{"0"});
            QCOMPARE(dump(path, "SELECT COUNT(*) FROM events WHERE loan_id IS NOT NULL OR location_id IS NOT NULL OR "
                                "from_location_id IS NOT NULL"),
                     QStringList{"0"});
            QCOMPARE(dump(path, "SELECT changes FROM events WHERE id>" + lastEvent),
                     QStringList{R"({"schema_version":{"de":5,"para":7}})"});
            QCOMPARE(dump(path, "PRAGMA foreign_key_check"), QStringList{});
            QCOMPARE(dump(path, "PRAGMA integrity_check"), QStringList{"ok"});
            QCOMPARE(QDir(temp.filePath("backups")).entryList({"*.bak"}, QDir::Files).size(), 1);
            LocalDatabase reopened(path);
            QVERIFY(reopened.lastBackupPath().isEmpty());
        }
    }
    void backupBeforeV6() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v5.sqlite");
        QVERIFY(createV5(path));
        const auto loansBefore = dump(path, "SELECT * FROM loans ORDER BY id");
        const auto eventsBefore = dump(path, "SELECT * FROM events ORDER BY id");
        { LocalDatabase database(path); }
        const auto backups = QDir(temp.filePath("backups")).entryList({"*.pre-v7-*.bak"}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        const auto backup = QDir(temp.filePath("backups")).filePath(backups.first());
        QCOMPARE(dump(backup, "PRAGMA user_version"), QStringList{"5"});
        QCOMPARE(dump(backup, "SELECT * FROM loans ORDER BY id"), loansBefore);
        QCOMPARE(dump(backup, "SELECT * FROM events ORDER BY id"), eventsBefore);
    }
    void migrationV6FailureRollsBack() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v5.sqlite");
        QVERIFY(createV5(path));
        QVERIFY(execAll(path, {"ALTER TABLE loans ADD COLUMN origin_location TEXT"}));
        const auto eventsBefore = dump(path, "SELECT * FROM events ORDER BY id");
        QVERIFY_EXCEPTION_THROWN(LocalDatabase database(path), std::runtime_error);
        QCOMPARE(dump(path, "PRAGMA user_version"), QStringList{"5"});
        QCOMPARE(dump(path, "SELECT * FROM events ORDER BY id"), eventsBefore);
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM pragma_table_info('events') WHERE name IN "
                            "('loan_id','location_id','from_location_id')"),
                 QStringList{"0"});
        QCOMPARE(dump(path, "SELECT COUNT(*) FROM pragma_table_info('loans') WHERE name='origin_location_id'"),
                 QStringList{"0"});
    }
    void legacyRowsUntouchedByV6() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v5.sqlite");
        QVERIFY(createV5(path));
        const auto loans = dump(path, "SELECT * FROM loans ORDER BY id");
        const auto notebooks = dump(path, "SELECT * FROM notebooks ORDER BY id");
        LocalDatabase database(path);
        QCOMPARE(dump(path,
                      "SELECT "
                      "id,notebook_id,borrower,checked_out_at,returned_at,borrower_role,returned_by,returned_by_role,"
                      "borrower_person_id,borrower_subject,checkout_location_id,checkout_location,returned_by_person_"
                      "id,returned_by_subject,return_location_id,return_location FROM loans ORDER BY id"),
                 loans);
        QCOMPARE(dump(path, "SELECT * FROM notebooks ORDER BY id"), notebooks);
    }
    void loansOriginImmutable() {
        QTemporaryDir temp;
        const auto path = temp.filePath("origin.sqlite");
        QVERIFY(createV5(path));
        { LocalDatabase database(path); }
        // loan 3 aberto, loans 1 e 2 encerrados.
        for (const auto &id : {"1", "3"}) {
            QVERIFY(!execAll(path, {QString("UPDATE loans SET origin_location_id=1 WHERE id=") + id}));
            QVERIFY(!execAll(path, {QString("UPDATE loans SET origin_location='Outro' WHERE id=") + id}));
        }
        QCOMPARE(dump(path,
                      "SELECT COUNT(*) FROM loans WHERE origin_location_id IS NOT NULL OR origin_location IS NOT NULL"),
                 QStringList{"0"});
    }
    void eventsAppendOnlyAfterV6() {
        QTemporaryDir temp;
        const auto path = temp.filePath("events6.sqlite");
        QVERIFY(createV5(path));
        { LocalDatabase database(path); }
        const auto before = dump(path, "SELECT * FROM events ORDER BY id");
        QVERIFY(!execAll(path, {"UPDATE events SET loan_id=3"}));
        QVERIFY(!execAll(path, {"DELETE FROM events"}));
        QCOMPARE(dump(path, "SELECT * FROM events ORDER BY id"), before);
    }
    void eventsIndexesKeptByV6() {
        const QString objects =
            "SELECT type,name FROM sqlite_master WHERE tbl_name='events' AND type IN ('index','trigger') AND sql IS "
            "NOT NULL AND name NOT LIKE 'events_by_%' ORDER BY type,name";
        QTemporaryDir temp;
        const auto path = temp.filePath("v5.sqlite");
        QVERIFY(createV5(path));
        const auto before = dump(path, objects);
        { LocalDatabase upgraded(path); }
        QCOMPARE(dump(path, objects), before);
        const auto fresh = temp.filePath("fresh.sqlite");
        { LocalDatabase database(fresh); }
        QCOMPARE(dump(fresh, objects), before);
    }
};
QTEST_GUILESS_MAIN(StoreTests)
#include "StoreTests.moc"

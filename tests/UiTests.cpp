#include <QtTest>
#include <QtWidgets>
#include <QSqlQuery>
#include "ui/HistoryFormat.h"
#include "ui/AccessDialogs.h"
#include "ui/RecoveryDialogs.h"
#include "ui/AccountsPage.h"
#include "ui/LogsPage.h"
#include "ui/LocationsPage.h"
#include "ui/PeoplePage.h"
#include "ui/MainWindow.h"
#include "ui/PersonPickerDialog.h"
#include "ui/QuickPickField.h"
#include "ui/QrCodeView.h"
#include "application/DemoSeed.h"
#include "security/Totp.h"
#include "TestSupport.h"

class UiTests : public QObject {
    Q_OBJECT
    QPushButton *button(QWidget &parent, const QString &text) {
        for (auto *b : parent.findChildren<QPushButton *>())
            if (b->text() == text)
                return b;
        return nullptr;
    }
    void showAllLogs(QWidget &window) {
        auto *period = window.findChild<QComboBox *>("periodFilter");
        period->setCurrentIndex(period->findText("Todo o período"));
        window.findChild<QPushButton *>("applyFilters")->click();
    }
    // Escolhe na lista de sugestões a ocorrência `occurrence` do nome (homônimos são sugestões distintas).
    static void pickPerson(QDialog *dialog, const QString &name, int occurrence = 0) {
        auto *field = dialog->findChild<QuickPickField *>("movementPerson");
        QVERIFY(field);
        field->editor()->clear();
        QStringList same;
        for (const auto &label : field->suggestions())
            if (label.section(QString::fromUtf8(" · "), 0, 0) == name)
                same << label;
        QVERIFY2(occurrence < same.size(), qPrintable("Pessoa ausente das sugestões: " + name));
        field->choose(same.at(occurrence));
        QVERIFY(field->selectedId());
    }
    static void pickLocation(QDialog *dialog, const QString &place) {
        auto *field = dialog->findChild<QuickPickField *>("movementLocation");
        QVERIFY(field);
        field->choose(place);
        QVERIFY2(field->selectedId(), qPrintable("Local ausente: " + place));
    }
    static QString locationText(QDialog *dialog) {
        return dialog->findChild<QLineEdit *>("movementLocationSearch")->text();
    }
    void submitPersonMovement(const QString &person, const QString &place) {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QCOMPARE(dialog->objectName(), QString("movementDialog"));
        pickPerson(dialog, person);
        QVERIFY(dialog->findChild<QLineEdit *>("movementPersonSearch")
                    ->text()
                    .startsWith(person + QString::fromUtf8(" · ")));
        pickLocation(dialog, place);
        QApplication::processEvents();
        QVERIFY(dialog->grab().save("ui-movement.png"));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    }
    // Após fechar um modal, a plataforma pode não reativar a janela principal (offscreen); o foco é conferido com a
    // janela ativa.
    static QString scalarText(TestContext &ctx, const QString &sql) {
        QSqlQuery q(ctx.database.connection());
        if (!q.exec(sql) || !q.next())
            return QStringLiteral("ERRO");
        return q.value(0).toString();
    }
    void expectFocus(QWidget &window, QWidget *expected) {
        window.activateWindow();
        QTRY_COMPARE(QApplication::focusWidget(), expected);
    }
    // A janela fica bloqueada durante a reautenticação; age no diálogo só depois que ela é liberada.
    void whenEnabled(QWidget *widget, const std::function<void()> &action) {
        if (widget->isEnabled()) {
            action();
            return;
        }
        QTimer::singleShot(10, widget, [this, widget, action] {
            whenEnabled(widget, action);
        });
    }
    // Digita um nome sem cadastro e usa Adicionar pessoa; o diálogo de pessoa abre com o nome digitado.
    void registerFromMovement(QDialog *movement, const QString &name, ResponsibleRole role,
                              const QString &subject = {}) {
        auto *search = movement->findChild<QLineEdit *>("movementPersonSearch");
        search->setText(name);
        auto *add = movement->findChild<QPushButton *>("addPersonButton");
        QVERIFY(add->isVisible());
        QTimer::singleShot(0, this, [&, name, role, subject] {
            auto *person = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(person);
            QCOMPARE(person->objectName(), QString("personDialog"));
            QCOMPARE(person->findChild<QLineEdit *>("personName")->text(), name);
            person->findChild<QComboBox *>("personRole")->setCurrentIndex(role == ResponsibleRole::Professor ? 1 : 2);
            person->findChild<QLineEdit *>("personSubject")->setText(subject);
            QVERIFY(person->grab().save("ui-person-quick-register.png"));
            person->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        add->click();
    }
    void quickRegistrationFlow(bool returning) {
        QTemporaryDir temp;
        const auto path = temp.filePath("quick-person.sqlite");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-QUICK", "Dell", "Armário 1");
        const auto id = ctx.store.notebooks().first().id;
        createLocation(ctx, "Sala 12");
        if (returning)
            Fixture::checkout(ctx, ctx.session, id, "Ana", ResponsibleRole::Professor, "Sala 12");
        const auto common =
            ctx.createCommonAndLogin("operador", "senha-operador-segura", InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, common, path, ctx.locations, ctx.people);
        window.show();
        window.findChild<QTabWidget *>("inventoryTabs")->setCurrentIndex(returning ? 1 : 0);
        window.findChild<QTableWidget *>(returning ? "loanedTable" : "availableTable")->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            auto *movement = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(movement);
            pickLocation(movement, "Sala 12");
            registerFromMovement(movement, "Bruno Lima", ResponsibleRole::Professor, "História");
            QVERIFY(movement->findChild<QuickPickField *>("movementPerson")->selectedId());
            QVERIFY(movement->findChild<QLineEdit *>("movementPersonSearch")
                        ->text()
                        .startsWith(QString::fromUtf8("Bruno Lima · Professor · História")));
            QVERIFY(movement->findChild<QPushButton *>("addPersonButton")->isVisible());
            QCOMPARE(movement->findChild<QLabel *>("movementPersonRole")->text(), QString("Professor"));
            QCOMPARE(movement->findChild<QLabel *>("movementPersonSubject")->text(), QString("História"));
            QCOMPARE(locationText(movement), QString("Sala 12"));
            QVERIFY(movement->grab().save(returning ? "ui-quick-checkin.png" : "ui-quick-checkout.png"));
            movement->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        button(window, returning ? "Registrar devolução" : "Registrar retirada")->click();
        const auto people = ctx.people.people(CatalogFilter::Active, "Bruno Lima");
        QCOMPARE(people.size(), 1);
        const auto notebook = ctx.store.notebooks().first();
        QCOMPARE(returning ? notebook.returnedByPersonId : notebook.borrowerPersonId, people.first().id);
        const auto events = ctx.store.history();
        QCOMPARE(events[0].action, returning ? QString("Devolução") : QString("Retirada"));
        QCOMPARE(events[1].action, QString("Pessoa cadastrada"));
        QCOMPARE(events[1].actorUsername, QString("operador"));
        button(window, "  Pessoas")->click();
        auto *table = window.findChild<QTableWidget *>("peopleTable");
        QTRY_VERIFY(table->isEnabled());
        bool found = false;
        for (int r = 0; r < table->rowCount(); ++r)
            found = found || table->item(r, 0)->text() == "Bruno Lima";
        QVERIFY(found);
        TestContext reopened(path);
        QCOMPARE(reopened.people.people(CatalogFilter::Active, "Bruno Lima").first().id, people.first().id);
        QCOMPARE(returning ? reopened.store.notebooks().first().returnedByPersonId
                           : reopened.store.notebooks().first().borrowerPersonId,
                 people.first().id);
    }
    template <class T>
    static T *child(QWidget &parent, const char *name) {
        auto *found = parent.findChild<T *>(name);
        return found;
    }
    // Eventos são gravados com o horário real; o relógio da sessão acompanha para que "Hoje" os alcance.
    // Mudar o relógio de parede encerra a sessão administrativa; a principal entra de novo.
    static void syncClock(TestContext &ctx, qint64 extraSeconds = 0) {
        ctx.clock.advanceWall(ctx.clock.nowUtc().secsTo(QDateTime::currentDateTimeUtc()) + extraSeconds);
        if (ctx.auth.hasPrincipal())
            ctx.session = ctx.auth.login(TestPrincipal, TestPassword);
    }
    LogsPage *openLogs(MainWindow &window) {
        window.show();
        window.showPage("logs");
        return window.findChild<LogsPage *>("logsPage");
    }
    void pickInQuery(const std::function<bool(QTableWidget *, int)> &match) {
        QTimer::singleShot(0, this, [this, match] {
            auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(picker);
            auto *table = picker->findChild<QTableWidget *>("personPickerTable");
            for (int r = 0; r < table->rowCount(); ++r)
                if (match(table, r)) {
                    table->setCurrentCell(r, 0);
                    table->selectRow(r);
                    break;
                }
            button(*picker, "Selecionar")->click();
        });
    }
    void pressIn(QWidget &window, QWidget *target, Qt::Key key) {
        expectFocus(window, target);
        QTest::keyClick(target, key);
    }
private slots:
    void logsDraftVersusApplied() {
        QTemporaryDir dir;
        const auto path = dir.filePath("draft.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        const auto sala = createLocation(ctx, "Sala");
        const auto first = ctx.store.add(ctx.session, "NB-1", "Primeiro", sala);
        ctx.store.add(ctx.session, "NB-2", "Segundo", sala);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        QVERIFY(page);
        auto *grid = child<QTableWidget>(window, "events");
        auto *pending = child<QLabel>(window, "pendingFilters");
        child<QPushButton>(window, "filterToggle")->click();
        const int before = grid->rowCount();
        QVERIFY(before >= 3);
        QVERIFY(!pending->isVisible());
        auto *notebook = child<QComboBox>(window, "notebookFilter"),
             *place = child<QComboBox>(window, "locationFilter"), *account = child<QComboBox>(*page, "accountFilter");
        auto *type = child<QComboBox>(window, "actionFilter"), *period = child<QComboBox>(window, "periodFilter");
        notebook->setCurrentIndex(notebook->findText("NB-1 · Primeiro"));
        place->setCurrentIndex(place->findText("Sala"));
        account->setCurrentIndex(1);
        type->setCurrentIndex(type->findText("Notebook cadastrado"));
        period->setCurrentIndex(period->findText("Últimos 7 dias"));
        QCOMPARE(grid->rowCount(), before);
        QVERIFY(pending->isVisible());
        period->setCurrentIndex(period->findText("Hoje"));
        notebook->setCurrentIndex(0);
        place->setCurrentIndex(0);
        account->setCurrentIndex(0);
        type->setCurrentIndex(0);
        QVERIFY(!pending->isVisible());
        notebook->setCurrentIndex(notebook->findText("NB-1 · Primeiro"));
        QVERIFY(pending->isVisible());
        QCOMPARE(grid->rowCount(), before);
        child<QPushButton>(window, "applyFilters")->click();
        QVERIFY(!pending->isVisible());
        QVERIFY(grid->rowCount() > 0);
        QVERIFY(grid->rowCount() < before);
        for (int row = 0; row < grid->rowCount(); ++row)
            QCOMPARE(grid->item(row, 2)->text(), QString("NB-1 · Primeiro"));
        QCOMPARE(page->appliedFilter().notebookId, std::optional<qint64>(first));
    }
    void logsClearDoesNotApply() {
        QTemporaryDir dir;
        const auto path = dir.filePath("clear.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        Fixture::add(ctx, ctx.session, "NB-1", "Primeiro", "Sala");
        Fixture::add(ctx, ctx.session, "NB-2", "Segundo", "Sala");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        auto *toggle = child<QPushButton>(window, "filterToggle");
        toggle->click();
        auto *notebook = child<QComboBox>(window, "notebookFilter"), *period = child<QComboBox>(window, "periodFilter");
        notebook->setCurrentIndex(notebook->findText("NB-1 · Primeiro"));
        period->setCurrentIndex(period->findText("Últimos 7 dias"));
        child<QPushButton>(window, "applyFilters")->click();
        QCOMPARE(toggle->text(), QString("Filtros (2)"));
        auto *grid = child<QTableWidget>(window, "events");
        const int rows = grid->rowCount();
        const auto applied = page->appliedFilter();
        child<QPushButton>(window, "clearFilters")->click();
        QCOMPARE(period->currentText(), QString("Hoje"));
        QCOMPARE(notebook->currentIndex(), 0);
        expectFocus(window, period);
        QCOMPARE(grid->rowCount(), rows);
        QVERIFY(page->appliedFilter() == applied);
        QVERIFY(child<QLabel>(window, "pendingFilters")->isVisible());
        QCOMPARE(toggle->text(), QString("Filtros (2)"));
        child<QPushButton>(window, "applyFilters")->click();
        QCOMPARE(toggle->text(), QString("Filtros (1)"));
    }
    void logsEnterAppliesOnce() {
        QTemporaryDir dir;
        const auto path = dir.filePath("enter.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        Fixture::add(ctx, ctx.session, "NB-1", "Primeiro", "Sala");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        child<QPushButton>(window, "filterToggle")->click();
        auto *notebook = child<QComboBox>(window, "notebookFilter");
        auto *person = child<QLineEdit>(window, "personFilter");
        notebook->setCurrentIndex(notebook->findText("NB-1 · Primeiro"));
        const int before = page->queryCount();
        notebook->setFocus();
        pressIn(window, notebook, Qt::Key_Return);
        QCOMPARE(page->queryCount(), before + 1);
        QCOMPARE(page->appliedFilter().notebookId.has_value(), true);
        person->setFocus();
        QTest::keyClicks(person, "Ana");
        pressIn(window, person, Qt::Key_Enter);
        QCOMPARE(page->queryCount(), before + 2);
        QCOMPARE(page->appliedFilter().personText, QString("Ana"));
    }
    void logsCountRule() {
        QTemporaryDir dir;
        const auto path = dir.filePath("count.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        Fixture::add(ctx, ctx.session, "NB-1", "Primeiro", "Sala");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        openLogs(window);
        auto *toggle = child<QPushButton>(window, "filterToggle");
        auto *apply = child<QPushButton>(window, "applyFilters");
        QCOMPARE(toggle->text(), QString("Filtros (1)"));
        QCOMPARE(toggle->accessibleName(), QString("Filtros, 1 ativos"));
        auto *period = child<QComboBox>(window, "periodFilter");
        period->setCurrentIndex(period->findText("Todo o período"));
        apply->click();
        QCOMPARE(toggle->text(), QString("Filtros (0)"));
        auto *person = child<QLineEdit>(window, "personFilter");
        person->setText("   ");
        apply->click();
        QCOMPARE(toggle->text(), QString("Filtros (0)"));
        period->setCurrentIndex(period->findText("Últimos 7 dias"));
        person->setText("Ana");
        for (const char *name : {"notebookFilter", "locationFilter", "accountFilter", "actionFilter"}) {
            auto *combo = child<QComboBox>(*child<LogsPage>(window, "logsPage"), name);
            QVERIFY(combo->count() > 1);
            combo->setCurrentIndex(combo->count() - 1);
        }
        apply->click();
        QCOMPARE(toggle->text(), QString("Filtros (6)"));
        QCOMPARE(toggle->accessibleName(), QString("Filtros, 6 ativos"));
    }
    void logsCustomDateError() {
        QTemporaryDir dir;
        const auto path = dir.filePath("dates.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        child<QPushButton>(window, "filterToggle")->click();
        auto *period = child<QComboBox>(window, "periodFilter");
        period->setCurrentIndex(period->findText("Personalizado"));
        QVERIFY(child<QWidget>(window, "customDates")->isVisible());
        auto *from = child<QDateEdit>(window, "customFrom"), *to = child<QDateEdit>(window, "customTo");
        const QDate today = ctx.clock.nowUtc().toLocalTime().date();
        from->setDate(today);
        to->setDate(today.addDays(-1));
        auto *grid = child<QTableWidget>(window, "events");
        const int rows = grid->rowCount();
        const auto applied = page->appliedFilter();
        const int queries = page->queryCount();
        child<QPushButton>(window, "applyFilters")->click();
        auto *error = child<QLabel>(window, "dateError");
        QVERIFY(error->isVisible());
        QCOMPARE(error->text(), QString("A data inicial deve ser igual ou anterior à data final."));
        expectFocus(window, from);
        QVERIFY(page->appliedFilter() == applied);
        QCOMPARE(grid->rowCount(), rows);
        QCOMPARE(page->queryCount(), queries);
    }
    void logsEmptyStateVariants() {
        QTemporaryDir dir;
        {
            TestContext empty(dir.filePath("empty.sqlite"), false);
            HistoryQueryService service(empty.database);
            HistoryFilter all;
            all.period = PeriodPreset::All;
            QVERIFY(service.search(all, QDateTime::currentDateTimeUtc(), QTimeZone("UTC")).entries.isEmpty());
            LogsPage page(service, empty.auth.sessionClock(), empty.people);
            page.show();
            page.reloadApplied();
            QCOMPARE(page.findChild<QLabel *>("emptyTitle")->text(), QString("Nenhum evento hoje"));
            page.findChild<QComboBox *>("periodFilter")
                ->setCurrentIndex(page.findChild<QComboBox *>("periodFilter")->findText("Todo o período"));
            page.findChild<QPushButton *>("applyFilters")->click();
            QVERIFY(page.findChild<QFrame *>("emptyState")->isVisible());
            QVERIFY(!page.findChild<QTableWidget *>("events")->isVisible());
            QCOMPARE(page.findChild<QLabel *>("emptyTitle")->text(), QString("Nenhum evento registrado"));
            QVERIFY(page.findChild<QLabel *>("logsResult")->text().isEmpty());
        }
        const auto path = dir.filePath("past.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        Fixture::add(ctx, ctx.session, "NB-1", "Primeiro", "Sala");
        syncClock(ctx, 3 * 86400);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        openLogs(window);
        QVERIFY(child<QFrame>(window, "emptyState")->isVisible());
        QVERIFY(!child<QTableWidget>(window, "events")->isVisible());
        QCOMPARE(child<QLabel>(window, "emptyTitle")->text(), QString("Nenhum evento hoje"));
        QCOMPARE(child<QLabel>(window, "emptyBody")->text(),
                 QString("Abra Filtros, escolha um período maior, como Últimos 7 dias, e selecione Aplicar filtros."));
        auto *period = child<QComboBox>(window, "periodFilter");
        period->setCurrentIndex(period->findText("Todo o período"));
        child<QLineEdit>(window, "personFilter")->setText("zzz");
        child<QPushButton>(window, "applyFilters")->click();
        QVERIFY(child<QFrame>(window, "emptyState")->isVisible());
        QCOMPARE(child<QLabel>(window, "emptyTitle")->text(), QString("Nenhum evento encontrado"));
        child<QLineEdit>(window, "personFilter")->clear();
        child<QPushButton>(window, "applyFilters")->click();
        QVERIFY(child<QTableWidget>(window, "events")->isVisible());
        QVERIFY(!child<QFrame>(window, "emptyState")->isVisible());
        QVERIFY(child<QLabel>(window, "logsResult")->text().endsWith(" · Todo o período"));
    }
    void logsLoadErrorKeepsApplied() {
        QTemporaryDir dir;
        const auto path = dir.filePath("load-error.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        Fixture::add(ctx, ctx.session, "NB-1", "Primeiro", "Sala");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        auto *toggle = child<QPushButton>(window, "filterToggle");
        toggle->click();
        auto *period = child<QComboBox>(window, "periodFilter");
        period->setCurrentIndex(period->findText("Todo o período"));
        auto *apply = child<QPushButton>(window, "applyFilters");
        apply->click();
        QCOMPARE(toggle->text(), QString("Filtros (0)"));
        const auto applied = page->appliedFilter();
        const auto rename = [&](const QString &from, const QString &to) {
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", "ui-logs-error");
                db.setDatabaseName(path);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY2(q.exec(QString("ALTER TABLE %1 RENAME TO %2").arg(from, to)),
                         qPrintable(q.lastError().text()));
            }
            QSqlDatabase::removeDatabase("ui-logs-error");
        };
        rename("events", "events_hidden");
        auto *notebook = child<QComboBox>(window, "notebookFilter");
        notebook->setCurrentIndex(notebook->findText("NB-1 · Primeiro"));
        apply->click();
        auto *result = child<QLabel>(window, "logsResult");
        QCOMPARE(result->text(),
                 QString("Não foi possível carregar os logs. Selecione Aplicar filtros para tentar novamente."));
        QVERIFY(!child<QTableWidget>(window, "events")->isVisible());
        QVERIFY(!child<QFrame>(window, "emptyState")->isVisible());
        QCOMPARE(toggle->text(), QString("Filtros (0)"));
        QVERIFY(page->appliedFilter() == applied);
        QVERIFY(page->draftFilter().notebookId.has_value());
        QVERIFY(child<QLabel>(window, "pendingFilters")->isVisible());
        rename("events_hidden", "events");
        apply->click();
        QVERIFY(child<QTableWidget>(window, "events")->isVisible());
        QCOMPARE(toggle->text(), QString("Filtros (1)"));
    }
    void logsSevenColumnsAndSnapshots() {
        QTemporaryDir dir;
        const auto path = dir.filePath("columns.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        const auto sala = createLocation(ctx, "Sala 12"), biblioteca = createLocation(ctx, "Biblioteca");
        const auto ana = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, QString::fromUtf8("Matemática"));
        const auto id = ctx.store.add(ctx.session, "NB-1", "Dell", sala);
        ctx.store.checkout(ctx.session, id, ana, biblioteca);
        ctx.store.updateLocation(ctx.session, id, sala);
        const auto before = ctx.store.notebooks().first();
        Fixture::update(ctx, ctx.session, before, "NB-9", "Lenovo", before.location);
        for (const auto &row : ctx.locations.locations(CatalogFilter::All))
            if (row.id == biblioteca)
                ctx.locations.rename(ctx.session, row, "Biblioteca Nova");
        ctx.database.transaction([&] {
            ctx.database.appendEvent({"system",
                                      std::nullopt,
                                      std::nullopt,
                                      "Registro legado",
                                      "",
                                      std::nullopt,
                                      {},
                                      LocalDatabase::nowUtc()});
        });
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        openLogs(window);
        auto *events = child<QTableWidget>(window, "events");
        const QStringList headers{"Data e hora",     "Tipo",  "Notebook", "Pessoa responsável",
                                  "Conta operadora", "Local", "Detalhes"};
        for (int column = 0; column < headers.size(); ++column)
            QCOMPARE(events->horizontalHeaderItem(column)->text(), headers[column]);
        int checkout = 0, moved = 0, legacy = 0;
        for (int row = 0; row < events->rowCount(); ++row) {
            const auto type = events->item(row, 1)->text();
            if (type == "Retirada") {
                ++checkout;
                QCOMPARE(events->item(row, 2)->text(), QString("NB-1 · Dell"));
                QCOMPARE(events->item(row, 3)->text(), QString::fromUtf8("Ana Souza (Professor · Matemática)"));
                QCOMPARE(events->item(row, 4)->text(), TestPrincipal);
                QCOMPARE(events->item(row, 5)->text(), QString("Biblioteca"));
            } else if (type == QString::fromUtf8("Atualização de local")) {
                ++moved;
                QCOMPARE(events->item(row, 3)->text(), QString::fromUtf8("—"));
                QCOMPARE(events->item(row, 5)->text(), QString::fromUtf8("Biblioteca → Sala 12"));
            } else if (type == "Registro legado") {
                ++legacy;
                QCOMPARE(events->item(row, 4)->text(), QString::fromUtf8("Não informado"));
                QCOMPARE(events->item(row, 2)->text(), QString::fromUtf8("—"));
            }
        }
        QCOMPARE(checkout, 1);
        QCOMPARE(moved, 1);
        QCOMPARE(legacy, 1);
    }
    void logsTruncationNotice() {
        QTemporaryDir dir;
        const auto path = dir.filePath("truncated.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        ctx.database.transaction([&] {
            for (int i = 0; i < 501; ++i)
                ctx.database.appendEvent({"system",
                                          std::nullopt,
                                          std::nullopt,
                                          "Teste",
                                          QString::number(i),
                                          std::nullopt,
                                          {},
                                          LocalDatabase::nowUtc()});
        });
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        openLogs(window);
        QCOMPARE(child<QTableWidget>(window, "events")->rowCount(), 500);
        auto *notice = child<QLabel>(window, "truncatedNotice");
        QVERIFY(notice->isVisible());
        QCOMPARE(notice->text(),
                 QString("Mostrando os 500 eventos mais recentes. Refine os filtros para ver eventos anteriores."));
        QVERIFY(child<QLabel>(window, "logsResult")->text().startsWith("500 eventos · Hoje, "));
    }
    void logsTabOrderAndEsc() {
        QTemporaryDir dir;
        const auto path = dir.filePath("tab.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        Fixture::add(ctx, ctx.session, "NB-1", "Primeiro", "Sala");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        auto *toggle = child<QPushButton>(window, "filterToggle");
        toggle->click();
        auto *period = child<QComboBox>(window, "periodFilter");
        expectFocus(window, period);
        for (const char *name : {"notebookFilter", "locationFilter", "personFilter", "selectPerson", "accountFilter",
                                 "actionFilter", "clearFilters", "applyFilters", "events"}) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            QCOMPARE(QApplication::focusWidget()->objectName(), QString(name));
        }
        auto *notebook = child<QComboBox>(window, "notebookFilter");
        notebook->setCurrentIndex(notebook->findText("NB-1 · Primeiro"));
        period->setFocus();
        pressIn(window, period, Qt::Key_Escape);
        QVERIFY(!child<QFrame>(window, "filterPanel")->isVisible());
        expectFocus(window, toggle);
        QVERIFY(!toggle->isChecked());
        QVERIFY(page->draftFilter().notebookId.has_value());
    }
    void logsPreservedAcrossPages() {
        QTemporaryDir dir;
        const auto path = dir.filePath("preserve.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        const auto sala = createLocation(ctx, "Sala");
        const auto id = ctx.store.add(ctx.session, "NB-1", "Primeiro", sala);
        const auto ana = createPerson(ctx, "Ana", ResponsibleRole::Professor);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        child<QPushButton>(window, "filterToggle")->click();
        auto *notebook = child<QComboBox>(window, "notebookFilter");
        notebook->setCurrentIndex(notebook->findText("NB-1 · Primeiro"));
        auto *grid = child<QTableWidget>(window, "events");
        const int rows = grid->rowCount();
        button(window, "  Equipamentos")->click();
        button(window, "  Logs")->click();
        QVERIFY(child<QFrame>(window, "filterPanel")->isVisible());
        QCOMPARE(notebook->currentText(), QString("NB-1 · Primeiro"));
        QVERIFY(child<QLabel>(window, "pendingFilters")->isVisible());
        QVERIFY(!page->appliedFilter().notebookId);
        ctx.store.checkout(ctx.session, id, ana, sala);
        button(window, "  Equipamentos")->click();
        button(window, "  Logs")->click();
        QCOMPARE(grid->rowCount(), rows + 1);
        QVERIFY(page->draftFilter().notebookId.has_value());
    }
    void personPickerQueryMode() {
        QTemporaryDir dir;
        TestContext ctx(dir.filePath("picker.sqlite"));
        createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, QString::fromUtf8("Matemática"));
        createPerson(ctx, "Ana Souza", ResponsibleRole::Assistant);
        const auto carlos = createPerson(ctx, "Carlos Lima", ResponsibleRole::Assistant);
        ctx.people.inactivate(ctx.session, carlos);
        PersonPickerDialog query(ctx.people, {}, nullptr, PersonPickerDialog::Mode::Query);
        query.show();
        auto *status = query.findChild<QComboBox *>("personPickerStatus");
        QVERIFY(status);
        QCOMPARE(status->currentText(), QString("Todas"));
        QCOMPARE(status->itemText(1), QString("Ativas"));
        QCOMPARE(status->itemText(2), QString("Inativas"));
        QVERIFY(!query.findChild<QPushButton *>("quickRegisterPersonButton"));
        auto *table = query.findChild<QTableWidget *>("personPickerTable");
        QCOMPARE(table->horizontalHeaderItem(4)->text(), QString::fromUtf8("Situação"));
        QCOMPARE(table->rowCount(), 3);
        int anas = 0;
        for (int r = 0; r < table->rowCount(); ++r)
            anas += table->item(r, 0)->text() == "Ana Souza";
        QCOMPARE(anas, 2);
        status->setCurrentIndex(1);
        QCOMPARE(table->rowCount(), 2);
        status->setCurrentIndex(2);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 4)->text(), QString("Inativa"));
        PersonPickerDialog movement(ctx.people, [&] {
            return ctx.session;
        });
        movement.show();
        QVERIFY(!movement.findChild<QComboBox *>("personPickerStatus"));
        QVERIFY(movement.findChild<QPushButton *>("quickRegisterPersonButton"));
        QCOMPARE(movement.findChild<QTableWidget *>("personPickerTable")->rowCount(), 2);
    }
    void logsPersonSelectionAndRemove() {
        QTemporaryDir dir;
        const auto path = dir.filePath("person-filter.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        const auto sala = createLocation(ctx, "Sala");
        const auto id = ctx.store.add(ctx.session, "NB-1", "Dell", sala);
        const auto professor =
            createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, QString::fromUtf8("Matemática"));
        const auto assistant = createPerson(ctx, "Ana Souza", ResponsibleRole::Assistant);
        ctx.store.checkout(ctx.session, id, professor, sala);
        ctx.store.checkin(ctx.session, id, assistant, sala);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        child<QPushButton>(window, "filterToggle")->click();
        auto *person = child<QLineEdit>(window, "personFilter");
        auto *select = child<QPushButton>(window, "selectPerson");
        auto *remove = child<QPushButton>(window, "clearPerson");
        person->setText("ana");
        pickInQuery([](QTableWidget *t, int r) {
            return t->item(r, 1)->text() == "Professor";
        });
        select->click();
        QVERIFY(person->isReadOnly());
        QCOMPARE(person->text(), QString::fromUtf8("Ana Souza · Professor · Matemática (cadastro)"));
        QCOMPARE(child<QLabel>(window, "personHelp")->text(),
                 QString("Somente retiradas e devoluções deste cadastro."));
        QVERIFY(remove->isVisible());
        QCOMPARE(page->draftFilter().personId, std::optional<qint64>(professor));
        QVERIFY(page->draftFilter().personText.isEmpty());
        child<QPushButton>(window, "applyFilters")->click();
        auto *grid = child<QTableWidget>(window, "events");
        QCOMPARE(grid->rowCount(), 1);
        QCOMPARE(grid->item(0, 1)->text(), QString("Retirada"));
        ctx.people.inactivate(ctx.session, assistant);
        pickInQuery([](QTableWidget *t, int r) {
            return t->item(r, 4)->text() == "Inativa";
        });
        select->click();
        QVERIFY(person->text().endsWith(" (cadastro inativo)"));
        QCOMPARE(page->draftFilter().personId, std::optional<qint64>(assistant));
        remove->click();
        QVERIFY(!person->isReadOnly());
        QVERIFY(person->text().isEmpty());
        QVERIFY(!remove->isVisible());
        expectFocus(window, person);
        QVERIFY(!page->draftFilter().personId);
    }
    void logsComboMarkers() {
        QTemporaryDir dir;
        const auto path = dir.filePath("markers.sqlite");
        TestContext ctx(path);
        syncClock(ctx);
        createLocation(ctx, "Biblioteca");
        const auto old = createLocation(ctx, "Sala 9");
        ctx.locations.softDelete(ctx.session, old);
        const auto id = Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Biblioteca");
        ctx.store.inactivate(ctx.session, id);
        const auto account = ctx.accounts.createCommonAccount(
            ctx.session, "operador", ctx.auth.preparePassword(TestPassword), InitialPasswordMode::Assisted);
        ctx.accounts.inactivateCommonAccount(ctx.session, account);
        QSqlQuery q(ctx.database.connection());
        QVERIFY(q.exec(QString("INSERT INTO loans(notebook_id,borrower,checked_out_at,returned_at,checkout_location) "
                               "VALUES(%1,'José','2025-01-01T10:00:00.000Z','2025-01-01T11:00:00.000Z','Sala Antiga')")
                           .arg(id)));
        QVERIFY(q.exec(QString("INSERT INTO events(entity_type,entity_id,notebook_id,action,details,happened_at) "
                               "VALUES('notebook',%1,%1,'Retirada','histórico','2025-01-01T10:00:00.000Z')")
                           .arg(id)));
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        auto *page = openLogs(window);
        auto *notebook = child<QComboBox>(window, "notebookFilter"),
             *place = child<QComboBox>(window, "locationFilter"), *accounts = child<QComboBox>(*page, "accountFilter");
        QVERIFY(notebook->findText("NB-1 · Dell (inativo)") > 0);
        QVERIFY(accounts->findText("operador · Comum (inativa)") > 0);
        const int active = place->findText("Biblioteca"),
                  deleted = place->findText(QString::fromUtf8("Sala 9 (excluído)")),
                  legacy = place->findText("Sala Antiga (registro antigo)");
        QVERIFY(active > 0);
        QVERIFY(deleted > active + 1);
        QVERIFY(legacy > deleted + 1);
        child<QPushButton>(window, "filterToggle")->click();
        place->setCurrentIndex(legacy);
        QCOMPARE(page->draftFilter().legacyLocationText, QString("Sala Antiga"));
        QVERIFY(!page->draftFilter().locationId);
        auto *period = child<QComboBox>(window, "periodFilter");
        period->setCurrentIndex(period->findText("Todo o período"));
        child<QPushButton>(window, "applyFilters")->click();
        auto *grid = child<QTableWidget>(window, "events");
        QCOMPARE(grid->rowCount(), 1);
        QCOMPARE(grid->item(0, 5)->text(), QString("Sala Antiga"));
    }
    void logsDefaultTodayAndNotebookFilter() {
        QTemporaryDir dir;
        const auto path = dir.filePath("logs-filter.sqlite");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-001", "Primeiro", "Sala");
        Fixture::add(ctx, ctx.session, "NB-002", "Segundo", "Sala");
        ctx.clock.advanceWall(ctx.clock.nowUtc().secsTo(QDateTime::currentDateTimeUtc()));
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.showPage("logs");
        auto *toggle = window.findChild<QPushButton *>("filterToggle");
        auto *grid = window.findChild<QTableWidget *>("events");
        QCOMPARE(toggle->text(), QString("Filtros (1)"));
        const int before = grid->rowCount();
        QVERIFY(before >= 2);
        toggle->click();
        auto *combo = window.findChild<QComboBox *>("notebookFilter");
        combo->setCurrentIndex(combo->findText("NB-001 · Primeiro"));
        QCOMPARE(grid->rowCount(), before);
        window.findChild<QPushButton *>("applyFilters")->click();
        QCOMPARE(toggle->text(), QString("Filtros (2)"));
        QVERIFY(grid->rowCount() > 0);
        for (int row = 0; row < grid->rowCount(); ++row)
            QCOMPARE(grid->item(row, 2)->text(), QString("NB-001 · Primeiro"));
    }
    void maintenanceTooltipEscapesMarkup() {
        QTemporaryDir temp;
        const auto path = temp.filePath("tooltip.sqlite");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Sala 12");
        const auto id = ctx.store.notebooks().first().id;
        ctx.store.setMaintenance(ctx.session, id, true, "<b>reparo</b>");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.findChild<QTabWidget *>("inventoryTabs")->setCurrentIndex(2);
        const auto *cell = window.findChild<QTableWidget *>("maintenanceTable")->item(0, 4);
        QVERIFY(cell);
        QCOMPARE(cell->text(), QString("<b>reparo</b>"));
        QCOMPARE(cell->toolTip(), QString("<qt>&lt;b&gt;reparo&lt;/b&gt;</qt>"));
    }
    void catalogIdsVisibleAndSearchable() {
        QTemporaryDir temp;
        const auto path = temp.filePath("catalog-ids.sqlite");
        TestContext ctx(path);
        const auto personId = createPerson(ctx, "Ana ID", ResponsibleRole::Professor, "Física");
        createPerson(ctx, "Outra pessoa", ResponsibleRole::Assistant);
        const auto locationId = createLocation(ctx, "Sala ID");
        createLocation(ctx, "Outra sala");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.showPage("pessoas");
        auto *people = window.findChild<QTableWidget *>("peopleTable");
        QTRY_COMPARE(people->rowCount(), 2);
        QCOMPARE(people->horizontalHeader()->logicalIndex(0), 4);
        window.findChild<QLineEdit *>("peopleSearch")->setText(QString::number(personId));
        QTRY_COMPARE(people->rowCount(), 1);
        QCOMPARE(people->item(0, 0)->text(), QString("Ana ID"));
        QCOMPARE(people->item(0, 4)->text(), QString::number(personId));
        PersonPickerDialog picker(ctx.people, [&] {
            return ctx.session;
        });
        picker.show();
        picker.findChild<QLineEdit *>("personPickerSearch")->setText(QString::number(personId));
        auto *personTable = picker.findChild<QTableWidget *>("personPickerTable");
        QCOMPARE(personTable->rowCount(), 1);
        QCOMPARE(personTable->horizontalHeader()->logicalIndex(0), 3);
        QCOMPARE(personTable->item(0, 3)->text(), QString::number(personId));
        picker.reject();
        window.showPage("locais");
        auto *locations = window.findChild<QTableWidget *>("locationsTable");
        QTRY_COMPARE(locations->rowCount(), 2);
        QCOMPARE(locations->horizontalHeader()->logicalIndex(0), 2);
        window.findChild<QLineEdit *>("locationsSearch")->setText(QString::number(locationId));
        QTRY_COMPARE(locations->rowCount(), 1);
        QCOMPARE(locations->item(0, 0)->text(), QString("Sala ID"));
        QCOMPARE(locations->item(0, 2)->text(), QString::number(locationId));
        QVERIFY(window.grab().save("ui-location-ids.png"));
    }
    void defaultSessionDuration() {
        QTemporaryDir temp;
        TestContext ctx(temp.filePath("demo-duration.sqlite"));
        AuthService demoAuth(ctx.database, ctx.hasher, ctx.clock);
        const auto admin = demoAuth.login(TestPrincipal, TestPassword);
        QCOMPARE(demoAuth.sessionDeadlineUtc(admin), admin.issuedAtUtc().addSecs(15 * 60));
        ctx.clock.advance(15 * 60 - 1);
        QVERIFY(!demoAuth.sessionExpired(admin));
        ctx.clock.advance(1);
        QVERIFY(demoAuth.sessionExpired(admin));
    }
    void expiredSessionWithNestedDialogs_data() {
        QTest::addColumn<bool>("otherAccount");
        QTest::addColumn<bool>("byTimer");
        QTest::newRow("salvar-mesma-conta") << false << false;
        QTest::newRow("salvar-outra-conta") << true << false;
        QTest::newRow("timer-mesma-conta") << false << true;
        QTest::newRow("timer-outra-conta") << true << true;
    }
    void expiredSessionWithNestedDialogs() {
        QFETCH(bool, otherAccount);
        QFETCH(bool, byTimer);
        QTemporaryDir temp;
        const auto path = temp.filePath("nested-expiry.sqlite");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Sala 12");
        ctx.createCommonAndLogin("operador", "senha-operador-segura", InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        QSignalSpy replaced(&window, &MainWindow::sessionReplaced);
        window.findChild<QTableWidget *>("availableTable")->selectRow(0);
        const auto eventsBefore = ctx.store.history().size();
        bool sawLogin = false, closedBeforeLogin = false;
        QPointer<QDialog> movement, person;
        QTimer::singleShot(0, this, [&] {
            movement = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(movement);
            movement->findChild<QLineEdit *>("movementPersonSearch")->setText("Pessoa nova");
            QTimer::singleShot(0, this, [&] {
                person = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(person);
                person->findChild<QLineEdit *>("personName")->setText("Pessoa não deve existir");
                person->findChild<QComboBox *>("personRole")->setCurrentIndex(2);
                ctx.clock.advance(15 * 60);
                auto answerLogin = [&] {
                    auto *login = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    QVERIFY(login);
                    QVERIFY(login->findChild<QLineEdit *>("loginPassword"));
                    sawLogin = true;
                    QVERIFY(!window.isVisible());
                    closedBeforeLogin = (!movement || !movement->isVisible()) && (!person || !person->isVisible());
                    QCOMPARE(login->findChild<QLabel *>("errorText")->text(),
                             QString("Sessão administrativa expirada. Entre novamente para continuar."));
                    QCOMPARE(ctx.store.history().size(), eventsBefore);
                    QCOMPARE(scalarText(ctx, "SELECT count(*) FROM people"), QString("0"));
                    QCOMPARE(scalarText(ctx, "SELECT count(*) FROM loans"), QString("0"));
                    QVERIFY(login->grab().save("ui-nested-reauth.png"));
                    if (otherAccount)
                        login->findChild<QLineEdit *>("loginUsername")->setText("operador");
                    login->findChild<QLineEdit *>("loginPassword")
                        ->setText(otherAccount ? QString("senha-operador-segura") : QString::fromUtf8(TestPassword));
                    button(*login, "Acessar conta")->click();
                };
                if (byTimer) {
                    QTimer::singleShot(1100, this, answerLogin);
                    // Aguarda o timer de expiração (1 s) abrir o login, sem espera fixa.
                    QTRY_VERIFY_WITH_TIMEOUT(sawLogin, 5000);
                    if (person && person->isVisible())
                        person->reject();
                } else {
                    QTimer::singleShot(50, this, answerLogin);
                    person->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
                }
            });
            movement->findChild<QPushButton *>("addPersonButton")->click();
            // Mantém a etapa RED finita, mesmo se a versão anterior deixar a movimentação aberta.
            if (movement && movement->isVisible())
                movement->reject();
        });
        button(window, "Registrar retirada")->click();
        QVERIFY(sawLogin);
        QVERIFY(closedBeforeLogin);
        QCOMPARE(scalarText(ctx, "SELECT count(*) FROM people"), QString("0"));
        QCOMPARE(scalarText(ctx, "SELECT count(*) FROM loans"), QString("0"));
        QCOMPARE(ctx.store.history().size(), eventsBefore);
        QCOMPARE(replaced.count(), otherAccount ? 1 : 0);
        if (otherAccount) {
            const auto fresh = qvariant_cast<Session>(replaced.first().first());
            MainWindow replacement(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, fresh, path, ctx.locations,
                                   ctx.people);
            replacement.show();
            QCOMPARE(replacement.activeSession().username(), QString("operador"));
            QCOMPARE(replacement.findChild<QTableWidget *>("availableTable")->rowCount(), 1);
        } else {
            QVERIFY(window.isVisible());
            QVERIFY(window.findChild<QTableWidget *>("availableTable")->isEnabled());
            QCOMPARE(window.findChild<QTableWidget *>("availableTable")->rowCount(), 1);
        }
    }
    void quickRegisterReturnsSelected() { quickRegistrationFlow(false); }
    void quickRegisterInCheckin() { quickRegistrationFlow(true); }
    void quickRegisterHomonymWarning() {
        QTemporaryDir temp;
        TestContext ctx(temp.filePath("homonym.sqlite"));
        const auto existing = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor);
        PersonPickerDialog picker(ctx.people, [&] {
            return ctx.session;
        });
        QTimer::singleShot(0, this, [&] {
            auto *create = picker.findChild<QPushButton *>("quickRegisterPersonButton");
            if (!create) {
                picker.reject();
                QFAIL("Cadastro rápido ausente");
            }
            QTimer::singleShot(0, this, [&] {
                auto *person = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(person);
                auto *name = person->findChild<QLineEdit *>("personName");
                name->setText("ana souza");
                auto *role = person->findChild<QComboBox *>("personRole");
                role->setCurrentIndex(2);
                auto *save = person->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
                QTimer::singleShot(0, this, [&] {
                    auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    QVERIFY(confirm);
                    QCOMPARE(confirm->text(), QString("Já existe Ana Souza (Professor). Cadastrar mesmo assim?"));
                    QCOMPARE(confirm->textFormat(), Qt::PlainText);
                    button(*confirm, "Cancelar")->click();
                });
                save->click();
                QCOMPARE(name->text(), QString("ana souza"));
                QCOMPARE(role->currentIndex(), 2);
                QCOMPARE(ctx.people.activePeople().size(), 1);
                QTimer::singleShot(0, this, [&] {
                    auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    QVERIFY(confirm);
                    button(*confirm, "Cadastrar pessoa")->click();
                });
                save->click();
            });
            create->click();
        });
        QCOMPARE(picker.exec(), int(QDialog::Accepted));
        QVERIFY(picker.selectedPerson());
        QVERIFY(picker.selectedPerson()->id != existing);
        QCOMPARE(ctx.people.activePeople().size(), 2);
    }
    void quickRegisterCancelKeepsForms() {
        QTemporaryDir temp;
        const auto path = temp.filePath("cancel-quick.sqlite");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Sala 12");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.findChild<QTableWidget *>("availableTable")->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            auto *movement = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(movement);
            auto *search = movement->findChild<QLineEdit *>("movementPersonSearch");
            search->setText("bru");
            QTimer::singleShot(0, this, [&] {
                auto *person = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(person);
                person->reject();
            });
            movement->findChild<QPushButton *>("addPersonButton")->click();
            QVERIFY(movement->isVisible());
            QCOMPARE(search->text(), QString("bru"));
            QCOMPARE(locationText(movement), QString("Sala 12"));
            QVERIFY(!movement->findChild<QuickPickField *>("movementPerson")->selectedId());
            registerFromMovement(movement, "Bruno persistente", ResponsibleRole::Assistant);
            movement->reject();
        });
        button(window, "Registrar retirada")->click();
        QCOMPARE(scalarText(ctx, "SELECT count(*) FROM loans"), QString("0"));
        TestContext reopened(path);
        QCOMPARE(reopened.people.people(CatalogFilter::Active, "Bruno persistente").size(), 1);
        QCOMPARE(reopened.store.history().first().action, QString("Pessoa cadastrada"));
    }
    void peoplePageUsesSharedPersonDialog() {
        QTemporaryDir temp;
        const auto path = temp.filePath("shared-person.sqlite");
        TestContext ctx(path);
        createPerson(ctx, "Ana", ResponsibleRole::Professor);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        button(window, "  Pessoas")->click();
        auto *table = window.findChild<QTableWidget *>("peopleTable");
        QTRY_VERIFY(table->isEnabled());
        table->selectRow(0);
        for (const auto &text : {QString("Cadastrar pessoa"), QString("Editar pessoa")}) {
            QTimer::singleShot(0, this, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                const auto name = dialog->objectName();
                dialog->reject();
                QCOMPARE(name, QString("personDialog"));
            });
            button(window, text)->click();
        }
    }
    void commonSessionHidesInactivatePerson() {
        QTemporaryDir temp;
        const auto path = temp.filePath("people-common.sqlite");
        TestContext ctx(path);
        ctx.people.create(ctx.session, {"Ana", ResponsibleRole::Professor, {}});
        const auto common =
            ctx.createCommonAndLogin("operador", "senha-operador-segura", InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, common, path, ctx.locations, ctx.people);
        window.show();
        window.showPage("pessoas");
        auto *table = window.findChild<QTableWidget *>("peopleTable");
        QTRY_COMPARE(table->rowCount(), 1);
        table->selectRow(0);
        QVERIFY(!button(window, "Inativar pessoa"));
        QVERIFY(button(window, "Editar pessoa")->isEnabled());
    }
    void peoplePageFlow() {
        QTemporaryDir temp;
        const auto path = temp.filePath("people-ui.sqlite");
        TestContext ctx(path);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.showPage("pessoas");
        auto *table = window.findChild<QTableWidget *>("peopleTable");
        QVERIFY(table);
        auto *page = static_cast<PeoplePage *>(table->parentWidget());
        auto *state = window.findChild<QLabel *>("peopleState");
        auto *create = window.findChild<QPushButton *>("peopleCreate");
        auto *edit = button(window, "Editar pessoa");
        auto *remove = button(window, "Inativar pessoa");
        QVERIFY(create && edit && remove);
        page->reload();
        QCOMPARE(state->text(), QString("Carregando cadastros…"));
        QTRY_VERIFY(create->isEnabled());
        QTRY_VERIFY(window.findChild<QLabel *>("peopleEmptyBody")->isVisible());
        QVERIFY(!edit->isEnabled());
        QVERIFY(!remove->isEnabled());
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        create->click();
        expectFocus(window, create);
        QVERIFY(ctx.people.activePeople().isEmpty());
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>("personName");
            auto *role = dialog->findChild<QComboBox *>("personRole");
            auto *subject = dialog->findChild<QLineEdit *>("personSubject");
            auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
            save->click();
            QCOMPARE(dialog->findChild<QLabel *>("errorText")->text(),
                     QString("Preencha os campos obrigatórios indicados com *."));
            QCOMPARE(dialog->focusWidget(), static_cast<QWidget *>(name));
            name->setText("Ana Souza");
            save->click();
            QCOMPARE(dialog->findChild<QLabel *>("errorText")->text(), QString("Selecione Professor ou Auxiliar."));
            role->setCurrentIndex(1);
            QVERIFY(subject->isEnabled());
            subject->setText("Matemática");
            role->setCurrentIndex(2);
            QVERIFY(!subject->isEnabled());
            QCOMPARE(subject->text(), QString("Matemática"));
            role->setCurrentIndex(1);
            QVERIFY(subject->isEnabled());
            save->click();
        });
        create->click();
        QTRY_COMPARE(table->rowCount(), 1);
        QTRY_VERIFY(create->isEnabled());
        QCOMPARE(table->item(0, 2)->text(), QString("Matemática"));
        bool homonymCancelled = false, homonymAccepted = false;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>("personName");
            name->setText("ana souza");
            dialog->findChild<QComboBox *>("personRole")->setCurrentIndex(2);
            auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
            QTimer::singleShot(0, this, [&] {
                auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(confirm);
                QCOMPARE(confirm->text(), QString("Já existe Ana Souza (Professor). Cadastrar mesmo assim?"));
                QCOMPARE(confirm->textFormat(), Qt::PlainText);
                QCOMPARE(confirm->defaultButton()->text(), QString("Cancelar"));
                button(*confirm, "Cancelar")->click();
                homonymCancelled = true;
            });
            save->click();
            QVERIFY(homonymCancelled);
            QCOMPARE(name->text(), QString("ana souza"));
            QCOMPARE(ctx.people.activePeople().size(), 1);
            QTimer::singleShot(0, this, [&] {
                auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(confirm);
                button(*confirm, "Cadastrar pessoa")->click();
                homonymAccepted = true;
            });
            save->click();
        });
        create->click();
        QVERIFY(homonymAccepted);
        QTRY_COMPARE(table->rowCount(), 2);
        QTRY_VERIFY(table->isEnabled());
        int professorRow = -1;
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 1)->text() == "Professor")
                professorRow = row;
        QVERIFY(professorRow >= 0);
        const auto editedId = table->item(professorRow, 0)->data(Qt::UserRole).toLongLong();
        table->selectRow(professorRow);
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), QString("Editar pessoa"));
            dialog->findChild<QComboBox *>("personRole")->setCurrentIndex(2);
            QVERIFY(!dialog->findChild<QLineEdit *>("personSubject")->isEnabled());
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        edit->click();
        QTRY_VERIFY(table->isEnabled());
        QTRY_COMPARE(table->item(table->currentRow(), 2)->text(), QString("—"));
        QCOMPARE(table->item(table->currentRow(), 0)->data(Qt::UserRole).toLongLong(), editedId);
        bool confirmed = false;
        QTimer::singleShot(0, this, [&] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(confirm);
            QCOMPARE(confirm->text(), QString("Inativar Ana Souza? A pessoa deixará de aparecer nas novas seleções. O "
                                              "histórico será preservado."));
            QCOMPARE(confirm->defaultButton()->text(), QString("Cancelar"));
            QCOMPARE(confirm->textFormat(), Qt::PlainText);
            button(*confirm, "Inativar pessoa")->click();
            confirmed = true;
        });
        remove->click();
        QVERIFY(confirmed);
        QTRY_COMPARE(table->rowCount(), 1);
        QTRY_VERIFY(table->isEnabled());
        QVERIFY(
            window.findChild<QLabel *>("peopleFeedback")->text().contains("O cadastro não aparece no filtro atual."));
        window.findChild<QComboBox *>("peopleFilter")->setCurrentIndex(1);
        QTRY_VERIFY(table->isEnabled());
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toLongLong(), editedId);
        QCOMPARE(table->item(0, 3)->text(), QString("Inativa"));
        window.findChild<QLineEdit *>("peopleSearch")->setText("ninguém");
        QTRY_VERIFY(table->isEnabled());
        QTRY_COMPARE(table->rowCount(), 0);
        QCOMPARE(state->text(),
                 QString("Nenhum resultado para os filtros atuais. Revise a busca ou limpe os filtros."));
    }
    void notebookEditFlow() {
        QTemporaryDir temp;
        const auto path = temp.filePath("notebook-edit.sqlite");
        TestContext ctx(path);
        ctx.locations.create(ctx.session, "Sala 12");
        Fixture::add(ctx, ctx.session, "NB-001", "Dell", "Sala 12");
        const auto id = ctx.store.notebooks().first().id;
        Fixture::checkout(ctx, ctx.session, id, "Ana", ResponsibleRole::Professor, "Sala 12");
        Fixture::checkin(ctx, ctx.session, id, "Carlos", ResponsibleRole::Assistant, "Sala 12");
        const auto common =
            ctx.createCommonAndLogin("operador", "senha-operador-segura", InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, common, path, ctx.locations, ctx.people);
        window.show();
        auto *inventory = window.findChild<QTableWidget *>("availableTable");
        auto *edit = button(window, "Editar notebook");
        QVERIFY(inventory && edit);
        QVERIFY(!edit->isEnabled());
        QVERIFY(!button(window, "Inativar notebook"));
        QVERIFY(!button(window, "Reativar notebook"));
        inventory->selectRow(0);
        QVERIFY(edit->isEnabled());
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), QString("Editar notebook"));
            QCOMPARE(dialog->findChild<QLineEdit *>("notebookAssetTag")->text(), QString("NB-001"));
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        edit->click();
        expectFocus(window, edit);
        QCOMPARE(ctx.store.notebooks().first().assetTag, QString("NB-001"));
        inventory->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
            QCOMPARE(save->text(), QString("Salvar alterações"));
            QCOMPARE(dialog->findChild<QComboBox *>("notebookLocation")->currentText(), QString("Sala 12"));
            dialog->findChild<QLineEdit *>("notebookAssetTag")->setText("NB-101");
            dialog->findChild<QLineEdit *>("notebookName")->setText("Dell 5420");
            save->click();
        });
        edit->click();
        QCOMPARE(inventory->rowCount(), 1);
        QCOMPARE(inventory->item(0, 0)->text(), QString("NB-101"));
        QCOMPARE(inventory->item(0, 1)->text(), QString("Dell 5420"));
        QVERIFY(inventory->item(0, 0)->isSelected());
        QVERIFY(window.findChild<QLabel *>("feedback")->text().contains("Notebook atualizado."));
        button(window, "  Logs")->click();
        showAllLogs(window);
        auto *events = window.findChild<QTableWidget *>("events");
        bool found = false;
        int old = 0;
        for (int row = 0; row < events->rowCount(); ++row) {
            if (events->item(row, 1)->text() == "Notebook atualizado") {
                found = events->item(row, 6)->text().contains("Patrimônio: NB-001 → NB-101");
                QCOMPARE(events->item(row, 2)->text(), QString("NB-101 · Dell 5420"));
            }
            if (events->item(row, 1)->text() == "Retirada" || events->item(row, 1)->text() == "Devolução") {
                ++old;
                QCOMPARE(events->item(row, 2)->text(), QString("NB-001 · Dell"));
            }
        }
        QVERIFY(found);
        QCOMPARE(old, 2);
    }
    void notebookInactivateFlow() {
        QTemporaryDir temp;
        const auto path = temp.filePath("notebook-inactivate.sqlite");
        TestContext ctx(path);
        ctx.accounts.createAdministrator(ctx.session, "administradora", ctx.auth.preparePassword("senha-admin-segura"),
                                         InitialPasswordMode::Assisted);
        const auto admin = ctx.auth.login("administradora", "senha-admin-segura");
        {
            MainWindow empty(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, admin, path, ctx.locations, ctx.people);
            empty.show();
            QVERIFY(empty.findChild<QLabel *>("inventoryEmptyTitle")->isVisible());
            QCOMPARE(empty.findChild<QLabel *>("inventoryEmptyTitle")->text(), QString("Nenhum notebook cadastrado"));
            QCOMPARE(empty.findChild<QLabel *>("inventoryEmptyBody")->text(),
                     QString("Selecione Cadastrar notebook para adicionar o primeiro equipamento."));
            QCOMPARE(empty.findChild<QLabel *>("inventorySummary")->text(), QString("0 notebooks"));
        }
        Fixture::add(ctx, ctx.session, "NB-UI", "Notebook de teste", "Sala 12");
        Fixture::add(ctx, ctx.session, "NB-EMP", "Emprestado", "Sala 12");
        qint64 loanedId = 0;
        for (const auto &n : ctx.store.notebooks())
            if (n.assetTag == "NB-EMP")
                loanedId = n.id;
        Fixture::checkout(ctx, ctx.session, loanedId, "Ana", ResponsibleRole::Professor, "Sala 12");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, admin, path, ctx.locations, ctx.people);
        window.show();
        auto *inventory = window.findChild<QTableWidget *>("availableTable");
        auto *summary = window.findChild<QLabel *>("inventorySummary");
        auto *activity = window.findChild<QComboBox *>("activityFilter");
        auto *reason = window.findChild<QLabel *>("inactivateReason");
        auto *inactivate = button(window, "Inativar notebook");
        auto *reactivate = button(window, "Reativar notebook");
        QVERIFY(inventory && summary && activity && reason && inactivate && reactivate);
        QVERIFY(!inactivate->isHidden());
        QVERIFY(reactivate->isHidden());
        QVERIFY(!window.findChild<QLabel *>("inventoryEmptyTitle")->isVisible());
        QCOMPARE(activity->currentText(), QString("Ativos"));
        QCOMPARE(summary->text(), QString("1 notebook"));
        auto *tabs = window.findChild<QTabWidget *>("inventoryTabs");
        auto *loanedGrid = window.findChild<QTableWidget *>("loanedTable");
        QVERIFY(tabs && loanedGrid);
        auto rowOf = [&](const QString &tag) {
            for (int r = 0; r < inventory->rowCount(); ++r)
                if (inventory->item(r, 0)->text() == tag)
                    return r;
            return -1;
        };
        tabs->setCurrentIndex(1);
        QCOMPARE(loanedGrid->rowCount(), 1);
        loanedGrid->selectRow(0);
        QVERIFY(!inactivate->isEnabled());
        QVERIFY(!reason->isHidden());
        QCOMPARE(reason->text(), QString("Somente notebooks disponíveis podem ser inativados."));
        QCOMPARE(inactivate->toolTip(), reason->text());
        tabs->setCurrentIndex(0);
        inventory->selectRow(rowOf("NB-UI"));
        QVERIFY(inactivate->isEnabled());
        QVERIFY(reason->isHidden());
        QString question;
        QTimer::singleShot(0, this, [&] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(confirm);
            question = confirm->text();
            QCOMPARE(confirm->defaultButton()->text(), QString("Cancelar"));
            QCOMPARE(confirm->escapeButton()->text(), QString("Cancelar"));
            QCOMPARE(confirm->textFormat(), Qt::PlainText);
            button(*confirm, "Inativar notebook")->click();
        });
        inactivate->click();
        QCOMPARE(question, QString("Inativar o notebook NB-UI · Notebook de teste? Ele deixará de aparecer nas opções "
                                   "operacionais. O histórico será preservado."));
        QVERIFY(window.findChild<QLabel *>("feedback")->text().contains("Notebook inativado."));
        QCOMPARE(rowOf("NB-UI"), -1);
        QCOMPARE(summary->text(), QString("0 notebooks"));
        activity->setCurrentIndex(1);
        QCOMPARE(summary->text(), QString("1 notebook"));
        QCOMPARE(rowOf("NB-UI"), 0);
        QCOMPARE(inventory->item(0, 1)->text(), QString("Notebook de teste · Inativo"));
        inventory->selectRow(0);
        QVERIFY(!button(window, "Editar notebook")->isEnabled());
        QVERIFY(!button(window, "Registrar retirada")->isEnabled());
        QVERIFY(!button(window, "Atualizar local")->isEnabled());
        QVERIFY(!button(window, "Enviar à manutenção")->isEnabled());
        QVERIFY(!inactivate->isEnabled());
        // Inativo selecionado: só "Reativar" aparece; os dois nunca ficam visíveis juntos.
        QVERIFY(inactivate->isHidden());
        QVERIFY(!reactivate->isHidden());
        QVERIFY(reactivate->isEnabled());
        QVERIFY(window.grab().save("ui-equipamentos-reativar.png"));
        activity->setCurrentIndex(2);
        QCOMPARE(summary->text(), QString("1 notebook"));
        QCOMPARE(loanedGrid->rowCount(), 1);
        window.findChild<QLineEdit *>("inventorySearch")->setText("ninguém");
        QCOMPARE(summary->text(), QString("0 notebooks"));
        QCOMPARE(window.findChild<QLabel *>("metricValue")->text(), QString("1"));
        QVERIFY(window.grab().save("ui-equipamentos-inativos.png"));
        window.findChild<QLineEdit *>("inventorySearch")->clear();
        inventory->selectRow(rowOf("NB-UI"));
        QVERIFY(reactivate->isEnabled());
        QVERIFY(inactivate->isHidden());
        // Cancelar mantém o notebook inativo.
        QTimer::singleShot(0, this, [&] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(confirm);
            button(*confirm, "Cancelar")->click();
        });
        reactivate->click();
        QVERIFY(!ctx.store.notebooks().isEmpty());
        for (const auto &n : ctx.store.notebooks())
            if (n.assetTag == "NB-UI")
                QVERIFY(!n.active);
        QString reactivateQuestion;
        QTimer::singleShot(0, this, [&] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(confirm);
            reactivateQuestion = confirm->text();
            QCOMPARE(confirm->defaultButton()->text(), QString("Cancelar"));
            QCOMPARE(confirm->textFormat(), Qt::PlainText);
            button(*confirm, "Reativar notebook")->click();
        });
        reactivate->click();
        QCOMPARE(reactivateQuestion, QString("Reativar o notebook NB-UI · Notebook de teste? Ele voltará a aparecer "
                                             "nas opções operacionais com o status atual."));
        QVERIFY(window.findChild<QLabel *>("feedback")->text().contains("Notebook reativado."));
        for (const auto &n : ctx.store.notebooks())
            if (n.assetTag == "NB-UI")
                QVERIFY(n.active);
        inventory->selectRow(rowOf("NB-UI"));
        QCOMPARE(inventory->item(rowOf("NB-UI"), 1)->text(), QString("Notebook de teste"));
        QVERIFY(!inactivate->isHidden());
        QVERIFY(inactivate->isEnabled());
        QVERIFY(reactivate->isHidden());
        QCOMPARE(ctx.store.history().first().action, QString("Notebook reativado"));
        activity->setCurrentIndex(0);
        QCOMPARE(summary->text(), QString("1 notebook"));
    }
    void newFieldsRenderedInPortuguese() {
        QCOMPARE(describeChanges(LocalDatabase::change("location_deleted", 0, 1)),
                 QString("Situação do local: Ativo → Excluído"));
        QCOMPARE(describeChanges(LocalDatabase::change("person_role", "professor", "assistant")),
                 QString("Função: Professor → Auxiliar"));
        QCOMPARE(describeChanges(LocalDatabase::change("subject", "Matemática", QJsonValue::Null)),
                 QString("Matéria: Matemática → —"));
        QCOMPARE(describeChanges(LocalDatabase::change("notebook_active", 1, 0)),
                 QString("Situação do cadastro: Ativo → Inativo"));
        auto changes = LocalDatabase::change("location", "Sala 12", "Biblioteca");
        changes.insert("location_id", LocalDatabase::change("location_id", 12, 98).value("location_id"));
        QCOMPARE(describeChanges(changes), QString("Local: Sala 12 → Biblioteca"));
        QCOMPARE(describeChanges(LocalDatabase::change("location_deleted", QJsonValue::Null, 1)),
                 QString("Situação do local: — → Excluído"));
    }
    void locationsEditDeleteFlow() {
        QTemporaryDir temp;
        const auto path = temp.filePath("locations-actions.sqlite");
        TestContext ctx(path);
        const auto id = ctx.locations.create(ctx.session, "Sala 12");
        Fixture::add(ctx, ctx.session, "NB-1", "Um", "Sala 12");
        Fixture::add(ctx, ctx.session, "NB-2", "Dois", "Sala 12");
        const auto common =
            ctx.createCommonAndLogin("operador", "senha-operador-segura", InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, common, path, ctx.locations, ctx.people);
        window.show();
        window.showPage("locais");
        auto *table = window.findChild<QTableWidget *>("locationsTable");
        auto *feedback = window.findChild<QLabel *>("locationsFeedback");
        auto *edit = window.findChild<QPushButton *>("locationsEdit");
        auto *remove = button(window, "Excluir local");
        QVERIFY(table && feedback && edit && remove);
        QTRY_VERIFY(table->isEnabled());
        QVERIFY(!edit->isEnabled());
        QVERIFY(!remove->isEnabled());
        table->selectRow(0);
        QVERIFY(edit->isEnabled());
        QTimer::singleShot(0, this, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), QString("Editar local"));
            dialog->findChild<QLineEdit *>("locationName")->setText("Sala 12B");
            QCOMPARE(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->text(),
                     QString("Salvar alterações"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        edit->click();
        QTRY_COMPARE(table->item(0, 0)->text(), QString("Sala 12B"));
        QCOMPARE(feedback->text(), QString("Local atualizado."));
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        edit->click();
        expectFocus(window, edit);
        QString question;
        QTimer::singleShot(0, this, [&] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(confirm);
            question = confirm->text();
            QCOMPARE(confirm->textFormat(), Qt::PlainText);
            QCOMPARE(confirm->defaultButton()->text(), QString("Cancelar"));
            QCOMPARE(confirm->escapeButton()->text(), QString("Cancelar"));
            button(*confirm, "Cancelar")->click();
        });
        remove->click();
        QVERIFY(question.contains("2 notebooks ainda estão neste local."));
        QVERIFY(!ctx.locations.locations().first().deleted);
        expectFocus(window, remove);
        QTimer::singleShot(0, this, [&] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(confirm);
            button(*confirm, "Excluir local")->click();
        });
        remove->click();
        QTRY_COMPARE(table->rowCount(), 0);
        QCOMPARE(feedback->text(), QString("Local excluído."));
        QCOMPARE(ctx.locations.notebooksAt(id), 2);
        auto *inventory = window.findChild<QTableWidget *>("availableTable");
        QCOMPARE(inventory->rowCount(), 2);
        QCOMPARE(inventory->item(0, 2)->text(), QString("Sala 12B (excluído)"));
        QCOMPARE(inventory->item(1, 2)->text(), QString("Sala 12B (excluído)"));
        auto *filter = window.findChild<QComboBox *>("locationsFilter");
        filter->setCurrentIndex(1);
        QTRY_COMPARE(table->rowCount(), 1);
        table->selectRow(0);
        QVERIFY(!remove->isEnabled());
        QVERIFY(!edit->isEnabled());
    }
    void locationsEditDeleteExpiredAdmin_data() {
        QTest::addColumn<bool>("deleting");
        QTest::newRow("editar") << false;
        QTest::newRow("excluir") << true;
    }
    void locationsEditDeleteExpiredAdmin() {
        QFETCH(bool, deleting);
        QTemporaryDir temp;
        const auto path = temp.filePath("locations-action-expiry.sqlite");
        TestContext ctx(path);
        ctx.locations.create(ctx.session, "Sala 12");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.showPage("locais");
        auto *table = window.findChild<QTableWidget *>("locationsTable");
        QTRY_COMPARE(table->rowCount(), 1);
        QTRY_VERIFY(table->isEnabled());
        table->selectRow(0);
        const auto before = ctx.store.history().size();
        bool verified = false;
        QString error;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            ctx.clock.advance(15 * 60);
            QTimer::singleShot(20, this, [&, dialog] {
                auto *login = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(login && login != dialog);
                error = login->findChild<QLabel *>("errorText")->text();
                QCOMPARE(ctx.store.history().size(), before);
                QCOMPARE(ctx.locations.locations().first().name, QString("Sala 12"));
                login->findChild<QLineEdit *>("loginPassword")->setText(QString::fromUtf8(TestPassword));
                button(*login, "Acessar conta")->click();
                QTRY_VERIFY(login->isHidden());
                QCOMPARE(ctx.store.history().size(), before);
                QCOMPARE(ctx.locations.locations().first().name, QString("Sala 12"));
                verified = true;
                if (!deleting)
                    whenEnabled(dialog, [dialog] {
                        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
                    });
            });
            if (deleting) {
                auto *confirm = qobject_cast<QMessageBox *>(dialog);
                QVERIFY(confirm);
                button(*confirm, "Excluir local")->click();
            } else {
                dialog->findChild<QLineEdit *>("locationName")->setText("Sala modificada");
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            }
        });
        button(window, deleting ? "Excluir local" : "Editar local")->click();
        QVERIFY(verified);
        QCOMPARE(error, QString("Sessão administrativa expirada. Entre novamente para continuar."));
        QCOMPARE(ctx.store.history().size(), before);
        QCOMPARE(ctx.locations.locations().size(), 1);
        QCOMPARE(ctx.locations.locations().first().name, QString("Sala 12"));
    }
    void movementComboOffersOnlyActive() {
        QTemporaryDir temp;
        const auto path = temp.filePath("completer.sqlite");
        TestContext ctx(path);
        ctx.locations.create(ctx.session, "Biblioteca");
        const auto room = ctx.locations.create(ctx.session, "Sala 12");
        ctx.locations.softDelete(ctx.session, room);
        Fixture::add(ctx, ctx.session, "NB-1", "Um", "Biblioteca");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.findChild<QTableWidget *>("availableTable")->selectRow(0);
        bool checked = false;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *field = dialog->findChild<QuickPickField *>("movementLocation");
            QVERIFY(field);
            QCOMPARE(field->entryNames(), (QStringList{"Biblioteca"}));
            QCOMPARE(locationText(dialog), QString("Biblioteca"));
            field->editor()->setText("Sala 12");
            QVERIFY(!field->selectedId());
            auto *status = field->findChild<QLabel *>("quickPickStatus");
            QCOMPARE(status->text(), QString("Local não encontrado. Use Adicionar local."));
            QCOMPARE(status->property("tone").toString(), QString("error"));
            QVERIFY(dialog->findChild<QPushButton *>("addLocationButton")->isVisible());
            checked = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        });
        button(window, "Registrar retirada")->click();
        QVERIFY(checked);
    }
    void layoutFitsNotebookAndDesktop() {
        QTemporaryDir temp;
        const auto path = temp.filePath("layout.sqlite");
        TestContext ctx(path, false);
        const auto session =
            seedDemoDataset150(ctx.auth, ctx.store, ctx.accounts, DataProfile::Demo, ctx.locations, ctx.people);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, session, path, ctx.locations, ctx.people);
        window.show();
        QCOMPARE(window.minimumSize(), QSize(980, 640));
        for (const QSize size : {QSize(1366, 690), QSize(980, 640), QSize(1920, 1010)}) {
            window.resize(size);
            QApplication::processEvents();
            QCOMPARE(window.size(), size);
            QCOMPARE(window.isCompact(), size.height() < MainWindow::CompactHeight);
            for (const QString page : {"equipamentos", "pessoas", "locais", "logs", "logs-filtros", "contas"}) {
                window.showPage(page);
                QApplication::processEvents();
                QStringList squeezed;
                for (auto *widget : window.findChildren<QWidget *>()) {
                    if (!widget->isVisible() || !widget->layout() ||
                        qobject_cast<QAbstractScrollArea *>(widget->parentWidget()))
                        continue;
                    const auto minimum = widget->layout()->minimumSize();
                    if (widget->height() < minimum.height() || widget->width() < minimum.width())
                        squeezed << QString("%1 %2x%3 < %4x%5")
                                        .arg(widget->objectName().isEmpty() ? widget->metaObject()->className()
                                                                            : widget->objectName())
                                        .arg(widget->width())
                                        .arg(widget->height())
                                        .arg(minimum.width())
                                        .arg(minimum.height());
                }
                QVERIFY2(squeezed.isEmpty(), qPrintable(page + ": " + squeezed.join("; ")));
                QVERIFY(window.grab().save(QString("ui-layout-%1-%2.png").arg(size.width()).arg(page)));
            }
            window.showPage("equipamentos");
            QApplication::processEvents();
            auto *grid = window.findChild<QTableWidget *>("availableTable");
            if (size.height() >= 690)
                QVERIFY2(grid->viewport()->height() / grid->rowHeight(0) >= 6,
                         qPrintable(QString::number(grid->viewport()->height())));
        }
    }
    void movementQuickSearchAndAddLocation() {
        QTemporaryDir temp;
        const auto path = temp.filePath("quick-search.sqlite");
        TestContext ctx(path);
        const auto jose = createPerson(ctx, QString::fromUtf8("José Antônio"), ResponsibleRole::Professor,
                                       QString::fromUtf8("Matemática"));
        createPerson(ctx, "Maria Lima", ResponsibleRole::Assistant);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Sala 12");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.findChild<QTableWidget *>("availableTable")->selectRow(0);
        QStringList seen;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *person = dialog->findChild<QuickPickField *>("movementPerson");
            auto *search = person->editor();
            auto *personStatus = person->findChild<QLabel *>("quickPickStatus");
            // Enter nos campos registra a movimentação; os botões de cadastro só abrem por clique.
            QVERIFY(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isDefault());
            QVERIFY(!dialog->findChild<QPushButton *>("addPersonButton")->autoDefault());
            QVERIFY(!dialog->findChild<QPushButton *>("addLocationButton")->autoDefault());
            QTest::keyClicks(search, "jose");
            QCOMPARE(person->suggestions(),
                     (QStringList{QString::fromUtf8("José Antônio · Professor · Matemática · ID %1").arg(jose)}));
            seen << personStatus->text() << personStatus->property("tone").toString();
            QVERIFY(dialog->findChild<QPushButton *>("addPersonButton")->isVisible());
            auto *popup = person->findChild<QCompleter *>()->popup();
            QVERIFY(popup->isVisible());
            QCOMPARE(popup->model()->rowCount(), 1);
            QVERIFY(popup->grab().save("ui-movement-suggestions.png"));
            person->choose(person->suggestions().first());
            QCOMPARE(person->selectedId(), std::optional<qint64>(jose));
            seen << personStatus->text() << personStatus->property("tone").toString();
            QVERIFY(dialog->findChild<QPushButton *>("addPersonButton")->isVisible());
            auto *place = dialog->findChild<QuickPickField *>("movementLocation");
            auto *placeStatus = place->findChild<QLabel *>("quickPickStatus");
            place->editor()->clear();
            QTest::keyClicks(place->editor(), "Biblioteca nova");
            seen << placeStatus->text() << placeStatus->property("tone").toString();
            QApplication::processEvents();
            QVERIFY(dialog->grab().save("ui-movement-quick-add.png"));
            QTimer::singleShot(0, this, [&] {
                auto *form = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(form);
                QCOMPARE(form->objectName(), QString("locationFormDialog"));
                QCOMPARE(form->findChild<QLineEdit *>("locationName")->text(), QString("Biblioteca nova"));
                QVERIFY(form->grab().save("ui-location-quick-register.png"));
                form->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            });
            dialog->findChild<QPushButton *>("addLocationButton")->click();
            QVERIFY(place->selectedId());
            seen << placeStatus->text();
            QVERIFY(dialog->findChild<QPushButton *>("addLocationButton")->isVisible());
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        button(window, "Registrar retirada")->click();
        QCOMPARE(seen, (QStringList{"Escolha na lista ou use Adicionar pessoa.", "hint",
                                    QString("Pessoa encontrada (ID %1).").arg(jose), "ok",
                                    QString::fromUtf8("Local não encontrado. Use Adicionar local."), "error",
                                    "Local cadastrado."}));
        TestContext reopened(path);
        const auto notebook = reopened.store.notebooks().first();
        QCOMPARE(notebook.status, Status::Loaned);
        QCOMPARE(notebook.borrowerPersonId, jose);
        QCOMPARE(notebook.location, QString("Biblioteca nova"));
        const auto events = reopened.store.history();
        QCOMPARE(events[0].action, QString("Retirada"));
        QCOMPARE(events[1].action, QString("Local cadastrado"));
    }
    void checkoutWithPersonPickerFlow() {
        QTemporaryDir temp;
        const auto path = temp.filePath("picker.sqlite");
        TestContext ctx(path);
        createLocation(ctx, "Sala 12");
        createLocation(ctx, "Biblioteca");
        createPerson(ctx, "Ana", ResponsibleRole::Professor, "Física");
        createPerson(ctx, "Ana", ResponsibleRole::Assistant);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Biblioteca");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.findChild<QTableWidget *>("availableTable")->selectRow(0);
        QString warning, ambiguous, roleShown;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *field = dialog->findChild<QuickPickField *>("movementPerson");
            auto *status = field->findChild<QLabel *>("quickPickStatus");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            QVERIFY(dialog->isVisible());
            warning = status->text();
            field->editor()->setText("ana");
            QVERIFY(!field->selectedId());
            ambiguous = status->text();
            QCOMPARE(field->suggestions().size(), 2);
            pickPerson(dialog, "Ana", 1);
            roleShown = dialog->findChild<QLabel *>("movementPersonRole")->text();
            pickLocation(dialog, "Sala 12");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        button(window, "Registrar retirada")->click();
        QCOMPARE(warning, QString("Informe quem retirou o notebook."));
        QCOMPARE(ambiguous, QString("Nome repetido. Escolha a pessoa na lista."));
        const auto notebook = ctx.store.notebooks().first();
        QCOMPARE(notebook.status, Status::Loaned);
        QCOMPARE(notebook.location, QString("Sala 12"));
        QCOMPARE(notebook.borrowerRole, ResponsibleRole::Assistant);
        QCOMPARE(roleShown, QString("Auxiliar"));
        QCOMPARE(window.findChild<QLabel *>("feedback")->text(),
                 QString("✓  Retirada registrada. O notebook está em Indisponíveis."));
    }
    void personPickerListsOnlyActive() {
        QTemporaryDir temp;
        TestContext ctx(temp.filePath("picker-active.sqlite"));
        createPerson(ctx, "Ana", ResponsibleRole::Professor);
        createPerson(ctx, "Ana", ResponsibleRole::Assistant);
        const auto gone = createPerson(ctx, "Bruno", ResponsibleRole::Professor);
        ctx.people.inactivate(ctx.session, gone);
        PersonPickerDialog picker(ctx.people, [&] {
            return ctx.session;
        });
        picker.show();
        auto *table = picker.findChild<QTableWidget *>("personPickerTable");
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(!picker.selectedPerson());
        QVERIFY(!button(picker, "Selecionar")->isEnabled());
        picker.findChild<QLineEdit *>("personPickerSearch")->setText("Bruno");
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(picker.findChild<QLabel *>("personPickerEmpty")->isVisible());
        picker.findChild<QLineEdit *>("personPickerSearch")->clear();
        QCOMPARE(table->rowCount(), 2);
        table->selectRow(1);
        QVERIFY(button(picker, "Selecionar")->isEnabled());
        QCOMPARE(picker.selectedPerson()->role, ResponsibleRole::Assistant);
    }
    void createLocationAppearsInLogs() {
        QTemporaryDir temp;
        const auto path = temp.filePath("location-log.sqlite");
        TestContext ctx(path);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        auto *nav = button(window, "  Locais");
        QVERIFY(nav);
        nav->click();
        auto *create = window.findChild<QPushButton *>("locationsCreate");
        QVERIFY(create);
        QTRY_VERIFY(create->isEnabled());
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("locationName")->setText("Biblioteca");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        create->click();
        auto *table = window.findChild<QTableWidget *>("locationsTable");
        QVERIFY(table);
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QString("Biblioteca"));
        button(window, "  Logs")->click();
        showAllLogs(window);
        auto *events = window.findChild<QTableWidget *>("events");
        QVERIFY(events);
        QCOMPARE(events->item(0, 1)->text(), QString("Local cadastrado"));
        QCOMPARE(events->item(0, 4)->text(), TestPrincipal);
        QCOMPARE(events->item(0, 2)->text(), QString("—"));
        QCOMPARE(events->item(0, 5)->text(), QString("Biblioteca"));
    }
    void navigationOrderFinal() {
        QTemporaryDir temp;
        const auto path = temp.filePath("navigation.sqlite");
        TestContext ctx(path);
        const auto common = ctx.createCommonAndLogin("operador", "senha-de-operador", InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, common, path, ctx.locations, ctx.people);
        window.show();
        auto *sidebar = window.findChild<QFrame *>("sidebar");
        QVERIFY(sidebar);
        const auto buttons = sidebar->findChildren<QPushButton *>();
        QVERIFY(buttons.size() >= 4);
        QCOMPARE(buttons[0]->text(), QString("  Pessoas"));
        QCOMPARE(buttons[1]->text(), QString("  Equipamentos"));
        QCOMPARE(buttons[2]->text(), QString("  Locais"));
        QCOMPARE(buttons[3]->text(), QString("  Logs"));
        QVERIFY(!button(window, "  Contas"));
        auto *metric = window.findChild<QLabel *>("metricValue");
        QVERIFY(metric);
        QVERIFY(metric->isVisible());
        buttons[0]->click();
        QVERIFY(!metric->isVisible());
        buttons[2]->click();
        QVERIFY(!metric->isVisible());
        buttons[3]->click();
        QVERIFY(!metric->isVisible());
        buttons[1]->click();
        QVERIFY(metric->isVisible());
    }
    void locationsPageCreateAndStates() {
        QTemporaryDir temp;
        const auto path = temp.filePath("locations-page.sqlite");
        TestContext ctx(path);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.showPage("locais");
        auto *table = window.findChild<QTableWidget *>("locationsTable");
        QVERIFY(table);
        auto *page = static_cast<LocationsPage *>(table->parentWidget());
        auto *state = window.findChild<QLabel *>("locationsState");
        auto *create = window.findChild<QPushButton *>("locationsCreate");
        auto *summary = window.findChild<QLabel *>("locationsSummary");
        auto *feedback = window.findChild<QLabel *>("locationsFeedback");
        QVERIFY(state && create && summary && feedback);
        page->reload();
        QCOMPARE(state->text(), QString("Carregando cadastros…"));
        QVERIFY(!create->isEnabled());
        QVERIFY(!table->isEnabled());
        QTRY_VERIFY(create->isEnabled());
        QVERIFY(!table->isVisible());
        auto *empty = button(window, "  Locais");
        QVERIFY(empty);
        bool title = false;
        for (auto *label : page->findChildren<QLabel *>())
            if (label->text() == "Nenhum local cadastrado" && label->isVisible())
                title = true;
        QVERIFY(title);
        QCOMPARE(page->findChild<QLabel *>("locationsEmptyBody")->text(),
                 QString("Selecione Cadastrar local para adicionar um espaço da escola."));
        QCOMPARE(summary->text(), QString("0 locais"));
        QTimer::singleShot(0, this, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        create->click();
        expectFocus(window, create);
        QVERIFY(ctx.locations.locations().isEmpty());
        QTimer::singleShot(0, this, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>("locationName");
            auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
            save->click();
            QCOMPARE(dialog->findChild<QLabel *>("errorText")->text(),
                     QString("Preencha os campos obrigatórios indicados com *."));
            QCOMPARE(dialog->focusWidget(), static_cast<QWidget *>(name));
            name->setText("Sala 12");
            save->click();
            QCOMPARE(save->text(), QString("Salvando…"));
            QVERIFY(!save->isEnabled());
        });
        create->click();
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(summary->text(), QString("1 local"));
        QCOMPARE(feedback->text(), QString("Local cadastrado."));
        QVERIFY(table->item(0, 0)->isSelected());
        QTimer::singleShot(0, this, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>("locationName");
            name->setText(" sala 12");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            QTRY_COMPARE(dialog->findChild<QLabel *>("errorText")->text(),
                         QString("Já existe um local ativo com esse nome."));
            QVERIFY(dialog->isVisible());
            QCOMPARE(name->text(), QString(" sala 12"));
            dialog->reject();
        });
        create->click();
        QCOMPARE(ctx.locations.locations().size(), 1);
        auto *filter = window.findChild<QComboBox *>("locationsFilter");
        filter->setCurrentIndex(1);
        QTRY_COMPARE(state->text(),
                     QString("Nenhum resultado para os filtros atuais. Revise a busca ou limpe os filtros."));
        QCOMPARE(summary->text(), QString("0 locais"));
        auto *search = window.findChild<QLineEdit *>("locationsSearch");
        search->setText("sala");
        window.showPage("equipamentos");
        window.showPage("locais");
        QCOMPARE(search->text(), QString("sala"));
        QCOMPARE(filter->currentIndex(), 1);
        QTRY_VERIFY(create->isEnabled());
        QTimer::singleShot(0, this, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("locationName")->setText("Biblioteca");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        create->click();
        QTRY_COMPARE(feedback->text(), QString("Local cadastrado. O cadastro não aparece no filtro atual."));
        filter->setCurrentIndex(0);
        search->clear();
        QTRY_COMPARE(table->rowCount(), 2);
        QCOMPARE(summary->text(), QString("2 locais"));
        QVERIFY(window.grab().save("locations-page.png"));
    }
    void locationsExpiredAdminSession() {
        QTemporaryDir temp;
        const auto path = temp.filePath("locations-expiry.sqlite");
        TestContext ctx(path);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        window.showPage("locais");
        auto *create = window.findChild<QPushButton *>("locationsCreate");
        QTRY_VERIFY(create->isEnabled());
        bool checked = false;
        QString expired, draft;
        const auto before = ctx.store.history().size();
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>("locationName");
            name->setText("Biblioteca");
            ctx.clock.advance(15 * 60);
            QTimer::singleShot(20, this, [&, dialog, name] {
                auto *login = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(login && login != dialog);
                expired = login->findChild<QLabel *>("errorText")->text();
                draft = name->text();
                QCOMPARE(ctx.store.history().size(), before);
                QVERIFY(ctx.locations.locations().isEmpty());
                login->findChild<QLineEdit *>("loginPassword")->setText(QString::fromUtf8(TestPassword));
                button(*login, "Acessar conta")->click();
                QTRY_VERIFY(login->isHidden());
                QVERIFY(!dialog->isVisible());
                QCOMPARE(name->text(), QString("Biblioteca"));
                QCOMPARE(ctx.store.history().size(), before);
                QVERIFY(ctx.locations.locations().isEmpty());
                checked = true;
            });
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        create->click();
        QVERIFY(checked);
        QCOMPARE(expired, QString("Sessão administrativa expirada. Entre novamente para continuar."));
        QCOMPARE(draft, QString("Biblioteca"));
        QVERIFY(ctx.locations.locations().isEmpty());
        QCOMPARE(ctx.store.history().size(), before);
        QTRY_VERIFY(create->isEnabled());
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("locationName")->setText("Biblioteca");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        create->click();
        QCOMPARE(ctx.locations.locations().size(), 1);
        QCOMPARE(ctx.store.history().size(), before + 1);
    }
    void locationsReadFailureAndLiteralText() {
        QTemporaryDir temp;
        const auto path = temp.filePath("locations-error.sqlite");
        TestContext ctx(path);
        const QString longName = "<b>" + QString(110, QChar(0x00e1)) + "</b>";
        ctx.locations.create(ctx.session, longName);
        LocationsPage page(ctx.locations, ctx.session);
        page.resize(700, 500);
        page.show();
        auto *table = page.findChild<QTableWidget *>("locationsTable");
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), longName);
        QCOMPARE(table->item(0, 0)->toolTip(), longName);
        QCOMPARE(table->textElideMode(), Qt::ElideRight);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ui-location-read-failure");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("ALTER TABLE locations RENAME TO temporarily_unavailable_locations"));
        }
        QSqlDatabase::removeDatabase("ui-location-read-failure");
        page.reload();
        auto *state = page.findChild<QLabel *>("locationsState");
        QTRY_COMPARE(state->text(), QString("Não foi possível carregar os cadastros. Selecione Tentar novamente."));
        QVERIFY(!table->isVisible());
        auto *retry = page.findChild<QPushButton *>("locationsRetry");
        QVERIFY(retry->isVisible());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ui-location-read-recovery");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("ALTER TABLE temporarily_unavailable_locations RENAME TO locations"));
        }
        QSqlDatabase::removeDatabase("ui-location-read-recovery");
        retry->click();
        QTRY_VERIFY(table->isVisible());
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(!retry->isVisible());
    }
    void logsKeepFactsAfterEdits() {
        QTemporaryDir temp;
        const auto path = temp.filePath("ui-facts.sqlite");
        TestContext ctx(path);
        const auto room = ctx.locations.create(ctx.session, "Sala 12"),
                   library = ctx.locations.create(ctx.session, "Biblioteca");
        const auto ana = ctx.people.create(ctx.session, {"Ana Souza", ResponsibleRole::Professor, "Matemática"});
        Fixture::add(ctx, ctx.session, "NB-001", "Dell", "Sala 12");
        const auto id = ctx.store.notebooks().first().id;
        ctx.store.checkout(ctx.session, id, ana, Fixture::location(ctx, "Sala 12"));
        ctx.store.checkin(ctx.session, id, ana, Fixture::location(ctx, "Biblioteca"));
        Person current;
        for (const auto &row : ctx.people.people(CatalogFilter::All))
            if (row.id == ana)
                current = row;
        ctx.people.update(ctx.session, current, {"Ana Souza", ResponsibleRole::Assistant, {}});
        for (const auto &row : ctx.locations.locations(CatalogFilter::All))
            if (row.id == room)
                ctx.locations.rename(ctx.session, row, "Sala 12B");
        ctx.locations.softDelete(ctx.session, library);
        const auto before = ctx.store.notebooks().first();
        Fixture::update(ctx, ctx.session, before, "NB-101", "Dell 5420", before.location);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *logs = button(window, "  Logs");
        QVERIFY(logs);
        logs->click();
        showAllLogs(window);
        auto *events = window.findChild<QTableWidget *>("events");
        QVERIFY(events);
        int checkouts = 0, personUpdates = 0, notebookUpdates = 0;
        for (int row = 0; row < events->rowCount(); ++row) {
            const auto action = events->item(row, 1)->text(), details = events->item(row, 6)->text();
            if (action == "Retirada") {
                ++checkouts;
                QCOMPARE(events->item(row, 2)->text(), QString("NB-001 · Dell"));
                QCOMPARE(events->item(row, 3)->text(), QString("Ana Souza (Professor · Matemática)"));
                QCOMPARE(events->item(row, 5)->text(), QString("Sala 12"));
                QVERIFY2(details.contains("Professor · Matemática"), qPrintable(details));
                QVERIFY(details.contains("Sala 12"));
            } else if (action == "Pessoa atualizada") {
                ++personUpdates;
                QVERIFY2(details.contains("Função: Professor → Auxiliar"), qPrintable(details));
            } else if (action == "Notebook atualizado") {
                ++notebookUpdates;
                QCOMPARE(events->item(row, 2)->text(), QString("NB-101 · Dell 5420"));
                QVERIFY2(details.contains("Patrimônio: NB-001 → NB-101"), qPrintable(details));
            }
        }
        QCOMPARE(checkouts, 1);
        QCOMPARE(personUpdates, 1);
        QCOMPARE(notebookUpdates, 1);
    }
    void legacyNotebookEventsShowUnknown() {
        QTemporaryDir temp;
        const auto path = temp.filePath("legacy-history.sqlite");
        QVERIFY(LegacyFixtures::createV2(path));
        TestContext ctx(path);
        Fixture::updateLocation(ctx, ctx.session, 1, "Biblioteca");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *logs = button(window, "  Logs");
        QVERIFY(logs);
        logs->click();
        showAllLogs(window);
        auto *events = window.findChild<QTableWidget *>("events");
        QVERIFY(events);
        int legacy = 0, emptySubjects = 0, newNotebook = 0;
        for (int row = 0; row < events->rowCount(); ++row) {
            const auto action = events->item(row, 1)->text();
            if (action == "Retirada" || action == "Devolução") {
                ++legacy;
                QCOMPARE(events->item(row, 2)->text(), QString("Não informado"));
            } else if (action == "Local importado" || action == "Atualização do banco") {
                ++emptySubjects;
                QCOMPARE(events->item(row, 2)->text(), QString("—"));
                QCOMPARE(events->item(row, 3)->text(), QString("—"));
            } else if (action == "Atualização de local") {
                ++newNotebook;
                QCOMPARE(events->item(row, 2)->text(), QString("NB-7 · Dell Latitude"));
                QCOMPARE(events->item(row, 3)->text(), QString("—"));
            }
        }
        QCOMPARE(legacy, 2);
        QCOMPARE(emptySubjects, 2);
        QCOMPARE(newNotebook, 1);
        QVERIFY(window.grab().save("ui-history-v5.png"));
    }
    void importedLocationShownInLogs() {
        QTemporaryDir temp;
        const auto path = temp.filePath("v4.sqlite");
        QVERIFY(LegacyFixtures::createV4(path));
        TestContext ctx(path);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *logs = button(window, "  Logs");
        QVERIFY(logs);
        logs->click();
        showAllLogs(window);
        auto *events = window.findChild<QTableWidget *>("events");
        QVERIFY(events);
        int imported = 0;
        for (int row = 0; row < events->rowCount(); ++row)
            if (events->item(row, 1)->text() == "Local importado") {
                ++imported;
                QCOMPARE(events->item(row, 4)->text(), QString("Não informado"));
                QCOMPARE(events->item(row, 2)->text(), QString("—"));
                QCOMPARE(events->item(row, 3)->text(), QString("—"));
                QVERIFY(events->item(row, 6)->text().contains("Sala 12. Notebooks vinculados: 2."));
            }
        QCOMPARE(imported, 1);
    }
    void initTestCase() {
        QApplication::setStyle("Fusion");
        QFile style(":/style.qss");
        QVERIFY(style.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
    }
    void formsFiltersAndMovement() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("ui.sqlite"));
        auto &store = ctx.store;
        createPerson(ctx, "Ana", ResponsibleRole::Professor);
        createPerson(ctx, "Carlos", ResponsibleRole::Assistant);
        for (const auto *place : {"Sala 12", "Laboratório", "Estoque", "Sala 2"})
            createLocation(ctx, place);
        MainWindow window(store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, temp.filePath("ui.sqlite"),
                          ctx.locations, ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->findChildren<QLineEdit *>().size(), 2);
            dialog->findChild<QLineEdit *>("notebookAssetTag")->setText("NB-UI");
            dialog->findChild<QLineEdit *>("notebookName")->setText("Notebook de teste");
            auto *place = dialog->findChild<QComboBox *>("notebookLocation");
            place->setCurrentIndex(place->findText("Estoque"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        auto *add = button(window, "+  Cadastrar notebook");
        QVERIFY(add);
        add->click();
        QCOMPARE(store.notebooks().size(), 1);
        auto *tabs = window.findChild<QTabWidget *>("inventoryTabs");
        QVERIFY(tabs);
        auto *availableGrid = window.findChild<QTableWidget *>("availableTable"),
             *loanedGrid = window.findChild<QTableWidget *>("loanedTable"),
             *maintenanceGrid = window.findChild<QTableWidget *>("maintenanceTable");
        QCOMPARE(availableGrid->rowCount(), 1);
        availableGrid->setCurrentCell(0, 0);
        availableGrid->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            submitPersonMovement("Ana", "Sala 12");
        });
        auto *checkout = window.findChild<QPushButton *>("checkoutButton");
        QVERIFY(checkout->isEnabled());
        checkout->click();
        QCOMPARE(store.notebooks().first().status, Status::Loaned);
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(availableGrid->rowCount(), 0);
        QCOMPARE(loanedGrid->rowCount(), 1);
        QCOMPARE(loanedGrid->item(0, 2)->text(), QString("Ana (Professor)"));
        QCOMPARE(loanedGrid->item(0, 3)->text(), QString("—"));
        QCOMPARE(loanedGrid->item(0, 4)->text(), QString("Sala 12"));
        tabs->setCurrentIndex(1);
        loanedGrid->setCurrentCell(0, 0);
        loanedGrid->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            submitPersonMovement("Carlos", "Estoque");
        });
        auto *checkin = window.findChild<QPushButton *>("checkinButton");
        QVERIFY(checkin->isEnabled());
        checkin->click();
        QCOMPARE(store.notebooks().first().returnedBy, QString("Carlos"));
        tabs->setCurrentIndex(0);
        availableGrid->setCurrentCell(0, 0);
        availableGrid->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QString("locationDialog"));
            auto *combo = dialog->findChild<QComboBox *>("movementLocation");
            combo->setCurrentIndex(combo->findText("Sala 2"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QPushButton *>("availableLocationButton")->click();
        QCOMPARE(store.notebooks().first().location, QString("Sala 2"));
        auto *search = window.findChild<QLineEdit *>("inventorySearch");
        QVERIFY(search);
        for (const auto &term : {"Sala 2", "NB-UI", "teste"}) {
            search->setText(term);
            QCOMPARE(availableGrid->rowCount(), 1);
        }
        search->setText("inexistente");
        QCOMPARE(availableGrid->rowCount(), 0);
        QCOMPARE(window.findChild<QLabel *>("availableEmpty")->text(),
                 QString("Nenhum resultado para os filtros atuais. Revise a busca ou limpe os filtros."));
        search->clear();
        QVERIFY(!window.findChild<QComboBox *>("statusFilter"));
        availableGrid->setCurrentCell(0, 0);
        availableGrid->selectRow(0);
        QTimer::singleShot(0, this, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QPushButton *>("sendMaintenanceButton")->click();
        QCOMPARE(store.notebooks().first().status, Status::Maintenance);
        QCOMPARE(maintenanceGrid->rowCount(), 1);
        tabs->setCurrentIndex(2);
        maintenanceGrid->setCurrentCell(0, 0);
        maintenanceGrid->selectRow(0);
        QTimer::singleShot(0, this, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box)
                box->button(QMessageBox::Yes)->click();
        });
        window.findChild<QPushButton *>("finishMaintenanceButton")->click();
        QCOMPARE(store.notebooks().first().status, Status::Available);
        tabs->setCurrentIndex(0);
        availableGrid->setCurrentCell(0, 0);
        availableGrid->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            submitPersonMovement("Carlos", "Laboratório");
        });
        checkout->click();
        tabs->setCurrentIndex(1);
        loanedGrid->setCurrentCell(0, 0);
        loanedGrid->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            submitPersonMovement("Ana", "Estoque");
        });
        checkin->click();
        QCOMPARE(store.notebooks().first().returnedByRole, ResponsibleRole::Professor);
        QVERIFY(window.grab().save("ui-inventory.png"));
        auto *logs = button(window, "  Logs");
        QVERIFY(logs);
        logs->click();
        showAllLogs(window);
        QApplication::processEvents();
        QVERIFY(window.grab().save("ui-history.png"));
    }
    // ---------------- 03-03: devolução com seletor, sugestão de origem e combos de local ----------------
    void checkinWithPersonPickerFlow() {
        QTemporaryDir temp;
        const auto path = temp.filePath("checkin-ui.sqlite");
        TestContext ctx(path);
        const auto support = createLocation(ctx, "Suporte Técnico"), room = createLocation(ctx, "Sala 12");
        createLocation(ctx, "Armário 1");
        const auto ana = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, "Matemática");
        createPerson(ctx, "Beatriz Rocha", ResponsibleRole::Assistant);
        const auto nb = ctx.store.add(ctx.session, "NB-002", "Lenovo ThinkPad", support);
        ctx.store.checkout(ctx.session, nb, ana, room);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.resize(1440, 900);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("inventoryTabs");
        auto *loaned = window.findChild<QTableWidget *>("loanedTable"),
             *available = window.findChild<QTableWidget *>("availableTable");
        tabs->setCurrentIndex(1);
        QCOMPARE(loaned->rowCount(), 1);
        loaned->setCurrentCell(0, 0);
        loaned->selectRow(0);
        QString role, subject, title;
        bool roleIsLabel = false;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QString("movementDialog"));
            title = dialog->windowTitle();
            QCOMPARE(dialog->findChildren<QuickPickField *>().size(), 2);
            pickPerson(dialog, "Beatriz Rocha");
            role = dialog->findChild<QLabel *>("movementPersonRole")->text();
            subject = dialog->findChild<QLabel *>("movementPersonSubject")->text();
            roleIsLabel = dialog->findChild<QLabel *>("movementPersonRole") != nullptr;
            pickLocation(dialog, "Armário 1");
            QVERIFY(dialog->grab().save("ui-checkin-picker.png"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QPushButton *>("checkinButton")->click();
        QCOMPARE(title, QString("Registrar devolução"));
        QVERIFY(roleIsLabel);
        QCOMPARE(role, QString("Auxiliar"));
        QCOMPARE(subject, QString("—"));
        QCOMPARE(loaned->rowCount(), 0);
        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(available->rowCount(), 1);
        QCOMPARE(available->item(0, 0)->text(), QString("NB-002"));
        QCOMPARE(available->item(0, 2)->text(), QString("Armário 1"));
        QVERIFY(window.findChild<QLabel *>("feedback")
                    ->text()
                    .contains("Devolução registrada. O notebook está em Disponíveis."));
        QCOMPARE(scalarText(ctx, "SELECT returned_by FROM loans"), QString("Beatriz Rocha"));
    }
    void checkinSuggestsOriginLocation() {
        QTemporaryDir temp;
        const auto path = temp.filePath("suggest-ui.sqlite");
        TestContext ctx(path);
        const auto support = createLocation(ctx, "Suporte Técnico"), room = createLocation(ctx, "Sala 12");
        const auto ana = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor);
        const auto nb = ctx.store.add(ctx.session, "NB-002", "Lenovo", support);
        ctx.store.checkout(ctx.session, nb, ana, room);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("inventoryTabs");
        auto *loaned = window.findChild<QTableWidget *>("loanedTable");
        auto *checkin = window.findChild<QPushButton *>("checkinButton");
        QString shown;
        const auto open = [&] {
            tabs->setCurrentIndex(1);
            loaned->setCurrentCell(0, 0);
            loaned->selectRow(0);
            shown.clear();
            QTimer::singleShot(0, this, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                shown = locationText(dialog);
                dialog->reject();
            });
            checkin->click();
        };
        open();
        QCOMPARE(shown, QString("Suporte Técnico"));
        ctx.locations.softDelete(ctx.session, support);
        open();
        QCOMPARE(shown, QString());
        QCOMPARE(ctx.store.notebooks().first().status, Status::Loaned);
    }
    void registerNotebookUsesActiveCombo() {
        QTemporaryDir temp;
        const auto path = temp.filePath("register-ui.sqlite");
        TestContext ctx(path);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        auto *add = button(window, "+  Cadastrar notebook");
        QVERIFY(add);
        QString emptyText;
        bool comboEnabled = true, saveEnabled = true;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *combo = dialog->findChild<QComboBox *>("notebookLocation");
            QVERIFY(combo);
            emptyText = combo->currentText();
            comboEnabled = combo->isEnabled();
            saveEnabled = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled();
            dialog->reject();
        });
        add->click();
        QCOMPARE(emptyText, QString("Cadastre um local em Locais."));
        QVERIFY(!comboEnabled);
        QVERIFY(!saveEnabled);
        QCOMPARE(ctx.store.notebooks().size(), 0);
        createLocation(ctx, "Biblioteca");
        const auto gone = createLocation(ctx, "Depósito");
        ctx.locations.softDelete(ctx.session, gone);
        QStringList items;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *combo = dialog->findChild<QComboBox *>("notebookLocation");
            for (int i = 0; i < combo->count(); ++i)
                items << combo->itemText(i);
            dialog->findChild<QLineEdit *>("notebookAssetTag")->setText("NB-050");
            dialog->findChild<QLineEdit *>("notebookName")->setText("Acer Aspire");
            combo->setCurrentIndex(combo->findText("Biblioteca"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        add->click();
        QCOMPARE(items, (QStringList{"Selecione o local", "Biblioteca"}));
        QCOMPARE(ctx.store.notebooks().size(), 1);
        QCOMPARE(ctx.store.notebooks().first().location, QString("Biblioteca"));
        auto *available = window.findChild<QTableWidget *>("availableTable");
        QCOMPARE(available->rowCount(), 1);
        QCOMPARE(available->item(0, 2)->text(), QString("Biblioteca"));
    }
    void editNotebookKeepsDeletedLocation() {
        QTemporaryDir temp;
        const auto path = temp.filePath("edit-ui.sqlite");
        TestContext ctx(path);
        const auto home = createLocation(ctx, "Armário 1"), old = createLocation(ctx, "Depósito antigo");
        ctx.store.add(ctx.session, "NB-001", "Dell", home);
        const auto legacy = ctx.store.add(ctx.session, "NB-002", "HP", old);
        ctx.locations.softDelete(ctx.session, old);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        auto *available = window.findChild<QTableWidget *>("availableTable");
        auto *edit = button(window, "Editar notebook");
        auto rowOf = [&](const QString &tag) {
            for (int r = 0; r < available->rowCount(); ++r)
                if (available->item(r, 0)->text() == tag)
                    return r;
            return -1;
        };
        QString first, current;
        const auto inspect = [&](bool rename) {
            QTimer::singleShot(0, this, [&, rename] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto *combo = dialog->findChild<QComboBox *>("notebookLocation");
                first = combo->itemText(0);
                current = combo->currentText();
                if (!rename) {
                    dialog->reject();
                    return;
                }
                dialog->findChild<QLineEdit *>("notebookName")->setText("HP ProBook");
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            });
            edit->click();
        };
        available->selectRow(rowOf("NB-001"));
        inspect(false);
        QCOMPARE(current, QString("Armário 1"));
        available->selectRow(rowOf("NB-002"));
        inspect(false);
        QCOMPARE(first, QString("Manter local atual: Depósito antigo"));
        QCOMPARE(current, first);
        available->selectRow(rowOf("NB-002"));
        inspect(true);
        Notebook saved;
        for (const auto &n : ctx.store.notebooks())
            if (n.id == legacy)
                saved = n;
        QCOMPARE(saved.name, QString("HP ProBook"));
        QCOMPARE(saved.locationId, old);
        QCOMPARE(saved.location, QString("Depósito antigo"));
        QVERIFY(saved.locationDeleted);
    }
    // ---------------- 03-02: guias e matriz ----------------
    // Banco com um notebook em cada status: NB-001 disponível, NB-002 emprestado, NB-003 em manutenção.
    void seedThreeStatuses(TestContext &ctx) {
        const auto home = createLocation(ctx, "Armário 1");
        const auto room = createLocation(ctx, "Sala 12");
        const auto ana = createPerson(ctx, "Ana Souza", ResponsibleRole::Professor, "Matemática");
        for (const auto *tag : {"NB-001", "NB-002", "NB-003"})
            Fixture::add(ctx, ctx.session, tag, QString("Dell ") + tag, "Armário 1");
        Q_UNUSED(home);
        for (const auto &n : ctx.store.notebooks()) {
            if (n.assetTag == "NB-002")
                ctx.store.checkout(ctx.session, n.id, ana, room);
            if (n.assetTag == "NB-003")
                ctx.store.setMaintenance(ctx.session, n.id, true, "Bateria não carrega");
        }
    }
    void inventoryTabsMatrix() {
        QTemporaryDir temp;
        const auto path = temp.filePath("tabs.sqlite");
        TestContext ctx(path);
        seedThreeStatuses(ctx);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.resize(1440, 900);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("inventoryTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(0), QString("Disponíveis"));
        QCOMPARE(tabs->tabText(1), QString("Indisponíveis (emprestados)"));
        QCOMPARE(tabs->tabText(2), QString("Manutenção"));
        const QStringList grids{"availableTable", "loanedTable", "maintenanceTable"};
        const QList<QStringList> matrix{{"availableLocationButton", "sendMaintenanceButton", "checkoutButton"},
                                        {"loanedLocationButton", "checkinButton"},
                                        {"finishMaintenanceButton"}};
        const QStringList all{"availableLocationButton", "sendMaintenanceButton", "checkoutButton",
                              "loanedLocationButton",    "checkinButton",         "finishMaintenanceButton"};
        const QStringList shots{"ui-tabs-disponiveis.png", "ui-tabs-indisponiveis.png", "ui-tabs-manutencao.png"};
        for (int index = 0; index < 3; ++index) {
            tabs->setCurrentIndex(index);
            QTest::qWait(10);
            auto *grid = window.findChild<QTableWidget *>(grids[index]);
            QCOMPARE(grid->rowCount(), 1);
            for (const auto &name : all)
                QCOMPARE(window.findChild<QPushButton *>(name)->isVisible(), matrix[index].contains(name));
            for (const auto &name : matrix[index])
                QVERIFY(!window.findChild<QPushButton *>(name)->isEnabled());
            grid->setCurrentCell(0, 0);
            grid->selectRow(0);
            for (const auto &name : matrix[index])
                QVERIFY(window.findChild<QPushButton *>(name)->isEnabled());
            QVERIFY(window.grab().save(shots[index]));
        }
        auto *loanedGrid = window.findChild<QTableWidget *>("loanedTable");
        QCOMPARE(loanedGrid->item(0, 2)->text(), QString("Ana Souza (Professor)"));
        QCOMPARE(loanedGrid->item(0, 3)->text(), QString("Matemática"));
        QCOMPARE(loanedGrid->item(0, 4)->text(), QString("Sala 12"));
        QVERIFY(loanedGrid->item(0, 5)->text() != "—");
        auto *maintenanceGrid = window.findChild<QTableWidget *>("maintenanceTable");
        QCOMPARE(maintenanceGrid->item(0, 4)->text(), QString("Bateria não carrega"));
        QCOMPARE(maintenanceGrid->item(0, 4)->toolTip(), QString("<qt>Bateria não carrega</qt>"));
    }
    void promoHeaderRemoved() {
        QTemporaryDir temp;
        const auto path = temp.filePath("header.sqlite");
        TestContext ctx(path);
        seedThreeStatuses(ctx);
        Fixture::add(ctx, ctx.session, "NB-004", "Inativo", "Armário 1");
        for (const auto &n : ctx.store.notebooks())
            if (n.assetTag == "NB-004")
                ctx.store.inactivate(ctx.session, n.id);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        for (auto *text : window.findChildren<QLabel *>())
            for (const auto &removed : {"CONTROLE PATRIMONIAL", "Seus notebooks, organizados.",
                                        "Acompanhe a disponibilidade e cada movimentação dos aparelhos."})
                QVERIFY(text->text() != removed);
        QCOMPARE(window.findChild<QLabel *>("pageTitle")->text(), QString("Equipamentos"));
        auto *tabs = window.findChild<QTabWidget *>("inventoryTabs");
        for (int index = 0; index < 3; ++index) {
            tabs->setCurrentIndex(index);
            QVERIFY(button(window, "+  Cadastrar notebook")->isEnabled());
        }
        QStringList metrics;
        for (auto *value : window.findChildren<QLabel *>("metricValue"))
            metrics << value->text();
        QCOMPARE(metrics, (QStringList{"3", "1", "1", "1"}));
    }
    void selectionPreservedAfterReload() {
        QTemporaryDir temp;
        const auto path = temp.filePath("selection.sqlite");
        TestContext ctx(path);
        createLocation(ctx, "Armário 1");
        createLocation(ctx, "Sala 12");
        createPerson(ctx, "Ana", ResponsibleRole::Professor);
        Fixture::add(ctx, ctx.session, "NB-001", "Um", "Armário 1");
        Fixture::add(ctx, ctx.session, "NB-002", "Dois", "Armário 1");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        auto *grid = window.findChild<QTableWidget *>("availableTable");
        auto rowOf = [&](const QString &tag) {
            for (int r = 0; r < grid->rowCount(); ++r)
                if (grid->item(r, 0)->text() == tag)
                    return r;
            return -1;
        };
        auto selectedTag = [&] {
            const int r = grid->currentRow();
            return r >= 0 && grid->item(r, 0)->isSelected() ? grid->item(r, 0)->text() : QString();
        };
        grid->setCurrentCell(rowOf("NB-002"), 0);
        grid->selectRow(rowOf("NB-002"));
        button(window, "Atualizar")->click();
        QCOMPARE(selectedTag(), QString("NB-002"));
        // Retirar NB-001 (pelo serviço, como outra operação) e recarregar: a guia e a seleção de NB-002 permanecem.
        grid->setCurrentCell(rowOf("NB-001"), 0);
        grid->selectRow(rowOf("NB-001"));
        QTimer::singleShot(0, this, [&] {
            submitPersonMovement("Ana", "Sala 12");
        });
        window.findChild<QPushButton *>("checkoutButton")->click();
        QCOMPARE(window.findChild<QTabWidget *>("inventoryTabs")->currentIndex(), 0);
        QCOMPARE(grid->rowCount(), 1);
        grid->setCurrentCell(0, 0);
        grid->selectRow(0);
        button(window, "Atualizar")->click();
        QCOMPARE(selectedTag(), QString("NB-002"));
        QCOMPARE(window.findChild<QLabel *>("feedback")->text(), QString("✓  Dados atualizados."));
    }
    void tabsFollowPersistedState() {
        QTemporaryDir temp;
        const auto path = temp.filePath("persisted.sqlite");
        {
            TestContext ctx(path);
            seedThreeStatuses(ctx);
        }
        for (int round = 0; round < 2; ++round) {
            TestContext ctx(path);
            MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                              ctx.people);
            window.show();
            QCOMPARE(window.findChild<QTableWidget *>("availableTable")->item(0, 0)->text(), QString("NB-001"));
            QCOMPARE(window.findChild<QTableWidget *>("loanedTable")->item(0, 0)->text(), QString("NB-002"));
            QCOMPARE(window.findChild<QTableWidget *>("maintenanceTable")->item(0, 0)->text(), QString("NB-003"));
        }
    }
    void maintenanceNoteFlow() {
        QTemporaryDir temp;
        const auto path = temp.filePath("maintenance-ui.sqlite");
        TestContext ctx(path);
        createLocation(ctx, "Armário 1");
        Fixture::add(ctx, ctx.session, "NB-001", "Dell", "Armário 1");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        auto *grid = window.findChild<QTableWidget *>("availableTable");
        grid->setCurrentCell(0, 0);
        grid->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QString("maintenanceDialog"));
            auto *note = dialog->findChild<QLineEdit *>("maintenanceNote");
            QCOMPARE(note->maxLength(), 200);
            note->setText("Teclado com falha");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QPushButton *>("sendMaintenanceButton")->click();
        auto *tabs = window.findChild<QTabWidget *>("inventoryTabs");
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(window.findChild<QLabel *>("feedback")->text(),
                 QString("✓  Notebook enviado à manutenção. Ele está na guia Manutenção."));
        auto *maintenanceGrid = window.findChild<QTableWidget *>("maintenanceTable");
        QCOMPARE(maintenanceGrid->rowCount(), 1);
        QCOMPARE(maintenanceGrid->item(0, 4)->text(), QString("Teclado com falha"));
        QVERIFY(maintenanceGrid->item(0, 3)->text() != "—");
        tabs->setCurrentIndex(2);
        maintenanceGrid->setCurrentCell(0, 0);
        maintenanceGrid->selectRow(0);
        QString question;
        QTimer::singleShot(0, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            question = box->text();
            box->button(QMessageBox::Yes)->click();
        });
        window.findChild<QPushButton *>("finishMaintenanceButton")->click();
        QCOMPARE(question, QString("Concluir manutenção e disponibilizar este notebook?"));
        QCOMPARE(grid->rowCount(), 1);
        QCOMPARE(grid->item(0, 2)->text(), QString("Armário 1"));
        QCOMPARE(window.findChild<QLabel *>("feedback")->text(),
                 QString("✓  Manutenção concluída. O notebook está em Disponíveis."));
    }
    void updateLocationComboOnlyActive() {
        QTemporaryDir temp;
        const auto path = temp.filePath("location-combo.sqlite");
        TestContext ctx(path);
        createLocation(ctx, "Armário 1");
        const auto old = createLocation(ctx, "Depósito");
        createLocation(ctx, "Sala 12");
        Fixture::add(ctx, ctx.session, "NB-001", "Dell", "Depósito");
        ctx.locations.softDelete(ctx.session, old);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        auto *grid = window.findChild<QTableWidget *>("availableTable");
        grid->setCurrentCell(0, 0);
        grid->selectRow(0);
        QStringList names;
        QString current;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QString("locationDialog"));
            current = dialog->findChild<QLabel *>("currentLocationLabel")->text();
            auto *combo = dialog->findChild<QComboBox *>("movementLocation");
            for (int i = 0; i < combo->count(); ++i)
                names << combo->itemText(i);
            QCOMPARE(combo->currentIndex(), 0);
            combo->setCurrentIndex(combo->findText("Sala 12"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QPushButton *>("availableLocationButton")->click();
        QCOMPARE(current, QString("Local atual: Depósito (excluído)"));
        QCOMPARE(names, (QStringList{"Selecione o local", "Armário 1", "Sala 12"}));
        QCOMPARE(ctx.store.notebooks().first().location, QString("Sala 12"));
        QCOMPARE(window.findChild<QLabel *>("feedback")->text(), QString("✓  Local atualizado."));
    }
    void changesRenderedInPortuguese() {
        QCOMPARE(describeChanges(QJsonObject{{"active", QJsonObject{{"de", 1}, {"para", 0}}}}),
                 QString("Situação: Ativa → Inativa"));
        QCOMPARE(describeChanges(LocalDatabase::change("location", "Armário 1", "Sala 12")),
                 QString("Local: Armário 1 → Sala 12"));
        QCOMPARE(describeChanges(LocalDatabase::change("asset_tag", QJsonValue::Null, "NB-1")),
                 QString("Patrimônio: — → NB-1"));
        QJsonObject movement = LocalDatabase::change("status", "available", "loaned");
        movement.insert("location", QJsonObject{{"de", "Armário 1"}, {"para", "Sala 3"}});
        QCOMPARE(describeChanges(movement),
                 QString("Local: Armário 1 → Sala 3; Situação do notebook: Disponível → Emprestado"));
        QCOMPARE(describeChanges(LocalDatabase::change("schema_version", 2, 3)), QString("Versão do esquema: 2 → 3"));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("v2.sqlite");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ui-v2");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE notebooks(id INTEGER PRIMARY KEY, asset_tag TEXT NOT NULL COLLATE NOCASE "
                           "UNIQUE, name TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'available', location TEXT)"));
            QVERIFY(q.exec("CREATE TABLE loans(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES "
                           "notebooks(id), borrower TEXT NOT NULL, checked_out_at TEXT NOT NULL, returned_at TEXT, "
                           "borrower_role TEXT, returned_by TEXT, returned_by_role TEXT)"));
            QVERIFY(q.exec("CREATE TABLE events(id INTEGER PRIMARY KEY, notebook_id INTEGER NOT NULL REFERENCES "
                           "notebooks(id), action TEXT NOT NULL, details TEXT NOT NULL, happened_at TEXT NOT NULL)"));
            QVERIFY(q.exec("INSERT INTO notebooks VALUES(1,'NB-LOG','Dell Latitude','available','Armário 1')"));
            QVERIFY(q.exec("INSERT INTO events VALUES(1,1,'Cadastro','Notebook cadastrado: Dell Latitude. Local: "
                           "Armário 1','2026-01-01T09:00:00.000Z')"));
            QVERIFY(q.exec("PRAGMA user_version=2"));
        }
        QSqlDatabase::removeDatabase("ui-v2");
        TestContext ctx(path);
        auto &store = ctx.store;
        Fixture::updateLocation(ctx, ctx.session, 1, "Sala 12");
        MainWindow window(store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations, ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *logs = button(window, "  Logs");
        QVERIFY(logs);
        logs->click();
        showAllLogs(window);
        auto *events = window.findChild<QTableWidget *>("events");
        QVERIFY(events);
        QCOMPARE(events->rowCount(), 5);
        const QStringList headers{"Data e hora",     "Tipo",  "Notebook", "Pessoa responsável",
                                  "Conta operadora", "Local", "Detalhes"};
        QCOMPARE(events->columnCount(), headers.size());
        for (int column = 0; column < headers.size(); ++column)
            QCOMPARE(events->horizontalHeaderItem(column)->text(), headers[column]);
        QCOMPARE(events->item(0, 4)->text(), QString("principal"));
        QCOMPARE(events->item(0, 2)->text(), QString("NB-LOG · Dell Latitude"));
        QCOMPARE(events->item(0, 5)->text(), QString("Armário 1 → Sala 12"));
        QCOMPARE(events->item(0, 6)->text(), QString("Local: Armário 1 → Sala 12"));
        QCOMPARE(events->item(1, 1)->text(), QString("Conta principal criada"));
        QCOMPARE(events->item(1, 4)->text(), QString("principal"));
        QCOMPARE(events->item(2, 1)->text(), QString("Atualização do banco"));
        QCOMPARE(events->item(2, 4)->text(), QString("Não informado"));
        QCOMPARE(events->item(2, 2)->text(), QString("—"));
        QCOMPARE(events->item(2, 3)->text(), QString("—"));
        const QString upgrade = "Versão do esquema: 2 → " + QString::number(LocalDatabase::CurrentSchemaVersion);
        QVERIFY2(events->item(2, 6)->text().contains(upgrade), qPrintable(events->item(2, 6)->text()));
        int upgrades = 0;
        for (int row = 0; row < events->rowCount(); ++row)
            upgrades += events->item(row, 1)->text() == "Atualização do banco";
        QCOMPARE(upgrades, 1);
        QCOMPARE(events->item(3, 1)->text(), QString("Local importado"));
        QCOMPARE(events->item(4, 1)->text(), QString("Notebook cadastrado"));
        QCOMPARE(events->item(4, 4)->text(), QString("Não informado"));
        QCOMPARE(events->item(4, 6)->text(), QString("Notebook cadastrado: Dell Latitude. Local: Armário 1"));
        QVERIFY(window.grab().save("ui-history-v3.png"));
    }
    void accountEventsInLogs() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("logs.sqlite");
        TestContext ctx(path);
        const QByteArray password = "senha temporaria do operador";
        const qint64 id = ctx.accounts.createCommonAccount(
            ctx.session, "operador.um", ctx.auth.preparePassword(password), InitialPasswordMode::Temporary);
        ctx.accounts.inactivateCommonAccount(ctx.session, id);
        auto draft = ctx.recovery.beginEnrollment(ctx.session);
        const QByteArray secret = secretOf(*draft);
        const QStringList codes = ctx.recovery.confirmEnrollment(
            ctx.session, *draft, totp::code(secret, totp::counterAt(ctx.clock.nowUtc())));
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *logs = button(window, "  Logs");
        QVERIFY(logs);
        logs->click();
        showAllLogs(window);
        auto *events = window.findChild<QTableWidget *>("events");
        QVERIFY(events);
        QStringList forbidden{QString::fromUtf8(password), "$argon2id", "otpauth", totp::base32(secret)};
        forbidden << codes;
        QString created, inactivated;
        for (int row = 0; row < events->rowCount(); ++row) {
            const QString action = events->item(row, 1)->text();
            if (action == "Conta criada") {
                QCOMPARE(events->item(row, 4)->text(), QString("principal"));
                created = events->item(row, 6)->text();
            }
            if (action == "Conta inativada") {
                QCOMPARE(events->item(row, 4)->text(), QString("principal"));
                inactivated = events->item(row, 6)->text();
            }
            for (int column = 0; column < events->columnCount(); ++column) {
                const QString text = events->item(row, column) ? events->item(row, column)->text() : QString();
                for (const auto &secretText : forbidden)
                    QVERIFY2(!text.contains(secretText),
                             qPrintable(QString("linha %1, coluna %2").arg(row).arg(column)));
            }
        }
        QVERIFY2(created.contains("Nome de usuário: — → operador.um"), qPrintable(created));
        QVERIFY2(inactivated.contains("Situação: Ativa → Inativa"), qPrintable(inactivated));
        QVERIFY(window.grab().save("ui-account-logs.png"));
    }
    void bootstrapPending() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("pending.sqlite"), false);
        BootstrapPendingDialog pending(ctx.auth);
        pending.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pending));
        auto *title = pending.findChild<QLabel *>("dialogTitle");
        QVERIFY(title);
        QCOMPARE(title->text(), QString("Configuração inicial necessária"));
        QVERIFY(pending.findChildren<QLineEdit *>().isEmpty());
        auto *retry = button(pending, "Tentar novamente");
        QVERIFY(retry && button(pending, "Sair"));
        retry->click();
        QVERIFY(pending.isVisible());
        QCOMPARE(pending.result(), int(QDialog::Rejected));
        QVERIFY(pending.grab().save("ui-bootstrap-pending.png"));
        ctx.auth.bootstrapPrincipal(TestPrincipal, ctx.auth.preparePassword(TestPassword));
        retry->click();
        QCOMPARE(pending.result(), int(QDialog::Accepted));
        LoginDialog login(ctx.auth, ctx.recovery);
        login.show();
        QVERIFY(QTest::qWaitForWindowExposed(&login));
        for (auto *field : login.findChildren<QLineEdit *>())
            QVERIFY(field->text().isEmpty());
        BootstrapPendingDialog closing(ctx.auth);
        closing.show();
        button(closing, "Sair")->click();
        QCOMPARE(closing.result(), int(QDialog::Rejected));
    }
    void logout() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("logout.sqlite"));
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const auto id = ctx.store.notebooks().first().id;
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, temp.filePath("logout.sqlite"),
                          ctx.locations, ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *identity = window.findChild<QLabel *>("sessionIdentity");
        QVERIFY(identity);
        QCOMPARE(identity->text(), QString("Conta operadora: principal · Principal"));
        QCOMPARE(identity->textFormat(), Qt::PlainText);
        QSignalSpy requested(&window, &MainWindow::logoutRequested);
        auto *quit = button(window, "  Sair");
        QVERIFY(quit);
        quit->click();
        QCOMPARE(requested.count(), 1);
        const Session used = ctx.session;
        ctx.auth.logout(ctx.session);
        try {
            Fixture::checkout(ctx, used, id, "Ana", ResponsibleRole::Professor, "Sala 12");
            QFAIL("sessão encerrada autorizou retirada");
        } catch (const AuthorizationError &e) {
            QCOMPARE(e.reason(), AuthorizationError::Reason::NotAuthenticated);
        }
        QCOMPARE(ctx.store.notebooks().first().status, Status::Available);
    }
    void responsibleNotPrefilled() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("responsible.sqlite"));
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session,
                          temp.filePath("responsible.sqlite"), ctx.locations, ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *inventory = window.findChild<QTableWidget *>("availableTable");
        QVERIFY(inventory);
        inventory->setCurrentCell(0, 0);
        inventory->selectRow(0);
        QString responsible = "não verificado";
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            responsible = dialog->findChild<QLineEdit *>("movementPersonSearch")->text();
            QVERIFY(!dialog->findChild<QuickPickField *>("movementPerson")->selectedId());
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        });
        button(window, "Registrar retirada")->click();
        QCOMPARE(responsible, QString());
        QCOMPARE(ctx.store.notebooks().first().status, Status::Available);
    }
    void loginFlow() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("login.sqlite"), false);
        ctx.auth.bootstrapPrincipal(TestPrincipal, ctx.auth.preparePassword(TestPassword));
        LoginDialog dialog(ctx.auth, ctx.recovery);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto *title = dialog.findChild<QLabel *>("dialogTitle");
        QVERIFY(title);
        QCOMPARE(title->text(), QString("Entrar no GestaoEquipamentos"));
        auto *user = dialog.findChild<QLineEdit *>("loginUsername");
        auto *password = dialog.findChild<QLineEdit *>("loginPassword");
        QVERIFY(user && password);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QCOMPARE(dialog.findChildren<QLineEdit *>().size(), 2);
        auto *submit = button(dialog, "Acessar conta");
        QVERIFY(submit);
        user->setText("principal");
        password->setText("senha errada mas comprida");
        QVERIFY(dialog.grab().save("ui-login.png"));
        submit->click();
        QCOMPARE(submit->text(), QString("Entrando…"));
        QVERIFY(!submit->isEnabled());
        QTRY_VERIFY(submit->isEnabled());
        auto *error = dialog.findChild<QLabel *>("errorText");
        QVERIFY(error);
        QVERIFY(!error->isHidden());
        QCOMPARE(error->text(),
                 QString("Não foi possível entrar. Confira o nome de usuário e a senha e tente novamente."));
        QCOMPARE(user->text(), QString("principal"));
        QVERIFY(password->text().isEmpty());
        QCOMPARE(dialog.focusWidget(), password);
        QVERIFY(dialog.grab().save("ui-login-error.png"));
        password->setText(QString::fromUtf8(TestPassword));
        submit->click();
        QTRY_COMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(password->text().isEmpty());
        QVERIFY(dialog.session().isValid());
        QCOMPARE(dialog.session().username(), TestPrincipal);
    }
    void accountsPage() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("accounts.sqlite"));
        AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, ctx.session);
        page.resize(1100, 640);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *emptyTitle = page.findChild<QLabel *>("dialogTitle");
        QVERIFY(emptyTitle);
        QCOMPARE(emptyTitle->text(), QString("Nenhuma conta comum cadastrada"));
        auto *emptyBody = page.findChild<QLabel *>("helpText");
        QVERIFY(emptyBody);
        QCOMPARE(emptyBody->text(), QString("Selecione Criar conta comum para adicionar um operador."));
        auto *filter = page.findChild<QComboBox *>("accountFilter");
        QVERIFY(filter);
        QCOMPARE(filter->currentText(), QString("Ativas"));
        auto *reset = button(page, "Redefinir senha");
        auto *inactivate = button(page, "Inativar conta");
        QVERIFY(reset && inactivate);
        QVERIFY(!reset->isEnabled());
        QVERIFY(!inactivate->isEnabled());
        QCOMPARE(inactivate->objectName(), QString("destructive"));
        QString mismatch, pending;
        bool focusedConfirm = false;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialog->findChild<QLineEdit *>("accountUsername")->setText("operador.um");
            auto *password = dialog->findChild<QLineEdit *>("temporaryPassword");
            auto *confirm = dialog->findChild<QLineEdit *>("confirmTemporaryPassword");
            password->setText("senha temporaria do operador");
            confirm->setText("outra senha qualquer longa");
            auto *save = button(*dialog, "Salvar conta");
            save->click();
            mismatch = dialog->findChild<QLabel *>("errorText")->text();
            focusedConfirm = dialog->focusWidget() == confirm;
            QVERIFY(dialog->grab().save("ui-create-account.png"));
            confirm->setText("senha temporaria do operador");
            save->click();
            pending = save->isEnabled() ? QString("habilitado") : save->text();
        });
        button(page, "Criar conta comum")->click();
        QCOMPARE(mismatch, QString("As senhas não coincidem. Confira a confirmação."));
        QVERIFY(focusedConfirm);
        QCOMPARE(pending, QString("Salvando…"));
        auto *table = page.findChild<QTableWidget *>("accounts");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text() + " | " + table->item(0, 1)->text() + " | " + table->item(0, 2)->text(),
                 QString("operador.um | Comum | Ativa"));
        QCOMPARE(table->item(0, 0)->toolTip(), QString("operador.um"));
        QCOMPARE(page.findChild<QLabel *>("feedback")->text(), QString("✓  Conta salva."));
        table->setCurrentCell(0, 0);
        table->selectRow(0);
        QVERIFY(reset->isEnabled());
        QVERIFY(inactivate->isEnabled());
        QVERIFY(page.grab().save("ui-accounts.png"));
        auto *search = page.findChild<QLineEdit *>("accountSearch");
        QVERIFY(search);
        search->setText("inexistente");
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(emptyTitle->text(), QString("Nenhuma conta encontrada. Revise a busca ou limpe o filtro."));
        QVERIFY(!reset->isEnabled());
        search->clear();
        QCOMPARE(table->rowCount(), 1);
        table->setCurrentCell(0, 0);
        table->selectRow(0);
        QString defaultButton, escapeButton, question;
        QTimer::singleShot(0, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            question = box->text();
            defaultButton = box->defaultButton() ? box->defaultButton()->text() : QString();
            escapeButton = box->escapeButton() ? box->escapeButton()->text() : QString();
            QVERIFY(box->grab().save("ui-inactivate-confirm.png"));
            for (auto *b : box->buttons())
                if (b->text() == "Inativar conta")
                    b->click();
        });
        inactivate->click();
        QCOMPARE(
            question,
            QString("Inativar conta operador.um? Ela não poderá entrar. O histórico de operações será preservado."));
        QCOMPARE(defaultButton, QString("Cancelar"));
        QCOMPARE(escapeButton, QString("Cancelar"));
        QCOMPARE(page.findChild<QLabel *>("feedback")->text(), QString("✓  Conta inativada."));
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(emptyTitle->text(), QString("Nenhuma conta encontrada. Revise a busca ou limpe o filtro."));
        filter->setCurrentIndex(2);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 2)->text(), QString("Inativa"));
        table->setCurrentCell(0, 0);
        table->selectRow(0);
        QVERIFY(!inactivate->isEnabled());
    }
    void commonSessionHidesAccounts() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("hidden.sqlite"));
        const Session common = ctx.createCommonAndLogin(
            "operador.um", QByteArrayLiteral("frase pessoal do operador um"), InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, common, temp.filePath("hidden.sqlite"),
                          ctx.locations, ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(!button(window, "  Contas"));
        QVERIFY(!window.findChild<QTableWidget *>("accounts"));
        QCOMPARE(window.findChild<QLabel *>("sessionIdentity")->text(),
                 QString("Conta operadora: operador.um · Comum"));
        MainWindow admin(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, temp.filePath("hidden.sqlite"),
                         ctx.locations, ctx.people);
        admin.show();
        QVERIFY(QTest::qWaitForWindowExposed(&admin));
        auto *accounts = button(admin, "  Contas");
        QVERIFY(accounts);
        accounts->click();
        QVERIFY(admin.findChild<QTableWidget *>("accounts"));
        QVERIFY(admin.grab().save("ui-accounts-window.png"));
    }
    void adminExpiryBlocksAndReauth() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("expiry.sqlite");
        TestContext ctx(path);
        Fixture::add(ctx, ctx.session, "NB-1", "Dell", "Armário 1");
        const auto id = ctx.store.notebooks().first().id;
        Fixture::checkout(ctx, ctx.session, id, "Ana", ResponsibleRole::Professor, "Sala 12");
        createPerson(ctx, "Bruno", ResponsibleRole::Assistant);
        createLocation(ctx, "Armário 2");
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, ctx.session, path, ctx.locations,
                          ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *deadline = window.findChild<QLabel *>("sessionDeadline");
        QVERIFY(deadline);
        QVERIFY(!deadline->isHidden());
        QCOMPARE(deadline->text(), "Sessão administrativa até " +
                                       ctx.session.issuedAtUtc().addSecs(15 * 60).toLocalTime().toString("HH:mm:ss"));
        window.findChild<QTabWidget *>("inventoryTabs")->setCurrentIndex(1);
        auto *inventory = window.findChild<QTableWidget *>("loanedTable");
        QVERIFY(inventory);
        inventory->setCurrentCell(0, 0);
        inventory->selectRow(0);
        const auto before = ctx.store.history().size();
        QString expiredText, draftDuring, draftAfter;
        bool lockedDuring = false, unlockedAfter = false, stillOpen = false;
        qsizetype eventsDuring = -1, eventsAfter = -1;
        QTimer::singleShot(0, this, [&] {
            auto *movement = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!movement)
                return;
            pickPerson(movement, "Bruno");
            auto *personSearch = movement->findChild<QLineEdit *>("movementPersonSearch");
            pickLocation(movement, "Armário 2");
            ctx.clock.advance(15 * 60);
            QTimer::singleShot(0, this, [&, movement] {
                auto *login = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!login || login == movement)
                    return;
                expiredText = login->findChild<QLabel *>("errorText")->text();
                draftDuring = movement->findChild<QLineEdit *>("movementPersonSearch")
                                  ->text()
                                  .section(QString::fromUtf8(" · "), 0, 0);
                eventsDuring = ctx.store.history().size();
                lockedDuring = !inventory->isEnabled();
                QVERIFY(login->grab().save("ui-reauth.png"));
                login->findChild<QLineEdit *>("loginPassword")->setText(QString::fromUtf8(TestPassword));
                button(*login, "Acessar conta")->click();
            });
            auto *confirm = movement->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
            confirm->click();
            eventsAfter = ctx.store.history().size();
            stillOpen = movement->isVisible();
            draftAfter = personSearch->text().section(QString::fromUtf8(" · "), 0, 0);
            unlockedAfter = inventory->isEnabled();
        });
        button(window, "Registrar devolução")->click();
        QCOMPARE(expiredText, QString("Sessão administrativa expirada. Entre novamente para continuar."));
        QVERIFY(lockedDuring);
        QCOMPARE(draftDuring, QString("Bruno"));
        QCOMPARE(eventsDuring, before);
        QVERIFY(!stillOpen);
        QCOMPARE(draftAfter, QString("Bruno"));
        QCOMPARE(eventsAfter, before);
        QVERIFY(unlockedAfter);
        QCOMPARE(ctx.store.notebooks().first().status, Status::Loaned);
        inventory->selectRow(0);
        QTimer::singleShot(0, this, [&] {
            submitPersonMovement("Bruno", "Armário 2");
        });
        button(window, "Registrar devolução")->click();
        QCOMPARE(ctx.store.history().size(), before + 1);
        QCOMPARE(ctx.store.history().first().actorUsername, TestPrincipal);
        QCOMPARE(ctx.store.notebooks().first().status, Status::Available);
    }
    void adminSectionStates() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("admins.sqlite");
        TestContext ctx(path);
        const QByteArray adminPassword = QByteArrayLiteral("senha do administrador um");
        AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, ctx.session);
        page.resize(1100, 820);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *section = page.findChild<QWidget *>("adminSection");
        QVERIFY(section);
        auto *count = section->findChild<QLabel *>("adminCount");
        QVERIFY(count);
        QCOMPARE(count->text(), QString("1 de 3 ativos"));
        auto *admins = section->findChild<QTableWidget *>("administrators");
        QVERIFY(admins);
        QCOMPARE(admins->rowCount(), 1);
        QCOMPARE(admins->item(0, 0)->text() + " | " + admins->item(0, 1)->text(), QString("principal | Principal"));
        auto *create = button(*section, "Criar administrador");
        auto *reset = button(*section, "Redefinir senha");
        auto *inactivate = button(*section, "Inativar conta");
        QVERIFY(create && reset && inactivate);
        QVERIFY(create->isEnabled());
        auto *limit = section->findChild<QLabel *>("adminLimit");
        QVERIFY(limit);
        QVERIFY(limit->isHidden());
        admins->setCurrentCell(0, 0);
        admins->selectRow(0);
        QVERIFY(!reset->isEnabled());
        QVERIFY(!inactivate->isEnabled());
        ctx.accounts.createAdministrator(ctx.session, "admin.um", ctx.auth.preparePassword(adminPassword),
                                         InitialPasswordMode::Assisted);
        ctx.accounts.createAdministrator(ctx.session, "admin.dois", ctx.auth.preparePassword(adminPassword),
                                         InitialPasswordMode::Temporary);
        page.reload();
        QCOMPARE(count->text(), QString("3 de 3 ativos"));
        QCOMPARE(admins->rowCount(), 3);
        QVERIFY(!create->isEnabled());
        QVERIFY(!limit->isHidden());
        QCOMPARE(
            limit->text(),
            QString(
                "Limite de três administradores ativos atingido. Inative outro administrador para liberar uma vaga."));
        admins->setCurrentCell(2, 0);
        admins->selectRow(2);
        QCOMPARE(admins->item(2, 1)->text(), QString("Administrador"));
        QVERIFY(reset->isEnabled());
        QVERIFY(inactivate->isEnabled());
        QCOMPARE(inactivate->objectName(), QString("destructive"));
        QVERIFY(page.grab().save("ui-admins.png"));
        const Session admin = ctx.auth.login("admin.um", adminPassword);
        AccountsPage adminPage(ctx.accounts, ctx.auth, ctx.recovery, admin);
        QVERIFY(!adminPage.findChild<QWidget *>("adminSection"));
        QVERIFY(!adminPage.findChild<QWidget *>("recoverySection"));
        const Session common = ctx.createCommonAndLogin(
            "operador.um", QByteArrayLiteral("frase pessoal do operador um"), InitialPasswordMode::Assisted);
        MainWindow window(ctx.store, ctx.auth, ctx.accounts, ctx.recovery, common, path, ctx.locations, ctx.people);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.findChild<QLabel *>("sessionDeadline")->isHidden());
    }
    void enrollmentDialog() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("enroll.sqlite"));
        AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, ctx.session);
        page.resize(1100, 820);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *status = page.findChild<QLabel *>("recoveryStatus");
        QVERIFY(status);
        QCOMPARE(status->text(), QString("Recuperação não configurada"));
        int side = 0, listed = 0;
        bool whiteCorners = false, fullSize = false, finishLocked = false;
        QString typed;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto *qr = dialog->findChild<QrCodeView *>("enrollmentQr");
            if (!qr || qr->isEmpty()) {
                dialog->reject();
                return;
            }
            fullSize = qr->width() == qr->sizeHint().width() && qr->height() == qr->sizeHint().height();
            const QImage image = qr->grab().toImage();
            side = std::min(qr->width(), qr->height());
            whiteCorners = image.pixelColor(0, 0) == Qt::white &&
                           image.pixelColor(image.width() - 1, image.height() - 1) == Qt::white;
            QVERIFY(dialog->grab().save("ui-enrollment.png"));
            const QByteArray secret = ctx.memoryVault.entries.values().value(0);
            typed = totp::code(secret, totp::counterAt(ctx.clock.nowUtc()));
            dialog->findChild<QLineEdit *>("totpCode")->setText(typed);
            button(*dialog, "Confirmar vínculo")->click();
            listed = dialog->findChild<QPlainTextEdit *>("backupCodes")->toPlainText().split('\n').size();
            auto *finish = button(*dialog, "Concluir configuração");
            finishLocked = finish && !finish->isEnabled();
            dialog->findChild<QCheckBox *>("codesSaved")->setChecked(true);
            finish->click();
        });
        button(page, "Vincular Google Authenticator")->click();
        QCOMPARE(listed, 10);
        QVERIFY(finishLocked);
        QVERIFY2(fullSize, "QR cortado: o widget não acompanhou o sizeHint após setContent");
        QVERIFY(side >= QrCodeView::MinimumSide);
        QVERIFY(whiteCorners);
        QCOMPARE(typed.size(), 6);
        QVERIFY2(status->text().startsWith("Recuperação configurada em "), qPrintable(status->text()));
        QCOMPARE(page.findChild<QLabel *>("feedback")->text(), QString("✓  Google Authenticator vinculado."));
        QVERIFY(ctx.recovery.status(ctx.session).configured);
    }
    void recoveryFlow() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("recovery.sqlite"));
        auto draft = ctx.recovery.beginEnrollment(ctx.session);
        const QByteArray secret = ctx.memoryVault.entries.values().value(0);
        const QStringList codes = ctx.recovery.confirmEnrollment(
            ctx.session, *draft, totp::code(secret, totp::counterAt(ctx.clock.nowUtc())));
        LoginDialog reauth(ctx.auth, ctx.recovery, LoginDialog::Mode::Reauthentication);
        QVERIFY(!button(reauth, "Recuperar conta principal"));
        LoginDialog login(ctx.auth, ctx.recovery);
        login.show();
        QVERIFY(QTest::qWaitForWindowExposed(&login));
        auto *link = button(login, "Recuperar conta principal");
        QVERIFY(link);
        QString refused, busyText;
        bool codeCleared = false, codeFocused = false, closed = false;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog == &login)
                return;
            auto *submit = button(*dialog, "Redefinir senha");
            const auto fill = [&](const QString &proof) {
                dialog->findChild<QLineEdit *>("recoveryUsername")->setText(TestPrincipal);
                dialog->findChild<QRadioButton *>("methodBackup")->setChecked(true);
                dialog->findChild<QLineEdit *>("recoveryCode")->setText(proof);
                dialog->findChild<QLineEdit *>("newPassword")->setText("nova senha pela tela");
                dialog->findChild<QLineEdit *>("confirmPassword")->setText("nova senha pela tela");
            };
            fill("ZZZZ-ZZZZ-ZZZZ-ZZZZ");
            submit->click();
            busyText = submit->text();
            auto *error = dialog->findChild<QLabel *>("errorText");
            if (!QTest::qWaitFor(
                    [&] {
                        return !error->isHidden();
                    },
                    5000))
                return;
            refused = error->text();
            auto *code = dialog->findChild<QLineEdit *>("recoveryCode");
            codeCleared = code->text().isEmpty() && dialog->findChild<QLineEdit *>("newPassword")->text().isEmpty();
            codeFocused = dialog->focusWidget() == code;
            fill(codes[0]);
            submit->click();
            closed = QTest::qWaitFor(
                [&] {
                    return !dialog->isVisible();
                },
                5000);
        });
        link->click();
        QCOMPARE(busyText, QString("Verificando código…"));
        QCOMPARE(refused,
                 QString("Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método."));
        QVERIFY(codeCleared);
        QVERIFY(codeFocused);
        QVERIFY(closed);
        QCOMPARE(login.findChild<QLabel *>("feedback")->text(), QString("Senha redefinida. Entre com a nova senha."));
        QVERIFY(login.isVisible());
        ctx.auth.login(TestPrincipal, "nova senha pela tela");
    }
    void backupCodesStep() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("codes.sqlite"));
        AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, ctx.session);
        page.resize(1100, 820);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        bool lockedBeforeCheck = false, keptAfterBack = false, clearedOnClose = false, closed = false,
             askedTwice = false;
        QString question;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            const QByteArray secret = ctx.memoryVault.entries.values().value(0);
            dialog->findChild<QLineEdit *>("totpCode")
                ->setText(totp::code(secret, totp::counterAt(ctx.clock.nowUtc())));
            button(*dialog, "Confirmar vínculo")->click();
            auto *list = dialog->findChild<QPlainTextEdit *>("backupCodes");
            lockedBeforeCheck = !button(*dialog, "Concluir configuração")->isEnabled();
            QTimer::singleShot(0, this, [&] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!box)
                    return;
                question = box->text();
                button(*box, "Voltar aos códigos")->click();
            });
            dialog->reject();
            keptAfterBack = dialog->isVisible() && list->toPlainText().split('\n').size() == 10;
            QVERIFY(dialog->grab().save("ui-backup-codes.png"));
            QTimer::singleShot(0, this, [&] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!box)
                    return;
                askedTwice = true;
                button(*box, "Fechar mesmo assim")->click();
            });
            dialog->reject();
            clearedOnClose = list->toPlainText().isEmpty();
            closed = !dialog->isVisible();
        });
        button(page, "Vincular Google Authenticator")->click();
        QVERIFY(lockedBeforeCheck);
        QVERIFY(keptAfterBack);
        QVERIFY(askedTwice);
        QVERIFY(clearedOnClose);
        QVERIFY(closed);
        QCOMPARE(question, QString("Fechar sem confirmar que guardou os códigos? Eles não serão exibidos novamente."));
        QVERIFY(ctx.recovery.status(ctx.session).configured);
        QCOMPARE(page.findChild<QLabel *>("remainingCodes")->text(), QString("Códigos de recuperação restantes: 10"));
    }
    void rebindRequiredNotice() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        TestContext ctx(temp.filePath("rebind.sqlite"));
        auto draft = ctx.recovery.beginEnrollment(ctx.session);
        const QByteArray secret = ctx.memoryVault.entries.values().value(0);
        const QStringList codes = ctx.recovery.confirmEnrollment(
            ctx.session, *draft, totp::code(secret, totp::counterAt(ctx.clock.nowUtc())));
        ctx.recovery.recoverPrincipal(TestPrincipal, RecoveryMethod::BackupCode, codes[0],
                                      ctx.auth.preparePassword("senha depois do codigo"));
        const Session fresh = ctx.auth.login(TestPrincipal, "senha depois do codigo");
        AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, fresh);
        page.resize(1100, 820);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *section = page.findChild<QWidget *>("recoverySection");
        QVERIFY(section);
        auto *notice = section->findChild<QLabel *>("errorText");
        QVERIFY(notice);
        QVERIFY(!notice->isHidden());
        QCOMPARE(notice->text(), QString("O vínculo do Authenticator foi removido porque um código de recuperação foi "
                                         "usado. Vincule novamente para gerar novos códigos."));
        QVERIFY(section->findChild<QLabel *>("recoveryStatus")->isHidden());
        QCOMPARE(section->findChild<QLabel *>("remainingCodes")->text(),
                 QString("Códigos de recuperação restantes: 9"));
        auto *enroll = button(*section, "Vincular Google Authenticator");
        QVERIFY(enroll && enroll->isEnabled());
        QVERIFY(page.grab().save("ui-rebind-notice.png"));
    }
    void enrollmentErrors() {
        const auto withDialog = [this](AccountsPage &page, const std::function<void(QDialog *)> &check) {
            bool ran = false;
            QTimer::singleShot(0, this, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                ran = true;
                check(dialog);
                if (dialog->isVisible())
                    button(*dialog, "Cancelar")->click();
            });
            button(page, "Vincular Google Authenticator")->click();
            return ran;
        };
        {
            QTemporaryDir temp;
            QVERIFY(temp.isValid());
            TestContext ctx(temp.filePath("wrong.sqlite"));
            AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, ctx.session);
            page.show();
            QVERIFY(QTest::qWaitForWindowExposed(&page));
            bool qrKept = false, focused = false, retry = false;
            QString error;
            QVERIFY(withDialog(page, [&](QDialog *dialog) {
                const QByteArray secret = ctx.memoryVault.entries.values().value(0);
                const QString right = totp::code(secret, totp::counterAt(ctx.clock.nowUtc()));
                auto *field = dialog->findChild<QLineEdit *>("totpCode");
                field->setText(QString::number((right.toInt() + 500000) % 1000000).rightJustified(6, '0'));
                button(*dialog, "Confirmar vínculo")->click();
                qrKept = !dialog->findChild<QrCodeView *>("enrollmentQr")->isEmpty();
                error = dialog->findChild<QLabel *>("errorText")->text();
                focused = dialog->focusWidget() == field && field->selectedText() == field->text();
                retry = button(*dialog, "Confirmar vínculo") && button(*dialog, "Confirmar vínculo")->isEnabled();
            }));
            QVERIFY(qrKept);
            QVERIFY(focused);
            QVERIFY(retry);
            QCOMPARE(error,
                     QString("Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método."));
            QVERIFY(!ctx.recovery.status(ctx.session).configured);
            QVERIFY(ctx.memoryVault.entries.isEmpty());
        }
        {
            QTemporaryDir temp;
            QVERIFY(temp.isValid());
            FailingSecretVault failing;
            failing.failOn = FailingSecretVault::Stage::Write;
            TestContext ctx(temp.filePath("vault.sqlite"), true, &failing);
            AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, ctx.session);
            page.show();
            QVERIFY(QTest::qWaitForWindowExposed(&page));
            bool qrHidden = false, confirmDisabled = false;
            QString error;
            QVERIFY(withDialog(page, [&](QDialog *dialog) {
                qrHidden = dialog->findChild<QScrollArea *>("qrArea")->isHidden() &&
                           dialog->findChild<QrCodeView *>("enrollmentQr")->isEmpty();
                confirmDisabled = !button(*dialog, "Confirmar vínculo")->isEnabled();
                error = dialog->findChild<QLabel *>("errorText")->text();
            }));
            QVERIFY(qrHidden);
            QVERIFY(confirmDisabled);
            QCOMPARE(error, QString("Não foi possível preparar a recuperação. Tente novamente. Se o problema "
                                    "continuar, procure o responsável pela instalação."));
            QVERIFY(!ctx.recovery.status(ctx.session).configured);
            QVERIFY(failing.entries.isEmpty());
        }
        {
            QTemporaryDir temp;
            QVERIFY(temp.isValid());
            TestContext ctx(temp.filePath("cancel.sqlite"));
            auto first = ctx.recovery.beginEnrollment(ctx.session);
            const QString activeKey = ctx.memoryVault.entries.keys().value(0);
            ctx.recovery.confirmEnrollment(
                ctx.session, *first,
                totp::code(ctx.memoryVault.entries.value(activeKey), totp::counterAt(ctx.clock.nowUtc())));
            const auto before = ctx.recovery.status(ctx.session);
            QVERIFY(before.configured);
            AccountsPage page(ctx.accounts, ctx.auth, ctx.recovery, ctx.session);
            page.show();
            QVERIFY(QTest::qWaitForWindowExposed(&page));
            qsizetype during = 0;
            QVERIFY(withDialog(page, [&](QDialog *) {
                during = ctx.memoryVault.entries.size();
            }));
            QCOMPARE(during, qsizetype(2));
            const auto after = ctx.recovery.status(ctx.session);
            QCOMPARE(after.generation, before.generation);
            QCOMPARE(after.enrolledAtUtc, before.enrolledAtUtc);
            QVERIFY(after.configured);
            QCOMPARE(ctx.memoryVault.entries.keys(), QStringList{activeKey});
        }
    }
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"

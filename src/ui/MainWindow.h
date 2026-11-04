#pragma once
#include <QMainWindow>
#include <QStringList>
#include "application/AccountService.h"
#include "application/RecoveryService.h"
#include "application/HistoryQueryService.h"
#include "storage/NotebookStore.h"
#include <exception>
#include <functional>
class AccountsPage;
class LocationsPage;
class LocationService;
class PeopleService;
class PeoplePage;
class LogsPage;
class QTableWidget;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QStackedWidget;
class QTabWidget;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(NotebookStore &store, AuthService &auth, AccountService &accounts, RecoveryService &recovery, Session session, const QString &path, LocationService &locations, PeopleService &people);
    const Session &activeSession() const { return session; }
    void showPage(const QString &key);
    // Abaixo desta altura (tela de notebook, 1366×768) margens, linhas e controles ficam compactos.
    static constexpr int CompactHeight = 820;
    bool isCompact() const { return compact; }
signals:
    void logoutRequested();
    // Reautenticação com outra conta (ou que exige troca de senha): a janela e seus rascunhos devem ser descartados.
    void sessionReplaced(const Session &session);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    NotebookStore &store;
    HistoryQueryService historyService;
    AuthService &auth;
    AccountService &accounts;
    RecoveryService &recovery;
    LocationService &locations;
    PeopleService &people;
    Session session;
    QVector<Notebook> data;
    QWidget *content;
    QStackedWidget *pages;
    QVector<QPushButton *> navButtons;
    AccountsPage *accountsPage = nullptr;
    LocationsPage *locationsPage = nullptr;
    PeoplePage *peoplePage = nullptr;
    LogsPage *logsPage = nullptr;
    QTabWidget *inventoryTabs;
    QTableWidget *availableTable, *loanedTable, *maintenanceTable;
    QLabel *availableEmpty, *loanedEmpty, *maintenanceEmpty;
    QLineEdit *search;
    QComboBox *activityFilter;
    QLabel *total, *available, *loaned, *maintenance, *feedback, *sessionDeadline, *inventorySummary, *inactivateReason, *emptyTitle, *emptyBody;
    // Matriz D-10: Disponíveis (local, manutenção, retirada); Indisponíveis (local, devolução); Manutenção (concluir).
    QPushButton *availableLocationButton, *sendMaintenanceButton, *checkoutButton, *loanedLocationButton, *returnButton, *finishMaintenanceButton, *editButton;
    QPushButton *inactivateButton = nullptr, *reactivateButton = nullptr;
    bool locked = false, reauthenticating = false, compact = false, densityApplied = false;
    void applyDensity(bool value);
    void showDeadline();
    void setLocked(bool value);
    void sessionExpired();
    bool handleExpired(const std::exception &e);
    void closeOpenDialogs();
    void refresh();
    void renderInventory();
    void updateActions();
    void addNotebook();
    void checkout();
    void checkin();
    // 03-01: retirada (e devolução na 03-03) por pessoa cadastrada e local ativo, ambos por id.
    void movementDialog(bool returning);
    void updateLocation();
    void sendToMaintenance();
    void finishMaintenance();
    QTableWidget *currentTable() const;
    void editNotebook();
    void inactivateNotebook();
    void reactivateNotebook();
    void selectNotebook(qint64 id);
    const Notebook *selected() const;
    void perform(const std::function<void()> &action, const QString &message);
};

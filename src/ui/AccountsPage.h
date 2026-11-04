#pragma once
#include "application/AccountService.h"
#include "application/RecoveryService.h"
#include <QWidget>
#include <functional>
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QVBoxLayout;

// Página Contas: toda regra e gravação ficam no AccountService, chamado com a Session ativa.
// As seções Recuperação e Administradores só existem para a principal.
class AccountsPage : public QWidget {
public:
    AccountsPage(AccountService &accounts, AuthService &auth, RecoveryService &recovery, const Session &session, QWidget *parent = nullptr);
    void reload();
    // Chamado quando a carga ou uma ação é recusada por prazo administrativo vencido.
    std::function<void()> onExpired;
private:
    AccountService &accounts;
    AuthService &auth;
    RecoveryService &recovery;
    const Session &session;
    QVector<AccountSummary> rows, admins;
    QLabel *recoveryStatus = nullptr, *rebindNotice = nullptr, *remainingCodes = nullptr, *fewCodes = nullptr;
    QPushButton *enrollButton = nullptr;
    void buildRecovery(QVBoxLayout *layout);
    void reloadRecovery();
    void enroll();
    QLineEdit *search;
    QComboBox *filter;
    QStackedWidget *states;
    QTableWidget *table, *adminTable = nullptr;
    QLabel *status, *emptyTitle, *emptyBody, *loadError, *feedback, *adminCount = nullptr, *adminLimit = nullptr;
    QPushButton *resetButton, *inactivateButton, *createAdminButton = nullptr, *adminResetButton = nullptr, *adminInactivateButton = nullptr;
    const AccountSummary *selected() const;
    void updateActions();
    void createAccount();
    void resetPassword();
    void inactivate();
    void succeed(const QString &message);
    bool confirmInactivation(const QString &question);
    bool reportExpired(const std::exception &e);
    void buildAdministrators(QVBoxLayout *layout);
    void reloadAdministrators();
    const AccountSummary *selectedAdministrator() const;
    void updateAdministratorActions();
    void createAdministrator();
    void resetAdministratorPassword();
    void inactivateAdministrator();
};

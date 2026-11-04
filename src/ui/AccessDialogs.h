#pragma once
#include "application/AuthService.h"
#include "application/RecoveryService.h"
#include <QDialog>
class QLabel;
class QLineEdit;
class QPushButton;

// Banco sem principal: aceita somente quando hasPrincipal() passar a ser verdadeiro.
class BootstrapPendingDialog : public QDialog {
public:
    explicit BootstrapPendingDialog(AuthService &auth, QWidget *parent = nullptr);
};

class LoginDialog : public QDialog {
public:
    // Reauthentication não oferece "Recuperar conta principal".
    enum class Mode {
        Login,
        Reauthentication
    };
    LoginDialog(AuthService &auth, RecoveryService &recovery, Mode mode = Mode::Login, QWidget *parent = nullptr);
    Session session() const { return issued; }
    // Prazo administrativo vencido: mostra a expiração, pré-preenche o usuário e oferece Cancelar (mantém o bloqueio) e
    // Sair.
    void setReauthentication(const QString &expiredUsername);
    bool logoutChosen() const { return leaving; }
    void reject() override;

private:
    AuthService &auth;
    RecoveryService &recovery;
    Session issued;
    QLineEdit *username, *password;
    QLabel *error, *feedback;
    QPushButton *submit, *leave, *cancel, *recover = nullptr;
    void openRecovery();
    bool busy = false, leaving = false;
    void attempt();
    void setBusy(bool value);
    void showFailure();
};

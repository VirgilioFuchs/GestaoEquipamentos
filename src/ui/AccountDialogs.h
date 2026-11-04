#pragma once
#include "application/AccountService.h"
#include <QDialog>
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QWidget;

// Required: troca obrigatória da própria senha temporária. Assisted: cria a conta comum com a senha escolhida
// pelo operador. Reset: redefine a senha de uma conta comum existente (temporária ou assistida).
class PasswordSetDialog : public QDialog {
public:
    enum class Mode {
        Required,
        Assisted,
        Reset
    };
    PasswordSetDialog(AuthService &auth, Session &session, QWidget *parent = nullptr);
    // kind Administrator: Assisted cria administrador e Reset redefine a senha de administrador (somente a principal).
    PasswordSetDialog(Mode mode, AuthService &auth, AccountService &accounts, const Session &session,
                      const QString &username, qint64 accountId = 0, QWidget *parent = nullptr,
                      AccountRole kind = AccountRole::Common);
    qint64 accountId() const { return target; }
    void reject() override;

private:
    Mode mode;
    AccountRole kind = AccountRole::Common;
    AuthService &auth;
    AccountService *accounts = nullptr;
    Session *ownSession = nullptr;
    const Session *actingSession = nullptr;
    QString username;
    qint64 target = 0;
    QLineEdit *password, *confirm;
    QRadioButton *temporary = nullptr;
    QLabel *error;
    QPushButton *submit;
    bool busy = false;
    void build();
    void attempt();
    void setBusy(bool value);
    void showError(const QString &message, QWidget *focus);
};

class CreateAccountDialog : public QDialog {
public:
    CreateAccountDialog(AuthService &auth, AccountService &accounts, const Session &session, QWidget *parent = nullptr,
                        AccountRole kind = AccountRole::Common);
    qint64 accountId() const { return created; }
    // Definição assistida: a etapa de nome fecha sem gravar; a conta só é criada pelo PasswordSetDialog Assisted.
    QString assistedUsername() const { return assistedName; }
    void reject() override;

private:
    AuthService &auth;
    AccountService &accounts;
    const Session &session;
    AccountRole kind;
    qint64 created = 0;
    QString assistedName;
    QLineEdit *username, *password, *confirm;
    QRadioButton *temporary, *assisted;
    QWidget *passwordFields;
    QLabel *error;
    QPushButton *submit;
    bool busy = false;
    void attempt();
    void setBusy(bool value);
    void showError(const QString &message, QWidget *focus);
};

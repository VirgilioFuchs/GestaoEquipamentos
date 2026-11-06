#pragma once
#include "application/AuthService.h"
#include "application/RecoveryService.h"
#include <QDialog>
#include <functional>
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

// Assistente de primeira configuração: aparece só quando o banco ainda não tem conta principal.
// Etapas: boas-vindas → conta principal → Authenticator (opcional) → concluído. A conta é criada por
// AuthService::bootstrapPrincipal, que recusa uma segunda principal; depois disso não há como voltar.
class FirstRunWizard : public QDialog {
public:
    enum Step {
        Welcome,
        Account,
        Authenticator,
        Done
    };
    // Abre o vínculo do Authenticator; o padrão usa EnrollmentDialog. Substituível em testes.
    using EnrollmentLauncher = std::function<void(RecoveryService &, const Session &, QWidget *)>;

    FirstRunWizard(AuthService &auth, RecoveryService &recovery, QWidget *parent = nullptr);
    ~FirstRunWizard() override;
    Step step() const { return current; }
    // Nome canônico da conta criada (vazio enquanto a conta não existir).
    QString createdUsername() const { return created; }
    void setEnrollmentLauncher(EnrollmentLauncher launcher) { launchEnrollment = std::move(launcher); }
    // Mostra uma etapa sem executar ações (usado pela captura de tela da documentação).
    void showStep(Step step);
    void reject() override;

private:
    AuthService &auth;
    RecoveryService &recovery;
    Session session;
    Step current = Welcome;
    QString created;
    bool existed = false;
    EnrollmentLauncher launchEnrollment;
    QStackedWidget *pages;
    QLabel *stepCaption, *error, *authenticatorStatus, *doneText;
    QLineEdit *username, *password, *confirm;
    QPushButton *back, *next, *quit, *enroll;
    void goNext();
    void createAccount();
    void openEnrollment();
    void refreshButtons();
    void endSession();
};

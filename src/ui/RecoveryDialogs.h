#pragma once
#include "application/RecoveryService.h"
#include <QDialog>
#include <memory>
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QWidget;
class QrCodeView;

// Vínculo do Google Authenticator: somente a principal. Cancelar, Esc ou fechar descartam o candidato do cofre
// sem tocar no vínculo anterior; o vínculo só fica ativo quando confirmEnrollment aceita o código.
// Depois do vínculo, a segunda etapa mostra os códigos de recuperação uma única vez.
class EnrollmentDialog : public QDialog {
public:
    EnrollmentDialog(RecoveryService &recovery, const Session &session, QWidget *parent = nullptr);
    ~EnrollmentDialog() override;
    // A sessão administrativa venceu durante o diálogo; quem abriu deve bloquear a janela.
    bool sessionExpired() const { return expired; }
    void reject() override;

private:
    RecoveryService &recovery;
    const Session &session;
    std::unique_ptr<EnrollmentDraft> draft;
    QLabel *title, *error;
    QWidget *enrollPage, *codesPage;
    QrCodeView *qr;
    QScrollArea *qrArea;
    QLineEdit *code;
    QPlainTextEdit *codes;
    QCheckBox *saved;
    QPushButton *submit, *finish;
    bool busy = false, expired = false, showingCodes = false;
    void unavailable(const QString &message);
    void discard();
    void attempt();
    void showCodes(const QStringList &list);
    void clearCodes();
};

// Recuperação da principal a partir do login: o worker só deriva a nova senha; recoverPrincipal roda na thread da
// interface.
class RecoveryDialog : public QDialog {
public:
    RecoveryDialog(AuthService &auth, RecoveryService &recovery, QWidget *parent = nullptr);
    void reject() override;

private:
    AuthService &auth;
    RecoveryService &recovery;
    QLineEdit *username, *code, *password, *confirm;
    QRadioButton *authenticator, *backup;
    QLabel *error;
    QPushButton *submit, *cancel;
    bool busy = false;
    void methodChanged();
    void attempt();
    void setBusy(bool value);
    void refuse(const QString &message, QLineEdit *focus);
};

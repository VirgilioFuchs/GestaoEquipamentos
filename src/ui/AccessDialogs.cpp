#include "AccessDialogs.h"
#include "RecoveryDialogs.h"
#include <QtWidgets>
#include <exception>
#include <memory>

namespace {
const QString LoginFailure =
    QStringLiteral("Não foi possível entrar. Confira o nome de usuário e a senha e tente novamente.");
const QString SessionExpired = QStringLiteral("Sessão administrativa expirada. Entre novamente para continuar.");
} // namespace

BootstrapPendingDialog::BootstrapPendingDialog(AuthService &auth, QWidget *parent) : QDialog(parent) {
    setWindowTitle("GestaoEquipamentos");
    setMinimumWidth(430);
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(14);
    auto *title = new QLabel("Configuração inicial necessária");
    title->setObjectName("dialogTitle");
    layout->addWidget(title);
    auto *body = new QLabel("Peça ao responsável que execute o utilitário local de configuração para criar a conta "
                            "principal. Depois, selecione Tentar novamente.");
    body->setWordWrap(true);
    layout->addWidget(body);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    auto *quit = new QPushButton("Sair");
    auto *retry = new QPushButton("Tentar novamente");
    retry->setObjectName("primary");
    retry->setDefault(true);
    actions->addWidget(quit);
    actions->addWidget(retry);
    layout->addLayout(actions);
    connect(quit, &QPushButton::clicked, this, &QDialog::reject);
    connect(retry, &QPushButton::clicked, this, [this, &auth] {
        if (auth.hasPrincipal())
            accept();
    });
}

LoginDialog::LoginDialog(AuthService &auth, RecoveryService &recovery, Mode mode, QWidget *parent)
    : QDialog(parent), auth(auth), recovery(recovery) {
    setWindowTitle("GestaoEquipamentos");
    setMinimumWidth(430);
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(14);
    auto *title = new QLabel("Entrar no GestaoEquipamentos");
    title->setObjectName("dialogTitle");
    layout->addWidget(title);
    auto *form = new QFormLayout;
    username = new QLineEdit;
    username->setObjectName("loginUsername");
    username->setMaxLength(64);
    password = new QLineEdit;
    password->setObjectName("loginPassword");
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(128);
    form->addRow("Nome de usuário", username);
    form->addRow("Senha", password);
    layout->addLayout(form);
    error = new QLabel;
    error->setObjectName("errorText");
    error->setWordWrap(true);
    error->setTextFormat(Qt::PlainText);
    error->hide();
    layout->addWidget(error);
    feedback = new QLabel;
    feedback->setObjectName("feedback");
    feedback->setWordWrap(true);
    feedback->setTextFormat(Qt::PlainText);
    feedback->hide();
    layout->addWidget(feedback);
    auto *actions = new QHBoxLayout;
    leave = new QPushButton("Sair");
    leave->hide();
    actions->addWidget(leave);
    actions->addStretch();
    cancel = new QPushButton("Cancelar");
    cancel->hide();
    actions->addWidget(cancel);
    submit = new QPushButton("Acessar conta");
    submit->setObjectName("primary");
    submit->setDefault(true);
    actions->addWidget(submit);
    layout->addLayout(actions);
    if (mode == Mode::Login) {
        recover = new QPushButton("Recuperar conta principal");
        recover->setObjectName("linkButton");
        recover->setFlat(true);
        recover->setCursor(Qt::PointingHandCursor);
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(recover);
        layout->addLayout(row);
        connect(recover, &QPushButton::clicked, this, &LoginDialog::openRecovery);
    }
    connect(submit, &QPushButton::clicked, this, &LoginDialog::attempt);
    connect(cancel, &QPushButton::clicked, this, &LoginDialog::reject);
    connect(leave, &QPushButton::clicked, this, [this] {
        if (busy)
            return;
        leaving = true;
        reject();
    });
    username->setFocus();
}

void LoginDialog::setReauthentication(const QString &expiredUsername) {
    username->setText(expiredUsername);
    error->setText(SessionExpired);
    error->show();
    leave->show();
    cancel->show();
    password->setFocus();
}

void LoginDialog::openRecovery() {
    if (busy)
        return;
    RecoveryDialog dialog(auth, recovery, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    password->clear();
    error->hide();
    feedback->setText("Senha redefinida. Entre com a nova senha.");
    feedback->show();
    username->setFocus();
}

void LoginDialog::reject() {
    if (busy)
        return;
    password->clear();
    QDialog::reject();
}

void LoginDialog::setBusy(bool value) {
    busy = value;
    submit->setEnabled(!value);
    submit->setText(value ? "Entrando…" : "Acessar conta");
    username->setEnabled(!value);
    password->setEnabled(!value);
    leave->setEnabled(!value);
    cancel->setEnabled(!value);
}

void LoginDialog::showFailure() {
    password->clear();
    error->setText(LoginFailure);
    error->show();
    password->setFocus();
}

void LoginDialog::attempt() {
    if (busy)
        return;
    std::shared_ptr<LoginAttempt> pending;
    try {
        pending = auth.prepareLogin(username->text(), password->text().toUtf8());
    } catch (const std::exception &) {
        showFailure();
        return;
    }
    setBusy(true);
    auto *worker = QThread::create([pending] {
        pending->verify();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, pending] {
        setBusy(false);
        try {
            issued = auth.completeLogin(*pending);
        } catch (const std::exception &) {
            showFailure();
            return;
        }
        password->clear();
        accept();
    });
    worker->start();
}

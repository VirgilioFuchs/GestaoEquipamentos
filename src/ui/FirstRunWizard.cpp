#include "FirstRunWizard.h"
#include "RecoveryDialogs.h"
#include "security/Crypto.h"
#include <QtWidgets>
#include <exception>

namespace {
const QString Origin = QStringLiteral("assistente de primeira configuração");

QLabel *textLabel(const QString &text, const char *name = nullptr) {
    auto *label = new QLabel(text);
    if (name)
        label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

QWidget *page(const QString &title, const QList<QWidget *> &content, QLayout *extra = nullptr) {
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    layout->addWidget(textLabel(title, "dialogTitle"));
    for (auto *item : content)
        layout->addWidget(item);
    if (extra)
        layout->addLayout(extra);
    layout->addStretch();
    return widget;
}
} // namespace

FirstRunWizard::FirstRunWizard(AuthService &auth, RecoveryService &recovery, QWidget *parent)
    : QDialog(parent), auth(auth), recovery(recovery) {
    setWindowTitle("Gestão de Equipamentos — Primeira configuração");
    setObjectName("firstRunWizard");
    setMinimumSize(520, 440);
    launchEnrollment = [](RecoveryService &service, const Session &active, QWidget *owner) {
        EnrollmentDialog dialog(service, active, owner);
        dialog.exec();
    };

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 20);
    layout->setSpacing(16);
    stepCaption = textLabel(QString(), "eyebrow");
    layout->addWidget(stepCaption);
    pages = new QStackedWidget;
    layout->addWidget(pages, 1);

    // Boas-vindas
    pages->addWidget(page(
        "Bem-vindo ao Gestão de Equipamentos",
        {textLabel("Este assistente prepara o aplicativo para o primeiro uso neste computador. Ele leva cerca de um "
                   "minuto."),
         textLabel("Você vai criar a conta principal: a conta de quem administra o sistema, cadastra outras contas e "
                   "pode recuperar o acesso. Os dados ficam guardados somente neste computador, na pasta do seu "
                   "usuário."),
         textLabel("A conta principal só pode ser criada uma vez.", "helpText")}));

    // Conta principal
    username = new QLineEdit;
    username->setObjectName("setupUsername");
    username->setMaxLength(64);
    username->setPlaceholderText("ex.: coordenacao");
    password = new QLineEdit;
    password->setObjectName("setupPassword");
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(128);
    confirm = new QLineEdit;
    confirm->setObjectName("setupConfirm");
    confirm->setEchoMode(QLineEdit::Password);
    confirm->setMaxLength(128);
    auto *form = new QFormLayout;
    form->setSpacing(8);
    form->addRow("Nome de usuário", username);
    form->addRow("Senha", password);
    form->addRow("Confirmar senha", confirm);
    error = textLabel(QString(), "errorText");
    error->setObjectName("errorText");
    error->hide();
    auto *accountPage = page("Criar a conta principal",
                             {textLabel("Nome de usuário: 3 a 64 caracteres — letras sem acento, números, ponto, hífen "
                                        "ou sublinhado. Senha: 8 a 128 caracteres.",
                                        "helpText")},
                             form);
    static_cast<QVBoxLayout *>(accountPage->layout())->insertWidget(3, error);
    pages->addWidget(accountPage);

    // Authenticator (opcional)
    enroll = new QPushButton("Vincular agora");
    enroll->setObjectName("setupEnroll");
    authenticatorStatus = textLabel(QString(), "feedback");
    authenticatorStatus->hide();
    auto *enrollRow = new QHBoxLayout;
    enrollRow->addWidget(enroll);
    enrollRow->addStretch();
    pages->addWidget(page(
        "Recuperação de acesso (opcional)",
        {textLabel("Recomendamos vincular um aplicativo autenticador (Google Authenticator, Microsoft Authenticator ou "
                   "similar) à conta principal. Se a senha for esquecida, um código do aplicativo — ou um dos códigos "
                   "de recuperação gerados no vínculo — permite definir uma nova senha."),
         textLabel("Você pode pular esta etapa e vincular depois, em Contas › Vincular Google Authenticator.",
                   "helpText"),
         authenticatorStatus},
        enrollRow));

    // Concluído
    doneText = textLabel(QString());
    pages->addWidget(page("Tudo pronto", {doneText}));

    auto *actions = new QHBoxLayout;
    quit = new QPushButton("Sair");
    back = new QPushButton("Voltar");
    next = new QPushButton;
    next->setObjectName("primary");
    next->setDefault(true);
    actions->addWidget(quit);
    actions->addStretch();
    actions->addWidget(back);
    actions->addWidget(next);
    layout->addLayout(actions);

    connect(quit, &QPushButton::clicked, this, &FirstRunWizard::reject);
    connect(back, &QPushButton::clicked, this, [this] {
        if (current == Account)
            showStep(Welcome);
    });
    connect(next, &QPushButton::clicked, this, &FirstRunWizard::goNext);
    connect(enroll, &QPushButton::clicked, this, &FirstRunWizard::openEnrollment);
    for (auto *field : {username, password, confirm})
        connect(field, &QLineEdit::returnPressed, this, &FirstRunWizard::goNext);
    showStep(Welcome);
}

FirstRunWizard::~FirstRunWizard() { endSession(); }

void FirstRunWizard::showStep(Step step) {
    current = step;
    pages->setCurrentIndex(int(step));
    stepCaption->setText(QString("ETAPA %1 DE 4").arg(int(step) + 1));
    if (step == Done)
        doneText->setText(
            existed ? QStringLiteral("A conta principal já estava configurada neste computador. Entre com ela na "
                                     "próxima tela.")
                    : QStringLiteral("A conta principal \"%1\" foi criada. Na próxima tela, entre com esse nome de "
                                     "usuário e a senha que você acabou de definir.")
                          .arg(created.isEmpty() ? username->text().trimmed().toLower() : created));
    refreshButtons();
    if (step == Account)
        username->setFocus();
}

void FirstRunWizard::refreshButtons() {
    const bool accountExists = !created.isEmpty() || existed;
    back->setVisible(current == Account);
    quit->setVisible(!accountExists);
    switch (current) {
    case Welcome:
        next->setText("Começar");
        break;
    case Account:
        next->setText("Criar conta");
        break;
    case Authenticator:
        next->setText(enroll->isEnabled() ? "Pular" : "Próximo");
        break;
    case Done:
        next->setText("Ir para o login");
        break;
    }
}

void FirstRunWizard::goNext() {
    switch (current) {
    case Welcome:
        showStep(Account);
        break;
    case Account:
        createAccount();
        break;
    case Authenticator:
        endSession();
        showStep(Done);
        break;
    case Done:
        accept();
        break;
    }
}

void FirstRunWizard::createAccount() {
    const auto refuse = [this](const QString &message, QLineEdit *focus) {
        error->setText(message);
        error->show();
        focus->setFocus();
        focus->selectAll();
    };
    if (password->text() != confirm->text()) {
        refuse("A confirmação não confere com a senha.", confirm);
        return;
    }
    QByteArray secret = password->text().toUtf8();
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        // Mesmas regras e mesma derivação (Argon2id) do utilitário técnico: nada de criptografia duplicada aqui.
        const auto prepared = auth.preparePassword(secret);
        auth.bootstrapPrincipal(username->text(), prepared, Origin);
        created = AuthService::canonicalUsername(username->text());
        // Sessão curta só para o vínculo opcional do Authenticator; encerrada ao sair da etapa.
        session = auth.login(created, secret);
    } catch (const AuthError &failure) {
        QGuiApplication::restoreOverrideCursor();
        crypto::wipe(secret);
        if (failure.code() == "already_initialized") {
            existed = true;
            password->clear();
            confirm->clear();
            showStep(Done);
            return;
        }
        refuse(QString::fromUtf8(failure.what()),
               failure.code() == "invalid_username" ? username : password);
        return;
    } catch (const std::exception &failure) {
        QGuiApplication::restoreOverrideCursor();
        crypto::wipe(secret);
        refuse(QString("Não foi possível criar a conta: %1").arg(QString::fromUtf8(failure.what())), password);
        return;
    }
    QGuiApplication::restoreOverrideCursor();
    crypto::wipe(secret);
    password->clear();
    confirm->clear();
    error->hide();
    for (auto *field : {username, password, confirm})
        field->setEnabled(false);
    showStep(session.isValid() ? Authenticator : Done);
}

void FirstRunWizard::openEnrollment() {
    if (!session.isValid())
        return;
    launchEnrollment(recovery, session, this);
    bool configured = false;
    try {
        configured = recovery.status(session).configured;
    } catch (const std::exception &) {
        configured = false;
    }
    if (configured) {
        authenticatorStatus->setText("Authenticator vinculado. Guarde os códigos de recuperação em local seguro.");
        authenticatorStatus->show();
        enroll->setEnabled(false);
    }
    refreshButtons();
}

void FirstRunWizard::endSession() {
    if (session.isValid())
        auth.logout(session);
}

void FirstRunWizard::reject() {
    // Depois de criada a conta, fechar equivale a concluir: o login vem em seguida.
    endSession();
    if (!created.isEmpty() || existed)
        QDialog::accept();
    else
        QDialog::reject();
}

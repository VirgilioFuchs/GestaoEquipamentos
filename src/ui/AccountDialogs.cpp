#include "AccountDialogs.h"
#include "security/Crypto.h"
#include <QtWidgets>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

namespace {
const QString Mismatch = QStringLiteral("As senhas não coincidem. Confira a confirmação.");
const QString SaveFailure = QStringLiteral("Não foi possível salvar a conta. Confira os dados e tente novamente.");
const QString Policy = QStringLiteral("Use de 8 a 128 caracteres. Espaços e símbolos são permitidos.");

// Mensagens de regra (AuthError/AuthorizationError) são exibidas; falhas de armazenamento nunca mostram SQL.
QString failureText(const std::exception_ptr &failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const AuthError &e) {
        return QString::fromUtf8(e.what());
    } catch (const AuthorizationError &e) {
        return QString::fromUtf8(e.what());
    } catch (...) {
        return SaveFailure;
    }
}
QString failureText(const std::function<void()> &operation) {
    try {
        operation();
        return {};
    } catch (...) {
        return failureText(std::current_exception());
    }
}

// work roda fora da thread da interface e só executa o KDF; done volta à thread de context, dona da conexão.
void runKdf(QObject *context, std::function<void()> work, std::function<void(std::exception_ptr)> done) {
    auto failure = std::make_shared<std::exception_ptr>();
    auto *worker = QThread::create([work, failure] {
        try {
            work();
        } catch (...) {
            *failure = std::current_exception();
        }
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(worker, &QThread::finished, context, [done, failure] {
        done(*failure);
    });
    worker->start();
}

QLineEdit *secretField(const char *name) {
    auto *field = new QLineEdit;
    field->setObjectName(name);
    field->setEchoMode(QLineEdit::Password);
    field->setMaxLength(128);
    return field;
}
QLabel *textLabel(const QString &text, const char *name) {
    auto *label = new QLabel(text);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
} // namespace

PasswordSetDialog::PasswordSetDialog(AuthService &auth, Session &session, QWidget *parent)
    : QDialog(parent), mode(Mode::Required), auth(auth), ownSession(&session), username(session.username()) {
    build();
}

PasswordSetDialog::PasswordSetDialog(Mode mode, AuthService &auth, AccountService &accounts, const Session &session,
                                     const QString &username, qint64 accountId, QWidget *parent, AccountRole kind)
    : QDialog(parent), mode(mode), kind(kind), auth(auth), accounts(&accounts), actingSession(&session),
      username(username), target(accountId) {
    build();
}

void PasswordSetDialog::build() {
    setWindowTitle("GestaoEquipamentos");
    setMinimumWidth(mode == Mode::Required ? 430 : 480);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    const QString title = mode == Mode::Required   ? QStringLiteral("Defina sua senha pessoal para continuar.")
                          : mode == Mode::Assisted ? "Definir senha inicial de " + username +
                                                         ". Um administrador deve acompanhar esta configuração."
                                                   : "Redefinir senha de " + username;
    layout->addWidget(textLabel(title, "dialogTitle"));
    auto *form = new QFormLayout;
    form->setSpacing(8);
    if (mode != Mode::Required)
        form->addRow("Nome de usuário", textLabel(username, "fixedUsername"));
    if (mode == Mode::Reset) {
        temporary = new QRadioButton("Senha temporária");
        temporary->setChecked(true);
        auto *choices = new QVBoxLayout;
        choices->setSpacing(4);
        choices->addWidget(temporary);
        choices->addWidget(new QRadioButton("Definição assistida agora"));
        form->addRow("Senha inicial", choices);
    }
    password = secretField("newPassword");
    confirm = secretField("confirmPassword");
    form->addRow("Nova senha", password);
    form->addRow("Confirmar nova senha", confirm);
    layout->addLayout(form);
    layout->addWidget(textLabel(Policy, "helpText"));
    error = textLabel({}, "errorText");
    error->hide();
    layout->addWidget(error);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    auto *cancel = new QPushButton("Cancelar");
    submit = new QPushButton(mode == Mode::Reset ? "Redefinir senha" : "Definir senha");
    submit->setObjectName("primary");
    submit->setDefault(true);
    actions->addWidget(cancel);
    actions->addWidget(submit);
    layout->addLayout(actions);
    connect(cancel, &QPushButton::clicked, this, &PasswordSetDialog::reject);
    connect(submit, &QPushButton::clicked, this, &PasswordSetDialog::attempt);
    password->setFocus();
}

void PasswordSetDialog::reject() {
    if (busy)
        return;
    password->clear();
    confirm->clear();
    QDialog::reject();
}

void PasswordSetDialog::setBusy(bool value) {
    busy = value;
    submit->setEnabled(!value);
    submit->setText(value ? "Salvando…" : mode == Mode::Reset ? "Redefinir senha" : "Definir senha");
    password->setEnabled(!value);
    confirm->setEnabled(!value);
}

void PasswordSetDialog::showError(const QString &message, QWidget *focus) {
    error->setText(message);
    error->show();
    focus->setFocus();
}

void PasswordSetDialog::attempt() {
    if (busy)
        return;
    if (password->text() != confirm->text()) {
        confirm->clear();
        showError(Mismatch, confirm);
        return;
    }
    std::shared_ptr<PasswordChangeAttempt> change;
    if (mode == Mode::Required) {
        const QString refused = failureText([&] {
            change = std::make_shared<PasswordChangeAttempt>(auth.prepareRequiredPasswordChange(*ownSession));
        });
        if (!refused.isEmpty()) {
            showError(refused, password);
            return;
        }
    }
    auto secret = std::make_shared<QByteArray>(password->text().toUtf8());
    auto prepared = std::make_shared<std::optional<PreparedPassword>>();
    const AuthService *hashing = &auth;
    const InitialPasswordMode chosen = temporary && !temporary->isChecked() ? InitialPasswordMode::Assisted
                                       : mode == Mode::Assisted             ? InitialPasswordMode::Assisted
                                                                            : InitialPasswordMode::Temporary;
    setBusy(true);
    runKdf(
        this,
        [change, secret, prepared, hashing] {
            if (change)
                change->prepare(*secret);
            else
                *prepared = hashing->preparePassword(*secret);
        },
        [this, change, secret, prepared, chosen](std::exception_ptr failure) {
            crypto::wipe(*secret);
            setBusy(false);
            QString refused = failure ? failureText(failure) : failureText([&] {
                if (mode == Mode::Required)
                    auth.completeRequiredPasswordChange(*ownSession, *change);
                else if (mode == Mode::Assisted && kind == AccountRole::Administrator)
                    target = accounts->createAdministrator(*actingSession, username, **prepared,
                                                           InitialPasswordMode::Assisted);
                else if (mode == Mode::Assisted)
                    target = accounts->createCommonAccount(*actingSession, username, **prepared,
                                                           InitialPasswordMode::Assisted);
                else if (kind == AccountRole::Administrator)
                    accounts->resetAdministratorPassword(*actingSession, target, **prepared, chosen);
                else
                    accounts->resetCommonPassword(*actingSession, target, **prepared, chosen);
            });
            password->clear();
            confirm->clear();
            if (!refused.isEmpty()) {
                showError(refused, password);
                return;
            }
            accept();
        });
}

CreateAccountDialog::CreateAccountDialog(AuthService &auth, AccountService &accounts, const Session &session,
                                         QWidget *parent, AccountRole kind)
    : QDialog(parent), auth(auth), accounts(accounts), session(session), kind(kind) {
    setWindowTitle("GestaoEquipamentos");
    setMinimumWidth(480);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->addWidget(
        textLabel(kind == AccountRole::Administrator ? "Criar administrador" : "Criar conta comum", "dialogTitle"));
    auto *form = new QFormLayout;
    form->setSpacing(8);
    username = new QLineEdit;
    username->setObjectName("accountUsername");
    username->setMaxLength(64);
    form->addRow("Nome de usuário", username);
    temporary = new QRadioButton("Senha temporária");
    temporary->setChecked(true);
    assisted = new QRadioButton("Definição assistida agora");
    auto *choices = new QVBoxLayout;
    choices->setSpacing(4);
    choices->addWidget(temporary);
    choices->addWidget(assisted);
    form->addRow("Senha inicial", choices);
    layout->addLayout(form);
    passwordFields = new QWidget;
    auto *secrets = new QVBoxLayout(passwordFields);
    secrets->setContentsMargins(0, 0, 0, 0);
    secrets->setSpacing(8);
    auto *secretForm = new QFormLayout;
    secretForm->setSpacing(8);
    password = secretField("temporaryPassword");
    confirm = secretField("confirmTemporaryPassword");
    secretForm->addRow("Senha temporária", password);
    secretForm->addRow("Confirmar senha temporária", confirm);
    secrets->addLayout(secretForm);
    secrets->addWidget(textLabel(Policy, "helpText"));
    layout->addWidget(passwordFields);
    error = textLabel({}, "errorText");
    error->hide();
    layout->addWidget(error);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    auto *cancel = new QPushButton("Cancelar");
    submit = new QPushButton("Salvar conta");
    submit->setObjectName("primary");
    submit->setDefault(true);
    actions->addWidget(cancel);
    actions->addWidget(submit);
    layout->addLayout(actions);
    connect(temporary, &QRadioButton::toggled, this, [this](bool on) {
        passwordFields->setVisible(on);
        if (!on) {
            password->clear();
            confirm->clear();
        }
        adjustSize();
    });
    connect(cancel, &QPushButton::clicked, this, &CreateAccountDialog::reject);
    connect(submit, &QPushButton::clicked, this, &CreateAccountDialog::attempt);
    username->setFocus();
}

void CreateAccountDialog::reject() {
    if (busy)
        return;
    password->clear();
    confirm->clear();
    QDialog::reject();
}

void CreateAccountDialog::setBusy(bool value) {
    busy = value;
    submit->setEnabled(!value);
    submit->setText(value ? "Salvando…" : "Salvar conta");
    for (QWidget *field :
         {static_cast<QWidget *>(username), static_cast<QWidget *>(password), static_cast<QWidget *>(confirm),
          static_cast<QWidget *>(temporary), static_cast<QWidget *>(assisted)})
        field->setEnabled(!value);
}

void CreateAccountDialog::showError(const QString &message, QWidget *focus) {
    error->setText(message);
    error->show();
    focus->setFocus();
}

void CreateAccountDialog::attempt() {
    if (busy)
        return;
    QString canonical;
    const QString invalid = failureText([&] {
        canonical = AuthService::canonicalUsername(username->text());
    });
    if (!invalid.isEmpty()) {
        showError(invalid, username);
        return;
    }
    if (assisted->isChecked()) {
        assistedName = canonical;
        accept();
        return;
    }
    if (password->text() != confirm->text()) {
        confirm->clear();
        showError(Mismatch, confirm);
        return;
    }
    auto secret = std::make_shared<QByteArray>(password->text().toUtf8());
    auto prepared = std::make_shared<std::optional<PreparedPassword>>();
    const AuthService *hashing = &auth;
    setBusy(true);
    runKdf(
        this,
        [secret, prepared, hashing] {
            *prepared = hashing->preparePassword(*secret);
        },
        [this, secret, prepared, canonical](std::exception_ptr failure) {
            crypto::wipe(*secret);
            setBusy(false);
            const QString refused = failure ? failureText(failure) : failureText([&] {
                created =
                    kind == AccountRole::Administrator
                        ? accounts.createAdministrator(session, canonical, **prepared, InitialPasswordMode::Temporary)
                        : accounts.createCommonAccount(session, canonical, **prepared, InitialPasswordMode::Temporary);
            });
            password->clear();
            confirm->clear();
            if (!refused.isEmpty()) {
                showError(refused, password);
                return;
            }
            accept();
        });
}

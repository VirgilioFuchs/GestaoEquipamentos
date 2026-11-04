#include "RecoveryDialogs.h"
#include "QrCodeView.h"
#include <QtWidgets>
#include <exception>
#include <optional>

namespace {
const QString Unavailable = QStringLiteral("Não foi possível preparar a recuperação. Tente novamente. Se o problema "
                                           "continuar, procure o responsável pela instalação.");
const QString Instruction =
    QStringLiteral("Escaneie o QR code no Google Authenticator e informe o código exibido para confirmar.");
const QString CodesHelp = QStringLiteral(
    "Guarde estes códigos em local seguro. Cada código só pode ser usado uma vez. Não serão exibidos novamente.");
const QString CloseCodes =
    QStringLiteral("Fechar sem confirmar que guardou os códigos? Eles não serão exibidos novamente.");
const QString Mismatch = QStringLiteral("As senhas não coincidem. Confira a confirmação.");
const QString Policy = QStringLiteral("Use de 8 a 128 caracteres. Espaços e símbolos são permitidos.");
const QString RecoveryFailure = QStringLiteral("Não foi possível redefinir a senha. Tente novamente.");
QLabel *textLabel(const QString &text, const char *name) {
    auto *label = new QLabel(text);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
bool isExpiry(const std::exception &e) {
    const auto *refused = dynamic_cast<const AuthorizationError *>(&e);
    return refused && refused->reason() == AuthorizationError::Reason::Expired;
}
void wipeField(QLineEdit *field) {
    field->setText(QString(field->text().size(), QChar(' ')));
    field->clear();
}
} // namespace

EnrollmentDialog::EnrollmentDialog(RecoveryService &recovery, const Session &session, QWidget *parent)
    : QDialog(parent), recovery(recovery), session(session) {
    setWindowTitle("GestaoEquipamentos");
    setMinimumWidth(480);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    title = textLabel("Vincular Google Authenticator", "dialogTitle");
    layout->addWidget(title);

    enrollPage = new QWidget;
    auto *enroll = new QVBoxLayout(enrollPage);
    enroll->setContentsMargins(0, 0, 0, 0);
    enroll->setSpacing(16);
    enroll->addWidget(textLabel(Instruction, "helpText"));
    qr = new QrCodeView;
    qr->setObjectName("enrollmentQr");
    qrArea = new QScrollArea;
    qrArea->setObjectName("qrArea");
    qrArea->setFrameShape(QFrame::NoFrame);
    qrArea->setWidget(qr);
    qrArea->setAlignment(Qt::AlignCenter);
    qrArea->setMinimumHeight(QrCodeView::MinimumSide + 24);
    enroll->addWidget(qrArea, 1);
    auto *form = new QFormLayout;
    form->setSpacing(8);
    code = new QLineEdit;
    code->setObjectName("totpCode");
    code->setMaxLength(6);
    code->setValidator(new QRegularExpressionValidator(QRegularExpression("\\d{0,6}"), code));
    code->setInputMethodHints(Qt::ImhDigitsOnly);
    form->addRow("Código do Authenticator", code);
    enroll->addLayout(form);
    layout->addWidget(enrollPage, 1);

    codesPage = new QWidget;
    codesPage->hide();
    auto *list = new QVBoxLayout(codesPage);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(12);
    list->addWidget(textLabel(CodesHelp, "helpText"));
    codes = new QPlainTextEdit;
    codes->setObjectName("backupCodes");
    codes->setReadOnly(true);
    codes->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    codes->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    codes->setMinimumHeight(220);
    list->addWidget(codes, 1);
    auto *copy = new QPushButton("Copiar códigos");
    copy->setObjectName("copyCodes");
    auto *copyRow = new QHBoxLayout;
    copyRow->addWidget(copy);
    copyRow->addStretch();
    list->addLayout(copyRow);
    saved = new QCheckBox("Guardei os códigos em local seguro");
    saved->setObjectName("codesSaved");
    list->addWidget(saved);
    layout->addWidget(codesPage, 1);

    error = textLabel({}, "errorText");
    error->hide();
    layout->addWidget(error);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    auto *cancel = new QPushButton("Cancelar");
    submit = new QPushButton("Confirmar vínculo");
    submit->setObjectName("primary");
    submit->setDefault(true);
    finish = new QPushButton("Concluir configuração");
    finish->setObjectName("primary");
    finish->setEnabled(false);
    finish->hide();
    actions->addWidget(cancel);
    actions->addWidget(submit);
    actions->addWidget(finish);
    layout->addLayout(actions);
    connect(cancel, &QPushButton::clicked, this, &EnrollmentDialog::reject);
    connect(submit, &QPushButton::clicked, this, &EnrollmentDialog::attempt);
    connect(copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(codes->toPlainText());
    });
    connect(saved, &QCheckBox::toggled, finish, &QPushButton::setEnabled);
    connect(finish, &QPushButton::clicked, this, [this] {
        clearCodes();
        accept();
    });
    try {
        draft = recovery.beginEnrollment(session);
        QByteArray uri = draft->uri().toUtf8();
        qr->setContent(uri);
        uri.fill('\0');
    } catch (const std::exception &e) {
        expired = isExpiry(e);
        discard();
        unavailable(dynamic_cast<const AuthorizationError *>(&e) ? QString::fromUtf8(e.what()) : Unavailable);
        return;
    }
    code->setFocus();
}

EnrollmentDialog::~EnrollmentDialog() {
    discard();
    clearCodes();
}

void EnrollmentDialog::unavailable(const QString &message) {
    qrArea->hide();
    qr->clear();
    error->setText(message);
    error->show();
    code->setEnabled(false);
    submit->setEnabled(false);
}

void EnrollmentDialog::discard() {
    qr->clear();
    if (draft) {
        recovery.cancelEnrollment(*draft);
        draft.reset();
    }
}

void EnrollmentDialog::clearCodes() {
    codes->setPlainText(QString(codes->toPlainText().size(), QChar(' ')));
    codes->clear();
}

void EnrollmentDialog::reject() {
    if (busy)
        return;
    if (showingCodes) {
        QMessageBox confirm(QMessageBox::Warning, "GestaoEquipamentos", CloseCodes, QMessageBox::NoButton, this);
        confirm.setTextFormat(Qt::PlainText);
        auto *back = confirm.addButton("Voltar aos códigos", QMessageBox::RejectRole);
        auto *close = confirm.addButton("Fechar mesmo assim", QMessageBox::AcceptRole);
        confirm.setDefaultButton(back);
        confirm.setEscapeButton(back);
        confirm.exec();
        if (confirm.clickedButton() != close)
            return;
        clearCodes();
        // O vínculo já está ativo; só a etapa dos códigos foi abandonada.
        QDialog::done(QDialog::Accepted);
        return;
    }
    discard();
    QDialog::reject();
}

void EnrollmentDialog::showCodes(const QStringList &list) {
    showingCodes = true;
    draft.reset();
    qr->clear();
    wipeField(code);
    title->setText("Códigos de recuperação");
    enrollPage->hide();
    codesPage->show();
    error->hide();
    codes->setPlainText(list.join('\n'));
    submit->hide();
    finish->show();
    finish->setDefault(true);
    saved->setFocus();
}

void EnrollmentDialog::attempt() {
    if (busy || !draft)
        return;
    busy = true;
    submit->setEnabled(false);
    submit->setText("Verificando código…");
    code->setEnabled(false);
    submit->repaint();
    QString refused;
    QStringList issued;
    bool closed = false;
    try {
        issued = recovery.confirmEnrollment(session, *draft, code->text());
    } catch (const AuthError &e) {
        refused = QString::fromUtf8(e.what());
        closed = e.code() == "recovery_changed";
    } catch (const AuthorizationError &e) {
        refused = QString::fromUtf8(e.what());
        expired = isExpiry(e);
        closed = true;
    } catch (const std::exception &) {
        refused = Unavailable;
    }
    busy = false;
    submit->setText("Confirmar vínculo");
    if (refused.isEmpty()) {
        showCodes(issued);
        return;
    }
    if (closed) {
        discard();
        unavailable(refused);
        if (expired)
            reject();
        return;
    }
    submit->setEnabled(true);
    code->setEnabled(true);
    error->setText(refused);
    error->show();
    code->selectAll();
    code->setFocus();
}

RecoveryDialog::RecoveryDialog(AuthService &auth, RecoveryService &recovery, QWidget *parent)
    : QDialog(parent), auth(auth), recovery(recovery) {
    setWindowTitle("GestaoEquipamentos");
    setMinimumWidth(480);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->addWidget(textLabel("Recuperar conta principal", "dialogTitle"));
    auto *form = new QFormLayout;
    form->setSpacing(8);
    username = new QLineEdit;
    username->setObjectName("recoveryUsername");
    username->setMaxLength(64);
    form->addRow("Nome de usuário", username);
    authenticator = new QRadioButton("Código do Authenticator");
    authenticator->setObjectName("methodAuthenticator");
    authenticator->setChecked(true);
    backup = new QRadioButton("Código de recuperação");
    backup->setObjectName("methodBackup");
    auto *methods = new QVBoxLayout;
    methods->setSpacing(4);
    methods->addWidget(authenticator);
    methods->addWidget(backup);
    form->addRow("Método", methods);
    code = new QLineEdit;
    code->setObjectName("recoveryCode");
    form->addRow("Código", code);
    password = new QLineEdit;
    password->setObjectName("newPassword");
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(128);
    confirm = new QLineEdit;
    confirm->setObjectName("confirmPassword");
    confirm->setEchoMode(QLineEdit::Password);
    confirm->setMaxLength(128);
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
    cancel = new QPushButton("Cancelar");
    submit = new QPushButton("Redefinir senha");
    submit->setObjectName("primary");
    submit->setDefault(true);
    actions->addWidget(cancel);
    actions->addWidget(submit);
    layout->addLayout(actions);
    connect(cancel, &QPushButton::clicked, this, &RecoveryDialog::reject);
    connect(submit, &QPushButton::clicked, this, &RecoveryDialog::attempt);
    connect(authenticator, &QRadioButton::toggled, this, &RecoveryDialog::methodChanged);
    methodChanged();
    username->setFocus();
}

void RecoveryDialog::methodChanged() {
    wipeField(code);
    if (authenticator->isChecked()) {
        code->setEchoMode(QLineEdit::Normal);
        code->setMaxLength(6);
        code->setValidator(new QRegularExpressionValidator(QRegularExpression("\\d{0,6}"), code));
        code->setInputMethodHints(Qt::ImhDigitsOnly);
    } else {
        code->setValidator(nullptr);
        code->setMaxLength(24);
        code->setEchoMode(QLineEdit::Password);
        code->setInputMethodHints(Qt::ImhNone);
    }
}

void RecoveryDialog::reject() {
    if (busy)
        return;
    wipeField(code);
    wipeField(password);
    wipeField(confirm);
    QDialog::reject();
}

void RecoveryDialog::setBusy(bool value) {
    busy = value;
    submit->setEnabled(!value);
    submit->setText(value ? "Verificando código…" : "Redefinir senha");
    for (QWidget *w :
         std::initializer_list<QWidget *>{username, authenticator, backup, code, password, confirm, cancel})
        w->setEnabled(!value);
}

void RecoveryDialog::refuse(const QString &message, QLineEdit *focus) {
    wipeField(code);
    wipeField(password);
    wipeField(confirm);
    error->setText(message);
    error->show();
    focus->setFocus();
}

void RecoveryDialog::attempt() {
    if (busy)
        return;
    if (password->text() != confirm->text()) {
        wipeField(password);
        wipeField(confirm);
        error->setText(Mismatch);
        error->show();
        password->setFocus();
        return;
    }
    setBusy(true);
    auto prepared = std::make_shared<std::optional<PreparedPassword>>();
    auto policy = std::make_shared<QString>();
    QByteArray secret = password->text().toUtf8();
    AuthService *service = &auth;
    auto *worker = QThread::create([service, prepared, policy, secret]() mutable {
        try {
            prepared->emplace(service->preparePassword(secret));
        } catch (const std::exception &e) {
            *policy = QString::fromUtf8(e.what());
        }
        secret.fill('\0');
    });
    secret.fill('\0');
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, prepared, policy] {
        setBusy(false);
        if (!prepared->has_value()) {
            wipeField(password);
            wipeField(confirm);
            error->setText(policy->isEmpty() ? RecoveryFailure : *policy);
            error->show();
            password->setFocus();
            return;
        }
        const RecoveryMethod method =
            authenticator->isChecked() ? RecoveryMethod::Authenticator : RecoveryMethod::BackupCode;
        try {
            recovery.recoverPrincipal(username->text(), method, code->text(), **prepared);
        } catch (const AuthError &e) {
            refuse(QString::fromUtf8(e.what()), code);
            return;
        } catch (const std::exception &) {
            refuse(RecoveryFailure, code);
            return;
        }
        wipeField(code);
        wipeField(password);
        wipeField(confirm);
        accept();
    });
    worker->start();
}

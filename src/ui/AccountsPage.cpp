#include "AccountsPage.h"
#include "AccountDialogs.h"
#include "RecoveryDialogs.h"
#include <QtWidgets>
#include <exception>

namespace {
const QString LoadFailure = QStringLiteral("Não foi possível carregar as contas. Selecione Tentar novamente.");
const QString SaveFailure = QStringLiteral("Não foi possível salvar a conta. Confira os dados e tente novamente.");
QLabel *textLabel(const QString &text, const char *name) {
    auto *label = new QLabel(text); label->setObjectName(name);
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    return label;
}
// Regras e permissões explicam a recusa; falhas de armazenamento nunca mostram SQL.
QString refusalText(const std::exception &e, const QString &fallback) {
    if (dynamic_cast<const AuthError *>(&e) || dynamic_cast<const AuthorizationError *>(&e)) return QString::fromUtf8(e.what());
    return fallback;
}
QTableWidget *accountTable(const char *name, int minimumHeight) {
    auto *table = new QTableWidget(0, 3); table->setObjectName(name);
    table->setHorizontalHeaderLabels({"Nome de usuário", "Perfil", "Situação"});
    table->verticalHeader()->hide(); table->verticalHeader()->setDefaultSectionSize(58);
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setShowGrid(false); table->setAlternatingRowColors(true);
    table->setTextElideMode(Qt::ElideRight); table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive); table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 360); table->setColumnWidth(1, 180); table->setMinimumHeight(minimumHeight);
    return table;
}
void fillAccounts(QTableWidget *table, const QVector<AccountSummary> &accounts) {
    table->setRowCount(0);
    for (const auto &account : accounts) {
        const int r = table->rowCount(); table->insertRow(r);
        const QStringList cells{account.username, accountRoleLabel(account.role), account.active ? QStringLiteral("Ativa") : QStringLiteral("Inativa")};
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells[c]); item->setData(Qt::UserRole, account.id);
            if (c == 0) item->setToolTip(account.username);
            table->setItem(r, c, item);
        }
    }
}
const AccountSummary *selectedIn(const QTableWidget *table, const QVector<AccountSummary> &accounts) {
    const int row = table->currentRow();
    if (row < 0 || !table->item(row, 0) || !table->item(row, 0)->isSelected()) return nullptr;
    const auto id = table->item(row, 0)->data(Qt::UserRole).toLongLong();
    for (const auto &account : accounts) if (account.id == id) return &account;
    return nullptr;
}
const QString AdminLimit = QStringLiteral("Limite de três administradores ativos atingido. Inative outro administrador para liberar uma vaga.");
}

AccountsPage::AccountsPage(AccountService &accounts, AuthService &auth, RecoveryService &recovery, const Session &session, QWidget *parent)
    : QWidget(parent), accounts(accounts), auth(auth), recovery(recovery), session(session) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(14);
    auto *heading = new QHBoxLayout; heading->addWidget(textLabel("Contas", "sectionTitle")); heading->addStretch();
    auto *create = new QPushButton("Criar conta comum"); create->setObjectName("primary"); heading->addWidget(create);
    layout->addLayout(heading);
    auto *toolbar = new QHBoxLayout;
    search = new QLineEdit; search->setObjectName("accountSearch"); search->setPlaceholderText("Buscar por nome de usuário");
    search->setClearButtonEnabled(true); search->setMinimumWidth(300);
    filter = new QComboBox; filter->setObjectName("accountFilter"); filter->addItems({"Ativas", "Inativas", "Todas"});
    toolbar->addWidget(search); toolbar->addWidget(filter); toolbar->addStretch(); layout->addLayout(toolbar);
    status = textLabel({}, "muted"); status->hide(); layout->addWidget(status);
    states = new QStackedWidget;
    table = accountTable("accounts", 190);
    states->addWidget(table);
    auto *empty = new QWidget; auto *emptyLayout = new QVBoxLayout(empty); emptyLayout->setContentsMargins(0,24,0,24); emptyLayout->setSpacing(8);
    emptyTitle = textLabel({}, "dialogTitle"); emptyBody = textLabel({}, "helpText");
    emptyLayout->addWidget(emptyTitle); emptyLayout->addWidget(emptyBody); emptyLayout->addStretch();
    states->addWidget(empty);
    auto *failed = new QWidget; auto *failedLayout = new QVBoxLayout(failed); failedLayout->setContentsMargins(0,24,0,24); failedLayout->setSpacing(8);
    loadError = textLabel(LoadFailure, "errorText"); failedLayout->addWidget(loadError);
    auto *retryRow = new QHBoxLayout; auto *retry = new QPushButton("Tentar novamente"); retryRow->addWidget(retry); retryRow->addStretch();
    failedLayout->addLayout(retryRow); failedLayout->addStretch();
    states->addWidget(failed);
    layout->addWidget(states, 1);
    auto *actions = new QHBoxLayout; actions->addStretch();
    resetButton = new QPushButton("Redefinir senha");
    inactivateButton = new QPushButton("Inativar conta"); inactivateButton->setObjectName("destructive");
    actions->addWidget(resetButton); actions->addWidget(inactivateButton); layout->addLayout(actions);
    if (session.role() == AccountRole::Principal) { buildRecovery(layout); buildAdministrators(layout); }
    feedback = textLabel({}, "feedback"); layout->addWidget(feedback);
    connect(create, &QPushButton::clicked, this, &AccountsPage::createAccount);
    connect(retry, &QPushButton::clicked, this, &AccountsPage::reload);
    connect(search, &QLineEdit::textChanged, this, [this] { reload(); });
    connect(filter, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    connect(table, &QTableWidget::itemSelectionChanged, this, &AccountsPage::updateActions);
    connect(resetButton, &QPushButton::clicked, this, &AccountsPage::resetPassword);
    connect(inactivateButton, &QPushButton::clicked, this, &AccountsPage::inactivate);
    reload();
}

void AccountsPage::buildRecovery(QVBoxLayout *layout) {
    auto *section = new QWidget; section->setObjectName("recoverySection");
    auto *column = new QVBoxLayout(section); column->setContentsMargins(0,10,0,0); column->setSpacing(6);
    auto *row = new QHBoxLayout; row->setSpacing(12);
    row->addWidget(textLabel("Recuperação", "sectionTitle"));
    recoveryStatus = textLabel({}, "recoveryStatus"); row->addWidget(recoveryStatus, 1);
    rebindNotice = textLabel(QStringLiteral("O vínculo do Authenticator foi removido porque um código de recuperação foi usado. Vincule novamente para gerar novos códigos."),
                             "errorText");
    rebindNotice->hide(); row->addWidget(rebindNotice, 1);
    enrollButton = new QPushButton("Vincular Google Authenticator"); enrollButton->setObjectName("enrollButton");
    row->addWidget(enrollButton);
    column->addLayout(row);
    remainingCodes = textLabel({}, "remainingCodes"); remainingCodes->hide(); column->addWidget(remainingCodes);
    fewCodes = textLabel(QStringLiteral("Restam poucos códigos. Vincule o Authenticator novamente para gerar um novo conjunto."), "helpText");
    fewCodes->hide(); column->addWidget(fewCodes);
    layout->addWidget(section);
    connect(enrollButton, &QPushButton::clicked, this, &AccountsPage::enroll);
}

void AccountsPage::reloadRecovery() {
    if (!recoveryStatus) return;
    try {
        const auto current = recovery.status(session);
        const int remaining = current.generation > 0 ? recovery.remainingBackupCodes(session) : 0;
        recoveryStatus->setText(current.configured
            ? "Recuperação configurada em " + current.enrolledAtUtc.toLocalTime().toString("dd/MM/yyyy HH:mm")
            : QStringLiteral("Recuperação não configurada"));
        recoveryStatus->setVisible(!current.needsRebinding); rebindNotice->setVisible(current.needsRebinding);
        remainingCodes->setText("Códigos de recuperação restantes: " + QString::number(remaining));
        remainingCodes->setVisible(current.generation > 0);
        fewCodes->setVisible(current.generation > 0 && remaining < 4);
        enrollButton->setEnabled(true);
    } catch (const std::exception &e) {
        recoveryStatus->setText(refusalText(e, QStringLiteral("Não foi possível consultar a recuperação.")));
        recoveryStatus->show(); rebindNotice->hide(); remainingCodes->hide(); fewCodes->hide();
        enrollButton->setEnabled(false);
    }
}

void AccountsPage::enroll() {
    EnrollmentDialog dialog(recovery, session, this);
    if (!dialog.sessionExpired()) {
        const bool linked = dialog.exec() == QDialog::Accepted;
        if (!dialog.sessionExpired()) {
            if (linked) succeed("Google Authenticator vinculado.");
            else reloadRecovery();
            return;
        }
    }
    if (onExpired) onExpired();
}

void AccountsPage::buildAdministrators(QVBoxLayout *layout) {
    auto *section = new QWidget; section->setObjectName("adminSection");
    auto *sectionLayout = new QVBoxLayout(section); sectionLayout->setContentsMargins(0,10,0,0); sectionLayout->setSpacing(10);
    auto *heading = new QHBoxLayout; heading->setSpacing(12);
    heading->addWidget(textLabel("Administradores", "sectionTitle"));
    adminCount = textLabel({}, "adminCount"); heading->addWidget(adminCount); heading->addStretch();
    createAdminButton = new QPushButton("Criar administrador"); createAdminButton->setObjectName("primary"); heading->addWidget(createAdminButton);
    sectionLayout->addLayout(heading);
    adminLimit = textLabel(AdminLimit, "adminLimit"); adminLimit->hide(); sectionLayout->addWidget(adminLimit);
    adminTable = accountTable("administrators", 150);
    sectionLayout->addWidget(adminTable, 1);
    auto *actions = new QHBoxLayout; actions->addStretch();
    adminResetButton = new QPushButton("Redefinir senha");
    adminInactivateButton = new QPushButton("Inativar conta"); adminInactivateButton->setObjectName("destructive");
    actions->addWidget(adminResetButton); actions->addWidget(adminInactivateButton); sectionLayout->addLayout(actions);
    layout->addWidget(section, 1);
    connect(createAdminButton, &QPushButton::clicked, this, &AccountsPage::createAdministrator);
    connect(adminTable, &QTableWidget::itemSelectionChanged, this, &AccountsPage::updateAdministratorActions);
    connect(adminResetButton, &QPushButton::clicked, this, &AccountsPage::resetAdministratorPassword);
    connect(adminInactivateButton, &QPushButton::clicked, this, &AccountsPage::inactivateAdministrator);
}

void AccountsPage::reloadAdministrators() {
    if (!adminTable) return;
    admins.clear();
    try { admins = accounts.administrators(session); }
    catch (const std::exception &e) {
        adminTable->setRowCount(0);
        adminCount->setText(refusalText(e, LoadFailure));
        createAdminButton->setEnabled(false); adminLimit->hide(); updateAdministratorActions();
        return;
    }
    fillAccounts(adminTable, admins);
    int active = 0;
    for (const auto &account : admins) active += account.active ? 1 : 0;
    adminCount->setText(QString::number(active) + " de 3 ativos");
    const bool full = active >= 3;
    createAdminButton->setEnabled(!full); createAdminButton->setToolTip(full ? AdminLimit : QString());
    adminLimit->setVisible(full);
    updateAdministratorActions();
}

const AccountSummary *AccountsPage::selectedAdministrator() const {
    return adminTable ? selectedIn(adminTable, admins) : nullptr;
}

// A linha da principal nunca habilita ações: ela não é inativada, transferida nem redefinida por aqui.
void AccountsPage::updateAdministratorActions() {
    if (!adminTable) return;
    const auto *account = selectedAdministrator();
    const bool manageable = account && account->role == AccountRole::Administrator && account->active;
    adminResetButton->setEnabled(manageable);
    adminInactivateButton->setEnabled(manageable);
}

void AccountsPage::createAdministrator() {
    CreateAccountDialog create(auth, accounts, session, this, AccountRole::Administrator);
    if (create.exec() != QDialog::Accepted) { reloadAdministrators(); return; }
    if (!create.assistedUsername().isEmpty()) {
        PasswordSetDialog step(PasswordSetDialog::Mode::Assisted, auth, accounts, session, create.assistedUsername(), 0, this, AccountRole::Administrator);
        if (step.exec() != QDialog::Accepted) { reloadAdministrators(); return; }
    }
    succeed("Conta salva.");
}

void AccountsPage::resetAdministratorPassword() {
    const auto *account = selectedAdministrator(); if (!account) return;
    PasswordSetDialog reset(PasswordSetDialog::Mode::Reset, auth, accounts, session, account->username, account->id, this, AccountRole::Administrator);
    if (reset.exec() == QDialog::Accepted) succeed("Senha definida.");
}

void AccountsPage::inactivateAdministrator() {
    const auto *account = selectedAdministrator(); if (!account) return;
    const qint64 id = account->id;
    if (!confirmInactivation("Inativar conta " + account->username + "? Ela não poderá entrar e a vaga de administrador será liberada. "
                             "O histórico de operações será preservado.")) return;
    try { accounts.inactivateAdministrator(session, id); }
    catch (const std::exception &e) {
        if (reportExpired(e)) return;
        QMessageBox::warning(this, "Inativar conta", refusalText(e, SaveFailure)); reload(); return;
    }
    succeed("Conta inativada.");
}

bool AccountsPage::reportExpired(const std::exception &e) {
    const auto *refused = dynamic_cast<const AuthorizationError *>(&e);
    if (!refused || refused->reason() != AuthorizationError::Reason::Expired || !onExpired) return false;
    onExpired();
    return true;
}

bool AccountsPage::confirmInactivation(const QString &question) {
    QMessageBox confirm(QMessageBox::Warning, "Inativar conta", question, QMessageBox::NoButton, this);
    confirm.setTextFormat(Qt::PlainText);
    auto *cancel = confirm.addButton("Cancelar", QMessageBox::RejectRole);
    auto *proceed = confirm.addButton("Inativar conta", QMessageBox::DestructiveRole); proceed->setObjectName("destructive");
    confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel);
    confirm.exec();
    return confirm.clickedButton() == proceed;
}

void AccountsPage::reload() {
    reloadRecovery();
    reloadAdministrators();
    status->setText("Carregando contas…"); status->show();
    table->setRowCount(0); rows.clear();
    const auto filterValue = filter->currentIndex() == 1 ? AccountFilter::Inactive : filter->currentIndex() == 2 ? AccountFilter::All : AccountFilter::Active;
    bool none = false;
    try {
        rows = accounts.commonAccounts(session, filterValue, search->text());
        if (rows.isEmpty()) none = accounts.commonAccounts(session, AccountFilter::All, {}).isEmpty();
    } catch (const std::exception &e) {
        status->hide();
        loadError->setText(refusalText(e, LoadFailure));
        states->setCurrentIndex(2); updateActions();
        reportExpired(e);
        return;
    }
    status->hide();
    fillAccounts(table, rows);
    if (!rows.isEmpty()) states->setCurrentIndex(0);
    else {
        emptyTitle->setText(none ? "Nenhuma conta comum cadastrada" : "Nenhuma conta encontrada. Revise a busca ou limpe o filtro.");
        emptyBody->setText(none ? "Selecione Criar conta comum para adicionar um operador." : QString());
        emptyBody->setVisible(none);
        states->setCurrentIndex(1);
    }
    updateActions();
}

const AccountSummary *AccountsPage::selected() const {
    if (states->currentIndex() != 0) return nullptr;
    return selectedIn(table, rows);
}

void AccountsPage::updateActions() {
    const auto *account = selected();
    resetButton->setEnabled(account && account->active);
    inactivateButton->setEnabled(account && account->active);
}

void AccountsPage::succeed(const QString &message) {
    reload();
    feedback->setText("✓  " + message);
}

void AccountsPage::createAccount() {
    CreateAccountDialog create(auth, accounts, session, this);
    if (create.exec() != QDialog::Accepted) return;
    if (!create.assistedUsername().isEmpty()) {
        PasswordSetDialog step(PasswordSetDialog::Mode::Assisted, auth, accounts, session, create.assistedUsername(), 0, this);
        if (step.exec() != QDialog::Accepted) return;
    }
    succeed("Conta salva.");
}

void AccountsPage::resetPassword() {
    const auto *account = selected(); if (!account) return;
    PasswordSetDialog reset(PasswordSetDialog::Mode::Reset, auth, accounts, session, account->username, account->id, this);
    if (reset.exec() == QDialog::Accepted) succeed("Senha definida.");
}

void AccountsPage::inactivate() {
    const auto *account = selected(); if (!account) return;
    const qint64 id = account->id;
    if (!confirmInactivation("Inativar conta " + account->username + "? Ela não poderá entrar. O histórico de operações será preservado.")) return;
    try { accounts.inactivateCommonAccount(session, id); }
    catch (const std::exception &e) {
        if (reportExpired(e)) return;
        QMessageBox::warning(this, "Inativar conta", refusalText(e, SaveFailure)); reload(); return;
    }
    succeed("Conta inativada.");
}

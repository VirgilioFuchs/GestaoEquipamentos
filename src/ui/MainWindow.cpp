#include "MainWindow.h"
#include "AccessDialogs.h"
#include "AccountsPage.h"
#include "LocationsPage.h"
#include "PeoplePage.h"
#include "LocationDialog.h"
#include "PersonDialog.h"
#include "QuickPickField.h"
#include "application/LocationService.h"
#include "application/PeopleService.h"
#include "HistoryFormat.h"
#include "LogsPage.h"
#include <QtWidgets>
#include <array>
#include <exception>
#include <tuple>

namespace {
QString displayTime(const QDateTime &date) {
    return date.isValid() ? date.toLocalTime().toString("dd/MM/yyyy HH:mm:ss") : QStringLiteral("—");
}
QString displayPerson(const QString &name, ResponsibleRole role) {
    return name.isEmpty() ? QStringLiteral("Não informado") : name + " (" + roleLabel(role) + ")";
}
// D-03: só locais ativos; placeholder com id -1. Sem locais ativos o combo fica desabilitado com orientação.
QComboBox *activeLocationCombo(LocationService &service, std::optional<qint64> preferred = std::nullopt) {
    auto *combo = new QComboBox; combo->setObjectName("movementLocation");
    const auto rows = service.locations(CatalogFilter::Active);
    if (rows.isEmpty()) { combo->addItem("Cadastre um local em Locais.", qint64(-1)); combo->setEnabled(false); return combo; }
    combo->addItem("Selecione o local", qint64(-1));
    for (const auto &place : rows) combo->addItem(place.name, place.id);
    if (preferred) { const int index = combo->findData(*preferred); if (index > 0) combo->setCurrentIndex(index); }
    return combo;
}
QLabel *label(const QString &text, const char *name) {
    auto *widget = new QLabel(text); widget->setObjectName(name); return widget;
}
QTableWidget *table(const QStringList &headers) {
    auto *t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->hide(); t->verticalHeader()->setDefaultSectionSize(58);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setShowGrid(false); t->setAlternatingRowColors(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    t->setMinimumHeight(190);
    return t;
}
QFrame *card(const QString &caption, QLabel *&value, const QString &color) {
    auto *frame = new QFrame; frame->setObjectName("metric");
    auto *layout = new QVBoxLayout(frame); layout->setContentsMargins(22,18,22,18);
    layout->addWidget(label(caption,"muted")); value = label("0","metricValue");
    value->setStyleSheet("color:" + color + ";"); layout->addWidget(value); return frame;
}
}
MainWindow::MainWindow(NotebookStore &store, AuthService &auth, AccountService &accounts, RecoveryService &recovery, Session session, const QString &path, LocationService &locations, PeopleService &people)
    : store(store), historyService(store.localDatabase()), auth(auth), accounts(accounts), recovery(recovery), locations(locations), people(people), session(std::move(session)) {
    setWindowTitle("GestaoEquipamentos · Controle de notebooks"); resize(1440,900); setMinimumSize(980,640);
    auto *root = new QWidget; setCentralWidget(root);
    auto *shell = new QHBoxLayout(root); shell->setContentsMargins(0,0,0,0); shell->setSpacing(0);
    auto *sidebar = new QFrame; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(240);
    auto *nav = new QVBoxLayout(sidebar); nav->setContentsMargins(22,32,22,24); nav->setSpacing(12);
    nav->addWidget(label("▣  GestaoEquipamentos","brand")); nav->addWidget(label("GESTÃO DE EQUIPAMENTOS","navCaption")); nav->addSpacing(32);
    auto *inventoryNav = new QPushButton("  Equipamentos"); inventoryNav->setObjectName("navActive"); inventoryNav->setProperty("pageKey","equipamentos");
    auto *historyNav = new QPushButton("  Logs"); historyNav->setObjectName("navButton"); historyNav->setProperty("pageKey","logs");
    auto *locationsNav = new QPushButton("  Locais"); locationsNav->setObjectName("navButton");
    locationsNav->setProperty("pageKey","locais");
    auto *peopleNav = new QPushButton("  Pessoas"); peopleNav->setObjectName("navButton"); peopleNav->setProperty("pageKey","pessoas");
    nav->addWidget(peopleNav); nav->addWidget(inventoryNav); nav->addWidget(locationsNav); nav->addWidget(historyNav);
    navButtons = {peopleNav, inventoryNav, locationsNav, historyNav};
    const bool administrator = this->session.role() != AccountRole::Common;
    if (administrator) { auto *accountsNav = new QPushButton("  Contas"); accountsNav->setObjectName("navButton"); accountsNav->setProperty("pageKey","contas"); nav->addWidget(accountsNav); navButtons << accountsNav; }
    nav->addStretch();
    auto *identity = label("Conta operadora: " + this->session.username() + " · " + accountRoleLabel(this->session.role()),"sessionIdentity");
    identity->setTextFormat(Qt::PlainText); identity->setWordWrap(true); nav->addWidget(identity);
    sessionDeadline = label({},"sessionDeadline"); sessionDeadline->setWordWrap(true); nav->addWidget(sessionDeadline);
    showDeadline();
    auto *logout = new QPushButton("  Sair"); logout->setObjectName("navButton"); nav->addWidget(logout); nav->addSpacing(12);
    connect(logout,&QPushButton::clicked,this,&MainWindow::logoutRequested);
    nav->addWidget(label("●  Armazenamento local","local")); nav->addWidget(label("VERSÃO 0.1  /  DESKTOP","navCaption"));
    shell->addWidget(sidebar);
    content = new QWidget; auto *body = new QVBoxLayout(content); body->setContentsMargins(30,28,30,20); body->setSpacing(20); shell->addWidget(content,1);
    pages = new QStackedWidget; body->addWidget(pages,1);
    auto *inventoryPage = new QWidget; auto *layout = new QVBoxLayout(inventoryPage); layout->setContentsMargins(0,0,0,0); layout->setSpacing(14);
    // D-08: cabeçalho operacional, sem texto promocional; cadastro (Cadastrar/Editar/Inativar) fora das guias.
    auto *heading = new QHBoxLayout;
    auto *pageTitle = label("Equipamentos","sectionTitle"); pageTitle->setObjectName("pageTitle"); pageTitle->setTextFormat(Qt::PlainText);
    heading->addWidget(pageTitle); heading->addStretch();
    editButton = new QPushButton("Editar notebook"); editButton->setObjectName("notebookEdit"); heading->addWidget(editButton);
    if (administrator) {
        inactivateButton = new QPushButton("Inativar notebook"); inactivateButton->setObjectName("destructive"); heading->addWidget(inactivateButton);
        // Reativar ocupa o mesmo lugar: só um dos dois fica visível conforme o notebook selecionado.
        reactivateButton = new QPushButton("Reativar notebook"); reactivateButton->setObjectName("notebookReactivate"); reactivateButton->hide(); heading->addWidget(reactivateButton);
    }
    auto *add = new QPushButton("+  Cadastrar notebook"); add->setObjectName("primary"); heading->addWidget(add); layout->addLayout(heading);
    inactivateReason = label("Somente notebooks disponíveis podem ser inativados.","inactivateReason"); inactivateReason->setTextFormat(Qt::PlainText); inactivateReason->hide();
    layout->addWidget(inactivateReason,0,Qt::AlignRight);
    auto *metrics = new QHBoxLayout; metrics->setSpacing(14);
    metrics->addWidget(card("Total de notebooks", total,"#15263c")); metrics->addWidget(card("Disponíveis",available,"#087f66"));
    metrics->addWidget(card("Emprestados",loaned,"#355cdf")); metrics->addWidget(card("Em manutenção",maintenance,"#b87915")); layout->addLayout(metrics);
    auto *toolbar = new QHBoxLayout;
    search = new QLineEdit; search->setObjectName("inventorySearch"); search->setPlaceholderText("Buscar patrimônio, notebook, local ou pessoa"); search->setClearButtonEnabled(true); search->setMinimumWidth(320);
    activityFilter = new QComboBox; activityFilter->setObjectName("activityFilter"); activityFilter->setAccessibleName("Situação dos notebooks"); activityFilter->addItems({"Ativos","Inativos","Todos"});
    auto *reload = new QPushButton("Atualizar"); toolbar->addWidget(search,1); toolbar->addWidget(activityFilter); toolbar->addWidget(reload); layout->addLayout(toolbar);
    // 03-PD-04: uma guia por status persistido, cada uma com sua tabela, vazio e barra de ações da matriz (D-10).
    inventoryTabs = new QTabWidget; inventoryTabs->setObjectName("inventoryTabs"); inventoryTabs->setDocumentMode(false);
    auto tab = [&](const QString &title, QTableWidget *&grid, const char *gridName, const QStringList &headers, QLabel *&empty, const char *emptyName, const QVector<QPushButton *> &buttons) {
        auto *page = new QWidget; auto *box = new QVBoxLayout(page); box->setContentsMargins(12,12,12,12); box->setSpacing(10);
        grid = table(headers); grid->setObjectName(gridName); box->addWidget(grid,1);
        empty = label({},emptyName); empty->setTextFormat(Qt::PlainText); empty->setWordWrap(true); box->addWidget(empty);
        auto *bar = new QHBoxLayout; bar->addStretch(); for (auto *b : buttons) bar->addWidget(b); box->addLayout(bar);
        inventoryTabs->addTab(page,title);
    };
    auto makeButton = [](const QString &text, const char *name, bool primary = false) {
        // objectName identifica o botão nos testes; o destaque primário vem da propriedade primaryAction no QSS.
        auto *b = new QPushButton(text); b->setObjectName(name); b->setProperty("primaryAction",primary); return b;
    };
    availableLocationButton = makeButton("Atualizar local","availableLocationButton"); sendMaintenanceButton = makeButton("Enviar à manutenção","sendMaintenanceButton");
    checkoutButton = makeButton("Registrar retirada","checkoutButton",true);
    loanedLocationButton = makeButton("Atualizar local","loanedLocationButton"); returnButton = makeButton("Registrar devolução","checkinButton",true);
    finishMaintenanceButton = makeButton("Concluir manutenção","finishMaintenanceButton",true);
    tab("Disponíveis",availableTable,"availableTable",{"Patrimônio","Notebook","Local atual"},availableEmpty,"availableEmpty",{availableLocationButton,sendMaintenanceButton,checkoutButton});
    tab("Indisponíveis (emprestados)",loanedTable,"loanedTable",{"Patrimônio","Notebook","Retirado por","Matéria","Local de uso","Retirada em"},loanedEmpty,"loanedEmpty",{loanedLocationButton,returnButton});
    tab("Manutenção",maintenanceTable,"maintenanceTable",{"Patrimônio","Notebook","Local atual","Enviado em","Observação"},maintenanceEmpty,"maintenanceEmpty",{finishMaintenanceButton});
    layout->addWidget(inventoryTabs,1);
    emptyTitle = label("Nenhum notebook cadastrado","sectionTitle"); emptyTitle->setObjectName("inventoryEmptyTitle"); emptyTitle->setTextFormat(Qt::PlainText); layout->addWidget(emptyTitle);
    emptyBody = label("Selecione Cadastrar notebook para adicionar o primeiro equipamento.","inventoryEmptyBody"); emptyBody->setTextFormat(Qt::PlainText); emptyBody->setWordWrap(true); layout->addWidget(emptyBody);
    inventorySummary = label("0 notebooks","inventorySummary"); inventorySummary->setTextFormat(Qt::PlainText); layout->addWidget(inventorySummary);
    peoplePage = new PeoplePage(people,this->session); peoplePage->onExpired = [this] { sessionExpired(); };
    peoplePage->onChanged = [this] { refresh(); };
    pages->addWidget(peoplePage);
    pages->addWidget(inventoryPage);
    pages->setCurrentWidget(inventoryPage);
    locationsPage = new LocationsPage(locations,this->session); locationsPage->onExpired = [this] { sessionExpired(); };
    locationsPage->onChanged = [this] { refresh(); };
    pages->addWidget(locationsPage);
    logsPage = new LogsPage(historyService,auth.sessionClock(),people); pages->addWidget(logsPage);
    auto *footer = new QHBoxLayout; footer->setSpacing(20);
    feedback = label("Pronto para começar. Cadastre seu primeiro notebook.","feedback"); feedback->setWordWrap(true); footer->addWidget(feedback,3);
    auto *location = label("Banco local: " + QDir::toNativeSeparators(path),"muted"); location->setTextInteractionFlags(Qt::TextSelectableByMouse); location->setWordWrap(true);
    location->setAlignment(Qt::AlignRight|Qt::AlignVCenter); footer->addWidget(location,2); body->addLayout(footer);
    if (administrator) {
        accountsPage = new AccountsPage(accounts, auth, recovery, this->session); accountsPage->onExpired = [this] { sessionExpired(); };
        pages->addWidget(accountsPage);
    }
    for (auto *button : navButtons)
        connect(button,&QPushButton::clicked,this,[this,button] { showPage(button->property("pageKey").toString()); });
    if (auth.sessionDeadlineUtc(this->session).isValid()) {
        auto *expiry = new QTimer(this); expiry->setInterval(1000);
        connect(expiry,&QTimer::timeout,this,[this] { if (!locked && this->auth.sessionExpired(this->session)) sessionExpired(); });
        expiry->start();
    }
    connect(add,&QPushButton::clicked,this,&MainWindow::addNotebook);
    connect(search,&QLineEdit::textChanged,this,[this]{renderInventory();});
    connect(activityFilter,&QComboBox::currentIndexChanged,this,[this]{renderInventory();});
    connect(editButton,&QPushButton::clicked,this,&MainWindow::editNotebook);
    if (inactivateButton) connect(inactivateButton,&QPushButton::clicked,this,&MainWindow::inactivateNotebook);
    if (reactivateButton) connect(reactivateButton,&QPushButton::clicked,this,&MainWindow::reactivateNotebook);
    connect(reload,&QPushButton::clicked,this,[this]{perform([]{},"Dados atualizados.");});
    for (auto *grid : {availableTable, loanedTable, maintenanceTable}) connect(grid,&QTableWidget::itemSelectionChanged,this,&MainWindow::updateActions);
    connect(inventoryTabs,&QTabWidget::currentChanged,this,[this]{ updateActions(); });
    connect(checkoutButton,&QPushButton::clicked,this,&MainWindow::checkout);
    connect(returnButton,&QPushButton::clicked,this,&MainWindow::checkin);
    connect(availableLocationButton,&QPushButton::clicked,this,&MainWindow::updateLocation);
    connect(loanedLocationButton,&QPushButton::clicked,this,&MainWindow::updateLocation);
    connect(sendMaintenanceButton,&QPushButton::clicked,this,&MainWindow::sendToMaintenance);
    connect(finishMaintenanceButton,&QPushButton::clicked,this,&MainWindow::finishMaintenance);
    refresh();
    if(!data.isEmpty()) feedback->setText("Inventário atualizado. Selecione um notebook para movimentar.");
}
void MainWindow::showPage(const QString &key) {
    if (key=="logs-filtros") { showPage("logs"); logsPage->setPanelExpanded(true); return; }
    for (int index=0; index<navButtons.size(); ++index) {
        if (navButtons[index]->property("pageKey").toString()!=key) continue;
        pages->setCurrentIndex(index);
        for (auto *button : navButtons) {
            button->setObjectName(button==navButtons[index] ? "navActive" : "navButton");
            button->style()->unpolish(button); button->style()->polish(button);
        }
        if (key=="logs" || key=="equipamentos") refresh();
        if (key=="locais") locationsPage->reload();
        if (key=="pessoas") peoplePage->reload();
        return;
    }
}
void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    applyDensity(height() < CompactHeight);
}
// Os valores originais de cada layout ficam guardados nele mesmo; o modo compacto usa a metade.
void MainWindow::applyDensity(bool value) {
    if (densityApplied && value == compact) return;
    densityApplied = true; compact = value;
    for (auto *layout : findChildren<QLayout *>()) {
        if (!layout->parentWidget() || layout->parentWidget()->window() != this) continue;
        if (!layout->property("baseMargins").isValid()) {
            layout->setProperty("baseMargins", QVariant::fromValue(layout->contentsMargins()));
            const auto *grid = qobject_cast<QGridLayout *>(layout);
            layout->setProperty("baseSpacing", QSize(grid ? grid->horizontalSpacing() : layout->spacing(), grid ? grid->verticalSpacing() : layout->spacing()));
        }
        const auto margins = layout->property("baseMargins").value<QMargins>();
        const auto spacing = layout->property("baseSpacing").toSize();
        const auto scaled = [value](int base) { return value && base > 0 ? base / 2 : base; };
        layout->setContentsMargins(value ? margins / 2 : margins);
        if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
            if (spacing.width() >= 0) grid->setHorizontalSpacing(scaled(spacing.width()));
            if (spacing.height() >= 0) grid->setVerticalSpacing(scaled(spacing.height()));
        } else if (spacing.width() >= 0) layout->setSpacing(scaled(spacing.width()));
    }
    for (auto *grid : findChildren<QTableWidget *>()) {
        if (grid->window() != this) continue;
        if (!grid->property("baseMinimumHeight").isValid()) grid->setProperty("baseMinimumHeight", grid->minimumHeight());
        const int minimum = grid->property("baseMinimumHeight").toInt();
        grid->setMinimumHeight(value ? minimum * 7 / 10 : minimum);
        grid->verticalHeader()->setDefaultSectionSize(value ? 40 : 58);
    }
    for (auto *frame : findChildren<QFrame *>("metric")) {
        if (auto *box = qobject_cast<QBoxLayout *>(frame->layout())) box->setDirection(value ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
        if (auto *number = frame->findChild<QLabel *>("metricValue")) number->setAlignment(value ? Qt::AlignRight|Qt::AlignVCenter : Qt::AlignLeft|Qt::AlignVCenter);
    }
    setProperty("compact", value);
    for (auto *widget : findChildren<QWidget *>()) if (widget->window() == this) { widget->style()->unpolish(widget); widget->style()->polish(widget); }
    style()->unpolish(this); style()->polish(this);
}
// Horário local só na apresentação; quem decide a validade é AuthService::authorize. Conta comum não tem prazo.
void MainWindow::showDeadline() {
    const QDateTime deadline = auth.sessionDeadlineUtc(session);
    sessionDeadline->setVisible(deadline.isValid());
    sessionDeadline->setText(deadline.isValid() ? "Sessão administrativa até " + deadline.toLocalTime().toString("HH:mm:ss") : QString());
}
// Sair continua disponível; páginas, navegação e ações ficam bloqueadas até nova autenticação.
void MainWindow::setLocked(bool value) {
    locked = value;
    content->setEnabled(!value);
    for (auto *b : navButtons) b->setEnabled(!value);
}
// Descartar os formulários antes do novo login; nenhuma ação é reenviada.
void MainWindow::sessionExpired() {
    if (reauthenticating) return;
    reauthenticating = true;
    setLocked(true);
    closeOpenDialogs();
    hide();
    feedback->setText("Sessão administrativa expirada. Entre novamente para continuar.");
    LoginDialog login(auth, recovery, LoginDialog::Mode::Reauthentication, this);
    login.setReauthentication(session.username());
    bool entered = false;
    do { entered = login.exec() == QDialog::Accepted; } while (!entered && !login.logoutChosen());
    reauthenticating = false;
    if (login.logoutChosen()) { closeOpenDialogs(); emit logoutRequested(); return; }
    if (!entered) return;
    const Session fresh = login.session();
    if (fresh.accountId() != session.accountId() || fresh.mustChangePassword()) { closeOpenDialogs(); emit sessionReplaced(fresh); return; }
    auth.logout(session);
    session = fresh;
    showDeadline(); setLocked(false);
    show();
    refresh();
    if (accountsPage) accountsPage->reload();
    locationsPage->reload();
    peoplePage->reload();
    feedback->setText("Sessão renovada. Abra a operação novamente e confira os dados.");
}
bool MainWindow::handleExpired(const std::exception &e) {
    const auto *refused = dynamic_cast<const AuthorizationError *>(&e);
    if (!refused || refused->reason() != AuthorizationError::Reason::Expired) return false;
    sessionExpired();
    return true;
}
void MainWindow::closeOpenDialogs() {
    // findChildren retorna pais antes dos descendentes. Guardas toleram destruição em reject().
    QList<QPointer<QDialog>> dialogs;
    for (auto *dialog : findChildren<QDialog *>()) dialogs.append(dialog);
    for (auto it=dialogs.crbegin();it!=dialogs.crend();++it) if (*it && (*it)->isVisible()) (*it)->reject();
}
void MainWindow::refresh() {
    data = store.notebooks(); int countAvailable=0,countLoaned=0,countMaintenance=0;
    int countActive=0;
    // Métricas contam apenas notebooks ativos (02-PD-05), sem relação com a busca.
    for (const auto &n:data) { if (!n.active) continue; ++countActive; countAvailable += n.status==Status::Available; countLoaned += n.status==Status::Loaned; countMaintenance += n.status==Status::Maintenance; }
    total->setText(QString::number(countActive)); available->setText(QString::number(countAvailable)); loaned->setText(QString::number(countLoaned)); maintenance->setText(QString::number(countMaintenance));
    renderInventory(); logsPage->reloadApplied();
}
namespace {
qint64 selectedId(QTableWidget *grid) {
    const int row=grid->currentRow();
    if(row<0 || !grid->item(row,0) || !grid->item(row,0)->isSelected()) return -1;
    return grid->item(row,0)->data(Qt::UserRole).toLongLong();
}
}
void MainWindow::renderInventory() {
    // 03-PD-06: a seleção de cada guia é lembrada por id e restaurada depois de recarregar.
    QHash<QTableWidget *, qint64> remembered;
    for (auto *grid : {availableTable, loanedTable, maintenanceTable}) { remembered[grid]=selectedId(grid); grid->blockSignals(true); grid->clearSelection(); grid->setRowCount(0); }
    const auto needle=search->text().trimmed();
    for(const auto &n:data) {
        if((activityFilter->currentIndex()==0 && !n.active) || (activityFilter->currentIndex()==1 && n.active)) continue;
        // U5: inativos (sempre disponíveis) só aparecem em Disponíveis quando o filtro inclui inativos.
        const auto place = n.location.isEmpty() ? QStringLiteral("Não informado") : n.location + (n.locationDeleted ? QStringLiteral(" (excluído)") : QString());
        const auto name = n.name + (n.active ? QString() : QStringLiteral(" · Inativo"));
        QTableWidget *grid = availableTable; QStringList cells; QString note;
        if (n.status==Status::Loaned) {
            grid = loanedTable;
            const auto subject = n.borrowerPersonId==0 ? QStringLiteral("Não informado") : n.borrowerSubject.isEmpty() ? QStringLiteral("—") : n.borrowerSubject;
            cells = {n.assetTag,name,displayPerson(n.borrower,n.borrowerRole),subject,place,displayTime(n.checkedOutAt)};
        } else if (n.status==Status::Maintenance) {
            grid = maintenanceTable; note = n.maintenanceNote;
            cells = {n.assetTag,name,place,displayTime(n.maintenanceSince),note.isEmpty() ? QStringLiteral("—") : note};
        } else cells = {n.assetTag,name,place};
        if(!cells.join(' ').contains(needle,Qt::CaseInsensitive)) continue;
        int r=grid->rowCount(); grid->insertRow(r);
        for(int c=0;c<cells.size();++c) { auto *item=new QTableWidgetItem(cells[c]); item->setData(Qt::UserRole,n.id); grid->setItem(r,c,item); }
        if (grid==maintenanceTable && !note.isEmpty()) grid->item(r,4)->setToolTip("<qt>" + note.toHtmlEscaped() + "</qt>");
        if(n.id==remembered[grid]) { grid->setCurrentCell(r,0); grid->selectRow(r); }
    }
    const bool filtered = !needle.isEmpty() || activityFilter->currentIndex()!=0;
    const QString noResult = QStringLiteral("Nenhum resultado para os filtros atuais. Revise a busca ou limpe os filtros.");
    const std::array<std::tuple<QTableWidget *, QLabel *, QString>,3> tabs{{{availableTable,availableEmpty,"Nenhum notebook disponível."},{loanedTable,loanedEmpty,"Nenhum notebook emprestado."},{maintenanceTable,maintenanceEmpty,"Nenhum notebook em manutenção."}}};
    for (const auto &[grid,empty,text] : tabs) { grid->blockSignals(false); empty->setText(filtered ? noResult : text); empty->setVisible(grid->rowCount()==0); }
    const int shown=currentTable()->rowCount();
    inventorySummary->setText(shown==1 ? QStringLiteral("1 notebook") : QString::number(shown)+" notebooks");
    inventoryTabs->setVisible(!data.isEmpty()); emptyTitle->setVisible(data.isEmpty()); emptyBody->setVisible(data.isEmpty());
    updateActions();
}
QTableWidget *MainWindow::currentTable() const {
    const int index=inventoryTabs->currentIndex();
    return index==1 ? loanedTable : index==2 ? maintenanceTable : availableTable;
}
void MainWindow::selectNotebook(qint64 id) {
    auto *grid=currentTable();
    for(int r=0;r<grid->rowCount();++r)
        if(grid->item(r,0)->data(Qt::UserRole).toLongLong()==id) { grid->setCurrentCell(r,0); grid->selectRow(r); return; }
}
const Notebook *MainWindow::selected() const {
    const auto id=selectedId(currentTable());
    if(id<0) return nullptr;
    for(const auto &n:data) if(n.id==id) return &n;
    return nullptr;
}
void MainWindow::updateActions() {
    // Notebook inativo selecionado: nenhuma movimentação nem edição. Cada guia só tem os botões da matriz.
    const auto *n=selected(); const bool active=n && n->active;
    editButton->setEnabled(active);
    availableLocationButton->setEnabled(active && n->status==Status::Available);
    sendMaintenanceButton->setEnabled(active && n->status==Status::Available);
    checkoutButton->setEnabled(active && n->status==Status::Available);
    loanedLocationButton->setEnabled(active && n->status==Status::Loaned);
    returnButton->setEnabled(active && n->status==Status::Loaned);
    finishMaintenanceButton->setEnabled(active && n->status==Status::Maintenance);
    const bool blocked=active && n->status!=Status::Available;
    if (inactivateButton) {
        inactivateButton->setEnabled(active && n->status==Status::Available);
        inactivateButton->setToolTip(blocked ? inactivateReason->text() : QString());
    }
    inactivateReason->setVisible(inactivateButton && blocked);
    if (reactivateButton) {
        const bool inactive=n && !n->active;
        inactivateButton->setVisible(!inactive); reactivateButton->setVisible(inactive); reactivateButton->setEnabled(inactive);
    }
    const int shown=currentTable()->rowCount();
    inventorySummary->setText(shown==1 ? QStringLiteral("1 notebook") : QString::number(shown)+" notebooks");
}
void MainWindow::perform(const std::function<void()> &action, const QString &message) {
    try { action(); refresh(); feedback->setText("✓  " + message); }
    catch(const std::exception &e) { if (!handleExpired(e)) QMessageBox::warning(this,"Não foi possível concluir",QString::fromUtf8(e.what())); }
}
void MainWindow::addNotebook() {
    QDialog dialog(this); dialog.setWindowTitle("Cadastrar notebook"); dialog.setMinimumWidth(430);
    QFormLayout form(&dialog); auto *tag=new QLineEdit; tag->setMaxLength(60); tag->setPlaceholderText("Ex.: NB-001"); auto *name=new QLineEdit;name->setMaxLength(120);name->setPlaceholderText("Ex.: Dell Latitude 5420");
    tag->setObjectName("notebookAssetTag"); name->setObjectName("notebookName");
    // D-07: só locais ativos; sem locais ativos o cadastro fica bloqueado com orientação.
    auto *place = activeLocationCombo(locations); place->setObjectName("notebookLocation");
    form.addRow("Patrimônio *",tag); form.addRow("Nome do notebook *",name); form.addRow("Local atual *",place);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Save)->setText("Cadastrar");buttons->button(QDialogButtonBox::Cancel)->setText("Cancelar");form.addRow(buttons);
    buttons->button(QDialogButtonBox::Save)->setEnabled(place->isEnabled());
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] { try { store.add(session,tag->text(),name->text(),place->currentData().toLongLong());dialog.accept(); } catch(const std::exception &e){ if (!handleExpired(e)) QMessageBox::warning(&dialog,"Confira o cadastro",QString::fromUtf8(e.what()));} });
    if(dialog.exec()==QDialog::Accepted) perform([]{},"Notebook cadastrado.");
}
void MainWindow::checkout() {
    movementDialog(false);
}
void MainWindow::checkin() {
    movementDialog(true);
}
void MainWindow::movementDialog(bool returning) {
    const auto *n = selected(); if (!n) return; const auto id = n->id;
    QDialog dialog(this); dialog.setObjectName("movementDialog");
    dialog.setWindowTitle(returning ? "Registrar devolução" : "Registrar retirada"); dialog.setMinimumWidth(580);
    QFormLayout form(&dialog);
    auto *equipment = new QLabel(n->assetTag + " · " + n->name); equipment->setTextFormat(Qt::PlainText); equipment->setWordWrap(true);
    form.addRow("Notebook",equipment);
    QVector<Person> roster = people.activePeople();
    auto personEntries = [&roster] {
        QVector<QuickPickField::Entry> rows;
        for (const auto &p : roster)
            rows.append({p.id, p.name, p.name + " · " + roleLabel(p.role) + (p.subject.isEmpty() ? QString() : " · " + p.subject) + " · ID " + QString::number(p.id)});
        return rows;
    };
    auto locationEntries = [this] {
        QVector<QuickPickField::Entry> rows;
        for (const auto &place : locations.locations(CatalogFilter::Active)) rows.append({place.id, place.name, place.name});
        return rows;
    };
    auto *person = new QuickPickField("Digite nome, função, matéria ou ID","Adicionar pessoa",
        {"Pessoa encontrada (ID %1).","Escolha na lista ou use Adicionar pessoa.","Pessoa não encontrada. Use Adicionar pessoa.",
         "Nome repetido. Escolha a pessoa na lista.",returning ? "Informe quem devolveu o notebook." : "Informe quem retirou o notebook."});
    person->setObjectName("movementPerson"); person->editor()->setObjectName("movementPersonSearch"); person->addButton()->setObjectName("addPersonButton");
    person->setEntries(personEntries());
    auto *personRole = new QLabel("—"); personRole->setObjectName("movementPersonRole"); personRole->setTextFormat(Qt::PlainText);
    auto *personSubject = new QLabel("—"); personSubject->setObjectName("movementPersonSubject"); personSubject->setTextFormat(Qt::PlainText);
    form.addRow(returning ? "Devolvido por *" : "Retirado por *",person);
    form.addRow("Função",personRole); form.addRow("Matéria",personSubject);
    connect(person,&QuickPickField::selectionChanged,&dialog,[&] {
        personRole->setText("—"); personSubject->setText("—");
        for (const auto &p : roster) if (person->selectedId() == p.id) {
            personRole->setText(roleLabel(p.role)); personSubject->setText(p.subject.isEmpty() ? QStringLiteral("—") : p.subject);
        }
    });
    // U1: na retirada, o local atual vem pré-selecionado quando ainda está ativo.
    // D-06: na devolução, a origem do empréstimo aberto (somente se ativa); senão o campo fica vazio.
    const auto preferred = returning ? store.suggestedReturnLocation(id) : n->locationId > 0 ? std::optional<qint64>(n->locationId) : std::nullopt;
    auto *place = new QuickPickField("Digite o nome do local","Adicionar local",
        {"Local cadastrado.","Escolha na lista ou use Adicionar local.","Local não encontrado. Use Adicionar local.",{},
         returning ? "Informe o local após a devolução." : "Informe o local de uso."});
    place->setObjectName("movementLocation"); place->editor()->setObjectName("movementLocationSearch"); place->addButton()->setObjectName("addLocationButton");
    place->setEntries(locationEntries());
    if (preferred) place->select(*preferred);
    form.addRow(returning ? "Local após devolução *" : "Local de uso *",place);
    if (returning) { auto *hint = new QLabel("A devolução pode ser feita por outra pessoa, professor ou auxiliar."); hint->setWordWrap(true); form.addRow(hint); }
    connect(person,&QuickPickField::addRequested,&dialog,[&](const QString &typed) {
        try {
            PersonDialog create(people,[this] { return session; },PersonDialog::Mode::Create,Person{0,typed,ResponsibleRole::Professor,{},true},&dialog);
            if (create.exec() != QDialog::Accepted || !dialog.isVisible()) return;
            roster = people.activePeople(); person->setEntries(personEntries()); person->select(create.savedPersonId());
        } catch (const std::exception &e) {
            if (!handleExpired(e)) QMessageBox::warning(&dialog,"Confira a pessoa",QString::fromUtf8(e.what()));
        }
    });
    connect(place,&QuickPickField::addRequested,&dialog,[&](const QString &typed) {
        try {
            LocationDialog create(locations,[this] { return session; },std::nullopt,typed,&dialog);
            if (create.exec() != QDialog::Accepted || !dialog.isVisible()) return;
            place->setEntries(locationEntries()); place->select(create.savedLocationId());
        } catch (const std::exception &e) {
            if (!handleExpired(e)) QMessageBox::warning(&dialog,"Confira o local",QString::fromUtf8(e.what()));
        }
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(returning ? "Registrar devolução" : "Registrar retirada");
    buttons->button(QDialogButtonBox::Save)->setObjectName("primary"); buttons->button(QDialogButtonBox::Save)->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText("Cancelar"); form.addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        const bool placeOk = place->validate(), personOk = person->validate();
        if (!personOk || !placeOk) return;
        try {
            const qint64 personId = *person->selectedId(), locationId = *place->selectedId();
            if (returning) store.checkin(session,id,personId,locationId);
            else store.checkout(session,id,personId,locationId);
            dialog.accept();
        } catch (const std::exception &e) { if (!handleExpired(e)) QMessageBox::warning(&dialog,"Confira a movimentação",QString::fromUtf8(e.what())); }
    });
    person->editor()->setFocus();
    if (dialog.exec()==QDialog::Accepted) perform([]{},returning ? "Devolução registrada. O notebook está em Disponíveis." : "Retirada registrada. O notebook está em Indisponíveis.");
}
void MainWindow::updateLocation() {
    const auto *n = selected(); if (!n) return; const auto id = n->id;
    QDialog dialog(this); dialog.setObjectName("locationDialog"); dialog.setWindowTitle("Atualizar local"); dialog.setMinimumWidth(460);
    QFormLayout form(&dialog);
    auto *equipment = new QLabel(n->assetTag + " · " + n->name); equipment->setTextFormat(Qt::PlainText); equipment->setWordWrap(true); form.addRow("Notebook",equipment);
    const auto current = n->location.isEmpty() ? QStringLiteral("Não informado") : n->location + (n->locationDeleted ? QStringLiteral(" (excluído)") : QString());
    auto *currentLabel = new QLabel("Local atual: " + current); currentLabel->setObjectName("currentLocationLabel"); currentLabel->setTextFormat(Qt::PlainText); form.addRow(currentLabel);
    auto *place = activeLocationCombo(locations); form.addRow("Novo local *",place);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save); save->setText("Salvar local"); save->setObjectName("primary"); save->setEnabled(place->isEnabled());
    buttons->button(QDialogButtonBox::Cancel)->setText("Cancelar"); form.addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        try { store.updateLocation(session,id,place->currentData().toLongLong()); dialog.accept(); }
        catch (const std::exception &e) { if (!handleExpired(e)) QMessageBox::warning(&dialog,"Confira o local",QString::fromUtf8(e.what())); }
    });
    if (dialog.exec()==QDialog::Accepted) perform([]{},"Local atualizado.");
}
void MainWindow::sendToMaintenance() {
    const auto *n = selected(); if (!n) return; const auto id = n->id;
    QDialog dialog(this); dialog.setObjectName("maintenanceDialog"); dialog.setWindowTitle("Enviar à manutenção"); dialog.setMinimumWidth(460);
    QFormLayout form(&dialog);
    auto *equipment = new QLabel(n->assetTag + " · " + n->name); equipment->setTextFormat(Qt::PlainText); equipment->setWordWrap(true); form.addRow("Notebook",equipment);
    auto *note = new QLineEdit; note->setObjectName("maintenanceNote"); note->setMaxLength(200); note->setPlaceholderText("Ex.: tela piscando");
    form.addRow("Observação (opcional)",note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save); save->setText("Enviar à manutenção"); save->setObjectName("primary");
    buttons->button(QDialogButtonBox::Cancel)->setText("Cancelar"); form.addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        try { store.setMaintenance(session,id,true,note->text()); dialog.accept(); }
        catch (const std::exception &e) { if (!handleExpired(e)) QMessageBox::warning(&dialog,"Confira a manutenção",QString::fromUtf8(e.what())); }
    });
    note->setFocus();
    if (dialog.exec()==QDialog::Accepted) perform([]{},"Notebook enviado à manutenção. Ele está na guia Manutenção.");
}
void MainWindow::finishMaintenance() {
    const auto *n=selected(); if(!n) return; const auto id=n->id;
    if(QMessageBox::question(this,"Concluir manutenção","Concluir manutenção e disponibilizar este notebook?")==QMessageBox::Yes)
        perform([&]{store.setMaintenance(session,id,false);},"Manutenção concluída. O notebook está em Disponíveis.");
}
void MainWindow::editNotebook() {
    const auto *current=selected(); if(!current || !current->active) return;
    const Notebook expected=*current;   // cópia por valor: recarregamentos não invalidam a referência
    QDialog dialog(this); dialog.setWindowTitle("Editar notebook"); dialog.setMinimumWidth(430);
    QFormLayout form(&dialog);
    auto *tag=new QLineEdit(expected.assetTag); tag->setObjectName("notebookAssetTag"); tag->setMaxLength(60); tag->setPlaceholderText("Ex.: NB-001");
    auto *name=new QLineEdit(expected.name); name->setObjectName("notebookName"); name->setMaxLength(120); name->setPlaceholderText("Ex.: Dell Latitude 5420");
    // D-07: combo de locais ativos; local atual excluído ou texto legado vira "Manter local atual: {rótulo}" (dado -1 = manter).
    auto *place=new QComboBox; place->setObjectName("notebookLocation");
    for (const auto &row : locations.locations(CatalogFilter::Active)) place->addItem(row.name,row.id);
    const int currentIndex = expected.locationId > 0 && !expected.locationDeleted ? place->findData(expected.locationId) : -1;
    if (currentIndex >= 0) place->setCurrentIndex(currentIndex);
    else { place->insertItem(0,"Manter local atual: " + (expected.location.isEmpty() ? QStringLiteral("Não informado") : expected.location),qint64(-1)); place->setCurrentIndex(0); }
    form.addRow("Patrimônio *",tag); form.addRow("Nome do notebook *",name); form.addRow("Local atual *",place);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);
    auto *save=buttons->button(QDialogButtonBox::Save); save->setText("Salvar alterações"); save->setObjectName("primary");
    buttons->button(QDialogButtonBox::Cancel)->setText("Cancelar"); form.addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        try {
            const qint64 chosen = place->currentData().toLongLong();
            store.update(session,expected,tag->text(),name->text(),chosen > 0 ? std::optional<qint64>(chosen) : std::nullopt);
            dialog.accept();
        }
        catch(const std::exception &e) { if (!handleExpired(e)) QMessageBox::warning(&dialog,"Confira o cadastro",QString::fromUtf8(e.what())); }
    });
    tag->setFocus();
    if(dialog.exec()==QDialog::Accepted) { perform([]{},"Notebook atualizado."); selectNotebook(expected.id); }
    editButton->setFocus();
}
void MainWindow::inactivateNotebook() {
    const auto *n=selected(); if(!inactivateButton || !inactivateButton->isEnabled() || !n) return;
    const auto id=n->id;
    QMessageBox confirm(QMessageBox::Question,"Inativar notebook",QString("Inativar o notebook %1 · %2? Ele deixará de aparecer nas opções operacionais. O histórico será preservado.").arg(n->assetTag,n->name),QMessageBox::NoButton,this);
    confirm.setTextFormat(Qt::PlainText); auto *cancel=confirm.addButton("Cancelar",QMessageBox::RejectRole); auto *accept=confirm.addButton("Inativar notebook",QMessageBox::DestructiveRole); accept->setObjectName("destructive");
    confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel); confirm.exec();
    if(confirm.clickedButton()!=accept) { inactivateButton->setFocus(); return; }
    feedback->setText("Inativando…");
    perform([&]{store.inactivate(session,id);},"Notebook inativado.");
    if (feedback->text()=="Inativando…") feedback->setText("Inativação não concluída. Confira o notebook selecionado.");
    if (inactivateButton->isEnabled()) inactivateButton->setFocus();
}
void MainWindow::reactivateNotebook() {
    const auto *n=selected(); if(!reactivateButton || !reactivateButton->isEnabled() || !n || n->active) return;
    const auto id=n->id;
    QMessageBox confirm(QMessageBox::Question,"Reativar notebook",QString("Reativar o notebook %1 · %2? Ele voltará a aparecer nas opções operacionais com o status atual.").arg(n->assetTag,n->name),QMessageBox::NoButton,this);
    confirm.setTextFormat(Qt::PlainText); auto *cancel=confirm.addButton("Cancelar",QMessageBox::RejectRole); auto *accept=confirm.addButton("Reativar notebook",QMessageBox::AcceptRole); accept->setObjectName("primary");
    confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel); confirm.exec();
    if(confirm.clickedButton()!=accept) { reactivateButton->setFocus(); return; }
    feedback->setText("Reativando…");
    perform([&]{store.reactivate(session,id);},"Notebook reativado.");
    if (feedback->text()=="Reativando…") feedback->setText("Reativação não concluída. Confira o notebook selecionado.");
    selectNotebook(id);
    if (inactivateButton->isVisible() && inactivateButton->isEnabled()) inactivateButton->setFocus();
}

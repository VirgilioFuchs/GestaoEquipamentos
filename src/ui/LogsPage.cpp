#include "LogsPage.h"
#include "HistoryFormat.h"
#include "PersonPickerDialog.h"
#include "application/PeopleService.h"
#include "domain/EventCatalog.h"
#include <QtWidgets>
#include <stdexcept>

namespace {
const QString LoadError = QStringLiteral("Não foi possível carregar os logs. Selecione Aplicar filtros para tentar novamente.");
const QString OptionsError = QStringLiteral("Não foi possível carregar as opções de filtro. Selecione Aplicar filtros para tentar novamente.");
const QString PersonTextHelp = QStringLiteral("Encontra retiradas e devoluções. O texto digitado também alcança registros antigos e pode incluir homônimos.");
const QString PersonIdHelp = QStringLiteral("Somente retiradas e devoluções deste cadastro.");
constexpr int LegacyRole = Qt::UserRole + 1;

QLabel *filterLabel(const QString &text, QWidget *buddy) {
    auto *label = new QLabel(text); label->setObjectName("filterLabel"); label->setBuddy(buddy); buddy->setAccessibleName(text);
    return label;
}
QVBoxLayout *cell(QLabel *label, QWidget *control, QWidget *extra = nullptr) {
    auto *box = new QVBoxLayout; box->setSpacing(4); box->addWidget(label); box->addWidget(control);
    if (extra) box->addWidget(extra);
    box->addStretch();
    return box;
}
QLabel *note(const QString &objectName, const QString &text = {}) {
    auto *label = new QLabel(text); label->setObjectName(objectName); label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    return label;
}
QComboBox *filterCombo(const QString &objectName) {
    auto *combo = new QComboBox; combo->setObjectName(objectName); combo->setMaxVisibleItems(15);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); combo->setMinimumContentsLength(12);
    return combo;
}
void selectId(QComboBox *combo, std::optional<qint64> &id) {
    const int index = id ? combo->findData(QVariant::fromValue<qint64>(*id)) : 0;
    if (index < 0) id.reset();
    combo->setCurrentIndex(qMax(0, index));
}
std::optional<qint64> currentId(QComboBox *combo) {
    const auto value = combo->currentData();
    return value.isValid() ? std::optional<qint64>(value.toLongLong()) : std::nullopt;
}
void setError(QLabel *label, bool error) {
    label->setProperty("error", error); label->style()->unpolish(label); label->style()->polish(label);
}
}

LogsPage::LogsPage(HistoryQueryService &service, const SessionClock &clock, PeopleService &people, QWidget *parent)
    : QWidget(parent), service(service), clock(clock), people(people) {
    setObjectName("logsPage");
    auto *outer = new QVBoxLayout(this); outer->setContentsMargins(0,0,0,0);
    auto *scroll = new QScrollArea; scroll->setObjectName("logsScroll"); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto *content = new QWidget; content->setObjectName("logsContent"); scroll->setWidget(content);
    auto *layout = new QVBoxLayout(content); layout->setContentsMargins(0,0,0,0); layout->setSpacing(16);

    auto *heading = new QHBoxLayout; auto *titles = new QVBoxLayout; titles->setSpacing(4);
    auto *title = new QLabel("Logs operacionais"); title->setObjectName("sectionTitle"); titles->addWidget(title);
    auto *subtitle = note("muted", "Cadastros, movimentações, contas e alterações do sistema, do mais recente para o mais antigo."); titles->addWidget(subtitle);
    heading->addLayout(titles, 1);
    toggle = new QPushButton; toggle->setObjectName("filterToggle"); toggle->setCheckable(true);
    toggle->setAccessibleDescription("Mostra ou oculta o painel de filtros"); heading->addWidget(toggle, 0, Qt::AlignTop);
    layout->addLayout(heading);

    panel = new QFrame; panel->setObjectName("filterPanel");
    auto *grid = new QGridLayout(panel); grid->setContentsMargins(16,16,16,16); grid->setHorizontalSpacing(16); grid->setVerticalSpacing(16);
    for (int column = 0; column < 3; ++column) grid->setColumnStretch(column, 1);
    period = filterCombo("periodFilter");
    for (auto p : {PeriodPreset::Today,PeriodPreset::Last7Days,PeriodPreset::Last30Days,PeriodPreset::All,PeriodPreset::Custom}) period->addItem(periodLabel(p), int(p));
    grid->addLayout(cell(filterLabel("Período", period), period), 0, 0);
    notebook = filterCombo("notebookFilter"); grid->addLayout(cell(filterLabel("Notebook", notebook), notebook), 0, 1);
    location = filterCombo("locationFilter"); grid->addLayout(cell(filterLabel("Local", location), location), 0, 2);

    const QDate today = clock.nowUtc().toLocalTime().date();
    customRow = new QWidget; customRow->setObjectName("customDates");
    auto *customGrid = new QGridLayout(customRow); customGrid->setContentsMargins(0,0,0,0); customGrid->setHorizontalSpacing(16);
    for (int column = 0; column < 3; ++column) customGrid->setColumnStretch(column, 1);
    auto dateEdit = [&](const QString &name) {
        auto *edit = new QDateEdit(today); edit->setObjectName(name); edit->setDisplayFormat("dd/MM/yyyy"); edit->setCalendarPopup(true);
        edit->setMinimumDate(QDate(2000,1,1)); edit->setMaximumDate(today); return edit;
    };
    from = dateEdit("customFrom"); to = dateEdit("customTo");
    dateError = note("dateError"); dateError->hide();
    customGrid->addLayout(cell(filterLabel("Data inicial", from), from, dateError), 0, 0);
    customGrid->addLayout(cell(filterLabel("Data final", to), to), 0, 1);
    customGrid->addWidget(note("helpText", "Dias inteiros no horário deste computador."), 0, 2, Qt::AlignBottom);
    grid->addWidget(customRow, 1, 0, 1, 3);

    person = new QLineEdit; person->setObjectName("personFilter"); person->setPlaceholderText("Digite parte do nome"); person->setMaxLength(120); person->setClearButtonEnabled(true);
    selectPerson = new QPushButton("Selecionar pessoa"); selectPerson->setObjectName("selectPerson");
    clearPerson = new QPushButton("Remover seleção"); clearPerson->setObjectName("clearPerson"); clearPerson->hide();
    personHelp = note("personHelp", PersonTextHelp); person->setAccessibleDescription(PersonTextHelp);
    auto *personRow = new QHBoxLayout; personRow->setSpacing(8); personRow->addWidget(person, 1); personRow->addWidget(selectPerson); personRow->addWidget(clearPerson);
    auto *personBox = new QVBoxLayout; personBox->setSpacing(4); personBox->addWidget(filterLabel("Pessoa responsável", person)); personBox->addLayout(personRow); personBox->addWidget(personHelp);
    grid->addLayout(personBox, 2, 0, 1, 2);
    account = filterCombo("accountFilter");
    account->setToolTip("Conta que registrou o evento. Eventos antigos sem conta aparecem como Não informado e só são listados com Todas as contas.");
    grid->addLayout(cell(filterLabel("Conta operadora", account), account), 2, 2);
    optionsError = note("optionsError"); optionsError->hide(); grid->addWidget(optionsError, 3, 0, 1, 3);
    action = filterCombo("actionFilter"); grid->addLayout(cell(filterLabel("Tipo de evento", action), action), 4, 0);
    clearButton = new QPushButton("Limpar filtros"); clearButton->setObjectName("clearFilters");
    applyButton = new QPushButton("Aplicar filtros"); applyButton->setObjectName("applyFilters"); applyButton->setProperty("primaryAction", true);
    auto *buttons = new QHBoxLayout; buttons->setSpacing(8); buttons->addStretch(); buttons->addWidget(clearButton); buttons->addWidget(applyButton);
    grid->addLayout(buttons, 4, 1, 1, 2, Qt::AlignBottom);
    layout->addWidget(panel); panel->hide();

    auto *resultBox = new QVBoxLayout; resultBox->setSpacing(8);
    auto *resultRow = new QHBoxLayout;
    result = note("logsResult"); resultRow->addWidget(result, 1);
    pending = note("pendingFilters", "Filtros alterados — selecione Aplicar filtros para atualizar a lista."); pending->hide(); resultRow->addWidget(pending, 0, Qt::AlignRight);
    resultBox->addLayout(resultRow);
    summary = note("appliedSummary"); summary->hide(); resultBox->addWidget(summary);
    truncatedNotice = note("truncatedNotice", "Mostrando os 500 eventos mais recentes. Refine os filtros para ver eventos anteriores."); truncatedNotice->hide(); resultBox->addWidget(truncatedNotice);
    layout->addLayout(resultBox);

    events = new QTableWidget(0, 7); events->setObjectName("events");
    events->setHorizontalHeaderLabels({"Data e hora","Tipo","Notebook","Pessoa responsável","Conta operadora","Local","Detalhes"});
    events->horizontalHeaderItem(0)->setToolTip("Horário local deste computador");
    events->verticalHeader()->hide(); events->verticalHeader()->setDefaultSectionSize(58);
    events->setEditTriggers(QAbstractItemView::NoEditTriggers); events->setSelectionBehavior(QAbstractItemView::SelectRows); events->setSelectionMode(QAbstractItemView::SingleSelection);
    events->setAlternatingRowColors(true); events->setShowGrid(false); events->setWordWrap(false); events->setTextElideMode(Qt::ElideRight); events->setMinimumHeight(190);
    auto *header = events->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive); header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const int widths[] = {0, 172, 220, 220, 140, 160};
    for (int column = 1; column < 6; ++column) events->setColumnWidth(column, widths[column]);
    // Detalhes ocupa a sobra, mas nunca fica abaixo de 240px; quando não cabe, a tabela rola na horizontal.
    events->viewport()->installEventFilter(this);
    connect(header, &QHeaderView::sectionResized, this, [this](int column) { if (column != 6) fitDetailsColumn(); });
    fitDetailsColumn();
    layout->addWidget(events, 1);
    emptyState = new QFrame; emptyState->setObjectName("emptyState"); emptyState->setMinimumHeight(190);
    auto *emptyLayout = new QVBoxLayout(emptyState); emptyLayout->setContentsMargins(24,24,24,24); emptyLayout->setSpacing(8);
    emptyTitle = note("emptyTitle"); emptyBody = note("emptyBody"); emptyLayout->addWidget(emptyTitle); emptyLayout->addWidget(emptyBody); emptyLayout->addStretch();
    emptyState->hide(); layout->addWidget(emptyState, 1);

    connect(toggle, &QPushButton::clicked, this, [this](bool checked) { setPanelExpanded(checked); if (checked) period->setFocus(); });
    connect(period, &QComboBox::currentIndexChanged, this, [this] {
        const auto previous = draft.period;
        draft.period = PeriodPreset(period->currentData().toInt());
        if (draft.period == PeriodPreset::Custom && previous != PeriodPreset::Custom) {
            const QDate day = this->clock.nowUtc().toLocalTime().date();
            const int span = previous == PeriodPreset::Last7Days ? 6 : previous == PeriodPreset::Last30Days ? 29 : 0;
            const QSignalBlocker a(from), b(to); from->setDate(day.addDays(-span)); to->setDate(day);
            draft.customFrom = from->date(); draft.customTo = to->date();
        }
        updateCustomDates(); updatePending();
    });
    connect(from, &QDateEdit::dateChanged, this, [this](QDate d) { draft.customFrom = d; updatePending(); });
    connect(to, &QDateEdit::dateChanged, this, [this](QDate d) { draft.customTo = d; updatePending(); });
    connect(notebook, &QComboBox::currentIndexChanged, this, [this] { draft.notebookId = currentId(notebook); updatePending(); });
    connect(account, &QComboBox::currentIndexChanged, this, [this] { draft.accountId = currentId(account); updatePending(); });
    connect(action, &QComboBox::currentIndexChanged, this, [this] { draft.action = action->currentData().toString(); updatePending(); });
    connect(location, &QComboBox::currentIndexChanged, this, [this] {
        draft.locationId = currentId(location); draft.legacyLocationText = location->currentData(LegacyRole).toString(); updatePending();
    });
    connect(person, &QLineEdit::textChanged, this, [this](const QString &text) { if (draft.personId) return; draft.personText = text.trimmed(); updatePending(); });
    connect(selectPerson, &QPushButton::clicked, this, &LogsPage::choosePerson);
    connect(clearPerson, &QPushButton::clicked, this, &LogsPage::removePerson);
    connect(clearButton, &QPushButton::clicked, this, &LogsPage::clearFilters);
    connect(applyButton, &QPushButton::clicked, this, &LogsPage::applyFilters);
    for (auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *shortcut = new QShortcut(QKeySequence(key), panel); shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this] {
            auto *focused = qobject_cast<QPushButton *>(QApplication::focusWidget());
            if (focused == clearButton || focused == selectPerson || focused == clearPerson) { focused->click(); return; }
            applyFilters();
        });
    }
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), panel); escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { setPanelExpanded(false); toggle->setFocus(); });
    const QList<QWidget *> chain{toggle, period, from, to, notebook, location, person, selectPerson, clearPerson, account, action, clearButton, applyButton, events};
    for (int i = 1; i < chain.size(); ++i) QWidget::setTabOrder(chain[i - 1], chain[i]);

    draft.customFrom = draft.customTo = today;
    setPanelExpanded(false); updateCustomDates(); updateToggle(); reloadOptions(); updatePending();
}
void LogsPage::setPanelExpanded(bool expanded) {
    toggle->setChecked(expanded); panel->setVisible(expanded);
    toggle->setToolTip(expanded ? QStringLiteral("Ocultar filtros") : QStringLiteral("Mostrar filtros"));
}
void LogsPage::updateCustomDates() {
    const QDate today = clock.nowUtc().toLocalTime().date();
    from->setMaximumDate(today); to->setMaximumDate(today);
    customRow->setVisible(draft.period == PeriodPreset::Custom);
    if (draft.period != PeriodPreset::Custom) dateError->hide();
}
void LogsPage::updatePending() { pending->setVisible(!(draft == applied)); }
void LogsPage::updateToggle() {
    const int count = applied.activeCount();
    toggle->setText(QString("Filtros (%1)").arg(count)); toggle->setAccessibleName(QString("Filtros, %1 ativos").arg(count));
}
void LogsPage::updatePersonField() {
    const QSignalBlocker blocker(person);
    const bool selected = draft.personId.has_value();
    person->setReadOnly(selected); person->setClearButtonEnabled(!selected);
    person->setText(selected ? selectedPersonLabel : draft.personText);
    clearPerson->setVisible(selected);
    personHelp->setText(selected ? PersonIdHelp : PersonTextHelp); person->setAccessibleDescription(personHelp->text());
}
void LogsPage::choosePerson() {
    PersonPickerDialog dialog(people, {}, this, PersonPickerDialog::Mode::Query);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    selectPerson->setFocus();
    const auto chosen = accepted ? dialog.selectedPerson() : std::nullopt;
    if (!chosen) return;
    draft.personId = chosen->id; draft.personText.clear();
    selectedPersonLabel = chosen->name + " · " + roleLabel(chosen->role) + (chosen->subject.isEmpty() ? QString() : " · " + chosen->subject)
                        + (chosen->active ? QStringLiteral(" (cadastro)") : QStringLiteral(" (cadastro inativo)"));
    updatePersonField(); updatePending();
}
void LogsPage::removePerson() {
    draft.personId.reset(); draft.personText.clear(); selectedPersonLabel.clear();
    updatePersonField(); person->setFocus(); updatePending();
}
void LogsPage::syncWidgetsFromDraft() {
    const QSignalBlocker a(period), b(from), c(to), d(notebook), e(location), f(account), g(action);
    period->setCurrentIndex(period->findData(int(draft.period)));
    from->setDate(draft.customFrom); to->setDate(draft.customTo);
    selectId(notebook, draft.notebookId); selectId(account, draft.accountId);
    action->setCurrentIndex(qMax(0, action->findData(draft.action)));
    if (draft.locationId) selectId(location, draft.locationId);
    else location->setCurrentIndex(draft.legacyLocationText.isEmpty() ? 0 : qMax(0, location->findData(draft.legacyLocationText, LegacyRole)));
    updatePersonField(); updateCustomDates();
}
void LogsPage::clearFilters() {
    draft = HistoryFilter{};
    draft.customFrom = draft.customTo = clock.nowUtc().toLocalTime().date();
    selectedPersonLabel.clear(); dateError->hide();
    syncWidgetsFromDraft(); period->setFocus(); updatePending();
}
bool LogsPage::reloadOptions() {
    HistoryFilterOptions options;
    try { options = service.filterOptions(); }
    catch (const std::exception &) { optionsError->setText(OptionsError); optionsError->show(); return false; }
    optionsError->hide();
    {
        const QSignalBlocker a(notebook), b(location), c(account), d(action);
        notebook->clear(); notebook->addItem("Todos os notebooks");
        for (const auto &option : options.notebooks) { notebook->addItem(option.label, QVariant::fromValue<qint64>(option.id)); notebook->setItemData(notebook->count() - 1, option.label, Qt::ToolTipRole); }
        location->clear(); location->addItem("Todos os locais");
        for (const auto &option : options.activeLocations) location->addItem(option.label, QVariant::fromValue<qint64>(option.id));
        if (!options.deletedLocations.isEmpty()) location->insertSeparator(location->count());
        for (const auto &option : options.deletedLocations) location->addItem(option.label, QVariant::fromValue<qint64>(option.id));
        if (!options.legacyLocationTexts.isEmpty()) location->insertSeparator(location->count());
        for (const auto &text : options.legacyLocationTexts) { location->addItem(text + QStringLiteral(" (registro antigo)")); location->setItemData(location->count() - 1, text, LegacyRole); }
        account->clear(); account->addItem("Todas as contas");
        for (const auto &option : options.accounts) account->addItem(option.label, QVariant::fromValue<qint64>(option.id));
        action->clear(); action->addItem("Todos os tipos", QString());
        QString group;
        for (const auto &recorded : options.actions) {
            const auto current = eventGroup(recorded);
            if (current != group) { action->insertSeparator(action->count()); group = current; }
            action->addItem(eventLabel(recorded), recorded);
        }
    }
    syncWidgetsFromDraft();
    return true;
}
std::optional<HistoryPage> LogsPage::runQuery(const HistoryFilter &filter) {
    ++queries;
    try { return service.search(filter, clock.nowUtc(), QTimeZone::systemTimeZone()); }
    catch (const std::exception &) { return std::nullopt; }
}
QString LogsPage::periodSummary(const HistoryFilter &filter) const {
    const QDate today = clock.nowUtc().toLocalTime().date();
    const auto range = [](const QDate &first, const QDate &last) {
        return first == last ? first.toString("dd/MM/yyyy") : first.toString("dd/MM/yyyy") + " a " + last.toString("dd/MM/yyyy");
    };
    switch (filter.period) {
    case PeriodPreset::Today: return "Hoje, " + today.toString("dd/MM/yyyy");
    case PeriodPreset::Last7Days: return range(today.addDays(-6), today);
    case PeriodPreset::Last30Days: return range(today.addDays(-29), today);
    case PeriodPreset::All: return QStringLiteral("Todo o período");
    case PeriodPreset::Custom: return range(filter.customFrom, filter.customTo);
    }
    return {};
}
QString LogsPage::draftSummary() const {
    QStringList parts;
    if (draft.notebookId) parts << "Notebook " + notebook->currentText();
    if (draft.locationId || !draft.legacyLocationText.isEmpty()) parts << "Local " + location->currentText();
    if (draft.personId) parts << "Pessoa " + selectedPersonLabel;
    else if (!draft.personText.isEmpty()) parts << "Pessoa contém \"" + draft.personText + "\"";
    if (draft.accountId) parts << "Conta " + account->currentText();
    if (!draft.action.isEmpty()) parts << "Tipo " + action->currentText();
    return parts.isEmpty() ? QString() : "Filtros aplicados: " + parts.join("; ");
}
void LogsPage::showPage(const HistoryPage &page, const HistoryFilter &filter) {
    events->setRowCount(0);
    for (const auto &entry : page.entries) {
        const int row = events->rowCount(); events->insertRow(row);
        const QStringList cells{entry.happenedAt.isValid() ? entry.happenedAt.toLocalTime().toString("dd/MM/yyyy HH:mm:ss") : QStringLiteral("—"),
                                eventLabel(entry.action), notebookText(entry), responsibleText(entry), actorText(entry), locationText(entry), historyDetails(entry)};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells[column]);
            const auto tooltip = column == 6 ? QString(cells[column]).replace("; ", "\n") : cells[column];
            item->setToolTip("<qt>" + tooltip.toHtmlEscaped().replace("\n", "<br>") + "</qt>");
            events->setItem(row, column, item);
        }
    }
    const bool none = page.entries.isEmpty();
    events->setVisible(!none); emptyState->setVisible(none);
    if (none) {
        const bool onlyPeriod = filter.activeCount() == int(filter.period != PeriodPreset::All);
        if (onlyPeriod && filter.period == PeriodPreset::Today) {
            emptyTitle->setText("Nenhum evento hoje");
            emptyBody->setText("Abra Filtros, escolha um período maior, como Últimos 7 dias, e selecione Aplicar filtros.");
        } else if (onlyPeriod && filter.period == PeriodPreset::All) {
            emptyTitle->setText("Nenhum evento registrado");
            emptyBody->setText("Os eventos aparecem aqui depois de cadastros, movimentações e alterações de contas.");
        } else {
            emptyTitle->setText("Nenhum evento encontrado");
            emptyBody->setText("Nenhum evento corresponde aos filtros aplicados. Amplie o período ou ajuste os filtros e selecione Aplicar filtros.");
        }
    }
    setError(result, false);
    const int count = page.entries.size();
    result->setText(none ? QString() : (count == 1 ? QStringLiteral("1 evento") : QString("%1 eventos").arg(count)) + " · " + periodSummary(filter));
    truncatedNotice->setVisible(page.truncated);
    summary->setText(appliedSummary); summary->setVisible(!appliedSummary.isEmpty());
}
void LogsPage::showLoadError() {
    events->setRowCount(0); events->hide(); emptyState->hide(); truncatedNotice->hide();
    setError(result, true); result->setText(LoadError);
}
void LogsPage::applyFilters() {
    if (applying) return;
    if (draft.period == PeriodPreset::Custom && draft.customFrom > draft.customTo) {
        dateError->setText("A data inicial deve ser igual ou anterior à data final."); dateError->show();
        from->setAccessibleDescription(dateError->text()); from->setFocus();
        return;
    }
    dateError->hide(); from->setAccessibleDescription({});
    applying = true;
    QPointer<QWidget> focused = QApplication::focusWidget();
    applyButton->setEnabled(false); applyButton->setText("Aplicando…"); setError(result, false); result->setText("Carregando eventos…");
    applyButton->repaint(); result->repaint();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto summaryText = draftSummary();
    reloadOptions();
    const auto candidate = draft;
    if (const auto page = runQuery(candidate)) { applied = candidate; appliedSummary = summaryText; showPage(*page, applied); }
    else showLoadError();
    QApplication::restoreOverrideCursor();
    applyButton->setText("Aplicar filtros"); applyButton->setEnabled(true);
    if (focused) focused->setFocus();
    updateToggle(); updatePending();
    applying = false;
}
void LogsPage::reloadApplied() {
    if (applying) return;
    reloadOptions();
    if (const auto page = runQuery(applied)) showPage(*page, applied); else showLoadError();
    updateToggle(); updatePending();
}
bool LogsPage::eventFilter(QObject *watched, QEvent *event) {
    if (watched == events->viewport() && event->type() == QEvent::Resize) fitDetailsColumn();
    return QWidget::eventFilter(watched, event);
}
void LogsPage::fitDetailsColumn() {
    int used = 0;
    for (int column = 0; column < 6; ++column) used += events->columnWidth(column);
    const int width = qMax(240, events->viewport()->width() - used);
    if (events->columnWidth(6) != width) events->setColumnWidth(6, width);
}

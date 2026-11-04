#pragma once
#include "application/HistoryQueryService.h"
#include "application/SessionClock.h"
#include <QWidget>
#include <optional>
class PeopleService;
class QComboBox;
class QDateEdit;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
// Rascunho (painel) × aplicado (tabela): só Aplicar filtros copia o rascunho e consulta. Nenhuma regra de banco aqui.
class LogsPage : public QWidget {
    Q_OBJECT
public:
    LogsPage(HistoryQueryService &service, const SessionClock &clock, PeopleService &people, QWidget *parent = nullptr);
    void reloadApplied();
    void setPanelExpanded(bool expanded);
    HistoryFilter appliedFilter() const { return applied; }
    HistoryFilter draftFilter() const { return draft; }
    int queryCount() const { return queries; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void fitDetailsColumn();
    void applyFilters();
    void clearFilters();
    bool reloadOptions();
    std::optional<HistoryPage> runQuery(const HistoryFilter &filter);
    void showPage(const HistoryPage &page, const HistoryFilter &filter);
    void showLoadError();
    void updateCustomDates();
    void updatePending();
    void updateToggle();
    void updatePersonField();
    void choosePerson();
    void removePerson();
    void syncWidgetsFromDraft();
    QString periodSummary(const HistoryFilter &filter) const;
    QString draftSummary() const;
    HistoryQueryService &service;
    const SessionClock &clock;
    PeopleService &people;
    HistoryFilter applied, draft;
    QString selectedPersonLabel, appliedSummary;
    bool applying = false;
    int queries = 0;
    QPushButton *toggle, *selectPerson, *clearPerson, *clearButton, *applyButton;
    QFrame *panel, *emptyState;
    QWidget *customRow;
    QComboBox *period, *notebook, *location, *account, *action;
    QDateEdit *from, *to;
    QLineEdit *person;
    QLabel *dateError, *personHelp, *optionsError, *pending, *result, *summary, *truncatedNotice, *emptyTitle,
        *emptyBody;
    QTableWidget *events;
};

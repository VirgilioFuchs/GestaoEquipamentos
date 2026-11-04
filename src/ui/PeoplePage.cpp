#include "PeoplePage.h"
#include "PersonDialog.h"
#include <QtWidgets>

namespace {
QLabel *plainLabel(const QString &text, const char *name) {
    auto *label=new QLabel(text); label->setObjectName(name); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); return label;
}
// O tooltip nativo interpreta HTML automaticamente; nomes são sempre texto simples.
class PeopleTable : public QTableWidget {
public:
    PeopleTable() : QTableWidget(0,5), tooltip(new QLabel(this,Qt::ToolTip)) { tooltip->setTextFormat(Qt::PlainText); tooltip->setMargin(8); }
protected:
    bool viewportEvent(QEvent *event) override {
        if (event->type()==QEvent::ToolTip) {
            const auto *help=static_cast<QHelpEvent *>(event); const auto *item=itemAt(help->pos());
            if (item) { tooltip->setText(item->toolTip()); tooltip->adjustSize(); tooltip->move(help->globalPos()+QPoint(12,16)); tooltip->show(); QTimer::singleShot(4000,tooltip,&QLabel::hide); }
            else tooltip->hide();
            return true;
        }
        if (event->type()==QEvent::Leave || event->type()==QEvent::MouseButtonPress) tooltip->hide();
        return QTableWidget::viewportEvent(event);
    }
private:
    QLabel *tooltip;
};
QString saveError(const std::exception &e) {
    const auto message=QString::fromUtf8(e.what());
    const QStringList expected={"Nome é obrigatório.","Nome excede o limite de caracteres.","Selecione Professor ou Auxiliar.","Matéria excede o limite de caracteres.",
        "Pessoa não encontrada.","Cadastros inativos não podem ser editados.","Nenhuma alteração para salvar.","Esta pessoa já está inativa.",
        "Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente."};
    if (dynamic_cast<const AuthorizationError *>(&e) || expected.contains(message)) return message;
    return "Não foi possível salvar o cadastro. Confira os dados e tente novamente.";
}
}

PeoplePage::PeoplePage(PeopleService &people, const Session &session, QWidget *parent) : QWidget(parent),people(people),session(session) {
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(24,24,24,24); layout->setSpacing(16);
    auto *heading=new QHBoxLayout; auto *title=plainLabel("Pessoas","sectionTitle"); title->setStyleSheet("font-size: 19px; font-weight: 600;"); heading->addWidget(title); heading->addStretch();
    create=new QPushButton("Cadastrar pessoa"); create->setObjectName("peopleCreate"); create->setMinimumHeight(40);
    create->setStyleSheet("QPushButton { background: #355cdf; color: white; } QPushButton:hover { background: #254bc8; } QPushButton:disabled { background: #d8e1ed; color: #6a7b8e; } QPushButton:focus { border: 2px solid #17365d; }");
    heading->addWidget(create); layout->addLayout(heading);
    layout->addWidget(plainLabel("Pessoas são responsáveis por movimentações. Contas de acesso são gerenciadas na Administração.","peopleHelp"));
    auto *toolbar=new QHBoxLayout;
    search=new QLineEdit; search->setObjectName("peopleSearch"); search->setPlaceholderText("Buscar pessoa por ID, nome, função ou matéria"); search->setAccessibleName(search->placeholderText()); search->setClearButtonEnabled(true);
    filter=new QComboBox; filter->setObjectName("peopleFilter"); filter->setAccessibleName("Situação das pessoas"); filter->addItems({"Ativas","Inativas","Todas"});
    toolbar->addWidget(search,1); toolbar->addWidget(filter); layout->addLayout(toolbar);
    state=plainLabel({},"peopleState"); layout->addWidget(state);
    emptyTitle=plainLabel("Nenhuma pessoa cadastrada","sectionTitle"); emptyTitle->setStyleSheet("font-size: 19px; font-weight: 600;"); layout->addWidget(emptyTitle);
    emptyBody=plainLabel("Selecione Cadastrar pessoa para criar o primeiro cadastro de responsável.","peopleEmptyBody"); layout->addWidget(emptyBody);
    retry=new QPushButton("Tentar novamente"); retry->setObjectName("peopleRetry"); layout->addWidget(retry,0,Qt::AlignLeft);
    table=new PeopleTable; table->setObjectName("peopleTable"); table->setHorizontalHeaderLabels({"Nome","Função","Matéria","Situação","ID"});
    table->horizontalHeader()->moveSection(4,0);
    table->verticalHeader()->hide(); table->verticalHeader()->setDefaultSectionSize(58); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection); table->setTextElideMode(Qt::ElideRight);
    table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch); table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch); table->horizontalHeader()->setSectionResizeMode(3,QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4,QHeaderView::ResizeToContents);
    table->setAlternatingRowColors(true); table->setShowGrid(false); layout->addWidget(table,1);
    summary=plainLabel("0 pessoas","peopleSummary"); layout->addWidget(summary);
    auto *actions=new QHBoxLayout; actions->addStretch();
    edit=new QPushButton("Editar pessoa"); edit->setObjectName("peopleEdit"); edit->setMinimumHeight(40); actions->addWidget(edit);
    if (session.role()==AccountRole::Administrator || session.role()==AccountRole::Principal) {
        remove=new QPushButton("Inativar pessoa"); remove->setObjectName("destructive"); remove->setMinimumHeight(40); actions->addWidget(remove);
        connect(remove,&QPushButton::clicked,this,&PeoplePage::inactivatePerson);
    }
    layout->addLayout(actions); feedback=plainLabel({},"peopleFeedback"); layout->addWidget(feedback);
    connect(create,&QPushButton::clicked,this,[this]{savePerson(std::nullopt);});
    connect(edit,&QPushButton::clicked,this,[this]{if (const auto *person=selected(); person && edit->isEnabled()) savePerson(*person);});
    connect(table,&QTableWidget::itemSelectionChanged,this,&PeoplePage::updateActions);
    connect(search,&QLineEdit::textChanged,this,&PeoplePage::reload); connect(filter,&QComboBox::currentIndexChanged,this,&PeoplePage::reload);
    connect(retry,&QPushButton::clicked,this,&PeoplePage::reload); reload();
}
const Person *PeoplePage::selected() const {
    const int row=table->currentRow();
    if (row<0 || !table->item(row,0) || !table->item(row,0)->isSelected()) return nullptr;
    const auto id=table->item(row,0)->data(Qt::UserRole).toLongLong();
    for (const auto &person : rows) if (person.id==id) return &person;
    return nullptr;
}
void PeoplePage::updateActions() {
    const auto *person=selected(); const bool enabled=table->isEnabled() && person && person->active;
    edit->setEnabled(enabled); if (remove) remove->setEnabled(enabled);
}
void PeoplePage::reload() {
    if (const auto *person=selected()) selectedId=person->id;
    const int generation=++loadGeneration;
    state->setText("Carregando cadastros…"); state->show(); emptyTitle->hide(); emptyBody->hide(); retry->hide();
    table->setEnabled(false); create->setEnabled(false); edit->setEnabled(false); if (remove) remove->setEnabled(false);
    QTimer::singleShot(0,this,[this,generation] {
        if (generation!=loadGeneration) return;
        try {
            rows=people.people(static_cast<CatalogFilter>(filter->currentIndex()),search->text());
            const bool hasAny=!rows.isEmpty() || !people.people(CatalogFilter::All).isEmpty();
            table->setRowCount(0); bool changedVisible=false;
            for (const auto &person : rows) {
                const int row=table->rowCount(); table->insertRow(row);
                const QStringList values={person.name,roleLabel(person.role),person.subject.isEmpty() ? QStringLiteral("—") : person.subject,person.active ? QStringLiteral("Ativa") : QStringLiteral("Inativa"),QString::number(person.id)};
                for (int column=0;column<values.size();++column) { auto *item=new QTableWidgetItem(values[column]); item->setData(Qt::UserRole,person.id); item->setToolTip(values[column]); table->setItem(row,column,item); }
                if (person.id==(changedId ? changedId : selectedId)) {table->setCurrentCell(row,0); table->selectRow(row);}
                changedVisible=changedVisible || person.id==changedId;
            }
            summary->setText(rows.size()==1 ? QStringLiteral("1 pessoa") : QString::number(rows.size())+" pessoas");
            table->setVisible(!rows.isEmpty()); state->setVisible(rows.isEmpty() && hasAny);
            if (rows.isEmpty() && hasAny) state->setText("Nenhum resultado para os filtros atuais. Revise a busca ou limpe os filtros.");
            emptyTitle->setVisible(!hasAny); emptyBody->setVisible(!hasAny);
            if (changedId) { feedback->setText(changedVisible ? savedMessage : savedMessage+" O cadastro não aparece no filtro atual."); changedId=0; }
            table->setEnabled(true);
        } catch (const std::exception &) {
            rows.clear(); table->setRowCount(0); table->hide(); summary->clear(); state->show();
            state->setText("Não foi possível carregar os cadastros. Selecione Tentar novamente."); retry->show();
        }
        create->setEnabled(true); updateActions();
    });
}
void PeoplePage::savePerson(const std::optional<Person> &expected) {
    // Copiar o cadastro esperado: reload() pode substituir as linhas da página.
    const auto person=expected.value_or(Person{});
    PersonDialog dialog(people,[this] { return session; },expected ? PersonDialog::Mode::Edit : PersonDialog::Mode::Create,person,this);
    try {
        if (dialog.exec()==QDialog::Accepted) {
            changedId=dialog.savedPersonId(); selectedId=changedId;
            savedMessage=expected ? QStringLiteral("Pessoa atualizada.") : QStringLiteral("Pessoa cadastrada.");
            feedback->setText(savedMessage); reload(); if (onChanged) onChanged();
        }
    } catch (const AuthorizationError &e) {
        feedback->setText(saveError(e));
        if (e.reason()==AuthorizationError::Reason::Expired && onExpired) onExpired();
    }
    (expected ? edit : create)->setFocus();
}
void PeoplePage::inactivatePerson() {
    const auto *person=selected(); if (!remove || !remove->isEnabled() || !person) return;
    const auto id=person->id; const auto name=person->name;
    QMessageBox confirm(QMessageBox::Question,"Inativar pessoa",QString("Inativar %1? A pessoa deixará de aparecer nas novas seleções. O histórico será preservado.").arg(name),QMessageBox::NoButton,this);
    confirm.setTextFormat(Qt::PlainText); auto *cancel=confirm.addButton("Cancelar",QMessageBox::RejectRole); auto *accept=confirm.addButton("Inativar pessoa",QMessageBox::DestructiveRole); accept->setObjectName("destructive");
    confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel); confirm.exec();
    if (confirm.clickedButton()!=accept) {remove->setFocus(); return;}
    remove->setEnabled(false); edit->setEnabled(false); table->setEnabled(false); feedback->setText("Inativando…");
    QTimer::singleShot(0,this,[this,id] {
        try {
            people.inactivate(session,id); changedId=id; selectedId=id; savedMessage="Pessoa inativada.";
            feedback->setText(savedMessage); reload(); if (onChanged) onChanged();
        } catch (const std::exception &e) {
            feedback->setText(saveError(e)); const auto *refused=dynamic_cast<const AuthorizationError *>(&e);
            if (refused && refused->reason()==AuthorizationError::Reason::Expired && onExpired) onExpired();
            table->setEnabled(true); updateActions();
        }
        remove->setFocus();
    });
}

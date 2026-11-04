#include "LocationsPage.h"
#include "LocationDialog.h"
#include <QtWidgets>

namespace {
// QToolTip auto-detecta HTML. O popup nativo usa QLabel com texto explicitamente simples.
class PlainTextTable : public QTableWidget {
public:
    PlainTextTable() : QTableWidget(0,3), tooltip(new QLabel(this,Qt::ToolTip)) {
        tooltip->setTextFormat(Qt::PlainText); tooltip->setMargin(8);
    }
protected:
    bool viewportEvent(QEvent *event) override {
        if (event->type() == QEvent::ToolTip) {
            const auto *help = static_cast<QHelpEvent *>(event);
            const auto *item = itemAt(help->pos());
            if (item) {
                tooltip->setText(item->toolTip()); tooltip->adjustSize();
                tooltip->move(help->globalPos()+QPoint(12,16)); tooltip->show();
                QTimer::singleShot(4000,tooltip,&QLabel::hide);
            } else tooltip->hide();
            return true;
        }
        if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) tooltip->hide();
        return QTableWidget::viewportEvent(event);
    }
private:
    QLabel *tooltip;
};
QLabel *plainLabel(const QString &text, const char *name) {
    auto *label = new QLabel(text); label->setObjectName(name); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); return label;
}
}

LocationsPage::LocationsPage(LocationService &locations, const Session &session, QWidget *parent)
    : QWidget(parent), locations(locations), session(session) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(24,24,24,24); layout->setSpacing(16);
    auto *heading = new QHBoxLayout;
    auto *title = plainLabel("Locais","sectionTitle"); title->setStyleSheet("font-size: 19px; font-weight: 600;"); heading->addWidget(title); heading->addStretch();
    create = new QPushButton("Cadastrar local"); create->setObjectName("locationsCreate");
    create->setStyleSheet("QPushButton { background: #355cdf; color: white; } QPushButton:hover { background: #254bc8; } QPushButton:disabled { background: #d8e1ed; color: #6a7b8e; } QPushButton:focus { border: 2px solid #17365d; }");
    create->setMinimumHeight(40); heading->addWidget(create); layout->addLayout(heading);
    auto *toolbar = new QHBoxLayout;
    search = new QLineEdit; search->setObjectName("locationsSearch"); search->setPlaceholderText("Buscar local por ID ou nome"); search->setAccessibleName(search->placeholderText()); search->setClearButtonEnabled(true);
    filter = new QComboBox; filter->setObjectName("locationsFilter"); filter->setAccessibleName("Situação dos locais"); filter->addItems({"Ativos","Excluídos","Todos"});
    toolbar->addWidget(search,1); toolbar->addWidget(filter); layout->addLayout(toolbar);
    state = plainLabel({},"locationsState"); layout->addWidget(state);
    emptyTitle = plainLabel("Nenhum local cadastrado","sectionTitle"); layout->addWidget(emptyTitle);
    emptyTitle->setStyleSheet("font-size: 19px; font-weight: 600;");
    emptyBody = plainLabel("Selecione Cadastrar local para adicionar um espaço da escola.","locationsEmptyBody"); layout->addWidget(emptyBody);
    retry = new QPushButton("Tentar novamente"); retry->setObjectName("locationsRetry"); layout->addWidget(retry,0,Qt::AlignLeft);
    table = new PlainTextTable; table->setObjectName("locationsTable"); table->setHorizontalHeaderLabels({"Nome","Situação","ID"});
    table->horizontalHeader()->moveSection(2,0);
    table->verticalHeader()->hide(); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch); table->setTextElideMode(Qt::ElideRight);
    table->horizontalHeader()->setStretchLastSection(false); table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    table->setAlternatingRowColors(true); table->setShowGrid(false); table->verticalHeader()->setDefaultSectionSize(58); layout->addWidget(table,1);
    summary = plainLabel("0 locais","locationsSummary"); layout->addWidget(summary);
    auto *actions = new QHBoxLayout; actions->addStretch();
    edit = new QPushButton("Editar local"); edit->setObjectName("locationsEdit"); edit->setMinimumHeight(40); actions->addWidget(edit);
    remove = new QPushButton("Excluir local"); remove->setObjectName("destructive"); remove->setMinimumHeight(40);
    actions->addWidget(remove); layout->addLayout(actions);
    feedback = plainLabel({},"locationsFeedback"); layout->addWidget(feedback);
    connect(create,&QPushButton::clicked,this,&LocationsPage::createLocation);
    connect(remove,&QPushButton::clicked,this,&LocationsPage::deleteLocation);
    connect(edit,&QPushButton::clicked,this,&LocationsPage::editLocation);
    connect(table,&QTableWidget::itemSelectionChanged,this,&LocationsPage::updateActions);
    connect(search,&QLineEdit::textChanged,this,&LocationsPage::reload);
    connect(filter,&QComboBox::currentIndexChanged,this,&LocationsPage::reload);
    connect(retry,&QPushButton::clicked,this,&LocationsPage::reload);
    reload();
}

void LocationsPage::reload() {
    if (table->currentRow() >= 0 && table->item(table->currentRow(),0)) selectedId=table->item(table->currentRow(),0)->data(Qt::UserRole).toLongLong();
    const int generation=++loadGeneration;
    state->setText("Carregando cadastros…"); state->show(); emptyTitle->hide(); emptyBody->hide(); retry->hide();
    table->setEnabled(false); create->setEnabled(false); remove->setEnabled(false); edit->setEnabled(false);
    QTimer::singleShot(0,this,[this,generation] {
        if (generation != loadGeneration) return;
        try {
            const auto rows=locations.locations(static_cast<CatalogFilter>(filter->currentIndex()),search->text());
            const bool hasAny=!rows.isEmpty() || !locations.locations(CatalogFilter::All).isEmpty();
            table->setRowCount(0); bool createdVisible=false;
            for (const auto &location : rows) {
                const int row=table->rowCount(); table->insertRow(row);
                const QStringList values={location.name,location.deleted ? QStringLiteral("Excluído") : QStringLiteral("Ativo"),QString::number(location.id)};
                for (int column=0; column<values.size(); ++column) {
                    auto *item=new QTableWidgetItem(values[column]); item->setData(Qt::UserRole,location.id); item->setToolTip(values[column]); table->setItem(row,column,item);
                }
                if (location.id == (createdId ? createdId : selectedId)) { table->setCurrentCell(row,0); table->selectRow(row); }
                createdVisible=createdVisible || location.id == createdId;
            }
            summary->setText(rows.size()==1 ? QStringLiteral("1 local") : QString::number(rows.size())+" locais");
            table->setVisible(!rows.isEmpty()); state->setVisible(rows.isEmpty() && hasAny);
            if (rows.isEmpty() && hasAny) state->setText("Nenhum resultado para os filtros atuais. Revise a busca ou limpe os filtros.");
            emptyTitle->setVisible(!hasAny); emptyBody->setVisible(!hasAny);
            if (createdId) {
                feedback->setText(createdVisible ? savedMessage : savedMessage + " O cadastro não aparece no filtro atual.");
                createdId=0;
            }
            table->setEnabled(true);
        } catch (const std::exception &) {
            table->setRowCount(0); table->hide(); summary->clear(); state->show();
            state->setText("Não foi possível carregar os cadastros. Selecione Tentar novamente."); retry->show();
        }
        create->setEnabled(true);
        updateActions();
    });
}

void LocationsPage::updateActions() {
    const int row = table->currentRow();
    remove->setEnabled(table->isEnabled() && row >= 0 && table->item(row,0) && table->item(row,0)->isSelected() && table->item(row,1)->text() == "Ativo");
    edit->setEnabled(remove->isEnabled());
}

void LocationsPage::deleteLocation() {
    if (!remove->isEnabled()) return;
    const auto *item = table->item(table->currentRow(),0);
    const qint64 id = item->data(Qt::UserRole).toLongLong();
    try {
        const int count = locations.notebooksAt(id);
        QString text = QString("Excluir o local %1? Ele deixará de aparecer nas novas seleções. Locais atuais e registros históricos serão preservados.").arg(item->text());
        if (count > 0) text += "\n\n" + (count == 1 ? QStringLiteral("1 notebook ainda está neste local.") : QString::number(count) + " notebooks ainda estão neste local.");
        QMessageBox confirm(QMessageBox::Question,"Excluir local",text,QMessageBox::NoButton,this);
        confirm.setTextFormat(Qt::PlainText);
        auto *cancel = confirm.addButton("Cancelar",QMessageBox::RejectRole);
        auto *accept = confirm.addButton("Excluir local",QMessageBox::DestructiveRole); accept->setObjectName("destructive");
        confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel);
        confirm.exec();
        if (confirm.clickedButton() == accept) {
            remove->setEnabled(false); feedback->setText("Excluindo local…");
            locations.softDelete(session,id);
            feedback->setText("Local excluído."); reload();
            if (onChanged) onChanged();
        }
    } catch (const std::exception &e) {
        feedback->setText(locationSaveError(e));
        const auto *refused = dynamic_cast<const AuthorizationError *>(&e);
        if (refused && refused->reason() == AuthorizationError::Reason::Expired && onExpired) onExpired();
        updateActions();
    }
    remove->setFocus();
}

void LocationsPage::createLocation() {
    saveLocation(std::nullopt);
}

void LocationsPage::editLocation() {
    if (!edit->isEnabled()) return;
    const auto *item = table->item(table->currentRow(),0);
    saveLocation(Location{item->data(Qt::UserRole).toLongLong(),item->text(),false,0});
}

void LocationsPage::saveLocation(const std::optional<Location> &expected) {
    LocationDialog dialog(locations,[this] { return session; },expected,{},this);
    dialog.onExpired=[this] { if (onExpired) onExpired(); };
    if (dialog.exec() == QDialog::Accepted) {
        createdId=dialog.savedLocationId();
        savedMessage=expected ? QStringLiteral("Local atualizado.") : QStringLiteral("Local cadastrado.");
        selectedId=createdId; reload(); feedback->setText(savedMessage);
        if (onChanged) onChanged();
    }
    (expected ? edit : create)->setFocus();
}

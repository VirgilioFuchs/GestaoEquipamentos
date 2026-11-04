#include "PersonPickerDialog.h"
#include "PersonDialog.h"
#include "application/PeopleService.h"
#include <QtWidgets>

PersonPickerDialog::PersonPickerDialog(PeopleService &people, std::function<Session()> currentSession, QWidget *parent,
                                       Mode mode)
    : QDialog(parent), people(people), mode(mode) {
    setObjectName("personPickerDialog");
    setWindowTitle("Selecionar pessoa");
    setMinimumSize(560, 420);
    auto *layout = new QVBoxLayout(this);
    auto *filters = new QHBoxLayout;
    search = new QLineEdit;
    search->setObjectName("personPickerSearch");
    search->setPlaceholderText("Buscar pessoa por ID, nome, função ou matéria");
    search->setClearButtonEnabled(true);
    search->setMaxLength(120);
    search->setAccessibleName(search->placeholderText());
    filters->addWidget(search, 1);
    if (mode == Mode::Query) {
        status = new QComboBox;
        status->setObjectName("personPickerStatus");
        status->setAccessibleName("Situação");
        status->addItem("Todas", int(CatalogFilter::All));
        status->addItem("Ativas", int(CatalogFilter::Active));
        status->addItem("Inativas", int(CatalogFilter::Inactive));
        filters->addWidget(status);
    }
    layout->addLayout(filters);
    const bool query = mode == Mode::Query;
    table = new QTableWidget(0, query ? 5 : 4);
    table->setObjectName("personPickerTable");
    table->setHorizontalHeaderLabels(query ? QStringList{"Nome", "Função", "Matéria", "ID", "Situação"}
                                           : QStringList{"Nome", "Função", "Matéria", "ID"});
    table->verticalHeader()->hide();
    table->horizontalHeader()->moveSection(3, 0);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table, 1);
    empty = new QLabel;
    empty->setObjectName("personPickerEmpty");
    empty->setTextFormat(Qt::PlainText);
    empty->setWordWrap(true);
    layout->addWidget(empty);
    auto *buttons = new QHBoxLayout;
    QPushButton *create = nullptr;
    if (!query) {
        create = new QPushButton("Cadastrar pessoa");
        create->setObjectName("quickRegisterPersonButton");
        buttons->addWidget(create);
    }
    buttons->addStretch();
    auto *cancel = new QPushButton("Cancelar");
    choose = new QPushButton("Selecionar");
    choose->setObjectName("primary");
    choose->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(choose);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(choose, &QPushButton::clicked, this, [this] {
        if (selectedPerson())
            accept();
    });
    connect(table, &QTableWidget::cellDoubleClicked, this, [this] {
        if (selectedPerson())
            accept();
    });
    connect(table, &QTableWidget::itemSelectionChanged, this, [this] {
        choose->setEnabled(selectedPerson().has_value());
    });
    connect(search, &QLineEdit::textChanged, this, &PersonPickerDialog::reload);
    if (status)
        connect(status, &QComboBox::currentIndexChanged, this, &PersonPickerDialog::reload);
    if (create)
        connect(create, &QPushButton::clicked, this, [this, currentSession] {
            try {
                PersonDialog dialog(this->people, currentSession, PersonDialog::Mode::Create, {}, this);
                if (dialog.exec() != QDialog::Accepted || !isVisible())
                    return;
                // A busca pode ocultar a pessoa criada; limpe-a só depois de salvar com sucesso.
                search->clear();
                reload();
                for (int row = 0; row < table->rowCount(); ++row) {
                    if (table->item(row, 0)->data(Qt::UserRole).toLongLong() == dialog.savedPersonId()) {
                        table->setCurrentCell(row, 0);
                        table->selectRow(row);
                        accept();
                        return;
                    }
                }
                empty->setText("A pessoa foi cadastrada, mas não está mais ativa. Atualize a seleção.");
                empty->show();
            } catch (const AuthorizationError &) {
                authorizationError = std::current_exception();
                reject();
            } catch (const std::exception &) {
                empty->setText("Não foi possível carregar os cadastros. Revise a busca para tentar novamente.");
                empty->show();
            }
        });
    reload();
    search->setFocus();
}
int PersonPickerDialog::exec() {
    authorizationError = nullptr;
    const int result = QDialog::exec();
    if (authorizationError)
        std::rethrow_exception(authorizationError);
    return result;
}
void PersonPickerDialog::reload() {
    const auto filter = status ? CatalogFilter(status->currentData().toInt()) : CatalogFilter::Active;
    rows = people.people(filter, search->text().trimmed());
    table->setRowCount(0);
    for (const auto &person : rows) {
        const int r = table->rowCount();
        table->insertRow(r);
        QStringList cells{person.name, roleLabel(person.role),
                          person.subject.isEmpty() ? QStringLiteral("—") : person.subject, QString::number(person.id)};
        if (mode == Mode::Query)
            cells << (person.active ? QStringLiteral("Ativa") : QStringLiteral("Inativa"));
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells[c]);
            item->setData(Qt::UserRole, person.id);
            table->setItem(r, c, item);
        }
    }
    const bool none = rows.isEmpty();
    if (mode == Mode::Query)
        empty->setText(none ? QStringLiteral("Nenhuma pessoa encontrada. Revise a busca ou digite parte do nome no "
                                             "filtro para alcançar registros antigos.")
                            : QString());
    else
        empty->setText(
            !none ? QString()
            : search->text().trimmed().isEmpty()
                ? QStringLiteral("Nenhuma pessoa cadastrada")
                : QStringLiteral("Nenhum resultado para os filtros atuais. Revise a busca ou limpe os filtros."));
    empty->setVisible(none);
    choose->setEnabled(false);
}
std::optional<Person> PersonPickerDialog::selectedPerson() const {
    const int row = table->currentRow();
    if (row < 0 || !table->item(row, 0) || !table->item(row, 0)->isSelected())
        return std::nullopt;
    const auto id = table->item(row, 0)->data(Qt::UserRole).toLongLong();
    for (const auto &person : rows)
        if (person.id == id)
            return person;
    return std::nullopt;
}

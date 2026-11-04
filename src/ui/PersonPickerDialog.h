#pragma once
#include <QDialog>
#include "application/AuthService.h"
#include "domain/Catalog.h"
#include <exception>
#include <functional>
#include <optional>
class PeopleService;
class QComboBox;
class QLineEdit;
class QTableWidget;
class QLabel;
class QPushButton;
// D-01: seleção de pessoa por id (homônimos são linhas distintas; nunca deduplicar).
// Movement lista só ativas e permite cadastro rápido; Query (filtro dos logs) lista também inativas e não cadastra.
class PersonPickerDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Movement, Query };
    PersonPickerDialog(PeopleService &people, std::function<Session()> currentSession, QWidget *parent = nullptr, Mode mode = Mode::Movement);
    int exec() override;
    std::optional<Person> selectedPerson() const;
private:
    PeopleService &people;
    Mode mode;
    QVector<Person> rows;
    QLineEdit *search;
    QComboBox *status = nullptr;
    QTableWidget *table;
    QLabel *empty;
    QPushButton *choose;
    std::exception_ptr authorizationError;
    void reload();
};

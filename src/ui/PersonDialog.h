#pragma once
#include "application/PeopleService.h"
#include <QDialog>
#include <exception>
#include <functional>

// Um único formulário para a página Pessoas e para o cadastro nas movimentações.
class PersonDialog : public QDialog {
public:
    enum class Mode { Create, Edit };
    PersonDialog(PeopleService &people, std::function<Session()> currentSession, Mode mode,
                 const Person &expected = {}, QWidget *parent = nullptr);
    int exec() override;
    qint64 savedPersonId() const { return savedId; }
private:
    qint64 savedId = 0;
    std::exception_ptr authorizationError;
};

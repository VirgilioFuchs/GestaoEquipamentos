#pragma once
#include "application/LocationService.h"
#include <QDialog>
#include <exception>
#include <functional>
#include <optional>

QString locationSaveError(const std::exception &e);

// Um único formulário para a página Locais e para o cadastro nas movimentações.
// Sem onExpired, uma falha de autorização fecha o diálogo e é relançada por exec().
class LocationDialog : public QDialog {
public:
    LocationDialog(LocationService &locations, std::function<Session()> currentSession,
                   const std::optional<Location> &expected = std::nullopt, const QString &initialName = {},
                   QWidget *parent = nullptr);
    int exec() override;
    qint64 savedLocationId() const { return savedId; }
    std::function<void()> onExpired;

private:
    qint64 savedId = 0;
    std::exception_ptr authorizationError;
};

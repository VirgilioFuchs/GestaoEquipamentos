#pragma once
#include <QString>
#include <QDateTime>

enum class Status { Available, Loaned, Maintenance };
enum class ResponsibleRole { Unknown, Professor, Assistant };
inline QString roleLabel(ResponsibleRole role) {
    switch (role) {
    case ResponsibleRole::Professor: return QStringLiteral("Professor");
    case ResponsibleRole::Assistant: return QStringLiteral("Auxiliar");
    default: return QStringLiteral("Não informado");
    }
}
inline QString statusLabel(Status status) {
    switch (status) {
    case Status::Available: return QStringLiteral("Disponível");
    case Status::Loaned: return QStringLiteral("Emprestado");
    case Status::Maintenance: return QStringLiteral("Manutenção");
    }
    return {};
}
struct Notebook {
    qint64 id;
    QString assetTag, name;
    Status status;
    QString borrower;
    QDateTime checkedOutAt, returnedAt;
    QString location, returnedBy;
    QString borrowerSubject, returnedBySubject;
    qint64 locationId = 0;
    bool locationDeleted = false;
    bool active = true;
    ResponsibleRole borrowerRole = ResponsibleRole::Unknown;
    ResponsibleRole returnedByRole = ResponsibleRole::Unknown;
    qint64 borrowerPersonId = 0; // 0 = sem vínculo (texto livre ou legado)
    qint64 returnedByPersonId = 0; // devolução do último empréstimo; 0 = aberto ou legado
    // 03-PD-03: preenchidos só em manutenção, a partir do último evento 'Manutenção'.
    QDateTime maintenanceSince;
    QString maintenanceNote;
};

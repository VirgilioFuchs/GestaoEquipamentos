#include "HistoryFormat.h"
#include "domain/Notebook.h"
#include <QJsonValue>
#include <QStringList>
#include <cmath>
#include <functional>
#include <vector>

namespace {
QString plain(const QJsonValue &value) {
    if (value.isBool())
        return value.toBool() ? QStringLiteral("Sim") : QStringLiteral("Não");
    if (value.isDouble()) {
        const double number = value.toDouble();
        return std::floor(number) == number ? QString::number(value.toInteger()) : QString::number(number);
    }
    return value.toString();
}
bool truthy(const QJsonValue &value) {
    return value.isBool() ? value.toBool() : value.toDouble() != 0;
}
QString yesNo(const QJsonValue &value) {
    return truthy(value) ? QStringLiteral("Sim") : QStringLiteral("Não");
}
QString activeLabel(const QJsonValue &value) {
    return truthy(value) ? QStringLiteral("Ativa") : QStringLiteral("Inativa");
}
QString notebookActive(const QJsonValue &value) {
    return truthy(value) ? QStringLiteral("Ativo") : QStringLiteral("Inativo");
}
QString locationDeleted(const QJsonValue &value) {
    return truthy(value) ? QStringLiteral("Excluído") : QStringLiteral("Ativo");
}
QString personRole(const QJsonValue &value) {
    if (value.toString() == "professor")
        return roleLabel(ResponsibleRole::Professor);
    if (value.toString() == "assistant")
        return roleLabel(ResponsibleRole::Assistant);
    return plain(value);
}
QString accountRole(const QJsonValue &value) {
    const auto role = value.toString();
    if (role == "principal")
        return QStringLiteral("Principal");
    if (role == "admin")
        return QStringLiteral("Administrador");
    if (role == "common")
        return QStringLiteral("Comum");
    return plain(value);
}
QString notebookStatus(const QJsonValue &value) {
    const auto status = value.toString();
    if (status == "available")
        return statusLabel(Status::Available);
    if (status == "loaned")
        return statusLabel(Status::Loaned);
    if (status == "maintenance")
        return statusLabel(Status::Maintenance);
    return plain(value);
}
QString methodLabel(const QJsonValue &value) {
    const auto method = value.toString();
    if (method == "authenticator")
        return QStringLiteral("Código do Authenticator");
    if (method == "backup_code")
        return QStringLiteral("Código de recuperação");
    if (method == "totp")
        return QStringLiteral("Google Authenticator");
    return plain(value);
}
QString localTime(const QJsonValue &value) {
    const auto date = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    return date.isValid() ? date.toLocalTime().toString("dd/MM/yyyy HH:mm:ss") : plain(value);
}
struct Field {
    QString key;
    QString label;
    std::function<QString(const QJsonValue &)> format;
};
const std::vector<Field> &fields() {
    static const std::vector<Field> list{
        {"username", "Nome de usuário", plain},
        {"role", "Perfil", accountRole},
        {"active", "Situação", activeLabel},
        {"must_change_password", "Troca de senha obrigatória", yesNo},
        {"initial_password_mode", "Senha inicial", plain},
        {"admin_slot", "Vaga de administrador", plain},
        {"asset_tag", "Patrimônio", plain},
        {"name", "Nome", plain},
        {"person_role", "Função", personRole},
        {"subject", "Matéria", plain},
        {"maintenance_note", "Observação", plain},
        {"notebook_active", "Situação do cadastro", notebookActive},
        {"location_deleted", "Situação do local", locationDeleted},
        {"location", "Local", plain},
        {"status", "Situação do notebook", notebookStatus},
        {"method", "Método", methodLabel},
        {"failed_attempts", "Tentativas recusadas", plain},
        {"cooldown_until", "Bloqueio até", localTime},
        {"generation", "Geração do vínculo", plain},
        {"totp_enrolled", "Authenticator vinculado", yesNo},
        {"schema_version", "Versão do esquema", plain},
    };
    return list;
}
QString side(const QJsonValue &value, const std::function<QString(const QJsonValue &)> &format) {
    return value.isNull() || value.isUndefined() ? QStringLiteral("—") : format(value);
}
} // namespace

QString describeChanges(const QJsonObject &changes) {
    QStringList parts;
    for (const auto &field : fields()) {
        if (!changes.contains(field.key))
            continue;
        const auto entry = changes.value(field.key).toObject();
        parts << field.label + ": " + side(entry.value("de"), field.format) + " → " +
                     side(entry.value("para"), field.format);
    }
    return parts.join("; ");
}
QString historyDetails(const HistoryEntry &entry) {
    const auto changes = describeChanges(entry.changes);
    if (changes.isEmpty())
        return entry.details;
    // "Local alterado: A → B" repete a alteração "Local: A → B" já descrita em changes.
    if (entry.details.isEmpty() ||
        (entry.details.startsWith(QStringLiteral("Local alterado:")) && entry.changes.contains("location")))
        return changes;
    return entry.details + "; " + changes;
}
QString historyAssetTagLabel(const HistoryEntry &entry) {
    if (!entry.assetTag.isEmpty())
        return entry.assetTag;
    return entry.entityType == "notebook" ? QStringLiteral("Não informado") : QStringLiteral("—");
}
QString historyNotebookNameLabel(const HistoryEntry &entry) {
    if (!entry.notebookName.isEmpty())
        return entry.notebookName;
    return entry.entityType == "notebook" ? QStringLiteral("Não informado") : QStringLiteral("—");
}
QString notebookText(const HistoryEntry &entry) {
    QStringList parts;
    if (!entry.assetTag.isEmpty())
        parts << entry.assetTag;
    if (!entry.notebookName.isEmpty())
        parts << entry.notebookName;
    if (!parts.isEmpty())
        return parts.join(QStringLiteral(" · "));
    return entry.entityType == "notebook" ? QStringLiteral("Não informado") : QStringLiteral("—");
}
QString responsibleText(const HistoryEntry &entry) {
    if (entry.action != "Retirada" && entry.action != QStringLiteral("Devolução"))
        return QStringLiteral("—");
    if (entry.responsibleName.isEmpty())
        return QStringLiteral("Não informado");
    const auto role = personRole(entry.responsibleRole);
    if (role.isEmpty())
        return entry.responsibleName;
    return entry.responsibleName + " (" + role +
           (entry.responsibleSubject.isEmpty() ? QString() : " · " + entry.responsibleSubject) + ")";
}
QString locationText(const HistoryEntry &entry) {
    const auto location = entry.changes.value("location").toObject();
    const auto after = location.value("para").toString(), before = location.value("de").toString();
    if (entry.action == "Retirada")
        return !entry.checkoutLocation.isEmpty() ? entry.checkoutLocation
               : !after.isEmpty()                ? after
                                                 : QStringLiteral("—");
    if (entry.action == QStringLiteral("Devolução"))
        return !entry.returnLocation.isEmpty() ? entry.returnLocation : !after.isEmpty() ? after : QStringLiteral("—");
    if (entry.action == QStringLiteral("Localização") || (entry.action == "Notebook atualizado" && !location.isEmpty()))
        return (before.isEmpty() ? QStringLiteral("—") : before) + " → " +
               (after.isEmpty() ? QStringLiteral("—") : after);
    if (entry.action == "Cadastro")
        return after.isEmpty() ? QStringLiteral("—") : after;
    if (entry.action == "Local cadastrado") {
        const auto name = entry.changes.value("name").toObject().value("para").toString();
        return name.isEmpty() ? QStringLiteral("—") : name;
    }
    return QStringLiteral("—");
}
QString actorText(const HistoryEntry &entry) {
    return entry.actorUsername.isEmpty() ? QStringLiteral("Não informado") : entry.actorUsername;
}

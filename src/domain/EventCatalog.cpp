#include "EventCatalog.h"

const QVector<EventKind> &eventCatalog() {
    static const QVector<EventKind> kinds = {
        {"Retirada", "Retirada", "Movimentações"},
        {"Devolução", "Devolução", "Movimentações"},
        {"Localização", "Atualização de local", "Movimentações"},
        {"Manutenção", "Envio à manutenção", "Movimentações"},
        {"Disponibilização", "Conclusão da manutenção", "Movimentações"},
        {"Cadastro", "Notebook cadastrado", "Notebooks"},
        {"Notebook atualizado", "Notebook atualizado", "Notebooks"},
        {"Notebook inativado", "Notebook inativado", "Notebooks"},
        {"Notebook reativado", "Notebook reativado", "Notebooks"},
        {"Pessoa cadastrada", "Pessoa cadastrada", "Pessoas"},
        {"Pessoa atualizada", "Pessoa atualizada", "Pessoas"},
        {"Pessoa inativada", "Pessoa inativada", "Pessoas"},
        {"Local importado", "Local importado", "Locais"},
        {"Local cadastrado", "Local cadastrado", "Locais"},
        {"Local atualizado", "Local atualizado", "Locais"},
        {"Local excluído", "Local excluído", "Locais"},
        {"Conta principal criada", "Conta principal criada", "Contas"},
        {"Conta criada", "Conta criada", "Contas"},
        {"Senha definida", "Senha definida", "Contas"},
        {"Senha redefinida", "Senha redefinida", "Contas"},
        {"Conta inativada", "Conta inativada", "Contas"},
        {"Administrador criado", "Administrador criado", "Contas"},
        {"Senha de administrador redefinida", "Senha de administrador redefinida", "Contas"},
        {"Administrador inativado", "Administrador inativado", "Contas"},
        {"Recuperação vinculada", "Recuperação vinculada", "Recuperação"},
        {"Recuperação bloqueada temporariamente", "Recuperação bloqueada temporariamente", "Recuperação"},
        {"Tentativa de recuperação recusada", "Tentativa de recuperação recusada", "Recuperação"},
        {"Conta principal recuperada", "Conta principal recuperada", "Recuperação"},
        {"Atualização do banco", "Atualização do banco", "Sistema"}};
    return kinds;
}
QString eventLabel(const QString &action) {
    for (const auto &kind : eventCatalog())
        if (kind.action == action)
            return kind.label;
    return action;
}
QString eventGroup(const QString &action) {
    for (const auto &kind : eventCatalog())
        if (kind.action == action)
            return kind.group;
    return QStringLiteral("Outros registros");
}

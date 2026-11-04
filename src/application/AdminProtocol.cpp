#include "AdminProtocol.h"
#include "security/Crypto.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <optional>

namespace AdminProtocol {
namespace {
struct Response {
    int exitCode;
    QByteArray json;
};
QByteArray reply(const QString &status, const QString &code, const QString &message) {
    return QJsonDocument(QJsonObject{{"protocol", Version}, {"status", status}, {"code", code}, {"message", message}})
        .toJson(QJsonDocument::Compact);
}
Response invalid(const QString &code = QStringLiteral("invalid_request")) {
    const QString message = code == "unsupported_protocol" ? QStringLiteral("Versão de protocolo não suportada.")
                                                           : QStringLiteral("Requisição inválida.");
    return {2, reply("error", code, message)};
}
Response internalError() {
    return {1,
            reply("error", "internal_error", QStringLiteral("Não foi possível concluir a operação no banco local."))};
}
std::optional<QJsonObject> objectOf(QByteArray &line) {
    std::optional<QJsonObject> result;
    if (line.size() <= MaxRequestBytes) {
        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject())
            result = document.object();
    }
    crypto::wipe(line);
    return result;
}
bool exactStringFields(const QJsonObject &object, const QSet<QString> &fields) {
    const QStringList keys = object.keys();
    if (QSet<QString>(keys.begin(), keys.end()) != fields)
        return false;
    for (const auto &field : fields)
        if (field != "protocol" && !object.value(field).isString())
            return false;
    return true;
}
std::optional<RecoveryMethod> methodOf(const QString &text) {
    if (text == "authenticator")
        return RecoveryMethod::Authenticator;
    if (text == "backup_code")
        return RecoveryMethod::BackupCode;
    return std::nullopt;
}

Response bootstrap(const QJsonObject &object, AuthService &auth) {
    QByteArray password = object.value("password").toString().toUtf8();
    try {
        const auto prepared = auth.preparePassword(password);
        crypto::wipe(password);
        auth.bootstrapPrincipal(object.value("username").toString(), prepared);
        return {0, reply("ok", "ok", QStringLiteral("Conta principal configurada."))};
    } catch (const AuthError &error) {
        crypto::wipe(password);
        return {1, reply("error", error.code(), QString::fromUtf8(error.what()))};
    } catch (const std::exception &) {
        crypto::wipe(password);
        return internalError();
    }
}

std::unique_ptr<VerifiedRecovery> verify(const QJsonObject &object, RecoveryMethod method, RecoveryService &recovery,
                                         Response &refused) {
    QByteArray proof = object.value("proof").toString().toUtf8();
    std::unique_ptr<VerifiedRecovery> verified;
    try {
        verified = recovery.verifyRecovery(object.value("username").toString(), method, QString::fromUtf8(proof));
    } catch (const AuthError &error) {
        refused = {1, reply("error", error.code(), QString::fromUtf8(error.what()))};
    } catch (const std::exception &) {
        refused = internalError();
    }
    crypto::wipe(proof);
    return verified;
}

Response commit(const QJsonObject &object, RecoveryMethod method, const VerifiedRecovery &verified, AuthService &auth,
                RecoveryService &recovery) {
    QByteArray password = object.value("password").toString().toUtf8();
    try {
        const auto prepared = auth.preparePassword(password);
        crypto::wipe(password);
        recovery.completeRecovery(verified, prepared);
        QString message = QStringLiteral("Senha redefinida. Entre com a nova senha.");
        if (method == RecoveryMethod::BackupCode)
            message += QStringLiteral(" Vincule o Google Authenticator novamente no aplicativo.");
        return {0, reply("ok", "ok", message)};
    } catch (const AuthError &error) {
        crypto::wipe(password);
        return {1, reply("error", error.code(), QString::fromUtf8(error.what()))};
    } catch (const std::exception &) {
        crypto::wipe(password);
        return internalError();
    }
}

Response recover(const QJsonObject &object, RecoveryMethod method, const ReadLine &read, const WriteLine &write,
                 AuthService &auth, RecoveryService &recovery) {
    Response refused{};
    const auto verified = verify(object, method, recovery, refused);
    if (!verified)
        return refused;
    write(reply("ok", "verified", QStringLiteral("Código conferido.")));
    auto line = read(CommitTimeoutMs);
    if (!line)
        return {1, reply("error", "cancelled",
                         QStringLiteral("A nova senha não foi recebida a tempo. Nada foi alterado."))};
    const auto second = objectOf(*line);
    if (!second || second->value("action").toString() != "recover_commit" ||
        !exactStringFields(*second, {"action", "password"}))
        return invalid();
    return commit(*second, method, *verified, auth, recovery);
}

Response dispatch(const ReadLine &read, const WriteLine &write, AuthService &auth, RecoveryService &recovery) {
    auto line = read(CommitTimeoutMs);
    if (!line)
        return invalid();
    const auto object = objectOf(*line);
    if (!object || !object->value("protocol").isDouble())
        return invalid();
    if (object->value("protocol").toDouble() != Version)
        return invalid("unsupported_protocol");
    const QString action = object->value("action").toString();
    if (action == "bootstrap" && exactStringFields(*object, {"protocol", "action", "username", "password"}))
        return bootstrap(*object, auth);
    if (action == "recover_verify" &&
        exactStringFields(*object, {"protocol", "action", "username", "method", "proof"})) {
        const auto method = methodOf(object->value("method").toString());
        if (!method)
            return invalid();
        return recover(*object, *method, read, write, auth, recovery);
    }
    return invalid();
}
} // namespace

int serve(const ReadLine &read, const WriteLine &write, AuthService &auth, RecoveryService &recovery) {
    const Response response = dispatch(read, write, auth, recovery);
    write(response.json);
    return response.exitCode;
}
} // namespace AdminProtocol

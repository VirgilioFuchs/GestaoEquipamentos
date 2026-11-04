#include "SecretVault.h"
#include <QEventLoop>
#include <QStandardPaths>
#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#else
#include <qtkeychain/keychain.h>
#endif

namespace {
// O backend pode concluir dentro de start(); o laço local só roda se o job ainda estiver pendente.
void runJob(QKeychain::Job &job) {
    job.setAutoDelete(false);
    job.setInsecureFallback(false);
    bool finished = false;
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, [&] {
        finished = true;
        loop.quit();
    });
    job.start();
    if (!finished)
        loop.exec();
    if (job.error() != QKeychain::NoError)
        throw VaultError("Cofre do sistema: " + job.errorString());
}
} // namespace

KeychainSecretVault::KeychainSecretVault(QString service) : service(std::move(service)) {}

void KeychainSecretVault::write(const QString &key, const QByteArray &secret) {
    QKeychain::WritePasswordJob job(service);
    job.setKey(key);
    job.setBinaryData(secret);
    runJob(job);
}

QByteArray KeychainSecretVault::read(const QString &key) {
    QKeychain::ReadPasswordJob job(service);
    job.setKey(key);
    runJob(job);
    return job.binaryData();
}

void KeychainSecretVault::remove(const QString &key) {
    QKeychain::DeletePasswordJob job(service);
    job.setKey(key);
    runJob(job);
}

QString vaultServiceName(DataProfile profile) {
    if (QStandardPaths::isTestModeEnabled())
        return QStringLiteral("GestaoEquipamentos-test");
    return profile == DataProfile::Demo ? QStringLiteral("GestaoEquipamentos-demo")
                                        : QStringLiteral("GestaoEquipamentos");
}

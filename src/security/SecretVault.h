#pragma once
#include "storage/LocalDatabase.h"
#include <QByteArray>
#include <QString>
#include <stdexcept>

// Mensagem técnica do backend; nunca contém o segredo.
class VaultError : public std::runtime_error {
public:
    explicit VaultError(const QString &message) : std::runtime_error(message.toStdString()) {}
};

// Operações síncronas na thread da interface; falhas (inclusive entrada inexistente na leitura) lançam VaultError.
class SecretVault {
public:
    virtual ~SecretVault() = default;
    virtual void write(const QString &key, const QByteArray &secret) = 0;
    virtual QByteArray read(const QString &key) = 0;
    virtual void remove(const QString &key) = 0;
};

// Cofre do usuário do SO (Credential Manager, Keychain, Secret Service) sem armazenamento em texto como alternativa.
class KeychainSecretVault : public SecretVault {
public:
    explicit KeychainSecretVault(QString service);
    void write(const QString &key, const QByteArray &secret) override;
    QByteArray read(const QString &key) override;
    void remove(const QString &key) override;
private:
    QString service;
};

// "GestaoEquipamentos-test" quando QStandardPaths está em modo de teste (--test-profile).
QString vaultServiceName(DataProfile profile);

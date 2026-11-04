#include <QtTest>
#include <QStandardPaths>
#include <QUuid>
#include "security/SecretVault.h"

// Cofre real do usuário do SO, sempre no serviço GestaoEquipamentos-test e com segredos fictícios.
// Sem backend disponível o teste é pulado, exceto com GESTAOEQUIPAMENTOS_REQUIRE_VAULT=1.
class VaultTests : public QObject {
    Q_OBJECT
private slots:
    void serviceNames() {
        QStandardPaths::setTestModeEnabled(false);
        QCOMPARE(vaultServiceName(DataProfile::Operational), QString("GestaoEquipamentos"));
        QCOMPARE(vaultServiceName(DataProfile::Demo), QString("GestaoEquipamentos-demo"));
        QStandardPaths::setTestModeEnabled(true);
        QCOMPARE(vaultServiceName(DataProfile::Operational), QString("GestaoEquipamentos-test"));
        QCOMPARE(vaultServiceName(DataProfile::Demo), QString("GestaoEquipamentos-test"));
    }
    void realVaultRoundTrip() {
        QStandardPaths::setTestModeEnabled(true);
        KeychainSecretVault vault(vaultServiceName(DataProfile::Operational));
        const QString prefix = "vault-test/" + QUuid::createUuid().toString(QUuid::Id128) + "/";
        const QString first = prefix + "1", second = prefix + "2";
        const QByteArray secretA = QByteArray::fromHex("00ff102030405060708090a0b0c0d0e0f0011223");
        const QByteArray secretB = QByteArray::fromHex("ffeeddccbbaa99887766554433221100a5a55a5a");
        const QByteArray secretC = QByteArray::fromHex("0102030405060708090a0b0c0d0e0f1011121314");
        try {
            vault.write(first, secretA);
        } catch (const VaultError &e) {
            const QString reason = QString("Cofre do sistema indisponível: ") + e.what();
            if (qEnvironmentVariable("GESTAOEQUIPAMENTOS_REQUIRE_VAULT") == "1") QFAIL(qPrintable(reason));
            QSKIP(qPrintable(reason));
        }
        struct Cleanup {
            KeychainSecretVault &vault; QStringList keys;
            ~Cleanup() { for (const auto &key : keys) try { vault.remove(key); } catch (const VaultError &) {} }
        } cleanup{vault, {first, second}};
        QCOMPARE(vault.read(first), secretA);
        vault.write(second, secretB);
        QCOMPARE(vault.read(second), secretB);
        QCOMPARE(vault.read(first), secretA);
        vault.write(first, secretC);
        QCOMPARE(vault.read(first), secretC);
        QCOMPARE(vault.read(second), secretB);
        vault.remove(first);
        vault.remove(second);
        QVERIFY_EXCEPTION_THROWN(vault.read(first), VaultError);
        QVERIFY_EXCEPTION_THROWN(vault.read(second), VaultError);
    }
};

QTEST_GUILESS_MAIN(VaultTests)
#include "VaultTests.moc"

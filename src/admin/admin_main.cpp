#include "application/AdminProtocol.h"
#include "security/Crypto.h"
#include "security/SecretVault.h"
#include "storage/LocalDatabase.h"
#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
int usage() {
    std::fputs("Uso: GestaoEquipamentosAdmin [--version] [--test-profile]\n"
               "As requisições JSON são lidas da entrada padrão, uma por linha. Nenhum caminho de banco é aceito.\n",
               stderr);
    return 2;
}
void writeOut(const QByteArray &bytes) {
    QFile out;
    out.open(stdout, QIODevice::WriteOnly);
    out.write(bytes);
    out.write("\n");
    out.flush();
}

// A leitura bloqueante fica numa thread própria para que cada espera tenha prazo. Usa o descritor bruto
// (sem FILE*), então a thread presa na leitura não segura travas da CRT quando o processo termina.
class StdinLines {
public:
    StdinLines() : state(std::make_shared<State>()) {
        std::thread([shared = state] {
            pump(*shared);
        }).detach();
    }
    std::optional<QByteArray> next(int timeoutMs) {
        State &s = *state;
        std::unique_lock<std::mutex> lock(s.mutex);
        s.changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return s.ended || s.buffer.contains('\n') || s.buffer.size() > AdminProtocol::MaxRequestBytes;
        });
        const qsizetype newline = s.buffer.indexOf('\n');
        if (newline >= 0)
            return take(s.buffer, newline, newline + 1);
        if (s.buffer.size() > AdminProtocol::MaxRequestBytes || (s.ended && !s.buffer.isEmpty()))
            return take(s.buffer, s.buffer.size(), s.buffer.size());
        return std::nullopt;
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        QByteArray buffer;
        bool ended = false;
    };
    static QByteArray take(QByteArray &buffer, qsizetype length, qsizetype consumed) {
        QByteArray line = buffer.left(length);
        if (line.endsWith('\r'))
            line.chop(1);
        std::fill(buffer.begin(), buffer.begin() + consumed, '\0');
        buffer.remove(0, consumed);
        return line;
    }
    static void pump(State &s) {
        QByteArray chunk(4096, '\0');
        for (;;) {
#ifdef Q_OS_WIN
            DWORD count = 0;
            const bool ok =
                ReadFile(GetStdHandle(STD_INPUT_HANDLE), chunk.data(), DWORD(chunk.size()), &count, nullptr) &&
                count > 0;
#else
            const auto count = ::read(STDIN_FILENO, chunk.data(), size_t(chunk.size()));
            const bool ok = count > 0;
#endif
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!ok) {
                crypto::wipe(chunk);
                s.ended = true;
                s.changed.notify_all();
                return;
            }
            s.buffer.append(chunk.constData(), qsizetype(count));
            s.changed.notify_all();
        }
    }
    std::shared_ptr<State> state;
};
} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    LocalDatabase::applyApplicationIdentity();
    bool testProfile = false;
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--version") {
            writeOut("GestaoEquipamentosAdmin protocolo " + QByteArray::number(AdminProtocol::Version));
            return 0;
        }
        if (args[i] == "--test-profile")
            testProfile = true;
        else
            return usage();
    }
    if (testProfile)
        QStandardPaths::setTestModeEnabled(true);
    try {
        crypto::initialize();
        StdinLines lines;
        LocalDatabase database(LocalDatabase::profilePath(DataProfile::Operational));
        PasswordHasher hasher;
        AuthService auth(database, hasher);
        KeychainSecretVault vault(vaultServiceName(DataProfile::Operational));
        RecoveryService recovery(database, auth, vault, SessionClock::system());
        return AdminProtocol::serve(
            [&](int timeoutMs) {
                return lines.next(timeoutMs);
            },
            writeOut, auth, recovery);
    } catch (const std::exception &) {
        writeOut(
            R"({"protocol":1,"status":"error","code":"internal_error","message":"Não foi possível abrir o banco local."})");
        return 1;
    }
}

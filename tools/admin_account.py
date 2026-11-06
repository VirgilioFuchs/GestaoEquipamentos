#!/usr/bin/env python3
"""Utilitário local do GestaoEquipamentos para configurar e recuperar a conta principal.

Usa somente a biblioteca padrão. Senha e código vão ao GestaoEquipamentosAdmin apenas
pela entrada padrão (JSON); nunca por argumentos, ambiente ou arquivos.
"""
import argparse
import getpass
import json
import queue
import subprocess
import sys
import threading
import warnings
from pathlib import Path

PROTOCOL = 1
POLICY = "A senha deve ter de 8 a 128 caracteres. Frases longas são recomendadas."
HELPER_NOT_FOUND = "Utilitário GestaoEquipamentosAdmin não encontrado. Compile o alvo GestaoEquipamentosAdmin ou informe --helper com o caminho do executável."
NO_HIDDEN_INPUT = "Não foi possível ocultar a senha neste terminal. Execute em um terminal interativo."
MISMATCH = "As senhas não coincidem. Confira a confirmação."
HELPER_NAMES = ("GestaoEquipamentosAdmin.exe", "GestaoEquipamentosAdmin")
METHOD_PROMPT = "Método (1 = Código do Authenticator, 2 = Código de recuperação): "
METHODS = {"1": "authenticator", "2": "backup_code"}
INVALID_METHOD = "Opção inválida. Digite 1 ou 2."
NO_REPLY = "O GestaoEquipamentosAdmin não respondeu a tempo. Nada foi confirmado."
BAD_REPLY = "Resposta inválida do GestaoEquipamentosAdmin."
REPLY_TIMEOUT = 120
# Códigos de saída do Windows quando o executável nem chega a iniciar.
WINDOWS_LOADER_ERRORS = {
    0xC0000135: "faltam DLLs (Qt, MinGW ou libsodium) ao lado do executável",
    0xC0000139: "uma DLL encontrada é de outra versão (ponto de entrada ausente)",
    0xC000007B: "mistura de DLLs 32/64 bits ou de compiladores diferentes (MinGW x MSVC)",
}
MISSING_DLLS_HINT = ("Recompile com o CMake atualizado (ele copia as DLLs para a pasta de build) "
                     "ou rode windeployqt no executável.")


def loader_error(returncode):
    """Mensagem para falhas do carregador do Windows, ou None."""
    reason = WINDOWS_LOADER_ERRORS.get(returncode & 0xFFFFFFFF) if returncode else None
    if reason:
        return f"O GestaoEquipamentosAdmin não iniciou: {reason}. {MISSING_DLLS_HINT}"
    return None


class ScriptError(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.message = message
        self.code = code


def _helper_candidates_from(root):
    roots = [root]
    if root.name == "tools":
        roots.append(root.parent / "build")
    else:
        roots.append(root / "build")
    for base in roots:
        if not base.exists():
            continue
        # Caminhos diretos dos presets comuns.
        for sub in (base, base / "release", base / "dev"):
            for name in HELPER_NAMES:
                candidate = sub / name
                if candidate.is_file():
                    yield candidate
        # Pastas geradas pelo Qt Creator (ex.: build/Desktop_Qt_...-Release/)
        for name in HELPER_NAMES:
            for candidate in base.rglob(name):
                if candidate.is_file():
                    yield candidate


def _pick_newest(paths):
    unique = {}
    for path in paths:
        unique[path.resolve()] = path
    if not unique:
        return None
    return max(unique.values(), key=lambda p: p.stat().st_mtime)


def find_helper(explicit):
    if explicit:
        candidate = Path(explicit)
        if candidate.is_file():
            return candidate
        raise ScriptError(HELPER_NOT_FOUND, 2)

    here = Path(__file__).resolve().parent
    cwd = Path.cwd()
    candidates = list(_helper_candidates_from(here)) + list(_helper_candidates_from(cwd))
    picked = _pick_newest(candidates)
    if picked:
        return picked
    raise ScriptError(HELPER_NOT_FOUND, 2)


def validate_helper(helper, test_profile):
    command = [str(helper), "--version"]
    if test_profile:
        command.append("--test-profile")
    try:
        result = subprocess.run(command, capture_output=True, shell=False, timeout=10)
    except (subprocess.SubprocessError, OSError):
        raise ScriptError("Não foi possível executar o GestaoEquipamentosAdmin informado.", 2) from None

    message = loader_error(result.returncode)
    if message:
        raise ScriptError(message, 2)
    out = result.stdout.decode("utf-8", errors="replace").strip()
    err = result.stderr.decode("utf-8", errors="replace").strip()
    if result.returncode != 0 or "protocolo" not in out.lower():
        hint = ""
        if out:
            hint += f"\nstdout: {out[:240]}"
        if err:
            hint += f"\nstderr: {err[:240]}"
        raise ScriptError("O executável informado não é um GestaoEquipamentosAdmin válido." + hint, 2)


def ask_secret(prompt):
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        try:
            return getpass.getpass(prompt)
        except getpass.GetPassWarning:
            raise ScriptError(NO_HIDDEN_INPUT, 2) from None


def ask_new_password(prompt="Senha: ", confirm="Confirmar senha: "):
    print(POLICY)
    for _ in range(3):
        first = ask_secret(prompt)
        second = ask_secret(confirm)
        if first == second:
            return first
        print(MISMATCH)
    raise ScriptError("A confirmação da senha falhou três vezes. Nada foi alterado.", 1)


def call_helper(helper, test_profile, request):
    command = [str(helper)]
    if test_profile:
        command.append("--test-profile")
    result = subprocess.run(command, input=json.dumps(request).encode("utf-8"),
                            capture_output=True, shell=False, timeout=120)
    message = loader_error(result.returncode)
    if message:
        raise ScriptError(message, 2)
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace")
    try:
        reply = json.loads(stdout)
    except ValueError:
        details = []
        if stdout.strip():
            details.append(f"stdout: {stdout.strip()[:240]}")
        if stderr.strip():
            details.append(f"stderr: {stderr.strip()[:240]}")
        if not details:
            details.append("o processo não retornou JSON")
        raise ScriptError(BAD_REPLY + " (" + " | ".join(details) + ")", result.returncode or 1) from None
    return result.returncode, reply


class HelperSession:
    """Um único GestaoEquipamentosAdmin; cada requisição é uma linha JSON e a resposta é lida antes da próxima."""

    def __init__(self, helper, test_profile):
        command = [str(helper)]
        if test_profile:
            command.append("--test-profile")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, shell=False)
        self.replies = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in iter(self.process.stdout.readline, b""):
            self.replies.put(line)
        self.replies.put(None)

    def exchange(self, request):
        try:
            self.process.stdin.write(json.dumps(request).encode("utf-8") + b"\n")
            self.process.stdin.flush()
        except OSError:
            pass  # O helper já encerrou (por exemplo, prazo esgotado); a resposta pendente explica.
        try:
            line = self.replies.get(timeout=REPLY_TIMEOUT)
        except queue.Empty:
            raise ScriptError(NO_REPLY, 1) from None
        if line is None:
            try:
                returncode = self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                returncode = None
            raise ScriptError(loader_error(returncode) or BAD_REPLY, 1)
        try:
            return json.loads(line.decode("utf-8"))
        except ValueError:
            raise ScriptError(BAD_REPLY, 1) from None

    def close(self):
        """Fecha a entrada (o helper encerra sem alterar nada se ainda esperava) e devolve o código de saída."""
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        return self.process.returncode


def bootstrap(args):
    helper = find_helper(args.helper)
    if not args.helper:
        validate_helper(helper, args.test_profile)
    username = input("Nome de usuário da conta principal: ").strip()
    password = ask_new_password()
    request = {"protocol": PROTOCOL, "action": "bootstrap", "username": username, "password": password}
    code, reply = call_helper(helper, args.test_profile, request)
    print(reply.get("message", ""))
    if code == 0:
        print("Abra o GestaoEquipamentos e entre com a conta principal.")
    return code


def ask_method():
    for _ in range(3):
        choice = input(METHOD_PROMPT).strip()
        if choice in METHODS:
            return METHODS[choice]
        print(INVALID_METHOD)
    raise ScriptError("Nenhum método válido foi escolhido. Nada foi alterado.", 1)


def recover(args):
    helper = find_helper(args.helper)
    if not args.helper:
        validate_helper(helper, args.test_profile)
    username = input("Nome de usuário da conta principal: ").strip()
    method = ask_method()
    proof = ask_secret("Código: ")
    session = HelperSession(helper, args.test_profile)
    try:
        reply = session.exchange({"protocol": PROTOCOL, "action": "recover_verify", "username": username,
                                  "method": method, "proof": proof})
        del proof
        if reply.get("status") == "ok":
            password = ask_new_password("Nova senha: ", "Confirmar nova senha: ")
            reply = session.exchange({"action": "recover_commit", "password": password})
            del password
        print(reply.get("message", ""))
    finally:
        code = session.close()
    return code


def build_parser():
    parser = argparse.ArgumentParser(description="Administração local do GestaoEquipamentos.")
    commands = parser.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("bootstrap", help="Configura a conta principal (somente na primeira vez).")
    restore = commands.add_parser("recover", help="Redefine a senha da conta principal com o código do Authenticator "
                                                  "ou um código de recuperação.")
    for sub in (setup, restore):
        sub.add_argument("--helper", help="Caminho do executável GestaoEquipamentosAdmin.")
        sub.add_argument("--test-profile", action="store_true", help="Usa o perfil de teste do Qt.")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return recover(args) if args.command == "recover" else bootstrap(args)
    except ScriptError as error:
        print(error.message, file=sys.stderr)
        return error.code
    except subprocess.TimeoutExpired:
        print("O GestaoEquipamentosAdmin não respondeu a tempo. Nada foi confirmado.", file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print("\nOperação cancelada.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())

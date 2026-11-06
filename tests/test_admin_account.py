import contextlib
import getpass
import importlib.util
import io
import json
import queue
import subprocess
import tempfile
import unittest
import warnings
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parent.parent / "tools" / "admin_account.py"
spec = importlib.util.spec_from_file_location("admin_account", SCRIPT)
admin_account = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin_account)

PASSWORD = "frase secreta bem comprida 7"
PROOF = "ABCD-EFGH-JKMN-PQRS"
INVALID_CODE = "Código inválido, expirado ou já utilizado. Confira o código ou escolha outro método."
OK_REPLY = json.dumps({"protocol": 1, "status": "ok", "code": "ok", "message": "Conta principal configurada."}).encode()
VERIFIED = json.dumps({"protocol": 1, "status": "ok", "code": "verified", "message": "Código conferido."}).encode()
RECOVERED = json.dumps({"protocol": 1, "status": "ok", "code": "ok", "message": "Senha redefinida. Entre com a nova senha."}).encode()
REFUSED = json.dumps({"protocol": 1, "status": "error", "code": "invalid_code", "message": INVALID_CODE}).encode()


class FakeHelper:
    """GestaoEquipamentosAdmin simulado no lugar de Popen: responde a cada linha recebida com a próxima resposta."""

    def __init__(self, replies, code=0):
        self.replies, self.code = list(replies), code
        self.command = self.kwargs = None
        self.sent, self.events = [], []
        self.stdin_closed = False
        self.returncode = None
        self.lines = queue.Queue()
        helper = self

        class Stdin:
            def write(self, data):
                request = json.loads(data.decode("utf-8"))
                helper.sent.append(request)
                helper.events.append("-> " + request["action"])
                helper.lines.put(helper.replies.pop(0) + b"\n" if helper.replies else b"")

            def flush(self):
                pass

            def close(self):
                helper.stdin_closed = True
                helper.lines.put(b"")

        class Stdout:
            def readline(self):
                return helper.lines.get(timeout=5)

        self.stdin, self.stdout = Stdin(), Stdout()

    def __call__(self, command, **kwargs):
        self.command, self.kwargs = command, kwargs
        return self

    def wait(self, timeout=None):
        self.returncode = self.code
        return self.code

    def kill(self):
        pass


class AdminAccountTests(unittest.TestCase):
    def setUp(self):
        handle = tempfile.NamedTemporaryFile(delete=False, suffix=".exe")
        handle.close()
        self.helper = handle.name
        self.addCleanup(Path(self.helper).unlink)

    def run_main(self, argv, secrets, reply=OK_REPLY, code=0, answers=None, session=None):
        prompts = []
        events = session.events if session is not None else []
        def fake_getpass(prompt):
            prompts.append(prompt)
            events.append(prompt)
            return secrets.pop(0)
        def fake_input(prompt):
            prompts.append(prompt)
            events.append(prompt)
            return answers.pop(0) if answers else "Principal"
        completed = subprocess.CompletedProcess([], code, stdout=reply, stderr=b"")
        popen = session if session is not None else mock.Mock(side_effect=AssertionError("Popen inesperado"))
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(admin_account.getpass, "getpass", side_effect=fake_getpass), \
             mock.patch("builtins.input", side_effect=fake_input), \
             mock.patch.object(admin_account.subprocess, "run", return_value=completed) as run, \
             mock.patch.object(admin_account.subprocess, "Popen", popen), \
             contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            result = admin_account.main(argv)
        return result, run, prompts, out.getvalue(), err.getvalue()

    def test_password_only_in_stdin(self):
        result, run, _, _, _ = self.run_main(["bootstrap", "--helper", self.helper, "--test-profile"], [PASSWORD, PASSWORD])
        self.assertEqual(result, 0)
        args, kwargs = run.call_args
        self.assertFalse(any(PASSWORD in str(part) for part in args[0]))
        self.assertIn("--test-profile", args[0])
        self.assertIs(kwargs["shell"], False)
        self.assertNotIn("env", kwargs)
        self.assertEqual(json.loads(kwargs["input"]), {"protocol": 1, "action": "bootstrap", "username": "Principal", "password": PASSWORD})

    def test_prompts_never_mention_database(self):
        _, _, prompts, out, _ = self.run_main(["bootstrap", "--helper", self.helper], [PASSWORD, PASSWORD])
        text = " ".join(prompts).lower() + out.lower()
        for word in ("banco", "caminho", "database", ".sqlite"):
            self.assertNotIn(word, text)
        self.assertIn("Abra o GestaoEquipamentos e entre com a conta principal.", out)
        self.assertNotIn(PASSWORD, out)

    def test_db_option_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            admin_account.main(["bootstrap", "--db", "outro.sqlite"])
        self.assertEqual(raised.exception.code, 2)

    def test_missing_helper(self):
        result, run, _, _, err = self.run_main(["bootstrap", "--helper", self.helper + ".ausente"], [])
        self.assertEqual(result, 2)
        self.assertIn(admin_account.HELPER_NOT_FOUND, err)
        run.assert_not_called()

    def test_mismatch_retries_three_times(self):
        result, run, _, out, _ = self.run_main(["bootstrap", "--helper", self.helper], ["a" * 15, "b" * 15] * 3)
        self.assertEqual(result, 1)
        self.assertEqual(out.count(admin_account.MISMATCH), 3)
        run.assert_not_called()
        result, run, _, out, _ = self.run_main(["bootstrap", "--helper", self.helper], ["a" * 15, "b" * 15, PASSWORD, PASSWORD])
        self.assertEqual(result, 0)
        run.assert_called_once()

    def test_unhidden_terminal_aborts(self):
        def warning_getpass(prompt):
            warnings.warn("Can not control echo on the terminal.", getpass.GetPassWarning)
            return PASSWORD
        err = io.StringIO()
        with mock.patch.object(admin_account.getpass, "getpass", side_effect=warning_getpass), \
             mock.patch("builtins.input", return_value="principal"), \
             mock.patch.object(admin_account.subprocess, "run") as run, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            result = admin_account.main(["bootstrap", "--helper", self.helper])
        self.assertEqual(result, 2)
        self.assertIn(admin_account.NO_HIDDEN_INPUT, err.getvalue())
        run.assert_not_called()

    def test_helper_error_exit_code_propagates(self):
        reply = json.dumps({"protocol": 1, "status": "error", "code": "already_initialized",
                            "message": "A conta principal já foi configurada. Use a recuperação autenticada."}).encode()
        result, _, _, out, _ = self.run_main(["bootstrap", "--helper", self.helper], [PASSWORD, PASSWORD], reply, 1)
        self.assertEqual(result, 1)
        self.assertIn("A conta principal já foi configurada.", out)
        self.assertNotIn("Abra o GestaoEquipamentos", out)

    def test_recover_verifies_before_asking_password(self):
        for choice, method in (("1", "authenticator"), ("2", "backup_code")):
            session = FakeHelper([VERIFIED, RECOVERED])
            result, run, prompts, out, _ = self.run_main(["recover", "--helper", self.helper, "--test-profile"],
                                                         [PROOF, PASSWORD, PASSWORD], answers=["Principal", choice],
                                                         session=session)
            self.assertEqual(result, 0)
            run.assert_not_called()
            self.assertFalse(any(PASSWORD in str(part) or PROOF in str(part) for part in session.command))
            self.assertIn("--test-profile", session.command)
            self.assertIs(session.kwargs["shell"], False)
            self.assertNotIn("env", session.kwargs)
            self.assertNotIn("input", session.kwargs)
            self.assertEqual(session.sent, [
                {"protocol": 1, "action": "recover_verify", "username": "Principal", "method": method, "proof": PROOF},
                {"action": "recover_commit", "password": PASSWORD}])
            self.assertEqual(session.events, ["Nome de usuário da conta principal: ", admin_account.METHOD_PROMPT, "Código: ",
                                              "-> recover_verify", "Nova senha: ", "Confirmar nova senha: ", "-> recover_commit"])
            self.assertTrue(session.stdin_closed)
            self.assertIn("Senha redefinida. Entre com a nova senha.", out)
            text = " ".join(prompts).lower() + out.lower()
            for word in ("banco", "caminho", "database", ".sqlite"):
                self.assertNotIn(word, text)
            self.assertNotIn(PROOF, out)
            self.assertNotIn(PASSWORD, out)

    def test_recover_invalid_code_never_asks_password(self):
        session = FakeHelper([REFUSED], code=1)
        result, _, prompts, out, _ = self.run_main(["recover", "--helper", self.helper], [PROOF],
                                                   answers=["Principal", "2"], session=session)
        self.assertEqual(result, 1)
        self.assertEqual(prompts, ["Nome de usuário da conta principal: ", admin_account.METHOD_PROMPT, "Código: "])
        self.assertEqual([request["action"] for request in session.sent], ["recover_verify"])
        self.assertEqual(out.strip(), INVALID_CODE)
        self.assertNotIn(admin_account.POLICY, out)
        self.assertTrue(session.stdin_closed)

    def test_recover_mismatch_sends_no_commit(self):
        session = FakeHelper([VERIFIED])
        result, _, _, out, err = self.run_main(["recover", "--helper", self.helper], [PROOF] + ["a" * 15, "b" * 15] * 3,
                                               answers=["Principal", "2"], session=session)
        self.assertEqual(result, 1)
        self.assertEqual(out.count(admin_account.MISMATCH), 3)
        self.assertIn("Nada foi alterado.", err)
        self.assertEqual([request["action"] for request in session.sent], ["recover_verify"])
        self.assertTrue(session.stdin_closed)

    def test_recover_invalid_method_repeats(self):
        session = FakeHelper([VERIFIED, RECOVERED])
        result, _, _, out, _ = self.run_main(["recover", "--helper", self.helper], [PROOF, PASSWORD, PASSWORD],
                                             answers=["Principal", "3", "", "2"], session=session)
        self.assertEqual(result, 0)
        self.assertEqual(out.count(admin_account.INVALID_METHOD), 2)
        self.assertEqual(session.sent[0]["method"], "backup_code")
        session = FakeHelper([])
        result, _, _, out, _ = self.run_main(["recover", "--helper", self.helper], [],
                                             answers=["Principal", "x", "y", "z"], session=session)
        self.assertEqual(result, 1)
        self.assertIsNone(session.command)

    def test_recover_rejects_secret_options(self):
        for option in (["--db", "outro.sqlite"], ["--proof", PROOF], ["--password", PASSWORD]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                admin_account.main(["recover", *option])
            self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()

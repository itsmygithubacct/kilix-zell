import contextlib
import importlib.util
import io
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import zell_secrets as vault

IDENTITY = '0123456789abcdef'*2
SYNTHETIC = b'zell-test-only-key-778899'


class VaultTests(unittest.TestCase):
    def test_record_identity_and_metadata_boundary(self):
        for value in ('../record', '', 'g'*32, IDENTITY+'x'):
            with self.assertRaises(ValueError):
                vault.record_id(value)
        with patch.object(vault.subprocess, 'run') as run:
            for operation in ('run', 'copy', 'add', 'unlock'):
                with self.assertRaises(ValueError):
                    vault.metadata(operation)
            run.assert_not_called()
        with patch.object(vault, 'command', return_value=['vault']), \
             patch.object(vault.subprocess, 'run', return_value=SimpleNamespace(returncode=1, stdout='private', stderr='private')):
            with self.assertRaises(RuntimeError) as caught:
                vault.metadata('show', IDENTITY)
            self.assertNotIn('private', str(caught.exception))

    def test_list_and_fields_contain_metadata_only(self):
        with patch.object(vault, 'metadata', return_value=IDENTITY+'\tapi-key\tAccount one\t0\n'):
            self.assertEqual(vault.records(), [('Account one / 01234567', IDENTITY)])
        with patch.object(vault, 'metadata', return_value='field=api-key\nfield=login-json\nfield=bad name\n'):
            self.assertEqual(vault.fields(IDENTITY), ['api-key', 'login-json'])

    def test_client_helpers_reference_records_without_persisting_keys(self):
        for provider in ('codex', 'claude'):
            command = vault.client_command(provider, IDENTITY)
            joined = ' '.join(command)
            self.assertIn(IDENTITY, joined)
            self.assertIn('token', joined)
            self.assertNotIn(SYNTHETIC.decode(), joined)
        command = vault.client_command('codex', IDENTITY)
        self.assertIn('cli_auth_credentials_store="ephemeral"', command)
        self.assertIn('model_providers.zell_vault.auth.command="'+sys.executable+'"', command)

    def test_save_uses_sealed_descriptor_and_never_logs_input(self):
        captured = []
        def save(argv, **kwargs):
            fd = kwargs['pass_fds'][0]
            self.assertNotIn(SYNTHETIC.decode(), ' '.join(argv))
            captured.append(os.read(fd, 1000))
            with self.assertRaises(OSError):
                os.write(fd, b'changed')
            return SimpleNamespace(returncode=0)
        output = io.StringIO()
        with patch.object(vault.sys.stdin, 'isatty', return_value=True), \
             patch('builtins.input', side_effect=['My account', '']), \
             patch.object(vault.getpass, 'getpass', return_value=SYNTHETIC.decode()), \
             patch.object(vault, 'command', side_effect=lambda *args: ['vault', *args]), \
             patch.object(vault.subprocess, 'run', side_effect=save), contextlib.redirect_stdout(output):
            vault.save_form('codex')
        self.assertEqual(captured, [SYNTHETIC])
        self.assertNotIn(SYNTHETIC.decode(), output.getvalue())

    def test_bad_descriptor_payload_and_file_output_fail_closed(self):
        for payload in (b'', b'line\nbreak', b'a'*65537):
            fd = os.memfd_create('synthetic')
            os.write(fd, payload); os.lseek(fd, 0, os.SEEK_SET)
            with patch.dict(os.environ, KILIX_SECRET_FD=str(fd)):
                with self.assertRaises(ValueError):
                    vault.read_descriptor()
            with self.assertRaises(OSError):
                os.fstat(fd)
        with tempfile.TemporaryFile() as stream:
            result = subprocess.run([sys.executable, vault.HERE, 'token', IDENTITY], stdout=stream, stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(stream.tell(), 0)

    def test_recovery_and_deletion_are_explicit_private_actions(self):
        ui = SimpleNamespace(choose=Mock(return_value='recovery'), page_text=Mock())
        with patch.object(vault, 'binary', return_value='/vault'), \
             patch.dict(os.environ, KILIX_ZELL_SECRETS_SOCKET='/isolated/socket'), \
             patch.object(vault, 'private_terminal') as terminal:
            vault.hub(ui)
        self.assertEqual(terminal.call_args.args[1][-2:], ['unlock', '--recovery'])
        ui.choose.side_effect = ['browse', IDENTITY, 'delete', False]
        with patch.object(vault, 'binary', return_value='/vault'), \
             patch.dict(os.environ, KILIX_ZELL_SECRETS_SOCKET='/isolated/socket'), \
             patch.object(vault, 'records', return_value=[('test', IDENTITY)]), \
             patch.object(vault, 'private_terminal') as terminal:
            vault.hub(ui)
        terminal.assert_not_called()


class RealVaultTests(unittest.TestCase):
    def test_isolated_vault_save_restart_lock_and_client_handoffs(self):
        source = Path(os.environ.get('KILIX_SECRETS_SOURCE', ROOT.parents[1]/'kilix-modules/kilix-secrets'))
        if not (source/'build/kilix-secretsd').is_file():
            self.skipTest('Build kilix-secrets to run its isolated vault integration')
        spec = importlib.util.spec_from_file_location('vault_integration', source/'tests/test_integration.py')
        harness = importlib.util.module_from_spec(spec); spec.loader.exec_module(harness)
        with tempfile.TemporaryDirectory(prefix='zell-vault-test-') as temporary:
            root = Path(temporary)
            socket = root/'runtime/control.sock'
            data = root/'data'
            cli = source/'build/kilix-secrets'
            daemon = harness.start_daemon(source/'build/kilix-secretsd', socket, data)
            env = {**os.environ, 'KILIX_ZELL_SECRETS': str(cli), 'KILIX_ZELL_SECRETS_SOCKET': str(socket),
                   'KILIX_ZELL_STATE': str(root/'zell-state')}
            try:
                initialized, recovery = harness.run_init(cli, socket, vault.APP, b'test-passphrase-only')
                self.assertEqual(initialized.returncode, 0)
                self.assertEqual(harness.run_cli(cli, socket, vault.APP, ['unlock'], b'test-passphrase-only').returncode, 1)
                self.assertEqual(harness.run_cli(cli, socket, vault.APP, ['unlock', '--recovery'], recovery).returncode, 0)
                added = harness.run_cli(cli, socket, vault.APP,
                    ['add', '--type', 'api-key', '--label', 'Synthetic test', '--field', 'api-key'], SYNTHETIC)
                self.assertEqual(added.returncode, 0)
                identity = vault.record_id(added.stdout.decode().strip())
                with patch.dict(os.environ, env):
                    self.assertEqual(vault.fields(identity), ['api-key'])
                    self.assertNotIn(SYNTHETIC.decode(), str(vault.records()))
                result = subprocess.run([sys.executable, vault.HERE, 'token', identity], capture_output=True, env=env)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, SYNTHETIC)
                self.assertEqual(result.stderr, b'')
                # Fake consumers verify handoff without contacting a provider.
                fake = root/'clients'; fake.mkdir()
                shutil.copyfile(source/'tests/fake_kitty.py', fake/'kitty')
                (fake/'kitty').chmod(0o700)
                clipboard = root/'synthetic-clipboard'
                copy_env = {**env, 'PATH': str(fake)+os.pathsep+env['PATH'],
                            'KSEC_TEST_CLIPBOARD_FILE': str(clipboard)}
                copied = harness.run_cli(cli, socket, vault.APP,
                    ['copy', identity, 'api-key', '--clear-seconds', '1'], environment=copy_env)
                self.assertEqual(copied.returncode, 0)
                self.assertEqual(clipboard.read_bytes(), b'')
                self.assertNotIn(SYNTHETIC, copied.stdout+copied.stderr)
                copy_env['KSEC_TEST_CLIPBOARD_MUTATE'] = '1'
                copied = harness.run_cli(cli, socket, vault.APP,
                    ['copy', identity, 'api-key', '--clear-seconds', '0'], environment=copy_env)
                self.assertEqual(copied.returncode, 0)
                self.assertEqual(clipboard.read_bytes(), b'newer-synthetic-clipboard')
                for provider, variable in [('grok', 'XAI_API_KEY'), ('qwen', 'OPENAI_API_KEY')]:
                    script = fake/provider
                    script.write_text('#!'+sys.executable+'\nimport os,sys\n'
                        'assert os.environ.get('+repr(variable)+') == '+repr(SYNTHETIC.decode())+'\n'
                        'assert "KILIX_SECRET_FD" not in os.environ\nprint("consumer-ok")\n')
                    script.chmod(0o700)
                    client_env = {**env, 'PATH': str(fake)+os.pathsep+env['PATH']}
                    result = subprocess.run([sys.executable, vault.HERE, 'launch', provider, identity, str(root)],
                                            capture_output=True, env=client_env)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.strip(), b'consumer-ok')
                harness.stop_daemon(daemon)
                daemon = harness.start_daemon(source/'build/kilix-secretsd', socket, data)
                result = subprocess.run([sys.executable, vault.HERE, 'token', identity], capture_output=True, env=env)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(SYNTHETIC, result.stdout+result.stderr)
                self.assertEqual(harness.run_cli(cli, socket, vault.APP, ['unlock'], b'test-passphrase-only').returncode, 0)
                result = subprocess.run([sys.executable, vault.HERE, 'token', identity], capture_output=True, env=env)
                self.assertEqual(result.stdout, SYNTHETIC)
                for path in data.rglob('*'):
                    if path.is_file():
                        self.assertNotIn(SYNTHETIC, path.read_bytes())
                self.assertFalse((root/'zell-state').exists())
            finally:
                harness.stop_daemon(daemon)


if __name__ == '__main__':
    unittest.main()

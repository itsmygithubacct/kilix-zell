"""Key Master: metadata menus and explicit, descriptor-based vault consumers.

Secret values never enter the dialogue protocol. Sensitive forms run in their
own PTY; the vault remains the sole persistent store of secret values.
"""
from __future__ import annotations

import argparse
import fcntl
import getpass
import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import stat
import subprocess
import sys

APP = 'kilix-zell'
PROVIDERS = {'codex': 'Codex / OpenAI', 'claude': 'Claude', 'grok': 'Grok / xAI', 'qwen': 'Qwen / DashScope'}
HERE = str(Path(__file__).resolve())


def binary():
    override = os.environ.get('KILIX_ZELL_SECRETS')
    candidates = [override] if override else [shutil.which('kilix-secrets'),
        str(Path(__file__).resolve().parents[1] / 'libexec/kilix-secrets')]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return str(Path(candidate).resolve())
    raise RuntimeError('kilix-secrets is not installed. Install the vault package or Zell runtime payload first.')


def command(*args):
    prefix = [binary()]
    if os.environ.get('KILIX_ZELL_SECRETS_SOCKET'):
        prefix += ['--socket', os.environ['KILIX_ZELL_SECRETS_SOCKET']]
    return [*prefix, '--app', APP, *map(str, args)]


def record_id(value):
    if not re.fullmatch(r'[0-9a-f]{32}', value):
        raise ValueError('Invalid vault record identity.')
    return value


def metadata(*args):
    if not args or args[0] not in {'list', 'show', 'doctor', 'lock'}:
        raise ValueError('Only non-secret operations may return to the room.')
    result = subprocess.run(command(*args), capture_output=True, text=True, timeout=15)
    if result.returncode:
        # A failed backend must never spill arbitrary output into a room report.
        raise RuntimeError('The vault is locked or unavailable. Unlock it, or check the vault service.')
    return result.stdout[:64000]


def records():
    result = []
    for line in metadata('list').splitlines():
        parts = line.split('\t')
        if len(parts) == 4 and re.fullmatch(r'[0-9a-f]{32}', parts[0]):
            result.append((parts[2][:160] + ' / ' + parts[0][:8], parts[0]))
    return result


def fields(identity):
    return [line[6:] for line in metadata('show', record_id(identity)).splitlines()
            if line.startswith('field=') and re.fullmatch(r'[a-zA-Z0-9_.-]{1,32}', line[6:])]


def private_terminal(ui, argv):
    from zell_terminal import terminal_loop
    if argv[0] != sys.executable:
        argv = [sys.executable, HERE, 'private-command', *argv]
    return terminal_loop(ui, argv, title='KEY MASTER / PRIVATE VAULT', sensitive=True)


def hub(ui):
    try:
        binary()
        if not os.environ.get('KILIX_ZELL_SECRETS_SOCKET'):
            (Path.home()/'.local/share/kilix-secrets').mkdir(parents=True, exist_ok=True, mode=0o700)
            # Socket activation leaves existing vault contents untouched.
            # Missing units fail through the normal actionable error below.
            subprocess.run(['systemctl', '--user', 'start', 'kilix-secrets.socket'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        action = ui.choose('Key Master / encrypted vault', [
            ('Unlock vault', 'unlock'), ('Lock vault', 'lock'),
            ('Confirm recovery code / recover access', 'recovery'),
            ('Save an API key', 'api'), ('Save a login', 'login'),
            ('Save another secret', 'secret'), ('Browse / retrieve secrets', 'browse'),
            ('Launch a client with a saved key', 'launch'),
            ('Initialize a new vault', 'init'), ('Vault health', 'doctor')])
        if action in {'unlock', 'init', 'recovery'}:
            if action == 'init':
                ui.page_text('The vault will ask for a new passphrase and show a recovery code. '
                             'Keep the recovery code privately; Zell does not record it. '
                             'Then choose Confirm recovery code to activate the vault.')
            return private_terminal(ui, command('unlock', '--recovery') if action == 'recovery' else command(action))
        if action in {'lock', 'doctor'}:
            return ui.page_text(metadata(action) or 'Vault locked.')
        if action in {'api', 'login', 'secret'}:
            provider = ui.choose('API key provider', [(name, key) for key, name in PROVIDERS.items()]+[('Other API key', 'api')]) if action == 'api' else None
            if action == 'api' and not provider:
                return
            # Even labels are collected in the private form, outside history.
            return private_terminal(ui, [sys.executable, HERE, 'save', provider or action])
        if action in {'browse', 'launch'}:
            identity = ui.choose('Saved records (values hidden)', records())
            if identity is None:
                return
            record_id(identity)
            if action == 'launch':
                if 'api-key' not in fields(identity):
                    return ui.page_text('Choose an API-key record for a client launch.')
                provider = ui.choose('Client', [(name, key) for key, name in PROVIDERS.items()])
                if provider is None:
                    return
                if shutil.which(provider) is None:
                    return ui.page_text(f'{PROVIDERS[provider]} is not installed. Install its client, then return to Key Master.')
                from zell_rooms import _project_path
                cwd = _project_path(input('Project directory (Enter = home): ').strip() or Path.home())
                text = f'Use this saved API key for a new {PROVIDERS[provider]} session? API usage may be billed separately.'
                if provider in {'grok', 'qwen'}:
                    text += ' The key is supplied only to that client process and its children through its supported environment.'
                if ui.choose(text, [('Launch client', True), ('Cancel', False)]) is not True:
                    return
                return ui.open_tab([sys.executable, HERE, 'launch', provider, identity, str(cwd)], title='Zell / vault client')
            operation = ui.choose('Secret record / values hidden', [
                ('Metadata (no values)', 'show'), ('Copy a field for 20 seconds', 'copy'),
                ('Delete this record', 'delete')])
            if operation == 'show':
                return ui.page_text(metadata('show', identity))
            if operation == 'copy':
                field = ui.choose('Field to copy', [(f, f) for f in fields(identity)])
                if field:
                    return private_terminal(ui, command('copy', identity, field, '--clear-seconds', '20'))
            if operation == 'delete':
                if ui.choose('Permanently delete this vault record?', [('Cancel', False), ('Delete record', True)]) is True:
                    return private_terminal(ui, command('delete', identity, '--confirm', identity))
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
        # Backend errors never carry secret-bearing subprocess output.
        ui.page_text('Key Master could not complete that operation. Check that kilix-secrets is installed, '
                     'its user service is running, and the vault is unlocked. No plaintext fallback is used.')


def save_form(kind):
    if not sys.stdin.isatty():
        raise ValueError('Secret entry requires a private terminal.')
    label = input('Name for this secret (no secret values in the name): ').strip()
    if not label or len(label.encode()) > 160 or any(ord(c) < 32 for c in label):
        raise ValueError('Invalid record label.')
    if kind == 'login':
        username = getpass.getpass('Login name (hidden): ')
        password = getpass.getpass('Password (hidden): ')
        if not username or not password:
            raise ValueError('Both login fields are required.')
        # One encrypted structured field: copying it retrieves both fields as
        # JSON, and is labelled as such before the user opts into clipboard use.
        value = json.dumps({'username': username, 'password': password}).encode()
        field, record_type = 'login-json', 'login'
    else:
        value = getpass.getpass('API key (hidden): ' if kind in {*PROVIDERS, 'api'} else 'Secret value (hidden): ').encode()
        field, record_type = ('api-key', 'api-key') if kind in {*PROVIDERS, 'api'} else ('value', 'secret')
        if kind in PROVIDERS:
            label = PROVIDERS[kind] + ' / ' + label
    if not value or len(value) > 65536:
        raise ValueError('Secret is empty or exceeds the vault limit.')
    fd = os.memfd_create('zell-vault-input', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        with os.fdopen(os.dup(fd), 'wb') as stream:
            stream.write(value)
        del value
        os.lseek(fd, 0, os.SEEK_SET)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
        result = subprocess.run(command('add', '--type', record_type, '--label', label, '--field', field,
                                        '--secret-fd', str(fd)), pass_fds=(fd,), capture_output=True, timeout=20)
        if result.returncode:
            raise RuntimeError('Vault save failed.')
        print('Saved in the encrypted vault.' + (' Copying login-json returns both login fields.' if kind == 'login' else ''))
        input('Enter to return... ')
    finally:
        os.close(fd)


def read_descriptor():
    fd = int(os.environ.pop('KILIX_SECRET_FD'))
    try:
        value = bytearray()
        while len(value) <= 65536:
            chunk = os.read(fd, min(4096, 65537-len(value)))
            if not chunk:
                break
            value.extend(chunk)
        if not value or len(value) > 65536 or any(c < 33 or c > 126 for c in value):
            raise ValueError('Invalid API key payload.')
        return value
    finally:
        os.close(fd)


def emit_token():
    if os.isatty(1) or not stat.S_ISFIFO(os.fstat(1).st_mode):
        raise ValueError('Token output requires a client pipe; terminal and file output are refused.')
    value = read_descriptor()
    try:
        stream = memoryview(value)
        while stream:
            stream = stream[os.write(1, stream):]
    finally:
        value[:] = b'\0' * len(value)


def client_command(provider, identity):
    record_id(identity)
    if provider == 'codex':
        config = {'model_provider': 'zell_vault', 'model_providers.zell_vault.name': 'Key Master',
                  'model_providers.zell_vault.base_url': 'https://api.openai.com/v1',
                  'model_providers.zell_vault.wire_api': 'responses',
                  'model_providers.zell_vault.auth.command': sys.executable,
                  'model_providers.zell_vault.auth.args': [HERE, 'token', identity],
                  'cli_auth_credentials_store': 'ephemeral'}
        return ['codex', '--no-daemon', *[arg for key, val in config.items() for arg in ('-c', key + '=' + json.dumps(val))]]
    if provider == 'claude':
        return ['claude', '--bare', '--settings', json.dumps({'apiKeyHelper': shlex.join([sys.executable, HERE, 'token', identity])})]
    if provider in {'grok', 'qwen'}:
        return command('run', identity, 'api-key', '--', sys.executable, HERE, 'environment-client', provider)
    raise ValueError('Unsupported provider.')


def main(argv=None):
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    parser = argparse.ArgumentParser(description='Zell Key Master vault bridge')
    parser.add_argument('action', choices=['save', 'launch', 'token', 'emit-token', 'environment-client', 'private-command'])
    parser.add_argument('args', nargs=argparse.REMAINDER)
    opts = parser.parse_args(argv)
    try:
        if opts.action == 'private-command' and opts.args:
            if not sys.stdin.isatty() or opts.args[0] != binary():
                raise ValueError('A vault command requires a private terminal.')
            status = subprocess.call(opts.args)
            print('Vault operation finished.' if status == 0 else 'Vault operation did not complete.')
            input('Enter to return... ')
            return status
        elif opts.action == 'save' and len(opts.args) == 1:
            if opts.args[0] not in {*PROVIDERS, 'api', 'login', 'secret'}:
                raise ValueError('Unknown secret kind.')
            save_form(opts.args[0])
        elif opts.action == 'token' and len(opts.args) == 1:
            if os.isatty(1) or not stat.S_ISFIFO(os.fstat(1).st_mode):
                raise ValueError('Client pipe required.')
            os.execv(command('run')[0], command('run', record_id(opts.args[0]), 'api-key', '--', sys.executable, HERE, 'emit-token'))
        elif opts.action == 'emit-token' and not opts.args:
            emit_token()
        elif opts.action == 'launch' and len(opts.args) == 3:
            provider, identity, cwd = opts.args
            from zell_rooms import _project_path
            os.chdir(_project_path(cwd))
            argv = client_command(provider, identity)
            # An explicitly chosen vault record must not silently lose to a
            # pre-existing environment credential for another account.
            for key in list(os.environ):
                if key in {'OPENAI_API_KEY', 'XAI_API_KEY', 'GROK_CODE_XAI_API_KEY', 'DASHSCOPE_API_KEY'} or (
                        key.startswith(('ANTHROPIC_', 'CLAUDE_CODE_')) and key.endswith(('_KEY', '_TOKEN'))):
                    os.environ.pop(key, None)
            if provider == 'claude':
                for key in list(os.environ):
                    if key.startswith('CLAUDE_CODE_USE_'):
                        os.environ.pop(key, None)
                os.environ['ANTHROPIC_BASE_URL'] = 'https://api.anthropic.com'
            os.execvp(argv[0], argv)
        elif opts.action == 'environment-client' and len(opts.args) == 1 and opts.args[0] in {'grok', 'qwen'}:
            provider = opts.args[0]
            value = read_descriptor()
            env = os.environ.copy()
            for key in (('XAI_API_KEY', 'GROK_CODE_XAI_API_KEY') if provider == 'grok' else ('OPENAI_API_KEY', 'DASHSCOPE_API_KEY')):
                env[key] = value.decode('ascii')
            value[:] = b'\0' * len(value)
            argv = ['grok'] if provider == 'grok' else ['qwen', '--auth-type', 'openai', '--openai-base-url', 'https://dashscope-intl.aliyuncs.com/compatible-mode/v1']
            os.execvpe(argv[0], argv, env)
        else:
            raise ValueError('Invalid vault operation.')
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired, EOFError, KeyboardInterrupt):
        print('Key Master: operation stopped or unavailable; no secret was printed.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

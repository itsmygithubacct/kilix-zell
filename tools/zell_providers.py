"""Shared provider registry for Mana, vitals, and Key Master.

Account mode stays unset until a caller stores one of the two filters.
Neither filter is a default. Fleet instruction storage is not decided here.

A discovered key, vault record, or CLI login is only a candidate. Verification
is an adapter's read-only result for that source. Credential values, header
values, emails, and raw provider bodies are not stored.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import stat
import time
from urllib.parse import urlsplit

IDS = ("codex", "claude", "grok", "qwen", "minimax", "gemini", "elevenlabs")
LABELS = {
    "codex": "Codex",
    "claude": "Claude",
    "grok": "Grok",
    "qwen": "Qwen",
    "minimax": "MiniMax",
    "gemini": "Gemini",
    "elevenlabs": "ElevenLabs",
}
ENV_NAMES = {
    "codex": "OPENAI_API_KEY",
    "claude": "ANTHROPIC_API_KEY",
    "grok": "XAI_API_KEY",
    "qwen": "DASHSCOPE_API_KEY",
    "minimax": "MINIMAX_API_KEY",
    "gemini": "GEMINI_API_KEY",
    "elevenlabs": "ELEVENLABS_API_KEY",
}
# Names the existing Claudex usage command accepts. Anything else fails that
# command before it reads the names it does accept.
CLAUDEX_USAGE = ("codex", "claude", "grok", "qwen")
CLI_FILES = {
    "codex": ".codex/auth.json",
    "claude": ".claude/.credentials.json",
    "grok": ".grok/auth.json",
}
WIRE_STATES = ("configured", "verified", "unknown", "stale", "invalid")
WIRE_SOURCES = ("unknown", "api", "cli", "vault")
REASONS = (
    "none", "verification_unavailable", "network", "missing_scope", "zero_quota",
    "quota_unavailable", "vault_locked", "auth_rejected", "logged_out", "stale",
    "rotated", "cancelled", "timeout", "account_mode",
)
UNSET = "unset"
MODE_API = "api"
MODE_API_AND_CLI = "api_and_cli"
MODES = (UNSET, MODE_API, MODE_API_AND_CLI)
MODE_TEXT = {
    UNSET: "unset",
    MODE_API: "verified API-key accounts only",
    MODE_API_AND_CLI: "verified API-key accounts plus verified signed-in CLI subscriptions",
}
FRESH_SECONDS = 30 * 60
ROW_CAP = 32
STATE_NAME = "providers.json"
PAID_PATHS = (
    "/chat/completions", "/responses", "/messages", "/completions", "/complete",
    "/text-to-speech", "/tts", "/generate", "/images/generations", "/audio/speech",
    "/embeddings", "/audio/transcriptions",
)
_SOURCE_ID = re.compile(r"env:[A-Z0-9_]{1,64}|vault:[0-9a-f]{32}|cli:[a-z]{1,16}")
_REVISION = re.compile(r"[A-Za-z0-9._:-]{1,64}")
_RECORD = re.compile(r"[0-9a-f]{32}")
_ADAPTERS = {
    "codex": {
        "api": ("GET", "https://api.openai.com/v1/models", "Authorization", False),
        "cli": None,
    },
    "claude": {
        "api": ("GET", "https://api.anthropic.com/v1/models", "x-api-key", False),
        "cli": None,
    },
    "grok": {
        "api": ("GET", "https://api.x.ai/v1/models", "Authorization", False),
        "cli": ("GET", "https://cli-chat-proxy.grok.com/v1/billing?format=credits", "Authorization", False),
    },
    "qwen": {
        "api": ("GET", "https://dashscope-intl.aliyuncs.com/api/v1/models", "Authorization", False),
        "cli": None,
    },
    "minimax": {
        "api": ("GET", "https://www.minimax.io/v1/token_plan/remains", "Authorization", False),
        "cli": None,
    },
    "gemini": {
        "api": ("GET", "https://generativelanguage.googleapis.com/v1beta/models", "x-goog-api-key", False),
        "cli": None,
    },
    "elevenlabs": {
        "api": ("GET", "https://api.elevenlabs.io/v1/user/subscription", "xi-api-key", True),
        "cli": None,
    },
}
_memory = {}


def fixture_path():
    return Path(__file__).resolve().parents[1] / "tests/fixtures/providers/adapters.json"


def blank_state():
    return {
        "version": 1,
        "account_mode": UNSET,
        "sources": [],
        "bindings": {},
        "check": {"generation": 0, "running": False, "owner": 0},
        "snapshot_generation": 0,
    }


def state_directory(directory=None):
    """Explicit directory, or KILIX_ZELL_STATE. Never invent a home path."""
    if directory is not None:
        return Path(directory)
    env = os.environ.get("KILIX_ZELL_STATE")
    if not env:
        return None
    return Path(env)


def whole(value, low, high):
    return isinstance(value, int) and not isinstance(value, bool) and low <= value <= high


def assert_readonly(method, url):
    """Refuse anything other than a documented GET with no secret in the URL."""
    if method != "GET":
        raise ValueError("only a read-only GET may check a provider")
    parts = urlsplit(url)
    if parts.scheme != "https" or not parts.netloc:
        raise ValueError("provider check URL is not https")
    path = parts.path.lower()
    if any(marker in path for marker in PAID_PATHS):
        raise ValueError("paid inference is not a credential check")
    if parts.query:
        for pair in parts.query.split("&"):
            key, _, value = pair.partition("=")
            if key != "format" or value != "credits":
                raise ValueError("provider check URL must not carry a credential")


def request_for(provider, kind):
    """The read-only request, or None when no documented check exists."""
    if provider not in _ADAPTERS or kind not in ("api", "vault", "cli"):
        return None
    route = "cli" if kind == "cli" else "api"
    spec = _ADAPTERS[provider][route]
    if spec is None:
        return None
    method, url, header, quota = spec
    assert_readonly(method, url)
    return {"method": method, "url": url, "header": header, "quota": quota, "provider": provider, "kind": kind}


def reduce_characters(body):
    """ElevenLabs subscription counts only. Every other field is discarded."""
    if not isinstance(body, dict):
        return None
    used, limit = body.get("character_count"), body.get("character_limit")
    if not whole(used, 0, 10**12) or not whole(limit, 1, 10**12):
        return None
    remaining = max(0, limit - used)
    return round(100 * remaining / limit)


def normalize(provider, kind, http_status, body):
    """Keep status and an optional percent. Drop the provider body."""
    if not whole(http_status, 0, 599):
        return {"error": "network"}
    if http_status != 200:
        return {"http_status": http_status}
    percent = None
    if provider == "elevenlabs" and kind in ("api", "vault"):
        percent = reduce_characters(body)
    elif provider == "grok" and kind == "cli" and isinstance(body, dict):
        value = body.get("remaining_percent")
        percent = value if whole(value, 0, 100) else None
    return {"http_status": 200, "remaining_percent": percent}


def _source(provider, kind, source_id, revision):
    if provider not in IDS or kind not in ("api", "cli", "vault"):
        raise ValueError("unknown provider source")
    if not _SOURCE_ID.fullmatch(source_id) or not _REVISION.fullmatch(revision):
        raise ValueError("source identity is not opaque")
    return {
        "provider": provider,
        "kind": kind,
        "source_id": source_id,
        "revision": revision,
        "state": "configured",
        "percent": -1,
        "reason": "none",
        "verified_at": None,
        "checked_at": None,
        "last_percent": None,
    }


def discover_env(environ):
    """A nonempty variable is a candidate. The value is not copied."""
    found = []
    for provider, name in ENV_NAMES.items():
        value = environ.get(name) if isinstance(environ, dict) else None
        if isinstance(value, str) and value.strip():
            found.append(_source(provider, "api", f"env:{name}", "untracked"))
    return found


def discover_cli(home):
    """Presence of a regular auth file. The file is not opened."""
    if home is None:
        return []
    root = Path(home)
    found = []
    for provider, relative in CLI_FILES.items():
        path = root / relative
        try:
            info = path.lstat()
        except OSError:
            continue
        if not stat.S_ISREG(info.st_mode):
            continue
        found.append(_source(provider, "cli", f"cli:{provider}", str(int(info.st_mtime))))
    return found


def bind_vault_record(state, record_id, provider):
    """Bind a provider to a vault record id. A label is not an identity."""
    if not _RECORD.fullmatch(record_id or "") or provider not in IDS:
        raise ValueError("vault binding needs a record id and a known provider")
    state.setdefault("bindings", {})[record_id] = provider
    return state


def discover_vault(state, listed_ids, locked):
    """Locked, missing, and listed records stay distinct from invalid keys."""
    if locked:
        return [], "vault_locked"
    listed = {item for item in listed_ids or () if _RECORD.fullmatch(item or "")}
    found = []
    for record_id, provider in (state.get("bindings") or {}).items():
        if provider not in IDS or record_id not in listed:
            continue
        found.append(_source(provider, "vault", f"vault:{record_id}", "none"))
    return found, "open"


def _copy_source(raw):
    if not isinstance(raw, dict):
        return None
    try:
        source = _source(raw.get("provider"), raw.get("kind"), raw.get("source_id"), raw.get("revision"))
    except (ValueError, TypeError):
        return None
    state = raw.get("state")
    reason = raw.get("reason")
    source["state"] = state if state in WIRE_STATES else "configured"
    source["reason"] = reason if reason in REASONS else "none"
    source["percent"] = raw.get("percent") if whole(raw.get("percent"), -1, 100) and raw.get("percent") != -0 else -1
    if source["percent"] != -1 and not whole(source["percent"], 0, 100):
        source["percent"] = -1
    verified = raw.get("verified_at")
    checked = raw.get("checked_at")
    last = raw.get("last_percent")
    source["verified_at"] = verified if whole(verified, 0, 10**12) else None
    source["checked_at"] = checked if whole(checked, 0, 10**12) else None
    source["last_percent"] = last if whole(last, 0, 100) else None
    return source


def _clean_state(raw):
    state = blank_state()
    if not isinstance(raw, dict) or raw.get("version") != 1:
        return state
    mode = raw.get("account_mode")
    state["account_mode"] = mode if mode in MODES else UNSET
    generation = raw.get("snapshot_generation")
    state["snapshot_generation"] = generation if whole(generation, 0, 1_000_000_000) else 0
    check = raw.get("check") if isinstance(raw.get("check"), dict) else {}
    for key in ("generation", "owner"):
        value = check.get(key)
        state["check"][key] = value if whole(value, 0, 1_000_000_000) else 0
    state["check"]["running"] = check.get("running") is True
    bindings = raw.get("bindings") if isinstance(raw.get("bindings"), dict) else {}
    for record_id, provider in bindings.items():
        if _RECORD.fullmatch(record_id or "") and provider in IDS:
            state["bindings"][record_id] = provider
    seen = set()
    for item in raw.get("sources") or []:
        source = _copy_source(item)
        if source is None or source["source_id"] in seen:
            continue
        seen.add(source["source_id"])
        state["sources"].append(source)
    return state


def _path(directory):
    return Path(directory) / STATE_NAME


def read_state(directory):
    path = _path(directory)
    if path.is_symlink():
        raise OSError("refusing a symlinked provider state file")
    if not path.exists():
        return blank_state()
    return _clean_state(json.loads(path.read_text(encoding="utf-8")))


def write_state(directory, state):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(directory, 0o700)
    clean = _clean_state(state)
    blob = json.dumps(clean, indent=2, sort_keys=True) + "\n"
    temporary = directory / f".{STATE_NAME}.{os.getpid()}.{time.time_ns()}.tmp"
    fd = os.open(str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        os.write(fd, blob.encode("utf-8"))
    finally:
        os.close(fd)
    os.chmod(temporary, 0o600)
    os.replace(temporary, _path(directory))
    _memory[str(directory)] = clean
    return clean


def forget(directory):
    """Drop this process's copy so the next load behaves as a restart."""
    _memory.pop(str(directory), None)


def load_state(directory, now=None):
    key = str(directory)
    if key in _memory:
        return _memory[key]
    try:
        state = read_state(directory)
    except (OSError, ValueError):
        state = blank_state()
    changed = _downgrade_untracked(state)
    if now is not None:
        changed = _note_expiry(state, now) or changed
    if changed and _path(directory).exists():
        return write_state(directory, state)
    _memory[key] = state
    return state


def _downgrade_untracked(state):
    """An environment key has no stored revision, so a restart is not verified."""
    changed = False
    for source in state["sources"]:
        if source["revision"] == "untracked" and source["state"] == "verified":
            source["last_percent"] = source["percent"] if source["percent"] >= 0 else source["last_percent"]
            source["percent"] = -1
            source["state"] = "configured"
            source["verified_at"] = None
            source["reason"] = "none"
            changed = True
    return changed


def _note_expiry(state, now):
    changed = False
    for source in state["sources"]:
        if source["state"] == "verified" and not _fresh(source, now):
            source["last_percent"] = source["percent"] if source["percent"] >= 0 else source["last_percent"]
            source["percent"] = -1
            source["state"] = "stale"
            source["reason"] = "stale"
            changed = True
    return changed


def _fresh(source, now):
    stamp = source.get("verified_at")
    return whole(stamp, 0, 10**12) and 0 <= now - stamp <= FRESH_SECONDS


def peek_mode(directory=None):
    """Read the stored mode without creating or rewriting state."""
    directory = state_directory(directory)
    if directory is None:
        return UNSET
    path = _path(directory)
    try:
        if not path.exists() or path.is_symlink():
            return UNSET
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return UNSET
    mode = raw.get("account_mode") if isinstance(raw, dict) else None
    return mode if mode in MODES else UNSET


def account_mode(directory=None):
    return peek_mode(directory)


def write_account_mode(directory, mode):
    """Store a mode only when the caller names one. Missing stays unset."""
    if mode not in MODES:
        raise ValueError("unknown account mode")
    state = load_state(directory)
    state["account_mode"] = mode
    return write_state(directory, state)


def mode_line(directory=None):
    return mode_line_from(peek_mode(directory))


def empty_text(mode):
    if mode == UNSET:
        return "Account mode is unset. Mana is not choosing which verified accounts to show."
    if mode == MODE_API:
        return "Add a key: Key Master"
    if mode == MODE_API_AND_CLI:
        return "Add a key or sign in: Key Master"
    raise ValueError("unknown account mode")


def effective_state(source, now):
    if source["state"] == "verified" and not _fresh(source, now):
        return "stale"
    return source["state"]


def eligible(source, mode, now):
    if mode not in MODES or mode == UNSET:
        return False
    if effective_state(source, now) != "verified":
        return False
    if source["kind"] == "cli":
        return mode == MODE_API_AND_CLI
    return source["kind"] in ("api", "vault") and mode in (MODE_API, MODE_API_AND_CLI)


def attention(sources, mode, now):
    """Configured, failed, stale, and filtered accounts. No live mana bar."""
    rows = []
    for source in sources:
        if eligible(source, mode, now):
            continue
        reason = source["reason"]
        if effective_state(source, now) == "verified" and source["kind"] == "cli" and mode == MODE_API:
            reason = "account_mode"
        rows.append({
            "provider": source["provider"],
            "source_id": source["source_id"],
            "state": effective_state(source, now),
            "reason": reason,
            "live_bar": False,
            "last_percent": source["last_percent"] if source["last_percent"] is not None else (
                source["percent"] if effective_state(source, now) == "stale" and source["percent"] >= 0 else None),
        })
    return rows


def published_rows(sources, mode, now):
    """One eligible verified row per provider. Other accounts stay in state."""
    chosen = {}
    for source in sources:
        if not eligible(source, mode, now):
            continue
        current = chosen.get(source["provider"])
        if current is None or _prefer(source, current):
            chosen[source["provider"]] = source
    rows = []
    for provider in IDS:
        source = chosen.get(provider)
        if source is None:
            continue
        rows.append({
            "id": provider,
            "percent": source["percent"],
            "state": "verified",
            "source": source["kind"],
            "plan": "unknown",
            "source_id": source["source_id"],
        })
    return rows


def _prefer(candidate, current):
    """A known percent outranks a missing one. Zero outranks a higher balance."""
    if candidate["percent"] < 0:
        return False
    if current["percent"] < 0:
        return True
    if candidate["percent"] != current["percent"]:
        return candidate["percent"] < current["percent"]
    return candidate["source_id"] < current["source_id"]


def verified_mean(sources, mode, now):
    """Eligible known percents only. Missing and stale readings are not zero."""
    if mode == UNSET:
        return None
    values = [row["percent"] for row in published_rows(sources, mode, now) if row["percent"] >= 0]
    if not values:
        return None
    return round(sum(values) / len(values))


def format_g(generation, rows):
    if not whole(generation, 0, 1_000_000_000):
        raise ValueError("bad provider generation")
    if len(rows) > ROW_CAP:
        raise ValueError("provider row capacity exceeded")
    lines = [f"1 {generation} {len(rows)}"]
    seen = set()
    for row in rows:
        if row["id"] in seen or row["id"] not in IDS or row["state"] not in WIRE_STATES:
            raise ValueError("bad provider row")
        if row["source"] not in WIRE_SOURCES or row.get("plan", "unknown") != "unknown":
            raise ValueError("bad provider row")
        if not whole(row["percent"], -1, 100) or row["percent"] < -1:
            raise ValueError("bad provider percent")
        seen.add(row["id"])
        lines.append(f"{row['id']} {row['percent']} {row['state']} {row['source']} unknown")
    return "\n".join(lines)


def format_h(generation, running):
    if not whole(generation, 0, 1_000_000_000) or running not in (0, 1):
        raise ValueError("bad check frame")
    return f"1 {generation} {running}"


def _token(line):
    if not line or line[0] == " " or line[-1] == " " or "  " in line:
        return None
    parts = line.split(" ")
    if any(not part or any(ord(ch) < 32 or ord(ch) == 127 for ch in part) for part in parts):
        return None
    return parts


def _uint(text, limit):
    if not text or len(text) > 10 or any(ch < "0" or ch > "9" for ch in text):
        return None
    value = int(text)
    if value > limit:
        return None
    return value


def parse_wire(text):
    """Parse a whole G candidate. None means reject it unchanged."""
    if not isinstance(text, str):
        return None
    body = text[:-1] if text.endswith("\n") else text
    if not body or body.endswith("\n"):
        return None
    lines = body.split("\n")
    header = _token(lines[0])
    if header is None or len(header) != 3 or header[0] != "1":
        return None
    generation = _uint(header[1], 1_000_000_000)
    count = _uint(header[2], ROW_CAP)
    if generation is None or count is None or len(lines) != count + 1:
        return None
    rows = []
    seen = set()
    for line in lines[1:]:
        field = _token(line)
        if field is None or len(field) != 5 or field[0] not in IDS or field[0] in seen:
            return None
        if field[2] not in WIRE_STATES or field[3] not in WIRE_SOURCES or field[4] != "unknown":
            return None
        if field[1] == "-1":
            percent = -1
        else:
            percent = _uint(field[1], 100)
            if percent is None:
                return None
        seen.add(field[0])
        rows.append({"id": field[0], "percent": percent, "state": field[2], "source": field[3], "plan": "unknown"})
    return {"generation": generation, "rows": rows}


def replace_snapshot(previous, text):
    """Keep the previous snapshot unless the candidate parses completely."""
    parsed = parse_wire(text)
    if parsed is None:
        return previous, False
    return text, True


def usage_batch(names):
    """The all-or-nothing Claudex command. A new name refuses the whole batch."""
    names = tuple(names)
    if any(name not in CLAUDEX_USAGE for name in names):
        raise ValueError("refusing a new provider name in the Claudex usage batch")
    return ["usage", "read", *names, "--json"]


def usage_commands(names):
    """One accepted provider per command. New names are left out, not batched."""
    commands = []
    for name in names:
        if name not in CLAUDEX_USAGE:
            continue
        commands.append(["usage", "read", name, "--json"])
    return commands


def begin_check(state):
    """One owner. A second press uses the check that is already running."""
    check = state["check"]
    if check["running"]:
        return check["generation"], False
    nxt = check["generation"] + 1
    if nxt > 1_000_000_000:
        nxt = 1
    check["generation"] = nxt
    check["owner"] = nxt
    check["running"] = True
    return nxt, True


def finish_check(state, generation):
    """A late result from an older generation does not clear the newer check."""
    check = state["check"]
    if generation != check["owner"] or not check["running"]:
        return False
    check["running"] = False
    return True


def _apply_result(source, result, now):
    source["checked_at"] = int(now)
    error = result.get("error") if isinstance(result, dict) else "network"
    if error in ("network", "timeout", "cancelled"):
        source["reason"] = error
        if source["state"] != "verified":
            source["state"] = "unknown"
            source["percent"] = -1
        return source
    status = result.get("http_status") if isinstance(result, dict) else None
    if status == 401:
        source["state"] = "invalid"
        source["reason"] = "auth_rejected"
        source["percent"] = -1
        source["verified_at"] = None
        return source
    if status == 403:
        source["reason"] = "missing_scope"
        if source["state"] == "verified":
            source["last_percent"] = source["percent"] if source["percent"] >= 0 else source["last_percent"]
        source["state"] = "unknown"
        source["percent"] = -1
        source["verified_at"] = None
        return source
    if status != 200:
        source["reason"] = "network"
        if source["state"] != "verified":
            source["state"] = "unknown"
            source["percent"] = -1
        return source
    source["state"] = "verified"
    source["verified_at"] = int(now)
    percent = result.get("remaining_percent") if isinstance(result, dict) else None
    if whole(percent, 0, 100):
        source["percent"] = percent
        source["reason"] = "zero_quota" if percent == 0 else "none"
    else:
        source["percent"] = -1
        source["reason"] = "quota_unavailable"
    return source


def _mark(source, reason, now, stale=False):
    source["checked_at"] = int(now)
    source["reason"] = reason
    if source["percent"] >= 0:
        source["last_percent"] = source["percent"]
    source["percent"] = -1
    source["verified_at"] = None
    source["state"] = "stale" if stale else "unknown"
    return source


def merge_sources(previous, found, searched):
    """Keep every account. A revision change or a removed searched source is not verified."""
    by_id = {item["source_id"]: item for item in previous}
    found_ids = set()
    merged = []
    for source in found:
        found_ids.add(source["source_id"])
        old = by_id.get(source["source_id"])
        if old is None:
            merged.append(source)
            continue
        if old["revision"] != source["revision"]:
            old["last_percent"] = old["percent"] if old["percent"] >= 0 else old["last_percent"]
            old["revision"] = source["revision"]
            old["state"] = "configured"
            old["reason"] = "rotated"
            old["percent"] = -1
            old["verified_at"] = None
        merged.append(old)
    for old in previous:
        if old["source_id"] in found_ids:
            continue
        if old["kind"] not in searched:
            merged.append(old)
            continue
        gone = dict(old)
        if gone["percent"] >= 0:
            gone["last_percent"] = gone["percent"]
        gone["percent"] = -1
        gone["verified_at"] = None
        gone["state"] = "unknown"
        gone["reason"] = "logged_out"
        merged.append(gone)
    return merged


def discover(state, environ=None, home=None, listed_ids=None, vault=None):
    """Collect candidates. vault is 'locked', 'open', or omitted."""
    found = []
    searched = set()
    if environ is not None:
        found.extend(discover_env(environ))
        searched.add("api")
    if home is not None:
        found.extend(discover_cli(home))
        searched.add("cli")
    vault_status = "unknown"
    if vault == "locked":
        vault_status = "vault_locked"
    elif vault == "open":
        records, vault_status = discover_vault(state, listed_ids, locked=False)
        found.extend(records)
        searched.add("vault")
    return merge_sources(state["sources"], found, searched), vault_status


def check_sources(sources, transport, now, vault_locked=False, cancelled=None):
    """Check each source alone. One failure does not stop the others."""
    checked = []
    commands = usage_commands(source["provider"] for source in sources)
    for source in sources:
        current = dict(source)
        if cancelled is not None and cancelled():
            checked.append(_mark(current, "cancelled", now))
            continue
        if current["kind"] == "vault" and vault_locked:
            checked.append(_mark(current, "vault_locked", now, stale=True))
            continue
        spec = request_for(current["provider"], current["kind"])
        if spec is None:
            checked.append(_mark(current, "verification_unavailable", now))
            continue
        try:
            result = transport(spec)
        except TimeoutError:
            result = {"error": "timeout"}
        except OSError:
            result = {"error": "network"}
        if not isinstance(result, dict):
            result = {"error": "network"}
        checked.append(_apply_result(current, result, now))
    return checked, commands


def run_check(state, transport, now, vault_locked=False, cancelled=None):
    """Start one check. A coalesced press does not emit another busy frame."""
    generation, started = begin_check(state)
    if not started:
        return state, None, None, []
    try:
        state["sources"], commands = check_sources(
            state["sources"], transport, now, vault_locked=vault_locked, cancelled=cancelled)
    finally:
        cleared = finish_check(state, generation)
    end = format_h(generation, 0) if cleared else None
    return state, format_h(generation, 1), end, commands


def report(state, now):
    mode = state["account_mode"]
    lines = [mode_line_from(mode)]
    rows = published_rows(state["sources"], mode, now)
    if rows:
        for row in rows:
            lines.append(f"{row['id']} {row['percent']} {row['state']} {row['source']}")
    else:
        lines.append(empty_text(mode))
    lines.append("What needs attention:")
    pending = attention(state["sources"], mode, now)
    if not pending:
        lines.append("none")
    for item in pending:
        lines.append(f"{item['provider']} {item['source_id']} {item['state']} {item['reason']}")
    return "\n".join(lines)


def mode_line_from(mode):
    if mode == UNSET:
        return ("Account mode: unset. Neither filter is selected. "
                "Verified API-key accounts only: not selected. "
                "Verified API-key accounts plus verified signed-in CLI subscriptions: not selected.")
    return "Account mode: " + MODE_TEXT[mode]


def monitor_frames(directory, now=None):
    """Versioned rows only after an explicit mode. Unset stays visible in text."""
    if now is None:
        now = time.time()
    try:
        state = load_state(directory, now)
    except (OSError, ValueError):
        return []
    frames = []
    if state["account_mode"] != UNSET:
        rows = published_rows(state["sources"], state["account_mode"], now)
        body = "\n".join(
            f"{row['id']} {row['percent']} {row['state']} {row['source']} unknown" for row in rows)
        previous = state.get("_wire_body")
        if previous != body:
            nxt = state["snapshot_generation"] + 1
            if nxt > 1_000_000_000:
                nxt = 1
            state["snapshot_generation"] = nxt
            state["_wire_body"] = body
        try:
            frames.append(("G", format_g(state["snapshot_generation"], rows)))
        except ValueError:
            pass
    if state["check"]["generation"]:
        frames.append(("H", format_h(state["check"]["generation"], 1 if state["check"]["running"] else 0)))
    return frames


def live_transport(_spec):
    """Live checks stay acceptance work. Tests pass a fake transport."""
    raise RuntimeError("live provider checks are not enabled")

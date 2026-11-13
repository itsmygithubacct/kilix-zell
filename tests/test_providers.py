#!/usr/bin/env python3
"""Provider checks: fake transports, both account filters, and no stored secrets."""
import contextlib
import io
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import zell_providers as providers  # noqa: E402
import zell_secrets as vault  # noqa: E402
import zell_vitals  # noqa: E402

CANARY = "CANARY-SECRET-VALUE-9f3a"
EMAIL = "canary@example.invalid"
NOW = 1_800_000_000
RECORD_A = "a" * 32
RECORD_B = "b" * 32


def configured(provider, kind, source_id, revision):
    return providers._source(provider, kind, source_id, revision)


def verified(provider, kind, source_id, revision, percent, now=NOW):
    source = configured(provider, kind, source_id, revision)
    source["state"] = "verified"
    source["percent"] = percent
    source["verified_at"] = now
    source["reason"] = "none"
    return source


class ProviderTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.directory = Path(self.folder.name)
        self.addCleanup(lambda: providers.forget(self.directory))

    def assert_clean(self, *blobs):
        for blob in blobs:
            text = blob if isinstance(blob, str) else json.dumps(blob)
            self.assertNotIn(CANARY, text)
            self.assertNotIn(EMAIL, text)

    def test_unset_is_not_a_chosen_filter(self):
        self.assertEqual(providers.blank_state()["account_mode"], providers.UNSET)
        self.assertEqual(providers.peek_mode(self.directory), providers.UNSET)
        self.assertEqual(providers.account_mode(self.directory), providers.UNSET)
        self.assertFalse((self.directory / providers.STATE_NAME).exists())
        line = providers.mode_line(self.directory)
        self.assertIn("Verified API-key accounts only: not selected.", line)
        self.assertIn("Verified API-key accounts plus verified signed-in CLI subscriptions: not selected.", line)
        self.assertEqual(providers.empty_text(providers.UNSET),
                         "Account mode is unset. Mana is not choosing which verified accounts to show.")
        source = verified("grok", "cli", "cli:grok", "10", 33)
        state = providers.blank_state()
        state["sources"] = [source]
        self.assertEqual(providers.published_rows(state["sources"], providers.UNSET, NOW), [])
        self.assertIsNone(providers.verified_mean(state["sources"], providers.UNSET, NOW))
        report = providers.report(state, NOW)
        self.assertIn("not selected", report)
        self.assertIn(providers.empty_text(providers.UNSET), report)
        self.assertNotIn("grok 33 verified cli", report)
        self.assert_clean(report)
        providers.write_account_mode(self.directory, providers.MODE_API)
        providers.write_account_mode(self.directory, providers.UNSET)
        raw = json.loads((self.directory / providers.STATE_NAME).read_text())
        self.assertEqual(raw["account_mode"], "unset")
        (self.directory / providers.STATE_NAME).write_text(json.dumps({"version": 1, "account_mode": "recommended"}))
        providers.forget(self.directory)
        self.assertEqual(providers.peek_mode(self.directory), providers.UNSET)
        self.assertEqual(providers.load_state(self.directory)["account_mode"], providers.UNSET)

    def test_both_filters_keep_a_verified_cli_distinct(self):
        cli = verified("grok", "cli", "cli:grok", "10", 33)
        api = verified("codex", "api", "env:OPENAI_API_KEY", "untracked", 50)
        present = configured("claude", "cli", "cli:claude", "10")
        sources = [cli, api, present]
        self.assertFalse(providers.eligible(cli, providers.UNSET, NOW))
        self.assertFalse(providers.eligible(cli, providers.MODE_API, NOW))
        self.assertTrue(providers.eligible(cli, providers.MODE_API_AND_CLI, NOW))
        self.assertTrue(providers.eligible(api, providers.MODE_API, NOW))
        self.assertTrue(providers.eligible(api, providers.MODE_API_AND_CLI, NOW))
        self.assertFalse(providers.eligible(present, providers.MODE_API_AND_CLI, NOW))
        held = providers.attention(sources, providers.MODE_API, NOW)
        grok = next(item for item in held if item["source_id"] == "cli:grok")
        self.assertEqual(grok["state"], "verified")
        self.assertEqual(grok["reason"], "account_mode")
        self.assertFalse(grok["live_bar"])
        board = providers.published_rows(sources, providers.MODE_API, NOW)
        self.assertEqual([row["id"] for row in board], ["codex"])
        both = providers.published_rows(sources, providers.MODE_API_AND_CLI, NOW)
        self.assertEqual([(row["id"], row["source"]) for row in both], [("codex", "api"), ("grok", "cli")])
        self.assertEqual(providers.empty_text(providers.MODE_API), "Add a key: Key Master")
        self.assertEqual(providers.empty_text(providers.MODE_API_AND_CLI), "Add a key or sign in: Key Master")
        self.assertIsNone(providers.request_for("codex", "cli"))
        self.assertIsNone(providers.request_for("claude", "cli"))
        self.assertEqual(providers.request_for("grok", "cli")["url"],
                         "https://cli-chat-proxy.grok.com/v1/billing?format=credits")

    def test_discovery_does_not_store_or_read_secrets(self):
        found = providers.discover_env({
            "OPENAI_API_KEY": CANARY,
            "ANTHROPIC_API_KEY": "  ",
            "UNRELATED": CANARY,
            "HOME": str(self.directory),
        })
        self.assertEqual([item["source_id"] for item in found], ["env:OPENAI_API_KEY"])
        self.assertEqual(found[0]["state"], "configured")
        self.assert_clean(found)
        home = self.directory / "home"
        auth = home / ".grok/auth.json"
        auth.parent.mkdir(parents=True)
        auth.write_text(CANARY + "\n" + EMAIL)
        (home / ".codex").mkdir()
        (home / ".codex/auth.json").write_text(CANARY)
        (home / ".qwen").mkdir()
        (home / ".qwen/auth.json").write_text(CANARY)
        with patch.object(Path, "read_text", side_effect=AssertionError("read")), \
                patch.object(Path, "read_bytes", side_effect=AssertionError("read")), \
                patch.object(Path, "open", side_effect=AssertionError("open")), \
                patch.object(Path, "home", side_effect=AssertionError("home")):
            cli = providers.discover_cli(home)
        names = {item["provider"] for item in cli}
        self.assertEqual(names, {"codex", "grok"})
        self.assertTrue(all(item["state"] == "configured" for item in cli))
        self.assert_clean(cli)
        self.assertEqual(providers.discover_cli(None), [])
        checked, commands = providers.check_sources(
            found, lambda spec: {"http_status": 200}, NOW)
        self.assertEqual(checked[0]["state"], "verified")
        self.assertEqual(checked[0]["reason"], "quota_unavailable")
        self.assert_clean(checked, commands)
        with patch.dict(os.environ, {"OPENAI_API_KEY": CANARY, "ELEVENLABS_API_KEY": CANARY}):
            sniffed = providers.load_state(self.directory, NOW)
            frames = providers.monitor_frames(self.directory, NOW)
        self.assertEqual(sniffed["sources"], [])
        self.assert_clean(sniffed, frames)

    def test_restart_downgrades_untracked_env_verification(self):
        source = verified("codex", "api", "env:OPENAI_API_KEY", "untracked", 42)
        state = providers.blank_state()
        state["sources"] = [source]
        providers.write_state(self.directory, state)
        self.assertEqual(providers.load_state(self.directory)["sources"][0]["state"], "verified")
        providers.forget(self.directory)
        loaded = providers.load_state(self.directory, NOW)
        saved = loaded["sources"][0]
        self.assertEqual(saved["state"], "configured")
        self.assertEqual(saved["percent"], -1)
        self.assertEqual(saved["last_percent"], 42)
        self.assertIsNone(saved["verified_at"])
        self.assertFalse(providers.eligible(saved, providers.MODE_API, NOW))
        self.assert_clean(loaded, (self.directory / providers.STATE_NAME).read_text())

    def test_freshness_boundary(self):
        fresh = verified("qwen", "api", "env:DASHSCOPE_API_KEY", "untracked", 15, NOW - providers.FRESH_SECONDS)
        stale = verified("qwen", "api", "env:DASHSCOPE_API_KEY", "untracked", 15, NOW - providers.FRESH_SECONDS - 1)
        self.assertTrue(providers.eligible(fresh, providers.MODE_API, NOW))
        self.assertEqual(providers.effective_state(fresh, NOW), "verified")
        self.assertFalse(providers.eligible(stale, providers.MODE_API, NOW))
        self.assertEqual(providers.effective_state(stale, NOW), "stale")
        self.assertIsNone(providers.verified_mean([stale], providers.MODE_API, NOW))
        state = providers.blank_state()
        state["account_mode"] = providers.MODE_API
        state["sources"] = [verified("gemini", "vault", "vault:" + RECORD_A, "none", 15, NOW - 1801)]
        providers.write_state(self.directory, state)
        providers.forget(self.directory)
        loaded = providers.load_state(self.directory, NOW)["sources"][0]
        self.assertEqual(loaded["state"], "stale")
        self.assertEqual(loaded["reason"], "stale")
        self.assertEqual(loaded["last_percent"], 15)
        self.assertEqual(loaded["percent"], -1)

    def test_locked_vault_is_not_invalid_and_cli_can_continue(self):
        state = providers.blank_state()
        providers.bind_vault_record(state, RECORD_A, "elevenlabs")
        self.assertEqual(state["bindings"][RECORD_A], "elevenlabs")
        with self.assertRaises(ValueError):
            providers.bind_vault_record(state, "label-is-not-identity", "grok")
        with self.assertRaises(ValueError):
            providers.bind_vault_record(state, RECORD_A, "nope")
        listed, status = providers.discover_vault(state, [RECORD_A], locked=False)
        self.assertEqual(status, "open")
        self.assertEqual(listed[0]["state"], "configured")
        self.assertEqual(listed[0]["source_id"], "vault:" + RECORD_A)
        empty, status = providers.discover_vault(state, [], locked=False)
        self.assertEqual((empty, status), ([], "open"))
        locked_rows, status = providers.discover_vault(state, [RECORD_A], locked=True)
        self.assertEqual((locked_rows, status), ([], "vault_locked"))
        vault_row = verified("elevenlabs", "vault", "vault:" + RECORD_A, "none", 70)
        cli = configured("grok", "cli", "cli:grok", "10")
        state["sources"] = [vault_row, cli]
        merged, status = providers.discover(state, vault="locked")
        self.assertEqual(status, "vault_locked")
        self.assertEqual(merged[0]["state"], "verified")
        self.assertNotEqual(merged[0]["reason"], "logged_out")
        self.assertEqual(merged[1]["state"], "configured")

        def transport(spec):
            self.assertEqual(spec["provider"], "grok")
            self.assertNotIn(CANARY, json.dumps(spec))
            return providers.normalize("grok", "cli", 200, {"remaining_percent": 22, "email": EMAIL})

        checked, commands = providers.check_sources(merged, transport, NOW, vault_locked=True)
        self.assertEqual(checked[0]["state"], "stale")
        self.assertEqual(checked[0]["reason"], "vault_locked")
        self.assertNotEqual(checked[0]["state"], "invalid")
        self.assertEqual(checked[1]["state"], "verified")
        self.assertEqual(checked[1]["percent"], 22)
        self.assert_clean(checked, commands)

    def test_network_missing_scope_rejection_timeout_and_cancel(self):
        codex = verified("codex", "api", "env:OPENAI_API_KEY", "untracked", 20)
        claude = configured("claude", "api", "env:ANTHROPIC_API_KEY", "untracked")

        def transport(spec):
            if spec["provider"] == "codex":
                raise OSError("down")
            return {"http_status": 200}

        checked, _commands = providers.check_sources([codex, claude], transport, NOW)
        self.assertEqual(checked[0]["state"], "verified")
        self.assertEqual(checked[0]["reason"], "network")
        self.assertEqual(checked[0]["percent"], 20)
        self.assertEqual(checked[1]["state"], "verified")
        self.assertNotEqual(checked[0]["state"], "invalid")
        fresh = configured("codex", "api", "env:OPENAI_API_KEY", "untracked")
        failed, _commands = providers.check_sources([fresh], lambda _spec: (_ for _ in ()).throw(OSError("down")), NOW)
        self.assertEqual(failed[0]["state"], "unknown")
        self.assertEqual(failed[0]["reason"], "network")
        self.assertNotEqual(failed[0]["state"], "invalid")
        denied = providers._apply_result(dict(codex), {"http_status": 403}, NOW)
        self.assertEqual(denied["state"], "unknown")
        self.assertEqual(denied["reason"], "missing_scope")
        self.assertEqual(denied["last_percent"], 20)
        self.assertNotEqual(denied["state"], "invalid")
        rejected = providers._apply_result(dict(codex), {"http_status": 401}, NOW)
        self.assertEqual((rejected["state"], rejected["reason"], rejected["percent"]), ("invalid", "auth_rejected", -1))
        timed = verified("grok", "api", "env:XAI_API_KEY", "untracked", 8)

        def timeout(_spec):
            raise TimeoutError("slow")

        kept, _commands = providers.check_sources([timed], timeout, NOW)
        self.assertEqual(kept[0]["state"], "verified")
        self.assertEqual(kept[0]["reason"], "timeout")
        state = providers.blank_state()
        state["sources"] = [verified("qwen", "api", "env:DASHSCOPE_API_KEY", "untracked", 9)]
        _state, start, end, _commands = providers.run_check(state, lambda _spec: {"http_status": 200}, NOW, cancelled=lambda: True)
        self.assertTrue(start.endswith(" 1"))
        self.assertTrue(end.endswith(" 0"))
        self.assertFalse(state["check"]["running"])
        self.assertEqual(state["sources"][0]["reason"], "cancelled")
        self.assertEqual(state["sources"][0]["state"], "unknown")
        self.assertNotEqual(state["sources"][0]["state"], "invalid")
        self.assertEqual(state["sources"][0]["last_percent"], 9)

    def test_zero_quota_is_kept_and_missing_quota_is_not_zero(self):
        self.assertIsNone(providers.reduce_characters({"character_count": True, "character_limit": 10}))
        self.assertEqual(providers.reduce_characters({"character_count": 100, "character_limit": 100}), 0)
        body = {"character_count": 100, "character_limit": 100, "tier": "creator", "email": EMAIL}
        result = providers.normalize("elevenlabs", "api", 200, body)
        self.assertEqual(result, {"http_status": 200, "remaining_percent": 0})
        self.assert_clean(result)
        eleven = configured("elevenlabs", "api", "env:ELEVENLABS_API_KEY", "untracked")
        providers._apply_result(eleven, result, NOW)
        self.assertEqual((eleven["state"], eleven["percent"], eleven["reason"]), ("verified", 0, "zero_quota"))
        hidden = {"email": EMAIL, "data": [{"id": "gpt"}]}
        missing = providers.normalize("codex", "api", 200, hidden)
        self.assertEqual(missing, {"http_status": 200, "remaining_percent": None})
        self.assert_clean(missing)
        codex = configured("codex", "api", "env:OPENAI_API_KEY", "untracked")
        providers._apply_result(codex, missing, NOW)
        self.assertEqual((codex["state"], codex["percent"], codex["reason"]), ("verified", -1, "quota_unavailable"))
        self.assertEqual(providers.verified_mean([eleven, codex], providers.MODE_API, NOW), 0)
        self.assertIsNone(providers.verified_mean([codex], providers.MODE_API, NOW))
        guessed = providers.normalize("minimax", "api", 200, {"remain_percent": 5, "email": EMAIL})
        self.assertIsNone(guessed["remaining_percent"])
        self.assert_clean(guessed)
        grok_cli = providers.normalize("grok", "cli", 200, {"remaining_percent": 12, "email": EMAIL})
        grok_api = providers.normalize("grok", "api", 200, {"remaining_percent": 12, "email": EMAIL})
        self.assertEqual(grok_cli["remaining_percent"], 12)
        self.assertIsNone(grok_api["remaining_percent"])
        self.assert_clean(grok_cli, grok_api)
        rows = providers.published_rows([eleven], providers.MODE_API, NOW)
        self.assertEqual(rows[0]["percent"], 0)
        self.assertEqual(providers.format_g(1, rows).splitlines()[-1], "elevenlabs 0 verified api unknown")

    def test_rotation_then_rejection(self):
        home = self.directory / "home"
        auth = home / ".grok/auth.json"
        auth.parent.mkdir(parents=True)
        auth.write_text(CANARY)
        state = providers.blank_state()
        state["sources"] = providers.discover_cli(home)
        state["sources"][0]["state"] = "verified"
        state["sources"][0]["percent"] = 40
        state["sources"][0]["verified_at"] = NOW
        os.utime(auth, (NOW + 50, NOW + 50))
        merged, _status = providers.discover(state, home=home)
        self.assertEqual(merged[0]["state"], "configured")
        self.assertEqual(merged[0]["reason"], "rotated")
        self.assertEqual(merged[0]["last_percent"], 40)
        self.assert_clean(merged)

        def transport(spec):
            self.assertNotIn(CANARY, json.dumps(spec))
            self.assertEqual(spec["url"], "https://cli-chat-proxy.grok.com/v1/billing?format=credits")
            return {"http_status": 401}

        checked, commands = providers.check_sources(merged, transport, NOW)
        self.assertEqual(checked[0]["state"], "invalid")
        self.assertEqual(checked[0]["reason"], "auth_rejected")
        self.assert_clean(checked, commands)

    def test_removed_source_is_logged_out_not_invalid(self):
        api = verified("codex", "api", "env:OPENAI_API_KEY", "untracked", 15)
        cli = verified("grok", "cli", "cli:grok", "10", 16)
        state = providers.blank_state()
        state["sources"] = [api, cli]
        merged, _status = providers.discover(state, environ={})
        by_id = {item["source_id"]: item for item in merged}
        self.assertEqual(by_id["env:OPENAI_API_KEY"]["reason"], "logged_out")
        self.assertEqual(by_id["env:OPENAI_API_KEY"]["state"], "unknown")
        self.assertEqual(by_id["env:OPENAI_API_KEY"]["last_percent"], 15)
        self.assertNotEqual(by_id["env:OPENAI_API_KEY"]["state"], "invalid")
        self.assertEqual(by_id["cli:grok"]["state"], "verified")
        home = self.directory / "empty-home"
        home.mkdir()
        gone, _status = providers.discover(state, home=home)
        cli_row = next(item for item in gone if item["source_id"] == "cli:grok")
        api_row = next(item for item in gone if item["source_id"] == "env:OPENAI_API_KEY")
        self.assertEqual(cli_row["reason"], "logged_out")
        self.assertEqual(cli_row["state"], "unknown")
        self.assertEqual(api_row["state"], "verified")

    def test_two_accounts_publish_the_lower_percent(self):
        low = verified("grok", "vault", "vault:" + RECORD_B, "none", 5)
        high = verified("grok", "vault", "vault:" + RECORD_A, "none", 20)
        sources = [high, low]
        rows = providers.published_rows(sources, providers.MODE_API, NOW)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["percent"], 5)
        self.assertEqual(rows[0]["source_id"], "vault:" + RECORD_B)
        self.assertEqual(len(sources), 2)
        frame = providers.format_g(3, rows)
        self.assertNotIn("vault:", frame)
        self.assertIn("grok 5 verified vault unknown", frame)
        tied = [
            verified("grok", "vault", "vault:" + RECORD_B, "none", 10),
            verified("grok", "vault", "vault:" + RECORD_A, "none", 10),
        ]
        chosen = providers.published_rows(tied, providers.MODE_API, NOW)[0]
        self.assertEqual(chosen["source_id"], "vault:" + RECORD_A)
        unknown = [
            verified("gemini", "api", "env:GEMINI_API_KEY", "untracked", -1),
            verified("gemini", "vault", "vault:" + RECORD_A, "none", -1),
        ]
        self.assertEqual(providers.published_rows(unknown, providers.MODE_API, NOW)[0]["source_id"], "env:GEMINI_API_KEY")

    def test_unsupported_provider_is_omitted_from_usage_commands(self):
        nope = {
            "provider": "nope",
            "kind": "api",
            "source_id": "env:NOPE",
            "revision": "untracked",
            "state": "configured",
            "percent": -1,
            "reason": "none",
            "verified_at": None,
            "checked_at": None,
            "last_percent": None,
        }
        codex = configured("codex", "api", "env:OPENAI_API_KEY", "untracked")
        called = []

        def transport(spec):
            called.append(spec["provider"])
            self.assertNotIn(CANARY, json.dumps(spec))
            return {"http_status": 200}

        checked, commands = providers.check_sources([nope, codex], transport, NOW)
        self.assertEqual(checked[0]["reason"], "verification_unavailable")
        self.assertNotEqual(checked[0]["state"], "verified")
        self.assertEqual(checked[1]["state"], "verified")
        self.assertEqual(called, ["codex"])
        self.assertEqual(commands, [["usage", "read", "codex", "--json"]])
        with self.assertRaises(ValueError):
            providers.usage_batch(("codex", "minimax"))
        self.assertEqual(
            providers.usage_batch(("codex", "claude", "grok")),
            ["usage", "read", "codex", "claude", "grok", "--json"])
        split = providers.usage_commands(providers.IDS)
        self.assertEqual([item[2] for item in split], ["codex", "claude", "grok", "qwen"])
        self.assertTrue(all(len(item) == 4 and item[-1] == "--json" for item in split))
        state = providers.blank_state()
        state["sources"] = [codex, configured("minimax", "api", "env:MINIMAX_API_KEY", "untracked")]
        with patch.object(subprocess, "run", side_effect=AssertionError("executed")):
            _state, _start, _end, emitted = providers.run_check(
                state, lambda spec: {"http_status": 200}, NOW)
        self.assertEqual([item[2] for item in emitted], ["codex"])
        self.assertNotIn("minimax", json.dumps(emitted))
        self.assert_clean(checked, commands, emitted)

    def test_wire_round_trip_and_rejection(self):
        def row(provider, percent, state, source):
            return {"id": provider, "percent": percent, "state": state, "source": source, "plan": "unknown"}

        seven = [
            row("codex", 5, "verified", "api"),
            row("claude", 6, "verified", "cli"),
            row("grok", 7, "verified", "vault"),
            row("qwen", 8, "configured", "api"),
            row("minimax", 9, "unknown", "api"),
            row("gemini", -1, "stale", "api"),
            row("elevenlabs", 0, "verified", "api"),
        ]
        text = providers.format_g(70, seven)
        self.assertEqual(providers.parse_wire(text), {"generation": 70, "rows": seven})
        self.assertEqual(providers.replace_snapshot("previous", text), (text, True))
        five = providers.format_g(5, seven[:5])
        self.assertEqual(providers.parse_wire(five)["rows"], seven[:5])
        self.assertEqual(providers.ROW_CAP, 32)
        with self.assertRaises(ValueError):
            providers.format_g(1, seven + [row("codex", 1, "verified", "api")] * 26)
        with self.assertRaises(ValueError):
            providers.format_g(1, [row("codex", 1, "verified", "api"), row("codex", 2, "verified", "api")])
        many = "1 72 32\n" + "\n".join(["codex 1 verified api unknown"] * 32)
        rejected = [
            "1 2 33",
            many,
            "1 9 1\nnope 1 verified api unknown",
            "1 9 1\ncodex 101 verified api unknown",
            "1 9 2\ncodex 1 verified api unknown\ncodex 2 verified api unknown",
            "1 9 1\ncodex 1 verified api unknown extra",
            "1 9 1\ncodex 1 verified api pro",
            " 1 1 0",
            "1 1 1\ncodex 1 verified api unknown ",
        ]
        for candidate in rejected:
            self.assertEqual(providers.replace_snapshot(text, candidate), (text, False))
        self.assert_clean(text, many)
        self.assertEqual(providers.format_h(8, 0), "1 8 0")

    def test_state_file_drops_poison_and_refuses_symlink(self):
        poison = {
            "version": 1,
            "account_mode": "unset",
            "email": EMAIL,
            "secret": CANARY,
            "Authorization": "Bearer " + CANARY,
            "body": {"email": EMAIL},
            "sources": [
                {
                    "provider": "codex",
                    "kind": "api",
                    "source_id": "env:OPENAI_API_KEY",
                    "revision": "untracked",
                    "state": "configured",
                    "percent": -1,
                    "reason": "none",
                    "email": EMAIL,
                    "secret": CANARY,
                    "body": CANARY,
                },
                {
                    "provider": "codex",
                    "kind": "api",
                    "source_id": "env:" + EMAIL,
                    "revision": CANARY,
                    "state": "verified",
                    "percent": 1,
                    "reason": "none",
                },
            ],
        }
        path = self.directory / providers.STATE_NAME
        path.write_text(json.dumps(poison))
        providers.forget(self.directory)
        loaded = providers.load_state(self.directory, NOW)
        self.assertEqual(len(loaded["sources"]), 1)
        self.assert_clean(loaded)
        self.assertIn(CANARY, path.read_text())
        providers.write_state(self.directory, loaded)
        rewritten = path.read_text()
        self.assert_clean(rewritten)
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.directory.stat().st_mode), 0o700)
        path.unlink()
        real = self.directory / "other.json"
        real.write_text(json.dumps({"secret": CANARY}))
        path.symlink_to(real)
        with self.assertRaises(OSError):
            providers.read_state(self.directory)
        providers.forget(self.directory)
        blank = providers.load_state(self.directory, NOW)
        self.assertTrue(path.is_symlink())
        self.assertEqual(blank["sources"], [])
        self.assertIn(CANARY, real.read_text())
        path.unlink()
        path.symlink_to(self.directory / "missing.json")
        with self.assertRaises(OSError):
            providers.read_state(self.directory)

    def test_check_owner_coalesces_and_late_result_does_not_clear(self):
        state = providers.blank_state()
        state["sources"] = [configured("codex", "api", "env:OPENAI_API_KEY", "untracked")]
        generation, started = providers.begin_check(state)
        self.assertTrue(started)

        def refuse(_spec):
            raise AssertionError("second press ran")

        again, start, end, commands = providers.run_check(state, refuse, NOW)
        self.assertIs(again, state)
        self.assertIsNone(start)
        self.assertIsNone(end)
        self.assertEqual(commands, [])
        self.assertEqual(state["check"]["generation"], generation)
        self.assertTrue(state["check"]["running"])
        self.assertFalse(providers.finish_check(state, generation - 1))
        self.assertTrue(state["check"]["running"])
        providers.write_state(self.directory, state)
        providers.forget(self.directory)
        loaded = providers.load_state(self.directory)
        _generation, started = providers.begin_check(loaded)
        self.assertFalse(started)
        self.assertTrue(loaded["check"]["running"])
        fresh = providers.blank_state()
        fresh["sources"] = [configured("claude", "api", "env:ANTHROPIC_API_KEY", "untracked")]

        def bump(_spec):
            fresh["check"]["owner"] = 99
            return {"http_status": 200}

        _fresh, start, end, _commands = providers.run_check(fresh, bump, NOW)
        self.assertTrue(start.endswith(" 1"))
        self.assertIsNone(end)
        self.assertTrue(fresh["check"]["running"])
        self.assertEqual(fresh["check"]["owner"], 99)
        raised = providers.blank_state()
        raised["sources"] = [configured("elevenlabs", "api", "env:ELEVENLABS_API_KEY", "untracked")]
        with self.assertRaises(RuntimeError):
            providers.run_check(raised, providers.live_transport, NOW)
        self.assertFalse(raised["check"]["running"])
        self.assertEqual(raised["sources"][0]["state"], "configured")
        wrapped = providers.blank_state()
        wrapped["check"]["generation"] = 1_000_000_000
        wrapped["check"]["owner"] = 1_000_000_000
        nxt, started = providers.begin_check(wrapped)
        self.assertEqual((nxt, started), (1, True))
        failed = providers.blank_state()
        failed["sources"] = [configured("qwen", "api", "env:DASHSCOPE_API_KEY", "untracked")]

        def down(_spec):
            raise OSError("down")

        _failed, _start, end, _commands = providers.run_check(failed, down, NOW)
        self.assertTrue(end.endswith(" 0"))
        self.assertFalse(failed["check"]["running"])
        self.assertEqual(failed["sources"][0]["reason"], "network")
        self.assertNotEqual(failed["sources"][0]["state"], "invalid")

    def test_monitor_frames_hide_unset_and_publish_an_explicit_mode(self):
        with patch.object(providers, "live_transport", side_effect=AssertionError("live")):
            self.assertEqual(providers.monitor_frames(self.directory, NOW), [])
        self.assertFalse((self.directory / providers.STATE_NAME).exists())
        providers.write_account_mode(self.directory, providers.MODE_API)
        first = providers.monitor_frames(self.directory, NOW)
        second = providers.monitor_frames(self.directory, NOW)
        self.assertEqual(first, [("G", "1 1 0")])
        self.assertEqual(second, [("G", "1 1 0")])
        state = providers.load_state(self.directory)
        state["check"] = {"generation": 4, "running": True, "owner": 4}
        providers.write_state(self.directory, state)
        providers.forget(self.directory)
        frames = providers.monitor_frames(self.directory, NOW)
        self.assertEqual(frames[0][0], "G")
        self.assertEqual(frames[1], ("H", "1 4 1"))
        providers.write_account_mode(self.directory, providers.UNSET)
        providers.forget(self.directory)
        hidden = providers.monitor_frames(self.directory, NOW)
        self.assertEqual(hidden, [("H", "1 4 1")])
        self.assertTrue(all(kind != "G" for kind, _payload in hidden))

    def test_adapters_match_the_redacted_fixture(self):
        fixture = json.loads(providers.fixture_path().read_text(encoding="utf-8"))
        self.assert_clean(fixture)
        self.assertEqual(tuple(fixture["providers"]), providers.IDS)
        for name, spec in fixture["providers"].items():
            api = spec["api"]
            self.assertEqual(providers._ADAPTERS[name]["api"], (api["method"], api["url"], api["header"], api["quota"]))
            providers.assert_readonly(api["method"], api["url"])
            cli = spec["cli"]
            if cli is None:
                self.assertIsNone(providers._ADAPTERS[name]["cli"])
            else:
                self.assertEqual(
                    providers._ADAPTERS[name]["cli"],
                    (cli["method"], cli["url"], cli["header"], cli["quota"]))
                providers.assert_readonly(cli["method"], cli["url"])
        self.assertTrue(providers.request_for("gemini", "api")["url"].endswith("/v1beta/models"))
        self.assertNotIn("?", providers.request_for("gemini", "api")["url"])
        self.assertTrue(providers.request_for("grok", "api")["url"].endswith("/v1/models"))
        self.assertNotIn("api-key", providers.request_for("grok", "api")["url"])
        self.assertTrue(providers.request_for("elevenlabs", "api")["url"].endswith("/v1/user/subscription"))
        self.assertTrue(providers.request_for("minimax", "api")["url"].endswith("/v1/token_plan/remains"))
        with self.assertRaises(ValueError):
            providers.assert_readonly("POST", "https://api.openai.com/v1/models")
        with self.assertRaises(ValueError):
            providers.assert_readonly("GET", "https://api.openai.com/v1/chat/completions")
        with self.assertRaises(ValueError):
            providers.assert_readonly("GET", "https://generativelanguage.googleapis.com/v1beta/models?key=" + CANARY)
        with self.assertRaises(RuntimeError):
            providers.live_transport({"method": "GET", "url": "https://api.openai.com/v1/models"})

    def test_explicit_mode_can_replace_the_vitals_mean(self):
        source = verified("grok", "vault", "vault:" + RECORD_A, "none", 40)
        state = providers.blank_state()
        state["account_mode"] = providers.MODE_API
        state["sources"] = [source]
        providers.write_state(self.directory, state)
        providers.forget(self.directory)
        window = {"remaining_percent": 80, "resets_at": NOW + 100}
        cache = {"providers": {
            name: {"status": "ok", "checked_at": NOW - 10, "windows": [dict(window)]}
            for name in ("codex", "claude", "grok")
        }}

        def read_json(path):
            if str(path).endswith("usage-pages.json"):
                return cache
            return {}

        with patch.object(zell_vitals, "config_path", return_value=self.directory / "loop.json"), \
                patch.object(zell_vitals, "read_json", side_effect=read_json):
            value, _age, lines = zell_vitals.mana(SimpleNamespace(), NOW, self.directory)
            self.assertEqual(value, 40)
            self.assertIn("Account mode: api", lines)
            state["account_mode"] = providers.UNSET
            providers.write_state(self.directory, state)
            providers.forget(self.directory)
            value, _age, lines = zell_vitals.mana(SimpleNamespace(), NOW, self.directory)
        self.assertEqual(value, 80)
        self.assertIn("Account mode: unset", lines)
        home = self.directory / "home-state"
        home.mkdir()
        (home / providers.STATE_NAME).write_text(json.dumps({"version": 1, "account_mode": "api"}))
        with patch.dict(os.environ, {"HOME": str(home)}):
            os.environ.pop("KILIX_ZELL_STATE", None)
            self.assertEqual(providers.account_mode(None), providers.UNSET)
            self.assertIsNone(providers.state_directory())

    def test_key_master_offers_registry_ids_without_inferring_a_binding(self):
        menu = vault.api_key_menu()
        self.assertEqual([key for _label, key in menu],
                         ["codex", "claude", "grok", "qwen", "minimax", "gemini", "elevenlabs", "api"])
        ui = SimpleNamespace(choose=Mock(side_effect=["api", None]), page_text=Mock())
        with patch.object(vault, "binary", return_value="/vault"), \
                patch.dict(os.environ, {"KILIX_ZELL_SECRETS_SOCKET": "/isolated/socket"}):
            vault.hub(ui)
        self.assertEqual(ui.choose.call_args_list[1].args[1], menu)
        with patch.object(vault, "save_form") as form:
            self.assertEqual(vault.main(["save", "minimax"]), 0)
        form.assert_called_once_with("minimax")
        with patch.object(vault, "save_form") as form, contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(vault.main(["save", "nope"]), 1)
        form.assert_not_called()
        self.assertNotIn(CANARY, err.getvalue())
        self.assertNotIn(EMAIL, err.getvalue())
        with self.assertRaises(ValueError):
            vault.client_command("minimax", RECORD_A)
        command = vault.client_command("codex", RECORD_A)
        self.assertIn("codex", command[0])
        self.assert_clean(command)
        captured = []

        def save(argv, **kwargs):
            captured.append(argv)
            os.read(kwargs["pass_fds"][0], 1000)
            return SimpleNamespace(returncode=0, stdout=CANARY + " " + RECORD_A, stderr=EMAIL)

        with patch.object(vault.sys.stdin, "isatty", return_value=True), \
                patch("builtins.input", side_effect=["Account", "", "Account", ""]), \
                patch.object(vault.getpass, "getpass", return_value=CANARY), \
                patch.object(vault, "command", side_effect=lambda *args: ["vault", *args]), \
                patch.object(vault.subprocess, "run", side_effect=save), \
                patch.object(providers, "bind_vault_record", side_effect=AssertionError("inferred")), \
                patch.dict(os.environ, {"KILIX_ZELL_STATE": str(self.directory), "KILIX_ZELL_SECRETS_SOCKET": "/isolated/socket"}), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            vault.save_form("minimax")
            vault.save_form("codex")
        self.assertIn("Saved in the encrypted vault.", out.getvalue())
        self.assertNotIn(CANARY, out.getvalue())
        self.assertNotIn(EMAIL, out.getvalue())
        self.assertIn("MiniMax / Account", captured[0])
        self.assertIn("Codex / OpenAI / Account", captured[1])
        self.assert_clean(captured)
        self.assertFalse((self.directory / providers.STATE_NAME).exists())


if __name__ == "__main__":
    unittest.main()

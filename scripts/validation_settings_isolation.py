"""Process-boundary isolation for development Unreal validation runners.

The original INI is never restored or written. Each engine process receives a
private clone via the UE-native -GameUserSettingsINI= destination override.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path


def _state(path: Path) -> dict:
    if not path.exists():
        return {"path": str(path), "exists": False, "sha256": None, "bytes": 0}
    if not path.is_file():
        raise RuntimeError("Expected a regular GameUserSettings INI: " + str(path))
    content = path.read_bytes()
    return {"path": str(path), "exists": True,
            "sha256": hashlib.sha256(content).hexdigest(), "bytes": len(content)}


def normalized_ini_path_sha1(path: Path) -> str:
    """Matches UE full/normalized slash/collapse/windows-lower UTF8 identity."""
    normalized = path.resolve().as_posix()
    if os.name == "nt":
        normalized = normalized.lower()
    return hashlib.sha1(normalized.encode("utf-8")).hexdigest()


class RunSettingsIsolation:
    """Preserve exact startup bytes, and check again after process exit/flush."""

    def __init__(self, repo: Path, output: Path):
        self.repo = repo.resolve()
        self.output = output.resolve()
        config = self.repo / "Saved/Config"
        if self.output.is_relative_to(config):
            raise RuntimeError("Validation output must not be the player's Config directory")
        # UnrealEditor -game and native Windows commandlets are the supported
        # runners. Protect both possible Windows generated-config branches,
        # plus every already-existing platform GameUserSettings file.
        candidates = {config / "WindowsEditor/GameUserSettings.ini",
                      config / "Windows/GameUserSettings.ini"}
        if config.exists():
            candidates.update(config.glob("*/GameUserSettings.ini"))
        self.paths = sorted(path.resolve() for path in candidates)
        self.original_bytes = {}
        self.before = []
        self.archive = self.output / "_user_settings_preservation"
        self.archive.mkdir(parents=True, exist_ok=False)
        for path in self.paths:
            data = path.read_bytes() if path.is_file() else None
            if path.exists() and data is None:
                raise RuntimeError("Generated settings destination is not a file: " + str(path))
            self.original_bytes[path] = data
            entry = {"path": str(path), "exists": data is not None,
                     "sha256": hashlib.sha256(data).hexdigest() if data is not None else None,
                     "bytes": len(data) if data is not None else 0}
            if data is not None:
                backup = self.archive / (path.parent.name + "-original-GameUserSettings.ini")
                backup.write_bytes(data)
                entry["backup"] = str(backup)
            self.before.append(entry)
        preferred = config / "WindowsEditor/GameUserSettings.ini"
        if self.original_bytes.get(preferred.resolve()) is None:
            preferred = config / "Windows/GameUserSettings.ini"
        self.seed = self.original_bytes.get(preferred.resolve())
        # An absent user INI remains absent. Empty private config lets Unreal
        # merge project/engine defaults without creating the player's file.
        if self.seed is None:
            self.seed = b""
        self._write(self.archive / "before.json", {"schema_version": 1, "originals": self.before,
                    "original_bytes_ever_restored": False,
                    "scope": "Only GameUserSettings generated INIs; other engine config/cache writes are outside this contract."})

    @staticmethod
    def _write(path: Path, value: dict):
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def current(self) -> dict:
        after = [_state(path) for path in self.paths]
        same = all(all(before.get(key) == now.get(key) for key in ("exists", "sha256", "bytes"))
                   for before, now in zip(self.before, after))
        # Also detect a previously absent platform branch newly created by UE.
        known = set(self.paths)
        config = self.repo / "Saved/Config"
        unexpected = sorted(str(path.resolve()) for path in config.glob("*/GameUserSettings.ini")
                            if path.resolve() not in known) if config.exists() else []
        return {"originals_unchanged": same and not unexpected, "originals_before": self.before,
                "originals_after": after, "unexpected_new_user_ini_paths": unexpected,
                "automatic_restore_performed": False}

    def prepare_case(self, folder: Path) -> Path:
        folder = folder.resolve()
        if not folder.is_relative_to(self.output):
            raise RuntimeError("Isolated settings must remain inside this run's output")
        proof = self.current()
        if not proof["originals_unchanged"]:
            self._write(self.archive / "preservation-failure-before-launch.json", proof)
            raise RuntimeError("Original GameUserSettings changed before case launch; refusing to overwrite or restore unknown user changes")
        folder.mkdir(parents=True, exist_ok=True)
        isolated = folder / "isolated-GameUserSettings.ini"
        if isolated.exists():
            raise RuntimeError("A case must not reuse an earlier process's isolated settings")
        isolated.write_bytes(self.seed)
        return isolated

    def finish_case(self, folder: Path, isolated: Path) -> dict:
        proof = self.current()
        proof.update(schema_version=1, check_timing="after_subprocess_return_or_timeout_kill_wait",
                     engine_override_key="GameUserSettingsINI", engine_override_path=str(isolated.resolve()),
                     isolated_seed_sha256=hashlib.sha256(self.seed).hexdigest(),
                     isolated_after=_state(isolated), native_effective_destination_readback_required=True,
                     scope="Parent process byte/existence preservation only; actual GGameUserSettingsIni readback and graphical quality acceptance are separate.")
        self._write(folder.resolve() / "settings-isolation.json", proof)
        self._write(self.archive / "after-latest-case.json", proof)
        return proof

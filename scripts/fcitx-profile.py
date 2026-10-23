#!/usr/bin/env python3
"""Safely enable/disable TiPE in an Fcitx5 profile.

The editor preserves unrelated profile text, writes atomically, and keeps a
small sidecar state file so disable can reverse the exact enable operation.
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, shutil, sys, tempfile, time
from pathlib import Path

ITEM_RE = re.compile(r"^Groups/(\d+)/Items/(\d+)$")
GROUP_RE = re.compile(r"^Groups/(\d+)$")
SECTION_RE = re.compile(r"^\[([^]]+)\]\s*$")
KEY_RE = re.compile(r"^([^#;=\s][^=]*)=(.*)$")

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def atomic_write(path: Path, data: bytes, mode: int | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        if mode is not None:
            os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)
        dfd = os.open(path.parent, os.O_DIRECTORY)
        try: os.fsync(dfd)
        finally: os.close(dfd)
    finally:
        try: os.unlink(tmp)
        except FileNotFoundError: pass

def parse(data: bytes):
    # Fcitx profiles are UTF-8 text; preserve the original newline style.
    text = data.decode("utf-8")
    lines = text.splitlines(keepends=True)
    sections = []
    current = None
    for i, line in enumerate(lines):
        m = SECTION_RE.match(line.rstrip("\r\n"))
        if m:
            current = {"name": m.group(1), "start": i, "end": len(lines), "keys": {}}
            sections.append(current)
        elif current:
            km = KEY_RE.match(line.rstrip("\r\n"))
            if km:
                current["keys"][km.group(1).strip()] = (i, km.group(2))
    for a, b in zip(sections, sections[1:]): a["end"] = b["start"]
    return text, lines, sections

def section_props(lines, sec):
    return {k: v for k, (_i, v) in sec["keys"].items()}

def group_choice(sections):
    groups = []
    items = []
    for s in sections:
        if (m := GROUP_RE.match(s["name"])): groups.append((int(m.group(1)), s))
        if (m := ITEM_RE.match(s["name"])): items.append((int(m.group(1)), int(m.group(2)), s))
    # Prefer a group which already contains TiPE, then the first group with a
    # DefaultIM, then the lowest numbered group.
    for gid, idx, s in items:
        if section_props([], s).get("Name", "").strip().lower() == "tipe": return gid
    for gid, s in groups:
        if "DefaultIM" in s["keys"]: return gid
    return groups[0][0] if groups else 0

def find_tipe(sections):
    for s in sections:
        if ITEM_RE.match(s["name"]):
            name = s["keys"].get("Name", (None, ""))[1].strip().lower()
            if name == "tipe": return s
    return None

def next_item_index(sections, gid):
    vals = [int(m.group(2)) for s in sections if (m := ITEM_RE.match(s["name"])) and int(m.group(1)) == gid]
    return max(vals, default=-1) + 1

def insert_before(lines, index, block):
    # Keep [GroupOrder] at the end where possible; inserting before it avoids
    # disrupting the profile's ordering semantics.
    for i, line in enumerate(lines):
        if line.strip() == "[GroupOrder]":
            lines[i:i] = block
            return
    lines.extend(block)

def enable(profile: Path) -> int:
    if not profile.is_file():
        print(f"找不到 fcitx5 profile: {profile}", file=sys.stderr); return 1
    original = profile.read_bytes()
    _text, lines, sections = parse(original)
    tipe = find_tipe(sections)
    gid = group_choice(sections)
    # If TiPE is already present and active, enabling is a true no-op.
    group = next((s for s in sections if s["name"] == f"Groups/{gid}"), None)
    current_default = group["keys"].get("DefaultIM", (None, ""))[1].strip() if group else ""
    if tipe is not None and current_default.lower() == "tipe":
        print("TiPE 已启用（profile 未修改）。"); return 0

    state_path = profile.with_name(profile.name + ".tipe-state")
    state = None
    if state_path.exists():
        try: state = json.loads(state_path.read_text())
        except Exception: state = None
    backup_path = Path(state["backup"]) if state and state.get("backup") else None
    if backup_path is None or not backup_path.exists():
        backup_path = profile.with_name(profile.name + ".tipe-backup." + time.strftime("%Y%m%d%H%M%S"))
        atomic_write(backup_path, original, profile.stat().st_mode & 0o777)

    inserted = False
    # A malformed/empty profile still gets a valid group before its item.
    if group is None:
        block = [f"[Groups/{gid}]\n", "Name=Default\n", "\n"]
        insert_before(lines, next((i for i,l in enumerate(lines) if l.strip()=="[GroupOrder]"), len(lines)), block)
        _t, lines, sections = parse("".join(lines).encode())
        group = next((s for s in sections if s["name"] == f"Groups/{gid}"), None)
    if tipe is None:
        idx = next_item_index(sections, gid)
        block = [f"[Groups/{gid}/Items/{idx}]\n", "Name=tipe\n", "Layout=\n", "\n"]
        insert_before(lines, next((i for i,l in enumerate(lines) if l.strip()=="[GroupOrder]"), len(lines)), block)
        inserted = True
        # Reparse after insertion to locate the selected group reliably.
        _t, lines, sections = parse("".join(lines).encode())
        group = next((s for s in sections if s["name"] == f"Groups/{gid}"), None)
    # Set DefaultIM in the selected group, preserving its original position.
    changed_default = False
    if group is None:
        return 1
    elif "DefaultIM" in group["keys"]:
        i, _old = group["keys"]["DefaultIM"]
        eol = "\r\n" if lines[i].endswith("\r\n") else "\n"
        lines[i] = f"DefaultIM=tipe{eol}"; changed_default = True
    else:
        # Add after the group header and existing Name/Layout keys.
        pos = group["start"] + 1
        while pos < group["end"] and ("=" in lines[pos] or not lines[pos].strip()): pos += 1
        lines[pos:pos] = ["DefaultIM=tipe\n"]; changed_default = True

    updated = "".join(lines).encode("utf-8")
    if updated == original:
        print("TiPE 已启用（profile 未修改）。"); return 0
    atomic_write(profile, updated, profile.stat().st_mode & 0o777)
    state = {"version": 1, "backup": str(backup_path), "post_sha256": sha(updated),
             "inserted_item": inserted, "group": gid, "prior_default": current_default,
             "prior_had_default": bool(group and "DefaultIM" in group["keys"]),
             "created_at": int(time.time())}
    atomic_write(state_path, (json.dumps(state, ensure_ascii=False, indent=2)+"\n").encode(), 0o600)
    print(f"已加入当前用户 fcitx5 profile。备份：{backup_path}")
    print("请重新加载 fcitx5 后选择 TiPE；脚本未修改 niri 配置。")
    return 0

def disable(profile: Path) -> int:
    if not profile.is_file(): return 0
    state_path = profile.with_name(profile.name + ".tipe-state")
    state = None
    if state_path.exists():
        try: state = json.loads(state_path.read_text())
        except Exception: state = None
    original = profile.read_bytes()
    if state and state.get("backup") and sha(original) == state.get("post_sha256"):
        backup = Path(state["backup"])
        if backup.exists():
            atomic_write(profile, backup.read_bytes(), profile.stat().st_mode & 0o777)
            try: state_path.unlink()
            except FileNotFoundError: pass
            print("已恢复启用 TiPE 前的 fcitx5 profile。")
            return 0
    # Surgical fallback preserves edits made after enable.
    _t, lines, sections = parse(original)
    for tipe in reversed(sections):
        if ITEM_RE.match(tipe["name"]) and tipe["keys"].get("Name", (None, ""))[1].strip().lower() == "tipe":
            # Comments appended after the last item can be unrelated user
            # edits. Remove item keys, keeping those comments verbatim.
            lines[tipe["start"]:tipe["end"]] = [line for line in lines[tipe["start"]:tipe["end"]]
                if line.lstrip().startswith(("#", ";"))]
    _t, lines, sections = parse("".join(lines).encode())
    for group in sections:
        match = GROUP_RE.match(group["name"])
        if not match or "DefaultIM" not in group["keys"]:
            continue
        i, value = group["keys"]["DefaultIM"]
        if value.strip().lower() != "tipe":
            continue
        gid = int(match.group(1))
        remaining = [s["keys"].get("Name", (None, ""))[1].strip()
                     for s in sections if (m := ITEM_RE.match(s["name"])) and int(m.group(1)) == gid]
        prior = state.get("prior_default", "") if state and state.get("group") == gid else ""
        # Remove invalid defaults in every group, also without a sidecar.
        # Preserve other IMs and never select one the user has since removed.
        fallback = prior if prior in remaining else next(
            (name for name in remaining if name and not name.startswith("keyboard-")),
            next((name for name in remaining if name), ""))
        eol = "\r\n" if lines[i].endswith("\r\n") else "\n"
        lines[i] = f"DefaultIM={fallback}{eol}"
    updated = "".join(lines).encode()
    if updated != original: atomic_write(profile, updated, profile.stat().st_mode & 0o777)
    try: state_path.unlink()
    except FileNotFoundError: pass
    print("已从当前用户 fcitx5 profile 移除 TiPE。")
    return 0

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("action", choices=["enable", "disable"]); ap.add_argument("profile", type=Path)
    a = ap.parse_args(); return enable(a.profile) if a.action == "enable" else disable(a.profile)
if __name__ == "__main__": raise SystemExit(main())

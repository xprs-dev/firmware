#!/usr/bin/env python3
"""Did this board get the configuration its sdkconfig.defaults asked for?

A directive in `sdkconfig.defaults` is a REQUEST. Kconfig is free to refuse
it -- the value is out of range, the symbol is a choice member that cannot be
negated, another symbol `select`s it, a dependency is unmet -- and when it
refuses, nothing says so. The generated `sdkconfig.<env>` is what builds.

That has cost this project real bugs, all found on 2026-10-03 on the T-Deck:

  * `ESP_TASK_WDT_TIMEOUT_S=90` was outside Kconfig's `range 1 60`, so it was
    discarded and the default 5 stood, while `ESP_TASK_WDT_PANIC=y` applied.
    A panicking five-second watchdog on a board whose config said ninety.
  * `# CONFIG_BT_NIMBLE_ENABLED is not set` could never work, because the
    symbol is a member of the `BT_HOST` choice and a choice member cannot be
    negated from a defaults file. The file claimed a saving on the strength
    of a line that did nothing.
  * `SPIRAM_IGNORE_NOTFOUND=y` is mutually exclusive with
    `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, which the same file sets.

A divergence that has been understood and accepted is recorded IN the
defaults file, next to the request, so the knowledge does not live here:

    # drift-ok: CONFIG_SPIRAM_IGNORE_NOTFOUND mutually exclusive with the
    #   BSS segment setting below; see docs/esp32.md

Usage: check_sdkconfig_drift.py [models/<board>/firmware ...]
With no arguments it checks every board. Exit 1 on an unrecorded divergence.
"""
import glob
import os
import re
import sys

SET = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
UNSET = re.compile(r"^#\s*(CONFIG_[A-Za-z0-9_]+) is not set\s*$")
OK = re.compile(r"^#\s*drift-ok:\s*(CONFIG_[A-Za-z0-9_]+)\b")

OFF = object()      # a bool that is off, however it was spelled


def read(path):
    """{symbol: value}, where OFF covers `=n`, `is not set` and absence."""
    out, allowed = {}, set()
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = OK.match(line)
            if m:
                allowed.add(m.group(1))
                continue
            m = SET.match(line)
            if m:
                out[m.group(1)] = OFF if m.group(2) == "n" else m.group(2)
                continue
            m = UNSET.match(line)
            if m:
                out[m.group(1)] = OFF
    return out, allowed


def show(v):
    return "off" if v is OFF else ("absent" if v is None else v)


def check(project):
    defaults = os.path.join(project, "sdkconfig.defaults")
    if not os.path.isfile(defaults):
        return [], 0
    built = [b for b in sorted(glob.glob(os.path.join(project, "sdkconfig.*")))
             if not b.endswith(("defaults", ".old", ".bak"))]
    if not built:
        print(f"{project}: no generated sdkconfig yet (build once), skipped")
        return [], 0

    asked, allowed = read(defaults)
    bad, checked = [], 0
    for gen in built:
        got, _ = read(gen)
        for sym, want in asked.items():
            if sym in allowed:
                continue
            checked += 1
            have = got.get(sym)          # None = not mentioned at all
            if want is OFF:
                # Off was asked for. Absent and `is not set` both satisfy it.
                if have is not None and have is not OFF:
                    bad.append((gen, sym, want, have))
            else:
                # A value was asked for. Absence means the symbol does not
                # exist in this build, which is still not what was asked.
                if have != want:
                    bad.append((gen, sym, want, have))
    return bad, checked


def main(argv):
    projects = argv[1:] or sorted(glob.glob("models/*/firmware")) + ["multiboard"]
    bad, checked = [], 0
    for p in projects:
        b, c = check(p)
        bad += b
        checked += c
    if not bad:
        print(f"sdkconfig: {checked} directives checked, every one was taken")
        return 0
    print(f"sdkconfig: {len(bad)} of {checked} directives were asked for and "
          "NOT taken\n")
    for gen, sym, want, have in bad:
        print(f"  {gen}\n    {sym}: asked {show(want)}, built {show(have)}")
    print("\nEach one is a request Kconfig refused, and nothing else would "
          "have told you.\nFix the request, or record it with a `# drift-ok: "
          "<SYMBOL> <why>` line beside it.\nSee docs/esp32.md, \"A directive "
          "asked for is not a directive taken\".")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Turns docks.json into firmware/dock_display/docks_config.h.

docks.json holds up to four screens, each a pair of docks: "home" (where you
pick a bike up) and "dest" (where you drop it off). The designer
(tools/designer) writes it; you can also edit it by hand. Each dock:

    "id":    TfL BikePoints_<id> number. 0 = look it up on the device with "query".
    "query": TfL search text, needed only when id is 0.
    "name":  TfL's full name, for people reading the file. Not sent to the device.
    "label": what the screen shows, e.g. "WENLOCK RD". Printable ASCII, at most
             16 characters (the device font has nothing else).

Run before compiling (tools/flash.sh does it for you):

    python3 tools/docks_json.py            # docks.json -> docks_config.h
    python3 tools/docks_json.py --check    # validate only

Standard library only.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
JSON_PATH = os.path.join(ROOT, "docks.json")
HEADER_PATH = os.path.join(ROOT, "firmware", "dock_display", "docks_config.h")

MAX_SCREENS = 4       # docks.h MAX_SCREENS
LABEL_LEN = 16        # docks.h DOCK_LABEL_LEN
QUERY_LEN = 60
ROLES = ("home", "dest")


def validate(cfg):
    """Returns a list of problems; empty when cfg is good."""
    errors = []
    if not isinstance(cfg, dict) or not isinstance(cfg.get("screens"), list):
        return ['expected {"screens": [...]}']
    screens = cfg["screens"]
    if not 1 <= len(screens) <= MAX_SCREENS:
        errors.append(f"need 1 to {MAX_SCREENS} screens, found {len(screens)}")
    for i, screen in enumerate(screens[:MAX_SCREENS], start=1):
        if not isinstance(screen, dict):
            errors.append(f"screen {i}: not an object")
            continue
        for role in ROLES:
            where = f"screen {i} {role}"
            dock = screen.get(role)
            if not isinstance(dock, dict):
                errors.append(f"{where}: missing")
                continue
            dock_id = dock.get("id", 0)
            if not isinstance(dock_id, int) or isinstance(dock_id, bool) or dock_id < 0:
                errors.append(f"{where}: id must be a whole number, 0 or more")
                dock_id = 0
            query = dock.get("query", "")
            if dock_id == 0:
                if not isinstance(query, str) or not query.strip():
                    errors.append(f"{where}: id is 0, so it needs a query")
                elif len(query) > QUERY_LEN or not _printable(query):
                    errors.append(f"{where}: query must be printable ASCII, at most {QUERY_LEN} characters")
            label = dock.get("label", "")
            if not isinstance(label, str) or not label.strip():
                errors.append(f"{where}: needs a label")
            elif len(label) > LABEL_LEN or not _printable(label):
                errors.append(f"{where}: label must be printable ASCII, at most {LABEL_LEN} characters")
    return errors


def _printable(s):
    return all(" " <= c <= "~" for c in s)


def _c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def header(cfg):
    """C header text for a valid cfg."""
    lines = [
        "// Generated from docks.json by tools/docks_json.py. Don't edit: change",
        "// docks.json (or use tools/designer) and run the script again.",
        "#pragma once",
        '#include "docks.h"',
        "",
        "#define DOCKS_CONFIG_GENERATED 1",
        f"#define NSCREENS {len(cfg['screens'])}",
        "",
        "static const ScreenConfig SCREENS[NSCREENS] = {",
    ]
    for screen in cfg["screens"]:
        refs = []
        for role in ROLES:
            dock = screen[role]
            dock_id = dock.get("id", 0)
            query = dock.get("query", "") if dock_id == 0 else ""
            refs.append(f"{{{dock_id}, {_c_string(query)}, {_c_string(dock['label'])}}}")
        name = f"{screen['home'].get('name') or screen['home']['label']} -> {screen['dest'].get('name') or screen['dest']['label']}"
        name = "".join(c for c in name if " " <= c <= "~")
        lines.append(f"    {{{{{refs[0]}, {refs[1]}}}}}, // {name}")
    lines += ["};", ""]
    return "\n".join(lines)


def write(cfg, json_path=JSON_PATH, header_path=HEADER_PATH):
    """Validates cfg, then writes docks.json and the header. Returns problems."""
    errors = validate(cfg)
    if errors:
        return errors
    with open(json_path, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    with open(header_path, "w") as f:
        f.write(header(cfg))
    return []


def main(argv):
    with open(JSON_PATH) as f:
        try:
            cfg = json.load(f)
        except json.JSONDecodeError as e:
            print(f"docks.json: {e}", file=sys.stderr)
            return 1
    errors = validate(cfg)
    for e in errors:
        print(f"docks.json: {e}", file=sys.stderr)
    if errors:
        return 1
    if "--check" in argv:
        print(f"docks.json: ok, {len(cfg['screens'])} screens")
        return 0
    with open(HEADER_PATH, "w") as f:
        f.write(header(cfg))
    print(f"wrote {os.path.relpath(HEADER_PATH, ROOT)} ({len(cfg['screens'])} screens)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

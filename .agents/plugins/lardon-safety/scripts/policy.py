#!/usr/bin/env python3

import json
import re
import sys

try:
    payload = json.load(sys.stdin)
except Exception:
    sys.exit(0)

event = str(payload.get("event", ""))
tool = str(payload.get("tool_name", ""))
tool_input = payload.get("tool_input") or {}

blob = json.dumps(tool_input, ensure_ascii=False)

def deny(reason):
    print(json.dumps({
        "decision": "block",
        "reason": reason,
    }))
    sys.exit(0)

# Explicitly forbidden areas.
if re.search(r'(^|[/\s"\'"])scan3d(?:/|["\'\s]|$)', blob):
    deny("scan3d/** is outside the Lardon3D agent scope")

if re.search(r'(^|[/\s"\'"])\.git(?:/|["\'\s]|$)', blob):
    deny(".git/** must not be accessed directly")

if event == "PreToolUse" and "shell" in tool:
    command = str(tool_input.get("command", ""))

    forbidden = [
        r'(^|[;&|]\s*|\s)sudo(\s|$)',
        r'(^|[;&|]\s*|\s)doas(\s|$)',
        r'(^|[;&|]\s*|\s)(pacman|yay|paru)(\s|$)',

        r'\bgit\s+add\b',
        r'\bgit\s+commit\b',
        r'\bgit\s+push\b',
        r'\bgit\s+reset\b',
        r'\bgit\s+clean\b',
        r'\bgit\s+checkout\b',
        r'\bgit\s+restore\b',
        r'\bgit\s+rebase\b',
        r'\bgit\s+merge\b',
        r'\bgit\s+switch\b',

        r'(^|[;&|]\s*|\s)rm(\s|$)',
        r'(^|[;&|]\s*|\s)chmod(\s|$)',
        r'(^|[;&|]\s*|\s)chown(\s|$)',
        r'(^|[;&|]\s*|\s)(kill|pkill|killall)(\s|$)',
        r'(^|[;&|]\s*|\s)(poweroff|reboot|shutdown)(\s|$)',
    ]

    for pattern in forbidden:
        if re.search(pattern, command):
            deny(f"blocked by Lardon3D safety policy: {command}")

sys.exit(0)

#!/usr/bin/env python3
"""Validate local file targets in Markdown and simple embedded HTML."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
MARKDOWN_LINK = re.compile(r"!?\[[^\]]*\]\(([^)\n]+)\)")
HTML_LINK = re.compile(r"<(?:a|img)\b[^>]*?\b(?:href|src)=[\"']([^\"']+)[\"']", re.IGNORECASE)
FENCE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE = re.compile(r"`[^`]*`")
SKIP_SCHEMES = {"http", "https", "mailto", "tel", "data"}


def prose(text: str) -> str:
    """Remove fenced and inline code so examples are not treated as links."""
    output: list[str] = []
    fence: str | None = None
    for line in text.splitlines():
        match = FENCE.match(line)
        if match:
            marker = match.group(1)
            if fence is None:
                fence = marker[0]
            elif marker[0] == fence:
                fence = None
            continue
        if fence is None:
            output.append(INLINE_CODE.sub("", line))
    return "\n".join(output)


def destination(raw: str) -> str:
    value = raw.strip()
    if value.startswith("<") and ">" in value:
        return value[1 : value.index(">")]
    return value.split(maxsplit=1)[0]


def main() -> int:
    failures: list[str] = []
    for document in sorted(ROOT.rglob("*.md")):
        text = prose(document.read_text(encoding="utf-8"))
        targets = [destination(match) for match in MARKDOWN_LINK.findall(text)]
        targets.extend(HTML_LINK.findall(text))

        for target in targets:
            parsed = urlsplit(target)
            if not parsed.path or parsed.scheme.lower() in SKIP_SCHEMES or parsed.netloc:
                continue

            relative = Path(unquote(parsed.path))
            candidate = (ROOT / relative.relative_to("/")) if relative.is_absolute() else (document.parent / relative)
            try:
                resolved = candidate.resolve()
                resolved.relative_to(ROOT)
            except (OSError, ValueError):
                failures.append(f"{document.relative_to(ROOT)}: target escapes repository: {target}")
                continue

            if not resolved.exists():
                failures.append(f"{document.relative_to(ROOT)}: missing target: {target}")

    if failures:
        print("Broken local Markdown targets:", file=sys.stderr)
        print("\n".join(f"- {failure}" for failure in failures), file=sys.stderr)
        return 1

    print("All local Markdown targets resolve.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

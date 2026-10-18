#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check that every cross-document link in the docs resolves.

Two questions, one pass: the file a link names has to exist, and the heading it names has to
be in that file. The second is the one that rots silently — a section renamed in one release
breaks every link to it, and nothing notices until someone clicks.
"""
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FILES = ['README.md', 'CONTRIBUTING.md', 'CHANGELOG.md', 'docs/ARCHITECTURE.md',
         'docs/configuration.md', 'docs/web-api.md', 'docs/providers.md',
         '.github/pull_request_template.md']
LINK = re.compile(r'\]\((?!https?://)([^)#\s]*)(#[^)]*)?\)')
HEADING = re.compile(r'^(#+)\s+(.*)$')


def slug(heading):
    text = heading.strip().lower()
    text = re.sub(r'[`*_]', '', text)
    text = re.sub(r'[^a-z0-9 \-]', '', text)
    return text.replace(' ', '-')


def headings(path):
    """Every anchor a reader can land on, from the headings and the explicit HTML ones."""
    found = set()
    for line in pathlib.Path(path).read_text().splitlines():
        match = HEADING.match(line)
        if match:
            found.add(slug(match.group(2)))
        for anchor in re.findall(r'<a id="([^"]+)"', line):
            found.add(anchor)
    return found


def main():
    bad = 0
    for name in FILES:
        base = os.path.dirname(name)
        text = open(name).read()
        for match in LINK.finditer(text):
            target, anchor = match.group(1), match.group(2)
            path = os.path.normpath(os.path.join(base, target)) if target else name
            if not os.path.exists(path):
                print(f'{name}: broken -> {target or "(this file)"}')
                bad += 1
                continue
            if anchor and path.endswith('.md'):
                wanted = anchor[1:]
                if wanted not in headings(path):
                    print(f'{name}: #{wanted} is not a heading of {os.path.basename(path)}')
                    bad += 1
    print('all links resolve' if not bad else f'{bad} broken link(s)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Reject a release whose tag, built firmware and checked-in notes disagree."""
import re
import sys
from pathlib import Path


def validate(tag, build_version, root=Path('.')):
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?', tag):
        raise ValueError('Invalid release tag')
    expected = (root / 'version.txt').read_text().strip()
    if tag != f'v{expected}' or build_version.split('+', 1)[0] != expected:
        raise ValueError('Tag, version.txt and compiled firmware version must match')
    notes = root / 'docs' / 'releases' / f'{tag}.md'
    text = notes.read_text()
    if len(re.findall(r'[\u4e00-\u9fff]', text)) < 100:
        raise ValueError('Release notes must include a detailed Chinese description')
    return notes


if __name__ == '__main__':
    print(validate(sys.argv[1], sys.argv[2]))

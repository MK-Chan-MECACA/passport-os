"""Every character in a C/C++ string literal must exist in the Latin-only body font.

The fonts carry no CJK glyphs (tools/gen_fonts.sh), so a missing character renders
as nothing on the device. Comments are ignored; log strings are checked too so the
firmware stays English-only.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def glyphs(path):
    return {chr(int(m, 16)) for m in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})', path.read_text(encoding='utf8'))}


def main():
    available = glyphs(ROOT / 'main/fonts/buddy_font_16.c') | {'\n', '\t'}
    failures = []
    for top in ('main', 'components', 'bootloader_components'):
        for path in sorted((ROOT / top).rglob('*')):
            if path.suffix not in ('.c', '.cc', '.cpp', '.h', '.inc') or 'fonts' in path.parts:
                continue
            text = path.read_text(encoding='utf8', errors='replace')
            for m in TOKEN.finditer(text):
                literal = m.group()
                if not literal.startswith(('"', 'R"')):
                    continue
                missing = sorted({c for c in literal if ord(c) > 0x7E and c not in available})
                if missing:
                    line = text.count('\n', 0, m.start()) + 1
                    failures.append(f'{path.relative_to(ROOT)}:{line}: {literal} missing {"".join(missing)}')
    for f in failures:
        print('FAIL', f)
    if failures:
        sys.exit(1)
    print('Fonts: every string literal is covered by the Latin-only body font: PASS')


if __name__ == '__main__':
    main()

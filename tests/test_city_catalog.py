"""Validate the full offline payload and every city glyph, without the Internet."""
import gzip
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, 'tools/generate_city_catalog.py', '--check'], cwd=ROOT, check=True)
raw = gzip.decompress((ROOT / 'main/apps/home/cities.json.gz').read_bytes())
rows = json.loads(raw)
assert rows == json.loads((ROOT / 'main/apps/home/cities.json').read_text('utf-8'))
assert len({r[0] for r in rows}) == len(rows)
states = {'Johor', 'Kedah', 'Kelantan', 'Melaka', 'Negeri Sembilan', 'Pahang', 'Penang', 'Perak',
          'Perlis', 'Sabah', 'Sarawak', 'Selangor', 'Terengganu', 'Kuala Lumpur', 'Labuan', 'Putrajaya'}
assert {r[4] for r in rows} == states and all(r[5] == r[4] for r in rows)
assert {'MY'} == {r[6] for r in rows}
for key, maximum in ((1,47),(4,31),(5,47)):
    assert all(len(r[key].encode('utf-8')) <= maximum for r in rows)
assert all(0 <= r[2] <= 8 and 99 <= r[3] <= 120 for r in rows)
glyphs = {int(c,16) for c in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})',
                                     (ROOT/'main/fonts/buddy_font_16.c').read_text('utf-8'))}
missing = {c for r in rows for c in r[1]+r[4]+r[5] if ord(c) not in glyphs}
assert not missing, f'missing city glyphs: {missing}'
kl = rows[0]
assert kl[1:] == ['Kuala Lumpur', 3.1412, 101.6865, 'Kuala Lumpur', 'Kuala Lumpur', 'MY']
george_town = next(r for r in rows if r[1] == 'George Town')
assert george_town[2:6] == [5.4112, 100.3354, 'Penang', 'Penang']
# The phone page's state filter must offer exactly the catalog's states.
page = (ROOT / 'main/apps/home/phone_setup.html').read_text('utf-8')
listed = re.search(r"const provinces=\[([^\]]*)\]", page).group(1)
assert set(re.findall(r"'([^']+)'", listed)) == states
print(f'Offline catalog: {len(rows)} places, all coordinates/ids/profile bounds and city glyphs: PASS')

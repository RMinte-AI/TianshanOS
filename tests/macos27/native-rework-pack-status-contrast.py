"""Sample normal-mode config-pack text against adjacent rendered cell pixels."""
import json
import re
from pathlib import Path
from PIL import Image
root = Path('output/macos27-20260929-native-rework')
rows = []
def luminance(rgb):
    values = [x / 255 for x in rgb]
    values = [x / 12.92 if x <= .04045 else ((x + .055) / 1.055) ** 2.4 for x in values]
    return sum(a * b for a, b in zip(values, [.2126, .7152, .0722]))
for version in ['v15', 'v16', 'v17']:
    data = json.loads((root / f'pack-list-{version}-colors/raw.json').read_text())
    assert data['completed'] and not data['errors'] and not data['writes']
    assert len(data['rows']) == 18
    for record in data['rows']:
        image = Image.open(record['screenshot']).convert('RGB')
        for status in record['statuses'][:2]:
            assert float(status['opacity']) == 1
            cell, rect = status['cell'], status['rect']
            x, y = int(cell['left'] + 6), int((rect['top'] + rect['bottom']) / 2)
            assert 0 <= x < image.width and 0 <= y < image.height
            background = image.getpixel((x, y))
            foreground = tuple(map(int, re.findall(r'\d+', status['color'])[:3]))
            low, high = sorted([luminance(foreground), luminance(background)])
            ratio = (high + .05) / (low + .05)
            rows.append(dict(version=version, language=record['language'], width=record['width'], text=status['text'], foreground=foreground, samplePoint=[x, y], background=background, contrast=ratio, status='PASS' if ratio >= 4.5 else 'FAIL'))
output = root / 'pack-list-v17-colors/contrast.json'
assert not output.exists(), 'Preserve existing evidence'
output.write_text(json.dumps(dict(scope='First valid/invalid status, normal mode, computed opaque text and nearby actual screenshot cell background; not every row, fallback mode or Safari.', rows=rows), ensure_ascii=False, indent=2) + '\n')
for version in ['v15', 'v16', 'v17']:
    selected = [r for r in rows if r['version'] == version]
    print(version, 'samples', len(selected), 'minimum', min(r['contrast'] for r in selected), 'failures', sum(r['status'] == 'FAIL' for r in selected))
assert all(r['status'] == 'PASS' for r in rows if r['version'] == 'v17')

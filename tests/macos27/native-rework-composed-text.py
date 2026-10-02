"""Analyze captured text ranges; no whole-page or control-boundary PASS claim."""
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from PIL import Image


def luminance(rgb):
    values = [v / 255 for v in rgb]
    linear = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in values]
    return sum(v * w for v, w in zip(linear, [0.2126, 0.7152, 0.0722]))


def contrast(fg, bg, alpha):
    ink = luminance([fg[i] * alpha + bg[i] * (1 - alpha) for i in range(3)])
    paper = luminance(bg)
    return (max(ink, paper) + .05) / (min(ink, paper) + .05)


assert abs(contrast([0, 0, 0], [255, 255, 255], 1) - 21) < 1e-9
assert contrast([0, 0, 0], [255, 255, 255], 0) == 1
assert 1 < contrast([0, 0, 0], [255, 255, 255], .5) < 4.5


def parse_color(value):
    match = re.fullmatch(r'rgba?\(([^)]+)\)', value)
    if match and '%' not in match[1]:
        values = [float(v) for v in re.findall(r'[\d.]+', match[1])]
        if len(values) in (3, 4):
            return values[:3], values[3] if len(values) == 4 else 1
    match = re.fullmatch(r'color\(srgb ([\d.]+) ([\d.]+) ([\d.]+)(?: / ([\d.]+))?\)', value)
    if match:
        channels = [float(match[i]) for i in (1, 2, 3)]
        alpha = float(match[4]) if match[4] else 1
        if all(0 <= v <= 1 for v in channels + [alpha]):
            return [v * 255 for v in channels], alpha
    return None


assert parse_color('color(srgb 1 0.5 0 / 0.5)') == ([255, 127.5, 0], 0.5)
assert parse_color('color(srgb 0 0 0)') == ([0, 0, 0], 1)
assert parse_color('color(display-p3 1 0 0)') is None
assert parse_color('color(srgb 2 0 0)') is None

directory = Path(sys.argv[1])
target = directory / 'analysis.json'
if target.exists():
    raise SystemExit('Preserve existing analysis; use a new capture directory')
raw_path = directory / 'raw.json'
raw = json.loads(raw_path.read_text())
if not raw.get('completed') or not raw.get('configurationUnchanged'):
    raise SystemExit('Completed identity-stable collection required')
if raw.get('errors') != [] or not raw.get('rows'):
    raise SystemExit('Error-free nonempty collection required')
results = []
for row in raw['rows']:
    image_path = directory / (row['id'] + '-background.png')
    assert hashlib.sha256(image_path.read_bytes()).hexdigest() == row['screenshots']['background']
    normal_path = directory / (row['id'] + '.png')
    assert hashlib.sha256(normal_path.read_bytes()).hexdigest() == row['screenshots']['normal']
    image = Image.open(image_path).convert('RGB')
    assert row['viewport']['dpr'] == 1
    assert image.size == (row['viewport']['width'], row['viewport']['height'])
    for index, text in enumerate(row['text']):
        finding = {'capture': row['id'], 'index': index, **text}
        reason = None
        if not row['stable'] or text.get('stable') is False:
            reason = 'Text or geometry changed between captures'
        elif text['disabled']:
            reason = 'Disabled control: separate discernibility review required'
        elif text['opacity'] != 1 or text['unsupported']:
            reason = 'Opacity group/filter/blend requires separate composition measurement'
        parsed = parse_color(text['color'])
        if parsed is None:
            reason = 'Unsupported text-fill color representation'
        if reason:
            finding.update(status='INCONCLUSIVE', reason=reason)
            results.append(finding)
            continue
        values, alpha = parsed
        # Sample the entire visible line rectangle, including spaces. This is a
        # conservative bound, not antialiased-glyph pixel contrast.
        colors = set()
        for rect in text['rects']:
            left, top = max(0, math.ceil(rect['x'])), max(0, math.ceil(rect['y']))
            right = min(image.width, math.floor(rect['x'] + rect['width']))
            bottom = min(image.height, math.floor(rect['y'] + rect['height']))
            if right > left and bottom > top:
                colors.update(image.crop((left, top, right, bottom)).getdata())
        if not colors:
            finding.update(status='INCONCLUSIVE', reason='No fully contained background pixels')
        else:
            ratios = [contrast(values[:3], bg, alpha) for bg in colors]
            weight = float(text['fontWeight']) if str(text['fontWeight']).isdigit() else 400
            threshold = 3 if text['fontSize'] >= 24 or (text['fontSize'] >= 18.6667 and weight >= 700) else 4.5
            finding.update(minContrast=min(ratios), maxContrast=max(ratios), threshold=threshold,
                           distinctBackgroundColors=len(colors), status='PASS' if min(ratios) >= threshold else 'FAIL')
        results.append(finding)
summary = {state: sum(r['status'] == state for r in results) for state in ['PASS', 'FAIL', 'INCONCLUSIVE']}
output = {'scope': 'Only recorded visible text nodes. Form values/placeholders, icons, control boundaries, hidden states and other entries remain separate. Screenshot inspection and masking validation required.',
          'rawSha256': hashlib.sha256(raw_path.read_bytes()).hexdigest(),
          'sourceErrors': raw['errors'], 'summary': summary, 'results': results}
target.write_text(json.dumps(output, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(summary))

"""Read-only PDF checks; write a reproducible QA report for the 0.6.2 guides."""
from pathlib import Path
import hashlib
import json
import re
import pdfplumber
import pypdfium2 as pdfium
from PIL import Image, ImageChops
from pypdf import PdfReader

BASE = Path(__file__).resolve().parents[1]
OUT = BASE / 'output/pdf/0.6.2'
QA = BASE / 'tmp/pdfs/guides-062'

def norm(text):
    return re.sub(r'\s+', '', text)

def flatten(items):
    for item in items:
        if isinstance(item, list):
            yield from flatten(item)
        else:
            yield item

def check(role):
    path = OUT / ('TianshanOS Admin User Guide - 中英双语.pdf' if role == 'admin'
                  else 'TianshanOS Root Operations Guide - 中英双语.pdf')
    baseline = json.loads((QA / f'{role}-validation.json').read_text())
    reader = PdfReader(path)
    assert baseline['sha256'] == hashlib.sha256(path.read_bytes()).hexdigest()
    headings = baseline['headings']
    bookmarks = list(flatten(reader.outline))
    assert len(bookmarks) == len(headings)
    for destination, heading in zip(bookmarks, headings):
        page = reader.get_destination_page_number(destination)
        assert destination.title == heading['title']
        assert page + 1 == heading['page']
        assert norm(heading['title']) in norm(reader.pages[page].extract_text())

    links = []
    toc_pages = []
    for index, page in enumerate(reader.pages):
        text = page.extract_text()
        assert '0.6.1' not in text
        assert '2026-10-09' not in text
        assert '\ufffd' not in text and '\x00' not in text
        if index:
            assert 'BASED ON 0.6.2' in text and 'REVISED 2026-10-10' in text
        for ref in page.get('/Annots', []):
            annotation = ref.get_object()
            assert annotation['/Subtype'] == '/Link'
            dest = annotation.get('/Dest')
            assert dest is not None
            target = reader.get_page_number(dest[0].get_object())
            assert 0 <= target < len(reader.pages)
            rect = [float(x) for x in annotation['/Rect']]
            assert 0 <= rect[0] < rect[2] <= float(page.mediabox.width) + 1
            assert 0 <= rect[1] < rect[3] <= float(page.mediabox.height) + 1
            links.append({'toc_page': index + 1, 'target_page': target + 1})
            if index + 1 not in toc_pages:
                toc_pages.append(index + 1)
    # ReportLab emits one title link and one page-number link per TOC entry.
    expected = [h for h in headings if re.match(r'^(TianshanOS|[1-9][0-9]?\. |第一部分|第二部分|Part [IV]+:)', h['title'])]
    assert len(links) == 2 * len(expected)
    for number, heading in enumerate(expected):
        pair = links[number * 2:number * 2 + 2]
        assert all(item['target_page'] == heading['page'] for item in pair)
        toc_text = reader.pages[pair[0]['toc_page'] - 1].extract_text()
        assert norm(heading['title']) in norm(toc_text)

    geometry = []
    with pdfplumber.open(path) as pdf:
        for index, page in enumerate(pdf.pages):
            text_chars = [c for c in page.chars if c['text'].strip()]
            assert text_chars
            outside = [c for c in text_chars if c['x0'] < -1 or c['x1'] > page.width + 1
                       or c['top'] < -1 or c['bottom'] > page.height + 1]
            assert not outside, (role, index + 1, outside[:3])
            body = [c for c in text_chars if 40 < c['top'] < page.height - 40] if index else []
            # CJK layout intentionally hangs final punctuation by at most one glyph.
            hanging = [c for c in body if c['x1'] > page.width - 49]
            assert all(c['text'] in '。！？、，；：）】》”’' and
                       c['x1'] <= page.width - 52 + c['size'] + 1 for c in hanging), (role, index + 1)
            assert all(c['x0'] >= 49 for c in body), (role, index + 1)
            geometry.append({'page': index + 1, 'characters': len(text_chars),
                             'body_bottom': round(max((c['bottom'] for c in body), default=0), 2),
                             'cjk_hanging_punctuation': len(hanging),
                             'out_of_page_characters': len(outside)})

    rendered = pdfium.PdfDocument(str(path))
    for index in range(len(rendered)):
        current = rendered[index].render(scale=1.3).to_pil().convert('RGB')
        inspected = Image.open(QA / role / f'page-{index + 1:02}.png').convert('RGB')
        assert current.size == inspected.size
        assert ImageChops.difference(current, inspected).getbbox() is None

    return {'pages': len(reader.pages), 'inspected_render_matches_current_pdf': True,
            'bookmarks_checked': len(bookmarks),
            'toc_entries_checked': len(expected), 'links_checked': len(links),
            'toc_pages': toc_pages, 'geometry': geometry,
            'source_content_missing': baseline['missing_content'],
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}

if __name__ == '__main__':
    report = {role: check(role) for role in ['admin', 'root']}
    (QA / 'structural-checks.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({role: {k: v for k, v in result.items() if k != 'geometry'}
                      for role, result in report.items()}, ensure_ascii=False, indent=2))

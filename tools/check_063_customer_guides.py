"""Read-only PDF checks; write a reproducible QA report for the 0.6.3 guides."""
from pathlib import Path
import hashlib
import json
import re
import pdfplumber
import pypdfium2 as pdfium
from PIL import Image, ImageChops
from pypdf import PdfReader

BASE = Path(__file__).resolve().parents[1]
OUT = BASE / 'output/pdf/0.6.3-20261028-customer'
QA = BASE / 'tmp/pdfs/guides-063-20261028-customer'

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
    extracted = norm(''.join(page.extract_text() or '' for page in reader.pages))
    required_content = [
        '文件只能上传到 SD 卡，不能上传到 SPIFFS。',
        'Files can only be uploaded to the SD Card, not SPIFFS.',
        '恢复配置不会同时恢复这些文件。',
        'Restoring configuration does not restore those files.',
        '只上传主固件或 WebUI，不能完成这次升级。',
        'Uploading only the firmware or WebUI cannot complete this upgrade.',
        '点击“中止”不会撤销已写入的固件或已经完成的操作。',
        '“Abort” does not undo firmware already written or operations already completed.',
        '标记消失不代表已经保存成功',
        'A cleared error mark does not confirm a successful save',
        '不要立即重复断电、删除、启动任务或升级。',
        'Do not immediately repeat power, delete, task-start, or upgrade operations.',
    ]
    if role == 'root':
        required_content += [
            '不要删除旧指纹来跳过核对。',
            'Do not delete the old fingerprint to skip this check.',
            '后台运行需要 SSH 密钥认证',
            'Background execution requires SSH key authentication',
            '相关模板会保留为停用草稿',
            'retains related templates as disabled drafts',
            '每轮最多 100 次',
            'up to 100 times per run',
            '最多保存 64 条规则',
            'Up to 64 rules can be saved',
            '当前含 Webhook 的签名规则包仍不能导入。',
            'Signed rule packages containing Webhook still cannot be imported.',
            '已有暂停不会自动解除',
            'An existing pause is not lifted',
            '恢复是整套替换，不是合并。',
            'Restore replaces the whole set; it does not merge.',
            '内部副本不会自动跟随更新',
            'The internal copy does not update automatically.',
            '强删不跳过管理权限、配置变化检查或存储写入错误。',
            'Force deletion does not bypass management permissions, configuration-change checks, or storage-write failures.',
        ]
    for required in required_content:
        assert norm(required) in extracted, (role, required)
    for obsolete in ['已保存，重启后生效；当前运行版本未改变。', '只读导入规则不能直接编辑。', '当前动作执行功能尚未实现']:
        assert norm(obsolete) not in extracted, (role, obsolete)
    cover = reader.pages[0].extract_text()
    assert '0.6.3' in cover and '2026-10-28' in cover and 'October 28, 2026' in cover
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
        assert '\ufffd' not in text and '\x00' not in text
        if index:
            assert 'BASED ON 0.6.3' in text and 'REVISED 2026-10-28' in text
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

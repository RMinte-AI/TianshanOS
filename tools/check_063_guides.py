"""Check content, bilingual structure, navigation and rendering of 0.6.3 guides."""
from pathlib import Path
import hashlib
import json
import re
import pdfplumber
import pypdfium2 as pdfium
from PIL import Image, ImageChops
from pypdf import PdfReader

BASE = Path(__file__).resolve().parents[1]
OUT = BASE / 'output/pdf/0.6.3-20261028'
QA = BASE / 'tmp/pdfs/guides-063-20261028'
SOURCE = BASE / 'docs/user-guides/0.6.3'

def norm(text):
    return re.sub(r'[\s“”]', '', text)

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
    for required in [
        '文件只能上传到 SD 卡，不能上传到 SPIFFS。',
        'Files can only be uploaded to the SD Card, not SPIFFS.',
        '点击“收起”可隐藏详情。页面提供 × 时，可点击它关闭提示。',
        'Select “Hide” to hide the details. If a × button is shown, select it to dismiss the message.',
        '不要立即重复断电、删除、启动任务或升级。',
        'Do not immediately repeat power, delete, task-start, or upgrade operations.',
        '标记消失不代表已经保存成功',
        'A cleared error mark does not confirm a successful save',
        '首次从 0.6.2 等旧存储布局升级时，需要发行方确认的配套 USB／串口升级包。',
        'The first upgrade from an older storage layout, such as 0.6.2, needs a matched USB / serial package approved by the publisher.',
        '只上传主固件或 WebUI，不能完成这次升级。',
        'Uploading only the firmware or WebUI cannot complete this upgrade.',
        '不要刷入旧版密码重置固件。',
        'Do not flash an old password-reset image.',
        '挂载成功不等于已改用卡内自动化配置。',
        'Mounting the card does not mean Automation has switched to its configuration.',
        '/sdcard/config/automation',
        '进度达到 100% 或显示文件已写入，不代表重启和版本核对已完成。',
        'A 100% reading or a message that files were written does not confirm a completed restart and version check.',
    ]:
        assert norm(required) in extracted, (role, required)
    cover = reader.pages[0].extract_text()
    assert '0.6.3' in cover and '2026-10-28' in cover and 'October 28, 2026' in cover
    assert len(reader.pages[0].images) == 1
    embedded = reader.pages[0].images[0].image.convert('RGB')
    banner = Image.open(BASE / 'assets/tianshanos-banner-en.png').convert('RGB')
    assert embedded.size == banner.size
    assert ImageChops.difference(embedded, banner).getbbox() is None
    for obsolete in ['已保存，待重启', '重启后生效', '导入的规则包为只读', '备用副本未同步',
                     'Imported rule packages are read-only', 'Saved; restart required',
                     'backup copy is not synced', 'Run the current, older version',
                     'Go to Automation', '前往自动化']:
        assert norm(obsolete) not in extracted, (role, obsolete)
    if role == 'admin':
        assert norm('仍无法确认时，请 root 用户检查。') in extracted
        assert norm('Ask a root user to investigate if the result remains unclear.') in extracted
        assert norm('需要核验、查看日志或停止后台服务时，请 root 用户处理。') in extracted
        assert norm('Ask a root user to verify, view logs, or stop a background service.') in extracted
    else:
        for required in [
            '连接前核对主机指纹', 'Check the Host Fingerprint Before Connecting',
            '不要删除旧指纹来跳过核对。', 'Do not delete the old fingerprint to skip this check.',
            '读取失败不等于日志没有内容，也不代表进程已经退出。',
            'A failed read does not mean the log is empty or the process has exited.',
            '仍无法确认时，在“终端”打开“系统日志”查看原因。',
            'If the result remains unclear, open “System Logs” on Terminal to investigate.',
            '以下步骤适用于网页“安全”中的 SSH 主机测试',
            'These steps apply to SSH host tests on the web “Security” page',
            '“最终强制删除”', '“Force delete”',
            '“重新检查删除影响”', '“Review deletion impact”',
            '导入后的规则可以编辑、启停和删除。',
            'Imported rules can be edited, enabled, disabled, and deleted.',
            '最多保存 64 条规则。', 'You can save up to 64 rules.',
            '每轮最多 100 次', 'up to 100 times per run',
            '“保存应急副本”', '“Save emergency backup”',
            '“从应急副本恢复”', '“Restore emergency backup”',
            '“将当前配置保存到 SD 卡并使用”', '“Save current configuration to SD card and use it”',
            '列表“刷新”只读取已知记录，不代替核验。',
            '“Refresh” only reads known records; it does not replace verification.',
            '后台运行需要 SSH 密钥认证。', 'Background runs require SSH key authentication.',
            '不要先保存应急副本，否则会覆盖准备恢复的旧副本。',
            'Do not save an emergency backup first; that would overwrite the old copy you intend to restore.',
            '重启也不会替你完成这次选择。', 'Restarting does not make the choice for you.',
            '“已包含当前修改”', '“Includes the current changes”',
            '私钥、图片、字体、日志或远端运行状态',
            'private keys, images, fonts, logs, or remote run states',
            'auto --config status', 'ssh --host <host> --user <user> --shell',
        ]:
            assert norm(required) in extracted, (role, required)
        assert norm('请 root 用户') not in extracted
        assert norm('Ask a root user') not in extracted
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
        assert '2026-10-10' not in text
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
    for part in ['shared', 'root']:
        structures = []
        for lang in ['zh', 'en']:
            lines = (SOURCE / f'{part}.{lang}.md').read_text().splitlines()
            structures.append([len(re.match(r'^(#+)', line)[0]) if line.startswith('#')
                               else 'row' if line.startswith('|')
                               else 'list' if re.match(r'^(- |\d+\. )', line)
                               else 'paragraph' for line in lines if line.strip()])
        assert structures[0] == structures[1], (part, 'bilingual structure mismatch')
    report = {role: check(role) for role in ['admin', 'root']}
    (QA / 'structural-checks.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({role: {k: v for k, v in result.items() if k != 'geometry'}
                      for role, result in report.items()}, ensure_ascii=False, indent=2))

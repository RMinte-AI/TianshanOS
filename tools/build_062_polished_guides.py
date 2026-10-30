"""Build and verify the 0.6.2 bilingual user manuals from shared editable sources."""
from pathlib import Path
from html import escape
import re
import json
import hashlib
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import BaseDocTemplate, PageTemplate, Frame, Paragraph, Spacer, PageBreak, Table, TableStyle, KeepTogether, Flowable, NextPageTemplate
from reportlab.platypus.tableofcontents import TableOfContents
from pypdf import PdfReader
import pypdfium2 as pdfium
from PIL import Image, ImageDraw

BASE = Path(__file__).resolve().parents[1]
SOURCE = BASE / 'docs/user-guides/0.6.2-polished'
OUT = BASE / 'output/pdf/0.6.2-20261013-banner'
QA = BASE / 'tmp/pdfs/guides-062-20261013-banner'
W, H = A4
M = 52
CW = W - M * 2
NAVY = colors.HexColor('#16334A')
TEAL = colors.HexColor('#176B79')
INK = colors.HexColor('#24313B')
GRAY = colors.HexColor('#526374')
RULE = colors.HexColor('#CCD9DF')
PALE = colors.HexColor('#F0F5F7')
NOTICE_ZH = '本指南依据 TianshanOS 0.6.2 编写。后续版本中未变化的功能和操作，仍可参考本指南。若界面、提示或操作结果不同，请查阅所用版本的说明。'
NOTICE_EN = 'This guide is based on TianshanOS 0.6.2. It can still be used with later versions where features and steps remain unchanged. If controls, messages, or results differ, check the guidance for your installed version.'
NAMES = {'admin': 'TianshanOS Admin User Guide - 中英双语.pdf', 'root': 'TianshanOS Root Operations Guide - 中英双语.pdf'}

def register_fonts():
    paths = {'Chinese': '/System/Library/Fonts/STHeiti Light.ttc', 'ChineseBold': '/System/Library/Fonts/STHeiti Medium.ttc', 'Latin': '/System/Library/Fonts/Supplemental/Arial.ttf', 'LatinBold': '/System/Library/Fonts/Supplemental/Arial Bold.ttf', 'Mono': '/System/Library/Fonts/Supplemental/Courier New.ttf'}
    for name, path in paths.items():
        pdfmetrics.registerFont(TTFont(name, path))
    pdfmetrics.registerFontFamily('Chinese', normal='Chinese', bold='ChineseBold')
    pdfmetrics.registerFontFamily('Latin', normal='Latin', bold='LatinBold')

def markup(s, zh=True, bold=False):
    s = s.replace('–', '-').replace('‑', '-')
    output = []
    for part in re.split(r'(`[^`]+`)', s):
        code = part.startswith('`') and part.endswith('`')
        if code:
            part = part[1:-1]
        for run in re.findall(r'[\x20-\x7e]+|[^\x20-\x7e]+', part) if zh or code else [part]:
            font = ('Mono' if run.isascii() else 'Chinese') if code else ('Chinese' if zh and not run.isascii() else 'Latin')
            if bold and font in ('Chinese', 'Latin'):
                font += 'Bold'
            output.append(f'<font name="{font}">{escape(run)}</font>')
    return ''.join(output)

def styles(zh):
    base = dict(fontName='Chinese' if zh else 'Latin', fontSize=11.1, leading=18.5 if zh else 16.4, textColor=INK, wordWrap='CJK' if zh else None, spaceAfter=8, allowWidows=0, allowOrphans=0)
    st = {'body': ParagraphStyle('body', **base)}
    st['list'] = ParagraphStyle('list', parent=st['body'], leftIndent=18, bulletIndent=0, bulletFontName='Latin', bulletFontSize=10, spaceAfter=5)
    for key, size, lead, before in [('language', 23, 31, 0), ('part', 17.5, 25, 20), ('chapter', 17.5, 25, 20), ('section', 13.1, 20, 15), ('minor', 11.6, 18, 10)]:
        st[key] = ParagraphStyle(key, fontName='ChineseBold' if zh else 'LatinBold', fontSize=size, leading=lead, textColor=NAVY if key in ('language', 'chapter') else TEAL, spaceBefore=before, spaceAfter=9, keepWithNext=True, wordWrap='CJK' if zh else None)
    st['table'] = ParagraphStyle('table', parent=st['body'], fontSize=10.2, leading=16.5 if zh else 14.8, spaceAfter=0)
    st['notice'] = ParagraphStyle('notice', parent=st['body'], fontSize=10.1, leading=16 if zh else 14.5, textColor=GRAY)
    return st

def para(s, style, zh, bullet=None, bold=False):
    return Paragraph(markup(s, zh, bold), style, bulletText=bullet)

class Cover(Flowable):
    width = W
    height = H
    def __init__(self, role):
        super().__init__()
        self.width, self.height, self.role = W, H, role
    def draw(self):
        c = self.canv
        c.setFillColor(colors.HexColor('#F6F8FA'))
        c.rect(0, 0, W, H, stroke=0, fill=1)
        c.drawImage(str(BASE / 'assets/tianshanos-banner-en.png'), 0, H-W/2, width=W, height=W/2)
        c.setFillColor(NAVY)
        c.rect(0, H-W/2-40, W, 40, stroke=0, fill=1)
        c.setFont('LatinBold', 11)
        c.setFillColor(colors.white)
        c.drawString(M, H-W/2-26, self.role.upper() + '  /  USER DOCUMENTATION')
        c.setFillColor(NAVY)
        c.setFont('LatinBold', 29)
        c.drawString(M, 444, 'TianshanOS')
        c.setFont('ChineseBold', 29)
        c.drawString(M, 400, '日常使用指南' if self.role == 'admin' else '运维指南')
        c.setFont('Latin', 17)
        c.drawString(M, 364, 'Admin User Guide' if self.role == 'admin' else 'Root Operations Guide')
        p = para('编写依据版本：0.6.2  /  Based on version 0.6.2', styles(True)['body'], True, bold=True)
        p.wrap(CW, 40)
        p.drawOn(c, M, 317)
        c.setStrokeColor(TEAL)
        c.setLineWidth(2)
        c.line(M, 294, M+54, 294)
        p = para(NOTICE_ZH, styles(True)['notice'], True)
        _, ph = p.wrap(CW, 200)
        p.drawOn(c, M, 276-ph)
        q = para(NOTICE_EN, styles(False)['notice'], False)
        _, qh = q.wrap(CW, 200)
        q.drawOn(c, M, 262-ph-qh)
        c.setFont('Chinese', 9)
        c.setFillColor(GRAY)
        c.drawString(M, 82, '中英双语 · 修订日期：2026-10-13')
        c.setFont('Latin', 9)
        c.drawRightString(W-M, 82, 'Chinese / English · October 13, 2026')
        c.setStrokeColor(RULE)
        c.setLineWidth(.5)
        c.line(M, 64, W-M, 64)

class Doc(BaseDocTemplate):
    def __init__(self, path, role):
        super().__init__(str(path), pagesize=A4, leftMargin=M, rightMargin=M, topMargin=54, bottomMargin=49, title='TianshanOS '+role+' Guide | 0.6.2', author='RMinte', pageCompression=1)
        self.role, self.headings = role, []
        self.addPageTemplates([
            PageTemplate('cover', [Frame(0, 0, W, H, leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)], onPage=lambda c,d:None),
            PageTemplate('body', [Frame(M, 49, CW, H-103, leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)], onPage=self.page_header)
        ])
    def beforeDocument(self):
        self.headings = []
    def page_header(self, c, doc):
        c.setFont('Latin', 8)
        c.setFillColor(GRAY)
        c.drawString(M, H-30, 'TianshanOS | '+self.role.upper())
        c.drawRightString(W-M, H-30, 'BASED ON 0.6.2  ·  REVISED 2026-10-13')
        c.setStrokeColor(RULE)
        c.setLineWidth(.5)
        c.line(M, H-38, W-M, H-38)
        c.line(M, 36, W-M, 36)
        c.drawString(M, 23, 'TianshanOS Documentation')
        c.drawRightString(W-M, 23, str(doc.page))
    def afterFlowable(self, f):
        if not hasattr(f, 'anchor'):
            return
        self.canv.bookmarkPage(f.anchor)
        self.canv.addOutlineEntry(f.getPlainText(), f.anchor, level=f.outline, closed=True)
        self.headings.append({'title': f.getPlainText(), 'page': self.page})
        if f.toc is not None:
            self.notify('TOCEntry', (f.toc, f.getPlainText(), self.page, f.anchor))

def assemble(role, lang):
    zh = lang == 'zh'
    common = (SOURCE / f'shared.{lang}.md').read_text()
    if role == 'admin':
        intro = '# TianshanOS admin 日常使用指南\n\n本指南面向使用 admin 账户的用户，介绍系统查看、设备控制、网络、文件和固件升级。终端、远程指令与自动化配置由 root 运维指南说明；安全管理请参阅单独的安全指南。' if zh else '# TianshanOS Admin User Guide\n\nThis guide covers system checks, device controls, networking, files, and firmware updates for the admin account. The Root Operations Guide covers terminal access, remote commands, and Automation configuration. See the separate Security Guide for security management.'
    else:
        intro = '# TianshanOS root 运维指南\n\n本指南面向使用 root 账户的用户。第一部分包含日常功能，第二部分介绍终端、远程指令与自动化。root 操作可能影响设备和远程主机；执行前确认目标和正在运行的任务。安全管理请参阅单独的安全指南。' if zh else '# TianshanOS Root Operations Guide\n\nThis guide covers routine functions in Part I and terminal access, remote commands, and Automation in Part II. Root operations can affect the device and remote hosts; check the target and active tasks before proceeding. See the separate Security Guide for security management.'
        if zh:
            common = common.replace('3. 用户名保持为 `admin`，输入设备提供的 admin 密码。', '3. 输入 `root` 和设备提供的 root 密码。')
            common = common.replace('admin 日常使用的主要入口如下：', 'root 可使用以下入口：')
            common = common.replace('- “安全”：', '- “终端”：执行设备命令并查看系统日志。\n- “指令”：管理和运行远程 SSH 指令。\n- “自动化”：配置数据源、规则和动作模板。\n- “安全”：')
            common = common.replace('长时间没有变化时，刷新页面或联系 root 用户检查。', '长时间没有变化时，可点击“前往自动化”，并在“终端”的“系统日志”查看原因。')
            common = common.replace('联系 root 用户处理。不要反复点击启动。', '点击“前往自动化”，检查状态；在“终端”打开“系统日志”查找原因。恢复配置后再使用卡片，不要反复点击启动。')
            common = common.replace('如需添加，请联系 root 用户检查规则的“显示在面板”设置。', '可点击“前往自动化”，新建规则或检查已有规则的“显示在面板”设置。')
            common = common.replace('先请 root 用户确认当前使用的版本，再执行。', '先核对规则列表中的当前运行版本，再执行。')
            common = common.replace('或请 root 用户检查规则、远程指令和主机连接。', '或检查规则、远程指令和主机连接，方法见第 11、12 章。')
            common = common.replace('规则配置包请交给 root 用户，从“自动化”页面导入。', '规则配置包请从“自动化”页面导入，步骤见第 12 章。')
            common = common.replace('先请 root 用户确认可以卸载。', '先确认配置和正在运行的任务允许卸载。')
            common = common.replace('请 root 用户或设备提供方确认配置兼容性', '请先确认配置兼容性，必要时联系设备提供方')
            common = common.replace('请 root 用户或设备提供方确认旧固件能否读取这些配置', '请确认旧固件能否读取这些配置，必要时联系设备提供方')
            common = common.replace('保留提示和详情，交给 root 用户检查。', '保留提示和详情，在“终端”打开“系统日志”检查。')
            common = common.replace('需要求助时，保留提示和详情', '需要进一步检查时，保留提示和详情')
            common = common.replace('仍无法确认时，请 root 用户检查。', '仍无法确认时，在“终端”打开“系统日志”查看原因。')
            common = common.replace('请 root 用户检查规则设置', '在“自动化”检查规则设置（见第 12 章）')
            common = common.replace('请 root 用户检查主机连接。', '在“安全”检查主机连接（见第 11 章）。')
            common = common.replace('请 root 用户核对规则版本和待重启提示。', '在“自动化”核对规则版本和待重启提示（见第 12 章）。')
            common = common.replace('请 root 用户检查引擎和配置状态，不要自行重启设备。', '在“自动化”检查引擎和配置状态。按提示处理；重启前检查正在运行的任务。')
        else:
            common = common.replace('3. Keep `admin` as the username and enter the admin password supplied with the device.', '3. Enter `root` and the root password supplied with the device.')
            common = common.replace('The main admin pages are:', 'Root users can access these pages:')
            common = common.replace('- “Security”:', '- “Terminal”: Run device commands and view system logs.\n- “Commands”: Manage and run remote SSH commands.\n- “Automation”: Configure sources, rules, and templates.\n- “Security”:')
            common = common.replace('If the message persists, refresh the page or ask a root user to investigate.', 'If the message persists, select “Go to Automation” and check System Logs in Terminal.')
            common = common.replace('Ask a root user for help. Do not keep selecting start.', 'Select “Go to Automation” to check status. Open “System Logs” on Terminal to find the cause. Restore configuration before using the cards; do not keep selecting start.')
            common = common.replace('If you need one, ask a root user to check the rules’ “Show on panel” setting.', 'Select “Go to Automation” to create a rule or check an existing rule’s “Show on panel” setting.')
            common = common.replace('ask a root user to confirm which version is in use before running it.', 'check the running version in the rule list before using it.')
            common = common.replace('or ask a root user to check the rule, remote command, and host connection.', 'or check the rule, remote command, and host connection as described in Chapters 11 and 12.')
            common = common.replace('ask a root user to confirm that it is safe to unmount.', 'confirm that the configuration and running tasks allow unmounting.')
            common = common.replace('Ask a root user to confirm that unmounting is safe.', 'Confirm that configuration and running tasks allow unmounting.')
            common = common.replace('ask a root user or the device provider to confirm compatibility', 'confirm compatibility, consulting the device provider if necessary,')
            common = common.replace('Give rule packages to a root user to import on Automation.', 'Import rule packages on Automation, as described in Chapter 12.')
            common = common.replace('ask a root user or the device provider whether it can read these configurations', 'check whether it can read these configurations, consulting the device provider if needed')
            common = common.replace('keep a record of the message and details for a root user to check.', 'keep a record of the message and details, then open “System Logs” on Terminal to investigate.')
            common = common.replace('When asking for help, keep a record of the message and details', 'For further checks, keep a record of the message and details')
            common = common.replace('Ask a root user to investigate if the result remains unclear.', 'If the result remains unclear, open “System Logs” on Terminal to investigate.')
            common = common.replace('Ask a root user to check the rule settings.', 'Check the rule settings on “Automation” (Chapter 12).')
            common = common.replace('ask a root user to check the host connection.', 'check the host connection on “Security” (Chapter 11).')
            common = common.replace('Ask a root user to check the rule version and pending-restart message.', 'Check the rule version and pending-restart message on “Automation” (Chapter 12).')
            common = common.replace('Ask a root user to check the engine and configuration state. Do not restart the device yourself.', 'Check the engine and configuration state on “Automation.” Follow the message; check active tasks before restarting the device.')
        # Part headings are level 2; chapters and their subheadings move down one.
        common = re.sub(r'^(#{2,4}) ', r'\1# ', common, flags=re.M)
        root_only = re.sub(r'^(#{2,4}) ', r'\1# ', (SOURCE/f'root.{lang}.md').read_text(), flags=re.M)
        common = ('## 第一部分：日常功能\n\n' if zh else '## Part I: Routine Functions\n\n') + common + ('\n## 第二部分：root 专属功能\n\n' if zh else '\n## Part II: Root-only Functions\n\n') + root_only
    orientation = '按设备页面显示的功能操作。部分选项需要指定硬件或配置才会出现。电脑上将指针停在图标按钮上，可查看按钮名称。' if zh else 'Use the controls shown on your device. Some need specific hardware or configuration. On a computer, hover over an icon button to see its name.'
    return intro+'\n\n'+orientation+'\n\n'+common.strip()+'\n'

def story_for(text, role):
    lines = text.splitlines()
    story, seq, i, zh = [], 0, 0, True
    st = styles(zh)
    while i < len(lines):
        s = lines[i].strip()
        i += 1
        if not s:
            continue
        match = re.match(r'^(#{1,5}) (.*)', s)
        if match:
            level, title = len(match[1]), match[2]
            if level == 1:
                if seq:
                    story.append(PageBreak())
                zh = bool(re.search(r'[\u4e00-\u9fff]', title))
                st = styles(zh)
            kind = 'language' if level == 1 else 'part' if role == 'root' and level == 2 else 'chapter' if level == (3 if role == 'root' else 2) else 'section' if level == (4 if role == 'root' else 3) else 'minor'
            seq += 1
            p = para(title, st[kind], zh, bold=True)
            p.anchor, p.outline = f'{role}-{seq}', level-1
            p.toc = level-1 if kind in ('language', 'part', 'chapter') else None
            story.append(p)
            continue
        if s.startswith('```'):
            code = []
            while i < len(lines) and not lines[i].startswith('```'):
                code.append(lines[i]); i += 1
            i += 1
            t = Table([[para(l, st['table'], zh)] for l in code], colWidths=[CW], hAlign='LEFT')
            t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,-1),PALE),('BOX',(0,0),(-1,-1),.5,RULE),('LEFTPADDING',(0,0),(-1,-1),10),('TOPPADDING',(0,0),(-1,-1),8),('BOTTOMPADDING',(0,0),(-1,-1),8)]))
            story.extend([KeepTogether([t]), Spacer(1,9)]); continue
        if s.startswith('|'):
            raw = [s]
            while i < len(lines) and lines[i].strip().startswith('|'):
                raw.append(lines[i].strip()); i += 1
            rows = []
            for l in raw:
                cells = l.strip('|').split('|')
                if all(re.fullmatch(r'\s*:?-+:?\s*', x) for x in cells):
                    continue
                rows.append([para(x.strip(), st['table'], zh, bold=not rows) for x in cells])
            t = Table(rows, colWidths=[CW*.29, CW*.71], repeatRows=1, hAlign='LEFT')
            t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),colors.HexColor('#DCEAF0')),('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.white,PALE]),('LINEBELOW',(0,0),(-1,0),.8,TEAL),('LINEBELOW',(0,1),(-1,-1),.4,RULE),('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),9),('RIGHTPADDING',(0,0),(-1,-1),9),('TOPPADDING',(0,0),(-1,-1),8),('BOTTOMPADDING',(0,0),(-1,-1),8)]))
            story.extend([KeepTogether([t]), Spacer(1,10)]); continue
        m = re.match(r'^(\d+\.|-) (.*)', s)
        if m:
            items = []
            while m:
                items.append(para(m[2], st['list'], zh, '•' if m[1] == '-' else m[1]))
                if i >= len(lines):
                    break
                m = re.match(r'^\s*(\d+\.|-) (.*)', lines[i])
                if m:
                    i += 1
            height = sum(p.wrap(CW-18, 1000)[1]+p.getSpaceAfter() for p in items)
            story.extend([KeepTogether(items)] if height < 260 else items)
            story.append(Spacer(1,3)); continue
        story.append(KeepTogether([para(s, st['body'], zh)]))
    # Keep a short introduction with the list or table it introduces.
    joined, i = [], 0
    while i < len(story):
        current = story[i]
        if isinstance(current, KeepTogether) and len(current._content) == 1 and isinstance(current._content[0], Paragraph):
            first = current._content[0]
            following = story[i+1] if i+1 < len(story) else None
            if first.getPlainText().rstrip().endswith((':', '：')) and isinstance(following, KeepTogether):
                joined.append(KeepTogether(current._content + following._content))
                i += 2
                continue
        joined.append(current)
        i += 1
    story = joined
    result, i = [], 0
    while i < len(story):
        f = story[i]
        if not hasattr(f, 'anchor'):
            result.append(f); i += 1; continue
        cluster = []
        while i < len(story) and hasattr(story[i], 'anchor'):
            cluster.append(story[i]); i += 1
        if i < len(story) and not isinstance(story[i], PageBreak):
            content = story[i]
            cluster.extend(content._content if isinstance(content, KeepTogether) else [content]); i += 1
        result.append(KeepTogether(cluster))
    return result

def normalize(s):
    return re.sub(r'\s+|`', '', s).replace('–', '-').replace('‑', '-')

def verify_and_render(path, text, doc, role):
    reader = PdfReader(path)
    extracted = ''.join(p.extract_text() or '' for p in reader.pages)
    missing = []
    for l in text.splitlines():
        s = l.strip()
        if not s or s.startswith('```'):
            continue
        if s.startswith('|'):
            cells = [x.strip() for x in s.strip('|').split('|') if not re.fullmatch(r'\s*:?-+:?\s*',x)]
        else:
            cells = [re.sub(r'^(#{1,5}\s+|\d+\.\s+|-\s+)', '', s)]
        for cell in cells:
            if normalize(cell) not in normalize(extracted):
                missing.append(cell)
    def outline(items):
        for item in items:
            if isinstance(item, list):
                yield from outline(item)
            else:
                yield item
    dest = [reader.get_destination_page_number(x) for x in outline(reader.outline)]
    assert all(0 <= p < len(reader.pages) for p in dest)
    assert all((p.extract_text() or '').strip() for p in reader.pages)
    assert normalize(NOTICE_ZH) in normalize(reader.pages[0].extract_text())
    assert normalize(NOTICE_EN) in normalize(reader.pages[0].extract_text())
    report = {'pages':len(reader.pages),'missing_content':missing,'bookmarks':len(dest),'links':sum(len(p.get('/Annots',[])) for p in reader.pages),'headings':doc.headings,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    (QA/f'{role}-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
    assert not missing, missing[:8]
    folder = QA/role
    folder.mkdir(parents=True,exist_ok=True)
    pdf = pdfium.PdfDocument(str(path))
    images = []
    for i in range(len(pdf)):
        img = pdf[i].render(scale=1.3).to_pil().convert('RGB')
        target = folder/f'page-{i+1:02}.png'
        img.save(target); images.append(target)
    for start in range(0,len(images),4):
        sheet = Image.new('RGB',(1004,1450),'#E5EAF0')
        draw = ImageDraw.Draw(sheet)
        for j,path in enumerate(images[start:start+4]):
            im = Image.open(path)
            im.thumbnail((480,680))
            x,y = 10+(j%2)*499, 10+(j//2)*720
            sheet.paste(im,(x,y)); draw.text((x,y+685),str(start+j+1),fill='black')
        sheet.save(QA/f'{role}-contact-{start//4+1}.png')
    print(role, 'pages',len(reader.pages),'missing',len(missing),'bookmarks',len(dest),'links',report['links'])
    return report

def main():
    register_fonts()
    OUT.mkdir(parents=True,exist_ok=True)
    QA.mkdir(parents=True,exist_ok=True)
    reports = {}
    for role, name in NAMES.items():
        text = assemble(role,'zh')+'\n'+assemble(role,'en')
        (OUT/f'{role}.md').write_text(text)
        toc = TableOfContents()
        toc.levelStyles = [ParagraphStyle(f'toc{i}',fontName='Chinese',fontSize=10.2 if i<2 else 9.9,leading=16.8,spaceBefore=4,leftIndent=i*12,textColor=NAVY,wordWrap='CJK') for i in range(3)]
        flow = [Cover(role),NextPageTemplate('body'),PageBreak(),para('目录 / Contents',styles(True)['language'],True,bold=True),para('点击章节名称即可跳转。 / Select a chapter to jump to it.',styles(True)['body'],True),Spacer(1,10),toc,PageBreak()]
        flow.extend(story_for(text,role))
        doc = Doc(OUT/name,role)
        doc.multiBuild(flow)
        reports[role] = verify_and_render(OUT/name,text,doc,role)
    (QA/'summary.json').write_text(json.dumps(reports,ensure_ascii=False,indent=2))

if __name__ == '__main__':
    main()

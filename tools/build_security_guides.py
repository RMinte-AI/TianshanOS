#!/usr/bin/env python3
"""Build the bilingual, user-facing Security Page guides for TianShanOS 0.6.1."""

import html
import re
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib import textsplit
from reportlab.platypus import paragraph as paragraph_layout
from reportlab.platypus import (
    BaseDocTemplate, CondPageBreak, Frame, NextPageTemplate, PageBreak, PageTemplate,
    Paragraph, Spacer, Table, TableStyle,
)
from reportlab.platypus.tableofcontents import TableOfContents

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'docs/guides/security'
OUTPUT = ROOT / 'output/pdf'
NAVY = colors.HexColor('#102A43')
INK = colors.HexColor('#243B53')
TEAL = colors.HexColor('#168C8C')
MUTED = colors.HexColor('#627D98')
LINE = colors.HexColor('#D9E2EC')
WIDTH = A4[0] - 44 * mm
VERSION = '0.6.1'
REVIEW_DATE = '2026-10-09'
GUIDES = {
    'zh': ('TianShanOS-安全页面完整操作指南-中文.pdf', '安全页面完整操作指南'),
    'en': ('TianShanOS-Complete-Security-Page-Guide-English.pdf', 'Complete Guide to the Security Page'),
}
COVER_NOTE = {
    'zh': '本指南依据 TianShanOS 0.6.1 的功能与界面编写。升级到后续版本后，未发生变化的功能仍可参照本指南；版本号不同并不意味着本指南全部失效。若界面、操作步骤或结果提示发生变化，请结合当前版本说明确认后再操作。',
    'en': 'This guide describes the features and interface in TianShanOS 0.6.1. It may remain useful for later versions where those features are unchanged. A different version number does not automatically make the guide obsolete. If the interface, steps, or result messages differ, check the release notes for your installed version before proceeding.',
}


def fonts():
    # ReportLab's Japanese defaults omit several common Chinese closing marks.
    textsplit.ALL_CANNOT_START += '，；：！？）》】”’'
    paragraph_layout.ALL_CANNOT_START = textsplit.ALL_CANNOT_START
    entries = [
        ('Hei', '/System/Library/Fonts/STHeiti Light.ttc'),
        ('HeiBold', '/System/Library/Fonts/STHeiti Medium.ttc'),
        ('ArialGuide', '/System/Library/Fonts/Supplemental/Arial.ttf'),
        ('ArialGuideBold', '/System/Library/Fonts/Supplemental/Arial Bold.ttf'),
        ('MonoGuide', '/System/Library/Fonts/Supplemental/Courier New.ttf'),
    ]
    for name, path in entries:
        pdfmetrics.registerFont(TTFont(name, path, subfontIndex=0))
    for normal, bold in [('Hei', 'HeiBold'), ('ArialGuide', 'ArialGuideBold')]:
        pdfmetrics.registerFontFamily(normal, normal=normal, bold=bold, italic=normal, boldItalic=bold)


def markup(text):
    parts = re.split(r'(`[^`]+`|\*\*[^*]+\*\*)', text.strip())
    result = []
    for part in parts:
        if part.startswith('`') and part.endswith('`'):
            result.append('<font name="MonoGuide" color="#7A3E00">' + html.escape(part[1:-1]) + '</font>')
        elif part.startswith('**') and part.endswith('**'):
            result.append('<b>' + html.escape(part[2:-2]) + '</b>')
        else:
            result.append(html.escape(part))
    return ''.join(result)


def styles(lang):
    normal = 'Hei' if lang == 'zh' else 'ArialGuide'
    bold = 'HeiBold' if lang == 'zh' else 'ArialGuideBold'
    common = dict(fontName=normal, wordWrap='CJK' if lang == 'zh' else None, textColor=INK)
    s = {
        'body': ParagraphStyle('body', fontSize=10.5, leading=17, spaceAfter=3 * mm,
                               allowOrphans=0, allowWidows=0, alignment=TA_LEFT, **common),
        'h2': ParagraphStyle('h2', fontName=bold, fontSize=17, leading=23, textColor=NAVY,
                             spaceAfter=5 * mm, keepWithNext=True, wordWrap=common['wordWrap']),
        'h3': ParagraphStyle('h3', fontName=bold, fontSize=12, leading=17, textColor=TEAL,
                             spaceBefore=4 * mm, spaceAfter=2.5 * mm, keepWithNext=True,
                             wordWrap=common['wordWrap']),
        'table': ParagraphStyle('table', fontSize=9.5, leading=14, **common),
        'tableHead': ParagraphStyle('tableHead', fontName=bold, fontSize=9.5, leading=14,
                                    textColor=colors.white, wordWrap=common['wordWrap']),
        'note': ParagraphStyle('note', fontSize=10, leading=16, backColor=colors.HexColor('#FFF5DF'),
                               borderPadding=3 * mm, leftIndent=3 * mm, rightIndent=3 * mm,
                               spaceBefore=2 * mm, spaceAfter=5 * mm, **common),
        'toc': ParagraphStyle('toc', fontName=bold, fontSize=11, leading=19,
                              textColor=NAVY, spaceBefore=3 * mm, wordWrap=common['wordWrap']),
    }
    s['normal'], s['bold'] = normal, bold
    return s


class Guide(BaseDocTemplate):
    def __init__(self, path, lang):
        title = 'TianShanOS ' + GUIDES[lang][1]
        super().__init__(str(path), pagesize=A4, title=title + ' | 0.6.1', author='TianShanOS',
                         subject='Security Page user guide for TianShanOS 0.6.1',
                         leftMargin=22 * mm, rightMargin=22 * mm,
                         topMargin=23 * mm, bottomMargin=19 * mm)
        self.lang, self.header = lang, title

    def beforeDocument(self):
        self.heading_index = 0

    def afterFlowable(self, flowable):
        if isinstance(flowable, Paragraph) and hasattr(flowable, 'heading_level'):
            self.heading_index += 1
            key = 'heading-' + str(self.heading_index)
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(flowable.getPlainText(), key, flowable.heading_level, False)
            if flowable.heading_level == 0:
                self.notify('TOCEntry', (0, flowable.getPlainText(), self.page, key))


def cover_background(canvas, doc):
    w, h = A4
    canvas.saveState()
    canvas.setFillColor(NAVY)
    canvas.rect(0, 0, w, h, fill=1, stroke=0)
    canvas.setFillColor(colors.HexColor('#173F5F'))
    canvas.circle(w - 20 * mm, h - 10 * mm, 48 * mm, fill=1, stroke=0)
    canvas.setFillColor(TEAL)
    canvas.circle(w + 10 * mm, 12 * mm, 60 * mm, fill=1, stroke=0)
    canvas.setStrokeColor(colors.HexColor('#27465D'))
    canvas.setLineWidth(.5)
    for offset in range(0, 110, 12):
        canvas.line(0, (25 + offset) * mm, w, offset * mm)
    canvas.restoreState()


def content_background(canvas, doc):
    canvas.saveState()
    canvas.setFont('Hei' if doc.lang == 'zh' else 'ArialGuide', 7.5)
    canvas.setFillColor(MUTED)
    canvas.drawString(22 * mm, A4[1] - 13 * mm, doc.header)
    canvas.drawRightString(A4[0] - 22 * mm, 10.5 * mm, f'TianShanOS 0.6.1  |  {doc.page}')
    canvas.setStrokeColor(LINE)
    canvas.line(22 * mm, A4[1] - 17 * mm, A4[0] - 22 * mm, A4[1] - 17 * mm)
    canvas.setFillColor(TEAL)
    canvas.rect(22 * mm, 10 * mm, 18 * mm, .8 * mm, fill=1, stroke=0)
    canvas.restoreState()


def cover(lang, s):
    def cs(name, size, leading, color=colors.white, bold=False):
        return ParagraphStyle(name, fontName=s['bold'] if bold else s['normal'],
                              fontSize=size, leading=leading, textColor=color,
                              wordWrap='CJK' if lang == 'zh' else None)
    title = cs('coverTitle', 27 if lang == 'zh' else 25, 37 if lang == 'zh' else 33, bold=True)
    subtitle = '操作步骤、结果确认与安全提醒' if lang == 'zh' else 'Set up, check, and maintain your device\'s security settings'
    label = '对应版本：TianShanOS 0.6.1' if lang == 'zh' else 'Version covered: TianShanOS 0.6.1'
    date_label = '修订日期：2026-10-09' if lang == 'zh' else 'Revised: October 9, 2026'
    return [
        Spacer(1, 36 * mm), Paragraph('TianShanOS<br/>' + GUIDES[lang][1], title),
        Spacer(1, 7 * mm), Paragraph(subtitle, cs('subtitle', 11, 18, colors.HexColor('#D9EAF2'))),
        Spacer(1, 17 * mm), Paragraph(label, cs('version', 16, 23, bold=True)),
        Spacer(1, 8 * mm), Paragraph(COVER_NOTE[lang], cs('compatibility', 11, 18, colors.HexColor('#D9EAF2'))),
        Spacer(1, 18 * mm), Paragraph(date_label, cs('date', 10, 15, colors.HexColor('#B8D7E5'))),
        NextPageTemplate('body'), PageBreak(),
    ]


def table(lines, s):
    rows = [[cell.strip() for cell in line.strip().strip('|').split('|')] for line in lines]
    rows = [r for r in rows if not all(re.fullmatch(r':?-{3,}:?', c) for c in r)]
    assert all(len(r) == 2 for r in rows)
    values = [[Paragraph(markup(c), s['tableHead' if i == 0 else 'table']) for c in row]
              for i, row in enumerate(rows)]
    item = Table(values, colWidths=[WIDTH * .37, WIDTH * .63], repeatRows=1, hAlign='LEFT')
    item.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, 0), NAVY), ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#F7FAFC')]),
        ('GRID', (0, 0), (-1, -1), .35, LINE),
        ('LEFTPADDING', (0, 0), (-1, -1), 2.5 * mm), ('RIGHTPADDING', (0, 0), (-1, -1), 2.5 * mm),
        ('TOPPADDING', (0, 0), (-1, -1), 2.3 * mm), ('BOTTOMPADDING', (0, 0), (-1, -1), 2.3 * mm),
    ]))
    return item


def body(text, s):
    lines = text.splitlines()
    story, i, chapter = [], 0, 0
    while i < len(lines):
        value = lines[i].strip()
        if not value or value.startswith('# '):
            i += 1
            continue
        if value.startswith('## '):
            if chapter:
                story.append(CondPageBreak(90 * mm))
            p = Paragraph(markup(value[3:]), s['h2'])
            p.heading_level = 0
            story.append(p)
            chapter += 1
        elif value.startswith('### '):
            p = Paragraph(markup(value[4:]), s['h3'])
            p.heading_level = 1
            story.append(p)
        elif value.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].strip().startswith('|'):
                rows.append(lines[i].strip())
                i += 1
            story.extend([table(rows, s), Spacer(1, 4 * mm)])
            continue
        elif value.startswith('> '):
            story.append(Paragraph(markup(value[2:]), s['note']))
        elif re.match(r'^(\d+\. |\- )', value):
            rows = []
            ordered = bool(re.match(r'^\d+\.', value))
            pattern = r'^\d+\. ' if ordered else r'^\- '
            while i < len(lines) and re.match(pattern, lines[i].strip()):
                rows.append(re.sub(pattern, '', lines[i].strip()))
                i += 1
            for idx, row in enumerate(rows):
                item = ParagraphStyle('item', parent=s['body'], leftIndent=7 * mm,
                                      bulletIndent=1 * mm, bulletFontName=s['bold'],
                                      bulletFontSize=9.5, bulletColor=TEAL, spaceAfter=1.7 * mm,
                                      keepWithNext=idx == 0 and len(rows) > 1)
                story.append(Paragraph(markup(row), item, bulletText=f'{idx + 1}.' if ordered else '•'))
            story.append(Spacer(1, 2 * mm))
            continue
        else:
            story.append(Paragraph(markup(value), s['body']))
        i += 1
    return story


def build(lang):
    s = styles(lang)
    doc = Guide(OUTPUT / GUIDES[lang][0], lang)
    frame = Frame(22 * mm, 19 * mm, WIDTH, A4[1] - 42 * mm, id='main')
    doc.addPageTemplates([
        PageTemplate(id='cover', frames=[frame], onPage=cover_background),
        PageTemplate(id='body', frames=[frame], onPage=content_background),
    ])
    toc = TableOfContents()
    toc.levelStyles = [s['toc']]
    story = cover(lang, s)
    story.extend([Paragraph('目录' if lang == 'zh' else 'Contents', s['h2']),
                  Spacer(1, 5 * mm), toc, PageBreak()])
    story.extend(body((SOURCE / f'guide.{lang}.md').read_text(), s))
    doc.multiBuild(story)


if __name__ == '__main__':
    fonts()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for language in GUIDES:
        build(language)

#!/usr/bin/env python3
"""把 compare-all.sh / dialogs-all.cjs 的结果整理成分组对照报告（每组页面/弹窗一份）。

  python3 tests/macos27-r2/report.py --pages <compare-all 的标签前缀> --dialogs <dialogs-all 的 --prefix>

输入：output/macos27-r2/compare/<前缀>-<页>/summary.json、output/macos27-r2/dialogs/<前缀>-<弹窗>/summary.json
输出：output/macos27-r2/reports/group-A.md … group-E.md 与 reports/README.md（索引 + 汇总表）
数字全部读自 summary.json，不手抄；「原因」列来自下面的 NOTES（每条写清是有意偏差 / 稿里的设计标注 / 示例数据 / 测试工具局限）。
"""
import argparse, json, os, re, collections

REPO = '/Users/massif/TianshanOS'
OUT = os.path.join(REPO, 'output/macos27-r2')
REL = 'output/macos27-r2'

# 页面分组：(组号, 组名, [(标签后缀, 稿, 路由, 页面名)])
PAGES = [
    ('A', '系统页 + 网络页', [('sys-main', 'Main', '/', '系统页'), ('net', 'Net', '/network', '网络页')]),
    ('B', '文件页 + 终端页', [('files', 'Files', '/files', '文件页'), ('term', 'Term', '/terminal', '终端页')]),
    ('C', '自动化页 + 指令页', [('auto', 'Auto', '/automation', '自动化页'), ('cmds', 'Cmds', '/commands', '指令页')]),
    ('D', '固件升级页 + 安全页', [('ota', 'Ota', '/ota', '固件升级页'), ('sec', 'Sec', '/security', '安全页')]),
]

# 页面已知偏差及原因（按页）。写「这类偏差为什么存在」，具体数字在表里。
PAGE_NOTES = {
    'sys-main': [
        '「资源监控」卡标题按用户要求改为「带外管理芯片资源监控」（英文 Management Chip Resources；新增语言包 key `system.resourceMonitorOob`，原 key 未动），避免被误认为是被管设备的 CPU/内存。',
        '**按真机数据调整（有意偏离稿）**：稿是按短示例数据画的，真机上固件号 `0.5.2+ac109b00d.09301547` 会把系统总览撑变形，设备只有 1 个风扇，超宽屏上内容会被拉得很散。所以：①页面内容最大宽度 1920px、居中（≤1920 不变）；②顶部三张卡宽度比 5:6:5（稿 1:1:1），窗口窄于 1420px 时改两列（第三张独占一行）；③卡片里的值单行显示、放不下用省略号，悬停后可横向滚动，完整内容在 `title` 里，标签一律不折行；④第二行「设备面板 | 风扇控制」**始终等高**：≥1390px 并排（风扇控制固定窄列 340–420px，只有一个风扇时去掉内层小卡，内容摊开、多出的高度均匀分到各块之间），更窄时上下堆叠（风扇面板整宽、左右分栏）；⑤服务型快捷动作（带状态、日志/停止）稿里没画，按同风格补做：名称一行，状态点+状态文字和「日志」「停止」按钮一行，按钮用标准 `.btn.sm`，「停止」用 `.dg`。因此第二行的位置与稿不同，对照里显示为位置偏差，不是缺陷。',
        '快捷操作卡片的副标题（「手动·引用尚未解析」）和组件数值来自夹具数据；稿里是示例文字（「手动触发·示例规则」「42°C」「8.1W」），在对照里显示为缺失/多出，不是布局差异。',
        '稿底部的设计标注块（「另有仪表·图标·文本·双值·百分比·数值 / 共 12 类组件…」）是说明文字，不是产品内容。',
        '环形图中间的「42%」：稿的 SVG 写了 Quantify 字体，但设计文档另有「数字不使用 Quantify，回到系统字体」，产品按文档用系统字体（3 个记号的字体/位置不同）。',
        '页面比稿高 1px（1321 对 1320）：稿的画布高度固定，把主内容面截短了 1px；产品页面高度由内容决定，页脚因此下移 1px。',
        '「当前时间」是实时时间，与稿里的示例时间不同（对照工具已忽略时间格式的文本）。',
        '图标：82 个图标全部是稿的风格（16×16、描边 1.35、圆角端点），其中 47 个直接取自稿的原图形，35 个稿里没有、按同规则补画（见 README 的图标一节）。',
    ],
    'net': [
        '稿在以太网卡片下方画了一张「切换到「WiFi」标签时显示」的示意卡（带标注、下拉框和 4 个按钮）；产品只显示当前标签，切到 WiFi 才出现，所以这批框和文字在对照里是「缺失」（8 个框 / 28 个记号），不是漏做。',
        '稿里的设计标注文字不是产品内容。',
    ],
    'files': ['稿里的示例文件名/大小与夹具略有不同；已通过 `--pre` 选中第二行让批量工具栏出现，与稿状态一致。除顶栏语言按钮外文本 99/99、盒子 44/44 一致。'],
    'term': [
        '终端区域里的文字是 xterm 的实时输出（欢迎框、提示符、彩色文字），稿是静态文本：「多出 123 个记号」以及位置/颜色/字距差异都来自 xterm 逐字符渲染，外框、按钮、提示行都对齐。',
        '`lint.cjs` 报的 ASCII 框图字距（-5.19px）和青色 `#57c7c0` 也是 xterm 的渲染结果（`#57c7c0` 本身取自稿的终端配色）。',
    ],
    'auto': ['除顶栏语言按钮外，文本 173/176、盒子 71/71，与稿一致。'],
    'cmds': [
        '稿的「空状态」标注块使稿里「执行结果」卡整体比产品低 36px：卡内那批按钮在对照里同时显示为「缺失」（稿的位置）和「多出」（产品的位置），像素差 10% 几乎全部来自这 36px 偏移，不是样式差异；已在 `--pre` 里把执行结果区展开到与稿一致的状态。',
        '指令行里有一条带额外图标按钮（nohup/服务类命令的操作），稿的行只画了 3 个按钮，这是夹具数据造成的差异。',
    ],
    'ota': [
        '「同时升级 www」稿画成未勾选，产品保持原默认（勾选）：功能默认值不改。',
        '进度条下方多一行「阶段 · 已传/总量 · 消息」：升级状态与错误信息必须显示，稿只画了进度条；卡片因此高 22px。',
        '折叠标题的「›」：稿里是文本字符，产品用 CSS 画同样的箭头，所以对照里显示为缺少这个文本记号。',
        '导航仍高亮「系统」（稿如此；`router.js` 一行）。',
    ],
    'sec': [
        'HTTPS 证书卡的按钮：稿画的是「已有密钥对」状态（按钮可点），夹具是「未生成密钥」，产品按原逻辑把「生成 CSR / 安装证书 / 查看证书 / 删除凭证」置灰，属于状态差异。',
        '页面比稿高 14px：稿的画布高度固定，把主内容面截短了；产品页面高度由内容决定，页脚位置相应下移。其余 512/534 个文本记号逐个一致。',
        '「已知主机指纹」小节稿里没有说明句，文案改放进标题的 `title` 悬停提示（`securityPage.fingerprintHint`）。',
    ],
}
# 所有页面共有：顶栏语言按钮的地球图标
COMMON_NOTE = '顶栏语言按钮：Main、Modal 两张稿带地球图标，其余 8 张稿没有，产品统一带图标，所以除 Main 外，右上角状态/语言文字整体位移 21px（每页约 3–6 个文本记号）。 另有两处有意偏离稿：①页脚品牌字「TianshanOS · RMinte® AI」按用户要求用 Quantify Bold（稿为 Regular，Bold 与顶栏字标同源、更干净）；②复选框选中态加白色对勾（稿的 `.chk.on` 只是蓝色方块，不符合 macOS 习惯；`boardcss.cjs` 对这两处做了遮罩）。'

# 弹窗备注（按 dialogs.json 名称）；没写的按通用规则解释
DLG_NOTES = {
    'keygen': '下拉框箭头背景图差异属测试工具局限（稿用 `button.field.sel` 画下拉，产品用原生 `<select>`）。',
    'cert-genkey': '稿里「（仅在已有密钥对时显示）」是设计标注。',
    'pack-export': '「复制到剪贴板 / 下载到本地」两个按钮生成配置包之后才出现（稿把它们画在生成前）；文件列表用夹具造了 2 个文件。',
    'mismatch': '稿把主机写成 `192.0.2.30`，产品显示 `主机:端口`。',
    'fan-help': '**按用户要求恢复原来的 4 段详细说明**（稿里只有一句话的简版），弹窗因此比稿高、宽 600。',
    'fan-curve': '保留了稿没有的多变量权重绑定行、导入/导出按钮和「最小调节间隔」（原有功能）；稿的「公式」输入框后端不支持。**曲线预览按用户要求恢复坐标轴数字（横轴 °C、纵轴 %，每 20 一档）和每个节点的「温度°/转速%」标注，预览框因此比稿高（230px，稿 100px），弹窗整体变高。**',
    'widget-manager': '稿的拖拽把手没有实现，保留上移/下移箭头；另有独立的「添加/编辑组件」sheet（稿只画了列表）。',
    'led-board': '产品多一个「启用 LED 设备」开关行（原有功能）；标签条只在矩阵屏上显示。',
    'command': '稿把「检测间隔」画成秒，产品保持原有的毫秒（画成秒会改变语义）。',
    'file-picker': '按稿去掉了「上一级 / 确定 / 双击」：面包屑可点、单击文件即选中并关闭、单击目录进入，功能等价。',
    'logs': '日志区高度：稿 200px；按你的决定改为 320px（只改系统日志弹窗，快捷日志不变），弹窗因此比稿高 120px。',
    'confirm-danger': '确认 sheet 是新增组件（稿的 `SheetsStates` 第 1 个）：标题统一为「确认」，红色主按钮不响应 Enter，默认聚焦「取消」。',
    'wifi-connect': '稿在密码框下有一行「替代原生 prompt()」标注，产品没有，故比稿矮 22px。',
    'rule-edit': '动作行保留延迟/重复/执行条件（放第二行小控件）与图标面板展开按钮，稿只画了模板选择 + 移除。',
    'action-led': '「控制类型」行在选择设备后才显示（保持原行为，稿一直显示）。',
}
DLG_GENERIC = '文本/盒子未完全一致的部分见对应目录的 `report.md`；常见原因：稿里的设计标注、稿示例值与产品默认值不同、`select` 文字抽取不到（测试工具局限）。'

GROUP_E_TITLE = '弹窗（分组表单 sheet）'
GROUP_F_TITLE = '响应式与其它'


def load(p):
    try:
        return json.load(open(p, encoding='utf-8'))
    except Exception:
        return None


def pct(a, b):
    return f'{a}/{b}' if b else '-'


def rel(path):
    return path.replace(REPO + '/', '')


def top_devs(dev, n=10):
    out = []
    for d in dev[:n]:
        w = d['where'].replace('|', '/')[:46]
        out.append(f"| {d['kind']} | {w} | {str(d['want']).replace('|', '/')[:40]} | {str(d['got']).replace('|', '/')[:40]} |")
    return out


def page_section(prefix, tag, board, route, name):
    d = load(f'{OUT}/compare/{prefix}-{tag}/summary.json')
    lines = [f'### {name}（{route} ↔ boards/{board}.html）', '']
    if not d:
        return lines + ['> 没有找到对照结果（未运行）。', ''], None
    s, dev = d['summary'], d['deviations']
    t, b, px = s['tokens'], s['boxes'], s['pixel']
    kinds = ', '.join(f'{k}×{v}' for k, v in sorted(s['kinds'].items(), key=lambda kv: -kv[1])) or '无'
    lines += [
        f"- 画布：稿 {s['board_size'][0]}×{s['board_size'][1]}，页面 {s['page_size'][0]}×{s['page_size'][1]}；整体偏移（中位）dx={s['shift']['dx']:.1f}，dy={s['shift']['dy']:.1f}",
        f"- 文本记号：稿 {t['board']}，页面 {t['page']}，匹配 {t['matched']}（**完全一致 {t['exact']}**），缺失 {t['missing']}，多出 {t['extra']}",
        f"- 绘制盒：稿 {b['board']}，页面 {b['page']}，匹配 {b['matched']}（**完全一致 {b['exact']}**），缺失 {b['missing']}，多出 {b['extra']}",
        f"- 像素：不一致 {px['mismatch_pct']}%，MAE {px['mae']}",
        f'- 偏差类别：{kinds}',
        f"- 截图（左稿 / 右页面 / 差异）：`{REL}/compare/{prefix}-{tag}/side.png`、`diff.png`",
        '',
        '**已知偏差及原因**',
    ]
    lines += [f'- {x}' for x in PAGE_NOTES.get(tag, [])] + [f'- {COMMON_NOTE}'] if tag != 'sys-main' else [f'- {x}' for x in PAGE_NOTES.get(tag, [])] + ['- 顶栏：Main 稿带地球图标，与产品一致，顶栏与页脚逐项零偏差。']
    lines += ['', '**偏差明细（按严重度前 10）**', '', '| 类别 | 位置 | 稿 | 页面 |', '|---|---|---|---|'] + top_devs(dev) + ['']
    return lines, s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pages', required=True)
    ap.add_argument('--dialogs', required=True)
    a = ap.parse_args()
    os.makedirs(f'{OUT}/reports', exist_ok=True)
    summary_rows = []
    for g, gname, pages in PAGES:
        lines = [f'# 对照报告 · 组 {g}：{gname}', '', '> 稿 = `docs/macos27/design-system/boards/*.html`；页面 = 本机替身（127.0.0.1）上的产品页面，Chrome 无头 1440 宽。**未在真机验证。**', '']
        for tag, board, route, name in pages:
            sec, s = page_section(a.pages, tag, board, route, name)
            lines += sec
            if s:
                summary_rows.append((g, name, s))
        open(f'{OUT}/reports/group-{g}.md', 'w', encoding='utf-8').write('\n'.join(lines) + '\n')

    # 弹窗
    man = json.load(open(f'{REPO}/tests/macos27-r2/dialogs.json', encoding='utf-8'))
    rows, tot = [], collections.Counter()
    for name, c in man.items():
        d = load(f'{OUT}/dialogs/{a.dialogs}-{name}/summary.json')
        where = f"{c.get('board')}#{c.get('nth', c.get('title', '·'))}" if 'board-sel' not in c else f"{c.get('board')}(选择器)"
        if not d:
            rows.append(f'| `{name}` | {where} | 未运行 | | | | |')
            continue
        s = d['summary']
        t, b = s['tokens'], s['boxes']
        tot['t_ex'] += t['exact']; tot['t_bd'] += t['board']; tot['b_ex'] += b['exact']; tot['b_bd'] += b['board']; tot['n'] += 1
        tot['perfect'] += int(t['exact'] == t['board'] and b['exact'] == b['board'] and t['extra'] == 0 and b['extra'] == 0)
        note = DLG_NOTES.get(name, '')
        rows.append(f"| `{name}` | {where} | {s['size']['board'][0]}×{s['size']['board'][1]} / {s['size']['page'][0]}×{s['size']['page'][1]} | {pct(t['exact'], t['board'])}（多出 {t['extra']}） | {pct(b['exact'], b['board'])}（多出 {b['extra']}） | {s['pixel']['mismatch_pct']}% | {note} |")
    lines = [f'# 对照报告 · 组 E：{GROUP_E_TITLE}', '',
             '> 每个弹窗：稿里的 `.sheet` 与产品里打开后的弹窗元素，坐标相对各自弹窗左上角；文本记号 + 绘制盒 + 像素。**未在真机验证。**', '',
             f"- 共 {tot['n']} 个弹窗；其中文本、盒子均与稿完全一致且无多出的 **{tot['perfect']}** 个；文本完全一致 {tot['t_ex']}/{tot['t_bd']}，盒子完全一致 {tot['b_ex']}/{tot['b_bd']}。",
             f"- 截图：`{REL}/dialogs/{a.dialogs}-<名称>/side.png`（左稿 / 右页面）、`report.md`（逐项偏差）。",
             f'- {DLG_GENERIC}', '',
             '| 弹窗 | 稿 | 尺寸 稿 / 页面 | 文本 一致/稿 | 盒子 一致/稿 | 像素差 | 备注（有意偏差 / 原因） |', '|---|---|---|---|---|---|---|'] + rows
    open(f'{OUT}/reports/group-E.md', 'w', encoding='utf-8').write('\n'.join(lines) + '\n')

    idx = ['# 第二轮 WebUI 对照报告索引', '',
           '| 组 | 内容 | 文件 |', '|---|---|---|'] + [f'| {g} | {gname} | [group-{g}.md](group-{g}.md) |' for g, gname, _ in PAGES] + [f'| E | {GROUP_E_TITLE} | [group-E.md](group-E.md) |', '']
    idx += ['## 页面汇总', '', '| 组 | 页面 | 文本 一致/稿 | 盒子 一致/稿 | 像素差 |', '|---|---|---|---|---|']
    for g, name, s in summary_rows:
        idx.append(f"| {g} | {name} | {pct(s['tokens']['exact'], s['tokens']['board'])} | {pct(s['boxes']['exact'], s['boxes']['board'])} | {s['pixel']['mismatch_pct']}% |")
    idx += ['', '## 图标', '',
            '- 82 个图标（10-03 起）：80 个取自 Phosphor Regular（MIT，@phosphor-icons/core 2.1.1，16px 下约 1.0px 描边），2 个保留自绘（路由器——Phosphor 没有；开关状态圆点）。图标 id（`ri-…`）不变，只换图形。对应表与可视对照：[icons-phosphor-mapping.html](icons-phosphor-mapping.html)；映射数据 `tests/macos27-r2/phosphor-map.json`；生成方式 `python3 tests/macos27-r2/icons_from_phosphor.py`（幂等，`--check` 只检查）。',
            '- 第一轮用的是稿的风格（稿的原图形 47 个 + 补画 35 个，描边 1.35，后按用户要求改 1.15），再往前是 Remix 字体；`icons-compare.png` 是第一轮的新旧对照，已过时。',
            '- 对应时顺带改进的表达：扫描 WiFi（原画成刷新箭头）→ 取景框；二维码（原画成四个方块）→ 真二维码；风扇曲线（原画成柱状）→ 折线；GPU 小组件（原为游戏手柄）→ 显卡；仪表小组件 → 表盘；LED 主板灯带 → 电路板。用户另选了「滤镜效果」= palette、「数字」= number-square-one。']
    idx += ['', '最终结论与验证汇总见 [final.md](final.md)。']
    open(f'{OUT}/reports/README.md', 'w', encoding='utf-8').write('\n'.join(idx) + '\n')
    print('已写入', f'{OUT}/reports/', '（README.md, group-A..E.md）')


main()

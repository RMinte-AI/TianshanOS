
function runtimeText(key) {
    const translated = typeof t === 'function' ? t('runtimeRepair.' + key) : '';
    return translated && translated !== 'runtimeRepair.' + key ? translated : key;
}
function runtimeSaveError(result) {
    const code = result.data?.error_code || result.rawMessage || result.error || result.message;
    const known = ['restart_pending', 'revision_conflict', 'source_read_only', 'recovery_required', 'commit_unknown', 'execution_busy', 'service_busy', 'busy_retired_config'];
    return known.includes(code) ? runtimeText(code) : (typeof code === 'string' ? code : runtimeText('saveFailed'));
}
async function ruleWriteWithRevision(method, id) {
    const current = await api.call('automation.rules.get', { id });
    if (current.code !== 0 || !Number.isInteger(current.data?.revision)) return current;
    return api.call(method, { id, expected_revision: current.data.revision });
}
/**
 * TianshanOS Web App - Main Application
 */
if (typeof window.t === 'undefined') window.t = function(k) { return k; };

// =========================================================================
//                         全局状态
// =========================================================================

// Capture once at entry; do not ask which page is current after an await.
function capturePageValidity() {
    const navigation = typeof router !== 'undefined' ? router.navigation : null;
    const content = document.getElementById('page-content');
    return () => (typeof router === 'undefined' || router.navigation === navigation) &&
        (!navigation || navigation.isCurrent()) && document.getElementById('page-content') === content;
}

let ws = null;
let refreshInterval = null;
let subscriptionManager = null;  // WebSocket 订阅管理器

// =========================================================================
//                         WebSocket 订阅管理器
// =========================================================================

class SubscriptionManager {
    constructor(ws) {
        this.ws = ws;
        this.subscriptions = new Map(); // topic -> Set(callbacks)
        this.activeSubs = new Set();    // 已激活的 topic
    }

    /**
     * 订阅主题
     * @param {string} topic - 主题名称 (system.info, device.status, ota.progress)
     * @param {function} callback - 数据回调函数
     * @param {object} params - 订阅参数 (interval 等)
     */
    subscribe(topic, callback, params = {}) {
        // 添加回调
        if (!this.subscriptions.has(topic)) {
            this.subscriptions.set(topic, new Set());
        }
        this.subscriptions.get(topic).add(callback);

        // 发送订阅消息（只在首次订阅时）
        if (!this.activeSubs.has(topic)) {
            this.ws.send({
                type: 'subscribe',
                topic: topic,
                params: params
            });
            this.activeSubs.add(topic);
            console.log(`[SubscriptionMgr] Subscribed to: ${topic}`, params);
        }
    }

    /**
     * 取消订阅
     * @param {string} topic - 主题名称
     * @param {function} callback - 回调函数（不传则移除所有）
     */
    unsubscribe(topic, callback = null) {
        if (!this.subscriptions.has(topic)) return;

        if (callback) {
            // 移除特定回调
            this.subscriptions.get(topic).delete(callback);
        } else {
            // 移除所有回调
            this.subscriptions.get(topic).clear();
        }

        // 如果没有回调了，发送取消订阅消息
        if (this.subscriptions.get(topic).size === 0) {
            this.subscriptions.delete(topic);
            if (this.activeSubs.has(topic)) {
                this.ws.send({
                    type: 'unsubscribe',
                    topic: topic
                });
                this.activeSubs.delete(topic);
                console.log(`[SubscriptionMgr] Unsubscribed from: ${topic}`);
            }
        }
    }

    /**
     * 处理 WebSocket 消息
     * @param {object} msg - WebSocket 消息
     */
    handleMessage(msg) {
        // 处理订阅确认
        if (msg.type === 'subscribed' || msg.type === 'unsubscribed') {
            const status = msg.success ? 'OK' : 'Fail';
            console.log(`[SubscriptionMgr] ${msg.type}: ${msg.topic} ${status}`);
            if (!msg.success && msg.error) {
                console.error(`[SubscriptionMgr] Error: ${msg.error}`);
            }
            return;
        }

        // 分发数据到订阅回调
        if (msg.type === 'data' && msg.topic) {
            const callbacks = this.subscriptions.get(msg.topic);
            if (callbacks && callbacks.size > 0) {
                callbacks.forEach(cb => {
                    try {
                        cb(msg, msg.timestamp);
                    } catch (e) {
                        console.error(`[SubscriptionMgr] Callback error for ${msg.topic}:`, e);
                    }
                });
            }
            // 没有回调时静默丢弃（页面切换时的正常行为）
        }
    }

    /**
     * 清理所有订阅
     */
    clear() {
        this.activeSubs.forEach(topic => {
            this.ws.send({ type: 'unsubscribe', topic });
        });
        this.subscriptions.clear();
        this.activeSubs.clear();
        console.log('[SubscriptionMgr] Cleared all subscriptions');
    }
}

// =========================================================================
//                         初始化
// =========================================================================

document.addEventListener('DOMContentLoaded', () => {
    // 初始化认证 UI
    updateAuthUI();
    validateStoredSession();

    // 仅当 localStorage 无有效语言偏好时，才从设备 system.language 同步（不覆盖用户已有选择）
    (async function syncLanguageFromDevice() {
        try {
            const saved = localStorage.getItem('ts_language');
            if (saved) return; /* 用户已有选择，优先尊重 */
            const r = await api.configGet('system.language');
            if (r && r.code === 0 && r.data && r.data.value !== undefined) {
                const webLang = r.data.value === 1 ? 'zh-CN' : 'en-US';
                if (typeof selectLanguage === 'function' && i18n && i18n.getLanguage() !== webLang) {
                    selectLanguage(webLang);
                }
            }
        } catch (e) {}
    })();

    // 更新 Footer 版本号
    updateFooterVersion();

    // 注册路由（系统页面作为首页）
    router.register('/', loadSystemPage);
    router.register('/system', loadSystemPage);
    router.register('/network', loadNetworkPage);
    router.register('/ota', loadOtaPage);
    router.register('/files', loadFilesPage);
    // 日志页面已整合到终端页面模态框（重定向到终端并打开模态框）
    router.register('/logs', () => {
        window.location.hash = '#/terminal';
        setTimeout(() => {
            if (typeof showTerminalLogsModal === 'function') {
                showTerminalLogsModal();
            }
        }, 100);
    });
    router.register('/terminal', loadTerminalPage);
    router.register('/commands', loadCommandsPage);
    router.register('/security', loadSecurityPage);
    router.register('/automation', loadAutomationPage);

    // 语言切换时重新渲染当前页，使主内容使用新语言；下一帧恢复右上角登录态（避免 translateDOM 覆盖 #user-name）
    window.addEventListener('languageChanged', (event) => {
        if (event.detail?.initial) return;
        router.navigate();
        setTimeout(() => updateAuthUI(), 0);
    });

    window.dispatchEvent(new CustomEvent('appReady'));
    // 启动 WebSocket
    setupWebSocket();

    // 全局键盘快捷键
    document.addEventListener('keydown', (e) => {
        // Esc 键取消 SSH 命令执行
        if (e.key === 'Escape' && typeof currentExecSessionId !== 'undefined' && currentExecSessionId) {
            e.preventDefault();
            cancelExecution();
        }
    });
});

// =========================================================================
//                         认证
// =========================================================================

window.addEventListener('authExpired', () => {
    updateAuthUI();
    showLoginModal();
});

async function validateStoredSession() {
    if (!api.isLoggedIn()) return;

    try {
        const status = await api.checkAuthStatus();
        if (!status || !status.valid) {
            api.clearAuth();
            updateAuthUI();
            showLoginModal();
        }
    } catch (error) {
        console.debug('Stored session validation failed:', error);
    }
}

function updateAuthUI() {
    const loginBtn = document.getElementById('login-btn');
    const userName = document.getElementById('user-name');
    if (api.isLoggedIn()) {
        const username = api.getUsername();
        const level = api.getLevel();

        loginBtn.textContent = t('security.logoutBtn');
        userName.textContent = username;
        userName.title = (typeof t === 'function' ? t('ui.permissionLevel') : '权限级别') + ': ' + level;
        loginBtn.onclick = logout;

        // 更新导航菜单可见性
        router.updateNavVisibility();
    } else {
        loginBtn.textContent = t('security.login');
        userName.textContent = t('ui.notLoggedIn');
        userName.title = '';
        loginBtn.onclick = showLoginModal;

        // 隐藏需要权限的导航项
        router.updateNavVisibility();
    }
}

function showLoginModal() {
    document.getElementById('login-modal').classList.remove('hidden');
    // 聚焦用户名输入框
    setTimeout(() => document.getElementById('username')?.focus(), 100);
}

function closeLoginModal() {
    document.getElementById('login-modal').classList.add('hidden');
    document.getElementById('login-form').reset();
    document.getElementById('login-error')?.classList.add('hidden');
}

document.getElementById('login-form')?.addEventListener('submit', async (e) => {
    e.preventDefault();
    const username = document.getElementById('username').value;
    const password = document.getElementById('password').value;
    const errorEl = document.getElementById('login-error');
    const submitBtn = e.target.querySelector('button[type="submit"]');

    // 显示加载状态
    submitBtn.disabled = true;
    submitBtn.textContent = t('login.loggingIn');
    errorEl?.classList.add('hidden');

    try {
        const result = await api.login(username, password);

        if (result.code === 0) {
            closeLoginModal();
            updateAuthUI();

            // 检查是否需要修改密码
            if (!result.data.password_changed) {
                showPasswordChangeReminder();
            }

            router.navigate();
            showToast(t('login.welcomeName', { name: username }), 'success');
        } else {
            // 显示错误信息
            if (errorEl) {
                errorEl.textContent = result.message || t('login.loginFailed');
                errorEl.classList.remove('hidden');
            }
            showToast(result.message || t('login.loginFailed'), 'error');
        }
    } catch (error) {
        if (errorEl) {
            errorEl.textContent = error.message || t('login.networkError');
            errorEl.classList.remove('hidden');
        }
        showToast(t('login.loginFailed') + ': ' + error.message, 'error');
    } finally {
        submitBtn.disabled = false;
        submitBtn.textContent = t('login.loginButton');
    }
});

async function logout() {
    try {
        const result = await api.logout();
        if (api.token) return; // A newer sign-in owns the UI now.
        showToast(t(result.serverConfirmed ? 'promptRepair.logoutConfirmed' : 'promptRepair.logoutLocal'), result.serverConfirmed ? 'info' : 'warning', 7000);
    } finally {
        updateAuthUI();
        if (!api.token) router.navigate('/');
    }
}

/**
 * 显示修改密码提醒
 */
function showPasswordChangeReminder() {
    const modal = document.createElement('div');
    modal.id = 'password-change-modal';
    modal.className = 'modal show';
    modal.innerHTML = sheet(460, t('login.securityReminder'),
        `<div class="t-body" style="color:var(--ink-2);margin-bottom:12px">${t('login.defaultPasswordHint')}</div>
        <form id="change-password-form">${grp(
            row(t('login.currentPassword'), inp('change-old-pwd', 200, '', '', `type="password" required aria-label="${t('login.currentPassword')}"`)) +
            row(t('login.newPassword'), inp('change-new-pwd', 200, '', '', `type="password" minlength="4" maxlength="64" required aria-label="${t('login.newPassword')}"`)) +
            row(t('login.confirmNewPassword'), inp('change-confirm-pwd', 200, '', '', `type="password" minlength="4" maxlength="64" required aria-label="${t('login.confirmNewPassword')}"`)))}
        <div id="change-pwd-error" class="form-error hidden"></div></form>`,
        `<button class="btn lg" onclick="closePasswordChangeModal()">${t('login.changeLater')}</button><button class="btn lg primary" onclick="submitPasswordChange()">${t('login.changeNow')}</button>`);
    document.body.appendChild(modal);
}

function closePasswordChangeModal() {
    const modal = document.getElementById('password-change-modal');
    if (modal) modal.remove();
}

async function submitPasswordChange() {
    const oldPwd = document.getElementById('change-old-pwd').value;
    const newPwd = document.getElementById('change-new-pwd').value;
    const confirmPwd = document.getElementById('change-confirm-pwd').value;
    const errorEl = document.getElementById('change-pwd-error');

    if (newPwd !== confirmPwd) {
        errorEl.textContent = typeof t === 'function' ? t('login.passwordMismatch') : '两次输入的新密码不一致';
        errorEl.classList.remove('hidden');
        return;
    }

    if (newPwd.length < 4) {
        errorEl.textContent = typeof t === 'function' ? t('login.passwordMinLength') : '新密码至少4个字符';
        errorEl.classList.remove('hidden');
        return;
    }

    try {
        const result = await api.changePassword(oldPwd, newPwd);
        if (result.code === 0) {
            closePasswordChangeModal();
            showToast(t('login.passwordChanged'), 'success');
        } else {
            errorEl.textContent = result.message || t('toast.saveFailed');
            errorEl.classList.remove('hidden');
        }
    } catch (error) {
        errorEl.textContent = error.message || t('login.networkError');
        errorEl.classList.remove('hidden');
    }
}

// =========================================================================
//                         Footer 版本号更新
// =========================================================================

async function updateFooterVersion() {
    try {
        const versionData = await api.call('ota.version');
        if (versionData?.data?.version) {
            const versionEl = document.getElementById('footer-version');
            if (versionEl) {
                versionEl.textContent = 'v' + versionData.data.version;
            }
        }
    } catch (error) {
        console.log('Failed to fetch version:', error);
    }
}

// =========================================================================
//                         WebSocket
// =========================================================================

let webSocketConnected = false;
function renderWsStatus(connected = webSocketConnected) {
    webSocketConnected = connected;
    const el = document.getElementById('ws-status');
    if (!el) return;
    el.classList.toggle('connected', connected);
    el.setAttribute('data-i18n-title', connected ? 'network.connected' : 'network.disconnected');
    el.title = t(connected ? 'network.connected' : 'network.disconnected');
    el.textContent = el.title;
    el.setAttribute('aria-label', el.title);
}
window.addEventListener('languageChanged', () => {
    renderWsStatus();
    const toast = document.getElementById('toast');
    if (toast) { clearTimeout(toastTimer); toast.classList.remove('show'); toastDeadline = 0; }
    if (lastPowerEvent) renderPowerNotice(lastPowerEvent);
    updateModalWsStatus(webSocketConnected);
});

function setupWebSocket() {
    ws = new TianShanWS(
        (msg) => handleEvent(msg),
        () => renderWsStatus(true),
        () => renderWsStatus(false)
    );
    ws.connect();

    // 初始化订阅管理器
    subscriptionManager = new SubscriptionManager(ws);

    // 暴露给全局，供日志页面使用
    window.ws = ws;
    window.subscriptionManager = subscriptionManager;
}

function handleEvent(msg) {
    // console.log('Event:', msg);

    // 处理订阅管理器消息 (subscribed/unsubscribed/data)
    if (subscriptionManager && (msg.type === 'subscribed' || msg.type === 'unsubscribed' || msg.type === 'data')) {
        subscriptionManager.handleMessage(msg);
        return;
    }

    // 处理日志消息
    if (msg.type === 'log') {
        // 日志页面处理
        if (typeof window.handleLogMessage === 'function') {
            window.handleLogMessage(msg);
        }

        // 模态框实时日志处理
        const modal = document.getElementById('terminal-logs-modal');
        if (modal && modal.style.display === 'flex') {
            if (typeof window.handleModalLogMessage === 'function') {
                window.handleModalLogMessage(msg);
            }
        }
        return;
    }

    // 处理日志订阅确认
    if (msg.type === 'log_subscribed') {
        if (typeof window.updateWsStatus === 'function') {
            window.updateWsStatus(true);
        }
        return;
    }

    // 处理历史日志响应
    if (msg.type === 'log_history') {
        const logs = msg.logs || [];

        // 日志页面
        if (typeof window.logEntries !== 'undefined') {
            window.logEntries = logs;
            if (typeof window.renderFilteredLogs === 'function') {
                window.renderFilteredLogs();
            }
            showToast(typeof t === 'function' ? t('toast.logsLoadedCount', { count: logs.length }) : `加载了 ${logs.length} 条历史日志`, 'success');
        }

        // 终端页面的日志模态框
        const modal = document.getElementById('terminal-logs-modal');
        if (modal && modal.style.display === 'flex') {
            modalLogEntries.length = 0;
            modalLogEntries.push(...logs.map(log => ({
                level: log.level || 3,
                levelName: getLevelName(log.level || 3),
                tag: log.tag || 'unknown',
                message: log.message || '',
                timestamp: log.timestamp || Date.now(),
                task: log.task || ''
            })));
            renderModalLogs();
        }

        return;
    }

    if (msg.type === 'event') {
        // 刷新相关页面数据
        if (router.currentPage) {
            router.currentPage();
        }
    }

    // 处理电压保护事件
    if (msg.type === 'power_event') {
        handlePowerEvent(msg);
    }

    // 处理 SSH Exec 流式输出消息
    if (msg.type && msg.type.startsWith('ssh_exec_')) {
        handleSshExecMessage(msg);
    }
}

// 处理电压保护事件
let lastPowerEvent = null;
function renderPowerNotice(msg) {
    let notice = document.getElementById('power-notice');
    if (!notice) {
        notice = document.createElement('div'); notice.id = 'power-notice';
        notice.setAttribute('role', 'alert');
        notice.style.cssText = 'position:fixed;bottom:20px;left:20px;z-index:1100;background:#fff;padding:12px;border:2px solid #e11d48;white-space:pre-wrap';
        document.body.appendChild(notice);
    }
    const key = msg.state === 'PROTECTED' ? 'toast.voltageProtectionTriggered' : msg.state === 'RECOVERY' ? 'toast.voltageRecovering' : 'toast.lowVoltageWarning';
    notice.textContent = t(key, {voltage: msg.voltage?.toFixed(2) || '?', countdown: msg.countdown || 0});
    notice.hidden = !['LOW_VOLTAGE', 'SHUTDOWN', 'PROTECTED', 'RECOVERY'].includes(msg.state);
}

function handlePowerEvent(msg) {
    lastPowerEvent = msg;
    renderPowerNotice(msg);
    const state = msg.state;
    const voltage = msg.voltage?.toFixed(2) || '?';
    const countdown = msg.countdown || 0;

    // 显示警告
    if (state === 'LOW_VOLTAGE' || state === 'SHUTDOWN') {
        showToast(typeof t === 'function' ? t('toast.lowVoltageWarning', { voltage, countdown }) : `低电压警告: ${voltage}V (${countdown}s)`, 'warning', 5000);
    } else if (state === 'PROTECTED') {
        showToast(typeof t === 'function' ? t('toast.voltageProtectionTriggered') : '电压保护已触发', 'error', 10000);
    } else if (state === 'RECOVERY') {
        showToast(typeof t === 'function' ? t('toast.voltageRecovering', { voltage }) : `电压恢复中: ${voltage}V`, 'info', 3000);
    }
}

// =========================================================================
//                         系统页面（合并原首页+系统）
// =========================================================================

// 带状态的按钮（AGX / LPMU 电源键）：图标 + 名称 + 状态
const pwHtml = (nameKey, status) => `<svg class="i"><use href="#ri-shut-down-line"/></svg><span class="nm">${t(nameKey)}</span><span class="st">${status}</span>`;
// 表格行内图标按钮（与设计稿的 btn icon sm 对应）
// 图标来自 index.html 里的 SVG 精灵（<symbol id="ri-…">）；设备上持久化的组件图标仍是旧的 <i class="ri-…"></i> 字符串，渲染前用 iconize 转成 svg
const ic = (name, cls = '') => `<svg class="i${cls ? ' ' + cls : ''}"><use href="#${name}"/></svg>`;
const iconize = html => String(html ?? '').replace(/<i class=(["'])(ri-[\w-]+)\1><\/i>/g, (m, q, n) => ic(n));
const icoBtn = (icon, title, onclick, cls = '', dis = false) => `<button class="btn icon sm ${cls}" onclick="${onclick}" title="${title}" aria-label="${title}"${dis ? ' disabled' : ''}><svg class="i"><use href="#${icon}"/></svg></button>`;
// 模板片段：与设计稿的键值行一一对应
const kvRow = (label, value = '-', id = '') => `<div class="kv"><span>${label}</span><span${id ? ` id="${id}"` : ''}>${value}</span></div>`;

// 弹窗（分组表单）片段：sheet 是弹窗元素（.modal）里面的内容，宽度与设计稿一致；close 是右上角 ✕ 的处理函数（稿里有取消按钮的弹窗不放 ✕）
const sheet = (w, title, body, foot = '', close = '', cls = '') =>
    `<div class="sheet m-float${cls ? ' ' + cls : ''}" style="width:${w}px"><div class="sh"><span class="st">${title}</span>${close ? `<button type="button" class="btn icon round xbtn" onclick="${close}" aria-label="${t('common.close')}" title="${t('common.close')}"><svg class="i"><use href="#ri-close-line"/></svg></button>` : ''}</div><div class="sb">${body}</div>${foot ? `<div class="sf">${foot}</div>` : ''}</div>`;
const gt = title => `<div class="gt">${title}</div>`;
const grp = (rows, style = '') => `<div class="grp"${style ? ` style="${style}"` : ''}>${rows}</div>`;
const row = (label, ctl, note = '', tip = '') => `<div class="row"><div class="rl"${tip ? ` title="${escapeHtml(tip)}"` : ''}>${label}${note ? `<small>${note}</small>` : ''}</div><div class="rc">${ctl}</div></div>`;
// 行内输入框：宽度按稿；cls 里 mono 用于标识符/命令/地址；extra 追加属性（type、min、max、value…）
const inp = (id, w, ph = '', cls = '', extra = '') => `<input class="field${cls ? ' ' + cls : ''}" id="${id}"${ph ? ` placeholder="${ph}" aria-label="${ph}"` : ''} style="width:${w}px"${extra ? ' ' + extra : ''}>`;
const unit = u => `<span class="t-note" style="min-width:22px">${u}</span>`;
const swc = (id, on = false, extra = '') => `<input type="checkbox" class="switch" role="switch" id="${id}"${on ? ' checked' : ''}${extra ? ' ' + extra : ''}>`;

// 导出 / 导入配置包弹窗的通用外观（指令、主机、数据源、规则、动作共用）。key 决定元素 id：
//   export-<key>-cert|result|btn；import-<key>-file|file-status|step2|preview|overwrite|result|btn
const exportSheet = (key, title, desc, hint, hide, run, extra = '') => sheet(560, title,
    `${desc ? `<div class="t-note" style="margin-bottom:10px">${desc}</div>` : ''}${extra}<div class="fl"><label>${t('securityPage.targetDeviceCert')}</label><textarea class="field mono" id="export-${key}-cert" style="height:96px" placeholder="${escapeHtml(hint)}"></textarea></div><div id="export-${key}-result" class="result-box hidden" style="margin-top:12px"></div>`,
    `<button class="btn lg" onclick="${hide}()">${t('common.cancel')}</button><button class="btn lg primary" id="export-${key}-btn" onclick="${run}(this.closest('.modal').dataset.exportId)"><svg class="i"><use href="#ri-download-line"/></svg>${t('common.export')}</button>`);
const importPlaceholder = key => row(t('securityPage.previewRowLabel'), `<span class="t-note">${t('securityPage.previewAfterSelect')}</span>`) + row(t('ssh.overwriteExisting'), swc(`import-${key}-overwrite`));
const importSheet = (key, title, desc, preview, confirm, hide, extra = '', w = 560, ok = t('ssh.confirmImport')) => sheet(w, title,
    `<div class="t-label" style="margin-bottom:10px">${desc}</div><div class="acts" style="align-items:center;gap:12px"><input type="file" id="import-${key}-file" accept=".tscfg" onchange="${preview}()" style="position:absolute;opacity:0;width:0;height:0;pointer-events:none"><button type="button" class="btn" onclick="document.getElementById('import-${key}-file').click()"><svg class="i"><use href="#ri-upload-line"/></svg>${t('common.selectFile')}</button><span id="import-${key}-file-status" class="t-note">${t('common.noFileSelected')}</span></div>
     ${gt(t('ssh.configPackContent'))}<div id="import-${key}-step2"><div class="grp" id="import-${key}-preview">${importPlaceholder(key)}</div>${extra}</div>
     <div id="import-${key}-result" class="result-box hidden" style="margin-top:10px"></div>`,
    `<button class="btn lg" onclick="${hide}()">${t('common.cancel')}</button><button class="btn lg primary" id="import-${key}-btn" onclick="${confirm}()" disabled>${ok}</button>`);
// 预览通过后的键值行（签名验证通过的提示由调用处写进 result-box）；typeVal 为空则不显示「类型」行
function renderImportPreview(key, data, typeVal) {
    document.getElementById(`import-${key}-preview`).innerHTML =
        row(t('securityPage.configId'), `<span class="mono">${escapeHtml(data.id)}</span>`) +
        (typeVal ? row(t('common.type'), typeVal) : '') +
        row(t('ssh.signer'), escapeHtml(data.signer) + (data.official ? ` <span class="state ok">${t('ssh.official')}</span>` : '')) +
        row(t('securityPage.noteLabel'), escapeHtml(data.note || t('ssh.restartToLoad'))) +
        row(t('ssh.overwriteExisting'), swc(`import-${key}-overwrite`), data.exists ? t('securityPage.configExistsWarning') : '');
}

async function loadSystemPage() {
    const pageCurrent = capturePageValidity();
    clearInterval(refreshInterval);

    // 取消之前设置的快捷操作刷新定时器（防止切换到其他页后仍触发）
    if (quickActionsTimeoutId) {
        clearTimeout(quickActionsTimeoutId);
        quickActionsTimeoutId = null;
    }


    // 停止 uptime 计算
    if (window.systemUptimeInterval) {
        clearInterval(window.systemUptimeInterval);
        window.systemUptimeInterval = null;
    }

    // 停止服务状态刷新（切换页面时会重新启动）
    stopServiceStatusRefresh();

    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page page-system">
            <div class="sys-top">
                <!-- 资源监控 -->
                <div class="card">
                    <div class="card-h">
                        <span class="t-section">${t('system.resourceMonitorOob')}</span>
                        <button class="btn sm" onclick="showServicesModal()"><span>${t('system.services')} <span id="services-running">-</span> / <span id="services-total">-</span></span></button>
                    </div>
                    <div class="cols g20">
                        <div>
                            <div class="t-label">${t('system.cpuUsage')}</div>
                            <div class="bigrow"><span class="t-big" id="cpu-avg">-</span><span class="t-unit">%</span></div>
                            <div id="cpu-cores"></div>
                        </div>
                        <hr class="sep v">
                        <div>
                            <div class="between"><span class="t-label">${t('system.memory')}</span><button class="btn sm quiet" onclick="showMemoryDetailModal()" title="${t('system.memoryDetail')}">${t('system.detail')}</button></div>
                            <div class="mblock first"><div class="kv"><span>DRAM</span><span id="heap-pct">-</span></div><div class="bar"><i id="heap-progress"></i></div><div class="t-note num" id="heap-text">-</div></div>
                            <div class="mblock"><div class="kv"><span>PSRAM</span><span id="psram-pct">-</span></div><div class="bar"><i id="psram-progress"></i></div><div class="t-note num" id="psram-text">-</div></div>
                        </div>
                    </div>
                </div>

                <!-- 系统总览（含电源） -->
                <div class="card">
                    <div class="card-h">
                        <span class="t-section">${t('system.title')}</span>
                        <div class="acts">
                            <button class="btn sm" onclick="showShutdownSettingsModal()" title="${t('system.shutdownSettings')}"><svg class="i"><use href="#ri-shut-down-line"/></svg>${t('system.shutdownSettings')}</button>
                            <button id="usb-mux-btn" class="btn sm" onclick="toggleUsbMux()"><span>USB: <span id="usb-mux-target">-</span></span></button>
                            <button class="btn sm danger" onclick="confirmReboot()"><svg class="i"><use href="#ri-restart-line"/></svg>${t('system.reboot')}</button>
                        </div>
                    </div>
                    <div class="cols">
                        <div>
                            ${kvRow(t('system.chip'), '-', 'sys-chip')}
                            ${kvRow(t('system.firmware'), '-', 'sys-version')}
                            ${kvRow('ESP-IDF', '-', 'sys-idf')}
                            ${kvRow(t('system.uptime'), '-', 'sys-uptime')}
                            <div class="t-note" id="sys-compile" hidden></div>
                        </div>
                        <hr class="sep v">
                        <div>
                            ${kvRow(t('system.inputVoltage'), '-', 'voltage')}
                            ${kvRow(t('system.internalVoltage'), '-', 'internal-voltage')}
                            ${kvRow(t('system.current'), '-', 'current')}
                            ${kvRow(t('system.wattage'), '-', 'power-watts')}
                            <div class="kv"><span>${t('system.protection')}</span><span class="inl"><span id="protection-status">-</span><button id="protection-toggle" class="switch" role="switch" aria-checked="false" aria-label="${t('system.protection')}" title="${t('system.toggleProtectionTitle')}" onclick="toggleProtection()"></button></span></div>
                        </div>
                    </div>
                </div>

                <!-- 网络与时间 -->
                <div class="card">
                    <div class="card-h">
                        <span class="t-section">${t('system.networkTime')}</span>
                        <button class="btn sm" onclick="router.navigate('/ota')"><svg class="i"><use href="#ri-upload-line"/></svg>${t('nav.ota')}</button>
                    </div>
                    <div class="cols">
                        <div>
                            <div class="kv"><span>${t('system.ethernet')}</span><span><span class="state" id="eth-status">-</span></span></div>
                            <div class="kv"><span>${t('system.wifi')}</span><span><span class="state" id="wifi-status">-</span></span></div>
                            ${kvRow(t('system.ipAddress'), '-', 'ip-addr')}
                        </div>
                        <hr class="sep v">
                        <div>
                            ${kvRow(t('system.currentTime'), '-', 'sys-datetime')}
                            <div class="kv"><span>${t('system.timeStatus')}</span><span><span id="sys-time-status">-</span><span id="sys-time-src"> · <span id="sys-time-source">-</span></span></span></div>
                            ${kvRow(t('system.timezone'), '-', 'sys-timezone')}
                        </div>
                    </div>
                    <div class="acts end">
                        <button type="button" class="btn sm time-sync-btn" onclick="syncTimeFromBrowser()"><svg class="i"><use href="#ri-time-line"/></svg>${t('system.syncTime')}</button>
                        <button class="btn sm" onclick="showTimezoneModal()"><svg class="i"><use href="#ri-global-line"/></svg>${t('system.timezone')}</button>
                    </div>
                </div>
            </div>

            <div class="sys-mid">
                <!-- 设备面板 -->
                <div class="card">
                    <div class="card-h">
                        <span class="t-section">${t('system.devicePanel')}</span>
                        <div class="acts">
                            <button id="agx-power-btn" class="btn sm pw bad" onclick="toggleAgxPower()">${pwHtml('system.agxPowerName', t('system.powerOff'))}</button>
                            <button id="lpmu-power-btn" class="btn sm pw warn" onclick="toggleLpmuPower()">${pwHtml('system.lpmuPowerName', t('system.lpmuUnknown'))}</button>
                            <button class="btn sm" onclick="showWidgetManager()"><svg class="i"><use href="#ri-apps-line"/></svg>${t('system.widgetManager')}</button>
                        </div>
                    </div>
                    <div id="quick-actions-grid" class="qa-grid">
                        <div class="t-note">${t('common.loading')}</div>
                    </div>
                    <hr class="sep" style="margin:16px 0">
                    <div id="data-widgets-grid" class="dw-grid"></div>
                    <div id="data-widgets-empty" class="empty" style="display:none;">
                        <p class="t-note">${t('system.noDataWidgetsYet')}</p>
                    </div>
                </div>

                <!-- 风扇控制 -->
                <div class="card fan-panel">
                    <div class="card-h">
                        <span class="t-section">${t('fan.title')}</span>
                        <div class="acts">
                            <button type="button" class="btn sm icon fan-refresh-btn" onclick="refreshFans()" title="${t('common.refresh')}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                            <button type="button" class="btn sm" onclick="showFanCurveModal()"><svg class="i"><use href="#ri-line-chart-line"/></svg>${t('fan.curve')}</button>
                        </div>
                    </div>
                    <div class="fan-body">
                        <div class="fan-status" id="fan-temp-status-bar">
                            <span class="t-label">${t('fan.effectiveTemp')} <b class="t-value" id="fan-global-temp">--</b></span>
                            <span class="t-label">${t('fan.currentOutput')} <b class="t-value" id="fan-global-duty">--</b></span>
                        </div>
                        <div class="fans-grid" id="fans-grid">
                            <div class="loading">${t('common.loading')}</div>
                        </div>
                        <div class="fan-test">
                            <span class="t-label">${t('fan.testTemp')}</span>
                            <input type="number" id="fan-test-temp" class="field sm" placeholder="--" min="0" max="100" step="1" style="width:64px" aria-label="${t('fan.testTemp')}">
                            <button class="btn sm" onclick="applyTestTemp()">${t('common.test')}</button>
                            <button class="btn sm" onclick="clearTestTemp()">${t('fan.clearTest')}</button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- LED 控制 -->
            <div class="card" style="flex:none">
                <div class="card-h">
                    <span class="t-section">${t('led.title')}</span>
                    <div class="acts">
                        <button type="button" class="btn sm icon led-refresh-btn" onclick="refreshSystemLeds()" title="${t('common.refresh')}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                        <button class="btn sm system-led-cc-btn" id="system-led-cc-btn" onclick="openLedModal('matrix', 'colorcorrection')" style="display:none"><svg class="i"><use href="#ri-contrast-line"/></svg>${t('ledPage.colorCorrectionTitle')}</button>
                        <button class="btn sm danger" onclick="allLedsOff()">${t('led.allOff')}</button>
                    </div>
                </div>
                <div id="system-led-devices-grid" class="led-grid">
                    <div class="t-note">${t('ledPage.loadingDevices')}</div>
                </div>
            </div>
        </div>

        <!-- 服务详情模态框 - 复刻全局色彩校正，关闭按钮右上角 -->
        <div id="services-modal" class="modal hidden">${sheet(520, t('system.serviceStatusTitle'), '<div class="grp" id="services-body"></div>', `<button class="btn lg primary" onclick="hideServicesModal()">${t('common.close')}</button>`, 'hideServicesModal()')}</div>
    `;

    // 初始加载（不含快捷操作，避免 ssh.commands.list 慢响应阻塞）
    await refreshSystemPageOnce();
    if (!pageCurrent()) return;

    // 加载数据监控面板（优先执行，确保组件能实时获取变量数据）
    await initDataWidgets();
    if (!pageCurrent()) return;

    // 快捷操作延迟 2 秒后台加载，避免与 initDataWidgets 的首次变量刷新竞争 API
    quickActionsTimeoutId = setTimeout(() => {
        if (!pageCurrent()) return;
        quickActionsTimeoutId = null;
        void refreshQuickActions();
    }, 2000);

    // 订阅 WebSocket 实时更新 - 使用聚合订阅（system.dashboard）
    if (subscriptionManager) {
        const dashboardCallback = (msg) => {
            if (!pageCurrent()) return;
            console.log('[System Page] Received dashboard:', msg);
            if (!msg.data) return;

            const data = msg.data;

            // 分发到各个更新函数
            if (data.info) updateSystemInfo(data.info);
            if (data.memory) updateMemoryInfo(data.memory);
            if (data.cpu) updateCpuInfo(data.cpu);
            if (data.network) updateNetworkInfo(data.network);
            if (data.power) updatePowerInfo(data.power);
            if (data.fan) updateFanInfo(data.fan);
            if (data.services) updateServiceList(data.services);
        };
        const manager = subscriptionManager;
        manager.subscribe('system.dashboard', dashboardCallback, {interval: 1000});
        if (typeof router !== 'undefined') router.navigation?.onDispose(() => manager.unsubscribe('system.dashboard', dashboardCallback));
    }

    // 启动浏览器本地时间更新定时器
    startLocalTimeUpdate();

    // 启动设备状态实时监控
    startDeviceStateMonitor();

    // 初始化设备面板长按拖拽排序
    const widgetGrid = document.getElementById('data-widgets-grid');
    if (widgetGrid && typeof initLongPressDragSort === 'function') {
        const { destroy: destroyWidgetSort } = initLongPressDragSort(widgetGrid, {
            itemSelector: '.dw-card',
            idAttribute: 'data-widget-id',
            holdMs: 1500,
            onReorder(oldIdx, newIdx) {
                const [moved] = dataWidgets.splice(oldIdx, 1);
                dataWidgets.splice(newIdx, 0, moved);
                saveDataWidgets();
                renderDataWidgets();
            }
        });
        window._destroyWidgetSort = destroyWidgetSort;
    }
    const quickGrid = document.getElementById('quick-actions-grid');
    if (quickGrid && typeof initLongPressDragSort === 'function') {
        const { destroy: destroyQuickSort } = initLongPressDragSort(quickGrid, {
            itemSelector: '.quick-action-card',
            idAttribute: 'data-rule-id',
            holdMs: 1500,
            updateDOM: true,
            onReorder() {
                const newOrder = [...quickGrid.querySelectorAll('.quick-action-card')]
                    .map(el => el.getAttribute('data-rule-id')).filter(Boolean);
                try { localStorage.setItem('quick_actions_order', JSON.stringify(newOrder)); }
                catch (e) { /* 静默忽略 */ }
            }
        });
        window._destroyQuickSort = destroyQuickSort;
    }
}

// 单次刷新（初始加载）；refreshSystemPage 为别名，供时区/服务操作等调用
async function refreshSystemPageOnce() {
    const pageCurrent = capturePageValidity();
    // 系统信息
    try {
        const info = await api.getSystemInfo();
        if (!pageCurrent()) return;
        if (info.data) {
            updateSystemInfo(info.data);
        }
    } catch (e) {
        if (!pageCurrent()) return; console.log('System info error:', e); }

    // 时间信息
    try {
        const time = await api.timeInfo();
        if (!pageCurrent()) return;
        if (time.data) {
            updateTimeInfo(time.data);
        }
    } catch (e) {
        if (!pageCurrent()) return; console.log('Time info error:', e); }

    // 内存
    try {
        const mem = await api.getMemoryInfo();
        if (!pageCurrent()) return;
        if (mem.data) {
            updateMemoryInfo(mem.data);
        }
    } catch (e) {
        if (!pageCurrent()) return; console.log('Memory info error:', e); }

    // 网络
    try {
        const netStatus = await api.networkStatus();
        if (!pageCurrent()) return;
        if (netStatus.data) {
            updateNetworkInfo(netStatus.data);
        }
    } catch (e) {
        if (!pageCurrent()) return;
        document.getElementById('eth-status').textContent = '-';
        document.getElementById('wifi-status').textContent = '-';
    }

    // 电源
    try {
        const powerStatus = await api.powerStatus();
        if (!pageCurrent()) return;
        if (powerStatus.data) {
            updatePowerInfo(powerStatus.data);
        }
        const protStatus = await api.powerProtectionStatus();
        if (!pageCurrent()) return;
        if (protStatus.data) {
            const running = protStatus.data.running || protStatus.data.initialized;
            updateProtectionUI(running);
        }
    } catch (e) {
        if (!pageCurrent()) return;
        document.getElementById('voltage').textContent = '-';
        document.getElementById('current').textContent = '-';
        document.getElementById('power-watts').textContent = '-';
    }

    // 风扇
    try {
        const fans = await api.fanStatus();
        if (!pageCurrent()) return;
        updateFanInfo(fans.data);
    } catch (e) {
        if (!pageCurrent()) return;
        document.getElementById('fans-grid').innerHTML = '<p class="text-muted">' + t('fan.statusUnavailable') + '</p>';
    }

    // 服务列表
    try {
        const services = await api.serviceList();
        if (!pageCurrent()) return;
        updateServiceList(services.data);
    } catch (e) {
        if (!pageCurrent()) return;
        console.log('Services error:', e);
    }

    // LED 设备
    await refreshSystemLeds();
    if (!pageCurrent()) return;

    // 快捷操作已移至 loadSystemPage 末尾后台执行，避免阻塞 initDataWidgets

    // USB Mux 状态
    await refreshUsbMuxStatus();
    if (!pageCurrent()) return;

    // AGX 电源状态
    await refreshAgxPowerState();
    if (!pageCurrent()) return;

    // LPMU 状态检测
    await refreshLpmuState();
    if (!pageCurrent()) return;
}
const refreshSystemPage = refreshSystemPageOnce;

// =========================================================================
// USB Mux 状态和切换 (支持 ESP32 / AGX / LPMU 三设备循环)
// =========================================================================
let usbMuxTarget = 'esp32';
let usbMuxConfigured = false;

const USB_MUX_TARGETS = ['esp32', 'agx', 'lpmu'];
const USB_MUX_DISPLAY = { 'esp32': 'ESP', 'agx': 'AGX', 'lpmu': 'LPMU' };

async function refreshUsbMuxStatus() {
    const pageCurrent = capturePageValidity();
    try {
        const result = await api.call('device.usb.status');
        if (!pageCurrent()) return;
        if (result.code === 0 && result.data) {
            usbMuxConfigured = result.data.configured !== false;
            usbMuxTarget = result.data.target || 'esp32';
            updateUsbMuxButton();
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.warn('USB Mux status unavailable:', e.message);
        usbMuxConfigured = false;
        updateUsbMuxButton();
    }
}

function updateUsbMuxButton() {
    const targetEl = document.getElementById('usb-mux-target');
    const btn = document.getElementById('usb-mux-btn');

    if (!usbMuxConfigured) {
        if (targetEl) targetEl.textContent = t('common.notConfigured');
        if (btn) btn.disabled = true;
        return;
    }

    if (targetEl) targetEl.textContent = USB_MUX_DISPLAY[usbMuxTarget] || usbMuxTarget.toUpperCase();
    if (btn) btn.disabled = false;
}

/**
 * 更新保护状态 UI（图标和文字）
 */
function updateProtectionUI(running) {
    const sw = document.getElementById('protection-toggle');
    const statusSpan = document.getElementById('protection-status');

    if (sw) {
        sw.classList.toggle('on', !!running);
        sw.setAttribute('aria-checked', running ? 'true' : 'false');
    }
    if (statusSpan) statusSpan.textContent = running ? t('status.enabled') : t('status.disabled');
}

/**
 * 切换电压保护状态
 */
async function toggleProtection() {
    const icon = document.getElementById('protection-toggle');

    // 获取当前状态
    let currentRunning = false;
    try {
        const protStatus = await api.powerProtectionStatus();
        currentRunning = protStatus.data?.running || protStatus.data?.initialized || false;
    } catch (e) {
        console.error('Failed to get protection status:', e);
    }

    const newState = !currentRunning;

    // 临时禁用图标防止重复点击
    if (icon) icon.style.pointerEvents = 'none';

    try {
        const result = await api.powerProtectionSet({ enable: newState });

        if (result.code === 0) {
            const isRunning = result.data?.running ?? newState;
            updateProtectionUI(isRunning);
            showToast(isRunning ? t('system.protectionEnabled') : t('system.protectionDisabled'), isRunning ? 'success' : 'warning');
        } else {
            // 恢复原状态
            updateProtectionUI(currentRunning);
            showToast(t('system.switchFailed') + ': ' + (result.message || t('common.unknown')), 'error');
        }
    } catch (e) {
        // 恢复原状态
        updateProtectionUI(currentRunning);
        showToast(t('system.switchFailed') + ': ' + e.message, 'error');
    } finally {
        if (icon) icon.style.pointerEvents = 'auto';
    }
}

async function toggleUsbMux() {
    if (!usbMuxConfigured) {
        showToast(t('system.usbMuxNotConfigured'), 'warning');
        return;
    }

    // 循环切换: esp32 → agx → lpmu → esp32
    const currentIdx = USB_MUX_TARGETS.indexOf(usbMuxTarget);
    const nextIdx = (currentIdx + 1) % USB_MUX_TARGETS.length;
    const newTarget = USB_MUX_TARGETS[nextIdx];
    const displayName = USB_MUX_DISPLAY[newTarget];

    try {
        showToast(t('system.usbSwitchTo', { name: displayName }), 'info');
        const result = await api.call('device.usb.set', { target: newTarget }, 'POST');

        if (result.code === 0) {
            usbMuxTarget = newTarget;
            updateUsbMuxButton();
            showToast(t('system.usbSwitchedTo', { name: displayName }), 'success');
        } else {
            showToast(t('system.switchFailed') + ': ' + (result.message || t('common.unknown')), 'error');
        }
    } catch (e) {
        showToast(t('system.switchFailed') + ': ' + e.message, 'error');
    }
}

// AGX 电源控制（持续电平：LOW=上电，HIGH=断电）
let agxPowerState = false; // false=断电(HIGH), true=上电(LOW)

async function refreshAgxPowerState() {
    const pageCurrent = capturePageValidity();
    try {
        const result = await api.call('device.status', { device: 'agx' });
        if (!pageCurrent()) return;
        if (result.code === 0 && result.data) {
            agxPowerState = result.data.state === 'on' || result.data.state === 'booting';
            updateAgxPowerButton();
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.warn('AGX status unavailable:', e.message);
    }
}

function updateAgxPowerButton() {
    const btn = document.getElementById('agx-power-btn');
    if (!btn) return;

    btn.className = 'btn sm pw ' + (agxPowerState ? 'ok' : 'bad');
    btn.innerHTML = pwHtml('system.agxPowerName', t(agxPowerState ? 'system.powerRunning' : 'system.powerOff'));
    btn.title = t(agxPowerState ? 'system.agxPowerOffTitle' : 'system.agxPowerOnTitle');
}

async function toggleAgxPower() {
    const action = agxPowerState ? 'off' : 'on';
    const toastInfo = agxPowerState ? t('system.agxPoweringOff') : t('system.agxPoweringOn');
    const toastSuccess = agxPowerState ? t('system.agxPowerOffSuccess') : t('system.agxPowerOnSuccess');

    try {
        showToast(toastInfo, 'info');
        const result = await api.call('device.power', { device: 'agx', action: action }, 'POST');

        if (result.code === 0) {
            agxPowerState = !agxPowerState;
            updateAgxPowerButton();
            showToast(toastSuccess, 'success');
        } else {
            showToast(t('system.agxPowerFail') + ': ' + (result.message || t('common.unknown')), 'error');
        }
    } catch (e) {
        showToast(t('system.agxPowerFail') + ': ' + e.message, 'error');
    }
}

// LPMU 电源控制（脉冲触发，像按物理按钮）
// LPMU 状态: 'unknown' | 'online' | 'offline' | 'detecting'
let lpmuState = 'unknown';
let deviceStateInterval = null;
let lpmuPollingInterval = null;
let lpmuPollingStartTime = 0;
let lpmuPollingMode = 'startup'; // 'startup' | 'shutdown'

async function toggleLpmuPower() {
    if (!await confirmAction(t('system.lpmuTriggerConfirm'), { primary: t('common.trigger'), tone: 'neutral' })) {
        return;
    }

    try {
        showToast(t('system.lpmuTriggering'), 'info');
        // 记录触发前的状态（用于决定检测逻辑）
        const wasOnline = (lpmuState === 'online');

        // 使用 toggle 动作直接发送脉冲，不检查当前状态
        const result = await api.call('device.power', { device: 'lpmu', action: 'toggle' }, 'POST');

        if (result.code === 0) {
            showToast(t('system.lpmuTriggerSuccess'), 'success');
            // 启动状态检测（传入之前的状态）
            startLpmuStatePolling(wasOnline);
        } else {
            showToast(t('system.lpmuTriggerFail') + ': ' + (result.message || t('common.unknown')), 'error');
        }
    } catch (e) {
        showToast(t('system.lpmuTriggerFail') + ': ' + e.message, 'error');
    }
}

// 启动 LPMU 状态轮询（触发电源后调用）
let lpmuPollGeneration = 0;
function startLpmuStatePolling(wasOnline = false) {
    stopLpmuStatePolling();
    const generation = lpmuPollGeneration;
    lpmuState = 'detecting';
    lpmuPollingMode = wasOnline ? 'shutdown' : 'startup';
    updateLpmuPowerButton();
    lpmuPollingStartTime = Date.now();
    const minWaitSec = wasOnline ? 40 : 0;
    const maxWaitSec = wasOnline ? 60 : 80;
    let pending = false;
    lpmuPollingInterval = setInterval(async () => {
        if (pending) return;
        pending = true;
        let reachable = null;
        try {
            const result = requireApiSuccess(await api.call('device.ping', {host: '10.10.99.99', timeout: 1000}), 'device.ping');
            if (typeof result.data?.reachable === 'boolean') reachable = result.data.reachable;
        } catch (error) { console.warn('LPMU reachability check failed', error); }
        finally { pending = false; }
        if (generation !== lpmuPollGeneration) return;
        const elapsed = (Date.now() - lpmuPollingStartTime) / 1000;
        const reachedTarget = elapsed >= minWaitSec && reachable === !wasOnline;
        if (reachedTarget || elapsed >= maxWaitSec) {
            lpmuState = reachable === null ? 'unknown' : reachable ? 'online' : 'offline';
            stopLpmuStatePolling();
            updateLpmuPowerButton();
            const key = reachable === null ? 'lpmuProbeFailed' : reachable ? 'lpmuReachable' : 'lpmuUnreachable';
            showToast(t('promptRepair.' + key), 'info', 7000);
        } else updateLpmuPowerButton(Math.max(0, Math.ceil(maxWaitSec - elapsed)));
    }, 5000);
}

function stopLpmuStatePolling() {
    lpmuPollGeneration++;
    if (lpmuPollingInterval) clearInterval(lpmuPollingInterval);
    lpmuPollingInterval = null;
}

function startDeviceStateMonitor() {
    if (deviceStateInterval) {
        clearInterval(deviceStateInterval);
    }

    if (!lpmuPollingInterval) {
        refreshLpmuState();
    }
    refreshAgxPowerState();

    deviceStateInterval = setInterval(() => {
        if (!lpmuPollingInterval) {
            refreshLpmuState();
        }
    }, 10000);
}

function stopDeviceStateMonitor() {
    if (deviceStateInterval) {
        clearInterval(deviceStateInterval);
        deviceStateInterval = null;
    }
}

async function refreshLpmuState() {
    const pageCurrent = capturePageValidity();
    if (lpmuPollingInterval) return;

    try {
        const result = await api.call('device.ping', { host: '10.10.99.99', timeout: 1000 });
        if (!pageCurrent()) return;
        if (result.code === 0 && typeof result.data?.reachable === 'boolean') {
            lpmuState = result.data.reachable ? 'online' : 'offline';
        } else {
            lpmuState = 'unknown';
        }
    } catch (e) {
        if (!pageCurrent()) return;
        lpmuState = 'unknown';
    }
    updateLpmuPowerButton();
}

function updateLpmuPowerButton(remainingSec = 0) {
    const btn = document.getElementById('lpmu-power-btn');
    if (!btn) return;

    let cls = 'warn', status = t('system.lpmuUnknown'), title = t('system.lpmuUnknownTitle');
    if (lpmuState === 'online') { cls = 'ok'; status = t('system.lpmuOnline'); title = t('system.lpmuOnlineTitle'); }
    else if (lpmuState === 'offline') { cls = 'bad'; status = t('system.lpmuOffline'); title = t('system.lpmuOfflineTitle'); }
    else if (lpmuState === 'detecting') { status = t('system.statusFetching') + (remainingSec > 0 ? ' (' + remainingSec + 's)' : ''); title = t('system.lpmuDetectingTitle'); }
    btn.className = 'btn sm pw ' + cls;
    btn.innerHTML = pwHtml('system.lpmuPowerName', status);
    btn.title = title;
}

// 更新系统信息
function updateSystemInfo(data) {
    if (!data) return;
    document.getElementById('sys-chip').textContent = data.chip?.model || '-';
    const verEl = document.getElementById('sys-version');
    verEl.textContent = data.app?.version || '-';
    verEl.title = verEl.textContent;
    document.getElementById('sys-idf').textContent = data.app?.idf_version || '-';
    const compileEl = document.getElementById('sys-compile');
    compileEl.textContent = ((data.app?.compile_date || '') + ' ' + (data.app?.compile_time || '')).trim();
    compileEl.hidden = !compileEl.textContent;

    // 直接显示服务器提供的运行时间（不再前端计算）
    const uptimeElem = document.getElementById('sys-uptime');
    if (uptimeElem && data.uptime_ms !== undefined) {
        uptimeElem.textContent = formatUptime(data.uptime_ms);
    }
}

// 更新时间信息
// 启动本地时间更新定时器
let localTimeInterval = null;
function startLocalTimeUpdate() {
    // 清除旧定时器（如果存在）
    if (localTimeInterval) {
        clearInterval(localTimeInterval);
    }

    // 立即更新一次
    updateLocalTime();

    // 每秒更新
    localTimeInterval = setInterval(updateLocalTime, 1000);
}

function updateLocalTime() {
    const now = new Date();
    const datetimeElem = document.getElementById('sys-datetime');
    if (datetimeElem) {
        datetimeElem.textContent = now.toLocaleTimeString('zh-CN', { hour12: false });
        datetimeElem.title = now.toLocaleString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
    }
}

// 自动同步标志（避免重复触发）
let autoSyncTriggered = false;

function updateTimeInfo(data) {
    // 系统时间现在使用浏览器本地时间（通过 startLocalTimeUpdate 定时器更新）
    // 此函数保留以备后续扩展（例如显示 NTP 同步状态等）
    if (!data) return;

    // 检查 ESP32 时间是否早于 2025 年，自动同步浏览器时间（只触发一次）
    const deviceYear = data.year || (data.datetime ? parseInt(data.datetime.substring(0, 4)) : 0);
    if (deviceYear > 0 && deviceYear < 2025 && !autoSyncTriggered && !data.synced) {
        console.log(`检测到 ESP32 时间早于 2025 年 (${deviceYear})，自动从浏览器同步...`);
        autoSyncTriggered = true;  // 标记已触发，避免重复
        setTimeout(() => syncTimeFromBrowser(true), 500);  // 延迟执行避免阻塞页面加载
    }

    const statusText = data.synced ? (typeof t === 'function' ? t('system.timeSynced') : '已同步') : (typeof t === 'function' ? t('system.timeNotSynced') : '未同步');
    const statusElem = document.getElementById('sys-time-status');
    if (statusElem) {
        statusElem.textContent = statusText;
    }
    const sourceMap = typeof t === 'function' ? { ntp: t('system.timeSourceNtp'), http: t('system.timeSourceHttp'), manual: t('system.timeSourceManual'), none: t('system.timeSourceNone') } : { ntp: 'NTP', http: '浏览器', manual: '手动', none: '未同步' };
    const sourceElem = document.getElementById('sys-time-source');
    if (sourceElem) {
        sourceElem.textContent = sourceMap[data.source] || data.source;
        const srcWrap = document.getElementById('sys-time-src');
        if (srcWrap) srcWrap.hidden = !data.source || data.source === 'none';
    }
    const timezoneElem = document.getElementById('sys-timezone');
    if (timezoneElem) {
        timezoneElem.textContent = data.timezone || '-';
    }
}

// 更新内存信息
function memPair(used, total) {
    const mb = total >= 1048576, u = mb ? 1048576 : 1024, d = mb ? 1 : 0;
    return `${(used / u).toFixed(d)} / ${(total / u).toFixed(d)} ${mb ? 'MB' : 'KB'}`;
}
function updateMemoryInfo(data) {
    if (!data) return;
    const put = (name, total, free) => {
        const pct = Math.round(((total - free) / total) * 100);
        document.getElementById(name + '-progress').style.width = pct + '%';
        document.getElementById(name + '-pct').textContent = pct + '%';
        document.getElementById(name + '-text').textContent = memPair(total - free, total);
    };
    put('heap', data.internal?.total || 1, data.internal?.free || data.free_heap || 0);
    if (data.psram?.total) put('psram', data.psram.total, data.psram.free || 0);
    else document.getElementById('psram-text').textContent = typeof t === 'function' ? t('ui.psramUnavailable') : '不可用';
}

// 更新 CPU 信息
function updateCpuInfo(data) {
    if (!data || !data.cores) {
        console.log('CPU data missing cores:', data);
        return;
    }

    const container = document.getElementById('cpu-cores');
    if (!container) return;

    container.innerHTML = data.cores.map((core, i) => {
        const usage = Math.round(core.usage || 0);
        return `<div class="meter"><div class="kv"><span>Core ${core.id ?? i}</span><span>${usage}%</span></div><div class="bar"><i class="${usage > 80 ? 'bad' : usage > 50 ? 'warn' : ''}" style="width:${usage}%"></i></div></div>`;
    }).join('');

    const avgEl = document.getElementById('cpu-avg');
    if (avgEl) avgEl.textContent = Math.round(data.total_usage ?? data.cores.reduce((sum, c) => sum + (c.usage || 0), 0) / data.cores.length);
}

// 更新网络信息
function updateNetworkInfo(data) {
    if (!data) return;
    const eth = data.ethernet || {};
    const wifi = data.wifi || {};
    const setState = (id, ok) => {
        const el = document.getElementById(id);
        el.className = ok ? 'state ok' : 'state';
        el.textContent = ok ? t('status.connected') : t('status.disconnected');
    };
    setState('eth-status', eth.status === 'connected');
    setState('wifi-status', !!wifi.connected);
    document.getElementById('ip-addr').textContent = eth.ip || wifi.ip || '-';
}

// 更新电源信息
function updatePowerInfo(data) {
    if (!data) return;

    // 输入电压：来自电源芯片 (GPIO47 UART)
    const inputVoltage = data.power_chip?.voltage_v;
    // 内部电压：来自 ADC 监控 (GPIO18 ADC)
    const internalVoltage = data.voltage?.supply_v;

    const current = data.power_chip?.current_a || data.current?.value_a;
    const power = data.power_chip?.power_w || data.power?.value_w;

    // 显示输入电压（主电压）
    document.getElementById('voltage').textContent =
        (typeof inputVoltage === 'number' ? inputVoltage.toFixed(1) + ' V' : '-');

    // 显示内部电压（如果可用）- ADC 需要 -1V 校准
    const internalVoltageElem = document.getElementById('internal-voltage');
    if (internalVoltageElem) {
        const calibratedVoltage = typeof internalVoltage === 'number' ? internalVoltage - 1.0 : null;
        internalVoltageElem.textContent =
            (calibratedVoltage !== null ? calibratedVoltage.toFixed(2) + ' V' : '-');
    }

    document.getElementById('current').textContent =
        (typeof current === 'number' ? current.toFixed(2) + ' A' : '-');
    document.getElementById('power-watts').textContent =
        (typeof power === 'number' ? power.toFixed(1) + ' W' : '-');
}

// 更新风扇信息
const fanSpeedDrafts = new Map();
function fanOutputKnown(fan) {
    return fan.duty_valid !== false && Number.isFinite(fan.duty) && fan.duty >= 0 && fan.duty <= 100;
}

function updateFanInfo(data) {
    const container = document.getElementById('fans-grid');

    // 更新全局温度状态栏（从 data.temperature 获取绑定变量的温度）
    const globalTempEl = document.getElementById('fan-global-temp');
    const globalDutyEl = document.getElementById('fan-global-duty');
    if (globalTempEl && data?.temperature !== undefined) {
        globalTempEl.textContent = `${typeof data.temperature === 'number' ? data.temperature.toFixed(1) : '--'} °C`;
        globalTempEl.classList.toggle('warn', !data.temp_valid);
    }
    if (globalDutyEl) {
        const fans = data?.fans || [];
        const curveFan = fans.find(f => f.mode === 'curve' || f.mode === 'auto');
        const shown = curveFan ? [curveFan] : fans;
        const known = shown.length > 0 && shown.every(fanOutputKnown);
        globalDutyEl.textContent = known ? `${Math.round(shown.reduce((sum, f) => sum + f.duty, 0) / shown.length)}%` : '--%';
    }

    // Polling must not replace a range input under the user's pointer.
    const activeSlider = document.activeElement;
    const activeFan = data?.fans?.find(f => activeSlider?.id === `fan-slider-${f.id}`);
    if (activeFan?.mode === 'manual' && fanOutputKnown(activeFan) && fanSpeedDrafts.has(activeFan.id)) {
        const card = activeSlider.closest('.fan-card');
        card.querySelector('.fan-speed-num').textContent = activeFan.duty;
        const state = card.querySelector('.state');
        state.textContent = activeFan.fault ? t('fan.adjustmentFailed') : t('fanPage.modeManual');
        state.className = activeFan.fault ? 'state bad' : 'state ok';
        return;
    }

    if (data?.fans && data.fans.length > 0) {
        container.innerHTML = data.fans.map(fan => {
            const mode = fan.mode || 'auto';
            // 所有模式的大读数都与已成功下发的 PWM 一致。
            const known = fanOutputKnown(fan);
            const displayDuty = known ? fan.duty : '--';
            if (mode !== 'manual' || !known) fanSpeedDrafts.delete(fan.id);
            const draft = fanSpeedDrafts.get(fan.id);
            const duty = draft?.value ?? (known ? fan.duty : 0);
            const sliderText = `${draft?.value ?? displayDuty}%`;
            const rpm = fan.rpm || 0;
            const isManual = mode === 'manual';
            const isOff = mode === 'off';
            const hasAutoTelemetry = mode === 'auto' && (
                fan.auto_state ||
                typeof fan.guard_temperature === 'number' ||
                typeof fan.predicted_temperature === 'number' ||
                fan.temp_stale === true ||
                fan.guard_active === true
            );

            const _off = t('fanPage.modeOff'), _manual = t('fanPage.modeManual'), _smart = t('fanPage.modeSmart'), _curve = t('fanPage.modeCurve');
            const autoHelpTitle = escapeHtml(t('fanPage.smartHelpTitle'));
            const autoStateLabels = {
                idle: t('fanPage.autoStateIdle'), baseline: t('fanPage.autoStateBaseline'), active: t('fanPage.autoStateSmart'),
                guard: t('fanPage.autoStateGuard'), stale: t('fanPage.autoStateStale'), unknown: t('fanPage.autoStateUnknown')
            };
            const autoState = fan.auto_state || 'unknown';
            const stateText = !known ? t('fan.outputUnknown') : fan.fault ? t('fan.adjustmentFailed') : mode === 'auto' ? (autoStateLabels[autoState] || autoState) : ({ off: _off, manual: _manual, curve: _curve }[mode] || mode);
            const stateCls = !known || fan.fault ? ' bad' : isOff ? '' : fan.guard_active ? ' warn' : fan.temp_stale ? ' bad' : ' ok';
            const stat = (label, value) => `<span class="fan-stat"><i>${label}</i><b>${value}</b></span>`;
            const meta = '<div class="t-note fan-meta">' + (hasAutoTelemetry ? (
                (typeof fan.guard_temperature === 'number' ? stat(t('fanPage.guardTempBrief'), `${fan.guard_temperature.toFixed(1)} °C`) : '') +
                (typeof fan.predicted_temperature === 'number' ? stat(t('fanPage.predictedTempBrief'), `${fan.predicted_temperature.toFixed(1)} °C`) : '') +
                (typeof fan.slope_c_per_min === 'number' ? stat(t('fanPage.slopeTempShort'), `${fan.slope_c_per_min.toFixed(2)} ${t('fanPage.slopeTempUnit')}`) : '')) : '') + '</div>';
            const tab = (m, label) => `<button class="${mode === m ? 'on' : ''}" onclick="setFanMode(${fan.id}, '${m}')">${label}</button>`;

            return `
            <div class="card in fan-card ${isOff ? 'is-off' : ''}">
                <div class="card-h fan-head">
                    <span class="t-section">${t('fanPage.fanN', { id: fan.id })}</span>
                    <span class="inl"><span class="state${stateCls}">${stateText}</span>${mode === 'auto' ? `<button type="button" class="tti" onclick="showFanAutoHelpModal()" title="${autoHelpTitle}" aria-label="${autoHelpTitle}">TTI</button>` : ''}</span>
                </div>
                <div class="fan-readout"><div class="bigrow fan-big"><span class="t-big fan-speed-num">${displayDuty}</span><span class="t-unit">%</span></div><div class="t-label num fan-rpm">${rpm > 0 ? rpm + ' RPM' : '&nbsp;'}</div></div>
                <div class="seg full">${tab('off', _off)}${tab('manual', _manual)}${tab('curve', _curve)}${tab('auto', _smart)}</div>
                <div class="slrow ${isManual ? '' : 'disabled'}">
                    <span class="t-label">${t('fanPage.speedAdjust')}</span>
                    <div class="sl" style="--p:${duty}%"><i></i><b></b><input type="range" class="fan-slider" min="0" max="100" value="${duty}" id="fan-slider-${fan.id}" onchange="setFanSpeed(${fan.id}, this.value)" oninput="updateFanSliderUI(${fan.id}, this.value)" ${!isManual || !known ? 'disabled title="' + t('fanPage.manualModeHint') + '"' : ''}></div>
                    <span class="t-value fan-slider-value">${sliderText}</span>
                </div>
                <div class="t-note fan-draft">${draft ? t('fan.draftSetting', {speed: draft.value}) : ''}</div>
                ${known && Number.isFinite(fan.target_duty) && fan.target_duty !== fan.duty ? `<div class="t-note fan-request">${t('fan.requestedSetting', {speed: fan.target_duty})}</div>` : ''}
                ${meta}
            </div>
        `;
        }).join('');
    } else {
        container.innerHTML = '<p class="t-note">' + (typeof t === 'function' ? t('fanPage.noFans') : '无可用风扇') + '</p>';
    }
}

function showFanAutoHelpModal() {
    const existing = document.getElementById('fan-auto-help-modal');
    if (existing) existing.remove();

    const modal = document.createElement('div');
    modal.id = 'fan-auto-help-modal';
    modal.className = 'modal';
    modal.onclick = (event) => {
        if (event.target === modal) closeFanAutoHelpModal();
    };
    modal.innerHTML = sheet(560, t('fanPage.smartHelpTitle'),
        `<p class="t-body" style="color:var(--ink-2);line-height:22px;margin:0">${escapeHtml(t('fanPage.smartHelpBody'))}</p>`,
        `<button class="btn lg primary" onclick="closeFanAutoHelpModal()">${t('fanPage.autoHelpGotIt')}</button>`, 'closeFanAutoHelpModal()');
    document.body.appendChild(modal);
}

function closeFanAutoHelpModal() {
    document.getElementById('fan-auto-help-modal')?.remove();
}

// 更新滑块 UI（实时反馈）
function updateFanSliderUI(fanId, value) {
    const slider = document.getElementById(`fan-slider-${fanId}`);
    if (!slider) return;

    fanSpeedDrafts.set(fanId, {value: Number(value)});
    const card = slider.closest('.fan-card');
    if (card) {
        // 更新滑块旁边的值
        const valSpan = card.querySelector('.fan-slider-value');
        if (valSpan) valSpan.textContent = value + '%';
        const draftSpan = card.querySelector('.fan-draft');
        if (draftSpan) draftSpan.textContent = t('fan.draftSetting', {speed: value});
        slider.parentElement.style.setProperty('--p', value + '%');
    }
}

// 更新服务列表
function updateServiceList(data) {
    if (!data || !data.services) return;

    const services = data.services;
    const runningCount = services.filter(s => s.state === 'RUNNING').length;
    const totalCount = services.length;

    // 更新卡片统计
    const runningElem = document.getElementById('services-running');
    const totalElem = document.getElementById('services-total');
    if (runningElem) runningElem.textContent = runningCount;
    if (totalElem) totalElem.textContent = totalCount;

    // 更新模态框列表
    const tbody = document.getElementById('services-body');
    if (!tbody) return;

    tbody.innerHTML = services.map(svc => {
        const cls = svc.state === 'RUNNING' ? (svc.healthy ? 'ok' : 'warn') : svc.state === 'ERROR' ? 'bad' : 'warn';
        return row(svc.name, `<span class="state ${cls}">${userStateLabel(svc.state)}</span><button class="btn sm" onclick="serviceAction('${svc.name}', 'restart')">${t('system.reboot')}</button>`, svc.phase ? servicePhaseLabel(svc.phase) : '');
    }).join('');
}

// 显示/隐藏服务模态框
function showServicesModal() {
    const modal = document.getElementById('services-modal');
    if (modal) modal.classList.remove('hidden');
}

/**
 * 应用测试温度
 */
async function applyTestTemp() {
    const input = document.getElementById('fan-test-temp');
    const temp = parseFloat(input?.value);

    if (isNaN(temp) || temp < 0 || temp > 100) {
        showToast((typeof t === 'function' ? t('fan.enterValidTemp') : '请输入有效温度 (0-100°C)'), 'warning');
        return;
    }

    try {
        // 使用 temp.manual API 设置手动温度
        const result = await api.call('temp.manual', { temperature: temp });

        if (result.code === 0) {
            showToast((typeof t === 'function' ? t('fan.testTempSet', { temp }) : `测试温度已设置为 ${temp}°C`), 'success');
            // 刷新风扇状态
            await refreshFans();
        } else {
            showToast((typeof t === 'function' ? t('fan.setFailed', { msg: result.message }) : `设置失败: ${result.message}`), 'error');
        }
    } catch (e) {
        console.error('设置测试温度失败:', e);
        showToast((typeof t === 'function' ? t('fan.setFailed', { msg: e.message }) : `设置失败: ${e.message}`), 'error');
    }
}

/**
 * 清除测试温度（恢复正常模式）
 */
async function clearTestTemp() {
    try {
        // 清除手动温度，恢复自动模式
        const result = await api.call('temp.select', { source: 'variable' });

        if (result.code === 0) {
            showToast((typeof t === 'function' ? t('fan.testTempCleared') : '测试温度已清除，恢复正常模式'), 'success');
            document.getElementById('fan-test-temp').value = '';
            // 刷新风扇状态
            await refreshFans();
        } else {
            showToast((typeof t === 'function' ? t('fan.clearFailed', { msg: result.message }) : `清除失败: ${result.message}`), 'error');
        }
    } catch (e) {
        console.error('清除测试温度失败:', e);
        showToast((typeof t === 'function' ? t('fan.clearFailed', { msg: e.message }) : `清除失败: ${e.message}`), 'error');
    }
}

function hideServicesModal() {
    const modal = document.getElementById('services-modal');
    if (modal) modal.classList.add('hidden');
}

async function setFanSpeed(id, speed) {
    const draft = fanSpeedDrafts.get(id);
    const requested = Number(speed);
    try {
        const result = await api.fanSet(id, requested);
        if (!result || result.code !== 0 || (result.httpStatus && result.httpStatus >= 400)) {
            console.error('fan.set rejected', result);
            showToast(t('fan.settingFailed', {id, speed: requested}), 'error');
        } else {
            showToast(t(result.data?.enabled === false ? 'fan.settingDisabled' : 'fan.speedSet', {id, speed: requested}), 'success');
        }
        if (fanSpeedDrafts.get(id) === draft) fanSpeedDrafts.delete(id);
        await refreshFans();
    } catch (e) {
        console.error('fan.set result unavailable', e);
        if (fanSpeedDrafts.get(id) === draft) fanSpeedDrafts.delete(id);
        if (e instanceof ApiOperationError && e.code !== undefined && !e.uncertain) {
            showToast(t('fan.settingFailed', {id, speed: requested}), 'error');
            await refreshFans();
            return;
        }
        showToast(t('fan.settingUnknown'), 'warning');
        // Do not repeat a command whose outcome is unknown. A later status poll
        // supplies a current snapshot; it cannot prove the outcome of this command.
        markFanOutputsUnknown(id);
    }
}

async function setFanMode(id, mode) {
    try {
        requireApiSuccess(await api.call('fan.mode', { id: id, mode: mode }), 'call');
        showToast(typeof t === 'function' ? t('fan.modeSwitch', { id, mode: t({off:'common.disabled', manual:'common.manual', auto:'fanPage.modeSmart', curve:'fan.curve'}[mode] || 'common.unknown') }) : `风扇 ${id} 模式已切换为 ${mode}`, 'success');
        await refreshFans();
    } catch (e) { showToast((typeof t === 'function' ? t('fan.setFanModeFailed', { msg: e.message }) : '设置风扇模式失败: ' + e.message), 'error'); }
}

function markFanOutputsUnknown(id = null) {
    if (id === null) fanSpeedDrafts.clear();
    else fanSpeedDrafts.delete(id);
    for (const slider of document.querySelectorAll('.fan-slider')) {
        if (id !== null && slider.id !== `fan-slider-${id}`) continue;
        const card = slider.closest('.fan-card');
        card.querySelector('.fan-speed-num').textContent = '--';
        card.querySelector('.fan-slider-value').textContent = '--%';
        card.querySelector('.fan-draft').textContent = '';
        card.querySelector('.state').textContent = t('fan.outputUnknown');
        card.querySelector('.state').className = 'state bad';
        slider.disabled = true;
    }
    const global = document.getElementById('fan-global-duty');
    if (global) global.textContent = '--%';
}

async function refreshFans() {
    try {
        const result = requireApiSuccess(await api.call('fan.status'), 'fan.status');
        if (result.data) updateFanInfo(result.data);
        else markFanOutputsUnknown();
    } catch (e) {
        console.error('fan.status unavailable:', e);
        markFanOutputsUnknown();
    }
}

/*===========================================================================*/
/*                          风扇曲线管理                                       */
/*===========================================================================*/

// 加权温度变量绑定编辑状态
let tempVarBindings = [];
// 可用变量列表缓存（加载后填充）
let availableTempVars = [];

// 存储当前编辑的风扇曲线配置
let fanCurveConfig = {
    fanId: 0,
    hysteresis: 3.0,
    minInterval: 2000,
    minDuty: 20,
    maxDuty: 100,
    curve: [
        { temp: 30, duty: 30 },
        { temp: 50, duty: 60 },
        { temp: 70, duty: 100 }
    ]
};

/**
 * 显示风扇曲线管理模态框
 */
async function showFanCurveModal(fanId = 0) {
    fanCurveConfig.fanId = fanId;

    const modal = document.createElement('div');
    modal.id = 'fan-curve-modal';
    modal.className = 'modal';
    modal.onclick = (e) => { if (e.target === modal) closeFanCurveModal(); };

    const fanOpts = [0, 1, 2, 3].map(i => `<option value="${i}">${t('fanPage.fanN', { id: i })}</option>`).join('');
    const num = (id, w, val, attrs) => inp(id, w, '', 'num', `type="number" value="${val}" ${attrs}`);
    modal.innerHTML = sheet(660, t('fanPage.curveManagement'), `
        ${grp(row(t('fanPage.fanRow'), `<select class="field" id="fan-curve-fan-select" style="width:120px" onchange="updateFanCurvePreview()">${fanOpts}</select>`))}
        ${gt(t('fanPage.tempVarBindingTitle'))}
        <div class="grp">
            <div class="row"><div class="rl">${t('common.status')}</div><div class="rc"><span id="fan-curve-temp-current" class="t-note">--°C</span><span id="variable-bind-status" class="state">${t('fanPage.unbound')}</span></div></div>
            <div class="rows" id="temp-var-bindings-container"></div>
            <div class="row"><div class="rl"></div><div class="rc"><button class="btn sm" onclick="addTempVarBinding()"><svg class="i"><use href="#ri-add-line"/></svg>${t('fanPage.addVariable')}</button><button class="btn sm" onclick="bindTempVariable()">${t('fanPage.bind')}</button><button class="btn sm dg" onclick="unbindTempVariable()">${t('fanPage.unbindBtn')}</button></div></div>
        </div>
        <div id="temp-var-formula" class="t-note" style="display:none;margin:6px 4px 0"></div>
        <div id="temp-var-weight-warn" class="t-note warn-t" style="display:none;margin:6px 4px 0"></div>
        <div id="temp-source-hint" class="t-note" style="margin:6px 4px 0">${t('fanPage.selectVariableWeightHint')}</div>
        <div class="sec-h" style="margin:18px 4px 6px"><span class="t-label">${t('fanPage.curveNodes')}</span><span class="acts"><button class="btn sm" onclick="importFanCurveConfig()"><svg class="i"><use href="#ri-upload-line"/></svg>${t('fan.importConfig')}</button><button class="btn sm" onclick="exportFanCurveConfig()"><svg class="i"><use href="#ri-download-line"/></svg>${t('fan.exportConfig')}</button><button class="btn sm" onclick="addCurvePoint()"><svg class="i"><use href="#ri-add-line"/></svg>${t('fanPage.addNode')}</button></span></div>
        <div class="grp" id="fan-curve-points">${renderCurvePoints()}</div>
        <div class="t-note" style="margin:6px 4px 0">${t('fanPage.curveHint')}</div>
        ${gt(t('fanPage.curvePreview'))}
        <div class="cvbox"><canvas id="fan-curve-canvas" width="1240" height="160"></canvas></div>
        ${gt(t('fanPage.limits'))}
        ${grp(
            row(t('fanPage.minSpeed'), num('fan-curve-min-duty', 90, fanCurveConfig.minDuty, 'min="0" max="100" step="1"') + unit('%'), '', t('fanPage.minDutyHint')) +
            row(t('fanPage.maxSpeed'), num('fan-curve-max-duty', 90, fanCurveConfig.maxDuty, 'min="0" max="100" step="1"') + unit('%'), '', t('fanPage.maxDutyHint')) +
            row(t('fanPage.tempDiff'), num('fan-curve-hysteresis', 90, fanCurveConfig.hysteresis, 'min="0" max="20" step="0.5"') + unit('°C'), '', t('fanPage.hysteresisHint')) +
            row(t('fanPage.minIntervalShort'), num('fan-curve-interval', 90, fanCurveConfig.minInterval, 'min="500" max="30000" step="100"') + unit('ms'), '', t('fanPage.intervalHint')))}`,
        `<button class="btn lg" onclick="closeFanCurveModal()">${t('fanPage.cancel')}</button><button class="btn lg primary" onclick="applyFanCurve()">${t('fanPage.saveCurve')}</button>`);

    document.body.appendChild(modal);

    document.getElementById('fan-curve-fan-select').value = fanId;

    loadTempSourceStatus();
    setTimeout(() => drawCurvePreview(), 50);

    // 异步加载设备配置并更新 UI（不阻塞 modal 显示）
    api.call('fan.config', { id: fanId }).then(result => {
        if (result.code === 0 && result.data) {
            const cfg = result.data;
            if (cfg.curve && cfg.curve.length >= 2) fanCurveConfig.curve = cfg.curve;
            if (typeof cfg.hysteresis === 'number') fanCurveConfig.hysteresis = cfg.hysteresis;
            if (typeof cfg.min_interval === 'number') fanCurveConfig.minInterval = cfg.min_interval;
            if (typeof cfg.min_duty === 'number') fanCurveConfig.minDuty = cfg.min_duty;
            if (typeof cfg.max_duty === 'number') fanCurveConfig.maxDuty = cfg.max_duty;
            const hEl = document.getElementById('fan-curve-hysteresis');
            const iEl = document.getElementById('fan-curve-interval');
            const minEl = document.getElementById('fan-curve-min-duty');
            const maxEl = document.getElementById('fan-curve-max-duty');
            if (hEl) hEl.value = fanCurveConfig.hysteresis;
            if (iEl) iEl.value = fanCurveConfig.minInterval;
            if (minEl) minEl.value = fanCurveConfig.minDuty;
            if (maxEl) maxEl.value = fanCurveConfig.maxDuty;
            refreshCurveEditor();
        }
    }).catch(e => console.warn('从设备加载配置失败，使用默认值:', e));
}

/**
 * 关闭风扇曲线模态框
 */
function closeFanCurveModal() {
    const modal = document.getElementById('fan-curve-modal');
    if (modal) modal.remove();
}

/**
 * 渲染加权温度变量绑定列表
 */
function renderTempVarBindings() {
    const container = document.getElementById('temp-var-bindings-container');
    if (!container) return;

    if (tempVarBindings.length === 0) {
        container.innerHTML = row(t('dataWidget.tempVariables'), `<span class="t-note">${t('fanPage.unbound')}</span>`);
        updateWeightedTempFormula();
        return;
    }

    container.innerHTML = tempVarBindings.map((binding, index) => row(index === 0 ? t('dataWidget.tempVariables') : '',
        `<select class="field" style="width:170px" onchange="updateTempVarName(${index}, this.value)"><option value="">${t('fanPage.selectVariable')}</option>${buildVarSelectOptions(binding.name)}</select>` +
        `<input type="number" class="field num" style="width:64px" value="${binding.weight}" min="0" max="1" step="0.05" title="${t('fanPage.weight')}" aria-label="${t('fanPage.weight')}" onchange="updateTempVarWeight(${index}, this.value)" oninput="updateWeightedTempFormula()">` +
        `<button class="btn icon sm dg" onclick="removeTempVarBinding(${index})" title="${t('common.delete')}" aria-label="${t('common.delete')}"><svg class="i"><use href="#ri-delete-bin-line"/></svg></button>`)).join('');

    updateWeightedTempFormula();
}

/**
 * 构建变量 select options HTML（带分组）
 */
function buildVarSelectOptions(selectedName) {
    if (!availableTempVars || availableTempVars.length === 0) return '';

    const isPriorityTempVar = (v) => {
        const name = (v.name || '').toLowerCase();
        return name.includes('temp') || name.includes('tj') ||
            name.includes('cpu') || name.includes('gpu');
    };
    const renderOption = (v) => {
        const name = v.name || '';
        const safeName = typeof escapeHtml === 'function' ? escapeHtml(name) : name;
        const valueText = typeof v.value === 'number' ? ` (${v.value.toFixed(1)}°C)` : '';
        const sel = name === selectedName ? 'selected' : '';
        return `<option value="${safeName}" ${sel}>${safeName}${valueText}</option>`;
    };
    const priorityVars = availableTempVars.filter(isPriorityTempVar);
    const otherVars = availableTempVars.filter(v => !isPriorityTempVar(v));

    let html = '';
    if (priorityVars.length > 0) {
        html += `<optgroup label="${t('dataWidget.tempVariables') || t('dataWidget.tempVariables')}">`;
        priorityVars.forEach(v => {
            html += renderOption(v);
        });
        html += `</optgroup>`;
    }
    if (otherVars.length > 0) {
        html += `<optgroup label="${t('dataWidget.otherNumericVariables') || t('dataWidget.otherNumericVariables')}">`;
        otherVars.forEach(v => {
            html += renderOption(v);
        });
        html += `</optgroup>`;
    }

    /* 如果已选变量不在列表中，追加显示 */
    if (selectedName && !availableTempVars.find(v => v.name === selectedName)) {
        const safeName = typeof escapeHtml === 'function' ? escapeHtml(selectedName) : selectedName;
        html += `<option value="${safeName}" selected>${safeName} (${t('common.current') || t('common.current')})</option>`;
    }

    return html;
}

/**
 * 添加一个空的温度变量绑定行
 */
function addTempVarBinding() {
    if (tempVarBindings.length >= 8) {
        showToast(t('fanPage.maxBoundVars', { max: 8 }), 'warning');
        return;
    }
    const defaultWeight = tempVarBindings.length === 0 ? 1.0 : 0.5;
    tempVarBindings.push({ name: '', weight: defaultWeight });
    renderTempVarBindings();
}

/**
 * 删除指定索引的温度变量绑定行
 */
function removeTempVarBinding(index) {
    tempVarBindings.splice(index, 1);
    renderTempVarBindings();
}

/**
 * 更新指定行的变量名
 */
function updateTempVarName(index, name) {
    if (index >= 0 && index < tempVarBindings.length) {
        tempVarBindings[index].name = name;
        updateWeightedTempFormula();
    }
}

/**
 * 更新指定行的权重
 */
function updateTempVarWeight(index, weight) {
    if (index >= 0 && index < tempVarBindings.length) {
        let w = parseFloat(weight);
        if (isNaN(w)) w = 0;
        if (w < 0) w = 0;
        if (w > 1) w = 1;
        tempVarBindings[index].weight = w;
        updateWeightedTempFormula();
    }
}

/**
 * 更新加权温度公式展示
 */
function updateWeightedTempFormula() {
    const formulaEl = document.getElementById('temp-var-formula');
    const warnEl = document.getElementById('temp-var-weight-warn');
    if (!formulaEl) return;

    const validBindings = tempVarBindings.filter(b => b.name && b.weight > 0);

    if (validBindings.length === 0) {
        formulaEl.style.display = 'none';
        if (warnEl) warnEl.style.display = 'none';
        return;
    }

    /* 从缓存的变量列表获取当前值 */
    let parts = [];
    let weightedSum = 0;
    let totalWeight = 0;
    let allValuesKnown = true;

    validBindings.forEach(b => {
        const varInfo = availableTempVars.find(v => v.name === b.name);
        const val = varInfo?.value;
        if (typeof val === 'number') {
            parts.push(`${val.toFixed(1)}°C × ${b.weight}`);
            weightedSum += val * b.weight;
        } else {
            allValuesKnown = false;
            parts.push(`--°C × ${b.weight}`);
        }
        totalWeight += b.weight;
    });

    let formula = parts.join(' + ');
    if (allValuesKnown && totalWeight > 0.001) {
        formula += ` = <span class="result">${(weightedSum / totalWeight).toFixed(1)}°C</span>`;
    } else {
        formula += ` = <span class="result">${t('fanPage.variableNoData')}</span>`;
    }

    formulaEl.innerHTML = `${t('fanPage.weightedTemp')}: ${formula}`;
    formulaEl.style.display = 'block';

    /* 权重总和警告 */
    if (warnEl) {
        const sum = tempVarBindings.reduce((s, b) => s + (b.weight || 0), 0);
        if (Math.abs(sum - 1.0) > 0.01 && tempVarBindings.length > 0) {
            warnEl.textContent = t('fanPage.weightSumWarning', { sum: sum.toFixed(2) });
            warnEl.style.display = 'block';
        } else {
            warnEl.style.display = 'none';
        }
    }
}

/**
 * 渲染曲线点列表
 */
function renderCurvePoints() {
    const _tempPh = t('fanPage.tempPlaceholder');
    const _speedPh = t('fanPage.speedPlaceholder');
    return fanCurveConfig.curve.map((point, index) => row(t('fanPage.nodeN', { n: index + 1 }),
        `<input type="number" class="field num" style="width:70px" value="${point.temp}" min="-20" max="120" step="1" onchange="updateCurvePoint(${index}, 'temp', this.value)" placeholder="${_tempPh}" aria-label="${_tempPh}">` +
        `<span class="t-note">°C →</span>` +
        `<input type="number" class="field num" style="width:70px" value="${point.duty}" min="0" max="100" step="1" onchange="updateCurvePoint(${index}, 'duty', this.value)" placeholder="${_speedPh}" aria-label="${_speedPh}">` +
        unit('%') +
        `<button class="btn icon sm dg" onclick="removeCurvePoint(${index})" title="${t('fanPage.deleteNode')}" aria-label="${t('fanPage.deleteNode')}" ${fanCurveConfig.curve.length <= 2 ? 'disabled' : ''}><svg class="i"><use href="#ri-delete-bin-line"/></svg></button>`)).join('');
}

/**
 * 添加曲线点
 */
function addCurvePoint() {
    if (fanCurveConfig.curve.length >= 10) {
        showToast(typeof t === 'function' ? t('fanPage.maxCurvePoints') : '最多支持 10 个曲线点', 'warning');
        return;
    }

    // 在最后一个点后添加
    const lastPoint = fanCurveConfig.curve[fanCurveConfig.curve.length - 1];
    const newTemp = Math.min(lastPoint.temp + 10, 100);
    const newDuty = Math.min(lastPoint.duty + 10, 100);

    fanCurveConfig.curve.push({ temp: newTemp, duty: newDuty });
    refreshCurveEditor();
}

/**
 * 删除曲线点
 */
function removeCurvePoint(index) {
    if (fanCurveConfig.curve.length <= 2) {
        showToast(typeof t === 'function' ? t('fanPage.minCurvePoints') : '至少需要 2 个曲线点', 'warning');
        return;
    }
    fanCurveConfig.curve.splice(index, 1);
    refreshCurveEditor();
}

/**
 * 更新曲线点
 */
function updateCurvePoint(index, field, value) {
    fanCurveConfig.curve[index][field] = parseFloat(value);
    // 排序（按温度升序）
    fanCurveConfig.curve.sort((a, b) => a.temp - b.temp);
    drawCurvePreview();
}

/**
 * 刷新曲线编辑器
 */
function refreshCurveEditor() {
    // 按温度排序
    fanCurveConfig.curve.sort((a, b) => a.temp - b.temp);

    const container = document.getElementById('fan-curve-points');
    if (container) {
        container.innerHTML = renderCurvePoints();
    }
    drawCurvePreview();
}

/**
 * 绘制曲线预览
 */
function drawCurvePreview() {
    const canvas = document.getElementById('fan-curve-canvas');
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const W = canvas.clientWidth, H = canvas.clientHeight;
    if (!W || !H) return;
    canvas.width = Math.round(W * dpr);
    canvas.height = Math.round(H * dpr);
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    const pad = { top: 28, right: 20, bottom: 28, left: 46 };
    const pw = W - pad.left - pad.right, ph = H - pad.top - pad.bottom;
    const px = temp => pad.left + Math.max(0, Math.min(100, temp)) / 100 * pw;
    const py = duty => pad.top + ph - Math.max(0, Math.min(100, duty)) / 100 * ph;
    const font = getComputedStyle(document.body).fontFamily || 'system-ui';

    // 网格（20 为一格）+ 坐标轴刻度数字：横轴温度 °C，纵轴转速 %
    ctx.lineWidth = 1;
    ctx.font = '11px ' + font;
    ctx.fillStyle = '#5f6670';
    for (let v = 0; v <= 100; v += 20) {
        const x = Math.round(px(v)) + 0.5, y = Math.round(py(v)) + 0.5;
        ctx.strokeStyle = v === 0 ? 'rgba(60,60,67,.32)' : 'rgba(60,60,67,.12)';
        ctx.beginPath(); ctx.moveTo(x, pad.top); ctx.lineTo(x, pad.top + ph); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(pad.left, y); ctx.lineTo(pad.left + pw, y); ctx.stroke();
        ctx.textAlign = 'center';
        ctx.fillText(v + '°C', px(v), H - 8);
        ctx.textAlign = 'right';
        ctx.fillText(v + '%', pad.left - 8, py(v) + 4);
    }
    if (fanCurveConfig.curve.length < 2) return;

    // 折线：低于最低点 / 高于最高点时保持水平；下方淡色填充
    const points = [...fanCurveConfig.curve].sort((a, b) => a.temp - b.temp);
    const right = pad.left + pw, base = pad.top + ph;
    ctx.beginPath();
    ctx.moveTo(pad.left, py(points[0].duty));
    points.forEach(pt => ctx.lineTo(px(pt.temp), py(pt.duty)));
    ctx.lineTo(right, py(points[points.length - 1].duty));
    ctx.lineTo(right, base);
    ctx.lineTo(pad.left, base);
    ctx.closePath();
    const grad = ctx.createLinearGradient(0, pad.top, 0, base);
    grad.addColorStop(0, 'rgba(0,122,255,.18)');
    grad.addColorStop(1, 'rgba(0,122,255,0)');
    ctx.fillStyle = grad;
    ctx.fill();
    ctx.beginPath();
    ctx.moveTo(pad.left, py(points[0].duty));
    points.forEach(pt => ctx.lineTo(px(pt.temp), py(pt.duty)));
    ctx.lineTo(right, py(points[points.length - 1].duty));
    ctx.strokeStyle = '#007aff';
    ctx.lineWidth = 2.5;
    ctx.lineJoin = 'round';
    ctx.stroke();

    // 节点：白底蓝圈 + 「温度°/转速%」标注：优先放在点左上方（折线向右上走，不压线），放不下再居中 / 靠左，仍冲突放到点下方
    const boxes = [];
    const hit = (r) => boxes.some(q => r.x < q.x + q.w && r.x + r.w > q.x && r.y < q.y + q.h && r.y + r.h > q.y);
    points.forEach(pt => {
        const x = px(pt.temp), y = py(pt.duty);
        ctx.beginPath(); ctx.arc(x, y, 4.5, 0, Math.PI * 2);
        ctx.fillStyle = '#fff'; ctx.fill();
        ctx.lineWidth = 2; ctx.strokeStyle = '#007aff'; ctx.stroke();
        const label = `${pt.temp}°/${pt.duty}%`;
        ctx.font = '600 10.5px ' + font;
        const tw = ctx.measureText(label).width, th = 12;
        const tries = [[x + 6 - tw, y - 20, 'right', x + 6], [x - tw / 2, y - 20, 'center', x], [x - 6, y - 20, 'left', x - 6], [x + 6 - tw, y + 8, 'right', x + 6], [x - tw / 2, y + 8, 'center', x]];
        let pick = tries.find(([lx, ly]) => lx >= pad.left - 2 && lx + tw <= right + 6 && ly >= 2 && ly + th <= H - 18 && !hit({ x: lx, y: ly, w: tw, h: th })) || tries[1];
        boxes.push({ x: pick[0], y: pick[1], w: tw, h: th });
        ctx.fillStyle = '#1d1d1f'; ctx.textAlign = pick[2];
        ctx.fillText(label, pick[3], pick[1] + 10);
    });
}

/**
 * 加载温度源状态
 */
async function loadTempSourceStatus() {
    try {
        const result = await api.call('temp.status');
        if (result.code === 0 && result.data) {
            const data = result.data;
            const tempEl = document.getElementById('fan-curve-temp-current');
            if (tempEl) {
                const temp = data.temperature_c?.toFixed(1) || '--';
                tempEl.textContent = `${temp}°C`;
                tempEl.style.color = data.valid ? '' : 'var(--warn)';
            }
        }
    } catch (e) {
        console.error('获取温度源状态失败:', e);
        const tempEl = document.getElementById('fan-curve-temp-current');
        if (tempEl) {
            tempEl.textContent = '??°C';
            tempEl.style.color = 'var(--bad)';
        }
    }
    await loadVariableBindStatus();
}

/**
 * 加载变量绑定状态和变量列表
 */
async function loadVariableBindStatus() {
    // 两个 API 调用独立 try-catch，互不阻塞
    try {
        const varsResult = await api.call('automation.variables.list', {
            include_value: true,
            include_meta: false
        });
        if (varsResult.code === 0 && varsResult.data?.variables) {
            const tempVars = varsResult.data.variables.filter(v => {
                const name = (v.name || '').toLowerCase();
                return v.type === 'float' || v.type === 'double' ||
                    v.type === 'number' || v.type === 'int' ||
                    name.includes('temp') || name.includes('cpu') ||
                    name.includes('gpu') || name.includes('tj');
            });
            availableTempVars = tempVars.length > 0 ? tempVars : varsResult.data.variables.filter(v =>
                v.type === 'float' || v.type === 'double' ||
                v.type === 'number' || v.type === 'int' ||
                typeof v.value === 'number'
            );
        }
    } catch (e) {
        console.warn('加载变量列表失败:', e.message);
    }

    try {
        const bindResult = await api.call('temp.bind');
        const statusEl = document.getElementById('variable-bind-status');

        if (bindResult.code === 0 && bindResult.data) {
            const data = bindResult.data;
            const boundVars = data.bound_variables || [];

            if (boundVars.length > 0) {
                tempVarBindings = boundVars.map(bv => ({
                    name: bv.name,
                    weight: bv.weight ?? 1.0
                }));
                boundVars.forEach(bv => {
                    if (!bv.name) return;
                    let existing = availableTempVars.find(v => v.name === bv.name);
                    if (!existing) {
                        existing = { name: bv.name, type: typeof bv.value === 'number' ? 'float' : 'unknown' };
                        availableTempVars.push(existing);
                    }
                    if (typeof bv.value === 'number') {
                        existing.value = bv.value;
                    }
                    existing.valid = bv.valid;
                    existing.stale = bv.stale;
                    existing.last_update_ms = bv.last_update_ms;
                    existing.age_ms = bv.age_ms;
                });
            } else if (data.bound_variable) {
                tempVarBindings = [{ name: data.bound_variable, weight: 1.0 }];
            } else {
                tempVarBindings = [];
            }

            const positiveBoundVars = boundVars.filter(bv => (bv.weight ?? 1.0) > 0.001);
            const positiveCount = typeof data.bound_total_count === 'number'
                ? data.bound_total_count : positiveBoundVars.length;
            const validCount = typeof data.bound_valid_count === 'number'
                ? data.bound_valid_count
                : positiveBoundVars.filter(bv => bv.valid === true).length;
            const staleCount = positiveBoundVars.filter(bv => bv.stale === true).length;
            const freshCount = Math.max(0, positiveCount - staleCount);
            const partialStale = data.partial_stale === true ||
                (positiveCount > 0 && validCount < positiveCount);
            const hasWeightedTemp = typeof data.weighted_temp_c === 'number';

            if (statusEl) {
                if (tempVarBindings.length > 0) {
                    if (positiveCount > 0 && validCount === 0 && staleCount === positiveCount) {
                        statusEl.textContent = t('fanPage.boundVarsStale', { count: tempVarBindings.length });
                        statusEl.className = 'state warn';
                    } else if (positiveCount > 0 && partialStale && validCount > 0) {
                        statusEl.textContent = t('fanPage.boundVarsPartialStale', { fresh: validCount, count: positiveCount });
                        statusEl.className = 'state warn';
                    } else if (positiveCount > 0 && !hasWeightedTemp) {
                        statusEl.textContent = t('fanPage.boundVarsInvalid', { count: tempVarBindings.length });
                        statusEl.className = 'state warn';
                    } else if (positiveCount === 0) {
                        statusEl.textContent = t('fanPage.boundVarsInvalid', { count: tempVarBindings.length });
                        statusEl.className = 'state warn';
                    } else {
                        statusEl.textContent = t('fanPage.boundVarCount', { count: tempVarBindings.length });
                        statusEl.className = 'state ok';
                    }
                } else {
                    statusEl.textContent = t('fanPage.unbound');
                    statusEl.className = 'state';
                }
            }

            const tempEl = document.getElementById('fan-curve-temp-current');
            if (tempEl && hasWeightedTemp) {
                tempEl.textContent = `${data.weighted_temp_c.toFixed(1)}°C`;
                tempEl.style.color = '';
            } else if (tempEl && positiveCount > 0) {
                if (validCount === 0 && staleCount === positiveCount) {
                    tempEl.textContent = t('fanPage.boundVarsStaleShort');
                } else if (partialStale && validCount > 0) {
                    tempEl.textContent = t('fanPage.boundVarsPartialStale', { fresh: validCount, count: positiveCount });
                } else {
                    tempEl.textContent = freshCount === 0 ? t('fanPage.boundVarsStaleShort') : t('fanPage.boundVarsInvalidShort');
                }
                tempEl.style.color = 'var(--warn)';
            } else if (tempEl && typeof data.temperature_c === 'number') {
                tempEl.textContent = `${data.temperature_c.toFixed(1)}°C`;
                tempEl.style.color = '';
            }
        }
    } catch (e) {
        console.warn('加载绑定状态失败:', e.message);
    }

    renderTempVarBindings();
}

/**
 * 绑定温度变量
 */
async function bindTempVariable() {
    const validBindings = tempVarBindings.filter(b => b.name && b.name.trim() !== '');

    if (validBindings.length === 0) {
        showToast(t('fanPage.noVarSelected'), 'warning');
        return;
    }

    try {
        const variables = validBindings.map(b => ({
            name: b.name,
            weight: Math.max(0, Math.min(1, b.weight || 0))
        }));

        const result = await api.call('temp.bind', { variables });

        if (result.code === 0) {
            requireApiSuccess(await api.call('temp.select', { source: 'variable' }), 'call');

            showToast(t('fanPage.tempBoundWeighted', { count: variables.length }), 'success');

            await loadTempSourceStatus();
        } else {
            showToast(t('fanPage.bindFailed') + ': ' + result.message, 'error');
        }
    } catch (e) {
        console.error('绑定温度变量失败:', e);
        showToast(t('fanPage.bindFailed') + ': ' + e.message, 'error');
    }
}

/**
 * 解绑温度变量
 */
async function unbindTempVariable() {
    try {
        const result = await api.call('temp.bind', { variables: [] });

        if (result.code === 0) {
            tempVarBindings = [];
            showToast(t('fanPage.unbindSuccess'), 'success');

            await loadTempSourceStatus();
        } else {
            showToast(t('fanPage.unbindFailed') + ': ' + result.message, 'error');
        }
    } catch (e) {
        console.error('解绑温度变量失败:', e);
        showToast(t('fanPage.unbindFailed') + ': ' + e.message, 'error');
    }
}

/**
 * 保存 AGX 服务器配置 (保留用于兼容)
 */
async function saveAgxConfig() {
    showToast((typeof t === 'function' ? t('toast.agxMovedToBinding') : 'AGX 配置已移至变量绑定'), 'info');
    await loadVariableBindStatus();
}

/**
 * 应用风扇曲线
 */
async function applyFanCurve() {
    const fanId = parseInt(document.getElementById('fan-curve-fan-select').value);
    const hysteresis = parseFloat(document.getElementById('fan-curve-hysteresis').value);
    const minInterval = parseInt(document.getElementById('fan-curve-interval').value);
    const minDuty = parseInt(document.getElementById('fan-curve-min-duty').value);
    const maxDuty = parseInt(document.getElementById('fan-curve-max-duty').value);

    // 验证
    if (fanCurveConfig.curve.length < 2) {
        showToast(typeof t === 'function' ? t('fanPage.minCurvePoints') : '至少需要 2 个曲线点', 'error');
        return;
    }

    if (minDuty > maxDuty) {
        showToast(typeof t === 'function' ? t('fanPage.dutyOrderError') : '最小占空比不能大于最大占空比', 'error');
        return;
    }

    // 验证温度迟滞和最小间隔（防止 NaN 导致保存失败）
    if (isNaN(hysteresis) || hysteresis < 0 || hysteresis > 20) {
        showToast(typeof t === 'function' ? t('fanPage.hysteresisRangeError') : '温度迟滞必须在 0-20°C 范围内', 'error');
        return;
    }

    if (isNaN(minInterval) || minInterval < 500 || minInterval > 30000) {
        showToast(typeof t === 'function' ? t('fanPage.intervalRangeError') : '最小间隔必须在 500-30000ms 范围内', 'error');
        return;
    }

    // 排序曲线点
    const sortedCurve = [...fanCurveConfig.curve].sort((a, b) => a.temp - b.temp);

    try {
        // 1. 设置占空比限制
        const limitsResult = await api.call('fan.limits', {
            id: fanId,
            min_duty: minDuty,
            max_duty: maxDuty
        });

        if (limitsResult.code !== 0) {
            throw new Error(limitsResult.message || t('dataWidget.setDutyLimitFailed'));
        }

        // 2. 设置曲线（同时传递 hysteresis 和 min_interval，会自动保存到 NVS）
        const curveResult = await api.call('fan.curve', {
            id: fanId,
            curve: sortedCurve,
            hysteresis: hysteresis,
            min_interval: minInterval
        });

        if (curveResult.code !== 0) {
            throw new Error(curveResult.message || t('dataWidget.setCurveFailed'));
        }

        // 3. 切换到曲线模式
        const modeResult = await api.call('fan.mode', {
            id: fanId,
            mode: 'curve'
        });

        if (modeResult.code !== 0) {
            throw new Error(modeResult.message || t('promptRepair.modeFailed'));
        }

        showToast(typeof t === 'function' ? t('fanPage.curveApplied', { id: fanId }) : `风扇 ${fanId} 曲线已应用并保存`, 'success');
        closeFanCurveModal();

        // 刷新风扇状态
        await refreshFans();

    } catch (e) {
        console.error('应用曲线失败:', e);
        showToast((typeof t === 'function' ? t('fanPage.applyCurveFailed') : '应用曲线失败') + ': ' + e.message, 'error');
    }
}

/**
 * 导出风扇曲线配置到本地 JSON 文件，并同时保存到 SD 卡 /sdcard/config
 */
async function exportFanCurveConfig() {
    const fanId = parseInt(document.getElementById('fan-curve-fan-select').value);
    const hysteresis = parseFloat(document.getElementById('fan-curve-hysteresis').value);
    const minInterval = parseInt(document.getElementById('fan-curve-interval').value);
    const minDuty = parseInt(document.getElementById('fan-curve-min-duty').value);
    const maxDuty = parseInt(document.getElementById('fan-curve-max-duty').value);

    // 构建导出配置
    const config = {
        version: 1,
        type: 'fan_curve_config',
        fan_id: fanId,
        curve: [...fanCurveConfig.curve].sort((a, b) => a.temp - b.temp),
        hysteresis: hysteresis,
        min_interval: minInterval,
        min_duty: minDuty,
        max_duty: maxDuty,
        exported_at: new Date().toISOString()
    };

    const json = JSON.stringify(config, null, 2);
    const blob = new Blob([json], { type: 'application/json' });

    // 1. 保存到本地（触发浏览器下载）
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `fan_curve_config_${fanId}.json`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);

    // 2. 同时保存到 SD 卡 /sdcard/config
    const sdcardPath = `/sdcard/config/fan_curve_config_${fanId}.json`;
    try {
        await api.fileUpload(sdcardPath, blob);
        showToast(typeof t === 'function' ? t('fanPage.curveExported', { id: fanId, path: sdcardPath }) : `风扇 ${fanId} 曲线配置已导出（本地 + SD 卡 ${sdcardPath}）`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('fanPage.curveExportSdFailed', { id: fanId, msg: e.message }) : `风扇 ${fanId} 曲线已保存到本地，SD 卡保存失败: ${e.message}`, 'warning');
    }
}

/**
 * 导入风扇曲线配置（从本地 JSON 文件）
 */
function importFanCurveConfig() {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';

    input.onchange = async (e) => {
        const file = e.target.files[0];
        if (!file) return;

        try {
            const text = await file.text();
            const config = JSON.parse(text);

            // 验证配置格式
            if (config.type !== 'fan_curve_config') {
                showToast(typeof t === 'function' ? t('fanPage.invalidConfigFormat') : '无效的配置文件格式', 'error');
                return;
            }

            if (!config.curve || !Array.isArray(config.curve) || config.curve.length < 2) {
                showToast(typeof t === 'function' ? t('fanPage.invalidCurvePoints') : '配置文件中曲线点无效', 'error');
                return;
            }

            for (const point of config.curve) {
                if (typeof point.temp !== 'number' || typeof point.duty !== 'number') {
                    showToast(typeof t === 'function' ? t('fanPage.curvePointFormatError') : '曲线点格式错误', 'error');
                    return;
                }
            }

            // 应用配置到当前界面
            fanCurveConfig.curve = config.curve.sort((a, b) => a.temp - b.temp);

            if (typeof config.hysteresis === 'number') {
                fanCurveConfig.hysteresis = config.hysteresis;
                document.getElementById('fan-curve-hysteresis').value = config.hysteresis;
            }
            if (typeof config.min_interval === 'number') {
                fanCurveConfig.minInterval = config.min_interval;
                document.getElementById('fan-curve-interval').value = config.min_interval;
            }
            if (typeof config.min_duty === 'number') {
                fanCurveConfig.minDuty = config.min_duty;
                document.getElementById('fan-curve-min-duty').value = config.min_duty;
            }
            if (typeof config.max_duty === 'number') {
                fanCurveConfig.maxDuty = config.max_duty;
                document.getElementById('fan-curve-max-duty').value = config.max_duty;
            }

            // 刷新曲线编辑器和预览
            refreshCurveEditor();
            drawCurvePreview();

            showToast(typeof t === 'function' ? t('fanPage.configImported', { name: file.name }) : `已导入配置文件: ${file.name}`, 'success');
        } catch (err) {
            console.error('导入配置失败:', err);
            showToast((typeof t === 'function' ? t('fanPage.importConfigFailed') : '导入配置失败') + ': ' + err.message, 'error');
        }
    };

    input.click();
}

/**
 * 更新曲线预览（风扇选择变化时）
 */
async function updateFanCurvePreview() {
    const newFanId = parseInt(document.getElementById('fan-curve-fan-select').value);

    // 如果切换了风扇，重新加载该风扇的配置
    if (newFanId !== fanCurveConfig.fanId) {
        fanCurveConfig.fanId = newFanId;

        try {
            const result = await api.call('fan.config', { id: newFanId });
            if (result.code === 0 && result.data) {
                const cfg = result.data;
                if (cfg.curve && cfg.curve.length >= 2) {
                    fanCurveConfig.curve = cfg.curve;
                }
                if (typeof cfg.hysteresis === 'number') {
                    fanCurveConfig.hysteresis = cfg.hysteresis;
                    document.getElementById('fan-curve-hysteresis').value = cfg.hysteresis;
                }
                if (typeof cfg.min_interval === 'number') {
                    fanCurveConfig.minInterval = cfg.min_interval;
                    document.getElementById('fan-curve-interval').value = cfg.min_interval;
                }
                if (typeof cfg.min_duty === 'number') {
                    fanCurveConfig.minDuty = cfg.min_duty;
                    document.getElementById('fan-curve-min-duty').value = cfg.min_duty;
                }
                if (typeof cfg.max_duty === 'number') {
                    fanCurveConfig.maxDuty = cfg.max_duty;
                    document.getElementById('fan-curve-max-duty').value = cfg.max_duty;
                }
                // 刷新曲线点编辑器
                refreshCurveEditor();
            }
        } catch (e) {
            console.warn('加载风扇配置失败:', e);
        }
    }

    drawCurvePreview();
}

async function serviceAction(name, action) {
    try {
        if (action === 'restart') requireApiSuccess(await api.serviceRestart(name), 'service.restart');
        else if (action === 'start') requireApiSuccess(await api.serviceStart(name), 'service.start');
        else if (action === 'stop') requireApiSuccess(await api.serviceStop(name), 'service.stop');
        showToast(typeof t === 'function' ? t({start:'promptRepair.serviceStarted', stop:'promptRepair.serviceStopped', restart:'promptRepair.serviceRestarted'}[action], {name}) : `服务 ${name} ${action} 成功`, 'success');
        await refreshSystemPage();
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.operationFailedMsg', { msg: e.message }) : `操作失败: ${e.message}`, 'error');
    }
}

async function confirmReboot() {
    if (await confirmAction(t('system.rebootConfirm'), { primary: t('system.reboot'), tone: 'danger' })) {
        showToast(t('system.rebootSending'), 'info');
        api.reboot(500)
            .then((result) => {
                requireApiSuccess(result, 'system.reboot');
                showToast(t('system.rebootingPleaseWait'), 'success');
            })
            .catch((err) => {
                console.error('Reboot failed:', err);
                showToast(t('system.rebootFailed') + ': ' + err.message, 'error');
            });
    }
}


// LED 控制（系统页面内嵌版）
async function refreshSystemLeds() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('system-led-devices-grid');
    if (!container) return;

    try {
        const result = await api.ledList();
        if (!pageCurrent()) return;

        if (result.data && result.data.devices && result.data.devices.length > 0) {
            // 存储设备信息
            result.data.devices.forEach(dev => {
                ledDevices[dev.name] = dev;
                if (dev.current && dev.current.animation) {
                    selectedEffects[dev.name] = dev.current.animation;
                }
                // 初始化 LED 状态
                if (dev.current) {
                    ledStates[dev.name] = dev.current.on || false;
                }
            });

            window.ledDevicesCache = result.data.devices;

            // 渲染设备卡片
            container.innerHTML = result.data.devices.map(dev => generateLedDeviceCard(dev)).join('');

            // 加载字体列表 & 显示色彩校正按钮
            if (result.data.devices.some(d => d.name === 'matrix' || d.layout === 'matrix')) {
                loadFontList();
                const ccBtn = document.getElementById('system-led-cc-btn');
                if (ccBtn) ccBtn.style.display = '';
            }
        } else {
            container.innerHTML = `
                <div class="empty">
                    <p class="t-body">${t('ledPage.ledNotFound')}</p>
                    <p class="t-note">${t('ledPage.ledNotStarted')}</p>
                </div>
            `;
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('LED list error:', e);
        container.innerHTML = `<div class="empty"><p class="t-note">${escapeHtml(typeof t === 'function' ? t('common.loadFailedMsg', { msg: e.message }) : '加载失败: ' + e.message)}</p></div>`;
    }
}

// ==================== 数据监控面板 - 动态可视化组件系统 ====================

/**
 * 可用的组件类型定义
 */
const WIDGET_TYPES = {
    ring: {
        get name() { return t('dataWidget.typeRing'); },
        icon: '<i class="ri-progress-6-line"></i>',
        get description() { return t('dataWidget.typeRingDesc'); },
        defaultConfig: { min: 0, max: 100, unit: '%', color: '#4dabf7', decimals: 0 }
    },
    gauge: {
        get name() { return t('dataWidget.typeGauge'); },
        icon: '<i class="ri-focus-line"></i>',
        get description() { return t('dataWidget.typeGaugeDesc'); },
        defaultConfig: { min: 0, max: 100, unit: '', color: '#69db7c', decimals: 1 }
    },
    temp: {
        get name() { return t('dataWidget.typeTemp'); },
        icon: '<i class="ri-temp-hot-line"></i>',
        get description() { return t('dataWidget.typeTempDesc'); },
        defaultConfig: { min: 0, max: 100, unit: '°C', color: '#ff8787', decimals: 0 }
    },
    number: {
        get name() { return t('dataWidget.typeNumber'); },
        icon: '<i class="ri-numbers-line"></i>',
        get description() { return t('dataWidget.typeNumberDesc'); },
        defaultConfig: { unit: 'W', color: '#74c0fc', icon: '<i class="ri-thunderstorms-line"></i>', decimals: 1 }
    },
    bar: {
        get name() { return t('dataWidget.typeBar'); },
        icon: '<i class="ri-bar-chart-line"></i>',
        get description() { return t('dataWidget.typeBarDesc'); },
        defaultConfig: { min: 0, max: 100, unit: '%', color: '#ffd43b', decimals: 1 }
    },
    text: {
        get name() { return t('led.text'); },
        icon: '<i class="ri-file-text-line"></i>',
        get description() { return t('dataWidget.typeTextDesc'); },
        defaultConfig: { unit: '', color: '#9ca3af' }
    },
    status: {
        get name() { return t('dataWidget.typeStatus'); },
        icon: '<i class="ri-record-circle-fill"></i>',
        get description() { return t('dataWidget.typeStatusDesc'); },
        defaultConfig: { thresholds: [0, 50, 80], colors: ['#40c057', '#fab005', '#fa5252'] }
    },
    icon: {
        get name() { return t('dataWidget.typeIcon'); },
        icon: '<i class="ri-emotion-line"></i>',
        get description() { return t('dataWidget.typeIconDesc'); },
        defaultConfig: { icons: { '0': '<i class="ri-close-line"></i>', '1': '<i class="ri-check-line"></i>', 'default': '<i class="ri-question-line"></i>' } }
    },
    dual: {
        get name() { return t('dataWidget.typeDual'); },
        icon: '<i class="ri-line-chart-line"></i>',
        get description() { return t('dataWidget.typeDualDesc'); },
        defaultConfig: { unit: '', color: '#74c0fc', decimals: 1 }
    },
    percent: {
        get name() { return t('dataWidget.typePercent'); },
        icon: '<i class="ri-percent-line"></i>',
        get description() { return t('dataWidget.typePercentDesc'); },
        defaultConfig: { min: 0, max: 100, color: '#4dabf7', decimals: 0 }
    },
    log: {
        get name() { return t('dataWidget.typeLog'); },
        icon: '<i class="ri-file-list-line"></i>',
        get description() { return t('dataWidget.typeLogDesc'); },
        defaultConfig: { maxLines: 15, color: '#495057', fullWidth: true }
    }
};

/**
 * 布局选项定义
 */
const LAYOUT_OPTIONS = {
    width: [
        { value: 'auto', get label() { return t('runtimeRepair.automatic'); }, get desc() { return t('dataWidget.layoutAutoDesc'); } },
        { value: 'small', get label() { return t('dataWidget.layoutSmall'); }, get desc() { return t('dataWidget.layoutSmallDesc'); } },
        { value: 'medium', get label() { return t('dataWidget.layoutMedium'); }, get desc() { return t('dataWidget.layoutMediumDesc'); } },
        { value: 'large', get label() { return t('dataWidget.layoutLarge'); }, get desc() { return t('dataWidget.layoutLargeDesc'); } },
        { value: 'full', get label() { return t('dataWidget.layoutFull'); }, get desc() { return t('dataWidget.layoutFullDesc'); } }
    ]
};

/**
 * 预设组件模板
 */
const WIDGET_PRESETS = [
    { id: 'cpu', label: 'CPU', type: 'ring', icon: '<i class="ri-cpu-line"></i>', color: '#4dabf7', unit: '%' },
    { id: 'mem', get label() { return t('system.memory'); }, type: 'ring', icon: '<i class="ri-brain-line"></i>', color: '#69db7c', unit: '%' },
    { id: 'disk', get label() { return t('dataWidget.presetDisk'); }, type: 'ring', icon: '<i class="ri-hard-drive-line"></i>', color: '#ffd43b', unit: '%' },
    { id: 'temp', get label() { return t('fan.temperature'); }, type: 'temp', icon: '<i class="ri-temp-hot-line"></i>', color: '#ff8787', unit: '°C' },
    { id: 'gpu', label: 'GPU', type: 'ring', icon: '<i class="ri-gamepad-line"></i>', color: '#da77f2', unit: '%' },
    { id: 'power', get label() { return t('dataWidget.presetPower'); }, type: 'number', icon: '<i class="ri-thunderstorms-line"></i>', color: '#74c0fc', unit: 'W' },
    { id: 'voltage', get label() { return t('system.voltage'); }, type: 'number', icon: '<i class="ri-plug-line"></i>', color: '#ffa94d', unit: 'V' },
    { id: 'current', get label() { return t('system.current'); }, type: 'number', icon: '<i class="ri-lightbulb-line"></i>', color: '#ff6b6b', unit: 'A' },
    { id: 'network', get label() { return t('dataWidget.presetNetwork'); }, type: 'bar', icon: '<i class="ri-global-line"></i>', color: '#38d9a9', unit: 'Mbps' },
    { id: 'status', get label() { return t('common.status'); }, type: 'status', icon: '<i class="ri-record-circle-fill"></i>', color: '#40c057', unit: '' },
    { id: 'uptime', get label() { return t('system.uptime'); }, type: 'text', icon: '<i class="ri-timer-line"></i>', color: '#9ca3af', unit: '' },
    { id: 'log', get label() { return t('dataWidget.typeLog'); }, type: 'log', icon: '<i class="ri-file-list-line"></i>', color: '#495057', maxLines: 15, layout: 'full' },
];

// 当前配置的组件列表
let dataWidgets = [];

// 数据刷新间隔配置（毫秒）
let dataWidgetsRefreshInterval = 5000;
let dataWidgetsIntervalId = null;

// 标记是否正在保存（防止重复保存）
let dataWidgetsSaving = false;

// 标记是否正在刷新（防止重叠刷新）
let dataWidgetsRefreshing = false;

/**
 * 加载数据组件配置
 * 优先级：后端 API (SD卡/NVS) > localStorage (兼容旧版)
 */
async function loadDataWidgets() {
    const pageCurrent = capturePageValidity();
    try {
        // 1. 尝试从后端加载
        const response = await api.call('ui.widgets.get');
        if (!pageCurrent()) return;
        // API 响应格式: {code: 0, data: {widgets: [...], refresh_interval: 5000, source: "sdcard"}}
        if (response && response.code === 0 && response.data && response.data.widgets) {
            const result = response.data;
            dataWidgets = result.widgets;
            dataWidgetsRefreshInterval = result.refresh_interval || 5000;
            console.log(`已从后端加载数据组件配置 (来源: ${result.source}, ${dataWidgets.length} 个组件)`);
            
            // 如果后端是默认空配置，检查 localStorage 是否有旧数据需要迁移
            if (result.source === 'default' && dataWidgets.length === 0) {
                const localData = loadDataWidgetsFromLocalStorage();
                if (localData && localData.length > 0) {
                    dataWidgets = localData;
                    console.log(`从 localStorage 迁移 ${localData.length} 个组件到后端`);
                    // 保存到后端
                    await saveDataWidgets();
                    if (!pageCurrent()) return;
                }
            }
            // 修复已损坏的图标数据
            _repairCorruptedWidgetIcons();
            return;
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.warn('从后端加载数据组件配置失败:', e);
    }
    
    // 2. 后端失败，回退到 localStorage
    const localData = loadDataWidgetsFromLocalStorage();
    if (localData) {
        dataWidgets = localData;
        console.log('从 localStorage 加载数据组件配置');
    } else {
        dataWidgets = [];
    }
    
    // 3. 修复已损坏的图标数据（旧版编辑面板未转义 HTML 属性导致 icon 被截断）
    _repairCorruptedWidgetIcons();
}

/**
 * 修复损坏的组件图标数据
 * 旧版编辑面板未对 icon 值进行 HTML 转义，导致 <i class="..."></i> 中的引号
 * 截断了 input value 属性，保存后 icon 变成 '<i class=' 等残缺值
 */
function _repairCorruptedWidgetIcons() {
    let repaired = false;
    dataWidgets.forEach(w => {
        if (w.icon && typeof w.icon === 'string') {
            const trimmed = w.icon.trim();
            // 检测损坏：以 <i 开始但不以 </i> 结束
            if (trimmed.startsWith('<i') && !trimmed.endsWith('</i>')) {
                console.warn(`修复损坏的图标 (组件 "${w.label}"):`, JSON.stringify(w.icon), '→ 恢复为类型默认图标');
                w.icon = WIDGET_TYPES[w.type]?.icon || '';
                repaired = true;
            }
        }
    });
    if (repaired) {
        // 异步保存修复后的数据
        saveDataWidgets();
    }
}

/**
 * 从 localStorage 加载（兼容旧版）
 */
function loadDataWidgetsFromLocalStorage() {
    try {
        let saved = localStorage.getItem('data_widgets_v2');
        
        // 兼容旧版数据：从 data_widgets 迁移
        if (!saved) {
            const oldSaved = localStorage.getItem('data_widgets');
            if (oldSaved) {
                const oldWidgets = JSON.parse(oldSaved);
                // 迁移旧数据：variable -> expression
                return oldWidgets.map(w => ({
                    ...w,
                    expression: w.variable ? `\${${w.variable}}` : null,
                    decimals: w.decimals ?? 1
                }));
            }
        }
        
        if (saved) {
            return JSON.parse(saved);
        }
    } catch (e) {
        console.warn('从 localStorage 加载失败:', e);
    }
    return null;
}

/**
 * 保存数据组件配置
 * 双写：后端 API (SD卡/NVS) + localStorage (备份)
 */
async function saveDataWidgets() {
    // 防止重复保存
    if (dataWidgetsSaving) return;
    dataWidgetsSaving = true;
    
    try {
        // 1. 保存到 localStorage（本地备份）
        localStorage.setItem('data_widgets_v2', JSON.stringify(dataWidgets));
        localStorage.setItem('data_widgets_refresh_interval', dataWidgetsRefreshInterval.toString());
        
        // 2. 保存到后端（SD卡 + NVS）
        const response = await api.call('ui.widgets.set', {
            widgets: dataWidgets,
            refresh_interval: dataWidgetsRefreshInterval
        }, 'POST');
        
        // API 响应格式: {code: 0, data: {sdcard_saved: true, nvs_saved: true}}
        if (response && response.code === 0 && response.data) {
            console.log(`数据组件配置已保存 (sdcard=${response.data.sdcard_saved}, nvs=${response.data.nvs_saved})`);
        }
    } catch (e) {
        console.warn('保存数据组件配置到后端失败:', e);
        // localStorage 已保存，不影响使用
    } finally {
        dataWidgetsSaving = false;
    }
}

/**
 * 加载刷新间隔配置（已整合到 loadDataWidgets）
 */
function loadDataWidgetsRefreshInterval() {
    // 刷新间隔已在 loadDataWidgets 中一起加载
    // 此函数保留用于兼容性
    try {
        const saved = localStorage.getItem('data_widgets_refresh_interval');
        if (saved) {
            dataWidgetsRefreshInterval = parseInt(saved) || 5000;
        }
    } catch (e) {
        dataWidgetsRefreshInterval = 5000;
    }
}

/**
 * 保存刷新间隔配置（已整合到 saveDataWidgets）
 */
function saveDataWidgetsRefreshInterval() {
    // 触发完整保存
    saveDataWidgets();
}

/**
 * 启动自动刷新
 */
function startDataWidgetsAutoRefresh() {
    stopDataWidgetsAutoRefresh();
    if (dataWidgetsRefreshInterval > 0) {
        dataWidgetsIntervalId = setInterval(() => {
            refreshDataWidgets();
        }, dataWidgetsRefreshInterval);
    }
}

/**
 * 停止自动刷新
 */
function stopDataWidgetsAutoRefresh() {
    dataWidgetsRefreshing = false;
    if (dataWidgetsIntervalId) {
        clearInterval(dataWidgetsIntervalId);
        dataWidgetsIntervalId = null;
    }
}

/**
 * 生成唯一 ID
 */
function generateWidgetId() {
    return 'w_' + Date.now().toString(36) + Math.random().toString(36).substr(2, 5);
}

/**
 * 计算表达式值
 * 支持: ${var}, 数学运算, 文本拼接
 * @param {string} expression - 表达式，如 "${var} * 100" 或 "${var1} + ${var2}"
 * @param {object} variables - 变量名到值的映射
 * @returns {any} - 计算后的值
 */
function evaluateExpression(expression, variables) {
    if (!expression) return null;
    
    // 替换所有 ${varName} 为实际值
    let evalStr = expression.replace(/\$\{([^}]+)\}/g, (match, varName) => {
        const val = variables[varName.trim()];
        if (val === undefined || val === null) return 'null';
        if (typeof val === 'string') return `"${val}"`;
        return val;
    });
    
    // 如果只是单个变量引用，直接返回
    if (expression.match(/^\$\{[^}]+\}$/)) {
        const varName = expression.slice(2, -1).trim();
        return variables[varName];
    }
    
    // 安全计算表达式
    try {
        // 只允许基本数学运算和字符串操作
        if (evalStr.includes('null')) return null;
        // eslint-disable-next-line no-new-func
        const result = new Function('return ' + evalStr)();
        return result;
    } catch (e) {
        console.warn('表达式计算失败:', expression, e);
        return null;
    }
}

/**
 * 格式化显示值
 * @param {any} value - 原始值
 * @param {object} config - 格式化配置 { decimals, prefix, suffix, format }
 */
function formatDisplayValue(value, config) {
    if (value === null || value === undefined) return '-';
    
    const { decimals = 1, prefix = '', suffix = '', format } = config || {};
    
    // 自定义格式
    if (format) {
        return format.replace('{value}', value).replace('{prefix}', prefix).replace('{suffix}', suffix);
    }
    
    // 数字格式化
    if (typeof value === 'number') {
        return prefix + value.toFixed(decimals) + suffix;
    }
    
    return prefix + String(value) + suffix;
}

/**
 * 校验并安全化组件图标 HTML
 * 防止损坏的 <i> 标签吞噬后续文本（如标签名称）
 */
function sanitizeWidgetIcon(iconHtml) {
    if (!iconHtml || typeof iconHtml !== 'string') return '';
    const trimmed = iconHtml.trim();
    // 有效图标：以 <i 开始，以 </i> 结束（正确闭合）
    if (trimmed.startsWith('<i ') && trimmed.endsWith('</i>')) return iconize(trimmed) + ' ';
    if (trimmed === '<i></i>') return '';
    // 如果只是 remixicon 类名（旧格式），包装成完整标签
    if (/^ri-[\w-]+$/.test(trimmed)) return ic(trimmed) + ' ';
    // 无效或损坏的图标 HTML — 丢弃，防止破坏 DOM
    return '';
}

/**
 * 渲染单个组件的 HTML
 */
function renderWidgetHtml(widget) {
    const { id, type, label, icon, unit } = widget;
    const vid = `dw-${id}-value`;
    const big = (inner) => `<div class="bigrow w32">${inner}</div>`;
    let contentHtml = '';
    
    switch (type) {
        case 'ring':
            contentHtml = `<div class="dw-ring"><svg width="64" height="64" viewBox="0 0 64 64"><circle cx="32" cy="32" r="26" fill="none" stroke="rgba(0,0,0,.07)" stroke-width="6"/><circle id="dw-${id}-ring" cx="32" cy="32" r="26" fill="none" stroke="#007aff" stroke-width="6" stroke-linecap="round" stroke-dasharray="0 163.4" transform="rotate(-90 32 32)"/></svg><span class="dw-ring-value" id="${vid}">-</span></div>`;
            break;
        case 'gauge':
            contentHtml = `<div class="dw-ring dw-gauge"><svg width="72" height="44" viewBox="0 0 72 44"><path d="M8 38A28 28 0 0 1 64 38" fill="none" stroke="rgba(0,0,0,.07)" stroke-width="6" stroke-linecap="round"/><path id="dw-${id}-gauge" d="M8 38A28 28 0 0 1 64 38" fill="none" stroke="#007aff" stroke-width="6" stroke-linecap="round" stroke-dasharray="0 87.96"/></svg><span class="dw-ring-value" id="${vid}">-</span></div>`;
            break;
        case 'temp':
            contentHtml = big(`<span class="t-big" id="${vid}">-</span><span class="t-unit">°C</span>`);
            break;
        case 'number':
            contentHtml = big(`<span class="t-big" id="${vid}">-</span><span class="t-unit">${unit || ''}</span>`);
            break;
        case 'percent':
            contentHtml = big(`<span class="t-big" id="${vid}">-</span><span class="t-unit">%</span>`);
            break;
        case 'bar':
            contentHtml = `<div class="kv" style="padding:10px 0 6px"><span></span><span id="${vid}">-</span></div><div class="bar"><i id="dw-${id}-fill" style="width:0"></i></div>`;
            break;
        case 'status':
            contentHtml = `<div class="dw-st"><span class="state" id="${vid}">-</span></div>`;
            break;
        case 'icon':
            contentHtml = `<div class="dw-ic"><span class="dw-icon-display" id="dw-${id}-icon"><svg class="i"><use href="#ri-question-line"/></svg></span><span class="t-body" id="${vid}">-</span></div>`;
            break;
        case 'dual':
            contentHtml = big(`<span class="t-big" id="${vid}">-</span><span class="t-unit">/</span><span class="t-value" id="dw-${id}-sub">-</span><span class="t-unit">${unit || ''}</span>`);
            break;
        case 'log': {
            const maxLines = widget.maxLines || 15;
            const isReading = widget._isReading || false;
            // 默认折叠（除非明确设置了 _isCollapsed: false）
            const isCollapsed = widget._isCollapsed !== false;
            contentHtml = `
                <div class="dw-log-h">
                    <span class="t-label">${sanitizeWidgetIcon(icon)}${escapeHtml(label)}</span>
                    <span class="dw-log-tools">
                        <span class="state${isReading ? ' ok' : ''}" id="dw-${id}-status">${isReading ? t('common.reading') : t('status.stopped')}</span>
                        <button class="btn sm" id="dw-${id}-toggle" onclick="event.stopPropagation();toggleLogReading('${id}')">${isReading ? t('fanPage.stopReading') : t('fanPage.reading')}</button>
                        <button class="btn sm" onclick="event.stopPropagation();refreshLogOnce('${id}')">${t('common.refreshOnce')}</button>
                        <button class="btn sm" onclick="event.stopPropagation();clearLogWidget('${id}')">${t('common.clear')}</button>
                        <button class="btn sm" id="dw-${id}-collapse" onclick="event.stopPropagation();toggleLogCollapse('${id}')">${isCollapsed ? t('dataWidget.expandLog') : t('dataWidget.collapseLog')}</button>
                    </span>
                </div>
                <div class="dw-log-container ${isCollapsed ? 'dw-log-collapsed' : ''}" id="dw-${id}-log" data-max-lines="${maxLines}">
                    <div class="dw-log-empty">${t('dataWidget.clickToRead')}</div>
                </div>`;
            break;
        }
        case 'text':
        default:
            contentHtml = `<div class="t-body dw-tx" id="${vid}">-</div>`;
            break;
    }
    
    const layout = widget.layout || 'auto';
    const layoutClass = layout !== 'auto' ? `dw-layout-${layout}` : '';
    const head = type === 'log' ? '' : `<div class="t-label">${sanitizeWidgetIcon(icon)}${escapeHtml(label)}</div>`;
    
    return `
        <div class="w-tile dw-card ${layoutClass}" data-widget-id="${id}" data-layout="${layout}" data-type="${type}" onclick="event.target.closest('button') || showWidgetManager('${id}')">
            ${head}${contentHtml}
        </div>
    `;
}

/**
 * 渲染所有组件
 */
function renderDataWidgets() {
    const grid = document.getElementById('data-widgets-grid');
    const empty = document.getElementById('data-widgets-empty');
    if (!grid) return;
    
    if (dataWidgets.length === 0) {
        grid.innerHTML = '';
        if (empty) empty.style.display = 'block';
    } else {
        if (empty) empty.style.display = 'none';
        grid.innerHTML = dataWidgets.map(w => renderWidgetHtml(w)).join('');
    }
}

/**
 * 更新单个组件的值
 */
function updateWidgetValue(widget, value) {
    const { id, type, unit, min = 0, max = 100, decimals = 1, thresholds, icons } = widget;
    const el = (suffix) => document.getElementById(`dw-${id}-${suffix}`);
    const valueEl = el('value');
    const setRing = (arc, percent, len) => { if (arc) arc.setAttribute('stroke-dasharray', `${(percent * len / 100).toFixed(1)} ${len}`); };
    
    // 处理空值
    if (value === null || value === undefined) {
        if (valueEl) valueEl.textContent = '-';
        if (type === 'ring') setRing(el('ring'), 0, 163.4);
        else if (type === 'gauge') setRing(el('gauge'), 0, 87.96);
        else if (type === 'bar' && el('fill')) el('fill').style.width = '0%';
        else if (type === 'status' && valueEl) valueEl.className = 'state';
        return;
    }
    
    const numVal = typeof value === 'number' ? value : parseFloat(value);
    const percent = isNaN(numVal) ? 0 : Math.min(100, Math.max(0, ((numVal - min) / (max - min)) * 100));
    const num = (d) => isNaN(numVal) ? value : numVal.toFixed(d);
    
    switch (type) {
        case 'ring':
            setRing(el('ring'), percent, 163.4);
            if (valueEl) valueEl.textContent = num(decimals) + (unit || '%');
            break;
        case 'gauge':
            setRing(el('gauge'), percent, 87.96);
            if (valueEl) valueEl.textContent = num(decimals) + (unit || '');
            break;
        case 'temp':
            if (valueEl) valueEl.textContent = num(0);
            break;
        case 'bar':
            if (el('fill')) el('fill').style.width = percent + '%';
            if (valueEl) valueEl.textContent = num(decimals) + (unit || '%');
            break;
        case 'number':
        case 'percent':
            if (valueEl) valueEl.textContent = num(decimals);
            break;
        case 'status': {
            const th = thresholds || [0, 50, 80];
            let cls = 'ok', statusText = t('dataWidget.statusNormal');
            if (!isNaN(numVal)) {
                if (numVal >= th[2]) { cls = 'bad'; statusText = t('dataWidget.statusWarning'); }
                else if (numVal >= th[1]) { cls = 'warn'; statusText = t('dataWidget.statusAttention'); }
            }
            if (valueEl) { valueEl.className = 'state ' + cls; valueEl.textContent = statusText; }
            break;
        }
        case 'icon': {
            const iconMap = icons || { '0': '<i class="ri-close-line"></i>', '1': '<i class="ri-check-line"></i>', 'default': '<i class="ri-question-line"></i>' };
            if (el('icon')) el('icon').innerHTML = iconize(iconMap[String(value)] || iconMap['default'] || '<i class="ri-question-line"></i>');
            if (valueEl) valueEl.textContent = value;
            break;
        }
        case 'dual': {
            if (valueEl) valueEl.textContent = num(decimals);
            const subEl = el('sub');
            // 副值需要从 expression2 获取
            if (subEl && widget.subValue !== undefined) {
                subEl.textContent = typeof widget.subValue === 'number' ? widget.subValue.toFixed(decimals) : widget.subValue;
            }
            break;
        }
        case 'log':
            // 日志组件特殊处理，在 refreshDataWidgets 中单独刷新
            break;
        default:
            if (valueEl) valueEl.textContent = String(value);
    }
}

/**
 * 初始化数据组件面板
 */
async function initDataWidgets() {
    const pageCurrent = capturePageValidity();
    await loadDataWidgets();
    if (!pageCurrent()) return;  // 异步加载（优先后端 API）
    loadDataWidgetsRefreshInterval();  // 兼容性保留
    renderDataWidgets();
    await refreshDataWidgets();
    if (!pageCurrent()) return;
    startDataWidgetsAutoRefresh();
}

/**
 * 刷新所有组件的数据
 * 使用 automation.variables.list 一次获取全部变量，避免 N 次 get 串行调用
 */
async function refreshDataWidgets() {
    const pageCurrent = capturePageValidity();
    if (dataWidgetsRefreshing) return;
    dataWidgetsRefreshing = true;

    try {
        // 先收集所有需要的变量名
        const varNames = new Set();
        dataWidgets.forEach(w => {
            if (w.type !== 'log' && w.expression) {
                const matches = w.expression.match(/\$\{([^}]+)\}/g);
                if (matches) {
                    matches.forEach(m => varNames.add(m.slice(2, -1).trim()));
                }
            }
            if (w.expression2) {
                const matches = w.expression2.match(/\$\{([^}]+)\}/g);
                if (matches) {
                    matches.forEach(m => varNames.add(m.slice(2, -1).trim()));
                }
            }
        });

        // 一次 API 调用获取全部变量（替代 N 次 variables.get）
        const variables = {};
        if (varNames.size > 0) {
            try {
                const resp = await api.call('automation.variables.list', { include_meta: false });
                if (!pageCurrent()) return;
                if (resp.code === 0 && resp.data?.variables) {
                    for (const v of resp.data.variables) {
                        if (varNames.has(v.name) && v.value !== undefined) {
                            variables[v.name] = v.value;
                        }
                    }
                }
            } catch (e) {
                if (!pageCurrent()) return;
                console.warn('获取变量列表失败:', e);
            }
        }

        // 更新每个组件（日志组件不自动刷新）
        for (const widget of dataWidgets) {
            if (widget.type === 'log') {
                continue;
            } else if (widget.expression) {
                const value = evaluateExpression(widget.expression, variables);
                updateWidgetValue(widget, value);
                if (widget.expression2) {
                    widget.subValue = evaluateExpression(widget.expression2, variables);
                }
            } else {
                updateWidgetValue(widget, null);
            }
        }
    } finally {
        if (pageCurrent()) dataWidgetsRefreshing = false;
    }
}

/**
 * 日志组件读取状态和定时器
 */
const logWidgetTimers = {};

/**
 * 切换日志组件折叠状态
 */
function toggleLogCollapse(widgetId) {
    const widget = dataWidgets.find(w => w.id === widgetId);
    if (!widget) return;
    
    widget._isCollapsed = !widget._isCollapsed;
    
    const container = document.getElementById(`dw-${widgetId}-log`);
    const btn = document.getElementById(`dw-${widgetId}-collapse`);
    if (container) container.classList.toggle('dw-log-collapsed', !!widget._isCollapsed);
    if (btn) btn.textContent = t(widget._isCollapsed ? 'dataWidget.expandLog' : 'dataWidget.collapseLog');
    
    // 保存状态
    saveDataWidgets();
}

/**
 * 切换日志读取状态
 */
function toggleLogReading(widgetId) {
    const widget = dataWidgets.find(w => w.id === widgetId);
    if (!widget) return;
    
    if (widget._isReading) {
        // 停止读取
        stopLogReading(widgetId);
    } else {
        // 开始读取
        startLogReading(widgetId);
    }
}

/**
 * 开始读取日志
 */
function startLogReading(widgetId) {
    const widget = dataWidgets.find(w => w.id === widgetId);
    if (!widget || !widget.expression) {
        showToast((typeof t === 'function' ? t('dataWidget.configLogVariableFirst') : '请先配置日志变量'), 'warning');
        return;
    }
    
    widget._isReading = true;
    
    // 开始读取时自动展开
    if (widget._isCollapsed !== false) {
        widget._isCollapsed = false;
        const container = document.getElementById(`dw-${widgetId}-log`);
        const btn = document.getElementById(`dw-${widgetId}-collapse`);
        if (container) container.classList.remove('dw-log-collapsed');
        if (btn) btn.textContent = t('dataWidget.collapseLog');
    }
    
    updateLogToggleButton(widgetId, true);
    
    // 立即读取一次
    refreshLogOnce(widgetId);
    
    // 设置定时器
    const interval = widget.refreshInterval || 2000;
    logWidgetTimers[widgetId] = setInterval(() => {
        refreshLogOnce(widgetId);
    }, interval);
}

/**
 * 停止读取日志
 */
function stopLogReading(widgetId) {
    const widget = dataWidgets.find(w => w.id === widgetId);
    if (widget) {
        widget._isReading = false;
    }
    
    if (logWidgetTimers[widgetId]) {
        clearInterval(logWidgetTimers[widgetId]);
        delete logWidgetTimers[widgetId];
    }
    
    updateLogToggleButton(widgetId, false);
}

/**
 * 更新日志切换按钮状态
 */
function updateLogToggleButton(widgetId, isReading) {
    const btn = document.getElementById(`dw-${widgetId}-toggle`);
    const status = document.getElementById(`dw-${widgetId}-status`);
    
    if (btn) btn.textContent = t(isReading ? 'fanPage.stopReading' : 'fanPage.reading');
    if (status) {
        status.className = isReading ? 'state ok' : 'state';
        status.textContent = t(isReading ? 'common.reading' : 'status.stopped');
    }
}

/**
 * 刷新日志组件一次（从变量读取）
 */
async function refreshLogOnce(widgetId) {
    const pageCurrent = capturePageValidity();
    const widget = dataWidgets.find(w => w.id === widgetId);
    if (!widget) return;
    
    const container = document.getElementById(`dw-${widget.id}-log`);
    if (!container) return;
    
    if (!widget.expression) {
        container.innerHTML = '<div class="dw-log-empty">' + (typeof t === 'function' ? t('fanPage.noLogVariable') : '未配置日志变量') + '</div>';
        return;
    }
    
    try {
        // 从表达式中提取变量名
        const varMatch = widget.expression.match(/\$\{([^}]+)\}/);
        if (!varMatch) {
            container.innerHTML = '<div class="dw-log-error">' + (typeof t === 'function' ? t('fanPage.invalidExpression') : '无效的变量表达式') + '</div>';
            return;
        }
        
        const varName = varMatch[1].trim();
        const result = await api.call('automation.variables.get', { name: varName });
        if (!pageCurrent()) return;
        
        if (result.code !== 0 || result.data?.value === undefined) {
            container.innerHTML = '<div class="dw-log-error">' + (typeof t === 'function' ? t('fanPage.variableNoData') : '变量不存在或无数据') + '</div>';
            return;
        }
        
        const logText = String(result.data.value);
        appendLogToWidget(widget.id, logText, widget.maxLines || 15);
        
    } catch (e) {
        if (!pageCurrent()) return;
        console.warn('获取日志变量失败:', e);
        container.innerHTML = '<div class="dw-log-error">' + (typeof t === 'function' ? t('fanPage.readFailed') : '读取失败') + '</div>';
    }
}

/**
 * 追加日志到组件（去重、限制行数）
 */
function appendLogToWidget(widgetId, newText, maxLines) {
    const container = document.getElementById(`dw-${widgetId}-log`);
    if (!container) return;
    
    // 获取现有内容
    let existingLines = [];
    const existingElements = container.querySelectorAll('.dw-log-line');
    existingElements.forEach(el => {
        existingLines.push(el.dataset.text || el.textContent);
    });
    
    // 处理新日志（可能是多行）
    const newLines = newText.split('\\n').filter(l => l.trim());
    
    // 追加新行（去重）
    newLines.forEach(line => {
        const trimmed = line.trim();
        if (trimmed && !existingLines.includes(trimmed)) {
            existingLines.push(trimmed);
        }
    });
    
    // 限制行数
    if (existingLines.length > maxLines) {
        existingLines = existingLines.slice(-maxLines);
    }
    
    // 渲染
    if (existingLines.length === 0) {
        container.innerHTML = '<div class="dw-log-empty">' + (typeof t === 'function' ? t('fanPage.noLogs') : '暂无日志') + '</div>';
    } else {
        container.innerHTML = existingLines.map(line => {
            const escaped = escapeHtml(line);
            // 尝试检测日志级别着色
            let colorClass = '';
            if (/\bERR(OR)?\b/i.test(line)) colorClass = 'dw-log-error-line';
            else if (/\bWARN(ING)?\b/i.test(line)) colorClass = 'dw-log-warn-line';
            else if (/\bINFO\b/i.test(line)) colorClass = 'dw-log-info-line';
            else if (/\bDEBUG\b/i.test(line)) colorClass = 'dw-log-debug-line';
            
            return `<div class="dw-log-line ${colorClass}" data-text="${escaped}">${escaped}</div>`;
        }).join('');
        
        // 滚动到底部
        container.scrollTop = container.scrollHeight;
    }
}

/**
 * 清空日志组件
 */
function clearLogWidget(widgetId) {
    const container = document.getElementById(`dw-${widgetId}-log`);
    if (container) {
        container.innerHTML = '<div class="dw-log-empty">' + (typeof t === 'function' ? t('fanPage.cleared') : '已清空') + '</div>';
    }
}

/**
 * 显示组件管理器
 */
const WIDGET_TYPE_KEYS = { ring: 'Ring', gauge: 'Gauge', temp: 'Temp', number: 'Number', bar: 'Bar', text: 'Text', status: 'Status', icon: 'Icon', dual: 'Dual', percent: 'Percent', log: 'Log' };
const widgetTypeName = type => t('dataWidget.type' + (WIDGET_TYPE_KEYS[type] || type));

function showWidgetManager(editWidgetId = null) {
    const modal = document.createElement('div');
    modal.className = 'modal';
    modal.id = 'widget-manager-modal';
    modal.onclick = (e) => { if (e.target === modal) closeModal('widget-manager-modal'); };
    
    const intervals = [[0, 'disabled'], [1000, 'seconds1'], [2000, 'seconds2'], [5000, 'seconds5'], [10000, 'seconds10'], [30000, 'seconds30'], [60000, 'minute1']];
    modal.innerHTML = sheet(600, t('system.widgetManager'), `
        ${gt(t('dataWidget.panelSettings'))}
        ${grp(row(t('dataWidget.autoRefreshInterval'), `<select class="field" id="dw-refresh-interval" onchange="updateRefreshInterval()" style="width:110px">${intervals.map(([v, k]) => `<option value="${v}" ${dataWidgetsRefreshInterval === v ? 'selected' : ''}>${t('dataWidget.' + k)}</option>`).join('')}</select>`))}
        ${gt(t('dataWidget.addedWidgets'))}
        <div class="grp" id="dw-manager-list"></div>
        <div style="margin-top:12px"><button class="btn sm" onclick="showAddWidgetPanel()"><svg class="i"><use href="#ri-add-line"/></svg>${t('dataWidget.addWidget')}</button></div>`,
        `<button class="btn lg primary" onclick="closeModal('widget-manager-modal')">${t('common.close')}</button>`, "closeModal('widget-manager-modal')");
    document.body.appendChild(modal);
    
    // 渲染组件列表
    renderWidgetManagerList();
    
    // 如果指定了编辑的组件，直接打开编辑面板
    if (editWidgetId) {
        showWidgetEditPanel(editWidgetId);
    }
}

/**
 * 更新刷新间隔
 */
function updateRefreshInterval() {
    const select = document.getElementById('dw-refresh-interval');
    if (select) {
        dataWidgetsRefreshInterval = parseInt(select.value) || 0;
        saveDataWidgetsRefreshInterval();
        startDataWidgetsAutoRefresh();
        const _sec = typeof t === 'function' ? t('dataWidget.second') : '秒';
        const intervalText = dataWidgetsRefreshInterval > 0 ? (dataWidgetsRefreshInterval / 1000) + ' ' + _sec : (typeof t === 'function' ? t('dataWidget.disabled') : '禁用');
        showToast(typeof t === 'function' ? t('toast.refreshIntervalSet', { interval: intervalText }) : `刷新间隔已设置为 ${intervalText}`, 'success');
    }
}

/**
 * 渲染管理器中的组件列表
 */
function renderWidgetManagerList() {
    const list = document.getElementById('dw-manager-list');
    if (!list) return;
    
    if (dataWidgets.length === 0) {
        list.innerHTML = row(`<span class="t-note">${t('dataWidget.noWidgets')}</span>`, '');
        return;
    }
    
    list.innerHTML = dataWidgets.map((w, idx) => row(`${escapeHtml(w.label)} · ${widgetTypeName(w.type)}`,
        icoBtn('ri-arrow-up-line', t('dataWidget.moveUp'), `moveWidget('${w.id}',-1)`, '', idx === 0) +
        icoBtn('ri-arrow-down-line', t('dataWidget.moveDown'), `moveWidget('${w.id}',1)`, '', idx === dataWidgets.length - 1) +
        icoBtn('ri-edit-line', t('common.edit'), `showWidgetEditPanel('${w.id}')`) +
        icoBtn('ri-delete-bin-line', t('dataWidget.delete'), `deleteDataWidget('${w.id}')`, 'dg'))).join('');
}

/**
 * 移动组件位置
 */
function moveWidget(widgetId, direction) {
    const idx = dataWidgets.findIndex(w => w.id === widgetId);
    if (idx === -1) return;
    
    const newIdx = idx + direction;
    if (newIdx < 0 || newIdx >= dataWidgets.length) return;
    
    [dataWidgets[idx], dataWidgets[newIdx]] = [dataWidgets[newIdx], dataWidgets[idx]];
    saveDataWidgets();
    renderWidgetManagerList();
    renderDataWidgets();
}

/**
 * 显示添加组件面板
 */
function showAddWidgetPanel() {
    document.getElementById('widget-add-modal')?.remove();
    const presetLabelKeys = { cpu: 'presetCpu', mem: 'presetMem', disk: 'presetDisk', temp: 'presetTemp', gpu: 'presetGpu', power: 'presetPower', voltage: 'presetVoltage', current: 'presetCurrent', network: 'presetNetwork', status: 'presetStatus', uptime: 'presetUptime', log: 'presetLog' };
    const presetsHtml = WIDGET_PRESETS.map(p => `<button class="btn" onclick="addWidgetFromPreset('${p.id}')">${iconize(p.icon)}${t('dataWidget.' + (presetLabelKeys[p.id] || p.id))}</button>`).join('');
    const typesHtml = Object.entries(WIDGET_TYPES).map(([key, cfg]) => {
        const nameKey = 'dataWidget.type' + (WIDGET_TYPE_KEYS[key] || key);
        return `<div class="row" style="cursor:pointer" onclick="createNewWidget('${key}')"><div class="rl"><span style="display:flex;gap:10px;align-items:center">${iconize(cfg.icon)}${t(nameKey)}</span></div><div class="rc"><span class="t-note">${t(nameKey + 'Desc')}</span></div></div>`;
    }).join('');
    
    const modal = document.createElement('div');
    modal.className = 'modal';
    modal.id = 'widget-add-modal';
    modal.onclick = (e) => { if (e.target === modal) closeModal('widget-add-modal'); };
    modal.innerHTML = sheet(600, t('dataWidget.addNewWidget'), `
        ${gt(t('dataWidget.quickAddPreset'))}
        <div style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px">${presetsHtml}</div>
        ${gt(t('dataWidget.customWidgetType'))}
        <div class="grp">${typesHtml}</div>`,
        `<button class="btn lg primary" onclick="closeModal('widget-add-modal')">${t('common.close')}</button>`, "closeModal('widget-add-modal')");
    document.body.appendChild(modal);
}

/**
 * 从预设添加组件
 */
function addWidgetFromPreset(presetId) {
    const preset = WIDGET_PRESETS.find(p => p.id === presetId);
    if (!preset) return;
    
    const widget = {
        id: generateWidgetId(),
        type: preset.type,
        label: preset.label,
        icon: preset.icon,
        color: preset.color,
        unit: preset.unit,
        min: 0,
        max: 100,
        decimals: WIDGET_TYPES[preset.type]?.defaultConfig?.decimals ?? 1,
        expression: null
    };
    
    dataWidgets.push(widget);
    saveDataWidgets();
    renderDataWidgets();
    renderWidgetManagerList();
    showWidgetEditPanel(widget.id);
    const _presetLabelKeys = { cpu: 'presetCpu', mem: 'presetMem', disk: 'presetDisk', temp: 'presetTemp', gpu: 'presetGpu', power: 'presetPower', voltage: 'presetVoltage', current: 'presetCurrent', network: 'presetNetwork', status: 'presetStatus', uptime: 'presetUptime', log: 'presetLog' };
    const addedName = typeof t === 'function' ? t('dataWidget.' + (_presetLabelKeys[preset.id] || preset.id)) : preset.label;
    showToast(typeof t === 'function' ? t('toast.widgetAdded', { name: addedName }) : `已添加 ${preset.label}`, 'success');
}

/**
 * 创建新的自定义组件
 */
function createNewWidget(type) {
    const typeConfig = WIDGET_TYPES[type];
    if (!typeConfig) return;
    
    const defaults = typeConfig.defaultConfig || {};
    
    const widget = {
        id: generateWidgetId(),
        type,
        label: typeof t === 'function' ? t('dataWidget.newWidget') : '新组件',
        icon: typeConfig.icon,
        color: defaults.color || '#4dabf7',
        unit: defaults.unit || '',
        min: defaults.min ?? 0,
        max: defaults.max ?? 100,
        decimals: defaults.decimals ?? 1,
        expression: null
    };
    
    if (type === 'status') {
        widget.thresholds = defaults.thresholds || [0, 50, 80];
        widget.colors = defaults.colors || ['#40c057', '#fab005', '#fa5252'];
    }
    if (type === 'icon') {
        widget.icons = defaults.icons || { '0': '<i class="ri-close-line"></i>', '1': '<i class="ri-check-line"></i>', 'default': '<i class="ri-question-line"></i>' };
    }
    if (type === 'log') {
        widget.maxLines = defaults.maxLines || 15;
        widget.refreshInterval = 2000;
        widget.layout = 'full';  // 日志组件默认独占一行
        widget.label = typeof t === 'function' ? t('dataWidget.presetLog') : '日志流';
    }
    
    dataWidgets.push(widget);
    saveDataWidgets();
    renderDataWidgets();
    renderWidgetManagerList();
    showWidgetEditPanel(widget.id);
}

/**
 * 显示组件编辑面板
 */
function showWidgetEditPanel(widgetId) {
    const widget = dataWidgets.find(w => w.id === widgetId);
    if (!widget) return;
    
    document.getElementById('widget-add-modal')?.remove();
    document.getElementById('widget-edit-modal')?.remove();
    const typeConfig = WIDGET_TYPES[widget.type] || {};
    const num = (id, w, val, attrs = '') => inp(id, w, '', 'num', `type="number" value="${val}" ${attrs}`);
    
    // 额外配置（根据组件类型）
    let extraRows = '';
    if (widget.type === 'status') {
        extraRows = row(t('dataWidget.thresholdSettings'), num('edit-threshold-1', 64, widget.thresholds?.[0] ?? 0) + num('edit-threshold-2', 64, widget.thresholds?.[1] ?? 50) + num('edit-threshold-3', 64, widget.thresholds?.[2] ?? 80));
    }
    if (widget.type === 'dual') {
        extraRows = row(t('dataWidget.secondaryExpression'), inp('edit-expression2', 220, t('dataWidget.secondaryExpressionPlaceholder'), 'mono', `value="${escapeHtml(widget.expression2 || '')}"`), '', t('dataWidget.secondaryExpressionHint'));
    }
    if (widget.type === 'log') {
        extraRows = row(t('dataWidget.displayLines'), num('edit-max-lines', 90, widget.maxLines || 15, 'min="5" max="100"')) +
                    row(t('dataWidget.refreshIntervalMs'), num('edit-refresh-interval', 90, widget.refreshInterval || 2000, 'min="500" max="60000" step="500"'));
    }
    const layouts = [['auto', 'Auto'], ['small', 'Small'], ['medium', 'Medium'], ['large', 'Large'], ['full', 'Full']];
    const layoutRows = layouts.map(([val, k]) => {
        const active = widget.layout === val || (!widget.layout && val === 'auto');
        return row(t('dataWidget.layout' + k), `<input type="radio" name="edit-layout" value="${val}" ${active ? 'checked' : ''}>`, t('dataWidget.layout' + k + 'Desc'));
    }).join('');
    const exprLabel = widget.type === 'log' ? t('dataWidget.logVariable') : t('dataWidget.dataExpression');
    const exprPh = widget.type === 'log' ? t('dataWidget.logVariablePlaceholder') : t('dataWidget.dataExpressionPlaceholder');
    const exprHint = widget.type === 'log' ? t('dataWidget.logVariableHint') : t('dataWidget.dataExpressionHint');
    
    const modal = document.createElement('div');
    modal.className = 'modal';
    modal.id = 'widget-edit-modal';
    modal.innerHTML = sheet(600, `${t('dataWidget.editWidget')} · ${escapeHtml(typeConfig.name || widget.type)}`, `
        ${grp(
            row(t('dataWidget.labelName'), inp('edit-label', 220, t('dataWidget.labelPlaceholder'), '', `value="${escapeHtml(widget.label)}"`)) +
            row(t('dataWidget.icon'), inp('edit-icon', 120, t('dataWidget.iconPlaceholder') || 'emoji', '', `value="${escapeHtml(widget.icon || '')}"`)) +
            row(t('dataWidget.color'), `<input type="color" class="field" id="edit-color" value="${widget.color || '#4dabf7'}" style="width:44px;padding:2px">`))}
        ${gt(t('dataWidget.layoutWidth'))}
        ${grp(layoutRows)}
        ${widget.type !== 'log' ? gt(t('dataWidget.unit')) + grp(
            row(t('dataWidget.unit'), inp('edit-unit', 90, '%、°C、W', '', `value="${escapeHtml(widget.unit || '')}"`)) +
            row(t('dataWidget.decimals'), num('edit-decimals', 70, widget.decimals ?? 1, 'min="0" max="4"')) +
            (widget.type !== 'text' && widget.type !== 'icon' && widget.type !== 'status' ?
                row(t('dataWidget.minValue'), num('edit-min', 90, widget.min ?? 0)) + row(t('dataWidget.maxValue'), num('edit-max', 90, widget.max ?? 100)) : '')) : ''}
        ${extraRows ? grp(extraRows, 'margin-top:8px') : ''}
        <div class="fl" style="margin-top:18px">
            <label>${exprLabel} <span class="tag">${t('common.core')}</span></label>
            <div style="display:flex;gap:8px"><input class="field mono" id="edit-expression" style="flex:1" value="${escapeHtml(widget.expression || '')}" placeholder="${exprPh}"><button class="btn" onclick="selectVariableForWidget()">${t('dataWidget.selectVariable')}</button></div>
        </div>
        <div class="t-note" style="margin:6px 4px 0">${exprHint}</div>
        ${widget.type !== 'log' ? gt(t('dataWidget.preview')) + `<div id="dw-preview-card" class="cvbox" style="height:auto;padding:12px">${renderWidgetHtml(widget)}</div>` : ''}`,
        `<button class="btn lg dg" style="margin-right:auto" onclick="deleteDataWidget('${widget.id}')">${t('dataWidget.delete')}</button><button class="btn lg" onclick="closeModal('widget-edit-modal')">${t('common.cancel')}</button><button class="btn lg primary" onclick="saveWidgetEdit('${widget.id}')">${t('dataWidget.save')}</button>`);
    modal.onclick = (e) => { if (e.target === modal) closeModal('widget-edit-modal'); };
    document.body.appendChild(modal);
}

/**
 * 选择变量插入到表达式
 */
async function selectVariableForWidget() {
    await showVariableSelectModal(null, 'replace');
    const modal = document.getElementById('variable-select-modal');
    if (modal) {
        modal.dataset.callback = 'widgetExpression';
    }
}

/**
 * 保存组件编辑
 */
function saveWidgetEdit(widgetId) {
    clearFieldErrors();
    const widget = dataWidgets.find(w => w.id === widgetId);
    if (!widget) return;
    
    const next = { ...widget };
    next.label = document.getElementById('edit-label')?.value?.trim() || widget.label;
    next.icon = document.getElementById('edit-icon')?.value?.trim() || '';
    next.color = document.getElementById('edit-color')?.value || '#4dabf7';
    next.layout = document.querySelector('input[name="edit-layout"]:checked')?.value || 'auto';
    try {
        if (widget.type !== 'log') {
            next.unit = document.getElementById('edit-unit')?.value?.trim() || '';
            next.decimals = readNumericInput('edit-decimals', true);
            if (document.getElementById('edit-min')) next.min = readNumericInput('edit-min');
            if (document.getElementById('edit-max')) next.max = readNumericInput('edit-max');
            if (['ring', 'gauge', 'bar'].includes(widget.type) && next.min >= next.max) {
                fieldError('edit-max', t('inputRepair.minMax'));
                return;
            }
        }
        if (widget.type === 'status') {
            next.thresholds = [1, 2, 3].map(n => readNumericInput('edit-threshold-' + n));
        }
        if (widget.type === 'log') {
            next.maxLines = readNumericInput('edit-max-lines', true);
            next.refreshInterval = readNumericInput('edit-refresh-interval', true);
        }
    } catch (_) { return; }
    next.expression = document.getElementById('edit-expression')?.value?.trim() || null;
    if (widget.type === 'dual') next.expression2 = document.getElementById('edit-expression2')?.value?.trim() || null;
    Object.assign(widget, next);
    if (widget.type === 'log' && widget._isReading) {
        stopLogReading(widgetId);
        startLogReading(widgetId);
    }

    saveDataWidgets();
    renderDataWidgets();
    renderWidgetManagerList();
    refreshDataWidgets();
    showToast(typeof t === 'function' ? t('toast.widgetSaved') : '组件已保存', 'success');
    
    // 关闭编辑与管理器弹窗
    closeModal('widget-edit-modal');
    closeModal('widget-manager-modal');
}

/**
 * 删除组件
 */
async function deleteDataWidget(widgetId) {
    const idx = dataWidgets.findIndex(w => w.id === widgetId);
    if (idx === -1) return;
    
    const widget = dataWidgets[idx];
    
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteWidget', { label: widget.label }) : `确定要删除"${widget.label}"组件吗？`, { primary: t('dataWidget.delete'), tone: 'danger' })) return;
    
    dataWidgets.splice(idx, 1);
    saveDataWidgets();
    renderDataWidgets();
    renderWidgetManagerList();
    
    closeModal('widget-edit-modal');
    
    showToast(typeof t === 'function' ? t('toast.widgetDeleted', { name: widget.label }) : `已删除 ${widget.label}`, 'info');
}

// ==================== 快捷操作（手动触发规则） ====================

let _sshExecCircuitBreaker = { failUntil: 0, failCount: 0 };

/**
 * 刷新快捷操作面板
 */
async function refreshQuickActions() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('quick-actions-grid');
    if (!container) {
        return;
    }
    
    try {
        // 确保 SSH 主机数据已加载（用于 nohup 按钮）
        if (!window._sshHostsData || Object.keys(window._sshHostsData).length === 0) {
            await loadSshHostsData();
            if (!pageCurrent()) return;
        }
        
        // 强制刷新 SSH 命令缓存，确保 nohup/serviceMode 等字段为最新
        await loadSshCommands();
        if (!pageCurrent()) return;
        
        const result = await api.call('automation.rules.list');
        if (!pageCurrent()) return;
        
        if (result.code === 0 && result.data && result.data.rules) {
            if (result.data.loaded === false || result.data.recovery_required === true) {
                const reason = result.data.load_error || 'recovery_required';
                const key = 'rulePack.errors.' + reason;
                const message = t(key) === key ? t('rulePack.errors.recovery_required') : t(key);
                container.innerHTML = `<div class="empty"><p class="t-note" style="color:var(--bad)">${escapeHtml(message)}</p></div>`;
                return;
            }
            // 过滤出启用且标记为可手动触发的规则
            if (!result.data.loaded || result.data.recovery_required) {
                container.textContent = runtimeText(result.data.recovery_required ? 'recovery_required' : 'configLoading'); return;
            }
            const allRules = result.data.rules;
            const manualRules = allRules.filter(r => r.show_on_dashboard === true);
            
            // 按 localStorage 保存的顺序排列快捷操作
            let savedOrder = [];
            try {
                const raw = localStorage.getItem('quick_actions_order');
                if (raw) {
                    savedOrder = JSON.parse(raw);
                    if (!Array.isArray(savedOrder)) savedOrder = [];
                }
            } catch (e) {
                if (!pageCurrent()) return; savedOrder = []; }
            if (savedOrder.length > 0) {
                manualRules.sort((a, b) => {
                    const ia = savedOrder.indexOf(a.id);
                    const ib = savedOrder.indexOf(b.id);
                    if (ia === -1 && ib === -1) return 0;
                    if (ia === -1) return 1;
                    if (ib === -1) return -1;
                    return ia - ib;
                });
            }
            
            if (manualRules.length > 0) {
                // 串行检查每个规则的 nohup 状态并生成卡片，避免多路 ssh.exec 并发导致后端串行/覆盖、结果错位
                const cardsHtml = [];
                for (const rule of manualRules) {
                    const nohupInfo = await checkRuleHasNohupSsh(rule);
                    if (!pageCurrent()) return;
                    let nohupBtns = '', statusHtml = '';
                    let isRunning = false;
                    if (nohupInfo?.serviceMode) {
                        statusHtml = `<span class="quick-action-service-status" data-command="${escapeHtml(nohupInfo.commandId)}"><span class="service-value">${runtimeText('unknown')}</span></span>`;
                        nohupBtns = `
                            <div class="quick-action-nohup-bar" onclick="event.stopPropagation()">
                                <button type="button" class="btn icon sm" onclick="verifyServiceState('${escapeHtml(nohupInfo.commandId)}')" title="${runtimeText('verifyState')}" aria-label="${runtimeText('verifyState')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                                <button type="button" class="btn sm" onclick="quickActionViewLog('${escapeHtml(nohupInfo.logFile)}', '${escapeHtml(nohupInfo.hostId)}')"><svg class="i"><use href="#ri-file-list-line"/></svg>${t('automationPage.logTitle')}</button>
                                <button type="button" class="btn sm dg" onclick="quickActionStopProcess('${escapeHtml(nohupInfo.commandId)}', ${!rule.manual_trigger})"><svg class="i"><use href="#ri-stop-fill"/></svg>${t('automationPage.stopProcess')}</button>
                            </div>`;
                    }
                    const cardOnClick = `triggerQuickAction('${escapeHtml(rule.id)}')`;
                    const cleanName = rule.name.replace(/^[\u{1F300}-\u{1F9FF}\u{2600}-\u{26FF}\u{2700}-\u{27BF}\u{FE00}-\u{FE0F}\u{1F1E0}-\u{1F1FF}\u200D]+\s*/gu, '').trim();
                    cardsHtml.push(`
                        <div class="quick-action-card${nohupInfo ? ' has-nohup' : ''}${nohupInfo?.serviceMode ? ' has-service' : ''}${isRunning ? ' is-running' : ''}" 
                             id="quick-action-${escapeHtml(rule.id)}"
                             data-rule-id="${escapeHtml(rule.id)}"
                             data-allowed="${rule.enabled && rule.allow_manual_trigger && !rule.reference_unresolved && !nohupInfo?.unresolved && rule.pending_change !== 'delete' && rule.runtime_active !== false}"
                             data-pending-change="${escapeHtml(rule.pending_change || 'none')}"
                             aria-disabled="${rule.pending_change === 'delete' || rule.runtime_active === false}"
                             data-service="${nohupInfo?.serviceMode ? escapeHtml(nohupInfo.commandId) : ''}"
                             data-state="${nohupInfo?.serviceMode ? 'unknown' : 'stopped'}"
                             onclick="${cardOnClick}" 
                             title="${escapeHtml(cleanName)}">
                            <div class="quick-action-head"><div class="quick-action-name">${escapeHtml(cleanName)}</div></div>
                            <div class="quick-action-foot"><small>${rule.pending_change === 'delete' ? t('rulePack.pendingDelete') + ' · ' : ''}${(!nohupInfo?.serviceMode || !rule.enabled || !rule.manual_trigger) ? runtimeText(!rule.enabled ? 'disabled' : rule.manual_trigger ? 'manual' : 'automatic') : ''}${(rule.reference_unresolved || nohupInfo?.unresolved) ? ' · ' + runtimeText('referenceUnresolved') : ''}${statusHtml && (!rule.enabled || !rule.manual_trigger) ? ' · ' : ''}${statusHtml}</small>${nohupBtns}</div>
                        </div>
                    `);
                }
                container.innerHTML = cardsHtml.join('');
                
                // 更新服务状态
                updateQuickActionServiceStatus();
                
                // 启动定时刷新服务状态（每 3 秒）
                startServiceStatusRefresh();
            } else {
                container.innerHTML = `
                    <div class="empty">
                        <p class="t-body">${typeof t === 'function' ? t('automationPage.noQuickActions') : '暂无快捷操作'}</p>
                        <p class="t-note">${typeof t === 'function' ? t('automationPage.quickActionsHint') : '在自动化规则中启用"手动触发"选项'}</p>
                    </div>
                `;
            }
        } else {
            container.innerHTML = '<p class="t-note">' + (typeof t === 'function' ? (t('automationPage.loadQuickActionsFailed') || '无法加载快捷操作') : '无法加载快捷操作') + '</p>';
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('Quick actions error:', e);
        container.innerHTML = '<p class="t-note">' + (typeof t === 'function' ? t('filePage.loadFailed') : '加载失败') + '</p>';
    }
}

/**
 * 更新快捷操作卡片中的服务状态
 */
let serviceStatusRefreshInterval = null;

function startServiceStatusRefresh() {
    // 清除之前的定时器
    stopServiceStatusRefresh();
    
    // 每 3 秒刷新一次服务状态
    serviceStatusRefreshInterval = setInterval(() => {
        const statusContainers = document.querySelectorAll('.quick-action-service-status, .service-mode-status .service-status');
        if (statusContainers.length === 0) {
            stopServiceStatusRefresh();
            return;
        }
        updateQuickActionServiceStatus();
    }, 3000);
}

function stopServiceStatusRefresh() {
    if (serviceStatusRefreshInterval) {
        clearInterval(serviceStatusRefreshInterval);
        serviceStatusRefreshInterval = null;
    }
}

let serviceStatusInFlight = false;
const serviceStateVersions = new Map();
function advanceServiceState(commandId) {
    const version = (serviceStateVersions.get(commandId) || 0) + 1;
    serviceStateVersions.set(commandId, version);
    return version;
}
const serviceVerifyInFlight = new Set();
const serviceStates = ['unknown', 'starting', 'checking', 'running', 'ready', 'stopping', 'stopped', 'timeout', 'failed'];
function renderServiceState(element, data = {}) {
    const launching = data.operation_phase === 'queued' || data.operation_phase === 'executing';
    const state = launching ? (data.operation_kind === 'stop' ? 'stopping' : data.operation_kind === 'verify' ? 'checking' : 'starting') : serviceStates.includes(data.state) ? data.state : 'unknown';
    for (const value of serviceStates) element.classList.remove('status-' + value);
    element.classList.add('status-' + state);
    const label = element.querySelector('.service-value') || element;
    label.textContent = runtimeText(launching ? (data.operation_phase === 'queued' ? 'controlQueued' : data.operation_kind === 'verify' ? 'controlVerifying' : state) : state);
    element.title = runtimeText('stateEvidence') + ': ' + (data.source || 'none') +
        (data.confirmed_ms ? ' · ' + runtimeText('confirmedAt') + ' ' + Math.floor(data.confirmed_ms / 1000) + 's' : '');
    const card = element.closest('.quick-action-card');
    if (card) card.dataset.state = state;
}
function renderServiceCommand(commandId, data) {
    for (const element of document.querySelectorAll('.quick-action-service-status, .service-mode-status .service-status'))
        if (element.dataset.command === commandId) renderServiceState(element, data);
}
function renderServiceOperation(commandId, data) {
    const output = document.getElementById('exec-result');
    if (!output || output.dataset.serviceCommand !== commandId || !output.dataset.operationId || !data.operation_id) return;
    const matches = Number(output.dataset.operationId) === data.operation_id;
    const phase = matches ? data.operation_phase : 'operationReplaced';
    if (!phase || phase === output.dataset.operationPhase) return;
    output.dataset.operationPhase = phase;
    output.textContent += '\n' + runtimeText(phase === 'expired' ? 'controlExpired' : 'launch_' + phase) +
        (matches && data.operation_error ? ' (' + data.operation_error + ')' : '');
}
async function refreshServiceStates() {
    if (document.hidden || serviceStatusInFlight) return;
    serviceStatusInFlight = true;
    try {
        for (const element of document.querySelectorAll('.quick-action-service-status, .service-mode-status .service-status')) {
            const commandId = element.dataset.command;
            const revision = serviceStateVersions.get(commandId);
            if (!element.isConnected || document.hidden) break;
            if (serviceVerifyInFlight.has(commandId)) continue;
            let data = {};
            try {
                const response = await api.call('automation.services.status', { command_id: commandId });
                if (response.code === 0) data = response.data || {};
            } catch (_) {}
            if (element.isConnected && revision === serviceStateVersions.get(commandId) && !serviceVerifyInFlight.has(commandId)) {
                renderServiceState(element, data);
                renderServiceOperation(commandId, data);
            }
        }
    } finally { serviceStatusInFlight = false; }
}
async function updateQuickActionServiceStatus() {
    await refreshServiceStates();
}
// Explicit controls poll only the local operation record. No automatic resubmission.
async function waitServiceOperation(commandId, operationId, isCurrent) {
    const deadline = Date.now() + 60000;
    while (isCurrent() && Date.now() < deadline) {
        const response = requireApiSuccess(await api.call('automation.services.status', {command_id: commandId}), 'automation.services.status');
        if (!isCurrent()) return null;
        const data = response.data || {};
        if (data.operation_id !== operationId) throw new Error(runtimeText('launch_operationReplaced'));
        if (data.operation_phase === 'succeeded') return data;
        if (data.operation_phase === 'expired') throw new Error(runtimeText('controlExpired'));
        if (data.operation_phase === 'failed' || data.operation_phase === 'unconfirmed')
            throw new Error(runtimeText('outcomeUnknown'));
        await new Promise(resolve => setTimeout(resolve, 1000));
    }
    if (isCurrent()) throw new Error(runtimeText('outcomeUnknown'));
    return null;
}
async function requestServiceControl(commandId, stop, isCurrent) {
    const endpoint = stop ? 'automation.services.stop' : 'automation.services.status';
    const params = stop ? {command_id: commandId} : {command_id: commandId, verify: true};
    const response = requireApiSuccess(await api.call(endpoint, params, 'POST'), endpoint);
    if (!Number.isInteger(response.data?.operation_id) || response.data.operation_id <= 0)
        throw new ApiOperationError({}, endpoint, {kind: 'format', uncertain: true});
    return waitServiceOperation(commandId, response.data.operation_id, isCurrent);
}
async function verifyServiceState(commandId) {
    if (serviceVerifyInFlight.has(commandId)) return;
    serviceVerifyInFlight.add(commandId);
    const revision = advanceServiceState(commandId);
    const pageCurrent = capturePageValidity();
    const isCurrent = () => pageCurrent() && revision === serviceStateVersions.get(commandId);
    try {
        const data = await requestServiceControl(commandId, false, isCurrent);
        if (!data || !isCurrent()) return;
        renderServiceCommand(commandId, data);
        showToast(runtimeText(data.state || 'unknown'), 'info');
        return data;
    } catch (e) {
        if (!isCurrent()) return;
        renderServiceCommand(commandId, {});
        showToast(e.code === 4 ? runtimeText('controlBusy') : e.message || runtimeText('outcomeUnknown'), 'warning');
    } finally { advanceServiceState(commandId); serviceVerifyInFlight.delete(commandId); }
}

// 触发快捷操作后的冷却时间（毫秒），避免连续触发导致后端只执行最后一个
let _quickActionTriggerCooldownUntil = 0;
let _quickActionLastTriggeredId = '';
let quickActionsTimeoutId = null;  // 用于导航时取消，避免 quick-actions-grid not found

/** 供 router 在页面切换时取消快捷操作定时器，并销毁拖拽排序（防止 ghost 残留） */
window.stopSystemPageTimers = function() {
    releaseExecDisplay();
    clearInterval(refreshInterval);
    refreshInterval = null;
    clearInterval(localTimeInterval);
    localTimeInterval = null;
    clearInterval(window.systemUptimeInterval);
    window.systemUptimeInterval = null;
    stopLpmuStatePolling();
    for (const id of Object.keys(logWidgetTimers)) stopLogReading(id);
    stopServiceStatusRefresh();
    if (quickActionsTimeoutId) {
        clearTimeout(quickActionsTimeoutId);
        quickActionsTimeoutId = null;
    }
    if (typeof window._destroyWidgetSort === 'function') {
        window._destroyWidgetSort();
        window._destroyWidgetSort = null;
    }
    if (typeof window._destroyQuickSort === 'function') {
        window._destroyQuickSort();
        window._destroyQuickSort = null;
    }
};

/**
 * 触发快捷操作
 * @param {string} ruleId - 规则 ID
 */
async function triggerQuickAction(ruleId) {
    const card = document.getElementById(`quick-action-${ruleId}`);
    if (!card) {
        console.error('triggerQuickAction: card not found for ruleId=', ruleId);
        showToast((typeof t === 'function' ? t('toast.cardNotFound') : '无法找到操作卡片'), 'error');
        return;
    }
    
    if (card.classList.contains('triggering')) {
        showToast(typeof t === 'function' ? t('toast.processing') : 'Operation in progress...', 'warning');
        return;
    }
    
    const now = Date.now();
    if (now < _quickActionTriggerCooldownUntil && ruleId !== _quickActionLastTriggeredId) {
        showToast((typeof t === 'function' ? t('toast.waitBeforeTrigger') : '请等待几秒后再触发其他模型'), 'warning');
        return;
    }
    
    try {
        // Keep verification and triggering within the same in-flight operation.
        if (card.dataset.service) advanceServiceState(card.dataset.service);
        card.classList.add('triggering');
        card.style.pointerEvents = 'none';

        // A user start request may verify unknown state. Periodic refresh remains local.
        if (card.dataset.allowed === 'true' && card.dataset.service && card.dataset.state === 'unknown') {
            try {
                const checked = await requestServiceControl(card.dataset.service, false,
                    () => card.isConnected && document.getElementById(`quick-action-${ruleId}`) === card);
                if (!checked) return;
                card.dataset.state = checked.state || 'unknown';
            } catch (_) { card.dataset.state = 'unknown'; }
        }
        if (card.dataset.allowed !== 'true' || (card.dataset.service && card.dataset.state !== 'stopped')) {
            showToast(card.dataset.pendingChange === 'delete' ? t('rulePack.pendingDelete') : runtimeText('startBlocked'), 'warning'); return;
        }

        if (card.dataset.service) card.dataset.state = 'starting';

        // 更新图标显示加载状态
        const iconEl = card.querySelector('.quick-action-icon');
        const originalIcon = iconEl?.innerHTML;
        if (iconEl) {
            iconEl.innerHTML = '<span class="spinner-small"><svg class="i"><use href="#ri-refresh-line"/></svg></span>';
        }
        
        const result = await api.call('automation.rules.trigger', { id: ruleId });
        
        if (result.code === 0) {
            showToast((typeof t === 'function' ? t('toast.operationExecuted') : '操作已执行'), 'success');
            _quickActionLastTriggeredId = ruleId;
            _quickActionTriggerCooldownUntil = Date.now() + 5000;  // 5 秒内勿触发其他规则，避免后端串行导致第二个未执行
            card.classList.add('is-running');
            setTimeout(() => refreshQuickActions(), 2500);
        } else {
            showToast((result.message || (typeof t === 'function' ? t('toast.execFailed') : '执行失败')), 'error');
            // 恢复原始图标
            if (iconEl && originalIcon) {
                iconEl.innerHTML = originalIcon;
            }
        }
        
    } catch (e) {
        console.error('triggerQuickAction error:', e);
        showToast(runtimeText('outcomeUnknown'), 'error');
        if (card?.dataset.service) card.dataset.state = 'unknown';
    } finally {
        if (card.dataset.service) advanceServiceState(card.dataset.service);
        card.classList.remove('triggering');
        card.style.pointerEvents = '';
    }
}

/**
 * 检查规则是否包含 nohup SSH 命令
 * @param {object} rule - 规则对象（列表中的简化数据）
 * @returns {object|null} - 返回 {logFile, keyword, hostId} 或 null
 */
async function checkRuleHasNohupSsh(rule) {
    // 列表 API 只返回 actions_count，需要获取完整规则
    if (!rule.actions_count || rule.actions_count === 0) {
        return null;
    }
    
    // 获取规则详情
    try {
        const detailResult = await api.call('automation.rules.get', { id: rule.id });
        if (detailResult.code !== 0 || !detailResult.data || !detailResult.data.actions) {
            return { unresolved: true };
        }
        
        const actions = detailResult.data.actions;
        
        // 确保 SSH 命令已加载
        if (Object.keys(sshCommands).length === 0) {
            await loadSshCommands();
        }
        
        // 遍历所有动作
        for (const action of actions) {
            let sshCmdId = null;
            
            // 方式1: 动作本身是 ssh_cmd_ref 类型（兼容两种 API 返回格式）
            // rules.get 返回 action.cmd_id（直接字段），actions.get 返回 action.ssh_ref.cmd_id（嵌套）
            if (!action.template_id && action.type === 'ssh_cmd_ref' && (action.ssh_ref?.cmd_id || action.cmd_id)) {
                sshCmdId = action.ssh_ref?.cmd_id || action.cmd_id;
            }
            // 方式2: 动作有 template_id，需要查询模板获取实际类型
            else if (action.template_id) {
                try {
                    const tplResult = await api.call('automation.actions.get', { id: action.template_id });
                    if (tplResult.code === 0 && tplResult.data) {
                        if (tplResult.data.type === 'ssh_cmd_ref' && (tplResult.data.ssh_ref?.cmd_id || tplResult.data.cmd_id)) {
                            sshCmdId = tplResult.data.ssh_ref?.cmd_id || tplResult.data.cmd_id;
                        }
                    } else return { unresolved: true };
                } catch (e) {
                    return { unresolved: true };
                }
            }
            
            if (sshCmdId) {
                const cmdId = String(sshCmdId);
                let foundCommand = false;
                // 在所有主机的命令中查找
                for (const [hostId, cmds] of Object.entries(sshCommands)) {
                    const cmd = cmds.find(c => String(c.id) === cmdId);
                    if (cmd) {
                        foundCommand = true;
                        if (cmd.nohup) {
                            // 找到了 nohup 命令
                            // safeName：优先从 cmd.name 提取英文数字，fallback 到 cmd.id
                            // （纯中文名如"嵌入模型拉起"提取不到任何字符，需要用 id）
                            const safeName = cmd.name.replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || String(cmd.id).replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || 'cmd';
                            const logFile = `/tmp/ts_nohup_${safeName}.log`;
                            const pidFile = `/tmp/ts_nohup_${safeName}.pid`;
                            const varName = cmd.varName || '';  // 服务模式变量名
                            
                            return {
                                logFile: logFile,
                                pidFile: pidFile,
                                keyword: cmd.command,
                                progName: safeName,
                                hostId: hostId,
                                cmdName: cmd.name,
                                commandId: cmdId,
                                // 服务模式信息
                                serviceMode: cmd.serviceMode || false,
                                varName: varName,
                                readyPattern: cmd.readyPattern || '',
                                serviceFailPattern: cmd.serviceFailPattern || ''
                            };
                        }
                    }
                }
                if (!foundCommand) return { unresolved: true };
            }
        }
    } catch (e) {
        console.error('checkRuleHasNohupSsh error:', e);
        return { unresolved: true };
    }
    return null;
}

/**
  * 快捷操作 - 查看日志
 */
let quickActionTailInterval = null;
let quickActionLastContent = '';

async function quickActionViewLog(logFile, hostId) {
    // 清空上次的日志缓存
    quickActionLastContent = '';
    
    // 获取主机信息
    const host = window._sshHostsData?.[hostId];
    if (!host) {
        showToast((typeof t === 'function' ? t('sshPage.hostNotExistShort') : '主机不存在'), 'error');
        return;
    }
    
    const logTitle = typeof t === 'function' ? t('automationPage.logTitle') : '日志';
    const stopTrackingText = typeof t === 'function' ? t('automationPage.stopTracking') : '停止跟踪';
    const intervalText = typeof t === 'function' ? t('automationPage.interval') : '间隔';
    const interval1Sec = typeof t === 'function' ? t('automationPage.interval1Sec') : '1秒';
    const interval2Sec = typeof t === 'function' ? t('automationPage.interval2Sec') : '2秒';
    const interval3Sec = typeof t === 'function' ? t('automationPage.interval3Sec') : '3秒';
    const interval5Sec = typeof t === 'function' ? t('automationPage.interval5Sec') : '5秒';
    const interval10Sec = typeof t === 'function' ? t('automationPage.interval10Sec') : '10秒';
    const interval30Sec = typeof t === 'function' ? t('automationPage.interval30Sec') : '30秒';
    const realTimeText = typeof t === 'function' ? t('automationPage.realTimeUpdating') : '● 实时更新中';
    const closeText = typeof t === 'function' ? t('common.close') : '关闭';
    const safeFile = escapeHtml(logFile), safeHost = escapeHtml(hostId);
    const modalHtml = `<div id="quick-log-modal" class="modal">${sheet(760, `${logTitle} <small class="t-note mono" style="font-weight:400">${safeFile}</small>`, `
            <pre class="term logv" id="quick-log-content" style="height:320px;margin:0;white-space:pre-wrap">${t('common.loading')}</pre>`,
            `<span class="inl" style="margin-right:auto"><button class="btn lg" id="quick-log-tail-btn" onclick="toggleQuickLogTail('${safeFile}', '${safeHost}')"><svg class="i"><use href="#ri-stop-fill"/></svg>${stopTrackingText}</button>
                <span class="t-note">${intervalText}</span>
                <select class="field" id="quick-log-interval" onchange="updateQuickLogInterval('${safeFile}', '${safeHost}')" style="width:90px">
                    <option value="1000">${interval1Sec}</option>
                    <option value="2000">${interval2Sec}</option>
                    <option value="3000">${interval3Sec}</option>
                    <option value="5000" selected>${interval5Sec}</option>
                    <option value="10000">${interval10Sec}</option>
                    <option value="30000">${interval30Sec}</option>
                </select>
                <span id="quick-log-status" class="t-note"><span style="color:var(--ok)">${realTimeText}</span></span></span>
            <button class="btn lg primary" onclick="closeQuickLogModal()">${closeText}</button>`, 'closeQuickLogModal()')}</div>
    `;
    
    // 添加模态框
    const existing = document.getElementById('quick-log-modal');
    if (existing) existing.remove();
    document.body.insertAdjacentHTML('beforeend', modalHtml);
    
    // 加载日志并自动开始跟踪
    await quickActionRefreshLog(logFile, hostId);
    
    // 自动开始实时跟踪（默认5秒间隔）
    startQuickLogTail(logFile, hostId, 5000);
}

async function quickActionRefreshLog(logFile, hostId) {
    const host = window._sshHostsData?.[hostId];
    if (!host) return;
    
    const contentEl = document.getElementById('quick-log-content');
    if (!contentEl || document.hidden || contentEl.dataset.loading === 'true') return;
    contentEl.dataset.loading = 'true';
    
    try {
        const result = await api.call('ssh.exec', {
            host: host.host,
            port: host.port,
            user: host.username,
            keyid: host.keyid,
            command: `if [ -f ${logFile} ]; then tail -c 60000 -- ${logFile}; else echo '${typeof t === 'function' ? t('automationPage.logFileEmptyBracket') : '[日志文件不存在或为空]'}'; fi`,
            timeout_ms: 15000
        });
        if (!contentEl.isConnected || document.getElementById('quick-log-content') !== contentEl) return;
        if (result.code !== 0 || !result.data) {
            contentEl.textContent = (typeof t === 'function' ? t('automationPage.logFetchFailed') : '[获取失败]') + ' ' + (result.message || 'code=' + result.code);
            return;
        }
        const _empty = typeof t === 'function' ? t('automationPage.logEmptyBracket') : '[空]';
        const output = (result.data.stdout || result.data.stderr || '').trim() || _empty;
        if (output !== quickActionLastContent) {
            contentEl.textContent = output;
            contentEl.scrollTop = contentEl.scrollHeight;
            quickActionLastContent = output;
        }
    } catch (e) {
        if (!contentEl.isConnected || document.getElementById('quick-log-content') !== contentEl) return;
        const _err = typeof t === 'function' ? t('automationPage.logError') : '[错误]';
        const _retry = typeof t === 'function' ? t('automationPage.deviceBusyRetry') : '若设备繁忙可稍后重试。';
        contentEl.textContent = _err + ' ' + e.message + '\n\n' + _retry;
    } finally {
        contentEl.dataset.loading = 'false';
    }
}

// 保存当前跟踪的参数，用于更新间隔时重启
let quickActionTailParams = { logFile: null, hostId: null };

/**
 * 开始日志跟踪
 * @param {string} logFile - 日志文件路径
 * @param {string} hostId - 主机 ID
 * @param {number} intervalMs - 刷新间隔（毫秒），默认 5000
 */
function startQuickLogTail(logFile, hostId, intervalMs = 5000) {
    if (quickActionTailInterval) {
        clearInterval(quickActionTailInterval);
    }
    
    // 保存参数
    quickActionTailParams = { logFile, hostId };
    
    const btn = document.getElementById('quick-log-tail-btn');
    const status = document.getElementById('quick-log-status');
    
    if (btn) {
        const stopText = typeof t === 'function' ? t('automationPage.stopTracking') : '停止跟踪';
        btn.innerHTML = '<svg class="i"><use href="#ri-stop-fill"/></svg>' + stopText;
    }
    if (status) {
        const realTimeText = typeof t === 'function' ? t('automationPage.realTimeUpdating') : '● 实时更新中';
        status.innerHTML = '<span style="color:var(--ok)">' + realTimeText + '</span>';
    }
    quickActionLastContent = '';
    
    // 定义刷新函数
    const doRefresh = async () => {
        // 检查模态框是否还存在
        if (!document.getElementById('quick-log-modal')) {
            clearInterval(quickActionTailInterval);
            quickActionTailInterval = null;
            return;
        }
        try {
            await quickActionRefreshLog(logFile, hostId);
        } catch (e) {
            console.error('Tail refresh error:', e);
        }
    };
    
    // 立即执行一次
    doRefresh();
    
    // 设置定时器
    quickActionTailInterval = setInterval(doRefresh, intervalMs);
}

/**
 * 更新日志刷新间隔
 */
function updateQuickLogInterval(logFile, hostId) {
    const select = document.getElementById('quick-log-interval');
    if (!select) return;
    
    const intervalMs = parseInt(select.value, 10);
    
    // 如果正在跟踪，重新启动以应用新间隔
    if (quickActionTailInterval) {
        startQuickLogTail(logFile, hostId, intervalMs);
    }
}

/**
 * 停止日志跟踪
 */
function stopQuickLogTail() {
    if (quickActionTailInterval) {
        clearInterval(quickActionTailInterval);
        quickActionTailInterval = null;
    }
    
    const btn = document.getElementById('quick-log-tail-btn');
    const status = document.getElementById('quick-log-status');
    
    if (btn) {
        const startText = typeof t === 'function' ? t('automationPage.startTracking') : '开始跟踪';
        btn.innerHTML = '<svg class="i"><use href="#ri-play-line"/></svg>' + startText;
    }
    if (status) status.textContent = (typeof t === 'function' ? t('automationPage.trackingStopped') : '已暂停');
}

/**
 * 切换日志跟踪状态
 */
function toggleQuickLogTail(logFile, hostId) {
    if (quickActionTailInterval) {
        stopQuickLogTail();
    } else {
        // 获取当前选择的间隔
        const select = document.getElementById('quick-log-interval');
        const intervalMs = select ? parseInt(select.value, 10) : 5000;
        startQuickLogTail(logFile, hostId, intervalMs);
    }
}

function closeQuickLogModal() {
    if (quickActionTailInterval) {
        clearInterval(quickActionTailInterval);
        quickActionTailInterval = null;
    }
    const modal = document.getElementById('quick-log-modal');
    if (modal) modal.remove();
}

/**
 * 快捷操作 - 终止进程（基于 PID 文件精确停止）
 * 后端验证实例身份并停止进程组，页面仅显示核验结果。
 */
const serviceStopInFlight = new Set();
async function quickActionStopProcess(commandId, automatic) {
    if (serviceStopInFlight.has(commandId)) return;
    if (!await confirmAction(runtimeText(automatic ? 'confirmStopAutomatic' : 'confirmStop'), { primary: t('common.stop'), tone: 'neutral' })) return;
    serviceStopInFlight.add(commandId);
    const revision = advanceServiceState(commandId);
    const pageCurrent = capturePageValidity();
    const isCurrent = () => pageCurrent() && revision === serviceStateVersions.get(commandId);
    renderServiceCommand(commandId, {state: 'stopping'});
    try {
        const data = await requestServiceControl(commandId, true, isCurrent);
        if (!data || !isCurrent()) return;
        renderServiceCommand(commandId, data);
        const stopped = data.state === 'stopped';
        showToast(runtimeText(stopped ? 'stopped' : 'outcomeUnknown'), stopped ? 'success' : 'warning');
    } catch (e) {
        if (isCurrent()) { renderServiceCommand(commandId, {}); showToast(e.code === 4 ? runtimeText('controlBusy') : e.message || runtimeText('outcomeUnknown'), 'warning'); }
    } finally { advanceServiceState(commandId); serviceStopInFlight.delete(commandId); await updateQuickActionServiceStatus(); }
}

// 时间同步功能
async function syncTimeFromBrowser(silent = false) {
    try {
        const now = Date.now();
        if (!silent) showToast(t('toast.timeSyncSyncing'), 'info');
        const result = await api.timeSync(now);
        if (result.data?.synced) {
            if (!silent) showToast(t('toast.timeSynced', { datetime: result.data.datetime }), 'success');
            
            // 重新获取时间信息并更新显示
            try {
                const timeInfo = await api.timeInfo();
                if (timeInfo.data) {
                    updateTimeInfo(timeInfo.data);
                }
            } catch (e) {
                console.error('Failed to refresh time info:', e);
            }
        } else {
            if (!silent) showToast(t('toast.timeSyncFailed'), 'error');
        }
    } catch (e) {
        if (!silent) showToast(t('toast.syncFailed') + ': ' + e.message, 'error');
    }
}

async function forceNtpSync() {
    try {
        showToast(t('toast.ntpSyncing'), 'info');
        const result = await api.timeForceSync();
        if (result.data?.syncing) {
            showToast(t('toast.ntpStarted'), 'success');
            setTimeout(refreshSystemPage, 3000);
        }
    } catch (e) {
        showToast(t('toast.ntpFailed') + ': ' + e.message, 'error');
    }
}

function showTimezoneModal() {
    let modal = document.getElementById('timezone-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'timezone-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = sheet(520, t('common.setTimezone'),
        grp(
            row(t('common.timezone'), `<select class="field" id="timezone-select" style="width:210px">
                    <option value="CST-8">${t('common.tzChinaStandard')}</option>
                    <option value="JST-9">${t('common.tzJapanStandard')}</option>
                    <option value="KST-9">${t('common.tzKoreaStandard')}</option>
                    <option value="UTC0">${t('common.tzUTC')}</option>
                    <option value="GMT0">${t('common.tzGMT')}</option>
                    <option value="EST5EDT">${t('common.tzUSEastern')}</option>
                    <option value="PST8PDT">${t('common.tzUSPacific')}</option>
                    <option value="CET-1CEST">${t('common.tzCentralEuropean')}</option>
                </select>`) +
            row(t('common.customTimezoneShort'), inp('timezone-custom', 210, t('common.timezoneExampleShort'), 'mono'), '', t('common.customTimezone'))),
        `<button class="btn lg" onclick="hideTimezoneModal()">${t('common.cancel')}</button><button class="btn lg primary" onclick="applyTimezone()">${t('common.apply')}</button>`);
    
    modal.classList.remove('hidden');
}

function hideTimezoneModal() {
    const modal = document.getElementById('timezone-modal');
    if (modal) modal.classList.add('hidden');
}

async function applyTimezone() {
    const select = document.getElementById('timezone-select');
    const custom = document.getElementById('timezone-custom');
    const timezone = custom.value.trim() || select.value;
    
    try {
        const result = await api.timeSetTimezone(timezone);
        if (result.data?.success) {
            showToast(t('common.timezoneSetSuccess', { timezone, localTime: result.data.local_time }), 'success');
            hideTimezoneModal();
            await refreshSystemPage();
        }
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.setFailed') : '设置失败') + ': ' + e.message, 'error');
    }
}

// =========================================================================
//                         LED 页面
// =========================================================================

// 存储设备信息和特效列表
let ledDevices = {};
let ledEffects = [];

async function loadLedPage() {
    clearInterval(refreshInterval);
    
    // 取消系统页面的订阅
    
    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page-led">
            <div class="led-page-header">
                <h1>${t('nav.led')}</h1>
                <div class="led-quick-actions">
                    <button type="button" class="btn btn-sm btn-gray led-refresh-btn" onclick="refreshLedPage()" title="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                    <button class="btn btn-sm btn-gray led-color-correction-btn" id="led-page-cc-btn" onclick="openLedModal('matrix', 'colorcorrection')" style="display:none"><svg class="i"><use href="#ri-contrast-line"/></svg> ${t('ledPage.colorCorrectionTitle')}</button>
                    <button class="btn btn-sm btn-gray" onclick="allLedsOff()">${typeof t === 'function' ? t('ledPage.allOff') : '全部关闭'}</button>
                </div>
            </div>
            <div id="led-devices-grid" class="led-devices-grid">
                <div class="loading-inline">${typeof t === 'function' ? t('ledPage.loadingDevices') : '加载设备中...'}</div>
            </div>
        </div>
    `;
    
    await refreshLedPage();
}

async function refreshLedPage() {
    const container = document.getElementById('led-devices-grid');
    
    try {
        const result = await api.ledList();
        
        if (result.data && result.data.devices && result.data.devices.length > 0) {
            // 存储设备信息
            result.data.devices.forEach(dev => {
                ledDevices[dev.name] = dev;
                if (dev.current && dev.current.animation) {
                    selectedEffects[dev.name] = dev.current.animation;
                }
                // 初始化 LED 状态
                if (dev.current) {
                    ledStates[dev.name] = dev.current.on || false;
                }
            });
            
            window.ledDevicesCache = result.data.devices;
            
            // 渲染设备卡片
            container.innerHTML = result.data.devices.map(dev => generateLedDeviceCard(dev)).join('');
            
            // 加载字体列表 & 显示色彩校正按钮
            if (result.data.devices.some(d => d.name === 'matrix' || d.layout === 'matrix')) {
                loadFontList();
                const ccBtn = document.getElementById('led-page-cc-btn');
                if (ccBtn) ccBtn.style.display = '';
            }
        } else {
            container.innerHTML = `
                <div class="led-empty-state">
                    <div class="empty-icon"><svg class="i" style="color:var(--warn-dot)"><use href="#ri-error-warning-line"/></svg></div>
                    <h3>${t('ledPage.ledNotFound')}</h3>
                    <p>${t('ledPage.ledNotStartedHint')}</p>
                    <ul>
                        <li>${t('promptRepair.ledServiceStatus')} (<code>service --status</code>)</li>
                        <li>${t('ledPage.checkGpioConfig')}</li>
                    </ul>
                </div>
            `;
        }
    } catch (e) {
        console.error('LED list error:', e);
        container.innerHTML = `<div class="error-state">${escapeHtml(typeof t === 'function' ? t('common.loadFailedMsg', { msg: e.message }) : '加载失败: ' + e.message)}</div>`;
    }
}

// 色块：[显示色, 下发给灯的颜色, 名称 key 后缀]
const LED_PRESETS = [['#ff3b30', '#ff0000', 'Red'], ['#ff6b00', '#ff6600', 'Orange'], ['#ffd60a', '#ffd700', 'Yellow'], ['#00c766', '#00d26a', 'Green'], ['#00e5ff', '#00ffff', 'Cyan'], ['#2d7dff', '#2d7dff', 'Blue'], ['#d630ff', '#d630ff', 'Purple'], ['#ffffff', '#ffffff', 'White'], ['#ffcccc', '#ffcccc', 'Pink'], ['#e0e0e0', '#e0e0e0', 'Gray']];

function generateLedDeviceCard(dev) {
    const description = t({ board: 'ledPage.descBoard', touch: 'ledPage.descTouch', matrix: 'ledPage.descMatrix' }[dev.name.toLowerCase()] || 'ledPage.deviceDefault');
    const current = dev.current || {};
    const isOn = current.on || false;
    const currentAnimation = current.animation || '';
    const colorHex = rgbToHex(current.color || {r: 255, g: 255, b: 255});
    const isMatrix = dev.name === 'matrix' || dev.layout === 'matrix';
    const brightness = dev.brightness ?? 0;

    let statusText = t('ledPage.statusOff');
    if (isOn) statusText = currentAnimation ? `▶ ${effectDisplayName(currentAnimation)}` : t('ledPage.statusOn');

    const icoBtn = (icon, title, action, cls = '') => `<button class="btn sm icon ${cls}" onclick="${action}" title="${title}" aria-label="${title}"><svg class="i"><use href="#${icon}"/></svg></button>`;
    const matrixBtns = isMatrix ? icoBtn('ri-qr-code-line', t('ledPage.contentTitle'), `openLedModal('${dev.name}', 'content')`, 'led-quick-effect')
        + icoBtn('ri-text', t('ledPage.textTitle'), `openLedModal('${dev.name}', 'text')`, 'led-quick-effect')
        + icoBtn('ri-color-filter-line', t('ledPage.filterEffect'), `openLedModal('${dev.name}', 'filter')`, 'led-quick-effect') : '';
    const powerTitle = isOn ? t('ledPage.clickOff') : t('ledPage.clickOn');
    
    return `
        <div class="w-tile led-device-card ${isOn ? 'is-on' : ''}" data-device="${dev.name}">
            <div class="led-h">
                <div>
                    <div class="t-section">${dev.name.charAt(0).toUpperCase() + dev.name.slice(1)}</div>
                    <div class="t-note" style="margin-top:2px">${description}</div>
                </div>
                <div class="between">
                    <span class="state led-device-status${isOn ? ' ok' : ''}">${escapeHtml(statusText)}</span>
                    <button class="btn sm" onclick="stopEffect('${dev.name}')">${t('ledPage.stopEffect')}</button>
                </div>
            </div>
            <hr class="sep">
            <div class="led-c">
                <div class="led-br">
                    <svg class="i"><use href="#ri-sun-line"/></svg>
                    <div class="sl" style="--p:${brightness / 2.55}%"><i></i><b></b><input type="range" min="0" max="255" value="${brightness}" class="led-brightness-slider" oninput="updateBrightnessDisplay('${dev.name}', this.value)" onchange="setBrightness('${dev.name}', this.value)" id="brightness-${dev.name}"></div>
                    <span class="t-value" id="brightness-val-${dev.name}">${brightness}</span>
                </div>
                <div class="led-dots">
                    <label class="dotc pick" title="${t('promptRepair.customColor')}"><input type="color" value="${colorHex}" id="color-picker-${dev.name}" onchange="fillColorFromPicker('${dev.name}', this.value)" aria-label="${t('promptRepair.customColor')}"></label>
                    ${LED_PRESETS.map(([shown, sent, name]) => `<button class="dotc" style="background:${shown}" onclick="quickFillColor('${dev.name}', '${sent}')" title="${t('promptRepair.color' + name)}" aria-label="${t('promptRepair.color' + name)}"></button>`).join('')}
                </div>
            </div>
            <hr class="sep">
            <div class="led-f">
                <button class="btn sm icon led-power-btn ${isOn ? 'on' : ''}" id="toggle-${dev.name}" onclick="toggleLed('${dev.name}')" title="${powerTitle}" aria-label="${powerTitle}"><svg class="i power-icon"><use href="#ri-lightbulb-line"/></svg></button>
                ${icoBtn('ri-play-line', t('ledPage.moreEffects'), `openLedModal('${dev.name}', 'effect')`, 'led-quick-effect')}
                ${matrixBtns}
                <span style="flex:1"></span>
                ${icoBtn('ri-save-line', t('led.saveConfig'), `saveLedConfig('${dev.name}')`)}
            </div>
        </div>
    `;
}

// 辅助函数
function rgbToHex(color) {
    const r = (color.r || 0).toString(16).padStart(2, '0');
    const g = (color.g || 0).toString(16).padStart(2, '0');
    const b = (color.b || 0).toString(16).padStart(2, '0');
    return '#' + r + g + b;
}

function updateBrightnessDisplay(device, value) {
    const label = document.getElementById(`brightness-val-${device}`);
    if (label) label.textContent = value;
    document.getElementById(`brightness-${device}`)?.parentElement.style.setProperty('--p', (value / 2.55) + '%');
}

async function fillColorFromPicker(device, color) {
    try {
        requireApiSuccess(await api.ledFill(device, color), 'ledFill');
        ledStates[device] = true;
        updateLedCardState(device, true);
        showToast(typeof t === 'function' ? t('toast.ledFilled', { device: getDeviceDescription(device), color }) : `${device} 已填充 ${color}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledFillFailed') + ': ' + e.message : `填充失败: ${e.message}`, 'error');
    }
}

async function quickFillColor(device, color) {
    const picker = document.getElementById(`color-picker-${device}`);
    if (picker) picker.value = color;
    try {
        requireApiSuccess(await api.ledFill(device, color), 'ledFill');
        ledStates[device] = true;
        updateLedCardState(device, true, null);
        showToast(typeof t === 'function' ? t('toast.ledFilled', { device: getDeviceDescription(device), color }) : `${device} → ${color}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledFillFailed') + ': ' + e.message : `填充失败: ${e.message}`, 'error');
    }
}

async function quickStartEffect(device, effect) {
    try {
        requireApiSuccess(await api.ledEffectStart(device, effect, { speed: 50 }), 'ledEffectStart');
        selectedEffects[device] = effect;
        ledStates[device] = true;
        updateLedCardState(device, true, effect);
        showToast(typeof t === 'function' ? t('toast.ledEffectStarted', { device: getDeviceDescription(device), effect }) : `${device}: ${effect}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledEffectStartFailed') + ': ' + e.message : `启动失败: ${e.message}`, 'error');
    }
}

async function allLedsOff() {
    const devices = window.ledDevicesCache || [];
    let success = 0; const failures = [];
    for (const dev of devices) {
        try {
            requireApiSuccess(await api.ledClear(dev.name), 'ledClear');
            success++;
            ledStates[dev.name] = false;
            updateLedCardState(dev.name, false);
        } catch (e) {
            failures.push({path: dev.name, message: e.message});
        }
    }
    showOperationSummary('promptRepair.ledSummary', success, failures);
}

function updateLedCardState(device, isOn, effect = undefined) {
    const card = document.querySelector(`.led-device-card[data-device="${device}"]`);
    if (!card) return;
    
    card.classList.toggle('is-on', !!isOn);
    
    // 更新状态显示
    const statusEl = card.querySelector('.led-device-status');
    if (statusEl) {
        statusEl.textContent = !isOn ? t('ledPage.statusOff') : effect ? `▶ ${effectDisplayName(effect)}` : t('ledPage.statusOn');
        statusEl.className = 'state led-device-status' + (isOn ? ' ok' : '');
    }
    
    // 更新电源按钮
    const powerBtn = card.querySelector('.led-power-btn');
    if (powerBtn) {
        powerBtn.classList.toggle('on', !!isOn);
        powerBtn.title = t(isOn ? 'ledPage.clickOff' : 'ledPage.clickOn');
    }
    
    // 更新快捷特效按钮状态
    card.querySelectorAll('.led-quick-effect').forEach(btn => {
        btn.classList.toggle('active', !!effect && btn.getAttribute('title') === effect);
    });
}

// 颜色选择模态框
function openColorModal(device) {
    const deviceData = window.ledDevicesCache?.find(d => d.name === device);
    const current = deviceData?.current || {};
    const currentColor = current.color || {r: 255, g: 0, b: 0};
    const colorHex = '#' + 
        currentColor.r.toString(16).padStart(2, '0') +
        currentColor.g.toString(16).padStart(2, '0') +
        currentColor.b.toString(16).padStart(2, '0');
    
    const modal = document.getElementById('led-modal');
    const title = document.getElementById('led-modal-title');
    const body = document.getElementById('led-modal-body');
    
    title.textContent = `${device} - ${typeof t === 'function' ? t('ui.colorSettings') : '颜色设置'}`;
    body.innerHTML = `
        <div class="modal-section">
            <h3>${typeof t === 'function' ? t('ledPage.colorSelect') : '颜色选择'}</h3>
            <div class="config-row">
                <input type="color" id="modal-color-picker-${device}" value="${colorHex}" style="width:60px;height:40px;">
                <button class="btn btn-service-style" onclick="applyColorFromModal('${device}')">${typeof t === 'function' ? t('ledPage.fillColor') : '填充颜色'}</button>
            </div>
            <h3 style="margin-top:16px;">${typeof t === 'function' ? t('ledPage.quickColors') : '快捷颜色'}</h3>
            <div class="preset-colors-grid">
                <button class="color-preset" style="background:#ff0000" onclick="quickFillFromModal('${device}', '#ff0000')"></button>
                <button class="color-preset" style="background:#ff6600" onclick="quickFillFromModal('${device}', '#ff6600')"></button>
                <button class="color-preset" style="background:#ffff00" onclick="quickFillFromModal('${device}', '#ffff00')"></button>
                <button class="color-preset" style="background:#00ff00" onclick="quickFillFromModal('${device}', '#00ff00')"></button>
                <button class="color-preset" style="background:#00ffff" onclick="quickFillFromModal('${device}', '#00ffff')"></button>
                <button class="color-preset" style="background:#0000ff" onclick="quickFillFromModal('${device}', '#0000ff')"></button>
                <button class="color-preset" style="background:#ff00ff" onclick="quickFillFromModal('${device}', '#ff00ff')"></button>
                <button class="color-preset" style="background:#ffffff" onclick="quickFillFromModal('${device}', '#ffffff')"></button>
                <button class="color-preset" style="background:#ffcccc" onclick="quickFillFromModal('${device}', '#ffcccc')"></button>
                <button class="color-preset" style="background:#ccffcc" onclick="quickFillFromModal('${device}', '#ccffcc')"></button>
                <button class="color-preset" style="background:#ccccff" onclick="quickFillFromModal('${device}', '#ccccff')"></button>
                <button class="color-preset" style="background:#000000" onclick="quickFillFromModal('${device}', '#000000')"></button>
            </div>
        </div>
    `;
    
    modal.classList.remove('hidden');
}

async function applyColorFromModal(device) {
    const color = document.getElementById(`modal-color-picker-${device}`)?.value || '#ffffff';
    try {
        requireApiSuccess(await api.ledFill(device, color), 'ledFill');
        ledStates[device] = true;
        updateToggleButton(device, true);
        showToast(typeof t === 'function' ? t('toast.ledFilled', { device: getDeviceDescription(device), color }) : `${device} 已填充 ${color}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledFillFailed') + ': ' + e.message : `填充失败: ${e.message}`, 'error');
    }
}

async function quickFillFromModal(device, color) {
    try {
        requireApiSuccess(await api.ledFill(device, color), 'ledFill');
        ledStates[device] = true;
        updateToggleButton(device, true);
        showToast(typeof t === 'function' ? t('toast.ledFilled', { device: getDeviceDescription(device), color }) : `${device} → ${color}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledFillFailed') + ': ' + e.message : `填充失败: ${e.message}`, 'error');
    }
}

// 更新开关按钮状态
function updateToggleButton(device, isOn) {
    // 更新新版卡片
    updateLedCardState(device, isOn);
    
    // 旧版兼容
    const btn = document.getElementById(`toggle-${device}`);
    if (btn && !btn.classList.contains('led-power-btn')) {
        if (isOn) {
            btn.classList.add('on');
            btn.innerHTML = '<svg class="i"><use href="#ri-sun-line"/></svg> ' + (typeof t === 'function' ? t('ledPage.on') : '已开启');
        } else {
            btn.classList.remove('on');
            btn.innerHTML = '<svg class="i"><use href="#ri-lightbulb-line"/></svg> ' + (typeof t === 'function' ? t('ledPage.off') : '已关闭');
        }
    }
}

// 生成 LED 模态框内容
// LED 弹窗小控件：滑块（--p 由 oninput 同步）、颜色色块、分段控件（选中值写进同名隐藏 input）
const ledSlider = (id, min, max, val, on) => `<div class="sl" style="width:200px;flex:none;--p:${(val - min) / (max - min) * 100}%"><i></i><b></b><input type="range" id="${id}" min="${min}" max="${max}" value="${val}" oninput="syncSliders();${on}"></div>`;
const ledVal = (id, txt) => `<span class="t-value num" id="${id}" style="width:40px;text-align:right">${txt}</span>`;
const swatch = (id, val, on = '') => `<input type="color" class="swi" id="${id}" value="${val}"${on ? ` oninput="${on}"` : ''}>`;
function syncSliders() {
    document.querySelectorAll('.sl>input[type="range"]').forEach(i => i.parentElement.style.setProperty('--p', ((i.value - i.min) / (i.max - i.min) * 100) + '%'));
}
function segPick(btn, hiddenId, value) {
    btn.parentElement.querySelectorAll('button').forEach(b => b.classList.toggle('on', b === btn));
    document.getElementById(hiddenId).value = value;
}
// 文本页：打开「自动位置」时 X / Y 置灰
function toggleTextAutoPos(auto) {
    document.getElementById('modal-text-pos-note').classList.toggle('hidden', !auto);
    for (const id of ['modal-text-x', 'modal-text-y']) {
        const el = document.getElementById(id);
        el.disabled = auto;
        el.closest('.row').classList.toggle('off', auto);
    }
}

// 每个标签页 → [正文, 底部按钮]
function generateLedModalContent(device, type) {
    const deviceData = window.ledDevicesCache?.find(d => d.name === device);
    const current = deviceData?.current || {};
    const currentAnimation = current.animation || '';
    const currentSpeed = current.speed || 50;
    const currentColor = current.color || {r: 255, g: 0, b: 0};
    const colorHex = '#' +
        currentColor.r.toString(16).padStart(2, '0') +
        currentColor.g.toString(16).padStart(2, '0') +
        currentColor.b.toString(16).padStart(2, '0');
    const deviceEffects = deviceData?.effects || [];
    const num = (id, w, val, attrs = '') => inp(id, w, '', 'num', `type="number" value="${val}" ${attrs}`);
    const cancelReset = (fn) => `<button class="btn lg" onclick="${fn}">${t('ledPage.ccReset')}</button>`;

    if (type === 'effect') {
        const effectsHtml = deviceEffects.length > 0
            ? deviceEffects.map(eff => `<button class="tile effect-btn${eff === currentAnimation ? ' on' : ''}" onclick="selectEffectInModal('${device}', '${eff}', this)" style="padding:10px 12px;justify-content:center"><span class="t-body">${effectDisplayName(eff)}</span></button>`).join('')
            : '<span class="t-note">' + t('ledPage.noEffects') + '</span>';
        const isOn = ledStates[device] || false;
        const body = grp(row(t('ledPage.deviceEnable'), swc(`modal-device-enabled-${device}`, isOn, `onchange="toggleLedFromModal('${device}', this.checked)"`))) +
            gt(t('ledPage.animationGroup')) +
            `<div class="effects-grid" style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">${effectsHtml}</div>
            <div class="gw" id="modal-effect-config-${device}" style="display:${currentAnimation ? 'block' : 'none'}">
                ${gt(t('ledPage.settings'))}
                ${grp(
                    row(t('ledPage.current'), `<span class="t-value" id="modal-effect-name-${device}">${currentAnimation ? effectDisplayName(currentAnimation) : t('ledPage.effectNotSelected')}</span>`) +
                    row(t('ledPage.speed'), ledSlider(`modal-effect-speed-${device}`, 1, 100, currentSpeed, `updateEffectSliderValue('${device}', this.value)`) + ledVal(`modal-speed-val-${device}`, currentSpeed)) +
                    `<div class="row" id="modal-color-row-${device}" style="display:${colorSupportedEffects.includes(currentAnimation) ? 'flex' : 'none'}"><div class="rl">${t('ledPage.color')}</div><div class="rc">${swatch(`modal-effect-color-${device}`, colorHex, `previewEffectFromModal('${device}')`)}</div></div>`)}
            </div>`;
        return [body, `<button class="btn lg" onclick="stopEffectFromModal('${device}')"><svg class="i"><use href="#ri-stop-circle-line"/></svg>${t('ledPage.stop')}</button>${cancelReset(`resetEffectFromModal('${device}')`)}<button class="btn lg primary" onclick="applyEffectFromModal('${device}')">${t('ledPage.ccApply')}</button>`];
    }
    if (type === 'content') {
        const body = `<div style="display:flex;justify-content:center;margin:12px 0 4px"><div class="seg"><button class="on" onclick="switchModalTab(this, 'modal-tab-image')">${t('ledPage.imageTab')}</button><button onclick="switchModalTab(this, 'modal-tab-qr')">${t('ledPage.qrTabSp')}</button></div></div>
            <div id="modal-tab-image">${grp(
                row(t('automationPage.imagePath'), inp('modal-image-path', 200, '/sdcard/images/...', 'mono', 'value="/sdcard/images/"') + `<button class="btn sm" onclick="browseImages()">${t('ledPage.browse')}</button>`) +
                row(t('ledPage.centerDisplay'), swc('modal-image-center', true)))}</div>
            <div class="gw" id="modal-tab-qr" style="display:none">
                ${gt(t('ledPage.contentGroup'))}
                <input type="text" class="field" id="modal-qr-text" placeholder="${t('ledPage.enterTextOrUrl')}" aria-label="${t('ledPage.enterTextOrUrl')}" style="width:100%">
                ${gt(t('ledPage.styleGroup'))}
                ${grp(
                    row(t('ledPage.errorCorrection'), `<select class="field" id="modal-qr-ecc" style="width:130px"><option value="L">L · 7%</option><option value="M" selected>M · 15%</option><option value="Q">Q · 25%</option><option value="H">H · 30%</option></select>`) +
                    row(t('ledPage.foregroundColor'), swatch('modal-qr-fg', '#ffffff')) +
                    row(t('ledPage.backgroundImage'), `<input class="field" id="modal-qr-bg-image" placeholder="${t('ledPage.noBackgroundImage')}" aria-label="${t('ledPage.noBackgroundImage')}" readonly style="width:150px;cursor:pointer" onclick="openFilePickerFor('modal-qr-bg-image', '/sdcard/images')"><button class="btn sm" onclick="openFilePickerFor('modal-qr-bg-image', '/sdcard/images')">${t('ledPage.browse')}</button><button class="btn sm" onclick="document.getElementById('modal-qr-bg-image').value=''">${t('ledPage.clear')}</button>`))}
            </div>`;
        return [body, `<button class="btn lg primary" id="led-foot-image" onclick="displayImageFromModal()">${t('ledPage.displayImage')}</button><button class="btn lg primary hidden" id="led-foot-qr" onclick="generateQrCodeFromModal()">${t('ledPage.generateQrCode')}</button>`];
    }
    if (type === 'text') {
        const body = gt(t('ledPage.textGroup')) +
            `<input type="text" class="field" id="modal-text-content" placeholder="${t('ledPage.enterTextToDisplay')}" aria-label="${t('ledPage.enterTextToDisplay')}" style="width:100%">` +
            gt(t('ledPage.fontAndStyle')) +
            grp(
                row(t('ledPage.font'), `<select class="field" id="modal-text-font" style="width:150px"><option value="default">${t('common.default')}</option></select><button class="btn icon sm" onclick="loadFontListForModal()" title="${t('ledPage.refreshFonts')}" aria-label="${t('ledPage.refreshFonts')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>`) +
                row(t('ledPage.alignment'), `<div class="seg"><button onclick="segPick(this,'modal-text-align','left')">${t('ledPage.alignShortLeft')}</button><button class="on" onclick="segPick(this,'modal-text-align','center')">${t('ledPage.alignShortCenter')}</button><button onclick="segPick(this,'modal-text-align','right')">${t('ledPage.alignShortRight')}</button></div><input type="hidden" id="modal-text-align" value="center">`) +
                row(t('ledPage.color'), swatch('modal-text-color', '#00ff00'))) +
            gt(t('ledPage.position')) +
            grp(
                row(t('ledPage.autoPosition'), swc('modal-text-auto-pos', true, 'onchange="toggleTextAutoPos(this.checked)"')) +
                `<div class="row off"><div class="rl">X</div><div class="rc">${num('modal-text-x', 80, 0, 'min="0" max="255" placeholder="X" disabled')}<span class="t-note" id="modal-text-pos-note">${t('ledPage.autoPosNote')}</span></div></div>` +
                `<div class="row off"><div class="rl">Y</div><div class="rc">${num('modal-text-y', 80, 0, 'min="0" max="255" placeholder="Y" disabled')}</div></div>`) +
            gt(t('ledPage.scroll')) +
            grp(
                row(t('ledPage.paramDirection'), `<select class="field" id="modal-text-scroll" style="width:110px"><option value="none">${t('ledPage.scrollDirNone')}</option><option value="left" selected>${t('ledPage.scrollLeft')}</option><option value="right">${t('ledPage.scrollRight')}</option><option value="up">${t('ledPage.scrollUp')}</option><option value="down">${t('ledPage.scrollDown')}</option></select>`) +
                row(t('ledPage.paramSpeed'), num('modal-text-speed', 80, 50, 'min="1" max="100"')) +
                row(t('ledPage.loopScroll'), swc('modal-text-loop', true)));
        return [body, `<button class="btn lg" onclick="stopTextFromModal()"><svg class="i"><use href="#ri-stop-circle-line"/></svg>${t('ledPage.stop')}</button><button class="btn lg primary" onclick="displayTextFromModal()">${t('ledPage.display')}</button>`];
    }
    if (type === 'filter') {
        const filters = ['pulse', 'breathing', 'blink', 'wave', 'scanline', 'glitch', 'rainbow', 'sparkle', 'plasma', 'sepia', 'posterize', 'contrast', 'invert', 'grayscale'];
        const body = gt(t('ledPage.filterGroup')) +
            `<div class="filters-grid" style="display:grid;grid-template-columns:repeat(5,1fr);gap:8px">${filters.map(f => `<button class="tile filter-btn" data-filter="${f}" onclick="selectFilterInModal('${f}', this)" style="padding:10px 12px;justify-content:center"><span class="t-body">${t('ledPage.filter' + f.charAt(0).toUpperCase() + f.slice(1))}</span></button>`).join('')}</div>` +
            gt(t('ledPage.settings')) +
            grp(row(t('ledPage.current'), `<span class="t-value" id="modal-filter-name">${t('ledPage.effectNotSelected')}</span>`) +
                `<div class="rows" id="modal-filter-params">${row(t('ledPage.paramsRow'), `<span class="t-note">${t('ledPage.paramsVary')}</span>`)}</div>`);
        return [body, `<button class="btn lg" onclick="stopFilterFromModal()"><svg class="i"><use href="#ri-stop-circle-line"/></svg>${t('ledPage.stop')}</button><button class="btn lg primary" id="modal-apply-filter-btn" onclick="applyFilterFromModal()" disabled>${t('ledPage.apply')}</button>`];
    }
    if (type === 'colorcorrection') {
        const ccRow = (label, id, min, max, note = '') => row(label, ledSlider(id, min, max, 100, `updateCcSliderValue('${id}-val', this.value / 100)`) + ledVal(`${id}-val`, '1.00'), note);
        const body = grp(row(t('ledPage.ccEnable'), swc('cc-enabled', false, 'onchange="previewColorCorrection()"'))) +
            gt(t('ledPage.ccWhiteBalanceTitle')) +
            grp(ccRow(t('ledPage.ccRed'), 'cc-wp-r', 0, 200) + ccRow(t('ledPage.ccGreen'), 'cc-wp-g', 0, 200) + ccRow(t('ledPage.ccBlue'), 'cc-wp-b', 0, 200)) +
            gt(t('ledPage.ccAdjust')) +
            grp(ccRow('Gamma', 'cc-gamma', 10, 400, t('ledPage.ccGammaNote')) + ccRow(t('ledPage.ccBrightness'), 'cc-brightness', 0, 200, t('ledPage.ccBrightnessNote')) + ccRow(t('ledPage.ccSaturation'), 'cc-saturation', 0, 200, t('ledPage.ccSaturationNote'))) +
            gt(t('ledPage.ccBackup')) +
            grp(row(t('ledPage.ccConfigFile'), `<button class="btn sm" onclick="ccExport()" title="${t('ledPage.ccExportTip')}">${t('ledPage.ccExportSd')}</button><button class="btn sm" onclick="ccImport()" title="${t('ledPage.ccImportTip')}">${t('ledPage.ccImportSd')}</button>`));
        return [body, `<button class="btn lg" onclick="resetColorCorrection()">${t('ledPage.ccReset')}</button><button class="btn lg primary" onclick="applyColorCorrection()">${t('ledPage.ccApply')}</button>`];
    }
    return [`<div class="t-note">${t('ledPage.unknownType')}</div>`, ''];
}

// LED 模态框存储
let currentLedModal = { device: null, type: null };
let selectedModalFilter = null;

// 打开 LED 模态框（矩阵屏有 5 个标签，其它设备只有程序动画）
function openLedModal(device, type) {
    currentLedModal = { device, type };
    selectedModalFilter = null;
    
    const deviceName = device.charAt(0).toUpperCase() + device.slice(1);
    const deviceData = window.ledDevicesCache?.find(d => d.name === device);
    const isMatrix = device === 'matrix' || deviceData?.layout === 'matrix';
    const tabs = isMatrix ? `<div class="seg full" style="margin-bottom:6px">${[['effect', 'effectTitle'], ['content', 'contentTitle'], ['text', 'textTitle'], ['filter', 'filterTitle'], ['colorcorrection', 'colorCorrectionTitle']].map(([k, key]) => `<button class="${k === type ? 'on' : ''}" onclick="openLedModal('${device}', '${k}')">${t('ledPage.' + key)}</button>`).join('')}</div>` : '';
    const [body, foot] = generateLedModalContent(device, type);
    
    const modal = document.getElementById('led-modal');
    modal.innerHTML = sheet(660, `${t('led.settingsTitle')} · ${type === 'colorcorrection' ? t('ledPage.colorCorrectionTitle') : deviceName}`, tabs + body, foot, 'closeLedModal()');
    modal.classList.remove('hidden');
    
    // 加载字体列表（如果是文本模态框）
    if (type === 'text') {
        loadFontListForModal();
    }
    
    // 加载色彩校正配置
    if (type === 'colorcorrection') {
        loadColorCorrectionConfig();
    }
}

// 关闭 LED 模态框
function closeLedModal() {
    const modal = document.getElementById('led-modal');
    modal.classList.add('hidden');
    currentLedModal = { device: null, type: null };
    selectedModalFilter = null;
}

// 图像 / QR 码 子标签切换（底部主按钮跟着换）
function switchModalTab(btn, tabId) {
    btn.parentElement.querySelectorAll('button').forEach(b => b.classList.toggle('on', b === btn));
    const isQr = tabId === 'modal-tab-qr';
    document.getElementById('modal-tab-image').style.display = isQr ? 'none' : 'block';
    document.getElementById('modal-tab-qr').style.display = isQr ? 'block' : 'none';
    document.getElementById('led-foot-image').classList.toggle('hidden', isQr);
    document.getElementById('led-foot-qr').classList.toggle('hidden', !isQr);
}

// 模态框内选择特效
function selectEffectInModal(device, effect, btn) {
    selectedEffects[device] = effect;
    
    // 更新按钮状态
    btn.closest('.effects-grid').querySelectorAll('.effect-btn').forEach(b => b.classList.remove('on'));
    btn.classList.add('on');
    
    // 显示特效名（RemixIcon，无 emoji，首字母大写）
    const effectName = document.getElementById(`modal-effect-name-${device}`);
    if (effectName) effectName.textContent = effectDisplayName(effect);
    
    // 显示/隐藏颜色选择器
    const colorRow = document.getElementById(`modal-color-row-${device}`);
    if (colorRow) {
        colorRow.style.display = colorSupportedEffects.includes(effect) ? 'flex' : 'none';
    }
    
    // 显示配置区
    const configEl = document.getElementById(`modal-effect-config-${device}`);
    if (configEl) configEl.style.display = 'block';

    // 自动触发实时预览
    previewEffectFromModal(device);
}

// 模态框内开关 LED
async function toggleLedFromModal(device, enabled) {
    try {
        if (enabled) {
            // 如果有选中的特效，启动它；否则仅开启（常亮或恢复之前状态）
            const effect = selectedEffects[device];
            if (effect) {
                await applyEffectFromModal(device);
            } else {
                await toggleLed(device); // 这会调用 api.ledClear(device) 如果 isOn，但这里我们传入了 enabled
                // 实际上 toggleLed 不需要 enabled 参数，它基于 ledStates[device]
                // 修正逻辑：如果当前关闭且 enabled 为 true，则开启
                if (!ledStates[device]) await toggleLed(device);
            }
        } else {
            // 关闭 LED
            if (ledStates[device]) await toggleLed(device);
        }
        
        // 更新卡片状态
        updateLedCardState(device, ledStates[device], selectedEffects[device]);
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.operationFailedMsg', { msg: e.message }) : `操作失败: ${e.message}`, 'error');
        // 恢复 UI 状态
        const cb = document.getElementById(`modal-device-enabled-${device}`);
        if (cb) cb.checked = ledStates[device];
    }
}

// 更新特效滑块值并预览
function updateEffectSliderValue(device, value) {
    const valEl = document.getElementById(`modal-speed-val-${device}`);
    if (valEl) valEl.textContent = value;
    
    // 实时预览
    previewEffectFromModal(device);
}

// 特效预览防抖
let effectPreviewDebounce = null;

// 实时预览特效
function previewEffectFromModal(device) {
    if (effectPreviewDebounce) clearTimeout(effectPreviewDebounce);
    effectPreviewDebounce = setTimeout(async () => {
        const effect = selectedEffects[device];
        if (!effect || !ledStates[device]) return;
        
        const speed = parseInt(document.getElementById(`modal-effect-speed-${device}`)?.value || '50');
        const color = document.getElementById(`modal-effect-color-${device}`)?.value || '#ff0000';
        
        try {
            const params = { speed };
            if (colorSupportedEffects.includes(effect)) {
                params.color = color;
            }
            // 预览使用 start 接口，但不显示成功 toast 以免干扰
            requireApiSuccess(await api.ledEffectStart(device, effect, params), 'ledEffectStart');
            
            // 更新卡片状态（静默更新）
            updateLedCardState(device, true, effect);
        } catch (e) {
            console.warn('Effect preview failed:', e);
        }
    }, 200);
}

// 重置特效设置
function resetEffectFromModal(device) {
    const deviceData = window.ledDevicesCache?.find(d => d.name === device);
    const current = deviceData?.current || {};
    const defaultSpeed = 50;
    const defaultColor = '#ff0000';
    
    const speedSlider = document.getElementById(`modal-effect-speed-${device}`);
    const colorPicker = document.getElementById(`modal-effect-color-${device}`);
    const speedVal = document.getElementById(`modal-speed-val-${device}`);
    
    if (speedSlider) speedSlider.value = defaultSpeed;
    if (speedVal) speedVal.textContent = defaultSpeed;
    if (colorPicker) colorPicker.value = defaultColor;
    
    syncSliders();
    showToast(t('ledPage.ccResetSuccess'), 'success');
    previewEffectFromModal(device);
}

// 模态框内应用动画
async function applyEffectFromModal(device) {
    const effect = selectedEffects[device];
    if (!effect) {
        showToast((typeof t === 'function' ? t('toast.selectAnimation') : '请先选择一个动画'), 'warning');
        return;
    }
    
    const speed = parseInt(document.getElementById(`modal-effect-speed-${device}`)?.value || '50');
    const color = document.getElementById(`modal-effect-color-${device}`)?.value || '#ff0000';
    
    try {
        const params = { speed };
        if (colorSupportedEffects.includes(effect)) {
            params.color = color;
        }
        requireApiSuccess(await api.ledEffectStart(device, effect, params), 'ledEffectStart');
        
        ledStates[device] = true;
        updateLedCardState(device, true, effect);
        
        showToast(typeof t === 'function' ? t('toast.ledEffectStarted', { device: getDeviceDescription(device), effect }) : `${device}: ${effect} 已启动`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledEffectStartFailed') + ': ' + e.message : `启动动画失败: ${e.message}`, 'error');
    }
}

// 模态框内停止动画
async function stopEffectFromModal(device) {
    try {
        requireApiSuccess(await api.ledEffectStop(device), 'ledEffectStop');
        delete selectedEffects[device];
        updateLedCardState(device, ledStates[device], null);
        showToast(typeof t === 'function' ? t('toast.ledEffectStopped', { device: getDeviceDescription(device) }) : `${device} 动画已停止`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledEffectStopFailed') + ': ' + e.message : `停止动画失败: ${e.message}`, 'error');
    }
}

// 模态框内显示图像
async function displayImageFromModal() {
    const path = document.getElementById('modal-image-path')?.value;
    const center = document.getElementById('modal-image-center')?.checked;
    
    if (!path) {
        showToast((typeof t === 'function' ? t('toast.enterImagePath') : '请输入图像路径'), 'warning');
        return;
    }
    
    try {
        requireApiSuccess(await api.call('led.image', { device: 'matrix', path, center }), 'call');
        showToast((typeof t === 'function' ? t('toast.imageDisplayed') : '图像已显示'), 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledImageFailed') + ': ' + e.message : `显示图像失败: ${e.message}`, 'error');
    }
}

// 模态框内生成 QR 码
async function generateQrCodeFromModal() {
    const text = document.getElementById('modal-qr-text')?.value;
    const ecc = document.getElementById('modal-qr-ecc')?.value || 'M';
    const fg = document.getElementById('modal-qr-fg')?.value || '#ffffff';
    const bgImage = document.getElementById('modal-qr-bg-image')?.value || '';
    
    if (!text) {
        showToast((typeof t === 'function' ? t('toast.enterTextToEncode') : '请输入要编码的文本'), 'warning');
        return;
    }
    
    try {
        requireApiSuccess(await api.call('led.qrcode', { device: 'matrix', text, ecc, fg_color: fg, bg_image: bgImage || undefined }), 'call');
        showToast((typeof t === 'function' ? t('toast.qrGenerated') : 'QR 码已生成'), 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledQrFailed') + ': ' + e.message : `生成 QR 码失败: ${e.message}`, 'error');
    }
}

// 加载字体列表（模态框版本）
async function loadFontListForModal() {
    const fontSelect = document.getElementById('modal-text-font');
    if (!fontSelect) return;
    
    // 保存当前选中的字体
    const currentFont = fontSelect.value;
    
    try {
        const result = await api.storageList('/sdcard/fonts');
        const files = result.data?.entries || [];
        
        // 筛选字体文件 (.fnt, .bdf, .pcf)
        const fontExts = ['.fnt', '.bdf', '.pcf'];
        const fonts = files.filter(f => {
            if (f.type === 'dir' || f.type === 'directory') return false;
            const ext = f.name.toLowerCase().substring(f.name.lastIndexOf('.'));
            return fontExts.includes(ext);
        });
        
        // 清空选项
        fontSelect.innerHTML = '';
        
        // 添加字体文件（移除扩展名，因为后端会自动添加 .fnt）
        fonts.forEach(f => {
            const option = document.createElement('option');
            // 移除扩展名 (.fnt, .bdf, .pcf)
            const baseName = f.name.substring(0, f.name.lastIndexOf('.'));
            option.value = baseName;
            option.textContent = f.name;  // 显示完整文件名
            fontSelect.appendChild(option);
        });
        
        // 恢复之前选中的字体
        if (currentFont && Array.from(fontSelect.options).some(opt => opt.value === currentFont)) {
            fontSelect.value = currentFont;
        }
    } catch (e) {
        console.error('加载字体失败:', e);
        // 如果加载失败，显示提示
        fontSelect.innerHTML = '<option value="">' + (typeof t === 'function' ? t('filePage.noFonts') : '无可用字体') + '</option>';
    }
}

// 模态框内显示文本
async function displayTextFromModal() {
    const text = document.getElementById('modal-text-content')?.value;
    const font = document.getElementById('modal-text-font')?.value;
    const align = document.getElementById('modal-text-align')?.value || 'center';
    const color = document.getElementById('modal-text-color')?.value || '#00ff00';
    const x = parseInt(document.getElementById('modal-text-x')?.value || '0');
    const y = parseInt(document.getElementById('modal-text-y')?.value || '0');
    const autoPos = document.getElementById('modal-text-auto-pos')?.checked;
    const scroll = document.getElementById('modal-text-scroll')?.value || 'none';
    const speed = parseInt(document.getElementById('modal-text-speed')?.value || '50');
    const loop = document.getElementById('modal-text-loop')?.checked;
    
    if (!text) {
        showToast((typeof t === 'function' ? t('toast.enterTextToDisplay') : '请输入要显示的文本'), 'warning');
        return;
    }
    
    try {
        const params = {
            device: 'matrix',
            text,
            align,
            color,
            scroll: scroll !== 'none' ? scroll : undefined,
            speed,
            loop
        };
        // 只有当用户选择了字体时才传递 font 参数
        if (font && font !== '') {
            params.font = font;
        }
        if (!autoPos) {
            params.x = x;
            params.y = y;
        }
        requireApiSuccess(await api.call('led.text', params), 'call');
        showToast((typeof t === 'function' ? t('toast.textDisplayed') : '文本已显示'), 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledTextFailed') + ': ' + e.message : `显示文本失败: ${e.message}`, 'error');
    }
}

// 模态框内停止文本
async function stopTextFromModal() {
    try {
        requireApiSuccess(await api.call('led.text.stop', { device: 'matrix' }), 'call');
        showToast((typeof t === 'function' ? t('toast.textScrollStopped') : '文本滚动已停止'), 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledTextStopFailed') + ': ' + e.message : `停止文本失败: ${e.message}`, 'error');
    }
}

// 模态框内选择滤镜
function selectFilterInModal(filter, btn) {
    selectedModalFilter = filter;
    
    btn.closest('.filters-grid').querySelectorAll('.filter-btn').forEach(b => b.classList.remove('on'));
    btn.classList.add('on');
    
    const filterName = document.getElementById('modal-filter-name');
    if (filterName) filterName.textContent = filterDisplayName(filter);
    
    const paramsDiv = document.getElementById('modal-filter-params');
    if (paramsDiv) {
        const config = filterConfig[filter];
        if (config && config.params && config.params.length > 0) {
            paramsDiv.innerHTML = config.params.map(param => {
                const paramInfo = paramLabels[param];
                const defaultValue = config.defaults[param] ?? 50;
                return row(getParamLabel(param), ledSlider(`modal-filter-${param}`, paramInfo.min, paramInfo.max, defaultValue, `document.getElementById('modal-filter-${param}-val').textContent=this.value+'${paramInfo.unit}'`) + ledVal(`modal-filter-${param}-val`, defaultValue + paramInfo.unit));
            }).join('');
        } else {
            paramsDiv.innerHTML = row(t('ledPage.paramsRow'), `<span class="t-note">${t('ledPage.paramsVary')}</span>`);
        }
    }
    
    const applyBtn = document.getElementById('modal-apply-filter-btn');
    if (applyBtn) applyBtn.disabled = false;
}

// \u6a21\u6001\u6846\u5185\u5e94\u7528\u6ede\u955c
async function applyFilterFromModal() {
    if (!selectedModalFilter) {
        showToast(typeof t === 'function' ? t('toast.selectFilter') : '请先选择一个滤镜', 'warning');
        return;
    }
    
    // \u6536\u96c6\u6240\u6709\u53c2\u6570
    const params = { device: 'matrix', filter: selectedModalFilter };
    const config = filterConfig[selectedModalFilter];
    
    if (config && config.params) {
        config.params.forEach(param => {
            const input = document.getElementById(`modal-filter-${param}`);
            if (input) {
                let value = parseInt(input.value);
                // \u6839\u636e\u53c2\u6570\u7c7b\u578b\u8f6c\u6362\u503c
                if (param === 'saturation' || param === 'frequency') {
                    value = Math.round(value * 2.55); // 0-100 \u8f6c 0-255

                } else if (param === 'amount') {
                    value = value - 50; // 0-100 \u8f6c -50 to +50
                }
                params[param] = value;
            }
        });
    }
    
    try {
        requireApiSuccess(await api.call('led.filter.start', params), 'call');
        showToast(typeof t === 'function' ? t('toast.filterApplied', { filter: filterDisplayName(selectedModalFilter) }) : `滤镜 ${selectedModalFilter} 已应用`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.filterApplyFailed') + ': ' + e.message : `应用滤镜失败: ${e.message}`, 'error');
    }
}

// 模态框内停止滤镜
async function stopFilterFromModal() {
    try {
        requireApiSuccess(await api.call('led.filter.stop', { device: 'matrix' }), 'call');
        showToast((typeof t === 'function' ? t('toast.filterStopped') : '滤镜已停止'), 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.filterStopFailed') + ': ' + e.message : `停止滤镜失败: ${e.message}`, 'error');
    }
}

// ============================================================
// 色彩校正功能
// ============================================================

// 保存进入模态框时的初始配置（用于重置）
let ccInitialConfig = null;

// 防抖定时器
let ccPreviewDebounce = null;

// 更新滑块值显示并实时预览
function updateCcSliderValue(elementId, value) {
    const el = document.getElementById(elementId);
    if (el) el.textContent = value.toFixed(2);
    
    // 防抖实时预览（200ms 延迟）
    if (ccPreviewDebounce) clearTimeout(ccPreviewDebounce);
    ccPreviewDebounce = setTimeout(() => {
        previewColorCorrection();
    }, 200);
}

// 实时预览色彩校正（不保存到 NVS）
async function previewColorCorrection() {
    const config = buildCcConfigFromUI();
    try {
        requireApiSuccess(await api.ledColorCorrectionSet(config), 'ledColorCorrectionSet');
    } catch (e) {
        console.warn('Preview failed:', e);
    }
}

// 从 UI 构建配置对象
function buildCcConfigFromUI() {
    return {
        enabled: document.getElementById('cc-enabled')?.checked || false,
        white_point: {
            r: parseFloat(document.getElementById('cc-wp-r')?.value || 100) / 100,
            g: parseFloat(document.getElementById('cc-wp-g')?.value || 100) / 100,
            b: parseFloat(document.getElementById('cc-wp-b')?.value || 100) / 100,
            enabled: true
        },
        gamma: {
            value: parseFloat(document.getElementById('cc-gamma')?.value || 100) / 100,
            enabled: true
        },
        brightness: {
            factor: parseFloat(document.getElementById('cc-brightness')?.value || 100) / 100,
            enabled: true
        },
        saturation: {
            factor: parseFloat(document.getElementById('cc-saturation')?.value || 100) / 100,
            enabled: true
        }
    };
}

// 加载色彩校正配置
async function loadColorCorrectionConfig() {
    try {
        const result = await api.ledColorCorrectionGet();
        if (result.code === 0 && result.data) {
            const config = result.data;
            
            // 保存初始配置（用于重置）
            ccInitialConfig = JSON.parse(JSON.stringify(config));
            
            // 更新 UI
            applyCcConfigToUI(config);
        }
    } catch (e) {
        console.error('Failed to load color correction config:', e);
    }
}

// 将配置应用到 UI
function applyCcConfigToUI(config) {
    // 更新启用状态
    const enabledEl = document.getElementById('cc-enabled');
    if (enabledEl) enabledEl.checked = config.enabled;
    
    // 更新白点（支持 red_scale/green_scale/blue_scale 格式）
    if (config.white_point) {
        const r = config.white_point.red_scale ?? config.white_point.r ?? 1.0;
        const g = config.white_point.green_scale ?? config.white_point.g ?? 1.0;
        const b = config.white_point.blue_scale ?? config.white_point.b ?? 1.0;
        updateCcSliderNoPreview('cc-wp-r', r * 100, r);
        updateCcSliderNoPreview('cc-wp-g', g * 100, g);
        updateCcSliderNoPreview('cc-wp-b', b * 100, b);
    }
    
    // 更新 gamma（支持 gamma 或 value 格式）
    if (config.gamma) {
        const val = config.gamma.gamma ?? config.gamma.value ?? 1.0;
        updateCcSliderNoPreview('cc-gamma', val * 100, val);
    }
    
    // 更新亮度
    if (config.brightness) {
        updateCcSliderNoPreview('cc-brightness', config.brightness.factor * 100, config.brightness.factor);
    }
    
    // 更新饱和度
    if (config.saturation) {
        updateCcSliderNoPreview('cc-saturation', config.saturation.factor * 100, config.saturation.factor);
    }
}

// 更新滑块（不触发预览）
function updateCcSliderNoPreview(sliderId, sliderValue, displayValue) {
    const slider = document.getElementById(sliderId);
    const valueEl = document.getElementById(sliderId + '-val');
    if (slider) { slider.value = Math.round(sliderValue); syncSliders(); }
    if (valueEl) valueEl.textContent = displayValue.toFixed(2);
}

// 应用色彩校正配置（保存到 NVS）
async function applyColorCorrection() {
    const config = buildCcConfigFromUI();
    
    try {
        // 先应用配置
        const result = await api.ledColorCorrectionSet(config);
        if (result.code === 0) {
            // 导出到 NVS（通过 export 保存）
            requireApiSuccess(await api.ledColorCorrectionExport(), 'ledColorCorrectionExport');
            // 更新初始配置为当前配置
            ccInitialConfig = JSON.parse(JSON.stringify(config));
            showToast(t('ledPage.ccApplySuccess'), 'success');
        } else {
            showToast(t('ledPage.ccApplyFailed') + ': ' + result.message, 'error');
        }
    } catch (e) {
        showToast(t('ledPage.ccApplyFailed') + ': ' + e.message, 'error');
    }
}

// 重置色彩校正配置（恢复到进入模态框时的状态）
async function resetColorCorrection() {
    if (!ccInitialConfig) {
        showToast(t('promptRepair.noInitialColor'), 'error');
        return;
    }
    
    try {
        // 恢复初始配置到 UI
        applyCcConfigToUI(ccInitialConfig);
        
        // 发送初始配置到设备
        const config = {
            enabled: ccInitialConfig.enabled,
            white_point: {
                r: ccInitialConfig.white_point?.red_scale ?? 1.0,
                g: ccInitialConfig.white_point?.green_scale ?? 1.0,
                b: ccInitialConfig.white_point?.blue_scale ?? 1.0,
                enabled: ccInitialConfig.white_point?.enabled ?? true
            },
            gamma: {
                value: ccInitialConfig.gamma?.gamma ?? 1.0,
                enabled: ccInitialConfig.gamma?.enabled ?? true
            },
            brightness: {
                factor: ccInitialConfig.brightness?.factor ?? 1.0,
                enabled: ccInitialConfig.brightness?.enabled ?? true
            },
            saturation: {
                factor: ccInitialConfig.saturation?.factor ?? 1.0,
                enabled: ccInitialConfig.saturation?.enabled ?? true
            }
        };
        
        const result = await api.ledColorCorrectionSet(config);
        if (result.code === 0) {
            showToast(t('ledPage.ccResetSuccess'), 'success');
        } else {
            showToast(t('ledPage.ccResetFailed') + ': ' + result.message, 'error');
        }
    } catch (e) {
        showToast(t('ledPage.ccResetFailed') + ': ' + e.message, 'error');
    }
}

// 导出色彩校正配置到 SD 卡
async function ccExport() {
    try {
        const result = await api.ledColorCorrectionExport();
        if (result.code === 0) {
            showToast(t('ledPage.ccExportSuccess'), 'success');
        } else {
            showToast(t('ledPage.ccExportFailed') + ': ' + result.message, 'error');
        }
    } catch (e) {
        showToast(t('ledPage.ccExportFailed') + ': ' + e.message, 'error');
    }
}

// 从 SD 卡导入色彩校正配置
async function ccImport() {
    try {
        const result = await api.ledColorCorrectionImport();
        if (result.code === 0) {
            // 重新加载配置到 UI
            await loadColorCorrectionConfig();
            showToast(t('ledPage.ccImportSuccess'), 'success');
        } else {
            showToast(t('ledPage.ccImportFailed') + ': ' + result.message, 'error');
        }
    } catch (e) {
        showToast(t('ledPage.ccImportFailed') + ': ' + e.message, 'error');
    }
}

function getDeviceIcon(name) {
    const icons = {
        'touch': '<svg class="i"><use href="#ri-lightbulb-flash-line"/></svg>',
        'board': '<svg class="i"><use href="#ri-dashboard-3-line"/></svg>',
        'matrix': '<svg class="i"><use href="#ri-apps-line"/></svg>'
    };
    return icons[name.toLowerCase()] || '<svg class="i"><use href="#ri-lightbulb-line"/></svg>';
}

function getDeviceDescription(name) {
    const keys = { 'touch': 'ledPage.deviceTouch', 'board': 'ledPage.deviceBoard', 'matrix': 'ledPage.deviceMatrix' };
    return t(keys[name.toLowerCase()] || 'ledPage.deviceDefault');
}

/** 程序动画模态框内使用的 RemixIcon（无 emoji，用已纳入 minimal 字体的 ri-play-line） */
function getEffectIconRemix(name) {
    if (!name) return '';
    return '<svg class="i"><use href="#ri-play-line"/></svg> ';
}

/** 动画名称首字母大写（用于更多动画弹窗展示） */
function filterDisplayName(name) {
    const keys = {"pulse": "ledPage.filterPulse", "breathing": "ledPage.filterBreathing", "blink": "ledPage.filterBlink", "wave": "ledPage.filterWave", "scanline": "ledPage.filterScanline", "glitch": "ledPage.filterGlitch", "rainbow": "ledPage.filterRainbow", "sparkle": "ledPage.filterSparkle", "plasma": "ledPage.filterPlasma", "sepia": "ledPage.filterSepia", "posterize": "ledPage.filterPosterize", "contrast": "ledPage.filterContrast", "invert": "ledPage.filterInvert", "grayscale": "ledPage.filterGrayscale"};
    return t(keys[name] || ({'fade-in':'promptRepair.filterFadeIn','fade-out':'promptRepair.filterFadeOut','color-shift':'promptRepair.filterColorShift'})[name] || 'promptRepair.unknownFilter');
}

function effectDisplayName(name) {
    if (!name) return '';
    return t('promptRepair.effect_' + (['rainbow','breathing','solid','sparkle','pulse','color_cycle','heartbeat','chase','comet','spin','breathe_wave','fire','rain','coderain','plasma','ripple'].includes(name) ? name : 'unknown'));
}

// 当前选中的动画
const selectedEffects = {};

// 支持颜色参数的动画
const colorSupportedEffects = ['breathing', 'solid', 'rain'];

// 选择动画（旧版兼容，保留）
function selectEffect(device, effect, btn) {
    selectedEffects[device] = effect;
    
    // 更新按钮状态
    const panel = btn.closest('.led-panel');
    panel.querySelectorAll('.effect-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
}

// 旧版 showEffectConfig 保持兼容
function showEffectConfig(device, effect) {
    selectedEffects[device] = effect;
}

async function applyEffect(device) {
    const effect = selectedEffects[device];
    if (!effect) {
        showToast((typeof t === 'function' ? t('toast.selectAnimation') : '请先选择一个动画'), 'warning');
        return;
    }
    
    const speed = parseInt(document.getElementById(`effect-speed-${device}`)?.value || '50');
    const color = document.getElementById(`effect-color-${device}`)?.value || '#ff0000';
    
    try {
        const params = { speed };
        // 只有支持颜色的动画才传递颜色参数
        if (colorSupportedEffects.includes(effect)) {
            params.color = color;
        }
        requireApiSuccess(await api.ledEffectStart(device, effect, params), 'ledEffectStart');
        
        // 更新状态为开启
        ledStates[device] = true;
        const btn = document.getElementById(`toggle-${device}`);
        if (btn) {
            btn.classList.add('on');
            const icon = btn.querySelector('.power-icon');
            if (icon) icon.innerHTML = '<svg class="i"><use href="#ri-sun-line"/></svg>';
        }
        
        // 更新顶部当前动画显示
        const currentAnim = document.getElementById(`current-anim-${device}`);
        if (currentAnim) currentAnim.textContent = `▶ ${effectDisplayName(effect)}`;
        
        showToast(typeof t === 'function' ? t('toast.ledEffectStarted', { device: getDeviceDescription(device), effect }) : `${device}: ${effect} 已启动`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledEffectStartFailed') + ': ' + e.message : `启动动画失败: ${e.message}`, 'error');
    }
}

function updateBrightnessLabel(device, value) {
    const label = document.getElementById(`brightness-val-${device}`);
    if (label) label.textContent = value;
}

async function setBrightness(device, value) {
    try {
        requireApiSuccess(await api.ledBrightness(device, parseInt(value)), 'ledBrightness');
        showToast(typeof t === 'function' ? t('toast.ledBrightnessSet', { device: getDeviceDescription(device), value }) : `${device} 亮度: ${value}`, 'success');
    } catch (e) { 
        showToast(typeof t === 'function' ? t('toast.ledBrightnessFailed', { device: getDeviceDescription(device) }) + ': ' + e.message : `设置 ${device} 亮度失败: ${e.message}`, 'error');
    }
}

// LED 开关状态记录
const ledStates = {};

async function toggleLed(device) {
    const isOn = ledStates[device] || false;
    
    try {
        if (isOn) {
            // 当前是开启状态，关闭它
            requireApiSuccess(await api.ledClear(device), 'ledClear');
            ledStates[device] = false;
            updateLedCardState(device, false);
            showToast(typeof t === 'function' ? t('toast.ledTurnedOff', { device: getDeviceDescription(device) }) : `${device} 已关闭`, 'success');
        } else {
            // 当前是关闭状态，开启它（白光）
            requireApiSuccess(await api.ledFill(device, '#ffffff'), 'ledFill');
            ledStates[device] = true;
            updateLedCardState(device, true, null);
            showToast(typeof t === 'function' ? t('toast.ledTurnedOn', { device: getDeviceDescription(device) }) : `${device} 已开启`, 'success');
        }
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.operationFailedMsg', { msg: e.message }) : `操作失败: ${e.message}`, 'error');
    }
}

async function ledOn(device, color = '#ffffff') {
    try {
        requireApiSuccess(await api.ledFill(device, color), 'ledFill');
        ledStates[device] = true;
        updateToggleButton(device, true);
        showToast(typeof t === 'function' ? t('toast.ledTurnedOn', { device: getDeviceDescription(device) }) : `${device} 已开启`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledOnFailed') + ': ' + e.message : `开启失败: ${e.message}`, 'error');
    }
}

async function fillColor(device) {
    const color = document.getElementById(`color-${device}`).value;
    try {
        requireApiSuccess(await api.ledFill(device, color), 'ledFill');
        // 更新状态为开启
        ledStates[device] = true;
        const btn = document.getElementById(`toggle-${device}`);
        if (btn) {
            btn.classList.add('on');
            btn.querySelector('.toggle-icon').innerHTML = '<svg class="i"><use href="#ri-checkbox-blank-circle-fill"/></svg>';
            btn.querySelector('.toggle-text').textContent = typeof t === 'function' ? t('ui.turnOffLight') : '关灯';
        }
        showToast(typeof t === 'function' ? t('toast.ledFilled', { device: getDeviceDescription(device), color }) : `${device} 已填充 ${color}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledFillFailed') + ': ' + e.message : `${device} 填充失败: ${e.message}`, 'error');
    }
}

async function quickFill(device, color) {
    document.getElementById(`color-${device}`).value = color;
    try {
        requireApiSuccess(await api.ledFill(device, color), 'ledFill');
        // 更新状态为开启
        ledStates[device] = true;
        const btn = document.getElementById(`toggle-${device}`);
        if (btn) {
            btn.classList.add('on');
            btn.querySelector('.toggle-icon').innerHTML = '<svg class="i"><use href="#ri-checkbox-blank-circle-fill"/></svg>';
            btn.querySelector('.toggle-text').textContent = typeof t === 'function' ? t('ui.turnOffLight') : '关灯';
        }
        showToast(typeof t === 'function' ? t('toast.ledFilled', { device: getDeviceDescription(device), color }) : `${device} → ${color}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledFillFailed') + ': ' + e.message : `填充失败: ${e.message}`, 'error');
    }
}

async function clearLed(device) {
    try {
        requireApiSuccess(await api.ledClear(device), 'ledClear');
        // 更新状态为关闭
        ledStates[device] = false;
        const btn = document.getElementById(`toggle-${device}`);
        if (btn) {
            btn.classList.remove('on');
            btn.querySelector('.toggle-icon').innerHTML = '<svg class="i"><use href="#ri-lightbulb-line"/></svg>';
            btn.querySelector('.toggle-text').textContent = typeof t === 'function' ? t('ui.turnOnLight') : '开灯';
        }
        showToast(typeof t === 'function' ? t('toast.ledTurnedOff', { device: getDeviceDescription(device) }) : `${device} 已关闭`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledOffFailed') + ': ' + e.message : `关闭失败: ${e.message}`, 'error');
    }
}

async function startEffect(device, effect) {
    try {
        requireApiSuccess(await api.ledEffectStart(device, effect), 'ledEffectStart');
        // 更新状态为开启
        ledStates[device] = true;
        const btn = document.getElementById(`toggle-${device}`);
        if (btn) {
            btn.classList.add('on');
            btn.querySelector('.toggle-icon').innerHTML = '<svg class="i"><use href="#ri-checkbox-blank-circle-fill"/></svg>';
            btn.querySelector('.toggle-text').textContent = typeof t === 'function' ? t('ui.turnOffLight') : '关灯';
        }
        showToast(typeof t === 'function' ? t('toast.ledEffectStarted', { device: getDeviceDescription(device), effect }) : `${device}: ${effect} 已启动`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledEffectStartFailed') + ': ' + e.message : `启动动画失败: ${e.message}`, 'error');
    }
}

async function stopEffect(device) {
    try {
        requireApiSuccess(await api.ledEffectStop(device), 'ledEffectStop');
        // 隐藏配置面板
        const controlsEl = document.getElementById(`effect-controls-${device}`);
        if (controlsEl) {
            controlsEl.style.display = 'none';
        }
        // 清除选中状态
        delete selectedEffects[device];
        showToast(typeof t === 'function' ? t('toast.ledEffectStopped', { device: getDeviceDescription(device) }) : `${device} 动画已停止`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledEffectStopFailed') + ': ' + e.message : `停止动画失败: ${e.message}`, 'error');
    }
}

async function saveLedConfig(device) {
    try {
        const response = requireApiSuccess(await api.call('led.save', {device}), 'led.save');
        const result = response.data || {};
        if (result.animation) {
            showToast(typeof t === 'function' ? t('toast.ledConfigSavedWithAnim', { device: getDeviceDescription(device), animation: effectDisplayName(result.animation) }) : `${device} 配置已保存: ${result.animation}`, 'success');
        } else {
            showToast(typeof t === 'function' ? t('toast.ledConfigSaved', { device: getDeviceDescription(device) }) : `${device} 配置已保存`, 'success');
        }
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledConfigSaveFailed') + ': ' + e.message : `保存配置失败: ${e.message}`, 'error');
    }
}

// =========================================================================
//                     Matrix 专属功能
// =========================================================================

// 文件选择器状态
let filePickerCurrentPath = '/sdcard/images';
let filePickerSelectedFile = null;
let filePickerCallback = null;

// 通用文件选择器 - 为指定输入框打开文件选择器
async function openFilePickerFor(inputId, startPath = '/sdcard/images') {
    filePickerCurrentPath = startPath;
    filePickerSelectedFile = null;
    filePickerCallback = (path) => {
        document.getElementById(inputId).value = path;
    };
    buildFilePicker();
    document.getElementById('file-picker-modal').classList.remove('hidden');
    await loadFilePickerDirectory(filePickerCurrentPath);
}

// 文件选择器弹窗外观：面包屑 + 目录列表；单击文件即选中并关闭，单击目录进入
function buildFilePicker() {
    document.getElementById('file-picker-modal').innerHTML = sheet(560, t('files.pickImageTitle'),
        `<div class="acts" style="align-items:center;margin-bottom:12px"><button class="btn icon sm" onclick="loadFilePickerDirectory('/sdcard')" title="${t('files.rootDir')}" aria-label="${t('files.rootDir')}"><svg class="i"><use href="#ri-home-line"/></svg></button><span id="file-picker-current-path" style="display:contents"></span></div>
        <div class="card" style="padding:0;background:var(--fill)" id="file-picker-list"></div>`,
        `<button class="btn lg" onclick="closeFilePicker()">${t('common.cancel')}</button>`);
}

// 浏览图像文件 - 打开文件选择器
async function browseImages() {
    filePickerCurrentPath = '/sdcard/images';
    filePickerSelectedFile = null;
    filePickerCallback = (path) => {
        // 优先填充模态框中的路径，否则填充旧版元素
        const modalInput = document.getElementById('modal-image-path');
        const oldInput = document.getElementById('matrix-image-path');
        if (modalInput) {
            modalInput.value = path;
        } else if (oldInput) {
            oldInput.value = path;
        }
    };
    buildFilePicker();
    document.getElementById('file-picker-modal').classList.remove('hidden');
    await loadFilePickerDirectory(filePickerCurrentPath);
}

// 加载文件选择器目录
async function loadFilePickerDirectory(path) {
    if (!document.getElementById('file-picker-list')) buildFilePicker();
    filePickerCurrentPath = path;
    const parts = path.split('/').filter(Boolean);
    document.getElementById('file-picker-current-path').innerHTML = parts.map((seg, i) => {
        const upto = '/' + parts.slice(0, i + 1).join('/');
        return `<span class="t-note">/</span>` + (i === parts.length - 1
            ? `<span class="t-body" style="font-weight:600">${escapeHtml(seg)}</span>`
            : `<span class="t-body" role="link" style="cursor:pointer" data-path="${escapeHtml(upto)}" onclick="loadFilePickerDirectory(this.dataset.path)">${escapeHtml(seg)}</span>`);
    }).join('');
    const listContainer = document.getElementById('file-picker-list');
    listContainer.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('common.loading') + '</div>';
    
    try {
        const result = await api.storageList(path);
        
        // 检查 API 返回的错误
        if (result.error) {
            // 目录不存在，尝试创建
            if (result.error.includes('not found') || result.error.includes('Directory')) {
                listContainer.innerHTML = `<div class="tr" style="--cols:1fr auto;color:var(--ink-3)"><span>${t('filePage.dirNotExist')}</span><button class="btn sm" data-path="${escapeHtml(path)}" onclick="createAndOpenDir(this.dataset.path)">${t('filePage.createDir')}</button></div>`;
                return;
            }
            throw new Error(result.error);
        }
        
        const files = result.data?.entries || [];
        
        // 筛选：只显示目录和图片文件
        const imageExts = ['.png', '.jpg', '.jpeg', '.gif', '.bmp'];
        const filtered = files.filter(f => {
            if (f.type === 'dir' || f.type === 'directory') return true;
            const ext = f.name.toLowerCase().substring(f.name.lastIndexOf('.'));
            return imageExts.includes(ext);
        });
        
        if (filtered.length === 0) {
            listContainer.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('filePage.noImages') + '</div>';
            return;
        }
        
        // 排序：目录在前，文件在后
        filtered.sort((a, b) => {
            const aIsDir = a.type === 'dir' || a.type === 'directory';
            const bIsDir = b.type === 'dir' || b.type === 'directory';
            if (aIsDir && !bIsDir) return -1;
            if (!aIsDir && bIsDir) return 1;
            return a.name.localeCompare(b.name);
        });
        
        listContainer.innerHTML = filtered.map(f => {
            const isDir = f.type === 'dir' || f.type === 'directory';
            const fullPath = path + (path.endsWith('/') ? '' : '/') + f.name;
            return `<div class="tr" style="--cols:24px 1fr 90px;cursor:pointer" data-path="${escapeHtml(fullPath)}" data-directory="${isDir}" onclick="filePickerItemClick(this, this.dataset.path, this.dataset.directory === 'true')"><div><svg class="i"><use href="#${isDir ? 'ri-folder-line' : 'ri-file-text-line'}"/></svg></div><div>${escapeHtml(f.name)}</div><div>${isDir ? '-' : formatFileSize(f.size)}</div></div>`;
        }).join('');
    } catch (e) {
        listContainer.innerHTML = `<div class="tr" style="--cols:1fr"><span class="state bad">${t('common.loadFailed')}: ${escapeHtml(e.message)}</span></div>`;
    }
}

// 创建并打开目录
async function createAndOpenDir(path) {
    try {
        requireApiSuccess(await api.storageMkdir(path), 'storageMkdir');
        await loadFilePickerDirectory(path);
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.createDirFailed', { msg: e.message }) : '创建目录失败: ' + e.message), 'error');
    }
}

// 文件选择器项目单击
function filePickerItemClick(element, path, isDir) {
    if (isDir) {
        loadFilePickerDirectory(path);
    } else {
        filePickerSelectedFile = path;
        confirmFilePicker();
    }
}

// 文件选择器项目双击
async function filePickerItemDblClick(path, isDir) {
    if (isDir) {
        await loadFilePickerDirectory(path);
    } else {
        // 双击文件直接确认
        filePickerSelectedFile = path;
        confirmFilePicker();
    }
}

// 文件选择器上级目录
async function filePickerGoUp() {
    if (filePickerCurrentPath === '/sdcard' || filePickerCurrentPath === '/') {
        return;
    }
    const parentPath = filePickerCurrentPath.substring(0, filePickerCurrentPath.lastIndexOf('/')) || '/sdcard';
    await loadFilePickerDirectory(parentPath);
}

// 关闭文件选择器
function closeFilePicker() {
    document.getElementById('file-picker-modal').classList.add('hidden');
    filePickerSelectedFile = null;
    filePickerCallback = null;
}

// 确认文件选择
function confirmFilePicker() {
    if (filePickerSelectedFile && filePickerCallback) {
        filePickerCallback(filePickerSelectedFile);
    }
    closeFilePicker();
}

// 显示图像
async function displayImage() {
    const pathInput = document.getElementById('matrix-image-path');
    const centerCheckbox = document.getElementById('matrix-image-center');
    
    const path = pathInput.value.trim();
    if (!path) {
        showToast((typeof t === 'function' ? t('toast.enterImagePath') : '请输入图像路径'), 'error');
        return;
    }
    
    try {
        const result = requireApiSuccess(await api.ledImage(path, 'matrix', centerCheckbox.checked), 'ledImage');
        showToast(typeof t === 'function' ? t('toast.imageDisplayed') : '图像显示成功', 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledImageFailed') + ': ' + e.message : `显示图像失败: ${e.message}`, 'error');
    }
}

// 生成 QR 码
async function generateQrCode() {
    const textInput = document.getElementById('matrix-qr-text');
    const eccSelect = document.getElementById('matrix-qr-ecc');
    const fgColor = document.getElementById('matrix-qr-fg');
    const bgImageInput = document.getElementById('matrix-qr-bg-image');
    
    const text = textInput.value.trim();
    if (!text) {
        showToast((typeof t === 'function' ? t('toast.enterQrContent') : '请输入 QR 码内容'), 'error');
        return;
    }
    
    const params = {
        ecc: eccSelect.value,
        color: fgColor.value
    };
    
    // 添加背景图（如果有）
    const bgImage = bgImageInput.value.trim();
    if (bgImage) {
        params.bg_image = bgImage;
    }
    
    try {
        const result = requireApiSuccess(await api.ledQrcode(text, params), 'ledQrcode');
        showToast(typeof t === 'function' ? t('toast.qrGenerated') : 'QR 码生成成功', 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledQrFailed') + ': ' + e.message : `生成 QR 码失败: ${e.message}`, 'error');
    }
}

// 清除 QR 码背景图
function clearQrBgImage() {
    document.getElementById('matrix-qr-bg-image').value = '';
}

// 加载字体列表
async function loadFontList() {
    const fontSelect = document.getElementById('matrix-text-font');
    if (!fontSelect) return;
    
    // 保存当前选中的字体
    const currentFont = fontSelect.value;
    
    try {
        const result = await api.storageList('/sdcard/fonts');
        const files = result.data?.entries || [];
        
        // 筛选字体文件 (.fnt, .bdf, .pcf)
        const fontExts = ['.fnt', '.bdf', '.pcf'];
        const fonts = files.filter(f => {
            if (f.type === 'dir' || f.type === 'directory') return false;
            const ext = f.name.toLowerCase().substring(f.name.lastIndexOf('.'));
            return fontExts.includes(ext);
        });
        
        // 清空选项
        fontSelect.innerHTML = '';
        
        if (fonts.length === 0) {
            // 没有字体时添加占位选项
            fontSelect.innerHTML = '<option value="" disabled>' + (typeof t === 'function' ? t('filePage.noFonts') : '无可用字体') + '</option>';
            showToast((typeof t === 'function' ? t('toast.fontNotFound') : '未找到字体文件，请上传到 /sdcard/fonts'), 'info');
        } else {
            fonts.forEach(f => {
                const option = document.createElement('option');
                // 使用文件名（不含扩展名）作为值和显示名
                // 后端会自动添加路径前缀和扩展名
                const fontName = f.name.substring(0, f.name.lastIndexOf('.'));
                option.value = fontName;
                option.textContent = fontName;
                fontSelect.appendChild(option);
            });
            
            // 恢复之前选中的字体
            if (currentFont && fontSelect.querySelector(`option[value="${currentFont}"]`)) {
                fontSelect.value = currentFont;
            }
        }
    } catch (e) {
        console.log('加载字体列表失败:', e);
        // 目录不存在时不报错，保持默认选项
    }
}

// 显示文本
async function displayText() {
    const textInput = document.getElementById('matrix-text-content');
    const fontSelect = document.getElementById('matrix-text-font');
    const alignSelect = document.getElementById('matrix-text-align');
    const colorInput = document.getElementById('matrix-text-color');
    const xInput = document.getElementById('matrix-text-x');
    const yInput = document.getElementById('matrix-text-y');
    const autoPos = document.getElementById('matrix-text-auto-pos');
    const scrollSelect = document.getElementById('matrix-text-scroll');
    const speedInput = document.getElementById('matrix-text-speed');
    const loopCheckbox = document.getElementById('matrix-text-loop');
    
    const text = textInput.value.trim();
    if (!text) {
        showToast((typeof t === 'function' ? t('toast.enterDisplayText') : '请输入显示文本'), 'error');
        return;
    }
    
    const params = {
        device: 'matrix',
        font: fontSelect.value,
        align: alignSelect.value,
        color: colorInput.value,
        scroll: scrollSelect.value,  // 滚动方向：none/left/right/up/down
        speed: parseInt(speedInput.value),
        loop: loopCheckbox.checked
    };
    
    // 添加坐标（如果不是自动定位）
    if (!autoPos.checked) {
        params.x = parseInt(xInput.value) || 0;
        params.y = parseInt(yInput.value) || 0;
    }
    
    try {
        const result = requireApiSuccess(await api.ledText(text, params), 'ledText');
        showToast(typeof t === 'function' ? t('toast.textDisplayed') : '文本显示成功', 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledTextFailed') + ': ' + e.message : `显示文本失败: ${e.message}`, 'error');
    }
}

// 停止文本
async function stopText() {
    try {
        requireApiSuccess(await api.ledTextStop('matrix'), 'ledTextStop');
        showToast((typeof t === 'function' ? t('toast.textStopped') : '文本已停止'), 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.ledTextStopFailed') + ': ' + e.message : `停止失败: ${e.message}`, 'error');
    }
}

// 滤镜配置：每个滤镜的参数列表和默认值
const filterConfig = {
    'pulse': { params: ['speed'], defaults: { speed: 50 } },
    'breathing': { params: ['speed'], defaults: { speed: 30 } },
    'blink': { params: ['speed'], defaults: { speed: 50 } },
    'wave': { params: ['speed', 'wavelength', 'amplitude', 'angle'], defaults: { speed: 40, wavelength: 8, amplitude: 128, angle: 0 } },
    'scanline': { params: ['speed', 'width', 'angle', 'intensity'], defaults: { speed: 60, width: 3, angle: 0, intensity: 150 } },
    'glitch': { params: ['intensity', 'frequency'], defaults: { intensity: 70, frequency: 30 } },
    'rainbow': { params: ['speed', 'saturation'], defaults: { speed: 50, saturation: 100 } },
    'sparkle': { params: ['speed', 'density', 'decay'], defaults: { speed: 5, density: 50, decay: 150 } },
    'plasma': { params: ['speed', 'scale'], defaults: { speed: 50, scale: 20 } },
    'posterize': { params: ['levels'], defaults: { levels: 4 } },
    'contrast': { params: ['amount'], defaults: { amount: 50 } },
    'fade-in': { params: ['speed'], defaults: { speed: 30 } },
    'fade-out': { params: ['speed'], defaults: { speed: 30 } },
    'color-shift': { params: ['speed'], defaults: { speed: 20 } },
    'invert': { params: [], defaults: {} },
    'grayscale': { params: [], defaults: {} },
    'sepia': { params: [], defaults: {} }
};

// 参数标签和范围定义（label 为中文兜底，显示时用 getParamLabel 做 i18n）
const paramLabels = {
    'speed': { get label() { return t('ledPage.speed'); }, min: 1, max: 100, unit: '', get help() { return t('ledPage.paramSpeedHelp'); } },
    'intensity': { get label() { return t('ledPage.paramIntensity'); }, min: 0, max: 255, unit: '', get help() { return t('ledPage.paramIntensityHelp'); } },
    'wavelength': { get label() { return t('ledPage.paramWavelength'); }, min: 1, max: 32, unit: 'px' },
    'amplitude': { get label() { return t('ledPage.paramAmplitude'); }, min: 0, max: 255, unit: '', get help() { return t('ledPage.paramAmplitudeHelp'); } },
    'direction': { get label() { return t('ledPage.paramDirection'); }, min: 0, max: 3, unit: '', get labels() { return ['horizontal','vertical','diagonalRight','diagonalLeft'].map(k => t('promptRepair.' + k)); } },
    'angle': { get label() { return t('ledPage.paramAngle'); }, min: 0, max: 360, unit: '°', get help() { return t('ledPage.paramAngleHelp'); } },
    'width': { get label() { return t('ledPage.paramWidth'); }, min: 1, max: 16, unit: 'px', get help() { return t('ledPage.paramWidthHelp'); } },
    'frequency': { get label() { return t('ledPage.paramFrequency'); }, min: 0, max: 100, unit: '%' },
    'saturation': { get label() { return t('ledPage.paramSaturation'); }, min: 0, max: 100, unit: '%' },
    'density': { get label() { return t('ledPage.paramDensity'); }, min: 0, max: 255, unit: '', get help() { return t('ledPage.paramDensityHelp'); } },
    'decay': { get label() { return t('ledPage.paramDecay'); }, min: 0, max: 255, unit: '', get help() { return t('ledPage.paramDecayHelp'); } },
    'scale': { get label() { return t('ledPage.paramScale'); }, min: 1, max: 100, unit: '' },
    'levels': { get label() { return t('ledPage.paramLevels'); }, min: 2, max: 16, unit: '' },
    'amount': { get label() { return t('ledPage.paramAmount'); }, min: 0, max: 100, unit: '%' }
};

let selectedFilter = null;

function getParamLabel(paramKey) {
    const keyMap = { speed: 'ledPage.paramSpeed', intensity: 'ledPage.paramIntensity', wavelength: 'ledPage.paramWavelength', amplitude: 'ledPage.paramAmplitude', direction: 'ledPage.paramDirection', angle: 'ledPage.paramAngle', width: 'ledPage.paramWidth', frequency: 'ledPage.paramFrequency', saturation: 'ledPage.paramSaturation', density: 'ledPage.paramDensity', decay: 'ledPage.paramDecay', scale: 'ledPage.paramScale', levels: 'ledPage.paramLevels', amount: 'ledPage.paramAmount' };
    const key = keyMap[paramKey];
    return (typeof t === 'function' && key) ? t(key) : (paramLabels[paramKey]?.label || paramKey);
}

// 选择滤镜
function selectFilter(filterName, btnElement) {
    selectedFilter = filterName;
    
    // 高亮当前选中的按钮
    document.querySelectorAll('.filter-btn').forEach(btn => btn.classList.remove('selected'));
    if (btnElement) btnElement.classList.add('selected');
    
    // 更新显示的滤镜名称
    const nameSpan = document.getElementById('selected-filter-name');
    if (nameSpan) nameSpan.textContent = typeof t === 'function' ? t('led.selectedFilter', { filter: filterDisplayName(filterName) }) : `已选择: ${filterName}`;
    
    // 启用应用按钮
    const applyBtn = document.getElementById('apply-filter-btn');
    if (applyBtn) applyBtn.disabled = false;
    
    // 动态生成参数控件
    const paramsDiv = document.getElementById('filter-params');
    const config = filterConfig[filterName];
    
    if (config && config.params && config.params.length > 0) {
        paramsDiv.style.display = 'block';
        paramsDiv.innerHTML = ''; // 清空现有控件
        
        config.params.forEach(param => {
            const paramInfo = paramLabels[param];
            const defaultValue = config.defaults[param] ?? 50;
            
            const row = document.createElement('div');
            row.className = 'config-row';
            row.style.cssText = 'display: flex; align-items: center; gap: 10px; margin-bottom: 10px;';
            
            const label = document.createElement('label');
            label.textContent = getParamLabel(param);
            label.style.minWidth = '60px';
            
            const slider = document.createElement('input');
            slider.type = 'range';
            slider.id = `filter-${param}`;
            slider.min = paramInfo.min;
            slider.max = paramInfo.max;
            slider.value = defaultValue;
            slider.style.flex = '1';
            
            const valueSpan = document.createElement('span');
            valueSpan.id = `filter-${param}-val`;
            valueSpan.textContent = defaultValue + paramInfo.unit;
            valueSpan.style.minWidth = '50px';
            valueSpan.style.textAlign = 'right';
            
            slider.oninput = () => {
                valueSpan.textContent = slider.value + paramInfo.unit;
            };
            
            row.appendChild(label);
            row.appendChild(slider);
            row.appendChild(valueSpan);
            paramsDiv.appendChild(row);
        });
    } else {
        paramsDiv.style.display = 'none';
        paramsDiv.innerHTML = '';
    }
}

// 应用选中的滤镜
async function applySelectedFilter() {
    if (!selectedFilter) {
        showToast((typeof t === 'function' ? t('toast.selectFilter') : '请先选择滤镜'), 'error');
        return;
    }
    
    // 收集所有参数
    const params = { device: 'matrix', filter: selectedFilter };
    const config = filterConfig[selectedFilter];
    
    if (config && config.params) {
        config.params.forEach(param => {
            const input = document.getElementById(`filter-${param}`);
            if (input) {
                let value = parseInt(input.value);
                // 根据参数类型转换值
                if (param === 'saturation' || param === 'frequency') {
                    value = Math.round(value * 2.55); // 0-100 转 0-255

                } else if (param === 'amount') {
                    value = value - 50; // 0-100 转 -50 to +50
                }
                params[param] = value;
            }
        });
    }
    
    try {
        requireApiSuccess(await api.call('led.filter.start', params), 'call');
        showToast(typeof t === 'function' ? t('toast.filterApplied', { filter: filterDisplayName(selectedFilter) }) : `已应用滤镜: ${selectedFilter}`, 'success');
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.filterApplyFailed') + ': ' + e.message : `应用滤镜失败: ${e.message}`, 'error');
    }
}

// 应用滤镜（兼容旧接口）
async function applyFilter(filterName, btnElement) {
    selectFilter(filterName, btnElement);
    await applySelectedFilter();
}

// 停止滤镜
async function stopFilter() {
    try {
        requireApiSuccess(await api.ledFilterStop('matrix'), 'ledFilterStop');
        showToast((typeof t === 'function' ? t('toast.filterStopped') : '滤镜已停止'), 'success');
        
        // 移除滤镜按钮高亮和选中状态
        document.querySelectorAll('.filter-btn').forEach(btn => {
            btn.classList.remove('active');
            btn.classList.remove('selected');
        });
        selectedFilter = null;
        
        // 重置 UI
        const nameSpan = document.getElementById('selected-filter-name');
        if (nameSpan) nameSpan.textContent = typeof t === 'function' ? t('ui.noFilterSelected') : '未选择滤镜';
        const applyBtn = document.getElementById('apply-filter-btn');
        if (applyBtn) applyBtn.disabled = true;
        const paramsDiv = document.getElementById('filter-params');
        if (paramsDiv) paramsDiv.style.display = 'none';
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.filterStopFailed') + ': ' + e.message : `停止滤镜失败: ${e.message}`, 'error');
    }
}

// =========================================================================
//                         网络页面
// =========================================================================

const NETWORK_LPMU_ACCESS_POLL_MS = 1500;
let networkLpmuAccessPollTimer = null;
let networkLpmuAccessRequesting = null;
let networkLpmuAccessSubmitting = null;
let networkLpmuAccessUncertain = false;
let networkLpmuAccessLastInfo = { stage: 'idle', running: false };

async function loadNetworkPage() {
    const pageCurrent = capturePageValidity();
    clearInterval(refreshInterval);
    stopServiceStatusRefresh();
    stopNetworkLpmuAccessPolling();
    
    // 取消系统页面的订阅
    
    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page page-network">
            <!-- 网络状态概览 -->
            <div class="net-tiles">
                <div class="tile" id="net-iface-eth">
                    <span class="tile-i"><svg class="i"><use href="#ri-network-line"/></svg></span>
                    <div class="tile-m"><div class="t-section">${t('network.ethernet')}</div><div><span class="state" id="eth-quick-status">-</span></div></div>
                    <span class="num t-value" id="eth-quick-ip">-</span>
                </div>
                <div class="tile" id="net-iface-wifi">
                    <span class="tile-i"><svg class="i"><use href="#ri-signal-wifi-3-line"/></svg></span>
                    <div class="tile-m"><div class="t-section">${t('network.wifiSta')}</div><div><span class="state" id="wifi-quick-status">-</span></div></div>
                    <span class="num t-value" id="wifi-quick-ip">-</span>
                </div>
                <div class="tile" id="net-iface-ap">
                    <span class="tile-i"><svg class="i"><use href="#ri-broadcast-line"/></svg></span>
                    <div class="tile-m"><div class="t-section">${t('network.wifiAp')}</div><div><span class="state" id="ap-quick-status">-</span></div></div>
                    <span class="num t-value" id="ap-quick-clients">-</span>
                </div>
            </div>
            
            <!-- 主要配置区域 -->
            <div class="net-grid">
                <!-- 左侧：接口配置 -->
                <div>
                    <div class="sec-h">
                        <span class="t-section sec-t">${t('network.interfaceConfig')}</span>
                        <div class="seg">
                            <button class="panel-tab on" onclick="switchNetTab('eth')">${t('network.ethernet')}</button>
                            <button class="panel-tab" onclick="switchNetTab('wifi')">${t('network.wifi')}</button>
                        </div>
                    </div>
                    
                    <!-- 以太网配置面板 -->
                    <div id="net-tab-eth">
                        <div class="card">
                            <div class="kv"><span>${t('network.linkStatus')}</span><span><span class="state" id="net-eth-link">-</span></span></div>
                            ${kvRow(t('network.ipAddress'), '-', 'net-eth-ip')}
                            ${kvRow(t('network.subnetMask'), '-', 'net-eth-netmask')}
                            ${kvRow(t('network.gateway'), '-', 'net-eth-gw')}
                            ${kvRow(t('network.dns'), '-', 'net-eth-dns')}
                            <div class="kv"><span>${t('network.mac')}</span><span><span class="mono" id="net-eth-mac">-</span></span></div>
                        </div>
                    </div>
                    
                    <!-- WiFi 配置面板 -->
                    <div class="hidden" id="net-tab-wifi">
                        <div class="card net-wifi">
                            <div class="between">
                                <span class="t-label">${t('network.mode')}</span>
                                <select id="wifi-mode-select" class="field" style="width:120px" onchange="setWifiMode()">
                                    <option value="off">${t('network.off')}</option>
                                    <option value="sta">${t('network.sta')}</option>
                                    <option value="ap">${t('network.ap')}</option>
                                    <option value="apsta">${t('network.apsta')}</option>
                                </select>
                            </div>

                            <!-- STA 信息 -->
                            <div id="wifi-sta-section">
                                <div class="t-label net-sub">${t('network.stationConnect')}</div>
                                <div class="kv"><span>${t('common.status')}</span><span><span class="state" id="net-wifi-sta-status">-</span></span></div>
                                ${kvRow(t('network.ssid'), '-', 'net-wifi-sta-ssid')}
                                ${kvRow('IP', '-', 'net-wifi-sta-ip')}
                                ${kvRow(t('network.signal'), '-', 'net-wifi-sta-rssi')}
                                <div class="acts end">
                                    <button class="btn sm" id="wifi-scan-btn" onclick="showWifiScan()"><svg class="i"><use href="#ri-scan-line"/></svg>${t('network.scan')}</button>
                                    <button class="btn sm hidden" id="wifi-disconnect-btn" onclick="disconnectWifi()">${t('network.disconnect')}</button>
                                </div>
                            </div>
                            <hr class="sep">
                            <!-- AP 信息 -->
                            <div id="wifi-ap-section">
                                <div class="between">
                                    <span class="t-label">${t('network.hotspot')}</span>
                                    <div class="acts">
                                        <button class="btn sm" id="ap-config-btn" onclick="showApConfig()">${t('network.config')}</button>
                                        <button class="btn sm" id="ap-stations-btn" onclick="showApStations()">${t('network.devices')}</button>
                                    </div>
                                </div>
                                <div class="kv"><span>${t('common.status')}</span><span><span class="state" id="net-wifi-ap-status">-</span></span></div>
                                ${kvRow(t('network.ssid'), '-', 'net-wifi-ap-ssid')}
                                ${kvRow('IP', '-', 'net-wifi-ap-ip')}
                                ${kvRow(t('network.clientCount'), '0', 'net-wifi-ap-sta-count')}
                            </div>
                        </div>
                    </div>
                </div>
                
                <!-- 右侧：服务配置 -->
                <div>
                    <div class="sec-h">
                        <span class="t-section sec-t">${t('network.networkServices')}</span>
                        <div class="acts"></div>
                    </div>
                    <div class="card svc">
                        <!-- 主机名 -->
                        <div style="padding-bottom:16px">
                            <div style="display:flex;justify-content:space-between;margin-bottom:8px"><span class="t-section">${t('network.hostname')}</span><span class="t-note" id="net-hostname">-</span></div>
                            <div class="acts">
                                <input type="text" id="hostname-input" class="field" style="flex:1" placeholder="${t('network.newHostname')}" aria-label="${t('network.newHostname')}">
                                <button class="btn" onclick="setHostname()">${t('network.set')}</button>
                            </div>
                        </div>
                        <hr class="sep">
                        <!-- DHCP 服务 -->
                        <div style="padding:16px 0">
                            <div class="between"><span class="t-section">${t('network.dhcpServer')}</span><span class="state" id="dhcp-badge">-</span></div>
                            <div class="between" style="margin-top:10px">
                                <div class="t-label" id="dhcp-interfaces-list"></div>
                                <button class="btn sm" onclick="showDhcpClients()">${t('network.clients')}</button>
                            </div>
                        </div>
                        <hr class="sep">
                        <!-- NAT -->
                        <div style="padding-top:16px">
                            <div class="between"><span class="t-section">${t('networkPage.natGateway')}</span><span class="state" id="nat-badge">-</span></div>
                            <div class="nat-row">
                                <span class="t-label">${t('system.wifi')} <b class="t-value" id="net-nat-wifi"><svg class="i"><use href="#ri-close-line"/></svg></b></span>
                                <span class="t-label">${t('system.ethernet')} <b class="t-value" id="net-nat-eth"><svg class="i"><use href="#ri-close-line"/></svg></b></span>
                                <span style="flex:1"></span>
                                <span class="t-label">${t('common.enable')}</span>
                                <button class="switch" id="nat-toggle-btn" role="switch" aria-checked="false" aria-label="${t('common.enable')}" onclick="toggleNat()"></button>
                                <button class="btn sm" onclick="saveNatConfig()">${t('common.save')}</button>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
            
            <!-- 接入上层网络 -->
            <div class="card net-lpmu" id="lpmu-access-section">
                <div>
                    <div class="t-section">${t('networkPage.lpmuAccessTitle')}</div>
                    <div id="network-lpmu-access-current" style="margin-top:4px"><span class="state">${t('networkPage.lpmuAccessIdle')}</span></div>
                    <div id="network-lpmu-access-summary" class="t-note" style="word-break:break-word"></div>
                </div>
                <button class="btn sm lpmu-access-btn" id="network-lpmu-access-btn" onclick="startNetworkLpmuAccess()">${t('networkPage.lpmuAccessBtn')}</button>
            </div>

            <!-- WiFi 扫描 -->
            <div class="modal hidden" id="wifi-scan-section">${sheet(560, t('networkPage.wifiScanTitle'), '<div class="card" style="padding:0;background:var(--fill)" id="wifi-scan-results"></div>',
                `<button class="btn lg" onclick="showWifiScan()"><svg class="i"><use href="#ri-refresh-line"/></svg>${t('common.refresh')}</button><button class="btn lg primary" onclick="hideWifiScan()">${t('common.close')}</button>`, 'hideWifiScan()')}</div>
            
            <!-- 热点接入设备 -->
            <div class="modal hidden" id="ap-stations-section">${sheet(560, t('networkPage.apStations'), `<div class="card" style="padding:0;background:var(--fill)"><div class="tr th cols-sta"><div>MAC</div><div>IP</div><div>RSSI</div></div><div id="ap-stations-results"></div></div>`,
                `<button class="btn lg primary" onclick="hideApStations()">${t('common.close')}</button>`, 'hideApStations()')}</div>
            
            <!-- DHCP 客户端 -->
            <div class="modal hidden" id="dhcp-clients-section">${sheet(560, t('networkPage.dhcpClients'), `
                <div style="display:flex;align-items:center;gap:8px;margin-bottom:12px"><span class="t-label">${t('networkPage.ifaceLabel')}</span><select id="dhcp-iface-select" class="field" style="width:140px" onchange="loadDhcpClients()"><option value="ap">WiFi AP</option><option value="eth">Ethernet</option></select><button class="btn icon sm" onclick="loadDhcpClients()" title="${t('common.refresh')}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button></div>
                <div class="card" style="padding:0;background:var(--fill)"><div class="tr th cols-dhcp"><div>MAC</div><div>IP</div><div>${t('networkPage.hostname')}</div></div><div id="dhcp-clients-results"></div></div>`,
                `<button class="btn lg primary" onclick="hideDhcpClients()">${t('common.close')}</button>`, 'hideDhcpClients()')}</div>
            
            <!-- WiFi 热点配置 -->
            <div class="modal hidden" id="ap-config-modal">${sheet(560, t('networkPage.apConfig'),
                grp(
                    row('SSID', inp('ap-ssid-input', 200, '', '', 'placeholder="TianshanOS" aria-label="SSID"')) +
                    row(t('network.password'), inp('ap-password-input', 200, t('networkPage.apPasswordHint'), '', 'type="password"'), '', t('networkPage.password')) +
                    row(t('networkPage.channel'), `<select id="ap-channel-input" class="field" style="width:90px"><option value="1">1</option><option value="6" selected>6</option><option value="11">11</option></select>`) +
                    row(t('networkPage.hideSSID'), swc('ap-hidden-input'))),
                `<button class="btn lg" onclick="hideApConfig()">${t('common.cancel')}</button><button class="btn lg primary" onclick="applyApConfig()">${t('common.apply')}</button>`)}</div>
        </div>
    `;
    
    await refreshNetworkPage();
    if (!pageCurrent()) return;
    await refreshNetworkLpmuAccessStatus(false);
    if (!pageCurrent()) return;
    if (!networkLpmuAccessUncertain && (networkLpmuAccessLastInfo?.running || networkLpmuAccessSubmitting)) {
        startNetworkLpmuAccessPolling();
    }
}

// 网络页面 Tab 切换
function switchNetTab(tab) {
    document.querySelectorAll('.panel-tab').forEach(t => t.classList.remove('on'));
    ['eth', 'wifi'].forEach(k => document.getElementById('net-tab-' + k).classList.add('hidden'));
    
    event.target.classList.add('on');
    document.getElementById('net-tab-' + tab).classList.remove('hidden');
}

async function refreshNetworkPage() {
    const pageCurrent = capturePageValidity();
    // 综合网络状态
    try {
        const status = await api.networkStatus();
        if (!pageCurrent()) return;
        if (status.data) {
            const data = status.data;
            
            const setState = (id, ok, text) => {
                const el = document.getElementById(id);
                el.className = ok ? 'state ok' : 'state';
                el.textContent = text;
            };
            const connText = (ok) => t(ok ? 'status.connected' : 'status.disconnected');

            // 主机名
            document.getElementById('net-hostname').textContent = data.hostname || '-';
            
            // 以太网
            const eth = data.ethernet || {};
            const ethConnected = eth.status === 'connected' || eth.link_up;
            updateIfaceStatus('net-iface-eth', ethConnected);
            setState('eth-quick-status', ethConnected, connText(ethConnected));
            document.getElementById('eth-quick-ip').textContent = eth.ip || '-';
            
            setState('net-eth-link', ethConnected, connText(ethConnected));
            document.getElementById('net-eth-ip').textContent = eth.ip || '-';
            document.getElementById('net-eth-netmask').textContent = eth.netmask || '-';
            document.getElementById('net-eth-gw').textContent = eth.gateway || '-';
            document.getElementById('net-eth-dns').textContent = eth.dns1 || '-';
            document.getElementById('net-eth-mac').textContent = eth.mac || '-';
            
            // WiFi STA
            const wifiSta = data.wifi_sta || {};
            const staConnected = wifiSta.connected || wifiSta.status === 'connected';
            updateIfaceStatus('net-iface-wifi', staConnected);
            setState('wifi-quick-status', staConnected, connText(staConnected));
            document.getElementById('wifi-quick-ip').textContent = wifiSta.ip || '-';
            
            setState('net-wifi-sta-status', staConnected, connText(staConnected));
            document.getElementById('net-wifi-sta-ssid').textContent = wifiSta.ssid || '-';
            document.getElementById('net-wifi-sta-ip').textContent = wifiSta.ip || '-';
            document.getElementById('net-wifi-sta-rssi').textContent = wifiSta.rssi ? `${wifiSta.rssi} dBm ${getSignalBars(wifiSta.rssi)}` : '-';
            
            // 根据连接状态显示/隐藏断开按钮
            document.getElementById('wifi-disconnect-btn').classList.toggle('hidden', !staConnected);
            
            // WiFi AP
            const wifiAp = data.wifi_ap || {};
            const apActive = wifiAp.status === 'connected' || wifiAp.active;
            const apClients = wifiAp.sta_count || 0;
            const apText = t(apActive ? 'status.running' : 'status.notEnabled');
            updateIfaceStatus('net-iface-ap', apActive);
            setState('ap-quick-status', apActive, apText);
            document.getElementById('ap-quick-clients').textContent = apActive ? t('networkPage.devicesCount', { count: apClients }) : '-';
            
            setState('net-wifi-ap-status', apActive, apText);
            document.getElementById('net-wifi-ap-ssid').textContent = wifiAp.ssid || '-';
            document.getElementById('net-wifi-ap-ip').textContent = wifiAp.ip || '-';
            document.getElementById('net-wifi-ap-sta-count').textContent = apClients;
        }
    } catch (e) {
        if (!pageCurrent()) return; console.log('Network status error:', e); }
    
    // WiFi 模式
    let currentWifiMode = 'off';
    try {
        const mode = await api.wifiMode();
        if (!pageCurrent()) return;
        if (mode.data) {
            currentWifiMode = mode.data.mode || 'off';
            document.getElementById('wifi-mode-select').value = currentWifiMode;
            
            // 根据模式显示/隐藏相关区域
            const staSection = document.getElementById('wifi-sta-section');
            const apSection = document.getElementById('wifi-ap-section');
            const scanBtn = document.getElementById('wifi-scan-btn');
            const apConfigBtn = document.getElementById('ap-config-btn');
            const apStationsBtn = document.getElementById('ap-stations-btn');
            
            const canSta = (currentWifiMode === 'sta' || currentWifiMode === 'apsta');
            const canAp = (currentWifiMode === 'ap' || currentWifiMode === 'apsta');
            
            staSection.style.display = canSta ? 'block' : 'none';
            apSection.style.display = canAp ? 'block' : 'none';
            
            scanBtn.disabled = !canSta;
            apConfigBtn.disabled = !canAp;
            apStationsBtn.disabled = !canAp;
        }
    } catch (e) {
        if (!pageCurrent()) return; console.log('WiFi mode error:', e); }
    
    // DHCP 状态
    try {
        const dhcp = await api.dhcpStatus();
        if (!pageCurrent()) return;
        if (dhcp.data) {
            const container = document.getElementById('dhcp-interfaces-list');
            const badge = document.getElementById('dhcp-badge');
            const setBadge = (ok, text) => { badge.className = ok ? 'state ok' : 'state'; badge.textContent = text; };
            if (dhcp.data.interfaces) {
                const runningCount = dhcp.data.interfaces.filter(i => i.running).length;
                setBadge(runningCount > 0, `${runningCount}/${dhcp.data.interfaces.length}`);
                
                container.innerHTML = dhcp.data.interfaces.map(iface => `
                    <div class="dhcp-iface-row">
                        <span class="dot ${iface.running ? 'ok' : ''}"></span>
                        <span>${iface.display_name || iface.interface}</span>
                        <span class="t-note">${t('networkPage.leasesCount', { n: iface.active_leases || 0 })}</span>
                    </div>
                `).join('');
            } else {
                setBadge(dhcp.data.running, t(dhcp.data.running ? 'status.running' : 'status.stopped'));
                container.textContent = t('networkPage.activeLeasesCount', { n: dhcp.data.active_leases || 0 });
            }
        }
    } catch (e) {
        if (!pageCurrent()) return; console.log('DHCP error:', e); }
    
    // NAT 状态
    try {
        const nat = await api.natStatus();
        if (!pageCurrent()) return;
        if (nat.data) {
            const enabled = nat.data.enabled;
            const wifiConnected = nat.data.wifi_connected;
            const ethUp = nat.data.eth_up;
            
            const badge = document.getElementById('nat-badge');
            badge.className = enabled ? 'state ok' : 'state';
            badge.textContent = t(enabled ? 'system.running' : 'system.stopped');
            
            const mark = (id, ok) => {
                const el = document.getElementById(id);
                el.innerHTML = '<svg class="i"><use href="#' + (ok ? 'ri-check-line' : 'ri-close-line') + '"/></svg>';
                el.style.color = ok ? 'var(--ok)' : 'var(--ink-3)';
            };
            mark('net-nat-wifi', wifiConnected);
            mark('net-nat-eth', ethUp);
            
            // NAT 开关
            const natSwitch = document.getElementById('nat-toggle-btn');
            natSwitch.classList.toggle('on', !!enabled);
            natSwitch.setAttribute('aria-checked', enabled ? 'true' : 'false');
            natSwitch.disabled = !(enabled || (wifiConnected && ethUp));
        }
    } catch (e) {
        if (!pageCurrent()) return; console.log('NAT error:', e); }
}

async function startNetworkLpmuAccess() {
    if (networkLpmuAccessSubmitting) return;
    if (networkLpmuAccessUncertain) {
        const pageCurrent = capturePageValidity();
        await refreshNetworkLpmuAccessStatus();
        if (!pageCurrent()) return;
        if (!networkLpmuAccessUncertain && networkLpmuAccessLastInfo.running) startNetworkLpmuAccessPolling();
        return;
    }
    if (networkLpmuAccessLastInfo?.running || document.getElementById('network-lpmu-password-modal')) return;
    const modal = document.createElement('div');
    modal.id = 'network-lpmu-password-modal';
    modal.className = 'modal';
    modal.setAttribute('role', 'dialog');
    modal.setAttribute('aria-modal', 'true');
    modal.setAttribute('aria-label', t('networkPage.lpmuAccessTitle'));
    const showLabel = t('securityPage.showPassword');
    modal.innerHTML = sheet(480, t('networkPage.lpmuAccessTitle'),
        `<div class="t-note" style="margin-bottom:12px">${t('networkPage.lpmuAccessPasswordHint')}</div>` +
        grp(row(t('networkPage.lpmuAccessSudoPassword'),
            `<div class="pwf"><input class="field" type="password" id="network-lpmu-sudo-password" autocomplete="off" required aria-label="${t('networkPage.lpmuAccessSudoPassword')}" style="width:200px"><button type="button" class="pwt" onclick="toggleAccountPasswordVisibility('network-lpmu-sudo-password', this)" title="${showLabel}" aria-label="${showLabel}"><svg class="i"><use href="#ri-eye-line"/></svg></button></div>`)),
        `<button type="button" class="btn lg" onclick="closeNetworkLpmuPasswordModal()">${t('common.cancel')}</button><button type="button" class="btn lg primary" onclick="submitNetworkLpmuAccess()">${t('networkPage.lpmuAccessBtn')}</button>`);
    modal.onclick = e => { if (e.target === modal) closeNetworkLpmuPasswordModal(); };
    modal.onkeydown = e => {
        if (e.key === 'Escape') { e.preventDefault(); closeNetworkLpmuPasswordModal(); }
        else if (e.key === 'Enter' && e.target.tagName === 'INPUT') { e.preventDefault(); submitNetworkLpmuAccess(); }
        else if (e.key === 'Tab') {
            const controls = [...modal.querySelectorAll('input, button')];
            const first = controls[0], last = controls.at(-1);
            if (e.shiftKey && e.target === first) { e.preventDefault(); last.focus(); }
            else if (!e.shiftKey && e.target === last) { e.preventDefault(); first.focus(); }
        }
    };
    document.body.appendChild(modal);
    const input = document.getElementById('network-lpmu-sudo-password');
    router.navigation?.onDispose(() => { input.value = ''; modal.remove(); });
    input.focus();
}

function closeNetworkLpmuPasswordModal() {
    const input = document.getElementById('network-lpmu-sudo-password');
    if (input) input.value = '';
    document.getElementById('network-lpmu-password-modal')?.remove();
    document.getElementById('network-lpmu-access-btn')?.focus();
}

function showNetworkLpmuAccessResult(info) {
    document.getElementById('network-lpmu-result-modal')?.remove();
    const modal = document.createElement('div');
    modal.id = 'network-lpmu-result-modal';
    modal.className = 'modal';
    modal.setAttribute('role', 'dialog');
    modal.setAttribute('aria-modal', 'true');
    modal.setAttribute('aria-label', t('networkPage.lpmuAccessTitle'));
    const close = () => { modal.remove(); document.getElementById('network-lpmu-access-btn')?.focus(); };
    modal.innerHTML = sheet(480, t('networkPage.lpmuAccessTitle'),
        `<div class="state ${info.success ? 'ok' : 'bad'}">${t(info.success ? 'networkPage.lpmuAccessSuccess' : 'networkPage.lpmuAccessFailed')}</div>` +
        `<div class="t-body" style="margin:8px 0 16px">${escapeHtml(info.summary).replace(/\n/g, '<br>')}</div>` +
        (info.success ? grp(row(t('networkPage.lpmuAccessInternet'), `<span class="state ok">${t('networkPage.lpmuAccessInternetConfirmed')}</span>`) +
            row(t('networkPage.lpmuAccessConfiguration'), `<span>${t('networkPage.lpmuAccessConfigurationDone')}</span>`)) : '') +
        `<details class="grp lpmu-log"><summary class="row lpmu-log-toggle"><span class="rl">${t('networkPage.lpmuAccessLog')}</span><svg class="i"><use href="#ri-arrow-down-line"/></svg></summary><div class="term logv lpmu-log-output" role="region" aria-label="${t('networkPage.lpmuAccessLog')}"><pre></pre></div></details>`,
        `<button type="button" class="btn lg primary">${t('networkPage.lpmuAccessResultDone')}</button>`);
    const details = modal.querySelector('details');
    let finished = false, loading = false, offset = 0, text = '';
    const technicalDetail = info.technicalError ? t('networkPage.lpmuAccessTechnicalDetail', {error: info.technicalError}) + '\n\n' : '';
    details.ontoggle = async () => {
        if (!modal.isConnected) return;
        modal.querySelector('.sheet').style.width = details.open ? '720px' : '480px';
        if (!details.open || finished || loading) return;
        loading = true;
        const output = modal.querySelector('pre');
        output.textContent = t('common.loading');
        try {
            for (;;) {
                const result = requireApiSuccess(await api.lpmuAccessLog(info.runId, offset), 'network.lpmu_access.log');
                if (!modal.isConnected) return;
                const data = result.data;
                if (!data || data.run_id !== info.runId || typeof data.output !== 'string' ||
                    typeof data.done !== 'boolean' || !Number.isSafeInteger(data.next_offset) ||
                    data.next_offset < offset || (!data.done && data.next_offset === offset)) {
                    throw new Error(t('networkPage.lpmuAccessLogInvalidPage'));
                }
                text += data.output;
                offset = data.next_offset;
                finished = data.done;
                if (finished || !details.open) {
                    output.textContent = technicalDetail + (text || t('networkPage.lpmuAccessLogEmpty'));
                    return;
                }
            }
        } catch (error) {
            finished = true; // Reopening must not automatically retry an invalid transcript.
            if (modal.isConnected) output.textContent = technicalDetail + t('networkPage.lpmuAccessLogFailed') + ': ' + error.message;
        } finally { loading = false; }
    };
    modal.querySelector('button').onclick = close;
    modal.onclick = e => { if (e.target === modal) close(); };
    modal.onkeydown = e => {
        if (e.key === 'Escape') { e.preventDefault(); close(); }
        else if (e.key === 'Tab') {
            const first = modal.querySelector('summary'), last = modal.querySelector('button');
            if (e.shiftKey && e.target === first) { e.preventDefault(); last.focus(); }
            else if (!e.shiftKey && e.target === last) { e.preventDefault(); first.focus(); }
        }
    };
    document.body.appendChild(modal);
    router.navigation?.onDispose(() => modal.remove());
    modal.querySelector('button').focus();
}

async function submitNetworkLpmuAccess() {
    if (networkLpmuAccessLastInfo?.running || networkLpmuAccessSubmitting || networkLpmuAccessUncertain) return;
    const pageCurrent = capturePageValidity();
    const input = document.getElementById('network-lpmu-sudo-password');
    if (!input) return;
    let password = input.value;
    if (!password || /[\r\n]/.test(password)) {
        fieldError(input, t('networkPage.lpmuAccessPasswordRequired'));
        return;
    }
    closeNetworkLpmuPasswordModal();
    stopNetworkLpmuAccessPolling();
    const submission = {};
    networkLpmuAccessSubmitting = submission;
    renderNetworkLpmuAccessStatus({ stage: 'check', status: 'running', running: true });

    try {
        const request = api.lpmuAccessStart(password);
        password = '';
        const result = requireApiSuccess(await request, 'network.lpmu_access.start');
        if (!pageCurrent()) return;
        networkLpmuAccessSubmitting = null;

        let data = result.data || {};
        if (data === null || typeof data !== 'object') {
            data = { summary: data };
        }
        if (!data.stage && !data.status && data.running !== false) {
            data = { ...data, stage: 'check', running: true };
        }

        renderNetworkLpmuAccessStatus(data);
        startNetworkLpmuAccessPolling();
        await refreshNetworkLpmuAccessStatus();
    } catch (e) {
        if (!pageCurrent()) return;
        networkLpmuAccessSubmitting = null;
        stopNetworkLpmuAccessPolling();
        const msg = e.message || (typeof t === 'function' ? t('common.unknown') : '未知错误');
        if (typeof e.code === 'number' && e.code !== 0 && !e.uncertain) {
            showToast(t('networkPage.lpmuAccessStartFailed') + ': ' + msg, 'error', 5000);
            // A rejected start can mean another task is already active. Read
            // its state rather than inventing a failed remote execution.
            await refreshNetworkLpmuAccessStatus(false);
            if (!pageCurrent()) return;
            if (!networkLpmuAccessUncertain && networkLpmuAccessLastInfo.running) startNetworkLpmuAccessPolling();
        } else {
            showNetworkLpmuAccessStatusUnknown(msg);
        }
    } finally {
        password = '';
        if (networkLpmuAccessSubmitting === submission) networkLpmuAccessSubmitting = null;
    }
}

function startNetworkLpmuAccessPolling() {
    stopNetworkLpmuAccessPolling();
    networkLpmuAccessPollTimer = setInterval(refreshNetworkLpmuAccessStatus, NETWORK_LPMU_ACCESS_POLL_MS);
}

function stopNetworkLpmuAccessPolling() {
    networkLpmuAccessRequesting = null;
    if (networkLpmuAccessPollTimer) {
        clearInterval(networkLpmuAccessPollTimer);
        networkLpmuAccessPollTimer = null;
    }
}

async function refreshNetworkLpmuAccessStatus(notifyResult = true) {
    const pageCurrent = capturePageValidity();
    if (networkLpmuAccessRequesting) return;
    if (!document.getElementById('network-lpmu-access-btn')) {
        stopNetworkLpmuAccessPolling();
        return;
    }

    const wasRunning = !!networkLpmuAccessLastInfo?.running;
    const request = { runId: networkLpmuAccessLastInfo.runId };
    networkLpmuAccessRequesting = request;
    const requestCurrent = () => pageCurrent() && networkLpmuAccessRequesting === request &&
        networkLpmuAccessLastInfo.runId === request.runId;

    try {
        const result = await api.lpmuAccessStatus();
        if (!requestCurrent()) return;
        if (!result || result.code !== 0) {
            throw new Error(result?.message || (typeof t === 'function' ? t('networkPage.lpmuAccessStatusFailed') : '状态获取失败'));
        }

        const info = renderNetworkLpmuAccessStatus(result.data || {});
        if (!info.running && !networkLpmuAccessSubmitting) {
            stopNetworkLpmuAccessPolling();
            if (notifyResult && wasRunning && request.runId === info.runId && (info.success || info.failed)) {
                showNetworkLpmuAccessResult(info);
            }
        }
    } catch (e) {
        if (!requestCurrent()) return;
        stopNetworkLpmuAccessPolling();
        const msg = e.message || (typeof t === 'function' ? t('common.unknown') : '未知错误');
        showNetworkLpmuAccessStatusUnknown(msg);
    } finally {
        if (networkLpmuAccessRequesting === request) networkLpmuAccessRequesting = null;
    }
}

function showNetworkLpmuAccessStatusUnknown(message) {
    networkLpmuAccessUncertain = true;
    const btn = document.getElementById('network-lpmu-access-btn');
    if (btn) {
        btn.disabled = !!networkLpmuAccessSubmitting;
        btn.textContent = t('networkPage.lpmuAccessRefreshStatus');
    }
    const current = document.getElementById('network-lpmu-access-current');
    if (current) current.innerHTML = '<span class="state">' + escapeHtml(t('networkPage.lpmuAccessStatusUnknown')) + '</span>';
    const summary = document.getElementById('network-lpmu-access-summary');
    if (summary) summary.textContent = message;
}

function renderNetworkLpmuAccessStatus(statusData = {}, fallbackMessage = '') {
    const info = normalizeNetworkLpmuAccessStatus(statusData, fallbackMessage);
    networkLpmuAccessLastInfo = info;
    networkLpmuAccessUncertain = false;

    const btn = document.getElementById('network-lpmu-access-btn');
    if (btn) {
        btn.disabled = !!info.running || !!networkLpmuAccessSubmitting;
        btn.innerHTML = info.running
            ? (typeof t === 'function' ? t('networkPage.lpmuAccessRunning') : '处理中')
            : (typeof t === 'function' ? t('networkPage.lpmuAccessBtn') : '通过LPMU接入');
    }

    const currentEl = document.getElementById('network-lpmu-access-current');
    if (currentEl) {
        const cls = info.failed ? ' bad' : (info.success ? ' ok' : '');
        const label = getNetworkLpmuAccessStageLabel(info.stage);
        const text = info.running ? (typeof t === 'function' ? t('networkPage.lpmuAccessRunning') : '处理中') + ': ' + label : label;
        currentEl.innerHTML = '<span class="state' + cls + '">' + escapeHtml(text) + '</span>';
    }

    const summaryEl = document.getElementById('network-lpmu-access-summary');
    if (summaryEl) {
        if (info.summary) {
            const label = info.failed
                ? (typeof t === 'function' ? t('networkPage.lpmuAccessError') : '错误')
                : (typeof t === 'function' ? t('networkPage.lpmuAccessOutput') : '输出');
            summaryEl.innerHTML = '<span' + (info.failed ? ' style="color:var(--bad)"' : '') + '>' + label + ': ' + escapeHtml(info.summary) + '</span>';
        } else {
            summaryEl.textContent = '';
        }
    }

    return info;
}

function normalizeNetworkLpmuAccessStatus(statusData = {}, fallbackMessage = '') {
    let data = statusData || {};
    if (data === null || typeof data !== 'object') {
        data = { summary: data };
    }

    const raw = String(data.stage || data.step || data.status || data.state || '').toLowerCase();
    let stage = normalizeNetworkLpmuAccessStage(raw, data);
    let running = data.running === true || data.busy === true;

    if (!running && ['check', 'upload', 'unpack', 'execute'].includes(stage)) {
        running = data.running !== false && data.done !== true;
    }
    const failed = !running && (stage === 'failed' || !!data.error || !!data.last_error);
    const success = !running && stage === 'success' && !failed;
    if (running && (stage === 'success' || stage === 'failed')) stage = 'execute';

    const errorText = data.error || data.last_error || data.stderr;
    const safeFallback = fallbackMessage && fallbackMessage !== 'OK' ? fallbackMessage : '';
    const outputText = data.summary || data.output_tail || data.output || data.stdout || data.message || safeFallback;
    const unconfirmed = failed && data.exit_code === 0 &&
        (data.internet_confirmed === false || data.configuration_complete === false);
    const primaryError = compactNetworkLpmuAccessText(errorText ||
        (Number.isInteger(data.exit_code) && data.exit_code >= 0 ? t('networkPage.lpmuAccessExitCode', {code: data.exit_code}) : ''));
    const diagnosis = compactNetworkLpmuAccessOutput(outputText || data.stderr);
    const scriptFailed = /^setup-smart-route failed \(exit=\d+\): remote script failed$/.test(primaryError);
    const scriptError = scriptFailed ? extractNetworkLpmuAccessScriptError(outputText) : '';
    const summary = success ? t('networkPage.lpmuAccessResultDescription') :
        unconfirmed ? t('networkPage.lpmuAccessUnconfirmed') : failed ?
        scriptFailed ? (scriptError || diagnosis || t('networkPage.lpmuAccessFailed')) :
        [primaryError, diagnosis && diagnosis !== primaryError ? t('networkPage.lpmuAccessLastOutput', {output: diagnosis}) : ''].filter(Boolean).join('\n') : '';

    return { stage, running, success, failed, summary, technicalError: failed ? primaryError : '', runId: data.run_id };
}

function extractNetworkLpmuAccessScriptError(value) {
    const lines = String(value || '').replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '').split(/\r?\n/);
    // The script explicitly labels its error; route tables printed afterward
    // are diagnostic context, not the reason for failure.
    for (let i = lines.length - 1; i >= 0; i--) {
        const match = lines[i].trim().match(/^✗\s*错误[：:]\s*(.+)$/);
        if (match) return match[1] === '没有找到可用的互联网连接' ?
            t('networkPage.lpmuAccessNoInternet') : compactNetworkLpmuAccessText(match[1]);
    }
    if (lines.some(line => /^sudo: \d+ incorrect password attempts?$/.test(line.trim()))) {
        return t('networkPage.lpmuAccessPasswordIncorrect');
    }
    return '';
}

function normalizeNetworkLpmuAccessStage(raw, data = {}) {
    if (!raw && data.running) return 'check';
    if (!raw) return 'idle';
    if (raw.includes('fail') || raw.includes('error')) return 'failed';
    if (raw.includes('success') || raw.includes('done') || raw.includes('complete')) return 'success';
    if (raw.includes('queue') || raw.includes('find') || raw.includes('load') || raw.includes('connect') || raw.includes('verify')) return 'check';
    if (raw.includes('upload')) return 'upload';
    if (raw.includes('unpack') || raw.includes('extract')) return 'unpack';
    if (raw.includes('chmod') || raw.includes('exec') || raw.includes('run')) return 'execute';
    if (raw.includes('check') || raw.includes('prepare') || raw.includes('verify')) return 'check';
    if (raw === 'pending') return data.running ? 'check' : 'idle';
    if (raw === 'idle') return 'idle';
    return raw;
}

function getNetworkLpmuAccessStageLabel(stage) {
    const labels = {
        idle: typeof t === 'function' ? t('networkPage.lpmuAccessIdle') : '未启动',
        check: typeof t === 'function' ? t('networkPage.lpmuAccessStageCheck') : '检查远端项目',
        upload: typeof t === 'function' ? t('networkPage.lpmuAccessStageUpload') : '上传项目包',
        unpack: typeof t === 'function' ? t('networkPage.lpmuAccessStageUnpack') : '解包项目',
        execute: typeof t === 'function' ? t('networkPage.lpmuAccessStageExecute') : '执行路由脚本',
        success: typeof t === 'function' ? t('networkPage.lpmuAccessSuccess') : '成功',
        failed: typeof t === 'function' ? t('networkPage.lpmuAccessFailed') : '失败'
    };
    return labels[stage] || stage || labels.idle;
}

function compactNetworkLpmuAccessText(value) {
    if (value === null || value === undefined) return '';
    const text = String(value).replace(/\s+/g, ' ').trim();
    if (text.length <= 160) return text;
    return text.slice(0, 157) + '...';
}

function compactNetworkLpmuAccessOutput(value) {
    if (value === null || value === undefined) return '';
    const text = String(value).replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '')
        .split(/\r?\n/).map(line => line.trim())
        .filter(line => line && !/^={3,}/.test(line) && !/^\$ /.test(line))
        .slice(-2).join(' ').replace(/\s+/g, ' ');
    return text.length <= 160 ? text : '…' + text.slice(-159);
}

// 更新接口状态样式
function updateIfaceStatus(elementId, isActive) {
    const el = document.getElementById(elementId);
    if (el) el.dataset.active = isActive ? '1' : '0';
}

function getSignalBars(rssi) {
    if (rssi >= -50) return '████';
    if (rssi >= -60) return '███░';
    if (rssi >= -70) return '██░░';
    if (rssi >= -80) return '█░░░';
    return '░░░░';
}

// WiFi 模式显示文本
function getWifiModeDisplay(mode) {
    const modeMap = {
        'off': t('common.close'),
        'sta': t('network.sta'),
        'ap': t('network.ap'),
        'apsta': 'STA+AP'
    };
    return modeMap[mode] || mode;
}

// 设置 WiFi 模式
async function setWifiMode() {
    const mode = document.getElementById('wifi-mode-select').value;
    try {
        requireApiSuccess(await api.wifiMode(mode), 'wifiMode');
        showToast(typeof t === 'function' ? t('toast.wifiModeChanged', { mode: getWifiModeDisplay(mode) }) : `WiFi 模式已切换为 ${getWifiModeDisplay(mode)}`, 'success');
        await refreshNetworkPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.switchFailedMsg', { msg: e.message }) : '切换失败: ' + e.message), 'error');
    }
}

// 设置主机名
async function setHostname() {
    const name = document.getElementById('hostname-input').value.trim();
    if (!name) {
        showToast((typeof t === 'function' ? t('toast.enterHostname') : '请输入主机名'), 'error');
        return;
    }
    try {
        requireApiSuccess(await api.hostname(name), 'hostname');
        showToast((typeof t === 'function' ? t('toast.hostnameSet') : '主机名已设置'), 'success');
        document.getElementById('hostname-input').value = '';
        await refreshNetworkPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.setFailedMsg', { msg: e.message }) : '设置失败: ' + e.message), 'error');
    }
}

async function showWifiScan() {
    const section = document.getElementById('wifi-scan-section');
    const container = document.getElementById('wifi-scan-results');
    
    section.classList.remove('hidden');
    container.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('networkPage.scanning') + '</div>';
    
    try {
        const result = await api.wifiScan();
        if (result.data && result.data.networks) {
            if (result.data.networks.length === 0) {
                container.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('networkPage.noNetwork') + '</div>';
                return;
            }
            // 按信号强度排序
            const networks = result.data.networks.sort((a, b) => b.rssi - a.rssi);
            container.innerHTML = networks.map(net => `
                <div class="tr cols-wifi" title="CH ${net.channel}">
                    <div>${escapeHtml(net.ssid) || t('promptRepair.hiddenNetwork')}${net.auth && net.auth !== 'OPEN' ? `<span class="tag" style="margin-left:6px">${escapeHtml(net.auth)}</span>` : ''}</div>
                    <div><span class="state${net.rssi >= -60 ? ' ok' : ''}">${net.rssi} dBm</span></div>
                    <div class="act"><button class="btn sm" onclick="connectWifi('${escapeHtml(net.ssid)}')">${t('networkPage.connect')}</button></div>
                </div>
            `).join('');
        }
    } catch (e) {
        const errorMsg = e.message || '';
        if (errorMsg.includes('STA') || errorMsg.includes('APSTA') || errorMsg.includes('mode')) {
            container.innerHTML = `<div class="tr" style="--cols:1fr"><span class="state bad">${t('networkPage.needStaMode')}</span></div>`;
        } else {
            container.innerHTML = `<div class="tr" style="--cols:1fr"><span class="state bad">${t('toast.scanFailed')}: ${escapeHtml(errorMsg)}</span></div>`;
        }
    }
}

function hideWifiScan() {
    document.getElementById('wifi-scan-section').classList.add('hidden');
}

function getSignalIcon(rssi) {
    return '<svg class="i"><use href="#ri-signal-wifi-3-line"/></svg>';
}

function escapeHtml(str) {
    return String(str ?? '').replace(/[&<>"']/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
}

// 必填校验（替代 alert）：字段边框变红，下方一行 12px 红字，焦点移到该字段；提示消失时机：用户修改该字段
function clearFieldErrors() {
    document.querySelectorAll('.fe-msg').forEach(e => e.remove());
    document.querySelectorAll('.err[aria-invalid]').forEach(e => { e.classList.remove('err'); e.removeAttribute('aria-invalid'); });
}

function readNumericInput(id, integer = false) {
    const el = document.getElementById(id);
    const raw = el?.value?.trim() ?? '';
    const value = Number(raw);
    if (!raw || !Number.isFinite(value) || (integer && !Number.isInteger(value)) ||
        (el.min !== '' && el.min !== undefined && value < Number(el.min)) ||
        (el.max !== '' && el.max !== undefined && value > Number(el.max))) {
        fieldError(id, t('inputRepair.invalidNumber'));
        throw new RangeError('Invalid numeric input: ' + id);
    }
    return value;
}

function fieldError(id, message) {
    clearFieldErrors();
    const el = typeof id === 'string' ? document.getElementById(id) : id;
    if (!el) { showToast(message, 'error'); return; }
    const msg = document.createElement('small');
    msg.className = 'form-error fe-msg';
    msg.setAttribute('role', 'alert');
    msg.textContent = message;
    const rl = el.closest('.row')?.querySelector('.rl');
    const fl = el.closest('.fl');
    if (rl) { msg.style.cssText = 'display:block;margin:0'; rl.appendChild(msg); }
    else if (fl) { msg.style.marginTop = '0'; fl.appendChild(msg); }
    else el.after(msg);
    el.classList.add('err');
    el.setAttribute('aria-invalid', 'true');
    const clear = () => { msg.remove(); el.classList.remove('err'); el.removeAttribute('aria-invalid'); };
    el.addEventListener('input', clear, { once: true });
    el.addEventListener('change', clear, { once: true });
    if (el.focus && !el.classList.contains('grp')) el.focus();
}

function connectWifi(ssid) {
    document.getElementById('wifi-connect-modal')?.remove();
    const modal = document.createElement('div');
    modal.id = 'wifi-connect-modal';
    modal.className = 'modal';
    modal.onclick = e => { if (e.target === modal) modal.remove(); };
    const showPasswordLabel = t('securityPage.showPassword');
    modal.innerHTML = sheet(480, t('ui.wifiConnectTitle', { ssid: escapeHtml(ssid) }),
        `<div class="fl"><label>${t('network.password')}</label><div class="pwf"><input class="field" type="password" id="wifi-connect-password" autocomplete="off" placeholder="${t('ui.wifiPasswordPh')}" aria-label="${t('ui.wifiPasswordPh')}" style="width:100%" onkeydown="if(event.key==='Enter'){submitWifiConnect(this.dataset.ssid)}" data-ssid="${escapeHtml(ssid)}"><button type="button" class="pwt" onclick="toggleAccountPasswordVisibility('wifi-connect-password', this)" title="${showPasswordLabel}" aria-label="${showPasswordLabel}"><svg class="i"><use href="#ri-eye-line"/></svg></button></div></div>`,
        `<button type="button" class="btn lg" onclick="document.getElementById('wifi-connect-modal').remove()">${t('common.cancel')}</button><button type="button" class="btn lg primary" data-ssid="${escapeHtml(ssid)}" onclick="submitWifiConnect(this.dataset.ssid)">${t('networkPage.connect')}</button>`);
    document.body.appendChild(modal);
    document.getElementById('wifi-connect-password').focus();
}

// 开放网络可留空密码
function submitWifiConnect(ssid) {
    const password = document.getElementById('wifi-connect-password')?.value ?? '';
    document.getElementById('wifi-connect-modal')?.remove();
    api.wifiConnect(ssid, password)
        .then((result) => {
            requireApiSuccess(result, 'wifi.connect');
            showToast((typeof t === 'function' ? t('toast.connecting') : '正在连接...'), 'info');
            setTimeout(refreshNetworkPage, 3000);
        })
        .catch(e => showToast((typeof t === 'function' ? t('toast.connectFailedMsg', { msg: e.message }) : '连接失败: ' + e.message), 'error'));
}

async function disconnectWifi() {
    try {
        requireApiSuccess(await api.wifiDisconnect(), 'wifiDisconnect');
        showToast((typeof t === 'function' ? t('toast.wifiDisconnected') : '已断开 WiFi 连接'), 'success');
        await refreshNetworkPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.disconnectFailedMsg', { msg: e.message }) : '断开失败: ' + e.message), 'error');
    }
}

// AP 接入设备
async function showApStations() {
    const section = document.getElementById('ap-stations-section');
    const container = document.getElementById('ap-stations-results');
    
    section.classList.remove('hidden');
    container.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('common.loading') + '</div>';
    
    try {
        const result = await api.wifiApStations();
        if (result.data && result.data.stations) {
            if (result.data.stations.length === 0) {
                container.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('networkPage.noDevice') + '</div>';
                return;
            }
            container.innerHTML = result.data.stations.map(sta => `
                <div class="tr cols-sta"><div class="mono">${escapeHtml(sta.mac)}</div><div>${escapeHtml(sta.ip || '-')}</div><div>${sta.rssi} dBm</div></div>
            `).join('');
        }
    } catch (e) {
        container.innerHTML = `<div class="tr" style="--cols:1fr"><span class="state bad">${t('toast.fetchFailed')}: ${escapeHtml(e.message)}</span></div>`;
    }
}

function hideApStations() {
    document.getElementById('ap-stations-section').classList.add('hidden');
}

// AP 配置
function showApConfig() {
    document.getElementById('ap-config-modal').classList.remove('hidden');
}

function hideApConfig() {
    document.getElementById('ap-config-modal').classList.add('hidden');
}

async function applyApConfig() {
    const ssid = document.getElementById('ap-ssid-input').value.trim();
    const password = document.getElementById('ap-password-input').value;
    const channel = parseInt(document.getElementById('ap-channel-input').value);
    const hidden = document.getElementById('ap-hidden-input').checked;
    
    if (!ssid) {
        showToast((typeof t === 'function' ? t('toast.ssidRequired') : '请输入 SSID'), 'error');
        return;
    }
    
    if (password && password.length < 8) {
        showToast((typeof t === 'function' ? t('toast.passwordShort') : '密码至少 8 位'), 'error');
        return;
    }
    
    try {
        requireApiSuccess(await api.wifiApConfig(ssid, password, channel, hidden), 'wifiApConfig');
        showToast((typeof t === 'function' ? t('toast.hotspotApplied') : '热点配置已应用'), 'success');
        hideApConfig();
        await refreshNetworkPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.configFailedMsg', { msg: e.message }) : '配置失败: ' + e.message), 'error');
    }
}

// DHCP 客户端
function showDhcpClients() {
    document.getElementById('dhcp-clients-section').classList.remove('hidden');
    loadDhcpClients();
}

function hideDhcpClients() {
    document.getElementById('dhcp-clients-section').classList.add('hidden');
}

async function loadDhcpClients() {
    const pageCurrent = capturePageValidity();
    const iface = document.getElementById('dhcp-iface-select').value;
    const container = document.getElementById('dhcp-clients-results');
    
    container.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('common.loading') + '</div>';
    
    try {
        const result = await api.dhcpClients(iface);
        if (!pageCurrent()) return;
        if (result.data && result.data.clients) {
            if (result.data.clients.length === 0) {
                container.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('networkPage.noClientBrief') + '</div>';
                return;
            }
            container.innerHTML = result.data.clients.map(client => `
                <div class="tr cols-dhcp"><div class="mono">${escapeHtml(client.mac)}</div><div>${escapeHtml(client.ip)}<span class="tag">${client.is_static ? t('networkPage.static') : t('networkPage.dynamic')}</span></div><div>${escapeHtml(client.hostname || '-')}</div></div>
            `).join('');
        }
    } catch (e) {
        if (!pageCurrent()) return;
        container.innerHTML = `<div class="tr" style="--cols:1fr"><span class="state bad">${t('toast.fetchFailed')}: ${escapeHtml(e.message)}</span></div>`;
    }
}

async function toggleNat() {
    try {
        const status = await api.natStatus();
        if (status.data?.enabled) {
            requireApiSuccess(await api.natDisable(), 'natDisable');
            showToast(typeof t === 'function' ? t('toast.natDisabled') : 'NAT 已禁用', 'success');
        } else {
            requireApiSuccess(await api.natEnable(), 'natEnable');
            showToast(typeof t === 'function' ? t('toast.natEnabled') : 'NAT 已启用', 'success');
        }
        await refreshNetworkPage();
    } catch (e) { 
        showToast((typeof t === 'function' ? t('toast.operationFailedMsg', { msg: e.message }) : '操作失败: ' + e.message), 'error'); 
    }
}

async function saveNatConfig() {
    try {
        requireApiSuccess(await api.natSave(), 'natSave');
        showToast(typeof t === 'function' ? t('toast.natConfigSaved') : 'NAT 配置已保存', 'success');
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.saveFailed') : '保存失败') + ': ' + e.message, 'error');
    }
}

// =========================================================================
//                         文件管理页面
// =========================================================================

let currentFilePath = '/sdcard';

async function loadFilesPage() {
    const pageCurrent = capturePageValidity();
    clearInterval(refreshInterval);
    
    // 取消系统页面的订阅
    
    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page page-files">
            <div class="between">
                <div class="acts" id="breadcrumb"></div>
                <div class="acts">
                    <button class="btn sm" onclick="showUploadDialog()"><svg class="i"><use href="#ri-upload-line"/></svg>${t('files.uploadFiles')}</button>
                    <button class="btn sm" onclick="showNewFolderDialog()"><svg class="i"><use href="#ri-folder-add-line"/></svg>${t('files.newFolder')}</button>
                    <button type="button" class="btn icon sm files-refresh-btn" onclick="refreshFilesPage()" title="${t('common.refresh')}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                </div>
            </div>

            <div class="between">
                <div class="seg storage-tabs">
                    <button class="tab-btn on" onclick="navigateToPath('/sdcard')">${t('files.sdcard')}</button>
                    <button class="tab-btn" onclick="navigateToPath('/spiffs')">${t('files.spiffs')}</button>
                </div>
                <div id="storage-controls"></div>
            </div>

            <div class="card file-list" id="file-list" style="padding:0;overflow:hidden">
                <div class="loading">${t('common.loading')}</div>
            </div>

            <!-- 批量操作工具栏 -->
            <div class="m-float batch-toolbar hidden" id="batch-toolbar">
                <span class="t-value" id="selected-count">${t('files.selectedBrief', { n: 0 })}</span>
                <span class="vdiv"></span>
                <button class="btn sm" onclick="batchDownload()"><svg class="i"><use href="#ri-download-line"/></svg>${t('files.batchDownload')}</button>
                <button class="btn sm dg" onclick="batchDelete()"><svg class="i"><use href="#ri-delete-bin-line"/></svg>${t('files.batchDelete')}</button>
                <button class="btn sm quiet" onclick="clearSelection()">${t('files.clearSelection')}</button>
            </div>

            <!-- 存储状态 -->
            <div class="stor-st" id="storage-status"></div>
        </div>
        
        <!-- 上传对话框 -->
        <div id="upload-modal" class="modal hidden">${sheet(520, t('files.uploadTitle'), `
            <div class="dz" id="upload-area"><div><svg class="i"><use href="#ri-upload-line"/></svg><div class="t-label" style="margin-top:6px">${t('files.pickOrDrop')}</div></div><input type="file" id="file-input" multiple style="display:none" onchange="handleFileSelect(event)"></div>
            <div class="grp" id="upload-list" style="margin-top:12px"></div>`,
            `<button class="btn lg" onclick="closeUploadDialog()">${t('common.cancel')}</button><button class="btn lg primary" onclick="uploadFiles()">${t('common.upload')}</button>`)}</div>

        <!-- 新建文件夹对话框 -->
        <div id="newfolder-modal" class="modal hidden">${sheet(460, t('files.newFolderTitle'),
            grp(row(t('common.name'), inp('new-folder-name', 220, t('files.folderNamePlaceholder')))),
            `<button class="btn lg" onclick="closeNewFolderDialog()">${t('common.cancel')}</button><button class="btn lg primary" onclick="createNewFolder()">${t('files.create')}</button>`)}</div>
        
        <!-- 重命名对话框 -->
        <div id="rename-modal" class="modal hidden">${sheet(460, t('files.renameTitle'),
            grp(row(t('files.newName'), inp('rename-input', 220, t('files.newNamePlaceholder')))) + '<input type="hidden" id="rename-original-path">',
            `<button class="btn lg" onclick="closeRenameDialog()">${t('common.cancel')}</button><button class="btn lg primary" onclick="doRename()">${t('common.confirm')}</button>`)}</div>
    `;
    
    // 设置拖拽上传
    setupDragAndDrop();
    
    // 初始化选择状态
    selectedFiles.clear();
    
    await refreshFilesPage();
    if (!pageCurrent()) return;
}

// 批量选择相关
const selectedFiles = new Set();

function updateSelectionUI() {
    const toolbar = document.getElementById('batch-toolbar');
    const countSpan = document.getElementById('selected-count');
    
    if (selectedFiles.size > 0) {
        toolbar.classList.remove('hidden');
        countSpan.textContent = t('files.selectedBrief', { n: selectedFiles.size });
    } else {
        toolbar.classList.add('hidden');
    }
    
    // 更新全选复选框状态
    const selectAllCb = document.getElementById('select-all-cb');
    const allCheckboxes = document.querySelectorAll('.file-checkbox');
    if (selectAllCb && allCheckboxes.length > 0) {
        const checkedCount = document.querySelectorAll('.file-checkbox:checked').length;
        selectAllCb.checked = checkedCount === allCheckboxes.length;
        selectAllCb.indeterminate = checkedCount > 0 && checkedCount < allCheckboxes.length;
    }
}

function toggleFileSelection(path, checkbox) {
    if (checkbox.checked) {
        selectedFiles.add(path);
    } else {
        selectedFiles.delete(path);
    }
    updateSelectionUI();
}

function toggleSelectAll(selectAllCb) {
    const checkboxes = document.querySelectorAll('.file-checkbox');
    checkboxes.forEach(cb => {
        cb.checked = selectAllCb.checked;
        const path = cb.dataset.path;
        if (selectAllCb.checked) {
            selectedFiles.add(path);
        } else {
            selectedFiles.delete(path);
        }
    });
    updateSelectionUI();
}

function clearSelection() {
    selectedFiles.clear();
    document.querySelectorAll('.file-checkbox').forEach(cb => cb.checked = false);
    const selectAllCb = document.getElementById('select-all-cb');
    if (selectAllCb) selectAllCb.checked = false;
    updateSelectionUI();
}

function showOperationSummary(key, success, failures, extra = {}) {
    const message = t(key, {success, fail: failures.length, ...extra});
    showToast(message, failures.length ? (success ? 'warning' : 'error') : extra.invalid ? 'warning' : 'success', 7000);
    let panel = document.getElementById('operation-summary');
    if (!panel) {
        panel = document.createElement('div');
        panel.id = 'operation-summary';
        panel.setAttribute('role', 'status');
        document.querySelector('main').appendChild(panel);
    }
    panel.style.whiteSpace = 'pre-wrap';
    panel.textContent = message + (failures.length ? '\n' + failures.map(f => f.path + ': ' + f.message).join('\n') : '');
    return {success, failures};
}

async function batchDelete() {
    if (selectedFiles.size === 0) {
        showToast((typeof t === 'function' ? t('toast.selectFileToDelete') : '请先选择要删除的文件'), 'warning');
        return;
    }
    
    const count = selectedFiles.size;
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteFiles', { count }) : `确定要删除选中的 ${count} 个文件/文件夹吗？此操作不可撤销！`, { primary: t('common.delete'), tone: 'danger' })) {
        return;
    }
    
    showToast(typeof t === 'function' ? t('toast.deletingItems', { count }) : `正在删除 ${count} 个项目...`, 'info');
    
    let successCount = 0;
    const failures = [];
    
    for (const path of selectedFiles) {
        try {
            requireApiSuccess(await api.storageDelete(path), 'storageDelete');
            successCount++;
        } catch (e) {
            console.error('Delete failed:', path, e);
            failures.push({path, message: e.message});
        }
    }
    
    selectedFiles.clear();
    await refreshFilesPage();
    return showOperationSummary('promptRepair.deleteSummary', successCount, failures);
}

async function batchDownload() {
    if (selectedFiles.size === 0) {
        showToast((typeof t === 'function' ? t('toast.selectFileToDownload') : '请先选择要下载的文件'), 'warning');
        return;
    }
    
    // 过滤出文件（排除文件夹）
    const filesToDownload = [];
    for (const path of selectedFiles) {
        const row = document.querySelector(`.file-row[data-path="${CSS.escape(path)}"]`);
        if (row && row.dataset.type !== 'dir') {
            filesToDownload.push(path);
        }
    }
    
    if (filesToDownload.length === 0) {
        showToast((typeof t === 'function' ? t('toast.noDownloadableFiles') : '选中的项目中没有可下载的文件（文件夹不支持下载）'), 'warning');
        return;
    }
    
    showToast(typeof t === 'function' ? t('toast.downloadingFiles', { count: filesToDownload.length }) : `正在下载 ${filesToDownload.length} 个文件...`, 'info');
    
    let success = 0;
    const failures = [];
    for (const path of filesToDownload) {
        const result = await downloadFile(path, true);
        if (result.started) success++;
        else failures.push({path, message: result.error.message});
    }
    return showOperationSummary('promptRepair.downloadSummary', success, failures);
}

// SD 卡挂载/卸载
async function mountSdCard() {
    try {
        showToast(typeof t === 'function' ? t('filePage.mountingSd') : '正在挂载 SD 卡...', 'info');
        requireApiSuccess(await api.storageMount(), 'storageMount');
        showToast(typeof t === 'function' ? t('filePage.mountSdSuccess') : 'SD 卡挂载成功', 'success');
        await refreshFilesPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('filePage.mountSdFailed') : '挂载失败') + ': ' + e.message, 'error');
    }
}

async function unmountSdCard() {
    if (!await confirmAction(typeof t === 'function' ? t('filePage.confirmUnmountSd') : '确定要卸载 SD 卡吗？\n\n卸载后将无法访问 SD 卡上的文件。', { primary: t('ui.unmount'), tone: 'neutral' })) {
        return;
    }
    
    try {
        showToast(typeof t === 'function' ? t('filePage.unmountingSd') : '正在卸载 SD 卡...', 'info');
        requireApiSuccess(await api.storageUnmount(), 'storageUnmount');
        showToast(typeof t === 'function' ? t('filePage.unmountSdSuccess') : 'SD 卡已卸载', 'success');
        if (currentFilePath.startsWith('/sdcard')) {
            currentFilePath = '/spiffs';
        }
        await refreshFilesPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('filePage.unmountSdFailed') : '卸载失败') + ': ' + e.message, 'error');
    }
}

async function refreshFilesPage() {
    const pageCurrent = capturePageValidity();
    const result = await loadDirectory(currentFilePath);
    if (!pageCurrent()) return;
    if (result?.stale) return result;
    await loadStorageStatus();
    if (!pageCurrent()) return;
    return result;
}

let directoryGeneration = 0;
async function loadDirectory(path) {
    const generation = ++directoryGeneration;
    currentFilePath = path;
    const listContainer = document.getElementById('file-list');
    
    const current = () => generation === directoryGeneration && currentFilePath === path && document.getElementById('file-list') === listContainer;
    if (!listContainer) return {stale: true};
    // 移除旧的事件监听器
    listContainer.removeEventListener('click', handleFileListClick);
    
    console.log('Loading directory:', path);
    
    // 如果是 SD 卡路径，先检查挂载状态，避免不必要的错误请求
    if (path.startsWith('/sdcard')) {
        try {
            const status = requireApiSuccess(await api.storageStatus(), 'storage.status');
            if (!current()) return {stale: true};
            if (!status.data?.sd?.mounted) {
                console.log('SD card not mounted, showing mount prompt');
                listContainer.innerHTML = `
                    <div class="empty unmounted-notice">
                        <p class="t-body">${typeof t === 'function' ? t('filePage.sdCardNotMounted') : 'SD 卡未挂载'}</p>
                        <button class="btn primary" onclick="mountSdCard()">${typeof t === 'function' ? t('filePage.mountSdCard') : '挂载 SD 卡'}</button>
                    </div>
                `;
                updateBreadcrumb(path);
                return;
            }
        } catch (e) {
            if (!current()) return {stale: true};
            console.warn('Failed to check storage status:', e.message);
            // 继续尝试加载目录，让后续逻辑处理错误
        }
    }
    
    try {
        const result = requireApiSuccess(await api.storageList(path), 'storage.list');
        if (!current()) return {stale: true};
        console.log('storageList result:', result);
        const entries = result.data?.entries || [];
        
        // 更新面包屑
        updateBreadcrumb(path);
        
        // 更新存储标签页
        document.querySelectorAll('.storage-tabs .tab-btn').forEach(btn => {
            btn.classList.toggle('on', (path.startsWith('/sdcard') && btn.textContent.includes('SD')) || (path.startsWith('/spiffs') && btn.textContent.includes('SPIFFS')));
        });
        
        if (entries.length === 0) {
            listContainer.innerHTML = '<div class="empty"><p class="t-note">' + (typeof t === 'function' ? t('filePage.emptyFolder') : '空文件夹') + '</p></div>';
            // 仍然添加事件监听器（虽然没有文件）
            listContainer.addEventListener('click', handleFileListClick);
            return;
        }
        
        // 排序：目录在前，文件在后，按名称排序
        entries.sort((a, b) => {
            if (a.type === 'dir' && b.type !== 'dir') return -1;
            if (a.type !== 'dir' && b.type === 'dir') return 1;
            return a.name.localeCompare(b.name);
        });
        
        const cols = 'style="--cols:24px 1fr 120px 132px"';
        listContainer.innerHTML = `
            <div class="tr th" ${cols}>
                <div><input type="checkbox" id="select-all-cb" onchange="toggleSelectAll(this)" title="${t('files.selectAll')}" aria-label="${t('files.selectAll')}"></div>
                <div>${t('files.name')}</div>
                <div>${t('files.size')}</div>
                <div class="act">${t('files.action')}</div>
            </div>
            ${entries.map(entry => {
                const fullPath = path + '/' + entry.name;
                const isDir = entry.type === 'dir';
                const size = isDir ? '-' : formatFileSize(entry.size);
                const escapedPath = escapeHtml(fullPath);
                const escapedName = escapeHtml(entry.name);
                const isSelected = selectedFiles.has(fullPath);
                return `
                    <div class="tr file-row" ${cols} data-path="${escapedPath}" data-type="${entry.type}" data-name="${escapedName}">
                        <div><input type="checkbox" class="file-checkbox" data-path="${escapedPath}" ${isSelected ? 'checked' : ''} onchange="toggleFileSelection(this.dataset.path, this)" aria-label="${escapedName}"></div>
                        <div class="file-name ${isDir ? 'clickable' : ''}"><span class="fn"><svg class="i"><use href="#${isDir ? 'ri-folder-line' : 'ri-file-text-line'}"/></svg>${escapeHtml(entry.name)}</span></div>
                        <div>${size}</div>
                        <div class="act">${isDir ? '' : `<button class="btn icon sm btn-download" title="${t('common.download')}" aria-label="${t('common.download')}"><svg class="i"><use href="#ri-download-line"/></svg></button>`}<button class="btn icon sm btn-rename" title="${t('files.renameFile')}" aria-label="${t('files.renameFile')}"><svg class="i"><use href="#ri-edit-line"/></svg></button><button class="btn icon sm dg btn-delete" title="${t('common.delete')}" aria-label="${t('common.delete')}"><svg class="i"><use href="#ri-delete-bin-line"/></svg></button></div>
                    </div>
                `;
            }).join('')}
        `;
        
        // 使用事件委托处理点击
        listContainer.addEventListener('click', handleFileListClick);
    } catch (e) {
        if (!current()) return {stale: true};
        console.error('loadDirectory error:', e);
        
        // 检查是否是 SD 卡未挂载（后端返回 'SD card not mounted' 或 'Directory not found'）
        const isUnmounted = path.startsWith('/sdcard') && 
            (e.message.includes('not mounted') || e.message.includes(t('filePage.notMounted')) || e.message.includes('Directory not found'));
        
        if (isUnmounted) {
            listContainer.innerHTML = `
                <div class="empty unmounted-notice">
                    <p class="t-body">${typeof t === 'function' ? t('filePage.sdCardNotMounted') : 'SD 卡未挂载'}</p>
                    <button class="btn primary" onclick="mountSdCard()">${typeof t === 'function' ? t('filePage.mountSdCard') : '挂载 SD 卡'}</button>
                </div>
            `;
        } else {
            listContainer.textContent = t('promptRepair.listRefreshFailed') + '\n' + e.message;
        }
        return {error: e};
    }
    return {refreshed: true};
}

// 事件委托处理文件列表点击
function handleFileListClick(e) {
    const row = e.target.closest('.file-row');
    if (!row) return;
    
    const path = row.dataset.path;
    const type = row.dataset.type;
    const name = row.dataset.name;
    
    // 点击文件夹名称 - 进入目录
    if (e.target.closest('.file-name.clickable')) {
        navigateToPath(path);
        return;
    }
    
    // 点击下载按钮
    if (e.target.closest('.btn-download')) {
        downloadFile(path);
        return;
    }
    
    // 点击重命名按钮
    if (e.target.closest('.btn-rename')) {
        showRenameDialog(path, name);
        return;
    }
    
    // 点击删除按钮
    if (e.target.closest('.btn-delete')) {
        deleteFile(path);
        return;
    }
}

let storageStatusGeneration = 0;
async function loadStorageStatus() {
    const generation = ++storageStatusGeneration;
    const pageCurrent = capturePageValidity();
    try {
        const status = await api.storageStatus();
        if (!pageCurrent() || generation !== storageStatusGeneration) return;
        const container = document.getElementById('storage-status');
        const controlsContainer = document.getElementById('storage-controls');
        
        const sdMounted = status.data?.sd?.mounted;
        const spiffsMounted = status.data?.spiffs?.mounted;
        
        const mountState = (data) => data?.mounted ? `<span class="state ok">${t('filePage.mounted')}</span>` : `<span class="state">${t('filePage.notMounted')}</span>`;
        
        container.innerHTML = `
            <span class="t-label">SD ${mountState(status.data?.sd)}</span>
            <span class="t-label">SPIFFS ${mountState(status.data?.spiffs)}</span>
        `;
        
        if (controlsContainer) {
            controlsContainer.innerHTML = sdMounted
                ? `<button class="btn sm dg" onclick="unmountSdCard()" title="${t('filePage.unmountSdCard')}"><svg class="i"><use href="#ri-eject-line"/></svg>${t('filePage.unmountSdBtn')}</button>`
                : `<button class="btn sm" onclick="mountSdCard()" title="${t('filePage.mountSdCard')}">${t('filePage.mountSdBtn')}</button>`;
        }
    } catch (e) {
        if (!pageCurrent() || generation !== storageStatusGeneration) return;
        console.log('Storage status error:', e);
    }
}

function updateBreadcrumb(path) {
    const container = document.getElementById('breadcrumb');
    const parts = path.split('/').filter(p => p);
    
    let html = '<button class="field sel breadcrumb-item" style="width:84px;text-align:left" onclick="navigateToPath(\'/\')"><svg class="i"><use href="#ri-home-line"/></svg> /</button>';
    let currentPath = '';
    
    parts.forEach((part, i) => {
        currentPath += '/' + part;
        html += `<span class="t-note">/</span><button class="field sel breadcrumb-item${i === parts.length - 1 ? ' current' : ''}" style="min-width:120px;text-align:left" data-path="${escapeHtml(currentPath)}" onclick="navigateToPath(this.dataset.path)">${escapeHtml(part)}</button>`;
    });
    
    container.innerHTML = html;
}

function navigateToPath(path) {
    loadDirectory(path);
}

function getFileIcon(name) {
    return '';
}

function formatFileSize(bytes) {
    if (bytes === 0) return '0 B';
    if (bytes === undefined) return '-';
    const units = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(1024));
    return (bytes / Math.pow(1024, i)).toFixed(i > 0 ? 1 : 0) + ' ' + units[i];
}

// 上传相关
let filesToUpload = [];
let uploadGeneration = 0;
const uploadPathVersions = new Map();
const uploadPendingPaths = new Map();

function showUploadDialog() {
    uploadGeneration++;
    filesToUpload = [];
    document.getElementById('upload-list').innerHTML = '';
    document.getElementById('upload-modal').classList.remove('hidden');
}

function closeUploadDialog() {
    document.getElementById('upload-modal').classList.add('hidden');
}

function setupDragAndDrop() {
    const uploadArea = document.getElementById('upload-area');
    if (!uploadArea) return;
    
    uploadArea.onclick = () => document.getElementById('file-input').click();
    
    uploadArea.ondragover = (e) => {
        e.preventDefault();
        uploadArea.classList.add('drag-over');
    };
    
    uploadArea.ondragleave = () => {
        uploadArea.classList.remove('drag-over');
    };
    
    uploadArea.ondrop = (e) => {
        e.preventDefault();
        uploadArea.classList.remove('drag-over');
        handleFileSelect({ target: { files: e.dataTransfer.files } });
    };
}

function handleFileSelect(event) {
    const files = Array.from(event.target.files);
    filesToUpload = filesToUpload.concat(files);
    
    const listContainer = document.getElementById('upload-list');
    listContainer.replaceChildren();
    filesToUpload.forEach((file, index) => {
        const row = document.createElement('div'); row.className = 'upload-item row';
        const name = document.createElement('span'); name.textContent = file.name;
        const state = document.createElement('span'); state.textContent = formatFileSize(file.size);
        const remove = document.createElement('button'); remove.className = 'btn icon sm quiet';
        remove.innerHTML = '<svg class="i"><use href="#ri-close-line"/></svg>'; remove.setAttribute('aria-label', t('common.delete'));
        remove.onclick = () => removeUploadFile(index);
        row.append(name, state, remove); listContainer.appendChild(row);
    });
}

function removeUploadFile(index) {
    filesToUpload.splice(index, 1);
    handleFileSelect({ target: { files: [] } });
}

async function uploadFiles() {
    if (!filesToUpload.length) { showToast(t('toast.selectFileToUpload'), 'warning'); return; }
    const generation = ++uploadGeneration;
    const destination = currentFilePath;
    const pageCurrent = capturePageValidity();
    const list = document.getElementById('upload-list');
    const page = document.getElementById('page-content');
    const files = [...filesToUpload];
    const items = [...list.querySelectorAll('.upload-item')];
    const failures = [];
    let success = 0, invalid = 0;
    // Invalidate every old entry before starting the first request.
    for (const row of items) while (row.children.length > 3) row.children[3].remove();
    const versions = files.map(file => {
        const path = destination + '/' + file.name;
        const version = {contended: false};
        uploadPathVersions.set(path, version);
        return version;
    });
    for (const [index, file] of files.entries()) {
        const path = destination + '/' + file.name;
        const row = items[index];
        const current = () => generation === uploadGeneration &&
            document.getElementById('upload-list') === list && [...list.children].includes(row) &&
            uploadPathVersions.get(path) === versions[index];
        const state = row?.children[1];
        if (current() && state) state.textContent = t('files.uploading');
        const pending = uploadPendingPaths.get(path) || new Set();
        if (pending.size) {
            versions[index].contended = true;
            for (const version of pending) version.contended = true;
        }
        pending.add(versions[index]); uploadPendingPaths.set(path, pending);
        try {
            const result = await api.fileUpload(path, file);
            success++;
            const pack = result.config_pack;
            if (pack && pack.valid !== true) invalid++;
            if (!current()) continue;
            if (state) state.textContent = t('promptRepair.uploaded');
            if (pack?.valid === true && versions[index].contended) {
                invalid++;
                if (state) state.textContent = t('promptRepair.packRevalidate');
            } else if (pack?.valid === true) {
                if (state) state.textContent = t('promptRepair.packVerified');
                const apply = document.createElement('button');
                apply.className = 'btn sm'; apply.textContent = t('common.apply');
                const verified = () => current() && !versions[index].contended;
                apply.onclick = () => { if (verified()) showConfigPackApplyConfirm(path, pack, verified); };
                row.appendChild(apply);
            } else if (pack && state) {
                state.textContent = t('promptRepair.packInvalid') + '\n' + apiErrorMessage({error: pack.result_message});
            }
        } catch (error) {
            failures.push({path, message: error.message});
            if (current() && state) state.textContent = error.message;
        } finally {
            pending.delete(versions[index]);
            if (!pending.size) uploadPendingPaths.delete(path);
        }
    }
    if (generation !== uploadGeneration) return {success, failures};
    const summary = showOperationSummary('promptRepair.uploadSummary', success, failures, {invalid});
    // Refresh is a separate read: its failure must never change confirmed upload results.
    if (success && pageCurrent() && document.getElementById('page-content') === page && currentFilePath === destination && document.getElementById('file-list')) {
        await refreshFilesPage();
    }
    return summary;
}

// 新建文件夹
function showNewFolderDialog() {
    document.getElementById('new-folder-name').value = '';
    document.getElementById('newfolder-modal').classList.remove('hidden');
}

function closeNewFolderDialog() {
    document.getElementById('newfolder-modal').classList.add('hidden');
}

async function createNewFolder() {
    const name = document.getElementById('new-folder-name').value.trim();
    if (!name) {
        showToast((typeof t === 'function' ? t('toast.enterFolderName') : '请输入文件夹名称'), 'warning');
        return;
    }
    
    const path = currentFilePath + '/' + name;
    try {
        requireApiSuccess(await api.storageMkdir(path), 'storageMkdir');
        showToast((typeof t === 'function' ? t('toast.folderCreated') : '文件夹创建成功'), 'success');
        closeNewFolderDialog();
        refreshFilesPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.createFailedMsg', { msg: e.message }) : '创建失败: ' + e.message), 'error');
    }
}

// 重命名
function showRenameDialog(path, currentName) {
    document.getElementById('rename-input').value = currentName;
    document.getElementById('rename-original-path').value = path;
    document.getElementById('rename-modal').classList.remove('hidden');
}

function closeRenameDialog() {
    document.getElementById('rename-modal').classList.add('hidden');
}

async function doRename() {
    const newName = document.getElementById('rename-input').value.trim();
    const originalPath = document.getElementById('rename-original-path').value;
    
    if (!newName) {
        showToast((typeof t === 'function' ? t('toast.enterNewName') : '请输入新名称'), 'warning');
        return;
    }
    
    // 构建新路径
    const pathParts = originalPath.split('/');
    pathParts.pop();
    const newPath = pathParts.join('/') + '/' + newName;
    
    try {
        requireApiSuccess(await api.storageRename(originalPath, newPath), 'storageRename');
        showToast((typeof t === 'function' ? t('toast.renameSuccess') : '重命名成功'), 'success');
        closeRenameDialog();
        refreshFilesPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.renameFailedMsg', { msg: e.message }) : '重命名失败: ' + e.message), 'error');
    }
}

// 下载文件
async function downloadFile(path, silent = false) {
    let url;
    let anchor;
    try {
        const blob = await api.fileDownload(path);
        url = URL.createObjectURL(blob);
        anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = path.split('/').pop();
        document.body.appendChild(anchor);
        anchor.click();
        if (!silent) showToast(t('promptRepair.downloadStarted'), 'success');
        return {started: true};
    } catch (error) {
        if (!silent) showToast(t('toast.downloadFailedMsg', {msg: error.message}), 'error');
        return {started: false, error};
    } finally {
        if (anchor) anchor.remove();
        if (url) setTimeout(() => URL.revokeObjectURL(url), 1000);
    }
}

// 删除文件
async function deleteFile(path) {
    const name = path.split('/').pop();
    if (!await confirmAction(typeof t === 'function' ? t('common.confirmDeleteItem', { name }) : `确定要删除 "${name}" 吗？`, { primary: t('common.delete'), tone: 'danger' })) {
        return;
    }
    
    try {
        requireApiSuccess(await api.storageDelete(path), 'storageDelete');
        showToast((typeof t === 'function' ? t('toast.deleteSuccess') : '删除成功'), 'success');
        refreshFilesPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.deleteFailedMsg', { msg: e.message }) : '删除失败: ' + e.message), 'error');
    }
}

// =========================================================================
//                         配置页面
// =========================================================================

// 模块描述信息
const CONFIG_MODULE_INFO = {
    net: { get name() { return t('nav.network'); }, icon: 'ri-global-line', get description() { return t('configPage.moduleNetDesc'); } },
    dhcp: { name: 'DHCP', icon: 'ri-router-line', get description() { return t('configPage.moduleDhcpDesc'); } },
    wifi: { name: 'WiFi', icon: 'ri-wifi-line', get description() { return t('configPage.moduleWifiDesc'); } },
    led: { name: 'LED', icon: 'ri-lightbulb-line', get description() { return t('configPage.moduleLedDesc'); } },
    fan: { get name() { return t('automation.fan'); }, icon: 'ri-tornado-line', get description() { return t('configPage.moduleFanDesc'); } },
    device: { get name() { return t('common.devices'); }, icon: 'ri-computer-line', get description() { return t('configPage.moduleDeviceDesc'); } },
    system: { get name() { return t('nav.system'); }, icon: 'ri-settings-line', get description() { return t('configPage.moduleSystemDesc'); } }
};

// 配置项的用户友好描述
const CONFIG_KEY_LABELS = {
    // net
    'eth.enabled': { get label() { return t('configPage.ethEnabled'); }, type: 'bool' },
    'eth.dhcp': { get label() { return t('network.dhcpClients'); }, type: 'bool' },
    'eth.ip': { get label() { return t('system.ipAddress'); }, type: 'ip' },
    'eth.netmask': { get label() { return t('network.subnetMask'); }, type: 'ip' },
    'eth.gateway': { get label() { return t('network.gateway'); }, type: 'ip' },
    'hostname': { get label() { return t('network.hostname'); }, type: 'string' },
    // dhcp
    'enabled': { get label() { return t('common.enable'); }, type: 'bool' },
    'start_ip': { get label() { return t('configPage.startIp'); }, type: 'ip' },
    'end_ip': { get label() { return t('configPage.endIp'); }, type: 'ip' },
    'lease_time': { get label() { return t('configPage.leaseTime'); }, type: 'number' },
    // wifi
    'mode': { get label() { return t('network.mode'); }, type: 'select', options: ['off', 'ap', 'sta', 'apsta'] },
    'ap.ssid': { label: 'AP SSID', type: 'string' },
    'ap.password': { get label() { return t('configPage.apPassword'); }, type: 'password' },
    'ap.channel': { get label() { return t('configPage.apChannel'); }, type: 'number', min: 1, max: 13 },
    'ap.max_conn': { get label() { return t('network.maxConnections'); }, type: 'number', min: 1, max: 10 },
    'ap.hidden': { get label() { return t('networkPage.hideSSID'); }, type: 'bool' },
    // led
    'brightness': { get label() { return t('led.brightness'); }, type: 'number', min: 0, max: 255 },
    'effect_speed': { get label() { return t('configPage.effectSpeed'); }, type: 'number', min: 1, max: 100 },
    'power_on_effect': { get label() { return t('configPage.powerOnEffect'); }, type: 'string' },
    'idle_effect': { get label() { return t('configPage.idleEffect'); }, type: 'string' },
    // fan
    'min_duty': { get label() { return t('configPage.minDuty'); }, type: 'number', min: 0, max: 100 },
    'max_duty': { get label() { return t('configPage.maxDuty'); }, type: 'number', min: 0, max: 100 },
    'target_temp': { get label() { return t('configPage.targetTemp'); }, type: 'number', min: 20, max: 80 },
    // device
    'agx.auto_power_on': { get label() { return t('configPage.agxAutoPowerOn'); }, type: 'bool' },
    'agx.power_on_delay': { get label() { return t('configPage.powerOnDelay'); }, type: 'number' },
    'agx.force_off_timeout': { get label() { return t('configPage.forceOffTimeout'); }, type: 'number' },
    'monitor.enabled': { get label() { return t('configPage.monitorEnabled'); }, type: 'bool' },
    'monitor.interval': { get label() { return t('configPage.monitorInterval'); }, type: 'number' },
    // system
    'timezone': { get label() { return t('common.timezone'); }, type: 'string' },
    'log_level': { get label() { return t('configPage.logLevel'); }, type: 'select', options: ['none', 'error', 'warn', 'info', 'debug', 'verbose'] },
    'console.enabled': { get label() { return t('configPage.consoleEnabled'); }, type: 'bool' },
    'console.baudrate': { get label() { return t('configPage.baudrate'); }, type: 'select', options: [9600, 115200, 460800, 921600] },
    'webui.enabled': { get label() { return t('configPage.webuiEnabled'); }, type: 'bool' },
    'webui.port': { get label() { return t('configPage.webuiPort'); }, type: 'number', min: 1, max: 65535 }
};

// =========================================================================
//                         指令页面
// =========================================================================

// SSH 指令存储（ESP32 后端持久化，不再使用 localStorage）
let sshCommands = {};

/**
 * 从 ESP32 后端加载 SSH 指令
 * 所有指令都保存在 NVS 中，不同浏览器看到相同数据
 * 
 * 指令 ID 格式：
 * - 新格式（语义化）: 基于名称生成，如 "Start_Jetson_Inference", "Check_GPU_Status"
 * - 旧格式（兼容）: "cmd_xxxxxxxx" (随机 hex)
 */
async function loadSshCommands() {
    const pageCurrent = capturePageValidity();
    try {
        const result = await api.call('ssh.commands.list', {});
        if (!pageCurrent()) return;
        if (result && result.data && result.data.commands) {
            // 按 host_id 组织
            sshCommands = {};
            // 收集孤儿命令（单独分组）
            const orphanCommands = [];
            
            for (const cmd of result.data.commands) {
                // 如果是孤儿命令，单独收集
                if (cmd.orphan) {
                    orphanCommands.push({
                        id: cmd.id,
                        name: cmd.name,
                        command: cmd.command,
                        desc: cmd.desc || '',
                        icon: cmd.icon || 'ri-rocket-line',
                        nohup: cmd.nohup || false,
                        expectPattern: cmd.expectPattern || '',
                        failPattern: cmd.failPattern || '',
                        extractPattern: cmd.extractPattern || '',
                        varName: cmd.varName || '',
                        timeout: cmd.timeout || 30,
                        stopOnMatch: cmd.stopOnMatch || false,
                        serviceMode: cmd.serviceMode || false,
                        readyPattern: cmd.readyPattern || '',
                        serviceFailPattern: cmd.serviceFailPattern || '',
                        readyTimeout: cmd.readyTimeout || 120,
                        readyInterval: cmd.readyInterval || 5000,
                        orphan: true,
                        originalHostId: cmd.host_id  // 保留原始 host_id 用于显示
                    });
                    continue;
                }
                
                if (!sshCommands[cmd.host_id]) {
                    sshCommands[cmd.host_id] = [];
                }
                // 字段名与后端 API 返回一致 (camelCase)
                sshCommands[cmd.host_id].push({
                    id: cmd.id,
                    name: cmd.name,
                    command: cmd.command,
                    desc: cmd.desc || '',
                    icon: cmd.icon || 'ri-rocket-line',
                    nohup: cmd.nohup || false,
                    expectPattern: cmd.expectPattern || '',
                    failPattern: cmd.failPattern || '',
                    extractPattern: cmd.extractPattern || '',
                    varName: cmd.varName || '',
                    timeout: cmd.timeout || 30,
                    stopOnMatch: cmd.stopOnMatch || false,
                    // 服务模式字段
                    serviceMode: cmd.serviceMode || false,
                    readyPattern: cmd.readyPattern || '',
                    serviceFailPattern: cmd.serviceFailPattern || '',
                    readyTimeout: cmd.readyTimeout || 120,
                    readyInterval: cmd.readyInterval || 5000,
                    orphan: false
                });
            }
            
            // 如果有孤儿命令，创建特殊分组
            if (orphanCommands.length > 0) {
                sshCommands['__orphan__'] = orphanCommands;
            }
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('Failed to load SSH commands from backend:', e);
        sshCommands = {};
    }
}

/**
 * 保存单个 SSH 指令到后端
 * @param {string} hostId - 主机 ID
 * @param {object} cmdData - 指令数据
 * @param {string|null} existingId - 已有指令 ID（编辑时传入，如 "AGX_Power_On"）
 * @returns {Promise<string>} 返回指令 ID（新建时基于名称生成，如 "Start_Service"）
 */
async function saveSshCommandToBackend(hostId, cmdData, cmdId) {
    /* ID 是必填参数，由前端输入 */
    const params = {
        id: cmdId,  // 必填
        host_id: hostId,
        name: cmdData.name,
        command: cmdData.command,
        ...(cmdData.desc && { desc: cmdData.desc }),
        ...(cmdData.icon && { icon: cmdData.icon }),
        nohup: !!cmdData.nohup,  // 始终发送，确保能取消勾选
        ...(cmdData.expectPattern && { expectPattern: cmdData.expectPattern }),
        ...(cmdData.failPattern && { failPattern: cmdData.failPattern }),
        ...(cmdData.extractPattern && { extractPattern: cmdData.extractPattern }),
        ...(cmdData.varName && { varName: cmdData.varName }),
        ...(cmdData.timeout && { timeout: cmdData.timeout }),
        ...(cmdData.stopOnMatch !== undefined && { stopOnMatch: cmdData.stopOnMatch }),
        // 服务模式字段（仅在 nohup 时有效）
        serviceMode: !!cmdData.serviceMode,
        ...(cmdData.readyPattern && { readyPattern: cmdData.readyPattern }),
        ...(cmdData.serviceFailPattern && { serviceFailPattern: cmdData.serviceFailPattern }),
        ...(cmdData.readyTimeout && { readyTimeout: cmdData.readyTimeout }),
        ...(cmdData.readyInterval && { readyInterval: cmdData.readyInterval })
    };
    
    const result = await api.call('ssh.commands.add', params);
    if (result && result.code === 0 && result.data && result.data.id) {
        return result.data.id;
    }
    throw new ApiOperationError(result, 'ssh.commands.add');
}

/**
 * 从后端删除 SSH 指令
 * @param {string} cmdId - 指令 ID（如 "AGX_Power_On" 或 "cmd_xxxxxxxx"）
 */
// Deletion needs fresh remote evidence: a missing registry entry is unknown,
// including after a controller reboot. Reuse queued verification, never stop here.
const configurationDeletesInFlight = new Set();
async function verifyStoppedServiceForDelete(commandId, pageCurrent) {
    const revision = advanceServiceState(commandId);
    const isCurrent = () => pageCurrent() && revision === serviceStateVersions.get(commandId);
    try {
        const response = await api.call('ssh.commands.get', { id: commandId });
        if (!isCurrent()) return false;
        // A stale template can still be removed after its command has disappeared.
        if (response.code === 2) return true;
        const command = requireApiSuccess(response, 'ssh.commands.get').data || {};
        if (!command.nohup || !command.serviceMode) return true;
        const data = await requestServiceControl(commandId, false, isCurrent);
        if (!data || !isCurrent()) return false;
        renderServiceCommand(commandId, data);
        if (data.state !== 'stopped' || data.busy)
            throw new ApiOperationError({code: 4, error: 'service_delete_protected'});
        return true;
    } catch (error) {
        if (!isCurrent()) return false;
        throw error;
    } finally {
        // Retire this result and its in-flight reads without invalidating a newer control.
        if (isCurrent()) advanceServiceState(commandId);
    }
}

async function deleteSshCommandFromBackend(cmdId) {
    requireApiSuccess(await api.call('ssh.commands.remove', { id: cmdId }), 'call');
}

/**
 * 预创建 SSH 命令相关的变量（保留用于兼容，实际由后端处理）
 * 后端在保存指令时已自动创建变量
 * @param {string} varName - 变量名前缀（如 "ping_test"）
 */
async function preCreateCommandVariables(varName) {
    // 后端 ssh.commands.add API 在保存时已自动创建变量
    // 此函数保留作为兼容占位符
    console.debug(`Variables for ${escapeHtml(varName)}.* are managed by backend`);
}

/**
 * 确保所有已保存指令的变量都已创建（保留用于兼容，实际由后端处理）
 * 后端在 ESP32 启动时会自动预创建所有命令变量
 */
async function ensureAllCommandVariables() {
    // 后端在 ts_automation_init() 时会调用 ts_ssh_commands_precreate_variables()
    // 自动为 NVS 中保存的所有命令创建变量
    console.debug('Command variables are pre-created by backend on ESP32 boot');
}

async function loadCommandsPage() {
    const pageCurrent = capturePageValidity();
    clearInterval(refreshInterval);
    stopServiceStatusRefresh();
    
    
    // Leave remote execution untouched when replacing the page.
    releaseExecDisplay();
    
    // 加载已保存的指令（从后端）
    await loadSshCommands();
    if (!pageCurrent()) return;
    
    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page page-commands">
            <!-- 主机选择 -->
            <div class="sec-h">
                <span class="t-section sec-t" data-i18n="ssh.selectHost">${t('ssh.selectHost')}</span>
                <div class="acts">
                    <button class="btn sm" onclick="showImportSshCommandModal()"><svg class="i"><use href="#ri-download-line"/></svg><span data-i18n="ssh.importCommand">${t('ssh.importCommand')}</span></button>
                    <button class="btn sm" onclick="showAddCommandModal()"><svg class="i"><use href="#ri-add-line"/></svg><span data-i18n="ssh.newCommand">${t('ssh.newCommand')}</span></button>
                </div>
            </div>
            <div id="host-selector" class="host-selector">
                <div class="t-note">${t('sshPage.loadingHosts')}</div>
            </div>
            
            <!-- 指令列表 -->
            <div>
                <div class="sec-h">
                    <span class="t-section sec-t">${t('ssh.commandList')}</span>
                    <div class="acts"></div>
                </div>
                <div id="commands-list" class="card" style="padding:0">
                    <div class="empty"><p class="t-note">${t('ssh.selectHostFirst')}</p></div>
                </div>
            </div>
            
            <!-- 执行结果 -->
            <div class="card exec-card" id="exec-result-section" style="display:none">
                <div class="between wrap">
                    <span class="t-section">${t('sshPage.execResult')}</span>
                    <div class="acts wrap">
                        <button id="cancel-exec-btn" class="btn sm dg" onclick="cancelExecution()" style="display:none">${t('common.cancel')} (Esc)</button>
                        <button class="btn sm" onclick="clearExecResult()">${t('common.clear')}</button>
                        <!-- nohup 快捷操作按钮 -->
                        <div id="nohup-actions" class="acts wrap" style="display:none">
                            <span class="vsep"></span>
                            <button class="btn sm" id="nohup-view-log" onclick="nohupViewLog()">${t('sshPage.viewLog')}</button>
                            <button class="btn sm" id="nohup-tail-log" onclick="nohupTailLog()">${t('sshPage.tailLog')}</button>
                            <button class="btn sm" id="nohup-stop-tail" onclick="nohupStopTail()" style="display:none">${t('sshPage.stopTail')}</button>
                            <button class="btn sm" id="nohup-check-process" onclick="nohupCheckProcess()">${t('sshPage.checkProcess')}</button>
                            <button class="btn sm dg" id="nohup-stop-process" onclick="nohupStopProcess()">${t('sshPage.stopProcess')}</button>
                        </div>
                    </div>
                </div>
                <pre id="exec-result" class="term exec-result"></pre>
                
                <!-- 模式匹配结果面板 -->
                <div id="match-result-panel" class="match-panel" style="display:none">
                    <div class="between">
                        <span class="t-section">${t('sshPage.matchResultTitle')}</span>
                        <span class="state" id="match-status-badge"></span>
                    </div>
                    <div class="match-grid">
                        <div><div class="t-label">${t('sshPage.expectMatch')}</div><div class="t-value match-value" id="match-expect-result">-</div><code class="t-note mono">msg.expect_matched</code></div>
                        <div><div class="t-label">${t('sshPage.failMatch')}</div><div class="t-value match-value" id="match-fail-result">-</div><code class="t-note mono">msg.fail_matched</code></div>
                        <div><div class="t-label">${t('sshPage.extractContent')}</div><div class="t-value match-value match-extracted" id="match-extracted-result">-</div><code class="t-note mono">msg.extracted</code></div>
                        <div><div class="t-label">${t('sshPage.finalStatus')}</div><div class="t-value match-value" id="match-final-status">-</div><code class="t-note mono">msg.status</code></div>
                    </div>
                    <div class="t-note">${t('sshPage.wsMessageHint')}</div>
                </div>
            </div>
        </div>
        
        <!-- 新建/编辑指令模态框 -->
        <div id="command-modal" class="modal hidden"><form id="command-form" class="sheet m-float" style="width:660px" onsubmit="return false;">
            <div class="sh"><span class="st" id="command-modal-title">${t('ssh.newCommand')}</span></div>
            <div class="sb">
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:14px;align-items:end">
                    <div class="fl" id="cmd-id-group">
                        <label>${t('sshPage.cmdId')} *</label>
                        <input type="text" class="field" id="cmd-edit-id" placeholder="${t('ssh.cmdIdPlaceholderShort')}" title="${escapeHtml(t('sshPage.cmdIdHint'))}" pattern="^[a-zA-Z0-9][a-zA-Z0-9_-]*[a-zA-Z0-9]$|^[a-zA-Z0-9]$" oninput="validateCommandId(this)" required>
                    </div>
                    <div class="fl"><label>${t('common.name')} *</label><input type="text" class="field" id="cmd-name" placeholder="${t('sshPage.cmdNamePlaceholder')}" required></div>
                </div>
                <span id="cmd-id-error" class="form-error" style="display:none"></span>
                <div style="height:12px"></div>
                <div class="fl"><label>${t('common.command')} *</label><textarea class="field mono" id="cmd-command" style="height:64px" placeholder="${t('sshPage.cmdCommandPlaceholder')}" title="${escapeHtml(t('sshPage.multiLineHint'))}" required></textarea></div>
                <div style="height:12px"></div>
                <div class="fl"><label>${t('common.description')}</label><input type="text" class="field" id="cmd-desc" style="width:100%" placeholder="${t('sshPage.cmdDescPlaceholder')}" title="${escapeHtml(t('common.optional'))}"></div>
                <div class="gt" style="margin-top:16px">${t('dataWidget.icon')}</div>
                <div style="display:flex;justify-content:center;margin-bottom:10px"><div class="seg icon-type-tabs"><button type="button" class="on" data-type="emoji" onclick="switchCmdIconType('emoji')">${t('automation.iconTab')}</button><button type="button" data-type="image" onclick="switchCmdIconType('image')">${t('automation.imageTab')}</button></div></div>
                <div id="icon-emoji-picker" style="display:grid;grid-template-columns:repeat(6,1fr);gap:8px">
                    ${['ri-rocket-line', 'ri-refresh-line', 'ri-thunderstorms-line', 'ri-tools-line', 'ri-bar-chart-line', 'ri-search-line', 'ri-save-line', 'ri-delete-bin-line', 'ri-stop-line', 'ri-play-line', 'ri-box-3-line', 'ri-settings-line'].map(icon =>
                        `<button type="button" class="btn icon icon-btn" data-icon="${icon}" onclick="selectCmdIcon('${icon}')" style="width:100%;height:40px"><svg class="i"><use href="#${icon}"/></svg></button>`
                    ).join('')}
                </div>
                <div id="icon-image-picker" class="hidden">
                    <div class="acts" style="align-items:center">
                        <div id="cmd-icon-preview" class="icon-image-preview"><span class="t-note">${t('automation.previewNone')}</span></div>
                        <input type="text" class="field mono" id="cmd-icon-path" placeholder="/sdcard/images/..." readonly style="flex:1">
                        <button type="button" class="btn sm" onclick="browseCmdIconImage()">${t('common.browse')}</button>
                        <button type="button" class="btn sm" onclick="clearCmdIconImage()">${t('common.clear')}</button>
                    </div>
                </div>
                <input type="hidden" id="cmd-icon" value="ri-rocket-line">
                <input type="hidden" id="cmd-icon-type" value="emoji">
                ${gt(t('ssh.runMode'))}
                ${grp(
                    row(t('ssh.nohupShort'), swc('cmd-nohup', false, 'onchange="updateNohupState()"'), '', t('ssh.nohupHint')) +
                    `<div class="row hidden" id="cmd-service-mode-options"><div class="rl" title="${escapeHtml(t('ssh.serviceModeHint'))}">${t('ssh.serviceMode')}<small>${t('ssh.serviceModeNote')}</small></div><div class="rc">${swc('cmd-service-mode', false, 'onchange="updateServiceModeState()"')}</div></div>`)}
                <div class="gw hidden" id="cmd-service-mode-fields">
                    ${gt(t('ssh.serviceMode'))}
                    ${grp(
                        row(t('ssh.readyMatch'), inp('cmd-ready-pattern', 200, t('ssh.readyMatchPh'), 'mono'), '', t('ssh.readyPatternHint')) +
                        row(t('sshPage.failMatch'), inp('cmd-service-fail-pattern', 200, t('ssh.failMatchPh'), 'mono'), '', t('ssh.serviceFailPatternHint')) +
                        row(t('ssh.readyTimeoutSec'), inp('cmd-ready-timeout', 80, '', '', 'type="number" value="120" min="10" max="600" step="10"'), '', t('ssh.readyTimeoutHint')) +
                        row(t('ssh.pollIntervalMs'), inp('cmd-ready-interval', 80, '', '', 'type="number" value="5000" min="1000" max="30000" step="1000"'), '', t('ssh.readyIntervalHint')))}
                </div>
                ${gt(t('ssh.outputMatchTitle'))}
                ${grp(
                    `<div class="row" id="cmd-var-name-group"><div class="rl">${t('ssh.variableName')}</div><div class="rc">${inp('cmd-var-name', 200, t('ssh.cmdVarNamePlaceholder'), 'mono')}</div></div>` +
                    `<div class="rows" id="cmd-pattern-options">` +
                        row(t('ssh.expectMatch'), inp('cmd-expect-pattern', 200, t('ssh.cmdExpectPatternPlaceholder'), 'mono', 'oninput="updateTimeoutState()"'), '', t('ssh.successPatternHint')) +
                        row(t('sshPage.failMatch'), inp('cmd-fail-pattern', 200, t('ssh.failMatchPh'), 'mono', 'oninput="updateTimeoutState()"'), '', t('ssh.failPatternHint')) +
                        row(t('ssh.extractRegex'), inp('cmd-extract-pattern', 200, t('ssh.cmdExtractPatternPlaceholder'), 'mono'), '', t('ssh.extractPatternHint')) +
                        row(t('ssh.stopOnHit'), swc('cmd-stop-on-match', false, 'onchange="updateTimeoutState()"'), '', t('ssh.stopOnMatchHint')) +
                    `</div>` +
                    `<div class="row" id="cmd-timeout-group"><div class="rl">${t('ssh.timeoutSec')}</div><div class="rc">${inp('cmd-timeout', 80, '', '', 'type="number" value="30" min="5" max="300" step="5"')}</div></div>`)}
                <div class="t-note" id="cmd-var-name-hint" style="margin:6px 4px 0">${t('ssh.varNameHint')}</div>
                <small id="cmd-timeout-hint" class="hidden"></small>
            </div>
            <div class="sf"><button type="button" class="btn lg" onclick="closeCommandModal()">${t('sshPage.cancelBtn')}</button><button type="submit" class="btn lg primary" onclick="saveCommand()">${t('sshPage.saveBtn')}</button></div>
        </form></div>
    `;
    
    // 加载主机列表
    await loadHostSelector();
    if (!pageCurrent()) return;
    
    // 确保所有已保存指令的变量都已创建（后台执行，不阻塞 UI）
    ensureAllCommandVariables().catch(e => {
        console.warn('Failed to ensure command variables:', e);
    });
}

// 当前选中的主机
let selectedHostId = null;

async function loadHostSelector() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('host-selector');
    
    try {
        const result = await api.call('ssh.hosts.list', {});
        if (!pageCurrent()) return;
        const hosts = result.data?.hosts || [];
        
        if (hosts.length === 0) {
            container.innerHTML = `
                <div class="empty">
                    <p class="t-body">${t('sshPage.noDeployedHostsMsg')}</p>
                    <p class="t-note">${t('sshPage.deployKeyAtSecurityHint')}</p>
                </div>
            `;
            return;
        }
        
        // 存储主机数据
        window._cmdHostsList = hosts;
        
        // 检查是否有孤儿命令
        const orphanCount = sshCommands['__orphan__']?.length || 0;
        
        let html = hosts.map(h => `
            <div class="tile host-card ${selectedHostId === h.id ? 'on' : ''}" style="width:280px" onclick="selectHost('${escapeHtml(h.id)}')" data-host-id="${escapeHtml(h.id)}">
                <svg class="i host-icon"><use href="#ri-server-line"/></svg>
                <div>
                    <div class="t-body" style="font-weight:600">${escapeHtml(h.id)}</div>
                    <div class="t-note mono">${escapeHtml(h.username)}@${escapeHtml(h.host)}:${h.port}</div>
                </div>
            </div>
        `).join('');
        
        // 如果有孤儿命令，添加特殊分组
        if (orphanCount > 0) {
            html += `
            <div class="tile host-card orphan-group ${selectedHostId === '__orphan__' ? 'on' : ''}" style="width:300px;background:rgba(255,149,0,.10)" onclick="selectHost('__orphan__')" data-host-id="__orphan__">
                <span style="color:var(--warn)"><svg class="i"><use href="#ri-alert-line"/></svg></span>
                <div>
                    <div class="t-body" style="font-weight:600">${t('sshPage.orphanCommands')}</div>
                    <div class="t-note" style="color:var(--warn)">${t('promptRepair.orphanCount', {count: orphanCount})}</div>
                </div>
            </div>
            `;
        }
        
        container.innerHTML = html;
        
        // 刷新指令列表（始终刷新以显示"创建第一个指令"按钮）
        refreshCommandsList();
        
    } catch (e) {
        if (!pageCurrent()) return;
        container.innerHTML = `<p class="t-note" style="color:var(--bad)">${t('common.loadFailed')}: ${escapeHtml(e.message)}</p>`;
    }
}

function selectHost(hostId) {
    selectedHostId = hostId;
    
    // 更新选中状态
    document.querySelectorAll('.host-card').forEach(card => {
        card.classList.toggle('on', card.dataset.hostId === hostId);
    });
    
    // 刷新指令列表
    refreshCommandsList();
}

function refreshCommandsList() {
    const container = document.getElementById('commands-list');
    const createFirstBtn = `<button type="button" class="btn sm" onclick="showAddCommandModal()"><svg class="i"><use href="#ri-add-line"/></svg>${t('ssh.createFirstCommand')}</button>`;
    const empty = (msgKey) => `<div class="empty"><p class="t-note">${t(msgKey)}</p>${createFirstBtn}</div>`;
    
    if (!selectedHostId) {
        container.innerHTML = empty('ssh.selectHostFirst');
        return;
    }
    
    const hostCommands = sshCommands[selectedHostId] || [];
    
    if (hostCommands.length === 0) {
        container.innerHTML = empty('ssh.noCommandsForHost');
        return;
    }
    
    // 操作列宽按最多的按钮数定，保证各行对齐（默认 执行 / 编辑 / 删除 三个 = 96px）
    const extraCount = c => (c.varName ? 1 : 0) + (c.nohup && c.serviceMode ? 3 : 0) + 1;
    const nBtn = 3 + Math.max(...hostCommands.map(extraCount));
    const cols = `style="--cols:32px 1fr 1.6fr ${nBtn * 28 + (nBtn - 1) * 6}px"`;

    container.innerHTML = hostCommands.map((cmd, idx) => {
        const isOrphan = cmd.orphan === true;
        
        // 模式匹配 / nohup 标签
        const tag = (icon, title, cls = '') => `<span class="tag ${cls}" title="${title}"><svg class="i"><use href="#${icon}"/></svg></span>`;
        let tagsHtml = '';
        if (cmd.expectPattern) tagsHtml += tag('ri-check-line', t('promptRepair.expectedLabel') + ' ' + escapeHtml(cmd.expectPattern));
        if (cmd.failPattern) tagsHtml += tag('ri-close-line', t('promptRepair.failureLabel') + ' ' + escapeHtml(cmd.failPattern));
        if (cmd.extractPattern) tagsHtml += tag('ri-file-list-line', t('promptRepair.extractLabel') + ' ' + escapeHtml(cmd.extractPattern));
        if (cmd.nohup && !cmd.serviceMode) tagsHtml += tag('ri-rocket-line', t('ssh.nohupTitle'));
        if (cmd.nohup && cmd.serviceMode) {
            // 服务模式：显示服务状态（用 cmd.id 作唯一标识，避免多个服务时 ID 冲突）
            const statusId = `service-status-${cmd.id || idx}`;
            tagsHtml += `<span class="service-mode-status" title="${escapeHtml(t('promptRepair.servicePattern', {pattern: cmd.readyPattern}))}" data-var="${escapeHtml(cmd.varName)}" data-status-id="${statusId}"><span id="${statusId}" data-command="${escapeHtml(cmd.id)}" class="state service-status">...</span></span>`;
        }
        
        // 图标：RemixIcon 类名、图片路径或旧版 Emoji
        const iconValue = cmd.icon || 'ri-rocket-line';
        let iconHtml;
        if (iconValue.startsWith('/sdcard/')) {
            iconHtml = `<img src="/api/v1/file/download?path=${encodeURIComponent(iconValue)}" alt="icon" style="width:16px;height:16px;object-fit:contain" onerror="this.outerHTML='<svg class=\\'i\\'><use href=\\'#ri-rocket-line\\'/></svg>'">`;
        } else if (iconValue.startsWith('ri-')) {
            iconHtml = `<svg class="i"><use href="#${iconValue}"/></svg>`;
        } else {
            iconHtml = iconValue;
        }
        
        // 服务模式按钮（日志、停止）
        const safeName = cmd.name.replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || String(cmd.id).replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || 'cmd';
        const serviceActions = (cmd.nohup && cmd.serviceMode)
            ? icoBtn('ri-refresh-line', runtimeText('verifyState'), `verifyServiceState('${escapeHtml(cmd.id)}')`) + icoBtn('ri-file-text-line', t('sshPage.viewLog'), `viewServiceLog(${idx}, '${escapeHtml(safeName)}')`) + icoBtn('ri-stop-line', t('sshPage.stopService'), `stopServiceProcess(${idx})`, 'dg')
            : '';
        const varBtn = cmd.varName ? `<button class="btn icon sm cmd-variables" data-variable="${escapeHtml(cmd.varName)}" title="${escapeHtml(t('promptRepair.viewVariables', {name: cmd.varName}))}" aria-label="${escapeHtml(t('promptRepair.viewVariables', {name: cmd.varName}))}"><svg class="i"><use href="#ri-bar-chart-line"/></svg></button>` : '';
        const runTitle = isOrphan ? t('ssh.hostNotExistCannotExec') : t('common.run');
        const runBtn = `<button class="btn icon sm btn-exec" onclick="executeCommand(${idx})" title="${runTitle}" aria-label="${runTitle}" ${isOrphan ? 'disabled' : ''}><svg class="i"><use href="#ri-play-line"/></svg></button>`;
        
        return `
        <div class="tr command-card ${isOrphan ? 'orphan-command' : ''}" ${cols} data-cmd-idx="${idx}" data-has-service="${cmd.serviceMode || false}">
            <span style="color:var(--accent-text)">${iconHtml}</span>
            <div>
                <div class="t-body cmd-name" style="font-weight:500" title="${escapeHtml(cmd.name)}">${escapeHtml(cmd.name)}${tagsHtml ? ' ' + tagsHtml : ''}</div>
                <div class="t-note mono">${escapeHtml(cmd.id || '')}</div>
                ${isOrphan ? `<div class="t-note" style="color:var(--warn)">${escapeHtml(t('promptRepair.orphanHost', {id: cmd.originalHostId || '?'}))}</div>` : ''}
                ${cmd.desc ? `<div class="t-note" title="${escapeHtml(cmd.desc)}">${escapeHtml(cmd.desc)}</div>` : ''}
            </div>
            <span class="mono t-label cmd-code" title="${escapeHtml(cmd.command)}">${escapeHtml(cmd.command.split('\n')[0])}${cmd.command.includes('\n') ? ' ...' : ''}</span>
            <div class="act">${runBtn}${serviceActions}${varBtn}${icoBtn('ri-edit-line', t('common.edit'), `editCommand(${idx})`)}${icoBtn('ri-upload-line', t('ssh.exportConfig'), `exportSshCommand('${escapeHtml(cmd.id)}')`)}${icoBtn('ri-delete-bin-line', t('common.delete'), `deleteCommand(${idx})`, 'dg')}</div>
        </div>
    `}).join('');
    
    container.querySelectorAll('.cmd-variables').forEach(button => {
        button.addEventListener('click', () => showCommandVariables(button.dataset.variable));
    });
    // 更新服务模式状态
    updateServiceStatusInList();
}

/**
 * 更新指令列表中的服务状态
 * 查询每个服务模式指令的变量状态并更新显示
 */
async function updateServiceStatusInList() {
    await refreshServiceStates();
    if (document.querySelector('.service-mode-status')) startServiceStatusRefresh();
}

/**
 * 获取服务状态显示文本
 */
function getServiceStatusLabel(status) {
    const keyMap = { 'ready': 'sshPage.statusReady', 'checking': 'sshPage.statusChecking', 'timeout': 'sshPage.statusTimeout', 'failed': 'sshPage.statusFailed', 'idle': 'sshPage.statusIdle', 'stopped': 'sshPage.statusStopped' };
    const key = keyMap[status];
    return key ? t(key) : t('common.unknown');
}

function showAddCommandModal() {
    if (!selectedHostId) {
        showToast((typeof t === 'function' ? t('ssh.selectHostFirst') : '请先选择一个主机'), 'warning');
        return;
    }
    
    document.getElementById('command-modal-title').textContent = (typeof t === 'function' ? t('ssh.newCommand') : '新建指令');
    
    /* 新建模式：ID 可编辑 */
    const idInput = document.getElementById('cmd-edit-id');
    const idGroup = document.getElementById('cmd-id-group');
    idInput.value = '';
    idInput.readOnly = false;
    idInput.style.backgroundColor = '';
    idInput.style.cursor = '';
    idInput.style.borderColor = '';
    idGroup.classList.remove('edit-mode');
    document.getElementById('cmd-id-error').style.display = 'none';
    
    document.getElementById('cmd-name').value = '';
    document.getElementById('cmd-command').value = '';
    document.getElementById('cmd-desc').value = '';
    document.getElementById('cmd-icon').value = 'ri-rocket-line';
    document.getElementById('cmd-icon-type').value = 'emoji';
    document.getElementById('cmd-icon-path').value = '';
    
    // 重置图标选择 UI
    switchCmdIconType('emoji');
    updateCmdIconPreview(null);
    
    // 重置 nohup 选项
    const nohupCheckbox = document.getElementById('cmd-nohup');
    if (nohupCheckbox) nohupCheckbox.checked = false;
    
    // 重置高级选项
    document.getElementById('cmd-expect-pattern').value = '';
    document.getElementById('cmd-fail-pattern').value = '';
    document.getElementById('cmd-extract-pattern').value = '';
    document.getElementById('cmd-var-name').value = '';
    document.getElementById('cmd-timeout').value = 30;
    document.getElementById('cmd-stop-on-match').checked = false;
    
    // 重置服务模式选项
    const serviceModeCheckbox = document.getElementById('cmd-service-mode');
    if (serviceModeCheckbox) serviceModeCheckbox.checked = false;
    document.getElementById('cmd-ready-pattern').value = '';
    document.getElementById('cmd-ready-timeout').value = 120;
    document.getElementById('cmd-ready-interval').value = 5000;
    
    // 折叠高级选项面板
    const advDetails = document.querySelector('.advanced-options');
    if (advDetails) advDetails.open = false;
    
    // 重置图标选中状态
    document.querySelectorAll('.icon-btn').forEach(btn => btn.classList.remove('selected'));
    document.querySelector('.icon-btn')?.classList.add('selected');
    
    document.getElementById('command-modal').classList.remove('hidden');
    
    // 更新超时输入框状态
    updateTimeoutState();
    // 更新 nohup 状态
    updateNohupState();
    // 更新服务模式状态
    updateServiceModeState();
}

function closeCommandModal() {
    document.getElementById('command-modal').classList.add('hidden');
}

/**
 * 显示指令变量
 * @param {string} varName - 变量名前缀（不含 cmd.）
 */
// 数据源/指令变量弹窗：#source-variables-modal 是空容器，打开时用 sheet() 构建（正文 id 保持 source-variables-body）
function openSourceVarsSheet(title) {
    const modal = document.getElementById('source-variables-modal');
    if (!modal) return null;
    modal.innerHTML = sheet(660, title,
        `<div class="card" id="source-variables-body" style="padding:0;background:var(--fill)"><div class="tr" style="--cols:1fr;border-top:0;color:var(--ink-3)">${t('common.loading')}</div></div>`,
        `<button class="btn lg primary" onclick="closeSourceVariablesModal()">${t('common.close')}</button>`,
        'closeSourceVariablesModal()');
    modal.classList.remove('hidden');
    return document.getElementById('source-variables-body');
}

function setSourceVarsMessage(body, msg, isError = false) {
    body.innerHTML = `<div class="tr" style="--cols:1fr;border-top:0;${isError ? '' : 'color:var(--ink-3)'}">${isError ? `<span class="form-error">${msg}</span>` : msg}</div>`;
}

function renderSourceVarsTable(body, vars) {
    const cols = '--cols:1.4fr .8fr 1fr 1fr';
    body.innerHTML = `<div class="tr th" style="${cols}"><div>${t('sshPage.varTableName')}</div><div>${t('sshPage.varTableType')}</div><div>${t('sshPage.varTableValue')}</div><div>${t('sshPage.varTableUpdated')}</div></div>` +
        vars.map(v => `<div class="tr" style="${cols}"><div><span class="mono">${escapeHtml(v.name)}</span></div><div>${escapeHtml(v.type || '-')}</div><div>${formatVariableValue(v.value, v.type)}</div><div>${formatVariableUpdateTime(v)}</div></div>`).join('');
}

async function showCommandVariables(varName) {
    const body = openSourceVarsSheet(t('ui.variablesTitle', { name: escapeHtml(varName) + '.*' }));
    if (!body) return;
    
    try {
        const result = await api.call('automation.variables.list', {
            prefix: `${varName}.`,
            include_meta: true
        });
        if (result.code === 0 && result.data && result.data.variables) {
            const vars = result.data.variables.filter(v => 
                v.source_id === varName || v.name.startsWith(varName + '.'));
            
            if (vars.length === 0) {
                setSourceVarsMessage(body, t('sshPage.noVariableData'));
                return;
            }
            renderSourceVarsTable(body, vars);
        } else {
            setSourceVarsMessage(body, escapeHtml(result.message || t('sshPage.getVarFailed')), true);
        }
    } catch (error) {
        setSourceVarsMessage(body, escapeHtml(error.message), true);
    }
}

/* 更新超时输入框的启用状态 */
function updateTimeoutState() {
    const expectPattern = document.getElementById('cmd-expect-pattern')?.value?.trim();
    const failPattern = document.getElementById('cmd-fail-pattern')?.value?.trim();
    const stopOnMatch = document.getElementById('cmd-stop-on-match')?.checked;
    
    const timeoutGroup = document.getElementById('cmd-timeout-group');
    const timeoutInput = document.getElementById('cmd-timeout');
    const timeoutHint = document.getElementById('cmd-timeout-hint');
    
    /* 超时在以下情况有效：设定了成功/失败条件，或勾选了匹配后停止 */
    const isTimeoutEffective = stopOnMatch || expectPattern || failPattern;
    
    if (timeoutGroup) {
        timeoutGroup.style.opacity = isTimeoutEffective ? '1' : '0.5';
    }
    if (timeoutInput) {
        timeoutInput.disabled = !isTimeoutEffective;
    }
    if (timeoutHint) {
        timeoutHint.textContent = isTimeoutEffective 
            ? (typeof t === 'function' ? t('ssh.matchTimeoutHint') : '匹配超时后命令将被终止')
            : (typeof t === 'function' ? t('ssh.timeoutConditionHint') : '超时仅在设置了成功/失败模式或勾选了"匹配后停止"时有效');
        timeoutHint.style.color = isTimeoutEffective ? '' : 'var(--ink-3)';
    }
}

/* 更新 nohup 选项的状态（显示/隐藏服务模式选项，禁用模式匹配选项） */
function updateNohupState() {
    const nohup = document.getElementById('cmd-nohup')?.checked;
    const patternOptions = document.getElementById('cmd-pattern-options');
    const serviceModeOptions = document.getElementById('cmd-service-mode-options');
    const varNameGroup = document.getElementById('cmd-var-name-group');
    const varNameHint = document.getElementById('cmd-var-name-hint');
    
    // 显示/隐藏服务模式选项（仅 nohup 启用时显示）
    if (serviceModeOptions) {
        serviceModeOptions.classList.toggle('hidden', !nohup);
    }
    
    // nohup 模式下隐藏模式匹配选项
    if (patternOptions) {
        patternOptions.classList.toggle('hidden', nohup);
    }
    
    // 更新变量名提示
    if (varNameGroup && varNameHint) {
        if (nohup) {
            varNameHint.innerHTML = (typeof t === 'function' ? t('ssh.serviceModeVarHint') : '服务模式下，状态变量为 <code>${变量名}.status</code>（ready/checking/timeout）');
        } else {
            varNameHint.innerHTML = (typeof t === 'function' ? t('ssh.varNameHint') : '执行结果将存储为 <code>${变量名}.status</code>、<code>${变量名}.extracted</code> 等，可在后续命令中引用');
        }
    }
    
    // 如果启用 nohup，清空模式匹配选项
    if (nohup) {
        const fields = ['cmd-expect-pattern', 'cmd-fail-pattern', 'cmd-extract-pattern'];
        fields.forEach(id => {
            const el = document.getElementById(id);
            if (el) el.value = '';
        });
        const stopMatch = document.getElementById('cmd-stop-on-match');
        if (stopMatch) stopMatch.checked = false;
    } else {
        // 禁用 nohup 时，重置服务模式
        const serviceMode = document.getElementById('cmd-service-mode');
        if (serviceMode) serviceMode.checked = false;
        updateServiceModeState();
    }
}

/* 更新服务模式选项的状态（显示/隐藏配置字段） */
function updateServiceModeState() {
    const serviceMode = document.getElementById('cmd-service-mode')?.checked;
    const serviceModeFields = document.getElementById('cmd-service-mode-fields');
    const varNameInput = document.getElementById('cmd-var-name');
    
    if (serviceModeFields) {
        serviceModeFields.classList.toggle('hidden', !serviceMode);
    }
    
    // 如果启用服务模式，变量名字段变为必填并提示
    if (varNameInput) {
        if (serviceMode) {
            varNameInput.placeholder = typeof t === 'function' ? t('sshPage.varNameRequiredPlaceholder') : '必填，例如：vllm（用于状态变量）';
            varNameInput.style.borderColor = varNameInput.value ? '' : 'var(--warn-dot)';
        } else {
            varNameInput.placeholder = typeof t === 'function' ? t('ssh.cmdVarNamePlaceholder') : '例如：ping_test';
            varNameInput.style.borderColor = '';
        }
    }
}

/**
 * 切换图标类型（Emoji / 图片）
 */
function switchCmdIconType(type) {
    const iconTypeInput = document.getElementById('cmd-icon-type');
    iconTypeInput.value = type;
    
    // 更新分段控件状态
    document.querySelectorAll('.icon-type-tabs button').forEach(b => b.classList.toggle('on', b.dataset.type === type));
    
    // 切换面板显示
    document.getElementById('icon-emoji-picker').classList.toggle('hidden', type !== 'emoji');
    document.getElementById('icon-image-picker').classList.toggle('hidden', type !== 'image');
    
    // 如果切换到图标且当前是图片，恢复默认
    if (type === 'emoji') {
        const currentIcon = document.getElementById('cmd-icon').value;
        if (currentIcon.startsWith('/sdcard/')) {
            document.getElementById('cmd-icon').value = 'ri-rocket-line';
            selectCmdIcon('ri-rocket-line');
        }
    }
}

/**
 * 浏览 SD 卡图像
 */
async function browseCmdIconImage() {
    filePickerCurrentPath = '/sdcard/images';
    filePickerSelectedFile = null;
    filePickerCallback = (path) => {
        document.getElementById('cmd-icon').value = path;
        document.getElementById('cmd-icon-path').value = path;
        updateCmdIconPreview(path);
    };
    document.getElementById('file-picker-modal').classList.remove('hidden');
    await loadFilePickerDirectory(filePickerCurrentPath);
}

/**
 * 更新图标图片预览
 */
function updateCmdIconPreview(path) {
    const preview = document.getElementById('cmd-icon-preview');
    if (path && path.startsWith('/sdcard/')) {
        const loadFailed = typeof t === 'function' ? t('sshPage.iconPreviewFailed') : '加载失败';
        preview.innerHTML = `<img src="/api/v1/file/download?path=${encodeURIComponent(path)}" alt="icon" onerror="this.parentElement.innerHTML='<span class=\\'preview-placeholder\\'>${loadFailed}</span>'">`;
    } else {
        preview.innerHTML = '<span class="preview-placeholder">' + (typeof t === 'function' ? t('sshPage.iconPreviewNone') : '无') + '</span>';
    }
}

/**
 * 清除图标图片
 */
function clearCmdIconImage() {
    document.getElementById('cmd-icon').value = 'ri-rocket-line';
    document.getElementById('cmd-icon-path').value = '';
    document.getElementById('cmd-icon-type').value = 'emoji';
    updateCmdIconPreview(null);
    switchCmdIconType('emoji');
}

function selectCmdIcon(icon) {
    document.getElementById('cmd-icon').value = icon;
    document.getElementById('cmd-icon-type').value = 'emoji';
    document.querySelectorAll('.icon-btn').forEach(btn => {
        btn.classList.toggle('selected', btn.getAttribute('data-icon') === icon);
    });
}

/**
 * 验证指令 ID 格式
 * 规则：只允许字母、数字、下划线、连字符，不能以 _ 或 - 开头/结尾
 */
function validateCommandId(input) {
    const value = input.value;
    const errorSpan = document.getElementById('cmd-id-error');
    
    if (!value) {
        input.style.borderColor = '';
        errorSpan.style.display = 'none';
        return false;
    }
    
    // 验证规则
    const validPattern = /^[a-zA-Z0-9][a-zA-Z0-9_-]*[a-zA-Z0-9]$|^[a-zA-Z0-9]$/;
    const hasInvalidChars = /[^a-zA-Z0-9_-]/.test(value);
    const startsWithInvalid = /^[_-]/.test(value);
    const endsWithInvalid = /[_-]$/.test(value);
    
    let errorMsg = '';
    if (hasInvalidChars) {
        errorMsg = t('sshPage.idValidCharsOnly');
    } else if (startsWithInvalid) {
        errorMsg = t('sshPage.idNoStartUnderscore');
    } else if (endsWithInvalid) {
        errorMsg = t('sshPage.idNoEndUnderscore');
    } else if (value.length > 31) {
        errorMsg = t('sshPage.idTooLong');
    }
    
    if (errorMsg) {
        input.style.borderColor = 'var(--bad)';
        errorSpan.innerHTML = '<svg class="i"><use href="#ri-error-warning-line"/></svg> ' + errorMsg;
        errorSpan.style.display = 'block';
        return false;
    }
    
    input.style.borderColor = '';
    errorSpan.style.display = 'none';
    return true;
}

async function saveCommand() {
    const cmdId = document.getElementById('cmd-edit-id').value.trim();
    const name = document.getElementById('cmd-name').value.trim();
    let command = document.getElementById('cmd-command').value.trim();
    const desc = document.getElementById('cmd-desc').value.trim();
    const icon = document.getElementById('cmd-icon').value;
    const nohup = document.getElementById('cmd-nohup')?.checked || false;
    const expectPattern = document.getElementById('cmd-expect-pattern').value.trim();
    const failPattern = document.getElementById('cmd-fail-pattern').value.trim();
    const extractPattern = document.getElementById('cmd-extract-pattern').value.trim();
    const varName = document.getElementById('cmd-var-name').value.trim();
    const timeout = parseInt(document.getElementById('cmd-timeout').value) || 30;
    const stopOnMatch = document.getElementById('cmd-stop-on-match').checked;
    const isEditMode = document.getElementById('cmd-id-group').classList.contains('edit-mode');
    
    // 服务模式字段
    const serviceMode = document.getElementById('cmd-service-mode')?.checked || false;
    const readyPattern = document.getElementById('cmd-ready-pattern')?.value?.trim() || '';
    const serviceFailPattern = document.getElementById('cmd-service-fail-pattern')?.value?.trim() || '';
    const readyTimeout = parseInt(document.getElementById('cmd-ready-timeout')?.value) || 120;
    const readyInterval = parseInt(document.getElementById('cmd-ready-interval')?.value) || 5000;
    
    if (!name || !command) {
        showToast((typeof t === 'function' ? t('toast.fillCommandNameAndCmd') : '请填写指令名称和命令'), 'warning');
        return;
    }
    
    /* nohup 模式下自动检测并剥离用户多余的 nohup 包装。
     * 后端会在 nohup=true 时自动添加 nohup/重定向/PID 追踪，
     * 如果用户的命令里已经包含这些，会导致双重包装，日志只能读到 PID。 */
    if (nohup && command) {
        let cleaned = command;
        // 去掉开头的 nohup（后端会自己加）
        cleaned = cleaned.replace(/^\s*nohup\s+/, '');
        // 去掉尾部的 > /tmp/... 2>&1 & echo $! > /tmp/... 等 nohup 尾巴
        cleaned = cleaned.replace(/\s*>\s*\/tmp\/ts_nohup_\S+\.log\s+2>&1\s*&\s*echo\s+\$!\s*>\s*\/tmp\/ts_nohup_\S+\.pid\s*$/, '');
        // 更宽泛：去掉尾部的 > 任意路径.log 2>&1 & echo $! > 任意路径.pid
        cleaned = cleaned.replace(/\s*>\s*\S+\.log\s+2>&1\s*&\s*echo\s+\$!\s*>\s*\S+\.pid\s*$/, '');
        if (cleaned !== command) {
            document.getElementById('cmd-command').value = cleaned;
            command = cleaned;
            showToast((typeof t === 'function' ? t('toast.nohupStripped') : '已自动去除命令中多余的 nohup 包装（后端会自动添加）'), 'info');
        }
    }
    
    /* ID 验证（必填） */
    if (!cmdId) {
        showToast((typeof t === 'function' ? t('toast.fillCommandId') : '请填写指令 ID'), 'warning');
        document.getElementById('cmd-edit-id').focus();
        return;
    }
    if (!validateCommandId(document.getElementById('cmd-edit-id'))) {
        showToast((typeof t === 'function' ? t('toast.commandIdInvalid') : '指令 ID 格式不正确'), 'warning');
        document.getElementById('cmd-edit-id').focus();
        return;
    }
    
    // 服务模式验证
    if (nohup && serviceMode && !readyPattern) {
        showToast((typeof t === 'function' ? t('toast.serviceModeRequiresPattern') : '启用服务模式时必须设置就绪匹配模式'), 'warning');
        return;
    }
    if (nohup && serviceMode && !varName) {
        showToast((typeof t === 'function' ? t('toast.serviceModeRequiresVar') : '启用服务模式时必须设置变量名'), 'warning');
        return;
    }
    
    if (!sshCommands[selectedHostId]) {
        sshCommands[selectedHostId] = [];
    }
    
    const cmdData = { 
        name, command, desc, icon,
        // nohup 后台执行（优先于模式匹配）
        ...(nohup && { nohup: true }),
        // 高级选项（仅在有值时保存，nohup 时忽略）
        ...(!nohup && expectPattern && { expectPattern }),
        ...(!nohup && failPattern && { failPattern }),
        ...(!nohup && extractPattern && { extractPattern }),
        ...(varName && { varName }),  // varName 现在在 nohup 模式下也保留（用于服务模式）
        ...(!nohup && timeout !== 30 && { timeout }),
        ...(!nohup && stopOnMatch && { stopOnMatch }),
        // 服务模式字段（仅 nohup 时有效）
        ...(nohup && serviceMode && { serviceMode: true }),
        ...(nohup && serviceMode && readyPattern && { readyPattern }),
        ...(nohup && serviceMode && serviceFailPattern && { serviceFailPattern }),
        ...(nohup && serviceMode && readyTimeout !== 120 && { readyTimeout }),
        ...(nohup && serviceMode && readyInterval !== 5000 && { readyInterval })
    };
    
    try {
        /* 
         * ID 由用户在前端输入，直接传给后端
         * 后端会验证 ID 格式，如果 ID 已存在则执行更新
         */
        const savedId = await saveSshCommandToBackend(selectedHostId, cmdData, cmdId);
        cmdData.id = savedId;
        
        if (isEditMode) {
            // 编辑模式：更新本地缓存（根据 ID 查找）
            const existingIdx = sshCommands[selectedHostId].findIndex(c => c.id === cmdId);
            if (existingIdx >= 0) {
                sshCommands[selectedHostId][existingIdx] = cmdData;
            }
            showToast((typeof t === 'function' ? t('toast.commandUpdated') : '指令已更新'), 'success');
        } else {
            // 新建模式：添加到本地缓存
            sshCommands[selectedHostId].push(cmdData);
            showToast((typeof t === 'function' ? t('toast.commandCreated') : '指令已创建'), 'success');
        }
        
        closeCommandModal();
        refreshCommandsList();
        
    } catch (e) {
        console.error('Failed to save command:', e);
        showToast((typeof t === 'function' ? t('toast.saveCommandFailedMsg', { msg: e.message }) : '保存指令失败: ' + e.message), 'error');
    }
}

function editCommand(idx) {
    const cmd = sshCommands[selectedHostId]?.[idx];
    if (!cmd) return;
    
    document.getElementById('command-modal-title').textContent = typeof t === 'function' ? t('ssh.editCommand') : '编辑指令';
    
    /* 编辑模式：设置 ID 并标记为只读 */
    const idInput = document.getElementById('cmd-edit-id');
    const idGroup = document.getElementById('cmd-id-group');
    idInput.value = cmd.id || '';
    idInput.readOnly = true;
    idInput.style.backgroundColor = 'var(--fill)';
    idInput.style.cursor = 'not-allowed';
    idGroup.classList.add('edit-mode');
    
    document.getElementById('cmd-name').value = cmd.name;
    document.getElementById('cmd-command').value = cmd.command;
    document.getElementById('cmd-desc').value = cmd.desc || '';
    
    // 处理图标：判断是 RemixIcon/Emoji 还是图片路径
    const icon = cmd.icon || 'ri-rocket-line';
    document.getElementById('cmd-icon').value = icon;
    
    if (icon.startsWith('/sdcard/')) {
        // 图片路径
        document.getElementById('cmd-icon-type').value = 'image';
        document.getElementById('cmd-icon-path').value = icon;
        switchCmdIconType('image');
        updateCmdIconPreview(icon);
    } else {
        // 图标（RemixIcon）或旧版 Emoji
        document.getElementById('cmd-icon-type').value = 'emoji';
        document.getElementById('cmd-icon-path').value = '';
        switchCmdIconType('emoji');
        updateCmdIconPreview(null);
    }
    
    // nohup 选项
    const nohupCheckbox = document.getElementById('cmd-nohup');
    if (nohupCheckbox) {
        nohupCheckbox.checked = cmd.nohup || false;
    }
    
    // 高级选项
    document.getElementById('cmd-expect-pattern').value = cmd.expectPattern || '';
    document.getElementById('cmd-fail-pattern').value = cmd.failPattern || '';
    document.getElementById('cmd-extract-pattern').value = cmd.extractPattern || '';
    document.getElementById('cmd-var-name').value = cmd.varName || '';
    document.getElementById('cmd-timeout').value = cmd.timeout || 30;
    document.getElementById('cmd-stop-on-match').checked = cmd.stopOnMatch || false;
    
    // 服务模式选项
    const serviceModeCheckbox = document.getElementById('cmd-service-mode');
    if (serviceModeCheckbox) {
        serviceModeCheckbox.checked = cmd.serviceMode || false;
    }
    document.getElementById('cmd-ready-pattern').value = cmd.readyPattern || '';
    document.getElementById('cmd-service-fail-pattern').value = cmd.serviceFailPattern || '';
    document.getElementById('cmd-ready-timeout').value = cmd.readyTimeout || 120;
    document.getElementById('cmd-ready-interval').value = cmd.readyInterval || 5000;
    
    // 如果有高级选项，展开面板
    const advDetails = document.querySelector('.advanced-options');
    if (advDetails && (cmd.nohup || cmd.expectPattern || cmd.failPattern || cmd.extractPattern || cmd.varName || cmd.timeout !== 30 || cmd.stopOnMatch || cmd.serviceMode)) {
        advDetails.open = true;
    }
    
    // 更新图标选中状态
    const currentIconVal = cmd.icon || 'ri-rocket-line';
    document.querySelectorAll('.icon-btn').forEach(btn => {
        btn.classList.toggle('selected', btn.getAttribute('data-icon') === currentIconVal);
    });
    
    document.getElementById('command-modal').classList.remove('hidden');
    
    // 更新超时输入框状态
    updateTimeoutState();
    // 更新 nohup 状态（显示/隐藏服务模式选项）
    updateNohupState();
    // 更新服务模式状态
    updateServiceModeState();
}

/**
 * 导出 SSH 指令配置为 .tscfg 文件
 * 开发机：显示模态框输入目标证书
 * 非开发机：使用设备证书自加密
 */
async function exportSshCommand(cmdId) {
    // 确保已加载设备类型信息
    if (!window._configPackStatus) {
        try {
            const result = await api.configPackInfo();
            window._configPackStatus = result.data;
        } catch (e) {
            console.warn('无法获取设备类型信息，使用默认导出', e);
        }
    }
    
    // 检查设备类型
    const canExport = window._configPackStatus?.can_export;
    
    if (canExport) {
        // 开发机：显示模态框让用户输入目标证书和选项
        showExportSshCommandModal(cmdId);
    } else {
        // 非开发机：直接使用设备证书加密，询问是否包含主机
        const choice = await confirmSheet({ title: t('sshPage.exportSshCmdTitle'), body: t('ui.exportWithHostBody'), primary: t('ui.exportWithHost'), third: t('ui.exportCmdOnly') });
        if (!choice) return;
        await doExportSshCommand(cmdId, null, choice === true);
    }
}

/**
 * 显示导出 SSH 指令模态框（开发机专用）
 */
function showExportSshCommandModal(cmdId) {
    let modal = document.getElementById('export-ssh-cmd-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'export-ssh-cmd-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = exportSheet('ssh-cmd', t('sshPage.exportSshCmdTitle'), t('sshPage.exportSshCmdDesc', {cmdId: escapeHtml(cmdId)}), t('securityPage.targetCertHint'), 'hideExportSshCommandModal', 'doExportSshCommandFromModal',
        grp(row(t('ssh.includeHostConfig'), swc('export-ssh-cmd-include-host', true), '', t('ssh.includeHostConfigHint'))) + '<div style="height:12px"></div>');
    
    modal.dataset.exportId = cmdId;
    modal.classList.remove('hidden');
}

function hideExportSshCommandModal() {
    const modal = document.getElementById('export-ssh-cmd-modal');
    if (modal) modal.classList.add('hidden');
}

async function doExportSshCommandFromModal(cmdId) {
    const certText = document.getElementById('export-ssh-cmd-cert').value.trim();
    const includeHost = document.getElementById('export-ssh-cmd-include-host').checked;
    const resultBox = document.getElementById('export-ssh-cmd-result');
    const exportBtn = document.getElementById('export-ssh-cmd-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = (typeof t === 'function' ? t('securityPage.generatingPack') : '正在生成配置包...');
    exportBtn.disabled = true;
    
    try {
        await doExportSshCommand(cmdId, certText || null, includeHost);
        resultBox.className = 'result-box success';
        resultBox.textContent = (typeof t === 'function' ? t('toast.exportSuccess') : '导出成功！');
        setTimeout(() => hideExportSshCommandModal(), 1000);
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    } finally {
        exportBtn.disabled = false;
    }
}

/**
 * 执行导出 SSH 指令
 * @param {string} cmdId - 指令 ID
 * @param {string|null} recipientCert - 目标证书（null 使用设备证书）
 * @param {boolean} includeHost - 是否包含主机配置
 */
async function doExportSshCommand(cmdId, recipientCert, includeHost) {
    const params = { 
        id: cmdId,
        include_host: includeHost
    };
    if (recipientCert) {
        params.recipient_cert = recipientCert;
    }
    
    const result = await api.call('ssh.commands.export', params);
    
    if (result.code !== 0) {
        throw new Error(result.message || (typeof t === 'function' ? t('toast.exportFailed') : '导出失败'));
    }
    
    const data = result.data;
    if (!data?.tscfg) {
        throw new Error(typeof t === 'function' ? t('toast.invalidResponse') : '无效的响应数据');
    }
    
    // 下载文件
    const blob = new Blob([data.tscfg], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = data.filename || `${cmdId}.tscfg`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    
    const msg = data.host_included 
        ? (typeof t === 'function' ? t('toast.exportedCmdWithHost', {hostId: data.host_id, filename: data.filename}) : `已导出指令配置（包含主机 ${data.host_id}）: ${data.filename}`)
        : (typeof t === 'function' ? t('toast.exportedCmdConfig', {filename: data.filename}) : `已导出指令配置: ${data.filename}`);
    showToast(msg, 'success');
}

/**
 * 显示导入 SSH 指令配置弹窗
 */
async function showImportSshCommandModal() {
    let modal = document.getElementById('import-ssh-cmd-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'import-ssh-cmd-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    // 加载主机列表用于下拉选择
    let hostsOptions = `<option value="">-- ${typeof t === 'function' ? t('ssh.useConfigHost') : '使用配置中的主机'} --</option>`;
    try {
        const result = await api.call('ssh.hosts.list', {});
        const hosts = result.data?.hosts || [];
        for (const h of hosts) {
            hostsOptions += `<option value="${escapeHtml(h.id)}">${escapeHtml(h.id)} (${escapeHtml(h.host)}:${h.port})</option>`;
        }
    } catch (e) {
        console.warn('Failed to load hosts list:', e);
    }
    
    modal.innerHTML = importSheet('ssh-cmd', t('ssh.importSshCmdTitle'), t('ssh.importSshCmdDesc'), 'previewSshCommandImport', 'confirmSshCommandImport', 'hideImportSshCommandModal',
        `<div class="grp" style="margin-top:8px"><div class="row" id="import-ssh-cmd-host-group" style="display:none"><div class="rl">${t('ssh.importHostConfig')}</div><div class="rc">${swc('import-ssh-cmd-host', true)}</div></div>${row(t('ssh.bindToHost'), `<select class="field" id="import-ssh-cmd-target-host" style="width:220px">${hostsOptions}</select>`, '', t('ssh.bindToHostHint'))}</div>`);
    
    window._importSshCmdTscfg = null;
    modal.classList.remove('hidden');
}

function hideImportSshCommandModal() {
    const modal = document.getElementById('import-ssh-cmd-modal');
    if (modal) modal.classList.add('hidden');
    window._importSshCmdTscfg = null;
}

/**
 * 预览 SSH 指令导入内容
 */
async function previewSshCommandImport() {
    const fileInput = document.getElementById('import-ssh-cmd-file');
    const resultBox = document.getElementById('import-ssh-cmd-result');
    const step2 = document.getElementById('import-ssh-cmd-step2');
    const previewDiv = document.getElementById('import-ssh-cmd-preview');
    const hostGroup = document.getElementById('import-ssh-cmd-host-group');
    const importBtn = document.getElementById('import-ssh-cmd-btn');
    const statusEl = document.getElementById('import-ssh-cmd-file-status');
    
    if (!fileInput.files || !fileInput.files[0]) {
        if (statusEl) statusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件';
        return;
    }
    
    const file = fileInput.files[0];
    if (statusEl) statusEl.textContent = file.name;
    
    resultBox.classList.remove('hidden', 'success', 'error', 'warning');
    resultBox.textContent = (typeof t === 'function' ? t('ssh.verifyingPack') : '正在验证配置包...');
    importBtn.disabled = true;
    previewDiv.innerHTML = importPlaceholder('ssh-cmd');
    
    try {
        const content = await file.text();
        window._importSshCmdTscfg = content;
        window._importSshCmdFilename = file.name;  // 保存文件名
        
        const result = await api.call('ssh.commands.import', { 
            tscfg: content,
            filename: file.name,
            preview: true
        });
        
        if (result.code === 0 && result.data?.valid) {
            const data = result.data;
            renderImportPreview('ssh-cmd', data, data.type === 'ssh_command' ? t('ssh.sshCommand') : data.type);
            resultBox.className = 'result-box success';
            resultBox.textContent = (typeof t === 'function' ? t('ssh.signatureVerified') : '签名验证通过');
            importBtn.disabled = false;
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (result.message || (typeof t === 'function' ? t('ssh.cannotVerifyPack') : '无法验证配置包'));
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    }
}

/**
 * 确认导入 SSH 指令
 */
async function confirmSshCommandImport() {
    const overwrite = document.getElementById('import-ssh-cmd-overwrite').checked;
    const resultBox = document.getElementById('import-ssh-cmd-result');
    const importBtn = document.getElementById('import-ssh-cmd-btn');
    
    if (!window._importSshCmdTscfg) {
        showToast((typeof t === 'function' ? t('toast.selectFileFirst') : '请先选择文件'), 'error');
        return;
    }
    
    resultBox.classList.remove('hidden', 'success', 'error', 'warning');
    resultBox.textContent = (typeof t === 'function' ? t('ssh.savingConfig') : '正在保存配置...');
    importBtn.disabled = true;
    
    try {
        const params = { 
            tscfg: window._importSshCmdTscfg,
            filename: window._importSshCmdFilename,
            overwrite: overwrite
        };
        
        const result = await api.call('ssh.commands.import', params);
        
        if (result.code === 0) {
            const data = result.data;
            if (data?.exists && !data?.imported) {
                resultBox.className = 'result-box warning';
                resultBox.textContent = (typeof t === 'function' ? t('securityPage.configExistsCheckOverwrite', {id: data.id}) : `配置 ${data.id} 已存在，请勾选「覆盖」选项`);
                importBtn.disabled = false;
            } else {
                resultBox.className = 'result-box success';
                resultBox.innerHTML = `${typeof t === 'function' ? t('securityPage.savedConfig') : '已保存配置'}: <code>${escapeHtml(data?.id)}</code><br><small style="color:#6b7280">${typeof t === 'function' ? t('ssh.restartToTakeEffect') : '重启系统后生效'}</small>`;
                showToast((typeof t === 'function' ? t('toast.importedRestartRequired') : '已导入配置，重启后生效'), 'success');
                // 不刷新列表，因为还没加载
                setTimeout(() => hideImportSshCommandModal(), 2000);
            }
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (result.message || (typeof t === 'function' ? t('toast.importFailed') : '导入失败'));
            importBtn.disabled = false;
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
        importBtn.disabled = false;
    }
}

async function deleteCommand(idx) {
    const hostId = selectedHostId;
    const cmd = sshCommands[hostId]?.[idx];
    if (!cmd) return;
    const key = 'command:' + cmd.id;
    if (configurationDeletesInFlight.has(key)) return;
    configurationDeletesInFlight.add(key);
    const pageCurrent = capturePageValidity();
    try {
        if (!await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteCmd', { name: cmd.name }) : `确定要删除指令「${cmd.name}」吗？`, { primary: t('common.delete'), tone: 'danger' }) || !pageCurrent()) return;
        if (cmd.id) {
            showToast(t('toast.processing'), 'info');
            if (!await verifyStoppedServiceForDelete(cmd.id, pageCurrent) || !pageCurrent()) return;
            await deleteSshCommandFromBackend(cmd.id);
        }
        if (!pageCurrent()) return;
        const commands = sshCommands[hostId];
        const currentIndex = commands?.findIndex(item => item === cmd || (cmd.id && item.id === cmd.id));
        if (currentIndex >= 0) commands.splice(currentIndex, 1);
        if (selectedHostId === hostId) refreshCommandsList();
        showToast((typeof t === 'function' ? t('toast.commandDeleted') : '指令已删除'), 'success');
    } catch (e) {
        if (pageCurrent()) showToast((typeof t === 'function' ? t('toast.deleteCommandFailedMsg', { msg: e.message }) : '删除指令失败: ' + e.message), 'error');
    } finally {
        configurationDeletesInFlight.delete(key);
    }
}

/* 当前执行中的会话 ID */
let currentExecSessionId = null;

// A display belongs to one local invocation, then one acknowledged server operation.
let execDisplayOwner = null;
function releaseExecDisplay() {
    if (execDisplayOwner) execDisplayOwner.messages = [];
    execDisplayOwner = null;
    currentExecSessionId = null;
    if (tailIntervalId) clearInterval(tailIntervalId);
    tailIntervalId = null;
    const tail = document.getElementById('nohup-tail-log');
    const stop = document.getElementById('nohup-stop-tail');
    if (tail) tail.style.display = 'inline-block';
    if (stop) stop.style.display = 'none';
}
function claimExecDisplay(kind, element) {
    releaseExecDisplay();
    const owner = {kind, element, sessionId: null, finished: false, messages: [], bytes: 0};
    execDisplayOwner = owner;
    delete element.dataset.serviceCommand;
    delete element.dataset.operationId;
    delete element.dataset.operationPhase;
    const matchPanel = document.getElementById('match-result-panel');
    if (matchPanel) matchPanel.style.display = 'none';
    return owner;
}
function ownsExecDisplay(owner) {
    return owner && execDisplayOwner === owner && document.getElementById('exec-result') === owner.element;
}
function acknowledgeExecSession(owner, sessionId) {
    if (!ownsExecDisplay(owner)) return;
    owner.sessionId = sessionId;
    currentExecSessionId = sessionId;
    const buffered = owner.messages;
    owner.messages = [];
    owner.bytes = 0;
    if (owner.overflow) owner.element.textContent += '\n' + runtimeText('outcomeUnknown') + '\n';
    for (const message of buffered) handleSshExecMessage(message);
}

/* nohup 相关状态（用于快捷按钮） */
let currentNohupInfo = {
    logFile: null,
    processKeyword: null,
    hostId: null
};

/* nohup 快捷操作：查看日志 */
async function nohupViewLog() {
    if (!currentNohupInfo.logFile || !currentNohupInfo.hostId) {
        showToast((typeof t === 'function' ? t('toast.noLogInfo') : '没有可用的日志信息'), 'warning');
        return;
    }
    await executeNohupHelperCommand(`cat "${currentNohupInfo.logFile}"`);
}

/* nohup 实时跟踪状态 */
let tailIntervalId = null;
let lastTailContent = '';

/* nohup 快捷操作：实时跟踪 */
async function nohupTailLog() {
    if (!currentNohupInfo.logFile || !currentNohupInfo.hostId) {
        showToast((typeof t === 'function' ? t('toast.noLogInfo') : '没有可用的日志信息'), 'warning');
        return;
    }
    
    // 如果已在跟踪，则停止
    if (tailIntervalId) {
        nohupStopTail();
        return;
    }
    
    const tailBtn = document.getElementById('nohup-tail-log');
    const stopBtn = document.getElementById('nohup-stop-tail');
    const resultPre = document.getElementById('exec-result');
    const owner = claimExecDisplay('tail', resultPre);
    const info = {...currentNohupInfo};
    let fetching = false;
    
    // 切换按钮状态
    tailBtn.style.display = 'none';
    stopBtn.style.display = '';
    
    resultPre.textContent += `\n\n━━━━━━━━━━━━━━━━━━━━━━\n${typeof t === 'function' ? t('sshPage.startRealTimeTail', { logFile: currentNohupInfo.logFile }) : `开始实时跟踪: ${info.logFile}\n（点击"停止跟踪"按钮退出）`}\n━━━━━━━━━━━━━━━━━━━━━━\n`;
    lastTailContent = '';
    
    // 定时获取日志
    const fetchLog = async () => {
        if (!ownsExecDisplay(owner) || fetching || document.hidden) return;
        fetching = true;
        try {
            const host = window._cmdHostsList?.find(h => h.id === info.hostId);
            if (!host) return;
            
            const result = await api.call('ssh.exec', {
                host: host.host,
                port: host.port,
                user: host.username,
                keyid: host.keyid,
                command: `tail -100 "${info.logFile}" 2>/dev/null`,
                timeout_ms: 5000
            });
            
            if (!ownsExecDisplay(owner)) return;
            const content = result.data?.stdout || '';
            // 只显示新增内容
            if (content && content !== lastTailContent) {
                if (lastTailContent === '') {
                    resultPre.textContent += content;
                } else if (content.length > lastTailContent.length && content.startsWith(lastTailContent)) {
                    resultPre.textContent += content.substring(lastTailContent.length);
                } else {
                    // 内容完全变化，显示全部
                    resultPre.textContent += '\n' + content;
                }
                lastTailContent = content;
                resultPre.scrollTop = resultPre.scrollHeight;
            }
        } catch (e) {
            if (ownsExecDisplay(owner)) console.error('Tail log error:', e);
        } finally { fetching = false; }
    };
    
    // 立即获取一次
    await fetchLog();
    // 每2秒获取一次
    if (ownsExecDisplay(owner)) tailIntervalId = setInterval(fetchLog, 2000);
}

/* nohup 快捷操作：停止实时跟踪 */
function nohupStopTail() {
    if (tailIntervalId) {
        clearInterval(tailIntervalId);
        tailIntervalId = null;
    }
    
    const tailBtn = document.getElementById('nohup-tail-log');
    const stopBtn = document.getElementById('nohup-stop-tail');
    const resultPre = document.getElementById('exec-result');
    
    if (tailBtn) tailBtn.style.display = '';
    if (stopBtn) stopBtn.style.display = 'none';
    
    if (!resultPre || execDisplayOwner?.kind !== 'tail') return;
    releaseExecDisplay();
    resultPre.textContent += t('promptRepair.tailStopped');
    resultPre.scrollTop = resultPre.scrollHeight;
}

/* nohup 快捷操作：检查进程（使用 PID 文件） */
async function nohupCheckProcess() {
    if (currentNohupInfo.commandId) {
        const owner = execDisplayOwner;
        const state = await verifyServiceState(currentNohupInfo.commandId);
        if (state && ownsExecDisplay(owner)) owner.element.textContent += '\n' + runtimeText(state.state || 'unknown');
        return;
    }
    if (!currentNohupInfo.pidFile || !currentNohupInfo.hostId) {
        showToast((typeof t === 'function' ? t('toast.noProcessInfo') : '没有可用的进程信息'), 'warning');
        return;
    }
    // 使用 PID 文件检查进程状态，并显示进程详情
    await executeNohupHelperCommand(`if [ -f ${currentNohupInfo.pidFile} ]; then PID=$(cat ${currentNohupInfo.pidFile}); if kill -0 $PID 2>/dev/null; then echo "进程运行中 (PID: $PID)"; ps -p $PID -o pid,user,%cpu,%mem,etime,args --no-headers 2>/dev/null || ps -p $PID 2>/dev/null; else echo "进程已退出 (PID: $PID)"; fi; else echo "PID 文件不存在"; fi`);
}

/* nohup 快捷操作：停止进程（使用 PID 文件） */
async function nohupStopProcess() {
    if (currentNohupInfo.commandId) {
        await quickActionStopProcess(currentNohupInfo.commandId, false);
        return;
    }
    if (!currentNohupInfo.pidFile || !currentNohupInfo.hostId) {
        showToast((typeof t === 'function' ? t('toast.noProcessInfo') : '没有可用的进程信息'), 'warning');
        return;
    }
    
    // 确认对话框
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmStopProcess') : '确定要停止此后台进程吗？', { primary: t('common.stop'), tone: 'neutral' })) {
        return;
    }
    
    const info = {...currentNohupInfo};
    // 停止实时跟踪（如果正在进行）
    nohupStopTail();
    
    // 使用 PID 文件精确停止
    const owner = await executeNohupHelperCommand(`if [ -f ${info.pidFile} ]; then kill $(cat ${info.pidFile}) 2>/dev/null && rm -f ${info.pidFile} && echo "进程已停止"; else echo "PID 文件不存在"; fi`, info);
    if (!ownsExecDisplay(owner)) return;
    
    // 再次检查进程状态
    await executeNohupHelperCommand(`[ -f ${info.pidFile} ] && kill -0 $(cat ${info.pidFile}) 2>/dev/null && echo "进程仍在运行" || echo "确认：进程已停止"`, info);
}

/* 执行 nohup 辅助命令 */
async function executeNohupHelperCommand(command, info = currentNohupInfo) {
    const host = window._cmdHostsList?.find(h => h.id === info.hostId);
    if (!host) {
        showToast((typeof t === 'function' ? t('sshPage.hostNotFound') : '主机信息不存在'), 'error');
        return;
    }
    
    const resultPre = document.getElementById('exec-result');
    const owner = claimExecDisplay('log', resultPre);
    resultPre.textContent += `\n\n━━━━━━━━━━━━━━━━━━━━━━\n$ ${command}\n`;
    
    try {
        const result = await api.call('ssh.exec', {
            host: host.host,
            port: host.port,
            user: host.username,
            keyid: host.keyid,
            command: command,
            timeout_ms: 10000
        });
        
        // ssh.exec 返回 stdout 和 stderr，不是 output
        if (!ownsExecDisplay(owner)) return;
        const stdout = result.data?.stdout || '';
        const stderr = result.data?.stderr || '';
        if (stdout || stderr) {
            if (stdout) resultPre.textContent += stdout;
            if (stderr) resultPre.textContent += `[stderr] ${stderr}`;
        } else {
            resultPre.textContent += t('promptRepair.noOutput');
        }
    } catch (e) {
        if (!ownsExecDisplay(owner)) return;
        resultPre.textContent += t('promptRepair.executionFailed', {message: e.message});
    }
    
    // 滚动到底部
    resultPre.scrollTop = resultPre.scrollHeight;
    return owner;
}

/**
 * 服务模式：查看日志（从命令列表卡片调用）
 * @param {number} idx - 命令索引
 * @param {string} safeName - 安全名称（用于日志文件）
 */
async function viewServiceLog(idx, safeName) {
    const cmd = sshCommands[selectedHostId]?.[idx];
    if (!cmd) {
        showToast((typeof t === 'function' ? t('sshPage.cmdNotFound') : '命令不存在'), 'error');
        return;
    }
    
    const host = window._cmdHostsList?.find(h => h.id === selectedHostId);
    if (!host) {
        showToast((typeof t === 'function' ? t('sshPage.hostNotFound') : '主机信息不存在'), 'error');
        return;
    }
    
    const logFile = `/tmp/ts_nohup_${safeName}.log`;
    
    // 显示结果区域
    const resultSection = document.getElementById('exec-result-section');
    const resultPre = document.getElementById('exec-result');
    const owner = claimExecDisplay('log', resultPre);
    resultSection.style.display = '';
    document.getElementById('cancel-exec-btn').style.display = 'none';
    document.getElementById('nohup-actions').style.display = 'none';
    
    resultPre.textContent = (typeof t === 'function' ? t('sshPage.viewServiceLogFile', { name: cmd.name, file: logFile }) : `查看服务日志: ${cmd.name}\n文件: ${logFile}`) + '\n\n';
    resultSection.scrollIntoView({ behavior: 'smooth' });
    
    try {
        const result = await api.call('ssh.exec', {
            host: host.host,
            port: host.port,
            user: host.username,
            keyid: host.keyid,
            command: `tail -200 "${logFile}" 2>/dev/null || echo "日志文件不存在或为空"`,
            timeout_ms: 10000
        });
        
        if (!ownsExecDisplay(owner)) return;
        const stdout = result.data?.stdout || '';
        const stderr = result.data?.stderr || '';
        
        if (stdout) {
            resultPre.textContent += stdout;
        } else if (stderr) {
            resultPre.textContent += typeof t === 'function' ? t('sshPage.logErrorMsg', { msg: stderr }) : `[错误] ${stderr}`;
        } else {
            resultPre.textContent += (typeof t === 'function' ? t('ui.logEmpty') : '（日志为空）');
        }
    } catch (e) {
        if (!ownsExecDisplay(owner)) return;
        resultPre.textContent += typeof t === 'function' ? t('sshPage.getLogFailedMsg', { msg: e.message }) : `获取日志失败: ${e.message}`;
    }
    
    resultPre.scrollTop = resultPre.scrollHeight;
}

/**
 * 服务模式：停止进程（从命令列表卡片调用）
 * @param {number} idx - 命令索引
 * @param {string} safeName - 安全名称（用于 PID 文件）
 */
async function stopServiceProcess(idx) {
    const cmd = sshCommands[selectedHostId]?.[idx];
    if (cmd) await quickActionStopProcess(cmd.id, false);
}

const serviceStartInFlight = new Set();
async function executeManagedService(cmd, resultPre) {
    if (serviceStartInFlight.has(cmd.id)) return;
    serviceStartInFlight.add(cmd.id);
    const owner = claimExecDisplay('service', resultPre);
    advanceServiceState(cmd.id);
    const safeName = cmd.name.replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || String(cmd.id).replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || 'cmd';
    currentNohupInfo = {commandId: cmd.id, hostId: selectedHostId, logFile: `/tmp/ts_nohup_${safeName}.log`};
    resultPre.dataset.serviceCommand = cmd.id;
    delete resultPre.dataset.operationId;
    delete resultPre.dataset.operationPhase;
    resultPre.textContent = cmd.name + '\n' + runtimeText('starting') + '\n';
    try {
        const result = requireApiSuccess(await api.call('ssh.services.start', {command_id: cmd.id}, 'POST'), 'ssh.services.start');
        if (!Number.isInteger(result.data?.operation_id) || result.data.operation_id <= 0)
            throw new ApiOperationError({}, 'ssh.services.start', {kind: 'format', uncertain: true});
        if (!ownsExecDisplay(owner)) return;
        resultPre.dataset.operationId = String(result.data.operation_id);
        resultPre.textContent += runtimeText('launch_accepted');
    } catch (e) {
        if (ownsExecDisplay(owner))
            resultPre.textContent += e.uncertain || e.kind === 'timeout' || e.code === 5 ? runtimeText('outcomeUnknown') : e.message;
    } finally {
        advanceServiceState(cmd.id);
        serviceStartInFlight.delete(cmd.id);
        await updateServiceStatusInList();
    }
}

async function executeCommand(idx) {
    const cmd = sshCommands[selectedHostId]?.[idx];
    if (!cmd) return;
    
    const host = window._cmdHostsList?.find(h => h.id === selectedHostId);
    if (!host) {
        showToast((typeof t === 'function' ? t('sshPage.hostNotFound') : '主机信息不存在'), 'error');
        return;
    }
    
    // 检查是否有正在运行的命令（nohup 模式不需要检查）
    if (currentExecSessionId && !cmd.nohup) {
        showToast((typeof t === 'function' ? t('toast.commandRunning') : '有命令正在执行中，请先取消或等待完成'), 'warning');
        return;
    }
    
    // 显示结果区域
    const resultSection = document.getElementById('exec-result-section');
    const resultPre = document.getElementById('exec-result');
    const cancelBtn = document.getElementById('cancel-exec-btn');
    const nohupActions = document.getElementById('nohup-actions');
    resultSection.style.display = '';
    
    // nohup 模式下隐藏取消按钮，显示快捷按钮
    if (cmd.nohup) {
        cancelBtn.style.display = 'none';
        nohupActions.style.display = 'flex';
    } else {
        cancelBtn.style.display = '';
        cancelBtn.disabled = false;
        nohupActions.style.display = 'none';
    }
    
    if (cmd.nohup && cmd.serviceMode) {
        await executeManagedService(cmd, resultPre);
        return;
    }

    const owner = claimExecDisplay('ssh', resultPre);

    // 对于 nohup 命令，包装命令以实现后台执行，并记录日志和 PID
    let actualCommand = cmd.command;
    let nohupLogFile = null;
    let nohupPidFile = null;
    if (cmd.nohup) {
        // 基于命令名生成固定文件名（每次执行会覆盖），纯中文名 fallback 到 cmd.id
        const safeName = cmd.name.replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || String(cmd.id).replace(/[^a-zA-Z0-9]/g, '').slice(0, 20) || 'cmd';
        nohupLogFile = `/tmp/ts_nohup_${safeName}.log`;
        nohupPidFile = `/tmp/ts_nohup_${safeName}.pid`;
        
        // 使用 PID 文件方式（最可靠，能区分任意数量的不同命令）
        // 启动命令：nohup cmd > log 2>&1 & echo $! > pidfile; 检测启动
        actualCommand = `nohup ${cmd.command} > ${nohupLogFile} 2>&1 & echo $! > ${nohupPidFile}; sleep 0.3; cat ${nohupPidFile}`;
        
        // 保存 nohup 信息供快捷按钮使用
        currentNohupInfo = {
            logFile: nohupLogFile,
            pidFile: nohupPidFile,
            processKeyword: safeName,
            hostId: selectedHostId
        };
    }
    
    // 构建状态信息
    let statusInfo = (typeof t === 'function' ? t('sshPage.connecting') : 'Connecting') + `: ${cmd.name}\n` + (typeof t === 'function' ? t('securityPage.hostLabel') : 'Host') + `: ${host.username}@${host.host}:${host.port}\n` + (typeof t === 'function' ? t('common.command') : 'Command') + `: ${actualCommand}\n`;
    if (cmd.nohup) {
        statusInfo += `\n` + (typeof t === 'function' ? t('sshPage.nohupMode') : 'Background mode: command runs on server, unaffected by disconnect') + `\n`;
        statusInfo += t('promptRepair.logFile', {path: nohupLogFile});
    } else if (cmd.expectPattern || cmd.failPattern || cmd.extractPattern) {
        statusInfo += t('promptRepair.matchSettings');
        if (cmd.expectPattern) statusInfo += t('promptRepair.expectedPattern', {pattern: cmd.expectPattern});
        if (cmd.failPattern) statusInfo += t('promptRepair.failurePattern', {pattern: cmd.failPattern});
        if (cmd.extractPattern) statusInfo += t('promptRepair.extractPattern', {pattern: cmd.extractPattern});
        if (cmd.stopOnMatch) statusInfo += t('promptRepair.stopOnMatch');
        if (cmd.varName) statusInfo += t('promptRepair.storedVariable', {name: cmd.varName});
    }
    statusInfo += `\n`;
    resultPre.textContent = statusInfo;
    
    // 滚动到结果区域
    resultSection.scrollIntoView({ behavior: 'smooth' });
    
    try {
        // 构建 API 参数
        const params = {
            host: host.host,
            port: host.port,
            user: host.username,
            keyid: host.keyid,
            command: actualCommand
        };
        
        // 添加高级选项（仅非 nohup 模式）
        if (!cmd.nohup) {
            if (cmd.expectPattern) params.expect_pattern = cmd.expectPattern;
            if (cmd.failPattern) params.fail_pattern = cmd.failPattern;
            if (cmd.extractPattern) params.extract_pattern = cmd.extractPattern;
            if (cmd.varName) params.var_name = cmd.varName;
            if (cmd.timeout) params.timeout = cmd.timeout * 1000; // 转为毫秒
            if (cmd.stopOnMatch) params.stop_on_match = true;
        } else {
            // nohup 模式设置短超时，命令发送后立即返回
            params.timeout = 5000;
        }
        
        // 使用流式执行 API
        const result = requireApiSuccess(await api.call('ssh.exec_stream', params), 'ssh.exec_stream');
        if (!Number.isInteger(result.data?.session_id)) throw new ApiOperationError({}, 'ssh.exec_stream', {kind: 'format', uncertain: true});
        
        if (!ownsExecDisplay(owner)) return;
        
        if (cmd.nohup) {
            resultPre.textContent += t('promptRepair.sshAccepted');
            resultPre.textContent += t('promptRepair.sshCheckOutput');
            resultPre.textContent += t('promptRepair.logFileSpaced', {path: nohupLogFile});
            resultPre.textContent += t('promptRepair.processKeyword', {keyword: cmd.command.split(' ')[0]});
            // nohup 命令不跟踪会话
            // The display still follows this session; it is not a cancellable foreground command;
        } else {
            resultPre.textContent += t('promptRepair.sessionWaiting', {id: result.data.session_id});
        }
        
        acknowledgeExecSession(owner, result.data.session_id);
        if (cmd.nohup) currentExecSessionId = null;
        // 输出将通过 WebSocket 实时推送
        
    } catch (e) {
        if (!ownsExecDisplay(owner)) return;
        owner.finished = true;
        owner.messages = [];
        if (e.uncertain || e.kind === 'timeout' || e.code === 5 || e.code === 'TIMEOUT') {
            resultPre.textContent += '\n' + t('promptRepair.sshSubmissionUnknown');
            showToast(t('promptRepair.sshSubmissionUnknown'), 'warning', 10000);
            cancelBtn.style.display = 'none';
            return;
        }
        resultPre.textContent = typeof t === 'function' ? t('sshPage.startExecFailedDetail', { msg: e.message }) : `启动执行失败\n\n${e.message}`;
        showToast((typeof t === 'function' ? t('toast.startExecFailedMsg', { msg: e.message }) : '启动执行失败: ' + e.message), 'error');
        cancelBtn.style.display = 'none';
        currentExecSessionId = null;
    }
}

async function cancelExecution() {
    if (!currentExecSessionId) {
        showToast((typeof t === 'function' ? t('toast.noRunningCommand') : '没有正在执行的命令'), 'info');
        return;
    }
    
    const owner = execDisplayOwner;
    const sessionId = currentExecSessionId;
    const cancelBtn = document.getElementById('cancel-exec-btn');
    cancelBtn.disabled = true;
    cancelBtn.textContent = typeof t === 'function' ? t('ui.cancelling') : '取消中...';
    
    try {
        requireApiSuccess(await api.call('ssh.cancel', { session_id: sessionId }), 'call');
        if (!ownsExecDisplay(owner)) return;
        showToast((typeof t === 'function' ? t('toast.cancelSent') : '取消请求已发送'), 'info');
    } catch (e) {
        if (!ownsExecDisplay(owner)) return;
        showToast((typeof t === 'function' ? t('toast.cancelFailedMsg', { msg: e.message }) : '取消失败: ' + e.message), 'error');
        cancelBtn.disabled = false;
        cancelBtn.innerHTML = '<svg class="i"><use href="#ri-stop-line"/></svg> ' + (typeof t === 'function' ? t('sshPage.cancelEsc') : '取消 (Esc)');
    }
}

/* 处理 SSH Exec WebSocket 消息 */
function sshTerminalResult(msg) {
    const known = ['success', 'match_success', 'failed', 'match_failed', 'timeout', 'cancelled'];
    if (!known.includes(msg.status)) return {key: 'sshUnknown', type: 'warning'};
    const positive = msg.status === 'success' || msg.status === 'match_success';
    if ((positive && (msg.fail_matched === true || msg.success === false)) ||
        (!positive && msg.success === true) ||
        (msg.status === 'match_success' && msg.expect_matched === false) ||
        (msg.status === 'success' && msg.exit_code !== 0) ||
        (msg.status === 'match_failed' && msg.expect_matched === true && !msg.fail_matched)) {
        return {key: 'sshUnknown', type: 'warning'};
    }
    if (msg.status === 'timeout') return {key: 'sshTimeout', type: 'warning'};
    if (msg.status === 'cancelled') return {key: 'sshCancelled', type: 'info'};
    if (msg.status === 'match_failed') return {key: msg.fail_matched ? 'sshFailureOutput' : 'sshExpectedMissing', type: 'error'};
    if (msg.status === 'failed') return {key: 'sshFailed', type: 'error'};
    return {key: 'sshSuccess', type: 'success'};
}

function handleSshExecMessage(msg) {
    const resultPre = document.getElementById('exec-result');
    const cancelBtn = document.getElementById('cancel-exec-btn');
    const matchPanel = document.getElementById('match-result-panel');
    
    const owner = execDisplayOwner;
    if (!ownsExecDisplay(owner) || owner.kind !== 'ssh' || owner.finished) return;
    if (owner.sessionId === null) {
        const bytes = JSON.stringify(msg).length;
        if (owner.messages.length < 64 && owner.bytes + bytes <= 65536) {
            owner.messages.push(msg);
            owner.bytes += bytes;
        } else owner.overflow = true;
        return;
    }
    if (msg.session_id !== owner.sessionId) return;
    if (['ssh_exec_done', 'ssh_exec_error', 'ssh_exec_cancelled'].includes(msg.type)) owner.finished = true;
    
    switch (msg.type) {
        case 'ssh_exec_start':
            // 从 WebSocket 消息中获取 session_id
            // 总是更新 session_id，因为这是新执行的开始
            if (msg.session_id) {
                currentExecSessionId = msg.session_id;
                console.log('[SSH] Session ID from ssh_exec_start:', currentExecSessionId);
            }
            resultPre.textContent += t('promptRepair.sshBegin');
            // 隐藏匹配结果面板（新执行开始）
            if (matchPanel) matchPanel.style.display = 'none';
            break;
            
        case 'ssh_exec_output':
            // 接受消息如果：session_id 匹配，或者我们还没有 session_id（等待 API 返回）
            if (msg.session_id === owner.sessionId) {
                // 如果还没有 session_id，从消息中获取
                if (currentExecSessionId === null) {
                    currentExecSessionId = msg.session_id;
                    console.log('[SSH] Session ID from ssh_exec_output:', currentExecSessionId);
                }
                // 追加输出
                if (msg.is_stderr) {
                    resultPre.textContent += msg.data;
                } else {
                    resultPre.textContent += msg.data;
                }
                // 自动滚动到底部
                resultPre.scrollTop = resultPre.scrollHeight;
            }
            break;
            
        case 'ssh_exec_match':
            /* 实时匹配结果 */
            if (msg.session_id === owner.sessionId) {
                const isFinal = msg.is_final === true;  /* 是否为终止匹配（expect/fail 匹配）*/
                const isExtractOnly = !msg.expect_matched && !msg.fail_matched && msg.extracted;
                
                if (isFinal) {
                    /* 终止匹配（expect/fail 模式匹配成功）*/
                    resultPre.textContent += '\n' + t(msg.fail_matched ? 'promptRepair.sshFailureOutput' : 'promptRepair.sshExpectedFound') + '\n';
                    if (msg.expect_matched) {
                        resultPre.textContent += t('sshPage.expectMatch') + ': ' + t('common.yes') + '\n';
                    }
                    if (msg.fail_matched) {
                        resultPre.textContent += t('sshPage.failMatch') + ': ' + t('common.yes') + '\n';
                    }
                    if (msg.extracted) {
                        resultPre.textContent += t('promptRepair.extractedLine', {value: msg.extracted});
                    }
                    showToast(t(msg.fail_matched ? 'promptRepair.sshFailureOutput' : 'promptRepair.sshExpectedFound'), msg.fail_matched ? 'error' : 'info');
                } else if (isExtractOnly) {
                    /* 仅提取更新（持续提取场景）*/
                    /* 不在输出区显示，只更新面板 */
                }
                
                // 更新匹配结果面板
                updateMatchResultPanel(msg, isExtractOnly);
            }
            break;
            
        case 'ssh_exec_done':
            if (msg.session_id === owner.sessionId) {
                resultPre.textContent += t('promptRepair.sshEnd');
                resultPre.textContent += t('promptRepair.exitCode', {code: msg.exit_code ?? '?'});
                
                // 显示模式匹配结果
                if (msg.status) {
                    resultPre.textContent += t('promptRepair.' + sshTerminalResult(msg).key) + '\n';
                }
                
                // 显示期望模式匹配结果
                if (msg.expect_matched !== undefined) {
                    resultPre.textContent += t('sshPage.expectMatch') + ': ' + t(msg.expect_matched ? 'common.yes' : 'common.no') + '\n';
                }
                
                // 显示失败模式匹配结果
                if (msg.fail_matched !== undefined) {
                    resultPre.textContent += t('sshPage.failMatch') + ': ' + t(msg.fail_matched ? 'common.yes' : 'common.no') + '\n';
                }
                
                // 显示提取的内容
                if (msg.extracted) {
                    resultPre.textContent += t('promptRepair.extractedBlock', {value: msg.extracted});
                }
                
                // 更新匹配结果面板
                console.log('ssh_exec_done received:', JSON.stringify(msg, null, 2));
                updateMatchResultPanel(msg);
                
                if (cancelBtn) {
                    cancelBtn.style.display = 'none';
                }
                currentExecSessionId = null;
                
                const outcome = sshTerminalResult(msg);
                showToast(t('promptRepair.' + outcome.key), outcome.type);

            }
            break;
            
        case 'ssh_exec_error':
            if (msg.session_id === owner.sessionId) {
                resultPre.textContent += t('promptRepair.sshError', {message: apiErrorMessage({error: msg.error})});
                if (cancelBtn) {
                    cancelBtn.style.display = 'none';
                }
                currentExecSessionId = null;
                showToast((typeof t === 'function' ? t('toast.execErrorMsg', { msg: msg.error }) : '执行出错: ' + msg.error), 'error');
            }
            break;
            
        case 'ssh_exec_cancelled':
            if (msg.session_id === owner.sessionId) {
                resultPre.textContent += t('promptRepair.sshCancelOutput');
                if (cancelBtn) {
                    cancelBtn.style.display = 'none';
                }
                currentExecSessionId = null;
                showToast((typeof t === 'function' ? t('toast.commandCancelled') : '命令已取消'), 'info');
            }
            break;
    }
}

/* 更新匹配结果面板 */
function updateMatchResultPanel(msg, isExtractOnly = false) {
    const panel = document.getElementById('match-result-panel');
    if (!panel) {
        console.warn('match-result-panel not found');
        return;
    }
    
    console.log('updateMatchResultPanel called with:', msg, 'isExtractOnly:', isExtractOnly);
    
    // 始终显示面板（只要有匹配就显示）
    panel.style.display = '';
    
    // 更新状态徽章
    const statusBadge = document.getElementById('match-status-badge');
    if (statusBadge) {
        if (isExtractOnly) {
            // 持续提取模式 - 显示"提取中"
            statusBadge.textContent = typeof t === 'function' ? t('ui.extracting') : '提取中...';
            statusBadge.className = 'match-status extracting';
        } else {
            const outcome = sshTerminalResult(msg);
            statusBadge.textContent = t('promptRepair.' + outcome.key);
            statusBadge.className = 'match-status ' + (outcome.type === 'success' ? 'success' : 'failed');
        }
    }
    
    // 更新成功匹配结果
    const expectResult = document.getElementById('match-expect-result');
    if (expectResult) {
        if (msg.expect_matched !== undefined) {
            expectResult.textContent = t(msg.expect_matched ? 'common.yes' : 'common.no');
            expectResult.className = `match-value ${msg.expect_matched ? 'true' : 'false'}`;
        } else {
            expectResult.textContent = typeof t === 'function' ? t('ui.expectPatternConfigured') : '未配置';
            expectResult.className = 'match-value';
        }
    }
    
    // 更新失败匹配结果
    const failResult = document.getElementById('match-fail-result');
    if (failResult) {
        if (msg.fail_matched !== undefined) {
            failResult.textContent = msg.fail_matched ? (typeof t === 'function' ? t('sshPage.failMatchedTrue') : 'true (检测到错误)') : (typeof t === 'function' ? t('sshPage.failMatchedFalse') : 'false');
            failResult.className = `match-value ${msg.fail_matched ? 'false' : 'true'}`;
        } else {
            failResult.textContent = typeof t === 'function' ? t('ui.expectPatternConfigured') : '未配置';
            failResult.className = 'match-value';
        }
    }
    
    // 更新提取内容
    const extractedResult = document.getElementById('match-extracted-result');
    if (extractedResult) {
        if (msg.extracted) {
            extractedResult.textContent = msg.extracted;
            extractedResult.title = msg.extracted;
        } else {
            extractedResult.textContent = typeof t === 'function' ? t('ui.extractedNone') : '无';
        }
    }
    
    // 更新最终状态
    const finalStatus = document.getElementById('match-final-status');
    if (finalStatus) {
        const outcome = sshTerminalResult(msg);
        finalStatus.textContent = t('promptRepair.' + outcome.key);
        finalStatus.title = t('promptRepair.sshDiagnostics', {status: msg.status || '?', code: msg.exit_code ?? '?'});
    }
}

function clearExecResult() {
    releaseExecDisplay();
    document.getElementById('exec-result-section').style.display = 'none';
    document.getElementById('exec-result').textContent = '';
    document.getElementById('cancel-exec-btn').style.display = 'none';
    // 隐藏 nohup 快捷按钮
    const nohupActions = document.getElementById('nohup-actions');
    if (nohupActions) nohupActions.style.display = 'none';
    // 隐藏匹配结果面板
    const matchPanel = document.getElementById('match-result-panel');
    if (matchPanel) matchPanel.style.display = 'none';
    currentExecSessionId = null;
    // 清除 nohup 信息
    currentNohupInfo = { logFile: null, processKeyword: null, hostId: null };
}

// =========================================================================
//                         安全页面
// =========================================================================

async function submitRootPasswordSet() {
    const newPwdEl = document.getElementById('root-new-password');
    const confirmPwdEl = document.getElementById('root-confirm-password');
    const errorEl = document.getElementById('root-password-error');
    const newPwd = newPwdEl ? newPwdEl.value : '';
    const confirmPwd = confirmPwdEl ? confirmPwdEl.value : '';

    if (!errorEl) return;

    errorEl.classList.add('hidden');
    errorEl.textContent = '';

    if (newPwd !== confirmPwd) {
        errorEl.textContent = typeof t === 'function' ? t('securityPage.rootPasswordMismatch') : '两次输入的 root 新密码不一致';
        errorEl.classList.remove('hidden');
        return;
    }

    if (newPwd.length < 4 || newPwd.length > 64) {
        errorEl.textContent = typeof t === 'function' ? t('securityPage.rootPasswordLength') : 'root 密码长度必须为 4-64 个字符';
        errorEl.classList.remove('hidden');
        return;
    }

    try {
        const result = await api.setRootPassword(newPwd);
        if (result.success || result.code === 0 || result.code === 'OK') {
            if (newPwdEl) newPwdEl.value = '';
            if (confirmPwdEl) confirmPwdEl.value = '';
            api.passwordChanged = true;
            showToast(typeof t === 'function' ? t('securityPage.rootPasswordSetSuccess') : 'root 密码已更新', 'success');
        } else {
            errorEl.textContent = result.message || result.error || (typeof t === 'function' ? t('toast.saveFailed') : '保存失败');
            errorEl.classList.remove('hidden');
        }
    } catch (error) {
        errorEl.textContent = error.message || (typeof t === 'function' ? t('login.networkError') : '网络错误');
        errorEl.classList.remove('hidden');
    }
}

async function submitAdminPasswordSet() {
    const newPwdEl = document.getElementById('admin-new-password');
    const confirmPwdEl = document.getElementById('admin-confirm-password');
    const errorEl = document.getElementById('admin-password-error');
    const newPwd = newPwdEl ? newPwdEl.value : '';
    const confirmPwd = confirmPwdEl ? confirmPwdEl.value : '';

    if (!errorEl) return;

    errorEl.classList.add('hidden');
    errorEl.textContent = '';

    if (newPwd !== confirmPwd) {
        errorEl.textContent = typeof t === 'function' ? t('securityPage.adminPasswordMismatch') : '两次输入的 admin 新密码不一致';
        errorEl.classList.remove('hidden');
        return;
    }

    if (newPwd.length < 4 || newPwd.length > 64) {
        errorEl.textContent = typeof t === 'function' ? t('securityPage.adminPasswordLength') : 'admin 密码长度必须为 4-64 个字符';
        errorEl.classList.remove('hidden');
        return;
    }

    try {
        const result = await api.setAdminPassword(newPwd);
        if (result.success || result.code === 0 || result.code === 'OK') {
            if (newPwdEl) newPwdEl.value = '';
            if (confirmPwdEl) confirmPwdEl.value = '';
            showToast(typeof t === 'function' ? t('securityPage.adminPasswordSetSuccess') : 'admin 密码已更新', 'success');
        } else {
            errorEl.textContent = result.message || result.error || (typeof t === 'function' ? t('toast.saveFailed') : '保存失败');
            errorEl.classList.remove('hidden');
        }
    } catch (error) {
        errorEl.textContent = error.message || (typeof t === 'function' ? t('login.networkError') : '网络错误');
        errorEl.classList.remove('hidden');
    }
}

async function resetAdminPasswordToDefault() {
    const msg = typeof t === 'function'
        ? t('securityPage.confirmResetAdminPassword')
        : '确定要重置 admin 密码吗？\n\n此操作会将 admin 密码恢复为默认密码 rm01，并清除登录锁定状态。';

    if (!await confirmAction(msg, { primary: t('common.reset'), tone: 'danger' })) return;

    try {
        const result = await api.resetAdminPassword();
        if (result.success || result.code === 0 || result.code === 'OK') {
            showToast(typeof t === 'function' ? t('securityPage.adminPasswordResetSuccess') : 'admin 密码已重置为默认密码 rm01', 'success');
        } else {
            showToast(result.message || result.error || (typeof t === 'function' ? t('toast.saveFailed') : '操作失败'), 'error');
        }
    } catch (error) {
        showToast(error.message || (typeof t === 'function' ? t('login.networkError') : '网络错误'), 'error');
    }
}

function toggleAccountPasswordVisibility(inputId, button) {
    const input = document.getElementById(inputId);
    if (!input || !button) return;

    const showPassword = input.type === 'password';
    input.type = showPassword ? 'text' : 'password';

    const label = showPassword
        ? (typeof t === 'function' ? t('securityPage.hidePassword') : '隐藏密码')
        : (typeof t === 'function' ? t('securityPage.showPassword') : '显示密码');
    const icon = button.querySelector('i');
    if (icon) icon.className = 'ri-eye-line';
    button.classList.toggle('active', showPassword);
    button.title = label;
    button.setAttribute('aria-label', label);
}

async function loadSecurityPage() {
    const pageCurrent = capturePageValidity();
    clearInterval(refreshInterval);
    
    // 取消系统页面的订阅
    
    const content = document.getElementById('page-content');
    const showPasswordLabel = t('securityPage.showPassword');
    const pwField = (id, label, placeholder) => `
                    <div class="fl" style="flex:1">
                        <label>${label}</label>
                        <div class="pwf">
                            <input class="field lg" type="password" id="${id}" autocomplete="new-password" placeholder="${placeholder}" aria-label="${label}">
                            <button type="button" class="pwt" onclick="toggleAccountPasswordVisibility('${id}', this)" title="${showPasswordLabel}" aria-label="${showPasswordLabel}"><svg class="i"><use href="#ri-eye-line"/></svg></button>
                        </div>
                    </div>`;
    const pwCard = (title, desc, field1, field2, submitLabel, submitFn, errId) => `
                <div class="card">
                    <div class="t-section">${title}</div>
                    <div class="t-note" style="margin:2px 0 12px">${desc}</div>
                    <div class="pwrow">${field1}${field2}
                        <button class="btn lg primary" onclick="${submitFn}()">${submitLabel}</button>
                    </div>
                    <div id="${errId}" class="form-error hidden"></div>
                </div>`;
    const accountSecuritySection = api.isRoot() ? `
            <div>
                <div class="sec-h"><span class="t-section sec-t">${t('securityPage.accountSecurity')}</span><div class="acts"></div></div>
                <div class="t-note hint" style="margin-bottom:12px"><svg class="i"><use href="#ri-information-line"/></svg>${t('securityPage.accountSecurityDesc')}</div>
                <div style="display:grid;gap:12px">
                    ${pwCard(t('securityPage.rootPasswordManagement'), t('securityPage.rootPasswordManagementDesc'),
                        pwField('root-new-password', t('securityPage.newRootPassword'), t('securityPage.newRootPasswordPlaceholder')),
                        pwField('root-confirm-password', t('securityPage.confirmRootPassword'), t('securityPage.confirmRootPasswordPlaceholder')),
                        t('securityPage.setRootPassword'), 'submitRootPasswordSet', 'root-password-error')}
                    ${pwCard(t('securityPage.adminPasswordManagement'), t('securityPage.adminPasswordDescBrief'),
                        pwField('admin-new-password', t('securityPage.newAdminPassword'), t('securityPage.newAdminPasswordPlaceholder')),
                        pwField('admin-confirm-password', t('securityPage.confirmAdminPassword'), t('securityPage.confirmAdminPasswordPlaceholder')),
                        t('securityPage.setAdminPassword'), 'submitAdminPasswordSet', 'admin-password-error')}
                    <div class="card between" style="padding:16px 20px">
                        <div>
                            <div class="t-section">${t('securityPage.dangerZone')}</div>
                            <div class="t-note" style="margin-top:2px">${t('securityPage.resetAdminPasswordDesc')}</div>
                        </div>
                        <button class="btn dg" onclick="resetAdminPasswordToDefault()"><svg class="i"><use href="#ri-refresh-line"/></svg>${t('securityPage.resetAdminPassword')}</button>
                    </div>
                </div>
            </div>
            ` : '';
    content.innerHTML = `
        <div class="page page-security">
            ${accountSecuritySection}
            <div>
                <div class="sec-h">
                    <span class="t-section sec-t">${t('security.keyManagement')}</span>
                    <div class="acts"><button class="btn sm" onclick="showGenerateKeyModal()"><svg class="i"><use href="#ri-add-line"/></svg>${t('securityPage.generateNewKey')}</button></div>
                </div>
                <div class="card" style="padding:0">
                    <div class="tr th cols-keys"><div>${t('securityPage.keysTableId')}</div><div>${t('securityPage.keysTableType')}</div><div>${t('securityPage.keysTableComment')}</div><div>${t('securityPage.keysTableCreated')}</div><div>${t('securityPage.keysTableExportable')}</div><div class="ta-r">${t('securityPage.keysTableActions')}</div></div>
                    <div id="keys-table-body"></div>
                </div>
            </div>
            
            <div>
                <div class="sec-h">
                    <span class="t-section sec-t">${t('securityPage.deployedHosts')}</span>
                    <div class="acts"><button class="btn sm" onclick="showImportSshHostModal()"><svg class="i"><use href="#ri-download-line"/></svg>${t('securityPage.importHost')}</button></div>
                </div>
                <div class="t-note hint" style="margin-bottom:8px"><svg class="i"><use href="#ri-information-line"/></svg>${t('securityPage.hostsHint')}</div>
                <div class="card" style="padding:0">
                    <div class="tr th cols-hosts"><div>${t('securityPage.hostId')}</div><div>${t('securityPage.address')}</div><div>${t('securityPage.port')}</div><div>${t('securityPage.username')}</div><div>${t('securityPage.deployKey')}</div><div class="ta-r">${t('securityPage.keysTableActions')}</div></div>
                    <div id="ssh-hosts-table-body"></div>
                </div>
            </div>
            
            <div>
                <div class="sec-h"><span class="t-section sec-t" title="${escapeHtml(t('securityPage.fingerprintHint'))}">${t('securityPage.knownHostFingerprints')}</span><div class="acts"></div></div>
                <div class="card" style="padding:0">
                    <div class="tr th cols-known"><div>${t('securityPage.host')}</div><div>${t('securityPage.port')}</div><div>${t('securityPage.keyType')}</div><div>${t('securityPage.fingerprintSha256')}</div><div>${t('securityPage.addedTime')}</div><div>${t('securityPage.keysTableActions')}</div></div>
                    <div id="known-hosts-table-body"></div>
                </div>
            </div>
            
            <div>
                <div class="sec-h"><span class="t-section sec-t">${t('security.httpsCert')}</span><div class="acts"></div></div>
                <div id="cert-status-card" class="card">
                    <div class="between">
                        <span class="t-section"><span id="cert-status-icon"></span><span id="cert-status-text">${t('common.loading')}</span></span>
                        <span id="cert-expiry-badge" class="tag" style="display:none"></span>
                    </div>
                    <div class="cert-lines">
                        <span id="cert-material-state"></span>
                        <span id="cert-https-state"></span>
                        <span id="cert-blocked-state"></span>
                        <span id="cert-restart-state" role="status"></span>
                        <span id="cert-active-fingerprint" class="mono"></span>
                        <span id="cert-device-time"></span>
                    </div>
                    <div style="margin-top:12px"><button class="btn sm" id="cert-time-sync" onclick="syncCertificateTime()"><svg class="i"><use href="#ri-time-line"/></svg>${certText('pkiRepair.syncBrowser', 'Set device time from this computer')}</button></div>
                    <div id="cert-info-details" style="display:none">
                        <div class="kv-grid k2">
                            ${kvRow(t('securityPage.subjectCN'), '-', 'cert-subject-cn')}
                            ${kvRow(t('securityPage.issuer'), '-', 'cert-issuer-cn')}
                            ${kvRow(t('securityPage.notBefore'), '-', 'cert-not-before')}
                            ${kvRow(t('securityPage.notAfter'), '-', 'cert-not-after')}
                            ${kvRow(t('securityPage.serialNumber'), '-', 'cert-serial')}
                            ${kvRow(t('securityPage.validStatus'), '-', 'cert-valid-status')}
                        </div>
                    </div>
                    <div id="cert-no-key-hint" class="t-note" style="display:none;margin-top:8px;font-style:italic">${t('securityPage.noKeyHint')}</div>
                    <hr class="sep" style="margin:16px 0">
                    <div class="acts wrap">
                        <button class="btn sm" id="btn-cert-gen-key" onclick="showCertGenKeyModal()"><svg class="i"><use href="#ri-key-line"/></svg>${t('securityPage.genKeyPair')}</button>
                        <button class="btn sm" id="btn-cert-gen-csr" onclick="showCertCSRModal()" disabled><svg class="i"><use href="#ri-file-text-line"/></svg>${t('securityPage.genCsr')}</button>
                        <button class="btn sm" id="btn-cert-install" onclick="showCertInstallModal()" disabled><svg class="i"><use href="#ri-upload-line"/></svg>${t('securityPage.installCert')}</button>
                        <button class="btn sm" id="btn-cert-install-ca" onclick="showCertInstallCAModal()" disabled><svg class="i"><use href="#ri-shield-keyhole-line"/></svg>${t('securityPage.installCa')}</button>
                        <button class="btn sm" id="btn-cert-view" onclick="showCertViewModal()" disabled><svg class="i"><use href="#ri-eye-line"/></svg>${t('securityPage.viewCert')}</button>
                        <button class="btn sm dg" id="btn-cert-delete" onclick="deleteCertCredentials()" disabled><svg class="i"><use href="#ri-delete-bin-line"/></svg>${t('securityPage.deleteCredentials')}</button>
                    </div>
                </div>
            </div>
            
            <div>
                <div class="sec-h"><span class="t-section sec-t">${t('securityPage.configPack')}</span><div class="acts"></div></div>
                <div id="config-pack-status-card" class="card">
                    <div class="between">
                        <span class="t-section"><span id="pack-status-icon"></span><span id="pack-status-text">${t('common.loading')}</span></span>
                        <span id="pack-device-type-badge" class="tag" style="display:none"></span>
                    </div>
                    <div id="pack-info-details" style="display:none">
                        <div class="kv-grid k4">
                            <div>${kvRow(t('securityPage.deviceType'), '-', 'pack-device-type')}</div>
                            <div>${kvRow(t('securityPage.certCN'), '-', 'pack-cert-cn')}</div>
                            <div>${kvRow(t('securityPage.certFingerprintShort'), '-', 'pack-cert-fp')}</div>
                            <div>${kvRow(t('securityPage.formatVersion'), '-', 'pack-version')}</div>
                        </div>
                    </div>
                    <div class="t-note hint" style="margin:8px 0 16px"><svg class="i"><use href="#ri-information-line"/></svg>${t('securityPage.configPackDesc')}</div>
                    <hr class="sep" style="margin-bottom:16px">
                    <div class="acts wrap">
                        <button class="btn sm" onclick="showConfigPackExportCertModal()"><svg class="i"><use href="#ri-download-line"/></svg>${t('securityPage.exportDeviceCert')}</button>
                        <button class="btn sm" onclick="showConfigPackImportModal()"><svg class="i"><use href="#ri-upload-line"/></svg>${t('securityPage.importConfigPack')}</button>
                        <button class="btn sm" id="btn-pack-export" onclick="showConfigPackExportModal()" disabled><svg class="i"><use href="#ri-download-line"/></svg>${t('securityPage.configPackExport')}</button>
                        <button class="btn sm" onclick="showConfigPackListModal()"><svg class="i"><use href="#ri-file-text-line"/></svg>${t('securityPage.viewPackList')}</button>
                    </div>
                </div>
            </div>
            
            <!-- 配置包：导出设备证书弹窗 -->
            <div class="modal hidden" id="pack-export-cert-modal">${sheet(560, t('securityPage.exportCertTitle'), `
                <div class="t-note" style="margin-bottom:12px">${t('securityPage.exportCertDesc')}</div>
                <div id="pack-export-cert-loading" class="t-note" style="text-align:center;padding:20px">${t('common.loading')}</div>
                <div id="pack-export-cert-content" class="hidden">
                    ${grp(row(t('securityPage.certFingerprint'), '<input class="field mono" id="pack-cert-fingerprint" readonly style="width:300px">') + row(t('securityPage.certCn'), '<input class="field" id="pack-cert-cn-display" readonly style="width:300px">'))}
                    ${gt(t('securityPage.certPem'))}
                    <textarea class="field mono" id="pack-cert-pem" readonly style="height:150px"></textarea>
                </div>`,
                `<button class="btn lg" onclick="copyPackCertToClipboard()"><svg class="i"><use href="#ri-file-copy-line"/></svg>${t('common.copyToClipboard')}</button><button class="btn lg primary" onclick="hideConfigPackExportCertModal()">${t('common.close')}</button>`)}</div>
            
            <!-- 配置包：导入弹窗 -->
            <div class="modal hidden" id="pack-import-modal">${sheet(560, t('securityPage.importPackTitle'), `
                <div class="acts" style="align-items:center;gap:12px;margin-bottom:12px" title="${escapeHtml(t('securityPage.importPackDesc'))}">
                    <input type="file" id="pack-import-file" accept=".tscfg,.json" onchange="handlePackFileSelect(event)" style="position:absolute;opacity:0;width:0;height:0;pointer-events:none">
                    <button type="button" class="btn" onclick="document.getElementById('pack-import-file').click()"><svg class="i"><use href="#ri-upload-line"/></svg>${t('common.selectFile')}</button>
                    <span id="pack-import-file-status" class="t-note">${t('common.noFileSelected')}</span>
                </div>
                <div class="fl">
                    <label>${t('securityPage.configPackContent')}</label>
                    <textarea class="field mono" id="pack-import-content" style="height:96px" placeholder='{"tscfg_version":"1.0", ...}'></textarea>
                </div>
                <div id="pack-import-result" class="result-box hidden" style="margin-top:12px"></div>
                <div id="pack-import-preview" class="hidden" style="margin-top:12px">${gt(t('securityPage.configPackInfo'))}<div id="pack-preview-content"></div></div>`,
                `<button class="btn lg" onclick="hideConfigPackImportModal()">${t('common.cancel')}</button><button class="btn lg" onclick="verifyConfigPack()">${t('securityPage.verifyOnly')}</button><button class="btn lg primary" onclick="importConfigPack()">${t('common.import')}</button>`)}</div>
            
            <!-- 配置包：导出弹窗（仅 Developer 可用） -->
            <div class="modal hidden" id="pack-export-modal">${sheet(760, t('securityPage.configPackExport'), `
                ${gt(t('securityPage.packExportPick'))}
                ${grp(row(t('securityPage.dirShort'), `<input type="text" class="field mono" id="pack-export-browse-path" value="/sdcard/config" readonly style="width:200px"><button class="btn sm" onclick="packExportBrowseUp()">${t('files.parentFolder')}</button><button class="btn icon sm" onclick="packExportBrowseRefresh()" title="${escapeHtml(t('common.refresh'))}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>`))}
                <div class="grp" id="pack-export-file-list" style="margin-top:8px;max-height:180px;overflow-y:auto"><div class="row"><div class="rl t-note">${t('common.loading')}</div></div></div>
                <div class="acts" style="margin-top:8px">
                    <button class="btn sm" onclick="packExportSelectAll()">${t('common.selectAll')}</button>
                    <button class="btn sm" onclick="packExportDeselectAll()">${t('common.deselectAll')}</button>
                    <button class="btn sm" onclick="packExportSelectDir()">${t('securityPage.selectDirectory')}</button>
                </div>
                <div id="pack-export-selected" class="result-box" style="margin-top:8px;display:none;white-space:pre-wrap"><span id="pack-export-selected-file"></span></div>
                ${gt(t('securityPage.configPackInfo'))}
                ${grp(row(t('common.name'), inp('pack-export-name', 200, t('securityPage.autoFromFilename'), '', 'required')) + row(t('common.description'), inp('pack-export-desc', 200, t('securityPage.configDescPlaceholder'))))}
                ${gt(t('securityPage.recipientCert'))}
                <textarea class="field mono" id="pack-export-recipient-cert" style="height:64px" placeholder="-----BEGIN CERTIFICATE-----" title="${escapeHtml(t('securityPage.pasteTargetCert'))}" required></textarea>
                <div id="pack-export-result" class="result-box hidden" style="margin-top:12px"></div>
                ${gt(t('securityPage.tscfgLabel'))}
                <div id="pack-export-output">
                    <textarea class="field mono" id="pack-export-tscfg" readonly style="height:64px" placeholder="${t('securityPage.packWillShowHere')}"></textarea>
                    <div id="pack-export-saved-path" class="t-note" style="display:none;margin-top:6px"></div>
                </div>`,
                `<button class="btn lg" id="btn-pack-copy" onclick="copyPackTscfgToClipboard()" style="display:none"><svg class="i"><use href="#ri-file-copy-line"/></svg>${t('common.copyToClipboard')}</button><button class="btn lg" id="btn-pack-download" onclick="downloadPackTscfg()" style="display:none"><svg class="i"><use href="#ri-download-line"/></svg>${t('securityPage.downloadToLocal')}</button><button class="btn lg" onclick="hideConfigPackExportModal()">${t('common.cancel')}</button><button class="btn lg primary" id="btn-pack-export-generate" onclick="exportConfigPack()" disabled>${t('securityPage.generateConfigPack')}</button>`)}</div>
            
            <div class="modal hidden" id="pack-list-modal">${sheet(560, t('securityPage.packListTitle'), `
                <div style="display:flex;gap:8px;margin-bottom:12px">
                    <input type="text" class="field mono" id="pack-list-path" value="/sdcard/config" style="flex:1" placeholder="${t('common.path')}" aria-label="${t('securityPage.packListDirPath')}">
                    <button class="btn icon sm" onclick="refreshConfigPackList()" title="${escapeHtml(t('common.refresh'))}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                </div>
                <div id="pack-list-loading" class="t-note" style="text-align:center;padding:16px">${t('common.loading')}</div>
                <div class="card hidden" id="pack-list-table" style="padding:0;background:var(--fill)"><div id="pack-list-tbody"></div></div>`,
                `<button class="btn lg primary" onclick="hideConfigPackListModal()">${t('securityPage.close')}</button>`, 'hideConfigPackListModal()')}</div>
            
            <!-- 生成密钥弹窗 -->
            <div class="modal hidden" id="keygen-modal">${sheet(560, t('securityPage.generateNewKey'),
                grp(
                    row(t('securityPage.keyId'), inp('keygen-id', 200, t('securityPage.keyIdPlaceholder'), 'mono', 'required')) +
                    `<div class="row"><div class="rl">${t('securityPage.keysTableType')}<small id="keygen-ec-warn" class="hidden">${t('securityPage.ecdsaWarning')}</small></div><div class="rc"><select class="field" id="keygen-type" style="width:200px" onchange="document.getElementById('keygen-ec-warn').classList.toggle('hidden', this.value.indexOf('ec') !== 0)">
                        <option value="rsa2048" selected>${t('securityPage.rsaRecommended')}</option>
                        <option value="rsa4096">RSA 4096-bit</option>
                        <option value="ec256">ECDSA P-256</option>
                        <option value="ec384">ECDSA P-384</option>
                    </select></div></div>` +
                    row(t('securityPage.keysTableComment'), inp('keygen-comment', 200, t('securityPage.commentPlaceholder'))) +
                    row(t('sshPage.keyAlias'), inp('keygen-alias', 200, t('securityPage.aliasPlaceholder')), '', t('securityPage.aliasHint')) +
                    row(t('securityPage.keysTableExportable'), swc('keygen-exportable'), '', t('securityPage.allowExportPrivateKey')) +
                    row(t('securityPage.hideShort'), swc('keygen-hidden'), '', t('securityPage.hideKeyIdHint'))) +
                `<div id="keygen-status" class="t-note" role="status" aria-live="polite" style="white-space:pre-wrap;margin-top:12px"></div>`,
                `<button class="btn lg" id="keygen-close" onclick="hideGenerateKeyModal()">${t('common.cancel')}</button><button class="btn lg primary" id="keygen-submit" onclick="generateKey()">${t('common.generate')}</button>`)}</div>
            
            <!-- 部署密钥弹窗 -->
            <div class="modal hidden" id="deploy-key-modal">${sheet(560, t('securityPage.deployKeyBrief'), `
                <div class="t-note" style="margin-bottom:8px">${t('securityPage.deployKeyDescPre')} <span class="mono" id="deploy-key-id"></span> ${t('securityPage.deployKeyDescPost')}</div>
                ${grp(
                    row(t('securityPage.hostLabel'), inp('deploy-host', 200, t('securityPage.hostPlaceholder'), 'mono', 'required')) +
                    row(t('securityPage.username'), inp('deploy-user', 200, 'root', '', 'required')) +
                    row(t('securityPage.port'), inp('deploy-port', 80, '', '', 'type="number" min="1" max="65535" value="22"')) +
                    row(t('securityPage.sshLoginPassword'), inp('deploy-password', 200, t('securityPage.passwordPlaceholder'), '', 'type="password" required')))}
                <div id="deploy-result" class="result-box hidden" style="margin-top:12px"></div>`,
                `<button class="btn lg" onclick="hideDeployKeyModal()">${t('common.cancel')}</button><button class="btn lg primary" id="deploy-btn" onclick="deployKey()" title="${escapeHtml(t('securityPage.deployInfoHint'))}">${t('securityPage.startDeploy')}</button>`)}</div>
            
            <!-- 撤销密钥弹窗 -->
            <div class="modal hidden" id="revoke-key-modal">${sheet(560, t('securityPage.revokeKeyTitle'), `
                <div class="t-note" style="margin-bottom:8px">${t('securityPage.revokeKeyDescPre')} <span class="mono" id="revoke-key-id"></span></div>
                ${grp(
                    row(t('securityPage.hostLabel'), inp('revoke-host', 210, t('securityPage.hostPlaceholder'), 'mono', 'required')) +
                    row(t('securityPage.username'), inp('revoke-user', 210, 'root', '', 'required')) +
                    row(t('securityPage.port'), inp('revoke-port', 80, '', '', 'type="number" min="1" max="65535" value="22"')) +
                    row(t('securityPage.sshLoginPassword'), inp('revoke-password', 210, t('securityPage.passwordPlaceholder'), '', 'type="password" required')))}
                <div class="warnrow"><svg class="i"><use href="#ri-alert-line"/></svg><span>${t('securityPage.revokeWarning')}</span></div>
                <div id="revoke-result" class="result-box hidden" style="margin-top:12px"></div>`,
                `<button class="btn lg" onclick="hideRevokeKeyModal()">${t('common.cancel')}</button><button class="btn lg primary bad" id="revoke-btn" onclick="revokeKey()">${t('securityPage.revokeKey')}</button>`)}</div>
            
            <!-- 主机指纹不匹配警告弹窗 -->
            <div class="modal hidden" id="host-mismatch-modal">${sheet(560, t('securityPage.mismatchTitle'), `
                <div class="t-body" style="margin-bottom:8px">${t('securityPage.hostKeyChangedWarning')}</div>
                <div class="t-body" style="color:var(--ink-2);line-height:22px;margin-bottom:14px">• ${t('securityPage.mitmAttack')}<br>• ${t('securityPage.serverReinstalled')}<br>• ${t('securityPage.ipReassigned')}</div>
                ${grp(
                    row(t('securityPage.hostLabel'), '<span class="mono" id="mismatch-host"></span>') +
                    row(t('securityPage.storedFingerprint'), '<span class="mono t-label" id="mismatch-stored-fp"></span>') +
                    row(t('securityPage.currentFingerprint'), '<span class="mono t-label" id="mismatch-current-fp"></span>'))}
                <div class="t-note" style="margin-top:12px;line-height:18px">${t('securityPage.mismatchAdvice')}</div>`,
                `<button class="btn lg" onclick="hideHostMismatchModal()">${t('common.cancel')}</button><button class="btn lg primary bad" onclick="removeAndRetry()">${t('securityPage.updateHostKey')}</button>`)}</div>
            
            <!-- HTTPS 证书：生成密钥对弹窗 -->
            <div class="modal hidden" id="cert-genkey-modal">${sheet(520, t('securityPage.genHttpsKeyTitle'), `
                <div class="t-body" style="color:var(--ink-2)">${t('securityPage.genHttpsKeyDescFull')}</div>
                <div id="cert-genkey-existing-warning" class="warnrow hidden"><svg class="i"><use href="#ri-alert-line"/></svg><span>${t('securityPage.existingKeyWarning')}</span></div>
                <div id="cert-genkey-result" class="result-box hidden" style="margin-top:12px"></div>`,
                `<button class="btn lg" onclick="hideCertGenKeyModal()">${t('common.cancel')}</button><button class="btn lg primary" id="cert-genkey-btn" onclick="generateCertKeypair()">${t('common.generate')}</button>`)}</div>
            
            <!-- HTTPS 证书：生成/查看 CSR 弹窗 -->
            <div class="modal hidden" id="cert-csr-modal">${sheet(660, t('securityPage.csrGenerateTitle'), `
                ${grp(
                    row(t('securityPage.deviceIdCn'), inp('csr-device-id', 210, 'TIANSHAN-RM01-0001', 'mono'), '', t('securityPage.leaveEmptyForDefault')) +
                    row(t('securityPage.orgBrief'), inp('csr-org', 210, t('securityPage.orgBrief'))) +
                    row(t('securityPage.deptBrief'), inp('csr-ou', 210, 'Device')))}
                ${gt(t('securityPage.csrPemLabel'))}<div id="csr-result-box"><textarea class="field mono" id="csr-pem-output" readonly style="height:90px" placeholder="${t('securityPage.csrPlaceholder')}" title="${escapeHtml(t('securityPage.csrContentLabel'))}"></textarea></div>
                <div id="csr-gen-result" class="result-box hidden" style="margin-top:12px"></div>`,
                `<button class="btn lg" onclick="copyCSRToClipboard()"><svg class="i"><use href="#ri-file-copy-line"/></svg>${t('common.copyToClipboard')}</button><button class="btn lg" onclick="hideCertCSRModal()">${t('common.close')}</button><button class="btn lg primary" id="csr-gen-btn" onclick="generateCSR()">${t('securityPage.csrGenerateTitle')}</button>`)}</div>
            
            <!-- HTTPS 证书：安装证书弹窗 -->
            <div class="modal hidden" id="cert-install-modal">${sheet(560, t('securityPage.installCert'), `
                <textarea class="field mono" id="cert-pem-input" style="height:120px" placeholder="-----BEGIN CERTIFICATE-----" title="${escapeHtml(t('securityPage.installCertDesc'))}"></textarea>
                <div id="cert-install-result" class="result-box hidden" style="margin-top:12px"></div>`,
                `<button class="btn lg" onclick="hideCertInstallModal()">${t('common.cancel')}</button><button class="btn lg primary" id="cert-install-submit" onclick="installCertificate()">${t('common.install')}</button>`)}</div>
            
            <!-- HTTPS 证书：安装 CA 链弹窗 -->
            <div class="modal hidden" id="cert-ca-modal">${sheet(560, t('securityPage.installCa'), `
                <textarea class="field mono" id="ca-pem-input" style="height:120px" placeholder="-----BEGIN CERTIFICATE-----" title="${escapeHtml(t('securityPage.installCaDesc'))}"></textarea>
                <div id="ca-install-result" class="result-box hidden" style="margin-top:12px"></div>`,
                `<button class="btn lg" onclick="hideCertInstallCAModal()">${t('common.cancel')}</button><button class="btn lg primary" id="cert-ca-submit" onclick="installCAChain()">${t('common.install')}</button>`)}</div>
            
            <!-- HTTPS 证书：查看证书弹窗 -->
            <div class="modal hidden" id="cert-view-modal">${sheet(560, t('securityPage.viewCert'), `
                <div id="cert-view-loading" class="t-note" style="text-align:center;padding:20px">${t('common.loading')}</div>
                <div id="cert-view-content" class="hidden"><textarea class="field mono" id="cert-view-pem" readonly style="height:140px"></textarea></div>`,
                `<button class="btn lg" onclick="copyCertToClipboard()"><svg class="i"><use href="#ri-file-copy-line"/></svg>${t('common.copyToClipboard')}</button><button class="btn lg primary" onclick="hideCertViewModal()">${t('common.close')}</button>`)}</div>
        </div>
    `;
    
    await refreshSecurityPage();
    if (!pageCurrent()) return;
}

let securityKeysLoadVersion = 0;
async function refreshSecurityPage({keysOnly = false} = {}) {
    const pageCurrent = capturePageValidity();
    const keysVersion = ++securityKeysLoadVersion;
    let keysLoaded = false;
    // 密钥列表
    const tbody = document.getElementById('keys-table-body');
    let allKeysHtml = '';
    let sshKeys = [];
    
    // 1. 加载 SSH 密钥
    try {
        const keys = requireApiSuccess(await api.keyList(), 'key.list');
        if (!Array.isArray(keys.data?.keys)) throw new Error(t('promptRepair.invalidResponse'));
        keysLoaded = true;
        if (!pageCurrent()) return;
        const sshKeySelect = document.getElementById('ssh-keyid');
        
        // 更新 SSH 测试的密钥下拉列表
        if (sshKeySelect && keysVersion === securityKeysLoadVersion) {
            sshKeySelect.innerHTML = '<option value="">' + (typeof t === 'function' ? t('sshPage.selectKey') : '-- 选择密钥 --') + '</option>';
            if (keys.data?.keys && keys.data.keys.length > 0) {
                keys.data.keys.forEach(key => {
                    const option = document.createElement('option');
                    option.value = key.id;
                    // 隐藏密钥显示别名或掩码 ID，否则显示真实 ID
                    const displayName = (key.hidden && key.alias) ? key.alias : key.id;
                    option.textContent = `${key.hidden ? t('promptRepair.hiddenPrefix') : ''}${displayName} (${key.type_desc || key.type})`;
                    sshKeySelect.appendChild(option);
                });
            }
        }
        
        if (keys.data?.keys && keys.data.keys.length > 0) {
            sshKeys = keys.data.keys;
            allKeysHtml += keys.data.keys.map(key => {
                // 隐藏密钥显示别名，否则显示真实 ID
                const displayId = (key.hidden && key.alias) ? key.alias : key.id;
                const hiddenIcon = key.hidden ? '<svg class="i"><use href="#ri-lock-line"/></svg> ' : '';
                
                return `
                <div class="tr cols-keys">
                    <div>
                        <span class="mono">${hiddenIcon}${escapeHtml(displayId)}</span>
                        ${key.alias && !key.hidden ? `<div class="t-note">${escapeHtml(key.alias)}</div>` : ''}
                    </div>
                    <div>${escapeHtml(key.type_desc || key.type)}</div>
                    <div><span class="tag">SSH</span> ${(key.comment === 'use for control') ? t('securityPage.commentUseForControl') : (escapeHtml(key.comment) || '-')}</div>
                    <div>${formatTimestamp(key.created)}</div>
                    <div>${key.exportable ? t('common.yes') : t('common.no')}</div>
                    <div class="act">
                        <button class="btn sm" onclick="exportKey('${escapeHtml(key.id)}')" ${key.has_pubkey ? '' : 'disabled'}><svg class="i"><use href="#ri-download-line"/></svg>${t('securityPage.publicKey')}</button>
                        <button class="btn sm" onclick="exportPrivateKey('${escapeHtml(key.id)}')" ${key.exportable === false ? 'disabled' : ''} title="${key.exportable === false ? t('securityPage.cannotExportPrivateKey') : t('securityPage.exportPrivateKey')}"><svg class="i"><use href="#ri-key-line"/></svg>${t('securityPage.privateKey')}</button>
                        <button class="btn sm" onclick="showDeployKeyModal('${escapeHtml(key.id)}')" ${key.has_pubkey ? '' : 'disabled'} title="${t('securityPage.deployToServer')}"><svg class="i"><use href="#ri-upload-line"/></svg>${t('securityPage.deploy')}</button>
                        <button class="btn sm dg" onclick="showRevokeKeyModal('${escapeHtml(key.id)}')" ${key.has_pubkey ? '' : 'disabled'} title="${t('securityPage.revokeFromServer')}">${t('securityPage.revoke')}</button>
                        <button class="btn sm dg" onclick="deleteKey('${escapeHtml(key.id)}')">${t('common.delete')}</button>
                    </div>
                </div>
                `;
            }).join('');
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('加载 SSH 密钥失败:', e);
    }
    
    // 2. 加载 HTTPS 密钥（来自 ts_cert）
    try {
        const certStatus = await api.certStatus();
        if (!pageCurrent()) return;
        console.log('HTTPS cert status:', certStatus);
        
        if (certStatus.code === 0) {
            // 字段名是 has_private_key，不是 has_keypair
            const hasKeypair = certStatus.data?.has_private_key;
            const hasCert = certStatus.data?.has_certificate;
            const certInfo = certStatus.data?.cert_info || {};
            
            if (hasKeypair) {
                // 已有密钥对
                const comment = hasCert ? `CN=${certInfo.subject_cn || 'unknown'}` : t('securityPage.noCertInstalled');
                
                allKeysHtml += `
                <div class="tr cols-keys">
                    <div><span class="mono">https</span></div>
                    <div>ECDSA P-256</div>
                    <div><span class="tag">HTTPS</span> ${escapeHtml(comment)}</div>
                    <div>-</div>
                    <div>${t('common.no')}</div>
                    <div class="act">
                        <button class="btn sm" onclick="showCertCSRModal()" title="${t('securityPage.generateCsrTitle')}"><svg class="i"><use href="#ri-file-text-line"/></svg>CSR</button>
                        <button class="btn sm" onclick="showCertViewModal()" ${hasCert ? '' : 'disabled'} title="${t('securityPage.viewCert')}"><svg class="i"><use href="#ri-eye-line"/></svg>${t('securityPage.cert')}</button>
                        <button class="btn sm dg" onclick="deleteCertCredentials()" title="${t('securityPage.deleteHttpsKeyAndCert')}">${t('common.delete')}</button>
                    </div>
                </div>
                `;
            } else {
                // 未生成密钥对，显示提示行
                allKeysHtml += `
                <div class="tr cols-keys">
                    <div><span class="mono">https</span></div>
                    <div>-</div>
                    <div><span class="tag">HTTPS</span> <i class="t-note">${t('securityPage.noKeyGenerated')}</i></div>
                    <div>-</div>
                    <div>-</div>
                    <div class="act">
                        <button class="btn sm" onclick="showCertGenKeyModal()" title="${t('securityPage.generateHttpsKey')}"><svg class="i"><use href="#ri-key-line"/></svg>${t('securityPage.generateKey')}</button>
                    </div>
                </div>
                `;
            }
        }
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('加载 HTTPS 密钥状态失败:', e);
    }
    
    // A failed or superseded SSH load must not erase the existing key table.
    const keysCurrent = pageCurrent() && keysVersion === securityKeysLoadVersion;
    if (keysLoaded && keysCurrent && tbody) {
        tbody.innerHTML = allKeysHtml || `<div class="tr" style="--cols:1fr;color:var(--ink-3)">${t('securityPage.noKeysClickToGenerate')}</div>`;
    }
    const keyResult = {keysLoaded, keysCurrent};
    if (keysOnly) return keyResult;

    // SSH 已部署主机列表（加载数据并渲染到 DOM）
    await refreshSshHostsList();
    if (!pageCurrent()) return;
    
    // 已知主机指纹列表
    await refreshKnownHostsList();
    if (!pageCurrent()) return;
    
    // HTTPS 证书状态
    await refreshCertStatus();
    if (!pageCurrent()) return;
    
    // Config Pack 状态
    await refreshConfigPackStatus();
    if (!pageCurrent()) return;
    return keyResult;
}

/**
 * 刷新安全页面的已部署主机列表
 */
/**
 * 仅加载 SSH hosts 数据到 window._sshHostsData（不渲染 DOM）
 */
let sshHostsLoadVersion = 0;

async function loadSshHostsData() {
    const version = ++sshHostsLoadVersion;
    const pageCurrent = capturePageValidity();
    try {
        const result = requireApiSuccess(await api.call('ssh.hosts.list', {}), 'ssh.hosts.list');
        if (!pageCurrent()) return false;
        if (version !== sshHostsLoadVersion) return null; // Superseded, leave the newer list alone.
        const hosts = result.data?.hosts;
        if (!Array.isArray(hosts)) throw new Error(t('promptRepair.invalidResponse'));
        const data = {};
        hosts.forEach(h => { data[h.id] = h; });
        window._sshHostsData = data;
        console.log('loadSshHostsData: loaded', Object.keys(window._sshHostsData).length, 'hosts');
        return true;
    } catch (e) {
        if (!pageCurrent()) return false;
        if (version !== sshHostsLoadVersion) return null;
        console.error('loadSshHostsData error:', e);
        return false;
    }
}

async function refreshSshHostsList() {
    const pageCurrent = capturePageValidity();
    // 首先加载 SSH hosts 数据（无需 DOM）
    const loaded = await loadSshHostsData();
    if (!pageCurrent() || loaded === null) return false;
    
    const tbody = document.getElementById('ssh-hosts-table-body');
    if (!tbody) return loaded;  // DOM 渲染部分可选
    
    try {
        if (!loaded) throw new Error(t('common.loadFailed'));
        const hosts = Object.values(window._sshHostsData || {});
        window._sshHostsList = hosts;
        
        if (hosts.length === 0) {
            tbody.innerHTML = `<div class="tr" style="--cols:1fr;color:var(--ink-3)">${t('securityPage.noDeployedHostsHint')}</div>`;
            return true;
        }
        
        tbody.innerHTML = hosts.map((h, idx) => `
            <div class="tr cols-hosts">
                <div><span class="mono">${escapeHtml(h.id)}</span></div>
                <div>${escapeHtml(h.host)}</div>
                <div>${h.port}</div>
                <div>${escapeHtml(h.username)}</div>
                <div><span class="tag">${escapeHtml(h.keyid || 'default')}</span></div>
                <div class="act">
                    <button class="btn sm" onclick="testSshHostByIndex(${idx})" title="${t('securityPage.testConnection')}">${t('common.test')}</button>
                    <button class="btn sm" onclick="exportSshHost('${escapeHtml(h.id)}')" title="${t('securityPage.exportAsTscfg')}">${t('common.export')}</button>
                    <button class="btn sm dg" onclick="revokeKeyFromHost(${idx})" title="${t('securityPage.revokePubkey')}">${t('securityPage.revoke')}</button>
                    <button class="btn sm dg" onclick="removeHostByIndex(${idx})" title="${t('securityPage.removeLocalRecord')}">${t('securityPage.remove')}</button>
                </div>
            </div>
        `).join('');
        
        return true;
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('Refresh SSH hosts error:', e);
        tbody.innerHTML = `<div class="tr" style="--cols:1fr"><span class="state bad">${t('common.loadFailed')}</span></div>`;
        return false;
    }
}

/**
 * 刷新已知主机指纹列表
 */
async function refreshKnownHostsList() {
    const pageCurrent = capturePageValidity();
    const tbody = document.getElementById('known-hosts-table-body');
    if (!tbody) return;
    
    try {
        const result = await api.call('hosts.list', {});
        if (!pageCurrent()) return;
        const hosts = result.data?.hosts || [];
        
        if (hosts.length === 0) {
            tbody.innerHTML = `<div class="tr" style="--cols:1fr;color:var(--ink-3)">${t('securityPage.noKnownHostFingerprints')}</div>`;
            return;
        }
        
        // 存储已知主机列表
        window._knownHostsList = hosts;
        
        tbody.innerHTML = hosts.map((h, idx) => `
            <div class="tr cols-known">
                <div><span class="mono">${escapeHtml(h.host)}</span></div>
                <div>${h.port}</div>
                <div><span class="tag">${escapeHtml(h.type)}</span></div>
                <div><span class="mono">${escapeHtml(h.fingerprint.substring(0, 32))}...</span></div>
                <div>${formatTimestamp(h.added)}</div>
                <div class="act" style="justify-content:flex-start">
                    <button class="btn sm" onclick="showFullFingerprint(${idx})" title="${t('securityPage.viewFullFingerprint')}">${t('common.view')}</button>
                    <button class="btn sm dg" onclick="removeKnownHost(${idx})" title="${t('securityPage.deleteFingerprint')}">${t('common.delete')}</button>
                </div>
            </div>
        `).join('');
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('Refresh known hosts error:', e);
        tbody.innerHTML = `<div class="tr" style="--cols:1fr"><span class="state bad">${t('common.loadFailed')}</span></div>`;
    }
}

/**
 * 显示完整指纹
 */
function showFullFingerprint(index) {
    const host = window._knownHostsList?.[index];
    if (!host) return;
    
    document.getElementById('fingerprint-info-modal')?.remove();
    const modal = document.createElement('div');
    modal.id = 'fingerprint-info-modal';
    modal.className = 'modal';
    modal.onclick = e => { if (e.target === modal) modal.remove(); };
    modal.innerHTML = sheet(520, t('ui.fingerprintTitle'),
        grp(row(t('common.host'), `<span class="mono">${escapeHtml(host.host)}:${escapeHtml(host.port)}</span>`) +
            row(t('common.type'), `<span class="mono">${escapeHtml(host.type)}</span>`)) +
        `<div class="gt">${t('ui.fingerprintLabel')}</div><div class="grp" style="padding:12px 16px"><div class="mono" id="fingerprint-info-value" style="user-select:text;word-break:break-all">${escapeHtml(host.fingerprint)}</div></div>`,
        `<button type="button" class="btn lg" onclick="copyFingerprintInfo()">${t('ui.copyBtn')}</button><button type="button" class="btn lg primary" onclick="document.getElementById('fingerprint-info-modal').remove()">${t('common.close')}</button>`);
    document.body.appendChild(modal);
}

async function copyFingerprintInfo() {
    const text = document.getElementById('fingerprint-info-value')?.textContent || '';
    try {
        await navigator.clipboard.writeText(text);
        showToast(t('toast.copied'), 'success');
    } catch (e) {
        showToast(t('toast.copyFailedMsg', { msg: e.message }), 'error');
    }
}

/**
 * 删除已知主机指纹
 */
async function removeKnownHost(index) {
    const host = window._knownHostsList?.[index];
    if (!host) return;
    
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteFingerprint', { host: host.host, port: host.port }) : `确定要删除主机 ${host.host}:${host.port} 的指纹记录吗？\n\n删除后下次连接将重新验证服务器指纹。`, { primary: t('common.delete'), tone: 'danger' })) return;
    
    try {
        const result = await api.call('hosts.remove', { host: host.host, port: host.port });
        if (result.code === 0) {
            showToast((typeof t === 'function' ? t('toast.hostFingerprintDeleted') : '已删除主机指纹'), 'success');
            await refreshKnownHostsList();
        } else {
            showToast((typeof t === 'function' ? t('toast.deleteFailedMsg', { msg: result.message || t('common.unknown') }) : '删除失败: ' + (result.message || '未知错误')), 'error');
        }
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.deleteFailedMsg', { msg: e.message }) : '删除失败: ' + e.message), 'error');
    }
}

/**
 * 测试 SSH 连接
 */
async function testSshConnection(hostId) {
    showToast(typeof t === 'function' ? t('toast.testingConnection', { host: hostId }) : `正在测试连接 ${hostId}...`, 'info');
    
    try {
        // 获取主机信息
        const hostResult = await api.call('ssh.hosts.get', { id: hostId });
        console.log('ssh.hosts.get result:', hostResult);
        
        if (hostResult.code !== 0) {
            showToast(typeof t === 'function' ? t('toast.cannotGetHostInfo') + ': ' + (hostResult.message || t('common.unknown')) : `无法获取主机信息: ${hostResult.message || '未知错误'}`, 'error');
            return;
        }
        
        if (!hostResult.data) {
            showToast((typeof t === 'function' ? t('toast.hostInfoEmpty') : '主机信息为空'), 'error');
            return;
        }
        
        const host = hostResult.data;
        
        // 执行 ssh.exec 测试连接（执行简单命令）
        const execResult = await api.call('ssh.exec', {
            host: host.host,
            port: host.port,
            username: host.username,
            keyid: host.keyid || 'default',
            command: 'echo "TianshanOS SSH Test OK"'
        });
        
        if (execResult.code === 0) {
            showToast(typeof t === 'function' ? t('toast.connectionSuccess', { host: hostId }) : `连接 ${hostId} 成功！`, 'success');
        } else {
            showToast(typeof t === 'function' ? t('toast.connectFailedMsg', { msg: execResult.message || t('common.unknown') }) : `连接失败: ${execResult.message || '未知错误'}`, 'error');
        }
    } catch (e) {
        console.error('Test SSH connection error:', e);
        showToast(typeof t === 'function' ? t('toast.testConnectionFailed') + ': ' + e.message : `测试失败: ${e.message}`, 'error');
    }
}

/**
 * 通过索引测试 SSH 连接（避免 ID 中的特殊字符问题）
 */
async function testSshHostByIndex(index) {
    const host = window._sshHostsList?.[index];
    if (!host) {
        showToast((typeof t === 'function' ? t('sshPage.hostNotFound') : '主机信息不存在'), 'error');
        return;
    }
    
    showToast(typeof t === 'function' ? t('toast.testingConnection', { host: host.id }) : `正在测试连接 ${host.id}...`, 'info');
    
    try {
        const params = {
            host: host.host,
            port: host.port,
            user: host.username,  // API 需要 'user' 而不是 'username'
            keyid: host.keyid || 'default',
            command: 'echo "TianshanOS SSH Test OK"',
            trust_new: false
        };
        let execResult = await api.call('ssh.exec', params);
        if ([1001, 1002].includes(execResult.code)) {
            const fingerprint = execResult.data?.fingerprint || execResult.data?.current_fingerprint;
            if (fingerprint && await confirmSheet({
                title: t('ui.fingerprintTitle'),
                bodyHtml: `${escapeHtml(runtimeText('confirmFingerprint'))}<div class="mono" style="margin-top:10px;user-select:text;word-break:break-all;white-space:pre-wrap">${escapeHtml(host.host + ':' + host.port)}\n${escapeHtml(fingerprint)}</div>`,
                primary: t('ui.trustConnect') })) {
                params.confirmed_fingerprint = fingerprint;
                params.trust_new = execResult.code === 1002;
                params.accept_changed = execResult.code === 1001;
                execResult = await api.call('ssh.exec', params);
            }
        }
        
        if (execResult.code === 0) {
            showToast(typeof t === 'function' ? t('toast.connectionSuccess', { host: host.id }) : `连接 ${host.id} 成功！`, 'success');
        } else {
            showToast(typeof t === 'function' ? t('toast.connectFailedMsg', { msg: execResult.message || t('common.unknown') }) : `连接失败: ${execResult.message || '未知错误'}`, 'error');
        }
    } catch (e) {
        console.error('Test SSH connection error:', e);
        showToast(typeof t === 'function' ? t('toast.testConnectionFailed') + ': ' + e.message : `测试失败: ${e.message}`, 'error');
    }
}

/**
 * 导出 SSH 主机配置为 .tscfg 文件
 * 开发机：显示模态框输入目标证书
 * 非开发机：使用设备证书自加密
 */
async function exportSshHost(hostId) {
    // 确保已加载设备类型信息
    if (!window._configPackStatus) {
        try {
            const result = await api.configPackInfo();
            window._configPackStatus = result.data;
        } catch (e) {
            console.warn('无法获取设备类型信息，使用默认导出', e);
        }
    }
    
    // 检查设备类型
    const canExport = window._configPackStatus?.can_export;
    
    if (canExport) {
        // 开发机：显示模态框让用户输入目标证书
        showExportSshHostModal(hostId);
    } else {
        // 非开发机：直接使用设备证书加密
        await doExportSshHost(hostId, null);
    }
}

/**
 * 显示导出 SSH 主机模态框（开发机专用）
 */
function showExportSshHostModal(hostId) {
    let modal = document.getElementById('export-ssh-host-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'export-ssh-host-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = exportSheet('ssh-host', t('securityPage.exportSshHostTitle'), t('securityPage.exportSshHostDesc', {hostId: escapeHtml(hostId)}), t('securityPage.exportSshHostCertHint'), 'hideExportSshHostModal', 'doExportSshHostFromModal');
    
    modal.dataset.exportId = hostId;
    modal.classList.remove('hidden');
}

function hideExportSshHostModal() {
    const modal = document.getElementById('export-ssh-host-modal');
    if (modal) modal.classList.add('hidden');
}

async function doExportSshHostFromModal(hostId) {
    const certText = document.getElementById('export-ssh-host-cert').value.trim();
    const resultBox = document.getElementById('export-ssh-host-result');
    const exportBtn = document.getElementById('export-ssh-host-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = typeof t === 'function' ? t('securityPage.generatingPack') : 'Generating config pack...';
    exportBtn.disabled = true;
    
    try {
        await doExportSshHost(hostId, certText || null);
        resultBox.className = 'result-box success';
        resultBox.textContent = typeof t === 'function' ? t('toast.exportSuccess') : 'Export successful!';
        setTimeout(() => hideExportSshHostModal(), 1000);
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    } finally {
        exportBtn.disabled = false;
    }
}

/**
 * 执行导出 SSH 主机
 * @param {string} hostId - 主机 ID
 * @param {string|null} recipientCert - 目标证书（null 使用设备证书）
 */
async function doExportSshHost(hostId, recipientCert) {
    const params = { id: hostId };
    if (recipientCert) {
        params.recipient_cert = recipientCert;
    }
    
    const result = await api.call('ssh.hosts.export', params);
    
    if (result.code !== 0) {
        throw new Error(result.message || t('toast.exportFailed'));
    }
    
    const data = result.data;
    if (!data?.tscfg) {
        throw new Error(t('toast.invalidResponse'));
    }
    
    // 下载文件
    const blob = new Blob([data.tscfg], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = data.filename || `${hostId}.tscfg`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    
    showToast(typeof t === 'function' ? t('toast.hostConfigExported', { filename: data.filename }) : '已导出主机配置: ' + data.filename, 'success');
}

/**
 * 显示导入 SSH 主机配置弹窗
 */
function showImportSshHostModal() {
    let modal = document.getElementById('import-ssh-host-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'import-ssh-host-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = importSheet('ssh-host', t('securityPage.importSshHostTitle'), t('securityPage.importSshHostDesc'), 'previewSshHostImport', 'confirmSshHostImport', 'hideImportSshHostModal', '', 520, t('common.confirmImport'));
    
    // 存储 tscfg 内容
    window._importSshHostTscfg = null;
    
    modal.classList.remove('hidden');
}

function hideImportSshHostModal() {
    const modal = document.getElementById('import-ssh-host-modal');
    if (modal) modal.classList.add('hidden');
    window._importSshHostTscfg = null;
}

/**
 * 预览 SSH 主机导入内容
 */
async function previewSshHostImport() {
    const fileInput = document.getElementById('import-ssh-host-file');
    const resultBox = document.getElementById('import-ssh-host-result');
    const step2 = document.getElementById('import-ssh-host-step2');
    const previewDiv = document.getElementById('import-ssh-host-preview');
    const importBtn = document.getElementById('import-ssh-host-btn');
    const statusEl = document.getElementById('import-ssh-host-file-status');
    
    if (!fileInput.files || !fileInput.files[0]) {
        if (statusEl) statusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : 'No file selected';
        return;
    }
    
    const file = fileInput.files[0];
    if (statusEl) statusEl.textContent = file.name;
    
    resultBox.classList.remove('hidden', 'success', 'error', 'warning');
    resultBox.textContent = (typeof t === 'function' ? t('ssh.verifyingPack') : '正在验证配置包...');
    importBtn.disabled = true;
    previewDiv.innerHTML = importPlaceholder('ssh-host');
    
    try {
        const content = await file.text();
        window._importSshHostTscfg = content;
        window._importSshHostFilename = file.name;  // 保存文件名
        
        // 预览模式调用（轻量级验证，不解密）
        const result = await api.call('ssh.hosts.import', { 
            tscfg: content,
            filename: file.name,
            preview: true
        });
        
        if (result.code === 0 && result.data?.valid) {
            const data = result.data;
            renderImportPreview('ssh-host', data, '');
            resultBox.className = 'result-box success';
            resultBox.textContent = typeof t === 'function' ? t('ssh.signatureVerified') : '签名验证通过';
            importBtn.disabled = false;
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (result.message || (typeof t === 'function' ? t('ssh.cannotVerifyPack') : '无法验证配置包'));
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    }
}

/**
 * 确认导入 SSH 主机
 */
async function confirmSshHostImport() {
    const overwrite = document.getElementById('import-ssh-host-overwrite').checked;
    const resultBox = document.getElementById('import-ssh-host-result');
    const importBtn = document.getElementById('import-ssh-host-btn');
    
    if (!window._importSshHostTscfg) {
        showToast((typeof t === 'function' ? t('toast.selectFileFirst') : '请先选择文件'), 'error');
        return;
    }
    
    resultBox.classList.remove('hidden', 'success', 'error', 'warning');
    resultBox.textContent = typeof t === 'function' ? t('ssh.savingConfig') : '正在保存配置...';
    importBtn.disabled = true;
    
    try {
        const result = await api.call('ssh.hosts.import', { 
            tscfg: window._importSshHostTscfg,
            filename: window._importSshHostFilename,
            overwrite: overwrite
        });
        
        if (result.code === 0) {
            const data = result.data;
            if (data?.exists && !data?.imported) {
                resultBox.className = 'result-box warning';
                resultBox.textContent = typeof t === 'function' ? t('securityPage.configExistsCheckOverwrite', {id: data.id}) : `Config ${data.id} exists, check "Overwrite" option`;
                importBtn.disabled = false;
            } else {
                resultBox.className = 'result-box success';
                resultBox.innerHTML = `${typeof t === 'function' ? t('securityPage.savedConfig') : 'Saved config'}: <code>${escapeHtml(data?.id)}</code><br><small style="color:#6b7280">${typeof t === 'function' ? t('securityPage.restartToApply') : 'Restart to apply'}</small>`;
                showToast(typeof t === 'function' ? t('toast.importedRestartRequired') : 'Imported, restart to apply', 'success');
                // 不刷新列表，因为还没加载
                setTimeout(() => hideImportSshHostModal(), 2000);
            }
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (result.message || (typeof t === 'function' ? t('toast.importFailed') : 'Import failed'));
            importBtn.disabled = false;
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
        importBtn.disabled = false;
    }
}

/**
 * 通过索引移除主机记录
 */
async function removeHostByIndex(index) {
    const host = window._sshHostsList?.[index];
    if (!host) {
        showToast((typeof t === 'function' ? t('sshPage.hostNotFound') : '主机信息不存在'), 'error');
        return;
    }
    
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmRemoveHostLocal', { id: host.id }) : `确定要从列表中移除主机 "${host.id}" 吗？\n\n注意：这只会移除本地记录，不会删除已部署到服务器上的公钥。如需撤销公钥，请点击「撤销」按钮。`, { primary: t('securityPage.remove'), tone: 'danger' })) return;
    
    try {
        const result = await api.call('ssh.hosts.remove', { id: host.id });
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.sshHostRemoved', { id: host.id }) : `SSH 主机 ${host.id} 已从列表移除`, 'success');
            await loadSshHostsData();
        } else {
            showToast((typeof t === 'function' ? t('toast.removeFailedMsg', { msg: result.message || t('common.unknown') }) : '移除失败: ' + (result.message || '未知错误')), 'error');
        }
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.removeFailedMsg', { msg: e.message }) : '移除失败: ' + e.message), 'error');
    }
}

/**
 * 从已部署主机撤销公钥（弹出密码输入框）
 */
function revokeKeyFromHost(index) {
    const host = window._sshHostsList?.[index];
    if (!host) {
        showToast((typeof t === 'function' ? t('sshPage.hostNotFound') : '主机信息不存在'), 'error');
        return;
    }
    
    // 创建撤销确认弹窗
    let modal = document.getElementById('revoke-host-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'revoke-host-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }

    modal.innerHTML = sheet(560, t('securityPage.revokeKeyFromHost'),
        `<div class="t-note" style="margin-bottom:8px">${t('securityPage.revokeKeyFromHostDesc', {host: escapeHtml(host.username) + '@' + escapeHtml(host.host) + ':' + host.port, keyid: escapeHtml(host.keyid || 'default')})}</div>` +
        grp(row(t('securityPage.serverPassword'), inp('revoke-host-password', 210, t('securityPage.serverPasswordPlaceholder'), '', 'type="password"'))) +
        `<div id="revoke-host-result" class="result-box hidden" style="margin-top:12px"></div>`,
        `<button class="btn lg" onclick="hideRevokeHostModal()">${t('common.cancel')}</button><button class="btn lg primary bad" id="revoke-host-btn" onclick="doRevokeFromHost(${index})" title="${escapeHtml(t('securityPage.revokeHostHint'))}">${t('securityPage.revokeAndRemove')}</button>`);
    
    modal.classList.remove('hidden');
    document.getElementById('revoke-host-password').focus();
}

function hideRevokeHostModal() {
    const modal = document.getElementById('revoke-host-modal');
    if (modal) modal.classList.add('hidden');
}

async function doRevokeFromHost(index) {
    const host = window._sshHostsList?.[index];
    if (!host) return;
    
    const password = document.getElementById('revoke-host-password').value;
    if (!password) {
        showToast(typeof t === 'function' ? t('securityPage.enterPassword') : 'Please enter password', 'error');
        return;
    }
    
    const resultBox = document.getElementById('revoke-host-result');
    const revokeBtn = document.getElementById('revoke-host-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = typeof t === 'function' ? t('securityPage.revokingKey') : 'Revoking public key...';
    revokeBtn.disabled = true;
    
    try {
        const result = await api.sshRevoke(host.host, host.username, password, host.keyid || 'default', host.port);
        
        if (result.data?.revoked === true) {
            resultBox.textContent = typeof t === 'function' ? t('securityPage.revokeSuccess', {count: result.data.removed_count || 1}) : `Revoked! Removed ${result.data.removed_count || 1} matching public key(s)`;
            resultBox.classList.add('success');
            
            // 自动移除本地记录
            requireApiSuccess(await api.call('ssh.hosts.remove', { id: host.id }), 'call');
            showToast(typeof t === 'function' ? t('securityPage.revokedAndRemoved') : 'Revoked public key and removed host record', 'success');
            
            setTimeout(() => {
                hideRevokeHostModal();
                refreshSshHostsList();
            }, 1000);
        } else if (result.data?.found === false) {
            resultBox.textContent = typeof t === 'function' ? t('securityPage.keyNotFoundOnServer') : 'No matching public key found on server (may have been removed)\nRemove local record anyway?';
            resultBox.classList.add('error');
            
            // 提供移除本地记录的选项
            revokeBtn.innerHTML = '<svg class="i"><use href="#ri-delete-bin-line"/></svg> ' + (typeof t === 'function' ? t('securityPage.removeLocalRecord') : 'Remove Local Record Only');
            revokeBtn.onclick = async () => {
                requireApiSuccess(await api.call('ssh.hosts.remove', { id: host.id }), 'call');
                showToast(typeof t === 'function' ? t('securityPage.removedLocalRecord') : 'Removed local host record', 'success');
                hideRevokeHostModal();
                refreshSshHostsList();
            };
            revokeBtn.disabled = false;
            return;  // 不进入 finally
        } else {
            throw new Error(result.message || (typeof t === 'function' ? t('securityPage.revokeFailed') : 'Revoke failed'));
        }
    } catch (e) {
        resultBox.textContent = (typeof t === 'function' ? t('securityPage.revokeFailed') : 'Revoke failed') + ': ' + e.message;
        resultBox.classList.add('error');
    } finally {
        revokeBtn.disabled = false;
    }
}

/**
 * 从安全页面删除 SSH 主机（保留兼容性）
 */
async function deleteSshHostFromSecurity(id) {
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmRemoveHostLocal2', { id }) : `确定要从列表中移除主机 "${id}" 吗？\n\n注意：这只会移除本地记录，不会删除已部署到服务器上的公钥。如需撤销公钥，请使用密钥管理中的「撤销」功能。`, { primary: t('securityPage.remove'), tone: 'danger' })) return;
    
    try {
        const result = await api.call('ssh.hosts.remove', { id });
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.sshHostRemoved', { id }) : `SSH 主机 ${id} 已从列表移除`, 'success');
            await loadSshHostsData();
        } else {
            showToast((typeof t === 'function' ? t('toast.removeFailedMsg', { msg: result.message || t('common.unknown') }) : '移除失败: ' + (result.message || '未知错误')), 'error');
        }
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.removeFailedMsg', { msg: e.message }) : '移除失败: ' + e.message), 'error');
    }
}

async function deleteKey(id) {
    if (await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteKey', { id }) : `确定要删除密钥 "${id}" 吗？此操作不可撤销！`, { primary: t('common.delete'), tone: 'danger' })) {
        try {
            requireApiSuccess(await api.keyDelete(id), 'keyDelete');
            showToast((typeof t === 'function' ? t('toast.keyDeleted') : '密钥已删除'), 'success');
            await refreshSecurityPage();
        } catch (e) {
            showToast((typeof t === 'function' ? t('toast.deleteFailedMsg', { msg: e.message }) : '删除失败: ' + e.message), 'error');
        }
    }
}

async function exportKey(id) {
    try {
        const result = await api.keyExport(id);
        if (result.data?.public_key) {
            // 显示公钥弹窗
            showPubkeyModal(id, result.data.public_key, result.data.type, result.data.comment);
        } else {
            showToast((typeof t === 'function' ? t('toast.cannotGetPublicKey') : '无法获取公钥'), 'error');
        }
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.exportFailedMsg', { msg: e.message }) : '导出失败: ' + e.message), 'error');
    }
}

async function exportPrivateKey(id) {
    // 安全确认
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmExportPrivateKey', { id }) : `安全警告\n\n您正在导出私钥 "${id}"。\n\n私钥是高度敏感的安全凭证，请确保：\n• 不要在公共网络传输\n• 不要分享给他人\n• 安全存储在本地\n\n确定要继续吗？`, { primary: t('ui.exportAnyway'), tone: 'danger' })) {
        return;
    }
    
    try {
        const result = await api.keyExportPrivate(id);
        if (result.data?.private_key) {
            showPrivkeyModal(id, result.data.private_key, result.data.type, result.data.comment);
        } else {
            showToast((typeof t === 'function' ? t('toast.cannotGetPrivateKey') : '无法获取私钥'), 'error');
        }
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.exportFailedMsg', { msg: e.message }) : '导出失败: ' + e.message), 'error');
    }
}

function showPubkeyModal(id, pubkey, type, comment) {
    // 创建临时弹窗
    let modal = document.getElementById('pubkey-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'pubkey-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = sheet(560, `${t('securityPage.pubkeyExport')} - ${escapeHtml(id)}`,
        `<div class="t-note" style="margin-bottom:10px">${t('securityPage.keyTypeLabel')}: ${escapeHtml(type)}${comment ? ' | ' + t('securityPage.commentLabel') + ': ' + escapeHtml(comment) : ''}</div><textarea class="field mono" id="pubkey-content" readonly style="height:150px" title="${escapeHtml(t('securityPage.pubkeyHint'))}">${escapeHtml(pubkey)}</textarea>`,
        `<button class="btn lg" onclick="copyPubkey()"><svg class="i"><use href="#ri-file-copy-line"/></svg>${t('common.copyToClipboard')}</button><button class="btn lg" onclick="downloadPubkey('${escapeHtml(id)}')"><svg class="i"><use href="#ri-download-line"/></svg>${t('files.downloadFile')}</button><button class="btn lg primary" onclick="closePubkeyModal()">${t('common.close')}</button>`);
    
    modal.classList.remove('hidden');
}

function closePubkeyModal() {
    const modal = document.getElementById('pubkey-modal');
    if (modal) modal.classList.add('hidden');
}

function showPrivkeyModal(id, privkey, type, comment) {
    // 创建临时弹窗
    let modal = document.getElementById('privkey-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'privkey-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = sheet(560, `${t('securityPage.privkeyExport')} - ${escapeHtml(id)}`,
        `<div class="warnrow" style="margin:0 0 10px"><svg class="i"><use href="#ri-alert-line"/></svg><span><strong>${t('securityPage.securityWarning')}</strong>: ${t('securityPage.privkeyWarning')}</span></div><div class="t-note" style="margin-bottom:10px">${t('securityPage.keyTypeLabel')}: ${escapeHtml(type)}${comment ? ' | ' + t('securityPage.commentLabel') + ': ' + escapeHtml(comment) : ''}</div><textarea class="field mono" id="privkey-content" readonly style="height:200px" title="${escapeHtml(t('securityPage.privkeyHint', {id}))}">${escapeHtml(privkey)}</textarea>`,
        `<button class="btn lg" onclick="copyPrivkey()"><svg class="i"><use href="#ri-file-copy-line"/></svg>${t('common.copyToClipboard')}</button><button class="btn lg" onclick="downloadPrivkey('${escapeHtml(id)}')"><svg class="i"><use href="#ri-download-line"/></svg>${t('files.downloadFile')}</button><button class="btn lg primary" onclick="closePrivkeyModal()">${t('common.close')}</button>`);
    
    modal.classList.remove('hidden');
}

function closePrivkeyModal() {
    const modal = document.getElementById('privkey-modal');
    if (modal) modal.classList.add('hidden');
}

async function copyPubkey() {
    const textarea = document.getElementById('pubkey-content');
    if (textarea) {
        try {
            await navigator.clipboard.writeText(textarea.value);
            showToast((typeof t === 'function' ? t('toast.copied') : '已复制到剪贴板'), 'success');
        } catch (e) {
            // Fallback for older browsers
            textarea.select();
            document.execCommand('copy');
            showToast((typeof t === 'function' ? t('toast.copied') : '已复制到剪贴板'), 'success');
        }
    }
}

function downloadPubkey(id) {
    const textarea = document.getElementById('pubkey-content');
    if (textarea) {
        // 使用 Data URL 避免 HTTP 安全警告
        const dataUrl = 'data:text/plain;charset=utf-8,' + encodeURIComponent(textarea.value);
        const a = document.createElement('a');
        a.href = dataUrl;
        a.download = `${id}.pub`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        showToast(typeof t === 'function' ? t('toast.downloadedPublicKey', { id }) : `已下载 ${id}.pub`, 'success');
    }
}

async function copyPrivkey() {
    const textarea = document.getElementById('privkey-content');
    if (textarea) {
        try {
            await navigator.clipboard.writeText(textarea.value);
            showToast((typeof t === 'function' ? t('toast.copied') : '已复制到剪贴板'), 'success');
        } catch (e) {
            textarea.select();
            document.execCommand('copy');
            showToast((typeof t === 'function' ? t('toast.copied') : '已复制到剪贴板'), 'success');
        }
    }
}

function downloadPrivkey(id) {
    const textarea = document.getElementById('privkey-content');
    if (textarea) {
        // 使用 Data URL 避免 HTTP 安全警告
        const dataUrl = 'data:text/plain;charset=utf-8,' + encodeURIComponent(textarea.value);
        const a = document.createElement('a');
        a.href = dataUrl;
        a.download = id;  // 私钥文件不带扩展名
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        showToast(typeof t === 'function' ? t('toast.downloadedPrivateKey', { id }) : `已下载 ${id}`, 'success');
    }
}

// ====== 部署密钥功能 ======

let currentDeployKeyId = null;

function showDeployKeyModal(keyId) {
    currentDeployKeyId = keyId;
    document.getElementById('deploy-key-id').textContent = keyId;
    document.getElementById('deploy-host').value = '';
    document.getElementById('deploy-user').value = 'root';
    document.getElementById('deploy-port').value = '22';
    document.getElementById('deploy-password').value = '';
    const resultBox = document.getElementById('deploy-result');
    resultBox.classList.add('hidden');
    resultBox.textContent = '';
    document.getElementById('deploy-btn').disabled = false;
    document.getElementById('deploy-key-modal').classList.remove('hidden');
}

function hideDeployKeyModal() {
    document.getElementById('deploy-key-modal').classList.add('hidden');
    currentDeployKeyId = null;
}

async function deployKey() {
    if (!currentDeployKeyId) return;
    const keyId = currentDeployKeyId;
    const pageCurrent = capturePageValidity();
    
    const host = document.getElementById('deploy-host').value.trim();
    const user = document.getElementById('deploy-user').value.trim();
    const port = parseInt(document.getElementById('deploy-port').value) || 22;
    const password = document.getElementById('deploy-password').value;
    
    if (!host || !user || !password) {
        showToast((typeof t === 'function' ? t('toast.fillServerInfo') : '请填写完整的服务器信息'), 'error');
        return;
    }
    
    const resultBox = document.getElementById('deploy-result');
    const deployBtn = document.getElementById('deploy-btn');
    
    resultBox.classList.remove('hidden', 'success', 'warning', 'error');
    resultBox.textContent = typeof t === 'function' ? t('sshPage.deployingKey') : '正在部署密钥...';
    deployBtn.disabled = true;
    
    try {
        // 调用 ssh.copyid API（与 CLI 逻辑一致）
        const result = await api.sshCopyid(host, user, password, keyId, port, true);
        if (!pageCurrent()) return;
        const data = result?.data;
        // Registration failure must retain the acknowledged remote deployment.
        if (!(data?.deployed === true && data.registered === false)) {
            requireApiSuccess(result, 'ssh.copyid');
        }
        if (data?.deployed !== true) throw new Error(t('promptRepair.resultUnknown'));

        const refreshed = await refreshSshHostsList();
        if (!pageCurrent()) return;
        const registered = data.registered === true ||
            (data.registered === undefined && refreshed &&
             Object.values(window._sshHostsData || {}).some(h =>
                 h.host === host && Number(h.port) === port &&
                 h.username === user && h.keyid === keyId));
        const target = `${user}@${host}`;
        let tone = registered && data.verified === true ? 'success' : 'warning';
        if (data.registered === false) {
            const error = data.registration_error || '';
            const reason = t(error === 'ESP_ERR_INVALID_STATE' ? 'promptRepair.hostRegistrationBusy'
                           : error === 'ESP_ERR_NO_MEM' ? 'promptRepair.hostRegistrationFull'
                           : 'promptRepair.hostRegistrationStorage', {detail: error || t('promptRepair.resultUnknown')});
            resultBox.textContent = t('promptRepair.keyRegistrationFailed', {id: keyId, target, reason});
        } else if (!registered) {
            resultBox.textContent = t('promptRepair.keyRegistrationUnconfirmed', {id: keyId, target});
        } else {
            resultBox.textContent = t(data.verified === true ? 'promptRepair.keyVerified' : 'promptRepair.keyUnverified', {id: keyId, target});
        }
        if (!refreshed) {
            resultBox.textContent += '\n' + t('promptRepair.deployedHostsRefreshFailed');
            tone = 'warning';
        }
        resultBox.classList.add(tone);
        showToast(resultBox.textContent, tone, 6000);
    } catch (e) {
        if (!pageCurrent()) return;
        resultBox.textContent = (typeof t === 'function' ? t('pkiPage.deployFailedMsg', { msg: e.message }) : '部署失败: ' + e.message);
        resultBox.classList.add('error');
    } finally {
        deployBtn.disabled = false;
    }
}

// ====== 撤销密钥功能 ======

let currentRevokeKeyId = null;

function showRevokeKeyModal(keyId) {
    currentRevokeKeyId = keyId;
    document.getElementById('revoke-key-id').textContent = keyId;
    document.getElementById('revoke-host').value = '';
    document.getElementById('revoke-user').value = 'root';
    document.getElementById('revoke-port').value = '22';
    document.getElementById('revoke-password').value = '';
    const resultBox = document.getElementById('revoke-result');
    resultBox.classList.add('hidden');
    resultBox.textContent = '';
    document.getElementById('revoke-btn').disabled = false;
    document.getElementById('revoke-key-modal').classList.remove('hidden');
}

function hideRevokeKeyModal() {
    document.getElementById('revoke-key-modal').classList.add('hidden');
    currentRevokeKeyId = null;
}

async function revokeKey() {
    if (!currentRevokeKeyId) return;
    
    const host = document.getElementById('revoke-host').value.trim();
    const user = document.getElementById('revoke-user').value.trim();
    const port = parseInt(document.getElementById('revoke-port').value) || 22;
    const password = document.getElementById('revoke-password').value;
    
    if (!host || !user || !password) {
        showToast((typeof t === 'function' ? t('toast.fillServerInfo') : '请填写完整的服务器信息'), 'error');
        return;
    }
    
    const resultBox = document.getElementById('revoke-result');
    const revokeBtn = document.getElementById('revoke-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = typeof t === 'function' ? t('securityPage.revokingKey') : '正在撤销密钥...';
    revokeBtn.disabled = true;
    
    try {
        // 调用 ssh.revoke API（与 CLI 逻辑一致）
        const result = requireApiSuccess(await api.sshRevoke(host, user, password, currentRevokeKeyId, port), 'ssh.revoke');
        
        if (result.data?.revoked === true) {
            resultBox.textContent = typeof t === 'function' ? t('sshPage.revokeSuccess', { target: `${user}@${host}`, count: result.data.removed_count || 1 }) : `撤销成功！已从 ${user}@${host} 移除 ${result.data.removed_count || 1} 个匹配的公钥`;
            resultBox.classList.add('success');
            showToast((typeof t === 'function' ? t('toast.keyRevoked') : '密钥撤销成功'), 'success');
        } else if (result.data?.found === false) {
            resultBox.textContent = typeof t === 'function' ? t('sshPage.keyNotFound', { target: `${user}@${host}` }) : `该公钥未在 ${user}@${host} 上找到`;
            resultBox.classList.add('warning');
            showToast((typeof t === 'function' ? t('toast.publicKeyNotFound') : '公钥未找到'), 'warning');
        } else {
            throw new Error(t('securityPage.revokeFailed'));
        }
    } catch (e) {
        resultBox.textContent = (typeof t === 'function' ? t('pkiPage.revokeFailedMsg', { msg: e.message }) : '撤销失败: ' + e.message);
        resultBox.classList.add('error');
    } finally {
        revokeBtn.disabled = false;
    }
}

// ====== 主机指纹不匹配警告 ======

let currentMismatchInfo = null;

function showHostMismatchModal(info) {
    currentMismatchInfo = info;
    document.getElementById('mismatch-host').textContent = `${info.host}:${info.port || 22}`;
    document.getElementById('mismatch-stored-fp').textContent = info.stored_fingerprint || t('common.unknown');
    document.getElementById('mismatch-current-fp').textContent = info.current_fingerprint || t('common.unknown');
    document.getElementById('host-mismatch-modal').classList.remove('hidden');
}

function hideHostMismatchModal() {
    document.getElementById('host-mismatch-modal').classList.add('hidden');
    currentMismatchInfo = null;
}

async function removeAndRetry() {
    if (!currentMismatchInfo) return;
    
    try {
        // 使用新的 hosts.update API 更新主机密钥
        requireApiSuccess(await api.hostsUpdate(currentMismatchInfo.host, currentMismatchInfo.port || 22), 'hostsUpdate');
        showToast((typeof t === 'function' ? t('toast.oldHostKeyRemoved') : '旧主机密钥已移除，请重新连接以信任新密钥'), 'success');
        hideHostMismatchModal();
        await refreshSecurityPage();
    } catch (e) {
        showToast((typeof t === 'function' ? t('toast.updateFailedMsg', { msg: e.message }) : '更新失败: ' + e.message), 'error');
    }
}

async function removeHost(host, port) {
    if (await confirmAction(typeof t === 'function' ? t('ui.confirmRemoveKnownHost', { host, port }) : `确定要移除主机 "${host}:${port}" 的记录吗？`, { primary: t('securityPage.remove'), tone: 'danger' })) {
        try {
            requireApiSuccess(await api.hostsRemove(host, port), 'hostsRemove');
            showToast((typeof t === 'function' ? t('toast.hostRemoved') : '主机已移除'), 'success');
            await refreshSecurityPage();
        } catch (e) {
            showToast((typeof t === 'function' ? t('toast.removeFailedMsg', { msg: e.message }) : '移除失败: ' + e.message), 'error');
        }
    }
}

async function clearAllHosts() {
    if (await confirmAction(typeof t === 'function' ? t('ui.confirmClearKnownHosts') : '确定要清除所有已知主机记录吗？此操作不可撤销！', { primary: t('common.clear'), tone: 'danger' })) {
        try {
            requireApiSuccess(await api.hostsClear(), 'hostsClear');
            showToast((typeof t === 'function' ? t('toast.allHostsCleared') : '已清除所有已知主机'), 'success');
            await refreshSecurityPage();
        } catch (e) {
            showToast((typeof t === 'function' ? t('toast.clearFailedMsg', { msg: e.message }) : '清除失败: ' + e.message), 'error');
        }
    }
}

// =========================================================================
//                  Config Pack Management
// =========================================================================

/**
 * 刷新配置包状态卡片
 */
async function refreshConfigPackStatus() {
    const pageCurrent = capturePageValidity();
    const statusIcon = document.getElementById('pack-status-icon');
    const statusText = document.getElementById('pack-status-text');
    const deviceTypeBadge = document.getElementById('pack-device-type-badge');
    const infoDetails = document.getElementById('pack-info-details');
    const btnExport = document.getElementById('btn-pack-export');
    
    if (!statusIcon) return; // 不在安全页面
    
    try {
        const result = await api.configPackInfo();
        if (!pageCurrent()) return;
        const data = result.data;
        
        if (!data) throw new Error(t('promptRepair.noResponse'));
        
        // 存储状态供弹窗使用
        window._configPackStatus = data;
        
        // 更新状态
        const canExport = data.can_export;
        const deviceType = data.device_type;
        
        statusIcon.textContent = '';
        statusText.textContent = canExport ? (typeof t === 'function' ? t('securityPage.developerDevice') : 'Developer 设备') : (typeof t === 'function' ? t('securityPage.normalDevice') : 'Device 设备');
        
        // 设备类型徽章
        deviceTypeBadge.style.display = 'inline-block';
        deviceTypeBadge.textContent = deviceType;
        deviceTypeBadge.className = 'tag';
        
        // 显示详细信息
        infoDetails.style.display = 'block';
        document.getElementById('pack-device-type').textContent = deviceType;
        document.getElementById('pack-cert-cn').textContent = data.cert_cn || '-';
        document.getElementById('pack-cert-fp').textContent = data.cert_fingerprint 
            ? data.cert_fingerprint.substring(0, 32) + '...' 
            : '-';
        document.getElementById('pack-version').textContent = data.pack_version || '-';
        
        // 导出按钮只对 Developer 设备启用
        if (btnExport) {
            btnExport.disabled = !canExport;
            btnExport.title = canExport ? '' : (typeof t === 'function' ? t('securityPage.onlyDeveloperCanExport') : '仅 Developer 设备可导出配置包');
        }
        
    } catch (e) {
        if (!pageCurrent()) return;
        console.error('Refresh config pack status error:', e);
        statusIcon.textContent = '';
        statusText.textContent = (typeof t === 'function' ? t('common.loadFailed') : '加载失败');
        if (deviceTypeBadge) deviceTypeBadge.style.display = 'none';
        if (infoDetails) infoDetails.style.display = 'none';
    }
}

// 配置包：导出设备证书弹窗
function showConfigPackExportCertModal() {
    document.getElementById('pack-export-cert-modal').classList.remove('hidden');
    loadConfigPackCert();
}

function hideConfigPackExportCertModal() {
    document.getElementById('pack-export-cert-modal').classList.add('hidden');
}

async function loadConfigPackCert() {
    const loading = document.getElementById('pack-export-cert-loading');
    const content = document.getElementById('pack-export-cert-content');
    
    loading.style.display = 'block';
    content.classList.add('hidden');
    
    try {
        const result = await api.configPackExportCert();
        if (result.code !== 0) throw new Error(result.message || result.error);
        
        const data = result.data;
        document.getElementById('pack-cert-fingerprint').value = data.fingerprint || '';
        document.getElementById('pack-cert-cn-display').value = data.cn || '';
        document.getElementById('pack-cert-pem').value = data.certificate || '';
        
        loading.style.display = 'none';
        content.classList.remove('hidden');
    } catch (e) {
        loading.textContent = typeof t === 'function' ? t('pkiPage.loadFailedMsg', { msg: e.message }) : '加载失败: ' + e.message;
    }
}

function copyPackCertToClipboard() {
    const pem = document.getElementById('pack-cert-pem').value;
    navigator.clipboard.writeText(pem).then(() => {
        showToast(typeof t === 'function' ? t('toast.certCopied') : '证书已复制到剪贴板', 'success');
    }).catch(e => {
        showToast(typeof t === 'function' ? t('toast.copyFailedMsg', { msg: e.message }) : '复制失败: ' + e.message, 'error');
    });
}

// 配置包：导入弹窗
function showConfigPackImportModal() {
    document.getElementById('pack-import-modal').classList.remove('hidden');
    document.getElementById('pack-import-file').value = '';
    document.getElementById('pack-import-content').value = '';
    document.getElementById('pack-import-result').classList.add('hidden');
    document.getElementById('pack-import-preview').classList.add('hidden');
    const statusEl = document.getElementById('pack-import-file-status');
    if (statusEl) statusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件';
}

function hideConfigPackImportModal() {
    document.getElementById('pack-import-modal').classList.add('hidden');
}

function handlePackFileSelect(event) {
    const file = event.target.files[0];
    const statusEl = document.getElementById('pack-import-file-status');
    if (statusEl) statusEl.textContent = file ? file.name : (typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件');
    if (!file) return;
    
    const reader = new FileReader();
    reader.onload = (e) => {
        document.getElementById('pack-import-content').value = e.target.result;
    };
    reader.readAsText(file);
}

async function verifyConfigPack() {
    const content = document.getElementById('pack-import-content').value.trim();
    const resultBox = document.getElementById('pack-import-result');
    const preview = document.getElementById('pack-import-preview');
    
    if (!content) {
        resultBox.className = 'result-box error';
        resultBox.textContent = typeof t === 'function' ? t('pkiPage.uploadOrPasteContent') : '请上传文件或粘贴配置包内容';
        resultBox.classList.remove('hidden');
        return;
    }
    
    resultBox.className = 'result-box';
    resultBox.textContent = typeof t === 'function' ? t('pkiPage.verifying') : '验证中...';
    resultBox.classList.remove('hidden');
    preview.classList.add('hidden');
    
    try {
        const result = await api.configPackVerify(content);
        if (result.code !== 0) throw new Error(result.message || result.error);
        
        const data = result.data;
        if (data.valid) {
            resultBox.className = 'result-box success';
            resultBox.innerHTML = typeof t === 'function' ? t('ssh.signatureVerified') : '签名验证通过';
            
            // 显示签名信息
            if (data.signature) {
                const sig = data.signature;
                document.getElementById('pack-preview-content').innerHTML = grp(
                    row(t('ssh.signer'), escapeHtml(sig.signer_cn || '-')) +
                    row(t('promptRepair.organization'), escapeHtml(sig.signer_ou || '-')) +
                    row(t('securityPage.officialSignature'), sig.is_official ? t('common.yes') : t('common.no')) +
                    row(t('securityPage.signedAt'), sig.signed_at ? formatTimestamp(sig.signed_at) : '-'));
                preview.classList.remove('hidden');
            }
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (typeof t === 'function' ? t('pkiPage.verifyFailed') : '验证失败') + ': ' + (data.result_message || (typeof t === 'function' ? t('pkiPage.signatureInvalid') : '签名无效'));
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = (typeof t === 'function' ? t('pkiPage.verifyFailed') : '验证失败') + ': ' + e.message;
    }
}

async function importConfigPack() {
    const content = document.getElementById('pack-import-content').value.trim();
    const resultBox = document.getElementById('pack-import-result');
    const preview = document.getElementById('pack-import-preview');
    
    if (!content) {
        resultBox.className = 'result-box error';
        resultBox.textContent = typeof t === 'function' ? t('pkiPage.uploadOrPasteContent') : '请上传文件或粘贴配置包内容';
        resultBox.classList.remove('hidden');
        return;
    }
    
    resultBox.className = 'result-box';
    resultBox.textContent = typeof t === 'function' ? t('pkiPage.importing') : '导入中...';
    resultBox.classList.remove('hidden');
    
    try {
        const result = await api.configPackImport(content, null, false);
        if (result.code !== 0) throw new Error(result.message || result.error);
        
        const data = result.data;
        resultBox.className = 'result-box success';
        resultBox.innerHTML = `${typeof t === 'function' ? t('toast.configPackImported') : 'Config pack imported'}<br><small>${typeof t === 'function' ? t('securityPage.savedTo') : 'Saved to'}: ${data.saved_path || '-'}</small>`;
        
        // 显示详细信息（无解密内容）
        const sig = data.signature || {};
        const yesStr = typeof t === 'function' ? t('common.yes') : 'Yes';
        const noStr = typeof t === 'function' ? t('common.no') : 'No';
        document.getElementById('pack-preview-content').innerHTML = grp(
            row(t('securityPage.configName'), escapeHtml(data.name || '-')) +
            row(t('common.description'), escapeHtml(data.description || '-')) +
            row(t('securityPage.targetDevice'), escapeHtml(data.target_device || '-')) +
            row(t('securityPage.createdAt'), data.created_at ? formatTimestamp(data.created_at) : '-') +
            row(t('securityPage.signer'), `${escapeHtml(sig.signer_cn || '-')} (${escapeHtml(sig.signer_ou || '-')})`) +
            row(t('securityPage.signedAt'), sig.signed_at ? formatTimestamp(sig.signed_at) : '-') +
            row(t('securityPage.officialSignature'), sig.is_official ? yesStr : noStr) +
            row(t('securityPage.savePath'), escapeHtml(data.saved_path || '-'))) +
            '<div class="t-note" style="margin-top:8px">' + t('securityPage.packEncryptedHint') + '</div>';
        preview.classList.remove('hidden');
        
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = (typeof t === 'function' ? t('toast.importFailed') : 'Import failed') + ': ' + e.message;
    }
}

/**
 * 显示配置包应用确认对话框
 * 当通过文件管理上传 .tscfg 文件并验证成功后调用
 */
function showConfigPackApplyConfirm(path, packInfo, verificationCurrent = () => true) {
    if (packInfo?.valid !== true) { showToast(t('promptRepair.packInvalid'), 'error'); return; }
    if (!i18n.complete('securityPage.configPackSavedApplyNow')) return;
    closeConfigPackApplyConfirm();
    const sig = packInfo.signature || {};
    const signerInfo = sig.signer_cn ? `${sig.signer_cn}${sig.is_official ? t('promptRepair.officialSuffix') : ''}` : t('common.unknown');
    
    // 创建确认对话框
    const dialog = document.createElement('div');
    dialog.className = 'modal';
    dialog.id = 'config-pack-apply-confirm';
    dialog.innerHTML = sheet(420, t('securityPage.configPackUploaded'),
        `<div style="margin-bottom:12px"><span class="state ok">${t('securityPage.verifySuccess')}</span></div>` +
        grp(row(t('securityPage.fileName'), `<span class="mono">${escapeHtml(path.split('/').pop())}</span>`) +
            row(t('securityPage.signer'), escapeHtml(signerInfo) + (sig.is_official ? ` <span class="state ok">${t('securityPage.officialSignature')}</span>` : ''))) +
        `<div class="t-body" style="color:var(--ink-2);margin-top:12px">${t('securityPage.configPackSavedApplyNow')}</div>`,
        `<button class="btn lg" onclick="closeConfigPackApplyConfirm()">${t('securityPage.applyLater')}</button><button class="btn lg primary" id="config-pack-apply-button">${t('securityPage.applyNow')}</button>`);
    
    document.body.appendChild(dialog);
    dialog.verificationCurrent = verificationCurrent;
    document.getElementById('config-pack-apply-button').onclick = () => applyConfigPackFromPath(path);
}

function closeConfigPackApplyConfirm() {
    const dialog = document.getElementById('config-pack-apply-confirm');
    if (dialog) {
        dialog.remove();
    }
}

/**
 * 应用指定路径的配置包
 */
let configPackAttempt = 0;
async function applyConfigPackFromPath(path) {
    const dialog = document.getElementById('config-pack-apply-confirm');
    const button = document.getElementById('config-pack-apply-button');
    if (button?.disabled || !i18n.complete('securityPage.configPackSavedApplyNow')) return;
    if (dialog?.verificationCurrent && !dialog.verificationCurrent()) {
        showToast(t('promptRepair.packRevalidate'), 'warning'); return;
    }
    const attempt = ++configPackAttempt;
    const current = () => attempt === configPackAttempt && document.getElementById('config-pack-apply-confirm') === dialog;
    if (button) button.disabled = true;
    // Keep a per-task result outside the modal, including after the modal is closed.
    let results = document.getElementById('config-pack-task-results');
    if (!results) {
        results = document.createElement('div'); results.id = 'config-pack-task-results';
        results.setAttribute('role', 'status');
        const heading = document.createElement('h3'); heading.textContent = t('promptRepair.taskResults');
        results.appendChild(heading); document.querySelector('main').appendChild(results);
    }
    const row = document.createElement('div'); results.appendChild(row);
    const record = message => { row.textContent = t('promptRepair.taskAttempt', {path, attempt}) + ' — ' + message; };
    record(t('toast.applyingConfig'));
    showToast(path + ': ' + t('toast.applyingConfig'), 'info', 3000, {isCurrent: current});
    try {
        const result = requireApiSuccess(await api.call('config.pack.apply', { path }, 'POST'), 'config.pack.apply');
        const data = result.data;
        if (data?.success !== true) throw new ApiOperationError({message: data?.result_message}, 'config.pack.apply');
        const message = t('promptRepair.packApplied') + (data.applied_modules?.length ? '\n' + t('promptRepair.details', {detail: data.applied_modules.join(', ')}) : '');
        record(message);
        if (current()) {
            showToast(path + ': ' + message, 'success', 5000, {isCurrent: current});
            dialog?.remove();
        }
    } catch (e) {
        const message = e.uncertain ? e.message : t('toast.applyFailedMsg', {msg: e.message});
        record(message);
        if (current()) showToast(path + ': ' + message, 'error', 8000, {isCurrent: current});
    } finally {
        if (current() && button) button.disabled = false;
    }
}

// 配置包导出：文件浏览器状态
let packExportCurrentPath = '/sdcard/config';
let packExportSelectedFile = null;
let packExportFileContent = null;
let packExportSelectedFiles = new Map();  // Map<fullPath, {name, content, status}>
let packExportCurrentEntries = [];  // 当前目录的条目缓存

// 配置包：导出弹窗（仅 Developer）
function showConfigPackExportModal() {
    if (!window._configPackStatus?.can_export) {
        showToast(typeof t === 'function' ? t('securityPage.onlyDeveloperCanExport') : 'Only Developer devices can export config packs', 'error');
        return;
    }
    document.getElementById('pack-export-modal').classList.remove('hidden');
    document.getElementById('pack-export-name').value = '';
    document.getElementById('pack-export-desc').value = '';
    document.getElementById('pack-export-recipient-cert').value = '';
    document.getElementById('pack-export-result').classList.add('hidden');
    document.getElementById('pack-export-selected').style.display = 'none';
    document.getElementById('btn-pack-export-generate').disabled = true;
    
    // 重置文件选择状态
    packExportSelectedFile = null;
    packExportFileContent = null;
    packExportSelectedFiles.clear();
    packExportCurrentEntries = [];
    packExportCurrentPath = '/sdcard/config';
    document.getElementById('pack-export-browse-path').value = packExportCurrentPath;
    
    // 加载文件列表
    packExportBrowseRefresh();
}

function hideConfigPackExportModal() {
    document.getElementById('pack-export-modal').classList.add('hidden');
    // 重置按钮状态
    document.getElementById('btn-pack-copy').style.display = 'none';
    document.getElementById('btn-pack-download').style.display = 'none';
    document.getElementById('pack-export-saved-path').style.display = 'none';
}

// 文件浏览器：刷新当前目录
async function packExportBrowseRefresh() {
    const fileList = document.getElementById('pack-export-file-list');
    fileList.innerHTML = '<div class="row"><div class="rl t-note">' + t('common.loading') + '</div></div>';
    
    try {
        const result = await api.storageList(packExportCurrentPath);
        if (result.code !== 0) throw new Error(result.message);
        
        const entries = result.data?.entries || [];
        
        // 排序：目录在前，然后按名称排序
        entries.sort((a, b) => {
            if (a.type === 'dir' && b.type !== 'dir') return -1;
            if (a.type !== 'dir' && b.type === 'dir') return 1;
            return a.name.localeCompare(b.name);
        });
        
        // 只显示 .json 文件和目录
        const filteredEntries = entries.filter(e => 
            e.type === 'dir' || e.name.endsWith('.json')
        );
        
        // 缓存当前目录条目
        packExportCurrentEntries = filteredEntries;
        
        if (filteredEntries.length === 0) {
            fileList.innerHTML = '<div class="row"><div class="rl t-note">' + t('pkiPage.noConfigFiles') + '</div></div>';
            return;
        }
        
        let html = '';
        for (const entry of filteredEntries) {
            const fullPath = packExportCurrentPath + '/' + entry.name;
            const isSelected = packExportSelectedFiles.has(fullPath);
            // 转义文件名中的特殊字符
            const safeName = escapeHtml(entry.name);
            
            if (entry.type === 'dir') {
                // 目录：点击进入，无复选框
                html += `<div class="row" style="cursor:pointer" data-name="${safeName}" onclick="packExportBrowseInto(this.dataset.name)"><div class="rl"><span style="display:flex;gap:10px;align-items:center"><svg class="i"><use href="#ri-folder-line"/></svg>${escapeHtml(entry.name)}</span></div><div class="rc"></div></div>`;
            } else {
                const checkboxId = 'pack-export-cb-' + filteredEntries.indexOf(entry);
                html += `<div class="row"><div class="rl"><label style="display:flex;gap:10px;align-items:center" for="${checkboxId}"><input type="checkbox" id="${checkboxId}" ${isSelected ? 'checked' : ''} data-name="${safeName}" onclick="packExportToggleFile(this.dataset.name, this.checked)">${escapeHtml(entry.name)}</label></div><div class="rc"><span class="t-note num">${formatFileSize(entry.size)}</span></div></div>`;
            }
        }
        fileList.innerHTML = html;
        
        // 更新选择状态显示
        packExportUpdateSelectedDisplay();
        
    } catch (e) {
        fileList.innerHTML = `<div class="row"><div class="rl"><span class="state bad">${t('common.loadFailed')}: ${escapeHtml(e.message)}</span></div></div>`;
    }
}

// 文件浏览器：进入子目录
function packExportBrowseInto(dirName) {
    packExportCurrentPath = packExportCurrentPath + '/' + dirName;
    document.getElementById('pack-export-browse-path').value = packExportCurrentPath;
    packExportBrowseRefresh();
}

// 文件浏览器：返回上级目录
function packExportBrowseUp() {
    const parts = packExportCurrentPath.split('/').filter(p => p);
    if (parts.length <= 1) {
        // 不能再往上了
        return;
    }
    parts.pop();
    packExportCurrentPath = '/' + parts.join('/');
    document.getElementById('pack-export-browse-path').value = packExportCurrentPath;
    packExportBrowseRefresh();
}

// 文件浏览器：切换文件选中状态
async function packExportToggleFile(fileName, checked) {
    const fullPath = packExportCurrentPath + '/' + fileName;
    
    if (!checked) {
        // 取消选择
        packExportSelectedFiles.delete(fullPath);
        packExportUpdateSelectedDisplay();
        packExportBrowseRefresh();
        return;
    }
    
    // 选中文件，读取内容
    packExportSelectedFiles.set(fullPath, { name: fileName, content: null, status: 'loading' });
    packExportUpdateSelectedDisplay();
    
    try {
        const result = await api.storageRead(fullPath);
        if (result.code !== 0) throw new Error(result.message);
        
        const rawContent = result.data?.content;
        if (rawContent === undefined || rawContent === null) throw new Error(t('toast.fileContentEmpty'));
        
        // 后端 storage.read 会自动解析 JSON
        let contentStr;
        if (typeof rawContent === 'object') {
            contentStr = JSON.stringify(rawContent, null, 2);
        } else {
            contentStr = rawContent;
            JSON.parse(contentStr);  // 验证
        }
        
        packExportSelectedFiles.set(fullPath, { name: fileName, content: contentStr, status: 'ok' });
        
    } catch (e) {
        packExportSelectedFiles.set(fullPath, { name: fileName, content: null, status: 'error', error: e.message });
    }
    
    packExportUpdateSelectedDisplay();
    packExportBrowseRefresh();
}

// 更新选择状态显示
function packExportUpdateSelectedDisplay() {
    const selectedDiv = document.getElementById('pack-export-selected');
    const selectedSpan = document.getElementById('pack-export-selected-file');
    const generateBtn = document.getElementById('btn-pack-export-generate');
    
    const files = Array.from(packExportSelectedFiles.entries());
    const okFiles = files.filter(([_, v]) => v.status === 'ok');
    const loadingFiles = files.filter(([_, v]) => v.status === 'loading');
    const errorFiles = files.filter(([_, v]) => v.status === 'error');
    
    if (files.length === 0) {
        selectedDiv.style.display = 'none';
        generateBtn.disabled = true;
        return;
    }
    
    selectedDiv.style.display = 'block';
    
    let text = t('promptRepair.filesSelected', {count: files.length});
    if (loadingFiles.length > 0) {
        text += ` (${loadingFiles.length} ${t('securityPage.filesLoading')})`;
        selectedDiv.className = 'result-box';
        generateBtn.disabled = true;
    } else if (errorFiles.length > 0) {
        text += '\n' + t('promptRepair.fileErrors', {count: errorFiles.length, message: errorFiles[0][1].error});
        selectedDiv.className = 'result-box error';
        generateBtn.disabled = errorFiles.length === files.length;  // 全部错误则禁用
    } else {
        selectedDiv.className = 'result-box success';
        generateBtn.disabled = false;
    }
    
    selectedSpan.textContent = text;
    
    // 自动填充配置名称
    const nameInput = document.getElementById('pack-export-name');
    if (!nameInput.value && okFiles.length > 0) {
        if (okFiles.length === 1) {
            nameInput.value = okFiles[0][1].name.replace(/\.json$/i, '');
        } else {
            nameInput.value = 'batch_config_' + okFiles.length;
        }
    }
}

// 全选当前目录的文件
async function packExportSelectAll() {
    const files = packExportCurrentEntries.filter(e => e.type === 'file');
    if (files.length === 0) return;
    
    for (const file of files) {
        const fullPath = packExportCurrentPath + '/' + file.name;
        if (!packExportSelectedFiles.has(fullPath)) {
            await packExportToggleFile(file.name, true);
        }
    }
}

// 取消全选
function packExportDeselectAll() {
    packExportSelectedFiles.clear();
    packExportUpdateSelectedDisplay();
    packExportBrowseRefresh();
}

// 选择整个目录（当前目录下的所有 JSON 文件）
async function packExportSelectDir() {
    // 与全选功能相同，但可以在 UI 上有区分
    await packExportSelectAll();
    showToast(typeof t === 'function' ? t('toast.selectedJsonFiles') : '已选择当前目录下的所有 JSON 文件', 'success');
}

// 文件大小格式化
function formatFileSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

async function exportConfigPack() {
    const name = document.getElementById('pack-export-name').value.trim();
    const desc = document.getElementById('pack-export-desc').value.trim();
    const recipientCert = document.getElementById('pack-export-recipient-cert').value.trim();
    const resultBox = document.getElementById('pack-export-result');
    const outputBox = document.getElementById('pack-export-output');
    const copyBtn = document.getElementById('btn-pack-copy');
    const downloadBtn = document.getElementById('btn-pack-download');
    
    // 重置按钮状态
    copyBtn.disabled = true;
    downloadBtn.disabled = true;
    
    // 验证输入
    if (!name) {
        resultBox.className = 'result-box error';
        resultBox.style.visibility = 'visible';
        resultBox.textContent = typeof t === 'function' ? t('pkiPage.enterConfigName') : '请输入配置名称';
        return;
    }
    
    // 收集所有成功加载的文件
    const okFiles = Array.from(packExportSelectedFiles.entries()).filter(([_, v]) => v.status === 'ok');
    if (okFiles.length === 0) {
        resultBox.className = 'result-box error';
        resultBox.style.visibility = 'visible';
        resultBox.textContent = typeof t === 'function' ? t('pkiPage.selectConfigFile') : '请选择配置文件';
        return;
    }
    
    if (!recipientCert) {
        resultBox.className = 'result-box error';
        resultBox.style.visibility = 'visible';
        resultBox.textContent = typeof t === 'function' ? t('securityPage.pasteTargetCert') : '请粘贴目标设备证书';
        return;
    }
    
    // 合并多个配置文件为一个对象
    let content;
    try {
        if (okFiles.length === 1) {
            // 单文件：直接使用
            content = JSON.parse(okFiles[0][1].content);
        } else {
            // 多文件：合并到一个对象中，使用文件名作为 key
            content = { _batch: true, _files: {} };
            for (const [path, info] of okFiles) {
                const key = info.name.replace(/\.json$/i, '');
                content._files[key] = JSON.parse(info.content);
            }
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.style.visibility = 'visible';
        resultBox.textContent = (typeof t === 'function' ? t('pkiPage.invalidJsonConfigMsg', { msg: e.message }) : '配置文件不是有效的 JSON: ' + e.message);
        return;
    }
    
    resultBox.className = 'result-box';
    resultBox.style.visibility = 'visible';
    resultBox.textContent = typeof t === 'function' ? t('pkiPage.generatingPackWithCount', { count: okFiles.length }) : `生成配置包中 (${okFiles.length} 个文件)...`;
    document.getElementById('pack-export-tscfg').value = '';
    
    try {
        // 同时保存到 SD 卡
        const savePath = '/sdcard/output_config/' + name + '.tscfg';
        const result = await api.configPackExport(name, content, recipientCert, desc || null, savePath);
        console.log('[ConfigPack] Export result:', result);
        if (result.code !== 0) throw new Error(result.message || result.error);
        
        const data = result.data || {};
        const tscfgContent = data.tscfg || '';
        const fileSize = data.size || tscfgContent.length;
        const fileName = data.filename || (name + '.tscfg');
        const savedPath = data.saved_path || '';
        
        resultBox.className = 'result-box success';
        resultBox.style.whiteSpace = 'pre-wrap';
        resultBox.textContent = t('promptRepair.packGenerated', {name: fileName, size: fileSize, count: okFiles.length});
        if (savedPath) resultBox.textContent += '\n' + t('securityPage.savedTo') + ': ' + savedPath;

        // 显示输出
        const tscfgTextarea = document.getElementById('pack-export-tscfg');
        tscfgTextarea.value = tscfgContent;
        window._packExportFilename = fileName;
        
        // 显示按钮
        if (tscfgContent) {
            copyBtn.style.display = '';
            downloadBtn.style.display = '';
            copyBtn.disabled = false;
            downloadBtn.disabled = false;
        }
        
        // 显示保存路径
        const savedPathSpan = document.getElementById('pack-export-saved-path');
        if (savedPath && savedPathSpan) {
            savedPathSpan.textContent = typeof t === 'function' ? t('pkiPage.savedToDevice') : '已保存到设备';
            savedPathSpan.style.display = 'inline';
        }
        
        // 确保输出区域可见
        outputBox.style.display = 'block';
        
        if (!tscfgContent) {
            console.warn('[ConfigPack] tscfg content is empty!');
            resultBox.textContent += '\n' + t('pkiPage.contentEmpty');
        }
        
    } catch (e) {
        console.error('[ConfigPack] Export error:', e);
        resultBox.className = 'result-box error';
        resultBox.textContent = (typeof t === 'function' ? t('pkiPage.generationFailedMsg', { msg: e.message }) : '生成失败: ' + e.message);
    }
}

function copyPackTscfgToClipboard() {
    const tscfg = document.getElementById('pack-export-tscfg').value;
    navigator.clipboard.writeText(tscfg).then(() => {
        showToast(typeof t === 'function' ? t('toast.configPackCopied') : '配置包已复制到剪贴板', 'success');
    }).catch(e => {
        showToast(typeof t === 'function' ? t('toast.copyFailedMsg', { msg: e.message }) : '复制失败: ' + e.message, 'error');
    });
}

function downloadPackTscfg() {
    const tscfg = document.getElementById('pack-export-tscfg').value;
    const filename = window._packExportFilename || 'config.tscfg';
    
    const blob = new Blob([tscfg], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    
    showToast(t('promptRepair.downloadStarted'), 'info');
}

// 配置包：列表弹窗
function showConfigPackListModal() {
    document.getElementById('pack-list-modal').classList.remove('hidden');
    refreshConfigPackList();
}

function hideConfigPackListModal() {
    document.getElementById('pack-list-modal').classList.add('hidden');
}

async function refreshConfigPackList() {
    const pageCurrent = capturePageValidity();
    const path = document.getElementById('pack-list-path').value.trim() || '/sdcard/config';
    const loading = document.getElementById('pack-list-loading');
    const table = document.getElementById('pack-list-table');
    const tbody = document.getElementById('pack-list-tbody');
    
    loading.style.display = 'block';
    table.classList.add('hidden');
    
    try {
        const result = await api.configPackList(path);
        if (!pageCurrent()) return;
        if (result.code !== 0) throw new Error(result.message || result.error);
        
        const data = result.data;
        const files = data.files || [];
        
        if (files.length === 0) {
            tbody.innerHTML = '<div class="tr" style="--cols:1fr;color:var(--ink-3)">' + t('securityPage.noPacks') + '</div>';
        } else {
            const yesStr = t('common.yes');
            const noStr = t('common.no');
            const validStr = t('common.valid');
            const invalidStr = t('common.invalid');
            const importStr = t('securityPage.importBtn');
            tbody.innerHTML = '<div class="tr th cols-packs"><div>' + t('securityPage.fileName') + '</div><div>' + t('files.size') + '</div><div>' + t('securityPage.signerLabel') + '</div><div>' + t('securityPage.official') + '</div><div>' + t('common.status') + '</div><div></div></div>' + files.map(file => `
                <div class="tr cols-packs">
                    <div class="mono">${escapeHtml(file.name)}</div>
                    <div>${formatBytes(file.size || 0)}</div>
                    <div>${escapeHtml(file.signer || '-')}</div>
                    <div>${file.is_official ? yesStr : noStr}</div>
                    <div>${file.valid ? '<span class="state ok">' + validStr + '</span>' : '<span class="state bad">' + invalidStr + '</span>'}</div>
                    <div class="act"><button class="btn sm" data-path="${escapeHtml(path + '/' + file.name)}" onclick="importPackFromList(this.dataset.path)">${importStr}</button></div>
                </div>`).join('');
        }
        
        loading.style.display = 'none';
        table.classList.remove('hidden');
        
    } catch (e) {
        if (!pageCurrent()) return;
        loading.textContent = (typeof t === 'function' ? t('securityPage.packListLoadFailed') : '加载失败') + ': ' + e.message;
    }
}

async function importPackFromList(filePath) {
    const msg = typeof t === 'function' ? t('securityPage.confirmImportPack', { path: filePath }) : '确定要导入配置包: ' + filePath + ' ?';
    if (!await confirmAction(msg, { primary: t('common.import'), tone: 'neutral' })) return;
    
    try {
        const result = await api.configPackImport(null, filePath, false);
        if (result.code !== 0) throw new Error(result.message || result.error);
        
        showToast(typeof t === 'function' ? t('securityPage.importPackSuccess') : '配置包导入成功', 'success');
        hideConfigPackListModal();
    } catch (e) {
        showToast((typeof t === 'function' ? t('securityPage.importPackFailed') : '导入失败') + ': ' + e.message, 'error');
    }
}

// 辅助函数
function formatBytes(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
}

// =========================================================================
//                  HTTPS Certificate (PKI) Management
// =========================================================================

/**
 * 刷新证书状态卡片
 */
// Certificate UI helpers intentionally leave the global API protocol unchanged.
function certText(key, fallback, params = {}) {
    const value = typeof t === 'function' ? t(key, params) : key;
    return typeof value === 'string' && value !== key ? value : fallback;
}
function certError(result, fallback) {
    for (const value of [result?.message, result?.error]) {
        if (typeof value === 'string' && value.trim()) return value;
    }
    return fallback;
}
function certValidity(value) {
    const labels = {none: 'No certificate', invalid: 'Certificate cannot be parsed',
        time_unverified: 'Awaiting device time', not_yet_valid: 'Not yet valid',
        valid: 'Within validity period', expired: 'Expired'};
    return certText('pkiRepair.' + (labels[value] ? value : 'unknown'), labels[value] || 'Unknown / unconfirmed');
}
function certExpiry(info) {
    const seconds = info?.seconds_until_expiry;
    if (!['valid', 'expired'].includes(info?.validity) || typeof seconds !== 'number') return '';
    if (seconds >= 0 && seconds < 86400) return certText('pkiRepair.lessDay', 'Expires in less than a day');
    if (seconds < 0 && seconds > -86400) return certText('pkiRepair.expiredLessDay', 'Expired less than a day ago');
    const days = Math.floor(Math.abs(seconds) / 86400);
    return seconds < 0 ? certText('pkiPage.expiredDays', `Expired ${days} days ago`, {days}) :
        certText('pkiPage.remainingDays', `${days} days remaining`, {days});
}
async function refreshCertStatus() {
    const pageCurrent = capturePageValidity();
    if (!document.getElementById('cert-status-icon')) return;
    try {
        const result = await api.certStatus();
        if (!pageCurrent()) return;
        if (result.code !== 0 || !result.data) throw new Error(certError(result, 'Status unavailable'));
        const data = result.data;
        window._certPkiStatus = data;
        for (const [id, disabled] of Object.entries({
            'btn-cert-gen-key': false, 'btn-cert-gen-csr': !data.has_private_key,
            'btn-cert-install': !data.has_private_key, 'btn-cert-install-ca': false,
            'btn-cert-view': !data.has_certificate,
            'btn-cert-delete': !data.has_private_key && !data.has_certificate && !data.has_ca_chain
        })) document.getElementById(id).disabled = disabled;
        document.getElementById('cert-status-icon').textContent = '';
        document.getElementById('cert-status-text').textContent = certValidity(data.validity);
        const stored = certText('pkiRepair.stored', 'Saved');
        const missing = certText('pkiRepair.missing', 'Missing');
        const colon = certText('pkiRepair.colon', ': ');
        document.getElementById('cert-material-state').textContent =
            `${certText('pkiRepair.key', 'Key')}${colon}${data.has_private_key ? stored : missing} · ` +
            `${certText('pkiRepair.certificate', 'Certificate')}${colon}${data.has_certificate ? stored : missing} · ` +
            `${certText('pkiRepair.clientCa', 'Client verification CA')}${colon}${data.has_ca_chain ? stored : missing}`;
        const https = data.https;
        document.getElementById('cert-https-state').textContent = 'HTTPS' + colon +
            (typeof https?.running !== 'boolean' ? certValidity() :
                certText(https.running ? 'pkiRepair.running' : 'pkiRepair.stopped', https.running ? 'Running' : 'Not running')) +
            (https?.port ? ` (${https.port})` : '') +
            (https?.last_error ? ` — ${https.last_error_stage}: ${https.last_error}` : '');
        document.getElementById('cert-blocked-state').textContent = (data.blocked_by || []).map(
            reason => certText('pkiRepair.block_' + reason, reason)).join(' · ');
        document.getElementById('cert-restart-state').textContent = data.restart_required ?
            certText('pkiRepair.restart', 'Stored materials changed. The running service still uses the previous materials. Restart the device to apply; startup conditions must be met.') : '';
        document.getElementById('cert-active-fingerprint').textContent = https?.loaded_certificate_sha256 ?
            `${certText('pkiRepair.activeFingerprint', 'Active certificate SHA-256')}${colon}${https.loaded_certificate_sha256}` : '';
        document.getElementById('cert-no-key-hint').style.display = data.has_private_key ? 'none' : 'block';
        document.getElementById('cert-info-details').style.display = data.has_certificate ? 'block' : 'none';
        updateCertInfoDetails(data.cert_info);
        // Read the device clock, never display the browser's clock as device evidence.
        try {
            const time = await api.timeInfo();
            if (!pageCurrent()) return;
            if (time.code !== 0 || !time.data) throw new Error('Time unavailable');
            document.getElementById('cert-device-time').textContent =
                `${certText('pkiRepair.deviceTime', 'Device time')}${colon}${time.data.datetime || '—'} · ${time.data.source || '—'} · ` +
                certText(time.data.synced ? 'pkiRepair.synced' : 'pkiRepair.unsynced', time.data.synced ? 'Synchronized' : 'Not synchronized');
        } catch (_) {
            if (!pageCurrent()) return;
            document.getElementById('cert-device-time').textContent = certText('pkiRepair.timeUnknown', 'Device time unavailable; retry refresh');
        }
    } catch (e) {
        if (!pageCurrent()) return;
        document.getElementById('cert-status-text').textContent = certText('pkiPage.statusLoadFailed', 'Status refresh failed');
    }
}
function updateCertInfoDetails(info) {
    const badge = document.getElementById('cert-expiry-badge');
    badge.textContent = certExpiry(info);
    badge.style.display = badge.textContent ? 'inline-block' : 'none';
    if (!info) return;
    for (const [id, value] of Object.entries({
        'cert-subject-cn': info.subject_cn, 'cert-issuer-cn': info.issuer_cn,
        'cert-not-before': info.validity !== 'invalid' && info.not_before ? formatTimestamp(info.not_before) : '-',
        'cert-not-after': info.validity !== 'invalid' && info.not_after ? formatTimestamp(info.not_after) : '-',
        'cert-serial': (info.serial || '-') + (info.serial_truncated ? '…' : ''),
        'cert-valid-status': certValidity(info.validity)
    })) document.getElementById(id).textContent = value || '-';
}
async function syncCertificateTime() {
    const button = document.getElementById('cert-time-sync');
    button.disabled = true;
    try {
        const result = await api.timeSync(Date.now());
        if (result.code !== 0) throw new Error(certError(result, 'Time update failed'));
        await refreshCertStatus();
    } catch (e) {
        document.getElementById('cert-device-time').textContent = certError(e, 'Time update failed');
    } finally { button.disabled = false; }
}

function showCertGenKeyModal() {
    const modal = document.getElementById('cert-genkey-modal');
    const warningBox = document.getElementById('cert-genkey-existing-warning');
    const resultBox = document.getElementById('cert-genkey-result');
    
    // 如果已有密钥，显示警告
    if (window._certPkiStatus?.has_private_key) {
        warningBox.classList.remove('hidden');
    } else {
        warningBox.classList.add('hidden');
    }
    
    resultBox.classList.add('hidden');
    document.getElementById('cert-genkey-btn').classList.toggle('bad', !!window._certPkiStatus?.has_private_key);
    modal.classList.remove('hidden');
}

function hideCertGenKeyModal() {
    document.getElementById('cert-genkey-modal').classList.add('hidden');
}

async function generateCertKeypair() {
    const resultBox = document.getElementById('cert-genkey-result');
    const btn = document.getElementById('cert-genkey-btn');
    
    const force = window._certPkiStatus?.has_private_key;
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = certText('pkiPage.generatingKeyPair', '正在生成密钥对...');
    btn.disabled = true;
    
    try {
        const result = await api.certGenerateKeypair(force);
        if (result.code === 0) {
            resultBox.textContent = certText('pkiPage.ecdsaKeyPairSuccess', 'ECDSA P-256 密钥对生成成功！');
            resultBox.classList.add('success');
            showToast(certText('toast.keypairGenerated', '密钥对生成成功'), 'success');
            
            setTimeout(() => {
                hideCertGenKeyModal();
                refreshCertStatus();
            }, 1000);
        } else {
            throw new Error(certError(result, t('toast.generateFailed')));
        }
    } catch (e) {
        resultBox.textContent = (certText('pkiPage.generationFailedMsg', '生成失败: ' + e.message, { msg: e.message }));
        resultBox.classList.add('error');
    } finally {
        btn.disabled = false;
    }
}

function showCertCSRModal() {
    const modal = document.getElementById('cert-csr-modal');
    document.getElementById('csr-gen-result').classList.add('hidden');
    document.getElementById('csr-pem-output').value = '';
    modal.classList.remove('hidden');
}

function hideCertCSRModal() {
    document.getElementById('cert-csr-modal').classList.add('hidden');
}

async function generateCSR() {
    clearFieldErrors();
    const deviceId = document.getElementById('csr-device-id').value.trim();
    if (new TextEncoder().encode(deviceId).length > 63) {
        fieldError('csr-device-id', t('inputRepair.csrIdLimit'));
        return;
    }
    const org = document.getElementById('csr-org').value.trim();
    const ou = document.getElementById('csr-ou').value.trim();
    
    const resultBox = document.getElementById('csr-gen-result');
    const csrResultBox = document.getElementById('csr-result-box');
    const btn = document.getElementById('csr-gen-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = certText('pkiPage.generatingCsr', '正在生成 CSR...');
    btn.disabled = true;
    
    try {
        const opts = {};
        if (deviceId) opts.device_id = deviceId;
        if (org) opts.organization = org;
        if (ou) opts.org_unit = ou;
        
        const result = await api.certGenerateCSR(opts);
        if (result.code === 0 && result.data?.csr_pem) {
            resultBox.classList.add('hidden');
            csrResultBox.classList.remove('hidden');
            document.getElementById('csr-pem-output').value = result.data.csr_pem;
            showToast(certText('toast.csrGenerated', 'CSR 生成成功'), 'success');
        } else {
            throw new Error(certError(result, t('toast.generateFailed')));
        }
    } catch (e) {
        resultBox.textContent = (certText('pkiPage.generationFailedMsg', '生成失败: ' + e.message, { msg: e.message }));
        resultBox.classList.add('error');
    } finally {
        btn.disabled = false;
    }
}

function copyCSRToClipboard() {
    const csr = document.getElementById('csr-pem-output').value;
    navigator.clipboard.writeText(csr).then(() => {
        showToast(certText('toast.csrCopied', 'CSR 已复制到剪贴板'), 'success');
    }).catch(e => {
        showToast(certText('toast.copyFailedMsg', '复制失败: ' + e.message, { msg: e.message }), 'error');
    });
}

function showCertInstallModal() {
    const modal = document.getElementById('cert-install-modal');
    document.getElementById('cert-pem-input').value = '';
    document.getElementById('cert-install-result').classList.add('hidden');
    modal.classList.remove('hidden');
}

function hideCertInstallModal() {
    document.getElementById('cert-install-modal').classList.add('hidden');
}

async function installCertificate() { await installCertMaterial(false); }
async function installCertMaterial(ca) {
    const pem = document.getElementById(ca ? 'ca-pem-input' : 'cert-pem-input').value.trim();
    const resultBox = document.getElementById(ca ? 'ca-install-result' : 'cert-install-result');
    const button = document.getElementById(ca ? 'cert-ca-submit' : 'cert-install-submit');
    if (button.disabled) return;
    resultBox.classList.remove('hidden', 'success', 'error');
    if (!pem) {
        resultBox.textContent = certText('pkiRepair.empty', 'Provide PEM text');
        resultBox.classList.add('error'); return;
    }
    button.disabled = true;
    resultBox.textContent = certText('pkiPage.installingCert', 'Saving certificate materials…');
    let saved = false;
    try {
        const result = ca ? await api.certInstallCA(pem) : await api.certInstall(pem);
        saved = result.code === 0;
        if (saved) {
            resultBox.textContent = certText('pkiRepair.saved', 'Saved. Check validity and HTTPS status below.');
            resultBox.classList.add('success');
        } else {
            const msg = certError(result, certText('pkiPage.installFailed', 'Installation failed'));
            resultBox.textContent = certText('pkiPage.installFailedMsg', `Installation failed: ${msg}`, {msg});
            resultBox.classList.add('error');
        }
    } catch (_) {
        // A lost response is not proof that the server did not persist the request.
        resultBox.textContent = certText('pkiRepair.unconfirmed', 'Installation result unconfirmed. Refresh status to check before retrying.');
        resultBox.classList.add('error');
    } finally { button.disabled = false; }
    if (saved) {
        try { await refreshCertStatus(); } catch (_) { /* Saved result remains authoritative. */ }
        // Observe the asynchronous coordinator without reposting or starting HTTPS from UI.
        setTimeout(() => refreshCertStatus().catch(() => {}), 5500);
    }
}

function showCertInstallCAModal() {
    const modal = document.getElementById('cert-ca-modal');
    document.getElementById('ca-pem-input').value = '';
    document.getElementById('ca-install-result').classList.add('hidden');
    modal.classList.remove('hidden');
}

function hideCertInstallCAModal() {
    document.getElementById('cert-ca-modal').classList.add('hidden');
}

async function installCAChain() { await installCertMaterial(true); }

async function showCertViewModal() {
    const modal = document.getElementById('cert-view-modal');
    const loading = document.getElementById('cert-view-loading');
    const content = document.getElementById('cert-view-content');
    
    loading.style.display = 'block';
    content.classList.add('hidden');
    modal.classList.remove('hidden');
    
    try {
        const result = await api.certGetCertificate();
        if (result.code === 0 && result.data?.cert_pem) {
            document.getElementById('cert-view-pem').value = result.data.cert_pem;
            loading.style.display = 'none';
            content.classList.remove('hidden');
        } else {
            throw new Error(certError(result, t('toast.getCertFailed')));
        }
    } catch (e) {
        loading.textContent = certText('pkiPage.loadFailedMsg', '加载失败: ' + e.message, { msg: e.message });
    }
}

function hideCertViewModal() {
    document.getElementById('cert-view-modal').classList.add('hidden');
}

function copyCertToClipboard() {
    const cert = document.getElementById('cert-view-pem').value;
    navigator.clipboard.writeText(cert).then(() => {
        showToast(certText('toast.certCopied', '证书已复制到剪贴板'), 'success');
    }).catch(e => {
        showToast(certText('toast.copyFailedMsg', '复制失败: ' + e.message, { msg: e.message }), 'error');
    });
}

async function deleteCertCredentials() {
    if (!await confirmAction(certText('ui.confirmDeletePKI', '确定要删除所有 PKI 凭证吗？\n\n这将删除：\n• 私钥\n• 设备证书\n• CA 证书链\n\n此操作不可撤销！'), { primary: t('common.delete'), tone: 'danger' })) {
        return;
    }
    
    try {
        const result = await api.certDelete();
        if (result.code === 0) {
            showToast(certText('toast.pkiDeleted', 'PKI 凭证已删除'), 'success');
            await refreshCertStatus();
        } else {
            throw new Error(certError(result, t('errors.deleteFailed')));
        }
    } catch (e) {
        showToast((certText('toast.deleteFailedMsg', '删除失败: ' + e.message, { msg: e.message })), 'error');
    }
}

let keyGenerationOperation = null;
let keyGenerationModalVersion = 0;
let keyGenerationOperationSequence = 0;

function setKeyGenerationStatus(message) {
    const status = document.getElementById('keygen-status');
    if (status) status.textContent = message;
}

function showGenerateKeyModal() {
    ++keyGenerationModalVersion;
    document.getElementById('keygen-modal').classList.remove('hidden');
    document.getElementById('keygen-id').value = '';
    document.getElementById('keygen-type').value = 'rsa2048';
    document.getElementById('keygen-comment').value = '';
    document.getElementById('keygen-alias').value = '';
    document.getElementById('keygen-exportable').checked = false;
    document.getElementById('keygen-hidden').checked = false;
    document.getElementById('keygen-ec-warn').classList.add('hidden');
    const button = document.getElementById('keygen-submit');
    if (button) {
        button.disabled = !!keyGenerationOperation;
        button.dataset.keyGeneration = keyGenerationOperation ? String(keyGenerationOperation.sequence) : '';
    }
    const close = document.getElementById('keygen-close');
    if (close) close.textContent = t(keyGenerationOperation ? 'keyGeneration.closeRunning' : 'common.cancel');
    setKeyGenerationStatus(keyGenerationOperation ? t('keyGeneration.running', {id: keyGenerationOperation.id}) : '');
}

function hideGenerateKeyModal() {
    ++keyGenerationModalVersion;
    document.getElementById('keygen-modal').classList.add('hidden');
}

async function generateKey() {
    if (keyGenerationOperation) return;
    const id = document.getElementById('keygen-id').value.trim();
    const type = document.getElementById('keygen-type').value;
    const comment = document.getElementById('keygen-comment').value.trim();
    const alias = document.getElementById('keygen-alias').value.trim();
    const exportable = document.getElementById('keygen-exportable').checked;
    const hidden = document.getElementById('keygen-hidden').checked;
    if (!id) { showToast(t('toast.enterKeyId'), 'error'); return; }
    if (new TextEncoder().encode(id).length > 10 || id.includes(',')) {
        setKeyGenerationStatus(t('keyGeneration.invalidId'));
        return;
    }
    const pageCurrent = capturePageValidity();
    const modal = document.getElementById('keygen-modal');
    const version = keyGenerationModalVersion;
    const operation = {id, sequence: ++keyGenerationOperationSequence};
    keyGenerationOperation = operation;
    const ownsModal = () => pageCurrent() && version === keyGenerationModalVersion && document.getElementById('keygen-modal') === modal;
    const button = document.getElementById('keygen-submit');
    if (button) {
        button.disabled = true;
        button.dataset.keyGeneration = String(operation.sequence);
    }
    const close = document.getElementById('keygen-close');
    if (close) close.textContent = t('keyGeneration.closeRunning');
    setKeyGenerationStatus(t('keyGeneration.running', {id}));
    let requestId;
    const trace = (stage, fields) => api.recordKeyGeneration(stage, fields, requestId);
    try {
        const pending = api.keyGenerate(id, type, comment, exportable, alias, hidden);
        requestId = api.keyGenerationTrace?.requestId;
        const result = await pending;
        trace('api_return', {code: typeof result?.code === 'number' ? result.code : null});
        if (typeof result?.code === 'number' && result.code !== 0 && (!result.httpStatus || result.httpStatus < 400)) {
            const detail = result.data;
            const reasons = {'key_storage_full':'storageFull', 'key_id_occupied':'occupied', 'key_limit_reached':'limit',
                'key_invalid_id':'invalidId', 'Memory allocation failed':'memory'};
            const reason = typeof result.rawMessage === 'string' ? result.rawMessage : apiErrorReason(result);
            let message = reasons[reason] ? t('keyGeneration.' + reasons[reason]) : apiErrorMessage(result, 'keyGenerate');
            if (detail?.failed_stage) message += '\n' + t('keyGeneration.stage', {stage: detail.failed_stage, error: detail.esp_error || ''});
            if (detail?.cleanup_complete === false) message += '\n' + t('keyGeneration.cleanupFailed', {error: detail.cleanup_error || ''});
            if (reason === 'key_response_failed' || (!reason && !detail?.failed_stage)) throw new ApiOperationError(result, 'keyGenerate', {kind: 'format', uncertain: true});
            trace('business_failed', {stage: detail?.failed_stage, cleanupComplete: detail?.cleanup_complete});
            if (ownsModal()) { setKeyGenerationStatus(message); showToast(message, 'error'); }
            return;
        }
        if (result?.code !== 0 || (result.httpStatus && result.httpStatus >= 400) || result.data?.generated !== true || result.data?.id !== id) {
            throw new ApiOperationError(result, 'keyGenerate', {kind: 'format', uncertain: true});
        }
        trace('saved_confirmed');
        if (!pageCurrent()) return;
        let notificationVersion = null;
        if (ownsModal()) {
            hideGenerateKeyModal();
            notificationVersion = keyGenerationModalVersion;
            trace('modal_closed', {hidden: modal.classList.contains('hidden')});
            showToast(t('toast.keyGenerated', {name: alias || id}), 'success', 3000, {isCurrent: pageCurrent});
            trace('toast_shown', {visible: document.getElementById('toast')?.classList.contains('show') === true});
        }
        trace('list_start');
        try {
            const refreshed = await refreshSecurityPage({keysOnly: true});
            trace('list_done', {loaded: refreshed?.keysLoaded === true, current: refreshed?.keysCurrent === true});
            if (pageCurrent() && notificationVersion === keyGenerationModalVersion && document.getElementById('keygen-modal') === modal && refreshed?.keysCurrent && !refreshed?.keysLoaded) showToast(t('keyGeneration.refreshFailed'), 'error');
        } catch (e) {
            trace('list_error', {kind: e.name});
            if (pageCurrent() && notificationVersion === keyGenerationModalVersion && document.getElementById('keygen-modal') === modal) showToast(t('keyGeneration.refreshFailed'), 'error');
        }
    } catch (e) {
        trace('result_unknown', {kind: e.kind || e.name});
        if (!pageCurrent()) return;
        if (ownsModal()) setKeyGenerationStatus(t('keyGeneration.unknown'));
        // One read-only check is evidence of a current record, never a retry or proof of this POST.
        try {
            const info = await api.keyInfo(id);
            const found = info?.code === 0 && info.data?.id === id;
            trace('record_checked', {found});
            if (ownsModal()) setKeyGenerationStatus(t(found ? 'keyGeneration.recordFound' : 'keyGeneration.unknown'));
        } catch (_) { trace('record_check_failed'); }
        if (pageCurrent()) {
            try { await refreshSecurityPage({keysOnly: true}); } catch (_) { trace('list_error'); }
        }
    } finally {
        if (keyGenerationOperation === operation) {
            keyGenerationOperation = null;
            // A rebuilt modal may subscribe its controls to this same pending operation.
            // Only those controls are released; result messages still belong to the original modal.
            const currentButton = document.getElementById('keygen-submit');
            if (currentButton?.dataset.keyGeneration === String(operation.sequence)) {
                currentButton.disabled = false;
                currentButton.dataset.keyGeneration = '';
                const currentClose = document.getElementById('keygen-close');
                if (currentClose) currentClose.textContent = t('common.cancel');
                const currentModal = document.getElementById('keygen-modal');
                if (currentModal && !currentModal.classList.contains('hidden') &&
                    (version !== keyGenerationModalVersion || currentModal !== modal)) setKeyGenerationStatus('');
            }
        }
        trace('ui_done');
    }
}

function formatTimestamp(ts) {
    if (!ts) return '-';
    const date = new Date(ts * 1000);
    return date.toLocaleString('zh-CN');
}

// =========================================================================
//                         工具函数
// =========================================================================

function formatUptime(ms) {
    if (!ms) return '-';
    const seconds = Math.floor(ms / 1000);
    const minutes = Math.floor(seconds / 60);
    const hours = Math.floor(minutes / 60);
    const days = Math.floor(hours / 24);
    
    const d = typeof t === 'function' ? t('common.days') : 'd';
    const h = typeof t === 'function' ? t('common.hours') : 'h';
    const m = typeof t === 'function' ? t('common.minutes') : 'min';
    const s = typeof t === 'function' ? t('common.seconds') : 's';
    
    if (days > 0) return `${days} ${d} ${hours % 24} ${h}`;
    if (hours > 0) return `${hours} ${h} ${minutes % 60} ${m}`;
    if (minutes > 0) return `${minutes} ${m}`;
    return `${seconds} ${s}`;
}

function formatBytes(bytes) {
    if (!bytes) return '-';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

// 确认弹窗（替代原生 confirm）：返回 Promise。tone='danger' 时主按钮为红色且不响应 Enter；third 为中间的第二选项（返回 'alt'）
let confirmSheetSeq = 0;
function confirmSheet({ title, body = '', bodyHtml = '', primary, tone = 'neutral', secondary, third, width = 420 } = {}) {
    return new Promise(resolve => {
        const modal = document.createElement('div');
        modal.className = 'modal confirm-sheet';
        modal.id = 'confirm-sheet-' + (++confirmSheetSeq);
        let settled = false;
        const finish = v => {
            if (settled) return;
            settled = true;
            document.removeEventListener('keydown', onKey, true);
            modal.remove();
            resolve(v);
        };
        const onKey = e => {
            if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); finish(false); }
            else if (e.key === 'Enter' && tone !== 'danger' && e.target.tagName !== 'BUTTON') { e.preventDefault(); finish(true); }
        };
        modal.innerHTML = sheet(width, escapeHtml(title), `<div class="t-body" style="color:var(--ink-2);white-space:pre-line">${bodyHtml || escapeHtml(body)}</div>`,
            `<button type="button" class="btn lg" data-r="0">${secondary || t('common.cancel')}</button>` +
            (third ? `<button type="button" class="btn lg" data-r="alt">${third}</button>` : '') +
            `<button type="button" class="btn lg primary${tone === 'danger' ? ' bad' : ''}" data-r="1">${primary || t('common.confirm')}</button>`);
        modal.onclick = e => {
            if (e.target === modal) return finish(false);
            const b = e.target.closest('button[data-r]');
            if (b) finish(b.dataset.r === '1' ? true : b.dataset.r === 'alt' ? 'alt' : false);
        };
        document.body.appendChild(modal);
        document.addEventListener('keydown', onKey, true);
        // 破坏性确认默认聚焦"取消"，避免误触
        modal.querySelector(tone === 'danger' ? 'button[data-r="0"]' : 'button[data-r="1"]').focus();
    });
}

async function confirmAction(message, opts = {}) {
    if (!window.i18n?.isReady() || !message || message.includes(i18n.unavailable())) {
        showToast(i18n.unavailable(), 'error');
        return false;
    }
    return confirmSheet({ title: opts.title || t('common.confirm'), body: message, primary: opts.primary, tone: opts.tone, secondary: opts.secondary });
}

let toastTimer = null;
let toastDeadline = 0;
let toastPriority = 0;
function showToast(message, type = 'info', duration = 3000, attempt = null) {
    if (attempt && !attempt.isCurrent()) return;
    let toast = document.getElementById('toast');
    if (!toast) {
        toast = document.createElement('div');
        toast.id = 'toast';
        document.body.appendChild(toast);
    }
    const priority = type === 'error' ? 2 : type === 'warning' ? 1 : 0;
    // A routine success must not erase an active long warning.
    if (!attempt && Date.now() < toastDeadline && toastPriority > priority && toastDeadline - Date.now() > duration) return;
    clearTimeout(toastTimer);
    toast.textContent = String(message ?? '');
    toast.style.whiteSpace = 'pre-wrap';
    toast.setAttribute('role', priority ? 'alert' : 'status');
    toast.className = `toast toast-${type} show`;
    toastPriority = priority;
    toastDeadline = Date.now() + duration;
    toastTimer = setTimeout(() => { toast.classList.remove('show'); toastDeadline = 0; }, duration);
}

// =========================================================================
//                         终端页面
// =========================================================================

async function loadTerminalPage() {
    const pageCurrent = capturePageValidity();
    // 取消系统页面的订阅
    
    // 清理之前的终端实例
    if (webTerminal) {
        webTerminal.destroy();
        webTerminal = null;
    }
    
    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page page-terminal">
            <div class="acts">
                <button class="btn sm" onclick="window.showTerminalLogsModal();"><svg class="i"><use href="#ri-file-text-line"/></svg>${t('terminal.systemLogsButton')}</button>
                <button class="btn sm" onclick="terminalClear()"><svg class="i"><use href="#ri-delete-bin-line"/></svg>${t('terminal.clearScreen')}</button>
                <button class="btn sm dg" onclick="terminalDisconnect()">${t('terminal.disconnect')}</button>
            </div>
            <div class="terminal-box"><div class="terminal-container" id="terminal-container"></div></div>
            <div class="t-note terminal-help"><svg class="i"><use href="#ri-information-line"/></svg><span>${t('terminal.terminalHintBrief')}</span></div>
        </div>
        
        <!-- 日志弹窗 -->
        <div id="terminal-logs-modal" class="modal" style="display:none" onclick="if(event.target===this) closeTerminalLogsModal()">${sheet(760, t('terminal.systemLogTitle'), `
            <div style="display:flex;gap:8px;align-items:center;margin-bottom:12px">
                <select class="field" id="modal-log-level-filter" style="width:100px" onchange="updateModalLogFilter()">
                    <option value="5">${t('terminal.levelAll')}</option>
                    <option value="1">ERROR</option>
                    <option value="2">WARN+</option>
                    <option value="3" selected>INFO+</option>
                    <option value="4">DEBUG+</option>
                </select>
                <input type="text" class="field" id="modal-log-tag-filter" placeholder="${t('common.filterTag')}" aria-label="${t('common.filterTag')}" style="width:150px" onkeyup="debounceRenderModalLogs()">
                <input type="text" class="field" id="modal-log-keyword-filter" placeholder="${t('common.searchLogs')}" aria-label="${t('common.searchLogs')}" style="flex:1;min-width:0" onkeyup="debounceRenderModalLogs()">
                <label style="display:flex;gap:6px;align-items:center;margin-left:8px"><input type="checkbox" class="switch" role="switch" id="modal-log-auto-scroll" checked><span class="t-label">${t('terminal.autoScroll')}</span></label>
            </div>
            <div class="term logv" id="modal-log-container"><div class="log-empty">${t('terminal.waitingLogs')}</div></div>`,
            `<span class="inl" style="margin-right:auto"><span id="modal-ws-status" class="ws-status connecting" title="${t('common.wsStatus')}"></span><span id="modal-log-stats" class="t-note"></span></span><button class="btn lg" onclick="loadModalHistoryLogs()"><svg class="i"><use href="#ri-refresh-line"/></svg>${t('common.refreshLogs')}</button><button class="btn lg" onclick="clearModalLogs()">${t('common.clearLogs')}</button><button class="btn lg primary" onclick="closeTerminalLogsModal()">${t('common.close')}</button>`)}</div>
    `;
    
    // 初始化终端（标签页可见时才连接，避免后台标签抢占当前会话导致 session_closed）
    webTerminal = new WebTerminal('terminal-container');
    const terminal = webTerminal;
    const ok = await terminal.init();
    if (!pageCurrent()) return;
    if (ok) {
        function doConnect() {
            if (!pageCurrent()) return;
            if (document.visibilityState === 'visible') {
                terminal.connect();
            } else {
                document.addEventListener('visibilitychange', function handler() {
                    if (!pageCurrent()) { document.removeEventListener('visibilitychange', handler); return; }
                    if (document.visibilityState === 'visible') {
                        document.removeEventListener('visibilitychange', handler);
                        terminal.connect();
                    }
                });
            }
        }
        doConnect();
    }
}

function terminalClear() {
    if (webTerminal && webTerminal.terminal) {
        webTerminal.terminal.clear();
        webTerminal.writePrompt();
    }
}

function terminalDisconnect() {
    if (webTerminal) {
        webTerminal.disconnect();
        showToast(typeof t === 'function' ? t('toast.terminalDisconnected') : '终端已断开', 'info');
    }
}

// 终端页面日志模态框
let modalLogEntries = [];
let modalLogDebounceTimer = null;
let modalLogSubscribed = false;
const MAX_MODAL_LOG_ENTRIES = 1000;

function showTerminalLogsModal() {
    const modal = document.getElementById('terminal-logs-modal');
    if (!modal) return;
    
    modal.style.display = 'flex';
    modalLogEntries.length = 0;
    subscribeToModalLogs();
}

function closeTerminalLogsModal() {
    const modal = document.getElementById('terminal-logs-modal');
    if (modal) {
        modal.style.display = 'none';
        unsubscribeFromModalLogs();
        modalLogEntries.length = 0;
    }
}

// 订阅模态框日志
function subscribeToModalLogs() {
    const levelFilter = document.getElementById('modal-log-level-filter')?.value || '3';
    const minLevel = parseInt(levelFilter);
    
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        window.ws.send({
            type: 'log_subscribe',
            minLevel: minLevel
        });
        modalLogSubscribed = true;
        updateModalWsStatus(true);
        loadModalHistoryLogs();
    } else {
        updateModalWsStatus(false);
        setTimeout(subscribeToModalLogs, 1000);
    }
}

// 取消订阅模态框日志
function unsubscribeFromModalLogs() {
    if (window.ws && window.ws.readyState === WebSocket.OPEN && modalLogSubscribed) {
        window.ws.send({ type: 'log_unsubscribe' });
    }
    modalLogSubscribed = false;
    updateModalWsStatus(false);
}

// 加载历史日志
async function loadModalHistoryLogs() {
    if (!window.ws || window.ws.readyState !== WebSocket.OPEN) return;
    
    const levelFilter = document.getElementById('modal-log-level-filter')?.value || '3';
    
    // 通过 WebSocket 请求历史日志
    window.ws.send({
        type: 'log_get_history',
        limit: 500,
        minLevel: 1,
        maxLevel: parseInt(levelFilter)
    });
}

// 更新模态框WebSocket状态显示
function updateModalWsStatus(connected) {
    const statusEl = document.getElementById('modal-ws-status');
    if (statusEl) {
        if (connected) {
            statusEl.className = 'ws-status connected';
            statusEl.title = t('promptRepair.logConnected');
        } else {
            statusEl.className = 'ws-status connecting';
            statusEl.title = t('promptRepair.logDisconnected');
        }
    }
}

function updateModalLogFilter() {
    const levelFilter = document.getElementById('modal-log-level-filter')?.value || '3';
    const minLevel = parseInt(levelFilter);
    
    // 更新 WebSocket 订阅级别
    if (window.ws && window.ws.readyState === WebSocket.OPEN && modalLogSubscribed) {
        window.ws.send({
            type: 'log_set_level',
            minLevel: minLevel
        });
    }
    
    // 重新渲染现有日志
    renderModalLogs();
}

function debounceRenderModalLogs() {
    if (modalLogDebounceTimer) clearTimeout(modalLogDebounceTimer);
    modalLogDebounceTimer = setTimeout(renderModalLogs, 300);
}

function renderModalLogs() {
    const container = document.getElementById('modal-log-container');
    if (!container) return;
    
    // 获取过滤条件
    const levelFilter = parseInt(document.getElementById('modal-log-level-filter')?.value || '3');
    const tagFilter = document.getElementById('modal-log-tag-filter')?.value.toLowerCase().trim() || '';
    const keywordFilter = document.getElementById('modal-log-keyword-filter')?.value.toLowerCase().trim() || '';
    
    // 过滤日志
    let filtered = modalLogEntries.filter(entry => {
        // 级别过滤
        if (entry.level > levelFilter) return false;
        // TAG 过滤
        if (tagFilter && !entry.tag.toLowerCase().includes(tagFilter)) return false;
        // 关键词过滤
        if (keywordFilter && !entry.message.toLowerCase().includes(keywordFilter)) return false;
        return true;
    });
    
    // 更新统计
    const statsElem = document.getElementById('modal-log-stats');
    if (statsElem) {
        statsElem.textContent = typeof t === 'function' ? t('ui.displayStats', { filtered: filtered.length, total: modalLogEntries.length }) : `显示 ${filtered.length}/${modalLogEntries.length} 条`;
    }
    
    if (filtered.length === 0) {
        container.innerHTML = `<div class="log-empty">${t('fanPage.noLogs')}</div>`;
        return;
    }
    
    // 渲染日志
    const html = filtered.map(entry => {
        const time = new Date(entry.timestamp).toLocaleTimeString('zh-CN', { hour12: false });
        const levelClass = `level-${entry.levelName.toLowerCase()}`;
        
        // 高亮关键词
        let message = escapeHtml(entry.message);
        if (keywordFilter) {
            const regex = new RegExp(`(${escapeRegex(keywordFilter)})`, 'gi');
            message = message.replace(regex, '<span class="log-highlight">$1</span>');
        }
        
        return `
            <div class="log-entry ${levelClass}">
                <span class="log-time">${time}</span>
                <span class="log-level">${entry.levelName.charAt(0)}</span>
                <span class="log-tag">${escapeHtml(entry.tag)}</span>
                <span class="log-message">${message}${entry.task ? ` <span class="log-task">[${escapeHtml(entry.task)}]</span>` : ''}</span>
            </div>
        `;
    }).join('');
    
    container.innerHTML = html;
    
    // 自动滚动
    const autoScroll = document.getElementById('modal-log-auto-scroll')?.checked;
    if (autoScroll) {
        container.scrollTop = container.scrollHeight;
    }
}

function clearModalLogs() {
    modalLogEntries.length = 0;
    renderModalLogs();
}

// 处理模态框实时日志消息
function handleModalLogMessage(msg) {
    const logEntry = {
        level: msg.level || 3,
        levelName: getLevelName(msg.level || 3),
        tag: msg.tag || 'unknown',
        message: msg.message || '',
        timestamp: msg.timestamp || Date.now(),
        task: msg.task || ''
    };
    
    modalLogEntries.push(logEntry);
    
    if (modalLogEntries.length > MAX_MODAL_LOG_ENTRIES) {
        modalLogEntries.shift();  // 移除最旧的日志
    }
    
    // 重新渲染
    renderModalLogs();
}

function getLevelName(level) {
    const names = { 1: 'ERROR', 2: 'WARN', 3: 'INFO', 4: 'DEBUG', 5: 'VERBOSE' };
    return names[level] || 'UNKNOWN';
}



function escapeRegex(text) {
    return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// 暴露给全局作用域（WebSocket 处理需要）
window.getLevelName = getLevelName;
window.handleModalLogMessage = handleModalLogMessage;

// 暴露给 HTML onclick
window.showTerminalLogsModal = showTerminalLogsModal;
window.closeTerminalLogsModal = closeTerminalLogsModal;
window.loadModalHistoryLogs = loadModalHistoryLogs;
window.updateModalLogFilter = updateModalLogFilter;
window.debounceRenderModalLogs = debounceRenderModalLogs;
window.clearModalLogs = clearModalLogs;
window.closeLoginModal = closeLoginModal;
window.confirmReboot = confirmReboot;
window.syncTimeFromBrowser = syncTimeFromBrowser;
window.forceNtpSync = forceNtpSync;
window.showTimezoneModal = showTimezoneModal;
window.hideTimezoneModal = hideTimezoneModal;
window.applyTimezone = applyTimezone;
window.serviceAction = serviceAction;
window.setBrightness = setBrightness;
window.toggleLed = toggleLed;
window.clearLed = clearLed;
window.fillColor = fillColor;
window.quickFill = quickFill;
window.startEffect = startEffect;
window.stopEffect = stopEffect;
window.showEffectConfig = showEffectConfig;
window.applyEffect = applyEffect;
window.updateBrightnessLabel = updateBrightnessLabel;
window.showWifiScan = showWifiScan;
window.connectWifi = connectWifi;
window.toggleNat = toggleNat;
window.setFanSpeed = setFanSpeed;
// Commands page functions
window.loadCommandsPage = loadCommandsPage;
window.selectHost = selectHost;
window.showAddCommandModal = showAddCommandModal;
window.closeCommandModal = closeCommandModal;
window.selectCmdIcon = selectCmdIcon;
window.saveCommand = saveCommand;
window.editCommand = editCommand;
window.deleteCommand = deleteCommand;
window.exportSshCommand = exportSshCommand;
window.showExportSshCommandModal = showExportSshCommandModal;
window.hideExportSshCommandModal = hideExportSshCommandModal;
window.doExportSshCommandFromModal = doExportSshCommandFromModal;
window.showImportSshCommandModal = showImportSshCommandModal;
window.hideImportSshCommandModal = hideImportSshCommandModal;
window.previewSshCommandImport = previewSshCommandImport;
window.confirmSshCommandImport = confirmSshCommandImport;
window.executeCommand = executeCommand;
window.cancelExecution = cancelExecution;
window.clearExecResult = clearExecResult;
window.validateCommandId = validateCommandId;
// Security page functions
window.refreshSshHostsList = refreshSshHostsList;
window.refreshKnownHostsList = refreshKnownHostsList;
window.showFullFingerprint = showFullFingerprint;
window.removeKnownHost = removeKnownHost;
window.deleteSshHostFromSecurity = deleteSshHostFromSecurity;
window.testSshConnection = testSshConnection;
window.testSshHostByIndex = testSshHostByIndex;
window.removeHostByIndex = removeHostByIndex;
window.exportSshHost = exportSshHost;
window.showExportSshHostModal = showExportSshHostModal;
window.hideExportSshHostModal = hideExportSshHostModal;
window.doExportSshHostFromModal = doExportSshHostFromModal;
window.showImportSshHostModal = showImportSshHostModal;
window.hideImportSshHostModal = hideImportSshHostModal;
window.previewSshHostImport = previewSshHostImport;
window.confirmSshHostImport = confirmSshHostImport;
window.revokeKeyFromHost = revokeKeyFromHost;
window.hideRevokeHostModal = hideRevokeHostModal;
window.doRevokeFromHost = doRevokeFromHost;
window.deleteKey = deleteKey;
window.exportKey = exportKey;
window.exportPrivateKey = exportPrivateKey;
window.showPubkeyModal = showPubkeyModal;
window.closePubkeyModal = closePubkeyModal;
window.copyPubkey = copyPubkey;
window.downloadPubkey = downloadPubkey;
window.showPrivkeyModal = showPrivkeyModal;
window.closePrivkeyModal = closePrivkeyModal;
window.copyPrivkey = copyPrivkey;
window.downloadPrivkey = downloadPrivkey;
window.removeHost = removeHost;
window.clearAllHosts = clearAllHosts;
window.showGenerateKeyModal = showGenerateKeyModal;
window.hideGenerateKeyModal = hideGenerateKeyModal;
window.generateKey = generateKey;
window.showDeployKeyModal = showDeployKeyModal;
window.hideDeployKeyModal = hideDeployKeyModal;
window.deployKey = deployKey;
window.showRevokeKeyModal = showRevokeKeyModal;
window.hideRevokeKeyModal = hideRevokeKeyModal;
window.revokeKey = revokeKey;
window.showHostMismatchModal = showHostMismatchModal;
window.hideHostMismatchModal = hideHostMismatchModal;
window.removeAndRetry = removeAndRetry;
window.terminalClear = terminalClear;
window.terminalDisconnect = terminalDisconnect;
// 文件管理
window.navigateToPath = navigateToPath;
window.showUploadDialog = showUploadDialog;
window.closeUploadDialog = closeUploadDialog;
window.showNewFolderDialog = showNewFolderDialog;
window.closeNewFolderDialog = closeNewFolderDialog;
window.createNewFolder = createNewFolder;
window.showRenameDialog = showRenameDialog;
window.closeRenameDialog = closeRenameDialog;
window.doRename = doRename;
window.downloadFile = downloadFile;
window.deleteFile = deleteFile;
window.uploadFiles = uploadFiles;
window.handleFileSelect = handleFileSelect;
window.removeUploadFile = removeUploadFile;
window.refreshFilesPage = refreshFilesPage;
// 批量文件操作
window.toggleFileSelection = toggleFileSelection;
window.toggleSelectAll = toggleSelectAll;
window.clearSelection = clearSelection;
window.batchDelete = batchDelete;
window.batchDownload = batchDownload;
// Matrix 滤镜
window.selectFilter = selectFilter;
window.applySelectedFilter = applySelectedFilter;
window.applyFilter = applyFilter;
window.stopFilter = stopFilter;
// Matrix 功能
window.displayImage = displayImage;
window.generateQrCode = generateQrCode;
window.clearQrBgImage = clearQrBgImage;
window.displayText = displayText;
window.stopText = stopText;
window.saveLedConfig = saveLedConfig;
window.loadFontList = loadFontList;
// 文件选择器
window.openFilePickerFor = openFilePickerFor;
window.browseImages = browseImages;
window.filePickerItemClick = filePickerItemClick;
window.filePickerItemDblClick = filePickerItemDblClick;
window.filePickerGoUp = filePickerGoUp;
window.closeFilePicker = closeFilePicker;
window.confirmFilePicker = confirmFilePicker;
window.createAndOpenDir = createAndOpenDir;
// 网络配置
window.hideWifiScan = hideWifiScan;
window.disconnectWifi = disconnectWifi;
window.showApStations = showApStations;
window.hideApStations = hideApStations;
window.showApConfig = showApConfig;
window.hideApConfig = hideApConfig;
window.applyApConfig = applyApConfig;
window.showDhcpClients = showDhcpClients;
window.hideDhcpClients = hideDhcpClients;
window.loadDhcpClients = loadDhcpClients;
window.setWifiMode = setWifiMode;
window.setHostname = setHostname;
window.saveNatConfig = saveNatConfig;
// 数据监控组件
window.refreshDataWidgets = refreshDataWidgets;
window.showWidgetManager = showWidgetManager;
window.showAddWidgetPanel = showAddWidgetPanel;
window.addWidgetFromPreset = addWidgetFromPreset;
window.createNewWidget = createNewWidget;
window.showWidgetEditPanel = showWidgetEditPanel;
window.saveWidgetEdit = saveWidgetEdit;
window.deleteDataWidget = deleteDataWidget;
window.moveWidget = moveWidget;
window.selectVariableForWidget = selectVariableForWidget;
window.updateRefreshInterval = updateRefreshInterval;
// 日志组件
window.toggleLogReading = toggleLogReading;
window.startLogReading = startLogReading;
window.stopLogReading = stopLogReading;
window.refreshLogOnce = refreshLogOnce;
window.clearLogWidget = clearLogWidget;
window.updateLayoutPreview = function() {
    // 更新布局选项的激活状态
    document.querySelectorAll('.dw-layout-option').forEach(opt => {
        const radio = opt.querySelector('input[type="radio"]');
        opt.classList.toggle('active', radio?.checked);
    });
};

// 初始化滑块事件
document.addEventListener('DOMContentLoaded', function() {
    // 滤镜速度滑块
    document.body.addEventListener('input', function(e) {
        if (e.target.id === 'matrix-filter-speed') {
            const valueSpan = document.getElementById('filter-speed-value');
            if (valueSpan) valueSpan.textContent = e.target.value;
        }
    });
});

// =========================================================================
//                         OTA 页面
// =========================================================================

async function loadOtaPage() {
    const pageCurrent = capturePageValidity();
    clearInterval(refreshInterval);

    // 取消系统页面的订阅

    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page page-ota">
          <div class="ota-col">
            <h1 class="t-title">${t('ota.firmwareUpgrade')}</h1>
            
            <!-- 核心信息区：版本 + OTA 服务器 -->
            <div class="card">
                <div class="ota-ver">
                    <span class="t-label">${t('ota.currentVersion')}</span>
                    <span class="t-value ota-vnum" id="ota-current-version">-</span>
                </div>
                <div class="t-note" style="margin-top:4px" id="ota-version-meta">${t('common.loading')}</div>
                <hr class="sep" style="margin:16px 0">
                <div class="ota-server">
                    <span class="t-label">${t('ota.serverUrl')}</span>
                    <input type="text" id="ota-server-input" class="field" style="flex:1" placeholder="http://192.168.1.100:57807" aria-label="${t('ota.serverUrl')}">
                    <button class="btn" onclick="saveOtaServer()" title="${t('ota.saveToDevice')}">${t('ota.saveServer')}</button>
                    <button class="btn primary" onclick="checkForUpdates()">${t('ota.checkUpdate')}</button>
                </div>
                <!-- 更新状态区（动态显示） -->
                <div id="ota-update-status" class="ota-update-status" style="display:none"></div>
            </div>
            
            <!-- 分区管理 -->
            <details class="card ota-section" style="padding:0" open>
                <summary class="dis">${t('ota.partitionManage')}</summary>
                <div class="ota-partitions" id="ota-partitions">
                    <div class="t-note">${t('common.loading')}</div>
                </div>
            </details>
            
            <!-- 手动升级 -->
            <details class="card ota-section" style="padding:0" id="ota-manual">
                <summary class="dis">${t('ota.manualUpgrade')}</summary>
                <div class="ota-manual-body">
                    <div class="fl">
                        <label>${t('ota.fromUrl')}</label>
                        <div class="acts">
                            <input type="text" id="ota-url-input" class="field" style="flex:1" placeholder="http://example.com/firmware.bin" aria-label="${t('ota.fromUrl')}">
                            <button class="btn primary" onclick="otaFromUrl()"><svg class="i"><use href="#ri-upload-line"/></svg>${t('ota.upgrade')}</button>
                        </div>
                        <div class="ota-opts">
                            <label><input type="checkbox" id="ota-url-include-www" checked>${t('ota.includeWww')}</label>
                            <label><input type="checkbox" id="ota-url-skip-verify">${t('ota.skipCertVerify')}</label>
                        </div>
                    </div>
                    <hr class="sep">
                    <div class="fl">
                        <label>${t('ota.fromFile')}</label>
                        <div class="acts">
                            <input type="text" id="ota-file-input" class="field" style="flex:1" placeholder="/sdcard/firmware.bin" aria-label="${t('ota.fromFile')}">
                            <button class="btn primary" onclick="otaFromFile()"><svg class="i"><use href="#ri-upload-line"/></svg>${t('ota.upgrade')}</button>
                        </div>
                        <div class="ota-opts">
                            <label><input type="checkbox" id="ota-file-include-www" checked>${t('ota.includeWww')}</label>
                        </div>
                    </div>
                    <!-- 升级进度区（动态显示） -->
                    <div id="ota-progress-section" style="display:none">
                        <hr class="sep" style="margin-bottom:12px">
                        <div class="ota-prog">
                            <div class="bar" style="flex:1"><i id="ota-progress-bar" style="width:0%"></i></div>
                            <span class="t-value num" id="ota-progress-percent">0%</span>
                            <button class="btn sm dg" id="ota-abort-btn" onclick="abortOta()"><svg class="i"><use href="#ri-stop-line"/></svg>${t('ota.abort')}</button>
                        </div>
                        <div class="t-note ota-prog-note"><span id="ota-state-text">${t('ota.preparing')}</span> · <span id="ota-progress-size">0 / 0</span> <span id="ota-message"></span></div>
                    </div>
                </div>
            </details>
          </div>
        </div>
    `;
    
    // 加载数据
    await loadOtaData();
    if (!pageCurrent()) return;
    
    // 设置定时刷新进度
    refreshInterval = setInterval(refreshOtaProgress, 1000);
}

async function loadOtaData() {
    const pageCurrent = capturePageValidity();
    try {
        // 1. 加载 OTA 服务器地址
        const serverResult = await api.call('ota.server.get');
        if (!pageCurrent()) return;
        if (serverResult?.code === 0 && serverResult.data?.url) {
            document.getElementById('ota-server-input').value = serverResult.data.url;
        }
        
        // 2. 加载版本信息
        const versionResult = await api.call('ota.version');
        if (!pageCurrent()) return;
        if (versionResult?.code === 0 && versionResult.data) {
            const v = versionResult.data;
            document.getElementById('ota-current-version').textContent = v.version || t('common.unknown');
            document.getElementById('ota-version-meta').textContent = 
                `${v.project || 'TianshanOS'} · ${v.compile_date || ''} ${v.compile_time || ''} · IDF ${v.idf_version || ''}`;
            currentFirmwareVersion = v;
        }
        
        // 3. 加载分区信息
        const partResult = await api.call('ota.partitions');
        if (!pageCurrent()) return;
        if (partResult?.code === 0 && partResult.data) {
            displayPartitionsCompact(partResult.data);
        }
        
        // 4. 检查当前升级状态
        await refreshOtaProgress();
        if (!pageCurrent()) return;
        
    } catch (error) {
        if (!pageCurrent()) return;
        console.error('Failed to load OTA data:', error);
    }
}

function displayPartitionsCompact(data) {
    const container = document.getElementById('ota-partitions');
    let html = '';
    
    const addr = p => '0x' + p.address.toString(16).toUpperCase().padStart(8, '0');
    const tile = (p, stateHtml, version, action, desc) => `
        <div class="w-tile partition-card">
            <div class="between"><span class="mono" style="font-weight:600">${p.label}</span>${stateHtml}</div>
            <div class="t-body" style="font-weight:500">${version}</div>
            <div class="t-note num">${addr(p)} · ${formatSize(p.size)}</div>
            ${action}
            <div class="t-note" style="text-align:center">${desc}</div>
        </div>`;

    // 运行中的分区
    if (data.running) {
        const p = data.running;
        html += tile(p, `<span class="state ok">${t('ota.partitionRunning')}</span>`, p.version || '—',
            `<button class="btn" style="width:100%" onclick="validateOta()">${t('ota.markValid')}</button>`, t('ota.disableRollbackDesc'));
    }
    
    // 备用分区
    if (data.next) {
        const p = data.next;
        const hasVersion = p.is_bootable && p.version;
        const canRollback = data.can_rollback;  // 使用 API 返回的实际可回滚状态
        html += tile(p, `<span class="state${p.is_bootable ? ' warn' : ''}">${p.is_bootable ? t('ota.partitionBootable') : t('ota.partitionIdle')}</span>`,
            hasVersion ? p.version : (p.is_bootable ? t('otaPage.prevVersion') : t('otaPage.noFirmware')),
            canRollback ? `<button class="btn dg" style="width:100%" onclick="confirmRollback()">${t('ota.rollbackToThis')}</button>` : '',
            canRollback ? t('ota.loadAfterReboot') : (p.is_bootable ? t('otaPage.cannotRollback') : t('otaPage.partitionEmpty')));
    }
    
    container.innerHTML = html || '<p class="t-note">' + t('otaPage.noPartitionInfo') + '</p>';
}

async function refreshOtaInfo() {
    const pageCurrent = capturePageValidity();
    await loadOtaData();
    if (!pageCurrent()) return;
}

// OTA 两步升级状态
let otaStep = 'idle'; // 'idle' | 'app' | 'www'
let wwwOtaEnabled = true;  // 是否启用 WebUI 升级
let sdcardOtaSource = '';  // SD卡升级时的文件路径，用于推导 www.bin 路径
let sdcardWwwSource = '';  // SD卡升级时实际匹配到的 www.bin 路径

function inferSdcardWwwPath(source) {
    if (!source) return '';
    return source.match(/\.bin$/i)
        ? source.replace(/[^\/]+\.bin$/i, 'www.bin')
        : source.replace(/\/?$/, '/www.bin');
}

async function resolveSdcardOtaPaths(input, includeWww) {
    const source = input.trim();
    if (!source) {
        throw new Error(t('promptRepair.enterOtaPath'));
    }

    if (source.match(/\.bin$/i)) {
        const name = source.substring(source.lastIndexOf('/') + 1).toLowerCase();
        if (name === 'www.bin') {
            const dir = source.substring(0, source.lastIndexOf('/')) || '/sdcard';
            const resolved = await resolveSdcardOtaPaths(dir, includeWww);
            return {
                firmware: resolved.firmware,
                www: includeWww ? source : ''
            };
        }

        const firmwareInfo = await api.storageInfo(source);
        if (firmwareInfo.code !== 0 || firmwareInfo.data?.type !== 'file') {
            throw new Error(t('promptRepair.firmwareMissing', {path: source}));
        }

        const wwwPath = inferSdcardWwwPath(source);
        if (includeWww) {
            const wwwInfo = await api.storageInfo(wwwPath);
            if (wwwInfo.code !== 0 || wwwInfo.data?.type !== 'file') {
                throw new Error(t('promptRepair.wwwMissing', {path: wwwPath}));
            }
        }

        return { firmware: source, www: includeWww ? wwwPath : '' };
    }

    const dir = source.replace(/\/+$/, '') || '/sdcard';
    const list = await api.storageList(dir);
    if (list.code !== 0 || !Array.isArray(list.data?.entries)) {
        throw new Error(t('promptRepair.directoryUnreadable', {path: dir}));
    }

    const files = list.data.entries.filter(entry => entry.type === 'file');
    const firmware = files.find(entry => entry.name === 'TianShanOS.bin') ||
        files.find(entry => entry.name.toLowerCase() === 'tianshanos.bin') ||
        files.find(entry => entry.name.toLowerCase().endsWith('.bin') && entry.name.toLowerCase() !== 'www.bin');

    if (!firmware) {
        throw new Error(t('promptRepair.firmwareDirMissing', {path: dir}));
    }

    let wwwPath = '';
    if (includeWww) {
        const www = files.find(entry => entry.name.toLowerCase() === 'www.bin');
        if (!www) {
            throw new Error(t('promptRepair.wwwDirMissing', {path: dir}));
        }
        wwwPath = `${dir}/${www.name}`;
    }

    return { firmware: `${dir}/${firmware.name}`, www: wwwPath };
}

async function refreshOtaProgress() {
    const pageCurrent = capturePageValidity();
    try {
        // 根据当前步骤获取不同的进度
        let result;
        if (otaStep === 'www') {
            result = await api.call('ota.www.progress');
            if (!pageCurrent()) return;
        } else {
            result = await api.call('ota.progress');
            if (!pageCurrent()) return;
        }
        
        if (result.code === 0 && result.data) {
            const data = result.data;
            const state = data.state || 'idle';
            const percent = data.percent || 0;
            const received = data.received_size || data.received || 0;
            const total = data.total_size || data.total || 0;
            const message = data.message || '';
            
            // 更新状态文本
            const stateMap = {
                'idle': t('status.idle'),
                'checking': t('otaPage.stateChecking'),
                'downloading': otaStep === 'www' ? t('otaPage.downloadingWebUI') : t('otaPage.downloadingFirmware'),
                'verifying': t('otaPage.stateVerifying'),
                'writing': otaStep === 'www' ? t('otaPage.writingWebUI') : t('otaPage.writingFlash'),
                'pending_reboot': t('otaPage.statePendingReboot'),
                'completed': otaStep === 'www' ? t('otaPage.completedWebUI') : t('otaPage.completedFirmware'),
                'error': t('common.error')
            };
            
            const stateEl = document.getElementById('ota-state-text');
            const progressSection = document.getElementById('ota-progress-section');
            const abortBtn = document.getElementById('ota-abort-btn');
            
            if (!stateEl || !progressSection) return;
            
            // 显示当前步骤
            const stepText = otaStep === 'www' ? '[2/2] WebUI ' : (wwwOtaEnabled ? t('promptRepair.firmwareStep') : '');
            stateEl.textContent = stepText + (stateMap[state] || state);
            
            if (state !== 'idle') {
                progressSection.style.display = 'block'; document.getElementById('ota-manual')?.setAttribute('open', '');
                
                // 更新进度条
                document.getElementById('ota-progress-bar').style.width = percent + '%';
                document.getElementById('ota-progress-percent').textContent = percent + '%';
                document.getElementById('ota-progress-size').textContent = 
                    `${formatSize(received)} / ${formatSize(total)}`;
                
                // 更新消息
                document.getElementById('ota-message').textContent = message;
                
                // 显示中止按钮（除非已完成或出错）
                if (state !== 'pending_reboot' && state !== 'completed' && state !== 'error') {
                    abortBtn.style.display = '';
                } else {
                    abortBtn.style.display = 'none';
                }
                
                // 处理 App OTA 完成 - 开始 WWW OTA
                if (otaStep === 'app' && (state === 'pending_reboot' || state === 'completed') && wwwOtaEnabled) {
                    stateEl.textContent = typeof t === 'function' ? t('ui.firmwareUpgradeComplete') : '固件升级完成，准备升级 WebUI...';
                    await startWwwOta();
                    if (!pageCurrent()) return;
                    return;
                }
                
                // 处理 WWW OTA 完成或 App OTA 完成（无 www 升级）
                if ((otaStep === 'www' && (state === 'pending_reboot' || state === 'completed')) ||
                    (otaStep === 'app' && (state === 'pending_reboot' || state === 'completed') && !wwwOtaEnabled)) {
                    clearInterval(refreshInterval);
                    refreshInterval = null;
                    otaStep = 'idle';
                    
                    // 显示重启倒计时
                    stateEl.textContent = typeof t === 'function' ? t('ui.allUpgradeComplete') : '全部升级完成';
                    document.getElementById('ota-message').innerHTML = `
                        <div style="text-align:center">
                            <p>${t('otaPage.upgradeCompleteRebooting')}</p>
                            <p id="reboot-countdown" style="color:#9ca3af;margin-top:5px">${t('otaPage.triggeringReboot')}</p>
                        </div>
                    `;
                    
                    // 触发设备重启
                    try {
                        requireApiSuccess(await api.call('system.reboot', { delay: 1 }), 'call');
                        if (!pageCurrent()) return;
                    } catch (e) {
                        if (!pageCurrent()) return;
                        showToast(t('promptRepair.rebootUnknown'), 'warning', 10000);
                    }
                    
                    // 开始检测设备重启
                    startRebootDetection();
                } else if (state === 'error') {
                    showToast(typeof t === 'function' ? t('toast.upgradeFailedMsg', { msg: message }) : '升级失败: ' + message, 'error');
                    clearInterval(refreshInterval);
                    refreshInterval = null;
                    otaStep = 'idle';
                }
            } else {
                // 如果 app OTA 是 idle 但我们在 www 步骤，检查 www 进度
                if (otaStep !== 'www') {
                    progressSection.style.display = 'none';
                }
            }
        }
    } catch (error) {
        if (!pageCurrent()) return;
        console.error('Failed to get OTA status:', error);
    }
}

// 启动 WWW OTA（第二步）
async function startWwwOta() {
    try {
        let wwwSource = '';
        let isFromSdcard = false;
        
        // 判断来源：SD卡 或 HTTP URL
        if (sdcardOtaSource) {
            // SD卡升级：推导 www.bin 路径
            isFromSdcard = true;
            wwwSource = sdcardWwwSource || inferSdcardWwwPath(sdcardOtaSource);
        } else {
            // HTTP 升级：从服务器 URL 推导
            const serverUrl = document.getElementById('ota-server-input').value.trim() ||
                              document.getElementById('ota-url-input').value.trim();
            
            if (serverUrl) {
                // 尝试多种方式推导 www.bin URL
                if (serverUrl.includes('firmware.bin') || serverUrl.includes('TianshanOS.bin')) {
                    wwwSource = serverUrl.replace(/firmware\.bin|TianshanOS\.bin/gi, 'www.bin');
                } else if (serverUrl.match(/\.bin$/i)) {
                    wwwSource = serverUrl.replace(/[^\/]+\.bin$/i, 'www.bin');
                } else if (serverUrl.endsWith('/')) {
                    wwwSource = serverUrl + 'www.bin';
                } else {
                    wwwSource = serverUrl + '/www.bin';
                }
            }
        }
        
        if (!wwwSource) {
            console.log('No www source configured, skipping WebUI upgrade');
            wwwOtaEnabled = false;
            sdcardOtaSource = '';  // 重置
            sdcardWwwSource = '';
            return;
        }
        
        otaStep = 'www';
        
        document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('ui.step2Webui') : '[2/2] 开始升级 WebUI...';
        document.getElementById('ota-progress-bar').style.width = '0%';
        document.getElementById('ota-progress-percent').textContent = '0%';
        document.getElementById('ota-message').textContent = wwwSource;
        
        let result;
        if (isFromSdcard) {
            // SD卡方式
            result = await api.call('ota.www.start_sdcard', {
                file: wwwSource
            });
        } else {
            // HTTP 方式
            const skipVerify = document.getElementById('ota-url-skip-verify')?.checked || false;
            result = await api.call('ota.www.start', {
                url: wwwSource,
                skip_verify: skipVerify
            });
        }
        
        if (result.code !== 0) {
            showToast(typeof t === 'function' ? t('toast.webuiUpgradeStartFailed') + ': ' + result.message : 'WebUI 升级启动失败: ' + result.message, 'error');
            otaStep = 'idle';
            clearInterval(refreshInterval);
            refreshInterval = null;
            
            document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.stateError') : '错误';
            document.getElementById('ota-message').innerHTML = `
                <div style="text-align:center">
                    <p>${t('promptRepair.wwwPartial')}</p>
                    <p style="color:#9ca3af;margin-top:5px">${escapeHtml(result.message || t('promptRepair.wwwSibling'))}</p>
                    <button class="btn btn-service-style btn-small" onclick="startWwwOta()" style="margin-top:10px">${t('promptRepair.wwwRetry')}</button>
                </div>
            `;
            document.getElementById('ota-abort-btn').style.display = 'none';
            return;
        }

        sdcardOtaSource = '';  // 重置
        sdcardWwwSource = '';
        if (!refreshInterval) {
            refreshInterval = setInterval(refreshOtaProgress, 1000);
        }
        await refreshOtaProgress();
    } catch (error) {
        console.error('Failed to start WWW OTA:', error);
        otaStep = 'idle';
        document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.stateError') : '错误';
        document.getElementById('ota-message').textContent = error.message || t('toast.webuiUpgradeStartFailed');
        document.getElementById('ota-abort-btn').style.display = 'none';
    }
}

// 检测设备重启完成
let rebootCheckInterval = null;
let rebootStartTime = null;

function startRebootDetection() {
    rebootStartTime = Date.now();
    let checkCount = 0;
    
    // 每 2 秒检测一次设备是否恢复
    rebootCheckInterval = setInterval(async () => {
        checkCount++;
        const elapsed = Math.floor((Date.now() - rebootStartTime) / 1000);
        const countdownEl = document.getElementById('reboot-countdown');
        
        if (countdownEl) {
            countdownEl.textContent = typeof t === 'function' ? t('ui.waitedSeconds', { elapsed }) : `已等待 ${elapsed} 秒...`;
        }
        
        try {
            // 尝试连接设备
            const result = await api.call('ota.version');
            if (result.code === 0) {
                // 设备恢复了！
                clearInterval(rebootCheckInterval);
                rebootCheckInterval = null;
                
                const newVersion = result.data?.version || t('common.unknown');
                
                if (countdownEl) {
                    countdownEl.innerHTML = `
                        <span style="color:#059669">${t('otaPage.deviceRecovered')}</span>
                        <br><span style="font-size:0.9em">${t('ota.currentVersion')}:  ${newVersion}</span>
                    `;
                }
                
                showToast(typeof t === 'function' ? t('toast.otaUpgradeSuccess', { version: newVersion }) : `OTA 升级成功！当前版本: ${newVersion}`, 'success');
                
                // 3 秒后刷新页面
                setTimeout(() => {
                    window.location.reload();
                }, 3000);
            }
        } catch (e) {
            // 设备还在重启，继续等待
            if (checkCount > 60) {
                // 超过 2 分钟，提示用户手动检查
                clearInterval(rebootCheckInterval);
                rebootCheckInterval = null;
                
                if (countdownEl) {
                    countdownEl.innerHTML = `
                        <span style="color:#f43f5e">${typeof t === 'function' ? t('otaPage.waitTimeout') : '等待超时'}</span>
                        <br><span style="font-size:0.9em">${typeof t === 'function' ? t('otaPage.checkDeviceManually') : '请手动检查设备状态并刷新页面'}</span>
                        <br><button class="btn btn-service-style btn-small" onclick="window.location.reload()" 
                            style="margin-top:10px">${typeof t === 'function' ? t('otaPage.refreshPage') : '刷新页面'}</button>
                    `;
                }
            }
        }
    }, 2000);
}

async function otaFromUrl() {
    const url = document.getElementById('ota-url-input').value.trim();
    if (!url) {
        showToast(typeof t === 'function' ? t('toast.enterFirmwareUrl') : '请输入固件 URL', 'error');
        return;
    }
    
    // 允许 http 和 https
    if (!url.startsWith('http://') && !url.startsWith('https://')) {
        showToast(typeof t === 'function' ? t('toast.urlMustHttp') : 'URL 必须以 http:// 或 https:// 开头', 'error');
        return;
    }
    
    const skipVerify = document.getElementById('ota-url-skip-verify').checked;
    const includeWww = document.getElementById('ota-url-include-www').checked;
    
    const params = {
        url: url,
        no_reboot: true,  // 不自动重启，由前端控制流程
        skip_verify: skipVerify
    };
    
    // 设置 OTA 步骤
    otaStep = 'app';
    wwwOtaEnabled = includeWww;  // 根据用户选择决定是否升级 www
    
    // 立即显示进度区域，提供即时反馈
    const progressSection = document.getElementById('ota-progress-section');
    progressSection.style.display = 'block'; document.getElementById('ota-manual')?.setAttribute('open', '');
    document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.step1Connecting') : '[1/2] 正在连接服务器...';
    document.getElementById('ota-progress-bar').style.width = '0%';
    document.getElementById('ota-progress-percent').textContent = '0%';
    document.getElementById('ota-progress-size').textContent = typeof t === 'function' ? t('otaPage.preparing') : '准备中...';
    document.getElementById('ota-message').textContent = url;
    document.getElementById('ota-abort-btn').style.display = '';
    
    try {
        showToast(typeof t === 'function' ? t('toast.twoStepUpgrade') : '开始两步升级：固件 + WebUI', 'info');
        const result = await api.call('ota.upgrade_url', params);
        
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.firmwareUpgradeStarted') : '固件升级已启动', 'success');
            document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.downloading') : '下载中...';
            // 开始刷新进度
            if (!refreshInterval) {
                refreshInterval = setInterval(refreshOtaProgress, 1000);
            }
            // 立即刷新一次
            await refreshOtaProgress();
        } else {
            showToast(typeof t === 'function' ? t('toast.upgradeStartFailedMsg', { msg: result.message }) : '启动升级失败: ' + result.message, 'error');
            // 显示错误状态
            document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.stateError') : '错误';
            document.getElementById('ota-message').textContent = result.message || t('promptRepair.startFailed');
            document.getElementById('ota-abort-btn').style.display = 'none';
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.upgradeStartFailedMsg', { msg: error.message }) : '启动升级失败: ' + error.message, 'error');
        document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.stateError') : '错误';
        document.getElementById('ota-message').textContent = error.message || t('login.networkError');
        document.getElementById('ota-abort-btn').style.display = 'none';
    }
}

async function otaFromFile() {
    const inputPath = document.getElementById('ota-file-input').value.trim();
    if (!inputPath) {
        showToast(typeof t === 'function' ? t('toast.enterFilePath') : '请输入文件路径', 'error');
        return;
    }
    
    const includeWww = document.getElementById('ota-file-include-www').checked;

    let paths;
    try {
        paths = await resolveSdcardOtaPaths(inputPath, includeWww);
    } catch (error) {
        showToast(error.message || t('promptRepair.otaPathFailed'), 'error');
        return;
    }
    
    const params = {
        file: paths.firmware,
        no_reboot: true  // 不自动重启，由前端控制流程
    };
    
    // 设置 OTA 步骤
    otaStep = 'app';
    wwwOtaEnabled = includeWww;  // 根据用户选择决定是否升级 www
    sdcardOtaSource = paths.firmware;  // 保存 SD 卡路径用于推导 www.bin 路径
    sdcardWwwSource = paths.www;
    
    // 立即显示进度区域
    const progressSection = document.getElementById('ota-progress-section');
    progressSection.style.display = 'block'; document.getElementById('ota-manual')?.setAttribute('open', '');
    const stepText = includeWww ? '[1/2] ' : '';
    document.getElementById('ota-state-text').textContent = stepText + (typeof t === 'function' ? t('otaPage.readingFile') : '正在读取文件...');
    document.getElementById('ota-progress-bar').style.width = '0%';
    document.getElementById('ota-progress-percent').textContent = '0%';
    document.getElementById('ota-progress-size').textContent = typeof t === 'function' ? t('otaPage.preparing') : '准备中...';
    document.getElementById('ota-message').textContent = paths.firmware;
    document.getElementById('ota-abort-btn').style.display = '';
    
    try {
        showToast(typeof t === 'function' ? t('toast.startingFileUpgrade') : '开始从文件升级固件...', 'info');
        const result = await api.call('ota.upgrade_file', params);
        
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.firmwareUpgradeStarted') : '固件升级已启动', 'success');
            document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.writing') : '写入中...';
            // 开始刷新进度
            if (!refreshInterval) {
                refreshInterval = setInterval(refreshOtaProgress, 1000);
            }
            await refreshOtaProgress();
        } else {
            showToast(typeof t === 'function' ? t('toast.upgradeStartFailedMsg', { msg: result.message }) : '启动升级失败: ' + result.message, 'error');
            document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.stateError') : '错误';
            document.getElementById('ota-message').textContent = result.message || t('promptRepair.startFailed');
            document.getElementById('ota-abort-btn').style.display = 'none';
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.upgradeStartFailedMsg', { msg: error.message }) : '启动升级失败: ' + error.message, 'error');
        document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('otaPage.stateError') : '错误';
        document.getElementById('ota-message').textContent = error.message || t('login.networkError');
        document.getElementById('ota-abort-btn').style.display = 'none';
    }
}

async function validateOta() {
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmMarkFirmwareValid') : '确认将当前固件标记为有效？\n这将取消自动回滚保护。', { primary: t('ota.markValid'), tone: 'neutral' })) {
        return;
    }
    
    try {
        const result = await api.call('ota.validate');
        
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.firmwareValidated') : '固件已标记为有效', 'success');
            await refreshOtaInfo();
        } else {
            showToast(typeof t === 'function' ? t('toast.operationFailedMsg', { msg: result.message }) : '操作失败: ' + result.message, 'error');
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.operationFailedMsg', { msg: error.message }) : '操作失败: ' + error.message, 'error');
    }
}

async function confirmRollback() {
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmRollback') : '确认回滚到上一版本固件？\n\n系统将立即重启并加载上一个分区的固件。\n请确保上一版本固件可用！', { primary: t('ui.rollbackReboot'), tone: 'danger' })) {
        return;
    }
    
    rollbackOta();
}

async function rollbackOta() {
    try {
        showToast(typeof t === 'function' ? t('toast.rollingBack') : '正在回滚固件...', 'info');
        const result = await api.call('ota.rollback');
        
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.rollbackSuccess') : '回滚成功！系统将在 3 秒后重启...', 'success');
            // 3秒后页面会因为重启而断开连接
        } else {
            showToast(typeof t === 'function' ? t('toast.rollbackFailedMsg', { msg: result.message }) : '回滚失败: ' + result.message, 'error');
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.rollbackFailedMsg', { msg: error.message }) : '回滚失败: ' + error.message, 'error');
    }
}

async function abortOta() {
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmAbortUpgrade') : '确认中止当前升级？', { primary: t('ui.abortUpgrade'), tone: 'neutral' })) {
        return;
    }
    
    try {
        // 根据当前步骤中止相应的 OTA
        let result;
        if (otaStep === 'www') {
            result = await api.call('ota.www.abort');
        } else {
            result = await api.call('ota.abort');
        }
        
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.upgradeAborted') : '升级已中止', 'info');
            otaStep = 'idle';
            await refreshOtaInfo();
            clearInterval(refreshInterval);
            refreshInterval = null;
        } else {
            showToast(typeof t === 'function' ? t('toast.abortFailedMsg', { msg: result.message }) : '中止失败: ' + result.message, 'error');
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.abortFailedMsg', { msg: error.message }) : '中止失败: ' + error.message, 'error');
    }
}

function formatSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return (bytes / Math.pow(k, i)).toFixed(2) + ' ' + sizes[i];
}

// ============================================================================
// 语义化版本工具函数
// ============================================================================

/**
 * 解析语义化版本号
 * @param {string} version - 版本字符串 (如 "1.2.3-rc1+build123")
 * @returns {object} - { major, minor, patch, prerelease, build }
 */
function parseVersion(version) {
    const result = { major: 0, minor: 0, patch: 0, prerelease: '', build: '' };
    if (!version) return result;
    
    // 移除前缀 v/V
    let v = version.trim();
    if (v.startsWith('v') || v.startsWith('V')) {
        v = v.substring(1);
    }
    
    // 分离构建元数据 (+xxx)
    const buildIdx = v.indexOf('+');
    if (buildIdx !== -1) {
        result.build = v.substring(buildIdx + 1);
        v = v.substring(0, buildIdx);
    }
    
    // 分离预发布标识 (-xxx)
    const preIdx = v.indexOf('-');
    if (preIdx !== -1) {
        result.prerelease = v.substring(preIdx + 1);
        v = v.substring(0, preIdx);
    }
    
    // 解析核心版本号
    const parts = v.split('.');
    result.major = parseInt(parts[0]) || 0;
    result.minor = parseInt(parts[1]) || 0;
    result.patch = parseInt(parts[2]) || 0;
    
    return result;
}

/**
 * 比较两个语义化版本
 * @param {string} v1 - 第一个版本
 * @param {string} v2 - 第二个版本
 * @returns {number} - -1 (v1 < v2), 0 (v1 == v2), 1 (v1 > v2)
 */
function compareSemVer(v1, v2) {
    const a = parseVersion(v1);
    const b = parseVersion(v2);
    
    // 比较主版本号
    if (a.major !== b.major) return a.major > b.major ? 1 : -1;
    
    // 比较次版本号
    if (a.minor !== b.minor) return a.minor > b.minor ? 1 : -1;
    
    // 比较修订号
    if (a.patch !== b.patch) return a.patch > b.patch ? 1 : -1;
    
    // 比较预发布标识
    // 有预发布 < 无预发布 (1.0.0-rc1 < 1.0.0)
    if (a.prerelease && !b.prerelease) return -1;
    if (!a.prerelease && b.prerelease) return 1;
    if (a.prerelease && b.prerelease) {
        return a.prerelease.localeCompare(b.prerelease);
    }
    
    return 0;
}

/**
 * 格式化版本显示
 * @param {object} versionInfo - 版本信息对象
 * @returns {string} - 格式化的版本字符串
 */
function formatVersionDisplay(versionInfo) {
    if (!versionInfo) return 'Unknown';
    const v = versionInfo.version || '0.0.0';
    const date = versionInfo.compile_date || '';
    const time = versionInfo.compile_time || '';
    return `${v} (${date} ${time})`.trim();
}

// OTA 服务器相关函数
async function saveOtaServer() {
    const serverUrl = document.getElementById('ota-server-input').value.trim();
    
    try {
        const result = await api.call('ota.server.set', {
            url: serverUrl,
            save: true  // 保存到 NVS
        });
        
        if (result.code === 0) {
            if (serverUrl) {
                showToast(typeof t === 'function' ? t('toast.otaServerSaved') : 'OTA 服务器地址已保存', 'success');
            } else {
                showToast(typeof t === 'function' ? t('toast.otaServerCleared') : 'OTA 服务器地址已清除', 'info');
            }
        } else {
            showToast(typeof t === 'function' ? t('toast.saveFailedMsg', { msg: result.message }) : '保存失败: ' + result.message, 'error');
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.saveFailedMsg', { msg: error.message }) : '保存失败: ' + error.message, 'error');
    }
}

// 当前固件版本缓存
let currentFirmwareVersion = null;

async function checkForUpdates() {
    const serverUrl = document.getElementById('ota-server-input').value.trim();
    if (!serverUrl) {
        showToast(typeof t === 'function' ? t('toast.enterOtaServer') : '请先输入 OTA 服务器地址', 'error');
        return;
    }
    
    const statusDiv = document.getElementById('ota-update-status');
    statusDiv.style.display = 'block';
    statusDiv.className = 'ota-update-status';
    statusDiv.innerHTML = '<span class="t-label">' + t('otaPage.checking') + '</span>';
    
    try {
        // 尝试通过设备测试连接（如果 API 存在）
        try {
            console.log('Testing device connectivity to:', serverUrl);
            const testResult = await api.call('ota.test_connection', { url: serverUrl }, 'POST');
            if (testResult && testResult.data) {
                const testData = testResult.data;
                console.log('Device connection test result:', testData);
                
                if (!testData.dns_ok) {
                    throw new Error(t('promptRepair.dnsFailed', {host: testData.host}));
                }
                if (!testData.tcp_ok) {
                    throw new Error(t('promptRepair.tcpFailed', {host: testData.resolved_ip, port: testData.port}));
                }
                if (!testData.http_ok) {
                    throw new Error(t('promptRepair.httpFailed', {message: testData.http_error || t('common.noResponse')}));
                }
                console.log(`Device connectivity OK: DNS=${testData.dns_time_ms}ms, TCP=${testData.tcp_time_ms}ms, HTTP=${testData.http_time_ms}ms`);
            }
        } catch (testError) {
            // 连接测试 API 可能不存在（旧固件），跳过测试继续检查版本
            console.warn('Device connection test skipped:', testError.message);
        }
        
        // 获取服务器版本信息（从浏览器获取）
        const versionUrl = serverUrl.replace(/\/$/, '') + '/version';
        console.log('Checking for updates:', versionUrl);
        
        const response = await fetch(versionUrl);
        if (!response.ok) {
            throw new Error(t('promptRepair.httpStatus', {status: response.status}));
        }
        
        const serverInfo = await response.json();
        console.log('Server version info:', serverInfo);
        
        // 获取当前版本
        if (!currentFirmwareVersion) {
            const localResult = await api.call('ota.version');
            if (localResult && localResult.code === 0 && localResult.data) {
                currentFirmwareVersion = localResult.data;
            }
        }
        
        // 比较版本
        const localVersion = currentFirmwareVersion?.version || '0.0.0';
        const serverVersion = serverInfo.version || '0.0.0';
        const serverCompileDate = serverInfo.compile_date || '';
        const serverCompileTime = serverInfo.compile_time || '';
        const serverSize = serverInfo.size || 0;
        
        // 语义化版本比较
        const versionComparison = compareSemVer(serverVersion, localVersion);
        const hasUpdate = versionComparison > 0 || 
                         (versionComparison === 0 && (
                             serverCompileDate !== currentFirmwareVersion?.compile_date ||
                             serverCompileTime !== currentFirmwareVersion?.compile_time
                         ));
        
        // 版本变更类型说明
        let updateType = '';
        if (versionComparison > 0) {
            const localParts = parseVersion(localVersion);
            const serverParts = parseVersion(serverVersion);
            if (serverParts.major > localParts.major) {
                updateType = '<span class="state warn">' + t('otaPage.majorUpdate') + '</span>';
            } else if (serverParts.minor > localParts.minor) {
                updateType = '<span class="state warn">' + t('otaPage.featureUpdate') + '</span>';
            } else {
                updateType = '<span class="state ok">' + t('otaPage.patchUpdate') + '</span>';
            }
        }
        
        if (hasUpdate) {
            statusDiv.className = 'ota-update-status has-update';
            statusDiv.innerHTML = `
                <div class="between wrap">
                    <div>
                        <div class="acts"><span class="t-value">${t('otaPage.newVersionFound')}</span>${updateType}</div>
                        <div class="t-note num" style="margin-top:2px"><span class="mono">${localVersion}</span> → <span class="mono">${serverVersion}</span> · ${formatSize(serverSize)}</div>
                    </div>
                    <button class="btn sm primary" onclick="upgradeFromServer()">${t('otaPage.upgradeNow')}</button>
                </div>
            `;
        } else if (versionComparison < 0) {
            statusDiv.className = 'ota-update-status downgrade';
            statusDiv.innerHTML = `
                <div class="between wrap">
                    <div>
                        <div class="t-value">${t('otaPage.serverVersionOlder')}</div>
                        <div class="t-note num" style="margin-top:2px"><span class="mono">${localVersion}</span> → <span class="mono">${serverVersion}</span></div>
                    </div>
                    <button class="btn sm dg" onclick="upgradeFromServer()">${t('otaPage.downgrade')}</button>
                </div>
            `;
        } else {
            statusDiv.className = 'ota-update-status no-update';
            statusDiv.innerHTML = `
                <div class="acts"><span class="state ok">${t('otaPage.alreadyLatest')}</span><span class="t-note mono">${localVersion}</span></div>
            `;
        }
        
    } catch (error) {
        console.error('Check for updates failed:', error);
        statusDiv.className = 'ota-update-status error';
        statusDiv.innerHTML = `
            <div>
                <div class="state bad">${t('otaPage.checkUpdateFailed')}</div>
                <div class="t-note" style="margin-top:2px">${escapeHtml(error.message)}</div>
            </div>
        `;
    }
}

async function upgradeFromServer() {
    const serverUrl = document.getElementById('ota-server-input').value.trim();
    if (!serverUrl) {
        showToast(typeof t === 'function' ? t('toast.otaServerNotSet') : 'OTA 服务器地址未设置', 'error');
        return;
    }
    
    // 立即显示进度区域，给用户即时反馈
    const progressSection = document.getElementById('ota-progress-section');
    progressSection.style.display = 'block'; document.getElementById('ota-manual')?.setAttribute('open', '');
    document.getElementById('ota-state-text').textContent = typeof t === 'function' ? t('ui.preparingUpgrade') : 'Preparing upgrade...';
    document.getElementById('ota-progress-bar').style.width = '0%';
    document.getElementById('ota-progress-percent').textContent = '';
    document.getElementById('ota-progress-size').textContent = typeof t === 'function' ? t('ui.initializing') : '正在初始化...';
    document.getElementById('ota-message').textContent = serverUrl;
    document.getElementById('ota-abort-btn').style.display = 'none';
    
    // 隐藏更新状态区域
    const statusDiv = document.getElementById('ota-update-status');
    if (statusDiv) statusDiv.style.display = 'none';
    
    // 使用浏览器代理模式：浏览器下载固件后转发给 ESP32
    await upgradeViaProxy(serverUrl);
}

/**
 * 浏览器代理升级模式
 * 浏览器从 OTA 服务器下载固件，然后转发给 ESP32
 * 优势：ESP32 无需上网，只需要浏览器能访问 OTA 服务器
 */
async function upgradeViaProxy(serverUrl) {
    const includeWww = document.getElementById('ota-url-include-www')?.checked ?? true;
    
    // 获取进度区域元素（已在 upgradeFromServer 中显示）
    const progressSection = document.getElementById('ota-progress-section');
    const stateEl = document.getElementById('ota-state-text');
    const progressBar = document.getElementById('ota-progress-bar');
    const progressPercent = document.getElementById('ota-progress-percent');
    const progressSize = document.getElementById('ota-progress-size');
    const messageEl = document.getElementById('ota-message');
    const abortBtn = document.getElementById('ota-abort-btn');
    
    // 设置 OTA 步骤
    otaStep = 'app';
    wwwOtaEnabled = includeWww;
    
    // 计算总步骤数
    const totalSteps = includeWww ? 4 : 2;  // 下载固件、上传固件、[下载WebUI、上传WebUI]
    let currentStep = 0;
    
    const updateStep = (step, desc) => {
        currentStep = step;
        const prefix = `[${step}/${totalSteps}] `;
        stateEl.textContent = prefix + desc;
    };
    
    try {
        // ===== 第一步：浏览器下载固件 =====
        updateStep(1, t('promptRepair.firmwareDownloading'));
        const firmwareUrl = serverUrl.replace(/\/$/, '') + '/firmware';
        messageEl.textContent = typeof t === 'function' ? t('ui.downloadingFromServer') : '从 OTA 服务器下载';
        progressBar.style.width = '0%';
        progressPercent.textContent = '0%';
        progressSize.textContent = typeof t === 'function' ? t('ui.connectingServer') : '正在连接服务器...';
        abortBtn.style.display = 'none';  // 浏览器下载阶段暂不支持中止
        
        console.log('Proxy OTA: Downloading firmware from', firmwareUrl);
        
        // 使用 fetch 下载固件（带进度）
        const firmwareData = await downloadWithProgress(firmwareUrl, (loaded, total) => {
            const percent = total > 0 ? Math.round((loaded / total) * 100) : 0;
            progressBar.style.width = percent + '%';
            progressPercent.textContent = percent + '%';
            progressSize.textContent = `${formatSize(loaded)} / ${formatSize(total)}`;
        });
        
        console.log('Proxy OTA: Firmware downloaded,', firmwareData.byteLength, 'bytes');
        showToast(typeof t === 'function' ? t('ota.firmwareDownloadComplete', { size: formatSize(firmwareData.byteLength) }) : `固件下载完成 (${formatSize(firmwareData.byteLength)})`, 'success');
        
        // ===== 第二步：上传固件到 ESP32 =====
        updateStep(2, t('otaPage.uploadingFirmware'));
        messageEl.textContent = typeof t === 'function' ? t('ui.firmwareSize') + ': ' + formatSize(firmwareData.byteLength) : `固件大小: ${formatSize(firmwareData.byteLength)}`;
        progressBar.style.width = '0%';
        progressPercent.textContent = '';
        progressSize.textContent = typeof t === 'function' ? t('otaPage.writingFlash') : '正在写入 Flash（这可能需要1-2分钟）...';
        
        // 调用 ESP32 上传接口（复用现有的 /api/v1/ota/firmware）
        // 注意：不自动重启，等 www 也完成后再重启
        const uploadResult = await uploadFirmwareToDevice(firmwareData, false);
        
        if (!uploadResult.success) {
            throw new Error(uploadResult.error || t('otaPage.uploadFirmwareFailed'));
        }
        
        console.log('Proxy OTA: Firmware uploaded to device');
        showToast(typeof t === 'function' ? t('toast.firmwareWriteComplete') : '固件写入完成！', 'success');
        progressBar.style.width = '100%';
        progressPercent.textContent = '';
        
        // ===== 第三步：处理 WebUI（如果启用）=====
        if (includeWww) {
            updateStep(3, t('otaPage.downloadingWebUI'));
            const wwwUrl = serverUrl.replace(/\/$/, '') + '/www.bin';
            messageEl.textContent = typeof t === 'function' ? t('ui.downloadingFromServer') : '从 OTA 服务器下载';
            progressBar.style.width = '0%';
            progressPercent.textContent = '0%';
            progressSize.textContent = typeof t === 'function' ? t('otaPage.connecting') : '正在连接...';
            
            try {
                // 下载 www.bin
                const wwwData = await downloadWithProgress(wwwUrl, (loaded, total) => {
                    const percent = total > 0 ? Math.round((loaded / total) * 100) : 0;
                    progressBar.style.width = percent + '%';
                    progressPercent.textContent = percent + '%';
                    progressSize.textContent = `${formatSize(loaded)} / ${formatSize(total)}`;
                });
                
                console.log('Proxy OTA: WWW downloaded,', wwwData.byteLength, 'bytes');
                showToast(typeof t === 'function' ? t('ota.webuiDownloadComplete', { size: formatSize(wwwData.byteLength) }) : `WebUI 下载完成 (${formatSize(wwwData.byteLength)})`, 'success');
                
                // 上传 www.bin
                updateStep(4, t('otaPage.uploadingWebUI'));
                messageEl.textContent = typeof t === 'function' ? t('ui.webuiSize') + ': ' + formatSize(wwwData.byteLength) : `WebUI 大小: ${formatSize(wwwData.byteLength)}`;
                progressBar.style.width = '0%';
                progressPercent.textContent = '';
                progressSize.textContent = typeof t === 'function' ? t('ui.writingSpiffs') : '正在写入 SPIFFS...';
                
                const wwwResult = await uploadWwwToDevice(wwwData);
                
                if (!wwwResult.success) {
                    throw new Error(wwwResult.error || t('promptRepair.webUploadFailed'));
                }

                console.log('Proxy OTA: WWW uploaded to device');
                showToast(typeof t === 'function' ? t('toast.webuiWriteComplete') : 'WebUI 写入完成！', 'success');
                progressBar.style.width = '100%';
                progressPercent.textContent = '';
            } catch (wwwError) {
                console.warn('WWW download/upload failed:', wwwError);
                throw new Error(t('promptRepair.webUpgradeFailed') + (wwwError.message || wwwError));
            }
        }
        
        // ===== 最终步骤：升级完成，触发重启 =====
        stateEl.textContent = typeof t === 'function' ? t('ui.allUpgradeComplete') : '全部升级完成';
        progressBar.style.width = '100%';
        progressBar.style.background = 'linear-gradient(90deg, #059669, #10b981)';
        progressPercent.textContent = '';
        messageEl.innerHTML = `
            <div style="text-align:center">
                <p>${t('promptRepair.upgradeWritten')}</p>
                <p id="reboot-countdown" style="color:#9ca3af;margin-top:5px">${t('otaPage.triggeringReboot')}</p>
            </div>
        `;
        
        otaStep = 'idle';
        
        // 触发设备重启
        try {
            requireApiSuccess(await api.call('system.reboot', { delay: 1 }), 'call');
        } catch (e) {
            showToast(t('promptRepair.rebootUnknown'), 'warning', 10000);
        }
        
        // 检测设备重启
        startRebootDetection();
        
    } catch (error) {
        console.error('Proxy OTA failed:', error);
        stateEl.textContent = typeof t === 'function' ? t('ui.upgradeFailed') : '升级失败';
        messageEl.textContent = error.message;
        progressBar.style.width = '0%';
        progressPercent.textContent = '';
        otaStep = 'idle';
        showToast(typeof t === 'function' ? t('toast.upgradeFailedMsg', { msg: error.message }) : '升级失败: ' + error.message, 'error');
    }
}

/**
 * 带进度的文件下载
 */
async function downloadWithProgress(url, onProgress) {
    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(t('promptRepair.downloadHttpStatus', {status: response.status}));
    }
    
    const contentLength = response.headers.get('content-length');
    const total = contentLength ? parseInt(contentLength, 10) : 0;
    
    const reader = response.body.getReader();
    const chunks = [];
    let loaded = 0;
    
    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        
        chunks.push(value);
        loaded += value.length;
        
        if (onProgress) {
            onProgress(loaded, total);
        }
    }
    
    // 合并所有块
    const result = new Uint8Array(loaded);
    let position = 0;
    for (const chunk of chunks) {
        result.set(chunk, position);
        position += chunk.length;
    }
    
    return result.buffer;
}

/**
 * 上传固件到设备
 */
async function uploadFirmwareToDevice(firmwareData, autoReboot = false) {
    try {
        const url = getApiUrl(`/ota/firmware?auto_reboot=${autoReboot}`);
        
        const response = await fetch(url, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/octet-stream'
            },
            body: firmwareData
        });
        
        const result = await response.json();
        
        if (response.ok && result.status === 'success') {
            return { success: true, data: result };
        } else {
            return { success: false, error: result.message || result.error || t('files.uploadFailed') };
        }
    } catch (error) {
        return { success: false, error: error.message };
    }
}

/**
 * 上传 WebUI 到设备
 */
async function uploadWwwToDevice(wwwData) {
    try {
        const url = getApiUrl('/ota/www');
        
        const response = await fetch(url, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/octet-stream'
            },
            body: wwwData
        });
        
        const result = await response.json();
        
        if (response.ok && result.status === 'success') {
            return { success: true, data: result };
        } else {
            return { success: false, error: result.message || result.error || t('files.uploadFailed') };
        }
    } catch (error) {
        return { success: false, error: error.message };
    }
}

// 导出全局函数
window.loadOtaPage = loadOtaPage;
window.otaFromUrl = otaFromUrl;
window.otaFromFile = otaFromFile;
window.startWwwOta = startWwwOta;
window.validateOta = validateOta;
window.confirmRollback = confirmRollback;
window.rollbackOta = rollbackOta;
window.abortOta = abortOta;
window.saveOtaServer = saveOtaServer;
window.checkForUpdates = checkForUpdates;
window.upgradeFromServer = upgradeFromServer;
window.upgradeViaProxy = upgradeViaProxy;

// =========================================================================
//                         日志页面
// =========================================================================

let logRefreshInterval = null;
let logAutoScroll = true;
let logLastTimestamp = 0;
let logWsConnected = false;
// =========================================================================
// 日志页面已废弃 - 功能已整合到终端页面的日志模态框

// =========================================================================
//                         内存详情模态框
// =========================================================================

// 任务排序状态
let taskSortState = { key: 'stack_hwm', ascending: true };  // 默认按剩余栈升序（最危险的在前）
let cachedTasksData = [];  // 缓存任务数据用于排序

/**
 * 渲染任务行 HTML
 */
function servicePhaseLabel(phase) {
    return ["PLATFORM", "CORE", "HAL", "DRIVER", "NETWORK", "SECURITY", "SERVICE", "UI"].includes(phase) ? t('promptRepair.phase_' + phase) : t('common.unknown');
}

function userStateLabel(state) {
    const known = ["UNREGISTERED", "REGISTERED", "STARTING", "RUNNING", "STOPPING", "STOPPED", "ERROR", "Running", "Ready", "Blocked", "Suspended", "Deleted"];
    return known.includes(state) ? t('promptRepair.state_' + state) : t('common.unknown');
}

function renderTaskRows(tasks, formatBytes) {
    return tasks.map(task => {
        const hwm = task.stack_hwm || 0;
        const alloc = task.stack_alloc || 0;
        const used = task.stack_used || 0;
        const usagePct = task.stack_usage_pct || 0;
        const hwmState = hwm < 256 ? 'bad' : hwm < 512 ? 'warn' : '';
        const usageState = usagePct >= 90 ? 'bad' : usagePct >= 75 ? 'warn' : '';
        const stateCls = { 'Running': 'ok', 'Blocked': 'warn', 'Deleted': 'bad' }[task.state] || '';
        return `
        <tr>
            <td class="mono">${task.name}</td>
            <td>${alloc ? formatBytes(alloc) : '-'}</td>
            <td>${used ? formatBytes(used) : '-'}</td>
            <td><span class="state ${hwmState}" style="font-size:inherit;font-weight:500">${formatBytes(hwm)}</span></td>
            <td><span class="state ${usageState}" style="font-size:inherit">${usagePct}%</span></td>
            <td>${task.priority}</td>
            <td><span class="state ${stateCls}" style="font-size:inherit">${userStateLabel(task.state)}</span></td>
            ${task.cpu_percent !== undefined ? `<td>${task.cpu_percent}%</td>` : ''}
        </tr>
        `;
    }).join('');
}

/**
 * 对任务列表排序
 */
function sortTasks(tasks, key, ascending) {
    const stateOrder = { 'Running': 0, 'Ready': 1, 'Blocked': 2, 'Suspended': 3, 'Deleted': 4 };
    
    return [...tasks].sort((a, b) => {
        let valA, valB;
        
        if (key === 'state') {
            valA = stateOrder[a.state] ?? 5;
            valB = stateOrder[b.state] ?? 5;
        } else {
            valA = a[key] || 0;
            valB = b[key] || 0;
        }
        
        if (ascending) {
            return valA - valB;
        } else {
            return valB - valA;
        }
    });
}

/**
 * 初始化任务表格排序
 */
function initTaskTableSort() {
    const table = document.getElementById('task-memory-table');
    if (!table) return;
    
    const headers = table.querySelectorAll('th.sortable');
    headers.forEach(th => {
        th.style.cursor = 'pointer';
        th.addEventListener('click', () => {
            const key = th.dataset.sort;
            
            // 切换排序方向
            if (taskSortState.key === key) {
                taskSortState.ascending = !taskSortState.ascending;
            } else {
                taskSortState.key = key;
                taskSortState.ascending = true;
            }
            
            // 更新表头指示器
            headers.forEach(h => {
                const baseText = h.textContent.replace(/ [↑↓⇅]$/, '');
                if (h.dataset.sort === key) {
                    h.textContent = baseText + (taskSortState.ascending ? ' ↑' : ' ↓');
                } else {
                    h.textContent = baseText + ' ⇅';
                }
            });
            
            // 重新排序并渲染
            const sortedTasks = sortTasks(cachedTasksData, key, taskSortState.ascending);
            const tbody = document.getElementById('task-table-body');
            if (tbody) {
                // 复用已定义的 formatBytes
                const formatBytes = (bytes) => {
                    if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(2) + ' MB';
                    if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
                    return bytes + ' B';
                };
                tbody.innerHTML = renderTaskRows(sortedTasks, formatBytes);
            }
        });
    });
}

/**
 * 显示内存详情模态框
 */
async function showMemoryDetailModal() {
    const modal = document.getElementById('memory-detail-modal');
    modal.innerHTML = sheet(720, t('system.memoryDetailTitle'), `<div id="memory-detail-body"></div>`,
        `<span id="memory-detail-timestamp" class="t-note num" style="margin-right:auto">-</span><button class="btn lg" onclick="hideMemoryDetailModal()">${t('common.close')}</button><button class="btn lg primary" onclick="refreshMemoryDetail()"><svg class="i"><use href="#ri-refresh-line"/></svg>${t('common.refresh')}</button>`,
        'hideMemoryDetailModal()', 'mem hl');
    modal.classList.remove('hidden');
    await refreshMemoryDetail();
}

/**
 * 隐藏内存详情模态框
 */
function hideMemoryDetailModal() {
    const modal = document.getElementById('memory-detail-modal');
    modal.classList.add('hidden');
}

/**
 * 刷新内存详情数据
 */
async function refreshMemoryDetail() {
    const body = document.getElementById('memory-detail-body');
    const timestamp = document.getElementById('memory-detail-timestamp');
    
    body.innerHTML = '<div class="t-note" style="text-align:center;padding:20px">' + t('common.loading') + '</div>';
    
    try {
        const result = await api.getMemoryDetail();
        if (result.code !== 0 || !result.data) {
            throw new Error(result.message || t('memoryPage.getDataFailed'));
        }
        
        const data = result.data;
        const dram = data.dram || {};
        const psram = data.psram || {};
        const dma = data.dma || {};
        const tips = data.tips || [];
        const staticMem = data.static || {};
        const iram = data.iram || {};
        const rtc = data.rtc || {};
        const nvs = data.nvs || {};
        const caps = data.caps || {};
        
        // 格式化字节数
        const formatBytes = (bytes) => {
            if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(2) + ' MB';
            if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
            return bytes + ' B';
        };
        // 占用率：≥85 警告（琥珀），≥95 故障（红），其余中性
        // 概览里的数字：KB 取整、MB 一位小数
        const sumBytes = bytes => bytes >= 1024 * 1024 ? (bytes / 1024 / 1024).toFixed(1) + ' MB' : bytes >= 1024 ? Math.round(bytes / 1024) + ' KB' : bytes + ' B';
        const usedState = pct => pct >= 95 ? 'bad' : pct >= 85 ? 'warn' : '';
        const usedColor = pct => pct >= 95 ? 'var(--bad-dot)' : pct >= 85 ? 'var(--warn-dot)' : 'var(--accent)';
        const gauge = (name, m) => {
            const pct = m.used_percent || 0;
            const total = sumBytes(m.total || 0);
            let used = sumBytes(m.used || 0);
            if (used.split(' ')[1] === total.split(' ')[1]) used = used.split(' ')[0];
            return `<div>
                <div class="between" style="align-items:baseline"><span class="t-label">${name}</span><span class="state ${usedState(pct)}">${pct}% ${t('memoryPage.used')}</span></div>
                <div style="display:flex;align-items:baseline;gap:6px;margin:6px 0 10px"><span class="t-big">${used}</span><span class="t-unit">/ ${total}</span></div>
                <div class="bar"><i style="width:${Math.min(100, pct)}%;background:${usedColor(pct)}"></i></div>
            </div>`;
        };
        // 表格：表头 + 行（第一列加粗）
        const grid = (cols, head, rows) => `<div class="mgrid t-note" style="grid-template-columns:${cols}">${head.map(h => `<span>${h}</span>`).join('')}</div>` +
            rows.map(r => `<div class="mgrid t-body num" style="grid-template-columns:${cols}">${r.map((c, i) => `<span${i === 0 ? ' style="font-weight:500"' : ''}>${c}</span>`).join('')}</div>`).join('');
        const sect = (title, inner, note = '') => `<div style="margin-top:24px"><div class="t-section" style="margin-bottom:8px">${title}${note}</div>${inner}</div>`;
        const kvs = items => `<div class="kv-grid k2">${items.map(([k, v, d]) => kvRow(k + (d ? ` <span class="t-note">${d}</span>` : ''), v)).join('')}</div>`;
        
        let tipsHtml = '';
        if (tips.length > 0) {
            const tipMap = {
                'dram_fragmented': t('memoryPage.dramFragmented'),
                'psram_sufficient': t('memoryPage.psramSufficient'),
                'dram_low': t('memoryPage.dramLow'),
                'psram_low': t('memoryPage.psramLow')
            };
            tipsHtml = sect(t('memoryPage.optimizationTips'), tips.map(tip => {
                const [level, msg] = tip.split(':');
                return `<div class="tr" style="--cols:1fr;min-height:36px;padding:0"><span class="state ${level === 'critical' ? 'bad' : level === 'warning' ? 'warn' : ''}">${tipMap[msg] || msg}</span></div>`;
            }).join(''));
        }
        
        const heapRows = [['DRAM', formatBytes(dram.largest_block || 0), `${(dram.fragmentation || 0).toFixed(1)}%`, dram.alloc_blocks || '-', dram.free_blocks || '-', formatBytes(dram.min_free_ever || 0)]];
        if (psram.total) heapRows.push(['PSRAM', formatBytes(psram.largest_block || 0), `${(psram.fragmentation || 0).toFixed(1)}%`, psram.alloc_blocks || '-', psram.free_blocks || '-', formatBytes(psram.min_free_ever || 0)]);
        if (dma.total) heapRows.push(['DMA', formatBytes(dma.largest_block || 0), '-', '-', '-', '-']);

        const capRows = [
            [t('memoryPage.cap8bit'), formatBytes(caps.d8_free || 0), formatBytes(caps.d8_total || 0), t('memoryPage.cap8bitDesc')],
            [t('memoryPage.cap32bit'), formatBytes(caps.d32_free || 0), formatBytes(caps.d32_total || 0), t('memoryPage.cap32bitDesc')],
            [t('memoryPage.capDefault'), formatBytes(caps.default_free || 0), formatBytes(caps.default_total || 0), t('memoryPage.capDefaultDesc')]
        ];
        if (dma.total) capRows.push([t('memoryPage.capDma'), formatBytes(dma.free || 0), formatBytes(dma.total || 0), t('memoryPage.capDmaDesc')]);

        body.innerHTML = `
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:32px">${gauge('DRAM', dram)}${psram.total ? gauge('PSRAM', psram) : ''}</div>
            ${sect(t('memoryPage.heapShort'), grid('1.1fr repeat(5,1fr)', [t('memoryPage.typeCol'), t('memoryPage.largestBlock'), t('memoryPage.fragmentation'), t('memoryPage.allocBlocks'), t('memoryPage.freeBlocks'), t('memoryPage.minFreeEver')], heapRows))}
            ${sect(t('memoryPage.staticMemory'), kvs([
                ['.data', formatBytes(staticMem.data_size || 0), t('memoryPage.dataDesc')],
                ['.bss', formatBytes(staticMem.bss_size || 0), t('memoryPage.bssDesc')],
                ['.rodata', formatBytes(staticMem.rodata_size || 0), t('memoryPage.rodataDesc')],
                [t('memoryPage.dramStaticTotal'), formatBytes(staticMem.total_dram_static || 0), t('memoryPage.staticDataBss')]]))}
            ${sect(t('memoryPage.iramTitle'), kvs([
                [t('memoryPage.codeSection'), formatBytes(iram.text_size || 0)],
                [t('memoryPage.heapTotal'), formatBytes(iram.heap_total || 0)],
                [t('memoryPage.heapFree'), formatBytes(iram.heap_free || 0)]]))}
            ${rtc.total_available ? sect(t('memoryPage.rtcMemory'), `<div class="bar"><i style="width:${Math.min(100, (rtc.total_used / rtc.total_available * 100) || 0)}%"></i></div><div class="between t-note" style="margin-top:6px"><span>${t('memoryPage.rtcUsed')} ${formatBytes(rtc.total_used || 0)}</span><span>${t('memoryPage.rtcTotal')} ${formatBytes(rtc.total_available)}</span></div>`) : ''}
            ${sect(t('memoryPage.memCapability'), grid('1.4fr 1fr 1fr 1.4fr', [t('memoryPage.capType'), t('memoryPage.free'), t('memoryPage.total'), t('memoryPage.capDesc')], capRows))}
            ${nvs.total_entries ? sect(t('memoryPage.nvsStorage'), `<div class="bar"><i style="width:${Math.min(100, nvs.used_percent || 0)}%;background:${usedColor(nvs.used_percent || 0)}"></i></div>${kvs([
                [t('memoryPage.usedEntries'), nvs.used_entries], [t('memoryPage.freeEntries'), nvs.free_entries],
                [t('memoryPage.namespaceCount'), nvs.namespace_count], [t('memoryPage.usagePercent'), nvs.used_percent + '%']])}`) : ''}
            ${tipsHtml}
            ${data.tasks && data.tasks.length > 0 ? sect(t('memoryPage.taskStackUsage') + ' (' + t('memoryPage.taskCountLabel', { count: data.tasks.length }) + ')', `
                <table class="mtable" id="task-memory-table">
                    <thead><tr>
                        <th>${t('memoryPage.taskName')}</th>
                        <th data-sort="stack_alloc" class="sortable">${t('memoryPage.allocStack')} ⇅</th>
                        <th data-sort="stack_used" class="sortable">${t('memoryPage.usedStack')} ⇅</th>
                        <th data-sort="stack_hwm" class="sortable">${t('memoryPage.remainStack')} ⇅</th>
                        <th data-sort="stack_usage_pct" class="sortable">${t('memoryPage.usage')} ⇅</th>
                        <th data-sort="priority" class="sortable">${t('memoryPage.priority')} ⇅</th>
                        <th data-sort="state" class="sortable">${t('memoryPage.state')} ⇅</th>
                        ${data.tasks[0]?.cpu_percent !== undefined ? '<th data-sort="cpu_percent" class="sortable">' + t('memoryPage.cpu') + ' ⇅</th>' : ''}
                    </tr></thead>
                    <tbody id="task-table-body">${renderTaskRows(data.tasks, formatBytes)}</tbody>
                </table>
                ${data.total_stack_allocated ? `<div class="t-note" style="margin-top:8px">${t('memoryPage.taskStackTotal')}: ${formatBytes(data.total_stack_allocated)} · ${t('memoryPage.totalTaskCount')}: ${data.task_count}</div>` : ''}
                <div class="t-note" style="margin-top:4px">${t('memoryPage.stackHint')} · ${t('memoryPage.clickToSort')}</div>`) : ''}
            ${sect(t('memoryPage.runtimeStats'), kvs([
                [t('memoryPage.historyMinFreeHeap'), formatBytes(data.history?.min_free_heap_ever || 0)],
                [t('memoryPage.currentTaskCount'), data.task_count || 0]]))}
        `;
        
        // 缓存任务数据并初始化排序
        if (data.tasks && data.tasks.length > 0) {
            cachedTasksData = data.tasks;
            // 应用默认排序
            const sortedTasks = sortTasks(cachedTasksData, taskSortState.key, taskSortState.ascending);
            const tbody = document.getElementById('task-table-body');
            if (tbody) {
                tbody.innerHTML = renderTaskRows(sortedTasks, formatBytes);
            }
            // 初始化排序事件
            initTaskTableSort();
        }
        
        timestamp.textContent = t('memoryPage.updatedAt', { time: new Date().toLocaleTimeString() });
        
    } catch (error) {
        console.error('Memory detail error:', error);
        body.innerHTML = `<div class="state bad">${t('memoryPage.loadFailed')}</div><div class="t-note" style="margin-top:4px">${escapeHtml(error.message)}</div>`;
    }
}

// 点击模态框背景关闭
document.addEventListener('click', (e) => {
    const modal = document.getElementById('memory-detail-modal');
    if (e.target === modal) {
        hideMemoryDetailModal();
    }
});

// 导出全局函数
window.showMemoryDetailModal = showMemoryDetailModal;
window.hideMemoryDetailModal = hideMemoryDetailModal;
window.refreshMemoryDetail = refreshMemoryDetail;
// 旧路由 #/logs 会自动重定向到 #/terminal 并打开日志模态框
// =========================================================================

// =========================================================================
//                         自动化引擎页面
// =========================================================================

/**
 * 加载自动化引擎测试页面
 */
async function loadAutomationPage() {
    const pageCurrent = capturePageValidity();
    // 取消之前的订阅
    stopServiceStatusRefresh();
    
    const content = document.getElementById('page-content');
    content.innerHTML = `
        <div class="page page-automation">
            <div class="acts">
                <button class="btn sm pw ok" onclick="automationControl('start')"><svg class="i"><use href="#ri-play-line"/></svg>${t('common.start')}</button>
                <button class="btn sm pw bad" onclick="automationControl('stop')"><svg class="i"><use href="#ri-stop-line"/></svg>${t('common.stop')}</button>
                <button class="btn sm" onclick="automationControl('pause')"><svg class="i"><use href="#ri-pause-line"/></svg>${t('common.pause')}</button>
                <button class="btn sm" onclick="automationControl('reload')"><svg class="i"><use href="#ri-refresh-line"/></svg>${t('common.reload')}</button>
            </div>
            
            <!-- 状态 -->
            <div class="card stat" id="automation-status" style="padding:16px 20px">
                <div class="t-note" style="grid-column:1/-1">${t('common.loading')}</div>
            </div>
            
            <div>
                <div class="sec-h">
                    <span class="t-section sec-t">${t('automation.sources')}</span>
                    <div class="acts">
                        <button class="btn sm" onclick="showAddSourceModal()"><svg class="i"><use href="#ri-add-line"/></svg>${t('common.add')}</button>
                        <button class="btn sm" onclick="showImportSourceModal()" title="${t('securityPage.importConfigPack')}"><svg class="i"><use href="#ri-download-line"/></svg>${t('common.import')}</button>
                        <button type="button" class="btn icon sm automation-refresh-btn" onclick="refreshSources()" title="${t('common.refresh')}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                    </div>
                </div>
                <div class="card" style="padding:0" id="sources-list"><div class="empty"><p class="t-note">${t('common.loading')}</p></div></div>
            </div>
            
            <div>
                <div class="sec-h">
                    <span class="t-section sec-t">${t('automation.rules')}</span>
                    <div class="acts">
                        <button class="btn sm" onclick="showAddRuleModal()"><svg class="i"><use href="#ri-add-line"/></svg>${t('common.add')}</button>
                        <button class="btn sm" onclick="showImportRuleModal()" title="${t('securityPage.importConfigPack')}"><svg class="i"><use href="#ri-download-line"/></svg>${t('common.import')}</button>
                        <button type="button" class="btn icon sm automation-refresh-btn" onclick="refreshRules()" title="${t('common.refresh')}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                    </div>
                </div>
                <div class="card" style="padding:0" id="rules-list"><div class="empty"><p class="t-note">${t('common.loading')}</p></div></div>
            </div>
            
            <div>
                <div class="sec-h">
                    <span class="t-section sec-t">${t('automation.actions')}</span>
                    <div class="acts">
                        <button class="btn sm" onclick="showAddActionModal()"><svg class="i"><use href="#ri-add-line"/></svg>${t('common.add')}</button>
                        <button class="btn sm" onclick="showImportActionModal()" title="${t('securityPage.importConfigPack')}"><svg class="i"><use href="#ri-download-line"/></svg>${t('common.import')}</button>
                        <button type="button" class="btn icon sm automation-refresh-btn" onclick="refreshActions()" title="${t('common.refresh')}" aria-label="${t('common.refresh')}"><svg class="i"><use href="#ri-refresh-line"/></svg></button>
                    </div>
                </div>
                <div class="card" style="padding:0" id="actions-list"><div class="empty"><p class="t-note">${t('common.loading')}</p></div></div>
            </div>
        </div>
    `;
    
    // 加载所有数据（忽略单个失败，允许部分加载）
    await Promise.allSettled([
        refreshAutomationStatus(),
        refreshRules(),
        refreshSources(),
        refreshActions()
    ]);
    if (!pageCurrent()) return;
}

/**
 * 刷新自动化引擎状态
 */
async function refreshAutomationStatus() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('automation-status');
    if (!container) return;
    
    try {
        const result = await api.call('automation.status');
        if (!pageCurrent()) return;
        if (result.code === 0 && result.data) {
            const d = result.data;
            const stateClass = d.state === 'running' ? 'ok' : d.state === 'paused' ? 'warn' : '';
            const stateText = t(d.state === 'running' ? 'automationPage.stateRunning' : d.state === 'paused' ? 'automationPage.statePaused' : 'automationPage.stateStopped');
            const cell = (value, label) => `<div><div class="t-value stat-v">${value}</div><div class="t-label" style="margin-top:2px">${t(label)}</div></div>`;
            
            container.innerHTML =
                cell(`<span class="stat-st ${stateClass}"><i></i>${stateText}</span>`, 'automationPage.engineStatus') +
                cell(d.rules_count || 0, 'automationPage.rulesLabel') +
                cell(d.variables_count || 0, 'automationPage.variablesLabel') +
                cell(d.sources_count || 0, 'automationPage.sourcesLabel') +
                cell(d.rule_triggers || 0, 'automationPage.triggerCountLabel') +
                cell(formatUptimeSec(Math.floor((d.uptime_ms || 0) / 1000)), 'automationPage.runtimeLabel');
        } else {
            container.innerHTML = '<div class="t-note" style="grid-column:1/-1">' + (result.message || (typeof t === 'function' ? t('automationPage.getStatusFailed') : '获取状态失败')) + '</div>';
        }
    } catch (error) {
        if (!pageCurrent()) return;
        const isNetworkError = error.message.includes('fetch') || error.message.includes('network');
        container.innerHTML = '<div class="t-note" style="grid-column:1/-1">' + (isNetworkError && typeof t === 'function' ? t('automationPage.networkFailed') : error.message) + '</div>';
    }
}

/**
 * 格式化运行时长（秒）
 */
function formatUptimeSec(seconds) {
    if (seconds < 60) return t('automationPage.uptimeSecsBrief', { n: seconds });
    if (seconds < 3600) return t('automationPage.uptimeMinSecBrief', { m: Math.floor(seconds / 60), s: seconds % 60 });
    return t('automationPage.uptimeHrMinBrief', { h: Math.floor(seconds / 3600), m: Math.floor((seconds % 3600) / 60) });
}

/**
 * 自动化引擎控制
 */
async function automationControl(action) {
    try {
        const result = await api.call(`automation.${action}`);
        const message = (result.rawMessage || result.error || result.message) === 'restart_pending' ? runtimeText('restart_pending') : result.message || 'OK';
        showToast(typeof t === 'function' ? t('toast.actionResult', { action, msg: message }) : `${action}: ${message}`, result.code === 0 ? 'success' : 'error');
        if (result.code === 0) {
            await refreshAutomationStatus();
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.actionFailedMsg', { action, msg: error.message }) : `${action} 失败: ${error.message}`, 'error');
    }
}

/**
 * 刷新规则列表
 */
async function refreshRules() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('rules-list');
    if (!container) return;
    
    try {
        const result = await api.call('automation.rules.list');
        if (!pageCurrent()) return;
        if (result.code === 0 && result.data && result.data.rules) {
            const rules = result.data.rules;
            if (rules.length === 0) {
                container.innerHTML = '<div class="empty"><p class="t-note">' + t('automationPage.noRules') + '</p></div>';
                return;
            }
            
            const cols = 'style="--cols:1.2fr 1.4fr 1fr .6fr .6fr .7fr 196px"';
            container.innerHTML = `
                <div class="tr th" ${cols}>
                    <div>ID</div><div>${t('automationPage.ruleNameHeader')}</div><div>${t('common.enable')}</div><div>${t('automationPage.conditionHeader')}</div><div>${t('automationPage.actionHeader')}</div><div>${t('automationPage.triggerHeader')}</div><div class="act">${t('automationPage.operationHeader')}</div>
                </div>
                ${rules.map(r => {
                    const label = r.enabled ? t('common.disabled') : t('common.enabled');
                    const locked = r.restart_required || r.readonly;
                    const active = r.runtime_active !== false;
                    const deleting = r.pending_change === 'delete';
                    const handler = (name, ...args) => escapeHtml(name + '(' + args.map(x => JSON.stringify(x)).join(',') + ')');
                    return `
                    <div class="tr" ${cols}>
                        <div><span class="mono">${escapeHtml(r.id)}</span></div>
                        <div>${escapeHtml(r.name || r.id)}${r.restart_required ? ` <span class="tag">${t(deleting ? 'rulePack.pendingDelete' : 'rulePack.pending')}</span>` : ''}${r.manual_trigger ? ' ' : ''}${r.manual_trigger ? `<span class="tag" style="margin-left:6px">${t('common.manual')}</span>` : ''}</div>
                        <div><button class="switch ${r.enabled ? 'on' : ''}" role="switch" aria-checked="${!!r.enabled}" aria-label="${label}" title="${label}" onclick="${handler('toggleRule', r.id, !r.enabled)}"${locked ? ' disabled' : ''}></button></div>
                        <div>${r.conditions_count || 0}</div>
                        <div>${r.actions_count || 0}</div>
                        <div>${r.trigger_count || 0}</div>
                        <div class="act">${icoBtn('ri-play-line', t(deleting ? 'rulePack.pendingDelete' : r.restart_required ? 'rulePack.runCurrent' : 'automation.manualTrigger'), handler('triggerRule', r.id), '', !active || deleting)}${icoBtn('ri-edit-line', t('common.edit'), handler('editRule', r.id), '', locked)}${icoBtn('ri-download-line', t('rulePack.exportCurrent'), handler('showExportRuleModal', r.id), '', !active)}${icoBtn('ri-delete-bin-line', t('common.delete'), handler('deleteRule', r.id), 'dg', locked)}</div>
                    </div>`;
                }).join('')}
            `;
        } else {
            container.innerHTML = '<div class="empty"><p class="t-note">' + (result.message || (typeof t === 'function' ? t('automationPage.getRulesFailed') : '获取规则失败')) + '</p></div>';
        }
    } catch (error) {
        if (!pageCurrent()) return;
        const isNetworkError = error.message.includes('fetch') || error.message.includes('network');
        container.innerHTML = '<div class="empty"><p class="t-note" style="color:var(--bad)">' + (isNetworkError && typeof t === 'function' ? t('automationPage.networkFailed') : error.message) + '</p></div>';
    }
}

/**
 * 切换规则启用状态
 */
async function toggleRule(id, enable) {
    try {
        const action = enable ? 'automation.rules.enable' : 'automation.rules.disable';
        const result = await ruleWriteWithRevision(action, id);
        showToast(typeof t === 'function' ? t('toast.ruleToggled', { id, state: enable ? t('status.enabled') : t('status.disabled') }) + ': ' + (result.message || 'OK') : `规则 ${id} ${enable ? '启用' : '禁用'}: ${result.message || 'OK'}`, result.code === 0 ? 'success' : 'error');
        if (result.code === 0) {
            await refreshRules();
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.toggleRuleFailed') + ': ' + error.message : `切换规则状态失败: ${error.message}`, 'error');
    }
}

/**
 * 手动触发规则
 */
async function triggerRule(id) {
    try {
        const result = await api.call('automation.rules.trigger', { id });
        showToast(typeof t === 'function' ? t('toast.ruleTriggered', { id }) + ': ' + (result.message || 'OK') : `触发规则 ${id}: ${result.message || 'OK'}`, result.code === 0 ? 'success' : 'error');
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.triggerRuleFailed') + ': ' + error.message : `触发规则失败: ${error.message}`, 'error');
    }
}

/**
 * 刷新数据源列表
 */
async function refreshSources() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('sources-list');
    if (!container) return;
    
    try {
        const result = await api.call('automation.sources.list');
        if (!pageCurrent()) return;
        if (result.code === 0 && result.data && result.data.sources) {
            const sources = result.data.sources;
            if (sources.length === 0) {
                container.innerHTML = '<div class="empty"><p class="t-note">' + t('automationPage.noSources') + '</p></div>';
                return;
            }
            
            const cols = 'style="--cols:1.2fr 1.2fr .8fr 1fr .8fr 156px"';
            container.innerHTML = `
                <div class="tr th" ${cols}>
                    <div>ID</div><div>${t('automationPage.labelHeader')}</div><div>${t('automationPage.typeHeader')}</div><div>${t('common.enable')}</div><div>${t('automationPage.updateIntervalHeader')}</div><div class="act">${t('automationPage.operationHeader')}</div>
                </div>
                ${sources.map(s => {
                    const label = s.enabled ? t('common.disabled') : t('common.enabled');
                    return `
                    <div class="tr" ${cols}>
                        <div><span class="mono">${s.id}</span></div>
                        <div>${s.label || s.id}</div>
                        <div>${s.type || 'unknown'}</div>
                        <div><button class="switch ${s.enabled ? 'on' : ''}" role="switch" aria-checked="${!!s.enabled}" aria-label="${label}" title="${label}" onclick="toggleSource('${s.id}', ${!s.enabled})"></button></div>
                        <div>${s.poll_interval_ms ? (s.poll_interval_ms / 1000) + ' ' + t('time.seconds') : '-'}</div>
                        <div class="act">${icoBtn('ri-eye-line', t('automation.viewVariables'), `showSourceVariables('${s.id}')`)}${icoBtn('ri-download-line', t('securityPage.exportConfigPack'), `showExportSourceModal('${s.id}')`)}${icoBtn('ri-delete-bin-line', t('common.delete'), `deleteSource('${s.id}')`, 'dg')}</div>
                    </div>`;
                }).join('')}
            `;
        } else {
            container.innerHTML = '<div class="empty"><p class="t-note">' + (result.message || (typeof t === 'function' ? t('automationPage.getSourcesFailed') : '获取数据源失败')) + '</p></div>';
        }
    } catch (error) {
        if (!pageCurrent()) return;
        const isNetworkError = error.message.includes('fetch') || error.message.includes('network');
        container.innerHTML = '<div class="empty"><p class="t-note" style="color:var(--bad)">' + (isNetworkError && typeof t === 'function' ? t('automationPage.networkFailed') : error.message) + '</p></div>';
    }
}

// 缓存变量数据用于过滤
let allVariables = [];

/**
 * 刷新变量列表
 */
async function refreshVariables() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('variables-list');
    const countBadge = document.getElementById('variables-count');
    if (!container) return;
    
    container.innerHTML = '<div class="loading-small">' + t('common.loading') + '</div>';
    
    try {
        const result = await api.call('automation.variables.list', { include_meta: true });
        if (!pageCurrent()) return;
        if (result.code === 0 && result.data && result.data.variables) {
            allVariables = result.data.variables;
            if (countBadge) countBadge.textContent = allVariables.length;
            renderVariables(allVariables);
        } else {
            container.innerHTML = `<p style="text-align:center;color:var(--ink-3)">${escapeHtml(result.message || (typeof t === 'function' ? t('sshPage.getVarFailed') : '获取变量失败'))}</p>`;
        }
    } catch (error) {
        if (!pageCurrent()) return;
        container.innerHTML = `<p style="text-align:center;color:var(--bad)">${escapeHtml(error.message)}</p>`;
    }
}

/**
 * 过滤变量
 */
function filterVariables() {
    const filter = document.getElementById('variable-filter').value.toLowerCase().trim();
    if (!filter) {
        renderVariables(allVariables);
        return;
    }
    const filtered = allVariables.filter(v => 
        v.name.toLowerCase().includes(filter) || 
        (v.source_id && v.source_id.toLowerCase().includes(filter))
    );
    renderVariables(filtered);
}

/**
 * 渲染变量列表
 */
function renderVariables(variables) {
    const container = document.getElementById('variables-list');
    if (!container) return;
    
    if (variables.length === 0) {
        container.innerHTML = '<p style="text-align:center;color:var(--ink-3)">' + (typeof t === 'function' ? t('automationPage.noVariables') : '暂无变量数据') + '</p>';
        return;
    }
    
    // 按来源分组
    const grouped = {};
    variables.forEach(v => {
        const source = v.source_id || 'system';
        if (!grouped[source]) grouped[source] = [];
        grouped[source].push(v);
    });
    
    let html = '<div class="variables-grouped">';
    for (const [source, vars] of Object.entries(grouped)) {
        html += `
            <details class="variable-group" open>
                <summary class="variable-group-header">
                    <span class="source-name"><svg class="i"><use href="#ri-signal-wifi-3-line"/></svg> ${source}</span>
                    <span class="variable-count">${t('promptRepair.variableCount', {count: vars.length})}</span>
                </summary>
                <div class="variable-items">
                    <table class="data-table compact">
                        <thead>
                            <tr>
                                <th>${typeof t === 'function' ? t('sshPage.varTableName') : '变量名'}</th>
                                <th>${typeof t === 'function' ? t('sshPage.varTableType') : '类型'}</th>
                                <th>${typeof t === 'function' ? t('sshPage.varTableValue') : '当前值'}</th>
                                <th>${typeof t === 'function' ? t('sshPage.varTableUpdated') : '更新时间'}</th>
                            </tr>
                        </thead>
                        <tbody>
                            ${vars.map(v => `
                                <tr>
                                    <td><code class="variable-name">${v.name}</code></td>
                                    <td><span class="type-badge type-${v.type || 'unknown'}">${v.type || '-'}</span></td>
                                    <td class="variable-value">${formatVariableValue(v.value, v.type)}</td>
                                    <td class="variable-time">${formatVariableUpdateTime(v)}</td>
                                </tr>
                            `).join('')}
                        </tbody>
                    </table>
                </div>
            </details>
        `;
    }
    html += '</div>';
    container.innerHTML = html;
}

/**
 * 格式化变量值显示
 */
function formatVariableValue(value, type) {
    if (value === undefined || value === null) return '<span class="null-value">null</span>';
    
    if (type === 'number') {
        // 数字保留2位小数
        const num = parseFloat(value);
        if (!isNaN(num)) {
            return `<span class="number-value">${num % 1 === 0 ? num : num.toFixed(2)}</span>`;
        }
    } else if (type === 'boolean') {
        return value ? '<span class="bool-true"><svg class="i"><use href="#ri-check-line"/></svg> true</span>' : '<span class="bool-false"><svg class="i"><use href="#ri-close-line"/></svg> false</span>';
    } else if (type === 'string') {
        const str = String(value);
        if (str.length > 50) {
            return `<span class="string-value" title="${str}">"${str.substring(0, 47)}..."</span>`;
        }
        return `<span class="string-value">"${str}"</span>`;
    }
    
    // 默认：JSON 格式
    const str = JSON.stringify(value);
    if (str.length > 60) {
        return `<code title="${str}">${str.substring(0, 57)}...</code>`;
    }
    return `<code>${str}</code>`;
}

/**
 * 格式化时间为相对时间
 */
function formatTimeAgo(timestamp) {
    const now = Date.now();
    const ts = typeof timestamp === 'number' ? timestamp * 1000 : new Date(timestamp).getTime();
    const diff = now - ts;

    if (diff >= 0 && diff < 86400000) {
        return formatAgeMs(diff);
    }
    return new Date(ts).toLocaleString();
}

function formatAgeMs(ageMs) {
    const age = Math.max(0, ageMs);
    if (age < 1000) return t('common.justNow');
    if (age < 60000) return `${Math.floor(age / 1000)}${t('common.secondsAgo')}`;
    if (age < 3600000) return `${Math.floor(age / 60000)}${t('common.minutesAgo')}`;
    if (age < 86400000) return `${Math.floor(age / 3600000)}${t('common.hoursAgo')}`;
    return `${Math.floor(age / 86400000)}${t('common.daysAgo')}`;
}

function formatVariableUpdateTime(variable) {
    if (!variable) return '-';
    if (typeof variable.age_ms === 'number') {
        return formatAgeMs(variable.age_ms);
    }
    if (variable.updated_at) {
        return formatTimeAgo(variable.updated_at);
    }
    return '-';
}

/**
 * 刷新动作模板列表
 */
async function refreshActions() {
    const pageCurrent = capturePageValidity();
    const container = document.getElementById('actions-list');
    if (!container) return;
    
    try {
        const result = await api.call('automation.actions.list', {});
        if (!pageCurrent()) return;
        const actions = result.data?.templates || [];
        
        if (actions.length === 0) {
            container.innerHTML = '<div class="empty"><p class="t-note">' + t('automationPage.noActions') + '</p></div>';
        } else {
            const cols = 'style="--cols:1.2fr 1.2fr .8fr .9fr 1.4fr 196px"';
            container.innerHTML = `
                <div class="tr th" ${cols}>
                    <div>ID</div><div>${t('automationPage.actionNameHeader')}</div><div>${t('automationPage.actionTypeHeader')}</div><div>${t('automationPage.actionModeHeader')}</div><div>${t('automationPage.descriptionHeader')}</div><div class="act">${t('automationPage.operationHeader')}</div>
                </div>
                ${actions.map(a => `
                    <div class="tr" ${cols}>
                        <div><span class="mono">${a.id}</span></div>
                        <div>${a.name || a.id}</div>
                        <div>${getActionTypeLabel(a.type)}</div>
                        <div>${a.async ? t('automation.asyncAction') : t('automationPage.syncMode')}</div>
                        <div>${a.description || '-'}</div>
                        <div class="act">${icoBtn('ri-play-line', t('common.test'), `testAction('${a.id}')`)}${icoBtn('ri-edit-line', t('common.edit'), `editAction('${a.id}')`)}${icoBtn('ri-download-line', t('securityPage.exportConfigPack'), `showExportActionModal('${a.id}')`)}${icoBtn('ri-delete-bin-line', t('common.delete'), `deleteAction('${a.id}')`, 'dg')}</div>
                    </div>
                `).join('')}
            `;
        }
    } catch (error) {
        if (!pageCurrent()) return;
        container.innerHTML = '<div class="empty"><p class="t-note" style="color:var(--bad)">' + t('filePage.loadFailed') + ': ' + error.message + '</p></div>';
    }
}

/**
 * 获取动作类型标签
 */
function getActionTypeLabel(type) {
    const labels = {
        'led': 'LED',
        'ssh_cmd': 'SSH',
        'gpio': 'GPIO',
        'webhook': 'Webhook',
        'log': t('common.log'),
        'set_var': t('common.variable'),
        'device_ctrl': t('common.devices')
    };
    return labels[type] || type;
}

/**
 * 获取动作类型徽章样式
 */
function getActionTypeBadge(type) {
    const badges = {
        'led': 'info',
        'ssh_cmd': 'primary',
        'gpio': 'warning',
        'webhook': 'secondary',
        'log': 'light',
        'set_var': 'dark',
        'device_ctrl': 'danger'
    };
    return badges[type] || 'secondary';
}

/**
 * 显示添加动作模板对话框
 */
function showAddActionModal() {
    const modal = document.createElement('div');
    modal.className = 'modal active';
    modal.id = 'action-modal';
    const types = [['cli', 'automation.actionTypeCli'], ['ssh_cmd_ref', 'automation.actionTypeSsh'], ['led', 'automation.actionTypeLed'], ['log', 'automation.actionTypeLog'], ['set_var', 'automation.actionTypeSetVar'], ['webhook', 'automation.actionTypeWebhook']];
    const tiles = types.map(([v, k], i) => `<label class="tile${i === 0 ? ' on' : ''}" data-type="${v}" style="padding:10px 12px;cursor:pointer"><input type="radio" name="action-type" value="${v}"${i === 0 ? ' checked' : ''} style="position:absolute;opacity:0;pointer-events:none"><span class="t-body" style="font-weight:600">${t(k)}</span></label>`).join('');
    modal.innerHTML = sheet(660, '<span id="action-modal-title"></span>', `
        ${gt(t('automation.actionType'))}
        <div class="action-type-grid" style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px">${tiles}</div>
        <div id="action-type-fields" class="gw"></div>
        ${gt(t('automation.basicInfo'))}
        ${grp(
            row(t('automation.actionId'), inp('action-id', 180, t('automationPage.actionIdPh'), 'mono'), t('automationPage.actionIdHintShort')) +
            row(t('automation.sourceLabel'), inp('action-name', 180, t('automation.displayNamePlaceholder')), t('automation.leaveEmptyUseId')) +
            row(t('common.description'), inp('action-description', 180, t('common.optional'))) +
            row(t('automation.executionDelay'), inp('action-delay', 80, '', 'num', 'type="number" value="0" min="0"')) +
            row(t('automation.asyncExecute'), swc('action-async'), t('automationPage.asyncHintShort')))}`,
        `<button class="btn lg" onclick="closeModal('action-modal')">${t('common.cancel')}</button><button class="btn lg primary" onclick="submitAction()">${t('automationPage.saveAction')}</button>`);
    document.body.appendChild(modal);

    // 绑定类型卡片点击事件
    modal.querySelectorAll('.action-type-grid input').forEach(radio => {
        radio.addEventListener('change', updateActionTypeFields);
    });

    updateActionTypeFields();
}

/**
 * 更新动作类型字段
 */
function updateActionTypeFields() {
    const checked = document.querySelector('input[name="action-type"]:checked');
    const type = checked ? checked.value : 'cli';
    const container = document.getElementById('action-type-fields');

    // 更新卡片选中状态与标题
    document.querySelectorAll('#action-modal .action-type-grid .tile').forEach(card => {
        card.classList.toggle('on', card.dataset.type === type);
    });
    const titleEl = document.getElementById('action-modal-title');
    if (titleEl) {
        const typeKeys = { cli: 'automation.actionTypeCli', ssh_cmd_ref: 'automation.actionTypeSsh', led: 'automation.actionTypeLed', log: 'automation.actionTypeLog', set_var: 'automation.actionTypeSetVar', webhook: 'automation.actionTypeWebhook' };
        const isEdit = document.getElementById('action-modal')?.dataset.edit === '1';
        titleEl.textContent = t(isEdit ? 'ui.editActionTemplate' : 'automation.newActionTemplate') + (typeKeys[type] ? ' · ' + t(typeKeys[type]) : '');
    }

    const quick = (cmd, label) => `<button type="button" class="btn sm" onclick="setCliPreset('${cmd}')">${label}</button>`;
    const fields = {
        cli: gt(t('automation.cliConfig')) + grp(
                row(t('automationPage.commandLine'), inp('action-cli-command', 230, t('automation.cliPlaceholder'), 'mono'), t('automationPage.actionCliHintShort'))) +
            `<div style="display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 0">${quick('gpio --set 48 1', 'GPIO')}${quick('device --power-on agx0', t('automationPage.quickAgxOn'))}${quick('device --reset agx0', t('automationPage.quickAgxRestart'))}${quick('fan --set --id 0 --speed 80', t('automation.fan'))}${quick('led --effect --device board --name fire', t('automationPage.quickLedEffect'))}</div>` +
            gt(t('automation.advancedOptions')) + grp(
                row(t('automation.resultVariable'), inp('action-cli-var', 180, t('automation.resultVarPlaceholder'), 'mono'), t('automation.resultVariableHint')) +
                row(t('automation.timeout'), inp('action-cli-timeout', 80, '', 'num', 'type="number" value="5000"')) ),
        ssh_cmd_ref: gt(t('automation.sshCmdConfig')) + grp(
                row(t('common.command'), `<select id="action-ssh-cmd-id" class="field" onchange="updateSshCmdRefPreview()" style="width:200px"><option value="">${t('automation.selectCommand')}</option></select>`, t('automation.sshCmdHint'))) +
            `<div id="ssh-cmd-preview" class="gw" style="display:none">${gt(t('automationPage.commandDetails'))}${grp(
                row(t('securityPage.host'), '<span class="t-label" id="preview-host">-</span>') +
                row(t('common.command'), '<span class="mono t-label" id="preview-cmd">-</span>') +
                row(t('common.variable'), '<span class="mono t-label" id="preview-var">-</span>'))}</div>`,
        led: gt(t('automation.ledConfig')) + grp(
                row(t('common.device'), `<select id="action-led-device" class="field" onchange="updateActionLedOptions()" style="width:190px"><option value="">-- ${t('automationPage.selectDevice')} --</option></select>`, t('automation.selectLedDeviceHint')) +
                `<div class="row" id="action-led-type-group" style="display:none"><div class="rl">${t('automationPage.controlType')}<small>${t('automationPage.ctrlTypeHint')}</small></div><div class="rc"><select id="action-led-type" class="field" onchange="updateActionLedTypeFields()" style="width:190px">
                    <option value="fill">${t('automationPage.ctrlFill')}</option>
                    <option value="effect">${t('automation.programEffect')}</option>
                    <option value="brightness">${t('automationPage.brightnessOnly')}</option>
                    <option value="off">${t('automationPage.turnOff')}</option>
                </select></div></div>` +
                `<div class="row" id="action-led-matrix-type-group" style="display:none"><div class="rl">${t('automationPage.controlType')}<small>${t('automationPage.ctrlTypeHint')}</small></div><div class="rc"><select id="action-led-matrix-type" class="field" onchange="updateActionLedTypeFields()" style="width:190px">
                    <option value="fill">${t('automationPage.ctrlFill')}</option>
                    <option value="effect">${t('automation.programEffect')}</option>
                    <option value="text">${t('automationPage.textDisplay')}</option>
                    <option value="image">${t('automation.displayImage')}</option>
                    <option value="qrcode">${t('automation.displayQrCode')}</option>
                    <option value="filter">${t('automationPage.filterDisplay')}</option>
                    <option value="filter_stop">${t('automationPage.filterStop')}</option>
                    <option value="text_stop">${t('automationPage.textStop')}</option>
                    <option value="brightness">${t('automationPage.brightnessOnly')}</option>
                    <option value="off">${t('automationPage.turnOffDevice')}</option>
                </select></div></div>` +
                '<div class="rows" id="action-led-params"></div>'),
        log: gt(t('automationPage.logConfig')) + grp(
                row(t('automationPage.logLevel'), `<select id="action-log-level" class="field" style="width:110px"><option value="3">INFO</option><option value="2">WARN</option><option value="1">ERROR</option><option value="4">DEBUG</option></select>`) +
                row(t('automationPage.logMessage'), inp('action-log-message', 230, t('automationPage.actionLogMsgPlaceholder')), t('automationPage.logMsgHintShort'))),
        set_var: gt(t('automationPage.varConfig')) + grp(
                row(t('automationPage.varNameLabel'), inp('action-var-name', 200, t('promptRepair.variableExample'), 'mono')) +
                row(t('automationPage.value'), inp('action-var-value', 200, t('promptRepair.valueExample'), 'mono'), t('automationPage.varValuePlaceholder'))),
        webhook: gt(t('promptRepair.webhookConfig')) + grp(
                row('URL', inp('action-webhook-url', 230, 'https://…', 'mono')) +
                row(t('automationPage.method'), `<select id="action-webhook-method" class="field" style="width:110px"><option value="POST">POST</option><option value="GET">GET</option><option value="PUT">PUT</option></select>`)) +
            gt(t('automationPage.requestBody')) + `<textarea class="field mono" id="action-webhook-body" style="height:72px;padding:8px 12px;width:100%" placeholder="${t('automationPage.requestBodyHint')}" aria-label="${t('automationPage.requestBody')}"></textarea>`
    };
    
    container.innerHTML = fields[type] || gt(t('automation.pleaseSelectActionType'));
    
    // SSH 命令类型时加载命令列表
    if (type === 'ssh_cmd_ref') {
        loadSshCommandsForAction();
    }
    
    // LED 类型时加载设备列表
    if (type === 'led') {
        loadLedDevicesForAction();
    }
}

/**
 * 提交动作模板
 */
async function submitAction(originalId = null) {
    clearFieldErrors();
    const id = document.getElementById('action-id').value.trim();
    const name = document.getElementById('action-name').value.trim();
    const checked = document.querySelector('input[name="action-type"]:checked');
    const type = checked ? checked.value : '';
    const description = document.getElementById('action-description').value.trim();
    const delay = parseInt(document.getElementById('action-delay').value) || 0;
    const async = document.getElementById('action-async')?.checked || false;
    
    if (!id) {
        showToast(typeof t === 'function' ? t('toast.fillActionId') : '请填写动作 ID', 'error');
        return;
    }
    if (!type) {
        showToast(typeof t === 'function' ? t('automation.pleaseSelectActionType') : '请选择动作类型', 'error');
        return;
    }
    
    if (originalId && id !== originalId) {
        fieldError('action-id', t('inputRepair.immutableId'));
        return;
    }
    const data = { id, name: name || id, type, description, delay_ms: delay, async,
        enabled: originalId ? document.getElementById('action-modal').dataset.enabled !== 'false' : true };
    
    // 根据类型收集特定字段
    try { switch (type) {
        case 'cli':
            const cliCmd = document.getElementById('action-cli-command')?.value?.trim();
            if (!cliCmd) {
                showToast(typeof t === 'function' ? t('toast.fillCommand') : '请填写命令行', 'error');
                return;
            }
            data.cli = {
                command: cliCmd,
                var_name: document.getElementById('action-cli-var')?.value?.trim() || '',
                timeout_ms: parseInt(document.getElementById('action-cli-timeout')?.value) || 5000
            };
            break;
        case 'ssh_cmd_ref':
            const cmdId = document.getElementById('action-ssh-cmd-id')?.value;
            if (!cmdId) {
                showToast(typeof t === 'function' ? t('toast.selectSshCommand') : '请选择 SSH 命令', 'error');
                return;
            }
            data.ssh_ref = { cmd_id: cmdId };
            break;
        case 'led':
            const ledDevice = document.getElementById('action-led-device')?.value;
            if (!ledDevice) {
                showToast(typeof t === 'function' ? t('toast.selectLedDevice') : '请选择 LED 设备', 'error');
                return;
            }
            const isMatrix = ledDevice === 'matrix';
            const ledTypeSelect = isMatrix 
                ? document.getElementById('action-led-matrix-type')
                : document.getElementById('action-led-type');
            const ledCtrlType = ledTypeSelect?.value || 'fill';
            
            data.led = {
                device: ledDevice,
                ctrl_type: ledCtrlType
            };
            
            // 根据控制类型收集参数
            switch (ledCtrlType) {
                case 'fill':
                    data.led.color = document.getElementById('action-led-color')?.value || '#FF0000';
                    data.led.brightness = readNumericInput('action-led-brightness', true);
                    data.led.index = readNumericInput('action-led-index', true);
                    break;
                case 'effect':
                    data.led.effect = document.getElementById('action-led-effect')?.value;
                    data.led.speed = readNumericInput('action-led-speed', true);
                    data.led.color = document.getElementById('action-led-color')?.value || '#FF0000';
                    if (!data.led.effect) {
                        showToast(typeof t === 'function' ? t('toast.selectAnimation') : '请选择动画', 'error');
                        return;
                    }
                    break;
                case 'brightness':
                    data.led.brightness = readNumericInput('action-led-brightness', true);
                    break;
                case 'off':
                    // 无需额外参数
                    break;
                case 'text':
                    data.led.text = document.getElementById('action-led-text')?.value?.trim();
                    if (!data.led.text) {
                        showToast(typeof t === 'function' ? t('toast.enterText') : '请输入文本内容', 'error');
                        return;
                    }
                    data.led.font = document.getElementById('action-led-font')?.value || '';
                    data.led.color = document.getElementById('action-led-color')?.value || '#00FF00';
                    data.led.align = document.getElementById('action-led-align')?.value || 'center';
                    data.led.scroll = document.getElementById('action-led-scroll')?.value || 'none';
                    data.led.speed = readNumericInput('action-led-speed', true);
                    data.led.loop = document.getElementById('action-led-loop')?.checked || false;
                    data.led.x = readNumericInput('action-led-x', true);
                    data.led.y = readNumericInput('action-led-y', true);
                    data.led.auto_pos = document.getElementById('action-led-auto-pos')?.checked || false;
                    break;
                case 'image':
                    data.led.image_path = document.getElementById('action-led-image-path')?.value?.trim();
                    if (!data.led.image_path) {
                        showToast((typeof t === 'function' ? t('toast.enterImagePath') : '请输入图像路径'), 'error');
                        return;
                    }
                    data.led.center = document.getElementById('action-led-center')?.checked || false;
                    break;
                case 'qrcode':
                    data.led.qr_text = document.getElementById('action-led-qr-text')?.value?.trim();
                    if (!data.led.qr_text) {
                        showToast(typeof t === 'function' ? t('toast.enterQrContent') : '请输入QR码内容', 'error');
                        return;
                    }
                    data.led.qr_ecc = document.getElementById('action-led-qr-ecc')?.value || 'M';
                    data.led.qr_fg = document.getElementById('action-led-qr-fg')?.value || '#FFFFFF';
                    data.led.qr_bg_image = document.getElementById('action-led-qr-bg')?.value || '';
                    break;
                case 'filter':
                    data.led.filter = document.getElementById('action-led-filter')?.value;
                    if (!data.led.filter) {
                        showToast(typeof t === 'function' ? t('toast.selectFilter') : '请选择滤镜', 'error');
                        return;
                    }
                    // 根据滤镜类型收集对应参数
                    const fConfig = filterConfig[data.led.filter];
                    const modal = document.getElementById('action-modal');
                    const sameFilter = originalId && modal.dataset.originalFilter === data.led.filter;
                    if (fConfig && fConfig.params) {
                        const params = sameFilter ? { ...modal._originalFilterParams } : {};
                        const changed = modal._filterParamEdits?.get(data.led.filter);
                        fConfig.params.forEach(param => {
                            const el = document.getElementById(`action-filter-${param}`);
                            if (el && (!sameFilter || changed?.has(param))) {
                                params[param] = readNumericInput(`action-filter-${param}`, true);
                            }
                        });
                        if (!sameFilter || modal._originalFilterParams !== undefined || Object.keys(params).length) {
                            data.led.filter_params = params;
                        }
                    }
                    break;
            }
            break;
        case 'log':
            const logMsg = document.getElementById('action-log-message')?.value?.trim();
            if (!logMsg) {
                showToast(typeof t === 'function' ? t('toast.fillLogMessage') : '请填写日志消息', 'error');
                return;
            }
            data.log = {
                level: parseInt(document.getElementById('action-log-level').value),
                message: logMsg
            };
            break;
        case 'set_var':
            const varName = document.getElementById('action-var-name')?.value?.trim();
            const varValue = document.getElementById('action-var-value')?.value?.trim();
            if (!varName || !varValue) {
                showToast(typeof t === 'function' ? t('toast.fillVarNameValue') : '请填写变量名和值', 'error');
                return;
            }
            data.set_var = {
                variable: varName,
                value: varValue
            };
            break;
        case 'webhook':
            const webhookUrl = document.getElementById('action-webhook-url')?.value?.trim();
            if (!webhookUrl) {
                showToast(typeof t === 'function' ? t('toast.fillWebhookUrl') : '请填写 Webhook URL', 'error');
                return;
            }
            data.webhook = {
                url: webhookUrl,
                method: document.getElementById('action-webhook-method').value,
                body_template: document.getElementById('action-webhook-body')?.value || ''
            };
            break;
    }
    
    } catch (_) { return; }

    try {
        const result = await api.call(originalId ? 'automation.actions.update' : 'automation.actions.add', data);
        if (result.code === 0) {
            showToast(originalId ? t('toast.saved') : t('toast.actionCreated', { id }), 'success');
            closeModal('action-modal');
            await refreshActions();
        } else {
            showToast(typeof t === 'function' ? t('toast.actionCreateFailed') + ': ' + result.message : `创建失败: ${result.message}`, 'error');
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.actionCreateFailed') + ': ' + error.message : `创建失败: ${error.message}`, 'error');
    }
}

/**
 * CLI 命令快捷填充
 */
function setCliPreset(cmd) {
    const input = document.getElementById('action-cli-command');
    if (input) {
        input.value = cmd;
        input.focus();
    }
}

/**
 * 加载 SSH 主机列表 (用于动作模板)
 */
async function loadSshHostsForAction() {
    try {
        const select = document.getElementById('action-ssh-host');
        if (!select) return;
        
        const result = await api.call('ssh.hosts.list', {});
        select.innerHTML = ("<option value=\"\">" + t('sshPage.selectHost') + "</option>");
        
        if (result.code === 0 && result.data?.hosts) {
            result.data.hosts.forEach(host => {
                const opt = document.createElement('option');
                opt.value = host.id;
                opt.textContent = `${host.name || host.id} (${host.host}:${host.port || 22})`;
                select.appendChild(opt);
            });
        }
        
        if (select.options.length === 1) {
            // 没有配置主机，提示用户
            select.innerHTML = ("<option value=\"\">" + t('sshPage.pleaseConfigSshHost') + "</option>");
        }
    } catch (e) {
        console.error('加载 SSH 主机列表失败:', e);
        const select = document.getElementById('action-ssh-host');
        if (select) {
            select.innerHTML = '<option value="">-- ' + (typeof t === 'function' ? t('common.loadFailed') : '加载失败') + ' --</option>';
        }
    }
}

/**
 * 加载 SSH 指令列表 (用于动作模板) - 保留用于兼容
 */
async function loadSshCommandsForAction() {
    try {
        const select = document.getElementById('action-ssh-cmd-id');
        if (!select) return;
        
        const result = await api.call('ssh.commands.list', {});
        select.innerHTML = ("<option value=\"\">" + t('automation.selectCommand') + "</option>");
        
        if (result.code === 0 && result.data?.commands) {
            result.data.commands.forEach(cmd => {
                const opt = document.createElement('option');
                opt.value = cmd.id;
                opt.textContent = `${cmd.name || cmd.id} (${cmd.host_id || 'localhost'})`;
                opt.dataset.host = cmd.host_id || '';
                opt.dataset.command = cmd.command || '';
                opt.dataset.varName = cmd.var_name || '';
                select.appendChild(opt);
            });
        }
    } catch (e) {
        console.error('加载 SSH 指令列表失败:', e);
    }
}

/**
 * 更新 SSH 指令预览
 */
async function updateSshCmdRefPreview() {
    const select = document.getElementById('action-ssh-cmd-id');
    const preview = document.getElementById('ssh-cmd-preview');
    if (!select || !preview) return;
    
    const cmdId = select.value;
    if (!cmdId) {
        preview.style.display = 'none';
        return;
    }
    
    // 从 API 获取完整指令信息
    try {
        const result = await api.call('ssh.commands.get', { id: cmdId });
        console.log('SSH command get result:', result);
        if (result.code === 0 && result.data) {
            const cmd = result.data;
            console.log('SSH command data:', cmd);
            document.getElementById('preview-host').textContent = cmd.host_id || '-';
            document.getElementById('preview-cmd').textContent = cmd.command || '-';
            // varName 字段只在配置了变量名时才存在
            const varName = cmd.varName || cmd.var_name || '';
            document.getElementById('preview-var').textContent = varName || t('ui.configNotSet');
            preview.style.display = 'block';
        } else {
            preview.style.display = 'none';
        }
    } catch (e) {
        console.error('获取 SSH 指令详情失败:', e);
        preview.style.display = 'none';
    }
}

/**
 * 加载 LED 设备列表 (用于动作模板)
 */
async function loadLedDevicesForAction() {
    try {
        const select = document.getElementById('action-led-device');
        const effectSelect = document.getElementById('action-led-effect');
        if (!select) return;
        
        const result = await api.ledList();
        select.innerHTML = ("<option value=\"\">" + t('sshPage.selectDevice') + "</option>");
        
        if (result.data?.devices) {
            result.data.devices.forEach(dev => {
                const opt = document.createElement('option');
                opt.value = dev.name;
                opt.textContent = `${dev.name} (${dev.count || 0} LEDs)`;
                opt.dataset.effects = JSON.stringify(dev.effects || []);
                select.appendChild(opt);
            });
        }
    } catch (e) {
        console.error('加载 LED 设备列表失败:', e);
    }
}

/**
 * 更新动作 LED 选项（根据设备类型显示不同控制类型）
 */
function updateActionLedOptions() {
    const deviceSelect = document.getElementById('action-led-device');
    const typeGroup = document.getElementById('action-led-type-group');
    const matrixTypeGroup = document.getElementById('action-led-matrix-type-group');
    const paramsContainer = document.getElementById('action-led-params');
    
    if (!deviceSelect) return;
    
    const deviceName = deviceSelect.value;
    const opt = deviceSelect.options[deviceSelect.selectedIndex];
    const isMatrix = deviceName === 'matrix';
    
    // 显示对应的控制类型选择器
    if (typeGroup) typeGroup.style.display = !deviceName ? 'none' : (isMatrix ? 'none' : '');
    if (matrixTypeGroup) matrixTypeGroup.style.display = isMatrix ? '' : 'none';
    
    // 存储设备特效列表
    if (opt && opt.dataset.effects) {
        window._actionLedEffects = JSON.parse(opt.dataset.effects || '[]');
    } else {
        window._actionLedEffects = [];
    }
    
    // 清空参数区域
    if (paramsContainer) paramsContainer.innerHTML = '';
    
    // 如果选择了设备，自动更新参数
    if (deviceName) {
        updateActionLedTypeFields();
    }
}

/**
 * 根据控制类型更新 LED 参数字段
 */
function updateActionLedTypeFields() {
    const deviceSelect = document.getElementById('action-led-device');
    const paramsContainer = document.getElementById('action-led-params');
    if (!deviceSelect || !paramsContainer) return;
    
    const deviceName = deviceSelect.value;
    const isMatrix = deviceName === 'matrix';
    const typeSelect = isMatrix 
        ? document.getElementById('action-led-matrix-type')
        : document.getElementById('action-led-type');
    
    if (!typeSelect) return;
    const ledType = typeSelect.value;
    const effects = window._actionLedEffects || [];
    
    let html = '';
    const setVal = (valId, suffix = '') => `document.getElementById('${valId}').textContent=this.value${suffix ? "+'" + suffix + "'" : ''}`;
    const slider = (id, min, max, val) => ledSlider(id, min, max, val, setVal(id + '-val')) + ledVal(id + '-val', val);
    const varBtn = target => `<button type="button" class="btn icon sm" onclick="showVariableSelectModal('${target}')" title="${t('automationPage.insertVariableTitle')}" aria-label="${t('automationPage.insertVariableTitle')}"><svg class="i"><use href="#ri-bar-chart-line"/></svg></button>`;
    const sel = (id, opts, w = 130) => `<select id="${id}" class="field" style="width:${w}px">${opts}</select>`;
    const hint = txt => row(t('common.params'), `<span class="t-note">${txt}</span>`);
    
    switch (ledType) {
        case 'fill': {
            const dots = ['#ff0000', '#ff6600', '#ffff00', '#00ff00', '#00ffff', '#0066ff', '#ffffff'].map(c => `<button type="button" class="dotc" style="background:${c}" onclick="setActionLedColor('${c}')" aria-label="${c}"></button>`).join('');
            html = row(t('dataWidget.color'), `<span class="inl">${swatch('action-led-color', '#ff0000')}${dots}</span>`) +
                row(t('ledPage.ccBrightness'), slider('action-led-brightness', 0, 255, 128)) +
                row(t('automationPage.indexPlaceholder'), inp('action-led-index', 80, t('automationPage.ledIndexPlaceholder'), 'num', 'type="number" value="255" min="0" max="255"'));
            break;
        }
        case 'effect': {
            const effectOptions = effects.map(e => `<option value="${e}">${e}</option>`).join('');
            html = row(t('ledPage.effects'), sel('action-led-effect', effectOptions || '<option value="">' + t('ledPage.noEffects') + '</option>', 190)) +
                row(t('ledPage.speed'), slider('action-led-speed', 1, 100, 50)) +
                row(t('dataWidget.color'), swatch('action-led-color', '#ff0000'));
            break;
        }
        case 'brightness':
            html = row(t('ledPage.ccBrightness'), slider('action-led-brightness', 0, 255, 128));
            break;
        case 'off':
            html = hint(t('automationPage.ledOffHint'));
            break;
        case 'filter_stop':
            html = hint(t('automationPage.filterStopHint'));
            break;
        case 'text_stop':
            html = hint(t('automationPage.textStopHint'));
            break;
        case 'text':
            html = row(t('automationPage.textContentLabel'), `<span class="inl">${inp('action-led-text', 200, t('automationPage.textPlaceholder'))}${varBtn('action-led-text')}</span>`) +
                row(t('ledPage.font'), sel('action-led-font', `<option value="">${t('ledPage.defaultFont')}</option>`)) +
                row(t('led.color'), swatch('action-led-color', '#00ff00')) +
                row(t('ledPage.alignment'), sel('action-led-align', `<option value="left">${t('ledPage.alignLeft')}</option><option value="center" selected>${t('ledPage.alignCenter')}</option><option value="right">${t('ledPage.alignRight')}</option>`, 110)) +
                row(t('automationPage.scroll'), sel('action-led-scroll', `<option value="none">${t('automationPage.scrollNone')}</option><option value="left" selected>← ${t('automationPage.scrollLeft')}</option><option value="right">→ ${t('automationPage.scrollRight')}</option><option value="up">↑ ${t('automationPage.scrollUp')}</option><option value="down">↓ ${t('automationPage.scrollDown')}</option>`, 110)) +
                row('X', inp('action-led-x', 80, '', 'num', 'type="number" value="0" min="0" max="255"')) +
                row('Y', inp('action-led-y', 80, '', 'num', 'type="number" value="0" min="0" max="255"')) +
                row(t('automationPage.autoPos'), swc('action-led-auto-pos', true)) +
                row(t('ledPage.speed'), inp('action-led-speed', 80, '', 'num', 'type="number" value="50" min="1" max="100"')) +
                row(t('automationPage.loopScroll'), swc('action-led-loop', true));
            // 加载字体列表
            setTimeout(loadActionLedFonts, 100);
            break;
        case 'image':
            html = row(t('automationPage.imagePath'), `<span class="inl">${inp('action-led-image-path', 200, t('automationPage.imagePathPlaceholder'), 'mono', 'value="/sdcard/images/"')}<button type="button" class="btn icon sm" onclick="browseActionImages()" title="${t('common.browse')}" aria-label="${t('common.browse')}"><svg class="i"><use href="#ri-folder-line"/></svg></button>${varBtn('action-led-image-path')}</span>`, t('automationPage.imagePathHint')) +
                row(t('automationPage.centerDisplay'), swc('action-led-center', true));
            break;
        case 'qrcode':
            html = row(t('automationPage.qrContentLabel'), `<span class="inl">${inp('action-led-qr-text', 200, t('promptRepair.qrVariableHint'))}${varBtn('action-led-qr-text')}</span>`) +
                row(t('led.errorLevel'), sel('action-led-qr-ecc', '<option value="L">L - 7%</option><option value="M" selected>M - 15%</option><option value="Q">Q - 25%</option><option value="H">H - 30%</option>', 110)) +
                row(t('led.foregroundColor'), swatch('action-led-qr-fg', '#ffffff')) +
                row(t('automationPage.qrBgImage'), `<span class="inl">${inp('action-led-qr-bg', 160, t('common.none'), '', 'readonly')}<button type="button" class="btn icon sm" onclick="browseActionQrBg()" title="${t('common.browse')}" aria-label="${t('common.browse')}"><svg class="i"><use href="#ri-folder-line"/></svg></button><button type="button" class="btn icon sm" onclick="document.getElementById('action-led-qr-bg').value=''" title="${t('common.clear')}" aria-label="${t('common.clear')}"><svg class="i"><use href="#ri-close-line"/></svg></button></span>`);
            break;
        case 'filter': {
            const fk = ['pulse', 'breathing', 'blink', 'wave', 'scanline', 'glitch', 'rainbow', 'sparkle', 'plasma', 'sepia', 'posterize', 'contrast', 'invert'];
            const opts = fk.map(k => `<option value="${k}">${t('automationPage.filter' + k.charAt(0).toUpperCase() + k.slice(1))}</option>`).join('') + `<option value="grayscale">${t('ledPage.filterGrayscale')}</option>`;
            html = row(t('automationPage.filterLabel'), `<select id="action-led-filter" class="field" onchange="updateActionFilterParams()" style="width:190px">${opts}</select>`) +
                '<div class="rows" id="action-filter-params"></div>';
            // 初始化滤镜参数
            setTimeout(updateActionFilterParams, 50);
            break;
        }
    }
    
    paramsContainer.innerHTML = html;
    syncSliders();
}

/**
 * 加载动作 LED 字体列表
 */
async function loadActionLedFonts() {
    const fontSelect = document.getElementById('action-led-font');
    if (!fontSelect) return;
    
    try {
        const result = await api.storageList('/sdcard/fonts');
        const files = result.data?.entries || [];
        const fontExts = ['.fnt', '.bdf', '.pcf'];
        const fonts = files.filter(f => {
            if (f.type === 'dir' || f.type === 'directory') return false;
            const ext = f.name.toLowerCase().substring(f.name.lastIndexOf('.'));
            return fontExts.includes(ext);
        });
        
        fontSelect.innerHTML = '<option value="">' + (typeof t === 'function' ? t('ledPage.defaultFont') : '默认') + '</option>';
        fonts.forEach(f => {
            const option = document.createElement('option');
            const baseName = f.name.substring(0, f.name.lastIndexOf('.'));
            option.value = baseName;
            option.textContent = f.name;
            fontSelect.appendChild(option);
        });
    } catch (e) {
        console.error('加载字体失败:', e);
    }
}

/**
 * 设置动作 LED 颜色
 */
function setActionLedColor(color) {
    const picker = document.getElementById('action-led-color');
    if (picker) picker.value = color;
}

/**
 * 显示图像选择模态框
 * @param {string} title - 模态框标题
 * @param {function} onSelect - 选择回调，接收完整路径
 */
async function showImageSelectModal(title, onSelect) {
    const modal = document.createElement('div');
    modal.id = 'image-select-modal';
    modal.className = 'modal show';
    modal.onclick = (e) => { if (e.target === modal) closeModal('image-select-modal'); };

    modal.innerHTML = sheet(560, title, `
        <div class="card" style="padding:0;background:var(--fill)">
            <div class="tr" id="image-select-loading" style="--cols:1fr;border-top:0;color:var(--ink-3)">${t('common.loading')}</div>
            <div id="image-select-list" style="display:none;max-height:400px;overflow-y:auto"></div>
            <div class="tr" id="image-select-empty" style="display:none;--cols:1fr;border-top:0;color:var(--ink-3)">${t('automationPage.noImageFiles')} · ${t('automationPage.supportedFormats')}</div>
        </div>`,
        `<button class="btn lg" onclick="closeModal('image-select-modal')">${t('common.cancel')}</button>`);
    document.body.appendChild(modal);
    
    // 加载图像列表
    try {
        const result = await api.storageList('/sdcard/images');
        const files = result.data?.entries || [];
        const imageExts = ['.png', '.jpg', '.jpeg', '.bmp', '.gif'];
        const images = files.filter(f => {
            if (f.type === 'dir' || f.type === 'directory') return false;
            const ext = f.name.toLowerCase().substring(f.name.lastIndexOf('.'));
            return imageExts.includes(ext);
        });
        
        document.getElementById('image-select-loading').style.display = 'none';
        
        if (images.length === 0) {
            document.getElementById('image-select-empty').style.display = 'grid';
            return;
        }
        
        // 按名称排序
        images.sort((a, b) => a.name.localeCompare(b.name));
        
        const listEl = document.getElementById('image-select-list');
        listEl.style.display = 'block';
        listEl.innerHTML = images.map((img, i) => {
            const fullPath = `/sdcard/images/${img.name}`;
            const icon = img.name.toLowerCase().endsWith('.gif') ? 'ri-movie-line' : 'ri-image-line';
            return `<div class="tr image-select-item" data-path="${escapeHtml(fullPath)}" style="--cols:24px 1fr 90px;cursor:pointer${i === 0 ? ';border-top:0' : ''}" onclick="selectImageItem(this, this.dataset.path)"><div><svg class="i"><use href="#${icon}"/></svg></div><div>${escapeHtml(img.name)}</div><div>${formatFileSize(img.size)}</div></div>`;
        }).join('');
        
        // 存储回调
        window._imageSelectCallback = onSelect;
        
    } catch (e) {
        console.error('加载图像列表失败:', e);
        const ld = document.getElementById('image-select-loading');
        if (ld) ld.innerHTML = `<span class="form-error">${t('common.loadFailed')}: ${escapeHtml(e.message)}</span>`;
    }
}

/**
 * 选择图像项目
 */
function selectImageItem(el, path) {
    // 调用回调
    if (window._imageSelectCallback) {
        window._imageSelectCallback(path);
    }
    
    // 关闭模态框
    closeModal('image-select-modal');
}

/**
 * 浏览图像文件 (动作模板用)
 */
async function browseActionImages() {
    showImageSelectModal(t('dataSource.selectImageFile'), (path) => {
        document.getElementById('action-led-image-path').value = path;
    });
}

/**
 * 浏览 QR 背景图 (动作模板用)
 */
async function browseActionQrBg() {
    showImageSelectModal(t('dataSource.selectBgImage'), (path) => {
        document.getElementById('action-led-qr-bg').value = path;
    });
}

// 变量选择器（三处共用）：搜索框 + 分组折叠表格 + 关闭；cb 决定 selectVariable 的回调模式
function buildVarSelectModal(title, cb, emptyExtra = '') {
    const old = document.getElementById('variable-select-modal');
    if (old) old.remove();
    const modal = document.createElement('div');
    modal.id = 'variable-select-modal';
    modal.className = 'modal show';
    if (cb) modal.dataset.callback = cb;
    modal.onclick = (e) => { if (e.target === modal) closeModal('variable-select-modal'); };
    modal.innerHTML = sheet(660, title, `
        <input class="field" id="var-search" placeholder="${t('automation.searchVariable')}" aria-label="${t('automation.searchVariable')}" oninput="filterVariableList(this.value)" style="width:100%">
        <div class="card" style="padding:0;margin-top:12px;background:var(--fill)">
            <div class="tr" id="variable-select-loading" style="--cols:1fr;border-top:0;color:var(--ink-3)">${t('automation.loadingVariables')}</div>
            <div id="variable-select-list" style="display:none;max-height:400px;overflow-y:auto"></div>
            <div class="tr" id="variable-select-empty" style="display:none;--cols:1fr;border-top:0;color:var(--ink-3)">${t('automation.noVariablesAvailable')}${emptyExtra ? ' · ' + emptyExtra : ''}</div>
        </div>`,
        `<button class="btn lg primary" onclick="closeModal('variable-select-modal')">${t('common.close')}</button>`,
        "closeModal('variable-select-modal')");
    document.body.appendChild(modal);
    return modal;
}

async function loadVarSelectList(asExpr = false) {
    try {
        const result = await api.call('automation.variables.list', {
            include_value: false,
            include_meta: false
        });
        const variables = result.data?.variables || [];
        document.getElementById('variable-select-loading').style.display = 'none';
        if (variables.length === 0) {
            document.getElementById('variable-select-empty').style.display = 'grid';
            return;
        }
        const grouped = {};
        variables.forEach(v => {
            const sourceId = v.source_id || '_system';
            if (!grouped[sourceId]) grouped[sourceId] = [];
            grouped[sourceId].push(v);
        });
        const listEl = document.getElementById('variable-select-list');
        listEl.style.display = 'block';
        const cols = '--cols:1.4fr 1fr .8fr';
        let html = '';
        let first = true;
        for (const [sourceId, vars] of Object.entries(grouped)) {
            const groupId = `var-group-${sourceId.replace(/[^a-zA-Z0-9]/g, '_')}`;
            const safeSourceId = escapeHtml(sourceId);
            const sourceLabel = sourceId === '_system' ? t('automation.systemVariables') : sourceId;
            html += `<div class="var-group" data-source="${safeSourceId}"${first ? '' : ' style="border-top:1px solid var(--hair)"'}>
                <div class="dis var-group-header" style="font-size:13px;cursor:pointer" onclick="toggleVarGroup('${groupId}')">
                    <span class="var-group-arrow" id="${groupId}-arrow" style="display:inline-block;color:var(--ink-3)">›</span>
                    <span title="${vars.length}">${escapeHtml(sourceLabel)}</span>
                </div>
                <div class="var-group-items" id="${groupId}" hidden>
                    <div class="tr th" style="${cols}"><div>${t('common.variable')}</div><div>${t('sshPage.varTableValue')}</div><div>${t('common.type')}</div></div>`;
            vars.forEach(v => {
                const safeName = escapeHtml(v.name || '');
                const val = v.value !== undefined ? String(v.value).substring(0, 30) + (String(v.value).length > 30 ? '...' : '') : '-';
                html += `<div class="tr var-select-item" data-name="${safeName}" data-source="${safeSourceId}" style="${cols};cursor:pointer" onclick="selectVariable(this.dataset.name)">
                        <div><span class="mono">${asExpr ? '\${' + safeName + '}' : safeName}</span></div>
                        <div>${escapeHtml(val)}</div>
                        <div>${escapeHtml(v.type || '-')}</div>
                    </div>`;
            });
            html += '</div></div>';
            first = false;
        }
        listEl.innerHTML = html;
        setTimeout(() => document.getElementById('var-search')?.focus(), 100);
    } catch (e) {
        console.error('加载变量列表失败:', e);
        const ld = document.getElementById('variable-select-loading');
        if (ld) ld.innerHTML = `<span class="form-error">${t('common.loadFailed')}: ${escapeHtml(e.message)}</span>`;
    }
}

/**
 * 显示变量选择模态框
 * @param {string} targetInputId - 目标输入框 ID
 * @param {string} mode - 'insert' 插入 ${var} 或 'replace' 替换整个值
 */
async function showVariableSelectModal(targetInputId, mode = 'insert') {
    buildVarSelectModal(t('automation.selectVariableTitle'), '', t('automationPage.configSourceFirst'));
    // 保存目标信息
    window._varSelectTarget = { inputId: targetInputId, mode: mode };
    await loadVarSelectList(true);
}

/**
 * 切换变量分组的折叠状态
 */
function toggleVarGroup(groupId) {
    const itemsEl = document.getElementById(groupId);
    const arrowEl = document.getElementById(groupId + '-arrow');
    if (!itemsEl) return;
    itemsEl.hidden = !itemsEl.hidden;
    if (arrowEl) arrowEl.style.transform = itemsEl.hidden ? '' : 'rotate(90deg)';
}

/**
 * 过滤变量列表
 */
function filterVariableList(keyword) {
    const kw = keyword.toLowerCase();
    document.querySelectorAll('.var-select-item').forEach(item => {
        item.hidden = !!kw && !item.dataset.name.toLowerCase().includes(kw);
    });
    document.querySelectorAll('.var-group').forEach(group => {
        const itemsContainer = group.querySelector('.var-group-items');
        const arrow = group.querySelector('.var-group-arrow');
        const any = !!group.querySelector('.var-select-item:not([hidden])');
        // 有关键词：展开有匹配的分组、隐藏无匹配的；无关键词：全部显示并折叠
        group.hidden = !!kw && !any;
        if (itemsContainer) itemsContainer.hidden = !kw;
        if (arrow) arrow.style.transform = kw ? 'rotate(90deg)' : '';
    });
}

/**
 * 选择变量
 */
function selectVariable(varName) {
    const varSelectModal = document.getElementById('variable-select-modal');
    
    // 检查是否是数据组件表达式编辑回调模式
    if (varSelectModal && varSelectModal.dataset.callback === 'widgetExpression') {
        const input = document.getElementById('edit-expression');
        if (input) {
            const curVal = input.value || '';
            // 在光标位置插入变量引用
            const start = input.selectionStart || curVal.length;
            const end = input.selectionEnd || curVal.length;
            const text = `\${${varName}}`;
            input.value = curVal.substring(0, start) + text + curVal.substring(end);
            input.focus();
            input.selectionStart = input.selectionEnd = start + text.length;
        }
        closeModal('variable-select-modal');
        delete varSelectModal.dataset.callback;
        return;
    }
    
    // 检查是否是数据组件绑定回调模式
    if (varSelectModal && varSelectModal.dataset.callback === 'widgetBind') {
        const widgetId = varSelectModal.dataset.widgetId;
        if (widgetId) {
            const widget = dataWidgets.find(w => w.id === widgetId);
            if (widget) {
                widget.variable = varName;
                saveDataWidgets();
                renderDataWidgets();
                refreshDataWidgets();
                showToast(typeof t === 'function' ? t('toast.widgetBound', { widget: widget.label, var: varName }) : `已绑定 ${widget.label} → ${varName}`, 'success');
            }
        }
        closeModal('variable-select-modal');
        return;
    }
    
    // 检查是否是数据组件编辑模态框回调模式
    if (varSelectModal && varSelectModal.dataset.callback === 'widgetEdit') {
        const input = document.getElementById('edit-widget-var');
        if (input) input.value = varName;
        closeModal('variable-select-modal');
        return;
    }
    
    // 检查是否是动作条件回调模式
    if (varSelectModal && varSelectModal.dataset.callback === 'actionCondition') {
        handleActionConditionVarSelect(varName);
        delete varSelectModal.dataset.callback;
        return;
    }
    
    // 检查是否是触发条件回调模式
    if (varSelectModal && varSelectModal.dataset.callback === 'ruleCondition') {
        handleConditionVarSelect(varName);
        delete varSelectModal.dataset.callback;
        return;
    }
    
    const target = window._varSelectTarget;
    if (!target) return;
    
    const input = document.getElementById(target.inputId);
    if (!input) return;
    
    if (target.mode === 'replace') {
        // 替换整个值
        input.value = `\${${varName}}`;
    } else {
        // 在光标位置插入
        const start = input.selectionStart || input.value.length;
        const end = input.selectionEnd || input.value.length;
        const text = `\${${varName}}`;
        input.value = input.value.substring(0, start) + text + input.value.substring(end);
        // 移动光标到插入文本之后
        input.selectionStart = input.selectionEnd = start + text.length;
    }
    
    input.focus();
    closeModal('variable-select-modal');
}

/**
 * 更新滤镜参数控件 (动作模板用)
 */
function updateActionFilterParams() {
    const filterSelect = document.getElementById('action-led-filter');
    const paramsContainer = document.getElementById('action-filter-params');
    if (!filterSelect || !paramsContainer) return;
    
    const filter = filterSelect.value;
    const config = filterConfig[filter];
    const modal = document.getElementById('action-modal');
    const original = modal?.dataset.edit === '1' && modal.dataset.originalFilter === filter;
    const edits = modal?._filterParamEdits?.get(filter);
    
    if (!config || !config.params || config.params.length === 0) {
        paramsContainer.innerHTML = row(t('common.params'), `<span class="t-note">${t('automationPage.noExtraParams')}</span>`);
        return;
    }
    
    let html = '';
    config.params.forEach(param => {
        const paramInfo = paramLabels[param];
        if (!paramInfo) return;
        
        const stored = original && Object.hasOwn(modal._originalFilterParams || {}, param);
        const custom = edits?.has(param) || stored || !original;
        const defaultValue = edits?.get(param) ?? (stored ? modal._originalFilterParams[param] : config.defaults[param] ?? 50);
        const unitTxt = paramInfo.unit || '';
        html += row(paramInfo.label,
            ledSlider(`action-filter-${param}`, paramInfo.min, paramInfo.max, defaultValue, `document.getElementById('action-filter-${param}-val').textContent=this.value+'${unitTxt}'`) +
            ledVal(`action-filter-${param}-val`, custom ? defaultValue + unitTxt : t('inputRepair.defaultParam')));
    });
    
    paramsContainer.innerHTML = html;
    syncSliders();
}

/**
 * 测试动作
 */
async function testAction(id) {
    try {
        showToast(typeof t === 'function' ? t('toast.actionExecuting', { id }) : `正在执行动作: ${id}...`, 'info');
        const result = await api.call('automation.actions.execute', { id });
        console.log('Action execute result:', result);
        
        if (result.code === 0) {
            let msg = t('toast.actionSuccess');
            if (result.data?.output) {
                msg += '\n' + t('promptRepair.outputPreview', {output: result.data.output.substring(0, 100)});
            }
            showToast(msg, 'success');
        } else {
            showToast(typeof t === 'function' ? t('toast.actionFailed', { id }) + ': ' + (result.message || t('common.unknown')) : `动作 ${id} 失败: ${result.message || '未知错误'}`, 'error');
        }
    } catch (error) {
        console.error('Action execute error:', error);
        showToast(typeof t === 'function' ? t('toast.actionExecuteFailed') + ': ' + error.message : `动作执行失败: ${error.message}`, 'error');
    }
}

/**
 * 编辑动作
 */
async function editAction(id) {
    try {
        const result = await api.call('automation.actions.get', { id });
        if (result.code !== 0) {
            showToast(typeof t === 'function' ? t('toast.getActionFailed') + ': ' + result.message : `获取动作详情失败: ${result.message}`, 'error');
            return;
        }
        
        const tpl = result.data;
        
        // 打开添加对话框并填充数据
        await showAddActionModal();
        document.getElementById('action-modal').dataset.enabled = String(tpl.enabled ?? true);
        document.getElementById('action-modal').dataset.edit = '1';
        const modal = document.getElementById('action-modal');
        modal.dataset.originalFilter = tpl.led?.ctrl_type === 'filter' ? tpl.led.filter : '';
        modal._originalFilterParams = tpl.led?.filter_params === undefined ? undefined : { ...tpl.led.filter_params };
        modal._filterParamEdits = new Map();
        const markFilterChange = event => {
            if (event.target.id?.startsWith('action-filter-')) {
                const filter = document.getElementById('action-led-filter')?.value;
                const key = event.target.id.slice('action-filter-'.length);
                if (!filterConfig[filter]?.params.includes(key)) return;
                if (!modal._filterParamEdits.has(filter)) modal._filterParamEdits.set(filter, new Map());
                modal._filterParamEdits.get(filter).set(key, event.target.value);
            }
        };
        modal.addEventListener('input', markFilterChange);
        modal.addEventListener('change', markFilterChange);
        
        // 等待 DOM 更新
        await new Promise(r => setTimeout(r, 100));
        
        // 填充基本信息
        document.getElementById('action-id').value = tpl.id;
        document.getElementById('action-id').disabled = true; // ID 不可编辑
        document.getElementById('action-name').value = tpl.name || '';
        document.getElementById('action-description').value = tpl.description || '';
        document.getElementById('action-delay').value = tpl.delay_ms || 0;
        document.getElementById('action-async').checked = tpl.async || false;
        
        // 选择类型
        const typeRadio = document.querySelector(`input[name="action-type"][value="${tpl.type}"]`);
        if (typeRadio) {
            typeRadio.checked = true;
            await updateActionTypeFields();
            await new Promise(r => setTimeout(r, 100));
            
            // 根据类型填充字段
            switch (tpl.type) {
                case 'cli':
                    if (tpl.cli) {
                        document.getElementById('action-cli-command').value = tpl.cli.command || '';
                        document.getElementById('action-cli-var').value = tpl.cli.var_name || '';
                        document.getElementById('action-cli-timeout').value = tpl.cli.timeout_ms || 5000;
                    }
                    break;
                case 'ssh_cmd_ref':
                    if (tpl.ssh_ref) {
                        await loadSshCommandsForAction();
                        await new Promise(r => setTimeout(r, 100));
                        document.getElementById('action-ssh-cmd-id').value = tpl.ssh_ref.cmd_id || '';
                        updateSshCmdRefPreview();
                    }
                    break;
                case 'led':
                    if (tpl.led) {
                        // 先等待设备列表加载完成
                        await loadLedDevicesForAction();
                        await new Promise(r => setTimeout(r, 100));
                        
                        // 设置设备
                        const deviceEl = document.getElementById('action-led-device');
                        if (deviceEl) {
                            deviceEl.value = tpl.led.device || 'board';
                        }
                        updateActionLedOptions();
                        await new Promise(r => setTimeout(r, 100));
                        
                        // 设置控制类型
                        const isMatrix = tpl.led.device === 'matrix';
                        const ctrlTypeEl = isMatrix 
                            ? document.getElementById('action-led-matrix-type')
                            : document.getElementById('action-led-type');
                        if (ctrlTypeEl && tpl.led.ctrl_type) {
                            ctrlTypeEl.value = tpl.led.ctrl_type;
                            updateActionLedTypeFields();
                            // 等待字段渲染和异步加载（如字体列表）
                            await new Promise(r => setTimeout(r, 300));
                        }
                        
                        // 根据控制类型填充对应字段
                        switch (tpl.led.ctrl_type) {
                            case 'fill':
                                if (tpl.led.color) {
                                    const colorEl = document.getElementById('action-led-color');
                                    if (colorEl) colorEl.value = tpl.led.color;
                                }
                                if (tpl.led.brightness !== undefined) {
                                    const brEl = document.getElementById('action-led-brightness');
                                    if (brEl) brEl.value = tpl.led.brightness;
                                    const brVal = document.getElementById('action-led-brightness-val');
                                    if (brVal) brVal.textContent = tpl.led.brightness;
                                }
                                if (tpl.led.index !== undefined && tpl.led.index !== 255) {
                                    const idxEl = document.getElementById('action-led-index');
                                    if (idxEl) idxEl.value = tpl.led.index;
                                }
                                break;
                            case 'effect':
                                if (tpl.led.effect) {
                                    const effectEl = document.getElementById('action-led-effect');
                                    if (effectEl) effectEl.value = tpl.led.effect;
                                }
                                if (tpl.led.color) {
                                    const colorEl = document.getElementById('action-led-color');
                                    if (colorEl) colorEl.value = tpl.led.color;
                                }
                                if (tpl.led.speed !== undefined) {
                                    const speedEl = document.getElementById('action-led-speed');
                                    if (speedEl) speedEl.value = tpl.led.speed;
                                }
                                break;
                            case 'brightness':
                                if (tpl.led.brightness !== undefined) {
                                    const brEl = document.getElementById('action-led-brightness');
                                    if (brEl) brEl.value = tpl.led.brightness;
                                    const brVal = document.getElementById('action-led-brightness-val');
                                    if (brVal) brVal.textContent = tpl.led.brightness;
                                }
                                break;
                            case 'text':
                                if (tpl.led.text) {
                                    const textEl = document.getElementById('action-led-text');
                                    if (textEl) textEl.value = tpl.led.text;
                                }
                                if (tpl.led.font) {
                                    // 字体列表可能还在加载，设置一个延迟重试
                                    const setFont = () => {
                                        const fontEl = document.getElementById('action-led-font');
                                        if (fontEl) {
                                            fontEl.value = tpl.led.font;
                                            // 如果字体选项还没加载，等待后重试
                                            if (fontEl.value !== tpl.led.font && fontEl.options.length <= 1) {
                                                setTimeout(setFont, 200);
                                            }
                                        }
                                    };
                                    setFont();
                                }
                                if (tpl.led.color) {
                                    const colorEl = document.getElementById('action-led-color');
                                    if (colorEl) colorEl.value = tpl.led.color;
                                }
                                if (tpl.led.scroll) {
                                    const scrollEl = document.getElementById('action-led-scroll');
                                    if (scrollEl) scrollEl.value = tpl.led.scroll;
                                }
                                if (tpl.led.speed !== undefined) {
                                    const speedEl = document.getElementById('action-led-speed');
                                    if (speedEl) speedEl.value = tpl.led.speed;
                                }
                                if (tpl.led.loop !== undefined) {
                                    const loopEl = document.getElementById('action-led-loop');
                                    if (loopEl) loopEl.checked = tpl.led.loop;
                                }
                                if (tpl.led.align) {
                                    const alignEl = document.getElementById('action-led-align');
                                    if (alignEl) alignEl.value = tpl.led.align;
                                }
                                break;
                            case 'image':
                                if (tpl.led.image_path) {
                                    const imgEl = document.getElementById('action-led-image-path');
                                    if (imgEl) imgEl.value = tpl.led.image_path;
                                }
                                if (tpl.led.center !== undefined) {
                                    const centerEl = document.getElementById('action-led-center');
                                    if (centerEl) centerEl.checked = tpl.led.center;
                                }
                                break;
                            case 'qrcode':
                                if (tpl.led.qr_text) {
                                    const qrEl = document.getElementById('action-led-qr-text');
                                    if (qrEl) qrEl.value = tpl.led.qr_text;
                                }
                                if (tpl.led.qr_ecc) {
                                    const eccEl = document.getElementById('action-led-qr-ecc');
                                    if (eccEl) eccEl.value = tpl.led.qr_ecc;
                                }
                                if (tpl.led.color) {
                                    const fgEl = document.getElementById('action-led-qr-fg');
                                    if (fgEl) fgEl.value = tpl.led.color;
                                }
                                break;
                            case 'filter':
                                if (tpl.led.filter) {
                                    const filterEl = document.getElementById('action-led-filter');
                                    if (filterEl) filterEl.value = tpl.led.filter;
                                    updateActionFilterParams();
                                    if (tpl.led.filter_params === undefined) {
                                        const hint = document.createElement('p');
                                        hint.className = 't-note';
                                        hint.textContent = t('inputRepair.defaultFilter');
                                        document.getElementById('action-filter-params').prepend(hint);
                                    }
                                }
                                break;
                        }
                    }
                    break;
                case 'log':
                    if (tpl.log) {
                        document.getElementById('action-log-level').value = tpl.log.level || 3;
                        document.getElementById('action-log-message').value = tpl.log.message || '';
                    }
                    break;
                case 'set_var':
                    if (tpl.set_var) {
                        document.getElementById('action-var-name').value = tpl.set_var.variable || '';
                        document.getElementById('action-var-value').value = tpl.set_var.value || '';
                    }
                    break;
                case 'webhook':
                    if (tpl.webhook) {
                        document.getElementById('action-webhook-url').value = tpl.webhook.url || '';
                        document.getElementById('action-webhook-method').value = tpl.webhook.method || 'POST';
                        document.getElementById('action-webhook-body').value = tpl.webhook.body_template || '';
                    }
                    break;
            }
        }
        
        // 更改模态框标题和按钮
        syncSliders();
        
        const submitBtn = document.querySelector('#action-modal button[onclick="submitAction()"]');
        if (submitBtn) {
            submitBtn.textContent = typeof t === 'function' ? t('ui.updateAction') : '更新';
            submitBtn.setAttribute('onclick', `updateAction('${tpl.id}')`);
        }
        
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.editActionFailed') + ': ' + error.message : `编辑动作失败: ${error.message}`, 'error');
    }
}

/**
 * 更新动作模板
 */
async function updateAction(originalId) {
    await submitAction(originalId);
}

/**
 * 删除动作
 */
async function deleteAction(id) {
    const key = 'action:' + id;
    if (configurationDeletesInFlight.has(key)) return;
    configurationDeletesInFlight.add(key);
    const pageCurrent = capturePageValidity();
    try {
        if (!await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteAction', { id }) : `确定要删除动作模板 "${id}" 吗？`, { primary: t('common.delete'), tone: 'danger' }) || !pageCurrent()) return;
        showToast(t('toast.processing'), 'info');
        const template = requireApiSuccess(await api.call('automation.actions.get', { id }), 'automation.actions.get').data;
        if (!pageCurrent()) return;
        if (template?.type === 'ssh_cmd_ref' && template.ssh_ref?.cmd_id &&
            !await verifyStoppedServiceForDelete(template.ssh_ref.cmd_id, pageCurrent)) return;
        if (!pageCurrent()) return;
        const result = await api.call('automation.actions.delete', { id });
        if (!pageCurrent()) return;
        showToast(typeof t === 'function' ? t('toast.deleteActionResult', { id }) + ': ' + (result.message || 'OK') : `删除动作 ${id}: ${result.message || 'OK'}`, result.code === 0 ? 'success' : 'error');
        if (result.code === 0) await refreshActions();
    } catch (error) {
        if (pageCurrent()) showToast(typeof t === 'function' ? t('toast.deleteFailedMsg', { msg: error.message }) : `删除失败: ${error.message}`, 'error');
    } finally {
        configurationDeletesInFlight.delete(key);
    }
}

/**
 * 切换数据源启用状态
 */
async function toggleSource(id, enable) {
    try {
        const action = enable ? 'automation.sources.enable' : 'automation.sources.disable';
        const result = await api.call(action, { id });
        showToast(typeof t === 'function' ? t('toast.sourceToggled', { id, state: enable ? t('status.enabled') : t('status.disabled') }) + ': ' + (result.message || 'OK') : `数据源 ${id} ${enable ? '启用' : '禁用'}: ${result.message || 'OK'}`, result.code === 0 ? 'success' : 'error');
        if (result.code === 0) {
            await refreshSources();
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.toggleSourceFailed') + ': ' + error.message : `切换数据源状态失败: ${error.message}`, 'error');
    }
}

/**
 * 删除数据源
 */
async function deleteSource(id) {
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteSource', { id }) : `确定要删除数据源 "${id}" 吗？此操作不可撤销。`, { primary: t('common.delete'), tone: 'danger' })) {
        return;
    }
    
    try {
        const result = await api.call('automation.sources.delete', { id });
        showToast(typeof t === 'function' ? t('toast.deleteSourceResult', { id }) + ': ' + (result.message || 'OK') : `删除数据源 ${id}: ${result.message || 'OK'}`, result.code === 0 ? 'success' : 'error');
        if (result.code === 0) {
            await Promise.all([refreshSources(), refreshAutomationStatus()]);
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.deleteSourceFailed') + ': ' + error.message : `删除数据源失败: ${error.message}`, 'error');
    }
}

/**
 * 显示数据源的变量列表
 */
async function showSourceVariables(sourceId) {
    const body = openSourceVarsSheet(t('ui.variablesTitle', { name: escapeHtml(sourceId) }));
    if (!body) return;
    
    try {
        const result = await api.call('automation.variables.list', {
            source_id: sourceId,
            include_meta: true
        });
        if (result.code === 0 && result.data && result.data.variables) {
            const vars = result.data.variables.filter(v => v.source_id === sourceId);
            
            if (vars.length === 0) {
                setSourceVarsMessage(body, t('automationPage.sourceNoData'));
                return;
            }
            renderSourceVarsTable(body, vars);
        } else {
            setSourceVarsMessage(body, escapeHtml(result.message || t('sshPage.getVarFailed')), true);
        }
    } catch (error) {
        setSourceVarsMessage(body, escapeHtml(error.message), true);
    }
}

/**
 * 关闭数据源变量模态框
 */
function closeSourceVariablesModal() {
    const modal = document.getElementById('source-variables-modal');
    if (modal) modal.classList.add('hidden');
}

// =========================================================================
//                         关机设置模态框
// =========================================================================

/**
 * 显示关机设置模态框
 */
async function showShutdownSettingsModal() {
    const modal = document.getElementById('shutdown-settings-modal');
    if (!modal) return;
    
    const num = (id, ph, extra) => inp(id, 90, ph, 'num', `type="number" ${extra}`);
    modal.innerHTML = sheet(600, t('system.shutdownSettings'),
        grp(
            row(t('system.lowVoltageLabel'), num('ss-low-voltage', '12.6', 'step="0.1" min="10" max="24"') + unit('V'), t('system.lowVoltageNote')) +
            row(t('system.recoveryVoltageLabel'), num('ss-recovery-voltage', '18.0', 'step="0.1" min="10" max="30"') + unit('V'), '', t('system.recoveryVoltageThresholdHint')) +
            row(t('system.shutdownDelayLabel'), num('ss-shutdown-delay', '60', 'step="1" min="10" max="600"') + unit(t('common.seconds')), t('system.shutdownDelayNote')) +
            row(t('system.recoveryHoldLabel'), num('ss-recovery-hold', '5', 'step="1" min="1" max="300"') + unit(t('common.seconds')), '', t('system.recoveryHoldHint')) +
            row(t('system.fanStopDelayLabel'), num('ss-fan-stop-delay', '60', 'step="1" min="10" max="600"') + unit(t('common.seconds')), '', t('system.fanStopDelayHint'))) +
        '<div id="shutdown-settings-error" class="form-error hidden"></div>',
        `<button class="btn lg" onclick="resetShutdownSettings()">${t('system.restoreDefaults')}</button><button class="btn lg" onclick="closeShutdownSettingsModal()">${t('common.cancel')}</button><button class="btn lg primary" onclick="saveShutdownSettings()">${t('common.save')}</button>`);
    
    // 显示模态框
    modal.classList.remove('hidden');
    
    // 加载当前配置
    try {
        const result = await api.powerProtectionConfig();
        if (result.code === 0 && result.data) {
            const config = result.data;
            document.getElementById('ss-low-voltage').value = config.low_voltage_threshold || 12.6;
            document.getElementById('ss-recovery-voltage').value = config.recovery_voltage_threshold || 18.0;
            document.getElementById('ss-shutdown-delay').value = config.shutdown_delay_sec || 60;
            document.getElementById('ss-recovery-hold').value = config.recovery_hold_sec || 5;
            document.getElementById('ss-fan-stop-delay').value = config.fan_stop_delay_sec || 60;
        }
    } catch (e) {
        console.error('Failed to load shutdown settings:', e);
        // 使用默认值
        document.getElementById('ss-low-voltage').value = 12.6;
        document.getElementById('ss-recovery-voltage').value = 18.0;
        document.getElementById('ss-shutdown-delay').value = 60;
        document.getElementById('ss-recovery-hold').value = 5;
        document.getElementById('ss-fan-stop-delay').value = 60;
    }
}

/**
 * 关闭关机设置模态框
 */
function closeShutdownSettingsModal() {
    const modal = document.getElementById('shutdown-settings-modal');
    if (modal) modal.classList.add('hidden');
}

/**
 * 保存关机设置
 */
async function saveShutdownSettings() {
    const errorDiv = document.getElementById('shutdown-settings-error');
    
    const config = {
        low_threshold: parseFloat(document.getElementById('ss-low-voltage').value),
        recovery_threshold: parseFloat(document.getElementById('ss-recovery-voltage').value),
        shutdown_delay: parseInt(document.getElementById('ss-shutdown-delay').value),
        recovery_hold: parseInt(document.getElementById('ss-recovery-hold').value),
        fan_stop_delay: parseInt(document.getElementById('ss-fan-stop-delay').value),
        persist: true  // 标记需要持久化
    };
    
    // 验证
    if (config.low_threshold >= config.recovery_threshold) {
        errorDiv.textContent = typeof t === 'function' ? t('ui.lowVoltageError') : '低电压阈值必须小于恢复电压阈值';
        errorDiv.classList.remove('hidden');
        return;
    }
    
    if (config.shutdown_delay < 10 || config.shutdown_delay > 600) {
        errorDiv.textContent = typeof t === 'function' ? t('ui.shutdownDelayError') : '关机倒计时必须在 10-600 秒之间';
        errorDiv.classList.remove('hidden');
        return;
    }
    
    try {
        const result = await api.powerProtectionSet(config);
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.shutdownSettingsSaved') : '关机设置已保存', 'success');
            closeShutdownSettingsModal();
        } else {
            errorDiv.textContent = result.message || t('errors.saveFailed');
            errorDiv.classList.remove('hidden');
        }
    } catch (e) {
        errorDiv.textContent = typeof t === 'function' ? t('toast.saveFailedMsg', { msg: e.message }) : '保存失败: ' + e.message;
        errorDiv.classList.remove('hidden');
    }
}

/**
 * 恢复默认关机设置
 */
async function resetShutdownSettings() {
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmRestoreDefaults') : '确认恢复默认设置？', { primary: t('system.restoreDefaults'), tone: 'neutral' })) return;
    
    const config = {
        low_threshold: 12.6,
        recovery_threshold: 18.0,
        shutdown_delay: 60,
        recovery_hold: 5,
        fan_stop_delay: 60,
        persist: true
    };
    
    try {
        const result = await api.powerProtectionSet(config);
        if (result.code === 0) {
            // 更新界面
            document.getElementById('ss-low-voltage').value = 12.6;
            document.getElementById('ss-recovery-voltage').value = 18.0;
            document.getElementById('ss-shutdown-delay').value = 60;
            document.getElementById('ss-recovery-hold').value = 5;
            document.getElementById('ss-fan-stop-delay').value = 60;
            showToast(typeof t === 'function' ? t('toast.defaultsRestored') : '已恢复默认设置', 'success');
        } else {
            showToast(typeof t === 'function' ? t('toast.restoreFailedMsg', { msg: result.message || t('common.unknown') }) : '恢复失败: ' + (result.message || '未知错误'), 'error');
        }
    } catch (e) {
        showToast(typeof t === 'function' ? t('toast.restoreFailedMsg', { msg: e.message }) : '恢复失败: ' + e.message, 'error');
    }
}

/**
 * 删除规则
 */
async function deleteRule(id) {
    if (!await confirmAction(typeof t === 'function' ? t('ui.confirmDeleteRule', { id }) : `确定要删除规则 "${id}" 吗？此操作不可撤销。`, { primary: t('common.delete'), tone: 'danger' })) {
        return;
    }
    
    try {
        const result = await ruleWriteWithRevision('automation.rules.delete', id);
        showToast(typeof t === 'function' ? t('toast.deleteRuleResult', { id }) + ': ' + (result.message || 'OK') : `删除规则 ${id}: ${result.message || 'OK'}`, result.code === 0 ? 'success' : 'error');
        if (result.code === 0) {
            await Promise.all([refreshRules(), refreshAutomationStatus()]);
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.deleteRuleFailed') + ': ' + error.message : `删除规则失败: ${error.message}`, 'error');
    }
}

/**
 * 编辑规则
 */
async function editRule(id) {
    try {
        // 获取规则详情
        const result = await api.call('automation.rules.get', { id });
        if (result.code !== 0 || !result.data) {
            showToast(typeof t === 'function' ? t('toast.getRuleDetailFailed') + ': ' + (result.message || t('common.unknown')) : `获取规则详情失败: ${result.message || '未知错误'}`, 'error');
            return;
        }
        
        // 打开编辑模态框
        showAddRuleModal(result.data);
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.getRuleDetailFailed') + ': ' + error.message : `获取规则详情失败: ${error.message}`, 'error');
    }
}

/**
 * 显示添加数据源模态框
 */
function showAddSourceModal() {
    // 移除可能存在的旧模态框
    const oldModal = document.getElementById('add-source-modal');
    if (oldModal) oldModal.remove();

    const modal = document.createElement('div');
    modal.id = 'add-source-modal';
    modal.className = 'modal';
    const typeName = { rest: 'REST API', websocket: 'WebSocket', socketio: 'Socket.IO', variable: t('automation.variableTab') };
    const testRow = (label, inputId, ph, btnId, fn, cls = 'mono') => `<div class="fl"><label>${label}</label><div style="display:flex;gap:8px"><input class="field ${cls}" style="flex:1" id="${inputId}" placeholder="${ph}" aria-label="${label}"><button class="btn" id="${btnId}" onclick="${fn}()">${t('automation.testBtn')}</button></div></div>`;
    const testPanel = (pre, toggle) => `<div id="${pre}-test-result" style="display:none"><div style="margin:12px 0 4px"><span class="test-status"></span> <span class="t-note" role="button" style="cursor:pointer" onclick="${toggle}()">${t('sshPage.rawData')} ▸</span></div><pre id="${pre}-json-preview" class="term" style="display:none;height:160px;overflow:auto;font-size:12px;line-height:18px;margin:8px 0 0;white-space:pre-wrap"></pre><div id="${pre}-var-selector"><div class="var-list"></div></div></div>`;
    const num = (id, w, val, attrs) => inp(id, w, '', 'num', `type="number" value="${val}" ${attrs}`);
    const cfgTitle = txt => `<div class="gt" style="margin-top:16px">${txt}</div>`;
    modal.innerHTML = sheet(660, `<span id="add-source-title">${t('automation.addSourceTitle')} · REST API</span>`, `
        <div class="seg full" id="source-type-tabs" style="display:flex">
            <button type="button" class="on" data-type="rest" onclick="switchSourceType('rest')">${t('automation.restTab')}</button>
            <button type="button" data-type="websocket" onclick="switchSourceType('websocket')">${t('automation.wsTab')}</button>
            <button type="button" data-type="socketio" onclick="switchSourceType('socketio')">${t('automation.sioTab')}</button>
            <button type="button" data-type="variable" onclick="switchSourceType('variable')">${t('automation.variableTab')}</button>
        </div>
        <input type="hidden" id="source-type" value="rest">
        <div style="height:14px"></div>
        <div style="display:grid;grid-template-columns:1fr 1fr;gap:14px;align-items:end">
            <div class="fl"><label>${t('automation.sourceId')}</label><input class="field" id="source-id" placeholder="${t('automation.sourceIdPlaceholder')}" aria-label="${t('automation.sourceId')}"></div>
            <div class="fl"><label>${t('automation.sourceLabel')}</label><input class="field" id="source-label" placeholder="${t('automation.sourceLabelPlaceholder')}" aria-label="${t('automation.sourceLabel')}"></div>
        </div>
        <div id="source-rest-config">
            ${cfgTitle(t('automation.restConfigTitle'))}
            ${testRow(t('automation.requestUrlReq'), 'source-rest-url', 'http://192.168.1.100/api/status', 'btn-test-rest', 'testRestConnection')}
            ${testPanel('rest', 'toggleJsonPreview')}
            ${gt(t('common.params'))}
            ${grp(
                row(t('sshPage.method'), `<select id="source-rest-method" class="field" style="width:100px"><option value="GET">GET</option><option value="POST">POST</option></select>`) +
                row(t('automation.pollIntervalMs'), num('source-interval', 100, 5000, 'min="500"')) +
                row(t('automation.authHeaderShort'), inp('source-rest-auth', 200, t('automation.authPlaceholder')), t('automation.optionalTag')) +
                row(t('automation.jsonPathLabel'), inp('source-rest-path', 200, 'data.temperature', 'mono'), t('automation.jsonPathNoteRest')))}
        </div>
        <div id="source-websocket-config" style="display:none">
            ${cfgTitle(t('automation.wsConfigTitle'))}
            ${testRow(t('automation.wsAddressReq'), 'source-ws-uri', t('automation.wsPlaceholder'), 'btn-test-ws', 'testWsConnection')}
            ${testPanel('ws', 'toggleWsJsonPreview')}
            ${gt(t('common.params'))}
            ${grp(
                row(t('automation.jsonPathLabel'), inp('source-ws-path', 200, 'data.temperature', 'mono'), t('automation.jsonPathNoteWs')) +
                row(t('automation.reconnectIntervalMs'), num('source-ws-reconnect', 100, 5000, 'min="1000"')))}
        </div>
        <div id="source-socketio-config" style="display:none">
            ${cfgTitle(t('automation.sioConfigTitleFull'))}
            ${testRow(t('automation.sioServerReq'), 'source-sio-url', t('automation.sioPlaceholder'), 'btn-test-sio', 'testSioConnection')}
            ${testPanel('sio', 'toggleSioJsonPreview')}
            ${gt(t('common.params'))}
            ${grp(
                row(t('automation.eventNameLabel'), inp('source-sio-event', 200, t('automation.eventNameShort')), t('automation.eventNamePlaceholder')) +
                row(t('automation.timeoutMs'), num('source-sio-timeout', 100, 15000, 'min="5000"')) +
                row(t('automation.jsonPathLabel'), inp('source-sio-path', 200, 'cpu.avg_usage', 'mono'), t('automation.jsonPathNoteSio')) +
                row(t('automation.autoDiscoverShort'), swc('source-sio-auto-discover', true), t('automation.autoDiscoverHint')))}
        </div>
        <div id="source-variable-config" class="gw" style="display:none">
            <div class="gt">${t('automation.variableConfigTitle')}</div>
            ${grp(
                row(t('automation.sshHostReq'), `<select id="source-ssh-host" class="field" style="width:190px" onchange="onSshHostChangeForSource()"><option value="">-- ${t('common.loading')} --</option></select>`, t('automation.sshHostNote')) +
                row(t('automation.cmdRow'), `<select id="source-ssh-cmd" class="field" style="width:190px" onchange="onSshCmdChange()"><option value="">${t('automation.selectHostFirst')}</option></select>`, t('automation.cmdRowNote')) +
                row(t('automation.pollIntervalSec'), num('source-var-interval', 80, 5, 'min="1" max="3600"'), t('automation.pollIntervalHint')))}
            <div id="source-ssh-cmd-preview" class="gw" style="display:none">
                ${gt(t('automation.cmdDetailTitle'))}
                ${grp(
                    row(t('common.command'), '<span class="mono t-label" id="preview-command">-</span>') +
                    row(t('common.description'), '<span class="t-label" id="preview-desc">-</span>') +
                    row(t('automation.timeoutLabel').replace(/[:：]\s*$/, ''), `<span class="t-label"><span id="preview-timeout">30</span> ${t('automation.seconds')}</span>`))}
            </div>
            <div class="t-note" style="margin-top:10px">${t('automation.varsPreviewTitle')} <span id="ssh-vars-list" class="ssh-vars-list"><span class="t-note">${t('automation.selectHostCmdFirst')}</span></span></div>
        </div>`,
        `<span class="inl" style="margin-right:auto;gap:8px"><input type="checkbox" class="switch" role="switch" id="source-enabled" checked><span class="t-body">${t('automation.enableAfterCreate')}</span></span><button class="btn lg" onclick="closeModal('add-source-modal')">${t('common.cancel')}</button><button class="btn lg primary" onclick="submitAddSource()">${t('automation.addSource')}</button>`);
    
    document.body.appendChild(modal);
    setTimeout(() => modal.classList.add('show'), 10);
}

// 存储测试结果数据
let lastTestData = null;
let wsTestSocket = null;

/**
 * 测试 REST API 连接
 */
async function testRestConnection() {
    const url = document.getElementById('source-rest-url').value.trim();
    const method = document.getElementById('source-rest-method').value;
    const auth = document.getElementById('source-rest-auth').value.trim();
    
    if (!url) {
        fieldError('source-rest-url', t('ui.alertEnterApiAddress'));
        return;
    }
    
    const btn = document.getElementById('btn-test-rest');
    const resultPanel = document.getElementById('rest-test-result');
    const statusSpan = resultPanel.querySelector('.test-status');
    
    btn.disabled = true;
    btn.textContent = t('ui.testing');
    resultPanel.style.display = 'block';
    statusSpan.innerHTML = '<span class="state warn">' + t('automationPage.testRequesting') + '</span>';
    
    try {
        // 通过 ESP32 代理请求（避免 CORS）
        const result = await api.call('automation.proxy.fetch', { 
            url, 
            method,
            headers: auth ? { 'Authorization': auth } : {}
        });
        
        if (result.code === 0 && result.data) {
            lastTestData = result.data.body;
            statusSpan.innerHTML = `<span class="state ok">${t('ssh.connectionSuccess')} (${result.data.status || 200})</span>`;
            
            // 解析并显示可选变量
            try {
                const jsonData = typeof lastTestData === 'string' ? JSON.parse(lastTestData) : lastTestData;
                renderVarSelector('rest-var-selector', jsonData, 'source-rest-path');
                document.getElementById('rest-json-preview').textContent = JSON.stringify(jsonData, null, 2);
            } catch (e) {
                // 非 JSON 响应
                document.querySelector('#rest-var-selector .var-list').innerHTML = 
                    ("<span class=\"t-note\">" + t('dataSource.responseNotJson') + "</span>");
                document.getElementById('rest-json-preview').textContent = lastTestData;
            }
        } else {
            statusSpan.innerHTML = `<span class="state bad">${t('automationPage.testFailed')}: ${escapeHtml(result.message || t('errors.unknownError'))}</span>`;
            document.querySelector('#rest-var-selector .var-list').innerHTML = '';
        }
    } catch (error) {
        statusSpan.innerHTML = `<span class="state bad">${t('common.error')}: ${escapeHtml(error.message)}</span>`;
        document.querySelector('#rest-var-selector .var-list').innerHTML = '';
    }
    
    btn.disabled = false;
    btn.textContent = t('common.test');
}

/**
 * 测试 WebSocket 连接
 */
async function testWsConnection() {
    const uri = document.getElementById('source-ws-uri').value.trim();
    
    if (!uri) {
        fieldError('source-ws-uri', t('ui.alertEnterWsAddress'));
        return;
    }
    
    const btn = document.getElementById('btn-test-ws');
    const resultPanel = document.getElementById('ws-test-result');
    const statusSpan = resultPanel.querySelector('.test-status');
    
    // 关闭之前的测试连接
    if (wsTestSocket) {
        wsTestSocket.close();
        wsTestSocket = null;
    }
    
    btn.disabled = true;
    btn.textContent = t('network.connecting');
    resultPanel.style.display = 'block';
    statusSpan.innerHTML = '<span class="state warn">' + t('toast.connecting') + '</span>';
    
    try {
        // 通过 ESP32 测试 WebSocket（获取第一条消息）
        const result = await api.call('automation.proxy.websocket_test', { uri, timeout_ms: 5000 });
        
        if (result.code === 0 && result.data) {
            lastTestData = result.data.message;
            statusSpan.innerHTML = `<span class="state ok">${t('automationPage.testConnected')}</span>`;
            
            try {
                const jsonData = typeof lastTestData === 'string' ? JSON.parse(lastTestData) : lastTestData;
                renderVarSelector('ws-var-selector', jsonData, 'source-ws-path');
                document.getElementById('ws-json-preview').textContent = JSON.stringify(jsonData, null, 2);
            } catch (e) {
                document.querySelector('#ws-var-selector .var-list').innerHTML = 
                    ("<span class=\"t-note\">" + t('dataSource.messageNotJson') + "</span>");
                document.getElementById('ws-json-preview').textContent = lastTestData;
            }
        } else {
            statusSpan.innerHTML = `<span class="state bad">${escapeHtml(result.message || t('ssh.connectionFailed'))}</span>`;
            document.querySelector('#ws-var-selector .var-list').innerHTML = '';
        }
    } catch (error) {
        statusSpan.innerHTML = `<span class="state bad">${t('common.error')}: ${escapeHtml(error.message)}</span>`;
        document.querySelector('#ws-var-selector .var-list').innerHTML = '';
    }
    
    btn.disabled = false;
    btn.textContent = t('common.test');
}

/**
 * 测试 Socket.IO 连接
 */
async function testSioConnection() {
    const url = document.getElementById('source-sio-url').value.trim();
    const event = document.getElementById('source-sio-event').value.trim();
    const timeout = parseInt(document.getElementById('source-sio-timeout').value) || 15000;
    
    if (!url) {
        fieldError('source-sio-url', t('ui.alertEnterSioAddress'));
        return;
    }
    
    const btn = document.getElementById('btn-test-sio');
    const resultPanel = document.getElementById('sio-test-result');
    const statusSpan = resultPanel.querySelector('.test-status');
    const eventInput = document.getElementById('source-sio-event');
    
    btn.disabled = true;
    btn.textContent = t('network.connecting');
    resultPanel.style.display = 'block';

    // 显示连接阶段状态
    const statusText = event ? t('promptRepair.waitEvent', {event}) : t('sshPage.connectingAutoDiscover');
    statusSpan.innerHTML = `<span class="state warn">${escapeHtml(statusText)}</span>`;
    
    try {
        // 通过 ESP32 测试 Socket.IO 连接
        // 如果没有指定事件，将获取服务器推送的第一个事件
        const params = { url, timeout_ms: timeout };
        if (event) params.event = event;
        
        const result = await api.call('automation.proxy.socketio_test', params);
        
        if (result.code === 0 && result.data) {
            const data = result.data;
            const eventName = data.event || t('sshPage.unknownEvent');
            lastTestData = data.data;
            
            // 显示成功状态和发现的事件
            let statusHtml = `<span class="state ok">${t('ssh.connectionSuccess')}</span>`;
            if (data.event) {
                statusHtml += ` <span class="t-note">${t('promptRepair.event')}: ${escapeHtml(eventName)}</span>`;
            }
            if (data.sid) {
                statusHtml += ` <span class="t-note">SID: ${escapeHtml(data.sid.substring(0, 8))}...</span>`;
            }
            statusSpan.innerHTML = statusHtml;
            
            // 自动填充发现的事件名（如果用户没有手动输入）
            if (data.event && !eventInput.value) {
                eventInput.value = data.event;

            }
            
            try {
                const jsonData = typeof lastTestData === 'string' ? JSON.parse(lastTestData) : lastTestData;
                renderVarSelector('sio-var-selector', jsonData, 'source-sio-path');
                document.getElementById('sio-json-preview').textContent = JSON.stringify(jsonData, null, 2);
            } catch (e) {
                document.querySelector('#sio-var-selector .var-list').innerHTML = 
                    ("<span class=\"t-note\">" + t('automationPage.eventDataNotJson') + "</span>");
                document.getElementById('sio-json-preview').textContent = String(lastTestData);
            }
        } else {
            // 显示详细错误信息
            let errorMsg = result.message || t('ssh.connectionFailed');
            if (result.data && result.data.sid) {
                errorMsg += ' ' + t('promptRepair.noEvent');
            }
            statusSpan.innerHTML = `<span class="state bad">${escapeHtml(errorMsg)}</span>`;
            document.querySelector('#sio-var-selector .var-list').innerHTML = 
                ("<span class=\"t-note\">" + t('automationPage.hintAutoDiscoverEvent') + "</span>");
            
            // 显示详细错误
            if (result.data && result.data.error) {
                document.getElementById('sio-json-preview').textContent = result.data.error;
                document.getElementById('sio-json-preview').style.display = 'block';
            }
        }
    } catch (error) {
        statusSpan.innerHTML = `<span class="state bad">${t('common.error')}: ${escapeHtml(error.message)}</span>`;
        document.querySelector('#sio-var-selector .var-list').innerHTML = '';
    }
    
    btn.disabled = false;
    btn.textContent = t('common.test');
}

/**
 * 切换 Socket.IO JSON 预览显示
 */
function toggleSioJsonPreview() {
    const preview = document.getElementById('sio-json-preview');
    preview.style.display = preview.style.display === 'none' ? 'block' : 'none';
}

/**
 * 渲染变量选择器
 */
function renderVarSelector(containerId, data, targetInputId, prefix = '') {
    const container = document.querySelector(`#${containerId} .var-list`);
    if (!container) return;
    
    const items = [];
    flattenJson(data, prefix, items);
    
    if (items.length === 0) {
        container.innerHTML = ("<span class=\"t-note\">" + t('ui.noSelectableFields') + "</span>");
        return;
    }
    
    container.replaceChildren(...items.map(item => {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'btn sm var-item';
        button.title = item.type + ': ' + item.preview;
        button.textContent = item.path;
        button.addEventListener('click', () => selectVarPath(targetInputId, item.path));
        return button;
    }));
}

/**
 * 扁平化 JSON 对象，提取所有叶子节点路径
 */
function flattenJson(obj, prefix, result, maxDepth = 5) {
    if (maxDepth <= 0) return;
    
    if (obj === null || obj === undefined) {
        result.push({ path: prefix || '(root)', type: 'null', preview: 'null' });
        return;
    }
    
    const type = typeof obj;
    
    if (Array.isArray(obj)) {
        if (obj.length === 0) {
            result.push({ path: prefix || '(root)', type: 'array', preview: '[]' });
        } else {
            // 显示数组本身
            result.push({ 
                path: prefix || '(root)', 
                type: `array[${obj.length}]`, 
                preview: `[${obj.length} items]` 
            });
            // 展开第一个元素作为示例
            if (typeof obj[0] === 'object' && obj[0] !== null) {
                flattenJson(obj[0], prefix ? `${prefix}[0]` : '[0]', result, maxDepth - 1);
            }
        }
    } else if (type === 'object') {
        const keys = Object.keys(obj);
        if (keys.length === 0) {
            result.push({ path: prefix || '(root)', type: 'object', preview: '{}' });
        } else {
            for (const key of keys) {
                const newPath = prefix ? `${prefix}.${key}` : key;
                const value = obj[key];
                const valueType = typeof value;
                
                if (value === null) {
                    result.push({ path: newPath, type: 'null', preview: 'null' });
                } else if (Array.isArray(value)) {
                    flattenJson(value, newPath, result, maxDepth - 1);
                } else if (valueType === 'object') {
                    flattenJson(value, newPath, result, maxDepth - 1);
                } else {
                    // 叶子节点
                    let preview = String(value);
                    if (preview.length > 30) preview = preview.substring(0, 27) + '...';
                    result.push({ path: newPath, type: valueType, preview });
                }
            }
        }
    } else {
        // 基本类型
        let preview = String(obj);
        if (preview.length > 30) preview = preview.substring(0, 27) + '...';
        result.push({ path: prefix || '(root)', type, preview });
    }
}

/**
 * 选择变量路径
 */
function selectVarPath(inputId, path) {
    const input = document.getElementById(inputId);
    if (input) {
        input.value = path;
        input.focus();
        // 高亮效果

    }
}

/**
 * 切换 JSON 预览显示
 */
function toggleJsonPreview() {
    const preview = document.getElementById('rest-json-preview');
    preview.style.display = preview.style.display === 'none' ? 'block' : 'none';
}

function toggleWsJsonPreview() {
    const preview = document.getElementById('ws-json-preview');
    preview.style.display = preview.style.display === 'none' ? 'block' : 'none';
}

/**
 * 切换数据源类型
 */
function switchSourceType(type) {
    // 更新隐藏字段
    document.getElementById('source-type').value = type;
    
    // 更新分段控件与标题后缀
    document.querySelectorAll('#source-type-tabs button').forEach(tab => tab.classList.toggle('on', tab.dataset.type === type));
    const titleEl = document.getElementById('add-source-title');
    if (titleEl) titleEl.textContent = t('automation.addSourceTitle') + ' · ' + ({ rest: 'REST API', websocket: 'WebSocket', socketio: 'Socket.IO', variable: t('automation.variableTab') }[type]);
    
    // 切换配置区块
    document.getElementById('source-rest-config').style.display = type === 'rest' ? 'block' : 'none';
    document.getElementById('source-websocket-config').style.display = type === 'websocket' ? 'block' : 'none';
    const sioConfig = document.getElementById('source-socketio-config');
    if (sioConfig) sioConfig.style.display = type === 'socketio' ? 'block' : 'none';
    const varConfig = document.getElementById('source-variable-config');
    if (varConfig) {
        varConfig.style.display = type === 'variable' ? 'block' : 'none';
        // 切换到指令变量类型时自动加载主机列表
        if (type === 'variable') {
            loadSshHostsForSource();
        }
    }
    
    // 处理数据源 ID 输入框的只读状态
    const sourceIdInput = document.getElementById('source-id');
    if (sourceIdInput) {
        if (type === 'variable') {
            // 指令变量类型：ID 由选择的命令决定，设为只读
            sourceIdInput.readOnly = true;
            sourceIdInput.placeholder = typeof t === 'function' ? t('automationPage.autoFilledByCmd') : '由选择的指令自动填入';
        } else {
            // 其他类型：允许手动输入
            sourceIdInput.readOnly = false;
            sourceIdInput.placeholder = typeof t === 'function' ? t('automation.sourceIdPlaceholder') : '如: agx_temp';
            sourceIdInput.value = '';  // 清空之前可能由指令填入的值
        }
    }
}

/**
 * 根据数据源类型更新表单字段（兼容旧调用）
 */
function updateSourceTypeFields() {
    const type = document.getElementById('source-type').value;
    switchSourceType(type);
}

// updateBuiltinFields 函数已移除 - 内置数据源由系统自动注册，无需手动配置

/**
 * 加载 SSH 主机列表（用于数据源配置）
 */
async function loadSshHostsForSource() {
    const hostSelect = document.getElementById('source-ssh-host');
    if (!hostSelect) return;
    
    hostSelect.innerHTML = '<option value="">-- ' + t('common.loading') + ' --</option>';
    
    // 重置命令选择
    const cmdSelect = document.getElementById('source-ssh-cmd');
    if (cmdSelect) {
        cmdSelect.innerHTML = '<option value="">' + (typeof t === 'function' ? t('automation.selectHostFirst') : '-- 先选择主机 --') + '</option>';
    }
    
    // 隐藏命令预览
    const preview = document.getElementById('source-ssh-cmd-preview');
    if (preview) preview.style.display = 'none';
    
    // 重置变量预览
    const varsListDiv = document.getElementById('ssh-vars-list');
    if (varsListDiv) varsListDiv.innerHTML = `<span class="text-muted">${typeof t === 'function' ? t('automation.selectHostCmdFirst') : 'Please select SSH host and command first'}</span>`;
    
    try {
        const result = await api.call('ssh.hosts.list');
        if (result.code === 0 && result.data && result.data.hosts) {
            const hosts = result.data.hosts;
            
            if (hosts.length === 0) {
                hostSelect.innerHTML = '<option value="">' + t('promptRepair.noHosts') + '</option>';
                return;
            }
            
            let html = '<option value="">' + (typeof t === 'function' ? t('automationPage.selectHostPrompt') : '-- 请选择主机 --') + '</option>';
            hosts.forEach(h => {
                const label = `${h.id} (${h.username}@${h.host}:${h.port || 22})`;
                html += `<option value="${escapeHtml(h.id)}">${escapeHtml(label)}</option>`;
            });
            hostSelect.innerHTML = html;
        } else {
            hostSelect.innerHTML = `<option value="">${escapeHtml(t('common.loadFailedMsg', {msg: result.message || t('errors.unknownError')}))}</option>`;
        }
    } catch (error) {
        hostSelect.innerHTML = `<option value="">${escapeHtml(t('common.loadFailedMsg', {msg: error.message}))}</option>`;
    }
}

/**
 * SSH 主机选择变化时的处理（数据源配置用）
 */
async function onSshHostChangeForSource() {
    const hostId = document.getElementById('source-ssh-host').value;
    const cmdSelect = document.getElementById('source-ssh-cmd');
    
    if (!cmdSelect) return;
    
    // 隐藏命令预览
    const preview = document.getElementById('source-ssh-cmd-preview');
    if (preview) preview.style.display = 'none';
    
    // 重置变量预览
    const varsListDiv = document.getElementById('ssh-vars-list');
    if (varsListDiv) varsListDiv.innerHTML = ("<span class=\"text-muted\">" + t('ui.selectCmdFirst') + "</span>");
    
    if (!hostId) {
        cmdSelect.innerHTML = '<option value="">' + (typeof t === 'function' ? t('automation.selectHostFirst') : '-- 先选择主机 --') + '</option>';
        return;
    }
    
    // 确保 sshCommands 已加载（异步操作）
    if (typeof sshCommands === 'undefined' || Object.keys(sshCommands).length === 0) {
        cmdSelect.innerHTML = '<option value="">-- ' + t('common.loading') + ' --</option>';
        await loadSshCommands();
    }
    
    // 获取该主机下的命令列表
    const commands = sshCommands[hostId] || [];
    
    if (commands.length === 0) {
        cmdSelect.innerHTML = '<option value="">' + t('promptRepair.noCommands') + '</option>';
        return;
    }
    
    let html = '<option value="">' + t('promptRepair.chooseCommand') + '</option>';
    commands.forEach((cmd, idx) => {
        const icon = cmd.icon || 'ri-rocket-line';
        const label = (icon && icon.startsWith && icon.startsWith('ri-')) ? cmd.name : `${icon} ${cmd.name}`;
        html += `<option value="${idx}">${escapeHtml(label)}</option>`;
    });
    cmdSelect.innerHTML = html;
}

/**
 * SSH 命令选择变化时的处理
 */
function onSshCmdChange() {
    const hostId = document.getElementById('source-ssh-host').value;
    const cmdIdx = document.getElementById('source-ssh-cmd').value;
    const preview = document.getElementById('source-ssh-cmd-preview');
    const varsListDiv = document.getElementById('ssh-vars-list');
    const sourceIdInput = document.getElementById('source-id');
    const sourceLabelInput = document.getElementById('source-label');
    
    if (!hostId || cmdIdx === '') {
        if (preview) preview.style.display = 'none';
        if (varsListDiv) varsListDiv.innerHTML = ("<span class=\"text-muted\">" + t('ui.selectCmdFirst') + "</span>");
        return;
    }
    
    // 获取选中的命令
    const cmd = sshCommands[hostId]?.[parseInt(cmdIdx)];
    if (!cmd) {
        if (preview) preview.style.display = 'none';
        if (varsListDiv) varsListDiv.innerHTML = ("<span class=\"text-muted\">" + t('ui.cmdNotExist') + "</span>");
        return;
    }
    
    // 显示命令详情预览
    if (preview) {
        preview.style.display = 'block';
        document.getElementById('preview-command').textContent = cmd.command;
        document.getElementById('preview-desc').textContent = cmd.desc || t('ui.noDescription');
        document.getElementById('preview-timeout').textContent = cmd.timeout || 30;
    }
    
    // 更新变量预览
    const varName = cmd.varName || cmd.name;  // 优先使用 varName，否则用 name
    if (varsListDiv) {
        varsListDiv.innerHTML = `
            <div class="var-item-preview"><code>${escapeHtml(varName)}.status</code> - ${t('varPreview.status')}</div>
            <div class="var-item-preview"><code>${escapeHtml(varName)}.exit_code</code> - ${t('varPreview.exitCode')}</div>
            <div class="var-item-preview"><code>${escapeHtml(varName)}.extracted</code> - ${t('varPreview.extracted')}</div>
            <div class="var-item-preview"><code>${escapeHtml(varName)}.expect_matched</code> - ${t('varPreview.expectMatched')}</div>
            <div class="var-item-preview"><code>${escapeHtml(varName)}.fail_matched</code> - ${t('varPreview.failMatched')}</div>
            <div class="var-item-preview"><code>${escapeHtml(varName)}.host</code> - ${t('varPreview.host')}</div>
            <div class="var-item-preview"><code>${escapeHtml(varName)}.timestamp</code> - ${t('varPreview.timestamp')}</div>
        `;
    }
    
    // 自动填充数据源 ID 和显示名称（基于命令的 varName）
    if (sourceIdInput) {
        sourceIdInput.value = varName;
        sourceIdInput.readOnly = true;  // 设为只读，因为必须与 varName 一致
    }
    if (sourceLabelInput && !sourceLabelInput.value) {
        sourceLabelInput.value = cmd.name || varName;
    }
}

/**
 * 提交添加数据源
 */
async function submitAddSource() {
    const id = document.getElementById('source-id').value.trim();
    const label = document.getElementById('source-label').value.trim() || id;
    const type = document.getElementById('source-type').value;
    const interval = parseInt(document.getElementById('source-interval')?.value) || 1000;
    const enabled = document.getElementById('source-enabled').checked;
    
    if (!id) {
        fieldError('source-id', t('ui.alertEnterSourceId'));
        return;
    }
    
    const params = { id, label, type, poll_interval_ms: interval, enabled };
    
    // 根据类型添加额外参数
    if (type === 'websocket') {
        params.uri = document.getElementById('source-ws-uri').value.trim();
        params.json_path = document.getElementById('source-ws-path').value.trim();
        params.reconnect_ms = parseInt(document.getElementById('source-ws-reconnect').value) || 5000;
        
        if (!params.uri) {
            fieldError('source-ws-uri', t('ui.alertEnterWsUri'));
            return;
        }
    } else if (type === 'rest') {
        params.url = document.getElementById('source-rest-url').value.trim();
        params.method = document.getElementById('source-rest-method').value;
        params.json_path = document.getElementById('source-rest-path').value.trim();
        params.auth_header = document.getElementById('source-rest-auth').value.trim();
        
        if (!params.url) {
            fieldError('source-rest-url', t('ui.alertEnterRestUrl'));
            return;
        }
    } else if (type === 'socketio') {
        // Socket.IO 数据源配置
        params.url = document.getElementById('source-sio-url').value.trim();
        params.event = document.getElementById('source-sio-event').value.trim();
        params.json_path = document.getElementById('source-sio-path').value.trim();
        params.timeout_ms = parseInt(document.getElementById('source-sio-timeout').value) || 15000;
        
        // 自动发现开关
        const autoDiscoverEl = document.getElementById('source-sio-auto-discover');
        params.auto_discover = autoDiscoverEl ? autoDiscoverEl.checked : true;
        
        if (!params.url) {
            fieldError('source-sio-url', t('ui.alertEnterSioAddress'));
            return;
        }
        if (!params.event) {
            fieldError('source-sio-event', t('ui.alertEnterSioEvent'));
            return;
        }
    } else if (type === 'variable') {
        // 指令变量数据源配置 - 选择已创建的指令
        const hostId = document.getElementById('source-ssh-host').value;
        const cmdIdx = document.getElementById('source-ssh-cmd').value;
        
        if (!hostId) {
            fieldError('source-ssh-host', t('ui.alertSelectSshHost'));
            return;
        }
        if (cmdIdx === '') {
            fieldError('source-ssh-cmd', t('ui.alertSelectSshCmd'));
            return;
        }
        
        // 获取选中的命令配置
        const cmd = sshCommands[hostId]?.[parseInt(cmdIdx)];
        if (!cmd) {
            fieldError('source-ssh-cmd', t('ui.alertCmdNotExist'));
            return;
        }
        
        // 使用命令的 varName 或 name 作为变量前缀
        const varName = cmd.varName || cmd.name;
        
        // SSH 命令配置 - 从已创建的指令中获取
        params.ssh_host_id = hostId;
        params.ssh_command = cmd.command;
        params.var_prefix = varName + '.';  // 变量前缀
        params.var_watch_all = true;         // 监视所有生成的变量
        
        // 高级选项（从命令配置中获取）
        if (cmd.expectPattern) params.ssh_expect_pattern = cmd.expectPattern;
        if (cmd.failPattern) params.ssh_fail_pattern = cmd.failPattern;
        if (cmd.extractPattern) params.ssh_extract_pattern = cmd.extractPattern;
        if (cmd.timeout && cmd.timeout !== 30) params.ssh_timeout = cmd.timeout;
        
        // 执行间隔（秒转毫秒）
        params.poll_interval_ms = (parseInt(document.getElementById('source-var-interval').value) || 60) * 1000;
    }
    
    try {
        const result = await api.call('automation.sources.add', params);
        if (result.code === 0) {
            showToast(typeof t === 'function' ? t('toast.sourceCreated', { id }) : `数据源 ${id} 创建成功`, 'success');
            closeModal('add-source-modal');
            await Promise.all([refreshSources(), refreshAutomationStatus()]);
        } else {
            showToast(typeof t === 'function' ? t('toast.sourceCreateFailed') + ': ' + result.message : `创建数据源失败: ${result.message}`, 'error');
        }
    } catch (error) {
        showToast(typeof t === 'function' ? t('toast.sourceCreateFailed') + ': ' + error.message : `创建数据源失败: ${error.message}`, 'error');
    }
}

// 规则图标：直接存储 RemixIcon 类名（ri-xxx），保留旧 emoji 映射以兼容历史数据
const RULE_ICON_LIST = [
    'ri-thunderstorms-line', 'ri-notification-line', 'ri-lightbulb-line',
    'ri-plug-line', 'ri-temp-hot-line', 'ri-timer-line',
    'ri-bar-chart-line', 'ri-focus-line', 'ri-rocket-line',
    'ri-settings-line', 'ri-tools-line', 'ri-music-line',
    'ri-smartphone-line', 'ri-computer-line', 'ri-global-line',
    'ri-lock-line', 'ri-shield-line', 'ri-file-text-line',
    'ri-movie-line', 'ri-refresh-line'
];
// 旧版 emoji → ri 映射（向后兼容历史数据）
const _EMOJI_TO_RI = {
    '⚡': 'ri-thunderstorms-line', '🔔': 'ri-notification-line', '💡': 'ri-lightbulb-line',
    '🔌': 'ri-plug-line', '🌡️': 'ri-temp-hot-line', '⏰': 'ri-timer-line',
    '📊': 'ri-bar-chart-line', '🎯': 'ri-focus-line', '🚀': 'ri-rocket-line',
    '⚙️': 'ri-settings-line', '🔧': 'ri-tools-line', '🎵': 'ri-music-line',
    '📱': 'ri-smartphone-line', '🖥️': 'ri-computer-line', '🌐': 'ri-global-line',
    '🔒': 'ri-lock-line', '🛡️': 'ri-shield-line', '📝': 'ri-file-text-line',
    '🎬': 'ri-movie-line', '🔄': 'ri-refresh-line'
};
function getRuleIconRi(icon) {
    if (!icon) return 'ri-thunderstorms-line';
    // 已经是 ri-xxx 格式，直接返回
    if (icon.startsWith('ri-')) return icon;
    // 旧版 emoji → ri 映射
    return _EMOJI_TO_RI[icon] || 'ri-thunderstorms-line';
}

/**
 * 显示添加/编辑规则模态框
 * @param {object} ruleData - 编辑时传入现有规则数据，添加时为 null
 */
function showAddRuleModal(ruleData = null) {
    const isEdit = !!ruleData;
    
    // 移除可能存在的旧模态框
    const oldModal = document.getElementById('add-rule-modal');
    if (oldModal) oldModal.remove();
    
    // 重置计数器
    conditionRowCount = 0;
    actionRowCount = 0;
    
    const modal = document.createElement('div');
    modal.id = 'add-rule-modal';
    modal.className = 'modal';
    const iconPickerHtml = RULE_ICON_LIST.map((ri, i) =>
        `<button type="button" class="btn icon sm icon-btn${i === 0 ? ' selected' : ''}" data-icon="${ri}" onclick="selectRuleIcon('${ri}')"><svg class="i"><use href="#${ri}"/></svg></button>`
    ).join('');
    const sw = (id, on, label, extra = '') => swc(id, on, `aria-label="${label}" ${extra}`);
    const remove = '<button type="button" class="btn icon sm quiet" onclick="this.closest(\'.row\').remove()" aria-label="' + t('securityPage.remove') + '" title="' + t('securityPage.remove') + '"><svg class="i"><use href="#ri-close-line"/></svg></button>';
    modal.innerHTML = sheet(760, isEdit ? t('automation.editRule') : t('automation.addRule'), `
        ${grp(
            row(t('automation.ruleId'), `<input type="text" id="rule-id" class="field mono" style="width:220px" aria-label="${t('automation.ruleId')}">`, t('automation.ruleIdPlaceholder')) +
            row(t('automation.ruleName'), inp('rule-name', 220, ''), t('automation.ruleNamePlaceholder')) +
            row(t('automation.iconLabel'), `<div class="seg"><button type="button" class="on icon-tab" onclick="switchRuleIconType('emoji')">${t('automation.iconTab')}</button><button type="button" class="icon-tab" onclick="switchRuleIconType('image')">${t('automation.imageTab')}</button></div><button type="button" class="btn icon" id="rule-icon-toggle" style="width:34px" onclick="toggleRuleIconPanel()" aria-label="${t('automation.iconLabel')}" title="${t('automation.iconLabel')}"><svg class="i"><use href="#ri-thunderstorms-line"/></svg></button>`) +
            `<div class="row" id="rule-icon-panel" hidden style="display:block;padding:12px 0">
                <div id="rule-icon-emoji-picker" class="icon-picker" style="display:flex;flex-wrap:wrap;gap:6px;align-items:center">
                    <input type="text" id="rule-emoji-input" class="field sm" placeholder="ri-xxx" maxlength="40" onchange="selectRuleIconFromInput()" style="width:140px;text-align:center">
                    ${iconPickerHtml}
                </div>
                <div id="rule-icon-image-picker" class="icon-image-picker hidden" style="display:flex;gap:8px;align-items:center">
                    <div id="rule-icon-preview" class="icon-image-preview t-note" style="width:34px;height:34px;display:flex;align-items:center;justify-content:center;overflow:hidden;border-radius:8px;background:var(--fill)"><span class="preview-placeholder">${t('automation.previewNone')}</span></div>
                    <input type="text" id="rule-icon-path" class="field" style="flex:1" readonly placeholder="${t('automation.selectImagePlaceholder')}" aria-label="${t('automation.selectImagePlaceholder')}">
                    <button type="button" class="btn sm" onclick="browseRuleIconImage()"><svg class="i"><use href="#ri-folder-open-line"/></svg>${t('common.browse')}</button>
                    <button type="button" class="btn icon sm" onclick="clearRuleIconImage()" aria-label="${t('common.clear')}" title="${t('common.clear')}"><svg class="i"><use href="#ri-close-line"/></svg></button>
                </div>
            </div>`)}
        <input type="hidden" id="rule-icon" value="ri-thunderstorms-line">
        <input type="hidden" id="rule-icon-type" value="emoji">
        <input type="hidden" id="rule-revision" value="${isEdit ? ruleData.revision : 0}">
        ${gt(t('automation.ruleOptions'))}
        ${grp(
            row(t('automation.conditionLogic'), `<select id="rule-logic" class="field" style="width:110px"><option value="and">AND</option><option value="or">OR</option></select>`) +
            row(t('automation.cooldownShort'), inp('rule-cooldown', 100, '', 'num', 'type="number" value="0" min="0"') + unit('ms')) +
            row(t('automation.enableImmediately'), sw('rule-enabled', true, t('automation.enableImmediately'))) +
            row(t('automation.ruleShowPanel'), sw('rule-show-dashboard', true, t('automation.ruleShowPanel'), `onchange="this.dataset.dirty='true'"`)) +
            row(t('automation.ruleAllowManualTrigger'), sw('rule-allow-manual', true, t('automation.ruleAllowManualTrigger'), `onchange="this.dataset.dirty='true'"`)))}
        <div class="sec-h" style="margin:18px 4px 6px">
            <span class="t-label">${t('automation.triggerConditions')}</span>
            <span style="display:flex;align-items:center;gap:12px">
                <span style="display:flex;gap:8px;align-items:center">${sw('rule-manual-only', false, t('automation.manualOnly'), 'onchange="toggleManualOnly()"')}<span class="t-body">${t('automation.manualOnly')}</span></span>
                <button class="btn sm" id="add-condition-btn" onclick="addConditionRow()"><svg class="i"><use href="#ri-add-line"/></svg>${t('common.add')}</button>
            </span>
        </div>
        <div class="grp" id="conditions-container"><div class="row empty-hint"><span class="t-note">${t('automation.addConditionHint')}</span></div></div>
        <div class="sec-h" style="margin:18px 4px 6px">
            <span class="t-label">${t('automation.executionActions')}</span>
            <button class="btn sm" onclick="addActionTemplateRow()"><svg class="i"><use href="#ri-add-line"/></svg>${t('common.add')}</button>
        </div>
        <div class="grp" id="actions-container"><div class="row empty-hint"><span class="t-note">${t('automation.selectFromTemplatesHint')}</span></div></div>
        <div class="t-note" style="margin:6px 4px 0">${t('automation.createActionFirstHint')}</div>`,
        `<button class="btn lg" onclick="closeModal('add-rule-modal')">${t('common.cancel')}</button><button class="btn lg primary" onclick="submitAddRule(${isEdit ? "'" + ruleData.id + "'" : ''})">${isEdit ? t('automationPage.saveRuleChanges') : t('automation.addRule')}</button>`);
    
    document.body.appendChild(modal);
    setTimeout(() => modal.classList.add('show'), 10);
    
    // 如果是编辑模式，填充现有数据
    if (isEdit && ruleData) {
        document.getElementById('rule-id').value = ruleData.id;
        document.getElementById('rule-id').disabled = true;
        document.getElementById('rule-show-dashboard').checked = ruleData.show_on_dashboard === true;
        document.getElementById('rule-show-dashboard').dataset.present = String(Object.hasOwn(ruleData, 'show_on_dashboard'));
        document.getElementById('rule-allow-manual').checked = ruleData.allow_manual_trigger === true;
        document.getElementById('rule-allow-manual').dataset.present = String(Object.hasOwn(ruleData, 'allow_manual_trigger'));
        document.getElementById('rule-name').value = ruleData.name || '';
        document.getElementById('rule-logic').value = ruleData.logic || 'and';
        document.getElementById('rule-cooldown').value = ruleData.cooldown_ms || 0;
        document.getElementById('rule-enabled').checked = ruleData.enabled !== false;
        
        // 填充图标（兼容旧 emoji 和新 ri-xxx）
        const icon = ruleData.icon || 'ri-thunderstorms-line';
        if (icon.startsWith('/sdcard/')) {
            document.getElementById('rule-icon').value = icon;
            document.getElementById('rule-icon-type').value = 'image';
            document.getElementById('rule-icon-path').value = icon;
            switchRuleIconType('image');
            updateRuleIconPreview(icon);
        } else {
            // selectRuleIcon 内部会自动将旧 emoji 转换为 ri-xxx
            document.getElementById('rule-icon-type').value = 'emoji';
            selectRuleIcon(icon);
        }
        
        // 填充条件
        if (ruleData.conditions && ruleData.conditions.length > 0) {
            ruleData.conditions.forEach(cond => {
                addConditionRow(cond);
            });
        }
        
        // 填充手动触发标记（在填充条件之后设置，以便正确更新 UI）
        if (ruleData.manual_trigger) {
            document.getElementById('rule-manual-only').checked = true;
            toggleManualOnly();  // 更新 UI 状态
        }
        
        // 填充动作
        if (ruleData.actions && ruleData.actions.length > 0) {
            // 异步加载动作模板行
            modal.dataset.loading = 'true';
            (async () => {
                try { for (const act of ruleData.actions) {
                    await addActionTemplateRow(
                        act.template_id, 
                        act.delay_ms || 0,
                        act.repeat_mode || 'once',
                        act.repeat_count || 1,
                        act.repeat_interval_ms ?? 1000,
                        act.condition || null, act
                    );
                } } finally { modal.dataset.loading = 'false'; }
            })();
        }
    }
}

/**
 * 切换仅手动触发模式
 */
function toggleManualOnly() {
    const checked = document.getElementById('rule-manual-only').checked;
    document.getElementById('add-condition-btn').disabled = checked;
    const container = document.getElementById('conditions-container');
    container.querySelectorAll('input, select, button').forEach(el => { el.disabled = checked; });
    let note = document.getElementById('rule-preserved-conditions');
    if (!note) { note = document.createElement('p'); note.id = 'rule-preserved-conditions'; note.className = 't-note'; note.style.margin = '6px 4px'; container.before(note); }
    note.textContent = checked ? runtimeText('conditionsPreserved') : '';
}


// ==================== 规则图标选择 ====================

function switchRuleIconType(type) {
    const emojiPicker = document.getElementById('rule-icon-emoji-picker');
    const imagePicker = document.getElementById('rule-icon-image-picker');
    const tabs = document.querySelectorAll('#add-rule-modal .icon-tab');
    
    tabs.forEach(tab => tab.classList.remove('on'));
    
    if (type === 'image') {
        emojiPicker.classList.add('hidden');
        imagePicker.classList.remove('hidden');
        tabs[1]?.classList.add('on');
        document.getElementById('rule-icon-type').value = 'image';
        document.getElementById('rule-icon-panel').hidden = false;
    } else {
        emojiPicker.classList.remove('hidden');
        imagePicker.classList.add('hidden');
        tabs[0]?.classList.add('on');
        document.getElementById('rule-icon-type').value = 'emoji';
    }
    refreshRuleIconToggle();
}

// 规则图标行右侧的按钮显示当前图标，点击展开/收起选择面板
function toggleRuleIconPanel() {
    const panel = document.getElementById('rule-icon-panel');
    if (panel) panel.hidden = !panel.hidden;
}

function refreshRuleIconToggle() {
    const btn = document.getElementById('rule-icon-toggle');
    const icon = document.getElementById('rule-icon')?.value || 'ri-thunderstorms-line';
    if (btn) btn.innerHTML = icon.startsWith('/sdcard/') ? '<svg class="i"><use href="#ri-image-line"/></svg>' : `<svg class="i"><use href="#${escapeHtml(icon)}"/></svg>`;
}

function selectRuleIcon(icon) {
    // icon 可以是 ri-xxx 或旧版 emoji，统一转为 ri-xxx
    const riIcon = getRuleIconRi(icon);
    document.getElementById('rule-icon').value = riIcon;
    document.getElementById('rule-icon-type').value = 'emoji';
    // 预览输入框显示图标类名
    const input = document.getElementById('rule-emoji-input');
    if (input) input.value = riIcon;
    document.querySelectorAll('#add-rule-modal .icon-btn').forEach(btn => {
        btn.classList.toggle('selected', btn.getAttribute('data-icon') === riIcon);
    });
    refreshRuleIconToggle();
}

function selectRuleIconFromInput() {
    const input = document.getElementById('rule-emoji-input');
    const icon = input.value.trim();
    if (icon) {
        // 如果用户输入了 ri-xxx 类名则直接使用，否则通过映射转换
        const riIcon = icon.startsWith('ri-') ? icon : getRuleIconRi(icon);
        document.getElementById('rule-icon').value = riIcon;
        document.getElementById('rule-icon-type').value = 'emoji';
        // 更新按钮选中状态
        document.querySelectorAll('#add-rule-modal .icon-btn').forEach(btn => {
            btn.classList.toggle('selected', btn.getAttribute('data-icon') === riIcon);
        });
        refreshRuleIconToggle();
    }
}

async function browseRuleIconImage() {
    filePickerCurrentPath = '/sdcard/images';
    filePickerSelectedFile = null;
    filePickerCallback = (path) => {
        document.getElementById('rule-icon').value = path;
        document.getElementById('rule-icon-path').value = path;
        updateRuleIconPreview(path);
        refreshRuleIconToggle();
    };
    document.getElementById('file-picker-modal').classList.remove('hidden');
    await loadFilePickerDirectory(filePickerCurrentPath);
}

function updateRuleIconPreview(path) {
    const preview = document.getElementById('rule-icon-preview');
    if (path && path.startsWith('/sdcard/')) {
        const loadFailed = typeof t === 'function' ? t('sshPage.iconPreviewFailed') : '加载失败';
        preview.innerHTML = `<img src="/api/v1/file/download?path=${encodeURIComponent(path)}" alt="icon" onerror="this.parentElement.innerHTML='<span class=\\'preview-placeholder\\'>${loadFailed}</span>'">`;
    } else {
        preview.innerHTML = '<span class="preview-placeholder">' + (typeof t === 'function' ? t('sshPage.iconPreviewNone') : '无') + '</span>';
    }
}

function clearRuleIconImage() {
    document.getElementById('rule-icon').value = 'ri-thunderstorms-line';
    document.getElementById('rule-icon-path').value = '';
    document.getElementById('rule-icon-type').value = 'emoji';
    updateRuleIconPreview(null);
    switchRuleIconType('emoji');
}

// 条件行计数器
let conditionRowCount = 0;

// Matches the operators and scalar types supported by ts_rule_codec.c.
const CONDITION_OPERATORS = ['eq', 'ne', 'lt', 'le', 'gt', 'ge', 'contains'];
function conditionEditorFields(row, kind) {
    const prefix = kind === 'trigger' ? 'cond' : 'action-condition';
    return {
        variable: row.querySelector('.' + prefix + '-variable'),
        variableButton: row.querySelector(kind === 'trigger' ? '.cond-variable-btn' : '.action-condition-var-btn'),
        operator: row.querySelector('.' + prefix + '-operator'),
        value: row.querySelector('.' + prefix + '-value')
    };
}
function fillConditionEditor(row, kind, condition = null) {
    const fields = conditionEditorFields(row, kind);
    row._originalCondition = condition ? structuredClone(condition) : null;
    const labels = {eq: '==', ne: '!=', lt: '<', le: '<=', gt: '>', ge: '>=', contains: t('automation.operatorContains')};
    for (const op of CONDITION_OPERATORS) {
        const option = document.createElement('option');
        option.value = op; option.textContent = labels[op]; fields.operator.appendChild(option);
    }
    const operator = condition?.operator ?? 'eq';
    if (!CONDITION_OPERATORS.includes(operator)) {
        const option = document.createElement('option');
        option.value = operator; option.textContent = t('conditionEditor.unsupportedOperator', {operator});
        fields.operator.appendChild(option);
    }
    fields.operator.value = operator;
    fields.variable.value = condition?.variable ?? '';
    fields.variableButton.textContent = fields.variable.value || t('automation.selectVariable');
    fields.value.value = condition ? JSON.stringify(condition.value) : '';
    for (const key of ['variableButton', 'operator', 'value']) fields[key].id = row.id + '-' + key;
}
function readConditionEditor(row, kind) {
    const fields = conditionEditorFields(row, kind);
    const fail = (field, key, params) => { fieldError(fields[field].id, t('conditionEditor.' + key, params)); return null; };
    const variable = fields.variable.value;
    if (!variable.trim()) return fail('variableButton', 'variableRequired');
    if (variable.includes('\0') || new TextEncoder().encode(variable).length >= 64)
        return fail('variableButton', 'variableInvalid');
    const operator = fields.operator.value;
    if (!CONDITION_OPERATORS.includes(operator)) return fail('operator', 'unsupportedOperator', {operator});
    const raw = fields.value.value.trim();
    let value;
    try { value = JSON.parse(raw); } catch (_) { value = raw; }
    let valueType;
    if (value === null) valueType = 0;
    else if (typeof value === 'boolean') valueType = 1;
    else if (typeof value === 'number') {
        if (!Number.isFinite(value)) return fail('value', 'numberInvalid');
        valueType = Number.isInteger(value) && value >= -2147483648 && value <= 2147483647 ? 2 : 3;
    } else if (typeof value === 'string') {
        if (value.includes('\0')) return fail('value', 'stringNul');
        if (new TextEncoder().encode(value).length >= 64) return fail('value', 'stringTooLong');
        valueType = 4;
    } else return fail('value', 'scalarRequired');
    const original = row._originalCondition;
    if (original && Object.is(value, original.value) && original.value_type !== undefined) {
        if (original.value_type !== valueType && !(original.value_type === 3 && typeof value === 'number'))
            return fail('value', 'typeInvalid');
        valueType = original.value_type;
    }
    return {variable, operator, value, value_type: valueType};
}

/** 添加条件行；接收完整条件 {variable, operator, value, value_type}。 */
function addConditionRow(condition = null) {
    const container = document.getElementById('conditions-container');
    container.querySelector('.empty-hint')?.remove();
    const manualOnly = document.getElementById('rule-manual-only');
    if (manualOnly?.checked) { manualOnly.checked = false; toggleManualOnly(); }
    const rowId = conditionRowCount;
    const row = document.createElement('div');
    row.className = 'row condition-row';
    row.style.gap = '8px';
    row.id = `condition-row-${rowId}`;
    row.innerHTML = `
        <button type="button" class="field sel cond-variable-btn" onclick="openConditionVarSelector(${rowId})" title="${t('automation.selectVariable')}" style="width:190px"></button>
        <input type="hidden" class="cond-variable">
        <select class="field cond-operator" style="width:90px" aria-label="${t('automation.conditionLogic')}"></select>
        <input type="text" class="field cond-value" style="width:120px" placeholder="${t('automation.conditionValue')}" aria-label="${t('automation.conditionValue')}">
        <button type="button" class="btn icon sm quiet" onclick="this.closest('.row').remove()" aria-label="${t('securityPage.remove')}" title="${t('securityPage.remove')}"><svg class="i"><use href="#ri-close-line"/></svg></button>
    `;
    fillConditionEditor(row, 'trigger', condition);
    container.appendChild(row);
    conditionRowCount++;
}

// 用于存储当前正在配置的条件行 ID
let currentConditionVarRowId = null;

/**
 * 打开触发条件变量选择器
 */
async function openConditionVarSelector(rowId) {
    currentConditionVarRowId = rowId;
    buildVarSelectModal(t('automation.selectTriggerVarTitle'), 'ruleCondition');
    await loadVarSelectList();
}

/**
 * 处理触发条件变量选择
 */
function handleConditionVarSelect(varName) {
    if (currentConditionVarRowId === null) return;
    
    const row = document.getElementById(`condition-row-${currentConditionVarRowId}`);
    if (!row) return;
    
    const varBtn = row.querySelector('.cond-variable-btn');
    const varInput = row.querySelector('.cond-variable');
    
    if (varBtn) varBtn.textContent = varName;
    if (varInput) varInput.value = varName;
    
    // 关闭模态框
    closeModal('variable-select-modal');
    
    currentConditionVarRowId = null;
}

// 动作行计数器
let actionRowCount = 0;

// 缓存的动作模板列表
let cachedActionTemplates = [];

/**
 * 加载动作模板列表
 */
async function loadActionTemplatesForRule() {
    try {
        const result = await api.call('automation.actions.list', {});
        if (result.code === 0 && result.data?.templates) {
            cachedActionTemplates = result.data.templates;
        }
    } catch (e) {
        console.error('加载动作模板失败:', e);
        cachedActionTemplates = [];
    }
}

/**
 * 添加动作模板选择行
 * @param {string} templateId - 预选中的模板 ID
 * @param {number} delayMs - 预填充的延迟时间
 * @param {string} repeatMode - 重复模式: 'once' | 'while_true' | 'count'
 * @param {number} repeatCount - 重复次数（当 repeatMode='count' 时）
 * @param {number} repeatIntervalMs - 重复间隔毫秒
 * @param {Object|null} condition - 动作条件配置 {variable, operator, value}
 */
async function addActionTemplateRow(templateId = '', delayMs = 0, repeatMode = 'once', repeatCount = 1, repeatIntervalMs = 1000, condition = null, original = null) {
    const container = document.getElementById('actions-container');
    
    if (original && !templateId) {
        const row = document.createElement('div'); row.className = 'row action-row';
        row._originalAction = structuredClone(original); row.dataset.inline = 'true';
        const label = document.createElement('pre'); label.className = 'mono'; label.style.cssText = 'margin:0;white-space:pre-wrap;font-size:12px'; label.textContent = JSON.stringify(original, null, 2);
        row.appendChild(label); container.querySelector('.empty-hint')?.remove(); container.appendChild(row);
        return;
    }
    // 先加载模板列表
    await loadActionTemplatesForRule();
    
    if (cachedActionTemplates.length === 0 && !original) {
        showToast(typeof t === 'function' ? t('toast.createActionFirst') : '请先创建动作模板', 'warning');
        return;
    }
    
    // 移除空提示
    const emptyP = container.querySelector('.empty-hint');
    if (emptyP) emptyP.remove();
    
    const row = document.createElement('div');
    row.className = 'row action-row template-select-row';
    row.style.cssText = 'gap:8px;flex-wrap:wrap;padding:8px 0';
    row._originalAction = original ? structuredClone(original) : null;
    row.id = `action-row-${actionRowCount}`;
    
    // 构建模板选项
    let optionsHtml = '<option value="">' + t('promptRepair.chooseAction') + '</option>';
    cachedActionTemplates.forEach(tpl => {
        const typeLabel = getActionTypeLabel(tpl.type);
        const selected = tpl.id === templateId ? 'selected' : '';
        optionsHtml += `<option value="${escapeHtml(tpl.id)}" ${selected}>${escapeHtml(tpl.name || tpl.id)} (${typeLabel})</option>`;
    });
    
    if (templateId && !cachedActionTemplates.some(tpl => tpl.id === templateId))
        optionsHtml += `<option value="${escapeHtml(templateId)}" selected>${escapeHtml(templateId)} (${runtimeText('referenceUnresolved')})</option>`;
    const rowId = actionRowCount;
    const showRepeatOptions = repeatMode !== 'once';
    const hasCondition = condition && condition.variable;
    
    const lab = 'display:flex;align-items:center;gap:6px';
    row.innerHTML = `
        <select class="field action-template-id" onchange="updateActionTemplatePreview(this)" style="width:300px" aria-label="${t('promptRepair.chooseAction')}">
            ${optionsHtml}
        </select>
        <button type="button" class="btn icon sm quiet" onclick="this.closest('.action-row').remove()" aria-label="${t('securityPage.remove')}" title="${t('securityPage.remove')}"><svg class="i"><use href="#ri-close-line"/></svg></button>
        <div class="t-note" style="flex-basis:100%;display:flex;gap:12px;align-items:center;flex-wrap:wrap">
            <label style="${lab}">${t('automation.delay')}
                <input type="number" class="field sm num action-delay" placeholder="0" value="${delayMs}" min="0" style="width:70px">ms
            </label>
            <label style="${lab}">${t('automationPage.execute')}
                <select class="field sm action-repeat-mode" onchange="toggleRepeatOptions(${rowId})">
                    <option value="once" ${repeatMode === 'once' ? 'selected' : ''}>${t('automationPage.repeatOnce')}</option>
                    <option value="while_true" ${repeatMode === 'while_true' ? 'selected' : ''}>${t('automationPage.repeatWhileTrue')}</option>
                    <option value="count" ${repeatMode === 'count' ? 'selected' : ''}>${t('automationPage.repeatCount')}</option>
                </select>
            </label>
            <span class="repeat-options" id="repeat-options-${rowId}" style="display:${showRepeatOptions ? 'flex' : 'none'};gap:12px;align-items:center">
                <label class="repeat-count-label" style="display:${repeatMode === 'count' ? 'flex' : 'none'};align-items:center;gap:6px">${t('automationPage.repeatTimes')}
                    <input type="number" class="field sm num action-repeat-count" value="${repeatCount}" min="1" max="100" style="width:60px">
                </label>
                <label style="${lab}">${t('automationPage.interval')}
                    <input type="number" class="field sm num action-repeat-interval" value="${repeatIntervalMs}" min="100" style="width:80px">ms
                </label>
            </span>
        </div>
        <div class="t-note" style="flex-basis:100%;display:flex;gap:8px;align-items:center;flex-wrap:wrap">
            <label style="${lab}">
                <input type="checkbox" class="switch action-has-condition" role="switch" onchange="toggleActionCondition(${rowId})" ${hasCondition ? 'checked' : ''}>
                ${t('automationPage.execCondition')}
            </label>
            <span class="action-condition-fields" id="action-condition-${rowId}" style="display:${hasCondition ? 'flex' : 'none'};gap:6px;align-items:center">
                <button type="button" class="field sm sel action-condition-var-btn" onclick="openActionConditionVarSelector(${rowId})" title="${t('automation.selectVariable')}" style="width:150px"></button>
                <input type="hidden" class="action-condition-variable">
                <select class="field sm action-condition-operator" style="width:90px">
                </select>
                <input type="text" class="field sm action-condition-value" placeholder="${t('automationPage.value')}" aria-label="${t('automationPage.value')}" style="width:90px">
            </span>
        </div>
    `;
    
    fillConditionEditor(row, 'action', condition);
    container.appendChild(row);
    actionRowCount++;
}

/**
 * 获取动作类型标签
 */
function getActionTypeLabel(type) {
    const labels = {
        'cli': 'CLI',
        'ssh_cmd_ref': 'SSH',
        'led': 'LED',
        'log': t('common.log'),
        'set_var': t('common.variable'),
        'webhook': 'Webhook',
        'gpio': 'GPIO',
        'device_ctrl': t('common.devices')
    };
    return labels[type] || type;
}

/**
 * 切换重复执行选项的显示
 */
function toggleRepeatOptions(rowId) {
    const row = document.getElementById(`action-row-${rowId}`);
    if (!row) return;
    
    const modeSelect = row.querySelector('.action-repeat-mode');
    const repeatOptions = document.getElementById(`repeat-options-${rowId}`);
    const countLabel = repeatOptions?.querySelector('.repeat-count-label');
    
    if (!modeSelect || !repeatOptions) return;
    
    const mode = modeSelect.value;
    
    if (mode === 'once') {
        repeatOptions.style.display = 'none';
    } else {
        repeatOptions.style.display = 'flex';
        if (countLabel) {
            countLabel.style.display = mode === 'count' ? 'flex' : 'none';
        }
    }
}

/**
 * 更新动作模板预览（可选）
 */
function updateActionTemplatePreview(selectElement) {
    const templateId = selectElement.value;
    if (!templateId) return;
    
    const tpl = cachedActionTemplates.find(t => t.id === templateId);
    if (tpl) {
        console.log('选择动作模板:', tpl);
    }
}

/**
 * 切换动作条件配置的显示
 */
function toggleActionCondition(rowId) {
    const row = document.getElementById(`action-row-${rowId}`);
    if (!row) return;
    
    const checkbox = row.querySelector('.action-has-condition');
    const conditionFields = document.getElementById(`action-condition-${rowId}`);
    
    if (!checkbox || !conditionFields) return;
    
    conditionFields.style.display = checkbox.checked ? 'flex' : 'none';
}

// 用于存储当前正在配置条件的动作行 ID
let currentConditionRowId = null;

/**
 * 打开动作条件变量选择器
 */
async function openActionConditionVarSelector(rowId) {
    currentConditionRowId = rowId;
    
    // 设置回调模式标记
    window._actionConditionMode = true;
    
    // 复用现有的变量选择模态框（通过创建一个临时 input）
    // showVariableSelectModal 需要一个 inputId，我们用特殊标记来识别
    await showVariableSelectModalForCondition();
}

/**
 * 为动作条件显示变量选择模态框
 */
async function showVariableSelectModalForCondition() {
    buildVarSelectModal(t('automation.selectConditionVar'), 'actionCondition');
    await loadVarSelectList();
}

/**
 * 处理动作条件变量选择
 */
function handleActionConditionVarSelect(varName) {
    if (currentConditionRowId === null) return;
    
    const row = document.getElementById(`action-row-${currentConditionRowId}`);
    if (!row) return;
    
    const varBtn = row.querySelector('.action-condition-var-btn');
    const varInput = row.querySelector('.action-condition-variable');
    
    if (varBtn) varBtn.textContent = varName;
    if (varInput) varInput.value = varName;
    
    // 关闭模态框
    closeModal('variable-select-modal');
    
    currentConditionRowId = null;
    window._actionConditionMode = false;
}

// 保留旧的 addActionRow 和 updateActionFields 用于兼容，但标记为废弃
/**
 * @deprecated 使用 addActionTemplateRow 代替
 */
function addActionRow() {
    console.warn('addActionRow 已废弃，请使用 addActionTemplateRow');
    addActionTemplateRow();
}

/**
 * 根据动作类型更新参数字段
 */
function updateActionFields(selectElement) {
    const row = selectElement.closest('.action-row');
    const paramsContainer = row.querySelector('.action-params');
    const type = selectElement.value;
    
    switch (type) {
        case 'led':
            paramsContainer.innerHTML = `
                <select class="input action-led-device">
                    <option value="board">Board</option>
                    <option value="matrix">Matrix</option>
                    <option value="touch">Touch</option>
                </select>
                <input type="number" class="input action-led-index" placeholder="${t('automationPage.indexPlaceholder')}" value="255" min="0" max="255" style="width:70px">
                <input type="text" class="input action-led-color" placeholder="#RRGGBB" value="#FF0000" style="width:90px">
            `;
            break;
        case 'gpio':
            paramsContainer.innerHTML = `
                <input type="number" class="input action-gpio-pin" placeholder="Pin" value="0" min="0" max="48" style="width:60px">
                <select class="input action-gpio-level">
                    <option value="true">${t('common.high')}</option>
                    <option value="false">${t('common.low')}</option>
                </select>
                <input type="number" class="input action-gpio-pulse" placeholder="${t('automationPage.pulseMsPlaceholder')}" value="0" min="0" style="width:80px">
            `;
            break;
        case 'device':
            paramsContainer.innerHTML = `
                <select class="input action-device-name">
                    <option value="agx0">AGX 0</option>
                    <option value="lpmu0">LPMU 0</option>
                </select>
                <select class="input action-device-action">
                    <option value="power_on">${t('device.powerOn')}</option>
                    <option value="power_off">${t('device.powerOff')}</option>
                    <option value="reset">${t('common.restart')}</option>
                    <option value="force_off">${t('promptRepair.forceOff')}</option>
                </select>
            `;
            break;
        case 'set_var':
            paramsContainer.innerHTML = `
                <input type="text" class="input action-setvar-name" placeholder="${typeof t === 'function' ? t('automationPage.varNamePlaceholder') : '变量名'}" style="width:120px">
                <input type="text" class="input action-setvar-value" placeholder="${typeof t === 'function' ? t('automationPage.jsonValuePlaceholder') : '值 (JSON)'}" style="flex:1">
            `;
            break;
        case 'log':
            paramsContainer.innerHTML = `
                <select class="input action-log-level" style="width:100px">
                    <option value="3">INFO</option>
                    <option value="4">WARN</option>
                    <option value="5">ERROR</option>
                </select>
                <input type="text" class="input action-log-message" placeholder="${typeof t === 'function' ? t('automationPage.logMessagePlaceholder') : '日志消息'}" style="flex:1">
            `;
            break;
        case 'webhook':
            paramsContainer.innerHTML = `
                <select class="input action-webhook-method" style="width:80px">
                    <option value="POST">POST</option>
                    <option value="GET">GET</option>
                    <option value="PUT">PUT</option>
                </select>
                <input type="text" class="input action-webhook-url" placeholder="URL" style="flex:1">
                <input type="text" class="input action-webhook-body" placeholder='Body JSON' style="width:120px">
            `;
            break;
    }
}

/**
 * 提交添加/更新规则
 * 规则只引用动作模板 ID，不再内联定义动作
 * @param {string} originalId - 编辑模式时传入原规则 ID
 */
async function submitAddRule(originalId = null) {
    const modal = document.getElementById('add-rule-modal');
    if (modal?.dataset.saving === 'true' || modal?.dataset.loading === 'true') return;
    const isEdit = !!originalId;
    const id = document.getElementById('rule-id').value.trim();
    const name = document.getElementById('rule-name').value.trim();
    const icon = document.getElementById('rule-icon').value || 'ri-thunderstorms-line';
    const logic = document.getElementById('rule-logic').value;
    const cooldown = parseInt(document.getElementById('rule-cooldown').value) || 0;
    const enabled = document.getElementById('rule-enabled').checked;
    const manualTrigger = document.getElementById('rule-manual-only')?.checked || false;
    
    if (!id) {
        fieldError('rule-id', t('automation.pleaseEnterRuleId'));
        return;
    }
    if (!name) {
        fieldError('rule-name', t('automation.pleaseEnterRuleName'));
        return;
    }
    
    // Dormant trigger conditions are preserved when manual-only is selected.
    const conditions = [];
    for (const row of document.querySelectorAll('#add-rule-modal .condition-row')) {
        const condition = readConditionEditor(row, 'trigger');
        if (!condition) return;
        conditions.push(condition);
    }

    // 收集动作模板引用（包含 template_id、delay_ms、重复选项和动作条件）
    const actions = [];
    for (const row of document.querySelectorAll('#add-rule-modal .action-row')) {
        if (row.dataset.inline === 'true') { actions.push(structuredClone(row._originalAction)); continue; }
        const templateId = row.querySelector('.action-template-id')?.value;
        const delay_ms = parseInt(row.querySelector('.action-delay')?.value) || 0;
        const repeat_mode = row.querySelector('.action-repeat-mode')?.value || 'once';
        const repeat_count = parseInt(row.querySelector('.action-repeat-count')?.value) || 1;
        const repeat_interval_ms = Number(row.querySelector('.action-repeat-interval')?.value ?? 1000);
        
        // 收集动作条件
        const hasCondition = row.querySelector('.action-has-condition')?.checked;
        if (hasCondition && !templateId) { fieldError('actions-container', t('ui.alertSelectAction')); return; }
        
        if (templateId) {
            const actionRef = row._originalAction?.template_id === templateId ? structuredClone(row._originalAction) : {};
            actionRef.template_id = templateId;
            actionRef.delay_ms = delay_ms;
            actionRef.repeat_mode = repeat_mode;
            actionRef.repeat_count = repeat_count;
            actionRef.repeat_interval_ms = repeat_interval_ms;
            delete actionRef.condition;
            
            // 只有非单次执行时才添加重复参数
            if (repeat_mode !== 'once') {
                actionRef.repeat_mode = repeat_mode;
                actionRef.repeat_interval_ms = repeat_interval_ms;
                if (repeat_mode === 'count') {
                    actionRef.repeat_count = repeat_count;
                }
            }
            
            if (hasCondition) {
                const condition = readConditionEditor(row, 'action');
                if (!condition) return;
                actionRef.condition = condition;
            }

            actions.push(actionRef);
        }
    }
    
    if (actions.length === 0) {
        fieldError('actions-container', t('ui.alertSelectAction'));
        return;
    }
    
    const params = {
        id,
        name,
        icon,
        logic,
        cooldown_ms: cooldown,
        enabled,
        manual_trigger: manualTrigger,
        show_on_dashboard: document.getElementById('rule-show-dashboard').checked,
        allow_manual_trigger: document.getElementById('rule-allow-manual').checked,
        expected_revision: Number(document.getElementById('rule-revision').value),
        conditions,
        actions
    };
    
    if (isEdit) {
        for (const [element, field] of [['rule-show-dashboard', 'show_on_dashboard'], ['rule-allow-manual', 'allow_manual_trigger']]) {
            const input = document.getElementById(element);
            if (input.dataset.present === 'false' && input.dataset.dirty !== 'true') delete params[field];
        }
    }
    modal.dataset.saving = 'true';
    try {
        const result = await api.call(isEdit ? 'automation.rules.update' : 'automation.rules.add', params);
        if (result.code === 0) {
            showToast(runtimeText(result.data?.mirror_synced === false ? 'mirrorWarning' : 'saved'), result.data?.mirror_synced === false ? 'warning' : 'success');
            closeModal('add-rule-modal');
            await Promise.all([refreshRules(), refreshAutomationStatus()]);
        } else {
            showToast(runtimeSaveError(result), 'error');
        }
    } catch (error) {
        showToast(runtimeText('saveUnknown'), 'warning');
        // Re-read once, never replay the write. Keep the editor and its draft.
        try {
            const current = await api.call('automation.rules.get', { id });
            if (current.code === 0) document.getElementById('rule-revision').dataset.observedRevision = current.data.revision;
        } catch (_) { /* Remain explicitly uncertain. */ }
    } finally { modal.dataset.saving = 'false'; }
}

/**
 * 关闭模态框
 */
function closeModal(modalId) {
    const modal = document.getElementById(modalId);
    if (modal) {
        modal.classList.remove('show');
        setTimeout(() => modal.remove(), 300);
    }
}

// 导出自动化页面函数
window.refreshAutomationStatus = refreshAutomationStatus;
window.automationControl = automationControl;
window.refreshRules = refreshRules;
window.toggleRule = toggleRule;
window.triggerRule = triggerRule;
window.deleteRule = deleteRule;
window.editRule = editRule;
window.refreshSources = refreshSources;
window.toggleSource = toggleSource;
window.deleteSource = deleteSource;
window.showAddSourceModal = showAddSourceModal;
window.switchSourceType = switchSourceType;
window.updateSourceTypeFields = updateSourceTypeFields;
window.loadSshHostsForSource = loadSshHostsForSource;
window.onSshHostChangeForSource = onSshHostChangeForSource;
window.onSshCmdChange = onSshCmdChange;
window.submitAddSource = submitAddSource;
window.showAddRuleModal = showAddRuleModal;
window.addConditionRow = addConditionRow;
window.openConditionVarSelector = openConditionVarSelector;
window.handleConditionVarSelect = handleConditionVarSelect;
window.addActionRow = addActionRow;
window.updateActionFields = updateActionFields;
window.submitAddRule = submitAddRule;
window.closeModal = closeModal;
window.toggleManualOnly = toggleManualOnly;
window.toggleRepeatOptions = toggleRepeatOptions;
window.toggleActionCondition = toggleActionCondition;
window.openActionConditionVarSelector = openActionConditionVarSelector;
window.handleActionConditionVarSelect = handleActionConditionVarSelect;
window.showVariableSelectModalForCondition = showVariableSelectModalForCondition;
// 动作模板管理
window.refreshActions = refreshActions;
window.showAddActionModal = showAddActionModal;
window.updateActionTypeFields = updateActionTypeFields;
window.submitAction = submitAction;
window.testAction = testAction;
window.editAction = editAction;
window.deleteAction = deleteAction;
window.showImageSelectModal = showImageSelectModal;
window.selectImageItem = selectImageItem;
window.browseActionImages = browseActionImages;
window.browseActionQrBg = browseActionQrBg;
window.showVariableSelectModal = showVariableSelectModal;
window.filterVariableList = filterVariableList;
window.selectVariable = selectVariable;
window.toggleVarGroup = toggleVarGroup;
// 规则图标相关
window.switchRuleIconType = switchRuleIconType;
window.selectRuleIcon = selectRuleIcon;
window.selectRuleIconFromInput = selectRuleIconFromInput;
window.browseRuleIconImage = browseRuleIconImage;
window.clearRuleIconImage = clearRuleIconImage;
window.updateRuleIconPreview = updateRuleIconPreview;
// 快捷操作
window.refreshQuickActions = refreshQuickActions;
window.triggerQuickAction = triggerQuickAction;

/*===========================================================================*/
/*              Automation Export/Import Functions                             */
/*===========================================================================*/

/**
 * 显示导出数据源配置模态框
 */
function showExportSourceModal(sourceId) {
    let modal = document.getElementById('export-source-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'export-source-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = exportSheet('source', t('automation.exportSourceTitle'), t('automation.exportSourceDesc', {id: escapeHtml(sourceId)}), t('securityPage.targetCertHint'), 'hideExportSourceModal', 'doExportSource');
    
    modal.dataset.exportId = sourceId;
    modal.classList.remove('hidden');
}

function hideExportSourceModal() {
    const modal = document.getElementById('export-source-modal');
    if (modal) modal.classList.add('hidden');
}

async function doExportSource(sourceId) {
    const certText = document.getElementById('export-source-cert').value.trim();
    const resultBox = document.getElementById('export-source-result');
    const exportBtn = document.getElementById('export-source-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = (typeof t === 'function' ? t('securityPage.generatingPack') : '正在生成配置包...');
    exportBtn.disabled = true;
    
    try {
        const params = { id: sourceId };
        if (certText) params.recipient_cert = certText;
        
        const result = await api.call('automation.sources.export', params);
        if (result.code !== 0) throw new Error(result.message || (typeof t === 'function' ? t('toast.exportFailed') : '导出失败'));
        
        const data = result.data;
        if (!data?.tscfg) throw new Error(typeof t === 'function' ? t('toast.invalidResponse') : '无效的响应数据');
        
        // 下载文件
        const blob = new Blob([data.tscfg], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = data.filename || `source_${sourceId}.tscfg`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        
        resultBox.className = 'result-box success';
        resultBox.textContent = (typeof t === 'function' ? t('toast.exportSuccess') : '导出成功');
        showToast((typeof t === 'function' ? t('toast.exportedSourceConfig', {filename: data.filename}) : `已导出数据源配置: ${data.filename}`), 'success');
        setTimeout(() => hideExportSourceModal(), 1000);
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    } finally {
        exportBtn.disabled = false;
    }
}

/**
 * 显示导入数据源配置模态框
 */
function showImportSourceModal() {
    let modal = document.getElementById('import-source-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'import-source-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = importSheet('source', t('automation.importSourceTitle'), t('automation.importSourceDesc'), 'previewSourceImport', 'confirmSourceImport', 'hideImportSourceModal');
    
    window._importSourceTscfg = null;
    const statusEl = document.getElementById('import-source-file-status');
    if (statusEl) statusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件';
    modal.classList.remove('hidden');
}

function hideImportSourceModal() {
    const modal = document.getElementById('import-source-modal');
    if (modal) modal.classList.add('hidden');
    window._importSourceTscfg = null;
}

async function previewSourceImport() {
    const fileInput = document.getElementById('import-source-file');
    const resultBox = document.getElementById('import-source-result');
    const step2 = document.getElementById('import-source-step2');
    const previewDiv = document.getElementById('import-source-preview');
    const importBtn = document.getElementById('import-source-btn');
    const statusEl = document.getElementById('import-source-file-status');
    
    if (!fileInput.files || !fileInput.files[0]) {
        if (statusEl) statusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件';
        return;
    }
    
    const file = fileInput.files[0];
    if (statusEl) statusEl.textContent = file.name;
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = (typeof t === 'function' ? t('ssh.verifyingPack') : '正在验证配置包...');
    importBtn.disabled = true;
    previewDiv.innerHTML = importPlaceholder('source');
    
    try {
        const content = await file.text();
        window._importSourceTscfg = content;
        window._importSourceFilename = file.name;
        
        const result = await api.call('automation.sources.import', { 
            tscfg: content,
            filename: file.name,
            preview: true
        });
        
        if (result.code === 0 && result.data?.valid) {
            const data = result.data;
            renderImportPreview('source', data, t('automation.packTypeSource'));
            resultBox.className = 'result-box success';
            resultBox.textContent = typeof t === 'function' ? t('ssh.signatureVerified') : '签名验证通过';
            importBtn.disabled = false;
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (result.message || t('ssh.cannotVerifyPack'));
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    }
}

async function confirmSourceImport() {
    const overwrite = document.getElementById('import-source-overwrite').checked;
    const resultBox = document.getElementById('import-source-result');
    const importBtn = document.getElementById('import-source-btn');
    
    if (!window._importSourceTscfg) {
        showToast((typeof t === 'function' ? t('toast.selectFileFirst') : '请先选择文件'), 'error');
        return;
    }
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = typeof t === 'function' ? t('ssh.savingConfig') : '正在保存配置...';
    importBtn.disabled = true;
    
    try {
        const params = { 
            tscfg: window._importSourceTscfg,
            filename: window._importSourceFilename,
            overwrite: overwrite
        };
        
        const result = await api.call('automation.sources.import', params);
        
        if (result.code === 0) {
            const data = result.data;
            if (data?.exists && !data?.imported) {
                resultBox.className = 'result-box warning';
                resultBox.textContent = typeof t === 'function' ? t('securityPage.configExistsCheckOverwrite', { id: data.id }) : `配置 ${data.id} 已存在，请勾选「覆盖」选项`;
                importBtn.disabled = false;
            } else {
                resultBox.className = 'result-box success';
                resultBox.innerHTML = `${typeof t === 'function' ? t('securityPage.savedConfig') : 'Saved config'}: <code>${escapeHtml(data?.id)}</code><br><small style="color:#6b7280">${typeof t === 'function' ? t('securityPage.restartToApply') : 'Restart to apply'}</small>`;
                showToast(typeof t === 'function' ? t('toast.configImported') : '已导入配置，重启后生效', 'success');
                setTimeout(() => hideImportSourceModal(), 2000);
            }
        } else {
            resultBox.className = 'result-box error';
resultBox.textContent = (result.message || (typeof t === 'function' ? t('toast.importFailed') : '导入失败'));
                importBtn.disabled = false;
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
        importBtn.disabled = false;
    }
}

/**
 * 显示导出规则配置模态框
 */
function showExportRuleModal(ruleId) {
    let modal = document.getElementById('export-rule-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'export-rule-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = exportSheet('rule', t('ruleConfig.exportTitle'), t('ruleConfig.exportDesc', {id: escapeHtml(ruleId)}), t('ruleConfig.certHint'), 'hideExportRuleModal', 'doExportRule');
    
    modal.dataset.exportId = ruleId;
    modal.classList.remove('hidden');
}

function hideExportRuleModal() {
    const modal = document.getElementById('export-rule-modal');
    if (modal) modal.classList.add('hidden');
}

async function doExportRule(ruleId) {
    const certText = document.getElementById('export-rule-cert').value.trim();
    const resultBox = document.getElementById('export-rule-result');
    const exportBtn = document.getElementById('export-rule-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = typeof t === 'function' ? t('securityPage.generatingPack') : '正在生成配置包...';
    exportBtn.disabled = true;
    
    try {
        const params = { id: ruleId };
        if (certText) params.recipient_cert = certText;
        
        const result = await api.call('automation.rules.export', params);
        if (result.code !== 0) throw new Error(result.message || t('toast.exportFailed'));
        
        const data = result.data;
        if (!data?.tscfg) throw new Error(t('toast.invalidResponse'));
        
        // 下载文件
        const blob = new Blob([data.tscfg], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = data.filename || `rule_${ruleId}.tscfg`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        
        resultBox.className = 'result-box success';
        resultBox.textContent = typeof t === 'function' ? t('toast.exportSuccess') : '导出成功';
        showToast(typeof t === 'function' ? t('toast.ruleConfigExported', { filename: data.filename }) : `已导出规则配置: ${data.filename}`, 'success');
        setTimeout(() => hideExportRuleModal(), 1000);
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    } finally {
        exportBtn.disabled = false;
    }
}

/**
 * 显示导入规则配置模态框
 */
function showImportRuleModal() {
    ++rulePackPreviewAttempt; rulePackPreview = null;
    let modal = document.getElementById('import-rule-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'import-rule-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = importSheet('rule', t('automation.importRuleTitle'), t('automation.importRuleDesc'), 'previewRuleImport', 'confirmRuleImport', 'hideImportRuleModal');
    
    window._importRuleTscfg = null;
    const ruleStatusEl = document.getElementById('import-rule-file-status');
    if (ruleStatusEl) ruleStatusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件';
    modal.classList.remove('hidden');
}

function hideImportRuleModal() {
    ++rulePackPreviewAttempt; rulePackPreview = null;
    const modal = document.getElementById('import-rule-modal');
    if (modal) modal.classList.add('hidden');
    window._importRuleTscfg = null;
}

let rulePackPreviewAttempt = 0;
let rulePackPreview = null;
function rulePackFailure(result) {
    const token = result?.data?.error_code || result?.rawMessage || result?.error || result?.message;
    const key = 'rulePack.errors.' + token;
    const text = t(key);
    return text !== key ? text : t('rulePack.errors.invalid_pack');
}
async function previewRuleImport() {
    const attempt = ++rulePackPreviewAttempt;
    rulePackPreview = null;
    window._importRuleTscfg = null;
    const input = document.getElementById('import-rule-file');
    const box = document.getElementById('import-rule-result');
    const button = document.getElementById('import-rule-btn');
    const preview = document.getElementById('import-rule-preview');
    button.disabled = true;
    if (!input.files?.[0]) return;
    const file = input.files[0];
    document.getElementById('import-rule-file-status').textContent = file.name;
    box.className = 'result-box'; box.textContent = t('ssh.verifyingPack');
    preview.innerHTML = importPlaceholder('rule');
    try {
        const bytes = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}).decode(await file.arrayBuffer());
        if (attempt !== rulePackPreviewAttempt) return;
        const result = await api.call('automation.rules.import', {tscfg: bytes, filename: file.name, preview: true});
        if (attempt !== rulePackPreviewAttempt) return;
        const data = result.data;
        if (result.code !== 0 || data?.valid !== true || data?.trusted !== true || data?.target_matches !== true ||
            !data.rule || data.rule.id !== data.id || !Array.isArray(data.rule.actions) || !data.rule.actions.length ||
            !Number.isInteger(data.expected_revision) || !Number.isInteger(data.expected_generation) ||
            !Number.isInteger(data.credential_generation) || !/^[0-9a-f]{64}$/.test(data.package_digest || '')) {
            box.className = 'result-box error'; box.textContent = rulePackFailure(result); return;
        }
        rulePackPreview = data;
        window._importRuleTscfg = bytes; window._importRuleFilename = file.name;
        renderImportPreview('rule', data, t('automation.packTypeRule'));
        const conditions = Array.isArray(data.rule.conditions) ? data.rule.conditions.length : data.rule.conditions?.items?.length || 0;
        preview.insertAdjacentHTML('afterbegin', row(t('common.name'), escapeHtml(data.rule.name || data.id)));
        preview.insertAdjacentHTML('beforeend', row(t('rulePack.summaryLabel'), escapeHtml(t('rulePack.summary', {conditions, actions: data.rule.actions?.length || 0}))));
        preview.insertAdjacentHTML('beforeend', `<details><summary>${escapeHtml(t('rulePack.content'))}</summary><pre style="white-space:pre-wrap;overflow-wrap:anywhere">${escapeHtml(JSON.stringify(data.rule, null, 2))}</pre></details>`);
        if (data.exists) preview.insertAdjacentHTML('beforeend', row(t('rulePack.overwriteLabel'), escapeHtml(t('rulePack.overwriteImpact', {revision: data.expected_revision}))));
        for (const warning of data.warnings || []) {
            const key = warning === 'dynamic_inputs' ? 'rulePack.dynamicInputs'
                : warning === 'dependency_disabled' ? 'rulePack.disabledDependency' : null;
            if (key) preview.insertAdjacentHTML('beforeend', row(t('securityPage.noteLabel'), escapeHtml(t(key))));
        }
        box.className = 'result-box success'; box.textContent = t('rulePack.verified');
        button.disabled = false;
    } catch (error) {
        if (attempt !== rulePackPreviewAttempt) return;
        box.className = 'result-box error'; box.textContent = t('rulePack.verifyFailed');
    }
}
async function confirmRuleImport() {
    const attempt = rulePackPreviewAttempt;
    const preview = rulePackPreview;
    const box = document.getElementById('import-rule-result');
    const button = document.getElementById('import-rule-btn');
    if (!preview || !window._importRuleTscfg) { box.textContent = t('rulePack.errors.preview_required'); return; }
    const bytes = window._importRuleTscfg;
    button.disabled = true;
    box.className = 'result-box'; box.textContent = t('ssh.savingConfig');
    let uncertain = false;
    try {
        const result = await api.call('automation.rules.import', {
            tscfg: bytes, filename: window._importRuleFilename,
            overwrite: document.getElementById('import-rule-overwrite').checked,
            expected_revision: preview.expected_revision, expected_generation: preview.expected_generation,
            credential_generation: preview.credential_generation, package_digest: preview.package_digest
        });
        if (attempt !== rulePackPreviewAttempt) return;
        const data = result.data;
        if (result.code === 0 && data?.saved === true && data?.durable === true && data?.runtime_applied === false) {
            box.className = data.cleanup_pending ? 'result-box warning' : 'result-box success';
            box.textContent = t(data.restart_required ? 'rulePack.saved' : 'rulePack.alreadyActive') + (data.cleanup_pending ? ' ' + t('rulePack.cleanupPending') : '');
            rulePackPreview = null;
            await refreshRules();
            return;
        }
        uncertain = result.code === 0 || data?.result_unknown === true || ['commit_unknown', 'recovery_required'].includes(result.rawMessage || result.error || result.message);
        if (!uncertain) {
            box.className = 'result-box error'; box.textContent = rulePackFailure(result);
            if ((result.rawMessage || result.error || result.message) === 'overwrite_required') button.disabled = false;
            else rulePackPreview = null;
            return;
        }
    } catch (error) { uncertain = true; }
    if (attempt !== rulePackPreviewAttempt) return;
    if (uncertain) {
        // Observe once; never replay an uncertain write.
        box.className = 'result-box warning'; box.textContent = t('rulePack.unknown');
        rulePackPreview = null;
        try {
            const state = await api.call('automation.rules.list');
            if (attempt !== rulePackPreviewAttempt) return;
            const saved = state.code === 0 && state.data?.loaded !== false && state.data?.recovery_required !== true && state.data?.rules?.find(r => r.id === preview.id && r.saved_exists !== false && r.pending_change !== 'delete' && r.package_digest === preview.package_digest);
            if (saved && Number.isInteger(saved.saved_revision) && saved.saved_generation >= preview.expected_generation) {
                box.className = 'result-box success'; box.textContent = t(saved.restart_required ? 'rulePack.saved' : 'rulePack.alreadyActive');
                await refreshRules();
            }
        } catch (error) { /* Keep the explicit unknown result. */ }
    }
}

/**
 * 显示导出动作模板配置模态框
 */
function showExportActionModal(actionId) {
    let modal = document.getElementById('export-action-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'export-action-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = exportSheet('action', t('automation.exportActionTitle'), t('automation.exportActionDesc', {actionId: escapeHtml(actionId)}), t('securityPage.targetCertHint'), 'hideExportActionModal', 'doExportAction');
    
    modal.dataset.exportId = actionId;
    modal.classList.remove('hidden');
}

function hideExportActionModal() {
    const modal = document.getElementById('export-action-modal');
    if (modal) modal.classList.add('hidden');
}

async function doExportAction(actionId) {
    const certText = document.getElementById('export-action-cert').value.trim();
    const resultBox = document.getElementById('export-action-result');
    const exportBtn = document.getElementById('export-action-btn');
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = (typeof t === 'function' ? t('securityPage.generatingPack') : '正在生成配置包...');
    exportBtn.disabled = true;
    
    try {
        const params = { id: actionId };
        if (certText) params.recipient_cert = certText;
        
        const result = await api.call('automation.actions.export', params);
        if (result.code !== 0) throw new Error(result.message || (typeof t === 'function' ? t('toast.exportFailed') : '导出失败'));
        
        const data = result.data;
        if (!data?.tscfg) throw new Error(typeof t === 'function' ? t('toast.invalidResponse') : '无效的响应数据');
        
        // 下载文件
        const blob = new Blob([data.tscfg], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = data.filename || `action_${actionId}.tscfg`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        
        resultBox.className = 'result-box success';
        resultBox.textContent = (typeof t === 'function' ? t('toast.exportSuccess') : '导出成功');
        showToast((typeof t === 'function' ? t('toast.exportedActionTemplate', {filename: data.filename}) : `已导出动作模板: ${data.filename}`), 'success');
        setTimeout(() => hideExportActionModal(), 1000);
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    } finally {
        exportBtn.disabled = false;
    }
}

/**
 * 显示导入动作模板配置模态框
 */
function showImportActionModal() {
    let modal = document.getElementById('import-action-modal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'import-action-modal';
        modal.className = 'modal';
        document.body.appendChild(modal);
    }
    
    modal.innerHTML = importSheet('action', t('automation.importActionTitle'), t('automation.importActionDesc'), 'previewActionImport', 'confirmActionImport', 'hideImportActionModal');
    
    window._importActionTscfg = null;
    const actionStatusEl = document.getElementById('import-action-file-status');
    if (actionStatusEl) actionStatusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件';
    modal.classList.remove('hidden');
}

function hideImportActionModal() {
    const modal = document.getElementById('import-action-modal');
    if (modal) modal.classList.add('hidden');
    window._importActionTscfg = null;
}

async function previewActionImport() {
    const fileInput = document.getElementById('import-action-file');
    const resultBox = document.getElementById('import-action-result');
    const step2 = document.getElementById('import-action-step2');
    const previewDiv = document.getElementById('import-action-preview');
    const importBtn = document.getElementById('import-action-btn');
    const statusEl = document.getElementById('import-action-file-status');
    
    if (!fileInput.files || !fileInput.files[0]) {
        if (statusEl) statusEl.textContent = typeof t === 'function' ? t('common.noFileSelected') : '未选择任何文件';
        return;
    }
    
    const file = fileInput.files[0];
    if (statusEl) statusEl.textContent = file.name;
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = (typeof t === 'function' ? t('ssh.verifyingPack') : '正在验证配置包...');
    importBtn.disabled = true;
    previewDiv.innerHTML = importPlaceholder('action');
    
    try {
        const content = await file.text();
        window._importActionTscfg = content;
        window._importActionFilename = file.name;
        
        const result = await api.call('automation.actions.import', { 
            tscfg: content,
            filename: file.name,
            preview: true
        });
        
        if (result.code === 0 && result.data?.valid) {
            const data = result.data;
            renderImportPreview('action', data, t('automation.packTypeAction'));
            resultBox.className = 'result-box success';
            resultBox.textContent = typeof t === 'function' ? t('ssh.signatureVerified') : '签名验证通过';
            importBtn.disabled = false;
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (result.message || t('ssh.cannotVerifyPack'));
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
    }
}

async function confirmActionImport() {
    const overwrite = document.getElementById('import-action-overwrite').checked;
    const resultBox = document.getElementById('import-action-result');
    const importBtn = document.getElementById('import-action-btn');
    
    if (!window._importActionTscfg) {
        showToast((typeof t === 'function' ? t('toast.selectFileFirst') : '请先选择文件'), 'error');
        return;
    }
    
    resultBox.classList.remove('hidden', 'success', 'error');
    resultBox.textContent = typeof t === 'function' ? t('ssh.savingConfig') : '正在保存配置...';
    importBtn.disabled = true;
    
    try {
        const params = { 
            tscfg: window._importActionTscfg,
            filename: window._importActionFilename,
            overwrite: overwrite
        };
        
        const result = await api.call('automation.actions.import', params);
        
        if (result.code === 0) {
            const data = result.data;
            if (data?.exists && !data?.imported) {
                resultBox.className = 'result-box warning';
                resultBox.textContent = typeof t === 'function' ? t('securityPage.configExistsCheckOverwrite', { id: data.id }) : `配置 ${data.id} 已存在，请勾选「覆盖」选项`;
                importBtn.disabled = false;
            } else {
                resultBox.className = 'result-box success';
                resultBox.innerHTML = `${typeof t === 'function' ? t('securityPage.savedConfig') : 'Saved config'}: <code>${escapeHtml(data?.id)}</code><br><small style="color:#6b7280">${typeof t === 'function' ? t('securityPage.restartToApply') : 'Restart to apply'}</small>`;
                showToast(typeof t === 'function' ? t('toast.configImported') : '已导入配置，重启后生效', 'success');
                setTimeout(() => hideImportActionModal(), 2000);
            }
        } else {
            resultBox.className = 'result-box error';
            resultBox.textContent = (result.message || (typeof t === 'function' ? t('toast.importFailed') : '导入失败'));
            importBtn.disabled = false;
        }
    } catch (e) {
        resultBox.className = 'result-box error';
        resultBox.textContent = e.message;
        importBtn.disabled = false;
    }
}

// 导出导入函数
window.showExportSourceModal = showExportSourceModal;
window.hideExportSourceModal = hideExportSourceModal;
window.doExportSource = doExportSource;
window.showImportSourceModal = showImportSourceModal;
window.hideImportSourceModal = hideImportSourceModal;
window.previewSourceImport = previewSourceImport;
window.confirmSourceImport = confirmSourceImport;
window.showExportRuleModal = showExportRuleModal;
window.hideExportRuleModal = hideExportRuleModal;
window.doExportRule = doExportRule;
window.showImportRuleModal = showImportRuleModal;
window.hideImportRuleModal = hideImportRuleModal;
window.previewRuleImport = previewRuleImport;
window.confirmRuleImport = confirmRuleImport;
window.showExportActionModal = showExportActionModal;
window.hideExportActionModal = hideExportActionModal;
window.doExportAction = doExportAction;
window.showImportActionModal = showImportActionModal;
window.hideImportActionModal = hideImportActionModal;
window.previewActionImport = previewActionImport;
window.confirmActionImport = confirmActionImport;

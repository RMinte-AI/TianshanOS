/**
 * TianshanOS Web Terminal
 * 基于 xterm.js 的 Web 终端实现
 */

/**
 * 按需加载 xterm.js 及其插件（从 CDN）
 * 仅在首次打开终端页面时触发，后续调用直接返回
 */
const _xtermReady = (function() {
    let _promise = null;

    function loadResource(url, css) {
        return new Promise((resolve, reject) => {
            const selector = css ? 'link[href="' + url + '"]' : 'script[src="' + url + '"]';
            const existing = document.querySelector(selector);
            if (existing?.dataset.ready === 'true') { resolve(); return; }
            if (existing) existing.remove();
            const node = document.createElement(css ? 'link' : 'script');
            if (css) { node.rel = 'stylesheet'; node.href = url; } else node.src = url;
            const timer = setTimeout(() => finish(false), 15000);
            function finish(ok) {
                clearTimeout(timer); node.onload = node.onerror = null;
                if (ok) { node.dataset.ready = 'true'; resolve(); }
                else { node.remove(); reject(new Error('Terminal resource unavailable: ' + url)); }
            }
            node.onload = () => finish(true); node.onerror = () => finish(false);
            document.head.appendChild(node);
        });
    }
    return function ensureXtermLoaded() {
        if (_promise) return _promise;
        _promise = loadResource('https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.css', true)
            .then(() => loadResource('https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.min.js', false))
            .then(() => loadResource('https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.min.js', false))
            .catch(error => { _promise = null; throw error; });
        return _promise;
    };
})();

class WebTerminal {
    constructor(containerId) {
        this.containerId = containerId;
        this.terminal = null;
        this.fitAddon = null;
        this.ws = null;
        this.connected = false;
        this.inputBuffer = '';
        this.history = [];
        this.historyIndex = -1;
        this.prompt = 'tianshan> ';
        this.cursorPosition = 0;
        
        // SSH Shell 模式
        this.sshMode = false;
        this.sshConnecting = false;
        this.sshDisconnecting = false;
        this.restoring = false;
        this.restoreTimer = null;
        this.destroyed = false;
    }

    /**
     * 初始化终端
     */
    async init() {
        const container = document.getElementById(this.containerId);
        if (!container) {
            console.error('Terminal container not found:', this.containerId);
            return false;
        }

        // 按需加载 xterm.js（首次访问终端页面时从 CDN 拉取）
        try { await _xtermReady(); }
        catch (error) {
            if (this.destroyed) return false;
            console.error(error);
            container.replaceChildren();
            const message = document.createElement('p'); message.textContent = t('promptRepair.terminalLoadFailed');
            const retry = document.createElement('button'); retry.className = 'btn'; retry.textContent = t('promptRepair.retry');
            retry.onclick = () => loadTerminalPage();
            container.append(message, retry);
            return false;
        }
        if (this.destroyed) return false;

        // 创建 xterm.js 终端
        this.terminal = new Terminal({
            cursorBlink: true,
            cursorStyle: 'block',
            fontSize: 14,
            fontFamily: '"Cascadia Code", "Fira Code", "Source Code Pro", monospace',
            theme: {
                background: '#1e1e2e',
                foreground: '#cdd6f4',
                cursor: '#f5e0dc',
                cursorAccent: '#1e1e2e',
                selectionBackground: '#585b70',
                black: '#45475a',
                red: '#f38ba8',
                green: '#a6e3a1',
                yellow: '#f9e2af',
                blue: '#89b4fa',
                magenta: '#f5c2e7',
                cyan: '#94e2d5',
                white: '#bac2de',
                brightBlack: '#585b70',
                brightRed: '#f38ba8',
                brightGreen: '#a6e3a1',
                brightYellow: '#f9e2af',
                brightBlue: '#89b4fa',
                brightMagenta: '#f5c2e7',
                brightCyan: '#94e2d5',
                brightWhite: '#a6adc8'
            },
            scrollback: 1000,
            convertEol: true
        });

        // 加载 fit 插件用于自适应大小
        if (typeof FitAddon !== 'undefined') {
            this.fitAddon = new FitAddon.FitAddon();
            this.terminal.loadAddon(this.fitAddon);
        }

        this.terminal.open(container);
        
        if (this.fitAddon) {
            this.fitAddon.fit();
        }

        // 设置输入处理
        this.setupInputHandler();

        // 监听窗口大小变化
        window.addEventListener('resize', () => this.fit());

        // 显示欢迎信息
        this.writeln('\x1b[1;36m╔══════════════════════════════════════════╗\x1b[0m');
        this.writeln('\x1b[1;36m║\x1b[0m     \x1b[1;33mTianshanOS Web Terminal\x1b[0m              \x1b[1;36m║\x1b[0m');
        this.writeln('\x1b[1;36m╚══════════════════════════════════════════╝\x1b[0m');
        this.writeln('');
        this.writeln(typeof t === 'function' ? t('terminal.connecting') : '正在连接到设备...');

        return true;
    }

    /**
     * 设置输入处理
     */
    setupInputHandler() {
        this.terminal.onData(data => {
            if (!this.connected || this.sshConnecting || this.sshDisconnecting) return;
            
            // SSH Shell 模式
            if (this.sshMode) {
                // 检查 Ctrl+\ (0x1C) 退出 SSH
                if (data.charCodeAt(0) === 0x1C) {
                    this.writeln('\r\n' + (typeof t === 'function' ? t('terminal.exitSshDisplay') : '^\\  (退出 SSH shell)'));
                    this.exitSshShell();
                    return;
                }
                // 其他输入直接转发
                this.sendSshInput(data);
                return;
            }

            // 处理特殊字符
            for (let i = 0; i < data.length; i++) {
                const char = data[i];
                const code = char.charCodeAt(0);

                if (code === 13) { // Enter
                    this.handleEnter();
                } else if (code === 127 || code === 8) { // Backspace
                    this.handleBackspace();
                } else if (code === 3) { // Ctrl+C
                    this.handleInterrupt();
                } else if (code === 27) { // Escape sequence
                    // 处理方向键等
                    if (data.slice(i, i + 3) === '\x1b[A') { // Up
                        this.handleHistoryUp();
                        i += 2;
                    } else if (data.slice(i, i + 3) === '\x1b[B') { // Down
                        this.handleHistoryDown();
                        i += 2;
                    } else if (data.slice(i, i + 3) === '\x1b[C') { // Right
                        this.handleCursorRight();
                        i += 2;
                    } else if (data.slice(i, i + 3) === '\x1b[D') { // Left
                        this.handleCursorLeft();
                        i += 2;
                    }
                } else if (code === 12) { // Ctrl+L (clear)
                    this.terminal.clear();
                    this.writePrompt();
                    this.terminal.write(this.inputBuffer);
                } else if (code >= 32) { // 可打印字符
                    this.handlePrintable(char);
                }
            }
        });
    }

    handleEnter() {
        this.terminal.write('\r\n');
        const cmd = this.inputBuffer.trim();
        
        if (cmd) {
            // 添加到历史记录
            this.history.push(cmd);
            if (this.history.length > 100) {
                this.history.shift();
            }
            this.historyIndex = this.history.length;
            
            // 发送命令
            this.sendCommand(cmd);
        } else {
            this.writePrompt();
        }
        
        this.inputBuffer = '';
        this.cursorPosition = 0;
    }

    handleBackspace() {
        if (this.cursorPosition > 0) {
            this.inputBuffer = 
                this.inputBuffer.slice(0, this.cursorPosition - 1) + 
                this.inputBuffer.slice(this.cursorPosition);
            this.cursorPosition--;
            this.refreshLine();
        }
    }

    handleInterrupt() {
        this.terminal.write('^C\r\n');
        this.inputBuffer = '';
        this.cursorPosition = 0;
        
        // 发送中断信号
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({ type: 'terminal_interrupt' }));
        }
        
        this.writePrompt();
    }

    handleHistoryUp() {
        if (this.historyIndex > 0) {
            this.historyIndex--;
            this.inputBuffer = this.history[this.historyIndex];
            this.cursorPosition = this.inputBuffer.length;
            this.refreshLine();
        }
    }

    handleHistoryDown() {
        if (this.historyIndex < this.history.length - 1) {
            this.historyIndex++;
            this.inputBuffer = this.history[this.historyIndex];
        } else {
            this.historyIndex = this.history.length;
            this.inputBuffer = '';
        }
        this.cursorPosition = this.inputBuffer.length;
        this.refreshLine();
    }

    handleCursorLeft() {
        if (this.cursorPosition > 0) {
            this.cursorPosition--;
            this.terminal.write('\x1b[D');
        }
    }

    handleCursorRight() {
        if (this.cursorPosition < this.inputBuffer.length) {
            this.cursorPosition++;
            this.terminal.write('\x1b[C');
        }
    }

    handlePrintable(char) {
        this.inputBuffer = 
            this.inputBuffer.slice(0, this.cursorPosition) + 
            char + 
            this.inputBuffer.slice(this.cursorPosition);
        this.cursorPosition++;
        this.refreshLine();
    }

    refreshLine() {
        // 清除当前行并重新绘制
        this.terminal.write('\r\x1b[K');
        this.terminal.write(this.prompt + this.inputBuffer);
        // 移动光标到正确位置
        const backMoves = this.inputBuffer.length - this.cursorPosition;
        if (backMoves > 0) {
            this.terminal.write(`\x1b[${backMoves}D`);
        }
    }

    /**
     * 连接到 WebSocket
     */
    connect() {
        if (this.destroyed) return;
        if (this.ws) this.disconnect();
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.host}/ws`;
        
        const ws = new WebSocket(wsUrl);
        this.ws = ws;
        const current = () => !this.destroyed && this.ws === ws;
        this.pingInterval = null;
        
        ws.onopen = () => {
            if (!current()) return;
            console.log('Terminal WebSocket connected');
            // 发送终端启动请求
            ws.send(JSON.stringify({ type: 'terminal_start' }));
            
            // 启动心跳机制
            this.pingInterval = setInterval(() => {
                if (current() && ws.readyState === WebSocket.OPEN) {
                    ws.send(JSON.stringify({ type: 'ping' }));
                }
            }, 15000); // 每15秒发送心跳
        };
        
        ws.onmessage = (event) => {
            if (!current()) return;
            try {
                const msg = JSON.parse(event.data);
                this.handleMessage(msg);
            } catch (e) {
                console.error('Failed to parse message:', e);
            }
        };
        
        ws.onclose = (event) => {
            if (!current()) return;
            console.log('Terminal WebSocket disconnected, code:', event.code);
            this.connected = false;
            this.sshMode = this.sshConnecting = this.sshDisconnecting = false;
            if (this.restoring) this.finishRestore(false);
            
            // 清除心跳
            if (this.pingInterval) {
                clearInterval(this.pingInterval);
                this.pingInterval = null;
            }
            
            this.writeln('\r\n\x1b[1;31m' + (typeof t === 'function' ? t('terminal.connectionDisconnected') : '连接已断开') + '\x1b[0m');
            
            // 尝试重连（仅当标签页可见时发起，避免后台 tab 抢占前台会话）
            if (event.code !== 1000) { // 非正常关闭
                this.writeln('\x1b[33m' + (typeof t === 'function' ? t('terminal.reconnectIn') : '5秒后尝试重新连接...') + '\x1b[0m');
                const tryReconnect = () => {
                    if (!current() || this.connected) return;
                    if (document.visibilityState !== 'visible') {
                        document.addEventListener('visibilitychange', function handler() {
                            document.removeEventListener('visibilitychange', handler);
                            tryReconnect();
                        }, { once: true });
                        return;
                    }
                    this.writeln(typeof t === 'function' ? t('terminal.reconnecting') : '正在重新连接...');
                    this.connect();
                };
                setTimeout(tryReconnect, 5000);
            }
        };
        
        ws.onerror = (error) => {
            if (!current()) return;
            console.error('Terminal WebSocket error:', error);
            this.writeln('\r\n\x1b[1;31m' + (typeof t === 'function' ? t('terminal.connectionError') : '连接错误') + '\x1b[0m');
        };
    }

    /**
     * 处理 WebSocket 消息
     */
    finishRestore(ok, timeout = false) {
        clearTimeout(this.restoreTimer); this.restoreTimer = null;
        this.restoring = false;
        const key = ok ? 'terminalRestored' : timeout ? 'terminalRestoreTimeout' : 'terminalRestoreFailed';
        this.writeln(t('promptRepair.' + key));
        showToast(t('promptRepair.' + key), ok ? 'success' : 'warning');
    }

    handleMessage(msg) {
        if (this.destroyed) return;
        switch (msg.type) {
            case 'connected':
                if (this.sshMode || this.sshConnecting || this.sshDisconnecting) return;
                if (this.restoring) this.finishRestore(true);
                this.connected = true;
                this.prompt = msg.prompt || 'tianshan> ';
                this.writeln('\x1b[1;32m' + (typeof t === 'function' ? t('terminal.connected') : '已连接到设备') + '\x1b[0m');
                this.writeln(typeof t === 'function' ? t('terminal.helpHint', { help: '\x1b[1;33mhelp\x1b[0m' }) : '输入 \x1b[1;33mhelp\x1b[0m 查看可用命令');
                this.writeln('');
                this.writePrompt();
                break;
                
            case 'output':
                // 命令输出
                if (msg.data) {
                    this.write(msg.data);
                }
                break;
                
            case 'done':
                if (this.sshMode || this.sshConnecting || this.sshDisconnecting) return;
                // 命令执行完成
                this.writePrompt();
                break;
                
            case 'error': {
                const errMsg = msg.message || (typeof t === 'function' ? t('terminal.unknownError') : '未知错误');
                if (this.sshMode || this.sshConnecting || this.sshDisconnecting) {
                    this.writeln('\x1b[1;31m' + this.sshMessageText(errMsg) + '\x1b[0m');
                    if (this.sshConnecting && !this.sshMode) { this.sshConnecting = false; this.writePrompt(); }
                    break; // request failure is not an operation terminal
                }
                if (errMsg.indexOf('Not a terminal session') >= 0 && this.ws && this.ws.readyState === WebSocket.OPEN) {
                    if (!this.restoring) {
                        this.restoring = true; this.connected = false;
                        this.ws.send(JSON.stringify({type: 'terminal_start'}));
                        showToast(t('promptRepair.terminalRestoring'), 'info');
                        const restoringSocket = this.ws;
                        this.restoreTimer = setTimeout(() => { if (!this.destroyed && this.ws === restoringSocket && this.restoring) this.finishRestore(false, true); }, 10000);
                    }
                } else {
                    if (this.restoring) this.finishRestore(false);
                    this.writeln('\x1b[1;31m' + (typeof t === 'function' ? t('terminal.errorLabel') : '错误') + ': ' + errMsg + '\x1b[0m');
                    this.writePrompt();
                }
                break;
            }
                
            case 'pong':
                // 心跳响应，忽略
                break;
            
            case 'power_event':
                // 电压保护事件通知
                this.handlePowerEvent(msg);
                break;
            
            // SSH Shell 消息
            case 'ssh_status':
                this.handleSshStatus(msg);
                break;
            case 'ssh_output':
                // SSH Shell 输出
                if (msg.data) {
                    this.write(msg.data);
                }
                break;
            
            case 'session_closed':
                this.connected = false;
                this.sshMode = this.sshConnecting = this.sshDisconnecting = false;
                this.writeln('\x1b[33m' + (typeof t === 'function' ? t('terminal.sessionClosed') : '会话已关闭，请重新连接') + '\x1b[0m');
                break;
                
            default:
                console.log('Unknown message type:', msg.type);
        }
    }

    /**
     * 处理电压保护事件
     */
    handlePowerEvent(msg) {
        if (this.destroyed || !this.terminal) return;
        const state = msg.state || 'UNKNOWN';
        const voltage = msg.voltage ? msg.voltage.toFixed(2) : '?.??';
        const countdown = msg.countdown || 0;
        const event = msg.event || 'unknown';
        
        let notification = '';
        let color = '\x1b[33m'; // 默认黄色
        
        const _t = (key, params, fallback) => (typeof t === 'function' ? t(key, params) : fallback);
        switch (event) {
            case 'low_voltage':
                color = '\x1b[1;31m'; // 亮红色
                notification = _t('terminal.lowVoltageWarning', { voltage }, `⚠️  低电压警告! 电压: ${voltage}V - 开始关机倒计时`);
                break;
            case 'countdown_tick':
                if (countdown <= 10) {
                    color = '\x1b[1;31m'; // 亮红色
                } else if (countdown <= 30) {
                    color = '\x1b[33m'; // 黄色
                } else {
                    return; // 不显示每秒倒计时，只显示关键时刻
                }
                notification = _t('terminal.countdownTick', { countdown, voltage }, `⏱️  关机倒计时: ${countdown}秒 | 电压: ${voltage}V`);
                break;
            case 'shutdown_start':
                color = '\x1b[1;31m';
                notification = _t('terminal.shutdownStart', { voltage }, `🔴 正在执行关机... 电压: ${voltage}V`);
                break;
            case 'protected':
                color = '\x1b[35m'; // 紫色
                notification = _t('terminal.protected', {}, '🛡️  进入保护状态 | 等待电压恢复...');
                break;
            case 'recovery_start':
                color = '\x1b[36m'; // 青色
                notification = _t('terminal.recoveryStart', { voltage }, `🔄 电压恢复中: ${voltage}V | 等待稳定...`);
                break;
            case 'recovery_complete':
                color = '\x1b[1;32m'; // 亮绿色
                notification = _t('terminal.recoveryComplete', { voltage }, `✅ 电压恢复完成! ${voltage}V | 系统即将重启`);
                break;
            case 'debug_tick':
                // 调试模式：每秒显示状态
                color = '\x1b[36m'; // 青色
                notification = _t('terminal.debugTick', { state, voltage, countdown }, `📊 [调试] ${state} | 电压: ${voltage}V | 倒计时: ${countdown}s`);
                break;
            case 'state_changed':
                if (state === 'NORMAL') {
                    color = '\x1b[32m';
                    notification = _t('terminal.voltageNormal', { voltage }, `✓ 电压状态正常: ${voltage}V`);
                } else {
                    notification = _t('terminal.stateChanged', { state, voltage }, `状态变更: ${state} | 电压: ${voltage}V`);
                }
                break;
            default:
                notification = _t('terminal.powerEvent', { event, state, voltage }, `[电源] ${event}: 状态=${state}, 电压=${voltage}V`);
        }
        
        if (notification) {
            const canRestoreLocalInput =
                !this.destroyed &&
                this.connected &&
                this.ws &&
                this.ws.readyState === WebSocket.OPEN &&
                !this.sshMode &&
                !this.sshConnecting &&
                !this.sshDisconnecting &&
                !this.restoring;
            if (!canRestoreLocalInput) {
                this.writeln(`\r\n${color}${notification}\x1b[0m`);
                return;
            }
            // 保存当前输入状态
            const savedBuffer = this.inputBuffer;
            const savedPosition = this.cursorPosition;
            
            // 清除当前行，显示通知
            this.terminal.write('\r\x1b[K');
            this.writeln(`${color}${notification}\x1b[0m`);
            
            // 恢复提示符和输入
            this.terminal.write(this.prompt + savedBuffer);
            const backMoves = savedBuffer.length - savedPosition;
            if (backMoves > 0) {
                this.terminal.write(`\x1b[${backMoves}D`);
            }
        }
    }

    /**
     * 发送命令
     */
    sendCommand(command) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            // 检查是否是 SSH shell 命令
            if (this.parseSshCommand(command)) {
                return; // SSH 命令已处理
            }
            
            this.ws.send(JSON.stringify({
                type: 'terminal_input',
                data: command
            }));
        } else {
            this.writeln('\x1b[1;31m' + (typeof t === 'function' ? t('terminal.notConnected') : '未连接到设备') + '\x1b[0m');
            this.writePrompt();
        }
    }
    
    /**
     * 解析并处理 SSH shell 命令
     * 返回 true 如果是 SSH 命令
     */
    parseSshCommand(command) {
        // 匹配 ssh --host xxx --user xxx [--password xxx] --shell
        const sshMatch = command.match(/^ssh\s+(.*)--shell/i);
        if (!sshMatch) return false;
        
        const argsStr = sshMatch[1];
        
        // 解析参数
        const hostMatch = argsStr.match(/--host\s+(\S+)/i);
        const userMatch = argsStr.match(/--user\s+(\S+)/i);
        const passwordMatch = argsStr.match(/--password\s+(\S+)/i);
        const portMatch = argsStr.match(/--port\s+(\d+)/i);
        
        if (!hostMatch || !userMatch) {
            this.writeln('\x1b[1;31m' + (typeof t === 'function' ? t('terminal.sshShellRequiresParams') : '错误: SSH shell 需要 --host 和 --user 参数') + '\x1b[0m');
            this.writePrompt();
            return true;
        }
        
        const sshParams = {
            host: hostMatch[1],
            user: userMatch[1],
            password: passwordMatch ? passwordMatch[1] : '',
            port: portMatch ? parseInt(portMatch[1]) : 22
        };
        
        this.startSshShell(sshParams);
        return true;
    }
    
    /**
     * 启动 SSH Shell
     */
    startSshShell(params) {
        if (this.sshConnecting || this.sshMode) {
            this.writeln('\x1b[1;31m' + (typeof t === 'function' ? t('terminal.sshSessionInProgress') : 'SSH 会话已在进行中') + '\x1b[0m');
            return;
        }
        if (!this.connected || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
        this.sshConnecting = true;
        const connMsg = typeof t === 'function' ? t('terminal.sshConnectingTo', { user: params.user, host: params.host, port: params.port }) : `正在连接到 ${params.user}@${params.host}:${params.port}...`;
        this.writeln(`\x1b[36m${connMsg}\x1b[0m`);
        
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({
                type: 'ssh_connect',
                host: params.host,
                port: params.port,
                user: params.user,
                password: params.password
            }));
        }
    }
    
    /**
     * 发送 SSH 输入
     */
    sendSshInput(data) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({
                type: 'ssh_input',
                data: data
            }));
        }
    }
    
    /**
     * 处理 SSH 状态消息
     */
    sshMessageText(message) {
        const keys = {
            "Missing host or user": "sshShellRequiresParams",
            "SSH result delivery capacity unavailable": "sshCapacity",
            "Failed to create SSH session": "sshCreateFailed",
            "Failed to open shell": "sshOpenFailed",
            "SSH shell closed during startup": "sshStartupClosed",
            "SSH connection status delivery failed": "sshStatusFailed",
            "SSH readiness delivery failed": "sshStatusFailed",
            "SSH status delivery failed": "sshStatusFailed",
            "SSH output delivery incomplete; remote command result is not confirmed": "sshOutputIncomplete",
            "SSH session closed": "sshClosed",
            "Closing SSH session...": "sshClosing",
            "Connecting to SSH server...": "sshConnecting",
            "SSH shell ready": "sshReady",
            "Service is stopping": "sshServiceStopping",
            'Unsupported SSH signal': 'sshControlUnsupported',
            'Invalid SSH control parameters': 'sshControlInvalid',
            'SSH control request failed; input was not replayed': 'sshControlFailed',
            'SSH channel failed; remote result is not confirmed': 'sshChannelUnknown',
            'This connection does not own an active SSH session': 'sshWrongOwner',
            'Another SSH session is active': 'sshBusy',
            'SSH request rejected': 'sshRequestRejected'
        };
        return keys[message] && typeof t === 'function' ? t('terminal.' + keys[message]) : message;
    }

    handleSshStatus(msg) {
        const status = msg.status;
        const message = this.sshMessageText(msg.message || '');
        
        switch (status) {
            case 'connecting':
                this.writeln(`\x1b[33m${message}\x1b[0m`);
                break;
            case 'connected':
                if (!this.sshConnecting) return;
                this.sshDisconnecting = false;
                this.sshMode = true;
                this.sshConnecting = false;
                this.writeln(`\x1b[1;32m${message}\x1b[0m`);
                this.writeln('\x1b[90m' + (typeof t === 'function' ? t('terminal.exitSshHint') : '(按 Ctrl+\\ 退出 SSH shell)') + '\x1b[0m');
                this.writeln('');
                break;
            case 'disconnecting':
                if (!this.sshMode) return;
                this.sshDisconnecting = true;
                this.writeln(`\r\n\x1b[33m${message}\x1b[0m`);
                break;
            case 'closed':
                if (!this.sshMode && !this.sshConnecting) return;
                this.sshDisconnecting = false;
                this.sshMode = false;
                this.sshConnecting = false;
                this.writeln(`\r\n\x1b[33m${message}\x1b[0m`);
                this.writePrompt();
                break;
            case 'error':
                if (!this.sshMode && !this.sshConnecting) return;
                this.sshDisconnecting = false;
                this.sshMode = false;
                this.sshConnecting = false;
                this.writeln(`\x1b[1;31m${message}\x1b[0m`);
                this.writePrompt();
                break;
        }
    }
    
    /**
     * 退出 SSH Shell
     */
    exitSshShell() {
        if (!this.sshMode) return;
        
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({ type: 'ssh_disconnect' }));
        }
    }

    /**
     * 写入提示符
     */
    writePrompt() {
        if (this.terminal) {
            this.terminal.write(this.prompt);
        }
    }

    /**
     * 写入文本
     */
    write(text) {
        if (this.terminal) {
            this.terminal.write(text);
        }
    }

    /**
     * 写入一行
     */
    writeln(text) {
        if (this.terminal) {
            this.terminal.writeln(text);
        }
    }

    /**
     * 调整大小
     */
    fit() {
        if (this.fitAddon) {
            this.fitAddon.fit();
        }
    }

    /**
     * 断开连接
     */
    disconnect() {
        clearTimeout(this.restoreTimer); this.restoring = false;
        // 清除心跳
        if (this.pingInterval) {
            clearInterval(this.pingInterval);
            this.pingInterval = null;
        }
        
        if (this.ws) {
            // 发送停止终端会话请求
            if (this.ws.readyState === WebSocket.OPEN) {
                this.ws.send(JSON.stringify({ type: 'terminal_stop' }));
            }
            this.ws.close(1000, 'User disconnect'); // 正常关闭代码
            this.ws = null;
        }
        this.connected = false;
        this.sshMode = this.sshConnecting = this.sshDisconnecting = false;
    }

    /**
     * 销毁终端
     */
    destroy() {
        this.destroyed = true;
        this.disconnect();
        if (this.terminal) {
            this.terminal.dispose();
            this.terminal = null;
        }
    }
}

// 全局终端实例
let webTerminal = null;

/** 供 router 在离开终端页时调用，主动关闭 WebSocket 避免 1006 */
window.destroyWebTerminal = function() {
    if (webTerminal) {
        webTerminal.destroy();
        webTerminal = null;
    }
};

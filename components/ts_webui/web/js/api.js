/**
 * TianshanOS API Client
 * 
 * 统一的 Core API 调用客户端，支持所有已注册的 API 端点
 */

// 开发模式配置：如果通过本地代理（如 VS Code Live Server）访问，
// 需要设置 ESP32 的实际 IP 地址
// 生产环境下留空，将自动使用 window.location.host
const DEV_ESP32_HOST = '';  // 例如: '192.168.0.152' 或 '192.168.1.100:80'

// 获取实际的 API 主机地址
function getApiHost() {
    // 如果设置了开发模式主机，使用它
    if (DEV_ESP32_HOST) {
        return DEV_ESP32_HOST;
    }
    // 检测是否是本地开发环境（localhost 或 127.0.0.1）
    const host = window.location.host;
    if (host.startsWith('localhost') || host.startsWith('127.0.0.1')) {
        console.debug('WebUI running on localhost - WebSocket/API may not work. Set DEV_ESP32_HOST in api.js if needed.');
    }
    return host;
}

const API_BASE = '/api/v1';

// 获取完整的 API URL（支持开发模式跨域）
function getApiUrl(endpoint) {
    const host = getApiHost();
    // 如果是开发模式（设置了 DEV_ESP32_HOST），使用完整 URL
    if (DEV_ESP32_HOST) {
        const protocol = window.location.protocol;
        return `${protocol}//${host}${API_BASE}${endpoint}`;
    }
    // 生产环境使用相对路径
    return `${API_BASE}${endpoint}`;
}

// Keep REST business failures as returned envelopes. Strict consumers opt in below.
function apiErrorReason(result) {
    for (const value of [result?.rawMessage, result?.error, result?.message]) {
        if (typeof value === 'string' && value.trim()) return value.trim();
    }
    return '';
}
function apiErrorMessage(result, operation = '') {
    const raw = apiErrorReason(result);
    const reasons = {
        'Invalid credentials': 'credentials', 'Invalid username or password': 'credentials', 'Old password is incorrect': 'oldPassword',
        'SD card not mounted': 'sdUnavailable', 'SD card already mounted': 'sdMounted',
        'File not found': 'fileMissing', 'Directory not found': 'directoryMissing',
        'Source file not found': 'fileMissing', 'Cannot delete root directory': 'rootDirectory',
        'Failed to stat file': 'fileInfo', 'Memory allocation failed': 'memory',
        'Request timeout': 'timeout', 'Failed to fetch': 'connection'
    };
    const codes = ['unknownError', 'invalidInput', 'notFound', 'permission', 'busy', 'timeout',
        'memory', 'internal', 'unsupported', 'hardware', 'connection', 'auth'];
    const names = {INVALID_ARG:1, NOT_FOUND:2, NO_PERMISSION:3, BUSY:4, TIMEOUT:5,
        NO_MEM:6, INTERNAL:7, NOT_SUPPORTED:8, HARDWARE:9, CONNECTION:10, AUTH:11};
    const code = typeof result?.code === 'number' ? result.code : names[result?.code];
    const key = result?.kind === 'timeout' ? (result.uncertain ? 'resultUnknown' : 'timeout')
        : result?.kind === 'network' ? (result.uncertain ? 'resultUnknown' : 'connection')
        : result?.kind === 'format' ? (result.uncertain ? 'resultUnknown' : 'invalidResponse')
        : reasons[raw] || codes[code] || 'unknownError';
    const message = t('promptRepair.' + key);
    // Unknown backend text remains diagnostic detail, not the entire user explanation.
    const detail = raw && !reasons[raw] ? '\n' + t('promptRepair.details', {detail: raw}) : '';
    return message + detail;
}
class ApiOperationError extends Error {
    constructor(result, operation = '', metadata = {}) {
        super(apiErrorMessage({...result, ...metadata}, operation));
        this.name = 'ApiOperationError';
        this.code = result?.code;
        this.rawMessage = apiErrorReason(result);
        this.operation = operation;
        this.httpStatus = result?.httpStatus;
        Object.assign(this, metadata);
    }
}
function requireApiSuccess(result, operation = '') {
    if (!result || result.code !== 0 || (result.httpStatus && result.httpStatus >= 400)) {
        throw new ApiOperationError(result, operation);
    }
    return result;
}

class TianShanAPI {
    constructor() {
        // 从 localStorage 恢复 token（注意 key 是 ts_token）
        this.token = localStorage.getItem('ts_token');
        this.username = localStorage.getItem('ts_username');
        this.level = localStorage.getItem('ts_level');
        
        // 检查 token 是否过期
        const expires = localStorage.getItem('ts_expires');
        if (expires && Date.now() > parseInt(expires)) {
            // Token 已过期，清除
            console.log('Token expired, clearing...');
            this.clearAuth();
        }
    }

    clearAuth() {
        this.token = null;
        this.username = null;
        this.level = null;
        this.passwordChanged = null;
        localStorage.removeItem('ts_token');
        localStorage.removeItem('ts_username');
        localStorage.removeItem('ts_level');
        localStorage.removeItem('ts_expires');
    }
    
    /**
     * 通用 API 请求方法
     */
    async request(endpoint, method = 'GET', data = null, timeout = 30000) {
        const headers = {
            'Content-Type': 'application/json'
        };
        
        if (this.token) {
            headers['Authorization'] = `Bearer ${this.token}`;
        }
        
        // 创建超时控制器
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), timeout);
        
        const options = {
            method,
            headers,
            signal: controller.signal
        };
        
        if (data && method !== 'GET') {
            options.body = JSON.stringify(data);
            // 调试：打印请求体大小
            console.debug(`API ${method} ${endpoint}: body size = ${options.body.length} bytes`);
        }
        
        let httpStatus;
        try {
            const response = await fetch(getApiUrl(endpoint), options);
            httpStatus = response.status;
            const text = await response.text();
            let json;
            try {
                json = JSON.parse(text);
            } catch (parseErr) {
                if (!response.ok) throw new ApiOperationError({error: text}, endpoint, {httpStatus: response.status});
                // 响应可能被 WebSocket 等数据污染（如 ~{"type"...），尝试从某处解析出带 code 的 API 格式
                if (parseErr instanceof SyntaxError && text) {
                    let idx = 0;
                    while ((idx = text.indexOf('{', idx)) >= 0) {
                        try {
                            const parsed = JSON.parse(text.slice(idx));
                            if (parsed != null && typeof parsed === 'object' && 'code' in parsed) {
                                json = parsed;
                                break;
                            }
                        } catch (_) {}
                        idx += 1;
                    }
                    if (json === undefined) throw parseErr;
                } else {
                    throw parseErr;
                }
            }
            
            // 返回 JSON 响应，即使是错误码也返回（让调用者决定如何处理）
            if (!response.ok) throw new ApiOperationError(json, endpoint, {httpStatus: response.status});
            const logoutResponse = endpoint === '/auth/logout' && method === 'POST';
            if (!json || typeof json !== 'object' || Array.isArray(json) ||
                (logoutResponse ? json.success !== true : !('code' in json))) {
                throw new ApiOperationError({}, endpoint, {kind: 'format', uncertain: method !== 'GET', httpStatus: response.status});
            }
            json.httpStatus = response.status;
            if (!logoutResponse && json.code !== 0) {
                json.rawMessage = apiErrorReason(json);
                json.message = apiErrorMessage(json, endpoint);
            }

            const isAuthEndpoint = endpoint === '/auth/login' || endpoint === '/auth/logout';
            if (!isAuthEndpoint && json && json.code === 'AUTH') {
                this.clearAuth();
                const message = typeof window.t === 'function'
                    ? window.t('login.sessionExpired')
                    : '登录已过期，请重新登录';
                json.message = message;
                window.dispatchEvent(new CustomEvent('authExpired', { detail: { message } }));
            }
            
            return json;
        } catch (error) {
            if (error instanceof ApiOperationError) throw error;
            const kind = controller.signal.aborted ? 'timeout' : error instanceof SyntaxError ? 'format' : 'network';
            throw new ApiOperationError({message: error.message}, endpoint, {kind, uncertain: method !== 'GET', httpStatus});
        } finally {
            clearTimeout(timeoutId);
        }
    }

    /**
     * 调用 Core API（将端点名转为 URL 路径）
     * 例如: call('system.info') -> GET /api/v1/system/info
     */
    async call(apiName, params = null, method = null) {
        let endpoint = '/' + apiName.replace(/\./g, '/');
        
        // 自动判断方法：有参数且非查询类用 POST
        if (!method) {
            const isQuery = apiName.includes('.list') || apiName.includes('.status') || 
                           apiName.includes('.info') || apiName.includes('.get') ||
                           apiName.includes('.version') || apiName.includes('.partitions') ||
                           apiName.includes('.progress') || apiName.includes('.stats');
            method = (params && !isQuery) ? 'POST' : 'GET';
        }
        
        // GET 请求时，将 params 转为 query string
        if (method === 'GET' && params) {
            const queryParams = new URLSearchParams();
            for (const [key, value] of Object.entries(params)) {
                if (value !== null && value !== undefined && value !== '') {
                    queryParams.append(key, value);
                }
            }
            const queryString = queryParams.toString();
            if (queryString) {
                endpoint += '?' + queryString;
            }
            params = null;  // GET 请求不发送 body
        }
        
        return this.request(endpoint, method, params);
    }
    
    // =====================================================================
    //                         认证 API
    // =====================================================================
    
    async login(username, password) {
        const result = await this.call('auth.login', { username, password }, 'POST');
        if (result.code === 0 && (typeof result.data?.token !== 'string' || !result.data.token)) {
            throw new ApiOperationError({}, 'auth.login', {kind: 'format'});
        }
        if (result.code === 0 && result.data?.token) {
            this.token = result.data.token;
            this.username = result.data.username;
            this.level = result.data.level;
            this.passwordChanged = result.data.password_changed;
            localStorage.setItem('ts_token', result.data.token);
            localStorage.setItem('ts_username', result.data.username);
            localStorage.setItem('ts_level', result.data.level);
            localStorage.setItem('ts_expires', Date.now() + (result.data.expires_in * 1000));
        }
        return result;
    }
    
    async logout() {
        // request() captures Authorization before local credentials are cleared.
        const pending = this.token ? this.call('auth.logout', {token: this.token}, 'POST') : null;
        this.clearAuth();
        try {
            if (pending) {
                await pending;
                return {localCleared: true, serverConfirmed: true};
            }
            return {localCleared: true, serverConfirmed: false};
        } catch (error) {
            return {localCleared: true, serverConfirmed: false, error};
        }
    }

    async checkAuthStatus() {
        if (!this.token) return { valid: false };
        const result = await this.call('auth.status', { token: this.token }, 'POST');
        if ((result.success || result.code === 0 || result.code === 'OK') && result.data) {
            return result.data;
        }
        return { valid: false };
    }
    
    async changePassword(oldPassword, newPassword) {
        return this.call('auth.change_password', {
            token: this.token,
            old_password: oldPassword,
            new_password: newPassword
        }, 'POST');
    }

    async setAdminPassword(newPassword) {
        return this.call('auth.admin.set_password', {
            token: this.token,
            new_password: newPassword
        }, 'POST');
    }

    async setRootPassword(newPassword) {
        return this.call('auth.root.set_password', {
            token: this.token,
            new_password: newPassword
        }, 'POST');
    }

    async resetAdminPassword() {
        return this.call('auth.admin.reset_password', {
            token: this.token
        }, 'POST');
    }
    
    isLoggedIn() {
        if (!this.token) {
            // 尝试从 localStorage 恢复
            const savedToken = localStorage.getItem('ts_token');
            const expires = localStorage.getItem('ts_expires');
            const parsed = expires ? parseInt(expires) : NaN;
            const now = Date.now();
            const valid = savedToken && expires && now < parsed;
            if (valid) {
                this.token = savedToken;
                this.username = localStorage.getItem('ts_username');
                this.level = localStorage.getItem('ts_level');
                return true;
            }
            return false;
        }
        // 检查是否过期
        const expires = localStorage.getItem('ts_expires');
        const parsed = expires ? parseInt(expires) : NaN;
        const expired = expires && Date.now() >= parsed;
        if (expired) {
            this.clearAuth();
            return false;
        }
        return true;
    }
    
    getUsername() {
        return this.username || localStorage.getItem('ts_username');
    }
    
    getLevel() {
        return this.level || localStorage.getItem('ts_level');
    }
    
    /**
     * 检查是否有 root 权限（可访问终端、自动化、指令）
     */
    isRoot() {
        return this.getLevel() === 'root';
    }
    
    /**
     * 检查是否是 admin 用户
     */
    isAdmin() {
        const level = this.getLevel();
        return level === 'admin' || level === 'root';
    }
    
    // =====================================================================
    //                         系统 API (system.*)
    // =====================================================================
    
    async getSystemInfo() { return this.call('system.info'); }
    async getMemoryInfo() { return this.call('system.memory'); }
    async getMemoryDetail() { return this.call('system.memory_detail'); }
    async getTasks() { return this.call('system.tasks'); }
    async reboot(delay = 0) { return this.call('system.reboot', { delay }, 'POST'); }
    
    // =====================================================================
    //                         时间 API (time.*)
    // =====================================================================
    
    async timeInfo() { return this.call('time.info'); }
    async timeSync(timestampMs) { return this.call('time.sync', { timestamp_ms: timestampMs }, 'POST'); }
    async timeSetNtp(server, index = 0) { return this.call('time.set_ntp', { server, index }, 'POST'); }
    async timeSetTimezone(timezone) { return this.call('time.set_timezone', { timezone }, 'POST'); }
    async timeForceSync() { return this.call('time.force_sync', null, 'POST'); }
    
    // =====================================================================
    //                         配置 API (config.*)
    // =====================================================================
    
    // 基础配置
    async configGet(key) { return this.call('config.get', { key }); }
    async configSet(key, value, persist = false) { return this.call('config.set', { key, value, persist }, 'POST'); }
    async configDelete(key) { return this.call('config.delete', { key }, 'POST'); }
    async configList(prefix = '') { return this.call('config.list', prefix ? { prefix } : null); }
    async configSave() { return this.call('config.save', null, 'POST'); }
    
    // 模块化配置
    async configModuleList() { return this.call('config.module.list'); }
    async configModuleShow(module) { return this.call('config.module.show', { module }); }
    async configModuleSet(module, key, value) { return this.call('config.module.set', { module, key, value }, 'POST'); }
    async configModuleSave(module = null) { return this.call('config.module.save', module ? { module } : null, 'POST'); }
    async configModuleReset(module, persist = true) { return this.call('config.module.reset', { module, persist }, 'POST'); }
    async configSync() { return this.call('config.sync', null, 'POST'); }
    
    // =====================================================================
    //                         服务 API (service.*)
    // =====================================================================
    
    async serviceList() { return this.call('service.list'); }
    async serviceStatus(name) { return this.call('service.status', { name }); }
    async serviceStart(name) { return this.call('service.start', { name }, 'POST'); }
    async serviceStop(name) { return this.call('service.stop', { name }, 'POST'); }
    async serviceRestart(name) { return this.call('service.restart', { name }, 'POST'); }
    
    // =====================================================================
    //                         LED API (led.*)
    // =====================================================================
    
    async ledList() { return this.call('led.list'); }
    async ledBrightness(device, brightness) { return this.call('led.brightness', { device, brightness }, 'POST'); }
    async ledClear(device) { return this.call('led.clear', { device }, 'POST'); }
    async ledSet(device, index, color) { return this.call('led.set', { device, index, color }, 'POST'); }
    async ledFill(device, color) { return this.call('led.fill', { device, color }, 'POST'); }
    async ledEffectList(device = null) { return this.call('led.effect.list', device ? { device } : null); }
    async ledEffectStart(device, effect, params = {}) { return this.call('led.effect.start', { device, effect, ...params }, 'POST'); }
    async ledEffectStop(device) { return this.call('led.effect.stop', { device }, 'POST'); }
    async ledColorParse(color) { return this.call('led.color.parse', { color }); }
    async ledFilterList() { return this.call('led.filter.list'); }
    async ledFilterStart(device, filter, speed = 50) { return this.call('led.filter.start', { device, filter, speed }, 'POST'); }
    async ledFilterStop(device) { return this.call('led.filter.stop', { device }, 'POST'); }
    async ledSave(device) { return this.call('led.save', { device }, 'POST'); }
    
    // Matrix-specific LED APIs
    async ledImage(path, device = 'matrix', center = 'image') { return this.call('led.image', { device, path, center }, 'POST'); }
    async ledQrcode(text, options = {}) { return this.call('led.qrcode', { device: 'matrix', text, ...options }, 'POST'); }
    async ledText(text, options = {}) { return this.call('led.text', { device: 'matrix', text, ...options }, 'POST'); }
    async ledTextStop(device = 'matrix') { return this.call('led.text.stop', { device }, 'POST'); }
    
    // Color Correction APIs
    async ledColorCorrectionGet() { return this.call('led.color_correction.get'); }
    async ledColorCorrectionSet(config) { return this.call('led.color_correction.set', config, 'POST'); }
    async ledColorCorrectionReset() { return this.call('led.color_correction.reset', null, 'POST'); }
    async ledColorCorrectionExport() { return this.call('led.color_correction.export', null, 'POST'); }
    async ledColorCorrectionImport() { return this.call('led.color_correction.import', null, 'POST'); }
    
    // =====================================================================
    //                         网络 API (network.*, dhcp.*, nat.*)
    // =====================================================================
    
    // 综合网络状态 (包含 ethernet, wifi_sta, wifi_ap)
    async networkStatus() { return this.call('network.status'); }
    async lpmuAccessStart() { return this.call('network.lpmu_access.start', null, 'POST'); }
    async lpmuAccessStatus() { return this.call('network.lpmu_access.status'); }
    
    // WiFi 相关
    async wifiMode(mode = null) { 
        return mode ? this.call('network.wifi.mode', { mode }, 'POST') : this.call('network.wifi.mode'); 
    }
    async wifiScan() { return this.call('network.wifi.scan'); }
    async wifiConnect(ssid, password) { return this.call('network.wifi.connect', { ssid, password }, 'POST'); }
    async wifiDisconnect() { return this.call('network.wifi.disconnect', null, 'POST'); }
    async wifiApConfig(ssid, password = '', channel = 6, hidden = false) { 
        return this.call('network.wifi.ap.config', { ssid, password, channel, hidden }, 'POST'); 
    }
    async wifiApStations() { return this.call('network.wifi.ap.stations'); }
    
    // 以太网
    async ethStatus() { return this.call('network.eth.status'); }
    
    // 主机名
    async hostname(name = null) { 
        return name ? this.call('network.hostname', { hostname: name }, 'POST') : this.call('network.hostname'); 
    }
    
    // DHCP 服务器
    async dhcpStatus(iface = null) { return this.call('dhcp.status', iface ? { interface: iface } : null); }
    async dhcpClients(iface = null) { return this.call('dhcp.clients', iface ? { interface: iface } : null); }
    async dhcpStart(iface = 'ap') { return this.call('dhcp.start', { interface: iface }, 'POST'); }
    async dhcpStop(iface = 'ap') { return this.call('dhcp.stop', { interface: iface }, 'POST'); }
    
    // NAT 网关
    async natStatus() { return this.call('nat.status'); }
    async natEnable() { return this.call('nat.enable', null, 'POST'); }
    async natDisable() { return this.call('nat.disable', null, 'POST'); }
    async natSave() { return this.call('nat.save', null, 'POST'); }
    
    // =====================================================================
    //                         设备 API (device.*, monitor.*, fan.*)
    // =====================================================================
    
    async deviceStatus(name = null) { return this.call('device.status', name ? { name } : null); }
    async devicePower(name, on) { return this.call('device.power', { name, on }, 'POST'); }
    async deviceReset(name) { return this.call('device.reset', { name }, 'POST'); }
    async agxStatus() { return this.call('monitor.status'); }
    async agxData() { return this.call('monitor.data'); }
    async fanStatus(id = null) { return this.call('fan.status', id !== null ? { id } : null); }
    async fanSet(id, duty) { return this.call('fan.set', { id, duty }, 'POST'); }
    async fanMode(id, mode) { return this.call('fan.mode', { id, mode }, 'POST'); }
    
    // =====================================================================
    //                         电源 API (power.*)
    // =====================================================================
    
    async powerStatus() { return this.call('power.status'); }
    async powerVoltage(channel = null) { return this.call('power.voltage', channel !== null ? { channel } : null); }
    async powerStats() { return this.call('power.stats'); }
    async powerProtectionStatus() { return this.call('power.protection.status'); }
    async powerProtectionSet(config) { return this.call('power.protection.set', config, 'POST'); }
    async powerProtectionConfig() { return this.call('power.protection.config'); }
    
    // =====================================================================
    //                         存储/GPIO/温度 API
    // =====================================================================
    
    async storageStatus() { return this.call('storage.status'); }
    async storageMount() { return this.call('storage.mount', null, 'POST'); }
    async storageUnmount() { return this.call('storage.unmount', null, 'POST'); }
    async storageList(path = '/sdcard') { return this.call('storage.list', { path }, 'POST'); }
    async storageRead(path) { return this.call('storage.read', { path }, 'POST'); }
    async storageDelete(path) { return this.call('storage.delete', { path }, 'POST'); }
    async storageMkdir(path) { return this.call('storage.mkdir', { path }, 'POST'); }
    async storageRename(from, to) { return this.call('storage.rename', { from, to }, 'POST'); }
    async storageInfo(path) { return this.call('storage.info', { path }, 'POST'); }
    
    /**
     * 下载文件
     * @param {string} path - 文件路径 (如 /sdcard/test.txt)
     * @returns {Promise<Blob>} 文件内容
     */
    async fileDownload(path) {
        const url = `${API_BASE}/file/download?path=${encodeURIComponent(path)}`;
        const response = await fetch(url, {
            headers: this.token ? { 'Authorization': `Bearer ${this.token}` } : {}
        });
        if (!response.ok) {
            const error = await response.json().catch(() => ({ message: 'Download failed' }));
            throw new ApiOperationError(error, 'file.download', {httpStatus: response.status});
        }
        return response.blob();
    }
    
    /**
     * 上传文件
     * @param {string} path - 目标路径 (如 /sdcard/uploads/file.txt)
     * @param {File|Blob|ArrayBuffer} content - 文件内容
     * @returns {Promise<Object>} 上传结果
     */
    async fileUpload(path, content) {
        const url = `${API_BASE}/file/upload?path=${encodeURIComponent(path)}`;
        const response = await fetch(url, {
            method: 'POST',
            headers: this.token ? { 'Authorization': `Bearer ${this.token}` } : {},
            body: content
        });
        const json = await response.json();
        if (!response.ok || json.status !== 'uploaded' || (json.code !== undefined && json.code !== 0)) {
            throw new ApiOperationError(json, 'file.upload', {httpStatus: response.status});
        }
        return json;
    }
    
    async gpioList() { return this.call('gpio.list'); }
    async gpioInfo(pin) { return this.call('gpio.info', { pin }); }
    async tempStatus() { return this.call('temp.status'); }
    
    // =====================================================================
    //                         SSH/SFTP API
    // =====================================================================
    
    // SSH 测试连接 - 支持密码或密钥认证
    // options: { trust_new: bool, accept_changed: bool }
    async sshTest(host, user, auth, port = 22, options = {}) { 
        const params = { host, user, port };
        if (typeof auth === 'string') {
            params.password = auth;  // 兼容旧调用方式
        } else if (auth.password) {
            params.password = auth.password;
        } else if (auth.keyid) {
            params.keyid = auth.keyid;
        }
        // 主机指纹验证参数
        params.trust_new = options.trust_new ?? false;      // 新主机必须明确确认指纹
        params.confirmed_fingerprint = options.confirmed_fingerprint;
        params.accept_changed = options.accept_changed ?? false; // 指纹变化是否接受（默认 false）
        return this.call('ssh.test', params, 'POST'); 
    }
    
    // SSH 执行命令 - 支持密码或密钥认证
    // options: { trust_new: bool, accept_changed: bool }
    async sshExec(host, user, auth, command, port = 22, options = {}) { 
        const params = { host, user, command, port };
        if (typeof auth === 'string') {
            params.password = auth;  // 兼容旧调用方式
        } else if (auth.password) {
            params.password = auth.password;
        } else if (auth.keyid) {
            params.keyid = auth.keyid;
        }
        // 主机指纹验证参数
        params.trust_new = options.trust_new ?? false;
        params.confirmed_fingerprint = options.confirmed_fingerprint;
        params.accept_changed = options.accept_changed ?? false;
        return this.call('ssh.exec', params, 'POST'); 
    }
    
    // 部署公钥 - options: { trust_new, accept_changed }
    async sshCopyid(host, user, password, keyid, port = 22, verify = true, options = {}) { 
        return this.call('ssh.copyid', { 
            host, user, password, keyid, port, verify,
            trust_new: options.trust_new ?? false,
            confirmed_fingerprint: options.confirmed_fingerprint,
            accept_changed: options.accept_changed ?? false
        }, 'POST'); 
    }
    
    // 撤销公钥 - options: { trust_new, accept_changed }
    async sshRevoke(host, user, password, keyid, port = 22, options = {}) { 
        return this.call('ssh.revoke', { 
            host, user, password, keyid, port,
            trust_new: options.trust_new ?? false,
            confirmed_fingerprint: options.confirmed_fingerprint,
            accept_changed: options.accept_changed ?? false
        }, 'POST'); 
    }
    async sshKeygen(id, type = 'ecdsa', comment = '') { return this.call('ssh.keygen', { id, type, comment }, 'POST'); }
    async sftpLs(host, user, password, path, port = 22) { return this.call('sftp.ls', { host, user, password, path, port }, 'POST'); }
    
    // =====================================================================
    //                         Key Management API
    // =====================================================================
    
    async keyList() { return this.call('key.list'); }
    async keyInfo(id) { return this.call('key.info', { id }); }
    async keyGenerate(id, type = 'rsa2048', comment = '', exportable = false, alias = '', hidden = false) { 
        return this.call('key.generate', { id, type, comment, exportable, alias, hidden }, 'POST'); 
    }
    async keyDelete(id) { return this.call('key.delete', { id }, 'POST'); }
    async keyExport(id) { return this.call('key.export', { id }); }
    async keyExportPrivate(id) { return this.call('key.exportPrivate', { id }, 'POST'); }
    
    // =====================================================================
    //                         Known Hosts API
    // =====================================================================
    
    async hostsList() { return this.call('hosts.list'); }
    async hostsInfo(host, port = 22) { return this.call('hosts.info', { host, port }); }
    async hostsRemove(host, port = 22) { return this.call('hosts.remove', { host, port }, 'POST'); }
    async hostsUpdate(host, port = 22) { return this.call('hosts.update', { host, port }, 'POST'); }
    async hostsClear() { return this.call('hosts.clear', {}, 'POST'); }
    
    // =====================================================================
    //                    Certificate (PKI) Management API
    // =====================================================================
    
    /** 获取 HTTPS 证书 PKI 状态 */
    async certStatus() { return this.call('cert.status'); }
    
    /** 生成 ECDSA P-256 密钥对 */
    async certGenerateKeypair(force = false) { 
        return this.call('cert.generate_keypair', { force }, 'POST'); 
    }
    
    /** 生成证书签名请求 (CSR) */
    async certGenerateCSR(opts = {}) { 
        return this.call('cert.generate_csr', opts, 'POST'); 
    }
    
    /** 安装签发的证书 */
    async certInstall(certPem) { 
        return this.call('cert.install', { cert_pem: certPem }, 'POST'); 
    }
    
    /** 安装 CA 证书链 */
    async certInstallCA(caPem) { 
        return this.call('cert.install_ca', { ca_pem: caPem }, 'POST'); 
    }
    
    /** 获取设备证书 PEM */
    async certGetCertificate() { 
        return this.call('cert.get_certificate'); 
    }
    
    /** 删除所有 PKI 凭证（需要 confirm=true） */
    async certDelete() { 
        return this.call('cert.delete', { confirm: true }, 'POST'); 
    }
    
    // =====================================================================
    //                      Config Pack API
    // =====================================================================
    
    /** 获取配置包系统信息 */
    async configPackInfo() { 
        return this.call('config.pack.info'); 
    }
    
    /** 导出设备证书（供他人加密配置给本设备） */
    async configPackExportCert() { 
        return this.call('config.pack.export_cert'); 
    }
    
    /** 验证 .tscfg 配置包签名 */
    async configPackVerify(content = null, path = null) { 
        const params = {};
        if (content) params.content = content;
        if (path) params.path = path;
        return this.call('config.pack.verify', params, 'POST'); 
    }
    
    /** 导入并解密 .tscfg 配置包 */
    async configPackImport(content = null, path = null, apply = false) { 
        const params = { apply };
        if (content) params.content = content;
        if (path) params.path = path;
        return this.call('config.pack.import', params, 'POST'); 
    }
    
    /** 导出配置为加密 .tscfg（仅 Developer 设备可用） */
    async configPackExport(name, content, recipientCert, description = null, savePath = null) { 
        const params = { 
            name, 
            content,
            recipient_cert: recipientCert
        };
        if (description) params.description = description;
        if (savePath) params.save_path = savePath;
        return this.call('config.pack.export', params, 'POST'); 
    }
    
    /** 列出目录中的 .tscfg 文件 */
    async configPackList(path = '/sdcard/config') { 
        return this.call('config.pack.list', { path }); 
    }
}

// =========================================================================
//                         WebSocket Client
// =========================================================================

class TianShanWS {
    constructor(onMessage, onConnect, onDisconnect) {
        this.ws = null;
        this.onMessage = onMessage;
        this.onConnect = onConnect;
        this.onDisconnect = onDisconnect;
        this.reconnectDelay = 1000;
        this.shouldReconnect = true;
    }
    
    connect() {
        const host = getApiHost();
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const url = `${protocol}//${host}/ws`;
        
        try {
            this.ws = new WebSocket(url);
            
            this.ws.onopen = () => {
                console.log('WebSocket connected');
                this.reconnectDelay = 1000;
                this.send({ type: 'subscribe', events: ['*'] });
                if (this.onConnect) this.onConnect();
            };
            
            this.ws.onmessage = (event) => {
                try {
                    const msg = JSON.parse(event.data);
                    if (this.onMessage) this.onMessage(msg);
                } catch (e) {
                    console.error('WebSocket parse error:', e);
                }
            };
            
            this.ws.onclose = () => {
                console.log('WebSocket disconnected');
                if (this.onDisconnect) this.onDisconnect();
                if (this.shouldReconnect) {
                    setTimeout(() => this.connect(), this.reconnectDelay);
                    this.reconnectDelay = Math.min(this.reconnectDelay * 2, 30000);
                }
            };
            
            this.ws.onerror = (error) => console.error('WebSocket error:', error);
        } catch (e) {
            console.error('WebSocket connect failed:', e);
        }
    }
    
    disconnect() {
        this.shouldReconnect = false;
        if (this.ws) { this.ws.close(); this.ws = null; }
    }
    
    send(data) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(data));
        }
    }
    
    // 暴露 readyState 属性，供日志页面检查连接状态
    get readyState() {
        return this.ws ? this.ws.readyState : WebSocket.CLOSED;
    }
}

// 全局实例
const api = new TianShanAPI();

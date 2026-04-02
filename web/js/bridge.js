/**
 * bridge.js — 与 WPF WebView2 宿主的通信桥接 + 设备信息
 */
const Bridge = {
  /** 是否在 WebView2 环境中 */
  isWebView2: !!(window.chrome && window.chrome.webview),

  /** 设备信息缓存 */
  deviceInfo: null,

  /** 消息回调列表 */
  _handlers: [],

  /** 单次响应 pending promises: responseType → {resolve, reject, timer} */
  _pendingRequests: new Map(),

  /** 向宿主发送消息 */
  postMessage(type, data = {}) {
    const msg = JSON.stringify({ type, ...data });
    if (this.isWebView2) {
      window.chrome.webview.postMessage(msg);
    } else {
      console.log('[Bridge →]', type, data);
    }
  },

  /** 移除已注册的消息监听 */
  offMessage(handler) {
    const idx = this._handlers.indexOf(handler);
    if (idx !== -1) this._handlers.splice(idx, 1);
  },

  /** 注册宿主消息监听 */
  onMessage(handler) {
    this._handlers.push(handler);
    if (this.isWebView2 && !this._webviewListenerAdded) {
      this._webviewListenerAdded = true;
      window.chrome.webview.addEventListener('message', (e) => {
        try {
          const msg = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
          // 先尝试派发给 pending promise
          this._dispatchPending(msg);
          // 再通知通用 handlers
          this._handlers.forEach(h => h(msg));
        } catch {
          this._handlers.forEach(h => h({ type: 'raw', data: e.data }));
        }
      });
    }
  },

  /** 内部：将响应消息路由到等待中的 Promise */
  _dispatchPending(msg) {
    const pending = this._pendingRequests.get(msg.type);
    if (!pending) return;
    this._pendingRequests.delete(msg.type);
    clearTimeout(pending.timer);
    pending.resolve(msg.data ?? msg);
  },

  /**
   * 向宿主发送请求并等待指定 responseType 消息返回（Promise）
   * @param {string} type         发送的消息类型
   * @param {object} data         附带数据
   * @param {string} responseType 期望收到的响应类型
   * @param {number} [timeoutMs=30000]
   */
  _request(type, data, responseType, timeoutMs = 30000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this._pendingRequests.delete(responseType);
        reject(new Error(`Bridge request timeout: ${responseType}`));
      }, timeoutMs);
      this._pendingRequests.set(responseType, { resolve, reject, timer });
      this.postMessage(type, data);
    });
  },

  /** 公开别名 (兼容 requestAsync 调用) */
  requestAsync(type, data, responseType, timeoutMs) {
    return this._request(type, data, responseType, timeoutMs);
  },

  // ── VB-Audio Virtual Cable 一键安装 API ────────────────────

  /**
   * 检测 VB-Cable 是否已安装
   * @returns {Promise<{installed: boolean, deviceName: string}>}
   */
  checkVBCable() {
    if (!this.isWebView2) {
      return Promise.resolve({ installed: false, deviceName: '' });
    }
    return this._request('check-vbcable', {}, 'vbcable-status', 8000);
  },

  /**
   * 检测默认录制设备是否为 CABLE Output
   * @returns {Promise<{isCableOutput: boolean, currentDevice: string, installed: boolean}>}
   */
  checkDefaultRecording() {
    if (!this.isWebView2) {
      return Promise.resolve({ isCableOutput: false, currentDevice: '', installed: false });
    }
    return this._request('check-default-recording', {}, 'default-recording-status', 5000);
  },

  /**
   * 将默认录制设备设置为 CABLE Output
   * @returns {Promise<{success: boolean, error?: string}>}
   */
  setDefaultRecordingToCable() {
    if (!this.isWebView2) {
      return Promise.reject(new Error('仅在客户端中支持'));
    }
    return this._request('set-default-recording-cable', {}, 'set-default-recording-result', 5000);
  },

  /**
   * 自动下载并安装 VB-Cable（会弹出 UAC 提示，需要管理员权限）
   * 过程中会收到 vbcable-install-progress 消息（通过 onMessage 监听）
   * @returns {Promise<{success: boolean, deviceName?: string, error?: string}>}
   */
  installVBCable() {
    if (!this.isWebView2) {
      return Promise.reject(new Error('仅在 WPF 客户端中支持自动安装'));
    }
    return this._request('install-vbcable', {}, 'vbcable-install-result', 120000);
  },

  /**
   * 利用 pnputil /delete-driver 卸载 VB-Cable（会弹出 UAC 提示）
   * 过程中会收到 vbcable-uninstall-progress 消息
   * @returns {Promise<{success: boolean, note?: string, error?: string}>}
   */
  uninstallVBCable() {
    if (!this.isWebView2) {
      return Promise.reject(new Error('仅在 WPF 客户端中支持卸载'));
    }
    return this._request('uninstall-vbcable', {}, 'vbcable-uninstall-result', 60000);
  },

  /** 请求设备信息 */
  requestDeviceInfo() {
    this.postMessage('request-device-info');
  },

  /** 请求进程列表 */
  requestProcessList() {
    this.postMessage('request-process-list');
  },

  /** 请求结束进程 */
  requestKillProcess(pid) {
    this.postMessage('kill-process', { pid });
  },

  /** 执行终端命令 */
  executeCommand(command) {
    this.postMessage('execute-command', { command });
  },

  /** 通知宿主：频道已创建 */
  notifyChannelCreated(channelId) {
    this.postMessage('channel-created', { channelId });
  },

  /** 通知宿主：共享已开始 */
  notifyShareStarted() {
    this.postMessage('share-started');
  },

  /** 通知宿主：共享已停止 */
  notifyShareStopped() {
    this.postMessage('share-stopped');
  },

  /** 通知宿主：发生错误 */
  notifyError(error) {
    this.postMessage('error', { error: String(error) });
  },

  /** 请求宿主进入全屏 */
  enterFullscreen() {
    this.postMessage('enter-fullscreen');
  },

  /** 请求宿主退出全屏 */
  exitFullscreen() {
    this.postMessage('exit-fullscreen');
  },

  // ── 键鼠锁定 API（仅 Native 模式） ───────────────────────

  /**
   * 安装键鼠锁定钩子
   * @returns {Promise<{ok: boolean, hotkey: string}>}
   */
  installInputLock() {
    if (!this.isWebView2) {
      return Promise.resolve({ ok: false, hotkey: '' });
    }
    return this._request('input-lock-install', {}, 'input-lock-install-result', 5000);
  },

  /**
   * 卸载键鼠锁定钩子
   * @returns {Promise<{ok: boolean}>}
   */
  uninstallInputLock() {
    if (!this.isWebView2) return Promise.resolve({ ok: true });
    return this._request('input-lock-uninstall', {}, 'input-lock-uninstall-result', 5000);
  },

  /**
   * 设置锁定热键
   * @param {string} hotkey 格式: "Ctrl+Alt+Shift+F12"
   * @returns {Promise<{hotkey: string}>}
   */
  setInputLockHotkey(hotkey) {
    if (!this.isWebView2) return Promise.resolve({ hotkey });
    return this._request('input-lock-set-hotkey', { hotkey }, 'input-lock-hotkey-changed', 5000);
  },

  /**
   * 切换锁定状态（也可通过热键触发）
   */
  toggleInputLock() {
    this.postMessage('input-lock-toggle');
  },

  /**
   * 查询当前锁定状态
   * @returns {Promise<{installed: boolean, locked: boolean, hotkey: string}>}
   */
  queryInputLock() {
    if (!this.isWebView2) {
      return Promise.resolve({ installed: false, locked: false, hotkey: 'Ctrl+Alt+Shift+F12' });
    }
    return this._request('input-lock-query', {}, 'input-lock-state-info', 5000);
  },

  /** 通知宿主：关闭 */
  notifyClose() {
    this.postMessage('close');
  },

  /** 隐藏窗口到后台（任务栏和托盘都不显示，Ctrl+Shift+M 恢复） */
  hideWindow() {
    this.postMessage('window-hide');
  },

  /** 恢复隐藏的窗口 */
  showWindow() {
    this.postMessage('window-show');
  },

  /** 隐身模式：窗口在 Alt+Tab / Win+Tab 中不可见 */
  setStealth(enable) {
    this.postMessage('window-stealth', { enable });
  },

  /** 开机自启：启用 */
  enableAutostart() {
    this.postMessage('autostart-enable');
  },

  /** 开机自启：禁用 */
  disableAutostart() {
    this.postMessage('autostart-disable');
  },

  /** 开机自启：查询状态 */
  queryAutostart() {
    this.postMessage('autostart-query');
  },

  /** 获取模拟设备信息（浏览器环境用） */
  getMockDeviceInfo() {
    return {
      computerName: 'DESKTOP-MDShare',
      userName: 'User',
      os: 'Windows 11 Pro',
      osVersion: '10.0.22631',
      cpu: navigator.hardwareConcurrency + ' Core CPU',
      memory: (navigator.deviceMemory || 8) + ' GB',
      gpu: 'Unknown',
      storage: 'Unknown',
      networkAdapters: [],
      clientVersion: '1.0.0',
      permissionGroup: 'User',
      cid: Date.now().toString()
    };
  },

};

/**
 * AppConfig — 持久化配置（localStorage）
 * bridge.js 最早加载，供 screen-share.js / viewer.js 读取
 */
const AppConfig = {
  _LS_SESSION : 'mdshare_session_token',
  _LS_CARDKEY : 'mdshare_card_key',    // 保存上次使用的卡密（用于预填）

  /** 读取 session token */
  getSessionToken() {
    try { return localStorage.getItem(this._LS_SESSION) || ''; } catch { return ''; }
  },
  /** 保存 session token */
  setSessionToken(token) {
    try { localStorage.setItem(this._LS_SESSION, token); } catch {}
  },
  /** 清除 session token（同时清理卡密缓存） */
  clearSession() {
    try {
      localStorage.removeItem(this._LS_SESSION);
      localStorage.removeItem(this._LS_CARDKEY);
    } catch {}
  },

  /**
   * 向服务器注销当前 session，再清理本地 storage
   * @returns {Promise<void>}
   */
  async logout() {
    const token = this.getSessionToken();
    if (token) {
      try {
        await fetch(`${this.resolveServerUrl()}/api/auth/logout`, {
          method: 'POST',
          headers: { Authorization: `Bearer ${token}` },
        });
      } catch { /* 网络失败不阻止本地注销 */ }
    }
    this.clearSession();
  },

  /** 读取上次使用的卡密（明文，仅用于预填输入框） */
  getSavedCardKey() {
    try { return localStorage.getItem(this._LS_CARDKEY) || ''; } catch { return ''; }
  },
  /** 保存卡密 */
  saveCardKey(key) {
    try { localStorage.setItem(this._LS_CARDKEY, key); } catch {}
  },

  /** 服务器地址（硬编码） */
  resolveServerUrl() {
    return 'http://38.76.209.172:3000';
  }
};

/**
 * remote-control.js — 远程控制模块（观看端输入捕获 + 发送）
 *
 * 参考：
 *   - noVNC 1.6.0 xtscancodes.js:  HTML event.code → AT Set 1 扫描码
 *   - noVNC keyboard.js:           AltGr/修饰键处理
 *   - noVNC gesturehandler.js:     触摸手势 → 鼠标事件
 *   - neko 3.0.0 control.go:       JSON 消息格式
 *
 * 使用方式：
 *   RemoteControl.init(videoElement, socketOrSendFn);
 *   RemoteControl.enable(channelId);
 *   RemoteControl.disable();
 */

// ── 快捷键配置系统（全局，供 app.js 使用） ──

const HotkeyConfig = {
  /** 快捷键定义 */
  _defs: [
    { id: 'exitFullscreen',    label: '退出全屏',           defaultKey: 'Shift+Digit8',            displayDefault: 'Shift + 8' },
    { id: 'togglePointerLock', label: '切换鼠标模式 (绝对/相对)', defaultKey: 'Shift+Alt',          displayDefault: 'Shift + Alt' },
    { id: 'inputLockToggle',   label: '键鼠锁定/解锁',       defaultKey: 'Ctrl+Alt+Shift+F12',     displayDefault: 'Ctrl+Alt+Shift+F12' },
    { id: 'toggleRemoteControl', label: '开始/结束远控',        defaultKey: 'Shift+F9',               displayDefault: 'Shift + F9' },
  ],
  _current: null,

  /** 加载配置（从 localStorage） */
  load() {
    try {
      const saved = localStorage.getItem('mdshare_hotkeys');
      const parsed = saved ? JSON.parse(saved) : {};
      this._current = {};
      for (const def of this._defs) {
        this._current[def.id] = parsed[def.id] || def.defaultKey;
      }
    } catch {
      this._current = {};
      for (const def of this._defs) {
        this._current[def.id] = def.defaultKey;
      }
    }
    return this._current;
  },

  /** 保存配置到 localStorage */
  save() {
    try {
      localStorage.setItem('mdshare_hotkeys', JSON.stringify(this._current));
    } catch {}
  },

  /** 获取指定快捷键的当前配置 */
  get(id) {
    if (!this._current) this.load();
    const def = this._defs.find(d => d.id === id);
    return this._current[id] || (def ? def.defaultKey : '');
  },

  /** 设置指定快捷键 */
  set(id, keyCombo) {
    if (!this._current) this.load();
    this._current[id] = keyCombo;
    this.save();
  },

  /** 重置指定快捷键为默认值 */
  reset(id) {
    const def = this._defs.find(d => d.id === id);
    if (def) this.set(id, def.defaultKey);
  },

  /** 重置所有快捷键 */
  resetAll() {
    for (const def of this._defs) {
      this._current[def.id] = def.defaultKey;
    }
    this.save();
  },

  /** 获取所有快捷键定义 */
  getDefs() { return this._defs; },

  /**
   * 解析快捷键字符串为匹配结构
   * @param {string} str - 如 'Shift+Digit8', 'Shift+Alt', 'Ctrl+Alt+Shift+F12'
   * @returns {{ mods: {ctrl,alt,shift,meta}, code: string|null }}
   */
  parse(str) {
    const parts = str.split('+');
    const mods = { ctrl: false, alt: false, shift: false, meta: false };
    let code = null;
    for (const p of parts) {
      const lower = p.toLowerCase();
      if (lower === 'ctrl' || lower === 'control') mods.ctrl = true;
      else if (lower === 'alt') mods.alt = true;
      else if (lower === 'shift') mods.shift = true;
      else if (lower === 'meta' || lower === 'win') mods.meta = true;
      else code = p; // non-modifier key (e.g. 'Digit8', 'F12')
    }
    return { mods, code };
  },

  /**
   * 检查键盘事件是否匹配已解析的快捷键
   * @param {{ mods, code }} parsed
   * @param {KeyboardEvent} e
   * @returns {boolean}
   */
  match(parsed, e) {
    if (parsed.code) {
      // 修饰键 + 非修饰主键（如 Shift+Digit8, Ctrl+Alt+Shift+F12）
      return e.code === parsed.code
        && e.ctrlKey === parsed.mods.ctrl
        && e.altKey === parsed.mods.alt
        && e.shiftKey === parsed.mods.shift;
    } else {
      // 纯修饰组合（如 Shift+Alt）：当前事件是修饰键且所有要求的修饰符都已满足
      const isModKey = e.code.startsWith('Control') || e.code.startsWith('Alt')
                     || e.code.startsWith('Shift') || e.code.startsWith('Meta');
      if (!isModKey) return false;
      // 当前键按下时，浏览器已经将该键的 modifier 标志设为 true
      const currentMods = {
        ctrl:  e.ctrlKey  || e.code.startsWith('Control'),
        alt:   e.altKey   || e.code.startsWith('Alt'),
        shift: e.shiftKey || e.code.startsWith('Shift'),
        meta:  e.metaKey  || e.code.startsWith('Meta'),
      };
      return currentMods.ctrl === parsed.mods.ctrl
        && currentMods.alt === parsed.mods.alt
        && currentMods.shift === parsed.mods.shift
        && currentMods.meta === parsed.mods.meta;
    }
  },

  /**
   * 检查键盘事件是否匹配指定 id 的快捷键
   * @param {string} id - 快捷键 ID
   * @param {KeyboardEvent} e
   * @returns {boolean}
   */
  matches(id, e) {
    const str = this.get(id);
    if (!str) return false;
    return this.match(this.parse(str), e);
  },

  /**
   * 将快捷键字符串转为用户友好的展示文本
   * @param {string} str - 如 'Shift+Digit8'
   * @returns {string} 如 'Shift + 8'
   */
  toDisplay(str) {
    if (!str) return '';
    const codeNames = {
      'Digit0': '0', 'Digit1': '1', 'Digit2': '2', 'Digit3': '3', 'Digit4': '4',
      'Digit5': '5', 'Digit6': '6', 'Digit7': '7', 'Digit8': '8', 'Digit9': '9',
      'KeyA': 'A', 'KeyB': 'B', 'KeyC': 'C', 'KeyD': 'D', 'KeyE': 'E',
      'KeyF': 'F', 'KeyG': 'G', 'KeyH': 'H', 'KeyI': 'I', 'KeyJ': 'J',
      'KeyK': 'K', 'KeyL': 'L', 'KeyM': 'M', 'KeyN': 'N', 'KeyO': 'O',
      'KeyP': 'P', 'KeyQ': 'Q', 'KeyR': 'R', 'KeyS': 'S', 'KeyT': 'T',
      'KeyU': 'U', 'KeyV': 'V', 'KeyW': 'W', 'KeyX': 'X', 'KeyY': 'Y', 'KeyZ': 'Z',
      'Space': 'Space', 'Escape': 'Esc', 'Backspace': 'Backspace', 'Tab': 'Tab',
      'Enter': 'Enter', 'Delete': 'Delete', 'Insert': 'Insert',
      'Home': 'Home', 'End': 'End', 'PageUp': 'PgUp', 'PageDown': 'PgDn',
      'ArrowUp': '↑', 'ArrowDown': '↓', 'ArrowLeft': '←', 'ArrowRight': '→',
    };
    return str.split('+').map(p => {
      const lower = p.toLowerCase();
      if (lower === 'ctrl' || lower === 'control') return 'Ctrl';
      if (lower === 'alt') return 'Alt';
      if (lower === 'shift') return 'Shift';
      if (lower === 'meta' || lower === 'win') return 'Win';
      return codeNames[p] || p;
    }).join(' + ');
  },

  /**
   * 录制快捷键：返回 Promise<string|null>，用户按下组合键后 resolve
   * Escape 取消返回 null
   */
  record() {
    return new Promise((resolve) => {
      const modifiers = new Set();
      let mainKey = null;
      let done = false;

      function onKeyDown(e) {
        e.preventDefault();
        e.stopPropagation();
        if (done) return;
        if (e.key === 'Escape') { cleanup(); resolve(null); return; }
        if (e.ctrlKey) modifiers.add('Ctrl');
        if (e.altKey) modifiers.add('Alt');
        if (e.shiftKey) modifiers.add('Shift');
        if (e.metaKey) modifiers.add('Meta');
        // 非修饰键 → 确定主键并完成
        if (!['Control', 'Alt', 'Shift', 'Meta'].includes(e.key)) {
          mainKey = e.code;
          finalize();
        }
      }
      function onKeyUp(e) {
        e.preventDefault();
        e.stopPropagation();
        if (done) return;
        // 只有修饰键被按下后释放 → 纯修饰组合
        if (!mainKey && modifiers.size > 0) {
          finalize();
        }
      }
      function finalize() {
        if (done) return;
        done = true;
        cleanup();
        const parts = [...modifiers];
        if (mainKey) parts.push(mainKey);
        resolve(parts.length > 0 ? parts.join('+') : null);
      }
      function cleanup() {
        document.removeEventListener('keydown', onKeyDown, true);
        document.removeEventListener('keyup', onKeyUp, true);
      }
      document.addEventListener('keydown', onKeyDown, true);
      document.addEventListener('keyup', onKeyUp, true);
    });
  },
};

window.HotkeyConfig = HotkeyConfig;

// ── HTML event.code → AT Set 1 扫描码映射表 ──
// 来源：noVNC xtscancodes.js (auto-generated from keymaps.csv)
// 0xE0xx 前缀 = 扩展键

const XT_SCANCODES = {
  "Again": 0xe005,
  "AltLeft": 0x38,
  "AltRight": 0xe038,
  "ArrowDown": 0xe050,
  "ArrowLeft": 0xe04b,
  "ArrowRight": 0xe04d,
  "ArrowUp": 0xe048,
  "AudioVolumeDown": 0xe02e,
  "AudioVolumeMute": 0xe020,
  "AudioVolumeUp": 0xe030,
  "Backquote": 0x29,
  "Backslash": 0x2b,
  "Backspace": 0x0e,
  "BracketLeft": 0x1a,
  "BracketRight": 0x1b,
  "BrowserBack": 0xe06a,
  "BrowserFavorites": 0xe066,
  "BrowserForward": 0xe069,
  "BrowserHome": 0xe032,
  "BrowserRefresh": 0xe067,
  "BrowserSearch": 0xe065,
  "BrowserStop": 0xe068,
  "CapsLock": 0x3a,
  "Comma": 0x33,
  "ContextMenu": 0xe05d,
  "ControlLeft": 0x1d,
  "ControlRight": 0xe01d,
  "Delete": 0xe053,
  "Digit0": 0x0b,
  "Digit1": 0x02,
  "Digit2": 0x03,
  "Digit3": 0x04,
  "Digit4": 0x05,
  "Digit5": 0x06,
  "Digit6": 0x07,
  "Digit7": 0x08,
  "Digit8": 0x09,
  "Digit9": 0x0a,
  "End": 0xe04f,
  "Enter": 0x1c,
  "Equal": 0x0d,
  "Escape": 0x01,
  "F1": 0x3b,
  "F2": 0x3c,
  "F3": 0x3d,
  "F4": 0x3e,
  "F5": 0x3f,
  "F6": 0x40,
  "F7": 0x41,
  "F8": 0x42,
  "F9": 0x43,
  "F10": 0x44,
  "F11": 0x57,
  "F12": 0x58,
  "Find": 0xe041,
  "Help": 0xe075,
  "Home": 0xe047,
  "Insert": 0xe052,
  "IntlBackslash": 0x56,
  "IntlRo": 0x73,
  "IntlYen": 0x7d,
  "KeyA": 0x1e,
  "KeyB": 0x30,
  "KeyC": 0x2e,
  "KeyD": 0x20,
  "KeyE": 0x12,
  "KeyF": 0x21,
  "KeyG": 0x22,
  "KeyH": 0x23,
  "KeyI": 0x17,
  "KeyJ": 0x24,
  "KeyK": 0x25,
  "KeyL": 0x26,
  "KeyM": 0x32,
  "KeyN": 0x31,
  "KeyO": 0x18,
  "KeyP": 0x19,
  "KeyQ": 0x10,
  "KeyR": 0x13,
  "KeyS": 0x1f,
  "KeyT": 0x14,
  "KeyU": 0x16,
  "KeyV": 0x2f,
  "KeyW": 0x11,
  "KeyX": 0x2d,
  "KeyY": 0x15,
  "KeyZ": 0x2c,
  "LaunchApp1": 0xe06b,
  "LaunchApp2": 0xe021,
  "LaunchMail": 0xe06c,
  "MediaPlayPause": 0xe022,
  "MediaSelect": 0xe06d,
  "MediaStop": 0xe024,
  "MediaTrackNext": 0xe019,
  "MediaTrackPrevious": 0xe010,
  "MetaLeft": 0xe05b,
  "MetaRight": 0xe05c,
  "Minus": 0x0c,
  "NumLock": 0x45,
  "Numpad0": 0x52,
  "Numpad1": 0x4f,
  "Numpad2": 0x50,
  "Numpad3": 0x51,
  "Numpad4": 0x4b,
  "Numpad5": 0x4c,
  "Numpad6": 0x4d,
  "Numpad7": 0x47,
  "Numpad8": 0x48,
  "Numpad9": 0x49,
  "NumpadAdd": 0x4e,
  "NumpadComma": 0x7e,
  "NumpadDecimal": 0x53,
  "NumpadDivide": 0xe035,
  "NumpadEnter": 0xe01c,
  "NumpadEqual": 0x59,
  "NumpadMultiply": 0x37,
  "NumpadSubtract": 0x4a,
  "PageDown": 0xe051,
  "PageUp": 0xe049,
  "Pause": 0xe046,
  "Period": 0x34,
  "Power": 0xe05e,
  "PrintScreen": 0x54,
  "Quote": 0x28,
  "ScrollLock": 0x46,
  "Semicolon": 0x27,
  "ShiftLeft": 0x2a,
  "ShiftRight": 0x36,
  "Slash": 0x35,
  "Sleep": 0xe05f,
  "Space": 0x39,
  "Tab": 0x0f,
  "WakeUp": 0xe063,
};

// ── RemoteControl 单例 ──

const RemoteControl = {
  /** @type {HTMLElement|null} */
  _videoEl: null,
  /** @type {Function|null} 发送函数 (msg) => void */
  _sendFn: null,
  /** @type {boolean} */
  _enabled: false,
  /** @type {string} */
  _channelId: '',
  /** @type {Object} 远端屏幕信息 */
  _screenInfo: null,
  /** @type {boolean} Pointer Lock 模式 */
  _pointerLocked: false,
  /** 鼠标移动节流：上一帧时间 */
  _lastMoveTime: 0,
  /** 鼠标移动最小间隔 (ms) — 默认约 40fps，减少链路排队延迟 */
  _moveThrottle: 25,
  /** 存储绑定的事件处理器，用于 disable 时移除 */
  _handlers: {},
  /** 缓存的视频内容矩形 */
  _cachedVideoRect: null,
  /** 缓存的视频内在尺寸 [w, h]，用于检测分辨率变化 */
  _cachedVideoDims: null,
  /** ResizeObserver 实例 */
  _resizeObserver: null,
  /** 双指触摸标记 */
  _twoFingerActive: false,
  /** 合并发送：待发送绝对移动 */
  _pendingAbsMove: null,
  /** 合并发送：待发送相对移动 */
  _pendingRelMove: null,
  /** 合并发送定时器 */
  _moveFlushTimer: null,
  /** 当前已按下的修饰键集合 (scancode → {sc, ext}) — 断开/禁用时自动释放 */
  _pressedModifiers: new Map(),
  /** 退出全屏回调 */
  _onExitFullscreen: null,
  /** 切换 Pointer Lock 回调（由 app.js 注册，用于更新 UI） */
  _onPointerLockToggle: null,
  /** 开始/结束远控切换回调（由 app.js 注册） */
  _onToggleRemoteControl: null,

  /**
   * 初始化远程控制模块
   * @param {HTMLElement} videoEl - 视频播放容器元素
   * @param {Function} sendFn - 发送输入事件的函数 (msg: object) => void
   */
  init(videoEl, sendFn) {
    this._videoEl = videoEl;
    this._sendFn = sendFn;
  },

  /**
   * 启用远程控制
   * @param {string} channelId
   */
  enable(channelId) {
    if (this._enabled) return;
    if (!this._videoEl || !this._sendFn) {
      console.error('[RemoteControl] 未初始化，请先调用 init()');
      return;
    }
    this._enabled = true;
    this._channelId = channelId;

    const el = this._videoEl;

    // 让视频容器可聚焦（接收键盘事件）
    el.tabIndex = 0;
    el.focus();

    // ── 键盘事件 ──
    this._handlers.keydown = (e) => {
      if (!this._enabled) return;

      // 可自定义快捷键：退出全屏
      if (HotkeyConfig.matches('exitFullscreen', e)) {
        e.preventDefault();
        e.stopPropagation();
        if (this._onExitFullscreen) this._onExitFullscreen();
        return;
      }
      // 可自定义快捷键：切换鼠标模式（绝对/相对）
      if (HotkeyConfig.matches('togglePointerLock', e)) {
        e.preventDefault();
        e.stopPropagation();
        // 释放已发送的修饰键（Shift+Alt 的第一个键可能已发送到远端）
        this._releaseAllPressedModifiers();
        const locked = this.togglePointerLock();
        if (this._onPointerLockToggle) this._onPointerLockToggle(locked);
        return;
      }
      // 可自定义快捷键：开始/结束远控切换
      if (HotkeyConfig.matches('toggleRemoteControl', e)) {
        e.preventDefault();
        e.stopPropagation();
        if (this._onToggleRemoteControl) this._onToggleRemoteControl();
        return;
      }

      e.preventDefault();
      e.stopPropagation();
      this._sendKey(e, true);
    };
    this._handlers.keyup = (e) => {
      if (!this._enabled) return;
      e.preventDefault();
      e.stopPropagation();
      this._sendKey(e, false);
    };
    // 窗口失焦 → 释放所有已按下的修饰键（防止远端粘键）
    this._handlers.blur = () => {
      this._releaseAllPressedModifiers();
    };
    document.addEventListener('keydown', this._handlers.keydown, true);
    document.addEventListener('keyup', this._handlers.keyup, true);
    window.addEventListener('blur', this._handlers.blur);

    // ── 鼠标移动 ──
    this._handlers.mousemove = (e) => {
      if (!this._enabled) return;
      if (document.pointerLockElement === el) {
        // Pointer Lock: 相对移动 — 补偿 DPI 缩放
        const dpr = window.devicePixelRatio || 1;
        this._queueMove({ t: 'mm', dx: Math.round(e.movementX * dpr), dy: Math.round(e.movementY * dpr) });
      } else {
        // 绝对坐标（归一化 0~1）— 基于视频实际渲染区域，非容器
        const vRect = this._getVideoContentRect();
        const x = (e.clientX - vRect.left) / vRect.width;
        const y = (e.clientY - vRect.top) / vRect.height;
        if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
          this._queueMove({ t: 'ma', x: +x.toFixed(5), y: +y.toFixed(5) });
        }
      }
    };
    el.addEventListener('mousemove', this._handlers.mousemove);

    // ── 鼠标按钮 ──
    this._handlers.mousedown = (e) => {
      if (!this._enabled) return;
      e.preventDefault();
      // 不自动请求 Pointer Lock（避免 WebView2 "按ESC显示光标" 提示）
      // 用户可通过工具栏按钮手动切换 Pointer Lock 模式
      // 先刷新挂起的鼠标移动，确保点击位置准确
      this._flushPendingMove();
      this._sendFn({ t: 'md', b: e.button });
    };
    this._handlers.mouseup = (e) => {
      if (!this._enabled) return;
      e.preventDefault();
      this._flushPendingMove();
      this._sendFn({ t: 'mu', b: e.button });
    };
    el.addEventListener('mousedown', this._handlers.mousedown);
    el.addEventListener('mouseup', this._handlers.mouseup);

    // ── 滚轮 ──
    this._handlers.wheel = (e) => {
      if (!this._enabled) return;
      e.preventDefault();
      // 标准化滚动量（Chrome deltaY=100 = 3行, line mode）
      let dy = e.deltaY;
      let dx = e.deltaX;
      // deltaMode: 0=pixel, 1=line, 2=page
      if (e.deltaMode === 1) { dy *= 40; dx *= 40; }
      else if (e.deltaMode === 2) { dy *= 800; dx *= 800; }
      // Windows WHEEL_DELTA = 120
      this._sendFn({ t: 'mw', dx: Math.round(-dx), dy: Math.round(-dy) });
    };
    el.addEventListener('wheel', this._handlers.wheel, { passive: false });

    // ── 右键菜单禁止 ──
    this._handlers.contextmenu = (e) => {
      if (this._enabled) e.preventDefault();
    };
    el.addEventListener('contextmenu', this._handlers.contextmenu);

    // ── Pointer Lock 状态变化 ──
    this._handlers.pointerlockchange = () => {
      const wasLocked = this._pointerLocked;
      this._pointerLocked = (document.pointerLockElement === el);
      // 如果远控仍在进行但 Pointer Lock 被意外退出（如浏览器 ESC），自动重新请求
      if (wasLocked && !this._pointerLocked && this._enabled) {
        setTimeout(() => {
          if (this._enabled && !document.pointerLockElement && this._videoEl) {
            this._videoEl.requestPointerLock?.();
          }
        }, 300);
      }
    };
    document.addEventListener('pointerlockchange', this._handlers.pointerlockchange);

    // ── ResizeObserver 监听容器尺寸变化，即时失效坐标缓存 ──
    if (typeof ResizeObserver !== 'undefined') {
      this._resizeObserver = new ResizeObserver(() => {
        this._cachedVideoRect = null;
      });
      this._resizeObserver.observe(el);
    }

    // ── 触摸事件（移动端适配） ──
    this._setupTouchHandlers(el);

    console.log(`[RemoteControl] 已启用 (频道: ${channelId})`);
  },

  /**
   * 禁用远程控制（幂等，多次调用安全）
   */
  disable() {
    const wasEnabled = this._enabled;
    this._enabled = false;
    this._channelId = '';

    const el = this._videoEl;

    // 释放远端所有已按下的修饰键（防止粘键）
    if (wasEnabled) {
      this._releaseAllPressedModifiers();
    }

    // 退出 Pointer Lock
    if (document.pointerLockElement) {
      document.exitPointerLock();
    }

    // 移除键盘事件
    if (this._handlers.keydown) {
      document.removeEventListener('keydown', this._handlers.keydown, true);
      document.removeEventListener('keyup', this._handlers.keyup, true);
    }
    // 移除窗口 blur 监听
    if (this._handlers.blur) {
      window.removeEventListener('blur', this._handlers.blur);
    }

    // 移除鼠标事件
    if (el && this._handlers.mousemove) {
      el.removeEventListener('mousemove', this._handlers.mousemove);
      el.removeEventListener('mousedown', this._handlers.mousedown);
      el.removeEventListener('mouseup', this._handlers.mouseup);
      el.removeEventListener('wheel', this._handlers.wheel);
      el.removeEventListener('contextmenu', this._handlers.contextmenu);
    }

    // 移除 Pointer Lock 监听
    if (this._handlers.pointerlockchange) {
      document.removeEventListener('pointerlockchange', this._handlers.pointerlockchange);
    }

    // 移除触摸事件
    if (el && this._handlers.touchstart) {
      el.removeEventListener('touchstart', this._handlers.touchstart);
      el.removeEventListener('touchmove', this._handlers.touchmove);
      el.removeEventListener('touchend', this._handlers.touchend);
    }

    this._handlers = {};
    this._cachedVideoRect = null;
    this._cachedVideoDims = null;
    this._twoFingerActive = false;
    if (this._resizeObserver) {
      this._resizeObserver.disconnect();
      this._resizeObserver = null;
    }
    this._pendingAbsMove = null;
    this._pendingRelMove = null;
    if (this._moveFlushTimer) {
      clearTimeout(this._moveFlushTimer);
      this._moveFlushTimer = null;
    }

    // 恢复视频容器状态：移除 tabIndex 和 cursor 覆盖
    if (el) {
      el.removeAttribute('tabindex');
      el.style.cursor = '';
      el.blur();
      // 确保焦点转移到 body，防止 WebView2 失焦导致快捷键失效
      document.body.focus();
    }

    if (wasEnabled) console.log('[RemoteControl] 已禁用');
  },

  /**
   * 更新远端屏幕信息
   * @param {Object} info - { primaryWidth, primaryHeight, dpiX, dpiY, ... }
   */
  setScreenInfo(info) {
    this._screenInfo = info;
  },

  /**
   * 发送特殊组合键
   * @param {string} combo - 如 'ctrl+alt+del', 'alt+tab', 'win' 等
   */
  sendCombo(combo) {
    if (!this._enabled || !this._sendFn) return;
    this._sendFn({ t: 'combo', combo });
  },

  /**
   * 发送文本（Unicode 直输，用于 IME/粘贴场景）
   * @param {string} text
   */
  sendText(text) {
    if (!this._enabled || !this._sendFn) return;
    this._sendFn({ t: 'text', text });
  },

  /** 是否已启用 */
  isEnabled() {
    return this._enabled;
  },

  /** 是否处于 Pointer Lock 模式 */
  isPointerLocked() {
    return this._pointerLocked;
  },

  /**
   * 手动切换 Pointer Lock 模式
   * @returns {boolean} 切换后是否处于 Pointer Lock
   */
  togglePointerLock() {
    if (!this._enabled || !this._videoEl) return false;
    if (document.pointerLockElement === this._videoEl) {
      document.exitPointerLock();
      return false;
    } else if (this._videoEl.requestPointerLock) {
      this._videoEl.requestPointerLock();
      return true;
    }
    return false;
  },

  /**
   * 请求进入 Pointer Lock 模式（不切换，仅进入）
   * @returns {boolean} 是否成功请求
   */
  requestPointerLock() {
    if (!this._enabled || !this._videoEl) return false;
    if (document.pointerLockElement === this._videoEl) return true; // 已经在 Pointer Lock
    if (this._videoEl.requestPointerLock) {
      this._videoEl.requestPointerLock();
      return true;
    }
    return false;
  },

  /**
   * 注册退出全屏回调（快捷键触发）
   */
  onExitFullscreen(cb) {
    this._onExitFullscreen = cb;
  },

  /**
   * 注册 Pointer Lock 切换回调
   * @param {Function} cb - (isLocked: boolean) => void
   */
  onPointerLockToggle(cb) {
    this._onPointerLockToggle = cb;
  },

  /**
   * 注册开始/结束远控切换回调（快捷键触发）
   */
  onToggleRemoteControl(cb) {
    this._onToggleRemoteControl = cb;
  },

  // ── 内部方法 ──

  /**
   * 计算视频实际渲染区域（处理 object-fit:contain 的 letterbox/pillarbox）
   * 返回 { left, top, width, height } 相对于 viewport
   */
  _getVideoContentRect() {
    const el = this._videoEl;
    const video = el.querySelector('video');

    // 缓存命中：容器未 resize 且视频内在尺寸未变
    if (this._cachedVideoRect && video) {
      if (this._cachedVideoDims &&
          video.videoWidth === this._cachedVideoDims[0] &&
          video.videoHeight === this._cachedVideoDims[1]) {
        return this._cachedVideoRect;
      }
    }

    const containerRect = el.getBoundingClientRect();

    if (!video || !video.videoWidth || !video.videoHeight) {
      this._cachedVideoRect = containerRect;
      this._cachedVideoDims = null;
      return containerRect;
    }

    const containerW = containerRect.width;
    const containerH = containerRect.height;
    const videoAR = video.videoWidth / video.videoHeight;
    const containerAR = containerW / containerH;

    let renderW, renderH, offsetX, offsetY;

    if (videoAR > containerAR) {
      // 视频更宽 → 填满宽度，上下留黑边
      renderW = containerW;
      renderH = containerW / videoAR;
      offsetX = 0;
      offsetY = (containerH - renderH) / 2;
    } else {
      // 容器更宽 → 填满高度，左右留黑边
      renderH = containerH;
      renderW = containerH * videoAR;
      offsetX = (containerW - renderW) / 2;
      offsetY = 0;
    }

    const rect = {
      left: containerRect.left + offsetX,
      top: containerRect.top + offsetY,
      width: renderW,
      height: renderH,
    };
    this._cachedVideoRect = rect;
    this._cachedVideoDims = [video.videoWidth, video.videoHeight];
    return rect;
  },

  /**
   * 合并高频鼠标移动，避免网络/桥接队列堆积导致远控延迟不断增大
   */
  _queueMove(moveMsg) {
    if (!this._enabled || !this._sendFn) return;

    if (moveMsg.t === 'ma') {
      // 绝对移动：只保留最新位置
      this._pendingAbsMove = moveMsg;
      this._pendingRelMove = null;
    } else if (moveMsg.t === 'mm') {
      // 相对移动：累计位移，减少消息数量
      if (!this._pendingRelMove) {
        this._pendingRelMove = { t: 'mm', dx: 0, dy: 0 };
      }
      this._pendingRelMove.dx += moveMsg.dx;
      this._pendingRelMove.dy += moveMsg.dy;
      this._pendingAbsMove = null;
    }

    const now = performance.now();
    const elapsed = now - this._lastMoveTime;
    const flush = () => {
      this._moveFlushTimer = null;
      if (!this._enabled || !this._sendFn) return;

      this._doFlushMove();
    };

    if (elapsed >= this._moveThrottle && !this._moveFlushTimer) {
      flush();
      return;
    }

    if (!this._moveFlushTimer) {
      const delay = Math.max(1, this._moveThrottle - elapsed);
      this._moveFlushTimer = setTimeout(flush, delay);
    }
  },

  /**
   * 立即发送挂起的鼠标移动（供 mousedown/mouseup 调用，确保点击位置准确）
   */
  _flushPendingMove() {
    if (this._moveFlushTimer) {
      clearTimeout(this._moveFlushTimer);
      this._moveFlushTimer = null;
    }
    this._doFlushMove();
  },

  /** 内部：执行 flush */
  _doFlushMove() {
    if (!this._enabled || !this._sendFn) return;
    if (this._pendingRelMove && (this._pendingRelMove.dx !== 0 || this._pendingRelMove.dy !== 0)) {
      this._sendFn(this._pendingRelMove);
    } else if (this._pendingAbsMove) {
      this._sendFn(this._pendingAbsMove);
    }
    this._pendingAbsMove = null;
    this._pendingRelMove = null;
    this._lastMoveTime = performance.now();
  },

  // ── 修饰键追踪（防止粘键） ──

  /** 修饰键 event.code 列表 */
  _MODIFIER_CODES: new Set([
    'ShiftLeft', 'ShiftRight', 'ControlLeft', 'ControlRight',
    'AltLeft', 'AltRight', 'MetaLeft', 'MetaRight',
  ]),

  /** 释放所有已追踪按下的修饰键（发送 keyup 到远端） */
  _releaseAllPressedModifiers() {
    if (!this._sendFn || this._pressedModifiers.size === 0) return;
    for (const [, info] of this._pressedModifiers) {
      try { this._sendFn({ t: 'ku', sc: info.sc, ext: info.ext }); } catch {}
    }
    this._pressedModifiers.clear();
  },

  /**
   * 发送按键事件
   * 使用 noVNC xtscancodes 映射 event.code → AT Set 1 扫描码
   */
  _sendKey(e, down) {
    if (!this._sendFn) return;

    const scanFull = XT_SCANCODES[e.code];
    if (scanFull === undefined) return;

    // 0xE0xx = 扩展键，取低字节为实际扫描码
    const extended = (scanFull & 0xE000) !== 0;
    const sc = extended ? (scanFull & 0xFF) : scanFull;

    // 追踪修饰键按下/释放状态
    if (this._MODIFIER_CODES.has(e.code)) {
      if (down) {
        this._pressedModifiers.set(e.code, { sc, ext: extended });
      } else {
        this._pressedModifiers.delete(e.code);
      }
    }

    this._sendFn({
      t: down ? 'kd' : 'ku',
      sc: sc,
      ext: extended,
    });
  },

  /**
   * 触摸手势处理（参考 noVNC gesturehandler.js）
   * 简化版：单指=鼠标移动+左键, 双指=右键
   */
  _setupTouchHandlers(el) {
    let _touchStartX = 0, _touchStartY = 0;
    let _touchMoved = false;
    let _touchStartTime = 0;
    const TAP_THRESHOLD = 10;   // 像素
    const TAP_TIMEOUT   = 300;  // ms

    this._handlers.touchstart = (e) => {
      if (!this._enabled) return;
      e.preventDefault();
      const touch = e.touches[0];
      _touchStartX = touch.clientX;
      _touchStartY = touch.clientY;
      _touchMoved = false;
      _touchStartTime = performance.now();

      // 双指 → 右键按下位置
      if (e.touches.length === 2) {
        this._twoFingerActive = true;
        const vRect = this._getVideoContentRect();
        const x = (touch.clientX - vRect.left) / vRect.width;
        const y = (touch.clientY - vRect.top) / vRect.height;
        this._flushPendingMove();
        this._sendFn({ t: 'ma', x: +x.toFixed(5), y: +y.toFixed(5) });
        this._sendFn({ t: 'md', b: 2 });
      }
    };

    this._handlers.touchmove = (e) => {
      if (!this._enabled) return;
      e.preventDefault();
      if (e.touches.length !== 1) return;
      const touch = e.touches[0];
      const dx = touch.clientX - _touchStartX;
      const dy = touch.clientY - _touchStartY;
      if (Math.abs(dx) > TAP_THRESHOLD || Math.abs(dy) > TAP_THRESHOLD) {
        _touchMoved = true;
      }
      // 通过合并通道发送，避免触屏高频消息堆积
      const vRect = this._getVideoContentRect();
      const x = (touch.clientX - vRect.left) / vRect.width;
      const y = (touch.clientY - vRect.top) / vRect.height;
      if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
        this._queueMove({ t: 'ma', x: +x.toFixed(5), y: +y.toFixed(5) });
      }
    };

    this._handlers.touchend = (e) => {
      if (!this._enabled) return;
      e.preventDefault();

      // 释放右键（仅在双指操作时）
      if (this._twoFingerActive && e.touches.length === 0) {
        this._flushPendingMove();
        this._sendFn({ t: 'mu', b: 2 });
        this._twoFingerActive = false;
        return;
      }

      const elapsed = performance.now() - _touchStartTime;
      if (!_touchMoved && elapsed < TAP_TIMEOUT && e.touches.length === 0) {
        // 单指轻触 → 左键点击
        const touch = e.changedTouches[0];
        const vRect = this._getVideoContentRect();
        const x = (touch.clientX - vRect.left) / vRect.width;
        const y = (touch.clientY - vRect.top) / vRect.height;
        this._flushPendingMove();
        this._sendFn({ t: 'ma', x: +x.toFixed(5), y: +y.toFixed(5) });
        this._sendFn({ t: 'md', b: 0 });
        setTimeout(() => this._sendFn({ t: 'mu', b: 0 }), 50);
      }
    };

    el.addEventListener('touchstart', this._handlers.touchstart, { passive: false });
    el.addEventListener('touchmove', this._handlers.touchmove, { passive: false });
    el.addEventListener('touchend', this._handlers.touchend, { passive: false });
  },
};

// 导出给全局作用域（非模块模式）
window.RemoteControl = RemoteControl;

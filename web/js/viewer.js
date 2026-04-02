/**
 * viewer.js — 拉流端（Viewer）：观看屏幕共享 + 麦克风上行
 *
 * 优化项：
 *  1. 下行网络质量监控 — 对应海马 haimaReceiverAckCount/LossCount
 *  2. 麦克风上行       — 观看端麦克风串流到推流端（云游戏场景）
 *  3. 实时统计采集     — 延迟 / 码率 / 丢包率
 */
const Viewer = {
  client:           null,
  channelInfo:      null,
  isViewing:        false,
  remoteVideoTrack: null,

  // 麦克风（独立语音覆盖频道，与 Go-Live 频道分离）
  _voiceClient: null,
  _micTrack:    null,
  _micEnabled:  false,

  // 统计定时器
  _statsTimer: null,

  /** 后端地址（可配置） */
  API_BASE: AppConfig.resolveServerUrl(),

  /** 构造带鉴权的请求头（Bearer session token） */
  _apiHeaders() {
    const h = { 'Content-Type': 'application/json' };
    const token = AppConfig.getSessionToken();
    if (token) h['Authorization'] = `Bearer ${token}`;
    return h;
  },

  /** 加入频道 */
  async joinChannel(channelId, videoContainer) {
    if (this.isViewing) return;

    // 1. 获取凭据
    const resp = await fetch(`${this.API_BASE}/api/channel/join`, {
      method:  'POST',
      headers: this._apiHeaders(),
      body:    JSON.stringify({ channelId })
    });
    const result = await resp.json();
    if (!result.success) throw new Error(result.error || '加入频道失败');
    this.channelInfo = result.data;
    console.log('[Viewer] 凭据获取成功:', result.data.channelName);

    // 2. 创建客户端（H.264，与推流端一致）
    // live 模式：码率更稳定，画质更好，延迟略高但更流畅
    this.client = AgoraRTC.createClient({ mode: 'live', codec: 'h264' });
    await this.client.setClientRole('audience');

    // 3. 监听远端发布事件
    this.client.on('user-published', async (user, mediaType) => {
      console.log(`[Viewer] 远端用户发布: UID=${user.uid}, type=${mediaType}`);
      await this.client.subscribe(user, mediaType);

      if (mediaType === 'video') {
        this.remoteVideoTrack = user.videoTrack;
        videoContainer.innerHTML = '';
        videoContainer.classList.remove('hidden');
        user.videoTrack.play(videoContainer, { fit: 'contain' });
        const placeholder = document.getElementById('viewerPlaceholder');
        if (placeholder) placeholder.classList.add('hidden');
        console.log('[Viewer] 视频播放中');
      }
      if (mediaType === 'audio') {
        user.audioTrack.play();
        console.log('[Viewer] 音频播放中');
      }
    });

    this.client.on('user-unpublished', (user, mediaType) => {
      console.log(`[Viewer] 远端用户取消发布: UID=${user.uid}, type=${mediaType}`);
      if (mediaType === 'video') {
        this.remoteVideoTrack = null;
        const placeholder = document.getElementById('viewerPlaceholder');
        if (placeholder) {
          placeholder.classList.remove('hidden');
          placeholder.querySelector('p').textContent = '共享已结束';
        }
        videoContainer.classList.add('hidden');
        videoContainer.innerHTML = '';
      }
    });

    this.client.on('user-left', (user) => {
      console.log(`[Viewer] 远端用户离开: UID=${user.uid}`);
      this.remoteVideoTrack = null;
      const placeholder = document.getElementById('viewerPlaceholder');
      if (placeholder) {
        placeholder.classList.remove('hidden');
        placeholder.querySelector('p').textContent = '共享者已离开';
      }
      videoContainer.classList.add('hidden');
      videoContainer.innerHTML = '';
      if (this._onEndCallback) this._onEndCallback();
    });

    // 4. 下行网络质量监控（对应 haimaReceiverAckLossCount）
    this.client.on('network-quality', (stats) => {
      const dq = stats.downlinkNetworkQuality; // 1好 → 6断
      if (this._onNetworkQuality) this._onNetworkQuality(dq);

      // 连续差 → 通知宿主
      if (dq >= 4) {
        Bridge.postMessage('viewer-quality-warning', { downlinkQuality: dq });
      }
    });

    // 5. 加入频道
    const info = this.channelInfo;
    const joinAppId = info.appId;
    const joinUid   = info.uid != null ? info.uid : null;
    await this.client.join(joinAppId, info.channelName, info.token, joinUid);
    console.log('[Viewer] 已加入频道:', info.channelName);

    // 6. 启动接收端统计上报
    this._startStatsReporter(channelId);

    // 7. 建立 Socket.io 连接（用于远程控制 + 消息等）
    this._connectSocket(channelId);

    this.isViewing = true;
    return info;
  },

  /** 离开频道 */
  async leaveChannel() {
    this._stopStatsReporter();
    await this.disableMic();

    // 停止远控
    this.stopRemoteControl();
    this._disconnectSocket();

    // 通知后端释放观众账号
    const info = this.channelInfo;
    if (info && info.channelId) {
      try {
        await fetch(`${this.API_BASE}/api/channel/${info.channelId}/leave`, {
          method: 'POST',
          headers: this._apiHeaders(),
          body: JSON.stringify({}),
        });
      } catch (e) {
        console.warn('[Viewer] 释放观众账号失败:', e.message);
      }
    }

    if (this.client) {
      await this.client.leave();
      this.client = null;
    }
    this.isViewing        = false;
    this.channelInfo      = null;
    this.remoteVideoTrack = null;
    console.log('[Viewer] 已离开频道');
  },

  // ─── 麦克风上行（独立语音覆盖频道） ──────────────────────────
  /**
   * 开启麦克风：加入独立语音覆盖频道并 publish 麦克风音频。
   *
   * Go-Live Agora token 是 audience 权限，不允许 viewer publish。
   * 使用自有 Agora App ID + PUBLISHER token 的独立语音频道来传输麦克风音频。
   * Broadcaster 的 screen-share.js 会在同一个语音频道中 subscribe 并输出到 VB-Cable。
   */
  async enableMic() {
    if (this._micEnabled || !this.channelInfo) return;
    try {
      // 1. 获取独立语音频道凭据
      const resp = await fetch(`${this.API_BASE}/api/channel/${this.channelInfo.channelId}/voice-token`, {
        method: 'POST',
        headers: this._apiHeaders(),
      });
      const result = await resp.json();
      if (!result.success) throw new Error(result.error || '获取语音凭据失败');
      const { appId, channelName, token, uid } = result.data;

      // 2. 创建独立语音客户端（rtc 模式，延迟最低）
      this._voiceClient = AgoraRTC.createClient({ mode: 'rtc', codec: 'vp8' });
      await this._voiceClient.join(appId, channelName, token, uid);
      console.log('[Viewer] 已加入语音覆盖频道:', channelName);

      // 3. 创建麦克风音轨并发布
      this._micTrack = await AgoraRTC.createMicrophoneAudioTrack({
        AEC: true,  // 回声消除
        ANS: true,  // 噪声抑制
        AGC: true   // 自动增益
      });
      await this._voiceClient.publish([this._micTrack]);
      this._micEnabled = true;
      console.log('[Viewer] 麦克风已开启并推送（语音覆盖频道）');
      Bridge.postMessage('mic-state', { enabled: true });
    } catch (e) {
      // 清理半成品
      if (this._micTrack) { this._micTrack.close(); this._micTrack = null; }
      if (this._voiceClient) {
        await this._voiceClient.leave().catch(() => {});
        this._voiceClient = null;
      }
      console.error('[Viewer] 开启麦克风失败:', e.message);
      throw e;
    }
  },

  /** 关闭麦克风并离开语音覆盖频道 */
  async disableMic() {
    if (!this._micEnabled) return;
    try {
      if (this._micTrack) {
        await this._voiceClient?.unpublish([this._micTrack]).catch(() => {});
        this._micTrack.close();
        this._micTrack = null;
      }
      if (this._voiceClient) {
        await this._voiceClient.leave().catch(() => {});
        this._voiceClient = null;
      }
      this._micEnabled = false;
      console.log('[Viewer] 麦克风已关闭，已离开语音覆盖频道');
      Bridge.postMessage('mic-state', { enabled: false });
    } catch (e) {
      console.warn('[Viewer] 关闭麦克风失败:', e.message);
    }
  },

  async toggleMic() {
    if (this._micEnabled) {
      await this.disableMic();
    } else {
      await this.enableMic();
    }
    return this._micEnabled;
  },

  isMicEnabled() { return this._micEnabled; },

  // ─── 实时统计上报 ─────────────────────────────────────────
  _startStatsReporter(channelId) {
    this._stopStatsReporter();
    this._statsTimer = setInterval(() => {
      if (!this.client || !this.isViewing) return;
      try {
        const rtc = this.client.getRTCStats();
        const remoteVideoStats = this.client.getRemoteVideoStats();

        // 取第一个远端用户的视频统计
        const firstUid = Object.keys(remoteVideoStats)[0];
        const vs       = firstUid ? remoteVideoStats[firstUid] : {};

        const payload = {
          // 对应 haimaReceiverAckCount (RTT)
          rttMs:           rtc.RTT ?? 0,
          // 接收码率 kbps
          recvBitrateKbps: Math.round((vs.receiveBitrate ?? 0) / 1000),
          // 丢包率 % (对应 haimaReceiverAckLossCount)
          packetLossRate:  vs.packetLossRate ?? 0,
          // 接收帧率
          recvFrameRate:   vs.receiveFramRate ?? 0,
          // 端到端延迟
          e2eDelayMs:      vs.end2EndDelay ?? 0
        };

        fetch(`${this.API_BASE}/api/channel/${channelId}/stats`, {
          method:  'POST',
          headers: this._apiHeaders(),
          body:    JSON.stringify({
            role: 'subscriber',
            ...payload
          })
        }).catch(() => {});

        if (this._onStats) this._onStats(payload);

      } catch (e) {
        console.warn('[Viewer] 统计采集失败:', e.message);
      }
    }, 5000);
  },

  _stopStatsReporter() {
    if (this._statsTimer) {
      clearInterval(this._statsTimer);
      this._statsTimer = null;
    }
  },

  // ─── 远程控制（原生 WebSocket） ─────────────────────────────
  /** @type {WebSocket|null} */
  _socket: null,
  /** @type {boolean} 远控是否激活 */
  _rcActive: false,
  /** @type {number|null} 自动重连定时器 */
  _wsReconnectTimer: null,
  /** @type {number} 重连次数 */
  _wsReconnectCount: 0,
  /** @type {boolean} 是否已绑定 Bridge 消息监听 */
  _bridgeBound: false,
  /** @type {boolean} Native 系统键拦截是否启用 */
  _nativeKeyhookEnabled: false,
  _rcVideoEl: null,
  /** @type {boolean} 控制端光标是否可见(仅绝对模式下为true，Pointer Lock下为false) */
  _viewerCursorVisible: false,
  /** WebSocket 心跳定时器 */
  _wsPingTimer: null,

  _ensureBridgeBindings() {
    if (this._bridgeBound || !Bridge.isWebView2) return;
    this._bridgeBound = true;
    Bridge.onMessage((msg) => {
      if (!msg || !msg.type) return;
      if (msg.type === 'viewer-keyhook-combo') {
        const combo = msg.data?.combo || msg.combo;
        if (!combo) return;
        // 检查是否匹配本地快捷键（如 Shift+F9 切换远控）
        const comboLower = combo.toLowerCase();
        if (typeof HotkeyConfig !== 'undefined') {
          const rcKey = HotkeyConfig.get('toggleRemoteControl');
          if (rcKey && rcKey.toLowerCase().replace(/\+/g, '+') === comboLower) {
            if (this._rcActive) {
              this.stopRemoteControl();
            } else if (this._rcVideoEl) {
              this.startRemoteControl(this._rcVideoEl);
            }
            return;
          }
        }
        // 非本地快捷键，转发到远端
        if (!this._rcActive || !this._socket || this._socket.readyState !== WebSocket.OPEN) return;
        this._socket.send(JSON.stringify({ type: 'rc-input', t: 'combo', combo }));
      }
    });
  },

  _startNativeKeyhook() {
    if (!Bridge.isWebView2 || this._nativeKeyhookEnabled) return;
    this._ensureBridgeBindings();
    Bridge.postMessage('viewer-keyhook-start');
    this._nativeKeyhookEnabled = true;
  },

  _stopNativeKeyhook() {
    if (!Bridge.isWebView2 || !this._nativeKeyhookEnabled) return;
    Bridge.postMessage('viewer-keyhook-stop');
    this._nativeKeyhookEnabled = false;
  },

  /**
   * 建立 WebSocket 连接（鉴权 + 频道信息通过 URL query 传递）
   * @param {string} channelId
   */
  _connectSocket(channelId) {
    if (this._socket) return;
    const serverUrl = AppConfig.resolveServerUrl().replace(/^http/, 'ws');
    const token = AppConfig.getSessionToken();
    const url = `${serverUrl}/ws?token=${encodeURIComponent(token)}&channel=${encodeURIComponent(channelId)}&role=viewer`;

    const ws = new WebSocket(url);
    this._socket = ws;

    ws.onopen = () => {
      console.log('[Viewer] WebSocket 已连接');
      this._wsReconnectCount = 0;
      // 启动心跳：每 30s 发送 ping，防止 Cloudflare 100s 空闲超时 / NAT 超时
      if (this._wsPingTimer) clearInterval(this._wsPingTimer);
      this._wsPingTimer = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) ws.send('{"type":"ping"}');
      }, 30000);
    };

    ws.onmessage = (evt) => {
      let msg;
      try { msg = JSON.parse(evt.data); } catch { return; }
      if (!msg || !msg.type) return;

      switch (msg.type) {
        case 'rc-started':
          console.log('[Viewer] 远控已启动:', msg);
          this._rcActive = true;
          this._viewerCursorVisible = false;
          if (typeof RemoteControl !== 'undefined' && this._rcVideoEl) {
            this._rcVideoEl.style.cursor = 'none';
            RemoteControl.enable(this.channelInfo?.channelId || '');
            // 自动进入 Pointer Lock 模式（延迟 300ms 确保 enable 和 fullscreen 完成）
            setTimeout(() => {
              if (this._rcActive && typeof RemoteControl !== 'undefined') {
                RemoteControl.requestPointerLock();
              }
            }, 300);
          }
          this._startNativeKeyhook();
          if (this._onRemoteControlState) this._onRemoteControlState(true);
          break;
        case 'rc-stop':
          console.log('[Viewer] 远控已停止:', msg);
          this._rcActive = false;
          this._viewerCursorVisible = false;
          if (this._rcVideoEl) this._rcVideoEl.style.cursor = '';
          this._stopNativeKeyhook();
          if (typeof RemoteControl !== 'undefined') RemoteControl.disable();
          if (this._onRemoteControlState) this._onRemoteControlState(false);
          break;
        case 'rc-error':
          console.error('[Viewer] 远控错误:', msg.error);
          this._rcActive = false;
          this._viewerCursorVisible = false;
          if (this._rcVideoEl) this._rcVideoEl.style.cursor = '';
          this._stopNativeKeyhook();
          if (typeof RemoteControl !== 'undefined') RemoteControl.disable();
          if (this._onRemoteControlState) this._onRemoteControlState(false);
          if (this._onRemoteControlError) this._onRemoteControlError(msg.error);
          break;
        case 'rc-screen-info':
          console.log('[Viewer] 收到远端屏幕信息:', msg);
          if (typeof RemoteControl !== 'undefined') RemoteControl.setScreenInfo(msg);
          break;
        case 'rc-cursor':
          // 光标形状同步：仅在控制端光标可见时（绝对模式）应用 CSS cursor
          if (this._rcActive && this._rcVideoEl && this._viewerCursorVisible) {
            this._rcVideoEl.style.cursor = msg.cursor || 'default';
          }
          break;
        case 'perf-file-available':
          // Perf数据实时同步：推流端有新文件可下载
          if (typeof PerfSync !== 'undefined') PerfSync.onFileAvailable(msg);
          break;
        case 'pong':
          break;
      }
    };

    ws.onclose = () => {
      console.log('[Viewer] WebSocket 已断开');
      if (this._wsPingTimer) { clearInterval(this._wsPingTimer); this._wsPingTimer = null; }
      this._rcActive = false;
      this._viewerCursorVisible = false;
      if (this._rcVideoEl) this._rcVideoEl.style.cursor = '';
      this._stopNativeKeyhook();
      if (typeof RemoteControl !== 'undefined') RemoteControl.disable();
      if (this._onRemoteControlState) this._onRemoteControlState(false);
      this._socket = null;
      // 自动重连（最多 10 次）
      if (this._wsReconnectCount < 10 && this.channelInfo) {
        const delay = Math.min(1000 * Math.pow(1.5, this._wsReconnectCount), 10000);
        this._wsReconnectCount++;
        this._wsReconnectTimer = setTimeout(() => {
          if (this.channelInfo) this._connectSocket(channelId);
        }, delay);
      }
    };

    ws.onerror = (e) => {
      console.error('[Viewer] WebSocket 错误:', e);
    };
  },

  /** 断开 WebSocket */
  _disconnectSocket() {
    if (this._wsReconnectTimer) { clearTimeout(this._wsReconnectTimer); this._wsReconnectTimer = null; }
    if (this._wsPingTimer) { clearInterval(this._wsPingTimer); this._wsPingTimer = null; }
    this._wsReconnectCount = 0;
    if (this._socket) {
      this._socket.onclose = null; // 阻止自动重连
      this._socket.close();
      this._socket = null;
    }
    this._rcActive = false;
    this._stopNativeKeyhook();
  },

  /** 发送 JSON 消息到 WebSocket */
  _wsSend(data) {
    if (this._socket && this._socket.readyState === WebSocket.OPEN) {
      this._socket.send(JSON.stringify(data));
    }
  },

  /**
   * 请求启用远程控制（观看端 → 后端 → 被控端）
   * @param {HTMLElement} videoEl - 视频容器元素
   */
  startRemoteControl(videoEl) {
    if (this._rcActive) return;
    if (!this._socket || this._socket.readyState !== WebSocket.OPEN || !this.channelInfo) {
      console.error('[Viewer] WebSocket 未连接或未加入频道');
      if (this._onRemoteControlError) {
        this._onRemoteControlError(!this._socket ? 'socket_not_connected' : 'socket_not_connected');
      }
      return;
    }
    const channelId = this.channelInfo.channelId;
    this._rcVideoEl = videoEl;

    // 初始化 RemoteControl 模块，绑定发送函数
    if (typeof RemoteControl !== 'undefined') {
      RemoteControl.init(videoEl, (data) => {
        if (this._socket && this._socket.readyState === WebSocket.OPEN && this._rcActive) {
          this._socket.send(JSON.stringify({ type: 'rc-input', ...data }));
        }
      });
    }

    // 发起远控请求
    this._wsSend({ type: 'rc-start' });
  },

  /**
   * 停止远程控制
   */
  stopRemoteControl() {
    if (!this._rcActive) {
      if (typeof RemoteControl !== 'undefined') {
        RemoteControl.disable();
      }
      this._stopNativeKeyhook();
      return;
    }
    this._rcActive = false;
    this._viewerCursorVisible = false;
    if (this._rcVideoEl) this._rcVideoEl.style.cursor = '';
    this._stopNativeKeyhook();
    if (typeof RemoteControl !== 'undefined') {
      RemoteControl.disable();
    }
    if (this.channelInfo) {
      this._wsSend({ type: 'rc-stop' });
    }
    if (this._onRemoteControlState) this._onRemoteControlState(false);
  },

  /** 远控是否激活 */
  isRemoteControlActive() { return this._rcActive; },

  /** 获取 WebSocket 实例 */
  getSocket() { return this._socket; },

  // ─── 回调注册 ─────────────────────────────────────────────
  onEnd(cb)                { this._onEndCallback       = cb; },
  onNetworkQuality(cb)     { this._onNetworkQuality    = cb; },
  onStats(cb)              { this._onStats             = cb; },
  onRemoteControlState(cb) { this._onRemoteControlState = cb; },
  onRemoteControlError(cb) { this._onRemoteControlError = cb; },
};

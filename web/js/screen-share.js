/**
 * screen-share.js — 推流端（Device）：屏幕共享核心逻辑
 *
 * 优化策略：
 *  1. H.264 硬件编码  — 对应 MediaFoundation + D3D11 零拷贝链路
 *  2. 主动丢帧控制器  — 对应 haimaSenderAbandonCount
 *  3. 网络自适应降级  — 对应 haimaReceiverAckLossCount 触发码率调整
 *  4. 实时统计上报    — RTT / bitrate / 丢包率
 */

// ─────────────────────────────────────────────────────────────
// HaimaFrameController — 主动丢帧（需要 Chrome 94+）
// 低于目标帧率预算的帧会被 close() 丢弃，而不是堆积在编码队列
// ─────────────────────────────────────────────────────────────
class HaimaFrameController {
  constructor(sourceTrack, targetFps) {
    this.targetFps   = targetFps;
    this.intervalMs  = 1000 / targetFps;
    this.lastSendMs  = 0;
    this.abandonedFrames = 0;
    this.sentFrames      = 0;
    this._outputTrack    = null;
    this._reader         = null;
    this._writer         = null;
    this._running        = false;

    if (
      typeof MediaStreamTrackProcessor === 'undefined' ||
      typeof MediaStreamTrackGenerator === 'undefined'
    ) {
      console.warn('[HaimaFC] 浏览器不支持 Insertable Streams，跳过帧控制器');
      this._outputTrack = sourceTrack;
      return;
    }

    const processor      = new MediaStreamTrackProcessor({ track: sourceTrack });
    const generator      = new MediaStreamTrackGenerator({ kind: 'video' });
    this._outputTrack    = generator;
    this._reader         = processor.readable.getReader();
    this._writer         = generator.writable.getWriter();
    this._running        = true;
    this._pump();
  }

  async _pump() {
    while (this._running) {
      let done, frame;
      try {
        ({ done, value: frame } = await this._reader.read());
      } catch { break; }
      if (done) break;

      const now     = performance.now();
      const elapsed = now - this.lastSendMs;

      if (elapsed >= this.intervalMs) {
        try {
          await this._writer.write(frame);
          this.lastSendMs = now;
          this.sentFrames++;
        } catch { frame.close(); }
      } else {
        // 超预算：主动丢帧（对应 haimaSenderAbandonCount）
        frame.close();
        this.abandonedFrames++;
      }
    }
  }

  setTargetFps(fps) {
    this.targetFps  = fps;
    this.intervalMs = 1000 / fps;
  }

  getStats() {
    return {
      sentFrames:      this.sentFrames,
      abandonedFrames: this.abandonedFrames,
      abandonRate: this.sentFrames + this.abandonedFrames > 0
        ? (this.abandonedFrames / (this.sentFrames + this.abandonedFrames) * 100).toFixed(1)
        : '0.0'
    };
  }

  getOutputTrack() { return this._outputTrack; }

  stop() {
    this._running = false;
    try { this._reader?.cancel(); } catch {}
    try { this._writer?.close(); } catch {}
  }
}

// ─────────────────────────────────────────────────────────────
// NetworkAdaptiveController — 自适应码率降级
// 监听声网 network-quality 事件，弱网自动降档，恢复后升回
// ─────────────────────────────────────────────────────────────
class NetworkAdaptiveController {
  /**
   * @param {object} opts
   * @param {function} opts.onDowngrade  (level: string) => void
   * @param {function} opts.onUpgrade    (level: string) => void
   * @param {object}   opts.qualityMap   码率映射表
   * @param {object}   opts.resolutionMap 分辨率映射表
   */
  constructor(opts = {}) {
    this._onDowngrade    = opts.onDowngrade || (() => {});
    this._onUpgrade      = opts.onUpgrade   || (() => {});
    this._qualityMap     = opts.qualityMap;
    this._resolutionMap  = opts.resolutionMap;
    this._currentLevel   = 'high';   // ultra | high | standard | low
    this._targetLevel    = 'high';   // 原始用户设置
    this._goodSince      = null;
    this._RECOVER_MS     = 8000;     // 连续 8s 好网络才升档
    this._screenTrack    = null;
  }

  attach(client, screenTrack, targetLevel) {
    this._screenTrack  = screenTrack;
    this._currentLevel = targetLevel;
    this._targetLevel  = targetLevel;
    this._goodSince    = null;

    client.on('network-quality', (stats) => {
      const uq = stats.uplinkNetworkQuality; // 1好 → 6断
      this._onQualityUpdate(uq);
    });
  }

  _onQualityUpdate(uq) {
    const levels = ['ultra', 'high', 'standard', 'low'];
    const idx    = levels.indexOf(this._currentLevel);

    if (uq >= 4 && idx < levels.length - 1) {
      // 网络变差 → 立即降一档
      this._goodSince = null;
      const next      = levels[idx + 1];
      this._applyLevel(next);
      this._onDowngrade(next, uq);

    } else if (uq <= 2) {
      // 网络良好 → 计时，持续 _RECOVER_MS 后升档
      if (!this._goodSince) {
        this._goodSince = Date.now();
      } else if (
        Date.now() - this._goodSince >= this._RECOVER_MS &&
        this._currentLevel !== this._targetLevel
      ) {
        const targetIdx = levels.indexOf(this._targetLevel);
        const curIdx    = levels.indexOf(this._currentLevel);
        if (curIdx > 0 && curIdx > targetIdx) {
          const next = levels[curIdx - 1];
          this._applyLevel(next);
          this._onUpgrade(next, uq);
        }
        this._goodSince = null;
      }
    } else {
      this._goodSince = null;
    }
  }

  _applyLevel(level) {
    if (!this._screenTrack || this._currentLevel === level) return;
    this._currentLevel = level;
    const bitrate = this._qualityMap[level] || 4000;
    try {
      this._screenTrack.setEncoderConfiguration({ bitrateMax: bitrate });
      console.log(`[NetAdaptive] 码率档位切换 → ${level} (${bitrate} kbps)`);
    } catch (e) {
      console.warn('[NetAdaptive] setEncoderConfiguration 失败:', e.message);
    }
  }

  setTargetLevel(level) { this._targetLevel  = level; }
  getCurrentLevel()     { return this._currentLevel; }
}

// ─────────────────────────────────────────────────────────────
// ScreenShare
// ─────────────────────────────────────────────────────────────
const ScreenShare = {
  client: null,
  screenTrack: null,
  audioTrack: null,
  channelInfo: null,
  isSharing: false,

  // 内部组件
  _frameController:    null,
  _netController:      null,
  _statsTimer:         null,

  // ── 云游戏语音注入 ─────────────────────────────────────────
  // 独立语音覆盖频道（使用自有 Agora 凭据，与 Go-Live 频道分离）
  _voiceClient:        null,
  // key = remoteUser.uid, value = IRemoteAudioTrack
  _remoteAudioTracks:  new Map(),
  // 目标音频输出设备 ID（如 VB-Cable），null 表示系统默认
  _voiceOutputDeviceId: null,

  /** 后端地址（可配置） */
  API_BASE: AppConfig.resolveServerUrl(),

  /** 构造带鉴权的请求头（Bearer session token） */
  _apiHeaders() {
    const h = { 'Content-Type': 'application/json' };
    const token = AppConfig.getSessionToken();
    if (token) h['Authorization'] = `Bearer ${token}`;
    return h;
  },

  /** 共享配置 */
  config: {
    resolution:       '1080p',
    framerate:        30,
    quality:          'high',
    shareAudio:       true,
    // 'motion'  → 优先流畅（滚动/游戏/视频，推荐日常使用）
    // 'detail'  → 优先清晰（PPT/代码/文档静止画面）
    optimizationMode: 'motion'
  },

  /** 分辨率映射 */
  resolutionMap: {
    '2k':    { width: 2560, height: 1440 },
    '1080p': { width: 1920, height: 1080 },
    '720p':  { width: 1280, height: 720 }
  },

  /** 画质/码率映射 (kbps) */
  qualityMap: {
    'ultra':    15000,  // 2K@30fps / 1080p@60fps 旗舰画质，需要 ~2MB/s 上行
    'high':     8000,   // 1080p@30fps 流畅高清，需要 ~1MB/s 上行
    'standard': 4000,   // 720p~1080p 日常使用
    'low':      2000    // 弱网保底，仍比原来 1200 高
  },

  /** 更新配置 */
  updateConfig(config) {
    Object.assign(this.config, config);
    if (this._netController) {
      this._netController.setTargetLevel(this.config.quality);
    }
    if (this._frameController) {
      this._frameController.setTargetFps(this.config.framerate);
    }
    console.log('[ScreenShare] 配置已更新:', this.config);
  },

  /** 创建频道并获取凭据 */
  async createChannel() {
    const resp = await fetch(`${this.API_BASE}/api/channel/create`, {
      method:  'POST',
      headers: this._apiHeaders(),
      body:    JSON.stringify({ resolution: this.config.resolution || '1080p' })
    });
    const result = await resp.json();
    if (!result.success) throw new Error(result.error || '创建频道失败');
    this.channelInfo = result.data;
    return result.data;
  },

  /** 开始屏幕共享 */
  async startShare() {
    // Native 模式（client-native DLL）：委托给 C++ Agora Native SDK
    if (this._isNative) return this._startNativeShare();

    if (this.isSharing || this._isStarting) return;
    this._isStarting = true;

    let info;
    try {
    // 1. 获取凭据
    info = await this.createChannel();
    console.log('[ScreenShare] 频道已创建:', info.channelName);

    // 2. 创建 Agora 客户端 — H.264 触发硬件编码（对应 MediaFoundation 链路）
    // mode:'live' 直播模式：码率控制更宽松不会激进降码率，画质显著优于 'rtc'
    // 代价是延迟增加约 200ms（从 ~200ms → ~400ms），对屏幕共享场景可接受
    this.client = AgoraRTC.createClient({ mode: 'live', codec: 'h264' });
    // 设置为 host 角色（live 模式下推流端必须是 host）
    await this.client.setClientRole('host');

    // 3. 加入频道
    const joinAppId = info.appId;
    const joinUid   = info.uid != null ? info.uid : null;
    console.log('[ScreenShare] join参数:', { appId: joinAppId, channel: info.channelName, uid: joinUid });
    await this.client.join(joinAppId, info.channelName, info.token, joinUid);
    console.log('[ScreenShare] 已加入频道');

    // 4. 构建编码配置
    const res     = this.resolutionMap[this.config.resolution] || this.resolutionMap['1080p'];
    const bitrate = this.qualityMap[this.config.quality] || 8000;
    const fps     = this.config.framerate || 30;

    // 5. 创建屏幕共享轨道
    let rawVideoTrack;
    try {
      const tracks = await AgoraRTC.createScreenVideoTrack(
        {
          encoderConfig: {
            width:      { max: res.width },
            height:     { max: res.height },
            frameRate:  { ideal: fps, max: fps + 5 }, // max 略高于目标，给编码器一点余量
            bitrateMax: bitrate,
            bitrateMin: Math.floor(bitrate * 0.4)  // 最低保底提升到 40%，避免弱网时糊成马赛克
          },
          // 'motion'：优先运动流畅度（滚动/切窗不跳帧）
          // 'detail'：优先静态清晰度（适合展示文档/代码静止画面）
          // 建议：日常共享用 motion，演示 PPT/代码用 detail
          optimizationMode: this.config.optimizationMode || 'motion'
        },
        this.config.shareAudio ? 'auto' : 'disable'
      );

      if (Array.isArray(tracks)) {
        this.screenTrack = tracks[0];
        this.audioTrack  = tracks[1];
      } else {
        this.screenTrack = tracks;
      }
      rawVideoTrack = this.screenTrack.getMediaStreamTrack();

      // 告知 Chrome 编码器内容类型，优化编码策略：
      // 'motion' → 优先帧率稳定（游戏/滚动），同码率下运动画面更清晰
      // 'detail' → 优先锐度（文字/代码），静止画面更清晰
      try {
        rawVideoTrack.contentHint = 'motion';
        console.log('[ScreenShare] contentHint 已设为 motion');
      } catch (e2) {
        console.warn('[ScreenShare] contentHint 设置失败:', e2.message);
      }
    } catch (e) {
      if (e.code === 'PERMISSION_DENIED') {
        console.log('[ScreenShare] 用户取消了屏幕选择');
        throw new Error('用户取消了屏幕选择');
      }
      throw e;
    }

    // 6. 挂载主动丢帧控制器（HaimaFrameController）
    this._frameController = new HaimaFrameController(rawVideoTrack, fps);
    const outputTrack     = this._frameController.getOutputTrack();

    // 如果帧控制器产出了新 track，替换声网轨道底层的 MediaStreamTrack
    if (outputTrack !== rawVideoTrack) {
      try {
        await this.screenTrack.replaceTrack(outputTrack, false);
        console.log('[ScreenShare] 帧控制器已挂载，targetFps =', fps);
      } catch (e) {
        console.warn('[ScreenShare] replaceTrack 失败，跳过帧控制器:', e.message);
        this._frameController.stop();
        this._frameController = null;
      }
    }

    // 7. 发布轨道
    const publishTracks = [this.screenTrack];
    if (this.audioTrack) publishTracks.push(this.audioTrack);
    await this.client.publish(publishTracks);
    console.log('[ScreenShare] 已开始推流 (H.264, bitrateMax=', bitrate, 'kbps)');

    // 8. 挂载网络自适应降级控制器
    this._netController = new NetworkAdaptiveController({
      qualityMap:    this.qualityMap,
      resolutionMap: this.resolutionMap,
      onDowngrade:   (level, uq) => {
        console.warn(`[NetAdaptive] 网络变差(q=${uq})，降级至 ${level}`);
        Bridge.postMessage('quality-change', { direction: 'down', level, uplinkQuality: uq });
        if (this._onQualityChange) this._onQualityChange('down', level);
      },
      onUpgrade: (level, uq) => {
        console.log(`[NetAdaptive] 网络恢复(q=${uq})，升级至 ${level}`);
        Bridge.postMessage('quality-change', { direction: 'up', level, uplinkQuality: uq });
        if (this._onQualityChange) this._onQualityChange('up', level);
      }
    });
    this._netController.attach(this.client, this.screenTrack, this.config.quality);

    // 9. 监听屏幕共享结束
    this.screenTrack.on('track-ended', () => {
      console.log('[ScreenShare] 用户通过浏览器按钮停止了屏幕共享');
      this.stopShare();
      if (this._onStopCallback) this._onStopCallback();
    });

    // 10. 监听远端用户加入/离开
    this.client.on('user-joined', (user) => {
      console.log('[ScreenShare] 观看者加入:', user.uid);
      if (this._onViewerChange) this._onViewerChange(this.getViewerCount());
    });
    this.client.on('user-left', (user) => {
      console.log('[ScreenShare] 观看者离开:', user.uid);
      if (this._onViewerChange) this._onViewerChange(this.getViewerCount());
    });

    // 11a. 加入独立语音覆盖频道（用于接收 Viewer 麦克风）
    // Go-Live Agora token 是 audience 权限，viewer 无法 publish。
    // 使用自有 Agora App ID 创建独立语音频道，双方均为 PUBLISHER。
    this._joinVoiceOverlay(info.channelId).catch(e => {
      console.warn('[ScreenShare] 语音覆盖频道加入失败（非致命）:', e.message);
    });

    // 11b. 启动实时统计上报（500ms 轮询，对应 haima* 监控数据）
    this._startStatsReporter(info.channelId);

    // 11c. 建立 Socket.io 连接（被控端，用于接收远控事件）
    this._connectRcSocket(info.channelId);

    this.isSharing = true;
    Bridge.notifyShareStarted();
    return info;
    } catch (e) {
      // 统一清理：释放 Agora 客户端 + 后端频道资源
      if (this.client) {
        await this.client.leave().catch(() => {});
        this.client = null;
      }
      if (info?.channelId) {
        await this._destroyChannel(info.channelId).catch(() => {});
        this.channelInfo = null;
      }
      throw e;
    } finally {
      this._isStarting = false;
    }
  },

  /** 停止屏幕共享 */
  async stopShare() {
    // Native 模式
    if (this._isNative) return this._stopNativeShare();

    this._stopStatsReporter();

    // 通知后端销毁频道（flush 计费数据）
    const channelId = this.channelInfo?.channelId;
    if (channelId) {
      await this._destroyChannel(channelId);
    }

    if (this._frameController) {
      this._frameController.stop();
      this._frameController = null;
    }
    this._netController = null;

    // 停止远控 Socket
    this._disconnectRcSocket();

    // 停止语音覆盖频道
    await this._leaveVoiceOverlay();

    // 停止所有远端音频（语音注入）
    for (const [uid, track] of this._remoteAudioTracks) {
      try { track.stop(); } catch {}
    }
    this._remoteAudioTracks.clear();

    if (this.screenTrack) {
      this.screenTrack.close();
      this.screenTrack = null;
    }
    if (this.audioTrack) {
      this.audioTrack.close();
      this.audioTrack = null;
    }
    if (this.client) {
      await this.client.leave();
      this.client = null;
    }
    this.isSharing    = false;
    this.channelInfo  = null;
    Bridge.notifyShareStopped();
    console.log('[ScreenShare] 已停止共享');
  },

  /** 通知后端销毁频道 */
  async _destroyChannel(channelId) {
    try {
      const resp = await fetch(`${this.API_BASE}/api/channel/${channelId}/destroy`, {
        method: 'POST',
        headers: this._apiHeaders(),
      });
      const result = await resp.json();
      if (result.quota && this._onQuotaUpdate) {
        this._onQuotaUpdate(result.quota);
      }
    } catch (e) {
      console.warn('[ScreenShare] 销毁频道失败:', e.message);
    }
  },

  // ─── 独立语音覆盖频道（Viewer 麦克风 → Broadcaster） ──────

  /**
   * 加入独立语音覆盖频道。
   * 使用自有 Agora App ID + PUBLISHER token，与 Go-Live 频道完全独立。
   * Viewer 在此频道 publish 麦克风 → Broadcaster 在此频道 subscribe → VB-Cable
   */
  async _joinVoiceOverlay(channelId) {
    try {
      const resp = await fetch(`${this.API_BASE}/api/channel/${channelId}/voice-token`, {
        method: 'POST',
        headers: this._apiHeaders(),
      });
      const result = await resp.json();
      if (!result.success) {
        console.warn('[VoiceOverlay] 获取语音凭据失败:', result.error);
        return;
      }
      const { appId, channelName, token, uid } = result.data;

      // rtc 模式 + opus：延迟最低，双向通话
      this._voiceClient = AgoraRTC.createClient({ mode: 'rtc', codec: 'vp8' });

      // 订阅 Viewer 发布的麦克风音频
      this._voiceClient.on('user-published', async (user, mediaType) => {
        if (mediaType !== 'audio') return;
        try {
          await this._voiceClient.subscribe(user, 'audio');
          const track = user.audioTrack;
          this._remoteAudioTracks.set(user.uid, track);

          // 路由到指定输出设备（VB-Cable），若未配置则用系统默认
          if (this._voiceOutputDeviceId) {
            try {
              await track.setPlaybackDevice(this._voiceOutputDeviceId);
            } catch (e) {
              console.warn('[VoiceOverlay] setPlaybackDevice 失败, 使用系统默认:', e.message);
            }
          }
          track.play();
          console.log(`[VoiceOverlay] 已订阅 Viewer(${user.uid}) 麦克风，输出设备: ${this._voiceOutputDeviceId ?? '系统默认'}`);
          if (this._onVoiceInjectionChange) {
            this._onVoiceInjectionChange(true, user.uid);
          }
        } catch (e) {
          console.error('[VoiceOverlay] 订阅 Viewer 音频失败:', e.message);
        }
      });

      this._voiceClient.on('user-unpublished', (user, mediaType) => {
        if (mediaType !== 'audio') return;
        const track = this._remoteAudioTracks.get(user.uid);
        if (track) {
          track.stop();
          this._remoteAudioTracks.delete(user.uid);
          console.log(`[VoiceOverlay] Viewer(${user.uid}) 麦克风已取消`);
          if (this._onVoiceInjectionChange) {
            this._onVoiceInjectionChange(false, user.uid);
          }
        }
      });

      this._voiceClient.on('user-left', (user) => {
        const track = this._remoteAudioTracks.get(user.uid);
        if (track) {
          try { track.stop(); } catch {}
          this._remoteAudioTracks.delete(user.uid);
        }
      });

      await this._voiceClient.join(appId, channelName, token, uid);
      console.log('[VoiceOverlay] 已加入语音覆盖频道:', channelName);
    } catch (e) {
      console.error('[VoiceOverlay] 加入失败:', e.message);
      if (this._voiceClient) {
        await this._voiceClient.leave().catch(() => {});
        this._voiceClient = null;
      }
    }
  },

  /** 离开语音覆盖频道 */
  async _leaveVoiceOverlay() {
    if (!this._voiceClient) return;
    try {
      await this._voiceClient.leave();
    } catch {}
    this._voiceClient = null;
    console.log('[VoiceOverlay] 已离开语音覆盖频道');
  },

  // ─── 实时统计上报 ─────────────────────────────────────────
  _startStatsReporter(channelId) {
    this._stopStatsReporter();
    this._statsTimer = setInterval(async () => {
      if (!this.isSharing) return;

      let payload;
      if (this._isNative) {
        // Native 模式：从 C++ agora-stats 事件缓存取数据（_nativeLastStats 由事件更新）
        const s = this._nativeLastStats || {};
        payload = {
          rttMs:           0,
          sendBitrateKbps: s.bitrate    ?? 0,
          packetLossRate:  0,
          sendFrameRate:   s.frameRate  ?? 0,
          hwEncoder:       s.hwEncoder  ?? false,
          codec:           s.codec      ?? '--',
          width:           s.width      ?? 0,
          height:          s.height     ?? 0,
          frameController: null,
          qualityLevel:    this.config.quality
        };
      } else {
        if (!this.client) return;
        try {
          const rtc            = this.client.getRTCStats();
          const localVideoStats = this.client.getLocalVideoStats();
          payload = {
            rttMs:           rtc.RTT ?? 0,
            sendBitrateKbps: Math.round((localVideoStats?.sendBitrate ?? 0) / 1000),
            packetLossRate:  localVideoStats?.packetLossRate ?? 0,
            sendFrameRate:   localVideoStats?.sendFrameRate ?? 0,
            frameController: this._frameController?.getStats() ?? null,
            qualityLevel:    this._netController?.getCurrentLevel() ?? this.config.quality
          };
        } catch (e) {
          console.warn('[ScreenShare] 统计采集失败:', e.message);
          return;
        }
      }

      // 上报给后端（并检查配额结果）
      try {
        const statsResp = await fetch(`${this.API_BASE}/api/channel/${channelId}/stats`, {
          method:  'POST',
          headers: this._apiHeaders(),
          body:    JSON.stringify({
            role: 'publisher',
            ...payload
          })
        });
        const statsResult = await statsResp.json();
        if (statsResult.quota?.exceeded) {
          console.warn('[ScreenShare] 配额已用尽，自动停止共享');
          this.stopShare();
          if (this._onStopCallback) this._onStopCallback('quota_exceeded');
          return;
        }
        // 从服务端获取真实观众数（Agora audience 模式用户不在 remoteUsers 中）
        if (statsResult.viewerCount !== undefined && this._onViewerChange) {
          this._onViewerChange(statsResult.viewerCount);
        }
        // 附加观众端统计到 payload（供状态栏显示观看丢包等）
        if (statsResult.subscriberStats) {
          payload.subscriberStats = statsResult.subscriberStats;
          this._lastSubscriberStats = statsResult.subscriberStats;
        }
      } catch (e) {
        // 统计上报失败不影响主流程
      }

      // 分发给 UI 回调
      if (this._onStats) this._onStats(payload);
    }, 5000);
  },

  _stopStatsReporter() {
    if (this._statsTimer) {
      clearInterval(this._statsTimer);
      this._statsTimer = null;
    }
  },

  // ─── Native 模式（client-native DLL，Agora C++ SDK）─────────

  /** 是否处于 Native 模式（由 C++ WebView2 宿主注入） */
  get _isNative() {
    return !!(typeof window !== 'undefined' && window.isNativeMode);
  },

  /**
   * Native 模式下的 startShare 流程：
   *  1. 后端获取 Agora 凭据（复用 createChannel）
   *  2. C++ 初始化引擎 + 枚举屏幕/窗口源
   *  3. 展示源选择器（JS UI）
   *  4. C++ 加入频道 + 开始捕获
   */
  async _startNativeShare() {
    if (this.isSharing || this._isStarting) return;
    this._isStarting = true;
    let info;
    try {
      // 1. 从后端创建频道（获取 appId / token / channelName）
      info = await this.createChannel();
      console.log('[NativeShare] 频道已创建:', info.channelName);

      // 2. 初始化 C++ 引擎
      const initResult = await Bridge._request('agora-init', { appId: info.appId }, 'agora-init-result', 5000);
      if (!initResult?.ok) throw new Error('C++ Agora 引擎初始化失败');

      // 3. 获取屏幕/窗口源列表
      const sources = await Bridge._request('agora-get-sources', {}, 'agora-sources', 5000);
      const sourceList = Array.isArray(sources) ? sources : [];
      if (sourceList.length === 0) throw new Error('未检测到可分享的屏幕或窗口');

      // 4. 展示源选择器并等待用户选择
      const selectedSource = await this._showNativeSourcePicker(sourceList);
      if (!selectedSource) {
        await this._destroyChannel(info.channelId).catch(() => {});
        this._isStarting = false;
        return;
      }

      // 5. 注册 C++ 事件监听
      this._setupNativeListeners(info.channelId);

      // 6. C++ 加入频道
      Bridge.postMessage('agora-join', {
        token:   info.token,
        channel: info.channelName,
        uid:     info.uid ?? 0
      });

      // 7. C++ 开始捕获
      const res     = this.resolutionMap[this.config.resolution] || this.resolutionMap['1080p'];
      const bitrate = this.qualityMap[this.config.quality] || 8000;
      Bridge.postMessage('agora-start-capture', {
        sourceType: selectedSource.type,
        sourceId:   selectedSource.id,
        frameRate:  this.config.framerate || 30,
        width:      res.width,
        height:     res.height,
        bitrate:    bitrate,
        shareAudio: !!this.config.shareAudio
      });

      this.isSharing   = true;
      this.channelInfo = info;

      // 加入独立语音覆盖频道（用于接收 Viewer 麦克风 → VB-Cable）
      this._joinVoiceOverlay(info.channelId).catch(e => {
        console.warn('[NativeShare] 语音覆盖频道加入失败（非致命）:', e.message);
      });

      // 立即启动心跳上报（不等 agora-joined），防止后端 30s 心跳超时
      this._startStatsReporter(info.channelId);

      // 建立 Socket.io 连接，接收远程控制指令
      this._connectRcSocket(info.channelId);

      Bridge.notifyShareStarted();
      console.log('[NativeShare] 已启动 Native 屏幕分享');
      return info;

    } catch (e) {
      if (info?.channelId) {
        await this._destroyChannel(info.channelId).catch(() => {});
        this.channelInfo = null;
      }
      throw e;
    } finally {
      this._isStarting = false;
    }
  },

  /** 停止 Native 模式的分享 */
  async _stopNativeShare() {
    this._stopStatsReporter();
    this._removeNativeListeners();
    this._nativeLastStats = null;

    // 断开远控 Socket
    this._disconnectRcSocket();

    // 离开语音覆盖频道
    await this._leaveVoiceOverlay();

    const channelId = this.channelInfo?.channelId;
    if (channelId) await this._destroyChannel(channelId);

    Bridge.postMessage('agora-stop-capture', {});
    Bridge.postMessage('agora-leave', {});

    this.isSharing   = false;
    this.channelInfo = null;
    Bridge.notifyShareStopped();
    console.log('[NativeShare] 已停止 Native 屏幕分享');
  },

  /** 展示 Native 源选择器，返回用户选择的 source 或 null（取消） */
  _showNativeSourcePicker(sources) {
    return new Promise((resolve) => {
      const overlay = document.createElement('div');
      overlay.style.cssText = [
        'position:fixed','top:0','left:0','right:0','bottom:0',
        'background:rgba(0,0,0,0.7)','z-index:9999',
        'display:flex','align-items:center','justify-content:center'
      ].join(';');

      const box = document.createElement('div');
      box.style.cssText = [
        'background:#1e2030','border-radius:12px','padding:24px',
        'max-width:560px','width:90%','max-height:70vh','overflow-y:auto',
        'box-shadow:0 8px 32px rgba(0,0,0,0.5)'
      ].join(';');
      box.innerHTML = `
        <div style="color:#e2e8f0;font-size:16px;font-weight:600;margin-bottom:16px">选择要分享的屏幕或窗口</div>
        <div id="native-source-list" style="display:flex;flex-wrap:wrap;gap:8px"></div>
        <div style="margin-top:16px;text-align:right">
          <button id="native-source-cancel" style="padding:8px 16px;border-radius:6px;background:#374151;color:#e2e8f0;border:none;cursor:pointer;font-size:14px">取消</button>
        </div>
      `;
      overlay.appendChild(box);
      document.body.appendChild(overlay);

      const list = box.querySelector('#native-source-list');
      sources.forEach(src => {
        const btn = document.createElement('button');
        btn.style.cssText = [
          'padding:8px 14px','border-radius:6px','background:#2d3748',
          'color:#e2e8f0','border:1px solid #4a5568','cursor:pointer',
          'font-size:13px','text-align:left','max-width:100%',
          'overflow:hidden','text-overflow:ellipsis','white-space:nowrap'
        ].join(';');
        const icon = src.type === 'display' ? '🖥️' : '🪟';
        btn.textContent = `${icon} ${src.name || ('Source ' + src.id)}`;
        btn.title = src.name || '';
        btn.onclick = () => {
          document.body.removeChild(overlay);
          resolve(src);
        };
        list.appendChild(btn);
      });

      box.querySelector('#native-source-cancel').onclick = () => {
        document.body.removeChild(overlay);
        resolve(null);
      };
    });
  },

  /** 注册 C++ 回调事件监听 */
  _setupNativeListeners(channelId) {
    this._removeNativeListeners();
    this._nativeHandler = (msg) => {
      switch (msg.type) {
        case 'agora-joined':
          console.log('[NativeShare] 已加入频道 uid=', msg.data?.uid);
          this._startStatsReporter(channelId);
          break;
        case 'agora-left':
          console.log('[NativeShare] 已离开频道');
          break;
        case 'agora-stats':
          this._nativeLastStats = msg.data;
          if (this._onStats) {
            const s = msg.data || {};
            this._onStats({
              rttMs:           0,
              sendBitrateKbps: s.bitrate   ?? 0,
              packetLossRate:  0,
              sendFrameRate:   s.frameRate ?? 0,
              hwEncoder:       s.hwEncoder ?? false,
              codec:           s.codec     ?? '--',
              width:           s.width     ?? 0,
              height:          s.height    ?? 0,
              frameController: null,
              qualityLevel:    this.config.quality,
              subscriberStats: this._lastSubscriberStats || null
            });
          }
          break;
        case 'agora-preview-frame':
          if (this._onPreviewFrame && msg.data?.frame) {
            this._onPreviewFrame(msg.data.frame);
          }
          break;
        case 'agora-error':
          console.error('[NativeShare] 错误:', msg.data);
          break;
      }
    };
    Bridge.onMessage(this._nativeHandler);
  },

  _removeNativeListeners() {
    if (this._nativeHandler) {
      const idx = Bridge._handlers.indexOf(this._nativeHandler);
      if (idx >= 0) Bridge._handlers.splice(idx, 1);
      this._nativeHandler = null;
    }
  },

  // ─── 公共接口 ─────────────────────────────────────────────
  getPreviewTrack()  { return this.screenTrack; },
  getViewerCount()   { return this.client?.remoteUsers?.length ?? 0; },
  onStop(cb)         { this._onStopCallback = cb; },
  onViewerChange(cb) { this._onViewerChange = cb; },
  onQualityChange(cb){ this._onQualityChange = cb; },
  onStats(cb)        { this._onStats = cb; },
  onQuotaUpdate(cb)  { this._onQuotaUpdate = cb; },
  /** 注册 Native 模式预览帧回调 (base64 BMP 数据) */
  onPreviewFrame(cb) { this._onPreviewFrame = cb; },

  // ─── 云游戏语音注入接口 ───────────────────────────────────

  /**
   * 设置语音输出设备（如 VB-Cable）。
   * 调用后对当前已订阅的所有远端音频生效，后续订阅的也会自动使用。
   * @param {string|null} deviceId  null = 系统默认
   */
  async setVoiceOutputDevice(deviceId) {
    this._voiceOutputDeviceId = deviceId;
    for (const [uid, track] of this._remoteAudioTracks) {
      try {
        if (deviceId) {
          await track.setPlaybackDevice(deviceId);
        }
        console.log(`[ScreenShare] Viewer(${uid}) 音频输出 → ${deviceId ?? '系统默认'}`);
      } catch (e) {
        console.warn(`[ScreenShare] Viewer(${uid}) setPlaybackDevice 失败:`, e.message);
      }
    }
  },

  /**
   * 获取当前系统所有音频输出设备列表（含 VB-Cable）
   * @returns {Promise<MediaDeviceInfo[]>}
   */
  async getAudioOutputDevices() {
    return AgoraRTC.getPlaybackDevices();
  },

  /** 当语音注入状态变化时回调 (active: bool, uid: number) */
  onVoiceInjectionChange(cb) { this._onVoiceInjectionChange = cb; },

  /** 当前是否有 Viewer 在输出语音 */
  hasActiveVoice() { return this._remoteAudioTracks.size > 0; },

  // ─── 远程控制：被控端 WebSocket 集成 ─────────────────────

  /** @type {WebSocket|null} */
  _rcSocket: null,
  /** @type {boolean} 远控会话是否激活 */
  _rcActive: false,
  /** @type {string|null} 当前远控控制者 ID */
  _rcControllerId: null,
  /** @type {number|null} 自动重连定时器 */
  _rcReconnectTimer: null,
  /** @type {number} 重连次数 */
  _rcReconnectCount: 0,

  /**
   * 建立 WebSocket 连接（被控端/推流端调用）
   * @param {string} channelId
   */
  _connectRcSocket(channelId) {
    if (this._rcSocket) return;
    const serverUrl = AppConfig.resolveServerUrl().replace(/^http/, 'ws');
    const token = AppConfig.getSessionToken();
    const url = `${serverUrl}/ws?token=${encodeURIComponent(token)}&channel=${encodeURIComponent(channelId)}&role=broadcaster`;

    const ws = new WebSocket(url);
    this._rcSocket = ws;

    // 注册 Bridge 光标转发（仅一次，持续监听）
    if (Bridge.isWebView2 && !this._rcBridgeHandlerAdded) {
      this._rcBridgeHandlerAdded = true;
      Bridge.onMessage((msg) => {
        if (msg.type === 'rc-cursor' && this._rcActive && this._rcSocket?.readyState === WebSocket.OPEN) {
          const cursor = msg.data?.cursor || msg.cursor || 'default';
          this._rcSocket.send(JSON.stringify({ type: 'rc-cursor', cursor }));
        }
      });
    }

    ws.onopen = () => {
      console.log('[ScreenShare] WebSocket 已连接(被控端)');
      this._rcReconnectCount = 0;
    };

    ws.onmessage = (evt) => {
      let msg;
      try { msg = JSON.parse(evt.data); } catch { return; }
      if (!msg || !msg.type) return;

      switch (msg.type) {
        case 'rc-start':
          console.log('[ScreenShare] 收到远控启动请求:', msg);
          this._rcActive = true;
          this._rcControllerId = msg.controllerId || null;

          // 通知 C++ 启用输入注入 + 键鼠锁定
          if (Bridge.isWebView2) {
            Bridge.postMessage('remote-control-enable');
          }

          // 上报屏幕信息给控制端
          if (Bridge.isWebView2) {
            Bridge._request('remote-screen-info', {}, 'remote-screen-info', 5000)
              .then(info => {
                if (this._rcSocket && this._rcSocket.readyState === WebSocket.OPEN) {
                  this._rcSocket.send(JSON.stringify({ type: 'rc-screen-info', channelId, ...info }));
                }
              })
              .catch(e => console.warn('[ScreenShare] 获取屏幕信息失败:', e.message));
          }

          // 确认远控已就绪，通知服务端转发给观看端
          if (ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ type: 'rc-ready', channelId }));
          }

          if (this._onRemoteControlState) this._onRemoteControlState(true, msg);
          break;

        case 'rc-stop':
          console.log('[ScreenShare] 远控已停止:', msg);
          this._rcActive = false;
          this._rcControllerId = null;
          if (Bridge.isWebView2) {
            Bridge.postMessage('remote-control-disable');
          }
          if (this._onRemoteControlState) this._onRemoteControlState(false, msg);
          break;

        case 'rc-input':
          // 收到输入事件 → 转发到 C++ 注入
          if (!this._rcActive) return;
          if (Bridge.isWebView2) {
            // 优化：直接修改 type 字段避免解构+扩展创建新对象
            msg.type = 'remote-input';
            window.chrome.webview.postMessage(JSON.stringify(msg));
          }
          break;

        case 'pong':
          break;
      }
    };

    ws.onclose = () => {
      console.log('[ScreenShare] WebSocket 已断开(被控端)');
      this._rcSocket = null;
      // 自动重连
      if (this._rcReconnectCount < 10 && this.channelInfo) {
        const delay = Math.min(1000 * Math.pow(1.5, this._rcReconnectCount), 10000);
        this._rcReconnectCount++;
        this._rcReconnectTimer = setTimeout(() => {
          if (this.channelInfo) this._connectRcSocket(channelId);
        }, delay);
      }
    };

    ws.onerror = (e) => {
      console.error('[ScreenShare] WebSocket 错误:', e);
    };
  },

  /** 断开被控端 WebSocket */
  _disconnectRcSocket() {
    if (this._rcActive && Bridge.isWebView2) {
      Bridge.postMessage('remote-control-disable');
    }
    if (this._rcReconnectTimer) { clearTimeout(this._rcReconnectTimer); this._rcReconnectTimer = null; }
    this._rcReconnectCount = 0;
    if (this._rcSocket) {
      this._rcSocket.onclose = null; // 阻止自动重连
      this._rcSocket.close();
      this._rcSocket = null;
    }
    this._rcActive = false;
    this._rcControllerId = null;
  },

  /** 远控是否激活（被控端视角） */
  isRemoteControlled() { return this._rcActive; },

  /** 获取当前 WebSocket 引用 (供 PerfSync 等模块使用) */
  get _ws() { return this._rcSocket; },

  /** 注册远控状态变化回调 */
  onRemoteControlState(cb) { this._onRemoteControlState = cb; },
};

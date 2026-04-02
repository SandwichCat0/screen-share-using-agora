/**
 * app.js — MDShare 主控逻辑：导航、设备信息、UI交互
 */
(function () {
  'use strict';

  // ============== 工具函数 ==============
  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => document.querySelectorAll(sel);
  const $id = (id) => document.getElementById(id);

  // ============== Toast 提示 ==============
  function showToast(message, type = 'info', duration = 3000) {
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;
    toast.textContent = message;
    document.body.appendChild(toast);
    setTimeout(() => {
      toast.classList.add('toast-out');
      setTimeout(() => toast.remove(), 300);
    }, duration);
  }

  // ============== 导航系统 ==============
  const navItems = $$('.nav-item');
  const mobileNavBtns = $$('.mobile-nav-btn');
  const viewSections = $$('.view-section');

  function switchView(viewId) {
    // 隐藏所有视图
    viewSections.forEach(s => s.classList.remove('active'));
    navItems.forEach(n => n.classList.remove('active'));
    mobileNavBtns.forEach(b => b.classList.remove('active'));

    // 显示目标视图
    const target = $id(viewId);
    if (target) {
      target.classList.add('active');
    }

    // 高亮导航项
    navItems.forEach(n => {
      if (n.dataset.view === viewId) n.classList.add('active');
    });
    mobileNavBtns.forEach(b => {
      if (b.dataset.view === viewId) b.classList.add('active');
    });

    console.log('[Nav] 切换到:', viewId);
  }

  navItems.forEach(item => {
    item.addEventListener('click', () => switchView(item.dataset.view));
  });

  mobileNavBtns.forEach(btn => {
    btn.addEventListener('click', () => switchView(btn.dataset.view));
  });

  // ============== 设备信息管理 ==============
  let deviceInfo = null;

  function updateDeviceInfo(info) {
    deviceInfo = info;
    Bridge.deviceInfo = info;

    // 更新标题栏
    $id('deviceName').textContent = info.computerName || '--';
    $id('connectionTag').textContent = '在线';
    $id('connectionTag').className = 'text-[9px] px-1.5 py-0.5 rounded bg-success/10 text-success font-medium';

    // 更新总览卡片
    $id('overview-device-name').textContent = info.computerName || '--';
    $id('overview-os').textContent = info.os || '--';
    $id('overview-ip-address').textContent = info.ipAddress || '--';
    $id('overview-ip-location').textContent = info.ipLocation || '--';
    $id('cidDisplay').textContent = 'CID: ' + (info.cid || '--');

    // IP信息
    $id('ipLocation').textContent = info.ipLocation || '--';
    $id('deviceIP').textContent = info.ipAddress || '--';

    // 电脑信息总览
    $id('deviceUsername').textContent = (info.computerName || '--') + '\\' + (info.userName || '--');
    $id('devicePermissionGroup').textContent = info.permissionGroup || '--';
    $id('deviceCPU').textContent = info.cpu || '--';
    $id('deviceMemory').textContent = info.memory || '--';
    $id('deviceOS').textContent = info.os || '--';
    $id('deviceOSVersion').textContent = info.osVersion || '--';
    $id('deviceClientVersion').textContent = info.clientVersion || '1.0.0';

    // GPU
    if (info.gpu) {
      $id('deviceGPU').textContent = info.gpu;
    }

    // 存储
    if (info.storage) {
      $id('deviceStorage').textContent = info.storage;
    }

    // 网络适配器
    if (info.networkAdapters && info.networkAdapters.length > 0) {
      const container = $id('deviceNetworkAdapters');
      container.innerHTML = info.networkAdapters.map(adapter => `
        <div class="bg-base-200 rounded p-2 mb-1">
          <div class="text-base-content/80 text-[11px] mb-0.5">${escapeHtml(adapter.name || '--')}</div>
          <div class="flex justify-between">
            <span class="text-base-content/50 text-[10px]">${escapeHtml(adapter.type || '')}</span>
            <span class="text-base-content/60 text-[10px] font-mono">${escapeHtml(adapter.mac || '')}</span>
          </div>
        </div>
      `).join('');
    }

    console.log('[App] 设备信息已更新:', info);
  }

  // 初始化时请求设备信息
  function initDeviceInfo() {
    if (Bridge.isWebView2) {
      Bridge.requestDeviceInfo();
    } else {
      // 浏览器环境使用模拟数据
      const mockInfo = Bridge.getMockDeviceInfo();
      updateDeviceInfo(mockInfo);
      fetchPublicIP();
    }
  }

  // 获取公网IP
  async function fetchPublicIP() {
    try {
      const resp = await fetch('https://api.ipify.org?format=json');
      const data = await resp.json();
      if (data.ip) {
        $id('overview-ip-address').textContent = data.ip;
        $id('deviceIP').textContent = data.ip;
      }
    } catch (e) {
      console.log('[App] 获取IP失败:', e.message);
    }
  }

  // ============== 活动日志 ==============
  const activityLog = [];

  function addActivity(text, icon = 'fa-circle', iconColor = 'text-primary', iconBg = 'bg-primary/10') {
    const now = new Date();
    const timeStr = `${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')} ${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}`;

    activityLog.unshift({ text, icon, iconColor, iconBg, time: timeStr });
    if (activityLog.length > 100) activityLog.pop();

    renderActivityLog();
  }

  function renderActivityLog() {
    const container = $id('activityLog');
    if (activityLog.length === 0) {
      container.innerHTML = '<div class="text-base-content/50 text-center py-8">暂无活动记录</div>';
      return;
    }

    container.innerHTML = activityLog.map(item => `
      <div class="activity-item">
        <div class="icon ${escapeHtml(item.iconBg)}">
          <i class="fas ${escapeHtml(item.icon)} ${escapeHtml(item.iconColor)}"></i>
        </div>
        <div class="content">
          <div class="text">${escapeHtml(item.text)}</div>
          <div class="time">${escapeHtml(item.time)}</div>
        </div>
      </div>
    `).join('');
  }

  // ============== 屏幕共享控制 ==============
  const btnStartShare = $id('btnStartShare');
  const btnStopShare = $id('btnStopShare');
  const btnScreenConfig = $id('btnScreenConfig');
  const screenShareStatus = $id('screenShareStatus');
  const shareChannelId = $id('shareChannelId');
  const btnCopyChannel = $id('btnCopyChannel');
  const channelInfoBar = $id('channelInfoBar');
  const localPreview = $id('localPreview');
  const screenPlaceholder = $id('screenPlaceholder');
  const screenStats = $id('screenStats');
  const screenStatsChannel = $id('screenStatsChannel');
  const screenDuration = $id('screenDuration');
  const screenViewers = $id('screenViewers');

  let shareStatsTimer = null;

  btnStartShare.addEventListener('click', async () => {
    btnStartShare.disabled = true;
    btnStartShare.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i> 启动中...';
    try {
      // ── 前置检查：PerfSync 目录有效性 ──
      if (typeof PerfSync !== 'undefined') {
        const perfStatus = PerfSync.getStatus();
        if (perfStatus.enabled && !perfStatus.dirReady) {
          showToast('Perf 数据目录无效，请点击 🔍 扫描 perf_data 文件夹路径', 'error', 6000);
          btnStartShare.disabled = false;
          btnStartShare.innerHTML = '<i class="fas fa-play mr-1"></i> 开始共享';
          return;
        }
      }

      // ── 前置检查：VBCable 默认录制设备 ──
      if (Bridge.isWebView2) {
        try {
          const recStatus = await Bridge.checkDefaultRecording();
          if (!recStatus.installed) {
            // VB-Cable 未安装，显示警告但允许继续
            showToast('你没有安装虚拟声卡驱动，观看端说的话游戏内可能听不到哦', 'warning', 10000);
          } else if (!recStatus.isCableOutput) {
            // VB-Cable 已安装但默认录制设备不是 CABLE Output，自动切换
            const setResult = await Bridge.setDefaultRecordingToCable();
            if (setResult.success) {
              showToast('已自动将默认录制设备切换为 CABLE Output', 'success', 3000);
            } else {
              showToast('无法自动切换录制设备，请手动将默认录制设备设为 CABLE Output', 'warning', 6000);
            }
          }
        } catch (e) {
          console.warn('[App] 录制设备检测失败:', e.message);
        }
      }

      const info = await ScreenShare.startShare();

      // 启动 PerfSync 推流端模式
      if (typeof PerfSync !== 'undefined') {
        PerfSync.startBroadcaster((msg) => {
          // 通过 ScreenShare 的 WebSocket 发送
          if (ScreenShare._ws && ScreenShare._ws.readyState === 1) {
            ScreenShare._ws.send(JSON.stringify(msg));
          }
        });
      }

      // 显示频道号
      shareChannelId.textContent = info.channelId;
      channelInfoBar.classList.remove('hidden');
      channelInfoBar.classList.add('flex');
      if (screenStatsChannel) screenStatsChannel.textContent = info.channelId;
      Bridge.notifyChannelCreated(info.channelId);

      // 本地预览
      const track = ScreenShare.getPreviewTrack();
      if (track) {
        // Web 模式：有 AgoraRTC JS 轨道，直接渲染
        localPreview.innerHTML = '';
        localPreview.classList.remove('hidden');
        track.play(localPreview, { fit: 'contain' });
        screenPlaceholder.classList.add('hidden');
      } else if (ScreenShare._isNative) {
        // Native 模式：C++ 后台线程定期 GDI 截屏 → BMP → Base64 → <img> 渲染
        localPreview.innerHTML = '<img id="nativePreviewImg" style="width:100%;height:100%;object-fit:contain;image-rendering:auto" />';
        localPreview.classList.remove('hidden');
        screenPlaceholder.classList.add('hidden');
        ScreenShare.onPreviewFrame((b64) => {
          const img = document.getElementById('nativePreviewImg');
          if (img) img.src = 'data:image/jpeg;base64,' + b64;
        });
      }

      // 更新 UI
      btnStartShare.classList.add('hidden');
      btnStopShare.classList.remove('hidden');
      screenShareStatus.textContent = '共享中';
      screenShareStatus.className = 'text-[10px] px-2 py-0.5 rounded bg-success/10 text-success pulse-dot';

      // 显示统计
      screenStats.classList.remove('hidden');
      startShareStatsTimer();

      addActivity('开始屏幕共享 (频道: ' + info.channelId + ')', 'fa-desktop', 'text-success', 'bg-success/10');
      showToast('屏幕共享已开始', 'success');
    } catch (e) {
      console.error('[App] 启动共享失败:', e);
      Bridge.notifyError(e.message);
      showToast('启动共享失败: ' + e.message, 'error');
    } finally {
      btnStartShare.disabled = false;
      btnStartShare.innerHTML = '<i class="fas fa-play mr-1"></i> 开始共享';
    }
  });

  btnStopShare.addEventListener('click', async () => {
    // 停止 PerfSync
    if (typeof PerfSync !== 'undefined') PerfSync.stopBroadcaster();
    await ScreenShare.stopShare();
    resetShareUI();
    addActivity('停止屏幕共享', 'fa-stop-circle', 'text-error', 'bg-error/10');
    showToast('屏幕共享已停止', 'info');
  });

  ScreenShare.onStop((reason) => {
    if (typeof PerfSync !== 'undefined') PerfSync.stopBroadcaster();
    resetShareUI();
    if (reason === 'quota_exceeded') {
      addActivity('配额已用尽，屏幕共享自动停止', 'fa-exclamation-circle', 'text-error', 'bg-error/10');
      showToast('配额已用尽，共享已自动停止。请联系管理员充值。', 'error', 8000);
    } else {
      addActivity('屏幕共享被中断', 'fa-exclamation-triangle', 'text-warning', 'bg-warning/10');
    }
  });

  ScreenShare.onViewerChange((count) => {
    screenViewers.textContent = '观看: ' + count;
  });

  // 配额实时更新回调
  ScreenShare.onQuotaUpdate((quota) => {
    window._quotaInfo = quota;
    _updateHeaderQuota(quota);
  });

  // 实时推流统计回调
  ScreenShare.onStats((stats) => {
    const rttEl       = $id('statRtt');
    const bitrateEl   = $id('statBitrate');
    const lossEl      = $id('statLoss');
    const fpsEl       = $id('statFps');
    const abandonedEl = $id('statAbandoned');
    if (rttEl)       rttEl.textContent       = (stats.rttMs ?? '--') + 'ms';
    if (bitrateEl)   bitrateEl.textContent   = (stats.sendBitrateKbps ?? '--') + ' kbps';
    if (lossEl) {
      const loss = stats.packetLossRate ?? 0;
      lossEl.textContent = loss.toFixed(1) + '%';
      lossEl.className   = loss > 5 ? 'text-error mr-3' : loss > 1 ? 'text-warning mr-3' : 'text-success mr-3';
    }
    if (fpsEl)       fpsEl.textContent       = stats.sendFrameRate ?? '--';
    const codecEl = $id('statCodec');
    if (codecEl) {
      const hw = stats.hwEncoder ? 'HW' : 'SW';
      const codec = stats.codec ?? '--';
      const res = (stats.width && stats.height) ? ` ${stats.width}×${stats.height}` : '';
      codecEl.textContent = `${codec} ${hw}${res}`;
      codecEl.className = stats.hwEncoder ? 'text-success mr-3' : 'text-warning mr-3';
    }
    if (abandonedEl && stats.frameController) {
      abandonedEl.textContent = stats.frameController.abandonRate + '%';
    }
    // 观看端统计（丢包 + 延迟）
    const viewerLossWrap = $id('screenViewerLoss');
    if (viewerLossWrap && stats.subscriberStats) {
      const sub = stats.subscriberStats;
      const hasData = sub.packetLossRate !== undefined || sub.rttMs !== undefined || sub.e2eDelayMs !== undefined;
      if (hasData) {
        viewerLossWrap.classList.remove('hidden');
        const vLossEl  = $id('screenViewerLossVal');
        const vDelayEl = $id('screenViewerDelayVal');
        if (vLossEl) {
          const vl = sub.packetLossRate ?? 0;
          vLossEl.textContent = vl.toFixed(1) + '%';
          vLossEl.className = vl > 5 ? 'text-error' : vl > 1 ? 'text-warning' : 'text-success';
        }
        if (vDelayEl) {
          const delay = sub.e2eDelayMs ?? sub.rttMs ?? 0;
          vDelayEl.textContent = Math.round(delay) + 'ms';
          vDelayEl.className = delay > 200 ? 'text-error' : delay > 100 ? 'text-warning' : 'text-success';
        }
      }
    }
  });

  // 网络自适应质量变化回调
  ScreenShare.onQualityChange((direction, level) => {
    const labels = { ultra: '极高画质', high: '高画质', standard: '标准', low: '流畅优先' };
    if (direction === 'down') {
      showToast('网络不稳定，已自动降级到: ' + (labels[level] || level), 'info', 4000);
      addActivity('自动降级画质: ' + (labels[level] || level), 'fa-arrow-down', 'text-warning', 'bg-warning/10');
    } else {
      showToast('网络恢复，已自动升级到: ' + (labels[level] || level), 'success', 3000);
    }
  });

  // 云游戏语音注入状态回调
  ScreenShare.onVoiceInjectionChange((active, uid) => {
    const dot   = $id('voiceInjectStatus');
    const label = $id('voiceInjectLabel');
    if (active) {
      dot   && (dot.className   = 'w-2 h-2 rounded-full bg-success inline-block transition-colors');
      label && (label.textContent = `观看者 ${uid} 正在语音输入`);
    } else {
      // 还有其他远端音频活跃？
      const still = ScreenShare.hasActiveVoice();
      dot   && (dot.className   = still
        ? 'w-2 h-2 rounded-full bg-success inline-block transition-colors'
        : 'w-2 h-2 rounded-full bg-base-content/30 inline-block transition-colors');
      label && (label.textContent = still ? '有观看者正在语音输入' : '未有观看者开启麦克风');
    }
  });

  // ─── VB-Cable 一键安装 & 语音输出设备管理 ──────────────────

  const voiceOutputSelect      = $id('voiceOutputSelect');
  const btnRefreshVoiceDevices = $id('btnRefreshVoiceDevices');
  const btnInstallVBCable      = $id('btnInstallVBCable');
  const btnUninstallVBCable    = $id('btnUninstallVBCable');
  const vbcableChecking        = $id('vbcableChecking');
  const vbcableNotInstalled    = $id('vbcableNotInstalled');
  const vbcableInstalled       = $id('vbcableInstalled');
  const vbcableStatusRow       = $id('vbcableStatusRow');
  const vbcableInstallProgress = $id('vbcableInstallProgress');

  /** 切换 VB-Cable 区域显示状态 */
  function setVBCableUI(state) {
    // state: 'checking' | 'installed' | 'not-installed'
    vbcableChecking?.classList.toggle('hidden', state !== 'checking');
    vbcableNotInstalled?.classList.toggle('hidden', state !== 'not-installed');
    vbcableInstalled?.classList.toggle('hidden', state !== 'installed');
    vbcableStatusRow?.classList.toggle('hidden', state !== 'installed');

    // 切回"未安装"时重置安装按钮，防止上次安装留下的 disabled+spinner 状态残留
    if (state === 'not-installed' && btnInstallVBCable) {
      btnInstallVBCable.disabled = false;
      btnInstallVBCable.innerHTML = '<i class="fas fa-download"></i> 一键安装虚拟音频驱动';
      if (vbcableInstallProgress) {
        vbcableInstallProgress.textContent = '';
        vbcableInstallProgress.classList.add('hidden');
      }
    }
    // 切回"未安装"时也重置卸载按钮（防止卸载中状态残留）
    if (state === 'not-installed' && btnUninstallVBCable) {
      btnUninstallVBCable.disabled = false;
      btnUninstallVBCable.innerHTML = '<i class="fas fa-trash"></i> 卸载驱动';
    }
  }

  /** 枚举 Agora 输出设备并填充下拉框，自动选中 VB-Cable */
  async function loadVoiceOutputDevices() {
    if (!voiceOutputSelect) return;
    try {
      const devices    = await ScreenShare.getAudioOutputDevices();
      const currentVal = voiceOutputSelect.value;
      while (voiceOutputSelect.options.length > 1) voiceOutputSelect.remove(1);

      let cableDeviceId = null;
      devices.forEach(d => {
        const opt = document.createElement('option');
        opt.value = d.deviceId;
        opt.text  = d.label || ('输出设备 ' + d.deviceId.slice(0, 6) + '...');
        voiceOutputSelect.appendChild(opt);
        if (/cable\s*input|vb.?audio|virtual\s*cable/i.test(d.label)) {
          cableDeviceId = d.deviceId;
        }
      });

      if (currentVal && [...voiceOutputSelect.options].some(o => o.value === currentVal)) {
        voiceOutputSelect.value = currentVal;
      } else if (cableDeviceId) {
        voiceOutputSelect.value = cableDeviceId;
        await ScreenShare.setVoiceOutputDevice(cableDeviceId);
      }
    } catch (e) {
      console.warn('[VoiceOutput] 获取输出设备失败:', e.message);
    }
  }

  /** 检测 VB-Cable 并更新 UI */
  async function checkAndShowVBCable() {
    setVBCableUI('checking');
    try {
      const result = await Bridge.checkVBCable();
      if (result.installed && !result.hasError) {
        setVBCableUI('installed');
        await loadVoiceOutputDevices();
      } else if (result.installed && result.hasError) {
        // 设备存在但有错误（如代码10重复安装）：显示已安装区域（卸载按钮可用），并警告
        setVBCableUI('installed');
        showToast('检测到 VB-Cable 设备存在错误，建议卸载后重新安装', 'warning', 6000);
      } else {
        setVBCableUI('not-installed');
      }
    } catch (e) {
      // 超时或出错：保守显示"未安装"提示；浏览器调试环境不会走到这里
      console.warn('[VBCable] 检测失败或超时:', e.message);
      setVBCableUI('not-installed');
    }
  }

  voiceOutputSelect && voiceOutputSelect.addEventListener('change', async () => {
    const deviceId = voiceOutputSelect.value || null;
    await ScreenShare.setVoiceOutputDevice(deviceId);
  });

  btnRefreshVoiceDevices && btnRefreshVoiceDevices.addEventListener('click', async () => {
    btnRefreshVoiceDevices.disabled = true;
    await loadVoiceOutputDevices();
    btnRefreshVoiceDevices.disabled = false;
    showToast('设备列表已刷新', 'info', 2000);
  });

  /** 一键安装 VB-Cable 按钮 */
  btnInstallVBCable && btnInstallVBCable.addEventListener('click', async () => {
    btnInstallVBCable.disabled = true;
    btnInstallVBCable.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>正在安装...';
    vbcableInstallProgress?.classList.remove('hidden');

    // 监听进度消息（通过通用 onMessage 通道）
    const progressHandler = (msg) => {
      if (msg.type === 'vbcable-install-progress' && vbcableInstallProgress) {
        const map = { downloading: '正在下载驱动包...', installing: '正在安装，请通过UAC授权...' };
        vbcableInstallProgress.textContent = map[msg.data?.status] || '处理中...';
      }
    };
    Bridge.onMessage(progressHandler);

    try {
      const result = await Bridge.installVBCable();
      if (result.success) {
        showToast('虚拟音频驱动安装成功！', 'success', 4000);
        addActivity('虚拟音频驱动安装成功', 'fa-check-circle', 'text-success', 'bg-success/10');
        setVBCableUI('installed');
        await loadVoiceOutputDevices();
      } else {
        const msg = result.error || '安装失败，请手动安装虚拟音频驱动';
        showToast(msg, 'error', 5000);
        btnInstallVBCable.disabled = false;
        btnInstallVBCable.innerHTML = '<i class="fas fa-download mr-1"></i>一键安装虚拟音频驱动';
        vbcableInstallProgress && (vbcableInstallProgress.textContent = '安装失败: ' + msg);
      }
    } catch (e) {
      showToast('安装出错: ' + e.message, 'error', 5000);
      btnInstallVBCable.disabled = false;
      btnInstallVBCable.innerHTML = '<i class="fas fa-download mr-1"></i>一键安装虚拟音频驱动';
      vbcableInstallProgress && (vbcableInstallProgress.textContent = '');
    }
  });

  /** 卸载 VBCable 按钮 */
  btnUninstallVBCable && btnUninstallVBCable.addEventListener('click', async () => {
    if (!confirm('确认卸载远程语音注入驱动？\n\n卸载后语音注入功能将不可用。\n会弹出 UAC 提权窗口请允许。')) return;
    btnUninstallVBCable.disabled = true;
    btnUninstallVBCable.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>卸载中...';

    const progressHandler = (msg) => {
      if (msg.type === 'vbcable-uninstall-progress') {
        const map = { uninstalling: '正在卸载驱动，请在 UAC 弹窗中点击“是”...', verifying: '正在验证卸载结果...' };
        btnUninstallVBCable.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>' +
          (map[msg.data?.status] || '处理中...');
      }
    };
    Bridge.onMessage(progressHandler);

    try {
      const result = await Bridge.uninstallVBCable();
      if (result.success) {
        if (result.note) showToast(result.note, 'warning', 5000);
        else showToast('虚拟音频驱动已卸载', 'success', 4000);
        addActivity('虚拟音频驱动已卸载', 'fa-trash', 'text-error', 'bg-error/10');
        setVBCableUI('not-installed');
      } else {
        const msg = result.error || '卸载失败';
        showToast(msg, 'error', 5000);
        btnUninstallVBCable.disabled = false;
        btnUninstallVBCable.innerHTML = '<i class="fas fa-trash"></i> 卸载驱动';
      }
    } catch (e) {
      showToast('卸载出错: ' + e.message, 'error', 5000);
      btnUninstallVBCable.disabled = false;
      btnUninstallVBCable.innerHTML = '<i class="fas fa-trash"></i> 卸载驱动';
    }
  });

  btnCopyChannel.addEventListener('click', () => {
    const id = shareChannelId.textContent;
    if (id && id !== '--') {
      navigator.clipboard.writeText(id).then(() => {
        showToast('频道号已复制', 'success');
      });
    }
  });

  function resetShareUI() {
    btnStartShare.classList.remove('hidden');
    btnStopShare.classList.add('hidden');
    ScreenShare.onPreviewFrame(null); // 清除预览帧回调
    localPreview.innerHTML = '';
    localPreview.classList.add('hidden');
    screenPlaceholder.classList.remove('hidden');
    shareChannelId.textContent = '--';
    channelInfoBar.classList.add('hidden');
    channelInfoBar.classList.remove('flex');
    if (screenStatsChannel) screenStatsChannel.textContent = '--';
    screenShareStatus.textContent = '未共享';
    screenShareStatus.className = 'text-[10px] px-2 py-0.5 rounded bg-base-300 text-base-content/60';
    screenStats.classList.add('hidden');
    // 重置观看端统计
    const viewerLossWrap = $id('screenViewerLoss');
    if (viewerLossWrap) viewerLossWrap.classList.add('hidden');
    if (screenViewers) screenViewers.textContent = '观看: 0';
    stopShareStatsTimer();
  }

  function startShareStatsTimer() {
    stopShareStatsTimer();
    const startTime = Date.now();
    shareStatsTimer = setInterval(() => {
      const elapsed = Math.floor((Date.now() - startTime) / 1000);
      const min = String(Math.floor(elapsed / 60)).padStart(2, '0');
      const sec = String(elapsed % 60).padStart(2, '0');
      screenDuration.textContent = `${min}:${sec}`;

      // 持续刷新 Perf 同步指示（防止被遗漏）
      if (typeof PerfSync !== 'undefined') {
        const ps = PerfSync.getStatus();
        const perfStat  = $id('screenPerfSyncStat');
        const perfLabel = $id('screenPerfSyncLabel');
        if (perfStat && perfLabel && ps.mode === 'broadcaster' && ps.enabled) {
          perfStat.classList.remove('hidden');
          perfLabel.textContent = `Perf: ${ps.stats.uploaded}↑`;
          perfStat.title = `监控目录: ${ps.dirPath || '未知'}`;
        }
      }
    }, 1000);
  }

  function stopShareStatsTimer() {
    if (shareStatsTimer) {
      clearInterval(shareStatsTimer);
      shareStatsTimer = null;
    }
  }

  // ============== 屏幕共享配置弹窗 ==============
  const screenConfigModal = $id('screenConfigModal');
  const btnCloseScreenConfig = $id('btnCloseScreenConfig');
  const btnScreenConfigApply = $id('btnScreenConfigApply');
  const btnScreenConfigCancel = $id('btnScreenConfigCancel');
  const shareAudioBtn = $id('screenConfigShareAudioBtn');

  let screenConfig = { resolution: '1080p', framerate: 30, quality: 'high', shareAudio: true };

  btnScreenConfig.addEventListener('click', () => {
    screenConfigModal.classList.remove('hidden');
    // 同步当前配置到按钮状态
    syncConfigUI();
    // 每次打开时自动检测 VB-Cable 状态
    checkAndShowVBCable();
  });

  btnCloseScreenConfig.addEventListener('click', () => screenConfigModal.classList.add('hidden'));
  btnScreenConfigCancel.addEventListener('click', () => screenConfigModal.classList.add('hidden'));

  btnScreenConfigApply.addEventListener('click', () => {
    ScreenShare.updateConfig(screenConfig);
    screenConfigModal.classList.add('hidden');
    showToast('配置已保存', 'success');
  });

  // 配置按钮组点击
  ['resolutionOptions', 'framerateOptions', 'qualityOptions'].forEach(groupId => {
    const group = $id(groupId);
    if (!group) return;
    group.addEventListener('click', (e) => {
      const btn = e.target.closest('.config-btn');
      if (!btn) return;
      group.querySelectorAll('.config-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');

      const value = btn.dataset.value;
      if (groupId === 'resolutionOptions') screenConfig.resolution = value;
      else if (groupId === 'framerateOptions') screenConfig.framerate = parseInt(value);
      else if (groupId === 'qualityOptions') screenConfig.quality = value;
    });
  });

  // 音频共享切换
  shareAudioBtn.addEventListener('click', () => {
    screenConfig.shareAudio = !screenConfig.shareAudio;
    shareAudioBtn.classList.toggle('active', screenConfig.shareAudio);
  });

  function syncConfigUI() {
    // 分辨率
    $id('resolutionOptions').querySelectorAll('.config-btn').forEach(b => {
      b.classList.toggle('active', b.dataset.value === screenConfig.resolution);
    });
    // 帧率
    $id('framerateOptions').querySelectorAll('.config-btn').forEach(b => {
      b.classList.toggle('active', b.dataset.value === String(screenConfig.framerate));
    });
    // 画质
    $id('qualityOptions').querySelectorAll('.config-btn').forEach(b => {
      b.classList.toggle('active', b.dataset.value === screenConfig.quality);
    });
    // 音频
    shareAudioBtn.classList.toggle('active', screenConfig.shareAudio);
  }

  // ============== 屏享观看控制 ==============
  const inputChannelId   = $id('inputChannelId');
  const btnJoinChannel   = $id('btnJoinChannel');
  const btnLeaveChannel  = $id('btnLeaveChannel');
  const viewerStatus     = $id('viewerStatus');
  const remoteVideo      = $id('remoteVideo');
  const viewerPlaceholder  = $id('viewerPlaceholder');
  const btnViewerFullscreen = $id('btnViewerFullscreen');
  const btnToggleMic     = $id('btnToggleMic');
  const btnInputLock     = $id('btnInputLock');
  const inputLockOverlay = $id('inputLockOverlay');
  const viewerNetQuality = $id('viewerNetQuality');
  const kioskSection     = $id('kiosk');

  // 键鼠锁定状态
  let inputLockInstalled = false;
  let inputLockActive    = false;

  btnJoinChannel.addEventListener('click', async () => {
    const channelId = inputChannelId.value.trim();
    if (!channelId) {
      showToast('请输入频道号', 'error');
      return;
    }

    btnJoinChannel.disabled = true;
    btnJoinChannel.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i> 连接中...';
    try {
      await Viewer.joinChannel(channelId, remoteVideo);

      // 启动 PerfSync 观看端模式
      if (typeof PerfSync !== 'undefined') PerfSync.startViewer();

      btnJoinChannel.classList.add('hidden');
      btnLeaveChannel.classList.remove('hidden');
      btnViewerFullscreen.classList.remove('hidden');
      btnToggleMic.classList.remove('hidden');
      if (Bridge.isWebView2 && btnInputLock) btnInputLock.classList.remove('hidden');
      if (btnRemoteControl) btnRemoteControl.classList.remove('hidden');
      viewerNetQuality.classList.remove('hidden');

      // 自动安装键鼠锁定钩子（仅 Native 模式）
      if (Bridge.isWebView2 && !inputLockInstalled) {
        Bridge.installInputLock().then(r => {
          if (r.ok) {
            inputLockInstalled = true;
            console.log('[App] 键鼠锁定钩子已安装, 热键:', r.hotkey);
            // 如果用户有自定义的键鼠锁定热键，同步到 C++
            const savedLockHotkey = HotkeyConfig.get('inputLockToggle');
            if (savedLockHotkey && savedLockHotkey !== 'Ctrl+Alt+Shift+F12') {
              Bridge.setInputLockHotkey(savedLockHotkey.replace(/Meta/g, 'Win'))
                .catch(e => console.warn('[App] 同步键鼠锁定热键失败:', e.message));
            }
          }
        }).catch(e => console.warn('[App] 安装键鼠锁定钩子失败:', e.message));
      }
      inputChannelId.disabled = true;
      viewerStatus.textContent = '观看中';
      viewerStatus.className = 'text-[10px] px-2 py-0.5 rounded bg-success/10 text-success';

      addActivity('加入观看频道: ' + channelId, 'fa-eye', 'text-success', 'bg-success/10');
      showToast('已加入频道', 'success');

      // 自动开启麦克风
      try {
        const micOn = await Viewer.toggleMic();
        if (micOn) {
          btnToggleMic.innerHTML = '<i class="fas fa-microphone text-success"></i>';
          btnToggleMic.title = '关闭麦克风';
          btnToggleMic.classList.add('bg-success');
          btnToggleMic.classList.remove('bg-base-300');
        }
      } catch (e) {
        console.warn('[App] 自动开启麦克风失败:', e.message);
      }

      // 加入成功后自动弹出远控快捷键弹窗（3秒倒计时）
      showRcHotkeyModal(true);
    } catch (e) {
      console.error('[App] 加入频道失败:', e);
      showToast('加入频道失败: ' + e.message, 'error');
    } finally {
      btnJoinChannel.disabled = false;
      btnJoinChannel.innerHTML = '<i class="fas fa-sign-in-alt mr-1"></i> 加入';
    }
  });

  btnLeaveChannel.addEventListener('click', async () => {
    if (typeof PerfSync !== 'undefined') PerfSync.stopViewer();
    await Viewer.leaveChannel();
    resetViewerUI();
    addActivity('离开观看频道', 'fa-sign-out-alt', 'text-base-content/60', 'bg-base-200');
    showToast('已离开频道', 'info');
  });

  // 麦克风开关（观看端麦克风上行到推流端）
  btnToggleMic.addEventListener('click', async () => {
    btnToggleMic.disabled = true;
    try {
      const enabled = await Viewer.toggleMic();
      btnToggleMic.innerHTML = enabled
        ? '<i class="fas fa-microphone text-success"></i>'
        : '<i class="fas fa-microphone-slash"></i>';
      btnToggleMic.title = enabled ? '关闭麦克风' : '开启麦克风';
      btnToggleMic.classList.toggle('bg-success', enabled);
      btnToggleMic.classList.toggle('bg-base-300', !enabled);
      addActivity(enabled ? '开启麦克风' : '关闭麦克风',
        enabled ? 'fa-microphone' : 'fa-microphone-slash',
        enabled ? 'text-success' : 'text-base-content/60',
        enabled ? 'bg-success/10' : 'bg-base-200');
      showToast(enabled ? '麦克风已开启' : '麦克风已关闭', enabled ? 'success' : 'info');
    } catch (e) {
      showToast('麦克风操作失败: ' + e.message, 'error');
    } finally {
      btnToggleMic.disabled = false;
    }
  });

  // 下行网络质量回调
  // 质量等级: 1=极好 2=好 3=一般 4=差 5=极差 6=断开
  Viewer.onNetworkQuality((dq) => {
    if (!viewerNetQuality) return;
    const map = {
      1: { label: '极好', color: 'text-success',  bg: 'bg-success/10' },
      2: { label: '良好', color: 'text-success',  bg: 'bg-success/5' },
      3: { label: '一般', color: 'text-warning', bg: 'bg-warning/10' },
      4: { label: '较差', color: 'text-orange-500', bg: 'bg-orange-500/10' },
      5: { label: '极差', color: 'text-error',    bg: 'bg-error/10' },
      6: { label: '断开', color: 'text-error',    bg: 'bg-error/20' }
    };
    const info = map[dq] || map[3];
    viewerNetQuality.className = `text-[10px] px-2 py-1 rounded font-mono ${info.bg} ${info.color}`;
    viewerNetQuality.innerHTML = `&#9679;&nbsp;${info.label}`;
  });

  // 接收端实时统计回调
  Viewer.onStats((stats) => {
    // 展示在 viewerNetQuality 小标签旇尿，使用 title属性
    if (viewerNetQuality) {
      viewerNetQuality.title =
        `RTT: ${stats.rttMs ?? '--'}ms | 码率: ${stats.recvBitrateKbps ?? '--'}kbps` +
        ` | 丢包: ${(stats.packetLossRate ?? 0).toFixed(1)}% | FPS: ${stats.recvFrameRate ?? '--'}`;
    }
  });

  Viewer.onEnd(async () => {
    if (typeof PerfSync !== 'undefined') PerfSync.stopViewer();
    // 彻底清理：先离开频道（释放资源、断开 WS），再重置 UI
    try { await Viewer.leaveChannel(); } catch (e) { console.warn('[App] leaveChannel 失败:', e.message); }
    resetViewerUI();
    addActivity('共享者已离开', 'fa-user-slash', 'text-warning', 'bg-warning/10');
  });

  inputChannelId.addEventListener('keyup', (e) => {
    if (e.key === 'Enter') btnJoinChannel.click();
  });

  // ── 远程控制（观看端） ───────────────────────────────────
  const btnRemoteControl = $id('btnRemoteControl');
  const rcToolbar        = $id('rcToolbar');
  const btnRcStop        = $id('btnRcStop');
  const rcStatusBadge    = $id('rcStatusBadge');

  // ── 远控快捷键弹窗逻辑 ──
  const rcHotkeyModal      = $id('rcHotkeyModal');
  const rcHotkeyList       = $id('rcHotkeyList');
  const btnRcHotkeyStart   = $id('btnRcHotkeyStart');
  const btnRcHotkeyCancel  = $id('btnRcHotkeyCancel');
  const btnRcHotkeySettings = $id('btnRcHotkeySettings');
  const rcHotkeySettingsPanel = $id('rcHotkeySettingsPanel');
  const rcHotkeySettingsFields = $id('rcHotkeySettingsFields');
  const btnRcHotkeyResetAll = $id('btnRcHotkeyResetAll');

  /** 初始化/刷新快捷键列表显示 */
  function refreshHotkeyDisplay() {
    if (!rcHotkeyList) return;
    HotkeyConfig.load();
    const defs = HotkeyConfig.getDefs();
    rcHotkeyList.innerHTML = defs.map(def => {
      const current = HotkeyConfig.get(def.id);
      const display = HotkeyConfig.toDisplay(current);
      return `<div class="flex items-center justify-between py-2 px-3 bg-base-200 rounded-lg">
        <span class="text-sm text-base-content/80">${def.label}</span>
        <span class="text-xs font-mono text-primary bg-base-300/50 px-2 py-1 rounded">${display}</span>
      </div>`;
    }).join('');
  }

  /** 初始化/刷新快捷键设置面板 */
  function refreshHotkeySettings() {
    if (!rcHotkeySettingsFields) return;
    const defs = HotkeyConfig.getDefs();
    rcHotkeySettingsFields.innerHTML = defs.map(def => {
      const current = HotkeyConfig.get(def.id);
      const display = HotkeyConfig.toDisplay(current);
      return `<div class="flex items-center justify-between">
        <span class="text-xs text-base-content/60 w-32">${def.label}</span>
        <div class="flex items-center gap-2">
          <button class="hotkey-record-btn bg-base-200 border border-base-300 hover:border-primary rounded px-3 py-1.5 text-xs font-mono min-w-[140px] text-center text-base-content transition-colors cursor-pointer" data-hotkey-id="${def.id}">
            ${display}
          </button>
          <button class="hotkey-reset-btn text-xs text-base-content/40 hover:text-base-content/70 transition-colors px-1" data-hotkey-id="${def.id}" title="重置为默认">
            <i class="fas fa-undo"></i>
          </button>
        </div>
      </div>`;
    }).join('');

    // 绑定录制按钮
    rcHotkeySettingsFields.querySelectorAll('.hotkey-record-btn').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-hotkey-id');
        btn.textContent = '按下新快捷键...';
        btn.classList.add('!border-warning', '!text-warning');
        const combo = await HotkeyConfig.record();
        btn.classList.remove('!border-warning', '!text-warning');
        if (combo) {
          HotkeyConfig.set(id, combo);
          // 完整刷新设置面板和快捷键列表，确保显示一致
          refreshHotkeyDisplay();
          refreshHotkeySettings();
          // 如果是键鼠锁定热键，同步到 C++
          if (id === 'inputLockToggle' && Bridge.isWebView2) {
            Bridge.setInputLockHotkey(combo.replace(/Meta/g, 'Win'))
              .catch(e => console.warn('[App] 设置键鼠锁定热键失败:', e.message));
          }
          showToast('快捷键已更新', 'success', 1500);
        } else {
          // 取消录制时也刷新，确保显示正确
          refreshHotkeySettings();
        }
      });
    });

    // 绑定重置按钮
    rcHotkeySettingsFields.querySelectorAll('.hotkey-reset-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const id = btn.getAttribute('data-hotkey-id');
        HotkeyConfig.reset(id);
        refreshHotkeySettings();
        refreshHotkeyDisplay();
        if (id === 'inputLockToggle' && Bridge.isWebView2) {
          Bridge.setInputLockHotkey('Ctrl+Alt+Shift+F12').catch(() => {});
        }
        showToast('已重置为默认', 'info', 1500);
      });
    });
  }

  let _rcHotkeyCountdownTimer = null;

  /** 显示远控快捷键弹窗 (withCountdown=true 时开始远控按钮需等待3秒) */
  function showRcHotkeyModal(withCountdown = false) {
    refreshHotkeyDisplay();
    if (rcHotkeySettingsPanel) rcHotkeySettingsPanel.classList.add('hidden');
    if (rcHotkeyModal) rcHotkeyModal.classList.remove('hidden');

    // 清除上次的倒计时
    if (_rcHotkeyCountdownTimer) { clearInterval(_rcHotkeyCountdownTimer); _rcHotkeyCountdownTimer = null; }

    if (withCountdown && btnRcHotkeyStart) {
      let remaining = 3;
      btnRcHotkeyStart.disabled = true;
      btnRcHotkeyStart.innerHTML = `<i class="fas fa-hourglass-half mr-1"></i> ${remaining}s`;
      _rcHotkeyCountdownTimer = setInterval(() => {
        remaining--;
        if (remaining > 0) {
          btnRcHotkeyStart.innerHTML = `<i class="fas fa-hourglass-half mr-1"></i> ${remaining}s`;
        } else {
          clearInterval(_rcHotkeyCountdownTimer);
          _rcHotkeyCountdownTimer = null;
          btnRcHotkeyStart.disabled = false;
          btnRcHotkeyStart.innerHTML = '<i class="fas fa-play mr-1"></i> 开始远控';
        }
      }, 1000);
    } else if (btnRcHotkeyStart) {
      btnRcHotkeyStart.disabled = false;
      btnRcHotkeyStart.innerHTML = '<i class="fas fa-play mr-1"></i> 开始远控';
    }
  }

  /** 隐藏弹窗 */
  function hideRcHotkeyModal() {
    if (_rcHotkeyCountdownTimer) { clearInterval(_rcHotkeyCountdownTimer); _rcHotkeyCountdownTimer = null; }
    if (rcHotkeyModal) rcHotkeyModal.classList.add('hidden');
  }

  // 自定义快捷键展开/折叠
  if (btnRcHotkeySettings) {
    btnRcHotkeySettings.addEventListener('click', () => {
      const panel = rcHotkeySettingsPanel;
      if (!panel) return;
      const isHidden = panel.classList.contains('hidden');
      if (isHidden) {
        refreshHotkeySettings();
        panel.classList.remove('hidden');
        btnRcHotkeySettings.innerHTML = '<i class="fas fa-chevron-up mr-1"></i> 收起设置';
      } else {
        panel.classList.add('hidden');
        btnRcHotkeySettings.innerHTML = '<i class="fas fa-cog mr-1"></i> 自定义快捷键';
      }
    });
  }

  // 全部重置
  if (btnRcHotkeyResetAll) {
    btnRcHotkeyResetAll.addEventListener('click', () => {
      HotkeyConfig.resetAll();
      refreshHotkeySettings();
      refreshHotkeyDisplay();
      if (Bridge.isWebView2) {
        Bridge.setInputLockHotkey('Ctrl+Alt+Shift+F12').catch(() => {});
      }
      showToast('所有快捷键已重置', 'info');
    });
  }

  // 取消按钮
  if (btnRcHotkeyCancel) {
    btnRcHotkeyCancel.addEventListener('click', hideRcHotkeyModal);
  }

  // ESC 键关闭远控快捷键弹窗
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && rcHotkeyModal && !rcHotkeyModal.classList.contains('hidden')) {
      hideRcHotkeyModal();
    }
  });

  // 开始远控按钮（启动远控，不自动全屏）
  if (btnRcHotkeyStart) {
    btnRcHotkeyStart.addEventListener('click', () => {
      hideRcHotkeyModal();
      Viewer.startRemoteControl(remoteVideo);
    });
  }

  // 远控按钮：点击时如果未在远控，弹出快捷键列表；已在远控则停止
  if (btnRemoteControl) {
    btnRemoteControl.addEventListener('click', () => {
      if (Viewer.isRemoteControlActive()) {
        Viewer.stopRemoteControl();
      } else {
        showRcHotkeyModal();
      }
    });
  }

  // 远控特殊组合键
  if (rcToolbar) {
    rcToolbar.querySelectorAll('.rc-combo-btn[data-combo]').forEach(btn => {
      btn.addEventListener('click', (e) => {
        e.preventDefault();
        e.stopPropagation();
        const combo = btn.getAttribute('data-combo');
        if (combo && typeof RemoteControl !== 'undefined' && RemoteControl.isEnabled()) {
          RemoteControl.sendCombo(combo);
          showToast(`已发送 ${combo.toUpperCase()}`, 'info', 1500);
        }
      });
    });
  }

  // Pointer Lock 手动切换按钮
  const btnRcPointerLock = $id('btnRcPointerLock');

  function updatePointerLockBtn(locked) {
    if (!btnRcPointerLock) return;
    btnRcPointerLock.classList.toggle('!bg-success', locked);
    btnRcPointerLock.title = locked ? '鼠标锁定模式（点击切换到绝对模式）' : '绝对模式（点击切换到鼠标锁定模式）';
  }

  if (btnRcPointerLock) {
    btnRcPointerLock.addEventListener('click', (e) => {
      e.preventDefault();
      e.stopPropagation();
      if (typeof RemoteControl !== 'undefined' && RemoteControl.isEnabled()) {
        const locked = RemoteControl.togglePointerLock();
        updatePointerLockBtn(locked);
      }
    });
  }

  // Shift+Alt 快捷键切换 Pointer Lock 时更新按钮 UI + 同步被控端光标可见性
  if (typeof RemoteControl !== 'undefined') {
    RemoteControl.onPointerLockToggle((locked) => {
      updatePointerLockBtn(locked);
      // 同步控制端光标可见性：
      // locked=true (Pointer Lock/相对模式) → 隐藏控制端光标
      // locked=false (绝对模式) → 显示控制端光标（跟随远端光标形状）
      if (Viewer.isRemoteControlActive()) {
        Viewer._viewerCursorVisible = !locked;
        if (Bridge.isWebView2) {
          Bridge.postMessage('set-cursor-hidden', { hidden: locked });
        }
        // 更新视频容器 CSS cursor
        if (Viewer._rcVideoEl) {
          Viewer._rcVideoEl.style.cursor = locked ? 'none' : 'default';
        }
      }
    });
    // 开始/结束远控切换快捷键（默认 Shift+F9）
    RemoteControl.onToggleRemoteControl(() => {
      // 当前 RC 已启用时（keydown handler 已激活），hotkey 停止远控
      Viewer.stopRemoteControl();
    });
  }

  // 文档级快捷键：RC 未启用时，直接启动远控（不弹窗）
  document.addEventListener('keydown', (e) => {
    if (!Viewer.isViewing || Viewer.isRemoteControlActive()) return;
    // 跳过输入框
    const tag = e.target?.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || e.target?.isContentEditable) return;
    if (typeof HotkeyConfig !== 'undefined' && HotkeyConfig.matches('toggleRemoteControl', e)) {
      e.preventDefault();
      e.stopPropagation();
      Viewer.startRemoteControl(remoteVideo);
    }
  });

  // 远控停止按钮
  if (btnRcStop) {
    btnRcStop.addEventListener('click', () => {
      Viewer.stopRemoteControl();
    });
  }

  // 远控状态回调（观看端）
  Viewer.onRemoteControlState((active) => {
    updateRemoteControlUI(active);
  });

  Viewer.onRemoteControlError((error) => {
    const msgs = {
      remote_control_not_allowed: '当前卡密未开启远程控制权限',
      channel_not_found: '频道不存在或已关闭',
      socket_not_connected: '远控通信未连接，请重新加入频道',
    };
    showToast(msgs[error] || ('远控错误: ' + error), 'error');
  });

  function updateRemoteControlUI(active) {
    if (btnRemoteControl) {
      btnRemoteControl.innerHTML = active
        ? '<i class="fas fa-hand-pointer text-warning mr-1"></i> 远控中'
        : '<i class="fas fa-hand-pointer mr-1"></i> 远控';
      btnRemoteControl.classList.toggle('bg-warning', active);
      btnRemoteControl.classList.toggle('bg-base-300', !active);
      btnRemoteControl.title = active ? '点击停止远程控制' : '远程控制';
    }
    if (rcToolbar) {
      if (active) {
        rcToolbar.classList.remove('hidden');
      } else {
        rcToolbar.classList.add('hidden');
        // 重置 Pointer Lock 按钮状态
        updatePointerLockBtn(false);
      }
    }
  }

  // 被控端远控状态（推流端）
  ScreenShare.onRemoteControlState((active, data) => {
    if (rcStatusBadge) {
      if (active) {
        rcStatusBadge.classList.remove('hidden');
        addActivity('远程控制已启动', 'fa-hand-pointer', 'text-warning', 'bg-warning/10');
      } else {
        rcStatusBadge.classList.add('hidden');
        addActivity('远程控制已停止', 'fa-hand-pointer', 'text-base-content/60', 'bg-base-200');
      }
    }
  });

  // 全屏按钮
  let isViewerFullscreen = false;

  function enterViewerFullscreen() {
    // 立即隐藏UI元素，只显示视频
    document.body.classList.add('viewer-fullscreen');
    isViewerFullscreen = true;
    // 同时请求窗口/浏览器全屏
    if (Bridge.isWebView2) {
      Bridge.enterFullscreen();
    } else {
      document.documentElement.requestFullscreen().catch(() => {});
    }
  }

  function exitViewerFullscreen() {
    document.body.classList.remove('viewer-fullscreen');
    isViewerFullscreen = false;
    if (Bridge.isWebView2) {
      Bridge.exitFullscreen();
    } else if (document.fullscreenElement) {
      document.exitFullscreen();
    }
  }

  btnViewerFullscreen.addEventListener('click', () => {
    if (!isViewerFullscreen) {
      enterViewerFullscreen();
    } else {
      exitViewerFullscreen();
    }
  });

  // 浏览器原生全屏变化
  document.addEventListener('fullscreenchange', () => {
    if (!document.fullscreenElement && isViewerFullscreen) {
      document.body.classList.remove('viewer-fullscreen');
      isViewerFullscreen = false;
    }
  });

  // 注册退出全屏回调（自定义快捷键触发，默认 Shift+8）
  if (typeof RemoteControl !== 'undefined') {
    RemoteControl.onExitFullscreen(() => {
      if (isViewerFullscreen) exitViewerFullscreen();
    });
  }

  // 退出全屏快捷键（非远控时：自定义快捷键 或 ESC；远控时由 remote-control.js 处理）
  document.addEventListener('keydown', (e) => {
    if (!isViewerFullscreen || Viewer.isRemoteControlActive()) return;
    if (e.key === 'Escape' || (typeof HotkeyConfig !== 'undefined' && HotkeyConfig.matches('exitFullscreen', e))) {
      e.preventDefault();
      exitViewerFullscreen();
    }
  });

  // ─── 键鼠锁定 ───────────────────────────────────────────

  // 监听 C++ 推送的锁定状态变化（热键触发时从 C++ 侧发来）
  Bridge.onMessage((msg) => {
    if (msg.type === 'input-lock-state') {
      inputLockActive = msg.data.locked;
      updateInputLockUI(inputLockActive);
    }
  });

  function updateInputLockUI(locked) {
    if (btnInputLock) {
      btnInputLock.innerHTML = locked
        ? '<i class="fas fa-lock text-error"></i>'
        : '<i class="fas fa-lock-open"></i>';
      btnInputLock.title = locked ? '键鼠已锁定 (按热键解锁)' : '锁定键鼠';
      btnInputLock.classList.toggle('bg-error', locked);
      btnInputLock.classList.toggle('bg-base-300', !locked);
    }
    // 全屏模式下显/隐锁定遮罩（已禁用）
    // if (inputLockOverlay) { ... }
  }

  // 手动点击按钮切换锁定
  if (btnInputLock) {
    btnInputLock.addEventListener('click', () => {
      Bridge.toggleInputLock();
    });
  }

  function resetViewerUI() {
    if (document.body.classList.contains('viewer-fullscreen')) {
      exitViewerFullscreen();
    }
    // 停止远控
    updateRemoteControlUI(false);
    // 卸载键鼠锁定钩子
    if (inputLockInstalled) {
      Bridge.uninstallInputLock().catch(() => {});
      inputLockInstalled = false;
      inputLockActive = false;
      updateInputLockUI(false);
    }
    btnJoinChannel.classList.remove('hidden');
    btnLeaveChannel.classList.add('hidden');
    btnViewerFullscreen.classList.add('hidden');
    btnToggleMic.classList.add('hidden');
    btnToggleMic.innerHTML = '<i class="fas fa-microphone-slash"></i>';
    btnToggleMic.title = '开启麦克风';
    btnToggleMic.classList.remove('bg-success');
    btnToggleMic.classList.add('bg-base-300');
    if (btnInputLock) {
      btnInputLock.classList.add('hidden');
      btnInputLock.innerHTML = '<i class="fas fa-lock-open"></i>';
      btnInputLock.classList.remove('bg-error');
      btnInputLock.classList.add('bg-base-300');
    }
    if (btnRemoteControl) {
      btnRemoteControl.classList.add('hidden');
      btnRemoteControl.innerHTML = '<i class="fas fa-hand-pointer mr-1"></i> 远控';
      btnRemoteControl.classList.remove('bg-warning');
      btnRemoteControl.classList.add('bg-base-300');
    }
    viewerNetQuality.classList.add('hidden');
    inputChannelId.disabled = false;
    inputChannelId.value = '';
    remoteVideo.innerHTML = '';
    remoteVideo.classList.add('hidden');
    viewerPlaceholder.classList.remove('hidden');
    viewerPlaceholder.querySelector('p').textContent = '输入频道号加入观看';
    viewerStatus.textContent = '未连接';
    viewerStatus.className = 'text-[10px] px-2 py-0.5 rounded bg-base-300 text-base-content/60';
    // 确保输入框可聚焦，防止远控退出后键盘事件无响应
    setTimeout(() => { inputChannelId.focus(); }, 50);
  }

  // ============== 顶部按钮 ==============
  // 分享链接
  $id('btnShareLink').addEventListener('click', () => {
    const channelId = shareChannelId.textContent;
    if (channelId && channelId !== '--') {
      const url = `${ScreenShare.API_BASE}/watch?channel=${channelId}`;
      navigator.clipboard.writeText(url).then(() => {
        showToast('分享链接已复制', 'success');
      });
    } else {
      showToast('请先开始屏幕共享', 'info');
    }
  });

  // 打开网页版
  $id('btnOpenBrowser').addEventListener('click', () => {
    window.open(ScreenShare.API_BASE, '_blank');
  });

  // 启动模式切换
  let startupMode = 'auto';
  $id('startupModeBtn').addEventListener('click', () => {
    startupMode = startupMode === 'auto' ? 'manual' : 'auto';
    $id('startupModeText').textContent = startupMode === 'auto' ? '自动' : '手动';
    $id('startupModeIcon').className = startupMode === 'auto' ? 'fas fa-play-circle' : 'fas fa-pause-circle';
    $id('startupModeBtn').title = startupMode === 'auto' ? '开机自启动（点击切换为手动）' : '手动启动（点击切换为自动）';
    Bridge.postMessage('set-startup-mode', { mode: startupMode });
    showToast(`已切换为${startupMode === 'auto' ? '自动' : '手动'}启动`, 'info');
  });

  // 关闭客户端
  $id('btnClose').addEventListener('click', () => {
    showModal('确认关闭', '确定要关闭 MDShare 客户端吗？', async () => {
      await AppConfig.logout();   // 服务端吊销 session
      Bridge.notifyClose();
      if (!Bridge.isWebView2) {
        showToast('浏览器环境无法关闭', 'info');
      }
    });
  });

  // 最小化到后台
  const btnHideToBackground = $id('btnHideToBackground');
  if (btnHideToBackground) {
    btnHideToBackground.addEventListener('click', () => {
      if (Bridge.isWebView2) {
        Bridge.hideWindow();
        showToast('已最小化到后台，按 Ctrl+Shift+M 恢复', 'info', 3000);
      } else {
        showToast('浏览器环境不支持此功能', 'info');
      }
    });
  }

  // ============== 通用弹窗 ==============
  function showModal(title, message, onConfirm, onCancel) {
    $id('modalTitle').textContent = title;
    $id('modalMessage').textContent = message;
    $id('customModal').classList.remove('hidden');

    const confirmHandler = () => {
      $id('customModal').classList.add('hidden');
      $id('modalConfirmBtn').removeEventListener('click', confirmHandler);
      $id('modalCancelBtn').removeEventListener('click', cancelHandler);
      if (onConfirm) onConfirm();
    };
    const cancelHandler = () => {
      $id('customModal').classList.add('hidden');
      $id('modalConfirmBtn').removeEventListener('click', confirmHandler);
      $id('modalCancelBtn').removeEventListener('click', cancelHandler);
      if (onCancel) onCancel();
    };

    $id('modalConfirmBtn').addEventListener('click', confirmHandler);
    $id('modalCancelBtn').addEventListener('click', cancelHandler);
  }

  // ============== 复制配置 ==============
  $id('btnCopyConfig').addEventListener('click', () => {
    if (!deviceInfo) {
      showToast('设备信息尚未加载', 'error');
      return;
    }
    const text = Object.entries(deviceInfo)
      .filter(([k]) => !['networkAdapters'].includes(k))
      .map(([k, v]) => `${k}: ${v}`)
      .join('\n');
    navigator.clipboard.writeText(text).then(() => {
      showToast('配置已复制到剪贴板', 'success');
    });
  });

  // ============== 终端 ==============
  const terminalInput = $id('terminalInput');
  const terminalOutput = $id('terminalOutput');

  terminalInput.addEventListener('keyup', (e) => {
    if (e.key === 'Enter') {
      const cmd = terminalInput.value.trim();
      if (!cmd) return;
      terminalOutput.textContent += `\n$ ${cmd}\n`;
      Bridge.executeCommand(cmd);
      terminalInput.value = '';
      terminalOutput.scrollTop = terminalOutput.scrollHeight;
      addActivity('执行命令: ' + cmd, 'fa-terminal', 'text-success', 'bg-success/10');
    }
  });

  $id('btnClearTerminal').addEventListener('click', () => {
    terminalOutput.textContent = 'C:\\Users\\User>\n';
  });

  // ============== 进程管理 ==============
  $id('btnRefreshProcess').addEventListener('click', () => {
    Bridge.requestProcessList();
    showToast('正在获取进程列表...', 'info');
  });

  function renderProcessList(processes) {
    const tbody = $id('processTableBody');
    if (!processes || processes.length === 0) {
      tbody.innerHTML = '<tr><td colspan="5" class="text-center text-base-content/50 py-8">无进程数据</td></tr>';
      return;
    }

    tbody.innerHTML = processes.map(p => {
      const safePid = Number.isInteger(p.pid) ? p.pid : 0;
      return `
      <tr>
        <td class="font-medium">${escapeHtml(p.name)}</td>
        <td class="text-base-content/50 font-mono">${safePid}</td>
        <td>${escapeHtml(p.memory || '--')}</td>
        <td>${escapeHtml(String(p.cpu || '--'))}</td>
        <td class="text-right">
          <button class="text-error hover:text-error/80 text-[10px] px-2 py-1 rounded hover:bg-error/10 transition-colors" onclick="Bridge.requestKillProcess(${safePid})">
            结束
          </button>
        </td>
      </tr>
    `;
    }).join('');
  }

  // 进程搜索
  $id('processSearch').addEventListener('input', (e) => {
    const keyword = e.target.value.toLowerCase();
    const rows = $id('processTableBody').querySelectorAll('tr');
    rows.forEach(row => {
      const name = row.querySelector('td')?.textContent?.toLowerCase() || '';
      row.style.display = name.includes(keyword) ? '' : 'none';
    });
  });

  // ============== 文件互传 ==============
  const fileDropZone = $id('fileDropZone');
  const btnUploadFile = $id('btnUploadFile');

  fileDropZone.addEventListener('dragover', (e) => {
    e.preventDefault();
    fileDropZone.classList.add('drag-over');
  });

  fileDropZone.addEventListener('dragleave', () => {
    fileDropZone.classList.remove('drag-over');
  });

  fileDropZone.addEventListener('drop', (e) => {
    e.preventDefault();
    fileDropZone.classList.remove('drag-over');
    const files = e.dataTransfer.files;
    handleFiles(files);
  });

  fileDropZone.addEventListener('click', () => {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.onchange = () => handleFiles(input.files);
    input.click();
  });

  btnUploadFile.addEventListener('click', () => fileDropZone.click());

  function handleFiles(files) {
    const list = $id('fileTransferList');
    for (const file of files) {
      const item = document.createElement('div');
      item.className = 'bg-base-200 border border-base-300 rounded-lg p-3 flex items-center justify-between';
      item.innerHTML = `
        <div class="flex items-center gap-3">
          <i class="fas fa-file text-primary"></i>
          <div>
            <div class="text-sm text-base-content">${escapeHtml(file.name)}</div>
            <div class="text-[10px] text-base-content/50">${formatFileSize(file.size)}</div>
          </div>
        </div>
        <div class="text-xs text-base-content/60">
          <i class="fas fa-check text-success"></i> 已选择
        </div>
      `;
      list.appendChild(item);
      addActivity('选择文件: ' + file.name, 'fa-file', 'text-primary', 'bg-primary/10');
    }
  }

  function formatFileSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
  }

  // ============== 消息/聊天 ==============
  const chatInput = $id('chatInput');
  const chatMessages = $id('chatMessages');

  $id('btnSendChat').addEventListener('click', sendChat);
  chatInput.addEventListener('keyup', (e) => {
    if (e.key === 'Enter') sendChat();
  });

  function sendChat() {
    const text = chatInput.value.trim();
    if (!text) return;
    const now = new Date();
    const timeStr = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;

    // 清除占位
    if (chatMessages.querySelector('.text-base-content\\/50')) {
      chatMessages.innerHTML = '';
    }

    const msg = document.createElement('div');
    msg.className = 'flex justify-end';
    msg.innerHTML = `
      <div class="max-w-[70%]">
        <div class="bg-primary text-primary-content text-sm rounded-xl rounded-tr-sm px-4 py-2">${escapeHtml(text)}</div>
        <div class="text-[10px] text-base-content/50 text-right mt-1">${escapeHtml(timeStr)}</div>
      </div>
    `;
    chatMessages.appendChild(msg);
    chatMessages.scrollTop = chatMessages.scrollHeight;
    chatInput.value = '';

    Bridge.postMessage('chat-message', { text });
  }

  // ============== 运行时长 ==============
  let uptimeTimer = null;
  function startUptimeTimer() {
    const start = Date.now();
    uptimeTimer = setInterval(() => {
      const elapsed = Math.floor((Date.now() - start) / 1000);
      const hours = Math.floor(elapsed / 3600);
      const mins = Math.floor((elapsed % 3600) / 60);
      const secs = elapsed % 60;
      $id('overview-uptime').textContent = `${hours}h ${mins}m ${secs}s`;
    }, 1000);
  }

  // ============== 宿主消息处理 ==============
  Bridge.onMessage((msg) => {
    console.log('[Bridge ←]', msg);

    switch (msg.type) {
      case 'device-info':
        updateDeviceInfo(msg.data || msg);
        break;

      case 'process-list':
        renderProcessList(msg.data || msg.processes || []);
        break;

      case 'command-output':
        terminalOutput.textContent += (msg.output || msg.data || '') + '\n';
        terminalOutput.scrollTop = terminalOutput.scrollHeight;
        break;

      case 'start-share':
        switchView('screen');
        btnStartShare.click();
        break;

      case 'join-channel':
        if (msg.channelId) {
          switchView('kiosk');
          inputChannelId.value = msg.channelId;
          btnJoinChannel.click();
        }
        break;

      case 'chat-message':
        receiveChatMessage(msg);
        break;

      case 'activity':
        addActivity(msg.text || msg.data, msg.icon, msg.iconColor, msg.iconBg);
        break;

      case 'clipboard':
        addClipboardLog(msg);
        break;

      case 'keyboard':
        addKeyboardLog(msg);
        break;

      default:
        console.log('[App] 未处理的消息类型:', msg.type);
    }
  });

  function receiveChatMessage(msg) {
    if (chatMessages.querySelector('.text-base-content\\/50')) {
      chatMessages.innerHTML = '';
    }
    const el = document.createElement('div');
    el.className = 'flex justify-start';
    el.innerHTML = `
      <div class="max-w-[70%]">
        <div class="bg-base-300 text-base-content text-sm rounded-xl rounded-tl-sm px-4 py-2">${escapeHtml(msg.text || '')}</div>
        <div class="text-[10px] text-base-content/50 mt-1">${escapeHtml(msg.time || '')}</div>
      </div>
    `;
    chatMessages.appendChild(el);
    chatMessages.scrollTop = chatMessages.scrollHeight;
  }

  // ============== 剪贴板/键盘日志 ==============
  const clipboardLogs = [];

  function addClipboardLog(msg) {
    clipboardLogs.unshift({
      type: 'clipboard',
      app: msg.app || 'Unknown',
      content: msg.content || msg.text || '',
      time: msg.time || new Date().toLocaleTimeString()
    });
    if (clipboardLogs.length > 200) clipboardLogs.pop();
    renderClipboardLog();
  }

  function addKeyboardLog(msg) {
    clipboardLogs.unshift({
      type: 'keyboard',
      app: msg.app || 'Unknown',
      content: msg.key || msg.content || '',
      time: msg.time || new Date().toLocaleTimeString()
    });
    if (clipboardLogs.length > 200) clipboardLogs.pop();
    renderClipboardLog();
  }

  function renderClipboardLog() {
    const container = $id('clipboardLog');
    const showClipboard = $id('filterClipboard').checked;
    const showKeyboard = $id('filterKeyboard').checked;

    const filtered = clipboardLogs.filter(log => {
      if (log.type === 'clipboard' && !showClipboard) return false;
      if (log.type === 'keyboard' && !showKeyboard) return false;
      return true;
    });

    if (filtered.length === 0) {
      container.innerHTML = '<div class="text-base-content/50 text-center py-8">暂无记录</div>';
      return;
    }

    container.innerHTML = filtered.slice(0, 50).map(log => `
      <div class="log-item">
        <div class="header">
          <span class="app-name">${escapeHtml(log.app)}</span>
          <span class="log-time">${escapeHtml(log.time)}</span>
        </div>
        <div class="log-content">${log.type === 'keyboard' ? formatKey(log.content) : escapeHtml(log.content)}</div>
      </div>
    `).join('');
  }

  function formatKey(key) {
    if (key.startsWith('[') && key.endsWith(']')) {
      return `<span class="special-key">${escapeHtml(key)}</span>`;
    }
    return escapeHtml(key);
  }

  function escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
  }

  $id('filterClipboard').addEventListener('change', renderClipboardLog);
  $id('filterKeyboard').addEventListener('change', renderClipboardLog);

  // ============================================================
  // 登录系统
  // ============================================================

  /** 等待 C# 发来 device-info 消息（含 machineGuid）*/
  let _machineGuid = '';
  let _deviceName  = '';
  let _loginReady  = false;   // machineGuid 已就位

  // 先注册一次 device-info 钩子
  Bridge.onMessage((msg) => {
    if (msg.type === 'device-info' && !_loginReady) {
      _machineGuid = msg.data?.machineGuid || msg.data?.computerName || 'unknown';
      _deviceName  = msg.data?.computerName || 'Unknown';
      _loginReady  = true;
      $id('loginDeviceId').textContent = _machineGuid.slice(0, 18) + '...';
    }
  });

  // 浏览器调试环境直接用 navigator.userAgent 作为设备ID
  if (!Bridge.isWebView2) {
    _machineGuid = 'browser-debug-' + (navigator.userAgent.length).toString();
    _deviceName  = 'Browser Debug';
    _loginReady  = true;
    $id('loginDeviceId').textContent = _machineGuid;
  }

  // 不预填卡密（明文存储 localStorage 存在安全风险）

  // 登录按钮逻辑（挂到 window 供 HTML onclick 调用）
  window._doLogin = async function () {
    const cardKey  = $id('loginCardKey').value.trim().toUpperCase();
    const statusEl = $id('loginStatus');
    const btn      = $id('loginBtn');

    if (!cardKey) {
      statusEl.innerHTML = '<span class="text-error">请输入卡密</span>';
      return;
    }
    if (!_loginReady) {
      statusEl.innerHTML = '<span class="text-warning">等待设备信息就绪...</span>';
      return;
    }

    btn.disabled = true;
    btn.innerHTML = '<i class="fas fa-spinner fa-spin mr-2"></i>验证中...';
    statusEl.innerHTML = '';

    try {
      const serverUrl = AppConfig.resolveServerUrl();
      const resp = await fetch(`${serverUrl}/api/auth/login`, {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body:    JSON.stringify({
          cardKey,
          deviceId:   _machineGuid,
          deviceName: _deviceName,
        }),
      });

      const result = await resp.json();

      if (!result.success) {
        statusEl.innerHTML = `<span class="text-error"><i class="fas fa-times-circle mr-1"></i>${escapeHtml(result.error || '验证失败')}</span>`;
        btn.disabled = false;
        btn.innerHTML = '<i class="fas fa-sign-in-alt mr-2"></i>登 录';
        return;
      }

      // 登录成功
      const { sessionToken, expireAt } = result.data;
      AppConfig.setSessionToken(sessionToken);

      // 显示有效期
      const expireDate = new Date(expireAt * 1000).toLocaleDateString('zh-CN');
      let loginInfo = `授权有效至 ${expireDate}`;
      $id('loginExpiryText').textContent = loginInfo;
      $id('loginExpiry').classList.remove('hidden');
      statusEl.innerHTML = '<span class="text-success"><i class="fas fa-check-circle mr-1"></i>验证成功，正在进入...</span>';

      // 登录后立即获取配额信息
      try {
        const qr = await fetch(`${serverUrl}/api/quota`, {
          headers: { 'Authorization': `Bearer ${sessionToken}` },
        });
        const qj = await qr.json();
        if (qj.success) window._quotaInfo = qj.data;
      } catch (_) { /* 配额获取失败不阻塞登录 */ }

      // 延迟 800ms 后隐藏登录层，进入主界面
      setTimeout(() => {
        const overlay = $id('loginOverlay');
        overlay.style.transition = 'opacity 0.4s ease';
        overlay.style.opacity = '0';
        setTimeout(() => overlay.remove(), 400);
        _initMainApp();
      }, 800);

    } catch (e) {
      statusEl.innerHTML = `<span class="text-error"><i class="fas fa-exclamation-triangle mr-1"></i>网络错误: ${escapeHtml(e.message || '未知')}</span>`;
      btn.disabled = false;
      btn.innerHTML = '<i class="fas fa-sign-in-alt mr-2"></i>登 录';
    }
  };

  // Enter 键触发登录
  $id('loginCardKey').addEventListener('keyup', (e) => {
    if (e.key === 'Enter') window._doLogin();
  });

  // 登录按钮点击
  $id('loginBtn').addEventListener('click', () => window._doLogin());

  // 请求 C# 发送设备信息（触发 machineGuid 填充）
  if (Bridge.isWebView2) Bridge.requestDeviceInfo();

  // ============================================================
  // 配额显示
  // ============================================================
  function _updateHeaderQuota(quota) {
    const el = $id('headerScreenQuota');
    if (!el || !quota) return;

    if (quota.exceeded) {
      el.innerHTML = `配额: <span class="text-error font-medium">已用尽</span>`;
    } else if (quota.totalMinutes != null) {
      const remaining = quota.remainingMinutes;
      const total = quota.totalMinutes;
      const pct = total > 0 ? ((total - remaining) / total * 100) : 0;
      // 根据剩余比例选颜色
      const color = pct >= 90 ? 'text-error' : pct >= 70 ? 'text-warning' : 'text-success';
      // 友好显示：>= 60 分钟用小时，否则用分钟
      let display;
      if (remaining >= 60) {
        display = (remaining / 60).toFixed(1) + ' 小时';
      } else {
        display = Math.round(remaining) + ' 分钟';
      }
      el.innerHTML = `配额: <span class="${color} font-medium">${display}</span>` +
        `<span class="text-base-content/40 text-[9px]"> / ${total >= 60 ? (total/60).toFixed(0) + 'h' : total + 'm'}</span>`;
    } else {
      el.innerHTML = `配额: <span class="text-success">正常</span>`;
    }
  }

  // ============================================================
  // 日志迁移 (Performance Data Migration)
  // ============================================================
  const PerfMigration = (() => {
    let scannedFiles = [];   // 扫描到的文件列表
    const HARDCODED_SERVER = 'http://114.66.39.60:5222';
    const HARDCODED_FAKE_DAYS = 15;

    // ── 接收方状态 ──
    let recvTargetPath = '';  // 选定的目标目录

    // ── 通用：渲染多目录选择器 ──
    function renderDirSelector(containerId, candidates, onSelect) {
      const container = $id(containerId);
      if (!container) return;
      container.classList.remove('hidden');
      container.innerHTML = `
        <div class="text-xs text-warning mb-2"><i class="fas fa-list mr-1"></i>找到 ${candidates.length} 个候选目录，请选择:</div>
        <div class="space-y-1.5 max-h-40 overflow-y-auto no-scrollbar">
          ${candidates.map((c, i) => `
            <div class="flex items-center gap-2 px-3 py-2 rounded-lg bg-base-200 border border-base-300 hover:border-primary/50 cursor-pointer transition-colors perf-dir-option" data-idx="${i}">
              <i class="fas fa-folder text-warning text-xs"></i>
              <span class="text-xs text-base-content/80 font-mono flex-1 truncate" title="${c}">${c}</span>
              <i class="fas fa-chevron-right text-base-content/30 text-[10px]"></i>
            </div>
          `).join('')}
        </div>
      `;
      container.querySelectorAll('.perf-dir-option').forEach(el => {
        el.addEventListener('click', () => {
          const idx = parseInt(el.dataset.idx);
          container.classList.add('hidden');
          onSelect(candidates[idx]);
        });
      });
    }

    // — 模式切换 —
    $id('perfBtnSender')?.addEventListener('click', () => {
      $id('perfModeSelect').classList.add('hidden');
      $id('perfSenderFlow').classList.remove('hidden');
    });
    $id('perfBtnReceiver')?.addEventListener('click', () => {
      $id('perfModeSelect').classList.add('hidden');
      $id('perfReceiverFlow').classList.remove('hidden');
      // 加载当前 CPU 信息
      Bridge.postMessage('perf-get-cpu-info', {});
    });
    $id('perfSenderBack')?.addEventListener('click', () => {
      $id('perfSenderFlow').classList.add('hidden');
      $id('perfModeSelect').classList.remove('hidden');
    });
    $id('perfReceiverBack')?.addEventListener('click', () => {
      $id('perfReceiverFlow').classList.add('hidden');
      $id('perfModeSelect').classList.remove('hidden');
    });

    // ═══════════════════════════════════════
    //  发送方
    // ═══════════════════════════════════════

    // — 自动扫描 —
    $id('perfBtnAutoScan')?.addEventListener('click', () => {
      $id('perfDirSelect')?.classList.add('hidden');
      const status = $id('perfScanStatus');
      status.textContent = '正在扫描...';
      status.className = 'text-xs text-warning mb-2';
      Bridge.postMessage('perf-scan-files', {});
    });

    // — 全盘扫描 —
    $id('perfBtnDriveScan')?.addEventListener('click', () => {
      $id('perfDirSelect')?.classList.add('hidden');
      const status = $id('perfScanStatus');
      status.textContent = '正在全盘搜索 (可能需要 30 秒)...';
      status.className = 'text-xs text-warning mb-2';
      Bridge.postMessage('perf-scan-drives', {});
    });

    // — 手动选择目录 —
    $id('perfBtnManualPath')?.addEventListener('click', () => {
      const el = $id('perfManualPathInput');
      el.classList.toggle('hidden');
      if (!el.classList.contains('hidden')) $id('perfManualPathValue')?.focus();
    });
    $id('perfBtnManualScan')?.addEventListener('click', () => {
      const dir = $id('perfManualPathValue')?.value?.trim();
      if (!dir) { showToast('请输入目录路径', 'error'); return; }
      const status = $id('perfScanStatus');
      status.textContent = '正在扫描 ' + dir + ' ...';
      status.className = 'text-xs text-warning mb-2';
      Bridge.postMessage('perf-scan-files', { path: dir });
    });
    $id('perfManualPathValue')?.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') $id('perfBtnManualScan')?.click();
    });

    // — 全选切换 —
    $id('perfSelectAll')?.addEventListener('change', (e) => {
      $$('#perfFileList input[type="checkbox"]').forEach(cb => cb.checked = e.target.checked);
      updateUploadBtn();
    });

    function updateUploadBtn() {
      const checked = $$('#perfFileList input[type="checkbox"]:checked').length;
      const btn = $id('perfBtnUpload');
      if (btn) btn.disabled = checked === 0;
    }

    function renderFileList(files) {
      scannedFiles = files;
      const container = $id('perfFileList');
      const countEl = $id('perfFileCount');
      const resultsEl = $id('perfScanResults');
      if (!container) return;

      container.innerHTML = '';
      countEl.textContent = files.length;
      resultsEl.classList.remove('hidden');

      files.forEach((f, i) => {
        const sizeKB = Math.round((f.fileSize || 0) / 1024);
        const anomaly = f.isTimeAnomaly ? '<span class="text-warning ml-1" title="时间戳异常">⚠</span>' : '';
        const ctime = f.creationTime || '--';
        const div = document.createElement('div');
        div.className = 'flex items-center gap-2 px-2 py-1.5 rounded hover:bg-base-200 transition-colors';
        div.innerHTML = `
          <input type="checkbox" checked data-idx="${i}" class="checkbox checkbox-sm checkbox-primary shrink-0" onchange="document.dispatchEvent(new Event('perf-check-change'))">
          <span class="text-xs text-base-content/80 flex-1 truncate" title="${f.fullPath}">${f.filename}</span>
          <span class="text-[10px] text-info/70 shrink-0" title="对局结束时间">${ctime}</span>
          <span class="text-[10px] text-base-content/40 shrink-0">${sizeKB} KB</span>
          ${anomaly}
        `;
        container.appendChild(div);
      });
      updateUploadBtn();
    }
    document.addEventListener('perf-check-change', updateUploadBtn);

    // — 打包上传 —
    $id('perfBtnUpload')?.addEventListener('click', () => {
      const serverUrl = $id('perfSenderServer')?.value?.trim() || HARDCODED_SERVER;

      const selectedFiles = [];
      $$('#perfFileList input[type="checkbox"]:checked').forEach(cb => {
        const idx = parseInt(cb.dataset.idx);
        if (scannedFiles[idx]) selectedFiles.push(scannedFiles[idx]);
      });
      if (selectedFiles.length === 0) { showToast('未选择任何文件', 'error'); return; }

      const btn = $id('perfBtnUpload');
      const status = $id('perfUploadStatus');
      btn.disabled = true;
      btn.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>处理中...';
      status.classList.remove('hidden');
      status.textContent = '正在打包...';
      status.className = 'text-xs text-warning mt-2';

      Bridge.postMessage('perf-pack-upload', { serverUrl, files: selectedFiles });
    });

    // — 复制按钮 —
    $id('perfCopyToken')?.addEventListener('click', () => {
      navigator.clipboard.writeText($id('perfUploadToken')?.textContent || '');
      showToast('Token 已复制');
    });
    $id('perfCopyKey')?.addEventListener('click', () => {
      navigator.clipboard.writeText($id('perfUploadKey')?.textContent || '');
      showToast('Key 已复制');
    });

    // ═══════════════════════════════════════
    //  接收方
    // ═══════════════════════════════════════

    function setRecvTargetPath(p) {
      recvTargetPath = p;
      const display = $id('perfRecvSelectedPath');
      const pathText = $id('perfRecvPathDisplay');
      if (display && pathText) {
        display.classList.remove('hidden');
        pathText.textContent = p;
      }
      const status = $id('perfRecvScanStatus');
      if (status) {
        status.textContent = '已选择目标路径';
        status.className = 'text-xs text-emerald-400 mb-1';
      }
      updateMigrationBtn();
    }

    function updateMigrationBtn() {
      const btn = $id('perfBtnStartMigration');
      const token = $id('perfReceiverToken')?.value?.trim();
      if (btn) btn.disabled = !recvTargetPath || !token;
    }

    // 监听 token/key 输入变化，自动更新按钮状态
    $id('perfReceiverToken')?.addEventListener('input', updateMigrationBtn);
    $id('perfReceiverKey')?.addEventListener('input', updateMigrationBtn);

    // — 接收方: 自动扫描目标路径 —
    $id('perfRecvBtnAutoScan')?.addEventListener('click', () => {
      $id('perfRecvDirSelect')?.classList.add('hidden');
      const status = $id('perfRecvScanStatus');
      status.textContent = '正在扫描...';
      status.className = 'text-xs text-warning mb-1';
      Bridge.postMessage('perf-scan-files', { _recvScan: true });
    });

    // — 接收方: 全盘扫描目标路径 —
    $id('perfRecvBtnDriveScan')?.addEventListener('click', () => {
      $id('perfRecvDirSelect')?.classList.add('hidden');
      const status = $id('perfRecvScanStatus');
      status.textContent = '正在全盘搜索 (可能需要 30 秒)...';
      status.className = 'text-xs text-warning mb-1';
      Bridge.postMessage('perf-scan-drives', { _recvScan: true });
    });

    // — 接收方: 手动选择 —
    $id('perfRecvBtnManualPath')?.addEventListener('click', () => {
      const el = $id('perfRecvManualPathInput');
      el.classList.toggle('hidden');
      if (!el.classList.contains('hidden')) $id('perfRecvManualPathValue')?.focus();
    });
    $id('perfRecvBtnManualConfirm')?.addEventListener('click', () => {
      const dir = $id('perfRecvManualPathValue')?.value?.trim();
      if (!dir) { showToast('请输入目标路径', 'error'); return; }
      $id('perfRecvManualPathInput').classList.add('hidden');
      setRecvTargetPath(dir);
    });
    $id('perfRecvManualPathValue')?.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') $id('perfRecvBtnManualConfirm')?.click();
    });

    // — CPU 型号修改 —
    $id('perfBtnSetCpuName')?.addEventListener('click', () => {
      const cpuName = $id('perfCpuNameInput')?.value?.trim();
      if (!cpuName) { showToast('CPU 名称不能为空', 'error'); return; }
      const btn = $id('perfBtnSetCpuName');
      btn.disabled = true;
      btn.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>应用中...';
      Bridge.postMessage('perf-set-cpu-name', { cpuName, fakeDays: 15 });
    });

    // — 接收方: 一键迁移 —
    $id('perfBtnStartMigration')?.addEventListener('click', () => {
      const token = $id('perfReceiverToken')?.value?.trim();
      const downloadKey = $id('perfReceiverKey')?.value?.trim() || '';
      if (!token) { showToast('请填写 Token', 'error'); return; }
      if (!recvTargetPath) { showToast('请先选择目标路径', 'error'); return; }

      const btn = $id('perfBtnStartMigration');
      btn.disabled = true;
      btn.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>迁移中...';
      const status = $id('perfMigrationStatus');
      status.classList.remove('hidden');
      status.textContent = '正在下载...';
      status.className = 'text-xs text-warning mt-2';

      // 一键迁移：下载 + 硬件检测 + 应用，全部由 C++ 后端完成
      Bridge.postMessage('perf-one-click-migrate', {
        serverUrl: HARDCODED_SERVER,
        token,
        downloadKey,
        targetPath: recvTargetPath,
        fakeDays: HARDCODED_FAKE_DAYS
      });
    });

    // ═══════════════════════════════════════
    //  消息监听
    // ═══════════════════════════════════════

    // 记录扫描是否来自接收方
    let _lastScanIsRecv = false;

    Bridge.onMessage(msg => {
      if (!msg.type || !msg.type.startsWith('perf-')) return;
      const data = msg.data ?? msg;

      switch (msg.type) {
        case 'perf-scan-result': {
          if (_lastScanIsRecv) {
            // 接收方扫描 → 选定目标路径
            _lastScanIsRecv = false;
            const status = $id('perfRecvScanStatus');
            if (data.success && data.targetPath) {
              setRecvTargetPath(data.targetPath);
            } else {
              status.textContent = data.errorMessage || '未找到目标目录';
              status.className = 'text-xs text-error mb-1';
            }
          } else {
            // 发送方扫描
            const status = $id('perfScanStatus');
            if (data.success && data.files?.length > 0) {
              status.textContent = '扫描完成 — ' + (data.targetPath || '');
              status.className = 'text-xs text-emerald-400 mb-2';
              renderFileList(data.files);
            } else {
              status.textContent = data.errorMessage || '未找到 perf_data 文件';
              status.className = 'text-xs text-error mb-2';
            }
          }
          break;
        }

        case 'perf-scan-drives-result': {
          if (_lastScanIsRecv) {
            _lastScanIsRecv = false;
            const status = $id('perfRecvScanStatus');
            if (data.success && data.candidates?.length > 0) {
              if (data.candidates.length === 1) {
                setRecvTargetPath(data.candidates[0]);
              } else {
                status.textContent = '找到多个候选目录';
                status.className = 'text-xs text-warning mb-1';
                renderDirSelector('perfRecvDirSelect', data.candidates, (dir) => {
                  setRecvTargetPath(dir);
                });
              }
            } else {
              status.textContent = '全盘扫描未找到 performance 目录';
              status.className = 'text-xs text-error mb-1';
            }
          } else {
            // 发送方全盘扫描
            const status = $id('perfScanStatus');
            if (data.success && data.candidates?.length > 0) {
              if (data.candidates.length === 1) {
                // 只有一个候选→直接扫描
                status.textContent = '找到 1 个候选路径，正在扫描...';
                status.className = 'text-xs text-emerald-400 mb-2';
                Bridge.postMessage('perf-scan-files', { path: data.candidates[0] });
              } else {
                // 多个候选→展示选择
                status.textContent = '找到 ' + data.candidates.length + ' 个候选路径';
                status.className = 'text-xs text-warning mb-2';
                renderDirSelector('perfDirSelect', data.candidates, (dir) => {
                  status.textContent = '正在扫描 ' + dir + ' ...';
                  status.className = 'text-xs text-warning mb-2';
                  Bridge.postMessage('perf-scan-files', { path: dir });
                });
              }
            } else {
              status.textContent = '全盘扫描未找到 performance 目录';
              status.className = 'text-xs text-error mb-2';
            }
          }
          break;
        }

        case 'perf-progress': {
          const stage = data.stage || '';
          const pct = Math.round((data.progress || 0) * 100);
          const labels = { packing: '打包', uploading: '上传', downloading: '下载', extracting: '解压', processing: '处理文件', timestamps: '时间戳', driver: '驱动', detecting: '硬件检测' };
          const label = labels[stage] || stage;
          const text = `${label}中... ${pct}%` + (data.current ? ` (${data.current}/${data.total})` : '');

          const uploadStatus = $id('perfUploadStatus');
          const migStatus = $id('perfMigrationStatus');
          if (uploadStatus && !uploadStatus.classList.contains('hidden')) uploadStatus.textContent = text;
          if (migStatus && !migStatus.classList.contains('hidden')) migStatus.textContent = text;
          break;
        }

        case 'perf-upload-result': {
          const btn = $id('perfBtnUpload');
          const status = $id('perfUploadStatus');
          btn.disabled = false;
          btn.innerHTML = '<i class="fas fa-rocket mr-1"></i>打包并上传';

          if (data.success) {
            status.textContent = '上传完成！';
            status.className = 'text-xs text-emerald-400 mt-2';
            $id('perfUploadResult').classList.remove('hidden');
            $id('perfUploadToken').textContent = data.token || '';
            $id('perfUploadKey').textContent = data.downloadKey || '';
          } else {
            status.textContent = '上传失败: ' + (data.errorMessage || '未知错误');
            status.className = 'text-xs text-error mt-2';
          }
          break;
        }

        case 'perf-one-click-result': {
          const btn = $id('perfBtnStartMigration');
          const status = $id('perfMigrationStatus');
          const result = $id('perfMigrationResult');
          btn.disabled = false;
          btn.innerHTML = '<i class="fas fa-play mr-1"></i>开始迁移';

          if (data.success) {
            status.textContent = '迁移完成！';
            status.className = 'text-xs text-emerald-400 mt-2';
            result.classList.remove('hidden');
            // 已接收文件列表
            let filesHtml = '';
            if (data.receivedFiles?.length > 0) {
              filesHtml = '<div class="mt-2 border-t border-base-300 pt-2"><div class="text-base-content/60 mb-1"><i class="fas fa-list mr-1"></i>已接收文件:</div>' +
                data.receivedFiles.map(f => `<div class="flex items-center justify-between py-0.5"><span class="text-base-content/80 truncate">${f.filename}</span><span class="text-info/70 shrink-0 ml-2">${f.creationTime || '--'}</span></div>`).join('') +
                '</div>';
            }
            result.innerHTML = `
              <div class="text-emerald-400"><i class="fas fa-check-circle mr-1"></i>文件处理: ${data.filesProcessed}/${data.totalFiles}</div>
              <div class="text-base-content/60"><i class="fas fa-clock mr-1"></i>时间戳设置: ${data.timestampsSet} 个文件</div>
              <div class="${data.driverFakeEntries ? 'text-emerald-400' : 'text-warning'}">
                <i class="fas fa-microchip mr-1"></i>驱动伪造: ${data.driverFakeEntries ? '成功' : (data.driverConnected ? '部分' : '跳过')}
              </div>
              ${filesHtml}
            `;
          } else {
            status.textContent = '迁移失败: ' + (data.errorMessage || '未知错误');
            status.className = 'text-xs text-error mt-2';
          }
          break;
        }

        case 'perf-driver-status-result': {
          const badge = $id('perfDriverBadge');
          if (badge) {
            badge.classList.remove('hidden');
            const connected = data.connected;
            badge.className = `text-[10px] px-2 py-0.5 rounded ${connected ? 'bg-success/10 text-success' : 'bg-base-300 text-base-content/60'}`;
            badge.innerHTML = `<i class="fas fa-microchip mr-1"></i>驱动: ${connected ? '已连接' : (data.loaded ? '已加载' : '未加载')}`;
          }
          break;
        }

        case 'perf-get-cpu-info-result': {
          const input = $id('perfCpuNameInput');
          const btn = $id('perfBtnSetCpuName');
          if (input) {
            input.value = data.fakeCpu || data.currentCpu || '';
            input.placeholder = data.currentCpu || '未知 CPU';
            btn && (btn.disabled = false);
          }
          break;
        }

        case 'perf-set-cpu-name-result': {
          const btn = $id('perfBtnSetCpuName');
          const status = $id('perfCpuStatus');
          if (btn) {
            btn.disabled = false;
            btn.innerHTML = '<i class="fas fa-pen mr-1"></i>应用';
          }
          if (status) {
            status.classList.remove('hidden');
            if (data.success) {
              status.className = 'text-[10px] text-emerald-400 mt-2';
              let detail = '修改已生效';
              if (data.driverPersist) detail += ' · 无需重启';
              if (!data.acpiWrite) detail += ' · ACPI 未找到 (无影响)';
              status.textContent = '✓ ' + detail;
            } else {
              status.className = 'text-[10px] text-error mt-2';
              status.textContent = '✗ ' + (data.errorMessage || '写入失败');
            }
          }
          break;
        }
      }
    });

    // ── 拦截发送方/接收方扫描消息以区分来源 ──
    const origPostMessage = Bridge.postMessage.bind(Bridge);
    Bridge.postMessage = function(type, data) {
      if ((type === 'perf-scan-files' || type === 'perf-scan-drives') && data?._recvScan) {
        _lastScanIsRecv = true;
        const cleanData = { ...data };
        delete cleanData._recvScan;
        return origPostMessage(type, cleanData);
      }
      if (type === 'perf-scan-files' || type === 'perf-scan-drives') {
        _lastScanIsRecv = false;
      }
      return origPostMessage(type, data);
    };

    // ═══════════════════════════════════════
    //  WeGame QQ 登录日志迁移
    // ═══════════════════════════════════════
    let wgScannedFiles = [];   // WeGame 扫描到的文件
    let wgReceivedFiles = [];  // 下载接收到的文件信息
    let wgExtractDir = '';     // 解压临时目录

    // — 模式切换 —
    $id('perfBtnWeGame')?.addEventListener('click', () => {
      $id('perfModeSelect').classList.add('hidden');
      $id('perfWeGameFlow').classList.remove('hidden');
    });
    $id('perfWeGameBack')?.addEventListener('click', () => {
      $id('perfWeGameFlow').classList.add('hidden');
      $id('perfModeSelect').classList.remove('hidden');
    });

    // — 发送/接收面板切换 —
    $id('wgTabSend')?.addEventListener('click', () => {
      $id('wgSendPanel').classList.remove('hidden');
      $id('wgRecvPanel').classList.add('hidden');
      $id('wgTabSend').className = 'text-xs px-4 py-2 rounded-lg btn btn-primary btn-sm';
      $id('wgTabRecv').className = 'text-xs px-4 py-2 rounded-lg btn btn-ghost btn-sm';
    });
    $id('wgTabRecv')?.addEventListener('click', () => {
      $id('wgSendPanel').classList.add('hidden');
      $id('wgRecvPanel').classList.remove('hidden');
      $id('wgTabRecv').className = 'text-xs px-4 py-2 rounded-lg btn btn-primary btn-sm';
      $id('wgTabSend').className = 'text-xs px-4 py-2 rounded-lg btn btn-ghost btn-sm';
      // 自动扫描本机
      if (!$id('wgLocalFiles').classList.contains('hidden')) return;
      $id('wgBtnScanLocal')?.click();
    });

    // — 扫描本机 WeGame QQ 登录记录 —
    $id('wgBtnScan')?.addEventListener('click', () => {
      const status = $id('wgScanStatus');
      status.textContent = '正在扫描...';
      status.className = 'text-xs text-warning mb-2';
      Bridge.postMessage('perf-scan-wegame', {});
    });

    // — 全选切换 —
    $id('wgSelectAll')?.addEventListener('change', (e) => {
      $$('#wgFileList input[type="checkbox"]').forEach(cb => cb.checked = e.target.checked);
      wgUpdateUploadBtn();
    });

    function wgUpdateUploadBtn() {
      const checked = $$('#wgFileList input[type="checkbox"]:checked').length;
      const btn = $id('wgBtnUpload');
      if (btn) btn.disabled = checked === 0;
    }

    function wgRenderFileList(files) {
      wgScannedFiles = files;
      const container = $id('wgFileList');
      const countEl = $id('wgFileCount');
      const resultsEl = $id('wgScanResults');
      if (!container) return;
      container.innerHTML = '';
      countEl.textContent = files.length;
      resultsEl.classList.remove('hidden');
      files.forEach((f, i) => {
        const div = document.createElement('div');
        div.className = 'flex items-center gap-2 px-2 py-1.5 rounded hover:bg-base-200 transition-colors';
        div.innerHTML = `
          <input type="checkbox" checked data-idx="${i}" class="checkbox checkbox-sm checkbox-primary shrink-0" onchange="document.dispatchEvent(new Event('wg-check-change'))">
          <span class="text-xs text-orange-500 w-28 shrink-0 font-mono">${f.filename}</span>
          <span class="text-[10px] text-info/70 flex-1 truncate">${f.creationTime || '--'}</span>
          <span class="text-[10px] text-base-content/50 shrink-0">${f.modificationTime || '--'}</span>
        `;
        container.appendChild(div);
      });
      wgUpdateUploadBtn();
    }
    document.addEventListener('wg-check-change', wgUpdateUploadBtn);

    // — 打包上传 —
    $id('wgBtnUpload')?.addEventListener('click', () => {
      const selectedFiles = [];
      $$('#wgFileList input[type="checkbox"]:checked').forEach(cb => {
        const idx = parseInt(cb.dataset.idx);
        if (wgScannedFiles[idx]) selectedFiles.push(wgScannedFiles[idx]);
      });
      if (selectedFiles.length === 0) { showToast('未选择任何文件', 'error'); return; }
      const btn = $id('wgBtnUpload');
      const status = $id('wgUploadStatus');
      btn.disabled = true;
      btn.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>处理中...';
      status.classList.remove('hidden');
      status.textContent = '正在打包...';
      status.className = 'text-xs text-warning mt-2';
      Bridge.postMessage('perf-pack-upload', { serverUrl: HARDCODED_SERVER, files: selectedFiles, wegame: true });
    });

    // — 复制 Token/Key —
    $id('wgCopyToken')?.addEventListener('click', () => {
      navigator.clipboard.writeText($id('wgUploadToken')?.textContent || '');
      showToast('Token 已复制');
    });
    $id('wgCopyKey')?.addEventListener('click', () => {
      navigator.clipboard.writeText($id('wgUploadKey')?.textContent || '');
      showToast('Key 已复制');
    });

    // — 接收方: 下载 —
    $id('wgBtnDownload')?.addEventListener('click', () => {
      const token = $id('wgRecvToken')?.value?.trim();
      const downloadKey = $id('wgRecvKey')?.value?.trim() || '';
      if (!token) { showToast('请填写 Token', 'error'); return; }
      const status = $id('wgDownloadStatus');
      status.textContent = '正在下载...';
      status.className = 'text-xs text-warning mb-2';
      Bridge.postMessage('perf-wegame-download', {
        serverUrl: HARDCODED_SERVER, token, downloadKey
      });
    });

    // — 接收方: 扫描本机 —
    $id('wgBtnScanLocal')?.addEventListener('click', () => {
      const status = $id('wgLocalStatus');
      status.textContent = '正在扫描本机...';
      status.className = 'text-xs text-warning mb-2';
      Bridge.postMessage('perf-scan-wegame', { localOnly: true });
    });

    // — 接收方: 应用 —
    $id('wgBtnApply')?.addEventListener('click', () => {
      if (wgReceivedFiles.length === 0) { showToast('没有可应用的文件', 'error'); return; }
      // 收集自定义时间
      const filesWithTimes = wgReceivedFiles.map((f, i) => {
        const ctEl = $id('wgRecvCT_' + i);
        const mtEl = $id('wgRecvMT_' + i);
        return {
          filename: f.filename,
          extractedPath: f.extractedPath,
          customCreationTime: ctEl?.value || '',
          customModificationTime: mtEl?.value || ''
        };
      });
      const btn = $id('wgBtnApply');
      const status = $id('wgApplyStatus');
      btn.disabled = true;
      btn.innerHTML = '<i class="fas fa-spinner fa-spin mr-1"></i>应用中...';
      status.classList.remove('hidden');
      status.textContent = '正在应用...';
      status.className = 'text-xs text-warning mt-2';
      Bridge.postMessage('perf-wegame-apply', { files: filesWithTimes });
    });

    // — WeGame 专用消息监听 —
    Bridge.onMessage(wgMsg => {
      if (!wgMsg.type || !wgMsg.type.startsWith('perf-')) return;
      const d = wgMsg.data ?? wgMsg;

      switch (wgMsg.type) {
        case 'perf-scan-wegame-result': {
          if (d.localOnly) {
            // 本机文件列表 (接收方)
            const status = $id('wgLocalStatus');
            if (d.success && d.files?.length > 0) {
              status.textContent = `本机有 ${d.files.length} 条记录`;
              status.className = 'text-xs text-emerald-400 mb-2';
              const localFiles = $id('wgLocalFiles');
              const list = $id('wgLocalFileList');
              localFiles.classList.remove('hidden');
              list.innerHTML = '';
              d.files.forEach(f => {
                const div = document.createElement('div');
                div.className = 'flex items-center gap-2 px-2 py-1 rounded hover:bg-base-200';
                div.innerHTML = `
                  <span class="text-xs text-orange-500 w-28 shrink-0 font-mono">${f.filename}</span>
                  <span class="text-[10px] text-info/70 flex-1 truncate">${f.creationTime || '--'}</span>
                  <span class="text-[10px] text-base-content/50 shrink-0">${f.modificationTime || '--'}</span>
                `;
                list.appendChild(div);
              });
            } else {
              status.textContent = d.errorMessage || '本机未找到 QQ 登录记录';
              status.className = 'text-xs text-base-content/50 mb-2';
            }
          } else {
            // 发送方扫描
            const status = $id('wgScanStatus');
            if (d.success && d.files?.length > 0) {
              status.textContent = '扫描完成';
              status.className = 'text-xs text-emerald-400 mb-2';
              wgRenderFileList(d.files);
            } else {
              status.textContent = d.errorMessage || '未找到 QQ 登录记录';
              status.className = 'text-xs text-error mb-2';
            }
          }
          break;
        }

        case 'perf-wegame-download-result': {
          const status = $id('wgDownloadStatus');
          if (d.success && d.files?.length > 0) {
            status.textContent = '下载完成';
            status.className = 'text-xs text-emerald-400 mb-2';
            wgReceivedFiles = d.files;
            wgExtractDir = d.extractDir || '';
            const container = $id('wgReceivedFiles');
            const list = $id('wgReceivedFileList');
            container.classList.remove('hidden');
            list.innerHTML = '';
            d.files.forEach((f, i) => {
              const div = document.createElement('div');
              div.className = 'flex items-center gap-2 px-2 py-1.5 rounded hover:bg-base-200';
              // datetime-local 需要格式: 2024-01-15T14:30
              const ctVal = f.creationTimeLocal || '';
              const mtVal = f.modificationTimeLocal || '';
              div.innerHTML = `
                <span class="text-xs text-orange-500 w-28 shrink-0 font-mono">${f.filename}</span>
                <input id="wgRecvCT_${i}" type="datetime-local" value="${ctVal}" step="1"
                  class="text-[10px] input input-bordered input-xs flex-1 min-w-0">
                <input id="wgRecvMT_${i}" type="datetime-local" value="${mtVal}" step="1"
                  class="text-[10px] input input-bordered input-xs flex-1 min-w-0">
              `;
              list.appendChild(div);
            });
            $id('wgBtnApply').disabled = false;
          } else {
            status.textContent = '下载失败: ' + (d.errorMessage || '未知错误');
            status.className = 'text-xs text-error mb-2';
          }
          break;
        }

        case 'perf-wegame-apply-result': {
          const btn = $id('wgBtnApply');
          const status = $id('wgApplyStatus');
          const result = $id('wgApplyResult');
          btn.disabled = false;
          btn.innerHTML = '<i class="fas fa-play mr-1"></i>应用';
          if (d.success) {
            status.textContent = '应用完成！';
            status.className = 'text-xs text-emerald-400 mt-2';
            result.classList.remove('hidden');
            result.innerHTML = `
              <div class="text-emerald-400"><i class="fas fa-check-circle mr-1"></i>成功写入 ${d.filesApplied || 0} 个文件</div>
              <div class="text-base-content/60"><i class="fas fa-clock mr-1"></i>时间戳设置: ${d.timestampsSet || 0} 个文件</div>
            `;
            // 自动刷新本机列表
            $id('wgBtnScanLocal')?.click();
          } else {
            status.textContent = '应用失败: ' + (d.errorMessage || '未知错误');
            status.className = 'text-xs text-error mt-2';
          }
          break;
        }

        // WeGame 上传结果 (复用 perf-upload-result，通过 wegame 标识区分)
        case 'perf-upload-result': {
          // 仅在 WeGame 面板可见时处理
          if ($id('wgSendPanel')?.classList.contains('hidden')) break;
          if ($id('perfWeGameFlow')?.classList.contains('hidden')) break;
          const btn = $id('wgBtnUpload');
          const status = $id('wgUploadStatus');
          if (!btn) break;
          btn.disabled = false;
          btn.innerHTML = '<i class="fas fa-rocket mr-1"></i>打包并上传';
          if (d.success) {
            status.textContent = '上传完成！';
            status.className = 'text-xs text-emerald-400 mt-2';
            $id('wgUploadResult').classList.remove('hidden');
            $id('wgUploadToken').textContent = d.token || '';
            $id('wgUploadKey').textContent = d.downloadKey || '';
          } else {
            status.textContent = '上传失败: ' + (d.errorMessage || '未知错误');
            status.className = 'text-xs text-error mt-2';
          }
          break;
        }
      }
    });

    return { renderFileList };
  })();

  // ── base64 helpers (chunked, stack-safe) ──
  function arrayBufferToBase64(buf) {
    const bytes = new Uint8Array(buf);
    const CHUNK = 0x8000; // 32 KB
    let binary = '';
    for (let i = 0; i < bytes.length; i += CHUNK) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + CHUNK, bytes.length)));
    }
    return btoa(binary);
  }
  function base64ToUint8Array(b64) {
    const raw = atob(b64);
    const out = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
    return out;
  }

  // ============================================================
  // PerfSync 初始化与 UI 绑定
  // ============================================================
  function _initPerfSync() {
    if (typeof PerfSync === 'undefined') {
      console.warn('[PerfSync] PerfSync 模块未加载');
      return;
    }

    PerfSync.init();

    // ── 开关按钮 ──
    const toggleBtn = $id('perfSyncToggleBtn');
    const toggleIcon = $id('perfSyncToggleIcon');
    if (toggleBtn) {
      toggleBtn.addEventListener('click', () => {
        const status = PerfSync.getStatus();
        PerfSync.setEnabled(!status.enabled);
      });
    }

    // ── 扫描按钮 ──
    $id('perfSyncAutoScan')?.addEventListener('click', () => PerfSync.discoverDir());
    $id('perfSyncFullScan')?.addEventListener('click', () => PerfSync.discoverDirFullScan());

    // ── 多目录选择回调 ──
    PerfSync.onDirCandidates((candidates) => {
      const container = $id('perfSyncDirCandidates');
      if (!container) return;
      container.classList.remove('hidden');
      container.innerHTML = `
        <div class="text-[10px] text-warning mb-1"><i class="fas fa-list mr-1"></i>找到 ${candidates.length} 个候选目录:</div>
        <div class="space-y-1 max-h-32 overflow-y-auto no-scrollbar">
          ${candidates.map(c => `
            <button class="w-full text-left bg-base-200 hover:bg-primary/10 border border-base-300 rounded px-2 py-1.5 text-[10px] text-base-content/70 truncate transition-colors perf-sync-candidate"
              data-path="${c.replace(/"/g, '&quot;')}" title="${c}">
              <i class="fas fa-folder text-warning mr-1"></i>${c}
            </button>
          `).join('')}
        </div>`;
      container.querySelectorAll('.perf-sync-candidate').forEach(btn => {
        btn.addEventListener('click', () => {
          PerfSync.selectCandidate(btn.dataset.path);
          container.classList.add('hidden');
        });
      });
    });

    // ── 状态变化回调 ──
    PerfSync.onStatusChange((s) => {
      // 开关图标
      if (toggleIcon) {
        toggleIcon.innerHTML = s.enabled
          ? '<i class="fas fa-toggle-on"></i>'
          : '<i class="fas fa-toggle-off text-base-content/40"></i>';
        toggleIcon.className = s.enabled ? 'text-success text-sm' : 'text-base-content/40 text-sm';
      }

      // 目录状态
      const dirPath = $id('perfSyncDirPath');
      const dirScanning = $id('perfSyncDirScanning');
      if (dirPath && dirScanning) {
        if (s.scanning) {
          dirPath.classList.add('hidden');
          dirScanning.classList.remove('hidden');
        } else {
          dirPath.classList.remove('hidden');
          dirScanning.classList.add('hidden');
          if (s.dirReady) {
            dirPath.textContent = s.dirPath;
            dirPath.title = s.dirPath;
            dirPath.className = 'text-[10px] text-success truncate';
          } else {
            dirPath.textContent = '未找到 perf_data 目录';
            dirPath.title = '';
            dirPath.className = 'text-[10px] text-base-content/50 truncate';
          }
        }
      }

      // 同步统计 (共享中才显示)
      const statsEl = $id('perfSyncStats');
      if (statsEl) {
        if (s.mode) {
          statsEl.classList.remove('hidden');
          $id('perfSyncUploaded').textContent = s.stats.uploaded;
          $id('perfSyncDownloaded').textContent = s.stats.downloaded;
          const errBadge = $id('perfSyncErrorBadge');
          if (errBadge) {
            if (s.stats.errors > 0) {
              errBadge.classList.remove('hidden');
              $id('perfSyncErrors').textContent = s.stats.errors;
            } else {
              errBadge.classList.add('hidden');
            }
          }
        } else {
          statsEl.classList.add('hidden');
        }
      }

      // 推流端统计条内的 perf 同步指示
      const screenPerfStat = $id('screenPerfSyncStat');
      const screenPerfLabel = $id('screenPerfSyncLabel');
      if (screenPerfStat && screenPerfLabel) {
        if (s.mode === 'broadcaster' && s.enabled) {
          screenPerfStat.classList.remove('hidden');
          screenPerfLabel.textContent = `Perf: ${s.stats.uploaded}↑`;
        } else {
          screenPerfStat.classList.add('hidden');
        }
      }

      // 观看端状态 badge
      const viewerBadge = $id('viewerPerfSyncBadge');
      const viewerLabel = $id('viewerPerfSyncLabel');
      if (viewerBadge && viewerLabel) {
        if (s.mode === 'viewer' && s.enabled) {
          viewerBadge.classList.remove('hidden');
          if (s.scanning) {
            viewerLabel.textContent = '扫描目录中...';
            viewerBadge.title = '正在扫描 perf_data 目录';
          } else if (!s.dirReady) {
            viewerLabel.textContent = '目录未就绪';
            viewerBadge.title = '未找到 perf_data 目录，无法接收文件';
          } else if (s.stats.downloaded > 0) {
            viewerLabel.textContent = `已接收 ${s.stats.downloaded}`;
            viewerBadge.title = `目标路径: ${s.dirPath}`;
          } else {
            viewerLabel.textContent = '同步就绪';
            viewerBadge.title = `目标路径: ${s.dirPath}`;
          }
        } else {
          viewerBadge.classList.add('hidden');
        }
      }
    });

    console.log('[PerfSync] 初始化完成');
  }

  // ============================================================
  // 列头拖拽调整宽度
  // ============================================================
  function _initColumnResize() {
    const header = $id('swColHeader');
    if (!header) return;
    let dragging = null, startX = 0, startW = 0;

    header.querySelectorAll('.sw-drag-handle').forEach(handle => {
      handle.addEventListener('mousedown', e => {
        e.preventDefault();
        const col = handle.parentElement;
        dragging = col;
        startX = e.clientX;
        startW = col.offsetWidth;
        handle.classList.add('active');
        document.body.style.cursor = 'col-resize';
        document.body.style.userSelect = 'none';
      });
    });

    document.addEventListener('mousemove', e => {
      if (!dragging) return;
      const diff = e.clientX - startX;
      const newW = Math.max(60, startW + diff);
      const colName = dragging.dataset.col;
      if (colName && colName !== 'name') {
        header.style.setProperty('--col-' + colName, newW + 'px');
      }
    });

    document.addEventListener('mouseup', () => {
      if (!dragging) return;
      dragging.querySelector('.sw-drag-handle')?.classList.remove('active');
      dragging = null;
      document.body.style.cursor = '';
      document.body.style.userSelect = '';
    });
  }

  // ============================================================
  // 主界面初始化（登录成功后调用）
  // ============================================================
  function _initMainApp() {
    console.log('[MDShare] 初始化完成', Bridge.isWebView2 ? '(WebView2)' : '(浏览器)');

    initDeviceInfo();
    startUptimeTimer();
    addActivity('MDShare 控制台已打开', 'fa-power-off', 'text-success', 'bg-success/10');

    // 初始化配额显示
    if (window._quotaInfo) _updateHeaderQuota(window._quotaInfo);

    // 启动时自动检测VB-Cable
    checkAndShowVBCable();

    // 初始化 PerfSync (perf_data 实时同步)
    _initPerfSync();

    // 初始化列头拖拽调整列宽
    _initColumnResize();

    // URL参数自动加入频道
    const urlParams = new URLSearchParams(window.location.search);
    const channelParam = urlParams.get('channel');
    if (channelParam) {
      switchView('kiosk');
      inputChannelId.value = channelParam;
      setTimeout(() => btnJoinChannel.click(), 500);
    }
  }

})();

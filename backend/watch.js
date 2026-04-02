/**
 * watch.js — 公开观看端前端逻辑
 * 由 Express 以 GET /watch.js 提供
 * 对应 viewer.html 加载的外部脚本
 */
(async function () {
  'use strict';

  // ── DOM 引用 ────────────────────────────────────────────────
  const videoWrap   = document.getElementById('videoWrap');
  const placeholder = document.getElementById('placeholder');
  const phSpinner   = document.getElementById('phSpinner');
  const phIcon      = document.getElementById('phIcon');
  const phTitle     = document.getElementById('phTitle');
  const phSubtitle  = document.getElementById('phSubtitle');
  const statusBadge = document.getElementById('statusBadge');
  const channelMeta = document.getElementById('channelMeta');
  const netbar      = document.getElementById('netbar');
  const netDot      = document.getElementById('netDot');
  const netLabel    = document.getElementById('netLabel');
  const toolbar     = document.getElementById('toolbar');
  const btnMic      = document.getElementById('btnMic');
  const micOff      = document.getElementById('micOff');
  const micOn       = document.getElementById('micOn');
  const btnFs       = document.getElementById('btnFullscreen');
  const fsExpand    = document.getElementById('fsExpand');
  const fsCompress  = document.getElementById('fsCompress');

  // ── 状态辅助 ───────────────────────────────────────────────
  const QUALITY_LABEL = ['未知', '极佳', '良好', '一般', '较差', '很差', '断开'];
  const QUALITY_CLASS = ['', '', '', 'warn', 'warn', 'bad', 'bad'];

  function setStatus(text, cls) {
    statusBadge.textContent = text;
    statusBadge.className   = 'badge-' + cls;
  }

  function showPlaceholder({ spinner = false, icon = '', title = '', subtitle = '' }) {
    videoWrap.classList.add('hidden');
    placeholder.classList.remove('hidden');
    phSpinner.classList.toggle('hidden', !spinner);
    phIcon.classList.toggle('hidden', !icon);
    phIcon.textContent = icon;
    phTitle.textContent    = title;
    phSubtitle.textContent = subtitle;
  }

  function showVideo() {
    placeholder.classList.add('hidden');
    videoWrap.classList.remove('hidden');
    netbar.classList.remove('hidden');
  }

  // ── 读取频道 ID ────────────────────────────────────────────
  const params    = new URLSearchParams(location.search);
  const channelId = (params.get('channel') || '').trim();

  if (!/^[0-9a-f]{12}$/i.test(channelId)) {
    setStatus('链接无效', 'error');
    showPlaceholder({
      icon:     '🔗',
      title:    '链接无效',
      subtitle: '分享链接格式错误，请向共享者重新获取链接。',
    });
    return;
  }

  channelMeta.textContent = `# ${channelId.slice(0, 6)}…`;

  // ── 获取观看凭据 ─────────────────────────────────────────
  showPlaceholder({ spinner: true, title: '正在连接', subtitle: '正在获取频道凭据，请稍候…' });
  setStatus('正在连接…', 'connecting');

  let tokenData;
  try {
    const resp = await fetch(`/api/viewer/token?channel=${encodeURIComponent(channelId)}`);
    const json = await resp.json();
    if (!json.success) throw new Error(json.error || '凭据获取失败');
    tokenData = json.data;
  } catch (err) {
    setStatus('连接失败', 'error');
    showPlaceholder({
      icon:     '📡',
      title:    '无法连接',
      subtitle: err.message.includes('不存在') || err.message.includes('404')
                  ? '该频道不存在或共享已结束，请联系共享者。'
                  : `错误：${err.message}`,
    });
    return;
  }

  // ── 创建 Agora 客户端 ──────────────────────────────────────
  // live 模式：与推流端匹配，码率更稳定，画质更好
  const client = AgoraRTC.createClient({ mode: 'live', codec: 'h264' });
  client.setClientRole('audience');

  AgoraRTC.setLogLevel(3); // warn only，减少控制台噪音

  // 下行网络质量
  client.on('network-quality', (stats) => {
    const dq  = stats.downlinkNetworkQuality;
    const lbl = QUALITY_LABEL[dq] || '未知';
    const cls = QUALITY_CLASS[dq] || '';
    netDot.className   = 'net-dot' + (cls ? ' ' + cls : '');
    netLabel.textContent = `下行网络：${lbl}`;
  });

  // 远端用户发布
  client.on('user-published', async (user, mediaType) => {
    await client.subscribe(user, mediaType);

    if (mediaType === 'video') {
      videoWrap.innerHTML = '';
      showVideo();
      setStatus('直播中', 'live');
      user.videoTrack.play(videoWrap, { fit: 'contain' });
    }
    if (mediaType === 'audio') {
      user.audioTrack.play();
    }
  });

  // 远端用户取消发布
  client.on('user-unpublished', (user, mediaType) => {
    if (mediaType === 'video') {
      videoWrap.innerHTML = '';
      netbar.classList.add('hidden');
      setStatus('已暂停', 'ended');
      showPlaceholder({
        icon:     '⏸',
        title:    '共享已暂停',
        subtitle: '等待共享者继续推流…',
      });
    }
  });

  // 推流端离线
  client.on('user-left', () => {
    videoWrap.innerHTML = '';
    netbar.classList.add('hidden');
    setStatus('已结束', 'ended');
    showPlaceholder({
      icon:     '✅',
      title:    '共享已结束',
      subtitle: '共享者已停止屏幕共享。',
    });
  });

  // 异常断开
  client.on('exception', (evt) => {
    console.warn('[Viewer] Agora exception:', evt);
  });

  // ── 加入频道 ───────────────────────────────────────────────
  try {
    showPlaceholder({ spinner: true, title: '正在加入频道', subtitle: '建立 RTC 连接中…' });
    const { appId, channelName, uid, token } = tokenData;
    await client.join(appId, channelName, token, uid || null);
    // join 成功后等待 user-published 事件来显示画面
    showPlaceholder({
      spinner:  true,
      title:    '等待画面',
      subtitle: '已连接，等待共享者推送画面…',
    });
    setStatus('已连接', 'connecting');
    // 显示控制栏
    toolbar.classList.add('visible');
  } catch (err) {
    setStatus('连接失败', 'error');
    showPlaceholder({
      icon:     '⚠️',
      title:    '加入频道失败',
      subtitle: `RTC 错误：${err.message || err}`,
    });
  }

  // ── 麦克风（独立语音覆盖频道） ─────────────────────────────
  let voiceClient = null;
  let micTrack    = null;
  let micEnabled  = false;

  btnMic.addEventListener('click', async () => {
    btnMic.disabled = true;
    try {
      if (!micEnabled) {
        // 获取语音凭据
        const resp = await fetch('/api/viewer/voice-token', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ channelId }),
        });
        const result = await resp.json();
        if (!result.success) throw new Error(result.error || '获取语音凭据失败');
        const vd = result.data;

        voiceClient = AgoraRTC.createClient({ mode: 'rtc', codec: 'vp8' });
        await voiceClient.join(vd.appId, vd.channelName, vd.token, vd.uid);

        micTrack = await AgoraRTC.createMicrophoneAudioTrack({
          AEC: true, ANS: true, AGC: true,
        });
        await voiceClient.publish([micTrack]);
        micEnabled = true;
        btnMic.classList.add('active');
        btnMic.title = '关闭麦克风';
        micOff.style.display = 'none';
        micOn.style.display  = '';
      } else {
        // 关闭
        if (micTrack) {
          await voiceClient?.unpublish([micTrack]).catch(() => {});
          micTrack.close();
          micTrack = null;
        }
        if (voiceClient) {
          await voiceClient.leave().catch(() => {});
          voiceClient = null;
        }
        micEnabled = false;
        btnMic.classList.remove('active');
        btnMic.title = '开启麦克风';
        micOff.style.display = '';
        micOn.style.display  = 'none';
      }
    } catch (e) {
      console.error('[Watch] 麦克风操作失败:', e);
      // 清理半成品
      if (micTrack) { micTrack.close(); micTrack = null; }
      if (voiceClient) { voiceClient.leave().catch(() => {}); voiceClient = null; }
      micEnabled = false;
      btnMic.classList.remove('active');
      micOff.style.display = '';
      micOn.style.display  = 'none';
    } finally {
      btnMic.disabled = false;
    }
  });

  // ── 全屏（Shift+F10 进入/退出） ─────────────────────────
  function toggleFullscreen() {
    if (!document.fullscreenElement) {
      document.documentElement.requestFullscreen().then(() => {
        // 尝试锁定 ESC 键，防止误按退出全屏
        if (navigator.keyboard && navigator.keyboard.lock) {
          navigator.keyboard.lock(['Escape']).catch(() => {});
        }
      }).catch(() => {});
    } else {
      if (navigator.keyboard && navigator.keyboard.unlock) {
        navigator.keyboard.unlock();
      }
      document.exitFullscreen().catch(() => {});
    }
  }

  function updateFsIcon() {
    const isFs = !!document.fullscreenElement;
    fsExpand.style.display   = isFs ? 'none' : '';
    fsCompress.style.display = isFs ? '' : 'none';
    btnFs.title = isFs ? '退出全屏 (Shift+F10)' : '全屏 (Shift+F10)';
  }

  btnFs.addEventListener('click', toggleFullscreen);
  document.addEventListener('fullscreenchange', updateFsIcon);

  // Shift+F10 快捷键
  document.addEventListener('keydown', (e) => {
    if (e.shiftKey && e.key === 'F10') {
      e.preventDefault();
      toggleFullscreen();
    }
  });

  // ── 全屏时隐藏鼠标（3 秒无动静后隐藏） ─────────────────────
  let cursorTimer = null;
  document.addEventListener('mousemove', () => {
    if (!document.fullscreenElement) return;
    document.documentElement.classList.add('show-cursor');
    clearTimeout(cursorTimer);
    cursorTimer = setTimeout(() => {
      document.documentElement.classList.remove('show-cursor');
    }, 3000);
  });

})();

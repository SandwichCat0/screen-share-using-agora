/**
 * perf-sync.js — 屏幕共享期间的 perf_data 实时同步模块
 *
 * 推流端 (broadcaster): 轮询 perf_data 目录 → 检测新文件 → 上传 → 通知观看端
 * 观看端 (viewer):       接收通知 → 下载 + 修改 protobuf header → 写入本地目录
 *
 * 依赖: Bridge (bridge.js), AppConfig (bridge.js)
 */

// eslint-disable-next-line no-unused-vars
const PerfSync = (() => {
  // ── 常量 ──
  const POLL_INTERVAL_MS = 3000;               // 轮询间隔
  const RELAY_SERVER = ''; // rzqy 中转服务器
  const STORAGE_KEY_ENABLED = 'mdshare_perf_sync_enabled';
  const STORAGE_KEY_DIR = 'mdshare_perf_sync_dir';

  // ── 状态 ──
  let _enabled = localStorage.getItem(STORAGE_KEY_ENABLED) !== 'false'; // 默认启用
  let _mode = null;           // 'broadcaster' | 'viewer' | null
  let _dirPath = localStorage.getItem(STORAGE_KEY_DIR) || '';
  let _dirReady = false;      // 目录已就绪 (扫描完成且选定)
  let _scanning = false;      // 正在扫描中
  let _pollTimer = null;      // 轮询定时器 ID
  let _knownFiles = new Map(); // filename → {size, mtime}  (已知文件基线)
  let _uploadQueue = [];       // 待上传文件名队列
  let _uploading = false;      // 正在上传中
  let _hwInfo = null;          // 观看端本机硬件信息 (缓存)
  let _hwDetecting = false;
  let _pendingReceives = [];   // 待接收的文件 [{token, downloadKey, filename}]
  let _receiving = false;
  let _wsSend = null;          // WebSocket 发送函数 (由外部设置)
  let _awaitingDirScan = false; // 正在等待目录扫描结果 (替代 _realtimeSync 标记)
  let _baselineCallback = null; // 基线构建完成回调 (非 null 时处于基线模式)

  // ── 统计 ──
  let _stats = {
    uploaded: 0,
    downloaded: 0,
    errors: 0,
    lastError: '',
  };

  // ── 回调 ──
  let _onStatusChange = null;  // (status: object) => void
  let _onDirCandidates = null; // (candidates: string[]) => void   多目录选择

  // ═══════════════════════════════════════
  //  目录发现
  // ═══════════════════════════════════════

  /** 自动扫描 perf_data 目录 (注册表 → 全盘搜索) */
  function discoverDir() {
    if (_scanning) return;
    _scanning = true;
    _dirReady = false;
    _awaitingDirScan = true;
    _notifyStatus();
    Bridge.postMessage('perf-scan-files', {});
  }

  /** 全盘搜索 perf_data 目录 */
  function discoverDirFullScan() {
    if (_scanning) return;
    _scanning = true;
    _dirReady = false;
    _awaitingDirScan = true;
    _notifyStatus();
    Bridge.postMessage('perf-scan-drives', {});
  }

  /** 手动设置目录 */
  function setDir(path) {
    _dirPath = path;
    _dirReady = true;
    _scanning = false;
    localStorage.setItem(STORAGE_KEY_DIR, path);
    _notifyStatus();
  }

  /** 处理来自 C++ 的扫描结果 (data 已是解包后的 payload) */
  function _handleScanResult(data) {
    _scanning = false;
    _awaitingDirScan = false;
    if (data.success && data.targetPath) {
      _dirPath = data.targetPath;
      _dirReady = true;
      localStorage.setItem(STORAGE_KEY_DIR, _dirPath);
    } else {
      _dirReady = false;
    }
    _notifyStatus();
  }

  /** 处理全盘搜索结果 (多个候选目录) */
  function _handleDrivesScanResult(data) {
    _scanning = false;
    _awaitingDirScan = false;
    if (data.success && data.candidates && data.candidates.length > 0) {
      if (data.candidates.length === 1) {
        // 唯一候选, 继续扫描该目录确认
        _scanning = true;
        _awaitingDirScan = true;
        Bridge.postMessage('perf-scan-files', { path: data.candidates[0] });
      } else {
        // 多个候选, 通知 UI 让用户选择
        if (_onDirCandidates) _onDirCandidates(data.candidates);
      }
    } else {
      _dirReady = false;
    }
    _notifyStatus();
  }

  /** 选择候选目录后, 继续扫描 */
  function selectCandidate(dir) {
    _scanning = true;
    _awaitingDirScan = true;
    _notifyStatus();
    Bridge.postMessage('perf-scan-files', { path: dir });
  }

  // ═══════════════════════════════════════
  //  推流端 (Broadcaster) 逻辑
  // ═══════════════════════════════════════

  /** 开始推流端同步 */
  function startBroadcaster(wsSendFn) {
    _mode = 'broadcaster';
    _wsSend = wsSendFn;
    _knownFiles.clear();
    _uploadQueue = [];
    _stats = { uploaded: 0, downloaded: 0, errors: 0, lastError: '' };

    console.log('[PerfSync] startBroadcaster: enabled:', _enabled, 'dirReady:', _dirReady, 'dirPath:', _dirPath);

    if (!_enabled || !_dirReady) {
      console.log('[PerfSync] startBroadcaster 跳过: 未启用或目录未就绪');
      _notifyStatus();
      return;
    }

    // 先获取当前文件列表作为基线 (不上传已有文件)
    _buildBaseline(() => {
      console.log('[PerfSync] 基线构建完成, knownFiles:', _knownFiles.size, '开始轮询');
      _startPolling();
    });
  }

  /** 停止推流端同步 */
  function stopBroadcaster() {
    _stopPolling();
    _mode = null;
    _wsSend = null;
    _uploading = false;
    _uploadQueue = [];
    _notifyStatus();
  }

  /** 构建基线文件列表
   *  复用 handleBridgeMessage 中的 _handleRealtimeListResult 路由，
   *  通过 _baselineCallback 标记区分基线模式与正常轮询模式 */
  function _buildBaseline(callback) {
    _baselineCallback = callback || (() => {});
    Bridge.postMessage('perf-realtime-list', { path: _dirPath });
  }

  /** 启动 3s 轮询 */
  function _startPolling() {
    _stopPolling();
    _pollTimer = setInterval(_pollOnce, POLL_INTERVAL_MS);
    _pollOnce(); // 立即执行一次
  }

  function _stopPolling() {
    if (_pollTimer) {
      clearInterval(_pollTimer);
      _pollTimer = null;
    }
  }

  /** 单次轮询: 列出文件 → 检测新文件 → 排队上传 */
  function _pollOnce() {
    if (!_enabled || !_dirReady || _mode !== 'broadcaster') {
      console.log('[PerfSync] pollOnce 跳过: enabled:', _enabled, 'dirReady:', _dirReady, 'mode:', _mode);
      return;
    }
    Bridge.postMessage('perf-realtime-list', { path: _dirPath });
  }

  /** 处理轮询返回的文件列表 */
  function _handleRealtimeListResult(data) {
    console.log('[PerfSync] _handleRealtimeListResult:', 'success:', data.success, 'files:', data.files?.length, 'mode:', _mode, 'baseline:', !!_baselineCallback, 'knownFiles:', _knownFiles.size);
    if (!data.success || !data.files || _mode !== 'broadcaster') return;

    // 基线模式：记录所有已有文件但不上传，然后启动轮询
    if (_baselineCallback) {
      _knownFiles.clear();
      for (const f of data.files) {
        _knownFiles.set(f.name, { size: f.size, mtime: f.mtime });
      }
      const cb = _baselineCallback;
      _baselineCallback = null;
      cb();
      return;
    }

    // 正常轮询模式：检测新文件并上传
    const newFiles = [];
    for (const f of data.files) {
      if (!_knownFiles.has(f.name)) {
        newFiles.push(f.name);
        _knownFiles.set(f.name, { size: f.size, mtime: f.mtime });
      }
    }

    if (newFiles.length > 0) {
      _uploadQueue.push(...newFiles);
      _processUploadQueue();
    }
  }

  /** 依次上传队列中的文件 */
  function _processUploadQueue() {
    if (_uploading || _uploadQueue.length === 0) return;
    _uploading = true;

    const filename = _uploadQueue.shift();
    const filePath = _dirPath.replace(/[/\\]$/, '') + '\\' + filename;

    Bridge.postMessage('perf-realtime-upload', {
      serverUrl: RELAY_SERVER,
      filePath: filePath,
    });
  }

  /** 处理上传结果 */
  function _handleRealtimeUploadResult(data) {
    _uploading = false;
    if (data.success) {
      _stats.uploaded++;
      // 通过 WebSocket 通知所有观看端
      if (_wsSend) {
        _wsSend({
          type: 'perf-file-available',
          token: data.token,
          downloadKey: data.downloadKey,
          filename: data.filename,
          serverUrl: RELAY_SERVER,
        });
      }
    } else {
      _stats.errors++;
      _stats.lastError = data.errorMessage || 'Upload failed';
    }
    _notifyStatus();
    // 继续处理队列
    _processUploadQueue();
  }

  // ═══════════════════════════════════════
  //  观看端 (Viewer) 逻辑
  // ═══════════════════════════════════════

  /** 开始观看端同步 */
  function startViewer() {
    _mode = 'viewer';
    _pendingReceives = [];
    _stats = { uploaded: 0, downloaded: 0, errors: 0, lastError: '' };

    // 预先检测本机硬件 (缓存)
    if (!_hwInfo && !_hwDetecting) {
      _hwDetecting = true;
      Bridge.postMessage('perf-detect-hardware', {});
    }

    // 如果目录未就绪，自动触发扫描 (确保观看端能接收文件)
    if (!_dirReady && !_scanning) {
      if (_dirPath) {
        // 有上次记忆的目录，验证是否仍有效
        _scanning = true;
        _awaitingDirScan = true;
        Bridge.postMessage('perf-scan-files', { path: _dirPath });
      } else {
        // 无记忆目录，自动扫描
        discoverDir();
      }
    }

    _notifyStatus();
  }

  /** 停止观看端同步 */
  function stopViewer() {
    _mode = null;
    _receiving = false;
    _pendingReceives = [];
    _notifyStatus();
  }

  /** 收到推流端发来的文件通知 (via WebSocket) */
  function onFileAvailable(msg) {
    if (!_enabled || !_dirReady || _mode !== 'viewer') return;

    _pendingReceives.push({
      token: msg.token,
      downloadKey: msg.downloadKey,
      filename: msg.filename,
      serverUrl: msg.serverUrl || RELAY_SERVER,
    });
    _processReceiveQueue();
  }

  /** 依次处理下载队列 */
  function _processReceiveQueue() {
    if (_receiving || _pendingReceives.length === 0) return;
    if (!_dirReady) return;
    _receiving = true;

    const item = _pendingReceives.shift();
    Bridge.postMessage('perf-realtime-receive', {
      serverUrl: item.serverUrl,
      token: item.token,
      downloadKey: item.downloadKey,
      filename: item.filename,
      targetDir: _dirPath,
      hwInfo: _hwInfo || {},
    });
  }

  /** 处理下载+应用结果 */
  function _handleRealtimeReceiveResult(data) {
    _receiving = false;
    if (data.success) {
      _stats.downloaded++;
    } else {
      _stats.errors++;
      _stats.lastError = data.errorMessage || 'Download failed';
    }
    _notifyStatus();
    _processReceiveQueue();
  }

  /** 处理硬件检测结果 */
  function _handleHardwareResult(data) {
    _hwDetecting = false;
    if (data.success) {
      _hwInfo = {
        gpuVendor: data.gpuVendor || '',
        gpuModel: data.gpuModel || '',
        driverVersion: data.driverVersion || '',
        cpuModel: data.cpuModel || '',
        macAddress: data.macAddress || '',
        deviceId: data.deviceId || '',
        hardwareId1: data.hardwareId1 || '',
        hardwareId2: data.hardwareId2 || '',
        osVersion: data.osVersion || '',
      };
    }
  }

  // ═══════════════════════════════════════
  //  Bridge 消息统一路由
  // ═══════════════════════════════════════

  /** 注册到 Bridge.onMessage 的全局处理器
   *  C++ 通过 MakeMessage 发送 {type, data:{...}}, 需要解包 data */
  function handleBridgeMessage(raw) {
    let msg;
    try { msg = typeof raw === 'string' ? JSON.parse(raw) : raw; } catch { return; }
    const d = msg.data || msg; // 解包: MakeMessage 包装 → {type, data:{...}}

    // 调试: 打印所有 perf 相关消息
    if (msg.type && msg.type.startsWith('perf-')) {
      console.log('[PerfSync] 收到消息:', msg.type, 'mode:', _mode, 'dirReady:', _dirReady, 'enabled:', _enabled, 'baseline:', !!_baselineCallback, 'data:', JSON.stringify(d).slice(0, 200));
    }

    switch (msg.type) {
      case 'perf-scan-result':
        // 仅当 PerfSync 正在等待扫描结果时处理, 避免与日志迁移面板冲突
        if (_awaitingDirScan) _handleScanResult(d);
        break;
      case 'perf-scan-drives-result':
        if (_awaitingDirScan) _handleDrivesScanResult(d);
        break;
      case 'perf-realtime-list-result':
        _handleRealtimeListResult(d);
        break;
      case 'perf-realtime-upload-result':
        _handleRealtimeUploadResult(d);
        break;
      case 'perf-realtime-receive-result':
        _handleRealtimeReceiveResult(d);
        break;
      case 'perf-hardware-result':
        // 仅在 viewer 模式且无缓存时处理
        if (_mode === 'viewer' && !_hwInfo) _handleHardwareResult(d);
        break;
    }
  }

  // ═══════════════════════════════════════
  //  状态与 UI 通知
  // ═══════════════════════════════════════

  function _notifyStatus() {
    if (_onStatusChange) _onStatusChange(getStatus());
  }

  function getStatus() {
    return {
      enabled: _enabled,
      mode: _mode,
      dirPath: _dirPath,
      dirReady: _dirReady,
      scanning: _scanning,
      polling: !!_pollTimer,
      uploading: _uploading,
      receiving: _receiving,
      stats: { ..._stats },
      pendingUploads: _uploadQueue.length,
      pendingDownloads: _pendingReceives.length,
    };
  }

  function setEnabled(v) {
    _enabled = !!v;
    localStorage.setItem(STORAGE_KEY_ENABLED, _enabled ? 'true' : 'false');
    if (!_enabled) {
      _stopPolling();
    } else if (_mode === 'broadcaster' && _dirReady) {
      _buildBaseline(() => _startPolling());
    }
    _notifyStatus();
  }

  function onStatusChange(fn) { _onStatusChange = fn; }
  function onDirCandidates(fn) { _onDirCandidates = fn; }

  // ═══════════════════════════════════════
  //  初始化
  // ═══════════════════════════════════════

  /** 初始化: 注册 Bridge 监听 + 自动扫描目录 */
  function init() {
    Bridge.onMessage(handleBridgeMessage);
    // 如果有记忆的目录, 验证是否仍有效
    if (_dirPath) {
      _scanning = true;
      _awaitingDirScan = true;
      Bridge.postMessage('perf-scan-files', { path: _dirPath });
    }
  }

  // ═══════════════════════════════════════
  //  公开 API
  // ═══════════════════════════════════════

  return {
    init,
    // 目录
    discoverDir,
    discoverDirFullScan,
    selectCandidate,
    setDir,
    // 模式控制
    startBroadcaster,
    stopBroadcaster,
    startViewer,
    stopViewer,
    // 观看端文件通知
    onFileAvailable,
    // 设置
    setEnabled,
    getStatus,
    // 回调
    onStatusChange,
    onDirCandidates,
  };
})();

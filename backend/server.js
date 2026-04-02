require('dotenv').config();
const path     = require('path');
const express  = require('express');
const cors     = require('cors');
const http     = require('http');
const crypto   = require('crypto');
const bcrypt   = require('bcrypt');
const jwt      = require('jsonwebtoken');
const { RtcTokenBuilder, RtcRole } = require('agora-token');

const {
  validateCard, activateCard, upsertDevice, createSession, verifySession,
  getAdminByUsername, createAdmin,
  listCards, getCardById, createCard, updateCard, renewCard, deleteCard,
  listActiveSessions, revokeSession, listDevices,
  hashKey, now,
  AGORA_MULTIPLIERS, getQuotaInfo, deductQuota, hasQuota, rechargeQuota,
  addUsageLog, getUsageLogs,
  getBillingConfig, saveBillingConfig, getEffectiveMultipliers, DEFAULT_AGORA_MULTIPLIERS,
} = require('./db');

const app    = express();
app.set('trust proxy', false);  // 直连模式，无反向代理
const server = http.createServer(app);

const {
  APP_ID, APP_CERTIFICATE,
  PORT          = 3000,
  TOKEN_EXPIRE  = 86400,
  CORS_ORIGINS  = 'https://mdshare.local',
  JWT_SECRET,
  ADMIN_USERNAME = 'admin',
  ADMIN_PASSWORD,
} = process.env;

// ============================================================
// 启动检查
// ============================================================
if (!APP_ID || !APP_CERTIFICATE || APP_ID === 'your_agora_app_id_here') {
  console.error('❌ 请在 .env 文件中配置 APP_ID 和 APP_CERTIFICATE');
  process.exit(1);
}
if (!JWT_SECRET) {
  console.error('❌ 请在 .env 文件中配置 JWT_SECRET（至少64字节随机字符串）');
  process.exit(1);
}

// 首次启动自动创建管理员账户
(async () => {
  const existing = getAdminByUsername(ADMIN_USERNAME);
  if (!existing && ADMIN_PASSWORD) {
    const hash = await bcrypt.hash(ADMIN_PASSWORD, 12);
    createAdmin(ADMIN_USERNAME, hash);
    console.log(`✅ 管理员账户已创建: ${ADMIN_USERNAME}`);
  }
})();

// ============================================================
// 安全中间件
// ============================================================
const helmet = require('helmet');

// /admin/* — 管理后台使用内联脚本，CSP 单独关闭
const helmetAdmin = helmet({ contentSecurityPolicy: false, crossOriginEmbedderPolicy: false });
// 其余所有路由启用完整 CSP
const helmetApp   = helmet({
  contentSecurityPolicy: {
    directives: {
      defaultSrc:  ["'self'"],
      scriptSrc:   ["'self'", "https://download.agora.io"],
      connectSrc:  ["'self'", "wss:", "ws:", "https:"],
      imgSrc:      ["'self'", "data:", "blob:"],
      mediaSrc:    ["'self'", "blob:"],
      fontSrc:     ["'self'", "https://cdnjs.cloudflare.com"],
    }
  },
  crossOriginEmbedderPolicy: false,
});
// /watch  — 公开观看端页面，需要从 download.agora.io 加载 SDK
const helmetViewer = helmet({
  contentSecurityPolicy: {
    directives: {
      defaultSrc:  ["'self'"],
      scriptSrc:   ["'self'", "https://download.agora.io"],
      connectSrc:  ["'self'", "wss:", "ws:", "https:"],
      imgSrc:      ["'self'", "data:"],
      mediaSrc:    ["'self'", "blob:"],
      workerSrc:   ["blob:"],   // Agora Web SDK 内部使用 blob: worker
    }
  },
  crossOriginEmbedderPolicy: false,
});
// /client* (web前端) — 需要 CDN 外部资源（tailwindcss, font-awesome, agora SDK）
const helmetClient = helmet({
  contentSecurityPolicy: {
    directives: {
      defaultSrc:  ["'self'"],
      scriptSrc:   ["'self'", "'unsafe-inline'", "https://cdn.tailwindcss.com", "https://download.agora.io"],
      connectSrc:  ["'self'", "wss:", "ws:", "https:"],
      imgSrc:      ["'self'", "data:", "blob:"],
      mediaSrc:    ["'self'", "blob:"],
      fontSrc:     ["'self'", "https://cdnjs.cloudflare.com"],
      styleSrc:    ["'self'", "'unsafe-inline'", "https://cdnjs.cloudflare.com", "https://cdn.tailwindcss.com"],
      workerSrc:   ["blob:"],
    }
  },
  crossOriginEmbedderPolicy: false,
});
app.use((req, res, next) => {
  if (req.path.startsWith('/admin'))                  return helmetAdmin(req, res, next);
  if (req.path === '/watch' || req.path === '/watch.js') return helmetViewer(req, res, next);
  // web前端静态资源（index.html, js/, css/）
  if (req.path === '/' || req.path.startsWith('/js/') || req.path.startsWith('/css/') || req.path.endsWith('.html'))
    return helmetClient(req, res, next);
  helmetApp(req, res, next);
});

const allowedOrigins = CORS_ORIGINS.split(',').map(s => s.trim()).filter(Boolean);

// CORS 只对 /api/ 客户端路由生效（跨域：mdshare.local → 38.76.209.172:3000）
// /admin/api/ 是同域请求，不需要 CORS
const corsMiddleware = cors({
  origin: (origin, cb) => {
    // 无 origin（服务器端直调 / 同域导航） → 放行
    if (!origin || allowedOrigins.includes(origin)) return cb(null, true);
    // 同域请求（公开观看页 /watch 发起的 fetch），origin 与服务自身域名匹配 → 放行
    // Express req 不可用于此回调，所以直接比较已知部署域名
    if (origin === 'http://38.76.209.172:3000' || origin === 'https://mdshare.local' || origin === 'http://mdshare.local') return cb(null, true);
    cb(new Error('CORS blocked'));
  },
  credentials: true,
  methods: ['GET', 'POST', 'PATCH', 'DELETE'],
  maxAge: 600,
});
app.use('/api/', corsMiddleware);

app.use(express.json({ limit: '64kb' }));

// ============================================================
// 速率限制
// ============================================================
const rateLimit = require('express-rate-limit');

// 通用 API 限制
const apiLimiter = rateLimit({
  windowMs: 60_000, max: 120,
  standardHeaders: true, legacyHeaders: false,
  message: { success: false, error: '请求过于频繁，请稍后重试' },
});
app.use('/api/', apiLimiter);

// 登录端点更严格：每 IP 每分钟 10 次
const authLimiter = rateLimit({
  windowMs: 60_000, max: 10,
  standardHeaders: true, legacyHeaders: false,
  message: { success: false, error: '登录尝试过于频繁，请稍后重试' },
});
// 公开观看端 token：每 IP 每分钟 60 次（独立于需鉴权的 apiLimiter）
const viewerLimiter = rateLimit({
  windowMs: 60_000, max: 60,
  standardHeaders: true, legacyHeaders: false,
  message: { success: false, error: '请求过于频繁，请稍后重试' },
});

// ============================================================
// 鉴权中间件
// ============================================================

/** 从 Cookie 字符串中提取指定名称的值（无需 cookie-parser） */
function parseCookie(req, name) {
  const header = req.headers.cookie || '';
  const entry  = header.split(';').map(c => c.trim()).find(c => c.startsWith(name + '='));
  return entry ? decodeURIComponent(entry.slice(name.length + 1)) : null;
}

/** Bearer session token 鉴权（客户端 API） */
function apiAuth(req, res, next) {
  const authHeader = req.headers['authorization'] || '';
  const token = authHeader.startsWith('Bearer ') ? authHeader.slice(7) : null;
  if (!token) return res.status(401).json({ success: false, error: '未授权' });

  const session = verifySession(token);
  if (!session) return res.status(401).json({ success: false, error: '会话无效或已过期，请重新登录' });

  req.session = session;
  next();
}

/** 管理员 JWT 鉴权 — 从 HttpOnly Cookie 读取，JS 无法访问 */
function adminAuth(req, res, next) {
  const token = parseCookie(req, 'mdshare_admin');
  if (!token) return res.status(401).json({ success: false, error: '未授权' });

  try {
    req.admin = jwt.verify(token, JWT_SECRET);
    next();
  } catch {
    return res.status(401).json({ success: false, error: 'Token 无效或已过期' });
  }
}

// ============================================================
// 客户端 Web 前端静态文件（web/ 目录）
// ============================================================
app.use(express.static(path.join(__dirname, '..', 'web'), {
  index: 'index.html',
  maxAge: '1h',
}));

// ============================================================
// 管理员后台静态文件（仅 backend/admin/ 目录）
// ============================================================
app.use('/admin', express.static(path.join(__dirname, 'admin')));

// ============================================================
// 公开观看端页面（无需鉴权）
// ============================================================
/** GET /watch  — 返回观看端 HTML */
app.get('/watch', (_req, res) => res.sendFile(path.join(__dirname, 'viewer.html')));
/** GET /watch.js — 观看端前端逻辑脚本 */
app.get('/watch.js', (_req, res) => res.sendFile(path.join(__dirname, 'watch.js')));

// ============================================================
// WebSocket（原生 ws，零依赖）
// ============================================================
const WebSocket = require('ws');
const wss = new WebSocket.Server({ noServer: true });
/** @type {Map<string, Set<WebSocket>>} channelId → 已连接的 WebSocket 集合 */
const wsRooms = new Map();

/** 向频道内所有连接广播（可排除发送者） */
function wsBroadcast(channelId, data, excludeWs) {
  const room = wsRooms.get(channelId);
  if (!room) return;
  const msg = typeof data === 'string' ? data : JSON.stringify(data);
  for (const ws of room) {
    if (ws !== excludeWs && ws.readyState === WebSocket.OPEN) ws.send(msg);
  }
}
/** 向频道内指定角色广播 */
function wsBroadcastToRole(channelId, role, data, excludeWs) {
  const room = wsRooms.get(channelId);
  if (!room) return;
  const msg = typeof data === 'string' ? data : JSON.stringify(data);
  for (const ws of room) {
    if (ws !== excludeWs && ws._wsRole === role && ws.readyState === WebSocket.OPEN) ws.send(msg);
  }
}

// HTTP Upgrade → WebSocket 握手（URL query 参数鉴权）
server.on('upgrade', (req, socket, head) => {
  let url;
  try { url = new URL(req.url, 'http://localhost'); } catch { socket.destroy(); return; }
  if (url.pathname !== '/ws') { socket.destroy(); return; }

  // CORS
  const origin = req.headers.origin || '';
  if (origin && !allowedOrigins.includes(origin)) {
    console.log(`[WS] CORS 拒绝: origin=${origin}`);
    socket.write('HTTP/1.1 403 Forbidden\r\n\r\n'); socket.destroy(); return;
  }
  // 鉴权
  const token = url.searchParams.get('token') || '';
  const session = verifySession(token);
  if (!session) {
    console.log(`[WS] 鉴权失败: token=${token.slice(0,8)}...`);
    socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n'); socket.destroy(); return;
  }

  const channel = (url.searchParams.get('channel') || '').slice(0, 64);
  const role = url.searchParams.get('role') || '';
  console.log(`[WS] 升级成功: channel=${channel}, role=${role}, cardId=${session.cardId}`);

  wss.handleUpgrade(req, socket, head, (ws) => {
    ws._session  = session;
    ws._token    = token;
    ws._channelId = channel;
    ws._wsRole   = role;  // broadcaster | viewer
    ws._rcChannelId = null;
    ws._rcRole      = null;
    wss.emit('connection', ws);
  });
});
console.log('✅ WebSocket (原生 ws) 已启用');

// ============================================================
// 工具函数
// ============================================================
const MAX_CHANNELS  = 5000;
const MAX_CHANSTATS = 5000;

const channels  = new Map();
const chanStats = new Map();

function generateChannelId() {
  return crypto.randomBytes(6).toString('hex');
}

function buildAgoraToken(channelName, uid, role) {
  const expireTs = Math.floor(Date.now() / 1000) + Number(TOKEN_EXPIRE);
  return RtcTokenBuilder.buildTokenWithUid(
    APP_ID, APP_CERTIFICATE, channelName, uid, role, expireTs, expireTs
  );
}

// 频道加入限速：单 IP 每分钟 30 次，防止枚举频道号
const joinLimiter = rateLimit({
  windowMs: 60_000, max: 30,
  standardHeaders: true, legacyHeaders: false,
  message: { success: false, error: '加入请求过于频繁，请稍后重试' },
});

// 内容安全工具
const CHANNEL_ID_RE = /^[0-9a-f]{12}$/i;  // generateChannelId() 输出格式

// 统计字段白名单（值必须为 finite number），防止任意字段注入撑爆内存
const STATS_ALLOWED_FIELDS = new Set([
  'rttMs', 'sendBitrateKbps', 'recvBitrateKbps',
  'packetLossRate', 'sendFrameRate', 'recvFrameRate', 'e2eDelayMs',
]);

// 每张卡同时可存活的最大频道数，防止单用户填满全局池
const MAX_CHANNELS_PER_CARD = 3;

// TTL 清理（内存存储）
setInterval(() => {
  const CHANNEL_TTL = 2 * 60 * 60 * 1000;
  const t = Date.now();
  for (const [k, v] of channels) {
    if (t - v.created > CHANNEL_TTL) {
      _flushChannelBilling(k, v);
      channels.delete(k);
    }
  }
  for (const [k, v] of chanStats) {
    const lu = Math.max(v.publisher?.updatedAt || 0, v.subscriber?.updatedAt || 0);
    if (t - lu > CHANNEL_TTL) chanStats.delete(k);
  }
}, 5 * 60 * 1000);

/**
 * 将频道的内存中累计计费写入数据库 + 生成 usage_log
 * 在频道销毁、TTL 清理时调用
 */
function _flushChannelBilling(channelId, channel) {
  if (!channel || !channel.billing) return;
  const billing = channel.billing;
  const remaining = billing.pubAccumulated + billing.viewAccumulated;
  if (remaining > 0) {
    deductQuota(channel.cardId, remaining);
    billing.pubFlushed     += billing.pubAccumulated;
    billing.viewFlushed    += billing.viewAccumulated;
    billing.totalFlushed   += remaining;
    billing.pubAccumulated  = 0;
    billing.viewAccumulated = 0;
  }
  // 写入用量日志（始终写入，方便管理员审计频道记录）
  const durationSec = (Date.now() - channel.created) / 1000;
  addUsageLog({
    cardId:         channel.cardId,
    channelId,
    resolution:     channel.resolution,
    startedAt:      Math.floor(channel.created / 1000),
    endedAt:        now(),
    durationSec:    Math.round(durationSec),
    pubStdMinutes:  parseFloat((billing.pubFlushed  || 0).toFixed(2)),
    viewStdMinutes: parseFloat((billing.viewFlushed || 0).toFixed(2)),
    totalStdMinutes: parseFloat((billing.totalFlushed || 0).toFixed(2)),
    peakViewers:    billing.peakViewers,
  });
}

// ============================================================
// 卡密登录端点（无需鉴权）
// ============================================================

/**
 * POST /api/auth/login
 * body: { cardKey, deviceId, deviceName? }
 * 返回: { sessionToken, expireAt, appId }
 */
app.post('/api/auth/login', authLimiter, (req, res) => {
  const { cardKey, deviceId } = req.body;
  // deviceName 截断为 128 字符，防止超长字符串写入数据库
  const deviceName = typeof req.body.deviceName === 'string' ? req.body.deviceName.slice(0, 128) : '';
  const ip = req.ip || req.socket?.remoteAddress || '';

  if (!cardKey || typeof cardKey !== 'string' || cardKey.length > 64) {
    return res.status(400).json({ success: false, error: '卡密格式错误' });
  }
  if (!deviceId || typeof deviceId !== 'string' || deviceId.length > 128) {
    return res.status(400).json({ success: false, error: '设备ID缺失' });
  }

  const result = validateCard(cardKey, deviceId);

  if (result.error === 'invalid')       return res.status(401).json({ success: false, error: '卡密无效' });
  if (result.error === 'banned')        return res.status(403).json({ success: false, error: '卡密已被封禁' });
  if (result.error === 'expired')       return res.status(403).json({ success: false, error: '卡密已过期' });
  if (result.error === 'quota_exceeded') return res.status(403).json({ success: false, error: '配额已用尽，请联系管理员充值' });
  if (result.error === 'device_limit')  return res.status(403).json({ success: false, error: `设备数量已达上限（最多 ${result.card?.max_devices ?? 1} 台）` });

  const { card } = result;

  // 首次激活：设置过期时间
  activateCard(card.id);

  // 更新/记录设备
  upsertDevice(card.id, deviceId, deviceName, ip);

  // 取最新的 expires_at（activateCard 可能刚写入）
  const freshCard = getCardById(card.id);
  const sessionExpireTs = freshCard.expires_at || (now() + Number(TOKEN_EXPIRE));

  // 创建会话 token
  const sessionToken = createSession(card.id, deviceId, sessionExpireTs);

  console.log(`🔑 卡密登录: ${card.key_display} / 设备: ${deviceId.slice(0, 8)}... / IP: ${ip}`);

  res.json({
    success: true,
    data: {
      sessionToken,
      expireAt: sessionExpireTs,
    },
  });
});

/** POST /api/auth/logout — 注销会话（token 在 Bearer 头中，无需 apiAuth） */
app.post('/api/auth/logout', (req, res) => {
  const authHeader = req.headers['authorization'] || '';
  const token = authHeader.startsWith('Bearer ') ? authHeader.slice(7) : null;
  if (token) revokeSession(token);
  res.json({ success: true });
});

/** POST /admin/api/logout — 管理员退出，清除 Cookie */
app.post('/admin/api/logout', (req, res) => {
  res.clearCookie('mdshare_admin', { path: '/admin' });
  res.json({ success: true });
});

// ============================================================
// 公开观看端 Token 接口（放在 apiAuth 之前，无需卡密鉴权）
// ============================================================
/**
 * GET /api/viewer/token?channel=<channelId>
 * 任何浏览器均可调用；频道必须在内存中存在（推流者在线）
 */
app.get('/api/viewer/token', viewerLimiter, async (req, res) => {
  const { channel } = req.query;
  if (!channel || !CHANNEL_ID_RE.test(channel)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  const ch = channels.get(channel);
  if (!ch) {
    return res.status(404).json({ success: false, error: '频道不存在或共享已结束' });
  }
  const channelName = ch.channelName;
  const uid = Math.floor(Math.random() * 2147483647) + 1;
  const token = buildAgoraToken(channelName, uid, RtcRole.SUBSCRIBER);

  const viewerEntry = {
    uid: `v_${Date.now()}`,
    joined: Date.now(),
  };
  if (ch.viewers.length >= 200) ch.viewers.shift();
  ch.viewers.push(viewerEntry);
  console.log(`👁️  浏览器观看者加入: ${channel}`);
  res.json({
    success: true,
    data: {
      appId: APP_ID,
      channelName,
      uid,
      token,
    },
  });
});

/**
 * POST /api/viewer/voice-token — 公开观看端语音覆盖频道凭据（无需鉴权）
 * 浏览器观看端使用此接口获取 PUBLISHER 权限 token
 */
app.post('/api/viewer/voice-token', viewerLimiter, (req, res) => {
  const { channelId } = req.body || {};
  if (!channelId || !CHANNEL_ID_RE.test(channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  if (!channels.has(channelId)) {
    return res.status(404).json({ success: false, error: '频道不存在' });
  }

  const voiceChannelName = `voice-${channelId}`;
  const uid = Math.floor(Math.random() * 2147483647) + 1;
  const token = buildAgoraToken(voiceChannelName, uid, RtcRole.PUBLISHER);

  res.json({
    success: true,
    data: {
      appId: APP_ID,
      channelName: voiceChannelName,
      token,
      uid,
    },
  });
});

// 所有 /api/ 路由之后都需要鉴权
app.use('/api/', apiAuth);

// ============================================================
// API 路由
// ============================================================

/** GET /api/status — 服务状态（现在需要鉴权） */
app.get('/api/status', (_req, res) => {
  res.json({
    success: true,
    data: {
      activeChannels:    channels.size,
      registeredDevices: 0,
      uptime:            process.uptime(),
    },
  });
});

/** POST /api/channel/create — 创建频道 */
app.post('/api/channel/create', async (req, res) => {

  if (channels.size >= MAX_CHANNELS) {
    return res.status(503).json({ success: false, error: '频道数已达上限' });
  }
  if (!hasQuota(req.session.cardId)) {
    return res.status(403).json({ success: false, error: '配额已用尽，无法开始共享' });
  }
  const cardChannelCount = [...channels.values()].filter(c => c.cardId === req.session.cardId).length;
  if (cardChannelCount >= MAX_CHANNELS_PER_CARD) {
    return res.status(429).json({ success: false, error: `单卡同时频道上限 ${MAX_CHANNELS_PER_CARD} 个` });
  }

  const resolution = ['720p', '1080p', '2k'].includes(req.body.resolution)
    ? req.body.resolution : '1080p';

  const channelId = generateChannelId();

  const channelName = channelId;
  const uid = Math.floor(Math.random() * 2147483647) + 1;
  const token = buildAgoraToken(channelName, uid, RtcRole.PUBLISHER);

  channels.set(channelId, {
    created:     Date.now(),
    channelName,
    deviceUid:   uid,
    viewers:     [],
    cardId:      req.session.cardId,
    resolution,
    billing: {
      lastTickMs:      Date.now(),
      lastFlushMs:     Date.now(),
      pubAccumulated:  0,
      viewAccumulated: 0,
      totalFlushed:    0,
      pubFlushed:      0,
      viewFlushed:     0,
      peakViewers:     0,
      currentViewers:  0,
    },
  });

  console.log(`✅ 频道创建: ${channelId}`);

  res.json({
    success: true,
    data: {
      channelId,
      channelName,
      uid,
      token,
      appId: APP_ID,
      expireTime: Number(TOKEN_EXPIRE),
    },
  });
});

/** POST /api/channel/join — 加入频道（观看端） */
app.post('/api/channel/join', joinLimiter, async (req, res) => {
  const { channelId } = req.body;
  if (!channelId || !CHANNEL_ID_RE.test(channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  if (!channels.has(channelId)) {
    return res.status(404).json({ success: false, error: '频道不存在' });
  }
  const channel = channels.get(channelId);

  const channelName = channel.channelName;
  const uid = Math.floor(Math.random() * 2147483647) + 1;
  const token = buildAgoraToken(channelName, uid, RtcRole.SUBSCRIBER);

  if (channel.viewers.length >= 200) channel.viewers.shift();
  channel.viewers.push({ uid: `v_${Date.now()}`, joined: Date.now() });

  console.log(`👁️ 观看者加入: ${channelId}`);

  res.json({
    success: true,
    data: {
      channelId,
      channelName,
      uid,
      token,
      appId: APP_ID,
      expireTime: Number(TOKEN_EXPIRE),
    },
  });
});

/**
 * POST /api/channel/:channelId/voice-token — 获取独立语音覆盖频道凭据
 *
 * Go-Live Agora token 为 audience 权限，不允许 viewer publish。
 * 此接口使用我们自有的 Agora App ID 生成 PUBLISHER token，
 * 让 viewer 和 broadcaster 在独立的语音频道中双向通话。
 * 频道名: voice-{channelId}  UID: 随机生成
 */
app.post('/api/channel/:channelId/voice-token', (req, res) => {
  const { channelId } = req.params;
  if (!CHANNEL_ID_RE.test(channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  if (!channels.has(channelId)) {
    return res.status(404).json({ success: false, error: '频道不存在' });
  }

  const voiceChannelName = `voice-${channelId}`;
  // 为每位请求者生成唯一 UID (1 ~ 2^31)
  const uid = Math.floor(Math.random() * 2147483647) + 1;
  const token = buildAgoraToken(voiceChannelName, uid, RtcRole.PUBLISHER);

  res.json({
    success: true,
    data: {
      appId: APP_ID,
      channelName: voiceChannelName,
      token,
      uid,
    },
  });
});

/**
 * POST /api/channel/:channelId/leave — 观看端离开频道（释放观众资源）
 */
app.post('/api/channel/:channelId/leave', (req, res) => {
  const { channelId } = req.params;
  if (!CHANNEL_ID_RE.test(channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  const channel = channels.get(channelId);
  if (!channel) {
    return res.status(404).json({ success: false, error: '频道不存在' });
  }
  console.log(`👋 观看者离开: channelId=${channelId}`);
  res.json({ success: true });
});

/** POST /api/channel/:channelId/destroy — 销毁频道 */
app.post('/api/channel/:channelId/destroy', (req, res) => {
  const { channelId } = req.params;
  if (!CHANNEL_ID_RE.test(channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  const channel = channels.get(channelId);
  if (!channel) return res.json({ success: true }); // 幂等
  if (channel.cardId !== req.session.cardId) {
    return res.status(403).json({ success: false, error: '无权销毁此频道' });
  }

  _flushChannelBilling(channelId, channel);
  channels.delete(channelId);
  chanStats.delete(channelId);

  console.log(`🗑️ 频道销毁: ${channelId}`);
  res.json({ success: true });
});

/** POST /api/channel/:channelId/stats — 上报统计 */
app.post('/api/channel/:channelId/stats', (req, res) => {
  const { channelId } = req.params;

  // 频道 ID 格式校验
  if (!CHANNEL_ID_RE.test(channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }

  // 频道必须在内存中存在，防止为幽灵频道预填充 chanStats slot（内存 DoS）
  if (!channels.has(channelId)) {
    return res.status(404).json({ success: false, error: '频道不存在' });
  }

  const channel = channels.get(channelId);
  const { role } = req.body;

  // 严格白名单过滤：只接受已知数值字段，防止任意字段注入撑爆内存
  const metrics = {};
  for (const key of STATS_ALLOWED_FIELDS) {
    const v = req.body[key];
    if (v !== undefined && typeof v === 'number' && Number.isFinite(v)) {
      metrics[key] = v;
    }
  }

  // ── 配额计费（仅 publisher 上报时触发）─────────────────────
  let quotaResponse = null;

  // 计费仅对频道所有者的 publisher 上报生效，防止他人加速扣费
  if (role === 'publisher' && channel && channel.billing && channel.cardId === req.session.cardId) {
    const nowMs   = Date.now();
    const billing = channel.billing;

    // 观众数由服务端计算（不信任客户端上报，防篡改）
    const viewerCount = channel.viewers ? channel.viewers.length : 0;
    billing.currentViewers = viewerCount;
    if (viewerCount > billing.peakViewers) billing.peakViewers = viewerCount;

    // 计算自上次 tick 以来的标准分钟消耗（使用可配置倍率）
    const elapsedMin  = (nowMs - billing.lastTickMs) / 60000;
    const effectiveMult = getEffectiveMultipliers();
    const mult        = effectiveMult[channel.resolution] || effectiveMult['1080p'];
    const pubCost     = elapsedMin * mult.publisher;
    const viewCost    = elapsedMin * mult.viewer * viewerCount;
    billing.pubAccumulated  += pubCost;
    billing.viewAccumulated += viewCost;
    billing.lastTickMs = nowMs;

    // 每 30 秒 flush 一次到数据库（避免高频写入）
    const FLUSH_INTERVAL_MS = 30_000;
    if (nowMs - billing.lastFlushMs >= FLUSH_INTERVAL_MS) {
      const toFlush = billing.pubAccumulated + billing.viewAccumulated;
      if (toFlush > 0) {
        deductQuota(channel.cardId, toFlush);
        billing.pubFlushed     += billing.pubAccumulated;
        billing.viewFlushed    += billing.viewAccumulated;
        billing.totalFlushed   += toFlush;
        billing.pubAccumulated  = 0;
        billing.viewAccumulated = 0;
        billing.lastFlushMs     = nowMs;
      }
    }

    const qi = getQuotaInfo(channel.cardId);
    if (qi) {
      const unflushed = billing.pubAccumulated + billing.viewAccumulated;
      const realUsed = qi.usedMinutes + unflushed;
      const exceeded = realUsed >= qi.quotaMinutes;
      const remaining = Math.max(0, qi.quotaMinutes - realUsed);
      quotaResponse = {
        exceeded,
        totalMinutes:     qi.quotaMinutes,
        usedMinutes:      Math.round(realUsed * 100) / 100,
        remainingMinutes: Math.round(remaining * 100) / 100,
      };
    }
  }

  if (!chanStats.has(channelId)) {
    if (chanStats.size >= MAX_CHANSTATS) {
      return res.status(503).json({ success: false, error: '统计存储已满' });
    }
    chanStats.set(channelId, { publisher: {}, subscriber: {} });
  }

  const entry  = chanStats.get(channelId);
  const field  = role === 'publisher' ? 'publisher' : 'subscriber';
  entry[field] = { ...metrics, updatedAt: Date.now() };

  // 推送频道统计给 WebSocket 连接
  wsBroadcast(channelId, {
    type: 'channel-stats', channelId,
    publisher: entry.publisher, subscriber: entry.subscriber,
  });

  // 返回观众数 + 观众端统计（供主播状态栏展示）
  const viewerCount = channel ? (channel.viewers?.length ?? 0) : 0;
  res.json({ success: true, quota: quotaResponse, viewerCount, subscriberStats: entry.subscriber || {} });
});

/** GET /api/quota — 查询当前卡密配额 */
app.get('/api/quota', (req, res) => {
  const quota = getQuotaInfo(req.session.cardId);
  if (!quota) return res.status(404).json({ success: false, error: '卡密不存在' });
  res.json({
    success: true,
    data: {
      exceeded:         quota.remainingMinutes <= 0,
      totalMinutes:     quota.quotaMinutes,
      usedMinutes:      Math.round(quota.usedMinutes * 100) / 100,
      remainingMinutes: Math.round(Math.max(0, quota.remainingMinutes) * 100) / 100,
    },
  });
});

/** GET /api/channel/:channelId/stats — 查询统计 */
app.get('/api/channel/:channelId/stats', (req, res) => {
  if (!CHANNEL_ID_RE.test(req.params.channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  // 频道归属校验
  const ch = channels.get(req.params.channelId);
  if (!ch || ch.cardId !== req.session.cardId) {
    return res.status(403).json({ success: false, error: '无权查看此频道' });
  }
  const entry = chanStats.get(req.params.channelId) || { publisher: {}, subscriber: {} };
  res.json({ success: true, data: entry });
});

/** GET /api/channel/:channelId/users — 用户列表 */
app.get('/api/channel/:channelId/users', (req, res) => {
  if (!CHANNEL_ID_RE.test(req.params.channelId)) {
    return res.status(400).json({ success: false, error: '频道号格式错误' });
  }
  const channel = channels.get(req.params.channelId);
  if (!channel) return res.status(404).json({ success: false, error: '频道不存在' });
  res.json({
    success: true,
    data: { device: { uid: channel.deviceUid }, viewers: channel.viewers },
  });
});

/** POST /api/device/register — 设备注册 */
app.post('/api/device/register', (req, res) => {
  const { deviceId, info } = req.body;
  if (!deviceId || typeof deviceId !== 'string') {
    return res.status(400).json({ success: false, error: '缺少 deviceId' });
  }
  // 更新数据库中的设备信息（deviceName 截断防超长）
  const ip = req.ip || '';
  const regDeviceName = typeof info?.computerName === 'string' ? info.computerName.slice(0, 128) : '';
  upsertDevice(req.session.cardId, deviceId, regDeviceName, ip);
  res.json({ success: true, data: { deviceId, registered: true } });
});



// ============================================================
// 管理员 API
// ============================================================

// 管理后台 API 限速（200/min，独立于客户端 API）
const adminApiLimiter = rateLimit({
  windowMs: 60_000, max: 200,
  standardHeaders: true, legacyHeaders: false,
  message: { success: false, error: '请求过于频繁，请稍后重试' },
});
app.use('/admin/api/', adminApiLimiter);

/** POST /admin/api/auth — 管理员登录 */
app.post('/admin/api/auth', authLimiter, async (req, res) => {
  const { username, password } = req.body;
  if (!username || !password) {
    return res.status(400).json({ success: false, error: '缺少用户名或密码' });
  }

  const admin = getAdminByUsername(username);
  if (!admin) return res.status(401).json({ success: false, error: '账号或密码错误' });

  const match = await bcrypt.compare(password, admin.password_hash);
  if (!match) return res.status(401).json({ success: false, error: '账号或密码错误' });

  const token = jwt.sign({ adminId: admin.id, username: admin.username }, JWT_SECRET, { expiresIn: '24h' });
  console.log(`🔐 管理员登录: ${username}`);
  // 以 HttpOnly Cookie 下发，JS 层不可读取，防止 XSS 窃取
  res.cookie('mdshare_admin', token, {
    httpOnly: true,
    secure:   false,
    sameSite: 'lax',
    maxAge:   24 * 60 * 60 * 1000,  // 24h (ms)
    path:     '/admin',
  });
  res.json({ success: true, data: { username: admin.username } });
});

/** GET /admin/api/me — 返回当前登录的管理员信息 */
app.get('/admin/api/me', adminAuth, (req, res) => {
  res.json({ success: true, data: { username: req.admin.username } });
});

/** GET /admin/api/cards — 卡密列表 */
app.get('/admin/api/cards', adminAuth, (req, res) => {
  const page  = parseInt(req.query.page) || 1;
  const limit = Math.min(parseInt(req.query.limit) || 50, 200);  // 硬上限 200，防止全表倾倒
  const result = listCards({ page, limit });
  res.json({ success: true, data: result });
});

/** POST /admin/api/cards — 创建卡密（批量） */
app.post('/admin/api/cards', adminAuth, (req, res) => {
  const { count = 1, durationDays = 30, maxDevices = 1, label = '', quotaMinutes = 10000 } = req.body;
  const n = Math.min(Math.max(parseInt(count) || 1, 1), 100);
  const quota = Math.max(parseInt(quotaMinutes) || 10000, 100); // 最少 100 标准分钟

  const generated = [];
  for (let i = 0; i < n; i++) {
    const raw     = 'MDSH-' + crypto.randomBytes(4).toString('hex').toUpperCase()
                     + '-' + crypto.randomBytes(4).toString('hex').toUpperCase();
    const keyHash    = hashKey(raw);
    const keyDisplay = raw.slice(0, 13) + '****';
    try {
      createCard({ keyHash, keyDisplay, label, durationDays: parseInt(durationDays), maxDevices: parseInt(maxDevices), quotaMinutes: quota });
      generated.push(raw);
    } catch {
      // hash 碰撞（极罕见），跳过
    }
  }

  console.log(`🎫 管理员创建 ${generated.length} 张卡密 (配额: ${quota} 标准分钟)`);
  res.json({ success: true, data: { cards: generated, count: generated.length } });
});

/** PATCH /admin/api/cards/:id — 修改卡密 */
app.patch('/admin/api/cards/:id', adminAuth, (req, res) => {
  const id   = parseInt(req.params.id);
  const card = getCardById(id);
  if (!card) return res.status(404).json({ success: false, error: '卡密不存在' });

  const { isBanned, additionalDays, label, maxDevices, rechargeMinutes, resetUsage, allowRemoteControl } = req.body;

  if (additionalDays) {
    renewCard(id, parseInt(additionalDays));
  }
  // 配额充值：增加标准分钟
  if (rechargeMinutes && parseInt(rechargeMinutes) > 0) {
    rechargeQuota(id, parseInt(rechargeMinutes));
  }
  // 重置用量归零
  if (resetUsage) {
    updateCard(id, { usedMinutes: 0 });
  }
  updateCard(id, {
    isBanned:            isBanned !== undefined ? (isBanned ? 1 : 0) : undefined,
    label:               label,
    maxDevices:          maxDevices ? parseInt(maxDevices) : undefined,
    allowRemoteControl:  allowRemoteControl !== undefined ? (allowRemoteControl ? 1 : 0) : undefined,
  });

  res.json({ success: true, data: getCardById(id) });
});

/** GET /admin/api/cards/:id/usage — 卡密用量日志 */
app.get('/admin/api/cards/:id/usage', adminAuth, (req, res) => {
  const id = parseInt(req.params.id);
  if (!getCardById(id)) return res.status(404).json({ success: false, error: '卡密不存在' });
  const logs = getUsageLogs(id);
  res.json({ success: true, data: logs });
});

/** DELETE /admin/api/cards/:id — 删除卡密 */
app.delete('/admin/api/cards/:id', adminAuth, (req, res) => {
  const id = parseInt(req.params.id);
  if (!getCardById(id)) return res.status(404).json({ success: false, error: '卡密不存在' });
  deleteCard(id);
  res.json({ success: true });
});

/** GET /admin/api/sessions — 在线会话 */
app.get('/admin/api/sessions', adminAuth, (req, res) => {
  res.json({ success: true, data: listActiveSessions() });
});

/** DELETE /admin/api/sessions — 强制下线（token 在请求体中，避免写入访问日志） */
app.delete('/admin/api/sessions', adminAuth, (req, res) => {
  const { token } = req.body;
  if (!token || typeof token !== 'string') {
    return res.status(400).json({ success: false, error: '缺少 token' });
  }
  revokeSession(token);
  res.json({ success: true });
});

/** GET /admin/api/cards/:id/devices — 卡密绑定设备 */
app.get('/admin/api/cards/:id/devices', adminAuth, (req, res) => {
  res.json({ success: true, data: listDevices(parseInt(req.params.id)) });
});

// ============================================================
// 计费倍率配置 API（管理员）
// ============================================================

/** GET /admin/api/billing — 获取计费倍率配置 */
app.get('/admin/api/billing', adminAuth, (req, res) => {
  const config = getBillingConfig();
  res.json({ success: true, data: config, defaults: DEFAULT_AGORA_MULTIPLIERS });
});

/** PUT /admin/api/billing — 更新计费倍率配置 */
app.put('/admin/api/billing', adminAuth, (req, res) => {
  const { globalMultiplier, resolution } = req.body;

  // 校验 globalMultiplier
  if (globalMultiplier !== undefined) {
    if (typeof globalMultiplier !== 'number' || !Number.isFinite(globalMultiplier) || globalMultiplier < 0 || globalMultiplier > 100) {
      return res.status(400).json({ success: false, error: '全局倍率必须在 0~100 之间' });
    }
  }

  // 校验 resolution
  const VALID_RESOLUTIONS = ['audio', '720p', '1080p', '2k'];
  if (resolution !== undefined) {
    if (typeof resolution !== 'object' || resolution === null) {
      return res.status(400).json({ success: false, error: '分辨率倍率格式错误' });
    }
    for (const [key, val] of Object.entries(resolution)) {
      if (!VALID_RESOLUTIONS.includes(key)) {
        return res.status(400).json({ success: false, error: `未知分辨率: ${key}` });
      }
      if (typeof val !== 'object' || val === null ||
          typeof val.publisher !== 'number' || !Number.isFinite(val.publisher) || val.publisher < 0 ||
          typeof val.viewer    !== 'number' || !Number.isFinite(val.viewer)    || val.viewer < 0) {
        return res.status(400).json({ success: false, error: `分辨率 ${key} 的 publisher/viewer 倍率必须为非负数` });
      }
    }
  }

  // 合并更新
  const current = getBillingConfig();
  if (globalMultiplier !== undefined) current.globalMultiplier = globalMultiplier;
  if (resolution !== undefined) {
    if (!current.resolution) current.resolution = { ...DEFAULT_AGORA_MULTIPLIERS };
    for (const [key, val] of Object.entries(resolution)) {
      current.resolution[key] = val;
    }
  }

  saveBillingConfig(current);
  console.log(`[Billing] 倍率配置已更新:`, JSON.stringify(current));
  res.json({ success: true, data: current });
});

/** POST /admin/api/billing/reset — 重置为默认倍率 */
app.post('/admin/api/billing/reset', adminAuth, (req, res) => {
  const defaultConfig = {
    globalMultiplier: 1.0,
    resolution: { ...DEFAULT_AGORA_MULTIPLIERS },
  };
  saveBillingConfig(defaultConfig);
  console.log('[Billing] 倍率配置已重置为默认值');
  res.json({ success: true, data: defaultConfig });
});


// ============================================================
// WebSocket 事件（原生 ws）
// ============================================================
wss.on('connection', (ws) => {
  const channelId = ws._channelId;
  console.log(`[WS] 新连接: channel=${channelId}, role=${ws._wsRole}, session.cardId=${ws._session?.cardId}`);

  // 加入频道房间
  if (channelId) {
    if (!wsRooms.has(channelId)) wsRooms.set(channelId, new Set());
    wsRooms.get(channelId).add(ws);
    console.log(`[WS] 房间 ${channelId} 人数: ${wsRooms.get(channelId).size}`);
    // 推送频道统计
    const entry = chanStats.get(channelId);
    if (entry) ws.send(JSON.stringify({ type: 'channel-stats', channelId, ...entry }));
  }

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw); } catch { return; }
    if (!msg || typeof msg.type !== 'string') return;

    switch (msg.type) {

      // ── 心跳 ──
      case 'ping':
        ws.send('{"type":"pong"}');
        break;

      // ── 频道内文字聊天 ──
      case 'chat': {
        if (!verifySession(ws._token)) { ws.close(); return; }
        const text = typeof msg.text === 'string' ? msg.text.slice(0, 2000) : '';
        if (!text || !channelId) return;
        wsBroadcast(channelId, { type: 'chat', text, time: msg.time || new Date().toISOString() }, ws);
        break;
      }

      // ── 远控：观看端请求开启 ──
      case 'rc-start': {
        console.log(`[WS][rc-start] channelId=${channelId}, wsRole=${ws._wsRole}`);
        const session = verifySession(ws._token);
        if (!session) { console.log('[WS][rc-start] session 验证失败'); ws.close(); return; }
        if (!channelId) { console.log('[WS][rc-start] 无 channelId'); return; }
        const card = getCardById(session.cardId);
        console.log(`[WS][rc-start] cardId=${session.cardId}, card.allow_remote_control=${card?.allow_remote_control}, card.id=${card?.id}`);
        if (!card || !card.allow_remote_control) {
          console.log(`[WS][rc-start] 权限拒绝: card=${!!card}, allow_rc=${card?.allow_remote_control}`);
          ws.send(JSON.stringify({ type: 'rc-error', error: 'remote_control_not_allowed' }));
          return;
        }
        const ch = channels.get(channelId);
        if (!ch) {
          console.log(`[WS][rc-start] 频道不存在: ${channelId}`);
          ws.send(JSON.stringify({ type: 'rc-error', error: 'channel_not_found' }));
          return;
        }
        ws._rcChannelId = channelId;
        ws._rcRole = 'controller';
        // 通知同频道推流端
        const roomBroadcasters = wsRooms.get(channelId);
        const bcCount = roomBroadcasters ? [...roomBroadcasters].filter(w => w._wsRole === 'broadcaster').length : 0;
        console.log(`[WS][rc-start] 发送给 broadcaster，频道=${channelId}, broadcaster 数=${bcCount}`);
        wsBroadcastToRole(channelId, 'broadcaster', { type: 'rc-start', controllerId: `ws_${Date.now()}` }, ws);
        // 不立即发 rc-started，等待 broadcaster 确认 (rc-ready)
        // 设置超时：5s 内没有确认则报错
        ws._rcStartTimeout = setTimeout(() => {
          ws._rcStartTimeout = null;
          if (ws.readyState === WebSocket.OPEN && ws._rcRole === 'controller') {
            ws.send(JSON.stringify({ type: 'rc-error', error: 'broadcaster_timeout' }));
            ws._rcChannelId = null;
            ws._rcRole = null;
          }
        }, 5000);
        break;
      }

      // ── 远控：被控端确认就绪 ──
      case 'rc-ready': {
        if (ws._wsRole !== 'broadcaster' || !channelId) return;
        const room = wsRooms.get(channelId);
        if (!room) return;
        for (const w of room) {
          if (w._rcChannelId === channelId && w._rcRole === 'controller' && w.readyState === WebSocket.OPEN) {
            if (w._rcStartTimeout) { clearTimeout(w._rcStartTimeout); w._rcStartTimeout = null; }
            w._rcTargetWs = ws;  // 缓存 broadcaster WS 引用，高频 rc-input 免遍历
            w.send(JSON.stringify({ type: 'rc-started', channelId }));
            console.log(`[WS][rc-ready] broadcaster 确认，已通知 controller`);
          }
        }
        break;
      }

      // ── 远控：停止 ──
      case 'rc-stop': {
        if (!channelId) return;
        ws._rcChannelId = null;
        ws._rcRole = null;
        wsBroadcast(channelId, { type: 'rc-stop', channelId }, ws);
        break;
      }

      // ── 远控：高频输入转发（鼠标/键盘） ──
      case 'rc-input': {
        if (!ws._rcChannelId) return;
        const str = raw.toString();
        if (str.length > 4096) return;
        // 优先使用缓存的 broadcaster WS 引用，避免每次遍历房间
        if (ws._rcTargetWs && ws._rcTargetWs.readyState === WebSocket.OPEN) {
          ws._rcTargetWs.send(str);
        } else {
          // 回退到广播模式
          ws._rcTargetWs = null;
          wsBroadcastToRole(ws._rcChannelId, 'broadcaster', str, ws);
        }
        break;
      }

      // ── 远控：被控端上报屏幕信息 ──
      case 'rc-screen-info': {
        if (!channelId) return;
        wsBroadcastToRole(channelId, 'viewer', JSON.stringify(msg), ws);
        break;
      }

      // ── 远控：被控端光标形状同步 ──
      case 'rc-cursor': {
        if (ws._wsRole !== 'broadcaster' || !channelId) return;
        wsBroadcastToRole(channelId, 'viewer', JSON.stringify(msg), ws);
        break;
      }

      // ── Perf 数据实时同步：推流端通知观看端有新文件可下载 ──
      case 'perf-file-available': {
        if (ws._wsRole !== 'broadcaster' || !channelId) return;
        // 转发给所有 viewer
        wsBroadcastToRole(channelId, 'viewer', JSON.stringify(msg), ws);
        break;
      }

    }
  });

  ws.on('close', () => {
    // 离开房间
    if (channelId) {
      const room = wsRooms.get(channelId);
      if (room) { room.delete(ws); if (room.size === 0) wsRooms.delete(channelId); }
    }
    // 控制者断开 → 清理超时定时器，通知被控端停止远控
    if (ws._rcChannelId && ws._rcRole === 'controller') {
      if (ws._rcStartTimeout) { clearTimeout(ws._rcStartTimeout); ws._rcStartTimeout = null; }
      ws._rcTargetWs = null;
      wsBroadcastToRole(ws._rcChannelId, 'broadcaster', { type: 'rc-stop', channelId: ws._rcChannelId });
    }
  });

  ws.on('error', () => {});
});

// ============================================================
// 全局错误处理（body-parser JSON 解析错误等）
// ============================================================
app.use((err, req, res, _next) => {
  // body-parser JSON 解析失败（客户端发送了畸形 JSON body）
  if (err.type === 'entity.parse.failed') {
    console.warn(`[BodyParser] JSON 解析失败: ${req.method} ${req.path} (${err.message})`);
    return res.status(400).json({ success: false, error: 'JSON 解析失败，请求体格式错误' });
  }
  // 其他未捕获的错误
  console.error(`[Express] 未处理错误: ${err.message}`);
  res.status(500).json({ success: false, error: '服务器内部错误' });
});

// ============================================================
// 启动
// ============================================================

server.listen(PORT, () => {
  console.log(`
  ╔══════════════════════════════════════════════════╗
  ║       MDShare Token Server  v5.0                 ║
  ║  http://localhost:${String(PORT).padEnd(5)}                       ║
  ╠══════════════════════════════════════════════════╣
  ║  POST /api/auth/login         卡密登录           ║
  ║  POST /api/channel/create     创建频道           ║
  ║  POST /api/channel/join       加入频道           ║
  ║  GET  /api/status             服务状态           ║
  ╠══════════════════════════════════════════════════╣
  ║  Agora: 自有服务                                 ║
  ║  管理后台: /admin/                               ║
  ╚══════════════════════════════════════════════════╝
  `);
});

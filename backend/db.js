/**
 * db.js — SQLite 数据库初始化与常用查询
 * 使用 better-sqlite3（同步 API）
 */
const Database = require('better-sqlite3');
const crypto   = require('crypto');
const path     = require('path');

const DB_PATH = path.join(__dirname, 'mdshare.db');
const db = new Database(DB_PATH);

// WAL 模式：更好的并发读写性能
db.pragma('journal_mode = WAL');
db.pragma('foreign_keys = ON');

// ============================================================
// 建表（idempotent）
// ============================================================
db.exec(`
  CREATE TABLE IF NOT EXISTS admins (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    created_at    INTEGER DEFAULT (unixepoch())
  );

  CREATE TABLE IF NOT EXISTS cards (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    key_hash      TEXT UNIQUE NOT NULL,
    key_display   TEXT NOT NULL,
    label         TEXT,
    duration_days INTEGER NOT NULL DEFAULT 30,
    max_devices   INTEGER NOT NULL DEFAULT 1,
    is_banned     INTEGER NOT NULL DEFAULT 0,
    created_at    INTEGER DEFAULT (unixepoch()),
    activated_at  INTEGER,
    expires_at    INTEGER
  );

  CREATE TABLE IF NOT EXISTS card_devices (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    card_id     INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,
    device_id   TEXT NOT NULL,
    device_name TEXT,
    ip_address  TEXT,
    last_seen   INTEGER DEFAULT (unixepoch()),
    UNIQUE(card_id, device_id)
  );

  CREATE TABLE IF NOT EXISTS sessions (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    card_id       INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,
    device_id     TEXT NOT NULL,
    session_token TEXT UNIQUE NOT NULL,
    created_at    INTEGER DEFAULT (unixepoch()),
    expires_at    INTEGER NOT NULL,
    last_seen     INTEGER DEFAULT (unixepoch())
  );

  CREATE INDEX IF NOT EXISTS idx_sessions_token ON sessions(session_token);
  CREATE INDEX IF NOT EXISTS idx_cards_hash     ON cards(key_hash);
`);

// ── 配额相关表与字段迁移（idempotent）──────────────────────────
// cards 表新增字段：quota_minutes（总配额标准分钟）、used_minutes（已消耗标准分钟）
try { db.exec(`ALTER TABLE cards ADD COLUMN quota_minutes REAL NOT NULL DEFAULT 10000`); } catch {}
try { db.exec(`ALTER TABLE cards ADD COLUMN used_minutes  REAL NOT NULL DEFAULT 0`);     } catch {}

// cards 表新增字段：allow_remote_control（远程控制权限，0=禁止, 1=允许）
try { db.exec(`ALTER TABLE cards ADD COLUMN allow_remote_control INTEGER NOT NULL DEFAULT 0`); } catch {}

// 用量明细日志表：每次共享结束后写入一条记录，用于审计和账单
db.exec(`
  CREATE TABLE IF NOT EXISTS usage_logs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    card_id         INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,
    channel_id      TEXT NOT NULL,
    resolution      TEXT NOT NULL DEFAULT '1080p',
    started_at      INTEGER NOT NULL,
    ended_at        INTEGER,
    duration_sec    REAL NOT NULL DEFAULT 0,
    pub_std_minutes REAL NOT NULL DEFAULT 0,
    view_std_minutes REAL NOT NULL DEFAULT 0,
    total_std_minutes REAL NOT NULL DEFAULT 0,
    peak_viewers    INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER DEFAULT (unixepoch())
  );
  CREATE INDEX IF NOT EXISTS idx_usage_logs_card ON usage_logs(card_id);
`);

// ============================================================
// 系统设置表（key-value 存储）
// ============================================================
db.exec(`
  CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at INTEGER DEFAULT (unixepoch())
  );
`);

/** 读取设置 */
function getSetting(key) {
  const row = db.prepare('SELECT value FROM settings WHERE key = ?').get(key);
  return row ? row.value : null;
}

/** 写入设置 */
function setSetting(key, value) {
  db.prepare(`
    INSERT INTO settings (key, value, updated_at) VALUES (?, ?, unixepoch())
    ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at
  `).run(key, value);
}

// ============================================================
// 配额管理
// ============================================================

/**
 * 声网标准时长折算系数 — 默认值（mode: 'live'）
 * 主播 = Publisher / 极速直播观众 = audience
 */
const DEFAULT_AGORA_MULTIPLIERS = {
  'audio': { publisher: 1,  viewer: 0.57 },
  '720p':  { publisher: 4,  viewer: 2    },
  '1080p': { publisher: 9,  viewer: 4.57 },
  '2k':    { publisher: 16, viewer: 8    },
};

/**
 * 获取当前计费倍率配置
 * 结构: { globalMultiplier, resolution: { audio/720p/1080p/2k: { publisher, viewer } } }
 */
function getBillingConfig() {
  const raw = getSetting('billing_config');
  if (raw) {
    try { return JSON.parse(raw); } catch {}
  }
  return {
    globalMultiplier: 1.0,
    resolution: { ...DEFAULT_AGORA_MULTIPLIERS },
  };
}

/** 保存计费倍率配置 */
function saveBillingConfig(config) {
  setSetting('billing_config', JSON.stringify(config));
}

/**
 * 获取当前有效的 AGORA_MULTIPLIERS（已应用全局倍率）
 * 返回格式与原 AGORA_MULTIPLIERS 一致，供计费代码使用
 */
function getEffectiveMultipliers() {
  const cfg = getBillingConfig();
  const g   = cfg.globalMultiplier ?? 1.0;
  const res = cfg.resolution || DEFAULT_AGORA_MULTIPLIERS;
  const out = {};
  for (const [key, val] of Object.entries(res)) {
    out[key] = {
      publisher: (val.publisher || 0) * g,
      viewer:    (val.viewer    || 0) * g,
    };
  }
  return out;
}

// 兼容旧代码的导出（使用默认值）
const AGORA_MULTIPLIERS = DEFAULT_AGORA_MULTIPLIERS;

/** 获取卡密配额信息 */
function getQuotaInfo(cardId) {
  const card = db.prepare('SELECT quota_minutes, used_minutes FROM cards WHERE id = ?').get(cardId);
  if (!card) return null;
  return {
    quotaMinutes:     card.quota_minutes,
    usedMinutes:      parseFloat(card.used_minutes.toFixed(2)),
    remainingMinutes: parseFloat(Math.max(0, card.quota_minutes - card.used_minutes).toFixed(2)),
    usagePercent:     parseFloat((card.used_minutes / card.quota_minutes * 100).toFixed(1)),
  };
}

/** 扣减配额（原子操作） */
function deductQuota(cardId, standardMinutes) {
  db.prepare('UPDATE cards SET used_minutes = used_minutes + ? WHERE id = ?')
    .run(standardMinutes, cardId);
}

/** 检查配额是否充足 */
function hasQuota(cardId) {
  const card = db.prepare('SELECT quota_minutes, used_minutes FROM cards WHERE id = ?').get(cardId);
  if (!card) return false;
  return card.used_minutes < card.quota_minutes;
}

/** 充值配额 */
function rechargeQuota(cardId, additionalMinutes) {
  db.prepare('UPDATE cards SET quota_minutes = quota_minutes + ? WHERE id = ?')
    .run(additionalMinutes, cardId);
}

/** 写入用量日志 */
function addUsageLog({ cardId, channelId, resolution, startedAt, endedAt, durationSec,
                       pubStdMinutes, viewStdMinutes, totalStdMinutes, peakViewers }) {
  db.prepare(`
    INSERT INTO usage_logs (card_id, channel_id, resolution, started_at, ended_at,
      duration_sec, pub_std_minutes, view_std_minutes, total_std_minutes, peak_viewers)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  `).run(cardId, channelId, resolution || '1080p', startedAt, endedAt || now(),
         durationSec, pubStdMinutes, viewStdMinutes, totalStdMinutes, peakViewers || 0);
}

/** 查询卡密用量日志 */
function getUsageLogs(cardId, limit = 50) {
  return db.prepare(`
    SELECT * FROM usage_logs WHERE card_id = ? ORDER BY created_at DESC LIMIT ?
  `).all(cardId, limit);
}

// ============================================================
// 工具
// ============================================================
function hashKey(rawKey) {
  return crypto.createHash('sha256').update(rawKey.trim().toUpperCase()).digest('hex');
}

function now() {
  return Math.floor(Date.now() / 1000);
}

// ============================================================
// 卡密查询
// ============================================================

/** 按 SHA-256 hash 查卡密 */
function getCardByHash(hash) {
  return db.prepare('SELECT * FROM cards WHERE key_hash = ?').get(hash);
}

/**
 * 登录时验证卡密 + 设备
 * @returns { card, isNewDevice, deviceCount }
 */
function validateCard(rawKey, deviceId) {
  const hash = hashKey(rawKey);
  const card = getCardByHash(hash);
  if (!card) return { error: 'invalid' };
  if (card.is_banned) return { error: 'banned' };
  if (card.expires_at && card.expires_at < now()) return { error: 'expired' };
  if (card.used_minutes >= card.quota_minutes) return { error: 'quota_exceeded' };

  // 检查设备是否已绑定
  const existingDevice = db.prepare(
    'SELECT * FROM card_devices WHERE card_id = ? AND device_id = ?'
  ).get(card.id, deviceId);

  const deviceCount = db.prepare(
    'SELECT COUNT(*) as cnt FROM card_devices WHERE card_id = ?'
  ).get(card.id).cnt;

  const isNewDevice = !existingDevice;

  if (isNewDevice && deviceCount >= card.max_devices) {
    return { error: 'device_limit' };
  }

  return { card, isNewDevice, deviceCount };
}

/**
 * 首次激活卡密（设置 activated_at / expires_at）
 */
function activateCard(cardId) {
  const card = db.prepare('SELECT * FROM cards WHERE id = ?').get(cardId);
  if (card.activated_at) return; // 已激活过
  const expireTs = now() + card.duration_days * 86400;
  db.prepare(
    'UPDATE cards SET activated_at = ?, expires_at = ? WHERE id = ?'
  ).run(now(), expireTs, cardId);
}

/**
 * 记录/更新设备信息
 */
function upsertDevice(cardId, deviceId, deviceName, ip) {
  db.prepare(`
    INSERT INTO card_devices (card_id, device_id, device_name, ip_address, last_seen)
    VALUES (?, ?, ?, ?, ?)
    ON CONFLICT(card_id, device_id)
    DO UPDATE SET device_name=excluded.device_name, ip_address=excluded.ip_address, last_seen=excluded.last_seen
  `).run(cardId, deviceId, deviceName || '', ip || '', now());
}

/**
 * 创建会话 token（覆盖该设备的旧 session）
 */
function createSession(cardId, deviceId, expireTs) {
  // 删除同设备的旧 session
  db.prepare('DELETE FROM sessions WHERE card_id = ? AND device_id = ?').run(cardId, deviceId);
  const token = crypto.randomBytes(32).toString('hex');
  db.prepare(`
    INSERT INTO sessions (card_id, device_id, session_token, expires_at)
    VALUES (?, ?, ?, ?)
  `).run(cardId, deviceId, token, expireTs);
  return token;
}

/**
 * 验证 session token，返回 { cardId, deviceId } 或 null
 */
function verifySession(token) {
  const row = db.prepare(`
    SELECT s.*, c.is_banned, c.expires_at AS card_expires
    FROM sessions s
    JOIN cards c ON c.id = s.card_id
    WHERE s.session_token = ?
  `).get(token);

  if (!row) return null;
  if (row.expires_at < now()) return null;   // session 过期
  if (row.is_banned) return null;             // 卡已封禁
  if (row.card_expires && row.card_expires < now()) return null; // 卡密过期

  // 更新 last_seen
  db.prepare('UPDATE sessions SET last_seen = ? WHERE session_token = ?').run(now(), token);

  return { cardId: row.card_id, deviceId: row.device_id };
}

// ============================================================
// 管理员查询
// ============================================================

function getAdminByUsername(username) {
  return db.prepare('SELECT * FROM admins WHERE username = ?').get(username);
}

function createAdmin(username, passwordHash) {
  return db.prepare(
    'INSERT OR IGNORE INTO admins (username, password_hash) VALUES (?, ?)'
  ).run(username, passwordHash);
}

// ============================================================
// 管理 API 用查询
// ============================================================

function listCards({ page = 1, limit = 50 } = {}) {
  const offset = (page - 1) * limit;
  const cards = db.prepare(`
    SELECT c.*,
      (SELECT COUNT(*) FROM card_devices WHERE card_id = c.id) AS device_count,
      (SELECT COUNT(*) FROM sessions WHERE card_id = c.id AND expires_at > ?) AS active_sessions
    FROM cards c ORDER BY c.created_at DESC LIMIT ? OFFSET ?
  `).all(now(), limit, offset);
  const total = db.prepare('SELECT COUNT(*) as cnt FROM cards').get().cnt;
  return { cards, total };
}

function getCardById(id) {
  return db.prepare('SELECT * FROM cards WHERE id = ?').get(id);
}

function createCard({ keyHash, keyDisplay, label, durationDays, maxDevices, quotaMinutes }) {
  return db.prepare(`
    INSERT INTO cards (key_hash, key_display, label, duration_days, max_devices, quota_minutes)
    VALUES (?, ?, ?, ?, ?, ?)
  `).run(keyHash, keyDisplay, label || '', durationDays, maxDevices, quotaMinutes || 10000);
}

function updateCard(id, fields) {
  // fields: { label, durationDays, maxDevices, isBanned, expiresAt, quotaMinutes, usedMinutes, allowRemoteControl }
  const allowed = {
    label:                fields.label,
    duration_days:        fields.durationDays,
    max_devices:          fields.maxDevices,
    is_banned:            fields.isBanned,
    expires_at:           fields.expiresAt,
    quota_minutes:        fields.quotaMinutes,
    used_minutes:         fields.usedMinutes,
    allow_remote_control: fields.allowRemoteControl,
  };
  const sets = Object.entries(allowed)
    .filter(([, v]) => v !== undefined)
    .map(([k]) => `${k} = ?`);
  const vals = Object.entries(allowed)
    .filter(([, v]) => v !== undefined)
    .map(([, v]) => v);
  if (sets.length === 0) return;
  db.prepare(`UPDATE cards SET ${sets.join(', ')} WHERE id = ?`).run(...vals, id);
}

function renewCard(id, additionalDays) {
  const card = getCardById(id);
  if (!card) return;
  const base = Math.max(card.expires_at || now(), now());
  db.prepare('UPDATE cards SET expires_at = ? WHERE id = ?')
    .run(base + additionalDays * 86400, id);
}

function deleteCard(id) {
  db.prepare('DELETE FROM cards WHERE id = ?').run(id);
}

function listActiveSessions() {
  const ONLINE_THRESHOLD = 120; // 2分钟内有活跃视为在线
  const nowTs = now();
  const rows = db.prepare(`
    SELECT s.session_token, s.device_id, s.created_at, s.last_seen, s.expires_at,
           c.key_display, c.label
    FROM sessions s
    JOIN cards c ON c.id = s.card_id
    WHERE s.expires_at > ?
    ORDER BY s.last_seen DESC
  `).all(nowTs);
  return rows.map(r => ({
    ...r,
    is_online: (nowTs - r.last_seen) <= ONLINE_THRESHOLD,
  }));
}

function revokeSession(token) {
  db.prepare('DELETE FROM sessions WHERE session_token = ?').run(token);
}

function listDevices(cardId) {
  return db.prepare('SELECT * FROM card_devices WHERE card_id = ?').all(cardId);
}

// ============================================================
// TTL 清理（每 10 分钟清除过期 session + 过期授权令牌）
// ============================================================
setInterval(() => {
  db.prepare('DELETE FROM sessions WHERE expires_at < ?').run(now());
}, 10 * 60 * 1000);

module.exports = {
  db, hashKey, now,
  AGORA_MULTIPLIERS, DEFAULT_AGORA_MULTIPLIERS,
  getBillingConfig, saveBillingConfig, getEffectiveMultipliers,
  getSetting, setSetting,
  validateCard, activateCard, upsertDevice, createSession, verifySession,
  getQuotaInfo, deductQuota, hasQuota, rechargeQuota, addUsageLog, getUsageLogs,
  getAdminByUsername, createAdmin,
  listCards, getCardById, createCard, updateCard, renewCard, deleteCard,
  listActiveSessions, revokeSession, listDevices,
};


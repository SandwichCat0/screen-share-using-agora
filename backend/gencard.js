#!/usr/bin/env node
/**
 * gencard.js — 命令行卡密生成工具
 *
 * 用法:
 *   node gencard.js --count 5 --days 30 --devices 1 --quota 10000 --label "测试批次"
 *   node gencard.js --list
 *   node gencard.js --ban <id>
 *   node gencard.js --unban <id>
 *   node gencard.js --renew <id> --days 30
 *   node gencard.js --recharge <id> --quota 5000
 *   node gencard.js --delete <id>
 *   node gencard.js --admin --username admin --password yourpassword
 */

require('dotenv').config();
const crypto = require('crypto');
const bcrypt = require('bcrypt');
const { createCard, updateCard, renewCard, deleteCard, listCards, hashKey,
        getCardById, createAdmin, getAdminByUsername, listDevices,
        rechargeQuota, getQuotaInfo } = require('./db');

const args = process.argv.slice(2);
const get  = (flag) => { const i = args.indexOf(flag); return i >= 0 ? args[i + 1] : null; };
const has  = (flag) => args.includes(flag);

function generateRawKey() {
  return 'MDSH-' +
    crypto.randomBytes(4).toString('hex').toUpperCase() + '-' +
    crypto.randomBytes(4).toString('hex').toUpperCase();
}

function formatDate(ts) {
  if (!ts) return '未激活';
  return new Date(ts * 1000).toLocaleString('zh-CN');
}

// ── 创建管理员账号 ──────────────────────────────────────
if (has('--admin')) {
  const username = get('--username') || 'admin';
  const password = get('--password');
  if (!password) {
    console.error('❌ 请提供 --password <密码>');
    process.exit(1);
  }
  (async () => {
    const existing = getAdminByUsername(username);
    if (existing) {
      console.log(`⚠️  用户 "${username}" 已存在，更新密码...`);
      const hash = await bcrypt.hash(password, 12);
      const { db } = require('./db');
      db.prepare('UPDATE admins SET password_hash = ? WHERE username = ?').run(hash, username);
      console.log(`✅ 密码已更新: ${username}`);
    } else {
      const hash = await bcrypt.hash(password, 12);
      createAdmin(username, hash);
      console.log(`✅ 管理员账号已创建: ${username}`);
    }
  })();
  return;
}

// ── 列出卡密 ────────────────────────────────────────────
if (has('--list')) {
  const { cards, total } = listCards({ page: 1, limit: 100 });
  console.log(`\n共 ${total} 张卡密:\n`);
  console.log('ID'.padEnd(6) + '卡密前缀'.padEnd(20) + '状态'.padEnd(10) + '配额(标准分钟)'.padEnd(22) + '有效期'.padEnd(22) + '激活时间'.padEnd(22) + '设备数 备注');
  console.log('─'.repeat(120));
  for (const c of cards) {
    const status = c.is_banned ? '🚫 封禁' : !c.activated_at ? '⚪ 未激活' : c.expires_at < Date.now()/1000 ? '🔴 过期' : '🟢 有效';
    const quotaStr = `${(c.used_minutes || 0).toFixed(1)}/${(c.quota_minutes || 0).toFixed(0)}`;
    console.log(
      String(c.id).padEnd(6) +
      c.key_display.padEnd(20) +
      status.padEnd(10) +
      quotaStr.padEnd(22) +
      formatDate(c.expires_at).padEnd(22) +
      formatDate(c.activated_at).padEnd(22) +
      `${c.device_count ?? 0}/${c.max_devices}  ${c.label || ''}`
    );
  }
  console.log();
  return;
}

// ── 封禁 ────────────────────────────────────────────────
if (has('--ban')) {
  const id = parseInt(get('--ban'));
  if (!getCardById(id)) { console.error(`❌ 卡密 ID ${id} 不存在`); process.exit(1); }
  updateCard(id, { isBanned: 1 });
  console.log(`✅ 已封禁卡密 ID: ${id}`);
  return;
}

// ── 解封 ────────────────────────────────────────────────
if (has('--unban')) {
  const id = parseInt(get('--unban'));
  if (!getCardById(id)) { console.error(`❌ 卡密 ID ${id} 不存在`); process.exit(1); }
  updateCard(id, { isBanned: 0 });
  console.log(`✅ 已解封卡密 ID: ${id}`);
  return;
}

// ── 续期 ────────────────────────────────────────────────
if (has('--renew')) {
  const id   = parseInt(get('--renew'));
  const days = parseInt(get('--days') || '30');
  if (!getCardById(id)) { console.error(`❌ 卡密 ID ${id} 不存在`); process.exit(1); }
  renewCard(id, days);
  const c = getCardById(id);
  console.log(`✅ 已续期卡密 ID: ${id}，新到期: ${formatDate(c.expires_at)}`);
  return;
}

// ── 充值配额 ────────────────────────────────────────────
if (has('--recharge')) {
  const id = parseInt(get('--recharge'));
  const quota = parseInt(get('--quota') || '5000');
  if (!getCardById(id)) { console.error(`❌ 卡密 ID ${id} 不存在`); process.exit(1); }
  rechargeQuota(id, quota);
  const info = getQuotaInfo(id);
  console.log(`✅ 已为卡密 ID ${id} 充值 ${quota} 标准分钟`);
  console.log(`   当前配额: ${info.total.toFixed(0)} 标准分钟, 已用: ${info.used.toFixed(1)}, 剩余: ${info.remaining.toFixed(1)}`);
  return;
}

// ── 删除 ────────────────────────────────────────────────
if (has('--delete')) {
  const id = parseInt(get('--delete'));
  if (!getCardById(id)) { console.error(`❌ 卡密 ID ${id} 不存在`); process.exit(1); }
  deleteCard(id);
  console.log(`✅ 已删除卡密 ID: ${id}`);
  return;
}

// ── 查看绑定设备 ─────────────────────────────────────────
if (has('--devices')) {
  const id      = parseInt(get('--devices'));
  const devices = listDevices(id);
  console.log(`\n卡密 ID ${id} 绑定的设备 (${devices.length} 台):\n`);
  for (const d of devices) {
    console.log(`  设备ID: ${d.device_id}  名称: ${d.device_name || '--'}  IP: ${d.ip_address || '--'}  最后登录: ${formatDate(d.last_seen)}`);
  }
  console.log();
  return;
}

// ── 生成卡密（默认操作）────────────────────────────────
const count        = parseInt(get('--count')   || '1');
const days         = parseInt(get('--days')    || '30');
const devices      = parseInt(get('--devices') || '1');
const quotaMinutes = parseInt(get('--quota')   || '10000');
const label        = get('--label') || '';

if (isNaN(count) || count < 1 || count > 1000) {
  console.error('❌ --count 必须在 1~1000 之间');
  process.exit(1);
}

const generated = [];
for (let i = 0; i < count; i++) {
  const raw     = generateRawKey();
  const keyHash = hashKey(raw);
  const keyDisplay = raw.slice(0, 13) + '****';
  try {
    createCard({ keyHash, keyDisplay, label, durationDays: days, maxDevices: devices, quotaMinutes });
    generated.push(raw);
  } catch {
    // 极罕见的 hash 碰撞
  }
}

console.log(`\n✅ 已生成 ${generated.length} 张卡密（有效期 ${days} 天，最多 ${devices} 台设备，配额 ${quotaMinutes} 标准分钟）:\n`);
for (const key of generated) {
  console.log('  ' + key);
}
if (label) console.log(`\n  备注: ${label}`);
console.log('\n⚠️  以上卡密仅显示一次，请妥善保存！\n');

#!/usr/bin/env node
/**
 * 生成 coturn REST 时效凭据（配合 turnserver.conf 的 use-auth-secret）：
 *   username = 凭据过期时间戳(秒)
 *   password = base64( HMAC-SHA1( secret, username ) )
 *
 * 用法：
 *   node gen-credential.js <secret> [有效分钟数，默认 60]
 * 输出可直接拼到客户端命令行的 --turn-user/--turn-pass
 */
const crypto = require('crypto');

const [secret, minutes = '60'] = process.argv.slice(2);
if (!secret) {
  console.error('用法: node gen-credential.js <secret> [minutes]');
  process.exit(1);
}
const expiry = Math.floor(Date.now() / 1000) + Number(minutes) * 60;
const username = String(expiry);
const password = crypto.createHmac('sha1', secret).update(username).digest('base64');
console.log(`--turn-user ${username} --turn-pass ${password}   # 有效至 ${new Date(expiry * 1000).toISOString()}`);

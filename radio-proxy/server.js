// ESP32 Radio Proxy v5
// 핵심 변경: Windows에서 ffmpeg를 PID 대신 이름으로 일괄 종료 (taskkill /IM)
// 이유: child.kill() + taskkill /PID 가 네트워크 I/O 중인 ffmpeg에서 실패하는 경우 있음
'use strict';

const http   = require('http');
const { spawn, execSync, spawnSync } = require('child_process');
const fs     = require('fs');
const path   = require('path');

const RESOLVE_CACHE_MS   = 5 * 60_000;
const STALL_MS           = 12_000;
const MAX_RETRIES        = 3;
const KEEPALIVE_IDLE_MS  = 5_000;
const KEEPALIVE_INTVL_MS = 3_000;
const KEEPALIVE_PROBES   = 3;

const FFMPEG     = process.env.FFMPEG || 'ffmpeg';
const STATIONS_F = path.join(__dirname, 'stations.json');

const IS_WIN = process.platform === 'win32';
const FFMPEG_EXE = path.basename(FFMPEG) + (IS_WIN ? '.exe' : '');

// ── config.ini 로드 (환경변수가 없을 때 폴백) ──────────────────────────
function loadIni() {
  try {
    return Object.fromEntries(
      fs.readFileSync(path.join(__dirname, 'config.ini'), 'utf8')
        .split(/\r?\n/)
        .filter(l => l.trim() && !l.trim().startsWith('#') && l.includes('='))
        .map(l => { const i = l.indexOf('='); return [l.slice(0,i).trim(), l.slice(i+1).trim()]; })
    );
  } catch (_) { return {}; }
}
const INI = loadIni();
const get = (envKey, iniKey, def) => process.env[envKey] ?? INI[iniKey] ?? def;

const PORT    = parseInt(get('PORT',    'PORT',    '8088'), 10);
const GAIN_DB = parseFloat(get('GAIN_DB', 'GAIN_DB', '6'));
const LIMIT   = parseFloat(get('LIMIT',   'LIMIT',   '0.85'));
const LOG_ON  = get('LOG_ENABLED', 'LOG_ENABLED', 'true').toLowerCase() !== 'false';

function log(msg) {
  if (!LOG_ON) return;
  process.stdout.write(`[${new Date().toISOString().slice(0,19).replace('T',' ')}] ${msg}\n`);
}

// ── ffmpeg 프로세스 전체 강제 종료 ──────────────────────────────────────
// Windows: taskkill /IM ffmpeg.exe /F — 이름으로 모든 인스턴스 일괄 종료
// PID 기반 kill 보다 훨씬 신뢰성 높음 (네트워크 I/O 중에도 작동)
function killAllFfmpeg(reason) {
  log(`  killAll(${reason})`);
  if (IS_WIN) {
    // 방법 1: 이름으로 전체 종료 (가장 신뢰성 높음)
    try {
      spawnSync('taskkill', ['/F', '/IM', FFMPEG_EXE], { timeout: 5000 });
    } catch (_) {}
  } else {
    // Linux/Mac: pkill
    try { spawnSync('pkill', ['-9', '-x', path.basename(FFMPEG)], { timeout: 3000 }); }
    catch (_) {}
  }
}

function killByPid(child, reason) {
  if (!child || child.exitCode !== null) return;
  const pid = child.pid;
  log(`  kill pid=${pid} (${reason})`);
  try { child.kill(); } catch (_) {}
  if (IS_WIN) {
    try { spawnSync('taskkill', ['/F', '/T', '/PID', String(pid)], { timeout: 3000 }); }
    catch (_) {}
  }
}

// ── 단일 스트림 전역 상태 ────────────────────────────────────────────────
const S = {
  ver:        0,
  ff:         null,
  res:        null,
  stallTimer: null,
  attempts:   0,
  id:         '',
};

function teardown(reason) {
  S.ver++;
  log(`  teardown(${reason}) → ver=${S.ver}`);

  if (S.stallTimer) { clearInterval(S.stallTimer); S.stallTimer = null; }

  // 1단계: 현재 ffmpeg 프로세스를 PID로 kill
  if (S.ff) { killByPid(S.ff, reason); S.ff = null; }

  // 2단계: Windows에서 잔여 ffmpeg.exe 전체 이름으로 일괄 kill (보험)
  if (IS_WIN) killAllFfmpeg(reason);

  // 3단계: HTTP 응답 종료
  if (S.res && !S.res.writableEnded) { try { S.res.end(); } catch (_) {} }
  S.res = null; S.attempts = 0;
}

// ── watchdog: 주기적으로 현재 ffmpeg 생존 여부 확인 ──────────────────────
// S.ff가 설정되어 있는데 프로세스가 이미 죽었으면 참조 정리
setInterval(() => {
  if (!S.ff) return;
  if (S.ff.exitCode !== null) {
    log(`  watchdog: ff[${S.ff.pid}] already dead (exitCode=${S.ff.exitCode}), clearing`);
    S.ff = null;
  }
}, 3000);

// ── 캐시 & 유틸 ─────────────────────────────────────────────────────────
const resolveCache = new Map();

function loadStations() {
  try {
    return (JSON.parse(fs.readFileSync(STATIONS_F, 'utf8')).stations || [])
      .filter(s => s?.id && s?.source);
  } catch (e) { log(`ERROR stations.json: ${e.message}`); return []; }
}

async function resolveSource(src, depth = 0) {
  if (depth > 3) throw new Error('playlist too deep');
  if (!/\.(pls|m3u)(\?|$)/i.test(src)) return src;
  const c = resolveCache.get(src);
  if (c?.expiresAt > Date.now()) { return c.url; }
  log(`  resolve: ${src}`);
  const r = await fetch(src, { signal: AbortSignal.timeout(5000) });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const txt = await r.text();
  let url = null;
  for (const line of txt.split(/\r?\n/)) {
    const t = line.trim();
    if (/^File\d+\s*=/i.test(t)) { url = t.slice(t.indexOf('=')+1).trim(); break; }
    if (t && !t.startsWith('#') && /^https?:\/\//i.test(t)) { url = t; break; }
  }
  if (!url) throw new Error('no URL in playlist');
  const final = await resolveSource(url, depth + 1);
  resolveCache.set(src, { url: final, expiresAt: Date.now() + RESOLVE_CACHE_MS });
  return final;
}

function spawnFfmpeg(inputUrl) {
  const af = GAIN_DB > 0
    ? `volume=${GAIN_DB}dB,alimiter=limit=${LIMIT}:attack=5:release=50`
    : `alimiter=limit=${LIMIT}:attack=5:release=50`;
  return spawn(FFMPEG, [
    '-hide_banner', '-loglevel', 'warning',
    '-reconnect', '1', '-reconnect_streamed', '1',
    '-reconnect_on_network_error', '1', '-reconnect_on_http_error', '4xx,5xx',
    '-reconnect_delay_max', '5', '-rw_timeout', '15000000',
    '-i', inputUrl, '-vn', '-af', af,
    '-c:a', 'libmp3lame', '-b:a', '128k', '-ar', '44100', '-ac', '2',
    '-write_xing', '0', '-id3v2_version', '0', '-flush_packets', '1',
    '-f', 'mp3', '-',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
}

// ── HTTP 서버 ─────────────────────────────────────────────────────────────
const server = http.createServer(async (req, res) => {
  const clientIp = (req.socket.remoteAddress || '?').replace('::ffff:', '');
  const id = decodeURIComponent(new URL(req.url, 'http://localhost').pathname.slice(1));

  if (!id || id === 'index.html') {
    const st = loadStations();
    res.writeHead(200, {'Content-Type':'application/json; charset=utf-8'});
    return res.end(JSON.stringify({ count:st.length,
      stations:st.map(s=>({id:s.id,name:s.name||s.id,url:`http://${req.headers.host}/${s.id}`}))},null,2));
  }
  if (id === 'health') {
    res.writeHead(200, {'Content-Type':'text/plain'});
    return res.end(`ok  pid=${S.ff?.pid||'none'}  ver=${S.ver}  id=${S.id||'-'}`);
  }
  if (id === 'chime') {
    const f = path.join(__dirname, 'chime.mp3');
    try {
      const st = fs.statSync(f);
      res.writeHead(200, {'Content-Type':'audio/mpeg','Content-Length':st.size,
        'Cache-Control':'no-cache','Connection':'close'});
      fs.createReadStream(f).pipe(res);
    } catch (_) { res.writeHead(404); res.end('chime.mp3 not found'); }
    return;
  }

  const station = loadStations().find(s => s.id === id);
  if (!station) {
    res.writeHead(404, {'Content-Type':'text/plain'});
    return res.end(`Unknown: ${id}`);
  }

  log(`► "${station.name}" (${id}) ← ${clientIp}  [현재 pid=${S.ff?.pid||'none'}]`);

  // ── 기존 스트림 즉시 종료 ─────────────────────────────────────────────
  teardown('new request');

  const myVer = S.ver;
  S.res = res;
  S.id  = id;

  // ── 소켓 keepalive ────────────────────────────────────────────────────
  const sock = req.socket;
  sock.setKeepAlive(true, KEEPALIVE_IDLE_MS);
  if (sock.setKeepAliveProbes)   sock.setKeepAliveProbes(KEEPALIVE_PROBES);
  if (sock.setKeepAliveInterval) sock.setKeepAliveInterval(KEEPALIVE_INTVL_MS);

  const die = (why) => () => { if (S.ver === myVer) { log(`  disconnect: ${why}`); teardown(why); } };
  req.on('close',  die('client disconnected'));
  res.on('close',  die('response closed'));
  res.on('error',  die('res error'));
  sock.on('error', die('sock error'));
  sock.on('timeout', () => sock.destroy());

  // ── 200 즉시 전송 ─────────────────────────────────────────────────────
  res.writeHead(200, {'Content-Type':'audio/mpeg','Cache-Control':'no-cache','Connection':'close'});

  // ── Playlist 해석 ─────────────────────────────────────────────────────
  let inputUrl;
  try {
    inputUrl = await resolveSource(station.source);
    log(`  URL: ${inputUrl}`);
  } catch (e) {
    log(`  resolve ERROR: ${e.message}`);
    if (S.ver === myVer) teardown(`resolve error`);
    return;
  }
  if (S.ver !== myVer) { log(`  v${myVer} preempted`); return; }
  if (res.writableEnded || req.destroyed) {
    if (S.ver === myVer) teardown('left'); return;
  }

  // ── ffmpeg 시작 ──────────────────────────────────────────────────────
  const startFf = () => {
    if (S.ver !== myVer || res.writableEnded) return;

    S.attempts++;
    const ff = spawnFfmpeg(inputUrl);
    S.ff = ff;
    log(`  ▶ pid=${ff.pid} "${id}" #${S.attempts}  ver=${myVer}`);

    let rxBytes = 0;
    if (S.stallTimer) clearInterval(S.stallTimer);
    S.stallTimer = setInterval(() => {
      if (S.ver !== myVer) return;
      if (rxBytes === 0 && ff.exitCode === null) {
        log(`  STALL pid=${ff.pid}`);
        killByPid(ff, 'stall');
        if (IS_WIN) killAllFfmpeg('stall');
      }
      rxBytes = 0;
    }, STALL_MS);

    ff.stdout.on('data', chunk => {
      if (S.ver !== myVer) return;
      rxBytes += chunk.length;
      if (!res.writableEnded) res.write(chunk);
    });

    ff.stderr.on('data', chunk =>
      chunk.toString().split(/\r?\n/).filter(Boolean)
           .forEach(l => log(`  ff[${ff.pid}]: ${l}`))
    );

    ff.on('error', e => log(`  ff error: ${e.message}`));

    ff.on('exit', (code, sig) => {
      if (S.ff === ff) S.ff = null;
      log(`  ff[${ff.pid}] exit code=${code} sig=${sig}  ver:${myVer}==${S.ver}?${S.ver===myVer}`);
      if (S.ver !== myVer || res.writableEnded || req.destroyed) return;
      if (S.attempts < MAX_RETRIES) {
        log(`  → retry ${S.attempts+1}/${MAX_RETRIES}`);
        setTimeout(startFf, 1500);
      } else {
        teardown('max retries');
      }
    });
  };

  startFf();
});

server.on('clientError', (_, sock) => {
  if (sock.writable) sock.end('HTTP/1.1 400 Bad Request\r\n\r\n');
});

// ── 종료 처리 ────────────────────────────────────────────────────────────
const shutdown = (sig) => {
  log(`${sig} → shutdown`);
  teardown('shutdown');
  setTimeout(() => process.exit(0), 500);
};
process.on('SIGINT',   () => shutdown('SIGINT'));
process.on('SIGTERM',  () => shutdown('SIGTERM'));
process.on('SIGBREAK', () => shutdown('SIGBREAK'));

// ── 서버 시작 ─────────────────────────────────────────────────────────────
server.listen(PORT, () => {
  log(`proxy  port=${PORT}  gain=${GAIN_DB}dB  limit=${LIMIT}  retries=${MAX_RETRIES}`);
  // 시작 시 잔여 ffmpeg 전체 정리
  if (IS_WIN) {
    killAllFfmpeg('startup cleanup');
    log(`startup: cleared orphan ffmpeg.exe`);
  }
});

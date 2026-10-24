/**
 * lan-broadcast.plugin — 局域网投送（客户端采集端）
 *
 * 协议对齐 go-musicfox 的 broadcast（见仓库根目录 lan_broadcast.md）：
 *   GET /events  Server-Sent Events，event: nowplaying + data: <JSON>
 *   GET /cover   当前封面图片
 *
 * 分工：
 *   - 本插件跑在**页面上下文**，负责采集（歌曲/歌词窗口/进度/封面）并 POST 给原生；
 *   - 服务端由原生 DLL 提供（页面 JS 无法监听端口）。
 *
 * 数据来源：
 *   ncm.player.current()        → song / lyric(lines, index) / cover
 *   ncm.player.getProgress()    → 原生精确进度（playedTime 秒）
 *   ncm.player.control.isPlaying() → 播放状态
 *
 * 可调参数见下面 CONFIG。
 */
(function () {
  if (window.__LAN_BROADCAST_PLUGIN__) return;
  window.__LAN_BROADCAST_PLUGIN__ = 1;

  var CONFIG = {
    port: 8765,           /* 与服务端 broadcast.json 的 port 保持一致 */
    windowBefore: 4,      /* 歌词窗口：当前行之前 */
    windowAfter: 8,       /* 歌词窗口：当前行之后 */
    coverSize: 512,       /* 封面请求分辨率 */
    tickMs: 2000,         /* 周期性刷新（校正漂移） */
    version: 1
  };

  /* 实际端口由原生决定（8765 可能被 go-musicfox 等程序占用，
   * 原生会在 8765..8774 里挑一个空闲的），这里通过既有通道查询。 */
  var BASE = 'http://127.0.0.1:' + CONFIG.port;
  var PORT_READY = false;

  /* ★ 插件脚本可能比宿主的 player API 更早执行（实测就是这样），
   *   所以这里**等待** player 初始化，而不是直接 return。 */
  /* 端口解析要重试：插件可能比原生服务先起来（实测会拿到 0）。 */
  function resolvePort(cb, tries) {
    tries = tries || 0;
    fetch('http://127.0.0.1:38021/bcast-port', { cache: 'no-store' })
      .then(function (r) { return r.text(); })
      .then(function (t) {
        var p = parseInt(String(t).trim(), 10);
        if (p > 0) {
          CONFIG.port = p;
          BASE = 'http://127.0.0.1:' + p;
          PORT_READY = true;
          cb();
          return;
        }
        retry();
      })
      .catch(function () { retry(); });

    function retry() {
      if (tries >= 40) {                 /* 最多等 20 秒，之后用默认端口 */
        PORT_READY = true;
        cb();
        return;
      }
      setTimeout(function () { resolvePort(cb, tries + 1); }, 500);
    }
  }

  function whenPlayerReady(cb, tries) {
    tries = tries || 0;
    if (window.ncm && window.ncm.player) { cb(); return; }
    if (tries > 120) {                        /* 最多等 60 秒 */
      try { ncm.log('warn', '[lan-broadcast] 等待 player API 超时（60s），放弃'); } catch (e) {}
      return;
    }
    setTimeout(function () { whenPlayerReady(cb, tries + 1); }, 500);
  }

  var seq = 0;
  var lastKey = '';
  var lastSongId = '';
  var lastCoverKey = '';
  var stopped = false;

  /* ── 歌词：拆出原文与翻译 ───────────────────────────────────────────── */
  function splitLine(raw) {
    var parts = String(raw || '').split(/\r?\n/).map(function (s) { return s.trim(); }).filter(Boolean);
    if (!parts.length) return { text: '', translation: '' };
    return { text: parts[0], translation: parts.slice(1).join(' ') };
  }

  /* ── 构造文档约定的 nowplaying 负载 ─────────────────────────────────── */
  function buildSnapshot() {
    var s = window.ncm.player.current();
    var prog = window.ncm.player.getProgress ? window.ncm.player.getProgress() : null;
    var playing = false;
    try {
      playing = !!(window.ncm.player.control && window.ncm.player.control.isPlaying());
    } catch (e) {}

    var lines = (s.lyric && s.lyric.lines) || [];
    var idx = (s.lyric && typeof s.lyric.index === 'number') ? s.lyric.index : -1;

    /* 播放位置：优先原生进度，其次当前行时间，最后 0 */
    var positionMs = 0;
    if (prog && typeof prog.playedTime === 'number' && (Date.now() - prog.at) < 5000) {
      positionMs = Math.round(prog.playedTime * 1000);
    } else if (idx >= 0 && lines[idx]) {
      positionMs = Math.round(lines[idx].t * 1000);
    }

    var durationMs = (s.audio && s.audio.durationMs) ? s.audio.durationMs : 0;

    /* 歌词窗口 */
    var win = [];
    if (lines.length && idx >= 0) {
      var from = Math.max(0, idx - CONFIG.windowBefore);
      var to = Math.min(lines.length - 1, idx + CONFIG.windowAfter);
      for (var i = from; i <= to; i++) {
        var cur = lines[i];
        var next = lines[i + 1];
        var startMs = Math.round((cur.t || 0) * 1000);
        var endMs = next ? Math.round((next.t || 0) * 1000) : (startMs + 4000);
        /* 载荷已把原文/翻译拆好；只有旧数据才需要按换行再分一次 */
        var text = cur.text || '';
        var translation = cur.translation || '';
        if (!translation && text.indexOf('\n') >= 0) {
          var t = splitLine(text);
          text = t.text;
          translation = t.translation;
        }
        win.push({
          index: i,
          startMs: startMs,
          endMs: endMs,
          text: text,
          translation: translation
        });
      }
    }

    var state = playing ? 'playing' : (durationMs ? 'paused' : 'stopped');

    return {
      type: 'nowplaying',
      version: CONFIG.version,
      seq: ++seq,
      playing: playing,
      state: state,
      positionMs: positionMs,
      durationMs: durationMs,
      updatedAtMs: Date.now(),
      song: {
        /* 用载荷解析出的可信 ID（与歌名/歌词同源），而不是音频的 preload ID */
        id: (s.song && s.song.id) || (s.audio && s.audio.songId ? parseInt(s.audio.songId, 10) || 0 : 0),
        name: s.song && s.song.name ? s.song.name : '',
        artists: s.song && s.song.artist ? s.song.artist : '',
        album: s.song && s.song.album ? s.song.album : '',
        durationMs: durationMs,
        picUrl: s.cover || '',
        coverUrl: '/cover'
      },
      lyric: win
    };
  }

  /* ── 推送快照 ───────────────────────────────────────────────────────── */
  function publish(snap) {
    try {
      fetch(BASE + '/publish', {
        method: 'POST',
        mode: 'no-cors',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(snap)
      }).catch(function () {});
    } catch (e) {}
  }

  /* ── 封面：拉成大图后用 canvas 转 JPEG（避免依赖 scheme/跨域） ───────── */
  function pushCover(src, size) {
    if (!src) return;
    var url = window.ncm.player.getCoverURL
      ? window.ncm.player.getCoverURL(size || CONFIG.coverSize)
      : src;

    var img = new Image();
    img.onload = function () {
      try {
        var c = document.createElement('canvas');
        c.width = img.naturalWidth || img.width;
        c.height = img.naturalHeight || img.height;
        if (!c.width || !c.height) return;
        c.getContext('2d').drawImage(img, 0, 0);
        var b64 = c.toDataURL('image/jpeg', 0.9).split(',')[1];
        var bin = atob(b64);
        var buf = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
        fetch(BASE + '/cover?type=image/jpeg', {
          method: 'POST', mode: 'no-cors', body: buf
        }).catch(function () {});
        try { ncm.log('info', '[lan-broadcast] 封面已推送 ' + c.width + 'x' + c.height); } catch (e) {}
      } catch (e) {}
    };
    img.onerror = function () {};
    img.src = url;
  }

  /* ── 主循环：变化即推 + 定期刷新 ─────────────────────────────────────── */
  function tick() {
    if (stopped) return;
    var snap;
    try { snap = buildSnapshot(); } catch (e) { return; }

    var key = snap.song.id + '|' + snap.playing + '|' + (snap.lyric.length ? snap.lyric[0].index : -1);
    if (key !== lastKey) {
      lastKey = key;
      publish(snap);
    } else {
      /* 周期性刷新：seq 递增，便于客户端判重 */
      publish(snap);
    }

    /* 歌曲变化 **或** 封面地址变化时重推（页面形态切换时封面来源会变） */
    var coverKey = String(snap.song.id) + '|' + snap.song.picUrl;
    if (coverKey !== lastCoverKey) {
      lastCoverKey = coverKey;
      if (snap.song.picUrl) pushCover(snap.song.picUrl, CONFIG.coverSize);
    }
  }

  /* 热重载支持：清理定时器与守卫标记 */
  function cleanup() {
    stopped = true;
    try { delete window.__LAN_BROADCAST_PLUGIN__; } catch (e) { window.__LAN_BROADCAST_PLUGIN__ = undefined; }
  }
  try { if (typeof ncm !== "undefined" && ncm.onDispose) ncm.onDispose(cleanup); } catch (e) {}

  resolvePort(function () {
    whenPlayerReady(function () {
      setInterval(tick, CONFIG.tickMs);
      setTimeout(tick, 300);
      try { ncm.log('info', '[lan-broadcast] 已启动，SSE 端点为 ' + BASE + '/events'); } catch (e) {}
    });
  });
})();

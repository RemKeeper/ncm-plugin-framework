/**
 * pet-override.plugin —— 用 dsh-pet 的 webm 动画接管网易云桌宠
 *
 * 只在桌宠窗口（subApp.html?route=pet-desktop）里生效：
 *   1) !important CSS 隐藏原生 <video>（App 每帧重置 inline visibility，普通样式压不住）
 *   2) 在 #pet-desktop-renderer 里双缓冲播放 data/pet-override/assets/*.webm
 *   3) 待机 → 随机动作 → 待机；点击回应；拖拽时播「被抓起悬空」并轻微倾斜跟手
 *
 * 交互事件来源：**页面收不到鼠标按键**（实测 OSR 只转发 mousemove），
 * 由原生 pet_hook.c 子类化桌宠窗口 WndProc，经 bcast 的 SSE 推 `event: pet`。
 *
 * 素材来源：https://github.com/PC2005-cloud/dsh-pet （MIT）
 */

(function () {
  'use strict';

  /* 只在桌宠窗口跑：主窗口的 app.html 没有 route 参数 */
  if (!/[?&]route=pet-desktop/.test(location.search)) return;

  var SLUG = 'pet-override';
  var API = 'http://127.0.0.1:38021';
  var assetUrl = function (name) {
    return API + '/asset?path=' + encodeURIComponent(SLUG) + '/assets/' + encodeURIComponent(name) + '.webm';
  };

  var pools = { idle: [], turn: [], clicks: [], drag: [], categories: [] };
  var videos = [];          // 双缓冲
  var front = 0;
  var curName = '';
  var endedHandler = null;
  var stage = null, bubble = null, hideStyle = null;
  var disposed = false;
  var bubbleTimer = 0;
  var dragging = false;
  var esRef = null;
  var lastClickAt = 0;
  var lastSongId = null;
  var hiddenPaused = false;

  /* 点击/拖拽时的台词（想接 AI 就改这里） */
  var LINES = [
    '嘿嘿，被摸到了～', '主人今天也要开心呀', '这首歌好听！', '再点一下嘛',
    '我在陪你听歌哦', '要不要一起摇摆？', '别戳啦，痒痒的', '今天状态不错～',
    '摸摸头会长高的', '我在认真听！'
  ];
  var DRAG_LINES = ['哇——放我下来！', '要飞起来了！', '别晃啦别晃啦', '这高度有点吓人…'];

  function pick(arr) {
    if (!arr || !arr.length) return null;
    return arr[Math.floor(Math.random() * arr.length)];
  }

  /* ── 1. 隐藏原生宠物 ─────────────────────────────────────────────── */
  function hideNative() {
    if (!hideStyle) {
      hideStyle = document.createElement('style');
      hideStyle.id = 'pet-override-hide';
      (document.head || document.documentElement).appendChild(hideStyle);
    }
    /* ★ 必须排除我们自己的节点：'#pet-desktop-renderer video' 也会匹配到 stage 里的 video，
     *   那条 !important 会把我们自己的画面一起隐藏（踩过一次）。 */
    hideStyle.textContent =
      '#pet-desktop-renderer video:not(.pet-ov-v){visibility:hidden !important}' +
      '#pet-desktop-renderer>div:not(#pet-override-stage){visibility:hidden !important}';
  }

  /* ── 2. 舞台 ─────────────────────────────────────────────────────── */
  function buildStage(host) {
    if (stage && stage.parentElement) return stage;
    stage = document.createElement('div');
    stage.id = 'pet-override-stage';
    stage.style.cssText =
      'position:absolute;left:0;top:0;width:100%;height:100%;' +
      'z-index:50;display:flex;align-items:flex-end;justify-content:center;' +
      'pointer-events:auto;cursor:pointer;will-change:transform;transition:transform .12s linear;';

    for (var i = 0; i < 2; i++) {
      var v = document.createElement('video');
      v.muted = true;
      v.playsInline = true;
      v.setAttribute('playsinline', 'true');
      v.preload = 'auto';
      v.className = 'pet-ov-v';
      /* 素材是 640×360（16:9），容器 200×200：cover + 底部对齐让宠物撑满高度 */
      v.style.cssText =
        'position:absolute;left:0;top:0;width:100%;height:100%;' +
        'object-fit:cover;object-position:center bottom;background:transparent;visibility:hidden;';
      stage.appendChild(v);
      videos.push(v);
    }

    bubble = document.createElement('div');
    bubble.id = 'pet-override-bubble';
    bubble.style.cssText =
      'position:absolute;left:50%;top:0;transform:translate(-50%,-100%);' +
      'max-width:180px;padding:7px 10px;border-radius:10px;' +
      'background:rgba(255,255,255,.96);color:#2b2b2b;font-size:12px;line-height:1.35;' +
      'box-shadow:0 4px 14px rgba(0,0,0,.18);white-space:normal;word-break:break-word;' +
      'opacity:0;transition:opacity .18s;pointer-events:none;z-index:60;display:none;';
    stage.appendChild(bubble);

    host.appendChild(stage);
    return stage;
  }

  function showBubble(text) {
    if (!bubble || !text) return;
    bubble.textContent = text;
    bubble.style.display = 'block';
    bubble.style.opacity = '1';
    clearTimeout(bubbleTimer);
    bubbleTimer = setTimeout(function () {
      if (!bubble) return;
      bubble.style.opacity = '0';
      setTimeout(function () { if (bubble && bubble.style.opacity === '0') bubble.style.display = 'none'; }, 200);
    }, 2600);
  }

  /* ── 3. 播放 ─────────────────────────────────────────────────────── */
  function play(name, once) {
    if (disposed || !videos.length) return;
    if (!name) return;
    var target = videos[1 - front];
    var prev = videos[front];
    curName = name;

    var gen = (target.__gen = (target.__gen || 0) + 1);

    /* ★ 先挂事件再换源：素材被缓存时 loadeddata 可能在 play() 返回前就触发，
     *   反过来写会导致 video 一直停在 visibility:hidden（实测过）。 */
    target.onloadeddata = function () {
      if (disposed || target.__gen !== gen) return;
      target.style.visibility = 'visible';
      prev.style.visibility = 'hidden';
      try { prev.pause(); } catch (e) {}
      front = 1 - front;
    };

    if (endedHandler) target.removeEventListener('ended', endedHandler);
    endedHandler = function () {
      if (disposed || target.__gen !== gen) return;
      if (!once) return;              /* loop 的不会 ended */
      if (dragging) return;           /* 拖拽中不自动回待机 */
      playIdle();
    };
    target.addEventListener('ended', endedHandler);

    target.loop = !once;
    target.muted = true;
    target.src = assetUrl(name);
    try { target.currentTime = 0; } catch (e) {}

    var p = target.play();
    if (p && p.catch) p.catch(function () { /* 自动播放被拒就静默 */ });

    /* 兜底：某些封装可能不触发 loadeddata，300ms 后仍未显示就强制显形 */
    setTimeout(function () {
      if (disposed || target.__gen !== gen) return;
      if (target.style.visibility !== 'visible' && target.videoWidth > 0) {
        target.style.visibility = 'visible';
        prev.style.visibility = 'hidden';
        front = (target === videos[1]) ? 1 : 0;
      }
    }, 300);
  }

  function playIdle() {
    if (disposed || dragging) return;
    var roll = Math.random();
    var next;
    if (roll < 0.55 || !pools.categories.length) {
      next = pick(pools.idle) || pick(pools.categories);
      play(next, false);              // 待机循环
    } else {
      next = pick(pools.categories);
      play(next, true);               // 随机动作播一次 → 回待机
    }
  }

  /* 换歌反应：随一个短动作 + 气泡报歌名 */
  function onSongChange(song) {
    if (disposed || dragging || !song) return;
    var act = pick(pools.categories);
    if (act) play(act, true);
    var name = String(song.name || "").slice(0, 16);
    if (name) showBubble("♪ " + name);
    try { ncm.emit("pet:song", song); } catch (e) {}
  }

  function doClick(source) {
    if (disposed) return;
    var now = Date.now();
    if (now - lastClickAt < 220) return;   /* 原生 + 合成点击可能重复，去抖 */
    lastClickAt = now;
    var click = pick(pools.clicks);
    if (click) play(click, true);
    showBubble(pick(LINES));
    try { ncm.emit('pet:click', { name: curName, source: source }); } catch (err) {}
  }

  /* ── 4. 原生交互事件（SSE） ──────────────────────────────────────── */
  function onPetEvent(d) {
    if (disposed || !d) return;
    switch (d.type) {
      case 'attach':
        ncm.log('info', '原生交互钩子已就绪');
        break;
      case 'down':
        /* 按下瞬间先不切动画，避免轻点也闪一下拖拽动作 */
        break;
      case 'dragstart':
        dragging = true;
        play(pick(pools.drag) || pick(pools.idle), false);
        showBubble(pick(DRAG_LINES));
        break;
      case 'drag':
        /* 跟手：按位移做轻微倾斜，视觉上"被拎着晃" */
        if (stage) {
          var tilt = Math.max(-12, Math.min(12, (d.x || 0) / 12));
          stage.style.transform = 'rotate(' + tilt + 'deg)';
        }
        break;
      case 'drop':
        dragging = false;
        if (stage) stage.style.transform = '';
        playIdle();
        break;
      case 'click':
        doClick('native');
        break;
      default:
        break;
    }
  }

  function connectEvents() {
    fetch(API + '/bcast-port', { cache: 'no-store' })
      .then(function (r) { return r.text(); })
      .then(function (t) {
        if (disposed) return;
        var port = parseInt(String(t).trim(), 10);
        if (!(port > 0)) throw new Error('没有 bcast 端口');
        var es = new EventSource('http://127.0.0.1:' + port + '/events');
        esRef = es;
        es.addEventListener('pet', function (e) {
          var d = null;
          try { d = JSON.parse(e.data); } catch (err) { return; }
          onPetEvent(d);
        });
        es.onerror = function () { if (!disposed) ncm.log('warn', '交互事件流断开，等待重连'); };
        /* 跟歌：lan-broadcast 插件把当前播放推到同一条 SSE 上，换歌时做个反应 */
        es.addEventListener("nowplaying", function (e) {
          if (disposed) return;
          var s = null;
          try { s = JSON.parse(e.data); } catch (err) { return; }
          var id = s && s.song && s.song.id;
          if (!id || id === lastSongId) return;
          var first = !lastSongId;
          lastSongId = id;
          if (first || dragging) return;   /* 首次快照/拖拽中不打断 */
          onSongChange(s.song);
        });
        ncm.log('info', '已订阅原生交互事件（端口 ' + port + '）');
      })
      .catch(function (e) {
        ncm.log('warn', '订阅交互事件失败: ' + (e && e.message) + '（点击/拖拽不可用）');
      });
  }

  /* ── 5. 素材发现 ─────────────────────────────────────────────────── */
  function classify(names) {
    var set = {};
    names.forEach(function (n) { set[n] = 1; });
    function keep(list) { return (list || []).filter(function (n) { return set[n]; }); }

    pools.idle    = keep(['待机呼吸休闲']);
    pools.turn    = keep(['东张西望']);
    pools.drag    = keep(['被鼠标拖拽悬空反馈']);
    pools.clicks  = names.filter(function (n) { return n.indexOf('点击回应') === 0; });

    var taken = {};
    [].concat(pools.idle, pools.turn, pools.drag, pools.clicks).forEach(function (n) { taken[n] = 1; });
    pools.categories = names.filter(function (n) { return !taken[n]; });

    if (!pools.idle.length) pools.idle = names.slice(0, Math.min(3, names.length));
    if (!pools.idle.length) pools.idle = pools.categories.slice(0, 1);
  }

  /* ── 6. 启动 ─────────────────────────────────────────────────────── */
  function start(host) {
    hideNative();
    buildStage(host);
    connectEvents();

    ncm.fs.list('assets').then(function (rows) {
      if (disposed) return;
      var names = (rows || [])
        .filter(function (r) { return r && !r.dir && /\.webm$/i.test(r.name); })
        .map(function (r) { return r.name.replace(/\.webm$/i, ''); });
      if (!names.length) {
        ncm.log('warn', '没找到素材：请把 *.webm 放到 data/pet-override/assets/');
        return;
      }
      classify(names);
      ncm.log('info', '素材 ' + names.length + ' 个 · 待机 ' + pools.idle.length +
                       ' · 拖拽 ' + pools.drag.length +
                       ' · 点击 ' + pools.clicks.length + ' · 随机 ' + pools.categories.length);
      play(pools.idle[0] || pools.categories[0], false);
    }).catch(function (e) {
      ncm.log('error', '列素材失败: ' + (e && e.message));
    });
  }

  var tries = 0;
  (function waitRenderer() {
    if (disposed) return;
    var host = document.getElementById('pet-desktop-renderer');
    if (host) { start(host); return; }
    if (++tries > 150) { ncm.log('warn', '等不到 #pet-desktop-renderer，放弃'); return; }
    setTimeout(waitRenderer, 200);
  })();

  /* ── 7. 卸载 ─────────────────────────────────────────────────────── */
  ncm.onDispose(function () {
    disposed = true;
    clearTimeout(bubbleTimer);
    if (esRef) { try { esRef.close(); } catch (e) {} esRef = null; }
    if (stage) {
      videos.forEach(function (v) { try { v.pause(); v.removeAttribute('src'); v.load(); } catch (e) {} });
      stage.remove();
      stage = null;
    }
    if (hideStyle) { hideStyle.remove(); hideStyle = null; }
    videos = [];
    ncm.log('info', 'pet-override 已卸载');
  });

  /* ── 8. 窗口隐藏时暂停，回到前台再继续（省 CPU/GPU） ──────────────── */
  document.addEventListener('visibilitychange', function () {
    if (disposed) return;
    var hidden = document.hidden;
    if (hidden && !hiddenPaused) {
      hiddenPaused = true;
      videos.forEach(function (v) { try { v.pause(); } catch (e) {} });
    } else if (!hidden && hiddenPaused) {
      hiddenPaused = false;
      var vis = videos.filter(function (v) { return getComputedStyle(v).visibility === 'visible'; })[0];
      if (vis) { var p = vis.play(); if (p && p.catch) p.catch(function () {}); }
      else playIdle();
    }
  });

  ncm.log('info', '桌宠接管已启动');
})();

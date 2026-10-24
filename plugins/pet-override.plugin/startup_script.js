/**
 * pet-override.plugin —— 用 dsh-pet 的 webm 动画接管网易云桌宠
 *
 * 只在桌宠窗口（subApp.html?route=pet-desktop）里生效：
 *   1) 用 !important CSS 隐藏原生的 21 个 <video>（App 每帧会重置 inline visibility，
 *      普通样式压不住，必须 !important）
 *   2) 在 #pet-desktop-renderer 里放自己的双缓冲 <video>，素材来自
 *      data/pet-override/assets/*.webm（经原生 /asset 端点以 video/webm 提供）
 *   3) 待机 → 随机动作 → 待机；点击触发「点击回应」片段
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

  var pools = { idle: [], turn: [], clicks: [], categories: [] };
  var videos = [];          // 双缓冲
  var front = 0;            // 当前前台下标
  var curName = '';
  var endedHandler = null;
  var stage = null, bubble = null, hideStyle = null;
  var disposed = false;
  var bubbleTimer = 0;

  /* 点击时随机说一句（不依赖任何在线服务；想接 AI 就改这里） */
  var LINES = [
    '嘿嘿，被摸到了～', '主人今天也要开心呀', '这首歌好听！', '再点一下嘛',
    '我在陪你听歌哦', '要不要一起摇摆？', '别戳啦，痒痒的', '今天状态不错～',
    '摸摸头会长高的', '我在认真听！'
  ];

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
      'pointer-events:auto;cursor:pointer;';

    for (var i = 0; i < 2; i++) {
      var v = document.createElement('video');
      v.muted = true;
      v.playsInline = true;
      v.setAttribute('playsinline', 'true');
      v.preload = 'auto';
      v.className = 'pet-ov-v';
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
    stage.addEventListener('click', onClick);
    return stage;
  }

  function showBubble(text) {
    if (!bubble) return;
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
      target.style.visibility = "visible";
      prev.style.visibility = "hidden";
      try { prev.pause(); } catch (e) {}
      front = 1 - front;
    };

    if (endedHandler) target.removeEventListener("ended", endedHandler);
    endedHandler = function () {
      if (disposed || target.__gen !== gen) return;
      if (!once) return;              /* loop 的不会 ended */
      playIdle();
    };
    target.addEventListener("ended", endedHandler);

    target.loop = !once;
    target.muted = true;
    target.src = assetUrl(name);
    try { target.currentTime = 0; } catch (e) {}

    var p = target.play();
    if (p && p.catch) p.catch(function () { /* 自动播放被拒就静默 */ });

    /* 兜底：某些封装可能不触发 loadeddata，300ms 后仍未显示就强制显形 */
    setTimeout(function () {
      if (disposed || target.__gen !== gen) return;
      if (target.style.visibility !== "visible" && target.videoWidth > 0) {
        target.style.visibility = "visible";
        prev.style.visibility = "hidden";
        front = (target === videos[1]) ? 1 : 0;
      }
    }, 300);
  }

  function playIdle() {
    if (disposed) return;
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

  function onClick(e) {
    if (disposed) return;
    e.stopPropagation();
    var click = pick(pools.clicks);
    if (click) play(click, true);
    showBubble(pick(LINES));
    try { ncm.emit('pet:click', { name: curName }); } catch (err) {}
  }

  /* ── 4. 素材发现：把 webm 名按 dsh-pet 的命名习惯归类 ─────────────── */
  function classify(names) {
    var set = {};
    names.forEach(function (n) { set[n] = 1; });

    function keep(list) { return (list || []).filter(function (n) { return set[n]; }); }

    pools.idle = keep(['待机呼吸休闲']);
    pools.turn = keep(['东张西望']);
    pools.clicks = names.filter(function (n) { return n.indexOf('点击回应') === 0; });

    var taken = {};
    [].concat(pools.idle, pools.turn, pools.clicks, ['被鼠标拖拽悬空反馈']).forEach(function (n) { taken[n] = 1; });
    pools.categories = names.filter(function (n) { return !taken[n]; });

    /* 兜底：素材命名不符合预期时，别让宠物不动 */
    if (!pools.idle.length) pools.idle = names.slice(0, Math.min(3, names.length));
    if (!pools.idle.length) pools.idle = pools.categories.slice(0, 1);
  }

  /* ── 5. 启动 ─────────────────────────────────────────────────────── */
  function start(host) {
    hideNative();
    buildStage(host);

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
                       ' · 点击 ' + pools.clicks.length + ' · 随机动作 ' + pools.categories.length);
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

  /* ── 6. 卸载 ─────────────────────────────────────────────────────── */
  ncm.onDispose(function () {
    disposed = true;
    clearTimeout(bubbleTimer);
    if (stage) {
      stage.removeEventListener('click', onClick);
      videos.forEach(function (v) { try { v.pause(); v.removeAttribute('src'); v.load(); } catch (e) {} });
      stage.remove();
      stage = null;
    }
    if (hideStyle) { hideStyle.remove(); hideStyle = null; }
    videos = [];
    ncm.log('info', 'pet-override 已卸载');
  });

  ncm.log('info', '桌宠接管已启动（' + location.search.slice(0, 40) + '）');
})();

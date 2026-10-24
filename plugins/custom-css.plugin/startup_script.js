/**
 * custom-css.plugin — 设置界面里的「自定义 CSS」面板
 *
 * 能力：
 *   1. 在设置页的锚点导航里插入「自定义 CSS」一项（沿用宿主样式）
 *   2. 面板里编辑 CSS，立即生效（注入 <style>，并同步到同源 iframe）
 *   3. 持久化：POST 给原生，存到 %LOCALAPPDATA%\NCMPlugin\custom.css
 *   4. DevTools 入口：显示是否已启用 + 可复制的启动命令
 *
 * 依赖宿主 API：无（只用 DOM + 我们自己的原生通道 38021）
 */
(function () {
  if (window.__NCM_CUSTOM_CSS_PLUGIN__) return;
  window.__NCM_CUSTOM_CSS_PLUGIN__ = 1;

  var API = 'http://127.0.0.1:38021';
  var STYLE_ID = 'ncm-custom-css-style';
  var PANEL_ID = 'ncm-custom-css-panel';
  var NAV_ID = 'ncm-custom-css-nav';
  var ACCENT = '#c62f2f';

  /* 热重载支持：把需要清理的东西记下来（interval / 观察者 / DOM） */
  var dirty = { timers: [], observers: [], nodes: [] };

  function regTimer(id) { dirty.timers.push(id); return id; }

  function cleanup() {
    dirty.timers.forEach(function (t) { try { clearInterval(t); } catch (e) {} });
    dirty.observers.forEach(function (o) { try { o.disconnect(); } catch (e) {} });
    dirty.nodes.forEach(function (n) { try { if (n && n.parentNode) n.parentNode.removeChild(n); } catch (e) {} });
    ['ncm-custom-css-style', 'ncm-custom-css-panel', 'ncm-custom-css-nav'].forEach(function (id) {
      var el = document.getElementById(id);
      if (el && el.parentNode) el.parentNode.removeChild(el);
    });
    try { delete window.__NCM_CUSTOM_CSS_PLUGIN__; } catch (e) { window.__NCM_CUSTOM_CSS_PLUGIN__ = undefined; }
  }

  var state = {
    css: '',
    loaded: false,
    devtools: null,     /* true / false / null(未知) */
    status: '就绪'
  };

  function log(msg) {
    try { if (window.ncm && window.ncm.log) window.ncm.log('info', '[custom-css] ' + msg); } catch (e) {}
  }

  /* ── 应用 CSS：主文档 + 同源 iframe ─────────────────────────────────── */
  function applyToDoc(doc, css) {
    if (!doc || !doc.head) return false;
    var el = doc.getElementById(STYLE_ID);
    if (!el) {
      el = doc.createElement('style');
      el.id = STYLE_ID;
      (doc.head || doc.documentElement).appendChild(el);
    }
    el.textContent = css || '';
    return true;
  }

  function applyAll(css) {
    var n = applyToDoc(document, css) ? 1 : 0;
    try {
      var frames = document.querySelectorAll('iframe');
      for (var i = 0; i < frames.length; i++) {
        var fd = null;
        try { fd = frames[i].contentDocument; } catch (e) { fd = null; }
        if (fd && fd.head) n += applyToDoc(fd, css) ? 1 : 0;
      }
    } catch (e) {}
    return n;
  }

  /* ── 持久化（原生文件） ─────────────────────────────────────────────── */
  function loadSaved() {
    return fetch(API + '/custom-css', { cache: 'no-store' })
      .then(function (r) { return r.text(); })
      .then(function (t) {
        state.css = t || '';
        state.loaded = true;
        applyAll(state.css);
        syncTextarea();
        setStatus(state.css ? ('已载入 ' + state.css.length + ' 字符') : '暂无保存的 CSS');
        return state.css;
      })
      .catch(function () { state.loaded = true; setStatus('读取失败（原生通道未就绪）'); });
  }

  function save(css) {
    return fetch(API + '/custom-css', {
      method: 'POST',
      mode: 'no-cors',
      body: css || ''
    }).then(function () {
      setStatus('已保存（' + (css || '').length + ' 字符）');
    }).catch(function () {
      setStatus('保存失败');
    });
  }

  /* ── UI ────────────────────────────────────────────────────────────── */
  var textarea = null, statusEl = null, devtoolsEl = null;

  function setStatus(t) {
    state.status = t;
    if (statusEl) statusEl.textContent = t;
  }

  function syncTextarea() {
    if (textarea && textarea.value !== state.css) textarea.value = state.css;
  }

  function el(tag, style, text) {
    var e = document.createElement(tag);
    if (style) e.setAttribute('style', style);
    if (text != null) e.textContent = text;
    return e;
  }

  function button(label, primary, onclick) {
    var b = el('button', null, label);
    b.setAttribute('style',
      'margin-right:8px;padding:5px 14px;border-radius:14px;cursor:pointer;font-size:12px;' +
      'border:1px solid ' + (primary ? ACCENT : 'rgba(128,128,128,.45)') + ';' +
      'background:' + (primary ? ACCENT : 'transparent') + ';' +
      'color:' + (primary ? '#fff' : 'inherit') + ';');
    b.onclick = onclick;
    return b;
  }

  function buildPanel() {
    var box = el('div',
      'margin:28px 0 40px 0;padding:18px 20px;border-radius:10px;' +
      'border:1px solid rgba(128,128,128,.22);background:rgba(128,128,128,.05);font-size:13px;');
    box.id = PANEL_ID;

    var title = el('div', 'font-size:15px;font-weight:600;margin-bottom:6px;', '自定义 CSS');
    box.appendChild(title);
    box.appendChild(el('div', 'opacity:.65;margin-bottom:12px;line-height:1.6;',
      '修改会立即注入页面（含同源 iframe），并保存到 %LOCALAPPDATA%\\NCMPlugin\\custom.css。'));

    textarea = el('textarea');
    textarea.setAttribute('spellcheck', 'false');
    textarea.setAttribute('placeholder',
      '/* 示例 */\n/* 隐藏侧边栏 */\n.left-aside { display: none !important; }\n\n/* 歌词字号 */\n[class*=lyric] .line { font-size: 20px; }');
    textarea.setAttribute('style',
      'width:100%;height:190px;box-sizing:border-box;padding:10px 12px;border-radius:8px;' +
      'border:1px solid rgba(128,128,128,.3);background:rgba(0,0,0,.06);color:inherit;' +
      'font:12px/1.6 Consolas,Menlo,monospace;resize:vertical;outline:none;');
    textarea.oninput = function () {
      state.css = textarea.value;
      applyAll(state.css);
      setStatus('预览中（未保存）');
    };
    box.appendChild(textarea);

    var bar = el('div', 'margin-top:12px;display:flex;align-items:center;flex-wrap:wrap;');
    bar.appendChild(button('保存并应用', true, function () {
      state.css = textarea.value;
      applyAll(state.css);
      save(state.css);
      log('已保存 ' + state.css.length + ' 字符');
    }));
    bar.appendChild(button('仅应用', false, function () {
      state.css = textarea.value;
      applyAll(state.css);
      setStatus('已应用（' + state.css.length + ' 字符，未保存）');
    }));
    bar.appendChild(button('重新载入', false, function () { loadSaved(); }));
    bar.appendChild(button('清空', false, function () {
      state.css = '';
      syncTextarea();
      applyAll('');
      save('');
      setStatus('已清空');
    }));
    statusEl = el('span', 'opacity:.6;font-size:12px;', state.status);
    bar.appendChild(statusEl);
    box.appendChild(bar);

    /* DevTools 区块 */
    var dt = el('div', 'margin-top:16px;padding-top:14px;border-top:1px dashed rgba(128,128,128,.25);');
    dt.appendChild(el('div', 'font-weight:600;margin-bottom:6px;', '开发：DevTools'));
    devtoolsEl = el('div', 'opacity:.75;line-height:1.7;font-size:12px;', '正在检测…');
    dt.appendChild(devtoolsEl);

    var cmd = '"C:\\Program Files\\NetEase\\CloudMusic\\cloudmusic.exe" --remote-debugging-port=9222 --remote-allow-origins=*';
    var cmdBox = el('div', null, cmd);
    cmdBox.setAttribute('style',
      'margin:8px 0;padding:8px 10px;border-radius:6px;background:rgba(0,0,0,.08);' +
      'font:11px/1.6 Consolas,Menlo,monospace;word-break:break-all;user-select:text;');
    dt.appendChild(cmdBox);

    var dtBar = el('div');
    dtBar.appendChild(button('复制启动命令', false, function () {
      try {
        var ta = document.createElement('textarea');
        ta.value = cmd;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
        setStatus('启动命令已复制');
      } catch (e) { setStatus('复制失败'); }
    }));
    dtBar.appendChild(button('检测端口', false, function () { probeDevtools(); }));
    dt.appendChild(dtBar);
    box.appendChild(dt);

    return box;
  }

  function probeDevtools() {
    if (devtoolsEl) devtoolsEl.textContent = '检测中…';
    fetch('http://127.0.0.1:9222/json/version', { mode: 'no-cors', cache: 'no-store' })
      .then(function () {
        state.devtools = true;
        if (devtoolsEl) {
          devtoolsEl.innerHTML = '';
          devtoolsEl.appendChild(el('span', 'color:#2e9e4f;font-weight:600;', '✅ 已启用'));
          devtoolsEl.appendChild(el('span', null, '  →  用 Chrome/Edge 打开 '));
          var a = el('a', 'color:' + ACCENT + ';text-decoration:underline;', 'http://127.0.0.1:9222');
          a.href = 'http://127.0.0.1:9222';
          a.target = '_blank';
          devtoolsEl.appendChild(a);
        }
      })
      .catch(function () {
        state.devtools = false;
        if (devtoolsEl) {
          devtoolsEl.innerHTML = '';
          devtoolsEl.appendChild(el('span', 'color:' + ACCENT + ';font-weight:600;', '未启用'));
          devtoolsEl.appendChild(el('span', null,
            '  用下面的命令重启网易云（或在快捷方式目标后追加这些参数），即可在浏览器里调试本页面。'));
        }
      });
  }

  /* ── 设置页注入 ─────────────────────────────────────────────────────── */
  function findSettingsNav() {
    var links = document.querySelectorAll('.cmd-anchor-link');
    if (!links.length) return null;
    /* 找到包含多个锚点的容器（设置页顶部那条） */
    for (var i = 0; i < links.length; i++) {
      var p = links[i].parentElement;
      if (p && p.querySelectorAll('.cmd-anchor-link').length >= 3) return p;
    }
    return links[0].parentElement;
  }

  function findSettingsBody(nav) {
    /* 设置正文通常与导航同一页面滚动容器；用 .setting-part 定位 */
    var parts = document.querySelectorAll('.setting-part');
    if (parts.length) return parts[parts.length - 1].parentElement;
    /* 兜底：用导航的祖先里可滚动的那个 */
    var node = nav;
    while (node && node !== document.body) {
      if (node.scrollHeight > node.clientHeight + 50) return node;
      node = node.parentElement;
    }
    return document.body;
  }

  function injectSettings() {
    if (!document.querySelector('.cmd-anchor-link')) return false;
    if (document.getElementById(PANEL_ID)) return true;   /* 已注入 */

    var nav = findSettingsNav();
    if (!nav) return false;
    var body = findSettingsBody(nav);

    /* 1) 导航项：克隆最后一个锚点，继承宿主样式 */
    var lastLink = nav.querySelectorAll('.cmd-anchor-link');
    var proto = lastLink[lastLink.length - 1];
    var navItem = proto.cloneNode(true);
    navItem.id = NAV_ID;
    /* 把文本替换成我们的标题（保留内部结构，样式才不会跑） */
    var titleEl = navItem.querySelector('.cmd-anchor-link-title') || navItem;
    titleEl.textContent = '自定义 CSS';
    navItem.onclick = function (ev) {
      if (ev && ev.preventDefault) ev.preventDefault();
      var panel = document.getElementById(PANEL_ID);
      if (panel && panel.scrollIntoView) panel.scrollIntoView({ behavior: 'smooth', block: 'start' });
    };
    nav.appendChild(navItem);

    /* 2) 面板 */
    var panel = buildPanel();
    body.appendChild(panel);

    probeDevtools();
    if (state.loaded) { state.css = state.css || ''; applyAll(state.css); }
    syncTextarea();
    log('设置页已注入「自定义 CSS」面板');
    return true;
  }

  /* 设置页是按需挂载的，用低频轮询 + 观察者兜住 */
  regTimer(setInterval(function () { try { injectSettings(); } catch (e) {} }, 1500));
  try {
    var mo = new MutationObserver(function () { try { injectSettings(); } catch (e) {} });
    mo.observe(document.documentElement, { childList: true, subtree: true });
    dirty.observers.push(mo);
  } catch (e) {}

  /* 插件被卸载/热重载时清理自己 */
  try {
    if (typeof ncm !== "undefined" && ncm.onDispose) ncm.onDispose(cleanup);
  } catch (e) {}

  /* ── 启动 ──────────────────────────────────────────────────────────── */
  window.__CUSTOM_CSS__ = {
    apply: function (css) { state.css = css; return applyAll(css); },
    load: loadSaved,
    save: function (css) { state.css = css; applyAll(css); return save(css); },
    get: function () { return state.css; },
    devtools: probeDevtools
  };

  loadSaved();
  setTimeout(function () { try { injectSettings(); } catch (e) {} }, 1000);
})();

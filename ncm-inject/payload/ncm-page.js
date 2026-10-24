/**
 * ncm-page.js — 页面侧插件宿主（跑在网易云的 V8 里，由 ncm-inject 注入）
 *
 * 这一段是整个方案的「功能面」：插件规范、生命周期、热重载、UI 都在这里，
 * C++ 侧只负责把它送进页面并附带 __NCM_HOST__ 数据（见 docs/07 §7.4）。
 *
 * 与参照项目（chromatic / BetterNCM）的对齐关系见 docs/08：
 *   - manifest 字段沿用 BetterNCM 的（manifest_version/name/slug/version/
 *     targets/load_priority/startup_script/injects/hijacks/ncm_version_req）
 *   - 生命周期沿用 chromatic 的 dispose 语义（卸载时统一回调）
 *   - 区别：宿主跑在页面 V8 里而不是独立 QuickJS，native API 由页面侧 shim 提供
 */
(function ncmPageHost() {
  'use strict';

  var H = window.__NCM_HOST__;
  if (!H) {
    console.error('[ncm] __NCM_HOST__ 缺失，注入载荷无法工作');
    return;
  }

  var HOST_VERSION = H.hostVersion || '0.0.0';

  /* ★ 只允许在**主窗口**出现注入角标。
   *   实测宿主会开多个 CEF 页面：
   *     app.html                        ← 主窗口（要显示角标）
   *     subApp.html?route=pet-desktop   ← 桌宠小窗（不该显示）
   *   另有桌面歌词等窗口，也一律不显示。
   *   插件宿主本身仍在各窗口运行（插件可以选择性使用），只有 UI 角标被限制。 */
  var IS_MAIN_WINDOW = (function () {
    try {
      var p = String(location.pathname || "").toLowerCase();
      var href = String(location.href || "").toLowerCase();
      if (p.indexOf("subapp.html") >= 0) return false;
      if (href.indexOf("route=") >= 0) return false;     /* subApp 带 route 参数 */
      return /\/app\.html$/.test(p) || /\/app\.html(\?|#|$)/.test(href);
    } catch (e) { return false; }
  })();
  var PREFIX = '[ncm:' + (H.processType || '?') + ']';

  /* ══ 0. 重入保护 ═══════════════════════════════════════════════════════
   * 同一个页面里可能被注入多次（热重载 = 重新投递）。第二次开始走"卸载旧的
   * + 装载新的"路径，而不是简单叠加 —— 这就是 chromatic reset() 的等价物。 */
  if (window.__NCM__ && window.__NCM__.loaded) {
    console.log(PREFIX + ' 检测到已存在的宿主，先卸载再重装');
    try { window.__NCM__.unloadAll(); } catch (e) { console.warn(PREFIX + ' 卸载异常', e); }
  }

  /* ══ 1. 通用工具 ═══════════════════════════════════════════════════════ */

  var listeners = Object.create(null);

  var g_logBudget = { n: 0, resetAt: 0 };

  function log(level, msg) {
    var line = PREFIX + " " + msg;
    var c = window.console;
    if (level === "error" && c.error) c.error(line);
    else if (level === "warn" && c.warn) c.warn(line);
    else c.log(line);

    /* 同时落盘到原生日志（限流 20 条/秒，避免刷爆） */
    try {
      var now = Date.now();
      if (now - g_logBudget.resetAt > 1000) { g_logBudget.resetAt = now; g_logBudget.n = 0; }
      if (g_logBudget.n++ < 20) {
        fetch("http://127.0.0.1:38021/log", {
          method: "POST", mode: "no-cors",
          body: "[" + (level || "info") + "] " + String(msg)
        }).catch(function () {});
      }
    } catch (e) {}
  }

  /* 语义化版本比较（只支持 manifest 里会用到的那点语法） */
  function parseVer(s) {
    var m = String(s || '0').replace(/^[v=\s]+/, '').split(/[.\-+]/);
    return [parseInt(m[0], 10) || 0, parseInt(m[1], 10) || 0, parseInt(m[2], 10) || 0];
  }
  function cmp(a, b) {
    for (var i = 0; i < 3; i++) {
      if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
  }
  /** 支持 ">=3.0.0 <4"、"^3.1.0"、"~3.1"、"*" */
  function satisfies(version, range) {
    if (!range || range === '*' || range === '') return true;
    var v = parseVer(version);
    var parts = String(range).trim().split(/\s+/);
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i];
      var mt = p.match(/^(>=|<=|>|<|=|\^|~)?\s*(.+)$/);
      if (!mt) continue;
      var op = mt[1] || '=', target = parseVer(mt[2]);
      var d = cmp(v, target);
      if (op === '>=' && d < 0) return false;
      if (op === '<=' && d > 0) return false;
      if (op === '>' && d <= 0) return false;
      if (op === '<' && d >= 0) return false;
      if (op === '=' && d !== 0) return false;
      if (op === '^' && (d < 0 || v[0] !== target[0])) return false;
      if (op === '~' && (d < 0 || v[0] !== target[0] || v[1] !== target[1])) return false;
    }
    return true;
  }

  /* ══ 2. ncm 宿主对象 ═══════════════════════════════════════════════════ */

  /* fs：当前阶段（P2）原生侧还没暴露文件句柄给页面，所以 fs 是"只读 + 有限"的。
   * 插件自己的源码已经由原生读进来了，放进 __NCM__.preloaded 里可读；
   * 其它路径一律返回空。P3 会用原生绑定补齐（docs/08 §8.5）。 */
  var preloaded = Object.create(null);
  function fsReadText(p) {
    if (Object.prototype.hasOwnProperty.call(preloaded, p)) return preloaded[p];
    log('warn', 'fs.readText: 该路径未预载（P2 限制）: ' + p);
    return '';
  }

  var ncm = {
    version: HOST_VERSION,
    ncmVersion: H.ncmVersion || 'unknown',
    processType: H.processType || 'unknown',
    profileDir: H.profileDir || '',
    pluginsDir: H.pluginsDir || '',
    dataRoot: H.dataRoot || '',
    injection: H.injection || '',
    injectedAt: H.injectedAt || '',

    log: function (level, msg) { log(level || 'info', msg); },

    on: function (ev, cb) {
      (listeners[ev] || (listeners[ev] = [])).push(cb);
      return function () { ncm.off(ev, cb); };
    },
    off: function (ev, cb) {
      var a = listeners[ev];
      if (!a) return;
      var i = a.indexOf(cb);
      if (i >= 0) a.splice(i, 1);
    },
    emit: function (ev) {
      var args = Array.prototype.slice.call(arguments, 1);
      (listeners[ev] || []).slice().forEach(function (cb) {
        try { cb.apply(null, args); } catch (e) { log('error', '监听器异常 ' + ev + ': ' + e); }
      });
    },

    fs: {
      readText: fsReadText,
      exists: function (p) { return Object.prototype.hasOwnProperty.call(preloaded, p); },
      /** 列出可读的预载文件（原生只把 manifest 与 startup_script 送进来） */
      readDir: function (dir) {
        var out = [];
        for (var k in preloaded) {
          if (!dir || k.indexOf(dir) === 0) out.push(k);
        }
        return out;
      },
      /** 写盘需要原生配合，本版本页面侧不提供 —— 明确报错而不是静默失败 */
      writeText: function () {
        log('warn', 'fs.writeText 需要原生桥（当前版本页面→原生未开放），已忽略');
        return false;
      }
    },

    config: {
      get: function (key, def) {
        try {
          var v = localStorage.getItem('ncm.config.' + key);
          return v === null ? def : JSON.parse(v);
        } catch (e) { return def; }
      },
      set: function (key, val) {
        try { localStorage.setItem('ncm.config.' + key, JSON.stringify(val)); return true; }
        catch (e) { return false; }
      }
    },

    ui: {
      /** 往页面塞一个自己的 DOM 节点，返回它。插件做界面就靠这个。 */
      mount: function (el) { (document.body || document.documentElement).appendChild(el); return el; },
      el: function (tag, style, text) {
        var e = document.createElement(tag);
        if (style) for (var k in style) e.style[k] = style[k];
        if (text != null) e.textContent = text;
        return e;
      },
      /** 轻提示：右下角浮一个，2.5 秒后自己消失 */
      toast: function (msg, ms) {
        var t = ncm.ui.el('div', {
          position: 'fixed', right: '16px', bottom: '56px', zIndex: '2147483647',
          padding: '8px 12px', borderRadius: '8px', background: 'rgba(0,0,0,.82)',
          color: '#fff', font: '13px/1.5 sans-serif', pointerEvents: 'none',
          maxWidth: '360px'
        }, String(msg));
        t.setAttribute('data-ncm-toast', '1');
        (document.body || document.documentElement).appendChild(t);
        setTimeout(function () { if (t.parentNode) t.parentNode.removeChild(t); }, ms || 2500);
        return t;
      },
      /** 简易面板：标题 + 内容节点，返回 {el, close} */
      panel: function (title, contentEl) {
        var wrap = ncm.ui.el('div', {
          position: 'fixed', right: '16px', bottom: '96px', zIndex: '2147483646',
          width: '320px', padding: '10px 12px', borderRadius: '10px',
          background: 'rgba(20,20,20,.92)', color: '#eee', font: '13px/1.6 sans-serif'
        });
        var h = ncm.ui.el('div', { fontWeight: '600', marginBottom: '6px' }, String(title));
        wrap.appendChild(h);
        if (contentEl) wrap.appendChild(contentEl);
        (document.body || document.documentElement).appendChild(wrap);
        return {
          el: wrap,
          close: function () { if (wrap.parentNode) wrap.parentNode.removeChild(wrap); }
        };
      }
    },

    /** 插件间通信：纯页面内消息总线，按通道名分发（对齐 BetterNCM 的 ncm.ipc 用法） */
    ipc: (function () {
      var chans = Object.create(null);
      return {
        createServer: function (name, handler) {
          chans[name] = chans[name] || [];
          chans[name].push(handler);
          return function () {
            var a = chans[name] || [];
            var i = a.indexOf(handler);
            if (i >= 0) a.splice(i, 1);
          };
        },
        call: function (name) {
          var args = Array.prototype.slice.call(arguments, 1);
          var a = chans[name] || [];
          var out = [];
          a.slice().forEach(function (h) {
            try { out.push(h.apply(null, args)); }
            catch (e) { log('error', 'ipc 处理器异常 ' + name + ': ' + e); }
          });
          return out;
        },
        has: function (name) { return !!(chans[name] && chans[name].length); }
      };
    })(),

    /** 运行时打补丁：before / after / replace，返回 uninstall */
    hook: (function () {
      function wrap(target, name, kind, fn) {
        if (!target || typeof target[name] !== 'function') {
          log('warn', 'hook 目标不存在: ' + name);
          return function () {};
        }
        var orig = target[name];
        if (orig.__ncmHooked) orig = orig.__ncmOrig;
        var patched = function () {
          var args = Array.prototype.slice.call(arguments);
          if (kind === 'before') {
            try { fn.apply(this, args); } catch (e) { log('error', 'hook before 异常: ' + e); }
            return orig.apply(this, args);
          }
          if (kind === 'after') {
            var r = orig.apply(this, args);
            try { fn.apply(this, [r].concat(args)); } catch (e) { log('error', 'hook after 异常: ' + e); }
            return r;
          }
          try { return fn.apply(this, [function () { return orig.apply(this, args); }].concat(args)); }
          catch (e) { log('error', 'hook replace 异常: ' + e); return orig.apply(this, args); }
        };
        patched.__ncmHooked = true;
        patched.__ncmOrig = orig;
        target[name] = patched;
        return function uninstall() {
          if (target[name] === patched) target[name] = orig;
        };
      }
      return {
        before: function (t, n, f) { return wrap(t, n, 'before', f); },
        after: function (t, n, f) { return wrap(t, n, 'after', f); },
        replace: function (t, n, f) { return wrap(t, n, 'replace', f); }
      };
    })(),

    /** 宿主信息（只读；能改的只有版本/路径这类环境数据） */
    app: {
      version: HOST_VERSION,
      ncmVersion: H.ncmVersion || 'unknown',
      profileDir: H.profileDir || '',
      dataDir: H.dataRoot || ''
      ,isMainWindow: IS_MAIN_WINDOW
    }
  };

  /* ══ 3. 插件记录与生命周期 ═════════════════════════════════════════════ */

  var loadedRecords = [];   /* [{slug, manifest, ncm, dispose, unload}] */
  var disposables = [];     /* 全局 dispose 队列（chromatic 的 dispose 等价物） */

  function onDispose(fn) { disposables.push(fn); }

  function unloadAll() {
    log('info', '卸载 ' + loadedRecords.length + ' 个插件');
    loadedRecords.slice().forEach(function (rec) {
      try { if (rec.dispose) rec.dispose(); } catch (e) { log('error', 'dispose 异常 ' + rec.slug + ': ' + e); }
    });
    disposables.splice(0).forEach(function (fn) {
      try { fn(); } catch (e) { log('error', 'dispose 回调异常: ' + e); }
    });
    loadedRecords.length = 0;
    window.__NCM__.loaded = false;
    removeBadge();
  }

  /* ══ 文件系统写能力（原生端点，沙箱在 %LOCALAPPDATA%\NCMPlugin\data） ══
   * 路径都以插件 slug 为前缀，插件之间互相看不见。 */
  var API_BASE = 'http://127.0.0.1:38021';

  function fsUrl(op, p) {
    return API_BASE + '/fs/' + op + '?path=' + encodeURIComponent(p || '');
  }

  ncm.fs.readTextAsync = function (p) {
    return fetch(fsUrl('read', p), { cache: 'no-store' }).then(function (r) { return r.text(); });
  };
  ncm.fs.writeText = function (p, s) {
    /* 写完之后用 stat 轮询确认落盘，避免"写完立刻读"的竞态 */
    return fetch(fsUrl('write', p), { method: 'POST', mode: 'no-cors', body: s == null ? '' : String(s) })
      .then(function () {
        var tries = 0;
        return new Promise(function (resolve) {
          function check() {
            fetch(fsUrl('stat', p), { cache: 'no-store' })
              .then(function (r) { return r.json(); })
              .then(function (st) {
                if (st && st.exists) { resolve(true); return; }
                if (++tries >= 6) { resolve(false); return; }
                setTimeout(check, 40);
              })
              .catch(function () { if (++tries >= 6) resolve(false); else setTimeout(check, 40); });
          }
          check();
        });
      })
      .catch(function () { return false; });
  };
  ncm.fs.list = function (p) {
    return fetch(fsUrl('list', p || ''), { cache: 'no-store' }).then(function (r) { return r.json(); })
      .catch(function () { return []; });
  };
  ncm.fs.remove = function (p) {
    return fetch(fsUrl('remove', p), { method: 'POST', mode: 'no-cors' })
      .then(function () { return true; }).catch(function () { return false; });
  };
  ncm.fs.stat = function (p) {
    return fetch(fsUrl('stat', p), { cache: 'no-store' }).then(function (r) { return r.json(); })
      .catch(function () { return { exists: false }; });
  };

  /* ══ 插件启用/禁用（存 data/disabled.json） ══════════════════════════ */
  var disabledSet = {};

  function loadDisabled() {
    return ncm.fs.readTextAsync('disabled.json').then(function (t) {
      var arr = [];
      try { arr = JSON.parse(t || '[]'); } catch (e) { arr = []; }
      if (!Array.isArray(arr)) arr = [];
      disabledSet = {};
      arr.forEach(function (s) { disabledSet[s] = 1; });
      return disabledSet;
    }).catch(function () { return disabledSet; });
  }

  function saveDisabled() {
    var arr = Object.keys(disabledSet);
    return ncm.fs.writeText('disabled.json', JSON.stringify(arr));
  }

  function isDisabled(slug) { return !!disabledSet[slug]; }

  function setDisabled(slug, off) {
    if (off) disabledSet[slug] = 1; else delete disabledSet[slug];
    return saveDisabled();
  }
  /** 给单个插件一个受限的 ncm 视图：日志自动带 slug，避免插件互相污染。 */
  function scopedNcm(slug) {
    var s = Object.create(ncm);
    s.log = function (level, msg) { log(level || 'info', '[' + slug + '] ' + msg); };
    s.on = function (ev, cb) { return ncm.on(slug + ':' + ev, cb); };
    s.onDispose = function (fn) { if (typeof fn === "function") onDispose(fn); };

    /* 每个插件拿到自己目录下的 fs 视图：路径自动加 "<slug>/" 前缀 */
    s.fs = Object.create(ncm.fs);
    (function () {
      var base = String(slug).replace(/\.plugin$/i, "");
      ["readTextAsync", "writeText", "list", "remove", "stat"].forEach(function (k) {
        s.fs[k] = function (p) {
          var args = Array.prototype.slice.call(arguments);
          args[0] = base + "/" + (p == null ? "" : p);
          return ncm.fs[k].apply(null, args);
        };
      });
      s.fs.readText = function (p) {
        var d = String(p == null ? "" : p);
        if (Object.prototype.hasOwnProperty.call(preloaded, d)) return preloaded[d];
        return preloaded[base + "/" + d] || null;
      };
      s.fs.exists = function (p) {
        var d = String(p == null ? "" : p);
        return Object.prototype.hasOwnProperty.call(preloaded, d) ||
               Object.prototype.hasOwnProperty.call(preloaded, base + "/" + d);
      };
    })();
    s.emit = function (ev) {
      var a = Array.prototype.slice.call(arguments, 1);
      ncm.emit.apply(null, [slug + ':' + ev].concat(a));
    };
    return s;
  }

  function loadOne(entry) {
    var slug = entry.slug || '(unnamed)';
    var rec = { slug: slug, manifest: entry.manifest, dispose: null, unload: null };

    if (entry.error) {
      log('error', '插件 ' + slug + ' 载入失败: ' + entry.error);
      rec.error = entry.error;
      loadedRecords.push(rec);
      return;
    }
    if (!entry.script) {
      log('warn', '插件 ' + slug + ' 没有脚本，跳过');
      return;
    }

    var mf = entry.manifest || {};
    if (!satisfies(ncm.ncmVersion, mf.ncm_version_req)) {
      log('warn', '插件 ' + slug + ' 要求网易云 ' + mf.ncm_version_req +
                  '，当前 ' + ncm.ncmVersion + ' → 跳过');
      rec.error = 'ncm_version_req 不满足';
      loadedRecords.push(rec);
      return;
    }
    if (mf.targets && mf.targets.length && mf.targets.indexOf('renderer') < 0 &&
        mf.targets.indexOf('main') >= 0 && mf.targets.indexOf('renderer') < 0) {
      log('warn', '插件 ' + slug + ' 只声明 targets=' + mf.targets.join(',') + '，跳过');
      return;
    }

    var mod = { exports: {} };
    try {
      /* 用 Function 而不是 eval：作用域可控，且能拿到 exports/module */
      var factory = new Function('ncm', 'exports', 'module', 'console', 'host',
                                 entry.script + '\n//# sourceURL=ncm-plugin://' + slug + '/' + (entry.entryFile || 'startup_script.js'));
      factory(scopedNcm(slug), mod.exports, mod, window.console, H);
    } catch (e) {
      log('error', '插件 ' + slug + ' 执行异常: ' + (e && e.stack ? e.stack : e));
      rec.error = String(e);
      loadedRecords.push(rec);
      return;
    }

    var ex = mod.exports || {};
    rec.dispose = ex.onUnload || ex.onDispose || null;
    if (typeof ex.onLoad === 'function') {
      try { ex.onLoad(scopedNcm(slug)); } catch (e) { log('error', slug + ' onLoad 异常: ' + e); }
    }
    loadedRecords.push(rec);
    log('info', '插件 ' + slug + ' v' + (mf.version || '?') + ' 载入完成');
    ncm.emit('plugin-loaded', slug);
  }

  /* ══ 4. injects / hijacks：把插件声明的代码投进页面 ═══════════════════ */

  /** URL 匹配：精确 / `*` 通配 / 前缀（`foo*`）。injects 与 hijacks 共用。 */
  function urlMatches(url, pattern) {
    if (pattern === url) return true;
    if (pattern.slice(-1) === '*' && url.indexOf(pattern.slice(0, -1)) === 0) return true;
    if (pattern.indexOf('*') > 0) {
      var re = new RegExp('^' + pattern.split('*').map(function (s) {
        return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
      }).join('.*') + '$');
      return re.test(url);
    }
    return false;
  }

  /**
   * injects：同时支持两种形状
   *   A. BetterNCM：{ "<url模式>": [ {type:'script'|'css', code} , ... ] }
   *   B. 旧版数组：  [ { type, code, path } ]
   * A 形状要求 URL 命中；B 形状无条件执行（兼容早期插件）。
   */
  function runInjects(mf, slug) {
    var inj = (mf && mf.injects);
    if (!inj) return;
    var url = location.href;
    var n = 0;

    function runOne(item, why) {
      if (!item) return;
      var code = item.code;
      if (!code) return;
      try {
        if (item.type === 'css') {
          var st = document.createElement('style');
          st.setAttribute('data-ncm-inject', slug);
          st.textContent = code;
          (document.head || document.documentElement).appendChild(st);
        } else {
          new Function('ncm', 'host', code + '\n//# sourceURL=ncm-inject://' + slug + '/' + (n++))(
            scopedNcm(slug), H);
        }
        log('info', '插件 ' + slug + ' inject 已应用（' + why + '，type=' + (item.type || 'script') + '）');
      } catch (e) {
        log('error', '插件 ' + slug + ' inject 失败: ' + e);
      }
    }

    if (Array.isArray(inj)) {
      inj.forEach(function (item) { runOne(item, 'legacy-array'); });
      return;
    }
    Object.keys(inj).forEach(function (pattern) {
      if (!urlMatches(url, pattern)) return;
      [].concat(inj[pattern] || []).forEach(function (item) { runOne(item, pattern); });
    });
  }

  function runHijacks(mf, slug) {
    var hj = (mf && mf.hijacks) || {};
    var url = location.href;
    Object.keys(hj).forEach(function (pattern) {
      var hit = false;
      if (pattern === url) hit = true;
      else {
        /* 极简通配：只支持以 * 结尾的前缀匹配 */
        if (pattern.slice(-1) === '*' && url.indexOf(pattern.slice(0, -1)) === 0) hit = true;
        if (pattern.indexOf('*') > 0) {
          var re = new RegExp('^' + pattern.split('*').map(function (s) {
            return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
          }).join('.*') + '$');
          if (re.test(url)) hit = true;
        }
      }
      if (!hit) return;
      var actions = [].concat(hj[pattern] || []);
      actions.forEach(function (a) {
        if (!a || !a.code) return;
        try {
          new Function('ncm', 'host', a.code + '\n//# sourceURL=ncm-hijack://' + slug)(scopedNcm(slug), H);
          log('info', '插件 ' + slug + ' hijack 命中 ' + pattern + '（type=' + (a.type || '?') + '）');
        } catch (e) {
          log('error', '插件 ' + slug + ' hijack 失败: ' + e);
        }
      });
    });
  }

  /* ══ 5. 可见证据：右下角徽标 + 插件面板 ═══════════════════════════════
   * 一个"注入成功"的说法必须能被人眼验证，所以这里在页面上留下实物。 */

  var badge = null, panel = null;

  function removeBadge() {
    if (badge && badge.parentNode) badge.parentNode.removeChild(badge);
    if (panel && panel.parentNode) panel.parentNode.removeChild(panel);
    /* ★ 全量清理：buildBadge 会被多次调用（路由变化/轮询），
     *   只删自己记着的引用会在标题栏里堆出一排重复图标（实测堆了 36 个）。 */
    try {
      var dup = document.querySelectorAll('#ncm-inject-badge');
      for (var i = 0; i < dup.length; i++) {
        if (dup[i].parentNode) dup[i].parentNode.removeChild(dup[i]);
      }
    } catch (e) {}
    badge = panel = null;
  }

  function buildBadge() {
    /* 只主窗口显示角标；其他窗口（桌宠 / 子应用）直接不建 */
    if (!IS_MAIN_WINDOW) {
      removeBadge();
      log("info", "非主窗口（" + location.pathname + "），跳过注入角标");
      return;
    }
    /* 幂等：已经挂在标题栏图标行里就不重建（避免反复追加） */
    var existing = document.getElementById('ncm-inject-badge');
    var bar = document.querySelector("[class*=MiniModeIconBar]");
    if (existing && bar && existing.parentElement === bar) return;
    removeBadge();
    var ok = loadedRecords.filter(function (r) { return !r.error; }).length;
    var tip = "NCM Plugin Host v" + HOST_VERSION + " · 插件 " + ok + "/" + loadedRecords.length;

    /* ★ 最终形态：做成**标题栏图标行里的一个独立小图标**，
     *   排在宿主自己的「私信 / 设置 / 皮肤 / 宠物」右侧，不再浮在页面右上角。
     *   样式对齐原生图标：20×20、右间距 10px、颜色 rgba(40,50,72,.6)。 */
    var SVG =
      '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" ' +
      'xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
      '<circle cx="12" cy="12" r="9.2" stroke="currentColor" stroke-width="1.6"/>' +
      '<path d="M9.2 16.4V8.6l5.6 1.9v5.2" stroke="currentColor" stroke-width="1.6" ' +
      'stroke-linecap="round" stroke-linejoin="round"/>' +
      '<circle cx="7.9" cy="16.6" r="1.4" fill="currentColor"/>' +
      '<circle cx="13.5" cy="16.1" r="1.4" fill="currentColor"/>' +
      '</svg>';

    function makeIcon(doc) {
      var el = doc.createElement('div');
      el.id = 'ncm-inject-badge';
      el.setAttribute('data-ncm-host', HOST_VERSION);
      el.setAttribute('role', 'button');
      el.setAttribute('title', tip);
      el.setAttribute('style',
        'width:20px;height:20px;margin-right:10px;display:inline-flex;' +
        'align-items:center;justify-content:center;cursor:pointer;flex:0 0 auto;' +
        'color:rgba(40,50,72,.6);background:transparent;border:0;padding:0;' +
        'transition:color .15s ease,opacity .15s ease;opacity:.9');
      el.innerHTML = SVG;
      el.onmouseenter = function () { el.style.color = 'rgba(198,47,47,.95)'; el.style.opacity = '1'; };
      el.onmouseleave = function () { el.style.color = 'rgba(40,50,72,.6)'; el.style.opacity = '.9'; };
      el.onclick = function (ev) { if (ev && ev.stopPropagation) ev.stopPropagation(); togglePanel(); };
      return el;
    }

    /* 首选：插进标题栏图标行（宿主自己的那一排，宠物图标右侧） */
    var group = document.querySelector("[class*=MiniModeIconBar]");
    if (group) {
      group.appendChild(makeIcon(document));
    } else {
      /* 兜底：图标行还没渲染时，先用右上角固定角标，等下一轮再迁移 */
      var fb = makeIcon(document);
      fb.setAttribute('style',
        'position:fixed;right:10px;top:10px;z-index:2147483647;width:22px;height:22px;' +
        'border-radius:11px;background:rgba(198,47,47,.92);color:#fff;display:flex;' +
        'align-items:center;justify-content:center;cursor:pointer');
      (document.documentElement || document.body).appendChild(fb);
    }

    /* 清掉历史横幅 */
    ['ncm-inject-banner'].forEach(function (id) {
      var old = document.getElementById(id);
      if (old && old.parentNode) old.parentNode.removeChild(old);
    });
  }

  /* ══ 插件管理面板（点击角标打开）═══════════════════════════════════════
   * 能力：查看列表/状态、启用/禁用、单个重载、立即热重载、数据目录。 */
  function panelRow(r) {
    var mf = r.manifest || {};
    var row = ncm.ui.el('div', { padding: '8px 0', borderTop: '1px solid rgba(255,255,255,.1)' });

    var line1 = ncm.ui.el('div', { display: 'flex', alignItems: 'center', gap: '6px' });
    line1.appendChild(ncm.ui.el('span', { fontWeight: '600' },
      (mf.name || r.slug) + '  v' + (mf.version || '?')));
    var badgeText = r.disabled ? '已禁用' : (r.error ? '出错' : '已载入');
    var badgeColor = r.disabled ? 'rgba(255,255,255,.35)' : (r.error ? '#e05a5a' : '#2e9e4f');
    line1.appendChild(ncm.ui.el('span', {
      fontSize: '10px', padding: '1px 6px', borderRadius: '8px',
      background: badgeColor, color: '#fff'
    }, badgeText));
    row.appendChild(line1);

    if (mf.author || mf.description) {
      row.appendChild(ncm.ui.el('div', { opacity: '.6', marginTop: '2px' },
        (mf.author ? mf.author + ' · ' : '') + (mf.description || '')));
    }
    if (r.error) {
      row.appendChild(ncm.ui.el('div', { color: '#e05a5a', marginTop: '2px' }, '❌ ' + r.error));
    }

    var bar = ncm.ui.el('div', { marginTop: '6px', display: 'flex', gap: '6px' });
    var btnStyle = 'padding:2px 10px;border-radius:10px;cursor:pointer;font-size:11px;' +
                   'border:1px solid rgba(255,255,255,.3);background:transparent;color:inherit;';

    var tgl = ncm.ui.el('button', null, r.disabled ? '启用' : '禁用');
    tgl.setAttribute('style', btnStyle);
    tgl.onclick = function () {
      setDisabled(r.slug, !r.disabled).then(function () {
        return hotReload(true);
      });
    };
    bar.appendChild(tgl);

    var re = ncm.ui.el('button', null, '重载');
    re.setAttribute('style', btnStyle);
    re.onclick = function () { hotReload(true); };
    bar.appendChild(re);

    row.appendChild(bar);
    return row;
  }

  function refreshPanel() {
    if (panel && panel.parentNode) {
      panel.parentNode.removeChild(panel);
      panel = null;
      togglePanel();
    }
  }

  function togglePanel() {
    if (panel && panel.parentNode) { panel.parentNode.removeChild(panel); panel = null; return; }

    panel = ncm.ui.el('div', {
      position: 'fixed', right: '12px', bottom: '92px', zIndex: '2147483000',
      width: '360px', maxHeight: '70vh', overflow: 'auto',
      background: 'rgba(20,20,24,.97)', color: '#eaeaea',
      font: '12px/1.6 system-ui,sans-serif', padding: '12px 14px',
      borderRadius: '10px', boxShadow: '0 4px 20px rgba(0,0,0,.5)'
    });

    panel.appendChild(ncm.ui.el('div', { fontWeight: '600', marginBottom: '4px' },
      '插件宿主 v' + HOST_VERSION));
    panel.appendChild(ncm.ui.el('div', { opacity: '.6', marginBottom: '8px' },
      'NCM ' + ncm.ncmVersion + ' · ' + H.processType + ' · ' + (AUTO_RELOAD ? '自动热重载已开启' : '手动热重载')));

    var bar = ncm.ui.el('div', { display: 'flex', gap: '6px', marginBottom: '8px' });
    var b1 = ncm.ui.el('button', { padding: '3px 12px', borderRadius: '12px', cursor: 'pointer',
      fontSize: '11px', border: '1px solid #c62f2f', background: '#c62f2f', color: '#fff' }, '立即热重载');
    b1.onclick = function () { hotReload(true); };
    bar.appendChild(b1);

    var b2 = ncm.ui.el('button', { padding: '3px 12px', borderRadius: '12px', cursor: 'pointer',
      fontSize: '11px', border: '1px solid rgba(255,255,255,.3)', background: 'transparent',
      color: 'inherit' }, '复制数据目录');
    b2.onclick = function () {
      try {
        var ta = document.createElement('textarea');
        ta.value = ncm.dataRoot || '';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
      } catch (e) {}
    };
    bar.appendChild(b2);
    panel.appendChild(bar);

    if (!loadedRecords.length) {
      panel.appendChild(ncm.ui.el('div', { opacity: '.7' }, '没有发现插件'));
    }
    loadedRecords.forEach(function (r) {
      try { panel.appendChild(panelRow(r)); } catch (e) {}
    });

    panel.appendChild(ncm.ui.el('div', { marginTop: '10px', opacity: '.45', fontSize: '11px' },
      '数据目录：' + (ncm.dataRoot || '(未知)')));
    panel.appendChild(ncm.ui.el('div', { opacity: '.45', fontSize: '11px' },
      '插件目录：' + (ncm.pluginsDir || '(未知)')));

    ncm.ui.mount(panel);
  }

  /* ══ 6. 启动 ═══════════════════════════════════════════════════════════ */

  window.__NCM__ = {
    loaded: false,
    version: HOST_VERSION,
    host: H,
    ncm: ncm,
    plugins: loadedRecords,
    onDispose: onDispose,
    unloadAll: unloadAll,
    /* 调试出口：方便在 DevTools / 命令通道里检查宿主内部状态 */
    debug: {
      fs: function () { return ncm.fs; },
      hotReload: function (force) { return hotReload(force !== false); },
      hostSig: function () { return lastHostSig; },
      hotStat: function () { return hotStat; },
      disabled: function () { return Object.keys(disabledSet); },
      preloaded: function () { return Object.keys(preloaded); }
    },
    /** 重新扫描并装载（原生侧会重新投递载荷时自动发生） */
    reload: function () {
      var list = (window.__NCM_HOST__ && window.__NCM_HOST__.plugins) || [];
      list.forEach(function (e) { loadOne(e); runInjects(e.manifest, e.slug); runHijacks(e.manifest, e.slug); });
      buildBadge();
    }
  };

  /* ══ 宿主应用（可重入）＋热重载 ═══════════════════════════════════════
   * 原生侧 /host 端点返回 ncm_host_json()（含每个插件的 mtime）。
   * 页面每 2s 轮询一次，签名变了就"卸载全部 → 重新装载"，
   * 于是改插件脚本不需要重启网易云。 */
  var lastHostSig = '';

  function hostSig(h) {
    return (h.plugins || []).map(function (p) {
      return (p.slug || '') + '@' + (p.mtime || 0) + (p.error ? '!' : '');
    }).join('|');
  }

  function applyHost(h) {
    if (!h) return 0;
    window.__NCM_HOST__ = h;

    (h.plugins || []).forEach(function (e) {
      if (e && e.dir) preloaded[e.dir + '/manifest.json'] = JSON.stringify(e.manifest || {});
      if (e && e.dir && e.script) preloaded[e.dir + '/' + (e.entryFile || 'startup_script.js')] = e.script;
    });

    var list = (h.plugins || []).slice().sort(function (a, b) {
      var pa = (a.manifest && a.manifest.load_priority) || 0;
      var pb = (b.manifest && b.manifest.load_priority) || 0;
      return pb - pa;
    });

    var loaded = 0, skipped = 0;
    list.forEach(function (e) {
      if (!e) return;
      if (isDisabled(e.slug)) {
        skipped++;
        loadedRecords.push({ slug: e.slug, manifest: e.manifest, disabled: true });
        log('info', '插件 ' + e.slug + ' 已禁用，跳过');
        return;
      }
      loadOne(e);
      runInjects(e.manifest, e.slug);
      runHijacks(e.manifest, e.slug);
      loaded++;
    });

    buildBadge();
    window.__NCM__.loaded = true;
    log('info', '插件装载完成：' + loaded + ' 个' + (skipped ? ('，禁用 ' + skipped + ' 个') : ''));
    return loaded;
  }

  var reloading = false;

  function hotReload(force) {
    if (reloading) return Promise.resolve(false);   /* 防重入：轮询与 SSE 可能同时触发 */
    reloading = true;
    return fetch(API_BASE + '/host', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (h) {
        var sig = hostSig(h);
        if (!force && sig === lastHostSig) return false;
        lastHostSig = sig;
        unloadAll();
        applyHost(h);
        refreshPanel();
        log('info', '热重载完成');
        return true;
      })
      .catch(function () { return false; })
      .then(function (r) { reloading = false; return r; },
            function (e) { reloading = false; throw e; });
  }

  /* ★ 自动热重载开关：实测会自我循环触发（签名反复变化），
   *   在定位清楚之前保持关闭；面板里的「重载 / 立即热重载」按钮仍可用。 */
  var AUTO_RELOAD = false;
  var hotStat = { polls: 0, hits: 0, lastErr: "" };

  function startHotReload() {
    if (!AUTO_RELOAD) {
      log("info", "自动热重载已停用（可用插件面板里的按钮手动重载）");
      return;
    }
    setInterval(function () {
      hotStat.polls++;
      try {
        hotReload(false).then(function (r) {
          if (r === true) { hotStat.hits++; log("info", "热重载完成"); }
        }).catch(function (e) { hotStat.lastErr = String(e && e.message); });
      } catch (e) {
        hotStat.lastErr = String(e && e.message);
      }
    }, 2000);
    log("info", "热重载已开启（插件脚本改动会在 2 秒内生效，无需重启）");

    /* ★ 真正的触发者是原生的 SSE 推送：窗口不可见时定时器会被 Chromium 冻结
     *   （实测 polls=0），而网络事件不受影响。轮询只作为兜底（60s 一次）。
     */
    (function connectReloadStream(tries) {
      tries = tries || 0;
      Promise.resolve(H && H.bcastPort ? String(H.bcastPort) : "")
        .then(function (t) {
          var p = parseInt(String(t).trim(), 10);
          if (!(p > 0)) throw new Error("no port");
          var es = new EventSource("http://127.0.0.1:" + p + "/events");
          var es = new EventSource("http://127.0.0.1:" + p + "/events");
          es.addEventListener("reload", function () {
            if (!AUTO_RELOAD) return;              /* 自动触发总开关（实测会自我循环） */
            log("info", "收到原生热重载推送");
            hotReload(true);
          });
          es.onerror = function () {
            try { es.close(); } catch (e) {}
            if (tries < 20) setTimeout(function () { connectReloadStream(tries + 1); }, 2000);
          };
          log("info", "已订阅原生热重载推送（SSE）");
        })
        .catch(function () {
          if (tries < 20) setTimeout(function () { connectReloadStream(tries + 1); }, 1500);
        });
    })();

    /* 兜底轮询：即使 SSE 断线也能自愈（低频，避免被冻结也无所谓） */
    setInterval(function () { try { hotReload(false); } catch (e) {} }, 60000);
  }
  loadDisabled();

  (function boot() {
    /* 预载：把插件自己的源码放进 fs 可见区，方便插件读自己目录里的资源清单 */
    (H.plugins || []).forEach(function (e) {
      if (e && e.dir) preloaded[e.dir + '/manifest.json'] = JSON.stringify(e.manifest || {});
      if (e && e.dir && e.script) preloaded[e.dir + '/' + (e.entryFile || 'startup_script.js')] = e.script;
    });

    var list = (H.plugins || []).slice();
    list.sort(function (a, b) {
      var pa = (a.manifest && a.manifest.load_priority) || 0;
      var pb = (b.manifest && b.manifest.load_priority) || 0;
      return pb - pa;   /* 值大者先加载 */
    });

    log('info', '宿主启动 v' + HOST_VERSION + ' · 网易云 ' + ncm.ncmVersion +
                ' · 角色 ' + H.processType + ' · 插件条目 ' + list.length);

    lastHostSig = hostSig(H);
    applyHost(H);

    /* 等 DOM 就绪再挂 UI（on_load_end 时文档已可用，但保险起见） */
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', buildBadge, { once: true });
    } else {
      buildBadge();
    }

    window.__NCM__.loaded = true;
    log('info', '宿主就绪：' + location.href);
    startHotReload();
    /* disabled.json 是异步读的：读完再重载一次，确保被禁用的插件不会先被加载 */
    loadDisabled().then(function () {
      if (Object.keys(disabledSet).length) hotReload(true);
    });
  })();
})();

/* ══════════════════════════════════════════════════════════════════════════
 * DOM 探针：把播放信息候选节点回传给原生侧
 *
 * 为什么要这一步：NCM 的页面结构没有公开文档，先把「哪些节点承载歌曲名/
 * 歌手/歌词/封面」摸清楚，才能做稳定的 ncm.player API。
 * 回传走 127.0.0.1:38021（DLL 内的极简 HTTP 服务），因为页面没有原生桥。
 * ══════════════════════════════════════════════════════════════════════════ */
(function () {
  if (window.__NCM_DOM_SCAN__) return;
  window.__NCM_DOM_SCAN__ = 1;

  var ENDPOINT = 'http://127.0.0.1:38021/dom';

  function text(el) {
    if (!el) return '';
    var t = (el.innerText || el.textContent || '').replace(/\s+/g, ' ').trim();
    return t.length > 100 ? t.slice(0, 100) : t;
  }
  function attr(el, names) {
    for (var i = 0; i < names.length; i++) {
      var v = el.getAttribute && el.getAttribute(names[i]);
      if (v) return String(v).slice(0, 160);
    }
    return '';
  }
  function push(list, el, kind) {
    if (!el || list.length >= 70) return;
    var cls = (el.className && String(el.className)) || '';
    var item = {
      k: kind,
      tag: el.tagName ? el.tagName.toLowerCase() : '?',
      c: cls.slice(0, 90),
      i: (el.id || '').slice(0, 60),
      x: text(el),
      src: attr(el, ['src', 'data-src', 'style'])
    };
    if (item.x || item.src) list.push(item);
  }

  function snapshot(reason) {
    var out = { reason: reason, url: location.href, title: document.title, t: Date.now() };
    var vh = window.innerHeight || 0;

    /* 1) 所有图片（封面候选）：带尺寸和祖先 class，便于定位 */
    out.imgs = [];
    try {
      document.querySelectorAll("img").forEach(function (el) {
        if (out.imgs.length >= 24) return;
        var r = el.getBoundingClientRect();
        if (r.width < 20 || r.height < 20) return;
        var par = el.parentElement;
        out.imgs.push({
          src: String(el.currentSrc || el.src || "").slice(0, 200),
          alt: (el.alt || "").slice(0, 60),
          cls: String(el.className || "").slice(0, 80),
          pcls: par ? String(par.className || "").slice(0, 80) : "",
          box: [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)]
        });
      });
    } catch (e) {}

    /* 2) 底部播放条：视口下半部分里、含 img 或 button 的容器 */
    out.bottom = [];
    try {
      var all = document.querySelectorAll("div,section,footer");
      for (var i = 0; i < all.length && out.bottom.length < 6; i++) {
        var el = all[i];
        var r = el.getBoundingClientRect();
        if (r.top < vh * 0.62 || r.height < 30 || r.height > 220) continue;
        if (r.width < (window.innerWidth || 0) * 0.5) continue;
        var txt = (el.innerText || "").replace(/\\s+/g, " ").trim();
        out.bottom.push({
          cls: String(el.className || "").slice(0, 90),
          box: [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)],
          txt: txt.slice(0, 220),
          imgs: el.querySelectorAll("img").length
        });
      }
    } catch (e) {}

    /* 3) 歌词候选 */
    out.lyrics = [];
    try {
      document.querySelectorAll("[class*=lyric],[class*=Lyric],[class*=lrc],[class*=Lrc]").forEach(function (el) {
        if (out.lyrics.length >= 10) return;
        var t = (el.innerText || "").replace(/\\s+/g, " ").trim();
        if (!t) return;
        out.lyrics.push({ cls: String(el.className || "").slice(0, 80), txt: t.slice(0, 260) });
      });
    } catch (e) {}

    /* 4) 媒体元素状态 */
    out.media = [];
    try {
      document.querySelectorAll("audio,video").forEach(function (el) {
        out.media.push({
          tag: el.tagName.toLowerCase(),
          src: String(el.currentSrc || el.src || "").slice(0, 160),
          cur: Math.round(el.currentTime || 0),
          dur: Math.round(el.duration || 0),
          paused: !!el.paused
        });
      });
    } catch (e) {}

    return out;
  }

  function send(reason) {
    try {
      var body = JSON.stringify(snapshot(reason));
      fetch(ENDPOINT, {
        method: 'POST',
        mode: 'no-cors',
        headers: { 'Content-Type': 'text/plain' },
        body: body
      }).catch(function () {});
    } catch (e) {}
  }

  /* 持续观察：只在「标题或播放器区域文本变化」时上报，外加低频心跳 */
  var lastKey = '';
  var lastBeat = 0;
  function key() {
    var bar = document.querySelector('[class*="player"],[class*="Player"]');
    return (document.title || '') + '|' + ((bar && bar.innerText) || '').slice(0, 200);
  }
  setTimeout(function () { send('load'); lastKey = key(); lastBeat = Date.now(); }, 1500);
  setInterval(function () {
    var k = key();
    var now = Date.now();
    if (k !== lastKey) {
      lastKey = k;
      send('change');
      lastBeat = now;
    } else if (now - lastBeat > 15000) {
      lastBeat = now;
      send('heartbeat');
    }
  }, 2000);

  window.__NCM_DOM_SNAPSHOT__ = function (reason) { send(reason || 'manual'); };
})();

/* ══════════════════════════════════════════════════════════════════════════
 * 网络层探针：抓 NCM 自己的接口流量（歌曲详情/歌词/音频地址/封面）
 *
 * 为什么不用 DOM 抓数据（用户提的问题）：
 *   DOM 类名是 CSS Modules 哈希、播放条按需渲染、歌词面板要展开，
 *   而且没有 <audio> 拿不到时长/进度 —— 对插件作者来说太脆、太难写。
 *   真正稳定的数据源是接口：song/detail、lyric、song/url、pic 等，
 *   URL 规则多年不变，而且可以**改写响应**来实现"修改"能力。
 * ══════════════════════════════════════════════════════════════════════════ */
(function () {
  if (window.__NCM_NET_HOOK__) return;
  window.__NCM_NET_HOOK__ = 1;

  var ENDPOINT = 'http://127.0.0.1:38021/api';
  var MAXBODY = 1200;
  var seen = 0;

  function interesting(url) {
    if (!url) return false;
    var u = String(url);
    return /\/api\/|song\/detail|lyric|song\/url|artist\/|album\/|playlist\/detail|\/weapi\/|\/eapi\//i.test(u);
  }

  function report(kind, url, status, body) {
    if (seen > 60) return;
    seen++;
    try {
      fetch(ENDPOINT, {
        method: 'POST',
        mode: 'no-cors',
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify({
          kind: kind,
          url: String(url).slice(0, 300),
          status: status,
          body: String(body || '').slice(0, MAXBODY),
          t: Date.now()
        })
      }).catch(function () {});
    } catch (e) {}
  }

  /* fetch */
  var origFetch = window.fetch;
  if (origFetch) {
    window.fetch = function (input, init) {
      var url = (typeof input === 'string') ? input : (input && input.url);
      var p = origFetch.apply(this, arguments);
      if (interesting(url)) {
        p.then(function (r) {
          try {
            r.clone().text().then(function (t) { report('fetch', url, r.status, t); }).catch(function () {});
          } catch (e) {}
        }).catch(function () {});
      }
      return p;
    };
  }

  /* XHR */
  var XO = XMLHttpRequest.prototype.open;
  var XS = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (m, u) {
    this.__ncm_url = u;
    return XO.apply(this, arguments);
  };
  XMLHttpRequest.prototype.send = function () {
    var self = this;
    if (interesting(self.__ncm_url)) {
      self.addEventListener('load', function () {
        var body = '';
        try { body = self.responseType === '' || self.responseType === 'text' ? self.responseText : '[binary]'; } catch (e) {}
        report('xhr', self.__ncm_url, self.status, body);
      });
    }
    return XS.apply(this, arguments);
  };
})();

/* ══════════════════════════════════════════════════════════════════════════
 * 命令通道 + channel 桥探针
 *
 * 1) 命令通道：原生侧写 %LOCALAPPDATA%\NCMPlugin\cmd.js，页面每 2 秒轮询
 *    http://127.0.0.1:38021/cmd 并 eval —— 这样调插件逻辑不用反复重启应用。
 * 2) channel 桥：NCM 的歌曲/歌词数据是 native 通过 channel.call 推给页面的
 *    （页面 fetch 抓不到），所以包一层，把调用名/参数/结果回传。
 * ══════════════════════════════════════════════════════════════════════════ */
(function () {
  var BASE = 'http://127.0.0.1:38021';
  var lastCmd = '';

  function report(kind, payload) {
    try {
      fetch(BASE + '/api', {
        method: 'POST', mode: 'no-cors',
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify({ kind: kind, body: payload, t: Date.now() })
      }).catch(function () {});
    } catch (e) {}
  }

  /* ── 命令通道 ── */
  setInterval(function () {
    if (document.hidden) return;
    fetch(BASE + '/cmd', { cache: 'no-store' })
      .then(function (r) { return r.text(); })
      .then(function (txt) {
        if (!txt || txt === lastCmd) return;
        lastCmd = txt;
        var out;
        try {
          /* 用 IndirectEval 让命令在全局作用域跑，并支持 return 值 */
          out = (new Function('return (function(){' + txt + '})()'))();
        } catch (e) {
          out = 'ERROR: ' + (e && e.message);
        }
        try {
          report('cmdresult', typeof out === 'string' ? out : JSON.stringify(out));
        } catch (e) {}
      })
      .catch(function () {});
  }, 2000);

  /* ── channel 桥探针 ── */
  var ch = window.channel;
  if (ch && typeof ch.call === 'function' && !ch.__ncm_wrapped) {
    var orig = ch.call;
    ch.call = function (name, ret, args) {
      var t0 = Date.now();
      var r;
      try {
        r = orig.apply(this, arguments);
      } catch (e) {
        report('channel', JSON.stringify({ name: String(name), err: String(e && e.message) }));
        throw e;
      }
      try {
        var argsStr = '';
        try { argsStr = JSON.stringify(args); } catch (e) { argsStr = String(args); }
        argsStr = String(argsStr || '').slice(0, 700);
        var preview;
        if (r && typeof r.then === 'function') {
          r.then(function (v) {
            var s;
            try { s = JSON.stringify(v); } catch (e) { s = String(v); }
            report('channel', JSON.stringify({
              name: String(name), async: 1, ms: Date.now() - t0,
              args: argsStr, result: String(s).slice(0, 900)
            }));
          }).catch(function (e) {
            report('channel', JSON.stringify({ name: String(name), asyncErr: String(e && e.message), args: argsStr }));
          });
        } else {
          try { preview = JSON.stringify(r); } catch (e) { preview = String(r); }
          report('channel', JSON.stringify({
            name: String(name), ms: Date.now() - t0, args: argsStr, result: String(preview).slice(0, 900)
          }));
        }
      } catch (e) {}
      return r;
    };
    ch.__ncm_wrapped = 1;
  }

  /* ── 原生数据拦截：宿主自己就在用 network.fetch 取这些接口 ────────────
   * 实测宿主切歌时会调用：
   *   network.fetch { url: "https://music.163.com/api/song/detail?ids=[<id>]" }
   *   network.fetch { url: "https://music.163.com/api/song/lyric?id=<id>&..." }
   * 直接把它的**响应体**截下来，比我们自己再请求一次更省、更同步。 */

  (function installNativeFetchHook() {
    var ch = window.channel;
    if (!ch || typeof ch.call !== 'function' || ch.__ncm_fetch_hook) return;
    var orig = ch.call;
    ch.call = function (name, ret, args) {
      window.__NFHITS__ = window.__NFHITS__ || { calls: 0, fetch: 0, hit: 0, cb: 0 };
      window.__NFHITS__.calls++;
      try {
        if (name === 'network.fetch' && typeof ret === 'function' &&
            args && args[0] && args[0].url) {
          window.__NFHITS__.fetch++;
          var url = String(args[0].url);
          if (/\/api\/song\/(detail|lyric)/.test(url)) {
            window.__NFHITS__.hit++;
            window.__NFHITS__.lastUrl = url.slice(0, 90);
            var wrapped = function (v) {
              window.__NFHITS__.cb++;
              try { if (window.__NCM_INGEST__) window.__NCM_INGEST__(url, v); } catch (e) { window.__NFHITS__.err = String(e && e.message); }
              return ret.apply(this, arguments);
            };
            return orig.call(this, name, wrapped, args);
          }
        }
      } catch (e) { window.__NFHITS__.err = String(e && e.message); }
      return orig.apply(this, arguments);
    };
    ch.__ncm_fetch_hook = 1;
    log('info', '已安装原生数据拦截（song/detail、song/lyric）');
  })();
  /* 顺带把常见全局状态名探一遍（有就上报） */
  setTimeout(function () {
    var names = ['__INITIAL_STATE__', '__NUXT__', '__NCM_STATE__', 'store', 'player', 'appStore'];
    var found = {};
    names.forEach(function (n) {
      try { if (window[n] !== undefined) found[n] = typeof window[n]; } catch (e) {}
    });
    report('globals', JSON.stringify(found));
  }, 3000);
})();

/* ══════════════════════════════════════════════════════════════════════════
 * ncm.player —— 播放信息 API（实机验证版）
 *
 * 设计取舍：NCM 的 DOM 是 CSS Modules 哈希类名、有多种形态（普通播放条 /
 * 全屏播放器模式）、没有 <audio>。这些脏活由框架承担，插件作者只用稳定接口：
 *
 *   ncm.player.current()  → { song:{name,artist,album,source}, lyric:{lines,current,index}, cover, mode }
 *   ncm.player.on('song'|'lyric'|'line'|'progress', cb)
 *   ncm.player.override({ lyric, cover })
 *
 * 实测字段来源（2026-10 实机验证）：
 *   歌名/专辑/歌手/来源  ← [class*=TrackTitleContainer] 的文本（形如「专辑：X / 歌手：Y」）
 *   歌词与时间戳        ← .line[data-time]（data-time 是**秒**，带小数）
 *   当前行              ← 歌词区垂直中心最近的一行（播放器模式会自动滚到当前行）
 *   封面                ← img.cover-img（普通模式在播放条里；播放器模式见下）
 * ══════════════════════════════════════════════════════════════════════════ */
(function () {
  if (window.__NCM_PLAYER_V2__) return;
  window.__NCM_PLAYER_V2__ = 1;

  var listeners = { song: [], lyric: [], line: [], progress: [] };
  var overrides = { lyric: null, cover: null };
  var last = { songKey: '', lyricKey: '', lineIdx: -2 };

  function txt(el) { return el ? (el.innerText || el.textContent || '') : ''; }

  /* ══ 原生元信息（比 DOM 可靠） ═══════════════════════════════════════
   * 桥方法 network.fetch 的参数形态（实测）：
   *   channel.call('network.fetch', cb, [{ url, method, headers, body }])
   * 回调参数：{ blob: '<响应体字符串>', code, headers, status, ... }
   * 用它直接请求 song/detail 与 song/lyric，不依赖界面是否渲染过歌词。
   */
  function nativeFetchJson(url, cb) {
    try {
      window.channel.call('network.fetch', function (v) {
        try {
          if (!v || typeof v.blob !== 'string') { cb(null, 'bad response'); return; }
          cb(JSON.parse(v.blob), null);
        } catch (e) { cb(null, String(e && e.message)); }
      }, [{ url: url, method: 'GET', headers: {}, body: '' }]);
    } catch (e) { cb(null, String(e && e.message)); }
  }

  function currentSongId() {
    /* 实测结论：playId 前缀 = 当前歌曲 ID（2086091247 查出来正是「你呀你呀」，与播放条一致）；
     * __NCM_AUDIO__.songId 是**预加载的下一首**，不能当当前曲；DOM 歌名会乱跳。 */
    var pid = window.__NCM_PLAYID__;
    if (pid && pid.playId) pid = pid.playId;
    if (typeof pid === 'string') {
      var head = pid.split('_')[0];
      if (/^\d{5,}$/.test(head)) return head;
    }
    var raw = window.__NCM_AUDIO_RAW__;
    if (raw && raw.curPlayId) {
      var h2 = String(raw.curPlayId).split('_')[0];
      if (/^\d{5,}$/.test(h2)) return h2;
    }
    return '';
  }

  /* LRC 解析："[mm:ss.mmm]文本"，支持一行多个时间戳，跳过 [by:]/[ti:] 等元信息 */
  function parseLrc(text) {
    var out = [], seen = {};
    if (!text) return out;
    String(text).split(/\r?\n/).forEach(function (line) {
      var times = [], re = /\[(\d{1,3}):(\d{1,2})[.:](\d{1,3})\]/g, m;
      while ((m = re.exec(line))) {
        var frac = (m[3] + '00').slice(0, 3);
        times.push(parseInt(m[1], 10) * 60000 + parseInt(m[2], 10) * 1000 + parseInt(frac, 10));
      }
      if (!times.length) return;
      var txt = line.replace(/\[[^\]]*\]/g, '').trim();
      times.forEach(function (ms) {
        if (seen[ms] !== undefined) return;
        seen[ms] = out.length;
        out.push({ startMs: ms, text: txt, translation: '' });
      });
    });
    out.sort(function (a, b) { return a.startMs - b.startMs; });
    return out;
  }

  function mergeTranslation(lines, ttext) {
    if (!ttext) return lines;
    var tl = parseLrc(ttext);
    if (!tl.length) return lines;
    var map = {};
    tl.forEach(function (x) { map[x.startMs] = x.text; });
    lines.forEach(function (l) {
      if (map[l.startMs] !== undefined) l.translation = map[l.startMs];
    });
    return lines;
  }

  var metaCache = {};   /* songId -> { name, artists, album, cover, durationMs, lines, metaOk, lyricOk, at } */

  /* ★ 原生数据入口：宿主自己会请求 song/detail 与 song/lyric，
   *   顶层钩子把响应转到这里（必须在 player 闭包里，才能访问 metaCache/parseLrc）。 */
  function ingestByUrl(url, v) {
    if (!v || typeof v.blob !== 'string') return 0;
    var mDetail = url.match(/\/api\/song\/detail\?ids=\[(\d+)\]/);
    var mLyric = url.match(/\/api\/song\/lyric\?.*?[?&]id=(\d+)/);
    var o;
    try { o = JSON.parse(v.blob); } catch (e) { return 0; }
    if (o && o.code && o.code !== 200) return 0;    /* 405/406 限流 */

    if (mDetail) {
      var sid = mDetail[1];
      var s = (o.songs && o.songs[0]) || null;
      if (!s) return 0;
      var rec = metaCache[sid] || {};
      rec.name = s.name || '';
      rec.artists = (s.artists || []).map(function (a) { return a.name; }).filter(Boolean).join('/');
      rec.album = (s.album && s.album.name) || '';
      rec.cover = (s.album && s.album.picUrl) || '';
      rec.durationMs = s.duration || s.dt || 0;
      rec.metaOk = true;
      rec.at = Date.now();
      metaCache[sid] = rec;
      mlog('原生歌曲信息(拦截): ' + sid + ' → ' + rec.name + ' / ' + rec.artists);
      return 1;
    }

    if (mLyric) {
      var sid2 = mLyric[1];
      /* 宿主刚为这首歌请求过歌词 → 这就是"当前歌曲"的强信号（零请求） */
      window.__NCM_LAST_LYRIC_ID__ = { id: sid2, at: Date.now() };
      var rec2 = metaCache[sid2] || {};
      var lines = parseLrc(o && o.lrc && o.lrc.lyric);
      if (o && o.tlyric && o.tlyric.lyric) lines = mergeTranslation(lines, o.tlyric.lyric);
      rec2.lines = lines;
      rec2.lyricOk = lines.length > 0;
      rec2.at = Date.now();
      metaCache[sid2] = rec2;
      mlog('原生歌词(拦截): ' + sid2 + ' → ' + lines.length + ' 行，含翻译 ' +
          lines.filter(function (l) { return l.translation; }).length + ' 行');
      return 1;
    }
    return 0;
  }
  function mlog(m) { try { if (window.ncm && window.ncm.log) window.ncm.log('info', m); } catch (e) {} }
  window.__NCM_INGEST__ = ingestByUrl;
  window.__NCM_META__ = metaCache;

  /* ── 当前歌曲 ID 的校验与兜底 ─────────────────────────────────────────
   * 教训：__NCM_AUDIO__.songId 会滞后（切歌瞬间还指向上一首/预加载的下首），
   * playId 的前缀也**不是**歌曲 ID（实测 459733605 vs 歌曲 34057427）。
   * 所以拿到候选 ID 后，必须用 DOM 上的歌名做校验；不一致就用搜索接口兜底。 */
  function normTitle(s) {
    return String(s || '').toLowerCase()
      .replace(/[\s\-_·・,，.。!！?？'""''()（）【】\[\]《》<>]/g, '');
  }

  function titleMatches(metaName, domName) {
    var a = normTitle(metaName), b = normTitle(domName);
    if (!a || !b) return true;                 /* 缺一方就不否决 */
    return a.indexOf(b) >= 0 || b.indexOf(a) >= 0;
  }

  /* 歌名对得上还不够：同名不同曲太多（实测「海屿你」就有多个歌手版本），
   * 所以歌手也要有交集，否则会拿到别首的歌词。 */
  function artistMatches(metaArtists, domArtist) {
    if (!metaArtists || !domArtist) return true;
    var ma = String(metaArtists).split('/').map(normTitle).filter(Boolean);
    var da = String(domArtist).split(/[\/、,，&]/).map(normTitle).filter(Boolean);
    if (!ma.length || !da.length) return true;
    for (var i = 0; i < da.length; i++) {
      for (var j = 0; j < ma.length; j++) {
        if (da[i] && ma[j] && (da[i].indexOf(ma[j]) >= 0 || ma[j].indexOf(da[i]) >= 0)) return true;
      }
    }
    return false;
  }

  function metaMatches(meta, dom) {
    if (!meta) return false;
    if (!titleMatches(meta.name, dom.name)) return false;
    return artistMatches(meta.artists, dom.artist);
  }

  var searchTried = {};    /* domName -> songId | '' */

  function searchSongId(domName, cb, domArtist) {
    if (!domName) { cb(''); return; }
    var key = domName + '|' + (domArtist || '');
    if (searchTried[key] !== undefined) { cb(searchTried[key]); return; }
    var q = domArtist ? (domName + ' ' + domArtist) : domName;
    var url = 'https://music.163.com/api/search/get?s=' + encodeURIComponent(q) + '&type=1&limit=5';
    nativeFetchJson(url, function (o) {
      var list = (o && o.result && o.result.songs) || [];
      /* 优先挑歌手也对得上的结果，避免同名不同曲 */
      var pick = null;
      for (var i = 0; i < list.length; i++) {
        var ar = (list[i].artists || []).map(function (x) { return x.name; }).join('/');
        if (!domArtist || normTitle(ar).indexOf(normTitle(domArtist)) >= 0 ||
            normTitle(domArtist).indexOf(normTitle(ar)) >= 0) { pick = list[i]; break; }
      }
      if (!pick) pick = list[0] || null;
      var id = pick ? String(pick.id) : '';
      searchTried[key] = id;
      if (id) mlog('搜索兜底: 「' + q + '」→ ' + id + ' (' + (pick.name || '') + ' / ' +
                   (pick.artists || []).map(function (x) { return x.name; }).join('/') + ')');
      cb(id);
    });
  }

  var metaFailAt = {};    /* sid -> 上次失败时间（限流退避） */

  function metaUsable(rec) {
    return !!(rec && (rec.metaOk || (rec.lines && rec.lines.length)));
  }

  /* ── 防频控 ───────────────────────────────────────────────────────────
   * 教训：快速连续切歌时，宿主自己会为每首都请求详情/歌词，我们如果也跟着请求，
   * 叠加起来会打到接口频控（实测 /api/song/detail → 406、/api/search/get → 405）。
   * 所以这里做三道闸：
   *   1) 稳定闸：同一个 ID 连续稳定 ≥1.5s 才发请求（跳过切歌风暴）；
   *   2) 全局节流：任意两次请求间隔 ≥1.2s；
   *   3) 指数退避：命中频控后 30s → 60s → 120s → 300s。
   */
  var STABLE_MS = 1500;
  var MIN_GAP_MS = 1200;
  var lastReqAt = 0;
  var backoffMs = 30000;
  var lastFailGlobal = 0;
  var seenSid = '';
  var seenSidAt = 0;

  function markSid(sid) {
    if (sid !== seenSid) { seenSid = sid; seenSidAt = Date.now(); }
    return Date.now() - seenSidAt;
  }

  function requestAllowed() {
    var now = Date.now();
    if (lastFailGlobal && (now - lastFailGlobal) < backoffMs) return false;
    if ((now - lastReqAt) < MIN_GAP_MS) return false;
    lastReqAt = now;
    return true;
  }

  function ensureMeta(sid) {
    if (!sid) return;
    var have = metaCache[sid];
    if (metaUsable(have)) return;                       /* 已有可用数据 */
    if (have && have.pending && (Date.now() - (have.at || 0)) < 8000) return;  /* 正在请求 */
    var failAt = metaFailAt[sid] || 0;
    if (failAt && (Date.now() - failAt) < 20000) return; /* 单曲退避 20s */

    /* 稳定闸：刚切过来的歌先等它稳定，避免在切歌风暴里刷接口 */
    if (markSid(sid) < STABLE_MS) return;
    if (!requestAllowed()) return;

    metaCache[sid] = { at: Date.now(), pending: true };

    nativeFetchJson('https://music.163.com/api/v3/song/detail?c=' + encodeURIComponent('[{"id":' + sid + '}]'), function (o) {
      var rec = metaCache[sid] || {};
      try {
        var s = (o && o.songs && o.songs[0]) || null;
        if (s) {
          var ars = s.ar || s.artists || [];
          var alb = s.al || s.album || {};
          rec.name = s.name || '';
          rec.artists = ars.map(function (a) { return a.name; }).filter(Boolean).join('/');
          rec.album = alb.name || '';
          rec.cover = alb.picUrl || '';
          rec.durationMs = s.dt || s.duration || 0;
          rec.nativeId = s.id;
          rec.metaOk = true;
        }
      } catch (e) {}
      metaCache[sid] = rec;
      mlog('原生歌曲信息: ' + (rec.name || '?') + ' / ' + (rec.artists || '?'));
    });

    nativeFetchJson('https://music.163.com/api/song/lyric?id=' + sid + '&lv=1&kv=1&tv=-1&yv=1',
      function (o) {
        var rec = metaCache[sid] || {};
        try {
          var lines = parseLrc(o && o.lrc && o.lrc.lyric);
          if (o && o.tlyric && o.tlyric.lyric) lines = mergeTranslation(lines, o.tlyric.lyric);
          rec.lines = lines;
          rec.lyricOk = lines.length > 0;
        } catch (e) { rec.lines = []; }
        rec.pending = false;
        if (o && o.code && o.code !== 200) {
          /* 频控（405/406）：指数退避，别把接口打死 */
          metaFailAt[sid] = Date.now();
          lastFailGlobal = Date.now();
          backoffMs = Math.min(backoffMs * 2, 300000);
          mlog("歌词被频控（code=" + o.code + "），退避 " + Math.round(backoffMs / 1000) + "s");
        } else if (!rec.lyricOk) {
          metaFailAt[sid] = Date.now();
        }
        metaCache[sid] = rec;
        mlog("原生歌词: " + ((rec.lines || []).length) + " 行（含翻译 " +
            (rec.lines || []).filter(function (l) { return l.translation; }).length + " 行）");
      });
  }

  /* DOM 里的歌词（只有渲染过歌词视图才存在） */
  var lyricCache = {};

  /* ── DOM 侧的歌名/歌手/专辑（原生数据缺失或滞后时的基准与兜底） ────── */
  function readSongFromDom() {
    var out = { name: '', artist: '', album: '', source: 'dom' };

    /* 播放器模式：TrackTitleContainer 文本是**一整行无分隔**的，
     * 形如「VIPMV我不难过VIPMV专辑：未完成歌手：孙燕姿来源：热歌榜」 */
    var tc = document.querySelector('[class*=TrackTitleContainer]');
    var s = tc ? (tc.innerText || '').replace(/\s+/g, ' ') : '';
    if (s) {
      /* 宿主在"无歌曲"时会渲染「专辑：未知歌手：来源：未知」这类占位文本，
       * 必须清洗掉，否则会把 未知/来源：未知 当成真实字段。 */
      var clean = function (v) {
        if (!v) return '';
        v = String(v).replace(/^(专辑|歌手|来源)[：:]\s*/, '').trim();
        if (/^(未知|unknown|暂无|无)$/i.test(v)) return '';
        return v;
      };
      var m;
      if ((m = s.match(/专辑[：:]\s*(.+?)(?=歌手[：:]|来源[：:]|$)/))) out.album = clean(m[1]);
      if ((m = s.match(/歌手[：:]\s*(.+?)(?=来源[：:]|专辑[：:]|$)/))) out.artist = clean(m[1]);
      if ((m = s.match(/来源[：:]\s*(.+)$/))) out.source = clean(m[1]);
      var head = s.split(/专辑[：:]|歌手[：:]|来源[：:]/)[0] || '';
      out.name = head.replace(/(VIP|MV|SQ|HQ)/g, '').replace(/未知/g, '').replace(/\s+/g, '').trim();
      /* 整行都是占位信息 → 认为没有歌曲 */
      if (!out.name && !out.album && !out.artist) {
        return { name: '', artist: '', album: '', source: '' };
      }
    }

    /* 迷你播放条：空格分隔，过滤标签词后取前两段 */
    if (!out.name) {
      var b = document.querySelector('[class*=DefaultBarWrapper]') || document.querySelector('[class*=BarWrapper]');
      if (b) {
        var parts = (b.innerText || '').replace(/\s+/g, ' ').trim().split(' ').filter(Boolean);
        var skip = /^(VIP|SQ|HQ|MV|无损|极高|较高|标准|杜比|全景声|\d+(\.\d+)?[wW]\+?|\d+\+?)$/;
        var kept = parts.filter(function (p) { return !skip.test(p) && !/^\d+$/.test(p); });
        out.name = kept[0] || '';
        if (!out.artist) out.artist = kept[1] || '';
      }
    }

    /* 兜底：mediaSession */
    if (!out.name) {
      var ms = null;
      try { ms = navigator.mediaSession && navigator.mediaSession.metadata; } catch (e) {}
      if (ms && ms.title) { out.name = ms.title; out.artist = ms.artist || ''; out.album = ms.album || ''; }
    }
    return out;
  }

  /* ── 统一的可信 ID 解析 ───────────────────────────────────────────────
   * 歌曲与歌词必须用**同一个** ID，否则会出现"界面 A 歌、歌词 B 歌"。
   * 判定顺序：候选 ID → 详情名字与 DOM 歌名吻合 → 采信；
   *           不吻合 → 用歌名搜索出来的 ID（结果缓存，避免重复请求）。
   */
  var trustedId = '';

  function resolveTrustedId() {
    /* ★ 以「音频流」为唯一权威：
     *   __NCM_AUDIO__ 来自宿主下发的 setFetchPreloadInfoResult，描述的是**真正在播放的音频**；
     *   DOM 上的歌名会滞后（实测界面还显示 A，实际已在播 B），
     *   所以不能拿 DOM 去否决音频 ID，否则会互相打架。
     *   音频 ID 缺失时，才退回按 DOM 歌名搜索。 */
    var sid = currentSongId();
    if (sid) {
      ensureMeta(sid);
      trustedId = sid;
      return sid;
    }

    var dom = readSongFromDom();
    /* 宿主刚请求过某首歌的歌词 → 直接采信（零请求、天然同步） */
    var lastLy = window.__NCM_LAST_LYRIC_ID__;
    if (lastLy && (Date.now() - lastLy.at) < 15000) {
      ensureMeta(lastLy.id);
      trustedId = lastLy.id;
      return lastLy.id;
    }
    if (dom.name) {
      var key = dom.name + '|' + (dom.artist || '');
      var found = searchTried[key];
      if (found !== undefined) {
        if (found) ensureMeta(found);
        trustedId = found || '';
        return trustedId;
      }
      searchSongId(dom.name, function (id) { if (id) ensureMeta(id); }, dom.artist);
    }
    trustedId = '';
    return '';
  }

  function readSong() {
    var dom = readSongFromDom();
    var sid = resolveTrustedId();
    var meta = sid ? metaCache[sid] : null;

    if (meta && (meta.name || meta.artists)) {
      return {
        id: parseInt(sid, 10) || 0,
        name: meta.name || dom.name,
        artist: meta.artists || dom.artist,
        album: meta.album || dom.album,
        source: 'native',
        durationMs: meta.durationMs || 0,
        cover: meta.cover || ''
      };
    }
    return dom;
  }

  /* 歌词缓存：NCM 只在「歌词视图」里渲染 .line[data-time]。
   * 迷你播放条模式下 DOM 里根本没有歌词节点，所以一旦看到过就按歌曲缓存起来，
   * 下次即使没打开歌词页也能继续推送（插件需要这个）。 */
  var lyricCache = {};

  function readLyric() {
    var sid = currentSongId();
    if (sid) ensureMeta(sid);

    /* 1) DOM 优先：只有渲染过歌词视图才有 .line[data-time]，但它的"当前行"最准 */
    var box = document.querySelector('.area.lyric-mode') || document.querySelector('[class*=lyric]');
    var lines = [];
    var index = -1;

    if (box) {
      var els = Array.prototype.slice.call(box.querySelectorAll('.line[data-time]'));
      lines = els.map(function (el) {
        /* 每行是**两个 <p>**：p[0]=原文，p[1]=翻译（innerText 会把它们拼成一行） */
        var ps = el.querySelectorAll('p');
        var text = ps.length ? (ps[0].innerText || ps[0].textContent || '').trim() : '';
        var trans = ps.length > 1 ? (ps[1].innerText || ps[1].textContent || '').trim() : '';
        if (!text) text = (el.innerText || '').trim();
        return { t: parseFloat(el.getAttribute('data-time')) || 0, text: text, translation: trans };
      });
      if (els.length) {
        var r = box.getBoundingClientRect();
        var mid = r.top + r.height / 2;
        var best = -1, bestD = 1e9;
        els.forEach(function (el, i) {
          var rr = el.getBoundingClientRect();
          if (rr.height <= 0) return;
          var d = Math.abs((rr.top + rr.height / 2) - mid);
          if (d < bestD) { bestD = d; best = i; }
        });
        index = best;
      }
    }

    if (lines.length) {
      if (sid) lyricCache[sid] = { lines: lines, at: Date.now() };
      return { lines: lines, index: index, current: index >= 0 ? lines[index] : null, cached: false, source: 'dom' };
    }

    /* 2) 原生歌词：从 song/lyric 拉取并解析（不需要界面渲染） */
    var meta = sid ? metaCache[sid] : null;
    if (meta && meta.lines && meta.lines.length) {
      var pr = window.__NCM_PROGRESS__;
      var t = pr && typeof pr.playedTime === 'number' ? pr.playedTime : -1;
      var idx = -1;
      if (t >= 0) {
        for (var k = 0; k < meta.lines.length; k++) {
          if (meta.lines[k].startMs <= t * 1000 + 300) idx = k; else break;
        }
      }
      var conv = meta.lines.map(function (l) {
        return { t: l.startMs / 1000, text: l.text, translation: l.translation };
      });
      return { lines: conv, index: idx, current: idx >= 0 ? conv[idx] : null, cached: true, source: 'native' };
    }

    /* 3) 兜底：之前从 DOM 观察到的缓存 */
    if (sid && lyricCache[sid] && (Date.now() - lyricCache[sid].at) < 30 * 60 * 1000) {
      var cached = lyricCache[sid].lines;
      var pr2 = window.__NCM_PROGRESS__;
      var idx2 = -1;
      if (pr2 && typeof pr2.playedTime === 'number') {
        for (var j = 0; j < cached.length; j++) {
          if (cached[j].t <= pr2.playedTime + 0.3) idx2 = j; else break;
        }
      }
      return { lines: cached, index: idx2, current: idx2 >= 0 ? cached[idx2] : null, cached: true, source: 'dom-cache' };
    }

    return { lines: [], index: -1, current: null, cached: false, source: 'none' };
  }

  function readCover() {
    /* 优先级很重要：页面上的 banner 广告图比播放条封面大得多，
     * 只按"最大图"会拿到广告图（实测取到了首页 banner）。 */
    var pick = '';

    /* 1) 迷你播放条里的封面（最准确） */
    var bar = document.querySelector('[class*=DefaultBarWrapper]') || document.querySelector('[class*=BarWrapper]');
    if (bar) {
      var bimg = bar.querySelector('img');
      if (bimg) {
        var bs = String(bimg.currentSrc || bimg.src || '');
        if (bs.indexOf('music.126.net') >= 0) pick = bs;
      }
    }

    /* 2) 播放器模式（黑胶页）里的封面 */
    if (!pick) {
      var vinyl = document.querySelector('[class*=VinylPageContainer]');
      if (vinyl) {
        var vi = vinyl.querySelector('img[src*="music.126.net"]');
        if (vi) pick = String(vi.currentSrc || vi.src || '');
      }
    }

    /* 3) 兜底：全页最大、且不像 UI 资源/广告的图 */
    if (!pick) {
      var area = 0;
      document.querySelectorAll('img').forEach(function (el) {
        var s = String(el.currentSrc || el.src || '');
        if (s.indexOf('music.126.net') < 0) return;
        if (/\/(vinyl|listenTogether|play|common|public)\//.test(s)) return;
        var r = el.getBoundingClientRect();
        var a = (r.width || 0) * (r.height || 0);
        if (a > area) { area = a; pick = s; }
      });
    }
    return pick;
  }

  /* 把封面 URL 的 thumbnail=NyN 改成目标尺寸（CDN 支持，实测 600y600 可用） */
  function coverUrlAt(size) {
    var s = readCover();
    if (!s) return '';
    var n = parseInt(size, 10) || 600;
    return s.replace(/thumbnail=\d+y\d+/, 'thumbnail=' + n + 'y' + n);
  }

  function mode() {
    if (document.querySelector('[class*=VinylPageContainer]')) return 'vinyl';
    if (document.querySelector('[class*=should-hide-under-vinyl-mode]')) return 'bar';
    return 'unknown';
  }

  function emit(evt, data) {
    (listeners[evt] || []).forEach(function (fn) {
      try { fn(data); } catch (e) { console.error('[ncm.player]', e); }
    });
  }

  function snapshot() {
    var song = readSong();
    var lyric = readLyric();
    var cover = (song && song.cover) || readCover();
    if (overrides.lyric) {
      try {
        var rep = overrides.lyric(lyric);
        if (rep && rep.lines) lyric = rep;
      } catch (e) {}
    }
    if (overrides.cover) {
      try { var c2 = overrides.cover(cover); if (c2) cover = c2; } catch (e) {}
    }
    var snapOut = { song: song, lyric: lyric, cover: cover, mode: mode(), at: Date.now() };
    window.__NCM_LAST_SNAP__ = snapOut;
    return snapOut;
  }

  function tick() {
    var snap = snapshot();
    var k1 = snap.song.name + '|' + snap.song.artist;
    if (snap.song.name && k1 !== last.songKey) { last.songKey = k1; emit('song', snap.song); }
    var k2 = snap.lyric.lines.length + ':' + (snap.lyric.lines[0] ? snap.lyric.lines[0].text : '');
    if (snap.lyric.lines.length && k2 !== last.lyricKey) { last.lyricKey = k2; emit('lyric', snap.lyric); }
    if (snap.lyric.index !== last.lineIdx) {
      last.lineIdx = snap.lyric.index;
      if (snap.lyric.current) emit('line', { index: snap.lyric.index, line: snap.lyric.current });
    }
    emit('progress', snap);
  }

  var api = {
    current: snapshot,
    on: function (evt, fn) { (listeners[evt] = listeners[evt] || []).push(fn); return api; },
    off: function (evt, fn) { var a = listeners[evt] || []; var i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); return api; },
    override: function (o) { overrides = { lyric: (o && o.lyric) || null, cover: (o && o.cover) || null }; return api; },
    getCoverURL: coverUrlAt,
    debug: function () { var s = snapshot(); return { mode: s.mode, name: s.song.name, artist: s.song.artist, album: s.song.album, lines: s.lyric.lines.length, idx: s.lyric.index, cover: s.cover }; }
  };

  window.ncm = window.ncm || {};
  window.ncm.player = api;

  try {
    var mo = new MutationObserver(function () { tick(); });
    mo.observe(document.documentElement, { subtree: true, childList: true, characterData: true });
  } catch (e) {}
  setInterval(tick, 1000);
  setTimeout(tick, 600);
})();
/* ══════════════════════════════════════════════════════════════════════════
 * ncm.player.audio —— 从原生桥捕获音频信息（实机验证）
 *
 * 数据来源：页面通过 channel.call('audioplayer.setFetchPreloadInfoResult', ret, args)
 * 收到预加载音频的信息，args[0].info 里含：
 *   { songId, musicurl, soundQuality, bitrate, br, fileSize, songDuration(ms), md5 }
 * 这是目前唯一能拿到「原始音频地址 + 时长 + 音质 + 码率」的地方（DOM 里没有 audio 元素）。
 * ══════════════════════════════════════════════════════════════════════════ */
(function () {
  if (window.__NCM_AUDIO_HOOK__) return;
  window.__NCM_AUDIO_HOOK__ = 1;
  window.__NCM_AUDIO__ = null;

  var ch = window.channel;
  if (ch && typeof ch.call === 'function') {
    var orig = ch.call;
    ch.call = function (name, ret, args) {
      try {
        if (String(name) === 'audioplayer.setFetchPreloadInfoResult') {
          var a0 = args && args[0];
          var info = a0 && (a0.info || a0);
          var prm = (a0 && a0.params) || {};
          window.__NCM_AUDIO_RAW__ = {
            preloadIndex: prm.preloadIndex, curPlayId: prm.curPlayId,
            preloadPlayId: prm.preloadPlayId, songId: info && info.songId,
            at: Date.now()
          };
          if (info && info.songId) {
            var rec = {
              songId: String(info.songId),
              url: String(info.musicurl || ""),
              quality: String(info.soundQuality || ""),
              bitrate: info.bitrate || 0,
              fileSize: info.fileSize || 0,
              durationMs: parseInt(info.songDuration || "0", 10) || 0,
              md5: String(info.md5 || ""),
              at: Date.now()
            };
            /* ★ preloadIndex: 0 = 当前正在播的音频，1 = 预加载的下一首。
             *   不区分会让"下一首"覆盖当前曲 → SSE 与播放对不上。 */
            var pidx = (prm.preloadIndex === undefined || prm.preloadIndex === null)
                       ? 0 : Number(prm.preloadIndex);
            if (pidx === 0) {
              window.__NCM_AUDIO__ = rec;
              window.__NCM_TRACK_AT__ = Date.now();
            } else {
              window.__NCM_AUDIO_NEXT__ = rec;
            }
          }
        }
      } catch (e) {}
      return orig.apply(this, arguments);
    };
  }

  /* 把 audio 挂到 ncm.player.current() 的返回里 */
  if (window.ncm && window.ncm.player && !window.ncm.player.__audio_patched) {
    var base = window.ncm.player.current;
    window.ncm.player.current = function () {
      var s = base();
      s.audio = window.__NCM_AUDIO__;
      return s;
    };
    window.ncm.player.__audio_patched = 1;
  }
})();

/* ══════════════════════════════════════════════════════════════════════════
 * ncm.native / betterncm 兼容层 —— 对齐参照项目的 API 形态
 *
 * 参照项目（BetterNCM）的风格是「一个 native 对象 + 点号命名空间」：
 *     betterncm_native.fs.readDir(...) / app.version / native_plugin.call(...)
 * 这里做同样的事，并额外提供媒体相关能力（参照项目把这块放在 JS 层）：
 *
 *   ncm.native.call(name, args) → Promise     底层桥
 *   ncm.native.audioplayer.*                  播放器原生方法
 *   betterncm.ncm.getPlayingSong()            当前歌曲（含音频信息）
 *   betterncm.ncm.getLyrics()                 歌词（带时间戳）
 *   betterncm.ncm.getPlayedTime()             当前播放位置（原生，精确）
 *   betterncm.ncm.play() / pause() / seek(ms) / setVolume(v)
 *
 * 原生桥签名（实测）：channel.call(name, callback, args)
 *   第三个参数必须存在但可为空数组；第二个必须是函数，否则抛
 *   "Invalid method arguments. second argument must be a function"。
 * ══════════════════════════════════════════════════════════════════════════ */
(function () {
  if (window.__NCM_NATIVE_API__) return;
  window.__NCM_NATIVE_API__ = 1;

  function rawCall(name, args) {
    return new Promise(function (resolve, reject) {
      var ch = window.channel;
      if (!ch || typeof ch.call !== 'function') { reject(new Error('no channel')); return; }
      var done = false;
      var timer = setTimeout(function () {
        if (!done) { done = true; resolve(undefined); }
      }, 4000);
      try {
        ch.call(name, function (v) {
          if (done) return;
          done = true;
          clearTimeout(timer);
          resolve(v);
        }, args || []);
      } catch (e) {
        if (!done) { done = true; clearTimeout(timer); reject(e); }
      }
    });
  }

  var native = {
    call: rawCall,
    audioplayer: {
      getPlayedTime: function () { return rawCall('audioplayer.getPlayedTime', []); },
      getPlaybackInfo: function () { return rawCall('audioplayer.getPlaybackInfo', []); },
      play: function () { return rawCall('audioplayer.play', []); },
      pause: function () { return rawCall('audioplayer.pause', []); },
      seek: function (ms) { return rawCall('audioplayer.seek', [ms]); },
      setVolume: function (v) { return rawCall('audioplayer.setVolume', [v]); },
      preLoad: function (info) { return rawCall('audioplayer.preLoad', [info]); }
    }
  };

  window.ncm = window.ncm || {};
  window.ncm.native = native;

  /* 把原生能力挂到 player 上，作为 DOM 推断之外的精确来源 */
  if (window.ncm.player) {
    window.ncm.player.getPlayedTime = native.audioplayer.getPlayedTime;
    window.ncm.player.getPlaybackInfo = native.audioplayer.getPlaybackInfo;
    /* ★ 控制一律走「页面级点击」：
     *   实测原生 audioplayer.play/pause/setVolume 经页面桥调用**不生效**
     *   （playedTime 照走、音量读回不变），而错误参数还会把进程打崩。
     *   点 UI 走的是宿主自己的代码路径，安全且有效（已实测：点暂停后按钮图标变化）。 */
    function clickIcon(ic) {
      if (!ic) return false;
      var btn = ic.closest('button') || ic;
      var r = btn.getBoundingClientRect();
      var opt = { bubbles: true, cancelable: true, clientX: r.left + r.width / 2, clientY: r.top + r.height / 2, button: 0 };
      ['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click'].forEach(function (t) {
        try { btn.dispatchEvent(new (t.indexOf('pointer') === 0 ? PointerEvent : MouseEvent)(t, opt)); } catch (e) {}
      });
      return true;
    }
    function barRoot() {
      return document.querySelector('[class*=DefaultBarWrapper]') ||
             document.querySelector('[class*=BarWrapper]') ||
             document.querySelector('[class*=VinylPageContainer]') ||
             document.body;
    }
    function iconBy(cls) {
      var root = barRoot();
      return root ? root.querySelector('.cmd-icon-' + cls) : null;
    }
    window.ncm.player.control = {
      play: function () { return clickIcon(iconBy('play')); },
      pause: function () { return clickIcon(iconBy('pause')); },
      toggle: function () { return clickIcon(iconBy('pause') || iconBy('play')); },
      next: function () { return clickIcon(iconBy('next')); },
      prev: function () { return clickIcon(iconBy('pre')); },
      isPlaying: function () {
        /* 某些界面没有播放条，改扫全文档的暂停/播放图标 */
        if (document.querySelector('.cmd-icon-pause')) return true;
        if (document.querySelector('.cmd-icon-play')) return false;
        return !!iconBy('pause');
      }
    };
  }

  /* ── betterncm 兼容层（插件按参照项目写法也能跑） ── */
  var bn = window.betterncm = window.betterncm || {};

  /* fs / app 直接映射到我们的实现（命名与参照项目一致） */
  bn.fs = bn.fs || (window.ncm && window.ncm.fs) || {};
  bn.app = bn.app || (window.ncm && window.ncm.app) || {};

  bn.ncm = bn.ncm || {};
  if (!bn.ncm.__filled) {
    bn.ncm.__filled = 1;
    bn.ncm.getPlayingSong = function () {
      var s = window.ncm.player.current();
      return {
        name: s.song.name,
        artist: s.song.artist,
        album: s.song.album,
        source: s.song.source,
        id: s.audio ? s.audio.songId : '',
        durationMs: (s.song && s.song.durationMs) || (s.audio ? s.audio.durationMs : 0),
        cover: s.cover,
        audioUrl: s.audio ? s.audio.url : '',
        quality: s.audio ? s.audio.quality : '',
        bitrate: s.audio ? s.audio.bitrate : 0
      };
    };
    bn.ncm.getLyrics = function () {
      var s = window.ncm.player.current();
      return s.lyric.lines;
    };
    bn.ncm.getCurrentLyricLine = function () {
      var s = window.ncm.player.current();
      return s.lyric.current;
    };
    bn.ncm.getCover = function () { return window.ncm.player.current().cover; };
    bn.ncm.getPlayedTime = native.audioplayer.getPlayedTime;
    bn.ncm.getPlaybackInfo = native.audioplayer.getPlaybackInfo;
    bn.ncm.play = native.audioplayer.play;
    bn.ncm.pause = native.audioplayer.pause;
    bn.ncm.seek = native.audioplayer.seek;
    bn.ncm.setVolume = native.audioplayer.setVolume;
    /* 监听 */
    bn.ncm.onSongChange = function (cb) { return window.ncm.player.on('song', cb); };
    bn.ncm.onLyricChange = function (cb) { return window.ncm.player.on('lyric', cb); };
    bn.ncm.onLyricLine = function (cb) { return window.ncm.player.on('line', cb); };
    bn.ncm.onProgress = function (cb) { return window.ncm.player.on('progress', cb); };
  }
})();

/* ══════════════════════════════════════════════════════════════════════════
 * ncm.player.progress —— 原生精确进度（实测可用）
 *
 * 实测（2026-10-24，播放中）：
 *   channel.call('audioplayer.getPlayedTime', cb, [{playId}])
 *   → { playedAudioTime: 155.666, playedTime: 155.615, result: true }
 *   playedTime 单位是**秒**（带小数）。
 *
 * 关键：必须传**真实的 playId**（形如 "1887140296_6YW7DQ"，后缀随机）。
 * 它由原生通过 audioplayer.setFetchPreloadInfoResult 的参数下发：
 *   args[0].params.curPlayId / args[0].params.preloadPlayId
 * 乱猜 playId 会得到 {playedTime:0, result:false}。
 *
 * 有了它，歌词同步就不再依赖"歌词区垂直中心"这种启发式。
 * ══════════════════════════════════════════════════════════════════════════ */
(function () {
  if (window.__NCM_PROGRESS_API__) return;
  window.__NCM_PROGRESS_API__ = 1;

  window.__NCM_PROGRESS__ = null;

  /* 1) 抓 playId */
  var ch = window.channel;
  if (ch && typeof ch.call === 'function' && !window.__NCM_PLAYID_WRAP__) {
    window.__NCM_PLAYID_WRAP__ = 1;
    var orig = ch.call;
    ch.call = function (name, cb, args) {
      try {
        if (String(name) === 'audioplayer.setFetchPreloadInfoResult' && args && args[0]) {
          var p = args[0].params || {};
          if (p.curPlayId) window.__NCM_PLAYID__ = p.curPlayId;
          else if (p.preloadPlayId) window.__NCM_PLAYID__ = p.preloadPlayId;
        }
      } catch (e) {}
      return orig.apply(this, arguments);
    };
  }

  /* ★ 实测：audioplayer.getPlayedTime 传**一个空对象** [{}] 就能拿到当前进度，
   *   不需要 playId（之前传 [] 失败，误判成"必须 playId"）。
   *   有 playId 时优先用它，失败再退回 [{}]。 */
  function fetchPlayedTime() {
    return new Promise(function (resolve) {
      if (!window.channel || typeof window.channel.call !== 'function') { resolve(null); return; }

      var done = false;
      var timer = setTimeout(function () { if (!done) { done = true; resolve(null); } }, 2000);

      function finish(v) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        if (v && typeof v.playedTime === 'number' && v.result) resolve(v);
        else resolve(null);
      }

      function ask(args) {
        try {
          window.channel.call('audioplayer.getPlayedTime', function (v) { finish(v); }, args);
        } catch (e) { finish(null); }
      }

      var pid = window.__NCM_PLAYID__;
      if (pid && pid.playId) pid = pid.playId;
      if (typeof pid === 'string' && pid) {
        /* 有 playId：先精确查询，失败自动退回空对象 */
        try {
          window.channel.call('audioplayer.getPlayedTime', function (v) {
            if (v && typeof v.playedTime === 'number' && v.result) { finish(v); return; }
            if (!done) ask([{}]);
          }, [{ playId: pid }]);
        } catch (e) { ask([{}]); }
      } else {
        ask([{}]);
      }
    });
  }

  /* 2) 进度锚点：原生 getPlayedTime 给"精确值"，本地时钟负责"两次轮询之间"的连续性。
   *    之前只依赖轮询成功，一旦返回失败（playId 失效等）进度就冻结 → 歌词停在开头。 */
  var anchor = null;      /* { playedTime, playedAudioTime, at, playId } */

  function detectPlaying() {
    /* 播放条在某些界面不存在，所以扫全文档的播放/暂停图标 */
    if (document.querySelector(".cmd-icon-pause")) return true;
    if (document.querySelector(".cmd-icon-play")) return false;
    return null;
  }

  function estimatePosition(durMs) {
    if (!anchor) return null;
    var playing = detectPlaying();
    var pos = anchor.playedTime + (playing === true ? (Date.now() - anchor.at) / 1000 : 0);
    var dur = (durMs || 0) / 1000;
    if (dur > 0 && pos > dur) pos = dur;
    if (pos < 0) pos = 0;
    return { pos: pos, playing: playing, fresh: (Date.now() - anchor.at) < 4000 };
  }

  setInterval(function () {
    var pid = window.__NCM_PLAYID__;
    if (!pid) return;
    fetchPlayedTime().then(function (v) {
      if (v) {
        anchor = {
          playedTime: v.playedTime,
          playedAudioTime: v.playedAudioTime,
          at: Date.now(),
          playId: pid
        };
        window.__NCM_PROGRESS__ = {
          playedTime: v.playedTime,
          playedAudioTime: v.playedAudioTime,
          at: anchor.at,
          playId: pid,
          ok: true
        };
      } else if (anchor) {
        /* 轮询失败：保留锚点，靠外推继续走 */
        window.__NCM_PROGRESS__ = {
          playedTime: anchor.playedTime,
          playedAudioTime: anchor.playedAudioTime,
          at: anchor.at,
          playId: anchor.playId,
          ok: false
        };
      }
    });
  }, 1000);

  /* 3) 用"外推后的位置"校正歌词当前行 */
  if (window.ncm && window.ncm.player && !window.ncm.player.__progress_patched) {
    var base = window.ncm.player.current;
    window.ncm.player.current = function () {
      var s = base();
      var dur = (s.song && s.song.durationMs) || (s.audio && s.audio.durationMs) || 0;
      var est = estimatePosition(dur);
      if (est) {
        s.progress = { playedTime: est.pos, at: Date.now(), playing: est.playing, estimated: !est.fresh };
        var lines = s.lyric.lines || [];
        if (lines.length) {
          var idx = -1;
          for (var i = 0; i < lines.length; i++) {
            if (lines[i].t <= est.pos + 0.3) idx = i; else break;
          }
          if (idx >= 0) { s.lyric.index = idx; s.lyric.current = lines[idx]; }
        }
        if (est.playing === false) s.playing = false;
        if (est.playing === true) s.playing = true;
      }
      return s;
    };
    window.ncm.player.getPlayedTime = fetchPlayedTime;
    window.ncm.player.getProgress = function () {
      var dur = 0;
      try { var c = window.ncm.player.current(); dur = c.song ? c.song.durationMs : 0; } catch (e) {}
      var est = estimatePosition(dur);
      return est ? { playedTime: est.pos, at: Date.now(), playing: est.playing } : null;
    };
    window.ncm.player.__progress_patched = 1;
  }
})();

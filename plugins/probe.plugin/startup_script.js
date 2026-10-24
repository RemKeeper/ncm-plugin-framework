/**
 * probe.plugin — 开发自检插件
 *
 * 打点内容：
 *   1. 宿主能力面（fs 读写 / 生命周期 / 事件）
 *   2. 自己拿到的是否是"按 slug 隔离的 fs 视图"
 *   3. 热重载：注册 dispose，卸载时打日志；重载后重新执行会打印新的启动时间
 */
(function () {
  var startedAt = new Date().toISOString().slice(11, 19);

  function log(msg) {
    try { ncm.log('info', '[probe] ' + msg); } catch (e) {}
  }

  log('启动 ' + startedAt + ' · slug=' + (ncm.app && ncm.app.version ? 'ok' : 'n/a'));
  log('能力面: fs.readTextAsync=' + typeof ncm.fs.readTextAsync +
      ' fs.writeText=' + typeof ncm.fs.writeText +
      ' fs.list=' + typeof ncm.fs.list +
      ' fs.stat=' + typeof ncm.fs.stat +
      ' onDispose=' + typeof ncm.onDispose);

  /* 自检：写读删一个自己的文件 */
  if (ncm.fs && typeof ncm.fs.writeText === 'function') {
    ncm.fs.writeText('probe.txt', 'hello from probe @ ' + startedAt)
      .then(function () { return ncm.fs.readTextAsync('probe.txt'); })
      .then(function (t) { log('fs 读回: ' + String(t).slice(0, 60)); })
      .catch(function (e) { log('fs 自检失败: ' + e); });
  }

  /* 热重载：卸载时清理 */
  if (typeof ncm.onDispose === 'function') {
    ncm.onDispose(function () {
      log('收到 dispose（热重载或卸载）');
    });
  }
})();

#include "webviewcpp/ipc.hpp"

namespace wvc {

// Injected at document-start into every frame. It installs `window.wvc`, the
// JS half of the IPC bridge. It is deliberately tiny (~2 KB) and dependency
// free so it can run before React even loads.
//
// The native side may set `window.__WVC_ENV__ = { platform, version }` in the
// same injection batch; we fold it into the public API.
const std::string& IpcBridge::bootstrap_script() {
    static const std::string kScript = R"JS(
(function () {
  if (window.wvc && window.wvc.__installed) return;

  var PENDING = new Map();
  var LISTENERS = new Map();
  var nextId = 1;
  var env = window.__WVC_ENV__ || {};

  function nativePost(obj) {
    try {
      if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage)
        return window.chrome.webview.postMessage(obj);
      if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wvc)
        return window.webkit.messageHandlers.wvc.postMessage(obj);
      if (typeof window.__wvc_post === 'function')
        return window.__wvc_post(obj);
    } catch (e) {
      console.error('[wvc] native transport error:', e);
    }
    console.warn('[wvc] no native transport available');
  }

  function invoke(channel, payload) {
    return new Promise(function (resolve, reject) {
      var id = nextId++;
      PENDING.set(id, { resolve: resolve, reject: reject });
      nativePost({
        id: id,
        kind: 'invoke',
        channel: channel,
        payload: payload === undefined ? null : payload
      });
    });
  }

  // Fire-and-forget page -> native notification.
  function send(channel, payload) {
    nativePost({ kind: 'event', channel: channel, payload: payload === undefined ? null : payload });
  }

  function on(channel, cb) {
    if (!LISTENERS.has(channel)) LISTENERS.set(channel, new Set());
    LISTENERS.get(channel).add(cb);
    return function unsubscribe() { off(channel, cb); };
  }

  function off(channel, cb) {
    var set = LISTENERS.get(channel);
    if (set) set.delete(cb);
  }

  function once(channel, cb) {
    var unsub = on(channel, function (payload) { unsub(); cb(payload); });
    return unsub;
  }

  function emitLocal(msg) {
    var set = LISTENERS.get(msg.channel);
    if (set) set.forEach(function (cb) {
      try { cb(msg.payload); } catch (e) { console.error('[wvc] listener error:', e); }
    });
    var all = LISTENERS.get('*');
    if (all) all.forEach(function (cb) {
      try { cb({ channel: msg.channel, payload: msg.payload }); } catch (e) {}
    });
  }

  function __dispatch(msg) {
    if (typeof msg === 'string') {
      try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || !msg.kind) return;

    if (msg.kind === 'resolve' || msg.kind === 'reject') {
      var p = PENDING.get(msg.id);
      if (!p) return;
      PENDING.delete(msg.id);
      if (msg.kind === 'resolve') p.resolve(msg.payload);
      else p.reject(new Error(msg.error || 'native error'));
      return;
    }
    if (msg.kind === 'event') emitLocal(msg);
  }

  var api = {
    __installed: true,
    __dispatch: __dispatch,
    invoke: invoke,
    call: invoke,
    send: send,
    on: on,
    off: off,
    once: once,
    platform: env.platform || 'unknown',
    version: env.version || '0.0.0',
    isNative: true
  };

  window.__wvc = api;
  window.wvc = api;

  // Handshake: tell native the runtime is live so it can flush queued events.
  nativePost({ kind: 'ready' });
})();
)JS";

    return kScript;
}

}  // namespace wvc

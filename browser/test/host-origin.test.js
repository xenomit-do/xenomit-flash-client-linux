const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const bridge = require('../src/bridge');
(async () => {
  const calls = [];
  let handler;
  const dbg = { attach() {}, isAttached: () => true,
    on: (name, fn) => { handler = fn; },
    sendCommand: async (method, params) => { calls.push({method, params}); } };
  const window = { webContents: { debugger: dbg }, loadURL: async url => { window.url = url; } };
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flash-origin-test-'));
  const file = path.join(dir, 'host.html');
  const html = '<html>private bootstrap stays local</html>';
  fs.writeFileSync(file, html);
  try {
    await bridge.installMoviePatch(window, 'https://cdn.example/game.swf?v=1', 'https://game.example/index.php');
    await assert.rejects(bridge.loadHostPage(window, file, 'file:///tmp/host.html'));
    await bridge.loadHostPage(window, file, 'https://game.example/index.php');
    assert.strictEqual(new URL(window.url).origin, 'https://game.example');
    const enabled = calls.filter(c => c.method === 'Fetch.enable').pop().params.patterns;
    assert.strictEqual(enabled.length, 2);
    assert(!enabled.some(p => p.urlPattern === '*'));
    await handler(null, 'Fetch.requestPaused', { requestId: 'host', request: {url: window.url, method:'GET'} });
    const reply = calls.pop();
    assert.strictEqual(reply.method, 'Fetch.fulfillRequest');
    assert.strictEqual(Buffer.from(reply.params.body, 'base64').toString(), html);
    await handler(null, 'Fetch.requestPaused', { requestId: 'other', request: {url:'https://game.example/asset.xml',method:'GET'} });
    assert.strictEqual(calls.pop().method, 'Fetch.continueRequest');
    dbg.sendCommand = async (method, params) => {
      calls.push({method, params});
      if (method === 'Fetch.fulfillRequest') throw new Error('test failure');
    };
    await handler(null, 'Fetch.requestPaused', { requestId: 'host-failed', request: {url: window.url,method:'GET'} });
    assert.strictEqual(calls.pop().method, 'Fetch.failRequest');
    console.log('host origin interception tests passed');
  } finally { fs.unlinkSync(file); fs.rmdirSync(dir); }
})().catch(e => { console.error(e); process.exitCode=1; });

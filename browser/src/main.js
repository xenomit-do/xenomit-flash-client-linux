const { app, BrowserWindow, session } = require('electron')
const crypto = require('crypto')
const fs = require('fs')
const os = require('os')
const path = require('path')
const bridge = require('./bridge')

const PEPPER_VERSION = '32.0.0.371'
const repoRoot = path.join(__dirname, '..', '..')

function log(message) {
  process.stderr.write(`[xenomit-flash] ${message}\n`)
}

function parseArgs(argv) {
  const options = {}
  for (let i = 1; i < argv.length; i++) {
    const arg = argv[i]
    if (!arg.startsWith('--')) continue
    const next = argv[i + 1]
    if (next && !next.startsWith('--')) {
      options[arg] = next
      i++
    } else {
      options[arg] = '1'
    }
  }
  return options
}

function readRecord(fd, buffer) {
  let line = ''
  while (line.length <= 65535) {
    const n = fs.readSync(fd, buffer, 0, 1, null)
    if (n === 0) break
    const ch = buffer.toString('utf8', 0, 1)
    if (ch === '\n') break
    if (ch !== '\r') line += ch
  }
  return line
}

function readBootstrap() {
  const buffer = Buffer.alloc(1)
  const movie = readRecord(0, buffer)
  const page = readRecord(0, buffer)
  const sid = readRecord(0, buffer)
  const vars = readRecord(0, buffer)
  const width = readRecord(0, buffer)
  const height = readRecord(0, buffer)
  if (!movie || !page || !sid || !vars || !width || !height) {
    log('missing bootstrap record')
    process.exit(2)
  }
  return { movie, page, sid, vars, width: Number(width), height: Number(height) }
}

function escapeAttr(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;')
}

function pepperPath(options) {
  return options['--flash'] || process.env.XENOMIT_FLASH_PLUGIN_PATH || path.join(repoRoot, 'runtime', 'libpepflashplayer.so')
}

function assertPepper(file) {
  if (!fs.existsSync(file)) {
    log('Pepper plugin is missing. Place the pinned libpepflashplayer.so in runtime/ or pass --flash')
    process.exit(2)
  }
  const expected = fs.readFileSync(path.join(repoRoot, 'runtime', 'pepper.sha256'), 'utf8').trim().toLowerCase()
  const actual = crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
  if (actual !== expected) {
    log('Pepper plugin hash does not match the pinned runtime')
    process.exit(2)
  }
}

function writeEmbed(bootstrap) {
  const width = Number.isFinite(bootstrap.width) && bootstrap.width > 100 ? bootstrap.width : 1280
  const height = Number.isFinite(bootstrap.height) && bootstrap.height > 100 ? bootstrap.height : 800
  const html = `<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><style>html,body{width:100%;height:100%}embed{display:block;width:100%;height:100%}</style>
<script>
function xenomitBackend() { return "pepper-native"; }
function xenomitNativeFrame() {}
function xenomitMetrics() {}
function xenomitCapture() {}
function xenomitNavigation() {}
function xenomitCatalog() {}
function xenomitItems() {}
function xenomitItemsFresh() {}
function xenomitCall(name, args) {
  var embed = document.querySelector("embed");
  if (!embed) throw new Error("Flash callback unavailable");
  var direct = embed[name];
  if (typeof direct === "function") {
    var value = direct.apply(embed, args);
    if (value == null) throw new Error("Flash callback unavailable");
    return String(value);
  }
  if (typeof embed.CallFunction !== "function") throw new Error("Flash callback unavailable");
  return parseFlashXml(embed.CallFunction(flashRequestXml(name, args)));
}
function flashEscape(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&apos;");
}
function flashRequestXml(name, args) {
  var xml = '<invoke name="' + flashEscape(name) + '" returntype="xml"><arguments>';
  for (var i = 0; i < args.length; i++) xml += "<string>" + flashEscape(args[i]) + "</string>";
  return xml + "</arguments></invoke>";
}
function flashUnescape(value) {
  return String(value)
    .replace(/&apos;/g, "'")
    .replace(/&quot;/g, '"')
    .replace(/&gt;/g, ">")
    .replace(/&lt;/g, "<")
    .replace(/&amp;/g, "&");
}
function parseFlashXml(response) {
  var text = String(response == null ? "" : response);
  var open = text.indexOf("<string");
  if (open < 0) return flashUnescape(text);
  var start = text.indexOf(">", open);
  var close = text.indexOf("</string>", start < 0 ? open : start);
  if (start < 0 || close < 0) return flashUnescape(text);
  return flashUnescape(text.slice(start + 1, close));
}
</script>
</head>
<body style="margin:0;background:#000;overflow:hidden">
<embed type="application/x-shockwave-flash"
  src="${escapeAttr(bootstrap.movie)}"
  base="${escapeAttr(bootstrap.page)}"
  flashvars="${escapeAttr(bootstrap.vars)}"
  width="100%" height="100%"
  quality="low" wmode="window"
  allowScriptAccess="always" allowNetworking="all">
</body>
</html>`
  const file = path.join(os.tmpdir(), `xenomit-flash-${process.pid}.html`)
  fs.writeFileSync(file, html, { mode: 0o600 })
  return { file, width, height }
}

const options = parseArgs(process.argv)
if (options['--backend'] && options['--backend'] !== 'native') {
  log('Linux flash client only supports --backend native')
  process.exit(2)
}
if (!options['--launch-stdin']) {
  log('--launch-stdin is required')
  process.exit(2)
}

const flashFile = pepperPath(options)
assertPepper(flashFile)
const bootstrap = readBootstrap()
const embed = writeEmbed(bootstrap)
const stopBridge = bridge.start()
app.on('will-quit', stopBridge)

app.commandLine.appendSwitch('no-sandbox')
app.commandLine.appendSwitch('ppapi-flash-path', flashFile)
app.commandLine.appendSwitch('ppapi-flash-version', PEPPER_VERSION)
app.commandLine.appendSwitch('disable-background-timer-throttling')
app.commandLine.appendSwitch('disable-renderer-backgrounding')
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows')

function watchParent() {
  const parent = Number(options['--parent-pid'] || '0')
  if (!parent) return
  setInterval(() => {
    try {
      process.kill(parent, 0)
    } catch (error) {
      if (error.code === 'ESRCH') app.exit(0)
    }
  }, 2000).unref()
}

app.whenReady().then(async () => {
  watchParent()
  const hidden = options['--hidden'] === '1'
  const window = new BrowserWindow({
    width: embed.width,
    height: embed.height,
    show: !hidden,
    title: 'Xenomit Flash Client',
    autoHideMenuBar: true,
    backgroundColor: '#000000',
    webPreferences: {
      plugins: true,
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,
      spellcheck: false,
      preload: path.join(__dirname, 'preload.js')
    }
  })
  window.webContents.setUserAgent('BigpointClient/1.6.9')
  bridge.attach(window)
  const page = bootstrap.page
  if (/^https?:\/\//i.test(page)) {
    await session.defaultSession.cookies.set({ url: page, name: 'dosid', value: bootstrap.sid })
  }
  await bridge.installMoviePatch(window, bootstrap.movie, page)
  await bridge.loadHostPage(window, embed.file, page)
  bridge.startNavigation()
  window.on('closed', () => {
    fs.unlink(embed.file, () => {})
  })
  log('player ready')
}).catch(error => {
  log(`player startup failed: ${error.message}`)
  app.exit(2)
})

app.on('window-all-closed', () => app.quit())

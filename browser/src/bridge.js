const { spawn } = require('child_process')
const fs = require('fs')
const http = require('http')
const https = require('https')
const os = require('os')
const path = require('path')
const { encodeNavigation } = require('./navigation')
let navigationTimer = null
let navigationBusy = false
let navigationMetrics = null

const repoRoot = path.join(__dirname, '..', '..')
const MAX_SWF = 64 * 1024 * 1024
const MAX_COMMAND = 65535

let playerWindow = null
let server = null
let socketPath = ''
let chain = Promise.resolve()
let hostPageUrl = ''
let hostPageBody = null
let moviePattern = null

function log(message) {
  process.stderr.write(`[xenomit-flash] ${message}\n`)
}

function bridgeSocketPath(pid) {
  return `/tmp/xenomit_flash_${pid}.sock`
}

function exclusive(task) {
  const run = chain.then(task, task)
  chain = run.then(() => {}, () => {})
  return run
}

function withTimeout(promise, ms, message) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(message)), ms)
    promise.then(
      (value) => {
        clearTimeout(timer)
        resolve(value)
      },
      (error) => {
        clearTimeout(timer)
        reject(error)
      }
    )
  })
}

function sanitize(message) {
  return String(message || 'bridge failure').replace(/[\r\n\0]/g, ' ').slice(0, 500)
}

function attach(window) {
  playerWindow = window
  window.on('closed', () => {
    if (playerWindow === window) playerWindow = null
  })
}

async function flashCall(name, args) {
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) throw new Error('unknown Flash callback')
  const window = playerWindow
  if (!window || window.isDestroyed()) throw new Error('player shutting down')
  const code = `xenomitCall(${JSON.stringify(name)}, ${JSON.stringify(args)})`
  try {
    const value = await withTimeout(
      window.webContents.executeJavaScript(code, true),
      5000,
      'Flash call timed out; execution may have started; do not retry automatically'
    )
    if (value == null || value === '') throw new Error('Flash callback unavailable')
    return String(value)
  } catch (error) {
    const message = error && error.message ? error.message : String(error)
    if (message.includes('timed out')) throw error
    if (/xenomitCall|Script failed|callback unavailable|is not a function/i.test(message)) {
      throw new Error('Flash callback unavailable')
    }
    throw error
  }
}

async function okCall(name, args) {
  try {
    const value = await flashCall(name, args)
    return `OK ${value}\n`
  } catch (error) {
    return `ERR ${sanitize(error.message)}\n`
  }
}

function stampSnapshot(text) {
  let value
  try {
    value = JSON.parse(text)
  } catch (error) {
    return text
  }
  if (!value || value.status !== 'ok' || !value.data || typeof value.data !== 'object' || Array.isArray(value.data)) {
    return text
  }
  if (value.data.observedAtUnixMs == null) value.data.observedAtUnixMs = Date.now()
  return JSON.stringify(value)
}

function splitInvoke(body) {
  const first = body.indexOf('\t')
  const second = first < 0 ? -1 : body.indexOf('\t', first + 1)
  if (first < 0 || second < 0) return null
  return {
    target: body.slice(0, first),
    method: body.slice(first + 1, second),
    args: body.slice(second + 1)
  }
}

async function frameCall(target, method, args) {
  const id = String(Date.now()) + String(Math.floor(Math.random() * 1000))
  let queued
  try {
    queued = await flashCall('queueAS', [id, target, method, args])
  } catch (error) {
    return `ERR ${sanitize(error.message)}\n`
  }
  if (queued !== 'queued') return `OK ${queued}\n`
  const deadline = Date.now() + 4500
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 8))
    let result
    try {
      result = await flashCall('resultAS', [id])
    } catch (error) {
      return `ERR ${sanitize(error.message)}\n`
    }
    if (result !== 'pending') return `OK ${result}\n`
  }
  return 'ERR frame call timed out; execution unknown; do not retry automatically\n'
}

async function dispatch(command) {
  if (command === 'METRICS') return `OK ${JSON.stringify(navigationMetrics)}\n`;
  if (command === 'STATUS') {
    const up = playerWindow && !playerWindow.isDestroyed()
    return `OK player=1 bridge=${up ? 1 : 0} ready=0 native=1\n`
  }
  if (command === 'BRIDGE') return okCall('pingBridge', [])
  if (command === 'REFERENCE') return okCall('referenceAS', [])
  if (command === 'BACKEND native' || command === 'BACKEND cached') {
    const mode = command.slice(8)
    try {
      const confirmed = await flashCall('configureBackendAS', [mode])
      if (confirmed !== mode) return 'ERR Flash did not confirm the requested backend\n'
      return `OK ${mode}\n`
    } catch (error) {
      return `ERR ${sanitize(error.message)}\n`
    }
  }
  if (command === 'CAPTURE 0' || command === 'CAPTURE 1') return okCall('configureAS', [command.slice(8)])
  if (command === 'WINDOW 0' || command === 'WINDOW 1') {
    const window = playerWindow
    if (!window || window.isDestroyed()) return 'ERR player shutting down\n'
    if (command.endsWith('1')) window.show()
    else window.hide()
    return `OK visible=${command.endsWith('1') ? 1 : 0}\n`
  }
  if (command === 'STATE' || command === 'STATE\tstage') return okCall('captureAS', [])
  if (command.startsWith('STATE\t')) {
    try {
      const value = await flashCall('stateAS', [command.slice(6)])
      return `OK ${stampSnapshot(value)}\n`
    } catch (error) {
      return `ERR ${sanitize(error.message)}\n`
    }
  }
  if (command.startsWith('INSPECT\t')) return okCall('inspectAS', [command.slice(8)])
  const framed = command.startsWith('FINVOKE\t')
  if (framed || command.startsWith('INVOKE\t')) {
    const body = command.slice(framed ? 8 : 7)
    const parts = splitInvoke(body)
    if (!parts) return 'ERR expected target, method, JSON arguments\n'
    if (framed) return frameCall(parts.target, parts.method, parts.args)
    return okCall('invokeAS', [parts.target, parts.method, parts.args])
  }
  return 'ERR unknown command\n'
}

function handleConnection(socket) {
  socket.setTimeout(12000)
  let buffer = Buffer.alloc(0)
  let phase = 'command'
  const fail = (message) => {
    if (!socket.destroyed) socket.end(`ERR ${sanitize(message)}\n`)
  }
  socket.on('timeout', () => {
    if (phase === 'command') fail('Flash bridge timed out; action outcome may be unknown; no automatic retry')
    socket.destroy()
  })
  socket.on('error', () => {})
  socket.on('data', (chunk) => {
    if (phase === 'ack') {
      socket.destroy()
      return
    }
    if (phase !== 'command') return
    buffer = Buffer.concat([buffer, chunk])
    const newline = buffer.indexOf(0x0a)
    if (newline < 0) {
      if (buffer.length > MAX_COMMAND) {
        fail('command exceeds 65535 bytes')
        socket.destroy()
      }
      return
    }
    phase = 'busy'
    socket.setTimeout(0)
    const command = buffer.slice(0, newline).toString('utf8').replace(/\r$/, '')
    if (command.includes('\0')) {
      socket.end('ERR embedded NUL\n')
      return
    }
    exclusive(() => dispatch(command))
      .then((reply) => {
        if (socket.destroyed) return
        phase = 'ack'
        socket.setTimeout(2000)
        socket.end(reply.endsWith('\n') ? reply : `${reply}\n`)
      })
      .catch((error) => {
        if (socket.destroyed) return
        phase = 'ack'
        socket.setTimeout(2000)
        socket.end(`ERR ${sanitize(error.message)}\n`)
      })
  })
}

function start() {
  socketPath = bridgeSocketPath(process.pid)
  try {
    if (fs.existsSync(socketPath)) fs.unlinkSync(socketPath)
  } catch (error) {
    log(`could not replace a stale command socket: ${error.message}`)
  }
  server = require('net').createServer(handleConnection)
  server.on('error', (error) => log(`command bridge failed: ${error.message}`))
  server.listen(socketPath, () => {
    try {
      fs.chmodSync(socketPath, 0o600)
    } catch (error) {
      log(`could not restrict the command socket: ${error.message}`)
    }
    log(`command bridge listening on ${socketPath}`)
  })
  let stopped = false
  const stop = () => {
    if (stopped) return
    stopped = true
    if (server) server.close()
    if (navigationTimer) clearInterval(navigationTimer)
    try { fs.unlinkSync(process.env.XENOMIT_FLASH_STATE + '.navigation') } catch (error) {}
    try {
      if (socketPath) fs.unlinkSync(socketPath)
    } catch (error) {}
  }
  process.on('exit', stop)
  return stop
}

function download(url, headers) {
  return new Promise((resolve, reject) => {
    let settled = false
    const finish = (ok, value) => {
      if (settled) return
      settled = true
      if (ok) resolve(value)
      else reject(value)
    }
    const lib = url.startsWith('https:') ? https : http
    const request = lib.get(url, { headers }, (response) => {
      const status = response.statusCode || 0
      if (status >= 300 && status < 400 && response.headers.location) {
        response.resume()
        finish(true, { redirect: new URL(response.headers.location, url).href })
        return
      }
      if (status !== 200) {
        response.resume()
        finish(false, new Error(`movie download returned ${status}`))
        return
      }
      const chunks = []
      let size = 0
      response.on('data', (chunk) => {
        size += chunk.length
        if (size > MAX_SWF) {
          request.destroy()
          finish(false, new Error('movie download is too large'))
          return
        }
        chunks.push(chunk)
      })
      response.on('end', () => finish(true, { body: Buffer.concat(chunks) }))
    })
    request.setTimeout(20000, () => {
      request.destroy()
      finish(false, new Error('movie download timed out'))
    })
    request.on('error', (error) => finish(false, error))
  })
}

async function downloadMovie(url, cookie) {
  let current = url
  const headers = { 'User-Agent': 'BigpointClient/1.6.9' }
  if (cookie) headers.Cookie = cookie
  for (let hop = 0; hop < 5; hop++) {
    const result = await download(current, headers)
    if (result.body) return result.body
    current = result.redirect
  }
  throw new Error('movie download redirected too many times')
}

function runInjector(input, output) {
  const injector = path.join(repoRoot, 'build', 'xenomit_flash_inject')
  return new Promise((resolve, reject) => {
    if (!fs.existsSync(injector)) {
      reject(new Error('bridge injector is missing; run scripts/build.sh'))
      return
    }
    const child = spawn(injector, [input, output], { stdio: ['ignore', 'ignore', 'pipe'] })
    let stderr = ''
    child.stderr.on('data', (chunk) => {
      stderr += chunk.toString('utf8')
    })
    child.on('error', reject)
    child.on('close', (code) => {
      if (stderr.trim()) process.stderr.write(stderr.endsWith('\n') ? stderr : `${stderr}\n`)
      if (code === 0) resolve()
      else reject(new Error(stderr.trim() || `bridge injector exited ${code}`))
    })
  })
}

async function cookieHeader(session, urls) {
  const pairs = []
  const seen = new Set()
  for (const url of urls) {
    if (!url) continue
    const cookies = await session.cookies.get({ url })
    for (const cookie of cookies) {
      if (seen.has(cookie.name)) continue
      seen.add(cookie.name)
      pairs.push(`${cookie.name}=${cookie.value}`)
    }
  }
  return pairs.join('; ')
}

function pattern(url) {
  return url.replace(/[*?\\]/g, '\\$&')
}

function sameResource(left, right) {
  try {
    const a = new URL(left)
    const b = new URL(right)
    a.hash = ''
    b.hash = ''
    return a.href === b.href
  } catch (error) {
    return left === right
  }
}

async function installMoviePatch(window, movieUrl, pageUrl) {
  if (!/^https?:\/\//i.test(movieUrl)) {
    log('movie URL is not http(s); reflection bridge was not injected')
    return
  }
  const dbg = window.webContents.debugger
  try {
    dbg.attach('1.3')
  } catch (error) {
    log(`could not attach the movie debugger: ${error.message}`)
    return
  }
  let patchedBody = null
  let settled = false
  const queued = []
  const send = (method, params) => dbg.sendCommand(method, params)
  const fulfill = (requestId, body) => send('Fetch.fulfillRequest', {
    requestId,
    responseCode: 200,
    responseHeaders: [
      { name: 'Content-Type', value: 'application/x-shockwave-flash' },
      { name: 'Content-Length', value: String(body.length) }
    ],
    body: body.toString('base64')
  })
  dbg.on('message', async (_event, method, params) => {
    if (method !== 'Fetch.requestPaused') return
    const requestId = params.requestId
    // Pepper disables ExternalInterface when its host document is file://.
    // Fulfill this synthetic document locally under the authenticated origin.
    // No host HTML or session data is sent to the remote server.
    if (params.request && params.request.url === hostPageUrl && hostPageBody) {
      try {
        await send('Fetch.fulfillRequest', {
          requestId,
          responseCode: 200,
          responseHeaders: [{ name: 'Content-Type', value: 'text/html; charset=utf-8' }],
          body: hostPageBody.toString('base64')
        })
      } catch (error) {
        await send('Fetch.failRequest', { requestId, errorReason: 'Failed' }).catch(() => {})
        log(`host document could not be served: ${error.message}`)
      }
      return
    }
    const resume = () => send('Fetch.continueRequest', { requestId }).catch(() => {})
    const movie = params.request && sameResource(params.request.url, movieUrl) && params.request.method === 'GET'
    if (!movie) {
      resume()
      return
    }
    if (patchedBody) {
      fulfill(requestId, patchedBody).catch(() => resume())
      return
    }
    if (settled) {
      queued.push(requestId)
      return
    }
    settled = true
    const input = path.join(os.tmpdir(), `xenomit-movie-${process.pid}.swf`)
    const output = path.join(os.tmpdir(), `xenomit-bridge-${process.pid}.swf`)
    try {
      const cookie = await cookieHeader(window.webContents.session, [movieUrl, pageUrl])
      const body = await downloadMovie(movieUrl, cookie)
      fs.writeFileSync(input, body, { mode: 0o600 })
      await runInjector(input, output)
      patchedBody = fs.readFileSync(output)
      await fulfill(requestId, patchedBody)
      for (const id of queued.splice(0)) {
        fulfill(id, patchedBody).catch(() => {})
      }
      log(`reflection bridge served for ${movieUrl}`)
    } catch (error) {
      settled = false
      log(`reflection bridge was not injected: ${error.message}`)
      resume()
      for (const id of queued.splice(0)) {
        send('Fetch.continueRequest', { requestId: id }).catch(() => {})
      }
    } finally {
      fs.unlink(input, () => {})
      fs.unlink(output, () => {})
    }
  })
  try {
    moviePattern = { urlPattern: pattern(movieUrl) }
    await send('Fetch.enable', { patterns: [moviePattern] })
  } catch (error) {
    log(`could not intercept the movie: ${error.message}`)
    try {
      dbg.detach()
    } catch (detachError) {}
  }
}

async function loadHostPage(window, file, pageUrl) {
  const base = new URL(pageUrl)
  if (!/^https?:$/.test(base.protocol) || !moviePattern || !window.webContents.debugger.isAttached()) {
    throw new Error('Flash host requires an HTTP(S) page origin and movie interception')
  }
  hostPageUrl = new URL(`__xenomit_host_${process.pid}.html`, base).href
  hostPageBody = fs.readFileSync(file)
  await window.webContents.debugger.sendCommand('Fetch.enable', {
    patterns: [moviePattern, { urlPattern: pattern(hostPageUrl) }]
  })
  await window.loadURL(hostPageUrl)
}

function startNavigation() {
  const state = process.env.XENOMIT_FLASH_STATE;
  if (!state) throw new Error('native state path is missing');
  const file = state + '.navigation';
  navigationTimer = setInterval(() => {
    if (navigationBusy) return;
    navigationBusy = true;
    exclusive(async () => {
      try {
        const encoded = encodeNavigation(await flashCall('navigationAS', []));
        fs.writeFileSync(file + '.tmp', encoded.buffer, {mode:0o600});
        fs.renameSync(file + '.tmp', file);
        navigationMetrics = encoded.metrics;
      } catch (error) {
        // Never advance a failed capture's timestamp. Native frames gate stale geometry.
      }
    }).finally(() => { navigationBusy = false; });
  }, 150);
  navigationTimer.unref();
}
module.exports = { attach, installMoviePatch, loadHostPage, startNavigation, start }


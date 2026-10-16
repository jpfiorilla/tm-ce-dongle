#!/usr/bin/env node
// Minimal standalone TM Sync v1 server (same protocol as wii-memcard-manager's
// electron/main/wiiSyncServer.ts). Serves one memory card image to the dongle.
//
//   node tools/tmsync-server.mjs /path/to/GTME.raw
//
// HTTP  :8642  GET/HEAD /tmsync/v1/ping, /tmsync/v1/saves/<CODE>.raw (ETag = crc32-size, 304 on If-None-Match)
// UDP   :8643  "TMSYNC_DISCOVER" -> "TMSYNC_HERE 1 8642"
import http from 'node:http'
import dgram from 'node:dgram'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const file = process.argv[2]
if (!file) {
  console.error('usage: node tools/tmsync-server.mjs /path/to/GTME.raw')
  process.exit(1)
}
const HTTP_PORT = Number(process.env.TMSYNC_PORT || 8642)
const UDP_PORT = 8643

const table = new Uint32Array(256).map((_, n) => {
  let c = n
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
  return c >>> 0
})
const crc32 = (buf) => {
  let c = 0xffffffff
  for (const b of buf) c = table[(c ^ b) & 0xff] ^ (c >>> 8)
  return (c ^ 0xffffffff) >>> 0
}
const hex8 = (n) => n.toString(16).padStart(8, '0')

function load() {
  const buf = fs.readFileSync(file)
  const crc = crc32(buf)
  return { buf, crc: hex8(crc), etag: `${hex8(crc)}-${buf.length.toString(16)}` }
}

const server = http.createServer((req, res) => {
  const url = (req.url || '/').split('?')[0]
  const stamp = new Date().toLocaleTimeString()
  if (url === '/tmsync/v1/ping') return res.end('tmsync 1\n')
  const m = /^\/tmsync\/v1\/saves\/([A-Za-z0-9]{4})\.raw$/.exec(url)
  if (!m || m[1].toUpperCase() !== path.basename(file, '.raw').toUpperCase()) {
    res.writeHead(404, { Connection: 'close' })
    return res.end('not found\n')
  }
  let card
  try {
    card = load()
  } catch (e) {
    res.writeHead(404, { Connection: 'close' })
    return res.end(`can't read ${file}\n`)
  }
  const headers = { ETag: `"${card.etag}"`, 'X-TMSync-CRC32': card.crc, 'Cache-Control': 'no-cache', Connection: 'close' }
  const inm = (req.headers['if-none-match'] || '').split(',').map((s) => s.trim().replace(/^W\//, '').replace(/^"|"$/g, ''))
  if (inm.includes(card.etag) || inm.includes('*')) {
    res.writeHead(304, headers)
    console.log(`${stamp} ${req.socket.remoteAddress} 304 up to date (${card.etag})`)
    return res.end()
  }
  res.writeHead(200, { ...headers, 'Content-Type': 'application/octet-stream', 'Content-Length': card.buf.length })
  res.end(req.method === 'HEAD' ? undefined : card.buf)
  console.log(`${stamp} ${req.socket.remoteAddress} 200 sent ${card.buf.length} bytes (${card.etag})`)
})
server.listen(HTTP_PORT, '0.0.0.0')

const udp = dgram.createSocket({ type: 'udp4', reuseAddr: true })
udp.on('message', (msg, r) => {
  if (!msg.toString('latin1').startsWith('TMSYNC_DISCOVER')) return
  udp.send(Buffer.from(`TMSYNC_HERE 1 ${HTTP_PORT}\n`), r.port, r.address)
  console.log(`${new Date().toLocaleTimeString()} discovery from ${r.address}`)
})
udp.bind(UDP_PORT, '0.0.0.0')

const ips = Object.values(os.networkInterfaces()).flat().filter((a) => a && a.family === 'IPv4' && !a.internal).map((a) => a.address)
console.log(`TM Sync serving ${file}`)
console.log(`  http://${ips[0] || 'localhost'}:${HTTP_PORT}/tmsync/v1/saves/${path.basename(file)}  (discovery on udp ${UDP_PORT})`)

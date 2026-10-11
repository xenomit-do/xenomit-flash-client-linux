const MAX_BODY = 1024 * 1024;
const MAX_AGE = 750;
const finite = n => typeof n === 'number' && Number.isFinite(n) && Math.abs(n) <= 1000000;
const point = p => p && finite(p.x) && finite(p.y);
function encodeNavigation(reply, now = Date.now()) {
  const parsed = JSON.parse(reply);
  if (parsed.status !== 'ok') throw new Error('navigation capture failed');
  const data = parsed.data;
  if (!data || !Number.isFinite(data.observedAtUnixMs) ||
      data.observedAtUnixMs > now + 1000 || now - data.observedAtUnixMs > MAX_AGE) throw new Error('stale navigation');
  const n = data.navigation;
  if (!n || !Number.isInteger(n.map_id) || n.map_id <= 0 ||
      typeof n.geometry_ready !== 'boolean' || !Array.isArray(n.obstacles) || n.obstacles.length > 256 ||
      !Array.isArray(n.portals) || n.portals.length > 8192) throw new Error('invalid navigation');
  for (const obstacle of n.obstacles) {
    const s = obstacle && obstacle.shape;
    if (!Number.isInteger(obstacle.id) || typeof obstacle.name !== 'string' || typeof obstacle.inverted !== 'boolean' || !s) throw new Error('invalid obstacle');
    let valid = false;
    if (s.type === 'circle') valid = point(s.center) && finite(s.radius) && s.radius > 0;
    if (s.type === 'rectangle') valid = s.bounds && finite(s.bounds.x) && finite(s.bounds.y) && finite(s.bounds.width) && finite(s.bounds.height) && s.bounds.width > 0 && s.bounds.height > 0;
    if (s.type === 'polygon') valid = Array.isArray(s.points) && s.points.length >= 3 && s.points.length <= 1024 && s.points.every(point);
    if (!valid) throw new Error('invalid obstacle shape');
  }
  for (const p of n.portals) {
    if (!p || !Number.isInteger(p.id) || !point(p.position) ||
        !(p.target_map == null || Number.isInteger(p.target_map)) || typeof p.jumping !== 'boolean') throw new Error('invalid portal');
  }
  // These separately cached Windows fields are null in navigationSnapshot.
  // Omit them so Rust's non-optional collections use their defaults.
  if (n.star_system == null) delete n.star_system;
  if (n.hero && n.hero.items == null) delete n.hero.items;
  const body = Buffer.from(JSON.stringify(n));
  if (body.length > MAX_BODY) throw new Error('navigation exceeds size limit');
  const header = Buffer.alloc(24);
  header.writeUInt32LE(0x56414e58, 0); // XNAV v1
  header.writeUInt32LE(1, 4);
  header.writeBigUInt64LE(BigInt(Math.trunc(data.observedAtUnixMs)), 8);
  header.writeInt32LE(n.map_id, 16);
  header.writeUInt32LE(body.length, 20);
  return { buffer: Buffer.concat([header, body]), metrics: {
    observedAtUnixMs: data.observedAtUnixMs, frames: data.frames, fps: data.fps,
    navigationMs: data.navigationMs, mapId: n.map_id, geometryReady: n.geometry_ready,
    obstacles: n.obstacles.length
  } };
}
module.exports = { encodeNavigation };

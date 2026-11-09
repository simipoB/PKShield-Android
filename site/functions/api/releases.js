// GET /api/releases: the published versions (newest first) as publish_cf.py records them: version, file, size, sha256, date, notes.
// The APKs themselves only come from POST /download.
import { releases } from '../../lib/releases.js';

export async function onRequestGet({ env }) {
  const list = (await releases(env)).map(({ version, file, size, sha256, date, notes }) => ({ version, file, size, sha256, date, notes }));
  return new Response(JSON.stringify(list), { headers: { 'Content-Type': 'application/json', 'Cache-Control': 'max-age=60' } });
}

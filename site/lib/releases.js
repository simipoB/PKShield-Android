// The published versions, newest first: releases.json in the R2 bucket, written by dm/android/tools/publish_cf.py.
// Each: { version, file, size, sha256, date, notes }
export async function releases(env) {
  const o = await env.APKS.get('releases.json');
  if (!o) return [];
  try { const l = JSON.parse(await o.text()); return Array.isArray(l) ? l : []; } catch { return []; }
}

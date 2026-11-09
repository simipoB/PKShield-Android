// The official download site as a Cloudflare Worker (wrangler.toml): the page and its script are static files (public/, served by the
// assets binding); this Worker answers the three addresses that are not files:
//   POST /download     one of the published APKs from R2, only with a Turnstile pass made on this site for the "download" action. Each pass
//                      works once and expires after 5 minutes, so a link downloads nothing and a scraper gets nothing without solving the
//                      check. Only files listed in releases.json (publish_cf.py) can be asked for. A GET goes back to the page
//   GET /api/releases  the published versions: version, file, size, sha256, date, notes
//   GET /api/config    the Turnstile site key (public)
import { releases } from '../lib/releases.js';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/download') return request.method === 'POST' ? download(request, env) : Response.redirect(new URL('/', url).toString(), 302);
    if (url.pathname === '/api/releases') {
      const list = (await releases(env)).map(({ version, file, size, sha256, date, notes }) => ({ version, file, size, sha256, date, notes }));
      return json(list, 60);
    }
    if (url.pathname === '/api/config') {
      const k = env.TURNSTILE_SITEKEY || '';
      return json({ sitekey: k.startsWith('REPLACE') ? '' : k }, 300);
    }
    return env.ASSETS.fetch(request);
  },
};

async function download(request, env) {
  let form;
  try { form = await request.formData(); } catch { return refuse('The human check is missing. Go back to the download page and try again.'); }   // (no form at all)
  const token = String(form.get('cf-turnstile-response') || '');
  if (!token || !env.TURNSTILE_SECRET) return refuse('The human check is missing. Go back to the download page and try again.');

  const check = new FormData();
  check.append('secret', env.TURNSTILE_SECRET);
  check.append('response', token);
  const ip = request.headers.get('CF-Connecting-IP');
  if (ip) check.append('remoteip', ip);
  const v = await (await fetch('https://challenges.cloudflare.com/turnstile/v0/siteverify', { method: 'POST', body: check })).json();
  const host = new URL(request.url).hostname;
  // (Cloudflare's testing keys answer hostname example.com and no action: accepted only in a local test run, wrangler dev --var TURNSTILE_TEST:1)
  const testing = env.TURNSTILE_TEST === '1' && v.metadata && v.metadata.result_with_testing_key;
  if (!v.success || (!testing && (v.action !== 'download' || (v.hostname && v.hostname !== host))))
    return refuse('The human check did not pass. Go back to the download page and try again.');

  const list = await releases(env);
  const want = String(form.get('file') || '');
  const rel = want ? list.find((r) => r.file === want) : list[0];
  if (!rel) return refuse('That version is not available.', 404);
  const obj = await env.APKS.get(rel.file);
  if (!obj) return refuse('That version is not available.', 404);
  return new Response(obj.body, {
    headers: {
      'Content-Type': 'application/vnd.android.package-archive',
      'Content-Disposition': `attachment; filename="${rel.file}"`,
      'Content-Length': String(obj.size),
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff',
    },
  });
}

function json(v, maxAge) {
  return new Response(JSON.stringify(v), { headers: { 'Content-Type': 'application/json', 'Cache-Control': `max-age=${maxAge}` } });
}
function refuse(msg, status = 403) {
  return new Response(msg, { status, headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' } });
}

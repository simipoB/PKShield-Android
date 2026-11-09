// POST /download (the page's form: cf-turnstile-response, file): one of the published APKs from R2, only with a Turnstile pass
// made on this site for the "download" action. Each pass works once and expires after 5 minutes, so a link to this
// address downloads nothing and a scraper gets nothing without solving the check. Only files listed in releases.json
// (publish_cf.py) can be asked for.
import { releases } from '../lib/releases.js';

export async function onRequestPost({ request, env }) {
  const form = await request.formData();
  const token = String(form.get('cf-turnstile-response') || '');
  if (!token || !env.TURNSTILE_SECRET) return refuse('The human check is missing. Go back to the download page and try again.');

  const check = new FormData();
  check.append('secret', env.TURNSTILE_SECRET);
  check.append('response', token);
  const ip = request.headers.get('CF-Connecting-IP');
  if (ip) check.append('remoteip', ip);
  const v = await (await fetch('https://challenges.cloudflare.com/turnstile/v0/siteverify', { method: 'POST', body: check })).json();
  const host = new URL(request.url).hostname;
  // (Cloudflare's testing keys answer hostname example.com and no action: accepted only in a local test run, wrangler pages dev --binding TURNSTILE_TEST=1)
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

// a GET (a shared link, a crawler): back to the page
export function onRequestGet({ request }) {
  return Response.redirect(new URL('/', request.url).toString(), 302);
}

function refuse(msg, status = 403) {
  return new Response(msg, { status, headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' } });
}

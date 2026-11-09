// GET /api/config: the Turnstile site key (public), from wrangler.toml's TURNSTILE_SITEKEY.
export function onRequestGet({ env }) {
  const sitekey = env.TURNSTILE_SITEKEY && !env.TURNSTILE_SITEKEY.startsWith('REPLACE') ? env.TURNSTILE_SITEKEY : '';
  return new Response(JSON.stringify({ sitekey }), { headers: { 'Content-Type': 'application/json', 'Cache-Control': 'max-age=300' } });
}

# site: the official download page

A small Cloudflare Worker that hands out the APK. GitHub releases keep the release notes, but the APK is not attached there any more. Instead it sits in a private R2 bucket. The site only gives it out after a Turnstile check (Cloudflare's "verify you are human" box), so scrapers that copy releases automatically get nothing. Someone can still download it by hand. The APK's About page and its license notice cover that case.

| File | What it does |
|---|---|
| `public/index.html` | The page (a static file): latest version, size, SHA-256, older versions, the Turnstile check and the Download button |
| `src/worker.js` | The addresses that are not files. `POST /download` checks the Turnstile pass (once, for this site, for the `download` action), then streams the APK from R2; only files listed in `releases.json` can be asked for. `GET /api/releases` is the version list, `GET /api/config` the site key |
| `lib/releases.js` | Reads `releases.json` from the bucket |
| `wrangler.toml` | The Worker: its name, the static files, the R2 binding `APKS` and the Turnstile site key |
| `../dm/android/tools/publish_cf.py` | Uploads a release-signed APK to R2 and puts it first in `releases.json` |

## Setting it up (once)

1. **Log in:** `npx wrangler login` (opens the browser).
2. **R2:** in the Cloudflare dashboard, open R2 and enable it. It needs a payment method on file, but 10 GB of storage and the downloads are free. Then run `npx wrangler r2 bucket create galar-native-apks`.
3. **Turnstile:** `npx wrangler turnstile widget create "Galar Native downloads" --domain <the Worker's host> --mode managed`.
   - Put the **site key** in `wrangler.toml` (`TURNSTILE_SITEKEY`).
   - The **secret key** goes in after the first deploy: `npx wrangler secret put TURNSTILE_SECRET` (run in `site/`).
4. **The Worker, connected to GitHub:** dashboard → Workers & Pages → Create → Import a repository → this repository.
   - Project name: `galar-native-android`, the `name` in `wrangler.toml`.
   - Build command: none. Deploy command: `npx wrangler deploy`.
   - Advanced settings → Path (root directory): `site`.
   - Every push to `main` redeploys it.
5. **Publish the current release:**
   `python dm/android/tools/publish_cf.py <apk> --version 0.57 --apksigner <build-tools>/apksigner`
   The script refuses an APK that isn't signed with the release key.

## Each release

Build the APK, then run `publish_cf.py` with the new version. Create the GitHub release with its notes but **without the APK**, and link the download page in the notes.

## Testing locally

```
cd site
npx wrangler r2 object put galar-native-apks/releases.json --file <list.json> --local
npx wrangler r2 object put galar-native-apks/GalarNative-v0.57.apk --file <apk> --local
npx wrangler dev --var TURNSTILE_SECRET:1x0000000000000000000000000000000AA --var TURNSTILE_SITEKEY:1x00000000000000000000AA --var TURNSTILE_TEST:1
```

Those are Cloudflare's always-pass test keys. `TURNSTILE_TEST=1` accepts their dummy answers, and it is never set on the live site.

## More protection with your own domain

With a domain on Cloudflare (Worker → Settings → Domains & Routes → Add a custom domain) you also get:
- Bot Fight Mode;
- rate limiting rules (for example, a few `POST /download` per minute per IP);
- WAF rules that block a scraper's user agent or network once it shows up in Security → Analytics.

Add the domain to the Turnstile widget's hostnames as well.

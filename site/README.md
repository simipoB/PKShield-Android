# site: the official download page

A small Cloudflare Pages site that hands out the APK. GitHub releases keep the release notes, but the APK is not attached there any more. Instead it sits in a private R2 bucket. The site only gives it out after a Turnstile check (Cloudflare's "verify you are human" box), so scrapers that copy releases automatically get nothing. Someone can still download it by hand. The APK's About page and its license notice cover that case.

| File | What it does |
|---|---|
| `public/index.html` | The page: latest version, size, SHA-256, older versions, the Turnstile check and the Download button |
| `functions/download.js` | `POST /download`: checks the Turnstile pass (once, for this site, for the `download` action), then streams the APK from R2. Only files listed in `releases.json` can be asked for |
| `functions/api/releases.js` | `GET /api/releases`: the version list |
| `functions/api/config.js` | `GET /api/config`: the Turnstile site key |
| `lib/releases.js` | Reads `releases.json` from the bucket |
| `wrangler.toml` | The Pages project: the R2 binding `APKS` and the Turnstile site key |
| `../dm/android/tools/publish_cf.py` | Uploads a release-signed APK to R2 and puts it first in `releases.json` |

## Setting it up (once)

1. **Log in:** `npx wrangler login` (opens the browser).
2. **R2:** in the Cloudflare dashboard, open R2 and enable it. It needs a payment method on file, but 10 GB of storage and the downloads are free. Then run `npx wrangler r2 bucket create galar-native-apks`.
3. **Turnstile:** dashboard → Turnstile → Add widget. Mode: Managed. Hostnames: `galar-native.pages.dev` (and your own domain if you add one).
   - Put the **site key** in `wrangler.toml` (`TURNSTILE_SITEKEY`).
   - The **secret key** goes in after the project exists (step 4): `npx wrangler pages secret put TURNSTILE_SECRET --project-name galar-native`.
4. **Pages, connected to GitHub:** dashboard → Workers & Pages → Create → Pages → Connect to Git → this repository.
   - Project name: `galar-native` (if it's taken, use another and change `name` in `wrangler.toml`).
   - Production branch: `main`. Root directory: `site`. Build command: none. Build output directory: `public`.
   - Every push to `main` that changes `site/` redeploys the page.
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
npx wrangler pages dev public --binding TURNSTILE_SECRET=1x0000000000000000000000000000000AA --binding TURNSTILE_SITEKEY=1x00000000000000000000AA --binding TURNSTILE_TEST=1
```

Those are Cloudflare's always-pass test keys. `TURNSTILE_TEST=1` accepts their dummy answers, and it is never set on the live site.

## More protection with your own domain

With a domain on Cloudflare (add it under the Pages project → Custom domains) you also get:
- Bot Fight Mode;
- rate limiting rules (for example, a few `POST /download` per minute per IP);
- WAF rules that block a scraper's user agent or network once it shows up in Security → Analytics.

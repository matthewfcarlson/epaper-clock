# epaper-clock OTA-failure relay

A small Cloudflare Worker that lets the clock auto-file a GitHub issue when
an OTA update fails, without the device ever holding a GitHub-scoped
credential. See "OTA failure reporting" in the repo root's `CLAUDE.md` for
the full picture and why this exists instead of a device-held PAT.

The Worker authenticates to GitHub as a **GitHub App installation** (short-
lived, auto-expiring tokens, issues filed under a real bot identity like
`your-app-name[bot]`) rather than a personal account or a long-lived PAT.
Setting that up is the fiddliest part — everything else is `wrangler deploy`.

## 1. Register the GitHub App

1. GitHub → your avatar → **Settings** → **Developer settings** →
   **GitHub Apps** → **New GitHub App**.
2. Fill in:
   - **GitHub App name**: anything unique, e.g. `epaper-clock-ota-bot`
     (this becomes the `[bot]` name issues are filed under).
   - **Homepage URL**: your repo's URL — required by the form, not
     otherwise used here.
   - **Webhook**: untick **Active**. This Worker doesn't receive webhooks.
   - **Repository permissions** → **Issues**: **Read and write**. Leave
     every other permission at **No access**.
   - **Where can this GitHub App be installed?**: "Only on this account"
     unless you specifically want it installable elsewhere.
3. **Create GitHub App**.
4. On the App's settings page, note the **App ID** near the top.
5. Scroll to **Private keys** → **Generate a private key**. This downloads
   a `.pem` file — GitHub generates it in **PKCS#1** format
   (`-----BEGIN RSA PRIVATE KEY-----`), which the Worker's Web Crypto call
   can't import directly. Convert it to PKCS#8 once, locally:
   ```bash
   openssl pkcs8 -topk8 -inform PEM -outform PEM -nocrypt \
     -in ~/Downloads/your-app-name.*.private-key.pem \
     -out pkcs8-key.pem
   ```
   You'll paste the *contents* of `pkcs8-key.pem` into a Wrangler secret
   below, then can delete both files locally.
6. Install the App: from the App's public page (**Install App** in the left
   sidebar of its settings), click **Install**, choose **Only select
   repositories**, and pick this repo. After installing, the browser URL
   looks like `.../settings/installations/12345678` — that trailing number
   is your **Installation ID**.

## 2. Create the KV namespace

From this directory:

```bash
npm install
npx wrangler kv namespace create OTA_RELAY_KV
```

Copy the `id` it prints into `wrangler.toml`'s `[[kv_namespaces]]` block
(replacing `REPLACE_WITH_KV_NAMESPACE_ID`).

## 3. Set `[vars]`

Edit `wrangler.toml`'s `[vars]` block — `GITHUB_OWNER`/`GITHUB_REPO` are the
repo the Worker files issues against (not secret, just config).

## 4. Set secrets

```bash
npx wrangler secret put GITHUB_APP_ID
# paste the App ID from step 1.4

npx wrangler secret put GITHUB_APP_INSTALLATION_ID
# paste the Installation ID from step 1.6

npx wrangler secret put GITHUB_APP_PRIVATE_KEY
# paste the full contents of pkcs8-key.pem from step 1.5, including the
# -----BEGIN/END PRIVATE KEY----- lines

npx wrangler secret put REPORT_SHARED_SECRET
# a random token the device and Worker both know — generate one with:
#   openssl rand -hex 32
# this is NOT a GitHub credential; it only ever authorizes a POST to this
# Worker's one rate-limited endpoint, so a fairly low-stakes secret to hold
# in device NVS compared to what it replaces
```

## 5. Deploy

```bash
npx wrangler deploy
```

Note the `https://epaper-clock-ota-relay.<your-subdomain>.workers.dev` URL
it prints.

## 6. Wire it into the device

- `src/config.h` (local dev) or the `OTA_REPORT_ENDPOINT` repo secret
  (release builds, see `.github/workflows/release-firmware.yml`): set to
  `https://.../report` (the deployed URL above, with `/report` appended).
- Provision the same value you set for `REPORT_SHARED_SECRET` onto the
  device via `docs/provision.html`'s "GitHub auto-report" field (`CFG
  SET_REPORT_TOKEN` under the hood).

## Optional: pre-create the `ota-failure` label

The Worker tags filed issues with an `ota-failure` label and tolerates the
label not existing (falls back to filing without it — see `createIssue()`
in `src/index.js`), but creating it once gives you a proper color/description:

```bash
gh label create ota-failure --color B60205 \
  --description "Filed automatically by the OTA-failure relay Worker"
```

## Local testing

```bash
npx wrangler dev
curl -X POST http://localhost:8787/report \
  -H "X-Report-Token: <your REPORT_SHARED_SECRET>" \
  -H "Content-Type: application/json" \
  -d '{"reason":"download_flash","attempted":"0.9.13","detail":"download_http_-1_attempt1","fw":"0.9.12","at":1737480000,"log":"Checking GitHub for newer firmware...\nNew firmware available: 0.9.13\nFirmware download HTTP error: -1"}'
```

A second identical request within the same failure "episode" should land as
a comment on the first request's issue rather than a new one.

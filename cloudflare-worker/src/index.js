// Cloudflare Worker relay for epaper-clock's OTA-failure auto-reporting.
//
// The device (reportOtaFailure() in src/main.cpp) POSTs a failure report
// here instead of holding a GitHub-scoped credential itself. This Worker:
//   1. Checks a shared secret (X-Report-Token header) and rate-limits.
//   2. Authenticates to GitHub as a GitHub App installation (short-lived
//      tokens, never a long-lived PAT sitting anywhere).
//   3. Dedupes: adds a comment to an existing open issue for the same
//      failure signature (reason+version) if one exists, otherwise files a
//      new one.
//
// See README.md in this directory for setup (GitHub App registration, KV
// namespace, wrangler secrets) and CLAUDE.md's "OTA failure reporting"
// section in the repo root for the full picture, including why this exists
// instead of a device-held PAT.

const REPORT_PATH = "/report";
const SIGNATURE_PREFIX = "ota-failure-signature:";
const ISSUE_LABEL = "ota-failure";

// Per-source and overall caps, independent of each other — a compromised or
// misbehaving single device can't exhaust the whole budget, and the whole
// budget bounds total GitHub API usage regardless of how many devices exist.
const RATE_LIMIT_PER_IP = { limit: 10, windowSeconds: 3600 };
const RATE_LIMIT_GLOBAL = { limit: 50, windowSeconds: 3600 };

// How much of the device's log tail to keep in the issue body — GitHub
// issue bodies have a generous limit, but there's no reason to keep more
// than this much context.
const LOG_TAIL_CHARS = 4000;

export default {
  async fetch(request, env) {
    if (request.method !== "POST") {
      return new Response("Method Not Allowed", { status: 405 });
    }
    const url = new URL(request.url);
    if (url.pathname !== REPORT_PATH) {
      return new Response("Not Found", { status: 404 });
    }

    const providedToken = request.headers.get("X-Report-Token") || "";
    if (!(await timingSafeEqual(providedToken, env.REPORT_SHARED_SECRET))) {
      return new Response("Unauthorized", { status: 401 });
    }

    const ip = request.headers.get("CF-Connecting-IP") || "unknown";
    const [ipOk, globalOk] = await Promise.all([
      checkRateLimit(env, `ip:${ip}`, RATE_LIMIT_PER_IP),
      checkRateLimit(env, "global", RATE_LIMIT_GLOBAL),
    ]);
    if (!ipOk || !globalOk) {
      return new Response("Rate limited", { status: 429 });
    }

    let payload;
    try {
      payload = await request.json();
    } catch (e) {
      return new Response("Bad Request: invalid JSON", { status: 400 });
    }

    const { reason, attempted } = payload;
    if (!reason || !attempted) {
      return new Response("Bad Request: reason and attempted are required", { status: 400 });
    }

    try {
      const token = await getInstallationToken(env);
      const result = await fileOrDedupeIssue(env, token, payload);
      return new Response(JSON.stringify(result), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    } catch (e) {
      // Logged server-side only (visible via `wrangler tail`) — the device
      // just sees a non-200 and retries on its next wake.
      console.error("report relay failed:", e);
      return new Response("Internal Error", { status: 500 });
    }
  },
};

// Constant-time string comparison — a naive `===` leaks the shared secret's
// prefix length via response timing to anyone who can hit this endpoint
// repeatedly. Both inputs are hashed first so the comparison itself always
// operates on fixed-length digests regardless of the (attacker-controlled)
// input length.
async function timingSafeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length === 0 || b.length === 0) {
    return false;
  }
  const enc = new TextEncoder();
  const [digestA, digestB] = await Promise.all([
    crypto.subtle.digest("SHA-256", enc.encode(a)),
    crypto.subtle.digest("SHA-256", enc.encode(b)),
  ]);
  const bytesA = new Uint8Array(digestA);
  const bytesB = new Uint8Array(digestB);
  let diff = 0;
  for (let i = 0; i < bytesA.length; i++) diff |= bytesA[i] ^ bytesB[i];
  return diff === 0;
}

// Fixed-window counter in KV. Coarser than a sliding window or token
// bucket, but simple, and the windows here (hourly) are long enough that
// the boundary-doubling edge case (a burst just before and just after a
// window flip) isn't worth the extra complexity for this volume of traffic.
async function checkRateLimit(env, key, { limit, windowSeconds }) {
  const windowStart = Math.floor(Date.now() / 1000 / windowSeconds) * windowSeconds;
  const kvKey = `ratelimit:${key}:${windowStart}`;
  const current = parseInt((await env.OTA_RELAY_KV.get(kvKey)) || "0", 10);
  if (current >= limit) return false;
  // expirationTtl has a 60s minimum in Workers KV; windowSeconds here is
  // always well above that.
  await env.OTA_RELAY_KV.put(kvKey, String(current + 1), { expirationTtl: windowSeconds + 60 });
  return true;
}

// --- GitHub App authentication ---

function base64url(bytes) {
  let str = "";
  for (const b of bytes) str += String.fromCharCode(b);
  return btoa(str).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function pemToArrayBuffer(pem) {
  const b64 = pem
    .replace(/-----BEGIN [^-]+-----/, "")
    .replace(/-----END [^-]+-----/, "")
    .replace(/\s+/g, "");
  const binary = atob(b64);
  const buf = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) buf[i] = binary.charCodeAt(i);
  return buf.buffer;
}

// Signs a short-lived (10 min, the max GitHub allows) JWT identifying this
// GitHub App, per https://docs.github.com/en/apps/creating-github-apps/authenticating-with-a-github-app/generating-a-json-web-token-jwt-for-a-github-app
// GITHUB_APP_PRIVATE_KEY must be PKCS#8 PEM — GitHub's own "Generate a
// private key" button on the App's settings page produces PKCS#1
// ("-----BEGIN RSA PRIVATE KEY-----"), which Web Crypto's importKey()
// can't read directly; see README.md for the one-line openssl conversion.
async function createAppJwt(appId, privateKeyPkcs8Pem) {
  const key = await crypto.subtle.importKey(
    "pkcs8",
    pemToArrayBuffer(privateKeyPkcs8Pem),
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
    false,
    ["sign"]
  );

  const now = Math.floor(Date.now() / 1000);
  const header = { alg: "RS256", typ: "JWT" };
  // iat backdated 60s to tolerate clock drift between this Worker and
  // GitHub's servers, per GitHub's own guidance; exp capped at 10 minutes
  // (GitHub's max) — this JWT only ever authenticates the single
  // installation-token exchange below, never a later API call.
  const payload = { iat: now - 60, exp: now + 600, iss: appId };

  const enc = new TextEncoder();
  const encodedHeader = base64url(enc.encode(JSON.stringify(header)));
  const encodedPayload = base64url(enc.encode(JSON.stringify(payload)));
  const signingInput = `${encodedHeader}.${encodedPayload}`;
  const signature = await crypto.subtle.sign(
    { name: "RSASSA-PKCS1-v1_5" },
    key,
    enc.encode(signingInput)
  );
  return `${signingInput}.${base64url(new Uint8Array(signature))}`;
}

// Installation tokens last ~1 hour; cached in KV so a burst of reports (or
// just repeated wakes retrying a still-unreported failure) doesn't re-sign
// a JWT and hit GitHub's token-exchange endpoint every single time.
async function getInstallationToken(env) {
  const cached = await env.OTA_RELAY_KV.get("gh_installation_token", { type: "json" });
  if (cached && cached.expires_at && Date.parse(cached.expires_at) - Date.now() > 5 * 60 * 1000) {
    return cached.token;
  }

  const jwt = await createAppJwt(env.GITHUB_APP_ID, env.GITHUB_APP_PRIVATE_KEY);
  const resp = await fetch(
    `https://api.github.com/app/installations/${env.GITHUB_APP_INSTALLATION_ID}/access_tokens`,
    {
      method: "POST",
      headers: {
        Authorization: `Bearer ${jwt}`,
        Accept: "application/vnd.github+json",
        "User-Agent": "epaper-clock-ota-relay",
      },
    }
  );
  if (!resp.ok) {
    throw new Error(`installation token exchange failed: ${resp.status} ${await resp.text()}`);
  }
  const data = await resp.json();
  // A bit under the real ~1hr lifetime so a token never gets used right at
  // its expiry boundary.
  await env.OTA_RELAY_KV.put("gh_installation_token", JSON.stringify(data), {
    expirationTtl: 50 * 60,
  });
  return data.token;
}

// --- Issue filing / dedup ---

function buildSignature(reason, attempted) {
  return `${reason}:${attempted}`;
}

function issueDetailsMarkdown(payload, when) {
  const logTail = payload.log ? String(payload.log).slice(-LOG_TAIL_CHARS) : "";
  return (
    `**Reason:** ${payload.reason}\n` +
    `**Attempted version:** ${payload.attempted}\n` +
    `**Running firmware:** ${payload.fw || "?"}\n` +
    `**Detail:** ${payload.detail || "n/a"}\n` +
    `**Recorded at:** ${when}` +
    (logTail ? `\n\n**Recent OTA log:**\n\`\`\`\n${logTail}\n\`\`\`` : "")
  );
}

// Creates an issue with the `ota-failure` label, tolerating a repo where
// that label doesn't exist yet — GitHub's issue-creation endpoint has been
// inconsistent about auto-vivifying labels across API versions, so this
// retries without the label rather than let a labeling nicety block the
// actual report. (README.md suggests creating the label once for a nicer
// color, but nothing here depends on that having been done.)
async function createIssue(env, headers, title, body) {
  const post = (withLabel) =>
    fetch(`https://api.github.com/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/issues`, {
      method: "POST",
      headers,
      body: JSON.stringify(withLabel ? { title, body, labels: [ISSUE_LABEL] } : { title, body }),
    });

  let resp = await post(true);
  if (resp.status === 422) {
    resp = await post(false);
  }
  if (!resp.ok) {
    throw new Error(`create issue failed: ${resp.status} ${await resp.text()}`);
  }
  return resp.json();
}

async function fileOrDedupeIssue(env, token, payload) {
  const headers = {
    Authorization: `Bearer ${token}`,
    Accept: "application/vnd.github+json",
    "User-Agent": "epaper-clock-ota-relay",
    "Content-Type": "application/json",
  };

  const signature = buildSignature(payload.reason, payload.attempted);
  const when = payload.at ? new Date(payload.at * 1000).toISOString() : new Date().toISOString();
  const details = issueDetailsMarkdown(payload, when);

  // Search is eventually consistent (GitHub's own caveat), so a report
  // filed moments ago might not be found yet — worst case that means an
  // extra issue instead of a comment, not a lost report either way.
  const q = `repo:${env.GITHUB_OWNER}/${env.GITHUB_REPO} is:issue is:open in:body "${SIGNATURE_PREFIX}${signature}"`;
  const searchResp = await fetch(`https://api.github.com/search/issues?q=${encodeURIComponent(q)}`, {
    headers,
  });
  const searchData = searchResp.ok ? await searchResp.json() : { items: [] };

  if (searchData.items && searchData.items.length > 0) {
    const issue = searchData.items[0];
    const commentResp = await fetch(
      `https://api.github.com/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/issues/${issue.number}/comments`,
      { method: "POST", headers, body: JSON.stringify({ body: `Reported again:\n\n${details}` }) }
    );
    if (!commentResp.ok) {
      throw new Error(`comment failed: ${commentResp.status} ${await commentResp.text()}`);
    }
    return { ok: true, action: "commented", issue_number: issue.number };
  }

  const title = `OTA update failed: ${payload.reason} (attempted ${payload.attempted})`;
  const body =
    `${details}\n\n` +
    `<!-- ${SIGNATURE_PREFIX}${signature} -->\n` +
    `_Filed automatically via the OTA-failure relay Worker (cloudflare-worker/)._`;
  const created = await createIssue(env, headers, title, body);
  return { ok: true, action: "created", issue_number: created.number };
}

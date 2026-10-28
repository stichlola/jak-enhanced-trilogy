// Jak Enhanced Trilogy: problem reports. "Report a problem.bat" (or the Enhanced menu's button) posts
// the player's report ZIP (logs, graphics card, settings) and what they wrote; it is kept in a
// private KV namespace. Only the developer reads them, with the ADMIN_KEY secret:
//   GET  /reports            the list (newest first)
//   GET  /reports/<key>      one ZIP
//   GET  /reports/<key>/note what the player wrote
// Nothing is public: a report holds Windows paths (the user name) and PC details.

const MAX_ZIP = 8 * 1024 * 1024; // a report is usually well under 1 MB
const MAX_NOTE = 4000;
const PER_HOUR = 5; // reports a player's address may send an hour
const KEEP_DAYS = 90; // then KV forgets them by itself

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "POST" && url.pathname === "/report") {
      return report(request, env);
    }
    if (request.method === "GET" && url.pathname.startsWith("/reports")) {
      return admin(request, env, url);
    }
    return new Response("Jak Enhanced Trilogy problem reports\n", { status: 404 });
  },
};

async function report(request, env) {
  const ip = request.headers.get("CF-Connecting-IP") || "unknown";
  if (!(await allow(env, ip))) {
    return text("Too many reports from here: try again in an hour.", 429);
  }
  let form;
  try {
    form = await request.formData();
  } catch {
    return text("Expected a form with the report ZIP.", 400);
  }
  const zip = form.get("report");
  if (!zip || typeof zip === "string") {
    return text("The report ZIP is missing.", 400);
  }
  if (zip.size > MAX_ZIP) {
    return text("The report is too big.", 413);
  }
  const bytes = new Uint8Array(await zip.arrayBuffer());
  // a ZIP starts with "PK"
  if (bytes.length < 4 || bytes[0] !== 0x50 || bytes[1] !== 0x4b) {
    return text("That is not a ZIP.", 400);
  }
  const note = String(form.get("note") || "").slice(0, MAX_NOTE);
  const version = String(form.get("version") || "").slice(0, 64);
  const id = new Date().toISOString().replace(/[:.]/g, "-") + "-" + crypto.randomUUID().slice(0, 8);
  const meta = { version, game: String(form.get("game") || "").slice(0, 16), country: request.cf?.country || "" };
  const keep = { expirationTtl: KEEP_DAYS * 86400 };
  await env.REPORTS.put(`zip/${id}`, bytes, { ...keep, metadata: { ...meta, size: bytes.length, note: note.slice(0, 200) } });
  await env.REPORTS.put(`note/${id}`, note, keep);
  return text(`Thanks! Report ${id} received.`, 200);
}

// a few reports an hour from one address, counted in KV too (the count expires with the hour)
async function allow(env, ip) {
  const hour = new Date().toISOString().slice(0, 13);
  const key = `_limits/${hour}/${await sha(ip)}`;
  const count = parseInt((await env.REPORTS.get(key)) || "0", 10) || 0;
  if (count >= PER_HOUR) {
    return false;
  }
  await env.REPORTS.put(key, String(count + 1), { expirationTtl: 7200 });
  return true;
}

async function admin(request, env, url) {
  const key = request.headers.get("Authorization") || "";
  if (!env.ADMIN_KEY || key !== `Bearer ${env.ADMIN_KEY}`) {
    return text("Not allowed.", 401);
  }
  const rest = url.pathname.slice("/reports".length).replace(/^\//, "");
  if (!rest) {
    const list = await env.REPORTS.list({ prefix: "zip/", limit: 1000 });
    const rows = list.keys
      .sort((a, b) => (a.name < b.name ? 1 : -1))
      .map((k) => ({ id: k.name.slice(4), ...k.metadata }));
    return new Response(JSON.stringify(rows, null, 2), { headers: { "Content-Type": "application/json" } });
  }
  const [id, part] = rest.split("/");
  const note = part === "note";
  const body = await env.REPORTS.get(note ? `note/${id}` : `zip/${id}`, { type: note ? "text" : "arrayBuffer" });
  if (body === null) {
    return text("No such report.", 404);
  }
  return new Response(body, { headers: { "Content-Type": note ? "text/plain; charset=utf-8" : "application/zip" } });
}

async function sha(s) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("").slice(0, 16);
}

function text(s, status) {
  return new Response(s + "\n", { status, headers: { "Content-Type": "text/plain; charset=utf-8" } });
}

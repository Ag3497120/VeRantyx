// POST /api/report: one report from a player's headset (raw-deflate JSON, Content-Type application/x-glovebox-report).
// No IP address or anything else about the sender is kept.
//
// On Vercel's Hobby plan Blob allows 2,000 put()s and 1 GB a month for the whole account, and going over shuts it
// off for 30 days, the site's sign-ups included. So this keeps at most REPORTS_PER_HOUR reports an hour (default 1,
// so at most ~720 put()s a month): each hour has that many slots, reports/<day>/<hour>-<slot>.json.deflate, and a
// report whose slot is taken is dropped. A head() checks the slot first (a "simple" operation, 10,000 a month), so a
// dropped report costs no put(). Reports over MAX_STORED are dropped too. REPORTS=off drops everything.
// A dropped report still gets a 204: the headset marks it sent and moves on instead of retrying.
//
// Every valid report, kept or dropped, also counts its headset for the day: a one-byte marker
// active/<day>/<install>/<Glovebox version>/<model> (activePath), which /api/admin/active tallies. A head() first, so
// the day's later reports from that headset cost no put(). Counting never fails a report. COUNT=off stops it (on
// Hobby each new marker is one of the 2,000 monthly put()s).
import { head, put, BlobNotFoundError } from "@vercel/blob";
import { MAX_BYTES, activePath, inflate, validate } from "../lib.js";

export const MAX_STORED = 1 << 20;

export function slotPath(install, now = new Date(), perHour = 1) {
  const t = now.toISOString();
  const slot = parseInt(install.replace(/-/g, "").slice(0, 8), 16) % perHour;
  return `reports/${t.slice(0, 10)}/${t.slice(11, 13)}-${slot}.json.deflate`;
}

async function countHeadset(r) {
  if (process.env.COUNT === "off") return;
  const path = activePath(r);
  try {
    await head(path);
    return;  // already counted today
  } catch (e) {
    if (!(e instanceof BlobNotFoundError)) return;
  }
  await put(path, "1", { access: "private", contentType: "text/plain", addRandomSuffix: false }).catch(() => {});
}

export async function POST(request) {
  if (request.headers.get("content-type") !== "application/x-glovebox-report") return new Response("type", { status: 415 });
  const buf = Buffer.from(await request.arrayBuffer());
  if (buf.length === 0 || buf.length > MAX_BYTES) return new Response("size", { status: 413 });
  let r;
  try { r = validate(JSON.parse(inflate(buf).toString("utf8"))); }
  catch (e) { return new Response("bad report", { status: 400 }); }
  await countHeadset(r);
  const perHour = Math.floor(Number(process.env.REPORTS_PER_HOUR ?? 1));
  if (process.env.REPORTS === "off" || !(perHour > 0) || buf.length > MAX_STORED) return new Response(null, { status: 204 });
  const path = slotPath(r.install, new Date(), perHour);
  try {
    await head(path);
    return new Response(null, { status: 204 });  // this hour's slot is taken
  } catch (e) {
    if (!(e instanceof BlobNotFoundError)) return new Response("store", { status: 503 });
  }
  try {
    await put(path, buf, { access: "private", contentType: "application/octet-stream", addRandomSuffix: false });
  } catch (e) {
    // another report took the slot between the head() and the put()
    if (!/already exists/i.test(String(e?.message))) return new Response("store", { status: 503 });
  }
  return new Response(null, { status: 204 });
}

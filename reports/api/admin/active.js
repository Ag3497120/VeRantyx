// GET /api/admin/active?since=2026-10-11&until=2026-10-17 (UTC days; until defaults to today, since to until):
// how many opted-in headsets sent a report each day, how many of them were new in the range, their Glovebox versions
// and models, and how many different headsets in all. Read from report.js's active/ markers; one list() per day per
// 1,000 markers. Protected host only.
import { list } from "@vercel/blob";
import { isProtectedHost, tally } from "../../lib.js";

const DAY = /^\d{4}-\d{2}-\d{2}$/;
const MAX_DAYS = 400;

export async function GET(request) {
  if (!isProtectedHost(request)) return new Response("not found", { status: 404 });
  const q = new URL(request.url).searchParams;
  const until = q.get("until") || new Date().toISOString().slice(0, 10);
  const since = q.get("since") || until;
  if (!DAY.test(since) || !DAY.test(until) || since > until) {
    return Response.json({ error: "since and until as YYYY-MM-DD, since first" }, { status: 400 });
  }
  const days = [];
  for (let t = Date.parse(since); t <= Date.parse(until); t += 86400000) days.push(new Date(t).toISOString().slice(0, 10));
  if (days.length > MAX_DAYS) return Response.json({ error: `at most ${MAX_DAYS} days` }, { status: 400 });

  const pathnames = [];
  for (const day of days) {
    let cursor;
    do {
      const res = await list({ prefix: `active/${day}/`, cursor, limit: 1000 });
      pathnames.push(...res.blobs.map((b) => b.pathname));
      cursor = res.hasMore ? res.cursor : undefined;
    } while (cursor);
  }
  return Response.json({ since, until, ...tally(pathnames) }, { headers: { "cache-control": "no-store" } });
}

// GET /api/admin/get?pathname=reports/...: one report as JSON (inflated); &summary=1 gives only who sent it (app,
// device, install, seq) and how many entries it holds. Protected host only.
import { get } from "@vercel/blob";
import { inflate, isProtectedHost } from "../../lib.js";

export async function GET(request) {
  if (!isProtectedHost(request)) return new Response("not found", { status: 404 });
  const pathname = new URL(request.url).searchParams.get("pathname") || "";
  if (!pathname.startsWith("reports/")) return new Response("bad path", { status: 400 });
  const res = await get(pathname, { access: "private" });
  if (!res || res.statusCode !== 200) return new Response("not found", { status: 404 });
  const buf = Buffer.from(await new Response(res.stream).arrayBuffer());
  if (!new URL(request.url).searchParams.get("summary"))
    return new Response(inflate(buf), { headers: { "content-type": "application/json" } });
  const r = JSON.parse(inflate(buf).toString("utf8"));
  return Response.json({ app: r.app, device: r.device, install: r.install, seq: r.seq,
    hitches: r.hitches?.length, entries: r.entries?.length, modules: r.modules?.length });
}

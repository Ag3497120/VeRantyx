// GET /api/admin/list?prefix=reports/2026-10-05&cursor=...: stored reports (pathname, size, time). Protected host only.
import { list } from "@vercel/blob";
import { isProtectedHost } from "../../lib.js";

export async function GET(request) {
  if (!isProtectedHost(request)) return new Response("not found", { status: 404 });
  const q = new URL(request.url).searchParams;
  const res = await list({ prefix: q.get("prefix") || "reports/", cursor: q.get("cursor") || undefined, limit: 1000 });
  return Response.json({
    blobs: res.blobs.map((b) => ({ pathname: b.pathname, size: b.size, uploadedAt: b.uploadedAt })),
    cursor: res.cursor || null, hasMore: res.hasMore,
  });
}

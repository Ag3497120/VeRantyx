// GET /api/admin/delete?pathname=reports/...: delete one stored report or active/ marker (test uploads, or a player
// who asks us to). Protected host only.
import { del } from "@vercel/blob";
import { isProtectedHost } from "../../lib.js";

export async function GET(request) {
  if (!isProtectedHost(request)) return new Response("not found", { status: 404 });
  const pathname = new URL(request.url).searchParams.get("pathname") || "";
  if (!/^reports\/\d{4}-\d{2}-\d{2}\/[A-Za-z0-9-]+\.json\.deflate$/.test(pathname) &&
      !/^active\/\d{4}-\d{2}-\d{2}\/[0-9a-f-]{36}\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+$/.test(pathname))
    return new Response("bad path", { status: 400 });
  await del(pathname);
  return Response.json({ deleted: pathname });
}

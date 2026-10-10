// Shared by the report endpoint and its admin routes. A report is a raw-deflate JSON document from a player's
// headset (diagnostics/ in the repo): the pipelines the game needed and the ones that stuttered, nothing personal.
import { inflateRawSync } from "node:zlib";

export const MAX_BYTES = 2 << 20;        // compressed upload limit
export const MAX_JSON = 32 << 20;        // after inflating
const HEX16 = /^[0-9a-f]{16}$/, NAME = /^[0-9a-f]{40}$/, UUID = /^[0-9a-f-]{36}$/, SHA = /^[0-9a-f]{64}$/;

export function inflate(buf) {
  return inflateRawSync(buf, { maxOutputLength: MAX_JSON });
}

// Throws on anything that is not a well-formed v1 report.
export function validate(r) {
  const bad = (why) => { throw new Error(why); };
  if (!r || r.v !== 1) bad("version");
  if (!UUID.test(r.install || "")) bad("install");
  if (!Number.isInteger(r.seq) || r.seq < 0) bad("seq");
  for (const k of ["glovebox", "kit"]) if (typeof r.app?.[k] !== "string" || r.app[k].length > 40) bad("app");
  if (typeof r.device?.model !== "string" || r.device.model.length > 40) bad("device");
  if (typeof r.device?.os !== "string" || r.device.os.length > 20) bad("os");
  for (const k of ["hitches", "entries", "modules"]) if (!Array.isArray(r[k])) bad(k);
  for (const h of r.hitches) if (!HEX16.test(h.p || "") || typeof h.ms !== "number" || typeof h.t !== "number") bad("hitch");
  for (const e of r.entries) if (!NAME.test(e.n || "") || typeof e.j !== "string") bad("entry");
  for (const m of r.modules)
    if (!NAME.test(m.n || "") || typeof m.j !== "string" || !SHA.test(m.sha256 || "") || !Number.isInteger(m.size)) bad("module");
  return r;
}

// Where report.js marks a headset as active today; /api/admin/active reads these back with tally().
export function activePath(r, now = new Date()) {
  const seg = (s) => {
    const t = String(s).replace(/[^A-Za-z0-9._-]/g, "_").slice(0, 40);
    return /^\.*$/.test(t) ? "_" : t;
  };
  return `active/${now.toISOString().slice(0, 10)}/${r.install}/${seg(r.app.glovebox)}/${seg(r.device.model)}`;
}

// Marker pathnames -> per day: different headsets, how many were new in this tally, and their Glovebox versions and
// models (a headset that updated during a day counts under both versions).
export function tally(pathnames) {
  const byDay = new Map();
  for (const p of pathnames) {
    const [top, day, install, version, model] = p.split("/");
    if (top !== "active" || !model) continue;
    if (!byDay.has(day)) byDay.set(day, { installs: new Set(), versions: {}, models: {}, seen: new Set() });
    const d = byDay.get(day);
    if (!d.seen.has(`${install}/${version}`)) {
      d.seen.add(`${install}/${version}`);
      d.versions[version] = (d.versions[version] || 0) + 1;
    }
    if (!d.installs.has(install)) {
      d.installs.add(install);
      d.models[model] = (d.models[model] || 0) + 1;
    }
  }
  const all = new Set();
  const days = [...byDay.keys()].sort().map((day) => {
    const d = byDay.get(day);
    const fresh = [...d.installs].filter((i) => !all.has(i));
    fresh.forEach((i) => all.add(i));
    return { day, headsets: d.installs.size, new: fresh.length, versions: d.versions, models: d.models };
  });
  return { headsets: all.size, days };
}

// Admin routes answer only on this deployment's own URL (VERCEL_URL, e.g. glovebox-reports-<hash>-<team>.vercel.app),
// which Vercel Authentication protects. Every alias (the production domain, the team alias) is public, so they all
// get a 404 for these routes.
export function isProtectedHost(request) {
  const host = (request.headers.get("host") || "").toLowerCase();
  const own = (process.env.VERCEL_URL || "").toLowerCase();
  return own !== "" && host === own;
}

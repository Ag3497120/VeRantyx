# glovebox-reports

The opt-in stutter-report endpoint (Vercel project `glovebox-reports`, private Blob store `glovebox-reports`).

- `POST https://glovebox-reports.vercel.app/api/report`: a report from a player's headset. The body is raw-deflate
  JSON with `Content-Type: application/x-glovebox-report` (format in `lib.js` → `validate`, writer in
  `diagnostics/`). Anything malformed gets a 400. Stored privately; nothing about the sender (no IP) is kept.
- It keeps at most `REPORTS_PER_HOUR` reports an hour (default 1) and none over 1 MB; the rest get a 204 and are
  dropped. On Vercel's Hobby plan Blob allows 2,000 writes and 1 GB a month for the whole account and shuts off for 30
  days past either (the site's sign-ups too). `REPORTS=off` drops everything. Delete reports once they're processed
  (`/api/admin/delete`; deleting is free).
- Every valid report, kept or dropped, counts its headset once a day: an `active/<day>/<install>/<version>/<model>`
  marker. `GET /api/admin/active?since=YYYY-MM-DD&until=YYYY-MM-DD` tallies them: opted-in headsets per day, how many
  were new, versions, models, and different headsets in all. Each headset's first report of a day is one write, so
  on Hobby set `COUNT=off`.
- `GET /api/admin/list`, `GET /api/admin/get?pathname=…`: read the stored reports; `GET /api/admin/delete?pathname=…`
  deletes one report or marker (test uploads, or a player who asks us to). They answer only on the
  deployment's own URL, which Vercel Authentication protects (maintainers read them through Vercel's login).
  Every public alias gets a 404 for them.

Deploy: the files here, as a production deployment of the project (framework none, Node 24).

Processing: save the reports (`/api/admin/get` JSON, or the stored `.json.deflate`) and run
`tools/reports_ingest.py <out dir> packages/alyx/pipelines/alyx.recipe <spirv-index.tsv> <reports>... --game <game dir>`.
It turns each install's fingerprints into a recipe through the index (`tools/spirv_index.py`), merges them after the
release's recipe (`pipelines.py merge`), proves the result rebuilds from the game's files, and triages the stutters
(`tools/hitch_report.py`). Reports are untrusted: malformed ones are skipped.

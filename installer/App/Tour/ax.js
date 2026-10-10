// osascript -l JavaScript ax.js <windows|texts|dump|press> <process> [name-or-description]
function run(argv) {
  const [cmd, app, target] = argv
  const se = Application('System Events')
  const p = app.includes('.') ? se.processes.whose({ bundleIdentifier: app })[0] : se.processes.byName(app)
  const wins = p.windows()
  const geo = e => { try { return `${e.position()} ${e.size()}` } catch (x) { return '' } }
  if (cmd === 'windows') return wins.map(w => `window "${w.name()}" ${geo(w)}`).join('\n')
  // Every text on every window (the guide's bubble is a window of its own), one window a line.
  if (cmd === 'texts') return wins.map(w => {
    const out = []
    try {
      for (const e of w.entireContents()) {
        try { if (e.role() === 'AXStaticText') { const v = String(e.value() ?? '') || String(e.description() ?? ''); if (v) out.push(v) } } catch (x) {}
      }
    } catch (x) {}
    return `[${geo(w)}] ${out.join(' | ')}`
  }).join('\n')
  if (!wins.length) return 'no windows'
  const w = wins[0]
  const all = w.entireContents()
  const label = e => {
    const v = k => { try { return String(e[k]() ?? '') } catch (x) { return '' } }
    return { r: v('role'), n: v('name'), d: v('description'), t: v('value').slice(0, 50) }
  }
  if (cmd === 'dump') return all.slice(0, 500).map(e => { const l = label(e); return `${l.r} | ${l.n} | ${l.d} | ${l.t} | ${geo(e)}` }).join('\n')
  if (cmd === 'pressat') {
    // the n-th element with this role, e.g. pressat Xcode AXButton -1 (the last button)
    const els = all.filter(e => { try { return e.role() === target } catch (x) { return false } })
    const i = Number(argv[3]); const e = els[i < 0 ? els.length + i : i]
    if (!e) return `no ${target} #${i} of ${els.length}`
    try { e.actions.byName('AXPress').perform(); return `pressed ${target} ${geo(e)}` } catch (x) { return `failed: ${x}` }
  }
  if (cmd === 'press') {
    // The front window first, then the others (Glovebox's main window isn't always its first).
    const rest = wins.slice(1).flatMap(x => { try { return x.entireContents() } catch (e) { return [] } })
    for (const e of all.concat(rest)) {
      const l = label(e)
      if (l.n === target || l.d === target) {
        try { e.actions.byName('AXPress').perform(); return `pressed ${l.r} ${geo(e)}` } catch (x) {}
        try { e.selected = true; return `selected ${l.r} ${geo(e)}` } catch (x) { return `failed ${l.r}: ${x}` }
      }
    }
    return `not found: ${target}`
  }
}

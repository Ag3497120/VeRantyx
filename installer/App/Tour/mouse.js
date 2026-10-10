// osascript -l JavaScript mouse.js <x> <y> [click]: glides the pointer to x,y (points from the top
// left of the main screen) the way a hand would, then clicks if asked. Through osascript, which
// may post events on CI where a plain binary may not. For the walkthrough video.
ObjC.import('Cocoa')
function run(argv) {
  const x = Number(argv[0]), y = Number(argv[1])
  let from = { x: x - 240, y: y + 160 }
  try { const p = $.CGEventGetLocation($.CGEventCreate(null)); from = { x: p.x, y: p.y } } catch (e) {}
  const steps = 45
  for (let i = 1; i <= steps; i++) {
    const t = i / steps, e = t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2
    const p = { x: from.x + (x - from.x) * e, y: from.y + (y - from.y) * e }
    $.CGEventPost($.kCGHIDEventTap, $.CGEventCreateMouseEvent(null, $.kCGEventMouseMoved, p, $.kCGMouseButtonLeft))
    delay(0.015)
  }
  if (argv[2] === 'click') {
    delay(0.25)
    $.CGEventPost($.kCGHIDEventTap, $.CGEventCreateMouseEvent(null, $.kCGEventLeftMouseDown, { x: x, y: y }, $.kCGMouseButtonLeft))
    delay(0.09)
    $.CGEventPost($.kCGHIDEventTap, $.CGEventCreateMouseEvent(null, $.kCGEventLeftMouseUp, { x: x, y: y }, $.kCGMouseButtonLeft))
  }
  return `mouse: ${Math.round(from.x)},${Math.round(from.y)} -> ${x},${y}${argv[2] === 'click' ? ' click' : ''}`
}

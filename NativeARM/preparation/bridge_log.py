import sys
from pathlib import Path
out=Path(sys.argv[1]);lines=[];size=0
for line in sys.stdin:
 if any(key in line for key in ('Tracking wait:','Tracking ready','Tracking origin:','Session state=','Runtime:','resolution:','renderer','failed','error','Error','unavailable')):
  lines.append(line);size+=len(line)
  while size>16384:size-=len(lines.pop(0))
  out.write_text(''.join(lines))

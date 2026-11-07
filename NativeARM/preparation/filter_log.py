"""Bounded preparation summary; discard ordinary engine/debug output."""
import sys
from pathlib import Path
out=Path(sys.argv[1]);written=0
with out.open('w') as f:
 for line in sys.stdin:
  if any(s in line for s in ('[foz]','guest fault','SIGSEGV','Assertion failed','device lost')) and written<65536:
   f.write(line);written+=len(line);f.flush()

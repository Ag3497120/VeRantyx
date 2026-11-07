from pathlib import Path
import struct,sys,os
old,new=map(Path,sys.argv[1:]);seen=set();parts=[];header=None
for p in (old,new):
 if not p.exists():continue
 b=p.read_bytes()
 if len(b)<16:continue
 if header is None:header=b[:16]
 if b[:16]!=header:raise ValueError('Incompatible local pipeline recording')
 at=16
 while at+56<=len(b):
  n=b[at:at+40];size=struct.unpack_from('<I',b,at+40)[0];end=at+56+size
  if end>len(b):break
  if n not in seen:parts.append(b[at:end]);seen.add(n)
  at=end
if header is not None:
 t=old.with_name(old.name+'.tmp');t.write_bytes(header+b''.join(parts));os.replace(t,old)
 if new.exists():new.unlink()

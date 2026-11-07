"""Normalize Windows Alyx slot names for the native Linux save browser."""
import os
import pathlib
import re
import sys

def normalize(root):
    root = pathlib.Path(root)
    if not root.is_dir():
        return []
    changed=[]
    for src in root.iterdir():
        if not src.is_dir() or not re.fullmatch(r'S-?\d+', src.name):
            continue
        dst=src.with_name(src.name.lower())
        if dst.exists() and not os.path.samefile(src,dst):
            raise RuntimeError('Separate uppercase and lowercase save slots exist: '+str(src))
        temporary=src.with_name('.case-normalize-'+src.name+'-'+str(os.getpid()))
        if temporary.exists():
            raise RuntimeError('Unfinished slot-name normalization: '+str(temporary))
        src.rename(temporary)
        try:
            temporary.rename(dst)
        except BaseException:
            temporary.rename(src)
            raise
        changed.append((src.name,dst.name))
    return changed

if __name__=='__main__':
    for arg in sys.argv[1:]:
        normalize(arg)

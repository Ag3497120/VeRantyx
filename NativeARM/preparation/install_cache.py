#!/usr/bin/env python3
"""Rebuild the portable no-code recipe from the owner's game, then pin identity."""
from pathlib import Path
import subprocess,sys

def prepare(base):
    base=Path(base).resolve();here=base/'preparation';(here/'cache').mkdir(parents=True,exist_ok=True)
    subprocess.run([sys.executable,str(here/'pipelines.py'),'build',str(here/'alyx.recipe'),str(base/'native-game/game'),str(here/'alyx.foz')],check=True)
    subprocess.run([sys.executable,str(here/'manage_cache.py'),'bootstrap'],check=True)

if __name__=='__main__':prepare(sys.argv[1])

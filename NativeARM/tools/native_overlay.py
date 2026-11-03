"""Private native probe roots, including read-only assets on another volume."""
import json
from pathlib import Path
import shutil
import subprocess

MUTABLE = ('game/hlvr/cfg', 'game/hlvr/save', 'game/hlvr/SAVE', 'game/hlvr/shadercache', 'cache', 'logs')


def prepare(source, overlay, load_save=False, read_only_assets=False):
    source, overlay = source.resolve(), overlay.resolve()
    if source == overlay or source in overlay.parents or overlay in source.parents:
        raise ValueError('probe overlay must be separate from the installation')
    if read_only_assets:
        # A copy cannot be cloned across volumes. Link only immutable content;
        # the child is OS-confined to writing inside the probe's own run root.
        def mirror(path, relative):
            target = overlay/relative
            if str(relative) in MUTABLE: return
            if path.is_dir() and not path.is_symlink():
                target.mkdir()
                for item in path.iterdir(): mirror(item, relative/item.name)
            else:
                resolved = path.resolve(strict=True)
                if not resolved.is_relative_to(source) or not resolved.is_file():
                    raise ValueError('immutable asset link escapes the installation')
                target.symlink_to(resolved)
        mirror(source, Path())
    else:
        subprocess.run(['cp', '-cR', str(source), str(overlay)], check=True)
    for relative in MUTABLE:
        target = overlay/relative
        if target.is_symlink(): target.unlink()
        elif target.exists(): shutil.rmtree(target)
        if relative == 'game/hlvr/cfg' or (load_save and relative.lower() == 'game/hlvr/save'):
            shutil.copytree(source/relative, target, symlinks=False)
        else: target.mkdir(parents=True)
    if not read_only_assets:
        if any(not p.resolve().is_relative_to(overlay) for p in overlay.rglob('*') if p.is_symlink()):
            raise ValueError('overlay still contains a link outside its root')


def confine(command, run):
    # The write restriction follows symlinks, protecting the installed files as
    # well as saves/preferences elsewhere on the Mac. Reads and Steam IPC keep
    # their normal behaviour. TMPDIR must also be private to this run.
    run = run.resolve()
    profile = run/'game-write-scope.sb'
    profile.write_text('(version 1)\n(allow default)\n(deny file-write*)\n'
                       '(allow file-write* (subpath '+json.dumps(str(run))+') '
                       '(literal "/dev/null"))\n')
    return ['/usr/bin/sandbox-exec', '-f', str(profile), *map(str, command)]

#!/usr/bin/env python3
"""Install the runtime; link user-owned Valve files locally, never ship them."""
from pathlib import Path
import argparse, datetime, json, os, shutil, struct, subprocess, sys, tempfile

def arm64_elf(path):
    try:
        with path.open('rb') as f:
            h = f.read(20)
        return len(h) == 20 and h[:6] == b'\x7fELF\x02\x01' and struct.unpack('<H', h[18:20])[0] == 183
    except OSError:
        return False

def link_entries(source, target, skip=()):
    target.mkdir(parents=True, exist_ok=True)
    for item in source.iterdir():
        if item.name not in skip and not item.name.startswith('.'):
            (target/item.name).symlink_to(item.resolve(), target_is_directory=item.is_dir())

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='Installed Alyx content directory')
    p.add_argument('depot', type=Path, help='Your SteamCMD depot_546564 directory')
    p.add_argument('--debian', type=Path, help='Extracted Debian root; otherwise fetch official packages')
    p.add_argument('--notproton', action='store_true', help='Install the optional macOS Steam compatibility tool')
    a = p.parse_args()
    package = Path(__file__).resolve().parent
    game, depot = a.game.resolve(), a.depot.resolve()
    if not (game/'game/hlvr').is_dir():
        p.error('game must contain game/hlvr')
    bins = [depot/'game/bin/linuxsteamrtarm64', depot/'game/hlvr/bin/linuxsteamrtarm64']
    if not all(b.is_dir() for b in bins) or not arm64_elf(bins[0]/'libengine2.so'):
        p.error('depot must contain Linux ARM64 Alyx libraries, not Windows/x86 Linux binaries')
    if not (package/'runtime/alyx_host').is_file():
        p.error('Use the runtime release archive; source archives need build-release.sh first')
    # A custom marker is checked rather than removing/replacing arbitrary folders.
    dest = game/'verantyx'
    if dest.exists() and not (dest/'moltenvr-install.json').is_file():
        p.error('An unmanaged verantyx folder exists. Rename it before installing this release.')
    processes = subprocess.check_output(['ps', 'ax', '-o', 'comm='], text=True).splitlines()
    if any(Path(x.strip()).name.startswith('alyx_host') for x in processes):
        p.error('Close native Alyx before installing')
    debian = a.debian.resolve() if a.debian else game/'.alyx-native-debian'
    if not a.debian:
        subprocess.run(['bash', str(package/'fetch-debian.sh'), str(debian)], check=True)
    deb_lib = debian/'usr/lib/aarch64-linux-gnu'
    if not deb_lib.is_dir():
        p.error('Debian root must contain usr/lib/aarch64-linux-gnu')
    stage = Path(tempfile.mkdtemp(prefix='.alyx-native-install-', dir=game))
    try:
        shutil.copytree(package/'runtime', stage, dirs_exist_ok=True)
        for name in ('alyx_host', 'bridge'):
            (stage/name).chmod(0o755)
        guest = stage/'guest-lib'; guest.mkdir()
        for directory in bins + [deb_lib]:
            for f in sorted(directory.glob('*.so*')):
                if f.name.startswith(('libvulkan_freedreno_hlvr', 'libmimalloc')) or not arm64_elf(f):
                    continue
                for name in (f.name, f.name.split('.so')[0]+'.so'):
                    if not (guest/name).exists():
                        (guest/name).symlink_to(f.resolve())
        root = stage/'native-game'; (root/'game').mkdir(parents=True)
        link_entries(game/'game', root/'game', ('bin', 'hlvr'))
        link_entries(game/'game/bin', root/'game/bin', ('linuxsteamrtarm64',))
        link_entries(game/'game/hlvr', root/'game/hlvr', ('bin', 'cfg', 'shadercache', 'gameinfo.gi', 'vr_fidelity_levels_override.csv'))
        link_entries(game/'game/hlvr/bin', root/'game/hlvr/bin', ('linuxsteamrtarm64',))
        for rel in ('game/bin/linuxsteamrtarm64', 'game/hlvr/bin/linuxsteamrtarm64'):
            (root/rel).symlink_to(depot/rel, target_is_directory=True)
        # Writable native settings and cache are independent of the content installation.
        cfg = root/'game/hlvr/cfg'
        if (game/'game/hlvr/cfg').is_dir():
            shutil.copytree(game/'game/hlvr/cfg', cfg)
        else:
            cfg.mkdir()
        (root/'game/hlvr/shadercache/vulkan').mkdir(parents=True)
        (root/'game/hlvr/vr_fidelity_levels_override.csv').symlink_to('../../../vr_fidelity_levels_override.csv')
        (root/'steam_appid.txt').write_text('546560\n')
        shutil.copy2(game/'game/hlvr/gameinfo.gi',root/'game/hlvr/gameinfo.gi')
        from prepare_native_ui import prepare
        prepare(game,root)
        shutil.copy2(package/'normalize_save_names.py',stage/'normalize_save_names.py')
        if dest.exists():
            for rel in ('native-vr-settings.txt', 'pipeline-cache', 'pipeline-cache-v2', 'native-game/game/hlvr/cfg', 'native-game/game/hlvr/shadercache', 'native-game/game/hlvr/save', 'preparation/cache'):
                old, new = dest/rel, stage/rel
                if old.exists() and new.exists() and old.resolve()==new.resolve():
                    continue
                if old.is_dir():
                    shutil.copytree(old, new, dirs_exist_ok=True)
                elif old.is_file():
                    shutil.copy2(old, new)
        # Build shader code only from the user's own verified game files.
        cache_installer=stage/'preparation/install_cache.py'
        subprocess.run([sys.executable,str(cache_installer),str(stage)],check=True)
        # Keep learned pipelines only when every recorded game-shader input matches.
        try:
            before=json.loads((dest/'preparation/recipe-contract.json').read_text())['expected_inputs']
            after=json.loads((stage/'preparation/recipe-contract.json').read_text())['expected_inputs']
            shader=lambda data:{k:v for k,v in data.items() if k.startswith(('core:', 'hlvr:'))}
            if shader(before) and shader(before)==shader(after):
                for name in ('recorded.foz','recorded.next.foz'):
                    if (dest/'preparation'/name).is_file():shutil.copy2(dest/'preparation'/name,stage/'preparation'/name)
        except (OSError,ValueError,KeyError,TypeError):
            pass
        (stage/'moltenvr-install.json').write_text(json.dumps({'version':'0.3.2','depot':str(depot)}, indent=2))
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        if dest.exists():
            dest.rename(dest.with_name('verantyx.bak.'+stamp))
        stage.rename(dest)
        launcher = game/'start.command'
        if launcher.exists():
            shutil.copy2(launcher, game/('start.command.bak.'+stamp))
        shutil.copy2(package/'start.command', launcher); launcher.chmod(0o755)
        if a.notproton:
            subprocess.run(['bash', str(package/'steam/install-notproton.sh'), str(game)], check=True)
        print('Installed. Start native macOS Steam and MoltenVR, then run:')
        print(f'bash "{launcher}"')
    finally:
        if stage.exists():
            shutil.rmtree(stage)

if __name__ == '__main__':
    main()

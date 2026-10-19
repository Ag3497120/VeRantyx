#!/usr/bin/env python3
"""Classify compute SPIR-V files using VXRuntime's in-tree lowering and MSL emitter."""

import argparse
import pathlib
import subprocess
import sys


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    default_translator = repo_root / "VXRuntime" / "build" / "spirv2msl"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--translator", type=pathlib.Path, default=default_translator,
                        help="path to the in-tree spirv2msl executable")
    parser.add_argument("--compile", action="store_true",
                        help="also ask Metal to compile each emitted shader")
    parser.add_argument("shaders", nargs="+", type=pathlib.Path, help="SPIR-V compute modules")
    args = parser.parse_args()

    failures = 0
    for shader in args.shaders:
        command = [str(args.translator)]
        if args.compile:
            command.append("--compile")
        command.append(str(shader))
        try:
            result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                    text=True, check=False)
        except OSError as error:
            print(f"{shader}: could not run {args.translator}: {error}", file=sys.stderr)
            return 2
        if result.returncode == 0:
            print(f"{shader}: translated" + (" and Metal-compiled" if args.compile else ""))
        elif result.returncode in (5, 6):
            print(f"{shader}: unsupported by the current VXRuntime lowering")
            if result.stderr:
                print(result.stderr.rstrip(), file=sys.stderr)
            failures += 1
        else:
            print(f"{shader}: invalid or failed (exit {result.returncode})")
            if result.stderr:
                print(result.stderr.rstrip(), file=sys.stderr)
            failures += 1
    if failures:
        print(f"spirv_check: {failures}/{len(args.shaders)} files need support or inspection", file=sys.stderr)
        return 1
    print(f"spirv_check: {len(args.shaders)} files passed the in-tree compute path")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# Native Alyx pipeline preparation helpers

These optional helpers reconstruct a no-code pipeline recipe from your own installed Alyx shader VPKs, combine locally recorded pipelines, track cache identity and run preparation/play sessions under a shared lock. They contain no Valve shader bytecode, generated Fossilize archives or game assets.

The recipe and `gb/` readers originate from [Glovebox](https://github.com/glovebox-vr/glovebox), under MIT; see `Glovebox-LICENSE` and `SOURCE.json`. Fossilize is pinned to `556da9ff7d7616704dda515756219ab1dab693ec`.

## Integration contract

This directory is intended to be copied to `preparation/` beside a compatible `alyx_host`, `guest-lib/`, `native-game/` private overlay and `moltenvk-game/`. Runtime replay/record callbacks and progress reporting are supplied by the optional MoltenVR integration in a separate PR. These Python files alone do not add Fossilize replay to the existing host and do not replace the existing `scripts/prewarm_shaders.sh`.

`install_cache.py` creates the owner-local recipe and expected runtime/content contract. `manage_cache.py verify` checks that contract; runtime changes must not silently certify an incompatible cache. `cache_control.py verify --force` rehashes bytes when corruption is suspected. A successful identity check is not proof that every pipeline is already compiled.

`session_runner.py` holds the preparation/play lock and forwards shutdown to its child. `prepare_cache.py` uses an isolated game overlay and bounded output, leaving saved preferences and saves intact. It does not need an active headset. Newly encountered materials can still require a first-use compilation; this is not a guarantee of complete level coverage.

## Offline tests

```sh
PYTHONPATH=NativeARM/preparation python3 -m unittest discover -s NativeARM/preparation -p 'test_*.py'
```

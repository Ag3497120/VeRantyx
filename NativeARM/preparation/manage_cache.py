#!/usr/bin/env python3
"""Local preparation identity and cache status. Never deletes shaders or saves."""
from pathlib import Path
import sys,json,time,os
import cache_control
root=Path(__file__).resolve().parent
base=root.parent
installation=base.parent
mode=sys.argv[1] if len(sys.argv)>1 else 'verify'
state=root/'cache/identity.json'
inputs={'host':base/'alyx_host','moltenvk':base/'moltenvk-game/libMoltenVK.dylib','recipe':root/'alyx.foz','fidelity':base/'vr_fidelity_levels_override.csv'}
if (root/'supplemental.foz').exists():inputs['supplemental']=root/'supplemental.foz'
source=base/'native-game'
vpks={n:source/'game'/n/'shaders_vulkan_dir.vpk' for n in ('core','hlvr')}
# Some shader trees use a shaders/ prefix; never silently omit verification.
for n,p in list(vpks.items()):
 if not p.exists():vpks[n]=p.parent/'shaders/shaders_vulkan_dir.vpk'
policy={'msaa':4,'metal_argument_buffers':0,'cache_schema':3,'quality':'preserved','async_game_workers':3}
if mode=='status':result=cache_control.status(state)
else:
 started=time.monotonic()
 for label,path in vpks.items():inputs.update(cache_control.vpk_inputs(label,path))
 def report(done,total,hashed,reused):
  path=os.getenv('ALYX_PREWARM_PROGRESS')
  if path and mode!='bootstrap':cache_control.atomic_write(path,{'state':0,'phase':'checking','done':done,'total':total,'hashed':hashed,'reused':reused})
 result=cache_control.verify(state,inputs,policy,force=mode=='force',on_progress=report)
 result['verification_seconds']=round(time.monotonic()-started,3)
 contract=root/'recipe-contract.json'
 if mode=='bootstrap':
  cache_control.atomic_write(contract,cache_control.make_contract(result,list(inputs),list(policy)))
 check=cache_control.validate_contract(contract,result)
 if not check['valid']:
  print(json.dumps(check,indent=2),file=sys.stderr)
  raise SystemExit('Preparation data no longer matches this game/runtime. Rebuild Test 3 data or launch the stable version.')
 result['contract_valid']=True
if mode=='status':print(json.dumps(result,indent=2,sort_keys=True))
else:print('Cache identity verified: %s files hashed, %s reused; %.2f s.'%(result['hashed'],result['reused'],result['verification_seconds']))

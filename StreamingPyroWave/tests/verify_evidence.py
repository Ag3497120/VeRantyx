"""Check the published scalar table against its machine-readable evidence."""
import json,math
from pathlib import Path
r=Path(__file__).parent/'evidence'
d=json.loads((r/'high-bitrate-summary.json').read_text())
assert len(d['rows'])==5
for row in d['rows']:
    assert row['samples']==21 and row['target_mbps']==2000
    for metric in row['metrics'].values():
        assert all(math.isfinite(v) for v in metric.values())
        assert metric['min']<=metric['mean']<=metric['max']
hi=d['rows'][0];assert hi['per_eye']==[2368,2560]
assert abs(hi['metrics']['actual_mbps']['mean']-2000.02)<.01
assert abs(hi['metrics']['encode_ms']['mean']-5.37)<.01
assert abs(hi['metrics']['decode_ms']['mean']-9.77)<.01
assert d['rows'][-1]['metrics']['reported_headset_fps']['mean']<70
ack=json.loads((r/'ack-600.json').read_text());assert len(ack['cases'])==3
assert [round(x['mean_ms'],2) for x in ack['cases']]==[29.80,31.67,59.56]
print('Historical evidence: 2000 Mbps table, overload case and 600 Mbps ACK rows verified')

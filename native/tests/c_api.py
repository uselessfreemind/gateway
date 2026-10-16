"""Exercise the exact C ABI consumed by the WinUI client."""
import ctypes as c
import json
from pathlib import Path
import sys
import tempfile
import time

lib=c.CDLL(str(Path(sys.argv[1]).resolve()))
lib.scada_demo.argtypes=[c.c_uint,c.c_uint,c.c_int,c.c_char_p];lib.scada_demo.restype=c.c_void_p
lib.scada_open.argtypes=[c.c_char_p];lib.scada_open.restype=c.c_void_p
lib.scada_close.argtypes=[c.c_void_p];lib.scada_close.restype=None
lib.scada_stop.argtypes=[c.c_void_p];lib.scada_stop.restype=c.c_int
lib.scada_snapshot.argtypes=[c.c_void_p,c.c_char_p];lib.scada_snapshot.restype=c.c_void_p
lib.scada_history.argtypes=[c.c_void_p,c.c_char_p,c.c_char_p,c.c_int64,c.c_int64,c.c_int,c.c_int64];lib.scada_history.restype=c.c_void_p
lib.scada_trend.argtypes=[c.c_void_p,c.c_char_p,c.c_char_p,c.c_int64,c.c_int64,c.c_int64];lib.scada_trend.restype=c.c_void_p
lib.scada_free.argtypes=[c.c_void_p];lib.scada_free.restype=None
lib.scada_last_error.restype=c.c_char_p

def take(p):
    assert p,lib.scada_last_error()
    try:return json.loads(c.string_at(p))
    finally:lib.scada_free(p)

with tempfile.TemporaryDirectory(prefix='scada-abi-') as temp:
    handle=lib.scada_demo(2,10,10,str(Path(temp)/'demo.db').encode())
    assert handle,lib.scada_last_error()
    try:
        until=time.monotonic()+5
        while True:
            snapshot=take(lib.scada_snapshot(handle,b'demo0'))
            if len(snapshot['samples'])==10 and snapshot['archivedSamples']>=20:break
            assert time.monotonic()<until,snapshot
            time.sleep(.02)
        assert len(snapshot['controllers'])==2
        assert all(s['quality']=='Good' for s in snapshot['samples'])
        now=int(time.time()*1000)
        history=take(lib.scada_history(handle,b'demo0',b'tag0',now-60000,now+1000,100,0))
        assert history['samples']
        trend=take(lib.scada_trend(handle,b'demo0',b'tag0',now-60000,now+1000,1000))
        assert trend and sum(b['goodCount'] for b in trend)>0
        assert lib.scada_stop(handle)==0
        assert lib.scada_stop(handle)==0
    finally:lib.scada_close(handle)
assert not lib.scada_open(b'/does-not-exist.json')
assert lib.scada_last_error()
assert not lib.scada_snapshot(None,b'')
assert b'Null' in lib.scada_last_error()
print('PASS: UI C ABI startup, current data, archive pages, trends, stop, error and memory ownership')

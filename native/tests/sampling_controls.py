"""Real UDP/TCP retry budget and manual/global sampling recovery through the UI C ABI."""
import asyncio
import ctypes as c
import json
from pathlib import Path
import sys
import tempfile
import time
from plc_simulators import FinsSimulator, cip_server

async def main():
    lib = c.CDLL(str(Path(sys.argv[1]).resolve()))
    lib.scada_open.argtypes=[c.c_char_p];lib.scada_open.restype=c.c_void_p
    lib.scada_snapshot.argtypes=[c.c_void_p,c.c_char_p];lib.scada_snapshot.restype=c.c_void_p
    lib.scada_free.argtypes=[c.c_void_p]
    lib.scada_retry.argtypes=[c.c_void_p,c.c_char_p];lib.scada_retry.restype=c.c_int
    lib.scada_restart_sampling.argtypes=[c.c_void_p];lib.scada_restart_sampling.restype=c.c_int
    lib.scada_close.argtypes=[c.c_void_p]
    enabled=False
    class Fins(FinsSimulator):
        attempts=0
        def datagram_received(self,data,addr):
            self.attempts+=1
            if enabled:super().datagram_received(data,addr)
    fins=Fins()
    transport,_=await asyncio.get_running_loop().create_datagram_endpoint(lambda:fins,local_addr=('127.0.0.1',0))
    sessions=0
    async def cip(reader,writer):
        nonlocal sessions
        sessions+=1
        if enabled:await cip_server(reader,writer)
        else:writer.close();await writer.wait_closed()
    server=await asyncio.start_server(cip,'127.0.0.1',0)
    def snapshot(handle):
        ptr=lib.scada_snapshot(handle,b'')
        assert ptr
        try:return json.loads(c.string_at(ptr))
        finally:lib.scada_free(ptr)
    async def until(handle,predicate):
        deadline=time.monotonic()+12
        while True:
            s=snapshot(handle)
            if predicate(s):return s
            assert time.monotonic()<deadline,s
            await asyncio.sleep(.02)
    with tempfile.TemporaryDirectory() as temp:
        config={'database':str(Path(temp)/'data.db'),'controllers':[
            {'id':id,'protocol':protocol,'host':'127.0.0.1','port':port,'intervalMs':50,'timeoutMs':50,'tags':[{'id':'value','address':address,'type':type,'svid':svid}]}
            for id,protocol,port,address,type,svid in [('fins','FinsUdp',transport.get_extra_info('sockname')[1],'D:0','UInt16',1),('cip','EtherNetIp',server.sockets[0].getsockname()[1],'Tag0','Float32',2)]]}
        path=Path(temp)/'site.json';path.write_text(json.dumps(config))
        h=lib.scada_open(str(path).encode());assert h
        try:
            s=await until(h,lambda s:all(x['connectionState']=='Aborted' for x in s['controllers']))
            assert all(x['retryAttempts']==3 for x in s['controllers'])
            assert fins.attempts==4 and sessions==4,(fins.attempts,sessions)
            await asyncio.sleep(.3);assert fins.attempts==4 and sessions==4
            assert lib.scada_retry(h,b'unknown')==-1
            enabled=True
            assert lib.scada_retry(h,b'fins')==0
            await until(h,lambda s:s['controllers'][0]['connectionState']=='Connected')
            assert sessions==4, 'manual retry must not release another controller'
            assert lib.scada_restart_sampling(h)==0
            s=await until(h,lambda s:all(x['connectionState']=='Connected' for x in s['controllers']))
            assert all(x['retryAttempts']==0 for x in s['controllers'])
            assert all(x['analogue'] and x['address'] for x in s['samples'])
        finally:lib.scada_close(h)
    transport.close();server.close();await server.wait_closed()
    print('PASS: FINS/CIP initial failure + three retries, abort, no background retries, isolated manual retry, global recovery, address metadata')
asyncio.run(main())

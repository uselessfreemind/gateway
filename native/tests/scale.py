#!/usr/bin/env python3
"""Optional 500-controller network topology stress. Reports, never promises, 10 ms timing."""
import asyncio
import json
from pathlib import Path
import sqlite3
import socket
import sys
import tempfile
import time
from plc_simulators import FinsSimulator, cip_server

async def run(binary):
    loop=asyncio.get_running_loop()
    transport,fins=await loop.create_datagram_endpoint(FinsSimulator,local_addr=('127.0.0.1',0))
    # One simulator socket represents 250 physical PLC receive buffers. Avoid
    # harness-only drops when 250 clients send their first read concurrently.
    transport.get_extra_info('socket').setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
    cip=await asyncio.start_server(cip_server,'127.0.0.1',0,backlog=512)
    with tempfile.TemporaryDirectory(prefix='scada-wire-scale-') as temp:
        root=Path(temp);controllers=[]
        for i in range(500):
            controllers.append({'id':f'plc{i}','protocol':'FinsUdp' if i%2==0 else 'EtherNetIp','host':'127.0.0.1',
                'port':transport.get_extra_info('sockname')[1] if i%2==0 else cip.sockets[0].getsockname()[1],
                'intervalMs':10,'timeoutMs':3000,'tags':[{'id':f't{j}','address':f'D:{j}' if i%2==0 else f'Tag{j}',
                'svid':i*700+j+1,'type':'UInt16' if i%2==0 else 'Float32'} for j in range(700)]})
        config={'database':str(root/'scale.db'),'queueCapacity':400000,'controllers':controllers}
        (root/'site.json').write_text(json.dumps(config));began=time.monotonic()
        process=await asyncio.create_subprocess_exec(binary,'--config',str(root/'site.json'),'--run-for','8','--summary',str(root/'summary.json'),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.STDOUT)
        try:
            out=await asyncio.wait_for(process.communicate(),90)
            assert process.returncode==0,out
            report=json.loads((root/'summary.json').read_text());states=report['controllers']
            (Path(binary).parent/'scale-wire-summary.json').write_text(json.dumps(report,indent=2))
            failed=[s for s in states if s['failedReads']>0]
            if failed:print('Failed controller diagnostics:',json.dumps(failed[:8]))
            assert report['totalLatest']==350000,report['totalLatest']
            assert all(s['scans']>=1 for s in states),sum(s['scans']>=1 for s in states)
            assert report['archiveError']=='',report['archiveError']
            assert all(s['failedReads']==0 for s in states),sum(s['failedReads'] for s in states)
            with sqlite3.connect(root/'scale.db') as db:
                distinct=db.execute('SELECT COUNT(*) FROM (SELECT controller,tag FROM native_samples GROUP BY controller,tag)').fetchone()[0]
                assert distinct==350000,distinct
                assert db.execute("SELECT value FROM native_samples WHERE controller='plc499' AND tag='t699' ORDER BY id DESC LIMIT 1").fetchone()[0]>89
            print(json.dumps({'result':'topology PASS; timing is measured, not guaranteed','controllers':500,'tags':350000,
                'elapsedSecondsIncludingDrain':round(time.monotonic()-began,2),'archiveRows':report['archiveRows'],
                'missedDeadlines':sum(s['missedDeadlines'] for s in states),'backpressureEvents':report['backpressureEvents'],
                'maxLastScanMs':max(s['scanMs'] for s in states),'minCompletedScans':min(s['scans'] for s in states),
                'finsReadRequests':fins.reads},indent=2))
        finally:
            if process.returncode is None:process.kill();await process.wait()
    transport.close();cip.close();await cip.wait_closed()

if __name__=='__main__':asyncio.run(run(str(Path(sys.argv[1]).resolve())))

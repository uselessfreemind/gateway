#!/usr/bin/env python3
"""Real UDP/TCP FINS, CIP and HSMS tests against the compiled C++ engine."""
import asyncio
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
from plc_simulators import FinsSimulator, cip_server

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]

def item_u(v): return b'\xb1\x04'+struct.pack('>I',v)
def item_l(*children):
    n=len(children)
    assert n<256
    return bytes([1,n])+b''.join(children)
def frame(stype,stream,function,system,body=b'',w=True):
    return struct.pack('>IHBBBBI',10+len(body),65535 if stype else 0,stream|(128 if not stype and w else 0),function,0,stype,system)+body

async def read_frame(reader):
    size=struct.unpack('>I',await asyncio.wait_for(reader.readexactly(4),10))[0]
    assert 10<=size<=1048576
    raw=await asyncio.wait_for(reader.readexactly(size),10)
    session,stream,function,ptype,stype,system=struct.unpack('>HBBBBI',raw[:10])
    assert ptype==0
    return {'session':session,'stream':stream&127,'w':bool(stream&128),'function':function,'stype':stype,'system':system,'body':raw[10:]}

async def host_checks(reader,writer,active=False):
    if active:
        request=await read_frame(reader);assert request['stype']==1
        writer.write(frame(2,0,0,request['system']))
        await writer.drain()
    else:
        packet=frame(1,0,0,1)
        writer.write(packet[:2]);await writer.drain();await asyncio.sleep(.005)
        writer.write(packet[2:]);await writer.drain()
        response=await read_frame(reader);assert response['stype']==2 and response['system']==1
    async def request(s,f,system,body=b''):
        writer.write(frame(0,s,f,system,body));await writer.drain()
        response=await read_frame(reader)
        while response['stream']==6 and response['function']==11:
            writer.write(frame(0,6,12,response['system'],b'\x21\x01\0',w=False));await writer.drain()
            response=await read_frame(reader)
        assert response['function']==f+1 and response['system']==system,response
        return response
    await request(1,13,2,item_l())
    status=await request(1,3,3,item_l(item_u(1)))
    assert status['body'][:4]==b'\x01\x01\x81\x08',status
    assert struct.unpack('>d',status['body'][4:12])[0]==0
    await request(1,17,4)
    await request(2,33,5,item_l(item_u(0),item_l(item_l(item_u(1),item_l(item_u(1))))))
    await request(2,35,6,item_l(item_u(0),item_l(item_l(item_u(1001),item_l(item_u(1))))))
    await request(2,37,7,item_l(b'\x25\x01\x01',item_l(item_u(1001))))
    event=await read_frame(reader)
    assert event['stream']==6 and event['function']==11 and event['w']
    assert item_u(1001) in event['body'] and b'\x81\x08'+struct.pack('>d',0) in event['body']
    writer.write(frame(0,6,12,event['system'],b'\x21\x01\0',w=False));await writer.drain()
    # PLC control through GEM must be rejected.
    command=await request(2,41,8,item_l())
    assert command['body']==item_l(b'\x21\x01\x01',item_l())

async def run(binary):
    loop=asyncio.get_running_loop()
    transport,fins=await loop.create_datagram_endpoint(FinsSimulator,local_addr=('127.0.0.1',0))
    cip=await asyncio.start_server(cip_server,'127.0.0.1',0)
    fins_port=transport.get_extra_info('sockname')[1];cip_port=cip.sockets[0].getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='scada-native-') as temp:
        root=Path(temp)
        for active in [False,True]:
            hsms_port=port();database=root/('active.db' if active else 'passive.db')
            controllers=[]
            for i in range(2):
                controllers.append({'id':f'plc{i}','protocol':'FinsUdp' if i==0 else 'EtherNetIp','host':'127.0.0.1','port':fins_port if i==0 else cip_port,'intervalMs':10,'timeoutMs':1000,
                    'tags':[{'id':f't{j}','address':f'D:{j}' if i==0 else f'Tag{j}','type':'UInt16' if i==0 else 'Float32','svid':i*700+j+1} for j in range(700)]})
            config={'database':str(database),'queueCapacity':100000,'controllers':controllers,'secs':{'enabled':True,'mode':'Active' if active else 'Passive','port':hsms_port,'eventMs':50}}
            file=root/'site.json';file.write_text(json.dumps(config));summary=root/'summary.json'
            accepted=asyncio.Queue()
            async def accept(r,w):await accepted.put((r,w))
            listener=await asyncio.start_server(accept,'127.0.0.1',hsms_port) if active else None
            process=await asyncio.create_subprocess_exec(binary,'--config',str(file),'--run-for','4','--summary',str(summary),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.STDOUT)
            try:
                if active:reader,writer=await asyncio.wait_for(accepted.get(),5)
                else:
                    until=time.monotonic()+5
                    while True:
                        try:reader,writer=await asyncio.open_connection('127.0.0.1',hsms_port);break
                        except OSError:
                            if time.monotonic()>until:raise
                            await asyncio.sleep(.02)
                await asyncio.sleep(.15)
                await host_checks(reader,writer,active)
                writer.close();await writer.wait_closed()
                output=await asyncio.wait_for(process.communicate(),15)
                assert process.returncode==0,output
                report=json.loads(summary.read_text());assert report['totalLatest']==1400,report['totalLatest']
                assert report['archiveError']=='',report['archiveError']
                assert all(c['scans']>0 and c['failedReads']==0 for c in report['controllers']),report['controllers']
                values={(s['controller'],s['tag']):s for s in report['samples']}
                with sqlite3.connect(database) as db:
                    count=db.execute('SELECT COUNT(*) FROM (SELECT controller,tag FROM native_samples GROUP BY controller,tag)').fetchone()[0]
                    assert count==1400,count
                    fins_value=db.execute("SELECT value FROM native_samples WHERE controller='plc0' AND tag='t699' ORDER BY id DESC LIMIT 1").fetchone()[0]
                    cip_value=db.execute("SELECT value FROM native_samples WHERE controller='plc1' AND tag='t699' ORDER BY id DESC LIMIT 1").fetchone()[0]
                    assert fins_value==699 and abs(cip_value-89.9)<.001,(fins_value,cip_value)
                    rows=db.execute('SELECT COUNT(*) FROM native_samples').fetchone()[0]
                    assert rows==report['archivedSamples']==report['archiveRows'],(rows,report['archivedSamples'])
                print(json.dumps({'case':'active' if active else 'passive','result':'PASS','archiveRows':rows,'controllers':report['controllers'],'backpressureEvents':report['backpressureEvents']}))
            finally:
                if process.returncode is None:process.terminate();await process.wait()
                if listener:listener.close();await listener.wait_closed()
        # No-response FINS must become Bad/null and must terminate within timeout.
        silent=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);silent.bind(('127.0.0.1',0))
        bad={'database':str(root/'bad.db'),'controllers':[{'id':'offline','host':'127.0.0.1','port':silent.getsockname()[1],'intervalMs':10,'timeoutMs':30,'tags':[{'id':'t','address':'D:0','svid':1}]}]}
        file=root/'bad.json';file.write_text(json.dumps(bad));summary=root/'bad-summary.json'
        process=await asyncio.create_subprocess_exec(binary,'--config',str(file),'--run-for','1','--summary',str(summary),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.STDOUT)
        output=await asyncio.wait_for(process.communicate(),10);silent.close();assert process.returncode==0,output
        report=json.loads(summary.read_text());assert report['samples'][0]['quality']=='Bad' and report['samples'][0]['value'] is None
        assert report['controllers'][0]['failedReads']>0 and report['controllers'][0]['missedDeadlines']>0
        print('PASS: disconnected FINS -> Bad/null, timeout cancellation, deadline counters')
    transport.close();cip.close();await cip.wait_closed()

if __name__=='__main__':asyncio.run(run(str(Path(sys.argv[1]).resolve())))

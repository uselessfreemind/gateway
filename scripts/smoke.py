#!/usr/bin/env python3
"""End-to-end protocol simulators and API/archive/live checks; no real PLC traffic.
Run from repository root after dotnet build. --controllers 500 --tags 700 exercises the maximum topology.
"""
import argparse
import asyncio
import contextlib
import json
import math
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

class FinsSimulator(asyncio.DatagramProtocol):
    reads = 0
    def connection_made(self, transport):
        self.transport = transport
    def datagram_received(self, data, addr):
        assert len(data) == 18 and data[10:12] == b'\x01\x01', 'Only FINS Memory Area Read is permitted'
        start = int.from_bytes(data[13:15], 'big')
        count = int.from_bytes(data[16:18], 'big')
        assert count <= 400
        reply = bytearray(data[:12])
        reply[0] = 0xc0
        reply[3:6], reply[6:9] = data[6:9], data[3:6]
        reply += b'\0\0'
        if data[12] < 0x80:
            reply += bytes((start * 16 + data[15] + i) % 2 for i in range(count))
        else:
            reply += b''.join(struct.pack('>H', (start + i) % 32768) for i in range(count))
        self.transport.sendto(reply, addr)
        self.reads += 1

async def cip_server(reader, writer):
    def cip_reply(request):
        if request[0] == 0x0a:
            assert request[2:6] == b'\x20\x02\x24\x01'
            body = request[6:]
            count = int.from_bytes(body[:2], 'little')
            offsets = struct.unpack_from('<' + 'H' * count, body, 2)
            replies = [cip_reply(body[offsets[i]:offsets[i+1] if i+1<count else len(body)]) for i in range(count)]
            offset = 2 + count * 2
            encoded_offsets = []
            for reply in replies:
                encoded_offsets.append(offset)
                offset += len(reply)
            return b'\x8a\0\0\0' + struct.pack('<H', count) + struct.pack('<' + 'H'*count, *encoded_offsets) + b''.join(replies)
        assert request[0] == 0x4c and request[-2:] == b'\x01\0', 'Only CIP Read Tag is permitted'
        size = request[3]
        name = request[4:4+size].decode('ascii')
        index = int(''.join(c for c in name if c.isdigit()) or 0)
        return b'\xcc\0\0\0\xca\0' + struct.pack('<f', 20 + index / 10)
    try:
        while True:
            header = bytearray(await reader.readexactly(24))
            command, size = struct.unpack_from('<HH', header)
            payload = await reader.readexactly(size)
            if command == 0x65:
                assert payload == b'\x01\0\0\0'
                reply = payload
            else:
                assert command == 0x6f and payload[12:14] == b'\xb2\0'
                cip = cip_reply(payload[16:])
                reply = b'\0'*6 + b'\x02\0' + b'\0'*4 + b'\xb2\0' + struct.pack('<H', len(cip)) + cip
            struct.pack_into('<H', header, 2, len(reply))
            struct.pack_into('<I', header, 4, 123)
            writer.write(header + reply)
            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        with contextlib.suppress(ConnectionResetError, BrokenPipeError):
            await writer.wait_closed()


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def http_json(port, path, key='test-key'):
    request = urllib.request.Request(f'http://127.0.0.1:{port}{path}', headers={'X-Api-Key': key})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)

async def hsms_check(port, first_svid):
    reader, writer = await asyncio.open_connection('127.0.0.1', port)
    async def exchange(stype, stream, function, system, body=b''):
        session = 65535 if stype else 0
        header = struct.pack('>HBBBBI', session, stream | (128 if not stype else 0), function, 0, stype, system)
        packet = struct.pack('>I', 10 + len(body)) + header + body
        # Exercise fragmented network delivery, including the length prefix.
        writer.write(packet[:2]); await writer.drain()
        await asyncio.sleep(0.01)
        writer.write(packet[2:]); await writer.drain()
        size = struct.unpack('>I', await reader.readexactly(4))[0]
        reply = await reader.readexactly(size)
        assert struct.unpack('>I', reply[6:10])[0] == system
        return reply
    reply = await exchange(1, 0, 0, 10)
    assert reply[5] == 2 and reply[3] == 0
    reply = await exchange(0, 1, 13, 11, b'\x01\0')
    assert reply[3] == 14
    item = b'\x01\x01\xb1\x04' + struct.pack('>I', first_svid)
    reply = await exchange(0, 1, 3, 12, item)
    assert reply[3] == 4 and reply[10:14] == b'\x01\x01\x81\x08', reply.hex()
    writer.close(); await writer.wait_closed()

async def live_check(port, controller):
    """SignalR JSON protocol over supported long polling, no external Python packages."""
    base = f'http://127.0.0.1:{port}'
    def request(path, method='GET', body=None):
        req = urllib.request.Request(base + path, data=body, method=method, headers={'X-Api-Key': 'test-key', 'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.read()
    negotiate = json.loads(await asyncio.to_thread(request, '/live/negotiate?negotiateVersion=1', 'POST', b''))
    path = '/live?id=' + negotiate['connectionToken']
    await asyncio.to_thread(request, path)
    await asyncio.to_thread(request, path, 'POST', b'{"protocol":"json","version":1}\x1e')
    await asyncio.to_thread(request, path, 'POST', json.dumps({'type': 1, 'invocationId': '1', 'target': 'Subscribe', 'arguments': [controller]}).encode() + b'\x1e')
    until = time.monotonic() + 10
    while time.monotonic() < until:
        payload = await asyncio.to_thread(request, path)
        for message in payload.split(b'\x1e'):
            if not message: continue
            parsed = json.loads(message)
            if parsed.get('target') == 'samples':
                samples = parsed['arguments'][0]
                assert samples and samples[0]['controllerId'] == controller and samples[0]['quality'] == 'Good'
                await asyncio.to_thread(request, path, 'DELETE')
                return
    raise AssertionError('No live committed sample received')

async def main(args):
    loop = asyncio.get_running_loop()
    transport, fins = await loop.create_datagram_endpoint(FinsSimulator, local_addr=('127.0.0.1', 0))
    fins_port = transport.get_extra_info('sockname')[1]
    cip = await asyncio.start_server(cip_server, '127.0.0.1', 0)
    cip_port = cip.sockets[0].getsockname()[1]
    api_port, secs_port = free_port(), free_port()
    with tempfile.TemporaryDirectory(prefix='gateway-smoke-') as temp:
        controllers = []
        for i in range(args.controllers):
            is_fins = i % 2 == 0
            controllers.append({'Id': f'plc{i}', 'Host': '127.0.0.1', 'Port': fins_port if is_fins else cip_port,
                'Protocol': 'FinsUdp' if is_fins else 'EtherNetIp', 'PollIntervalMs': args.interval, 'TimeoutMs': 3000,
                'Tags': [{'Id': f't{j}', 'Address': f'D:{j}' if is_fins else f'Tag{j}', 'Type': 'UInt16' if is_fins else 'Float32',
                          'Svid': i * args.tags + j + 1} for j in range(args.tags)]})
        conf = {'Gateway': {'Controllers': controllers, 'DatabasePath': str(Path(temp)/'archive.db'), 'QueueCapacity': 100000,
                           'Secs': {'Enabled': True, 'Port': secs_port}}}
        if args.postgres:
            conf['Gateway']['ArchiveProvider'] = 'Postgres'
        path = Path(temp)/'config.json'; path.write_text(json.dumps(conf))
        env = os.environ.copy(); env.update({'GatewayConfig': str(path), 'GatewayApiKey': 'test-key', 'Urls': f'http://127.0.0.1:{api_port}'})
        output = open(Path(temp)/'gateway.log', 'w+')
        process = subprocess.Popen(['dotnet', str(ROOT/'src/Gateway/bin/Debug/net8.0/Gateway.dll')], cwd=ROOT/'src/Gateway', env=env, stdout=output, stderr=subprocess.STDOUT)
        started = time.monotonic()
        try:
            until = started + args.timeout
            status = None
            while time.monotonic() < until:
                if process.poll() is not None: raise AssertionError(f'Gateway exited {process.returncode}')
                try:
                    status = await asyncio.to_thread(http_json, api_port, '/api/status')
                    if len(status) == args.controllers and all(c['completedScans'] >= 1 for c in status): break
                except (OSError, ValueError):
                    pass
                await asyncio.sleep(0.2)
            else: raise AssertionError('Controllers did not complete their first scan')
            assert all(c['failedReads'] == 0 for c in status), status[:2]
            latest = await asyncio.to_thread(http_json, api_port, '/api/latest')
            assert len(latest) == args.controllers * args.tags, len(latest)
            assert all(s['quality'] == 'Good' for s in latest)
            assert next(s for s in latest if s['controllerId']=='plc0' and s['tagId']=='t0')['value'] == 0
            if args.controllers > 1:
                assert math.isclose(next(s for s in latest if s['controllerId']=='plc1' and s['tagId']=='t0')['value'], 20)
            now = int(time.time()*1000)
            history = await asyncio.to_thread(http_json, api_port, f'/api/history/plc0/t0?from={now-60000}&to={now+10000}&limit=10')
            assert history['samples'] and history['samples'][0]['value'] == 0
            trend = await asyncio.to_thread(http_json, api_port, f'/api/trend/plc0/t0?from={now-60000}&to={now+10000}&bucketMs=1000')
            assert sum(b['goodCount'] for b in trend) > 0
            try:
                await asyncio.to_thread(http_json, api_port, '/api/latest', 'wrong')
                raise AssertionError('Invalid API key accepted')
            except urllib.error.HTTPError as e:
                assert e.code == 401
            await asyncio.wait_for(hsms_check(secs_port, 1), 10)
            await live_check(api_port, 'plc0')
            health = await asyncio.to_thread(http_json, api_port, '/health')
            assert health['archiveError'] is None
            # Disconnect simulators and verify that timeouts become Bad samples, not stale Good values.
            if args.controllers <= 2:
                transport.close()
                until = time.monotonic() + 10
                while time.monotonic() < until:
                    latest = await asyncio.to_thread(http_json, api_port, '/api/latest')
                    if any(s['controllerId']=='plc0' and s['quality']=='Bad' and s['value'] is None for s in latest): break
                    await asyncio.sleep(0.1)
                else: raise AssertionError('Disconnected PLC was not marked Bad')
            scans = [c['lastScanMs'] for c in status]
            report = {'result': 'PASS', 'controllers': args.controllers, 'tagsPerController': args.tags,
                  'tagCount': args.controllers*args.tags, 'elapsedSeconds': round(time.monotonic()-started, 2),
                  'archivedSamples': health['archivedSamples'], 'queueDepth': health['queueDepth'],
                  'firstScanMaxMs': round(max(scans), 2), 'finsReadRequests': fins.reads,
                  'checks': ['PLC reads', 'history', 'trend', 'API authentication', 'HSMS select/S1F13/S1F3', 'SignalR committed samples']}
        except BaseException:
            output.flush(); output.seek(0); print(output.read()[-12000:]); raise
        finally:
            process.terminate()
            try: await asyncio.to_thread(process.wait, 30)
            except subprocess.TimeoutExpired: process.kill(); await asyncio.to_thread(process.wait)
            output.close()
        assert process.returncode == 0, f'Gateway did not shut down cleanly: {process.returncode}'
        if not args.postgres:
            with sqlite3.connect(Path(temp)/'archive.db') as db:
                count = db.execute('SELECT COUNT(*) FROM (SELECT controller,tag FROM samples GROUP BY controller,tag)').fetchone()[0]
                assert count == args.controllers * args.tags, f'Only {count} tags durably archived'
            report['checks'].append('All configured tags durably archived after graceful shutdown')
        print(json.dumps(report, indent=2))
    transport.close(); cip.close(); await cip.wait_closed()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--controllers', type=int, default=2)
    parser.add_argument('--tags', type=int, default=700)
    parser.add_argument('--interval', type=int, default=1000)
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--postgres', action='store_true', help='Use Gateway__PostgresConnectionString from environment')
    args = parser.parse_args()
    assert 1 <= args.controllers <= 500 and 1 <= args.tags <= 10000
    asyncio.run(main(args))

"""Read-only wire simulators shared in behavior with the original gateway tests."""
import asyncio
import contextlib
import struct

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

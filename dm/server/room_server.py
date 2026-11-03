#!/usr/bin/env python3
# The room server for local play over the internet (the app's Online room, dm/host/hle_relay.c): players who enter the same room code share a virtual local
# network, as consoles in range do. Standard library only; run it anywhere with a public TCP port (Railway: dm/server/README.md), or on a PC for friends on one LAN.
#   python3 room_server.py            (port from $PORT, default 11450)
# Protocol: frames of u32 length (big-endian) + payload, over TCP (reliable and in order: the game's datagrams arrive as sent)
#   client → server  'H' u8 version, u8 code length, code, u8 name length, name      the room to join
#   server → client  'W' u32 virtual address, u32 mask                               welcome: this player's address (10.13.0.x)
#   both ways        'D' u32 source, u16 source port, u32 destination, u16 port, data   one datagram; a destination of x.x.255.255 / 255.255.255.255 goes to everyone else
#   server → client  'L' u32 address                                                 a member left
#   server → client  'F' reason                                                      refused (the room is full, a bad hello)
import asyncio, ipaddress, os, struct, sys, time

NET = 0x0A0D0000; MASK = 0xFFFF0000; MAX_MEMBERS = 8; MAX_FRAME = 70000; MAX_ROOMS = 2000
rooms = {}   # code → {address: writer}

def log(*a): print(time.strftime('%H:%M:%S'), *a, flush=True)

async def frame(r):
    n = struct.unpack('>I', await r.readexactly(4))[0]
    if n == 0 or n > MAX_FRAME: raise ValueError('bad frame length')
    return await r.readexactly(n)

def send(w, payload):
    try: w.write(struct.pack('>I', len(payload)) + payload)
    except Exception: pass

async def client(r, w):
    peer = w.get_extra_info('peername'); code = addr = None
    try:
        h = await asyncio.wait_for(frame(r), 15)
        if h[:1] != b'H' or len(h) < 4 or h[1] != 1: send(w, b'Fbad hello'); return
        cl = h[2]; code = h[3:3 + cl].decode('utf-8', 'replace').strip().lower(); nl = h[3 + cl]; name = h[4 + cl:4 + cl + nl].decode('utf-8', 'replace')
        if not code or len(code) > 32: send(w, b'Fbad room code'); return
        room = rooms.get(code)
        if room is None:
            if len(rooms) >= MAX_ROOMS: send(w, b'Fthe server is full'); return
            room = rooms[code] = {}
        if len(room) >= MAX_MEMBERS: send(w, b'Fthe room is full'); return
        addr = next(NET | k for k in range(2, 255) if NET | k not in room)
        room[addr] = w
        send(w, b'W' + struct.pack('>II', addr, MASK))
        log(f'{peer[0]} "{name}" joined room {code!r} as {ipaddress.IPv4Address(addr)} ({len(room)} in it)')
        while True:
            f = await frame(r)
            if f[:1] != b'D' or len(f) < 13: continue
            src, sp, dst, dp = struct.unpack('>IHIH', f[1:13])
            if src != addr: continue   # a member sends only as itself
            if dst & 0xFFFF == 0xFFFF or dst == 0xFFFFFFFF:
                for a, o in list(room.items()):
                    if a != addr: send(o, f)
            elif dst in room: send(room[dst], f)
    except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError, ValueError, OSError): pass
    finally:
        if code is not None and addr is not None and code in rooms and rooms[code].get(addr) is w:
            del rooms[code][addr]
            for o in rooms[code].values(): send(o, b'L' + struct.pack('>I', addr))
            log(f'{ipaddress.IPv4Address(addr)} left room {code!r} ({len(rooms[code])} in it)')
            if not rooms[code]: del rooms[code]
        try: w.close()
        except Exception: pass

async def main():
    port = int(os.environ.get('PORT', sys.argv[1] if len(sys.argv) > 1 else 11450))
    srv = await asyncio.start_server(client, '0.0.0.0', port)
    log(f'room server on port {port}')
    async with srv: await srv.serve_forever()

if __name__ == '__main__': asyncio.run(main())

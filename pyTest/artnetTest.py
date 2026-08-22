#!/usr/bin/env python3
"""
Art-Net probe for moduleBox.

Два режима:

    python artnetTest.py poll [sec] [ip]            - разослать ArtPoll и распечатать
                                                      все поля каждого ArtPollReply;
                                                      с указанным ip опрос идёт юникастом
                                                      (нужно, когда броадкаст уходит в VPN
                                                      или маски у ноды и ПК не совпадают)
    python artnetTest.py dmx <ip> <universe> [sec]  - гнать ArtDMX 44 Гц бегущей волной,
                                                      чтобы проверить приём на устройстве

Ничего кроме стандартной библиотеки не требуется.
"""

import socket
import struct
import sys
import time

ARTNET_PORT = 6454
ARTNET_ID = b"Art-Net\x00"

OP_POLL = 0x2000
OP_POLLREPLY = 0x2100
OP_DMX = 0x5000


def make_poll():
    pkt = bytearray(14)
    pkt[0:8] = ARTNET_ID
    struct.pack_into("<H", pkt, 8, OP_POLL)     # OpCode - младший байт первым
    struct.pack_into(">H", pkt, 10, 14)         # ProtVer - старший первым
    pkt[12] = 0x02                              # TalkToMe: слать ответ при изменениях
    pkt[13] = 0x00                              # Priority
    return bytes(pkt)


def make_dmx(universe, seq, data):
    pkt = bytearray(18 + len(data))
    pkt[0:8] = ARTNET_ID
    struct.pack_into("<H", pkt, 8, OP_DMX)
    struct.pack_into(">H", pkt, 10, 14)
    pkt[12] = seq
    pkt[13] = 0                                 # Physical
    pkt[14] = universe & 0xFF                   # SubUni
    pkt[15] = (universe >> 8) & 0x7F            # Net
    struct.pack_into(">H", pkt, 16, len(data))
    pkt[18:] = data
    return bytes(pkt)


def cstr(raw):
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace")


def show_reply(addr, pkt):
    if len(pkt) < 207:
        print(f"  {addr[0]}: слишком короткий ArtPollReply ({len(pkt)} байт)")
        return

    ip = ".".join(str(b) for b in pkt[10:14])
    port = struct.unpack_from("<H", pkt, 14)[0]
    ver = f"{pkt[16]}.{pkt[17]}"
    net_sw, sub_sw = pkt[18], pkt[19]
    oem = struct.unpack_from(">H", pkt, 20)[0]
    status1 = pkt[23]
    esta = pkt[24] | (pkt[25] << 8)
    short_name = cstr(pkt[26:44])
    long_name = cstr(pkt[44:108])
    node_report = cstr(pkt[108:172])
    num_ports = struct.unpack_from(">H", pkt, 172)[0]
    port_types = pkt[174:178]
    good_output = pkt[182:186]
    sw_out = pkt[190:194]
    style = pkt[200]
    mac = ":".join(f"{b:02X}" for b in pkt[201:207])
    bind_index = pkt[211] if len(pkt) > 211 else 0
    status2 = pkt[212] if len(pkt) > 212 else 0

    print(f"\n=== ArtPollReply from {addr[0]}:{addr[1]} ({len(pkt)} байт) ===")
    print(f"  IP / Port     : {ip}:{port}")
    print(f"  ShortName     : '{short_name}'      <- имя в списке нод")
    print(f"  LongName      : '{long_name}'")
    print(f"  NodeReport    : '{node_report}'")
    print(f"  VersInfo      : {ver}")
    print(f"  Oem / EstaMan : 0x{oem:04X} / 0x{esta:04X}")
    print(f"  Status1/2     : 0x{status1:02X} / 0x{status2:02X}")
    print(f"  Style         : 0x{style:02X} ({'StNode' if style == 0 else '?'})")
    print(f"  MAC           : {mac}")
    print(f"  BindIndex     : {bind_index}")
    print(f"  NumPorts      : {num_ports}   NetSwitch:{net_sw} SubSwitch:{sub_sw}")

    for i in range(min(num_ports, 4)):
        universe = (net_sw << 8) | (sub_sw << 4) | (sw_out[i] & 0x0F)
        direction = "output" if port_types[i] & 0x80 else ("input" if port_types[i] & 0x40 else "-")
        alive = "поток есть" if good_output[i] & 0x80 else "тишина"
        print(f"    port {i}: universe {universe} ({direction}), {alive}")


def local_ipv4s():
    """Все локальные IPv4, кроме loopback. Нужно, чтобы разослать ArtPoll с
    каждого интерфейса: при поднятом VPN обычный broadcast уходит в туннель."""
    found = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.add(info[4][0])
    except socket.gaierror:
        pass
    return sorted(a for a in found if not a.startswith("127."))


def do_poll(timeout=3.0, target=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", ARTNET_PORT))
    sock.settimeout(0.3)

    poll = make_poll()

    if target:
        sock.sendto(poll, (target, ARTNET_PORT))
        print(f"ArtPoll -> {target} (юникаст)")
    else:
        for src in local_ipv4s():
            try:
                out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                out.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                out.bind((src, 0))
                out.sendto(poll, ("255.255.255.255", ARTNET_PORT))
                out.close()
                print(f"ArtPoll -> 255.255.255.255 через {src}")
            except OSError as e:
                print(f"ArtPoll через {src} не ушёл: {e}")

    print(f"Слушаю ответы {timeout:.0f} с...")

    seen = set()
    deadline = time.time() + timeout

    while time.time() < deadline:
        try:
            pkt, addr = sock.recvfrom(1024)
        except socket.timeout:
            continue

        if len(pkt) < 10 or pkt[0:8] != ARTNET_ID:
            continue

        if struct.unpack_from("<H", pkt, 8)[0] != OP_POLLREPLY:
            continue

        key = (addr[0], pkt[211] if len(pkt) > 211 else 0)
        if key in seen:
            continue
        seen.add(key)
        show_reply(addr, pkt)

    print(f"\nНайдено ответов: {len(seen)}")
    return 0 if seen else 1


def do_dmx(ip, universe, seconds):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 1
    frames = 0
    end = time.time() + seconds

    print(f"ArtDMX -> {ip} universe {universe}, {seconds} с при 44 Гц")

    while time.time() < end:
        phase = frames % 256
        data = bytes((phase + i) % 256 for i in range(512))
        sock.sendto(make_dmx(universe, seq, data), (ip, ARTNET_PORT))
        seq = 1 if seq == 255 else seq + 1
        frames += 1
        time.sleep(1.0 / 44)

    print(f"Отправлено кадров: {frames}")
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("poll", "dmx"):
        print(__doc__)
        return 2

    if sys.argv[1] == "poll":
        return do_poll(float(sys.argv[2]) if len(sys.argv) > 2 else 3.0,
                       sys.argv[3] if len(sys.argv) > 3 else None)

    if len(sys.argv) < 4:
        print(__doc__)
        return 2

    return do_dmx(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 10)


if __name__ == "__main__":
    sys.exit(main())

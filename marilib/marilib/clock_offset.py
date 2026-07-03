"""Measure the wall-clock offset between two machines (SNTP-style).

Cross-machine experiment timing (e.g. a node's NODE_LEFT on one gateway's
computer vs its NODE_JOINED on another's) is only trustworthy if the two
machines' wall clocks are aligned. Rather than *assume* they are, measure it:
run this at the START and END of a run and record both offsets, so any skew
(and any drift between start and end) is known for that run.

Run the reference on one machine (e.g. the cloud host):
    mari-clock-offset server [--port 9999]
and the client on each other machine (each edge PC):
    mari-clock-offset client <server_host> [--port 9999] [--samples 200]

The client prints the median offset (server_clock - client_clock) using the
NTP formula offset = ((t2-t1)+(t3-t4))/2, which cancels the symmetric network
delay. To put SERVER-clock timestamps onto the CLIENT timeline, subtract that
offset from them (and vice versa). Direct UDP is used (not the MQTT broker) so
the path is short and roughly symmetric - do it on the same LAN as the run.
"""

import argparse
import socket
import statistics
import struct
import sys
import time

_MAGIC = b"MCLK"
_REPLY_FMT = "!qq"  # t2, t3 (server receive/send, microseconds)


def _now_us() -> int:
    """Wall-clock microseconds (not monotonic: we want the cross-machine offset)."""
    return time.time_ns() // 1000


def run_server(port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"clock-offset server on udp/{port} (Ctrl-C to stop)")
    while True:
        data, addr = sock.recvfrom(64)
        t2 = _now_us()
        if len(data) < 12 or not data.startswith(_MAGIC):
            continue
        seq = data[4:12]  # opaque, echoed back so late/duplicate replies are ignorable
        t3 = _now_us()
        sock.sendto(_MAGIC + seq + struct.pack(_REPLY_FMT, t2, t3), addr)


def run_client(host: str, port: int, samples: int, interval: float) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    offsets_us: list[float] = []
    delays_us: list[float] = []
    for i in range(samples):
        t1 = _now_us()
        sock.sendto(_MAGIC + struct.pack("!q", i), (host, port))
        try:
            data, _ = sock.recvfrom(64)
        except socket.timeout:
            continue
        t4 = _now_us()
        if len(data) < 28 or not data.startswith(_MAGIC) or data[4:12] != struct.pack("!q", i):
            continue
        t2, t3 = struct.unpack(_REPLY_FMT, data[12:28])
        offsets_us.append(((t2 - t1) + (t3 - t4)) / 2.0)
        delays_us.append((t4 - t1) - (t3 - t2))
        time.sleep(interval)

    if not offsets_us:
        print("no replies - check host / port / firewall", file=sys.stderr)
        sys.exit(1)

    off_ms = statistics.median(offsets_us) / 1000.0
    off_std_ms = (statistics.pstdev(offsets_us) / 1000.0) if len(offsets_us) > 1 else 0.0
    rtt_ms = statistics.median(delays_us) / 1000.0
    print(
        f"samples={len(offsets_us)}/{samples}  "
        f"offset(server-client)={off_ms:+.3f} ms (std {off_std_ms:.3f})  rtt~{rtt_ms:.3f} ms"
    )
    print(
        f"  -> to move server-clock timestamps onto the client timeline, "
        f"subtract {off_ms:+.3f} ms from them"
    )


def main() -> None:
    parser = argparse.ArgumentParser(prog="mari-clock-offset", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)

    ps = sub.add_parser("server", help="run the reference (responder)")
    ps.add_argument("--port", type=int, default=9999)

    pc = sub.add_parser("client", help="measure offset against a running server")
    pc.add_argument("host", help="server host/IP")
    pc.add_argument("--port", type=int, default=9999)
    pc.add_argument("--samples", type=int, default=200)
    pc.add_argument("--interval", type=float, default=0.01, help="seconds between samples")

    args = parser.parse_args()
    if args.mode == "server":
        run_server(args.port)
    else:
        run_client(args.host, args.port, args.samples, args.interval)


if __name__ == "__main__":
    main()

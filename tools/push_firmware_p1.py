#!/usr/bin/env python3
"""Push a signed firmware to a SenseCAP P1-Pro over the air -- no cable.

    tools/push_firmware_p1.py --gateway 192.168.178.133 --to X33ESX \
        --version 0.1.1 --hex models/sensecap-p1-pro/firmware/.pio/build/p1pro/firmware.hex \
        --fw-nsec ~/.xprs/fw.nsec --owner-nsec ~/.xprs/owner.nsec --from X38364 \
        [--bearer ble|lora] [--rate 4]

The same two signatures as tools/push_firmware.sh (XPRS.md 25.8), because
the station checks the same things the ESP32 boards check:

    the APPROVAL  sign_firmware.dart, the publisher key the station pins as
                  `fwkey`: a signature over "xprsfw1 sensecap-p1-pro <version>
                  <size> <sha256>". Nothing installs without it.
    the AUTH      sign_command.dart, an owner key from the station's own1..own4:
                  a signed `cmd:update ver: size: sha:` that opens the session.

What differs is the road. The P1-Pro has no HTTP, so the image travels as
XPRS packets through a gateway station that has both a LAN and a radio (a
T-Deck, a T-Dongle): POST /api/xprs/send puts each one on the bearer named,
and the P1-Pro's answers come back through the gateway's history. 160 bytes
a packet, base85, ~1000 packets for a 160 KB image; `cmd:zfwq` asks the
station which ones it is still missing and only those go again.

Over BLE this is a few minutes. Over LoRa it is a long evening and most of
the band's duty cycle -- possible, and the reason the station tolerates a
session idling for half an hour.
"""
import argparse, hashlib, json, os, re, subprocess, sys, time, urllib.request, urllib.parse

B85 = ("0123456789abcdefghijklmnopqrstuvwxyz"
       "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-+=^!/*?&<>()[]%$#@,;_")
CHUNK = 128


def b85(data: bytes) -> str:
    assert len(data) % 4 == 0
    out = []
    for i in range(0, len(data), 4):
        v = int.from_bytes(data[i:i + 4], "big")
        d = []
        for _ in range(5):
            d.append(B85[v % 85]); v //= 85
        out.append("".join(reversed(d)))
    return "".join(out)


def http(url, data=None, timeout=15):
    req = urllib.request.Request(url, data=data.encode() if data else None,
                                 headers={"Content-Type": "text/plain"} if data else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


def send(gw, wire, bearer):
    """One packet to the station. Over --gatt: a line into the probe's pipe,
    which forwards it as one GATT frame on the private 1:1 link
    (docs/ble5-gatt.md) -- no gateway, no broadcast plane. Otherwise the LAN
    gateway's /api/xprs/send; a rebooting gateway is waited for."""
    if GATT is not None:
        GATT.write((wire + "\n").encode()); GATT.flush()
        return None
    deadline = time.time() + 120
    while True:
        try:
            r = json.loads(http(f"http://{gw}/api/xprs/send",
                                json.dumps({"wire": wire, "bearer": bearer})))
            if not r.get("ok"):
                sys.exit(f"gateway refused: {r}")
            return r["id"]
        except SystemExit:
            raise
        except Exception as e:
            if time.time() > deadline:
                sys.exit(f"gateway {gw} not answering: {e}")
            print(f"  gateway not answering ({e.__class__.__name__}) -- waiting"); time.sleep(5)


LISTEN = None      # a serial.Serial on a station's console, or None
LISTEN_BUF = ""
GATT = None        # a serial.Serial on the probe's pipe (docs/ble5-gatt.md), or None
GATT_BUF = ""


def results(gw, station, rid, since_wires):
    """t:result wires from [station] carrying r:[rid], not seen before.

    From the gateway's history, or -- with --listen -- off the console of a
    station on USB (a T-Dongle under the pole): every wire it hears is a
    log line, and the answer is in there."""
    global LISTEN_BUF, GATT_BUF
    rows = []
    if GATT is not None:
        try: GATT_BUF += GATT.read(65536).decode("utf-8", "replace")
        except Exception: pass
        GATT_BUF = GATT_BUF[-200000:]
        parts = GATT_BUF.split("\n"); GATT_BUF = parts.pop()
        for ln in parts:
            ln = ln.strip()
            if ln.startswith("RX>"): rows.append(ln[3:].strip())
        out = []
        for w in rows:
            if w in since_wires: continue
            if rid is None or f" r:{rid} " in w + " ":
                since_wires.add(w); out.append(w)
        return out
    if LISTEN is not None:
        try:
            LISTEN_BUF += LISTEN.read(65536).decode("utf-8", "replace")
        except Exception:
            pass
        LISTEN_BUF = LISTEN_BUF[-200000:]
        clean = re.sub(r"\x1b\[[0-9;]*m", "", LISTEN_BUF)
        rows += re.findall(r"(t:result f:%s [^\r\n]*)" % re.escape(station), clean)
        rows = [re.sub(r"\s*\[0m$", "", r) for r in rows]
        if gw is None:
            out = []
            for w in rows:
                if f" r:{rid} " in w + " " and w not in since_wires:
                    since_wires.add(w); out.append(w)
            return out
    if LISTEN is None and GATT is None and gw:
        for url in (f"http://{gw}/api/xprs/history?only=result&call={station}&limit=40",
                    f"http://{gw}/api/xprs?type=result&from={station}&limit=40&recent=1"):
            try:
                rows += re.findall(r'"wire":"([^"]*)"', http(url, timeout=8))
            except Exception:
                pass
    out = []
    for w in rows:
        if f" r:{rid} " in w + " " and w not in since_wires:
            since_wires.add(w); out.append(w)
    return out


def wait_result(gw, station, rid, timeout, seen):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for w in results(gw, station, rid, seen):
            m = re.search(r" code:(\d+)", w)
            body = re.sub(r"^t:result f:\S+ d:\S+ (ts:\S+ )?r:\S+ ", "", w)
            body = re.sub(r" sig:\S+", "", body)
            print(f"  {station}: {body}")
            if m:
                return int(m.group(1)), body
        time.sleep(2)
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gateway", help="LAN gateway station (omit when using --gatt)")
    ap.add_argument("--to", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--hex", help="firmware.hex from the PlatformIO build")
    ap.add_argument("--bin", help="or the raw image")
    ap.add_argument("--fw-nsec", required=True)
    ap.add_argument("--owner-nsec", required=True)
    ap.add_argument("--from", dest="frm", required=True)
    ap.add_argument("--bearer", default="ble")
    ap.add_argument("--rate", type=float, default=4.0, help="packets per second")
    ap.add_argument("--flutter", default=os.environ.get("XPRS_FLUTTER",
                    os.path.join(os.path.dirname(__file__), "..", "..", "app")))
    ap.add_argument("--board", default="sensecap-p1-pro")
    ap.add_argument("--listen", help="serial port of a station on USB whose console shows what it hears")
    ap.add_argument("--gatt", help="serial port of a tinynimble_probe in pipe mode: the image goes over a private "
                                   "1:1 GATT link to a station that has dialled the probe, not the broadcast plane")
    a = ap.parse_args()
    global LISTEN, GATT
    if a.listen:
        import serial
        LISTEN = serial.Serial(a.listen, 115200, timeout=0)
    if a.gatt:
        import serial
        GATT = serial.Serial(a.gatt, 115200, timeout=0)
        GATT.write(b"P\n"); GATT.flush(); time.sleep(0.3)

    if a.bin:
        image = open(a.bin, "rb").read()
    elif a.hex:
        oc = os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-objcopy")
        binpath = a.hex[:-4] + ".bin"
        subprocess.check_call([oc, "-I", "ihex", "-O", "binary", a.hex, binpath])
        image = open(binpath, "rb").read()
        a.bin = binpath
    else:
        sys.exit("need --hex or --bin")
    if a.version.encode() not in image:
        sys.exit(f"the image does not embed version '{a.version}' -- is version.txt right?")
    size, sha = len(image), hashlib.sha256(image).hexdigest()
    print(f"image {size} B sha256 {sha}")

    # The approval, by the publisher key.
    out = subprocess.check_output(["dart", "run", "tool/sign_firmware.dart", "--board", a.board,
                                   "--version", a.version, "--bin", os.path.abspath(a.bin),
                                   "--nsec-file", os.path.abspath(os.path.expanduser(a.fw_nsec))],
                                  cwd=a.flutter, text=True)
    m = re.search(r'"sig":\s*"([^"]{60})"', out)
    if not m:
        sys.exit("no signature in sign_firmware.dart output:\n" + out)
    approval = m.group(1)
    print(f"approval {approval}")

    # The door, by an owner key.
    wire = subprocess.check_output(["dart", "run", "tool/sign_command.dart", "--to", a.to, "--cmd", "update",
                                    "--from", a.frm, "--nsec-file", os.path.abspath(os.path.expanduser(a.owner_nsec)),
                                    f"ver={a.version}", f"size={size}", f"sha={sha}"],
                                   cwd=a.flutter, text=True).strip()
    print(f"ask  {wire}")
    seen = set()
    rid = send(a.gateway, wire, a.bearer)
    code, body = wait_result(a.gateway, a.to, rid, 60, seen)
    if code == 200:
        print("already running that version"); return
    if code != 202:
        sys.exit(f"station answered {code}: {body}")

    # The image, then the approval. Padded to a word: the station writes
    # words and hashes the first <size> bytes.
    padded = image + b"\xff" * (-len(image) % 4)
    chunks = [padded[i:i + CHUNK] for i in range(0, len(padded), CHUNK)]
    print(f"sending {len(chunks)} chunks on {a.bearer} at {a.rate}/s")

    def send_chunk(i):
        send(a.gateway, f"t:command f:{a.frm} d:{a.to} cmd:zfw n:{i} m:{b85(chunks[i])}", a.bearer)
        time.sleep(1.0 / a.rate)

    t0 = time.time()
    for i in range(len(chunks)):
        send_chunk(i)
        if (i + 1) % 100 == 0:
            print(f"  {i + 1}/{len(chunks)} ({time.time() - t0:.0f}s)")
    send(a.gateway, f"t:command f:{a.frm} d:{a.to} cmd:zfwsig m:{approval}", a.bearer)

    # What did not arrive, until nothing is missing.
    for round_ in range(20):
        time.sleep(3)
        qid = send(a.gateway, f"t:command f:{a.frm} d:{a.to} cmd:zfwq", a.bearer)
        code, body = wait_result(a.gateway, a.to, qid, 30, seen)
        if code in (200, 202):
            break
        if code != 206 or not body:
            print("  no answer to zfwq -- asking again"); continue
        missing = []
        for part in body.replace("m:", "").split(","):
            part = part.strip()
            if "-" in part:
                lo, hi = part.split("-"); missing += range(int(lo), int(hi) + 1)
            elif part.isdigit():
                missing.append(int(part))
        print(f"  resending {len(missing)} chunks")
        for i in missing:
            send_chunk(i)
        if body.startswith("complete, send zfwsig") or code == 200:
            send(a.gateway, f"t:command f:{a.frm} d:{a.to} cmd:zfwsig m:{approval}", a.bearer)
    else:
        sys.exit("gave up: the station still reports missing chunks")

    # The install answer comes on the update command's id.
    code, body = wait_result(a.gateway, a.to, rid, 90, seen)
    print(f"done in {time.time() - t0:.0f}s: {code} {body}")


if __name__ == "__main__":
    main()

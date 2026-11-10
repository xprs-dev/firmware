#!/usr/bin/env python3
"""Collections (gallery, music, library rooms) against a running station.

    tools/qemu_c3.sh run 1800 &          # or any station with a card
    python3 tools/qemu_collections_test.py http://127.0.0.1:18080 [kinds] [--keep]

[kinds] is a comma list (default gallery,music,library). The keys are fixed
and a room of the kind already there is used again, so after the emulator
wedges (docs/esp32.md, "The C3 under QEMU": after a flash erase its
interrupt state goes wrong) a rerun on the same image carries on; --keep
leaves the rooms and the administrator for that rerun.

What the web chat page does with a collection room (spec/API-HTTP.md "A
gallery room", core test group 'music and library rooms'), driven over HTTP
with two visitor keys of its own, signed with the page's crypto
(../portal/contract/sign.js):

  alice claims the station (the first local visitor may), creates a gallery,
  a music and a library room, opens a folder in each (t:blog title:),
  stores a file as staff (?session=, the station's own: own/ on the card),
  posts the item (t:message r:<folder>) and its t:file companion, and reads
  the file back whole, by Range (206, the bytes match), past its end (416)
  and by HEAD; bob likes the item (200) and may not add one (403).

Without --keep, at the end it closes its rooms and releases the station, so the portal's
conformance suite (which needs a station with no administrator) and this
script can be run again on the same image. Exit status: the failures.
"""
import base64
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SIGN = os.path.join(HERE, '..', '..', 'portal', 'contract', 'sign.js')
ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
BASE = ARGS[0].rstrip('/') if ARGS else 'http://127.0.0.1:18080'
KINDS = [k for k in ARGS[1].split(',') if k] if len(ARGS) > 1 else ['gallery', 'music', 'library']
KEEP = '--keep' in sys.argv
fails = 0
passes = 0


def check(ok, what, extra=''):
    global fails, passes
    print(('  [PASS] ' if ok else '  [FAIL] ') + what + (('  -- ' + str(extra)[:300]) if (extra and not ok) else ''))
    if ok:
        passes += 1
    else:
        fails += 1


def node(*args):
    return subprocess.check_output(['node', SIGN] + list(args), text=True)


def ts(plus=0):
    return time.strftime('%Y-%m-%d_%H:%M:%S', time.gmtime(time.time() + plus))


def request(method, path, body=None, query='', headers=None, raw=False):
    """(status, body or parsed JSON, response headers). The emulated C3
    verifies a signature in seconds, one request at a time: patient, and
    asks again when its backlog drops a connection."""
    url = BASE + path + (('?' + query) if query else '')
    data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
    for attempt in range(6):
        req = urllib.request.Request(url, data=data, method=method)
        if data is not None and not isinstance(body, bytes):
            req.add_header('Content-Type', 'application/json')
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                b = r.read()
                return r.status, (b if raw else json.loads(b.decode() or '{}')), r.headers
        except urllib.error.HTTPError as e:
            b = e.read()
            try:
                return e.code, (b if raw else json.loads(b.decode() or '{}')), e.headers
            except Exception:
                return e.code, {}, e.headers
        except (ConnectionResetError, ConnectionRefusedError, urllib.error.URLError, TimeoutError, OSError) as e:
            if attempt == 5:
                raise
            print('  (%s %s: %s; asking again)' % (method, path, e), file=sys.stderr)
            time.sleep(3 + 3 * attempt)


def http(method, path, body=None, query=''):
    st, d, _ = request(method, path, body, query)
    return st, d


class Person:
    def __init__(self, priv):
        self.priv, self.session = priv, None
        self.npub = node('--npub', priv)
        self.call = 'X1' + self.npub[5:9].upper()

    def wire(self, canonical):
        return node(self.priv, canonical)

    def send(self, canonical):
        return http('POST', '/api/xprs/send', {'wire': self.wire(canonical)})

    def prove(self):
        self.send('t:identity f:%s ts:%s k:%s' % (self.call, ts(), self.npub))
        for _ in range(12):
            st, d = http('GET', '/api/session')
            st, d2 = http('POST', '/api/session', {'wire': self.wire('t:session f:%s ts:%s m:%s' % (self.call, ts(), d['nonce']))})
            if st == 409:
                time.sleep(3)
                self.send('t:identity f:%s ts:%s k:%s' % (self.call, ts(), self.npub))
                continue
            if st == 200:
                self.session = d2['session']
            return st
        return st


def ident(wire):
    return node('--id', wire)


def main():
    seed = int(time.time()) & 0xffff
    alice = Person('6a' * 32)
    bob = Person('7b' * 32)
    print('alice %s, bob %s, station %s' % (alice.call, bob.call, BASE))

    print('services')
    st, sv = http('GET', '/api/services')
    page = sv.get('page') or {}
    check(st == 200 and page.get('rooms') is True, 'the station hosts rooms', page)
    check(page.get('gallery') is True and page.get('storage') is True, 'and offers collections (page.gallery, page.storage)', page)

    print('the administrator')
    check(alice.prove() == 200, 'alice has a session')
    check(bob.prove() == 200, 'bob has a session')
    st, d = http('GET', '/api/admin', query='session=' + alice.session)
    if d.get('admin') and d.get('admin') != alice.call:
        print('  (the station has an administrator already: %s; this needs a station with none)' % d['admin'])
        sys.exit(2)
    if not d.get('you'):
        st, d = http('POST', '/api/admin', {'action': 'claim', 'session': alice.session})
        check(st == 200 and d.get('you') is True, 'alice claims the station from its network', d)
        if d.get('offer'):
            alice.send('t:moderate f:%s d:%s ts:%s r:%s accept:mod' % (alice.call, sv.get('station'), ts(), d['offer']))

    made = []
    folders = {'gallery': 'field-day', 'music': 'first-album', 'library': 'manuals'}
    for kind in KINDS:
        folder = folders[kind]
        print('%s room' % kind)
        st, d = http('GET', '/api/rooms', query='session=' + alice.session)
        there = [r for r in d.get('rooms', []) if r.get('kind') == kind]
        if there:
            print('  (a %s room is there from an earlier run: %s)' % (kind, there[0]['call']))
            d = {'room': there[0]['call'], 'rooms': there}
        else:
            st, d = http('POST', '/api/rooms', {'action': 'create', 'session': alice.session,
                                                'name': '%s %d' % (kind, seed), 'kind': kind})
            check(st == 200 and d.get('room', '').startswith('X5'), 'the administrator creates a %s room' % kind, d)
        room = d.get('room', '')
        if not room:
            continue
        made.append(room)
        mine = [r for r in d.get('rooms', []) if r['call'] == room]
        check(bool(mine) and mine[0].get('kind') == kind, 'listed with kind %s' % kind, mine)

        w = alice.wire('t:blog f:%s d:%s ts:%s title:%s m:the folder' % (alice.call, room, ts(), folder))
        st, d = http('POST', '/api/xprs/send', {'wire': w})
        check(st == 200, 'a folder: a titled post', d)
        fid = ident(w)

        data = os.urandom(3000) + kind.encode()
        sha = hashlib.sha256(data).digest()
        ext = {'gallery': 'jpg', 'music': 'mp3', 'library': 'pdf'}[kind]
        ref = base64.urlsafe_b64encode(sha).decode().rstrip('=') + '.' + ext
        st, d = http('POST', '/api/xprs/file/' + ref, data, query='session=' + alice.session)
        check(st == 200 and d.get('size') == len(data), 'the staff store the file (the station\'s own)', d)
        st, d = http('POST', '/api/xprs/file/' + ref, os.urandom(3000), query='session=' + alice.session)
        check(st == 400, 'bytes that do not hash to the reference are refused, staff or not', d)

        w = alice.wire('t:message f:%s d:%s ts:%s r:%s m:the item' % (alice.call, room, ts(1), fid))
        st, d = http('POST', '/api/xprs/send', {'wire': w})
        check(st == 200, 'the item: a message in the folder, no preview needed', d)
        iid = ident(w)
        st, d = alice.send('t:file f:%s d:%s ts:%s r:%s file:%s size:3KB name:item.%s' % (alice.call, room, ts(1), iid, ref, ext))
        check(st == 200, 'and its t:file companion names the file', d)

        st, body, h = request('GET', '/api/xprs/file/' + ref, raw=True)
        check(st == 200 and body == data and h.get('Accept-Ranges') == 'bytes', 'the file whole, ranges offered', (st, dict(h)))
        st, body, h = request('GET', '/api/xprs/file/' + ref, raw=True, headers={'Range': 'bytes=100-1099'})
        check(st == 206 and body == data[100:1100] and h.get('Content-Range') == 'bytes 100-1099/%d' % len(data),
              'bytes=100-1099: 206, those bytes', (st, dict(h)))
        st, body, h = request('GET', '/api/xprs/file/' + ref, raw=True, headers={'Range': 'bytes=-500'})
        check(st == 206 and body == data[-500:], 'bytes=-500: the last 500', (st, len(body)))
        st, body, h = request('GET', '/api/xprs/file/' + ref, raw=True, headers={'Range': 'bytes=2900-'})
        check(st == 206 and body == data[2900:], 'bytes=2900-: from there to the end', (st, len(body)))
        st, body, h = request('GET', '/api/xprs/file/' + ref, raw=True, headers={'Range': 'bytes=999999-'})
        check(st == 416 and h.get('Content-Range') == 'bytes */%d' % len(data), 'past the end: 416', (st, dict(h)))
        st, body, h = request('HEAD', '/api/xprs/file/' + ref, raw=True)
        check(st == 200 and h.get('Content-Length') == str(len(data)) and not body, 'HEAD: the length, no body', (st, dict(h)))

        st, d = bob.send('t:reaction f:%s d:%s ts:%s r:%s add:like' % (bob.call, room, ts(2), iid))
        check(st == 200, 'bob likes the item', d)
        st, d = bob.send('t:message f:%s d:%s ts:%s r:%s m:mine' % (bob.call, room, ts(2), fid))
        check(st == 403, 'bob may not add an item', d)
        st, d = bob.send('t:status f:%s d:%s ts:%s m:x' % (bob.call, room, ts(2)))
        check(st == 403 and 'folders, items and likes' in d.get('error', ''), 'a collection takes nothing else', d)

        # The archive files on its own schedule (a queue drained by its own
        # task): ask a few times, as for the history below.
        for _ in range(10):
            st, d = http('GET', '/api/rooms', query='topics=%s&session=%s' % (room, bob.session))
            if any(t.get('title') == folder for t in d.get('topics', [])):
                break
            time.sleep(2)
        check(st == 200 and any(t.get('title') == folder for t in d.get('topics', [])), 'the folder is in the room\'s index', d)
        rows = []
        for _ in range(10):
            st, h = http('GET', '/api/xprs/history', query='room=%s&kind=message,file,reaction&limit=50&session=%s' % (room, bob.session))
            rows = h.get('rows', [])
            if {'message', 'file', 'reaction'} <= {r.get('type') for r in rows}:
                break
            time.sleep(2)
        check({'message', 'file', 'reaction'} <= {r.get('type') for r in rows},
              'a reader\'s history of the room has the item, its file and the like', [r.get('type') for r in rows])

    if KEEP:
        print('%d passed, %d failed (rooms and administrator kept)' % (passes, fails))
        sys.exit(1 if fails else 0)
    print('tidy')
    for room in made:
        st, _ = http('POST', '/api/rooms', {'action': 'close', 'session': alice.session, 'call': room})
        check(st == 200, 'closed ' + room)
    st, d = http('POST', '/api/admin', {'action': 'release', 'session': alice.session})
    check(st == 200, 'the station released', d)
    print('%d passed, %d failed' % (passes, fails))
    sys.exit(1 if fails else 0)


main()

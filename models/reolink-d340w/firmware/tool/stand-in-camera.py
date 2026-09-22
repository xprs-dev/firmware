# A stand-in for the camera's own api.cgi, for working on the daemon without a
# camera (or without its password).
#
#   python3 tool/stand-in-camera.py &                 # answers on :8098
#   REOBELL_KEY=/tmp/bell.key REOBELL_API=http://127.0.0.1:8098 \
#   REOBELL_USER=admin REOBELL_PASS=sesame REOBELL_HTTP_PORT=8097 \
#     dart run bin/reobell.dart run 127.0.0.1
#   curl localhost:8098/press        # the button
#   curl localhost:8098/motion       # movement
#   curl localhost:8098/seen         # what the daemon asked of it
#
# It counts sessions the way the real one does (two, then "max session"), so a
# daemon that logs in per picture is caught here rather than on a doorstep.
#
# It answers Login, Logout, GetEvents and Snap the way
# docs/device-capabilities.md says the D340W does, including
# the session cap (it refuses a third login) and the token error.
#
# Control, for the test: /press, /motion, /calm, /seen, /expire
import http.server, json, os, pathlib, threading, urllib.parse

# Any JPEG will do; point at one with REOBELL_TEST_JPEG.
JPEG = pathlib.Path(os.environ.get("REOBELL_TEST_JPEG", "")) if os.environ.get(
    "REOBELL_TEST_JPEG") else pathlib.Path(__file__).with_name("still.jpg")
MAX_SESSIONS = 2
state = {"visitor": 0, "md": 0}
tokens = set()
seen = {"login": 0, "logout": 0, "events": 0, "snap": 0, "last_snap": "",
        "refused_over_cap": 0, "snap_without_token": 0}
lock = threading.Lock()

def body(h):
    n = int(h.headers.get('content-length', 0))
    return h.rfile.read(n) if n else b''

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, obj, ctype="application/json", raw=None):
        b = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("content-type", ctype)
        self.send_header("content-length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def q(self):
        return urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)

    def do_POST(self):
        q, raw = self.q(), body(self)
        cmd = (q.get("cmd") or [""])[0]
        with lock:
            if cmd == "Login":
                seen["login"] += 1
                try:
                    user = json.loads(raw)[0]["param"]["User"]
                except Exception:
                    return self.reply([{"cmd": "Login", "code": 1,
                                        "error": {"rspCode": -4}}])
                if user.get("password") != "sesame":
                    return self.reply([{"cmd": "Login", "code": 1,
                                        "error": {"detail": "login failed",
                                                  "rspCode": -7}}])
                if len(tokens) >= MAX_SESSIONS:
                    seen["refused_over_cap"] += 1
                    return self.reply([{"cmd": "Login", "code": 1,
                                        "error": {"detail": "max session",
                                                  "rspCode": -5}}])
                tok = "tok%d" % seen["login"]
                tokens.add(tok)
                return self.reply([{"cmd": "Login", "code": 0, "value":
                                    {"Token": {"leaseTime": 3600, "name": tok}}}])
            tok = (q.get("token") or [""])[0]
            if cmd == "Logout":
                seen["logout"] += 1
                tokens.discard(tok)
                return self.reply([{"cmd": "Logout", "code": 0, "value": {"rspCode": 200}}])
            if cmd == "GetEvents":
                seen["events"] += 1
                if tok not in tokens:
                    return self.reply([{"cmd": "GetEvents", "code": 1,
                                        "error": {"detail": "please login first",
                                                  "rspCode": -6}}])
                return self.reply([{"cmd": "GetEvents", "code": 0, "value": {
                    "channel": 0,
                    "visitor": {"alarm_state": state["visitor"], "support": 1},
                    "md": {"alarm_state": state["md"], "support": 1},
                    "ai": {"people": {"alarm_state": 0, "support": 1}}}}])
        self.send_error(404)

    def do_GET(self):
        q = self.q()
        cmd = (q.get("cmd") or [""])[0]
        p = urllib.parse.urlparse(self.path).path
        with lock:
            if cmd == "Snap":
                seen["snap"] += 1
                seen["last_snap"] = self.path
                tok = (q.get("token") or [""])[0]
                if tok not in tokens:
                    seen["snap_without_token"] += 1
                    return self.reply([{"cmd": "Snap", "code": 1,
                                        "error": {"rspCode": -6}}])
                return self.reply(None, "image/jpeg", JPEG.read_bytes())
            if p == "/press":
                state["visitor"] = 1
                threading.Timer(2.5, lambda: state.update(visitor=0)).start()
                return self.reply({"ok": True, "pressed": True})
            if p == "/motion":
                state["md"] = 1
                threading.Timer(3.0, lambda: state.update(md=0)).start()
                return self.reply({"ok": True, "motion": True})
            if p == "/calm":
                state["visitor"] = state["md"] = 0
                return self.reply({"ok": True})
            if p == "/expire":
                tokens.clear()
                return self.reply({"ok": True, "tokens": 0})
            if p == "/seen":
                return self.reply({**seen, "sessions": len(tokens), **state})
        self.send_error(404)

http.server.ThreadingHTTPServer(("127.0.0.1", 8098), H).serve_forever()

/// The camera, as the daemon on it sees it.
///
/// Two things live here: a session against the camera's own `api.cgi`, and the
/// small HTTP server that hands a still to whoever asks on the LAN.
///
/// ── Why one session, held ────────────────────────────────────────────────
///
/// The D340W allows only a few concurrent logins, there is no way to list or
/// force-expire one, and a leaked token is unusable for the rest of its lease
/// (docs/device-capabilities.md section 8). Exhausting them locks the
/// household out of its own doorbell, Reolink app included. So this holds
/// EXACTLY ONE login for the life of the process, renews it before the lease
/// ends, and hands it back on the way out. The shell poller it replaces logged
/// in again on every token error and leaked a lease on every restart.
///
/// ── Why the still is proxied ─────────────────────────────────────────────
///
/// XPRS.md 11.7.2: "The camera never puts its RTSP credentials in any URL it
/// hands out." The same goes for its API token. `/door/snapshot.jpg` is served
/// by this process, which calls `Snap` itself over loopback and streams the
/// bytes back; what a phone sees is a JPEG and no credential of any kind.
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

/// What the camera says is happening right now.
class CamEvents {
  const CamEvents({required this.visitor, required this.motion});
  final bool visitor; // the button, GetEvents visitor.alarm_state
  final bool motion; // md.alarm_state
}

/// One login against the camera's own API, held and renewed.
class CameraSession {
  CameraSession({
    required this.base,
    required this.user,
    required this.password,
    this.log,
  });

  /// Where api.cgi lives. On the camera itself this is loopback, which is the
  /// point: the credentials never cross a wire.
  final String base;
  final String user;
  final String password;
  final void Function(String line)? log;

  final HttpClient _http = HttpClient()
    ..connectionTimeout = const Duration(seconds: 8)
    ..idleTimeout = const Duration(seconds: 30);

  String? _token;
  DateTime _renewAt = DateTime.fromMillisecondsSinceEpoch(0);

  /// A refused login is not retried at once. The camera counts sessions and
  /// cannot be made to forget one, so a daemon that logs in every second with
  /// a password the owner mistyped is a daemon that locks the household out
  /// of its own doorbell. Backs off to a minute and stays there.
  DateTime _nextLoginAt = DateTime.fromMillisecondsSinceEpoch(0);
  Duration _backoff = const Duration(seconds: 2);
  static const Duration _backoffMax = Duration(seconds: 60);

  bool get haveSession => _token != null;

  void _say(String line) => log?.call(line);

  Uri _api(String cmd, {bool withToken = true, Map<String, String> extra = const {}}) {
    final q = <String, String>{'cmd': cmd, ...extra};
    if (withToken && _token != null) q['token'] = _token!;
    return Uri.parse('$base/cgi-bin/api.cgi').replace(queryParameters: q);
  }

  Future<List<int>?> _send(Uri uri, {String? body, Duration? timeout}) async {
    try {
      final req = body == null
          ? await _http.getUrl(uri)
          : await _http.postUrl(uri);
      if (body != null) {
        final bytes = utf8.encode(body);
        req.headers.contentType = ContentType.json;
        // Say how long it is. Without this Dart sends the body chunked, and
        // the camera's own cgi reads a content-length or nothing at all: the
        // login then fails with `rspCode -4`, an answer that looks like bad
        // credentials and is not.
        req.headers.contentLength = bytes.length;
        req.add(bytes);
      }
      final res = await req.close().timeout(timeout ?? const Duration(seconds: 15));
      final bytes = <int>[];
      await for (final chunk in res) {
        bytes.addAll(chunk);
        // A still off the substream is tens of kilobytes. Anything past this
        // is not a still and is not worth holding in a doorbell's memory.
        if (bytes.length > 4 * 1024 * 1024) {
          _say('api: answer over 4 MB, dropped');
          return null;
        }
      }
      if (res.statusCode < 200 || res.statusCode >= 300) {
        _say('api: ${uri.queryParameters['cmd']} answered ${res.statusCode}');
        return null;
      }
      return bytes;
    } on TimeoutException {
      _say('api: ${uri.queryParameters['cmd']} timed out');
      return null;
    } catch (e) {
      _say('api: ${uri.queryParameters['cmd']} failed: $e');
      return null;
    }
  }

  void _loginFailed() {
    _token = null;
    _nextLoginAt = DateTime.now().add(_backoff);
    _backoff = _backoff * 2 > _backoffMax ? _backoffMax : _backoff * 2;
  }

  /// Log in, once. Returns true when a token is held.
  Future<bool> login() async {
    final body = jsonEncode([
      {
        'cmd': 'Login',
        'action': 0,
        'param': {
          'User': {'Version': '0', 'userName': user, 'password': password}
        }
      }
    ]);
    final bytes = await _send(_api('Login', withToken: false), body: body);
    if (bytes == null) {
      _loginFailed();
      return false;
    }
    try {
      final answer = jsonDecode(utf8.decode(bytes));
      final value = (answer as List).first['value'];
      final token = value?['Token']?['name'] as String?;
      final lease = (value?['Token']?['leaseTime'] as num?)?.toInt() ?? 3600;
      if (token == null || token.isEmpty) {
        _loginFailed();
        _say('login refused, next try in ${_backoff.inSeconds}s: '
            '${utf8.decode(bytes).trim()}');
        return false;
      }
      _token = token;
      _backoff = const Duration(seconds: 2);
      // Renew at four fifths of the lease: early enough that a slow renewal
      // never leaves the poller without a session, late enough that a lease
      // is not spent three times over.
      _renewAt = DateTime.now().add(Duration(seconds: (lease * 4) ~/ 5));
      _say('logged in, lease ${lease}s');
      return true;
    } catch (e) {
      _loginFailed();
      _say('login answer not understood: $e');
      return false;
    }
  }

  /// A session, logging in only when there is none or its lease is nearly up.
  Future<bool> ensure() async {
    if (_token != null && DateTime.now().isBefore(_renewAt)) return true;
    if (DateTime.now().isBefore(_nextLoginAt)) return false;
    return login();
  }

  /// Hand the lease back. The camera cannot be made to forget one otherwise.
  Future<void> logout() async {
    if (_token == null) return;
    final body = jsonEncode([
      {'cmd': 'Logout', 'action': 0, 'param': {}}
    ]);
    await _send(_api('Logout'), body: body, timeout: const Duration(seconds: 4));
    _token = null;
    _say('logged out');
  }

  void close() => _http.close(force: true);

  /// True when the camera says the token is no longer good.
  bool _expired(String text) => text.contains('"rspCode"') &&
      (text.contains('-6') || text.contains('-5'));

  /// What is happening at the door. Null when the camera would not say.
  Future<CamEvents?> events() async {
    if (!await ensure()) return null;
    final body = jsonEncode([
      {
        'cmd': 'GetEvents',
        'action': 0,
        'param': {'channel': 0}
      }
    ]);
    var bytes = await _send(_api('GetEvents'), body: body);
    if (bytes != null && _expired(utf8.decode(bytes, allowMalformed: true))) {
      _token = null;
      if (!await login()) return null;
      bytes = await _send(_api('GetEvents'), body: body);
    }
    if (bytes == null) return null;
    try {
      final answer = jsonDecode(utf8.decode(bytes)) as List;
      final value = answer.first['value'];
      int state(String key) =>
          ((value?[key]?['alarm_state']) as num?)?.toInt() ?? 0;
      return CamEvents(visitor: state('visitor') == 1, motion: state('md') == 1);
    } catch (e) {
      _say('GetEvents answer not understood: $e');
      return null;
    }
  }

  /// A still, as JPEG bytes. Null when the camera did not give one.
  Future<Uint8List?> snap() async {
    if (!await ensure()) return null;
    Uri url() => _api('Snap', extra: {
          'channel': '0',
          // The camera wants a changing value or it answers from its cache.
          'rs': DateTime.now().millisecondsSinceEpoch.remainder(1000000).toString(),
        });
    var bytes = await _send(url());
    if (bytes != null && bytes.length < 512) {
      final text = utf8.decode(bytes, allowMalformed: true);
      if (_expired(text)) {
        _token = null;
        if (!await login()) return null;
        bytes = await _send(url());
      }
    }
    if (bytes == null || bytes.length < 4) return null;
    // A still is a JPEG. Anything else is the camera talking back, and
    // handing that to a phone as a picture is worse than handing it nothing.
    if (bytes[0] != 0xFF || bytes[1] != 0xD8) {
      _say('Snap did not answer with a JPEG (${bytes.length} bytes)');
      return null;
    }
    return Uint8List.fromList(bytes);
  }
}

/// The still, served to the LAN: API-HTTP.md's `/door/snapshot.jpg` and
/// `/door/stream.mjpeg`, and `/api/services` so a client can feature-detect
/// with one request instead of probing.
class SnapshotServer {
  SnapshotServer({
    required this.session,
    required this.port,
    required this.callsign,
    this.log,
    this.cacheFor = const Duration(milliseconds: 1000),
    this.streamGap = const Duration(milliseconds: 700),
  });

  final CameraSession session;
  final int port;
  final String callsign;
  final void Function(String line)? log;

  /// One camera call serves every viewer inside this window. Ten phones and a
  /// stream loop asking at once is one `Snap`, not eleven.
  final Duration cacheFor;

  /// How often the motion-JPEG stream takes a new frame. The camera serves
  /// h264 over RTSP and an ARMv7 doorbell cannot transcode, so the stream is
  /// honestly a run of stills: say so rather than let somebody discover it.
  final Duration streamGap;

  HttpServer? _server;
  Uint8List? _cached;
  DateTime _cachedAt = DateTime.fromMillisecondsSinceEpoch(0);
  Future<Uint8List?>? _inFlight;

  void _say(String line) => log?.call(line);

  /// The latest still, taken again only when the cached one is stale. One
  /// call at a time, so a burst of requests does not become a burst of logins.
  Future<Uint8List?> still() async {
    if (_cached != null && DateTime.now().difference(_cachedAt) < cacheFor) {
      return _cached;
    }
    if (_inFlight != null) return _inFlight;
    final work = session.snap().then((bytes) {
      if (bytes != null) {
        _cached = bytes;
        _cachedAt = DateTime.now();
      }
      _inFlight = null;
      return bytes;
    }, onError: (Object e) {
      _inFlight = null;
      _say('snap failed: $e');
      return null;
    });
    _inFlight = work;
    return work;
  }

  Future<void> start() async {
    final server = await HttpServer.bind(InternetAddress.anyIPv4, port);
    _server = server;
    server.defaultResponseHeaders.removeAll('x-frame-options');
    _say('serving stills on :$port');
    unawaited(_accept(server));
  }

  Future<void> _accept(HttpServer server) async {
    await for (final req in server) {
      // One misbehaving client must not take the doorbell down with it.
      unawaited(_handle(req).catchError((Object e) => _say('request failed: $e')));
    }
  }

  Future<void> _handle(HttpRequest req) async {
    final path = req.uri.path;
    if (req.method != 'GET') {
      return _json(req, {'ok': false, 'error': 'GET only'}, HttpStatus.methodNotAllowed);
    }
    switch (path) {
      case '/door/snapshot.jpg':
        return _snapshot(req);
      case '/door/stream.mjpeg':
        return _stream(req);
      case '/api/services':
        return _json(req, {
          'ok': true,
          // What this device is on the air, in the words XPRS.md section 13
          // owns. A doorbell relays nothing and archives nothing.
          'serve': <String>[],
          'features': {'digipeater': false, 'bridge': false, 'igate': false},
          'api': ['services', 'snapshot', 'stream'],
          'callsign': callsign,
        });
      default:
        return _json(req, {'ok': false, 'error': 'no such thing here'},
            HttpStatus.notFound);
    }
  }

  Future<void> _json(HttpRequest req, Map<String, Object?> body,
      [int status = HttpStatus.ok]) async {
    req.response
      ..statusCode = status
      ..headers.contentType = ContentType.json
      ..headers.set('access-control-allow-origin', '*')
      ..write(jsonEncode(body));
    await req.response.close();
  }

  Future<void> _snapshot(HttpRequest req) async {
    final bytes = await still();
    if (bytes == null) {
      return _json(req, {'ok': false, 'error': 'the camera gave no picture'},
          HttpStatus.serviceUnavailable);
    }
    req.response
      ..statusCode = HttpStatus.ok
      ..headers.contentType = ContentType('image', 'jpeg')
      ..headers.set('access-control-allow-origin', '*')
      ..headers.set('cache-control', 'no-store')
      ..add(bytes);
    await req.response.close();
  }

  Future<void> _stream(HttpRequest req) async {
    const boundary = 'reobellframe';
    req.response
      ..statusCode = HttpStatus.ok
      ..headers.set('content-type', 'multipart/x-mixed-replace; boundary=$boundary')
      ..headers.set('access-control-allow-origin', '*')
      ..headers.set('cache-control', 'no-store');
    try {
      // Runs until the viewer goes away: writing to a closed socket throws,
      // which is the signal to stop asking the camera for frames.
      for (;;) {
        final bytes = await still();
        if (bytes != null) {
          req.response.write('--$boundary\r\nContent-Type: image/jpeg\r\n'
              'Content-Length: ${bytes.length}\r\n\r\n');
          req.response.add(bytes);
          req.response.write('\r\n');
          await req.response.flush();
        }
        await Future<void>.delayed(streamGap);
      }
    } catch (_) {
      // the viewer closed the tab
    }
    try {
      await req.response.close();
    } catch (_) {}
  }

  Future<void> stop() async {
    await _server?.close(force: true);
    _server = null;
  }
}

/// This camera's address on the house network, for the `url:` it hands out.
/// Read again before each announcement rather than once at boot, because a
/// doorbell's address moves with the DHCP lease.
Future<String?> lanAddress() async {
  try {
    final interfaces = await NetworkInterface.list(
      includeLoopback: false,
      includeLinkLocal: false,
      type: InternetAddressType.IPv4,
    );
    for (final i in interfaces) {
      for (final a in i.addresses) {
        if (!a.isLoopback) return a.address;
      }
    }
  } catch (_) {
    // no interfaces is an answer: the url: is simply left off
  }
  return null;
}

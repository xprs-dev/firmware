/// reobell: the camera-side XPRS signer and broadcaster.
///
/// Runs on the doorbell (ARMv7 hard-float). Reuses the exact XPRS crypto
/// (`XprsCrypto` short-Schnorr, `NostrCrypto` keys/callsign) so every signature
/// and the derived X4 callsign are byte-identical to what the rest of the XPRS
/// network produces and verifies.
///
/// Modes:
///   reobell keygen                       generate the device key, print callsign+npub
///   reobell callsign                     print this device's callsign + npub
///   reobell sign-send "<wire>" [addr...] sign a wire (t: first, m: last) and broadcast
///   reobell identity [addr...]           build+sign+broadcast the presence t:identity
///   reobell run                          the daemon: watch the door, air what
///                                        happens, serve the still (see below)
///   reobell selftest                     sign/verify round-trip + callsign check
///
/// The device key (an nsec) is read from \$REOBELL_KEY (default ./reobell.key).
/// Presence fields come from env: \$REOBELL_NICK (default frontdoor),
/// \$REOBELL_URL (optional; by default the daemon works out its own address).
///
/// ── What `run` does, and why it is one process ───────────────────────────
///
/// It holds ONE login against the camera's own api.cgi over loopback, watches
/// `GetEvents` for the button and for motion, and airs what it sees the way
/// XPRS.md 11.7 says a device says it:
///
///   t:observation f:X4... state:pressed url:http://<ip>:8080/door/snapshot.jpg
///   t:observation f:X4... state:motion  url:...
///   t:observation f:X4... state:clear
///
/// and serves that url: itself, proxying `Snap` with the token it already
/// holds so no credential is ever in a URL it hands out (11.7.2).
///
/// It replaced a shell poller that logged in again on every token error and
/// leaked a lease on every restart, and that could only say `t:message` --
/// prose where the format has a word.
///
/// ── Why there is no `t:message` here at all ──────────────────────────────
///
/// `t:message` is chat. A station's chat ring admits exactly `t:message` and
/// `t:status`, and the phone's `#LOCAL` room admits any undirected
/// `scope:local` message with no regard for whether a person or a machine
/// sent it, so a doorbell airing one puts a bubble, an unread and a
/// notification in a conversation on every press. Section 11.3 of the format
/// draws the line for commands in so many words -- "it must not appear in a
/// conversation view even when it carries `m:`" -- and a device reporting is
/// the same shape. What a thing has to say is an observation (design rule 5:
/// one packet type carries every kind of observation), and a receiver decides
/// for itself whether that is worth interrupting anybody for.
///
/// Env it reads, all optional except the password:
///   REOBELL_API        camera API base          (default http://127.0.0.1)
///   REOBELL_USER       camera user              (default admin)
///   REOBELL_PASS       camera password          (no default; no session without it)
///   REOBELL_BCAST      extra broadcast address  (e.g. 192.168.1.255)
///   REOBELL_HTTP_PORT  where the still is served (default 8080; 80 is the camera's)
///   REOBELL_POLL_MS    how often the door is read (default 1000)
///   REOBELL_MOTION_DEBOUNCE_S  least gap between motion reports (default 30)
///   REOBELL_CLEAR_AFTER_S   quiet needed before `clear` (default 20)
///   REOBELL_IDENTITY_S they hear who it is this often (default 300)
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:hex/hex.dart';

import 'package:reobell/camera.dart';
import 'package:reobell/nostr_crypto.dart';
import 'package:reobell/xprs_crypto.dart';
import 'package:reobell/xprs_packet.dart';

/// Fields excluded from the signed text (added/changed after authorship).
const Set<String> kIdExcluded = {'sig', 'via'};
const int kPort = 4242;

String _env(String k, [String def = '']) => Platform.environment[k] ?? def;
String _keyPath() => _env('REOBELL_KEY', 'reobell.key');

String nowTs() {
  final u = DateTime.now().toUtc();
  String p(int v) => v.toString().padLeft(2, '0');
  return '${u.year.toString().padLeft(4, '0')}-${p(u.month)}-${p(u.day)}'
      '_${p(u.hour)}:${p(u.minute)}:${p(u.second)}';
}

/// The device identity, derived from its private key.
class DevKey {
  DevKey(this.privHex)
      : pubHex = NostrCrypto.derivePublicKey(privHex),
        d = BigInt.parse(privHex, radix: 16);
  final String privHex;
  final String pubHex;
  final BigInt d;

  String get npub => NostrCrypto.encodeNpub(pubHex);
  // A doorbell is an X4 device (automated, operated by a controller).
  String get callsign => 'X4${NostrCrypto.deriveCallsign(pubHex)}';
  Uint8List get pubXonly => Uint8List.fromList(HEX.decode(pubHex));
}

DevKey _loadKey() {
  final f = File(_keyPath());
  if (!f.existsSync()) {
    stderr.writeln('no key at ${_keyPath()} (run: reobell keygen)');
    exit(2);
  }
  final nsec = f.readAsStringSync().trim();
  try {
    return DevKey(NostrCrypto.decodeNsec(nsec));
  } catch (e) {
    stderr.writeln('bad key file ${_keyPath()}: $e');
    exit(2);
  }
}

/// Signs [p] in place per XPRS section 9.1: sha256 of the packet without
/// sig:/via:, short-Schnorr, base85, inserted as sig: before any m:.
XprsPacket signPacket(XprsPacket p, BigInt d) {
  final text = p.without(kIdExcluded).encode();
  final digest = NostrCrypto.sha256Bytes(Uint8List.fromList(utf8.encode(text)));
  final sig = XprsCrypto.b85encode(XprsCrypto.sign(digest, d));
  return p.with_('sig', sig);
}

Future<int> broadcast(String wire, List<String> extra) async {
  final payload = utf8.encode(wire);
  final socket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
  socket.broadcastEnabled = true;
  final targets = <String>{'255.255.255.255', ...extra};
  var sent = 0;
  for (final a in targets) {
    try {
      if (socket.send(payload, InternetAddress(a), kPort) > 0) sent++;
    } catch (_) {
      // one unreachable target must not stop the others
    }
  }
  socket.close();
  return sent;
}

Future<int> _cmdKeygen() async {
  final kp = NostrCrypto.generateKeyPair();
  final f = File(_keyPath());
  f.writeAsStringSync('${kp.nsec}\n', flush: true);
  try {
    Process.runSync('chmod', ['600', _keyPath()]);
  } catch (_) {}
  final callsign = 'X4${NostrCrypto.deriveCallsign(kp.publicKeyHex)}';
  stdout.writeln('callsign=$callsign');
  stdout.writeln('npub=${kp.npub}');
  stdout.writeln('key=${_keyPath()}');
  return 0;
}

int _cmdCallsign() {
  final k = _loadKey();
  stdout.writeln('callsign=${k.callsign}');
  stdout.writeln('npub=${k.npub}');
  return 0;
}

Future<int> _cmdSignSend(List<String> args) async {
  if (args.isEmpty) {
    stderr.writeln('usage: reobell sign-send "<wire>" [addr...]');
    return 2;
  }
  final k = _loadKey();
  final parsed = XprsPacket.parse(args.first);
  if (parsed == null) {
    stderr.writeln('not an XPRS wire (needs a leading t:)');
    return 2;
  }
  // The callsign must derive from the signing key, so force f: to ours.
  final withF = parsed.with_('f', k.callsign);
  final signed = signPacket(withF, k.d).encode();
  if (utf8.encode(signed).length > XprsPacket.maxBytes) {
    stderr.writeln('over 250 bytes after signing, refusing');
    return 2;
  }
  final n = await broadcast(signed, args.sublist(1));
  stdout.writeln(signed);
  return n > 0 ? 0 : 1;
}

Future<int> _cmdIdentity(List<String> args) async {
  final k = _loadKey();
  final nick = _env('REOBELL_NICK', 'frontdoor');
  final url = _env('REOBELL_URL');
  final fields = <MapEntry<String, String>>[
    const MapEntry('t', 'identity'),
    MapEntry('f', k.callsign),
    MapEntry('k', k.npub),
    MapEntry('nick', nick),
    if (url.isNotEmpty) MapEntry('url', url),
    MapEntry('ts', nowTs()),
    const MapEntry('scope', 'local'),
  ];
  final signed = signPacket(XprsPacket(fields), k.d).encode();
  final n = await broadcast(signed, args);
  stdout.writeln(signed);
  return n > 0 ? 0 : 1;
}

/// Proves the vendored crypto round-trips: sign then verify with the reference
/// XprsCrypto.verify, and confirm the callsign derives from the key.
int _cmdSelftest() {
  final kp = NostrCrypto.generateKeyPair();
  final d = BigInt.parse(kp.privateKeyHex, radix: 16);
  final pubXonly = Uint8List.fromList(HEX.decode(kp.publicKeyHex));
  final callsign = 'X4${NostrCrypto.deriveCallsign(kp.publicKeyHex)}';

  final base = XprsPacket.parse(
      't:observation f:$callsign state:pressed '
      'url:http://192.168.1.9:8080/door/snapshot.jpg ts:${nowTs()} scope:local')!;
  final signed = signPacket(base, d);
  final sigVal = signed['sig'];
  if (sigVal == null || sigVal.length != 60) {
    stdout.writeln('FAIL: sig missing or not 60 chars (${sigVal?.length})');
    return 1;
  }
  final sigBytes = XprsCrypto.b85decode(sigVal);
  if (sigBytes == null || sigBytes.length != 48) {
    stdout.writeln('FAIL: sig did not decode to 48 bytes');
    return 1;
  }
  final digest = NostrCrypto.sha256Bytes(Uint8List.fromList(
      utf8.encode(signed.without(kIdExcluded).encode())));
  final ok = XprsCrypto.verify(digest, sigBytes, pubXonly);
  if (!ok) {
    stdout.writeln('FAIL: reference verify rejected our signature');
    return 1;
  }
  // Tamper: a flipped byte must fail. `pressed` to `motion` is the change
  // that matters here -- a signature that survived it would let anybody turn
  // one of this doorbell's reports into another.
  final tampered = XprsPacket.parse(
      signed.encode().replaceFirst('state:pressed', 'state:motion'))!;
  final tdigest = NostrCrypto.sha256Bytes(Uint8List.fromList(
      utf8.encode(tampered.without(kIdExcluded).encode())));
  if (XprsCrypto.verify(tdigest, sigBytes, pubXonly)) {
    stdout.writeln('FAIL: tampered packet still verified');
    return 1;
  }
  // Callsign must derive from the key.
  if (!NostrCrypto.callsignMatchesKey(callsign, kp.publicKeyHex)) {
    stdout.writeln('FAIL: callsign does not match key');
    return 1;
  }
  stdout.writeln('OK callsign=$callsign sig=$sigVal');
  stdout.writeln('OK signed=${signed.encode()}');
  return 0;
}

/// ── The daemon ──────────────────────────────────────────────────────────
int _envInt(String k, int def) => int.tryParse(_env(k)) ?? def;

void _log(String line) {
  stdout.writeln('${nowTs()} $line');
}

/// Sign [fields] and put it on the air. Drops `url:` rather than the whole
/// packet when the wire would run past the 250-byte limit: a state nobody can
/// fetch a picture for still says what happened.
Future<void> _airFields(
    DevKey key, List<MapEntry<String, String>> fields, List<String> bcast) async {
  var packet = XprsPacket(fields);
  var signed = signPacket(packet, key.d);
  if (utf8.encode(signed.encode()).length > XprsPacket.maxBytes) {
    packet = XprsPacket(fields.where((f) => f.key != 'url').toList());
    signed = signPacket(packet, key.d);
    _log('wire too long with url:, aired without it');
  }
  if (utf8.encode(signed.encode()).length > XprsPacket.maxBytes) {
    _log('wire too long even without url:, not aired');
    return;
  }
  final n = await broadcast(signed.encode(), bcast);
  _log('aired ${signed.encode()}${n > 0 ? '' : ' (nobody to send to)'}');
}

Future<void> _airObservation(DevKey key, String state, String? url,
    List<String> bcast) async {
  await _airFields(key, [
    const MapEntry('t', 'observation'),
    MapEntry('f', key.callsign),
    MapEntry('state', state),
    if (url != null && url.isNotEmpty) MapEntry('url', url),
    MapEntry('ts', nowTs()),
    const MapEntry('scope', 'local'),
  ], bcast);
}

Future<void> _airIdentity(DevKey key, String? url, List<String> bcast) async {
  await _airFields(key, [
    const MapEntry('t', 'identity'),
    MapEntry('f', key.callsign),
    MapEntry('k', key.npub),
    MapEntry('nick', _env('REOBELL_NICK', 'frontdoor')),
    if (url != null && url.isNotEmpty) MapEntry('url', url),
    MapEntry('ts', nowTs()),
    const MapEntry('scope', 'local'),
  ], bcast);
}

Future<int> _cmdRun(List<String> args) async {
  final key = _loadKey();
  final bcast = <String>[
    ..._env('REOBELL_BCAST').split(RegExp(r'[ ,]+')).where((s) => s.isNotEmpty),
    ...args,
  ];
  final port = _envInt('REOBELL_HTTP_PORT', 8080);
  final pollMs = _envInt('REOBELL_POLL_MS', 1000);
  final motionDebounce =
      Duration(seconds: _envInt('REOBELL_MOTION_DEBOUNCE_S', 30));
  final clearAfter = Duration(seconds: _envInt('REOBELL_CLEAR_AFTER_S', 20));
  final identityEvery = Duration(seconds: _envInt('REOBELL_IDENTITY_S', 300));

  _log('reobell ${key.callsign} starting');

  final session = CameraSession(
    base: _env('REOBELL_API', 'http://127.0.0.1'),
    user: _env('REOBELL_USER', 'admin'),
    password: _env('REOBELL_PASS'),
    log: _log,
  );
  final server = SnapshotServer(
    session: session,
    port: port,
    callsign: key.callsign,
    log: _log,
  )..keyFile = _env('REOBELL_KEY');
  try {
    await server.start();
  } catch (e) {
    // No picture is a smaller loss than no doorbell: keep going.
    _log('could not serve stills on :$port: $e');
  }

  /// Where this camera's still can be fetched. Worked out again each time,
  /// because a doorbell's address moves with its DHCP lease.
  Future<String?> pictureUrl() async {
    final fixed = _env('REOBELL_URL');
    if (fixed.isNotEmpty) return fixed;
    final ip = await lanAddress();
    return ip == null ? null : 'http://$ip:$port/door/snapshot.jpg';
  }

  var stopping = false;
  Future<void> shutdown(String why) async {
    if (stopping) return;
    stopping = true;
    _log('stopping ($why)');
    await server.stop();
    await session.logout();
    session.close();
    exit(0);
  }

  ProcessSignal.sigterm.watch().listen((_) => shutdown('SIGTERM'));
  ProcessSignal.sigint.watch().listen((_) => shutdown('SIGINT'));

  // Who it is, now and every few minutes after.
  await _airIdentity(key, await pictureUrl(), bcast);
  Timer.periodic(identityEvery, (_) async {
    await _airIdentity(key, await pictureUrl(), bcast);
  });

  if (session.password.isEmpty) {
    _log('no REOBELL_PASS: the door cannot be watched and no still can be '
        'taken. Announcing only.');
  }

  var wasVisitor = false;
  var wasMotion = false;
  var lastMotion = DateTime.fromMillisecondsSinceEpoch(0);
  var lastPress = DateTime.fromMillisecondsSinceEpoch(0);
  DateTime? quietSince;
  var saidClear = true;

  while (!stopping) {
    if (session.password.isNotEmpty) {
      final ev = await session.events();
      if (ev != null) {
        final now = DateTime.now();
        final url = await pictureUrl();

        // The button. Aired every time it is pressed. The debounce is not a
        // judgement about whether a ring is worth anybody's attention -- that
        // is the receiver's to make -- it only stops a one-second poll reading
        // one press as two.
        if (ev.visitor && !wasVisitor &&
            now.difference(lastPress) > const Duration(seconds: 5)) {
          lastPress = now;
          saidClear = false;
          await _airObservation(key, 'pressed', url, bcast);
        }

        // Movement, debounced the same way and for the same reason: the
        // camera's md flag flaps several times while one person walks past,
        // and those are one movement rather than six. Whoever hears it decides
        // what to do about it.
        if (ev.motion && !wasMotion && now.difference(lastMotion) > motionDebounce) {
          lastMotion = now;
          saidClear = false;
          await _airObservation(key, 'motion', url, bcast);
        }

        // `clear` ends an event (11.7.2). Only after a quiet spell, because
        // motion flaps, and only once.
        final busy = ev.visitor || ev.motion;
        if (busy) {
          quietSince = null;
        } else {
          quietSince ??= now;
          if (!saidClear && now.difference(quietSince) >= clearAfter) {
            saidClear = true;
            await _airObservation(key, 'clear', null, bcast);
          }
        }
        wasVisitor = ev.visitor;
        wasMotion = ev.motion;
      }
    }
    await Future<void>.delayed(Duration(milliseconds: pollMs));
  }
  return 0;
}

Future<void> main(List<String> args) async {
  if (args.isEmpty) {
    stderr.writeln(
        'usage: reobell <run|keygen|callsign|sign-send|identity|selftest>');
    exit(2);
  }
  final cmd = args.first;
  final rest = args.sublist(1);
  int code;
  switch (cmd) {
    case 'keygen':
      code = await _cmdKeygen();
    case 'callsign':
      code = _cmdCallsign();
    case 'sign-send':
      code = await _cmdSignSend(rest);
    case 'identity':
      code = await _cmdIdentity(rest);
    case 'run':
      code = await _cmdRun(rest);
    case 'selftest':
      code = _cmdSelftest();
    default:
      stderr.writeln('unknown command: $cmd');
      code = 2;
  }
  exit(code);
}

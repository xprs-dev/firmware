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
///   reobell selftest                     sign/verify round-trip + callsign check
///
/// The device key (an nsec) is read from $REOBELL_KEY (default ./reobell.key).
/// Presence fields come from env: $REOBELL_NICK (default frontdoor),
/// $REOBELL_URL (optional, e.g. http://<cam-ip>/door/snapshot.jpg).
library;

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:hex/hex.dart';

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
      't:message f:$callsign ts:${nowTs()} scope:local m:Someone at the front door')!;
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
  // Tamper: a flipped message byte must fail.
  final tampered = XprsPacket.parse(
      signed.encode().replaceFirst('front door', 'back door'))!;
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

Future<void> main(List<String> args) async {
  if (args.isEmpty) {
    stderr.writeln('usage: reobell <keygen|callsign|sign-send|identity|selftest>');
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
    case 'selftest':
      code = _cmdSelftest();
    default:
      stderr.writeln('unknown command: $cmd');
      code = 2;
  }
  exit(code);
}

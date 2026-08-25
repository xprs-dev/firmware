/*
 * sign_firmware.dart -- approve a firmware image for XPRS stations (25.8).
 *
 * The station accepts an IMAGE, never a source, so this is what makes an
 * image installable: a signature by the key the station has pinned, over a
 * line that no XPRS packet can produce --
 *
 *     xprsfw1 <board> <version> <size> <sha256 as 64 lowercase hex>
 *
 * -- which binds the board, the version, the size and the content
 * together. A build for one board therefore cannot install on another, and
 * last version's approval cannot be replayed onto the next one.
 *
 * THE KEY DOES NOT GO INTO CI. It is the maintainer's identity key: the
 * same nsec that signs this operator's packets. A key in a build runner is
 * a key every maintainer with push access and every compromised action can
 * use, and this one can put code on a roof. So CI builds and hashes, and
 * approving a build is a deliberate local act.
 *
 *   dart run tools/sign_firmware.dart \
 *       --board m5stack-core --version 1.4.2 \
 *       --bin dist/xprs-m5stack-core-1.4.2.bin \
 *       --nsec-file ~/.xprs/fw.nsec
 *
 * It prints the sha256, the signature, and the manifest fragment to paste
 * into the channel file. It never writes the key anywhere.
 */
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:hex/hex.dart';

// Run from inside the flutter checkout so these resolve; the whole point is
// that this uses the SAME implementation the app and the device agree on
// (common/xprs_sig/test_xprssig_host.c cross-checks the C side).
import 'package:aurora/util/nostr_crypto.dart';
import 'package:aurora/util/xprs_crypto.dart';

String _arg(List<String> a, String name, {String? fallback}) {
  final i = a.indexOf('--$name');
  if (i >= 0 && i + 1 < a.length) return a[i + 1];
  if (fallback != null) return fallback;
  stderr.writeln('missing --$name');
  exit(2);
}

void main(List<String> argv) {
  final board = _arg(argv, 'board');
  final version = _arg(argv, 'version');
  final binPath = _arg(argv, 'bin');
  final nsecPath = _arg(argv, 'nsec-file');

  final bin = File(binPath);
  if (!bin.existsSync()) {
    stderr.writeln('no such image: $binPath');
    exit(2);
  }
  final bytes = bin.readAsBytesSync();
  final sha = crypto.sha256.convert(bytes).toString(); // lowercase hex

  // The canonical line. Single spaces, no trailing newline: the device
  // rebuilds this byte for byte before it checks anything.
  final line = 'xprsfw1 $board $version ${bytes.length} $sha';
  final digest = NostrCrypto.sha256Bytes(
      Uint8List.fromList(utf8.encode(line)));

  final nsec = File(nsecPath).readAsStringSync().trim();
  BigInt d;
  try {
    d = BigInt.zero;
    for (final b in HEX.decode(NostrCrypto.decodeNsec(nsec))) {
      d = (d << 8) | BigInt.from(b);
    }
  } catch (_) {
    stderr.writeln('that file does not hold an nsec');
    exit(2);
  }
  final sig = XprsCrypto.b85encode(XprsCrypto.sign(digest, d));

  stdout.writeln('signed line : $line');
  stdout.writeln('sha256      : $sha');
  stdout.writeln('size        : ${bytes.length}');
  stdout.writeln('signature   : $sig');
  stdout.writeln('');
  stdout.writeln('manifest fragment:');
  stdout.writeln(const JsonEncoder.withIndent('  ').convert({
    'version': version,
    'assets': [
      {
        'name': '${bin.uri.pathSegments.last}',
        'url': 'v$version/${bin.uri.pathSegments.last}',
        'size': bytes.length,
        'sha256': sha,
        'sig': sig,
      }
    ],
  }));
}

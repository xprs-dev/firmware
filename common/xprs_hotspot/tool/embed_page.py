#!/usr/bin/env python3
"""Regenerate chat_page.c from chat_page.html + the crypto JS.

The page is served from flash GZIPPED, with Content-Encoding: gzip -- it is
38,845 bytes of HTML+JS+base64 font and compresses to 18,139, which is the
single largest rodata item on the board after the LVGL fonts. Edit
chat_page.html (and, for the signing code, xprs_crypto.js), then run this
from the component dir:

    python3 tool/embed_page.py

The /*CRYPTO*/ marker in the HTML is replaced by xprs_crypto.js (its node
test block stripped). The crypto is validated against the XPRS.md 9.1.2
worked example by tool/test_crypto.sh.
"""
import os

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
html = open(os.path.join(here, 'chat_page.html')).read()
js = open(os.path.join(here, 'xprs_crypto.js')).read()

marker = '// ---- node test against the spec 9.1.2 worked example ----'
if marker in js:
    js = js[:js.index(marker)]

html = html.replace('/*CRYPTO*/', js)

# The logo font comes verbatim from the old chat page -- one theme, one font.
import re
old = open('/home/brito/code/xprs/xprs-esp32/common/xprs_http/'
           'chat_page.c').read()
m = re.search(r'"(@font-face\{[^\n]*?format\(\'woff2\'\);'
              r'font-display:swap\})', old)
if m:
    font = m.group(1).replace('\\"', '"').replace('\\\\', '\\')
    html = html.replace('/*FONTFACE*/', font)


import gzip

raw = html.encode('utf-8')
# mtime=0 so regenerating an unchanged page produces an identical file and
# the build does not relink for nothing.
gz = gzip.compress(raw, compresslevel=9, mtime=0)


def c_bytes(b):
    out = []
    for i in range(0, len(b), 16):
        out.append('    ' + ' '.join('0x%02x,' % c for c in b[i:i + 16]))
    return '\n'.join(out)


with open(os.path.join(here, 'chat_page.c'), 'w') as f:
    f.write('/* GENERATED from chat_page.html + xprs_crypto.js by '
            'tool/embed_page.py -- edit those, not this.\n'
            ' *\n'
            ' * Stored gzipped and served with Content-Encoding: gzip.\n'
            ' * %d bytes of page -> %d bytes of flash. */\n' % (len(raw), len(gz)))
    f.write('#include <stddef.h>\n\n')
    f.write('const unsigned char XPRS_CHAT_PAGE_GZ[] = {\n')
    f.write(c_bytes(gz))
    f.write('\n};\n\nconst size_t XPRS_CHAT_PAGE_GZ_LEN = '
            'sizeof(XPRS_CHAT_PAGE_GZ);\n')

print('chat_page.c: %d bytes of page -> %d gzipped (%d%%)'
      % (len(raw), len(gz), 100 * len(gz) // len(raw)))

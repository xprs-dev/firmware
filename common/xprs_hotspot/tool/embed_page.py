#!/usr/bin/env python3
"""Regenerate chat_page.c from chat_page.html + the crypto JS.

The page is served from flash as a C string. Edit chat_page.html (and, for
the signing code, xprs_crypto.js), then run this from the component dir:

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


def c_escape(s):
    out = []
    for line in s.split('\n'):
        e = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append('"%s\\n"' % e)
    return '\n'.join(out)


with open(os.path.join(here, 'chat_page.c'), 'w') as f:
    f.write('/* GENERATED from chat_page.html + xprs_crypto.js by '
            'tool/embed_page.py -- edit those, not this. */\n')
    f.write('#include <stddef.h>\n\n')
    f.write('const char XPRS_CHAT_PAGE[] =\n')
    f.write(c_escape(html))
    f.write(';\n\nconst size_t XPRS_CHAT_PAGE_LEN = sizeof(XPRS_CHAT_PAGE) - 1;\n')

print('chat_page.c: %d bytes of page' % len(html))

#!/usr/bin/env python3
"""Regenerate chat_page.c from the shared web chat page.

The page is no longer edited here: it lives in the sibling repo
xprs-dev/webchat (../webchat next to this checkout), which the Android app
serves too. Edit page/ there, then run this from anywhere:

    python3 common/xprs_hotspot/tool/embed_page.py

It asks webchat's build tool for the gzipped page as the C array this
component links (served from flash with Content-Encoding: gzip).
"""
import os
import subprocess
import sys

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
repo = os.path.dirname(os.path.dirname(here))
build = os.path.join(os.path.dirname(repo), 'webchat', 'tool', 'build.py')
if not os.path.exists(build):
    sys.exit('webchat is not checked out next to this repo: expected ' + build)
sys.exit(subprocess.call([sys.executable, build, '--c',
                          os.path.join(here, 'chat_page.c')]))

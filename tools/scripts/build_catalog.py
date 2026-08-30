#!/usr/bin/env python3
"""Build the hardware catalogue page from models/*/board.yml.

    tools/scripts/build_catalog.py [-o catalog.html] [--embed]

The page is GENERATED, never hand-edited, and that is the point: a board's
facts are written once in its board.yml, checked for shape by
check_board_yml.py, and rendered here. A catalogue maintained by hand goes
stale the first time a board changes and nobody can tell which half is true.

--embed inlines the photographs as data: URIs, downscaled, for publishing the
page somewhere that cannot serve the repository's files alongside it. Without
it the page refers to models/<id>/... relative paths, which is what a real
site wants.

DESIGN NOTES, so the next person changing the CSS knows what was deliberate:

  Colour is the station's own, not a new scheme. #ffa86a on #101010 with
  #f0f0f0 text is what xprs_ui.h and the hotspot chat page already use, so a
  person who has seen a T-Deck's screen recognises this page as the same
  project. The light theme derives from that accent rather than replacing it.

  The bearer matrix is the spine. Every card carries the same four cells --
  BLE5, LoRa, LAN, ESP-NOW -- because "what can this board actually do on the
  network" is the question somebody choosing firmware is really asking, and
  it is the one thing a spec table full of megahertz will not answer. Four
  fixed cells in a fixed order also means the answer reads DOWN the page as
  well as across a card.

  Type is three faces doing three jobs: Archivo for headings (industrial,
  label-like), Newsreader for prose (this catalogue has opinions in it and
  they need to be readable), JetBrains Mono for every number, pin and
  callsign, because half the facts here are log lines.
"""
import os
import sys
import glob
import json
import base64
import argparse
import io

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is needed: pip install pyyaml")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BEARERS = [("ble5", "BLE5"), ("lora", "LoRa"), ("lan", "LAN"), ("espnow", "ESP-NOW")]
FAMILY_LABEL = {"esp32": "ESP32", "esp32s3": "ESP32-S3",
                "esp32c3": "ESP32-C3", "nrf52": "nRF52840"}


def esc(v):
    if v is None:
        return ""
    return (str(v).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def thumb_data_uri(path, width=560):
    from PIL import Image
    im = Image.open(path)
    im = im.convert("RGB")
    if im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=72, optimize=True)
    return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()


def load_boards():
    boards = []
    for p in sorted(glob.glob(os.path.join(ROOT, "models", "*", "board.yml"))):
        if os.path.basename(os.path.dirname(p)) == "_template":
            continue
        d = yaml.safe_load(open(p)) or {}
        d["_dir"] = os.path.dirname(p)
        d["_rel"] = os.path.relpath(os.path.dirname(p), ROOT)
        boards.append(d)
    # Shipping first, then planned, then the rest; within a group, the ones
    # that carry more bearers lead, because that is what makes a board useful.
    rank = {"shipping": 0, "planned": 1, "legacy": 2, "unsupported": 3}
    def score(b):
        yes = sum(1 for k, _ in BEARERS if (b.get("bearers") or {}).get(k) == "yes")
        return (rank.get(b.get("status"), 9), -yes, b.get("id") or "")
    return sorted(boards, key=score)


def bearer_cells(b):
    out = []
    for key, label in BEARERS:
        v = (b.get("bearers") or {}).get(key) or "untested"
        note = (b.get("bearers") or {}).get(key + "_note") or ""
        mark = {"yes": "●", "no": "—", "untested": "◐"}.get(v, "?")
        out.append(
            f'<div class="bearer b-{esc(v)}"'
            f'{f" title={json.dumps(note)}" if note else ""}>'
            f'<span class="bearer-mark">{mark}</span>'
            f'<span class="bearer-name">{esc(label)}</span></div>')
    return "".join(out)


def spec_rows(b):
    sil, phys, fw = b.get("silicon") or {}, b.get("physical") or {}, b.get("firmware") or {}
    io_ = b.get("io") or {}
    rows = []

    def row(k, v):
        if v in (None, "", []):
            return
        rows.append(f'<div class="spec"><dt>{esc(k)}</dt>'
                    f'<dd class="mono">{esc(v)}</dd></div>')

    row("Core", sil.get("core"))
    mem = []
    if sil.get("ram_kb"):
        mem.append(f"{sil['ram_kb']} KB RAM")
    if sil.get("psram_mb"):
        mem.append(f"{sil['psram_mb']} MB PSRAM")
    if sil.get("flash_mb"):
        mem.append(f"{sil['flash_mb']} MB flash")
    row("Memory", " · ".join(mem) if mem else None)

    scr = io_.get("screen")
    row("Screen", f"{scr['controller']} {scr['width']}×{scr['height']}"
        if isinstance(scr, dict) else "none")
    row("GNSS", io_.get("gnss"))
    row("Power", phys.get("power"))
    row("Size", f"{phys['dimensions_mm']} mm" if phys.get("dimensions_mm") else None)
    row("Toolchain", fw.get("toolchain"))
    return "".join(rows)


def card(b, embed):
    fw = b.get("firmware") or {}
    status = b.get("status") or "unknown"
    sil = b.get("silicon") or {}
    fam = FAMILY_LABEL.get(sil.get("family"), sil.get("mcu") or "?")

    imgs = ""
    for im in (b.get("images") or [])[:4]:
        f = im.get("file")
        if not f:
            continue
        abspath = os.path.join(b["_dir"], f)
        if not os.path.isfile(abspath):
            continue
        src = thumb_data_uri(abspath) if embed else f"{b['_rel']}/{f}"
        imgs += (f'<figure class="shot"><img src="{esc(src)}" alt="{esc(im.get("caption"))}" '
                 f'loading="lazy"><figcaption>{esc(im.get("caption"))}'
                 f'<span class="credit">{esc(im.get("credit"))}</span></figcaption></figure>')
    if imgs:
        imgs = f'<div class="shots">{imgs}</div>'

    docs = "".join(
        f'<a class="doclink" href="{esc(d.get("url"))}">{esc(d.get("title"))}</a>'
        for d in (b.get("docs") or []) if d.get("url"))

    if fw.get("project"):
        build = (f'<div class="build"><div class="build-head">Build it</div>'
                 f'<pre class="mono">cd {esc(fw["project"])}\n'
                 f'~/.platformio/penv/bin/pio run{" -e " + esc(fw["env"]) if fw.get("env") else ""}\n'
                 f'~/.platformio/penv/bin/pio run{" -e " + esc(fw["env"]) if fw.get("env") else ""} -t upload</pre>'
                 f'<p class="flashnote">{esc(fw.get("flashing"))}</p></div>')
    else:
        build = (f'<div class="build build-none"><div class="build-head">No firmware yet</div>'
                 f'<p class="flashnote">{esc(fw.get("flashing")) or "Not buildable from this tree."}</p></div>')

    ver = (f'<span class="ver mono">v{esc(fw["version"])}</span>'
           if fw.get("version") else "")

    return f"""
<article class="card" data-status="{esc(status)}"
         data-bearers="{esc(' '.join(k for k, _ in BEARERS if (b.get('bearers') or {{}}).get(k) == 'yes'))}">
  <header class="card-head">
    <div class="card-title">
      <h2>{esc(b.get('name'))}</h2>
      <p class="vendor">{esc(b.get('vendor'))}{' · SKU ' + esc(b['sku']) if b.get('sku') else ''}</p>
    </div>
    <div class="card-tags">
      <span class="chip chip-fam mono">{esc(fam)}</span>
      <span class="chip chip-status s-{esc(status)}">{esc(status)}</span>
      {ver}
    </div>
  </header>
  <p class="summary">{esc(b.get('summary'))}</p>
  <div class="bearers" aria-label="XPRS bearers">{bearer_cells(b)}</div>
  <dl class="specs">{spec_rows(b)}</dl>
  {imgs}
  {build}
  <footer class="card-foot">{docs}</footer>
</article>"""


CSS = """
:root{
  /* The station's own palette. #ffa86a on #101010 with #f0f0f0 text is what
     xprs_ui.h and the hotspot chat page already draw with; this page is the
     same project seen from the outside, so it uses the same three. Light is
     derived from that accent rather than being a second, unrelated scheme. */
  --accent:#c2551a; --accent-soft:#f0e2d6;
  --ground:#f1eeea; --raised:#fbf9f7; --sunk:#e7e2dc;
  --ink:#1c1815; --ink-dim:#6b6259; --rule:#dcd5cc;
  --ok:#4e7d3c; --absent:#8a827a; --partial:#a9761a;
  --shadow:0 1px 2px rgba(28,24,21,.06),0 8px 24px -12px rgba(28,24,21,.18);
}
@media (prefers-color-scheme:dark){ :root:not([data-theme="light"]){
  --accent:#ffa86a; --accent-soft:#2a1c10;
  --ground:#101010; --raised:#1b1b1b; --sunk:#161616;
  --ink:#f0f0f0; --ink-dim:#948c84; --rule:#2e2b28;
  --ok:#86c06c; --absent:#7a7570; --partial:#e0a836;
  --shadow:0 1px 2px rgba(0,0,0,.5),0 10px 30px -14px rgba(0,0,0,.8);
}}
:root[data-theme="dark"]{
  --accent:#ffa86a; --accent-soft:#2a1c10;
  --ground:#101010; --raised:#1b1b1b; --sunk:#161616;
  --ink:#f0f0f0; --ink-dim:#948c84; --rule:#2e2b28;
  --ok:#86c06c; --absent:#7a7570; --partial:#e0a836;
  --shadow:0 1px 2px rgba(0,0,0,.5),0 10px 30px -14px rgba(0,0,0,.8);
}
*{box-sizing:border-box}
body{
  background:var(--ground); color:var(--ink);
  font-family:Newsreader,Georgia,serif; font-size:17px; line-height:1.6;
  margin:0; -webkit-font-smoothing:antialiased;
}
.mono{font-family:"JetBrains Mono",ui-monospace,SFMono-Regular,Menlo,monospace;
  font-variant-numeric:tabular-nums}
h1,h2,h3,.eyebrow,.chip,.bearer-name,.build-head,.filters{
  font-family:Archivo,"Helvetica Neue",Arial,sans-serif}
h1,h2{text-wrap:balance; margin:0}
a{color:var(--accent)}
:focus-visible{outline:2px solid var(--accent); outline-offset:3px; border-radius:3px}

.wrap{max-width:1080px; margin:0 auto; padding:0 24px 96px}

/* ── Masthead ─────────────────────────────────────────────────────────── */
.top{border-bottom:1px solid var(--rule); background:var(--raised)}
.top-in{max-width:1080px; margin:0 auto; padding:44px 24px 34px;
  display:flex; flex-wrap:wrap; gap:28px; align-items:flex-end;
  justify-content:space-between}
.eyebrow{font-size:11px; letter-spacing:.16em; text-transform:uppercase;
  color:var(--accent); font-weight:700; margin:0 0 10px}
h1{font-size:clamp(30px,5vw,46px); font-weight:800; letter-spacing:-.02em;
  line-height:1.05}
.blurb{color:var(--ink-dim); max-width:52ch; margin:14px 0 0; font-size:16px}
.counts{display:flex; gap:26px}
.count b{display:block; font-family:Archivo,sans-serif; font-weight:800;
  font-size:30px; line-height:1; font-variant-numeric:tabular-nums}
.count span{font-size:11px; letter-spacing:.13em; text-transform:uppercase;
  color:var(--ink-dim); font-family:Archivo,sans-serif}

/* ── Filters ──────────────────────────────────────────────────────────── */
.filters{display:flex; flex-wrap:wrap; gap:8px; align-items:center;
  padding:22px 0 4px; font-size:12px}
.filters .lbl{letter-spacing:.13em; text-transform:uppercase;
  color:var(--ink-dim); margin-right:4px; font-weight:600}
.fbtn{font:inherit; font-weight:600; cursor:pointer; border:1px solid var(--rule);
  background:var(--raised); color:var(--ink); padding:6px 13px; border-radius:999px;
  letter-spacing:.02em}
.fbtn[aria-pressed="true"]{background:var(--accent); border-color:var(--accent);
  color:var(--ground)}

/* ── Cards ────────────────────────────────────────────────────────────── */
.grid{display:grid; gap:22px; padding-top:22px}
.card{background:var(--raised); border:1px solid var(--rule); border-radius:14px;
  padding:26px 26px 22px; box-shadow:var(--shadow)}
.card[hidden]{display:none}
.card-head{display:flex; flex-wrap:wrap; gap:12px 20px;
  align-items:flex-start; justify-content:space-between}
.card-title h2{font-size:25px; font-weight:800; letter-spacing:-.015em}
.vendor{margin:3px 0 0; color:var(--ink-dim); font-size:14px}
.card-tags{display:flex; flex-wrap:wrap; gap:7px; align-items:center}
.chip{font-size:11px; font-weight:700; letter-spacing:.08em; text-transform:uppercase;
  padding:5px 10px; border-radius:5px; border:1px solid var(--rule); white-space:nowrap}
.chip-fam{background:var(--sunk)}
.s-shipping{color:var(--ok); border-color:currentColor}
.s-planned{color:var(--partial); border-color:currentColor}
.s-legacy,.s-unsupported{color:var(--absent); border-color:currentColor}
.ver{font-size:12px; color:var(--ink-dim)}
.summary{margin:16px 0 20px; max-width:64ch}

/* The spine: four fixed cells, same order on every card, so the answer reads
   down the page as well as across one board. */
.bearers{display:grid; grid-template-columns:repeat(4,1fr); gap:1px;
  background:var(--rule); border:1px solid var(--rule); border-radius:9px;
  overflow:hidden; margin-bottom:20px}
.bearer{background:var(--raised); padding:11px 8px; text-align:center; cursor:help}
.bearer-mark{display:block; font-size:17px; line-height:1.1}
.bearer-name{font-size:10px; font-weight:700; letter-spacing:.11em;
  text-transform:uppercase; color:var(--ink-dim)}
.b-yes .bearer-mark{color:var(--ok)}
.b-no .bearer-mark{color:var(--absent)}
.b-untested .bearer-mark{color:var(--partial)}
.b-no .bearer-name{opacity:.55}

.specs{display:grid; grid-template-columns:repeat(auto-fit,minmax(210px,1fr));
  gap:0 26px; margin:0 0 20px}
.spec{display:flex; gap:12px; justify-content:space-between; align-items:baseline;
  padding:7px 0; border-bottom:1px solid var(--rule)}
.spec dt{font-size:11px; letter-spacing:.11em; text-transform:uppercase;
  color:var(--ink-dim); font-family:Archivo,sans-serif; font-weight:600;
  white-space:nowrap}
.spec dd{margin:0; font-size:13px; text-align:right}

.shots{display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
  gap:12px; margin-bottom:20px}
.shot{margin:0}
.shot img{width:100%; height:150px; object-fit:cover; border-radius:8px;
  border:1px solid var(--rule); background:var(--sunk); display:block}
.shot figcaption{font-size:11.5px; color:var(--ink-dim); line-height:1.45;
  padding-top:7px}
.credit{display:block; opacity:.65; font-size:10.5px; font-style:italic}

.build{background:var(--sunk); border-radius:9px; padding:16px 18px}
.build-head{font-size:10.5px; letter-spacing:.13em; text-transform:uppercase;
  font-weight:700; color:var(--accent); margin-bottom:9px}
.build-none .build-head{color:var(--partial)}
.build pre{margin:0 0 10px; font-size:12.5px; line-height:1.75; overflow-x:auto;
  font-family:"JetBrains Mono",ui-monospace,monospace}
.flashnote{margin:0; font-size:13px; color:var(--ink-dim); max-width:70ch}
.card-foot{display:flex; flex-wrap:wrap; gap:16px; margin-top:16px;
  padding-top:14px; border-top:1px solid var(--rule)}
.doclink{font-size:13px; text-decoration:none; border-bottom:1px solid transparent}
.doclink:hover{border-bottom-color:currentColor}

.legend{display:flex; flex-wrap:wrap; gap:18px; padding:26px 0 0;
  border-top:1px solid var(--rule); margin-top:34px; font-size:12.5px;
  color:var(--ink-dim)}
.legend b{font-weight:700; font-style:normal}
.foot{color:var(--ink-dim); font-size:13px; padding-top:16px; max-width:70ch}
@media (max-width:640px){
  .bearers{grid-template-columns:repeat(2,1fr)}
  .card{padding:20px 18px}
}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
"""

JS = """
const btns=[...document.querySelectorAll('.fbtn')];
const cards=[...document.querySelectorAll('.card')];
function apply(){
  const on=btns.filter(b=>b.getAttribute('aria-pressed')==='true')
               .map(b=>b.dataset.bearer);
  cards.forEach(c=>{
    const has=(c.dataset.bearers||'').split(' ').filter(Boolean);
    c.hidden = on.length>0 && !on.every(b=>has.includes(b));
  });
  const n=cards.filter(c=>!c.hidden).length;
  document.getElementById('shown').textContent=n;
}
btns.forEach(b=>b.addEventListener('click',()=>{
  b.setAttribute('aria-pressed', b.getAttribute('aria-pressed')==='true'?'false':'true');
  apply();
}));
apply();
"""


def build(embed):
    boards = load_boards()
    fams = sorted({(b.get("silicon") or {}).get("family") for b in boards} - {None})
    shipping = sum(1 for b in boards if b.get("status") == "shipping")

    filters = "".join(
        f'<button class="fbtn" data-bearer="{k}" aria-pressed="false">{lbl}</button>'
        for k, lbl in BEARERS)

    return f"""<title>XPRS Board Catalogue</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@600;700;800&family=Newsreader:opsz,wght@6..72,400;6..72,500&family=JetBrains+Mono:wght@400;500&display=swap">
<style>{CSS}</style>

<header class="top"><div class="top-in">
  <div>
    <p class="eyebrow">XPRS firmware</p>
    <h1>Every board the fleet speaks for</h1>
    <p class="blurb">One station, many radios. Each board below runs the same
      XPRS station over whatever radios it happens to have — pick by what it
      can reach, not by what it costs.</p>
  </div>
  <div class="counts">
    <div class="count"><b id="shown">{len(boards)}</b><span>shown</span></div>
    <div class="count"><b>{shipping}</b><span>shipping</span></div>
    <div class="count"><b>{len(fams)}</b><span>chip families</span></div>
  </div>
</div></header>

<div class="wrap">
  <div class="filters">
    <span class="lbl">Must carry</span>{filters}
  </div>
  <main class="grid">{''.join(card(b, embed) for b in boards)}</main>

  <div class="legend">
    <span><b style="color:var(--ok)">●</b> carries it, measured on hardware</span>
    <span><b style="color:var(--partial)">◐</b> the chip can, the code cannot yet</span>
    <span><b style="color:var(--absent)">—</b> the hardware cannot, ever</span>
  </div>
  <p class="foot">Generated from <span class="mono">models/*/board.yml</span> by
    <span class="mono">tools/scripts/build_catalog.py</span> — never edited by hand,
    so it cannot disagree with the boards. Hover a bearer for the reason behind it.
    Photographs are credited to their source on each card.</p>
</div>
<script>{JS}</script>
"""


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "docs", "catalog.html"))
    ap.add_argument("--embed", action="store_true",
                    help="inline photographs as downscaled data: URIs")
    a = ap.parse_args()
    html = build(a.embed)
    open(a.out, "w").write(html)
    print(f"{a.out}: {len(html):,} bytes, {len(load_boards())} boards"
          f"{' (images embedded)' if a.embed else ''}")

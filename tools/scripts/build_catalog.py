#!/usr/bin/env python3
"""Build the hardware catalogue page from models/*/board.yml.

    tools/scripts/build_catalog.py [-o index.html] [--embed]
                                   [--json docs/boards.json]
                                   [--template-json docs/board.template.json]

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
REPO = "xprs-dev/firmware"
REPO_URL = f"https://github.com/{REPO}"
RAW_URL = f"https://raw.githubusercontent.com/{REPO}/main"
XPRS_ROLES = [("beacon", "Beacon"), ("digipeater", "Digipeater"), ("bridge", "Bridge"),
              ("igate", "iGate"), ("hotspot", "Hotspot"), ("api", "API"),
              ("indexer", "Indexer"), ("share", "Share"), ("reticulum", "Reticulum"),
              ("ota", "OTA"), ("gossip", "Gossip"), ("dashboard", "Dashboard"),
              ("chat", "Chat"), ("mesh_session", "Mesh session"), ("vhf", "VHF")]
HW_TAGS = [("screen", "Screen"), ("lora_radio", "LoRa radio"), ("vhf", "VHF radio"),
           ("gnss", "GNSS"), ("battery", "Battery")]


def card_tags(b):
    """Everything a filter button can ask for, as one space-separated list."""
    tags = [k for k, _ in BEARERS if (b.get("bearers") or {}).get(k) == "yes"]
    io_, rad, phys = b.get("io") or {}, b.get("radios") or {}, b.get("physical") or {}
    if isinstance(io_.get("screen"), dict):
        tags.append("screen")
    if rad.get("lora"):
        tags.append("lora_radio")
    if any("sa818" in (o.get("name") or "").lower() for o in (rad.get("other") or [])):
        tags.append("vhf")
    if io_.get("gnss"):
        tags.append("gnss")
    if "battery" in (phys.get("power") or ""):
        tags.append("battery")
    return " ".join(tags)


FLASH_SECTION = {"esp32": ("flash-esp32", "ESP32 over USB-serial"),
                 "esp32s3": ("flash-esp32s3", "ESP32-S3 / C3 over native USB"),
                 "esp32c3": ("flash-esp32s3", "ESP32-S3 / C3 over native USB"),
                 "nrf52": ("flash-nrf52", "nRF52 by UF2 drag-and-drop")}
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


def to_json(b):
    """One board as the catalogue JSON: board.yml verbatim, plus what a
    program outside this tree cannot derive -- where the folder is on GitHub
    and an absolute URL for every image."""
    rel = b.get("_rel") or f"models/{b.get('id')}"
    d = {k: v for k, v in b.items() if not k.startswith("_")}
    out = {"repo": REPO, "source_url": f"{REPO_URL}/tree/main/{rel}"}
    out.update(d)
    fw = out.get("firmware") or {}
    if fw.get("project"):
        fw["project_url"] = f"{REPO_URL}/tree/main/{fw['project']}"
    if fw.get("artifact"):
        fw["artifact_url"] = f"{RAW_URL}/{fw['artifact']}"
    for key in ("images", "screenshots"):
        for im in out.get(key) or []:
            if im.get("file"):
                im["image_url"] = f"{RAW_URL}/{rel}/{im['file']}"
    for doc in out.get("docs") or []:
        u = doc.get("url") or ""
        if u and "://" not in u:
            doc["url"] = f"{REPO_URL}/blob/main/{u}"
    return out


def prebuilt_block(b, fw):
    """Download links and, where an ESP Web Tools manifest exists, the
    install button. Generated from models/<id>/prebuilt/ by
    collect_prebuilt.py, so what is offered is what is in the tree."""
    pre = os.path.join(b["_dir"], "prebuilt")
    rel = f"{b['_rel']}/prebuilt"
    if not os.path.isdir(pre):
        return ""
    files = sorted(f for f in os.listdir(pre) if not f.startswith("."))
    if not files:
        return ""
    manifest = os.path.join(pre, "manifest.json")
    ver, button = None, ""
    if os.path.isfile(manifest):
        m = json.load(open(manifest))
        ver = m.get("version")
        button = (f'<esp-web-install-button manifest="{esc(rel)}/manifest.json">'
                  f'<span slot="unsupported">Web flashing needs Chrome or Edge.</span>'
                  f'<span slot="not-allowed">Web flashing needs this page served over HTTPS.</span>'
                  f'</esp-web-install-button>')
    elif fw.get("version"):
        ver = fw["version"]
    links = "".join(
        f'<a class="dl mono" href="{esc(rel)}/{esc(f)}" download>{esc(f)}'
        f'<span>{os.path.getsize(os.path.join(pre, f)) // 1024} KB</span></a>'
        for f in files)
    head = "Prebuilt image" + (f' <span class="mono">v{esc(ver)}</span>' if ver else "")
    return (f'<div class="prebuilt"><div class="build-head">{head}</div>'
            f'{button}<div class="dls">{links}</div></div>')


def tile(b, embed):
    """One gallery tile: photo, name, one line, status. Links to the card."""
    im = next((i for i in (b.get("images") or []) if i.get("file")), None)
    src = ""
    if im:
        abspath = os.path.join(b["_dir"], im["file"])
        if os.path.isfile(abspath):
            src = thumb_data_uri(abspath, 320) if embed else f"{b['_rel']}/{im['file']}"
    pic = (f'<img src="{esc(src)}" alt="" loading="lazy">' if src
           else '<div class="tile-nopic mono">no photo</div>')
    sil = b.get("silicon") or {}
    fam = FAMILY_LABEL.get(sil.get("family"), sil.get("mcu") or "?")
    return (f'<a class="tile" href="#{esc(b.get("id"))}" data-tags="{esc(card_tags(b))}" '
            f'data-status="{esc(b.get("status"))}">'
            f'<div class="tile-pic">{pic}</div>'
            f'<div class="tile-body"><div class="tile-name">{esc(b.get("name"))}'
            f'<span class="chip chip-status s-{esc(b.get("status"))}">{esc(b.get("status"))}</span></div>'
            f'<div class="tile-line">{esc(b.get("tagline") or "")}</div>'
            f'<div class="tile-meta mono">{esc(fam)} · {esc(b.get("vendor"))}</div></div></a>')


def xprs_chips(b):
    xp = b.get("xprs") or {}
    cells = []
    for k, label in XPRS_ROLES:
        v = xp.get(k) or "untested"
        note = xp.get(k + "_note") or ""
        cells.append(f'<span class="role role-{esc(v)}"'
                     f'{f" title={json.dumps(note)}" if note else ""}>{esc(label)}</span>')
    return "".join(cells)


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
    for im in (b.get("images") or []):
        f = im.get("file")
        if not f:
            continue
        abspath = os.path.join(b["_dir"], f)
        if not os.path.isfile(abspath):
            continue
        src = thumb_data_uri(abspath) if embed else f"{b['_rel']}/{f}"
        full = src if embed else f"{b['_rel']}/{f}"
        imgs += (f'<figure class="shot"><a class="shot-link" href="{esc(full)}" '
                 f'data-caption="{esc(im.get("caption"))}" data-credit="{esc(im.get("credit"))}">'
                 f'<img src="{esc(src)}" alt="{esc(im.get("caption"))}" loading="lazy"></a>'
                 f'<figcaption>{esc(im.get("caption"))}'
                 f'<span class="credit">{esc(im.get("credit"))}</span></figcaption></figure>')
    if imgs:
        imgs = f'<div class="shots">{imgs}</div>'

    docs = "".join(
        f'<a class="doclink" href="{esc(d.get("url"))}">{esc(d.get("title"))}</a>'
        for d in (b.get("docs") or []) if d.get("url"))

    sec_id, sec_label = FLASH_SECTION.get(sil.get("family"), ("flash-source", "from source"))
    if fw.get("project"):
        build = (f'<div class="build"><div class="build-head">Build from source</div>'
                 f'<pre class="mono">cd {esc(fw["project"])}\n'
                 f'~/.platformio/penv/bin/pio run{" -e " + esc(fw["env"]) if fw.get("env") else ""}\n'
                 f'~/.platformio/penv/bin/pio run{" -e " + esc(fw["env"]) if fw.get("env") else ""} -t upload</pre>'
                 f'<p class="flashnote">Flashing: <a href="#{sec_id}">{esc(sec_label)}</a>.</p></div>')
    else:
        build = (f'<div class="build build-none"><div class="build-head">No firmware yet</div>'
                 f'<p class="flashnote">Not buildable from this tree.</p></div>')
    build += prebuilt_block(b, fw)

    ver = (f'<span class="ver mono">v{esc(fw["version"])}</span>'
           if fw.get("version") else "")

    return f"""
<article class="card" id="{esc(b.get('id'))}" data-status="{esc(status)}"
         data-tags="{esc(card_tags(b))}">
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
  <div class="roles" aria-label="XPRS roles">{xprs_chips(b)}</div>
  {imgs}
  {build}
  <footer class="card-foot">{docs}<a class="doclink" href="{REPO_URL}/tree/main/{esc(b['_rel'])}">Source folder</a></footer>
</article>"""


CSS = """
.gallery{display:grid; grid-template-columns:repeat(auto-fill,minmax(230px,1fr)); gap:14px;
  padding-top:26px}
.tile{display:flex; flex-direction:column; text-decoration:none; color:var(--ink);
  background:var(--raised); border:1px solid var(--rule); border-radius:12px; overflow:hidden;
  box-shadow:var(--shadow); transition:border-color .15s, transform .15s}
.tile:hover{border-color:var(--accent); transform:translateY(-2px)}
.tile[hidden]{display:none}
.tile-pic{aspect-ratio:4/3; background:#fff; display:flex; align-items:center; justify-content:center;
  border-bottom:1px solid var(--rule)}
.tile-pic img{width:100%; height:100%; object-fit:contain; padding:8px}
.tile-nopic{color:var(--ink-dim); font-size:12px}
.tile-body{padding:12px 14px 14px}
.tile-name{font-family:Archivo,"Helvetica Neue",Arial,sans-serif; font-weight:700; font-size:15px;
  display:flex; justify-content:space-between; align-items:center; gap:8px}
.tile-name .chip{font-size:10px; padding:1px 7px}
.tile-line{font-size:14px; line-height:1.4; margin-top:6px; color:var(--ink)}
.tile-meta{font-size:11px; color:var(--ink-dim); margin-top:8px}
.card{scroll-margin-top:16px}
.prebuilt{margin-top:12px; padding:14px 16px; border:1px dashed var(--rule); border-radius:10px}
.dls{display:flex; flex-wrap:wrap; gap:8px; margin-top:10px}
.dl{font-size:12px; padding:4px 10px; border:1px solid var(--rule); border-radius:8px;
  background:var(--sunk); text-decoration:none; color:var(--ink)}
.dl span{color:var(--ink-dim); margin-left:6px}
.dl:hover{border-color:var(--accent)}
esp-web-install-button{--esp-tools-button-color:var(--accent);
  --esp-tools-button-text-color:var(--ground); --esp-tools-button-border-radius:999px}
.shot-link{display:block; cursor:zoom-in}
.lb{position:fixed; inset:0; background:rgba(0,0,0,.88); display:flex; flex-direction:column;
  align-items:center; justify-content:center; z-index:50; padding:24px}
.lb[hidden]{display:none}
.lb img{max-width:min(96vw,1400px); max-height:78vh; object-fit:contain; background:#fff; border-radius:6px}
.lb .lb-cap{color:#f0f0f0; font-size:15px; margin:14px 0 0; text-align:center; max-width:80ch}
.lb .lb-credit{color:#948c84; font-size:12px; display:block; margin-top:4px}
.lb button,.lb a.lb-dl{position:absolute; font:inherit; font-size:14px; font-weight:600;
  background:rgba(255,255,255,.12); color:#f0f0f0; border:1px solid rgba(255,255,255,.3);
  border-radius:999px; padding:8px 14px; cursor:pointer; text-decoration:none}
.lb button:hover,.lb a.lb-dl:hover{background:rgba(255,255,255,.25)}
.lb .lb-prev{left:16px; top:50%; transform:translateY(-50%)}
.lb .lb-next{right:16px; top:50%; transform:translateY(-50%)}
.lb .lb-close{right:16px; top:16px}
.lb a.lb-dl{left:16px; top:16px}
.lb .lb-n{position:absolute; bottom:16px; color:#948c84; font-size:12px}
.flash{margin-top:56px; padding-top:28px; border-top:1px solid var(--rule)}
.flash h2{font-size:24px; margin-bottom:6px}
.flash h3{font-size:16px; margin:26px 0 6px}
.flash p{margin:6px 0; max-width:78ch}
.flash pre{background:var(--sunk); padding:12px 14px; border-radius:8px; overflow-x:auto;
  font-size:13px; max-width:78ch}
.flash ul{max-width:78ch; padding-left:22px}
.roles{display:flex;flex-wrap:wrap;gap:6px;margin:12px 0 0}
.role{font-family:Archivo,"Helvetica Neue",Arial,sans-serif;font-size:11px;
  letter-spacing:.04em;padding:2px 8px;border-radius:999px;border:1px solid var(--rule);
  color:var(--ink-dim);background:var(--sunk)}
.role-yes{color:var(--ok);border-color:var(--ok)}
.role-untested,.role-planned{color:var(--partial);border-color:var(--partial)}
.role-no{opacity:.55;text-decoration:line-through}
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
.filters .lbl-2{margin-left:14px}
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
(function(){
  const lb=document.getElementById('lb'); if(!lb) return;
  const img=lb.querySelector('img'), cap=lb.querySelector('.lb-cap'), cred=lb.querySelector('.lb-credit'),
        dl=lb.querySelector('.lb-dl'), n=lb.querySelector('.lb-n');
  let group=[], i=0;
  function show(k){
    i=(k+group.length)%group.length; const a=group[i];
    img.src=a.href; img.alt=a.dataset.caption||''; cap.textContent=a.dataset.caption||'';
    cred.textContent=a.dataset.credit||''; dl.href=a.href; dl.download=a.href.split('/').pop();
    n.textContent=(i+1)+' / '+group.length; lb.hidden=false;
  }
  document.querySelectorAll('.card').forEach(card=>{
    const links=[...card.querySelectorAll('.shot-link')];
    links.forEach((a,k)=>a.addEventListener('click',e=>{e.preventDefault(); group=links; show(k);}));
  });
  lb.querySelector('.lb-prev').onclick=()=>show(i-1);
  lb.querySelector('.lb-next').onclick=()=>show(i+1);
  lb.querySelector('.lb-close').onclick=()=>lb.hidden=true;
  lb.addEventListener('click',e=>{ if(e.target===lb) lb.hidden=true; });
  document.addEventListener('keydown',e=>{
    if(lb.hidden) return;
    if(e.key==='Escape') lb.hidden=true;
    else if(e.key==='ArrowLeft') show(i-1);
    else if(e.key==='ArrowRight') show(i+1);
  });
})();
const btns=[...document.querySelectorAll('.fbtn')];
const cards=[...document.querySelectorAll('.card, .tile')];
function apply(){
  const on=btns.filter(b=>b.getAttribute('aria-pressed')==='true')
               .map(b=>b.dataset.tag);
  cards.forEach(c=>{
    const has=(c.dataset.tags||'').split(' ').filter(Boolean);
    c.hidden = on.length>0 && !on.every(b=>has.includes(b));
  });
  const n=cards.filter(c=>!c.hidden && c.classList.contains('card')).length;
  document.getElementById('shown').textContent=n;
}
btns.forEach(b=>b.addEventListener('click',()=>{
  b.setAttribute('aria-pressed', b.getAttribute('aria-pressed')==='true'?'false':'true');
  apply();
}));
apply();
"""


FLASHING = """
<section class="flash" id="flashing">
  <h2>Flashing</h2>
  <p>Each card links to the one of these that applies. The prebuilt images are
    whatever <span class="mono">tools/scripts/collect_prebuilt.py</span> last copied
    out of a <span class="mono">pio run</span>; the version is printed beside them.</p>

  <h3 id="flash-web">From this page (ESP32 boards)</h3>
  <p>The <b>Install</b> button on a card uses
    <a href="https://esphome.github.io/esp-web-tools/">ESP Web Tools</a>: the browser
    talks to the board over Web Serial and writes bootloader, partition table and
    application at the offsets in the board's <span class="mono">manifest.json</span>.
    It needs Chrome or Edge, and this page served over HTTPS (it does not work from a
    <span class="mono">file://</span> URL, and Firefox and Safari have no Web Serial).
    Tick "erase" on a board that ran something else before; it clears NVS and the OTA
    selector. On Linux the user must be in the <span class="mono">dialout</span> group.</p>

  <h3 id="flash-esp32">ESP32 over USB-serial</h3>
  <p>Original ESP32 boards (M5Stack Core, Heltec V1/V2, kv4p, DevKitC) have a
    CP210x or CH340 USB-serial bridge. It shows up as
    <span class="mono">/dev/ttyUSB0</span> on Linux, <span class="mono">COMx</span> on
    Windows. Most auto-reset into the bootloader; if esptool reports "Failed to
    connect", hold BOOT, tap EN/RST, release BOOT. The bootloader goes at
    <span class="mono">0x1000</span> on this chip.</p>
  <pre class="mono">pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash \\
  0x1000  bootloader.bin \\
  0x8000  partitions.bin \\
  0x20000 firmware.bin</pre>
  <p>Add <span class="mono">--erase-all</span> the first time, or after a different firmware.</p>

  <h3 id="flash-esp32s3">ESP32-S3 / ESP32-C3 over native USB</h3>
  <p>The S3 and C3 boards here (T-Dongle, T-Deck, Heltec V3, DevKitM-1, the
    e-paper board) use the chip's own USB-JTAG-serial port: no bridge chip, and it
    appears as <span class="mono">/dev/ttyACM0</span> or under
    <span class="mono">/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*</span>.
    Bootloader at <span class="mono">0x0</span>. If the port does not appear, hold BOOT
    while plugging in. The T-Dongle's cable and hub combination loses the link with
    the flasher stub, so its project pins <span class="mono">--no-stub</span> and
    115200 baud; the T-Deck does not need that.</p>
  <pre class="mono">esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash \\
  0x0     bootloader.bin \\
  0x8000  partitions.bin \\
  0x20000 firmware.bin</pre>
  <p>Use <span class="mono">--chip esp32c3</span> for the C3. With two S3 boards on
    one machine, use the by-id path; the projects do, so
    <span class="mono">pio run -t upload</span> cannot hit the wrong one.</p>

  <h3 id="flash-nrf52">nRF52 by UF2 drag-and-drop</h3>
  <p>The SenseCAP P1-Pro's XIAO nRF52840 has an Adafruit-style UF2 bootloader. Tap
    reset twice, quickly; a USB drive named <span class="mono">XIAO-SENSE</span>
    (or similar) mounts. Copy <span class="mono">firmware.uf2</span> onto it; the board
    reboots into the new image when the copy finishes. No driver, no tool, any OS. If
    the drive does not appear, the second tap was too slow or the board is not in
    reach of the reset button through the case: the button row is on the underside
    (see the photos).</p>

  <h3 id="flash-source">From source</h3>
  <p>Every card's "Build from source" block is the exact command pair. The projects
    pin their upload port by USB serial number in <span class="mono">platformio.ini</span>;
    on a different machine change <span class="mono">upload_port</span> or pass
    <span class="mono">--upload-port</span>. Multiboard targets marked legacy may not
    compile today; <span class="mono">multiboard/README.md</span> keeps the table.</p>
</section>
"""


def build(embed):
    boards = load_boards()
    fams = sorted({(b.get("silicon") or {}).get("family") for b in boards} - {None})
    shipping = sum(1 for b in boards if b.get("status") == "shipping")

    def buttons(pairs):
        return "".join(
            f'<button class="fbtn" data-tag="{k}" aria-pressed="false">{lbl}</button>'
            for k, lbl in pairs)
    filters = (f'<span class="lbl">Bearer</span>{buttons(BEARERS)}'
               f'<span class="lbl lbl-2">Hardware</span>{buttons(HW_TAGS)}')

    return f"""<title>XPRS Board Catalogue</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@600;700;800&family=Newsreader:opsz,wght@6..72,400;6..72,500&family=JetBrains+Mono:wght@400;500&display=swap">
<style>{CSS}</style>
<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js"></script>

<header class="top"><div class="top-in">
  <div>
    <p class="eyebrow">xprs-dev/firmware</p>
    <h1>Supported boards</h1>
    <p class="blurb">Each card is one folder under <span class="mono">models/</span>:
      what the board is, which XPRS bearers it carries, what the station on it
      does, how to build and flash it. Filter by bearer or by hardware.</p>
  </div>
  <div class="counts">
    <div class="count"><b id="shown">{len(boards)}</b><span>shown</span></div>
    <div class="count"><b>{shipping}</b><span>shipping</span></div>
    <div class="count"><b>{len(fams)}</b><span>chip families</span></div>
  </div>
</div></header>

<div class="wrap">
  <nav class="gallery" aria-label="Boards">{''.join(tile(b, embed) for b in boards)}</nav>

  <div class="filters">
    {filters}
  </div>
  <main class="grid">{''.join(card(b, embed) for b in boards)}</main>

  {FLASHING}

  <div class="legend">
    <span><b style="color:var(--ok)">●</b> yes, tested on hardware</span>
    <span><b style="color:var(--partial)">◐</b> untested, or hardware supports it but firmware does not yet</span>
    <span><b style="color:var(--absent)">—</b> not possible on this hardware</span>
  </div>
  <p class="foot">Generated from <span class="mono">models/*/board.yml</span> by
    <span class="mono">tools/scripts/build_catalog.py</span>. Do not edit this file;
    edit the board's <span class="mono">board.yml</span> and rebuild. Hover a bearer or
    a role for the note behind it. Photo credits are on each card; the same data is
    in <span class="mono">docs/boards.json</span>.</p>
</div>
<div class="lb" id="lb" hidden>
  <a class="lb-dl" href="#" download>Download</a>
  <button class="lb-close" type="button">Close (Esc)</button>
  <button class="lb-prev" type="button" aria-label="previous">&larr;</button>
  <img src="" alt="">
  <p class="lb-cap"></p><span class="lb-credit"></span>
  <button class="lb-next" type="button" aria-label="next">&rarr;</button>
  <span class="lb-n mono"></span>
</div>
<script>{JS}</script>
"""


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "index.html"))
    ap.add_argument("--json", default=None,
                    help="also write every board as one JSON array")
    ap.add_argument("--template-json", default=None,
                    help="also write models/_template/board.yml as JSON")
    ap.add_argument("--embed", action="store_true",
                    help="inline photographs as downscaled data: URIs")
    a = ap.parse_args()
    html = build(a.embed)
    open(a.out, "w").write(html)
    if a.json:
        boards = [to_json(b) for b in load_boards()]
        open(a.json, "w").write(json.dumps(boards, indent=2, ensure_ascii=False) + "\n")
    if a.template_json:
        t = yaml.safe_load(open(os.path.join(ROOT, "models", "_template", "board.yml"))) or {}
        t["_rel"] = "models/<id>"
        t["id"] = "<id>"
        open(a.template_json, "w").write(json.dumps(to_json(t), indent=2, ensure_ascii=False) + "\n")
    print(f"{a.out}: {len(html):,} bytes, {len(load_boards())} boards"
          f"{' (images embedded)' if a.embed else ''}")

#!/bin/bash
# Build the reobell-BAKED D340W firmware pak (the 4664 deployment).
#
# Unlike the SD-boot hook (see MODIFYING.md), this bakes the whole reobell
# payload into the rootfs at /reobell/ and launches it from start_app, so the
# camera runs the camera-side XPRS daemon with no SD card. The reobell payload
# is bigger than the stock 153-PEB rootfs slot, so the pak is REPACKED with a
# larger rootfs section: the app section is shifted forward, the section table
# (rootfs length, app start) is rewritten, and the pak header CRC recomputed.
#
# Run inside the reobell-mtd image (see Dockerfile), as root, MKNOD capable:
#   docker build -t reobell-mtd models/reolink-d340w/firmware/tool
#   docker run --rm --cap-add MKNOD \
#     -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
#     -e VER=4664_2509161282 reobell-mtd \
#     /repo/models/reolink-d340w/firmware/tool/build_reobell_pak.sh
#
# PAYLOAD (default /repo/models/reolink-d340w/firmware/payload/reobell) is copied verbatim into
# the rootfs. The repo copy keeps PASSWORD=CHANGE_ME. To run against a real
# camera, stage a copy of that folder OUTSIDE the repo with the real admin
# password in its `config`, and point PAYLOAD at it -- never commit the password.
set -e
W=/work
REPO=/repo
STOCK="${STOCK:-$REPO/models/reolink-d340w/firmware/stock/DB_566128M5MP_W.4662_2508071282.Reolink-Video-Doorbell-WiFi.OV05A10.5MP.WIFI8812.REOLINK.pak}"
PAYLOAD="${PAYLOAD:-$REPO/models/reolink-d340w/firmware/payload/reobell}"
VER="${VER:-4664_2509161282}"
OUT="${OUT:-$W/reobell_baked.pak}"
# Stock pak geometry (verified with pakler; see flashing.md section 2).
ROOTFS_START=2159592; APP_START=22213608; APP_LEN=19529728
ROOTFS_PART=33554432   # rootfs mtd partition size = the max the new UBI may be

mkdir -p "$W"
# Preflight: inputs must exist and be what we expect.
[ -f "$STOCK" ] || { echo "FAIL: stock pak not found: $STOCK"; exit 2; }
MAGIC=$(head -c4 "$STOCK" | od -An -tx1 | tr -d ' \n')
[ "$MAGIC" = "13597232" ] || { echo "FAIL: stock pak bad magic ($MAGIC)"; exit 2; }
for f in reobell boot.sh xprsbell.sh config; do
  [ -e "$PAYLOAD/$f" ] || { echo "FAIL: payload missing $PAYLOAD/$f"; exit 2; }
done
if grep -q 'CHANGE_ME' "$PAYLOAD/config" 2>/dev/null; then
  echo "WARN: PAYLOAD config still has PASSWORD=CHANGE_ME. The built pak will not"
  echo "      log into the camera api. Stage a copy OUTSIDE the repo with the real"
  echo "      password and pass -e PAYLOAD=/that/path. Continuing anyway."
fi
command -v mkfs.ubifs >/dev/null || { echo "FAIL: mkfs.ubifs missing; run in the reobell-mtd image"; exit 2; }
pip install -q --break-system-packages python-lzo >/dev/null 2>&1 || true

# 1. Carve the stock rootfs UBI, extract it as root (keeps perms/owners/nodes).
python3 - "$STOCK" "$W/rootfs.ubi" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
open(sys.argv[2],'wb').write(d[2159592:22213608])
PY
rm -rf "$W/rootfs_ex"
ubireader_extract_files -k -o "$W/rootfs_ex" "$W/rootfs.ubi" >/dev/null 2>&1
R=$(find "$W/rootfs_ex" -maxdepth 2 -name rootfs -type d | head -1)
[ -n "$R" ] || { echo "rootfs tree not found"; exit 1; }

# 2. Bake the reobell payload and launch it from start_app.
#    40s delay lets the network and local api.cgi come up first. boot.sh
#    generates the device key once (persisted at /mnt/para/reobell.key), starts
#    the signed presence beacon, and supervises the ring poller (xprsbell.sh).
cp -a "$PAYLOAD" "$R/reobell"
chmod +x "$R/reobell/reobell" "$R/reobell/boot.sh" "$R/reobell/xprsbell.sh"
cat >> "$R/etc/init.d/start_app" <<'HOOK'

# ---- reobell: camera-side XPRS (baked in rootfs) ----
( sleep 40; /reobell/boot.sh ) >/mnt/tmp/reobell_boot.log 2>&1 &
HOOK

# 3. Rebuild the rootfs UBIFS + UBI (D340W-proven geometry, lzo).
mkfs.ubifs -m 2048 -e 126976 -c 239 -x lzo -f 8 -k r5 -p 1 -l 4 -r "$R" -o "$W/_r.ubifs"
printf '[rootfs]\nmode = ubi\nimage = %s\nvol_type = dynamic\nvol_id = 0\nvol_name = rootfs\nvol_alignment = 1\n' \
  "$W/_r.ubifs" > "$W/_r.ini"
ubinize -p 131072 -m 2048 -O 2048 -s 2048 -x 1 -Q 153830686 -o "$W/rootfs_full.ubi" "$W/_r.ini"
RN=$(stat -c%s "$W/rootfs_full.ubi")
echo "new rootfs ubi $RN bytes ($((RN/131072)) PEBs); partition max $ROOTFS_PART"
[ "$RN" -le "$ROOTFS_PART" ] || { echo "rootfs exceeds mtd partition"; exit 1; }

# 4. Repack: bigger rootfs section, app shifted, section table + header CRC fixed.
STOCK="$STOCK" OUT="$OUT" VER="$VER" \
ROOTFS_START=$ROOTFS_START APP_START=$APP_START APP_LEN=$APP_LEN \
ROOTFS_UBI="$W/rootfs_full.ubi" python3 - <<'PY'
import os, struct, zlib, lzo, pakler
stock=open(os.environ["STOCK"],"rb").read()
newroot=open(os.environ["ROOTFS_UBI"],"rb").read()
RS=int(os.environ["ROOTFS_START"]); AS=int(os.environ["APP_START"]); AL=int(os.environ["APP_LEN"])
VER=os.environ["VER"].encode()
delta=len(newroot)-(AS-RS)
appdata=bytearray(stock[AS:AS+AL])
# version_file (uncompressed UBIFS data node): payload at pak 26499728.
P_vf=26499728-AS; N=P_vf-48
assert bytes(appdata[P_vf:P_vf+16])==b"4662_2508071282\n", "version_file moved"
appdata[P_vf:P_vf+16]=VER+b"\n"
L=struct.unpack_from("<I",appdata,N+16)[0]
struct.pack_into("<I",appdata,N+4,(zlib.crc32(bytes(appdata[N+8:N+L]))^0xffffffff)&0xffffffff)
# version.json (lzo UBIFS data node) at app offset 2346520, node len 285.
Nj=2346520; Lj=285
js=lzo.decompress(bytes(appdata[Nj+48:Nj+Lj]),False,781)
nv=js.replace(b"4662_2508071282",VER); chosen=None
for pad in range(0,80):
    for ch in (b"\n",b" ",b"\t"):
        c=nv+ch*pad
        if len(lzo.compress(c,9,False))==237: chosen=c; break
    if chosen: break
comp=lzo.compress(chosen,9,False); assert len(comp)==237, "cannot repad version.json to 237"
appdata[Nj+48:Nj+48+237]=comp
struct.pack_into("<I",appdata,Nj+40,len(chosen))
struct.pack_into("<I",appdata,Nj+4,(zlib.crc32(bytes(appdata[Nj+8:Nj+Lj]))^0xffffffff)&0xffffffff)
newpak=bytearray(stock[:RS]+newroot+bytes(appdata)+stock[AS+AL:])
# Section table: entry i at HEADER_HEADER_SIZE + i*SECTION_SIZE; start@+56 len@+60.
hhs=pakler.HEADER_HEADER_SIZE; ss=pakler.SECTION_SIZE
struct.pack_into("<I",newpak,hhs+6*ss+60,len(newroot))   # rootfs (sec6) length
struct.pack_into("<I",newpak,hhs+7*ss+56,RS+len(newroot)) # app (sec7) start
# Pak header CRC (non-standard crc32, see flashing.md section 2) over the new pak.
ent=bytes(newpak)[hhs:hhs+13*ss]
hc=(zlib.crc32(bytes(newpak)[1832:]+b"\x02\x00\x00\x00"+ent,0xffffffff)^0xffffffff)&0xffffffff
struct.pack_into("<I",newpak,pakler.HEADER_CRC_OFFSET,hc)
open(os.environ["OUT"],"wb").write(newpak)
print("repacked: rootfs len %d, app start %d, delta %d, size %d, header 0x%08x"
      %(len(newroot),RS+len(newroot),delta,len(newpak),hc))
pak=pakler.PAK.from_file(os.environ["OUT"])   # self-verify: parses + CRC ok
for s in pak.sections:
    if s.name in ("rootfs","app"): print("  ",s.name,"start",s.start,"len",s.len)
PY
ls -l "$OUT"
echo "Rename to DB_566128M5MP_W.$VER.Reolink-Video-Doorbell-WiFi.OV05A10.5MP.WIFI8812.REOLINK.pak before web upload."

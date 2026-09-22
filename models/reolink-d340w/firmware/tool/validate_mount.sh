#!/bin/bash
# Fast pre-flash check: does the built pak's rootfs mount on a real Linux MTD
# stack (nandsim), and is the reobell launch line present? This is the quick
# gate; the authoritative one is validate_update_qemu.sh (runs the camera's own
# `update` flasher). Neither touches the camera.
#
# Needs a REAL kernel with mtd/ubifs, so run --privileged with host modules:
#   docker run --rm --privileged -v /lib/modules:/lib/modules:ro \
#     -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
#     reobell-mtd /repo/models/reolink-d340w/firmware/tool/validate_mount.sh /work/reobell_baked.pak
set +e
PAK="${1:?usage: validate_mount.sh <pak>}"
[ -f "$PAK" ] || { echo "FAIL: pak not found: $PAK"; exit 2; }
MAGIC=$(head -c4 "$PAK" | od -An -tx1 | tr -d ' \n')
[ "$MAGIC" = "13597232" ] || { echo "FAIL: bad pak magic ($MAGIC), not a D340W pak"; exit 2; }
modprobe ubifs 2>/dev/null
grep -qw ubifs /proc/filesystems || { echo "FAIL: no ubifs in this kernel; run --privileged with -v /lib/modules:/lib/modules:ro"; exit 2; }
W=/work; mkdir -p "$W"
# Carve the rootfs UBI out of the pak by its section-table entry (robust to the
# repacked layout where the app section has moved).
ROOTFS_UBI="$W/_val_rootfs.ubi"
PAK="$PAK" OUT="$ROOTFS_UBI" python3 - <<'PY'
import os, pakler
pak=pakler.PAK.from_file(os.environ["PAK"])
d=open(os.environ["PAK"],"rb").read()
s=next(s for s in pak.sections if s.name=="rootfs")
open(os.environ["OUT"],"wb").write(d[s.start:s.start+s.len])
print("carved rootfs section: start %d len %d (%d PEBs)"%(s.start,s.len,s.len//131072))
PY

modprobe ubi 2>/dev/null; modprobe ubifs 2>/dev/null
rmmod nandsim 2>/dev/null
modprobe nandsim id_bytes=0x2c,0xda,0x90,0x95 2>/dev/null
for n in $(seq 0 5); do
  mknod /dev/mtd$n c 90 $((2*n)) 2>/dev/null
  mknod /dev/mtd${n}ro c 90 $((2*n+1)) 2>/dev/null
done
[ -e /dev/ubi_ctrl ] || mknod /dev/ubi_ctrl c 10 "$(grep ubi_ctrl /proc/misc | awk '{print $1}')" 2>/dev/null
MTD=$(grep -i "NAND simulator" /proc/mtd | cut -d: -f1); MNUM=${MTD#mtd}
echo "== nandsim $MTD =="; cat /proc/mtd
flash_erase /dev/$MTD 0 0 >/dev/null 2>&1
ubiformat /dev/$MTD -f "$ROOTFS_UBI" -O 2048 -y 2>&1 | tail -2
ubiattach /dev/ubi_ctrl -m "$MNUM" -O 2048 2>&1 | tail -1
# make the ubi0_0 node from sysfs (missing node reads as MOUNT FAILED -22)
for p in /sys/class/ubi/ubi0/dev /sys/class/ubi/ubi0_0/dev; do
  [ -f "$p" ] || continue; MM=$(cat "$p")
  case "$p" in */ubi0/dev) mknod /dev/ubi0 c ${MM%%:*} ${MM##*:} 2>/dev/null;;
               */ubi0_0/dev) mknod /dev/ubi0_0 c ${MM%%:*} ${MM##*:} 2>/dev/null;; esac
done
mkdir -p /mnt/t
mount -t ubifs /dev/ubi0_0 /mnt/t 2>&1 | tail -1
RC=1
if mountpoint -q /mnt/t; then
  echo ">>> ROOTFS MOUNTED OK"
  if grep -q '/reobell/boot.sh' /mnt/t/etc/init.d/start_app 2>/dev/null; then
    echo ">>> reobell launch line present in start_app"; ls -la /mnt/t/reobell/ 2>/dev/null | head
    RC=0
  else
    echo "!!! reobell launch line MISSING from start_app"
  fi
  umount /mnt/t
else
  echo ">>> ROOTFS MOUNT FAILED"
fi
ubidetach /dev/ubi_ctrl -m "$MNUM" 2>/dev/null; rmmod nandsim 2>/dev/null
echo "== dmesg UBIFS =="; dmesg 2>/dev/null | grep -iE "ubifs|ubi0" | tail -8
exit $RC

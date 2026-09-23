#!/bin/bash
# AUTHORITATIVE pre-flash check: run the camera's OWN static ARM flasher,
# `update 0 <pak> all`, against the built pak inside qemu-system-arm, and assert
# it reports "update rootfs success" and "update app success" with no
# crc/magic/board/version error. If this passes, the camera accepts the pak.
# Nothing here touches the real camera.
#
# Why full-system qemu (not qemu-user): `update` does MTD ioctls that qemu-user
# cannot emulate ("ioctl(): Function not implemented"). We boot a real Debian
# armhf 6.1 kernel with nandsim as the flash, and run `update` from an initramfs.
#
# Build the qemu image (adds qemu-system-arm + qemu-user to reobell-mtd):
#   printf 'FROM reobell-mtd\nRUN apt-get update -qq && apt-get install -y -qq \
#     qemu-system-arm qemu-user-static\n' | docker build -t reobell-qemu -
#   docker run --rm --cap-add MKNOD \
#     -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
#     reobell-qemu /repo/models/reolink-d340w/firmware/tool/validate_update_qemu.sh /work/reobell_baked.pak
set -e
PAK="${1:?usage: validate_update_qemu.sh <pak>}"
[ -f "$PAK" ] || { echo "FAIL: pak not found: $PAK"; exit 2; }
# pak magic 0x32725913 little-endian = 13 59 72 32
MAGIC=$(head -c4 "$PAK" | od -An -tx1 | tr -d ' \n')
[ "$MAGIC" = "13597232" ] || { echo "FAIL: bad pak magic ($MAGIC), not a D340W pak"; exit 2; }
command -v qemu-system-arm >/dev/null || { echo "FAIL: qemu-system-arm missing; build the reobell-qemu image (see header)"; exit 2; }
W=/work; Q="$W/qemu"; mkdir -p "$Q"
# KVER can age out of the Debian archive; override with -e KVER=6.1.0-<n>-armmp.
KVER="${KVER:-6.1.0-47-armmp}"

# 1. The camera's `update` binary: extract it from the pak's app UBIFS (once).
if [ ! -f "$Q/update" ]; then
  rm -rf "$Q/app_ex"
  APP="$W/_val_app.ubi"
  PAK="$PAK" OUT="$APP" python3 - <<'PY'
import os, pakler
pak=pakler.PAK.from_file(os.environ["PAK"]); d=open(os.environ["PAK"],"rb").read()
s=next(s for s in pak.sections if s.name=="app")
open(os.environ["OUT"],"wb").write(d[s.start:s.start+s.len])
PY
  ubireader_extract_files -k -o "$Q/app_ex" "$APP" >/dev/null 2>&1
  UPD=$(find "$Q/app_ex" -name update -type f | head -1)
  [ -n "$UPD" ] || { echo "FAIL: 'update' flasher not found in the pak's app fs"; exit 2; }
  cp "$UPD" "$Q/update"; chmod +x "$Q/update"
fi

# 2. Debian armhf kernel + its matching nandsim.ko (once). Uses apt on armhf.
if [ ! -f "$Q/vmlinuz" ] || [ ! -f "$Q/nandsim.ko" ]; then
  dpkg --add-architecture armhf; apt-get update -qq
  ( cd "$Q" && rm -f linux-image-*_armhf.deb && apt-get download "linux-image-$KVER:armhf" ) \
    || { echo "FAIL: apt could not download linux-image-$KVER:armhf."; \
         echo "      It likely aged out of the archive. Pick a current armhf"; \
         echo "      kernel (apt-cache search linux-image | grep armmp) and re-run"; \
         echo "      with -e KVER=<that version>. nandsim.ko is taken from the same"; \
         echo "      deb, so it stays matched."; exit 3; }
  dpkg-deb -x "$Q"/linux-image-$KVER*_armhf.deb "$Q/kdeb"
  cp "$Q/kdeb/boot/vmlinuz-$KVER" "$Q/vmlinuz"
  KO="$Q/kdeb/lib/modules/$KVER/kernel/drivers/mtd/nand/raw/nandsim.ko"
  [ -f "$KO" ] || { echo "FAIL: nandsim.ko not in this kernel deb at $KO"; exit 3; }
  cp "$KO" "$Q/nandsim.ko"
fi

# 3. Static busybox for the initramfs (once). ARMHF, like the kernel: this
# image is amd64, so an unqualified `apt-get download busybox-static` fetches
# an x86-64 binary, the guest boots and then sits in silence because it cannot
# execute /init, and the run dies on the timeout looking exactly like a kernel
# that would not boot. Asserted below rather than trusted.
if [ ! -f "$Q/busybox" ]; then
  dpkg --add-architecture armhf; apt-get update -qq
  ( cd "$Q" && rm -f busybox-static_*.deb && apt-get download busybox-static:armhf ) \
    || { echo "FAIL: apt could not download busybox-static:armhf"; exit 3; }
  dpkg-deb -x "$Q"/busybox-static_*armhf.deb "$Q/bbdeb"
  cp "$Q/bbdeb/bin/busybox" "$Q/busybox"
fi
case "$(od -An -tx1 -j18 -N2 "$Q/busybox" | tr -d ' ')" in
  2800) ;;   # EM_ARM
  *) echo "FAIL: $Q/busybox is not an ARM binary; delete $Q and re-run"; exit 3 ;;
esac

# 4. Build the initramfs: busybox + update + nandsim.ko + the pak + init.
IR="$Q/ir"; rm -rf "$IR"; mkdir -p "$IR/bin"
cp "$Q/busybox" "$IR/bin/busybox"; ln -sf busybox "$IR/bin/sh"
cp "$Q/update" "$IR/update"; cp "$Q/nandsim.ko" "$IR/nandsim.ko"; cp "$PAK" "$IR/p.pak"
cat > "$IR/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sys /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
echo "=BOOT= loading nandsim"
/bin/busybox insmod /nandsim.ko id_bytes=0x2c,0xda,0x90,0x95 access_delay=0 programm_delay=0 erase_delay=0
echo "=MTD="; /bin/busybox cat /proc/mtd
/bin/busybox mknod /dev/mtd12 c 90 0
/bin/busybox mknod /dev/mtd12ro c 90 1
echo "=RUN update 0 /p.pak all="
/update 0 /p.pak all
echo "=UPDATE_EXIT=$?="
/bin/busybox sync; /bin/busybox poweroff -f
INIT
chmod +x "$IR/init"
( cd "$IR" && find . | cpio -o -H newc 2>/dev/null | gzip > "$Q/initramfs.cpio.gz" )

# 5. Boot qemu-system-arm -M virt, capture the console.
LOG="$Q/run.log"
timeout 240 qemu-system-arm -M virt -m 2048 -smp 2 -nographic \
  -kernel "$Q/vmlinuz" -initrd "$Q/initramfs.cpio.gz" \
  -append "console=ttyAMA0 rdinit=/init panic=1" > "$LOG" 2>&1 || true

if ! grep -q '=RUN update 0' "$LOG"; then
  echo "FAIL: qemu did not reach the update step (boot problem or timeout)."
  echo "      Inspect $LOG. If the kernel did not boot, KVER may be wrong."
  exit 3
fi
echo "===== update result ====="
grep -iE 'update (loader|fdt|uboot|kernel|rootfs|app) (success|err)' "$LOG" || true
echo "========================="
# Pass = rootfs AND app success, and no crc/magic/board/version reject.
if grep -q 'update rootfs success' "$LOG" && grep -q 'update app success' "$LOG" \
   && ! grep -qiE 'crc.*(err|not correct)|magic.*err|board.*err|version.*err' "$LOG"; then
  echo ">>> PASS: the camera's update flasher accepts this pak."
  exit 0
else
  echo ">>> FAIL: inspect $LOG (rootfs/app not both success, or a reject appeared)."
  echo "    (fdt/uenv/sp/ep/NOUSE 'not found in img' lines are absent partitions on the"
  echo "     single nandsim device and are EXPECTED; only rootfs+app matter.)"
  exit 1
fi

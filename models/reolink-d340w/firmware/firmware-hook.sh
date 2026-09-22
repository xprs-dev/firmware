
# ---- reobell: run an editable boot script from the SD card, if present ----
# Injected at the end of rootfs /etc/init.d/start_app. Minimal and defensive:
# waits (up to ~5 min) for the SD card to mount, runs <SD>/reobell/boot.sh once,
# and does nothing if it is absent. All camera-side XPRS logic lives on the SD
# card, so it can be changed without ever reflashing the firmware again.
(
  _i=0
  while [ $_i -lt 150 ]; do
    for _sd in /mnt/sd /mnt/sda /mnt/sda1 /mnt/sdcard; do
      if [ -x "$_sd/reobell/boot.sh" ]; then
        exec "$_sd/reobell/boot.sh"
      fi
    done
    _i=$((_i + 1))
    sleep 2
  done
) >/mnt/tmp/reobell_boot.log 2>&1 &

# After the link: firmware.uf2 for the T1000-E-BOOT drive, and a size gate.
#
# The UF2 is what a person copies onto the card's boot drive (family
# 0xADA52840, application at 0x27000 above SoftDevice S140 7.x). The gate is
# the OTA's: update.cpp copies images through three 256 KB slots, so an image
# that outgrows one installs by cable and never again over the air. 240 KB
# leaves a margin for the next change to notice before it is too late.
Import("env")
import os, subprocess, sys

LIMIT = 240 * 1024

APP_BASE = "0x27000"

def make_uf2(source, target, env):
    """From a raw binary with the base given, NOT from the hex.

    The core's uf2conv.py ignores the hex's type-02 (segment address)
    records, and the GNU hex this link produces uses one (":02000002 2000"
    = 0x20000, plus 0x7000 in the data records). Converted from the hex, the
    image landed at 0x2000 -- on top of the SoftDevice -- and a person
    copying it onto the boot drive would have bricked the radio stack. A
    binary has no addresses to misread: the base is stated, once, here."""
    hexf = str(source[0])
    elf = os.path.splitext(hexf)[0] + ".elf"
    binf = os.path.splitext(hexf)[0] + ".app.bin"
    uf2 = os.path.splitext(hexf)[0] + ".uf2"
    objcopy = env.subst("$OBJCOPY")
    r = subprocess.run([objcopy, "-O", "binary", "-R", ".heap", elf, binf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        env.Exit(1)
    fw = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")
    conv = os.path.join(fw, "tools", "uf2conv", "uf2conv.py")
    r = subprocess.run([sys.executable, conv, binf, "-c", "-b", APP_BASE,
                        "-f", "0xADA52840", "-o", uf2], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(uf2):
        print(r.stdout + r.stderr)
        env.Exit(1)
    size = os.path.getsize(binf)
    print("t1000e: %s (%d bytes of image, limit %d)" % (os.path.basename(uf2), size, LIMIT))
    if size > LIMIT:
        print("t1000e: IMAGE TOO LARGE for a 256 KB OTA slot")
        env.Exit(1)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", make_uf2)

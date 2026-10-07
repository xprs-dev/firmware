# The version the image carries, from version.txt beside this file -- the
# string an approval names (xprsfw1 <board> <version> ...) and the one the
# station reports as fw:. One source, so the two cannot drift.
Import("env")
import os
v = open(os.path.join(env["PROJECT_DIR"], "version.txt")).read().strip()
env.Append(CPPDEFINES=[("XPRS_FW_VERSION", '\\"%s\\"' % v)])

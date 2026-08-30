# The station UI INTERFACE, and nothing else: xprs_ui.h, no implementation.
#
# There are two of those. xprs_ui draws the seven-panel dashboard on a
# 320x240 panel; xprs_ui_mini draws three rotating views on the T-Dongle's
# 160x80 strip, which xui_init() in the big one refuses outright (it wants
# 160x120 at least). Both answer to the same ~30 xui_* calls, so xprs_app
# -- which is the whole point, one station on every board -- includes this
# header and never learns which of them it is talking to.
#
# The BOARD chooses, by naming one implementation in its own component's
# REQUIRES. Naming both would give the linker two definitions of every
# symbol here and it would say so, which is the right way for that mistake
# to be found.
idf_component_register(INCLUDE_DIRS ".")

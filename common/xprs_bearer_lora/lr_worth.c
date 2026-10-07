/* What deserves LoRa airtime (lr_worth.h). Moved out of xprs_app.c, where
 * the reasoning was first written down, so the cards apply the same rule. */
#include "lr_worth.h"

#include <string.h>

/* Since the move to Meshtastic's LongFast a frame is one to two seconds on
 * the one EU channel both networks share, and XPRS.md 30.1 binds unsolicited
 * traffic to the strictest bearer a station transmits on. A bench of BLE and
 * LAN stations beaconing every minute, bridged onto that channel, kept two
 * stations at their full 10% and the channel busy enough that a Meshtastic
 * node's DM never got through (2026-09-19). So LoRa carries what somebody
 * is waiting for -- a message, a receipt, a call for help, a command and its
 * result, a key to verify them with -- and leaves presence to the bearers
 * that are cheap. t: is always first, so this is a token walk. */
bool lr_worth(const char *wire, int len, bool xprs_channel)
{
    if (xprs_channel) return true;
    static const char *const carried[] = {
        "message", "sos", "warning", "receipt", "reaction", "command",
        "result", "identity", "mailbox", "file", "request",
    };
    if (len < 3 || wire[0] != 't' || wire[1] != ':') return false;
    int n = 0;
    while (2 + n < len && wire[2 + n] != ' ') n++;
    for (size_t i = 0; i < sizeof carried / sizeof carried[0]; i++)
        if ((int)strlen(carried[i]) == n && memcmp(wire + 2, carried[i], (size_t)n) == 0)
            return true;
    return false;
}

bool lr_scope_local(const char *wire, int len)
{
    static const char k[] = " scope:local";
    const int kl = (int)sizeof k - 1;
    for (int i = 0; i + kl <= len; i++) {
        if (wire[i] != ' ') continue;
        if (i + 3 < len && wire[i + 1] == 'm' && wire[i + 2] == ':') break;
        if (memcmp(wire + i, k, (size_t)kl) == 0 &&
            (i + kl == len || wire[i + kl] == ' '))
            return true;
    }
    return false;
}

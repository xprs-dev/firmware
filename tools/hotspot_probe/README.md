# hotspot_probe

An ESP32-C3 used as test equipment for the walk-up chat. It scans for every
open `XPRS-*` hotspot in reach, joins each in turn, takes an address from its
DHCP, and fetches from the gateway the chat page, `/api/status` and
`/api/services`. One line per hotspot on its USB console, a round every two
minutes:

```
PROBE XPRS-X3UYPS OK rssi=-32 tries=1 ip=192.168.4.2 gw=192.168.4.1 portal=200 page=200/48010 status=200 call=X3UYPS services=200/452
PROBE ROUND 20 done 2/2
```

Before that line come the chat page's own first requests, timed, one line
each (`PROBE <ssid> PAGE <url> code=<c> len=<bytes> ms=<t>`): the page shows
"connecting" until the first history answer lands, so a board that serves
40 rows in a second but 200 in five is visible here. Every request carries
the headers an Android WebView sends; a server whose header buffer is too
small for a phone fails here with 431 as it does on the phone.

Then a burst: six of those requests in flight at once from three tasks,
as a WebView opens them (`PROBE <ssid> BURST <url> code=<c> ms=<t>`, then
`BURST OK|FAIL worst=<ms>`; FAIL past 8 s, the page's own timeout). The
association is tried up to three times like a phone's supplicant does,
each miss printed with its disconnect reason, and `tries=` in the summary
line says how many it took.

It exists so a host can check every board's hotspot without leaving its own
WiFi network: a Raspberry Pi whose internet is its WiFi would go offline the
moment it joined a station's hotspot.

```sh
~/.platformio/penv/bin/pio run -e c3probe -t upload --upload-port /dev/serial/by-id/<the C3>
```

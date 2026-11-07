# hotspot_probe

An ESP32-C3 used as test equipment for the walk-up chat. It scans for every
open `XPRS-*` hotspot in reach, joins each in turn, takes an address from its
DHCP, and fetches from the gateway the chat page, `/api/status` and
`/api/services`. One line per hotspot on its USB console, a round every two
minutes:

```
PROBE XPRS-X3UYPS OK rssi=-32 ip=192.168.4.2 gw=192.168.4.1 portal=200 page=200/48010 status=200 call=X3UYPS services=200/452
PROBE ROUND 20 done 2/2
```

It exists so a host can check every board's hotspot without leaving its own
WiFi network: a Raspberry Pi whose internet is its WiFi would go offline the
moment it joined a station's hotspot.

```sh
~/.platformio/penv/bin/pio run -e c3probe -t upload --upload-port /dev/serial/by-id/<the C3>
```

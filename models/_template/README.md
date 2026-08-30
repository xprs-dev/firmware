# <Vendor> <Board>

One paragraph: what this board IS, and what job it does in the fleet. Not a
spec dump -- `board.yml` holds the specs and a program reads them. This is
for a person who is about to work on it.

## What is in here

| | |
|---|---|
| Chip | |
| `firmware/` | its own project, or "a `multiboard` target" |
| `hardware/` | pinout, block diagram, photographs |
| `docs/` | anything true of this board and of no other |

## Building it

```sh
cd firmware
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
```

## What this board cannot do

The most useful section, and the one most often missing. A radio the chip
does not have, a screen too small for the shared dashboard, a heap that will
not hold the station -- say it here, with the measurement, so the next
person does not spend an afternoon rediscovering it.

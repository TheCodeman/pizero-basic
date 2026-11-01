# Pi Zero BASIC

Ken Segler's [pico-basic](https://github.com/TheCodeman/pico-basic) running
bare metal (no OS) on the Raspberry Pi Zero W (BCM2835, ARM1176JZF-S), with
the console on both HDMI and the serial port, a USB keyboard, and its files on
the SD card.

This build is GPLv3, because the USB keyboard comes from the USPi library.

## The plan

1. **Boot and serial console** (done): the start-up code, the MMU and caches,
   the full ARM clock, the PL011 serial console and newlib.
2. **BASIC on the SD card** (done): `basic.c`, FatFs, the editors and XMODEM
   on a new `basic_port_rpi.c`, with the SD card (EMMC controller) as the drive.
3. **HDMI and USB keyboard**: a 640x480 mailbox framebuffer with the
   interpreter's screen features, and USPi for the keyboard. Text goes to
   both HDMI and serial, and keys come from either one.
4. **Hardware commands**: PIN, PWM and sound on GPIO18, SPI, I2C, COM1,
   WATCHDOG and TEMP().

Wi-Fi comes later.

## Building

You need `arm-none-eabi-gcc` with newlib (Debian/Ubuntu:
`sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi`).

```bash
make            # build/kernel.img
make build/sd.img   # a 64 MB FAT32 card image with the examples, for QEMU
make qemu       # runs it in QEMU's raspi0 model (with build/sd.img if it's there)
```

In QEMU, Ctrl-A X quits.

## The SD card

Format a card as FAT32 and copy these files to it:

- `bootcode.bin`, `start.elf` and `fixup.dat` from the `boot/` folder of
  [raspberrypi/firmware](https://github.com/raspberrypi/firmware/tree/master/boot)
- `sdcard/config.txt` from this repo
- `build/kernel.img`

## The serial console

Use a 3.3 V USB-serial adapter, 115200 baud, 8N1:

| Zero W pin | To the adapter |
|---|---|
| 8 (GPIO14, TX) | RX |
| 10 (GPIO15, RX) | TX |
| 6 (GND) | GND |

Then run `picocom -b 115200 /dev/ttyUSB0` (ANSI or VT100 with UTF-8, at
least 80 x 30, as for pico-basic). You get the banner and BASIC's `Ok`.

`config.txt` takes the PL011 back from Bluetooth (`dtoverlay=disable-bt`) and
fixes its clock at 48 MHz (`init_uart_clock`).

## Using it

It is pico-basic, so its manual holds
([BASIC_MANUAL.md](https://github.com/TheCodeman/pico-basic/blob/main/docs/BASIC_MANUAL.md)),
with these differences:

- **Files** are on the SD card, the one drive (`SD:`), next to the
  firmware's own files. `SAVE`, `LOAD`, `FILES`, `OPEN`, `MKDIR`, `KILL` and
  the rest work as on the Pico. `AUTORUN.BAS` in the card's root runs at
  power-up (Esc or Ctrl-C within a second skips it).
- **Settings** are in `SD:/BASIC.CFG` (the Pico's `CONFIG.TXT`, renamed,
  because the card's `config.txt` is the firmware's): `TZ`, `CLOCK`,
  `WATCHDOG` and so on.
- **Getting files on and off:** paste a listing at the `Ok` prompt, or use
  `XRECV` and `XSEND` (XMODEM, YMODEM, ZMODEM) as on the Pico. Or put the
  card in the PC.
- **`EDIT`** opens the full-screen editor on the terminal.
- **Pins:** `PIN` and `PIN MODE` work on GPIO 0-27 of the header (14 and 15
  are the console). `PIN LED` is the green ACT LED (GPIO47). `TEMP()` is the
  SoC temperature.
- **Not yet:** the screen (graphics, `SCREEN`, `IMAGE`, the mouse) comes in
  step 3; `PWM`, `BEEP`/`SOUND`/`PLAY`, `I2C`, `SPI` and `COM1` in step 4.
  The Pi has no ADC (`ADC()` gives -1) and no PIO (no `NEOPIXEL`). Wi-Fi
  comes later.
- The Pi has no clock that keeps time while it is off: `DATE$` and `TIME$`
  start at 1 Jan 1970 until a program sets them.
- **Memory:** about 450 MB for BASIC.

## The layout

| Path | What's there |
|---|---|
| `boot/start.S` | the entry point: stacks, vectors, VFP, BSS |
| `boot/link.ld` | loads at 0x8000; the stacks, then the heap up to the top of the ARM's RAM |
| `src/rpi/` | BCM2835 drivers: GPIO, system timer, UART, mailbox, MMU, reset, faults, newlib syscalls |
| `src/rpi/emmc.c` | the SD card slot (the EMMC controller) |
| `src/sd_disk.c` | FatFs's disk functions on the card, and its clock |
| `src/console.c` | the console: serial now, HDMI and keyboard from step 3 |
| `src/basic_port_rpi.c` | `basic_port.h` for the Pi: what the interpreter needs from the board |
| `src/main.c` | board start-up, the banner, AUTORUN.BAS, BASIC's prompt |
| `src/basic/`, `src/fatfs/`, `src/term_editor.c`, `src/xfer.c`, `src/config.c` | pico-basic's own sources (MIT), as they are there |
| `src/compat/` | the few Pico SDK calls those use, on the Pi's timer |

The files from pico-basic are copied unchanged, except for FatFs's volume
names (just `SD`) in `ffconf.h` and a `PIZERO` case in `config.c`. Keep them
in step with pico-basic by copying them over again.

# Pi Zero BASIC

Ken Segler's [pico-basic](https://github.com/TheCodeman/pico-basic) running
bare metal (no OS) on the Raspberry Pi Zero W (BCM2835, ARM1176JZF-S), with
the console on both HDMI and the serial port, a USB keyboard, and its files on
the SD card.

This build is GPLv3, because the USB keyboard comes from the USPi library.

## The plan

1. **Boot and serial console** (done): the start-up code, the MMU and caches,
   the full ARM clock, the PL011 serial console and newlib.
2. **BASIC on the SD card**: `basic.c`, FatFs, the editors and XMODEM on a
   new `basic_port_rpi.c`, with the SD card (EMMC controller) as the drive.
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
make qemu       # runs it in QEMU's raspi0 model, the console on this terminal
```

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

Then run `picocom -b 115200 /dev/ttyUSB0`. With step 1 you get a banner (the
board revision, ARM clock and memory, and a floating-point check), then a
prompt that echoes each line back. `REBOOT` restarts the board.

`config.txt` takes the PL011 back from Bluetooth (`dtoverlay=disable-bt`) and
fixes its clock at 48 MHz (`init_uart_clock`).

## The layout

| Path | What's there |
|---|---|
| `boot/start.S` | the entry point: stacks, vectors, VFP, BSS |
| `boot/link.ld` | loads at 0x8000; the stacks, then the heap up to the top of the ARM's RAM |
| `src/rpi/` | BCM2835 drivers: GPIO, system timer, UART, mailbox, MMU, reset, faults, newlib syscalls |
| `src/console.c` | the console: serial now, HDMI and keyboard from step 3 |
| `src/main.c` | board start-up and (for now) the echo loop |

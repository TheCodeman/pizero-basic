/**
 * @file basic_port.h
 * @brief What the interpreter needs from the machine.
 *
 * basic_port_pico.c implements it for the Pico; a PC test harness can
 * implement it too. Output goes through printf/putchar and files through
 * FatFs.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Next key, or -1 if none arrives within timeout_us (0 = don't wait).
/// @param timeout_us how long to wait (0: don't)
/// @return the key, or -1
int port_key(uint32_t timeout_us);

/// Plays a beep of `freq_hz` for `ms` milliseconds (BEEP, SOUND).
/// @param freq_hz the frequency
/// @param ms how long
void port_beep(unsigned freq_hz, unsigned ms);
/// PLAY's background queue: adds a note (freq 0: a rest), false if it's
/// full; counts the notes not yet finished; stops and empties it.
/// @param freq_hz the note's frequency (0: a rest)
/// @param ms how long
/// @return false if the queue is full
bool port_play(unsigned freq_hz, unsigned ms);
/// How many notes PLAY has queued that haven't finished.
/// @return the notes queued or playing
int port_play_pending(void);
/// Stops the notes PLAY queued.
void port_play_stop(void);
/// SHELL "command": runs a shell command line. False if it can't be run
/// from BASIC.
/// @param command the command line
/// @return false if it can't run from BASIC
bool port_shell(const char *command);
/// The text screen's cells: a character and a PC attribute byte (foreground
/// + 16 * background). Rows 0-29, columns 0-79.
/// @param row the row
/// @param col the column
/// @param ch set to the character
/// @param attr set to its attribute
void port_get_cell(int row, int col, uint8_t *ch, uint8_t *attr);
/// Puts a character and attribute in a text cell.
/// @param row the row
/// @param col the column
/// @param ch the character
/// @param attr its attribute
void port_set_cell(int row, int col, uint8_t ch, uint8_t attr);
/// Shows c's picture at the cursor though it is a control code (CHR$(1) is a
/// smiley); output so far has been flushed.
/// @param c the character code
void port_put_glyph(int c);
/// Waits `ms` milliseconds.
/// @param ms milliseconds
void port_sleep_ms(unsigned ms);

/// The mouse: where it is in screen pixels (0-639, 0-479) and its buttons
/// (1 left, 2 right, 4 middle); false if none is plugged in. The wheel's
/// clicks since the last call (+ up). The pointer shown over the text
/// screen, or not.
/// @param x set to x
/// @param y set to y
/// @param buttons set to the buttons held
/// @return false if no mouse is connected
bool port_mouse(int *x, int *y, int *buttons);
/// The mouse wheel's movement since the last call (MOUSEW).
/// @return the wheel's clicks, + up
int port_mouse_wheel(void);
/// KEYDOWN(): whether the key with PC scan code `scan` (as INKEY$ gives for
/// cursor keys: 72 Up, 75 Left, 77 Right, 80 Down; 57 Space, 30 A...) is held
/// down on the USB keyboard now. Scan code 0: whether a USB keyboard is there
/// at all (the serial console can't tell).
/// @param scan the PC scan code
/// @return true while the key is held
bool port_key_down(int scan);
/// Shows or hides the mouse pointer on the text screen (MOUSE ON/OFF).
/// @param on shown or hidden
void port_mouse_pointer(bool on);

/// MUSIC: a MOD song playing in the background (mixed with PLAY, SOUND and
/// BEEP). play loads and starts it (NULL, or what went wrong); the rest
/// stop it, pause or resume it, set its volume (0-100), jump to a position,
/// and read its state: 0 playing (-1/0), 1 the position, 2 the row, 3 the
/// pattern.
/// @param path the song
/// @param loop start over at the end
/// @return NULL, or why it can't play
const char *port_music_play(const char *path, bool loop);
/// Stops the music MUSIC plays.
void port_music_stop(void);
/// Pauses or resumes the music.
/// @param paused pause or resume
void port_music_pause(bool paused);
/// Sets the music's volume, 0-100%.
/// @param percent 0-100
void port_music_volume(int percent);
/// Moves the music to order position `order`, row `row`.
/// @param order the order position
/// @param row the row
void port_music_pos(int order, int row);
/// The music's state: `what` 0 playing (-1/0), 1 the order position, 2 the row, 3 the pattern.
/// @param what 0 playing, 1 the order position, 2 the row, 3 the pattern
/// @return the value asked for
int port_music_state(int what);

/// Seconds since start-up, for TIMER.
/// @return seconds
double port_seconds(void);

/// Free heap, for FRE().
/// @return free bytes
size_t port_free_mem(void);

/// The screen's size in pixels and in 8x16 text cells. basic.c has weak
/// versions giving 640x480 and 80x30, for ports whose screen is that.
///@{
int port_screen_width(void);
int port_screen_height(void);
int port_text_cols(void);
int port_text_rows(void);
///@}

/// SCREEN RES w, h: makes the screen w x h pixels (the text grid follows):
/// NULL, or why not. With w 0: the monitor's own size. basic.c's weak version
/// says the port can't.
const char *port_screen_res(int w, int h);
/// SCREEN RES alone: says the screen's size (and what it can be).
void port_screen_res_show(void);

/// Graphics on the RGB332 screen (coordinates already clipped).
/// @param x the left
/// @param y the top
/// @param w the width
/// @param h the height
/// @param rgb332 the colour
void port_fill_rect(int x, int y, int w, int h, uint8_t rgb332);

/// Screen pages (SCREEN mode,, apage, vpage): drawing (port_fill_rect,
/// port_get_pixel, port_image) goes to page `active` while the screen shows
/// page `visual`; pages 0-7. Returns NULL, or why not (out of memory).
/// @param active the page drawn on
/// @param visual the page shown
/// @return NULL, or why not
const char *port_set_pages(int active, int visual);
/// Back to just page 0, shown, and frees the other pages.
void port_reset_pages(void);
/// PCOPY: copies screen page `from` over page `to`. NULL, or why not.
/// @param from the page copied
/// @param to the page overwritten
/// @return NULL, or why not
const char *port_copy_page(int from, int to);
/// The colour of the pixel at x, y (POINT).
/// @param x the x
/// @param y the y
/// @return the colour (RGB332)
uint8_t port_get_pixel(int x, int y);

/// True when the calling core's stack is nearly used up.
/// @return true when the stack is nearly used up
bool port_stack_low(void);

/// Opens `path` in the full-screen editor and returns when it is closed.
/// @param path the file
void port_edit(const char *path);

/// WIFI [ON | OFF | SCAN] (what: 0 the status, 1 on, 2 off, 3 scan) and NTP
/// (what: 4); false if the build has no Wi-Fi.
/// @param what which
/// @return the build has it
bool port_wifi(int what);

/// WIFI(n): 0 connected (-1, else 0), 1 the signal in dBm (0: none), 2 the
/// clock set by NTP (-1, else 0), 3 the link's state (CYW43_LINK_*); 0 for
/// all without Wi-Fi.
/// @param n which
/// @return it
int port_wifi_value(int n);

/// WIFI$(n): 0 the IP address, 1 the network's name ("" if none).
/// @param n which
/// @return it
const char *port_wifi_text(int n);

/// CLOCK = 12 in CONFIG.TXT: TIME$ gives 12-hour time with AM or PM.
/// @return it does
bool port_clock_12h(void);

/// MQTT (Wi-Fi builds; see mqtt.h): whether this build has it.
/// @return it has
bool port_mqtt_available(void);
/// MQTT CONNECT [host$[, port]]: NULL, or why it didn't connect.
const char *port_mqtt_connect(const char *host, int port);
/// MQTT DISCONNECT.
void port_mqtt_disconnect(void);
/// MQTT PUBLISH: NULL, or why it wasn't sent.
const char *port_mqtt_publish(const char *topic, const char *payload, int len, int qos, bool retain);
/// MQTT SUBSCRIBE / UNSUBSCRIBE: NULL, or why not.
const char *port_mqtt_subscribe(const char *topic, int qos, bool sub);
/// MQTT: the status.
void port_mqtt_status(void);
/// MQTT(n): 0 connected (-1/0), 1 messages waiting, 2 dropped; 0 without MQTT.
int port_mqtt_value(int n);
/// MQTT$(0): takes the next message's topic ("" if none); MQTT$(1): its payload.
/// @param n 0 or 1
/// @param len set to the length
/// @return the text
const char *port_mqtt_text(int n, int *len);

/// WATCHDOG ms (arm, 100 ms up to port_watchdog_max) or 0 (off).
/// @param ms the timeout
void port_watchdog_set(int ms);

/// WATCHDOG FEED: reloads it.
void port_watchdog_feed(void);

/// The timeout while it's armed, else 0.
/// @return ms
int port_watchdog_ms(void);

/// The longest timeout the chip has (8388 ms on the RP2040, 16777 on the RP2350).
/// @return ms
int port_watchdog_max(void);

/// WDRESET: the last reset was the watchdog's (it ran out).
/// @return it was
bool port_watchdog_reset(void);
/// The same for a text file that isn't the program (EDIT "file").
/// @param path the file
void port_edit_file(const char *path);

// ---- GPIO (PINMODE, PIN, PWM, SERVO, ADC(), TEMP(), PINS) -------------------

/// A GPIO pin's mode for PIN MODE.
enum { PORT_PIN_INPUT, PORT_PIN_OUTPUT, PORT_PIN_PULLUP, PORT_PIN_PULLDOWN };

/// @return the GPIO count
int port_pin_count(void);        ///< GPIOs are 0 .. count-1
/// @return the pin, or -1 if none
int port_led_pin(void);          ///< the board's LED
/// NULL if BASIC may use the pin, else what the system uses it for.
/// @param pin the GPIO
/// @return NULL, or what the system uses it for
const char *port_pin_reserved(int pin);
/// Sets GPIO pin `pin` to `mode`.
/// @param pin the GPIO
/// @param mode a PORT_PIN_ mode
void port_pin_mode(int pin, int mode);
/// @param pin the GPIO
/// @param level 0 or 1
void port_pin_write(int pin, int level);  ///< makes the pin an output if it isn't
/// Reads GPIO pin `pin`: 0 or 1.
/// @param pin the GPIO
/// @return 0 or 1
int port_pin_read(int pin);
/// PWM at freq Hz (0 = off) and duty 0-100 %. Prints a warning if the pin's
/// partner on the same PWM slice was running at another frequency.
/// @param pin the GPIO
/// @param freq the frequency in Hz (0: off)
/// @param duty the duty cycle, 0-100%
void port_pwm(int pin, double freq, double duty);
/// @param pin the GPIO
/// @return 0-4095, or -1 if the pin has no ADC
int port_adc(int pin);           ///< 0-4095, or -1 if the pin has no ADC
/// @return degrees C
double port_temperature(void);   ///< chip temperature, degrees C
void port_pins_list(void);       ///< what each pin is doing
void port_pins_reset(void);      ///< every pin BASIC touched back to input, PWM off

// ---- I2C and SPI (the chip's controllers) -------------------------------------
/// Opens I2C with SDA on an even pin and SCL on the next (the pins pick I2C0
/// or I2C1). NULL if done, else why not. Writes and reads return the bytes
/// moved, -1 if no device answered, -2 if I2C isn't open.
/// @param sda the SDA pin (even)
/// @param scl the SCL pin (sda + 1)
/// @param khz the clock in kHz
/// @return NULL, or why not
const char *port_i2c_open(int sda, int scl, int khz);
/// Stops I2C and frees its pins.
void port_i2c_close(void);
/// Writes `n` bytes to I2C device `addr`; with nostop the bus is kept. <0 on error.
/// @param addr the device (0-127)
/// @param data the bytes
/// @param n how many
/// @param nostop keep the bus for a read that follows
/// @return the bytes written, or < 0 on error
int port_i2c_write(int addr, const uint8_t *data, int n, bool nostop);
/// Reads `n` bytes from I2C device `addr`. <0 on error.
/// @param addr the device (0-127)
/// @param data where the bytes go
/// @param n how many
/// @return the bytes read, or < 0 on error
int port_i2c_read(int addr, uint8_t *data, int n);
/// SPI on SCK, MOSI and MISO pins of one SPI (mode 0-3, 8-bit words, MSB
/// first). Transfers send n bytes and keep what came back (in may be NULL).
/// @param sck the SCK pin
/// @param mosi the MOSI pin
/// @param miso the MISO pin
/// @param khz the clock in kHz
/// @param mode the SPI mode (0-3)
/// @return NULL, or why not
const char *port_spi_open(int sck, int mosi, int miso, int khz, int mode);
/// Stops SPI and frees its pins.
void port_spi_close(void);
/// Sends `n` bytes from `out` and stores what comes back in `in`; false on error.
/// @param out the bytes sent
/// @param in where the bytes received go
/// @param n how many
/// @return false on error
bool port_spi_transfer(const uint8_t *out, uint8_t *in, int n);

// ---- Serial port (OPEN "COM1:...") -------------------------------------------

/// Opens COM1 (UART1) on GPIOs tx and rx. parity is 'N', 'E' or 'O'. Returns
/// NULL, or why it can't (e.g. a pin is in use).
/// @param baud the speed
/// @param data_bits 5-8
/// @param parity 'N', 'E' or 'O'
/// @param stop_bits 1 or 2
/// @param tx the TX pin
/// @param rx the RX pin
/// @return NULL, or why not
const char *port_serial_open(unsigned baud, int data_bits, char parity, int stop_bits, int tx,
                             int rx);
/// Closes COM1 and frees its pins.
void port_serial_close(void);
/// @param on flow control on or off
/// @return NULL, or why not
const char *port_serial_flow(bool on);  ///< RTS/CTS on TX+2/TX+3; NULL or why not
/// Sends a BREAK on COM1 for `ms` milliseconds.
/// @param ms milliseconds
void port_serial_break(unsigned ms);
/// @param data the bytes
/// @param n how many
void port_serial_write(const char *data, int n);  ///< waits for room
/// @return the byte, or -1 if none
int port_serial_getc(void);      ///< next received byte, or -1 if none
/// @return how many
int port_serial_waiting(void);   ///< bytes received and not yet read
/// @return bytes free
int port_serial_room(void);      ///< free space in the receive buffer

// ---- Pictures (IMAGE, SAVEIMAGE) ---------------------------------------------

/// Draws the picture in `path` (BMP, PNG or JPEG) into the screen rectangle
/// (x, y, w, h), in screen pixels, clipped to (cx0, cy0)-(cx1, cy1). w = 0
/// means the picture's own size, each of its pixels sx by sy screen pixels;
/// with `fit` it keeps its shape inside the rectangle. Returns NULL, or why it
/// can't.
/// @param path the picture
/// @param x the left
/// @param y the top
/// @param w the width (0: the picture's)
/// @param h the height (0: the picture's)
/// @param sx scale x (as a percentage, or 0)
/// @param sy scale y
/// @param fit fit it in w x h, keeping its shape
/// @param cx0 the clip's left
/// @param cy0 its top
/// @param cx1 its right
/// @param cy1 its bottom
/// @return NULL, or why it can't
const char *port_image(const char *path, int x, int y, int w, int h, int sx, int sy, bool fit,
                       int cx0, int cy0, int cx1, int cy1);

/// Saves the screen as an 8-bit BMP. Returns NULL, or why it can't.
/// @param path the file
/// @return NULL, or why it can't
const char *port_save_image(const char *path);

// ---- Memory for programs, variables, arrays and strings -------------------------

/// As malloc/calloc/realloc/free, but returning NULL (never panicking) when
/// there's no room. On the Pico, big blocks go in PSRAM and small ones in SRAM.
/// @param n bytes wanted
/// @return the block, or NULL
void *port_alloc(size_t n);
/// calloc for BASIC: big blocks in PSRAM, small ones in SRAM.
/// @param count how many
/// @param size each one's size
/// @return the zeroed block, or NULL
void *port_calloc(size_t count, size_t size);
/// realloc for BASIC (see port_alloc).
/// @param p the block, or NULL
/// @param n the new size
/// @return the block (maybe moved), or NULL
void *port_realloc(void *p, size_t n);
/// Frees a block from port_alloc, port_calloc or port_realloc.
/// @param p the block, or NULL
void port_release(void *p);

/// NEOPIXEL: a strip of WS2812B LEDs on one pin, driven by a PIO state
/// machine. Open: `count` LEDs, GRB order (or RGB); NULL, or why not. Set
/// puts a colour in the buffer; show sends the buffer (scaled by the
/// brightness, 0-255); count is 0 while none is open.
/// @param pin the data pin
/// @param count the LEDs
/// @param rgb the strip takes red first (else green, as WS2812B do)
/// @return NULL, or why not
const char *port_pixels_open(int pin, int count, bool rgb);
void port_pixel_set(int n, int r, int g, int b);  ///< LED n's colour, 0-255 each
void port_pixels_show(void);  ///< sends the colours to the LEDs
void port_pixels_bright(int level);  ///< brightness 0-255 for the next shows
int port_pixels_count(void);  ///< the LEDs open, or 0
void port_pixels_close(void);  ///< frees the pin and the state machine

/// NEOPIXEL OPEN's default pin (CONFIG.TXT's NEOPIXEL).
/// @return the pin
int port_neopixel_pin(void);
/// CONFIG: prints the board's settings (CONFIG.TXT).
void port_config_show(void);

/// XSEND "file": sends it over the console by XMODEM (1K blocks with CRC-16,
/// or 128-byte blocks with a checksum if the receiver asks), YMODEM (its name
/// and size go first) or ZMODEM. XRECV "file": receives one into it (CRC-16,
/// or a checksum if the sender only has that; 128- or 1K-byte blocks),
/// replacing a file of that name, with XMODEM's ^Z padding taken off the end
/// (YMODEM and ZMODEM give the exact size, so nothing is taken off). `msg`
/// gets what happened; false if it failed.
/// @param path the file
/// @param proto 0 XMODEM, 1 YMODEM, 2 ZMODEM
/// @param msg set to what happened
/// @param size room in msg
/// @return it got through
bool port_xsend(const char *path, int proto, char *msg, int size);
/// Receives a file (see port_xsend). With YMODEM or ZMODEM the first file
/// is saved as `path` whatever the sender calls it.
/// @param path the file
/// @param proto 0 XMODEM, 1 YMODEM, 2 ZMODEM
/// @param msg set to what happened
/// @param size room in msg
/// @return it got through
bool port_xrecv(const char *path, int proto, char *msg, int size);

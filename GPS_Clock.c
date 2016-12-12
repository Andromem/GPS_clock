/*

    GPS Clock
    Copyright (C) 2016 Nicholas W. Sayer

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    
  */


// Unfortunately, to stay within the 1K limit, we need to do away with
// a *lot* of features.
//#define HACKADAY_1K

#include <stdlib.h>  
#include <stdio.h>  
#include <string.h>
#include <math.h>
#include <avr/io.h>
#include <avr/eeprom.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>

// 8 MHz.
#define F_CPU (8000000UL)
#include <util/delay.h>

// UBRR?_VALUE macros defined here are used below in serial initialization in main()
#define BAUD 9600
#include <util/setbaud.h>

// Port A is used for the display SPI interface and serial.
// We don't need to config the serial pins - it's enough to
// just turn on the USART.
#define PORT_MAX PORTA
#define BIT_MAX_CS _BV(PORTA3)
#define BIT_MAX_CLK _BV(PORTA4)
#define BIT_MAX_DO _BV(PORTA6)
#define DDR_BITS_A _BV(DDRA3) | _BV(DDRA4) | _BV(DDRA6)

#ifndef HACKADAY_1K
#endif

// The MAX6951 registers and their bits
#define MAX_REG_DEC_MODE 0x01
#define MAX_REG_INTENSITY 0x02
#define MAX_REG_SCAN_LIMIT 0x03
#define MAX_REG_CONFIG 0x04
#define MAX_REG_CONFIG_R _BV(5)
#define MAX_REG_CONFIG_T _BV(4)
#define MAX_REG_CONFIG_E _BV(3)
#define MAX_REG_CONFIG_B _BV(2)
#define MAX_REG_CONFIG_S _BV(0)
#define MAX_REG_TEST 0x07
// P0 and P1 are planes - used when blinking is turned on
// or the mask with the digit number 0-7. On the hardware, 0-5
// are the digits from left to right (D5 is single seconds, D0
// is tens of hours). The D7 and D6 decimal points are AM and PM (respectively).
// To blink, you write different stuff to P1 and P0 and turn on
// blinking in the config register (bit E to turn on, bit B for speed).
#define MAX_REG_MASK_P0 0x20
#define MAX_REG_MASK_P1 0x40
#define MAX_REG_MASK_BOTH (MAX_REG_MASK_P0 | MAX_REG_MASK_P1)
// When decoding is turned off, this is the bit mapping.
// Segment A is at the top, the rest proceed clockwise around, and
// G is in the middle. DP is the decimal point.
// When decoding is turned on, bits 0-3 are a hex value, 4-6 are ignored,
// and DP is as before.
#define MASK_DP _BV(7)
#define MASK_A _BV(6)
#define MASK_B _BV(5)
#define MASK_C _BV(4)
#define MASK_D _BV(3)
#define MASK_E _BV(2)
#define MASK_F _BV(1)
#define MASK_G _BV(0)

#define RX_BUF_LEN (96)

#ifndef HACKADAY_1K

// Port B is the switches and the PPS GPS input
#define PORT_SW PINB
#define DDR_BITS_B (0)
#define SW_0_BIT _BV(PINB0)
#define SW_1_BIT _BV(PINB2)
// Note that some versions of the AVR LIBC forgot to
// define the individual PUExn bit numbers. If you have
// a version like this, then just use _BV(0) | _BV(2).
#define PULLUP_BITS_B _BV(PUEB0) | _BV(PUEB2)

// These are return values from the DST detector routine.
// DST is not in effect all day
#define DST_NO 0
// DST is in effect all day
#define DST_YES 1
// DST begins at 0200
#define DST_BEGINS 2
// DST ends 0300 - that is, at 0200 pre-correction.
#define DST_ENDS 3

// EEPROM locations to store the configuration.
#define EE_TIMEZONE ((uint8_t*)0)
#define EE_DST_ENABLE ((uint8_t*)1)
#define EE_AM_PM ((uint8_t*)2)
#define EE_BRIGHTNESS ((uint8_t*)3)

// timer 1 runs at F_CPU / 1024 Hz. We want something like 50 ms.
#define DEBOUNCE_TICKS 390
// The buttons
#define SELECT 1
#define SET 2

#endif

volatile unsigned char disp_buf[8];
volatile unsigned char rx_buf[RX_BUF_LEN];
volatile unsigned char rx_str_len;
volatile unsigned char gps_locked;

#ifndef HACKADAY_1K
volatile char tz_hour;
volatile char dst_enabled;
volatile char ampm;
volatile char menu_pos;
unsigned int debounce_time;
unsigned char button_down;
unsigned char brightness;
#endif

// Delay, but pet the watchdog while doing it.
static void Delay(unsigned long ms) {
  while(ms > 100) {
    _delay_ms(100);
    wdt_reset();
    ms -= 100;
  }
  _delay_ms(ms);
  wdt_reset();
}

void write_reg(const unsigned char addr, const unsigned char val) {
	// We write a 16 bit word, with address at the top end.
	unsigned int data = (addr << 8) | val;

	// Start with clk low
	PORT_MAX &= ~BIT_MAX_CLK;
	// Now assert !CS
	PORT_MAX &= ~BIT_MAX_CS;
	// now clock each data bit in
	for(int i = 15; i >= 0; i--) {
		// Set the data bit to the appropriate data bit value
		if ((data >> i) & 0x1)
			PORT_MAX |= BIT_MAX_DO;
		else
			PORT_MAX &= ~BIT_MAX_DO;
		// Toggle the clock. The maximum clock frequency is something
		// like 50 MHz, so there's no need to add any delays.
		PORT_MAX |= BIT_MAX_CLK;
		PORT_MAX &= ~BIT_MAX_CLK;
	}
	// And finally, release !CS.
	PORT_MAX |= BIT_MAX_CS;
}

#ifndef HACKADAY_1K
// zero-based day of year number for first day of each month, non-leap-year
const unsigned int first_day[] PROGMEM = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

// Note that this routine is only defined for years between 2000 and 2099.
// If you're alive beyond that, then this is a Y2.1K bug for you to fix.
// You also get to figure out what to do about the NMEA two-digit
// year format. And probably update the GPS firmware or buy a new module.
static unsigned char first_sunday(unsigned char m, unsigned char y) {
	unsigned int day_of_year = 0;
	// after March, leap year comes into play
	if (m > 2) {
		// Y2.1K bug here. Every 100 years is not a leap year,
		// but every 400 years is again.
		if ((y % 4) == 0) day_of_year++;
	}
	// Now add in the offset for the month.
	day_of_year += pgm_read_dword(&(first_day[m - 1]));
	// This requires some explanation. We need to calculate the weekday of January 1st.
	// This all works because Y2K was a leap year and because we're doing all
	// of this for years *after* 2000.
	//
	// January 1st 2000 was a Saturday. Since we're 0 based and starting with Sunday...
	unsigned char weekday = 6;
	// For every year after 2000, add one day, since normal years advance the weekday by 1.
	weekday += y % 7;
	weekday %= 7;
	// For every leap year after 2000, add another day, since leap years advance the weekday by 2.
	// But you only do it for leap years *in the past*. Note that this won't work for 2000 because
	// of underflow.
	weekday += (((y - 1) / 4) + 1) % 7;
	weekday %= 7;
	// Y2.1K bug - this is where you're supposed to take a day *away*
	// every 100 years... but then add it *back* every 400.
	// Add in the day of the year
	weekday += day_of_year % 7;
	weekday %= 7;
	// Now figure out how many days before we hit a Sunday. But if we're already there, then just return 1.
	return (weekday == 0)?1:(8 - weekday);
}

static unsigned char calculateDST(unsigned char d, unsigned char m, unsigned char y) {
	// DST is in effect between the 2nd Sunday in March and the first Sunday in November
	// The return values here are that DST is in effect, or it isn't, or it's beginning
	// for the year today or it's ending today.
	unsigned char change_day;
	switch(m) {
		case 1: // December through February
		case 2:
		case 12:
			return DST_NO;
		case 3: // March
			change_day = first_sunday(m, y) + 7; // second Sunday.
			if (d < change_day) return DST_NO;
			else if (d == change_day) return DST_BEGINS;
			else return DST_YES;
			break;
		case 4: // April through October
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
			return DST_YES;
		case 11: // November
			change_day = first_sunday(m, y);
			if (d < change_day) return DST_YES;
			else if (d == change_day) return DST_ENDS;
			else return DST_NO;
			break;
		default: // This is impossible, since m can only be between 1 and 12.
			return 255;
	}
}
#endif

static void handle_time(char h, unsigned char m, unsigned char s, unsigned char dst_flags) {
	// What we get is the current second. We have to increment it
	// to represent the *next* second.
	s++;
	// Note that this also handles leap-seconds. We wind up pinning to 0
	// twice.
	if (s >= 60) { s = 0; m++; }
	if (m >= 60) { m = 0; h++; }
	if (h >= 24) { h = 0; }

#ifndef HACKADAY_1K
	// Move to local standard time.
	int hr_offset = tz_hour;

	if (dst_enabled) {
		unsigned char dst_offset = 0;
		switch(dst_flags) {
			case DST_NO: dst_offset = 0; break; // do nothing
			case DST_YES: dst_offset = 1; break; // add one hour
			case DST_BEGINS:
				dst_offset = (h >= 2)?1:0; break; // offset becomes 1 at 0200
			case DST_ENDS:
				dst_offset = (h >= 1)?0:1; break; // offset becomes 0 at 0200 (post-correction)
		}
		hr_offset += dst_offset;
	}

	h += tz_hour;
	while (h >= 24) h -= 24;
	while (h < 0) h += 24;

	unsigned char am = 0;
	if (ampm) {
		// Create AM or PM
                if (h == 0) { h = 12; am = 1; }
		else if (h < 12) { am = 1; }
		else {
			if (h > 12) h -= 12;
		}
	}
#endif

	disp_buf[5] = s % 10;
	disp_buf[4] = s / 10;
	disp_buf[3] = (m % 10) | MASK_DP;
	disp_buf[2] = m / 10;
	disp_buf[1] = (h % 10) | MASK_DP;
	disp_buf[0] = h / 10;
#ifndef HACKADAY_1K
	if (ampm) {
		disp_buf[7] = am ? MASK_DP:0;
		disp_buf[6] = (!am) ? MASK_DP:0;
	} else {
		disp_buf[6] = disp_buf[7] = 0;
	}
#endif
}

static char* skip_commas(char *ptr, int num) {
	for(int i = 0; i < num; i++) {
		ptr = strchr((const char *)ptr, ',');
		if (ptr == NULL) return NULL; // not enough commas
		ptr++; // skip over it
	}
	return ptr;
}

const char hexes[] PROGMEM = "0123456789abcdef";

static unsigned char hexChar(unsigned char c) {
	if (c >= 'A' && c <= 'F') c += ('a' - 'A'); // make lower case
	const char* outP = strchr_P(hexes, c);
	if (outP == NULL) return 0;
	return (unsigned char)(outP - hexes);
}

static void handleGPS() {
	unsigned int str_len = rx_str_len; // rx_str_len is where the \0 was written.
 
	if (str_len < 9) return; // No sentence is shorter than $GPGGA*xx
	// First, check the checksum of the sentence
	unsigned char checksum = 0;
	int i;
	for(i = 1; i < str_len; i++) {
		if (rx_buf[i] == '*') break;
		checksum ^= rx_buf[i];
	}
	if (i > str_len - 3) {
		return; // there has to be room for the "*" and checksum.
	}
	i++; // skip the *
	unsigned char sent_checksum = (hexChar(rx_buf[i]) << 4) | hexChar(rx_buf[i + 1]);
	if (sent_checksum != checksum) {
		return; // bad checksum.
	}
  
	char *ptr = (char *)rx_buf;
	if (!strncmp_P((const char*)rx_buf, PSTR("$GPRMC"), 6)) {
		// $GPRMC,172313.000,A,xxxx.xxxx,N,xxxxx.xxxx,W,0.01,180.80,260516,,,D*74\x0d\x0a
		ptr = skip_commas(ptr, 1);
		if (ptr == NULL) return; // not enough commas
		char h = (ptr[0] - '0') * 10 + (ptr[1] - '0');
		unsigned char min = (ptr[2] - '0') * 10 + (ptr[3] - '0');
		unsigned char s = (ptr[4] - '0') * 10 + (ptr[5] - '0');
		unsigned char dst_flags = 0;
#ifndef HACKADAY_1K
		ptr = skip_commas(ptr, 8);
		if (ptr == NULL) return; // not enough commas
		unsigned char d = (ptr[0] - '0') * 10 + (ptr[1] - '0');
		unsigned char mon = (ptr[2] - '0') * 10 + (ptr[3] - '0');
		unsigned char y = (ptr[4] - '0') * 10 + (ptr[5] - '0');
		// The problem is that our D/M/Y is UTC, but DST decisions are made in the local
		// timezone. We can adjust the day against standard time midnight, and
		// that will be good enough. Don't worry that this can result in d being either 0
		// or past the last day of the month. Neither of those will match the "decision day"
		// for DST, which is the only day on which the day of the month is significant.
		if (h + tz_hour < 0) d--;
		if (h + tz_hour > 23) d++;
		dst_flags = calculateDST(d, mon, y);
#endif
		handle_time(h, min, s, dst_flags);
#ifndef HACKADAY_1K
	} else if (!strncmp_P((const char*)rx_buf, PSTR("$GPGSA"), 6)) {
		// $GPGSA,A,3,02,06,12,24,25,29,,,,,,,1.61,1.33,0.90*01
		ptr = skip_commas(ptr, 2);
		if (ptr == NULL) return; // not enough commas
		gps_locked = (*ptr == '3');
#endif
	}
}

ISR(USART0_RX_vect) {
  unsigned char rx_char = UDR0;
  
  if (rx_str_len == 0 && rx_char != '$') return; // wait for a "$" to start the line.
  rx_buf[rx_str_len] = rx_char;
  if (rx_char == 0x0d || rx_char == 0x0a) {
    rx_buf[rx_str_len] = 0; // null terminate
    handleGPS();
    rx_str_len = 0; // now clear the buffer
    return;
  }
  if (++rx_str_len == RX_BUF_LEN) {
    // The string is too long. Start over.
    rx_str_len = 0;
  }
}

#ifndef HACKADAY_1K
static void write_no_sig() {
	// Clear out the digit data
	write_reg(MAX_REG_CONFIG, MAX_REG_CONFIG_R | MAX_REG_CONFIG_S);
	write_reg(MAX_REG_DEC_MODE, 0);
        write_reg(MAX_REG_MASK_BOTH | 0, MASK_E | MASK_G | MASK_C); // n
        write_reg(MAX_REG_MASK_BOTH | 1, MASK_E | MASK_G | MASK_C | MASK_D); // o
        write_reg(MAX_REG_MASK_BOTH | 3, MASK_A | MASK_F | MASK_G | MASK_C | MASK_D); // S
        write_reg(MAX_REG_MASK_BOTH | 4, MASK_B | MASK_C); // I
        write_reg(MAX_REG_MASK_BOTH | 5, MASK_A | MASK_F | MASK_E | MASK_G | MASK_C | MASK_D); // G
}
#endif

ISR(INT0_vect) {
#ifndef HACKADAY_1K
	if (menu_pos) return;
	if (!gps_locked) {
		write_no_sig();
		return;
	}
#endif
	write_reg(MAX_REG_DEC_MODE, 0x3f); // full decode for 6 digits.
	// Copy the display buffer data into the display
	for(int i = 0; i < sizeof(disp_buf); i++) {
		write_reg(MAX_REG_MASK_BOTH | i, disp_buf[i]);
	}
}

#ifndef HACKADAY_1K
static unsigned char check_buttons() {
	if (debounce_time != 0 && TCNT1 - debounce_time < DEBOUNCE_TICKS) {
		return 0;
	} else {
		debounce_time = 0; // debounce is over
	}
	unsigned char status = PORT_SW & (SW_0_BIT | SW_1_BIT);
	status ^= (SW_0_BIT | SW_1_BIT); // invert the buttons - 0 means down.
	if (!((button_down == 0) ^ (status == 0))) return 0; // either no button is down, or a button is still down
	if (!button_down && status) {
		button_down = 1; // a button has been pushed
		debounce_time = TCNT1;
		if (!debounce_time) debounce_time++; // it's not allowed to be zero
		return (status & SW_0_BIT)?SELECT:SET;
	}
	if (button_down && !status) {
		button_down = 0; // a button is no longer down
		debounce_time = TCNT1;
		if (!debounce_time) debounce_time++; // it's not allowed to be zero
		return 0;
	}
	return 0; // This should never actually happen
}

static void menu_render() {
			// blank the display
			write_reg(MAX_REG_CONFIG, MAX_REG_CONFIG_R | MAX_REG_CONFIG_S);
	switch(menu_pos) {
		case 0:
			// we're returning to time mode. Either leave it blank or indicate no signal.
			if (!gps_locked)
				write_no_sig();
			break;
		case 1: // zone
			write_reg(MAX_REG_DEC_MODE, 0x30); // decoding for last two digits only
        		write_reg(MAX_REG_MASK_BOTH | 0, MASK_D | MASK_E | MASK_F | MASK_G); // t
        		write_reg(MAX_REG_MASK_BOTH | 1, MASK_C | MASK_E | MASK_F | MASK_G); // h
			if (tz_hour < 0) {
        			write_reg(MAX_REG_MASK_BOTH | 3, MASK_G); // -
			}
        		write_reg(MAX_REG_MASK_BOTH | 4, abs(tz_hour) / 10);
        		write_reg(MAX_REG_MASK_BOTH | 5, abs(tz_hour) % 10);
			break;
		case 2: // 12/24 hour
			write_reg(MAX_REG_DEC_MODE, 0x6); // decoding for first two digits only (skipping one)
        		write_reg(MAX_REG_MASK_BOTH | 1, ampm?1:2);
        		write_reg(MAX_REG_MASK_BOTH | 2, ampm?2:4);
        		write_reg(MAX_REG_MASK_BOTH | 4, MASK_C | MASK_E | MASK_F | MASK_G); // h
        		write_reg(MAX_REG_MASK_BOTH | 5, MASK_E | MASK_G); // r
			break;
		case 3: // DST on/off
			write_reg(MAX_REG_DEC_MODE, 0); // no decoding
        		write_reg(MAX_REG_MASK_BOTH | 0, MASK_B | MASK_C | MASK_D | MASK_E | MASK_G); // d
        		write_reg(MAX_REG_MASK_BOTH | 1, MASK_A | MASK_C | MASK_D | MASK_F | MASK_G); // S
        		write_reg(MAX_REG_MASK_BOTH | 3, MASK_C | MASK_D | MASK_E | MASK_G); // o
			if (dst_enabled) {
        			write_reg(MAX_REG_MASK_BOTH | 4, MASK_C | MASK_E | MASK_G); // n
			} else {
        			write_reg(MAX_REG_MASK_BOTH | 4, MASK_A | MASK_E | MASK_F | MASK_G); // F
        			write_reg(MAX_REG_MASK_BOTH | 5, MASK_A | MASK_E | MASK_F | MASK_G); // F
			}
			break;
		case 4: // brightness
			write_reg(MAX_REG_DEC_MODE, 0); // no decoding
        		write_reg(MAX_REG_MASK_BOTH | 0, MASK_C | MASK_D | MASK_E | MASK_F | MASK_G); // b
        		write_reg(MAX_REG_MASK_BOTH | 1, MASK_E | MASK_G); // r
        		write_reg(MAX_REG_MASK_BOTH | 2, MASK_B | MASK_C); // I
        		write_reg(MAX_REG_MASK_BOTH | 3, MASK_A | MASK_C | MASK_D | MASK_E | MASK_F | MASK_G); // G
        		write_reg(MAX_REG_MASK_BOTH | 4, MASK_C | MASK_E | MASK_F | MASK_G); // h
        		write_reg(MAX_REG_MASK_BOTH | 5, MASK_D | MASK_E | MASK_F | MASK_G); // t
			write_reg(MAX_REG_INTENSITY, brightness);
			break;
	}
}

static void menu_select() {
	switch(menu_pos) {
		case 1:
			eeprom_write_byte(EE_TIMEZONE, tz_hour + 12);
			break;
		case 2:
			eeprom_write_byte(EE_AM_PM, ampm);
			break;
		case 3:
			eeprom_write_byte(EE_DST_ENABLE, dst_enabled);
			break;
		case 4:
			eeprom_write_byte(EE_BRIGHTNESS, brightness);
			break;
	}
	if (++menu_pos > 4) menu_pos = 0;
	menu_render();
}

static void menu_set() {
	switch(menu_pos) {
		case 0: return; // ignore SET when just running
		case 1: // timezone
			if (++tz_hour >= 13) tz_hour = -12;
			break;
		case 2: // 12/24 hour
			ampm = !ampm;
			break;
		case 3: // DST on/off
			dst_enabled = !dst_enabled;
			break;
		case 4: // brightness
			if (++brightness > 15) brightness = 0;
			break;
	}
	menu_render();
}

#endif

void main() {

	wdt_enable(WDTO_1S);

	// We don't use a lot of the chip, it turns out
#ifdef HACKADAY_1K
	PRR = ~_BV(PRUSART0);
#else
	PRR = ~(_BV(PRUSART0) | _BV(PRTIM1));
#endif

	// Make sure the CS pin is high and everything else is low.
	PORT_MAX = BIT_MAX_CS;
	DDRA = DDR_BITS_A;

#ifndef HACKADAY_1K
	DDRB = DDR_BITS_B;
	PUEB = PULLUP_BITS_B;
#endif

	UBRR0H = UBRRH_VALUE;
	UBRR0L = UBRRL_VALUE;
#if USE_2X
	UCSR0A = _BV(U2X0);
#else
	UCSR0A = 0;
#endif

	UCSR0B = _BV(RXCIE0) | _BV(RXEN0); // RX interrupt and RX enable

	// 8N1
	UCSR0C = _BV(UCSZ00) | _BV(UCSZ01);

	MCUCR = _BV(ISC01) | _BV(ISC00); // INT0 on rising edge
	GIMSK = _BV(INT0); // enable INT0

	rx_str_len = 0;
	gps_locked = 0;

#ifndef HACKADAY_1K
	TCCR1B = _BV(CS12) | _BV(CS10); // prescale by 1024

	unsigned char ee_rd = eeprom_read_byte(EE_TIMEZONE);
	if (ee_rd == 0xff)
		tz_hour = -8;
	else
		tz_hour = ee_rd - 12;
	dst_enabled = eeprom_read_byte(EE_DST_ENABLE);
	ampm = eeprom_read_byte(EE_AM_PM);

	menu_pos = 0;
	debounce_time = 0;
	button_down = 0;
#endif

	// Turn off the shut-down register, clear the digit data
	write_reg(MAX_REG_CONFIG, MAX_REG_CONFIG_R | MAX_REG_CONFIG_S);
	write_reg(MAX_REG_SCAN_LIMIT, 7); // display all 8 digits
#ifndef HACKADAY_1K
	{
		brightness = eeprom_read_byte(EE_BRIGHTNESS) & 0xf;
		write_reg(MAX_REG_INTENSITY, brightness);
	}
#else
	write_reg(MAX_REG_INTENSITY, 0xf); // Full intensity
#endif
	// Turn on the self-test for a second
	write_reg(MAX_REG_TEST, 1);
	Delay(1000);
	write_reg(MAX_REG_TEST, 0);
#ifndef HACKADAY_1K
	write_no_sig();
#endif

	// Turn on interrupts
	sei();

	while(1) {
		wdt_reset();
#ifndef HACKADAY_1K
		unsigned char button = check_buttons();
		if (!button) continue;

		switch(button) {
			case SELECT:
				menu_select();
				break;
			case SET:
				menu_set();
				break;
		}
#endif
	}
}

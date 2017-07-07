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

#include <stdlib.h>  
#include <stdio.h>  
#include <string.h>
#include <math.h>
#include <avr/cpufunc.h>
#include <avr/io.h>
#include <avr/eeprom.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>

// 32 MHz
#define F_CPU (32000000UL)

// CLK2X = 0. For 9600 baud @ 32 MHz: 
#define BSEL (12)
#define BSCALE (4)

// Port A is the anodes - so segments A through G + DP.
// Port D is the cathodes - so the digits from 0-7
#define DIGIT_VAL_REG PORTA.OUT
#define DIGIT_SEL_REG PORTD.OUT

#define MASK_A _BV(0)
#define MASK_B _BV(1)
#define MASK_C _BV(2)
#define MASK_D _BV(3)
#define MASK_E _BV(4)
#define MASK_F _BV(5)
#define MASK_G _BV(6)
#define MASK_DP _BV(7)

// Digit 7 has the two colons and the AM & PM lights
#define MASK_COLON_HM (MASK_E | MASK_F)
#define MASK_COLON_MS (MASK_B | MASK_C)
#define MASK_AM (MASK_A)
#define MASK_PM (MASK_D)

// Digit map
#define DIGIT_10_HR (0)
#define DIGIT_1_HR (1)
#define DIGIT_10_MIN (2)
#define DIGIT_1_MIN (3)
#define DIGIT_10_SEC (4)
#define DIGIT_1_SEC (5)
#define DIGIT_100_MSEC (6)
#define DIGIT_MISC (7)

// Port C is the serial port, switches and PPS.

// The buttons
#define PORT_SW PORTC.IN
#define SW_0_BIT _BV(5)
#define SW_1_BIT _BV(4)

// The buttons, from a software perspective
#define SELECT 1
#define SET 2

// The serial buffer length
#define RX_BUF_LEN (96)

// These are return values from the DST detector routine.
// DST is not in effect all day
#define DST_NO 0
// DST is in effect all day
#define DST_YES 1
// DST begins at 0200
#define DST_BEGINS 2
// DST ends 0300 - that is, at 0200 pre-correction.
#define DST_ENDS 3

// The possible values for dst_mode
#define DST_OFF 0
#define DST_US 1
#define DST_EU 2
#define DST_AU 3
#define DST_NZ 4
#define DST_MODE_MAX DST_NZ

// The possible colon states
#define COLON_OFF 0
#define COLON_ON 1
#define COLON_BLINK 2
#define COLON_STATE_MAX COLON_BLINK

// EEPROM locations to store the configuration.
#define EE_TIMEZONE ((uint8_t*)0)
#define EE_DST_MODE ((uint8_t*)1)
#define EE_AM_PM ((uint8_t*)2)
#define EE_BRIGHTNESS ((uint8_t*)3)
#define EE_TENTHS ((uint8_t*)4)
#define EE_COLONS ((uint8_t*)5)

// The refresh rate is F_CPU / (BRIGHTNESS_LEVELS * digit_count * REFRESH_PERIOD). REFRESH_PERIOD is
// a tradeoff between refresh rate (which is also directly tied to the display accuracy) and the
// amount of CPU left to actually run the clock.
//
// 10 kHz refresh rate - 4 interrupts per digit (brightness), 8 digits, 32 MHz freq.
#define REFRESH_PERIOD (100)

// This is the timer frequency - it's the system clock prescaled by 1
// Keep this synced with the configuration of Timer C4!
#define F_TICK (F_CPU / 1)

// We want something like 50 ms.
#define DEBOUNCE_TICKS (F_TICK / 20)

// If we don't get a PPS at least this often, then we've lost it.
// This is F_TICK*1.25 - a quarter second late.
#define LOST_PPS_TICKS (F_TICK + F_TICK / 4)

// For unknown reasons, we sometimes get a first PPS tick that's way, way
// too fast. Rather than have the display look weird, we'll just skip
// showing tenths anytime GPS tells us a tenth of a second is less than
// 50 ms worth of system clock.
#define FAST_PPS_TICKS (F_TICK / 20)

// How many brightness levels do we support? This is a tradeoff
// between refresh frequency and brightness granularity.
#define BRIGHTNESS_LEVELS 4

// disp reg is the "registers" for the disply. It's what's actively being
// displayed right now by the rastering system.
volatile unsigned char disp_reg[8];
// disp buf is the buffer where data is prepped for display during the next second.
// It's copied into disp_reg by the PPS ISR.
volatile unsigned char disp_buf[8];
volatile unsigned char brightness;

volatile unsigned char rx_buf[RX_BUF_LEN];
volatile unsigned char rx_str_len;
volatile unsigned char nmea_ready;
volatile unsigned long last_pps_tick;
volatile unsigned char last_pps_tick_good;
volatile unsigned long tenth_ticks;
volatile unsigned char gps_locked;
volatile unsigned char menu_pos;
volatile unsigned char tenth_enable;
volatile unsigned char disp_tenth;
unsigned char dst_mode;
unsigned char ampm;
char tz_hour;
unsigned char colon_state;
unsigned long debounce_time;
unsigned char button_down;
unsigned long fake_blink;

// digit to 7 segment table.
static const unsigned char character_set[] PROGMEM = {
	MASK_A | MASK_B | MASK_C | MASK_D | MASK_E | MASK_F, // 0
	MASK_B | MASK_C, // 1
	MASK_A | MASK_B | MASK_D | MASK_E | MASK_G, // 2
	MASK_A | MASK_B | MASK_C | MASK_D | MASK_G, // 3
	MASK_B | MASK_C | MASK_F | MASK_G, // 4
	MASK_A | MASK_C | MASK_D | MASK_F | MASK_G, // 5
	MASK_A | MASK_C | MASK_D | MASK_E | MASK_F | MASK_G, // 6
	MASK_A | MASK_B | MASK_C, // 7
	MASK_A | MASK_B | MASK_C | MASK_D | MASK_E | MASK_F | MASK_G, // 8
	MASK_A | MASK_B | MASK_C | MASK_D | MASK_F | MASK_G, // 9
	// The clock doesn't need A-F, but we include it for potential
	// debugging use.
	MASK_A | MASK_B | MASK_C | MASK_E | MASK_F | MASK_G, // A
	MASK_C | MASK_D | MASK_E | MASK_F | MASK_G, // b
	MASK_A | MASK_D | MASK_E | MASK_F, // C
	MASK_B | MASK_C | MASK_D | MASK_E | MASK_G, // d
	MASK_A | MASK_D | MASK_E | MASK_F | MASK_G, // E
	MASK_A | MASK_E | MASK_F | MASK_G // F
};
		
static const unsigned char month_tweak[] PROGMEM = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

static inline unsigned char first_sunday(unsigned char m, unsigned int y) {
	// first, what's the day-of-week for the first day of whatever month?
	// From http://en.wikipedia.org/wiki/Determination_of_the_day_of_the_week
	y -= m < 3;
	unsigned char month_tweak_val = pgm_read_byte(&(month_tweak[m - 1]));
	unsigned char dow = (y + y/4 - y/100 + y/400 + month_tweak_val + 1) % 7;

	// If the 1st is a Sunday, then the answer is 1. Otherwise, we count
	// up until we find a Sunday.
	return (dow == 0)?1:(8 - dow);
}

static inline unsigned char calculateDSTAU(const unsigned char d, const unsigned char m, const unsigned int y) {
	// DST is in effect between the first Sunday in October and the first Sunday in April
	unsigned char change_day;
	switch(m) {
		case 1: // November through March
		case 2:
		case 3:
		case 11:
		case 12:
			return DST_YES;
		case 4: // April
			change_day = first_sunday(m, y);
			if (d < change_day) return DST_YES;
			else if (d == change_day) return DST_ENDS;
			else return DST_NO;
			break;
		case 5: // April through September
		case 6:
		case 7:
		case 8:
		case 9:
			return DST_NO;
		case 10: // October
			change_day = first_sunday(m, y);
			if (d < change_day) return DST_NO;
			else if (d == change_day) return DST_BEGINS;
			else return DST_YES;
			break;
		default: // This is impossible, since m can only be between 1 and 12.
			return 255;
	}
}
static inline unsigned char calculateDSTNZ(const unsigned char d, const unsigned char m, const unsigned int y) {
	// DST is in effect between the last Sunday in September and the first Sunday in April
	unsigned char change_day;
		switch(m) {
		case 1: // October through March
		case 2:
		case 3:
		case 10:
		case 11:
		case 12:
			return DST_YES;
		case 4: // April
			change_day = first_sunday(m, y);
			if (d < change_day) return DST_YES;
			else if (d == change_day) return DST_ENDS;
			else return DST_NO;
			break;
		case 5: // April through August
		case 6:
		case 7:
		case 8:
			return DST_NO;
		case 9: // September
			change_day = first_sunday(m, y);
			while(change_day + 7 <= 30) change_day += 7; // last Sunday
			if (d < change_day) return DST_NO;
			else if (d == change_day) return DST_BEGINS;
			else return DST_YES;
			break;
		default: // This is impossible, since m can only be between 1 and 12.
			return 255;
	}
}
static inline unsigned char calculateDSTEU(const unsigned char d, const unsigned char m, const unsigned int y) {
	// DST is in effect between the last Sunday in March and the last Sunday in October
	unsigned char change_day;
	switch(m) {
		case 1: // November through February
		case 2:
		case 11:
		case 12:
			return DST_NO;
		case 3: // March
			change_day = first_sunday(m, y);
			while(change_day + 7 <= 31) change_day += 7; // last Sunday
			if (d < change_day) return DST_NO;
			else if (d == change_day) return DST_BEGINS;
			else return DST_YES;
			break;
		case 4: // April through September
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
			return DST_YES;
		case 10: // October
			change_day = first_sunday(m, y);
			while(change_day + 7 <= 31) change_day += 7; // last Sunday
			if (d < change_day) return DST_YES;
			else if (d == change_day) return DST_ENDS;
			else return DST_NO;
			break;
		default: // This is impossible, since m can only be between 1 and 12.
			return 255;
	}
}
static inline unsigned char calculateDSTUS(const unsigned char d, const unsigned char m, const unsigned int y) {
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
static inline unsigned char calculateDST(const unsigned char d, const unsigned char m, const unsigned int y) {
	switch(dst_mode) {
		case DST_US:
			return calculateDSTUS(d, m, y);
		case DST_EU:
			return calculateDSTEU(d, m, y);
		case DST_AU:
			return calculateDSTAU(d, m, y);
		case DST_NZ:
			return calculateDSTNZ(d, m, y);
		default: // off - should never happen
			return DST_NO;
	}
}

static inline void handle_time(char h, unsigned char m, unsigned char s, unsigned char dst_flags) {
	// What we get is the current second. We have to increment it
	// to represent the *next* second.
	s++;
	// Note that this also handles leap-seconds. We wind up pinning to 0
	// twice. We can't do other than that because we'd need to know that
	// the second after 59 is 60 instead of 0, and we can't know that.
	if (s >= 60) { s = 0; m++; }
	if (m >= 60) { m = 0; h++; }
	if (h >= 24) { h = 0; }

	// Move to local standard time.
	h += tz_hour;
	while (h >= 24) h -= 24;
	while (h < 0) h += 24;

	if (dst_mode != DST_OFF) {
		unsigned char dst_offset = 0;
		// For Europe, decisions are at 0100. Everywhere else it's 0200.
		unsigned char decision_hour = (dst_mode == DST_EU)?1:2;
		switch(dst_flags) {
			case DST_NO: dst_offset = 0; break; // do nothing
			case DST_YES: dst_offset = 1; break; // add one hour
			case DST_BEGINS:
				dst_offset = (h >= decision_hour)?1:0; // offset becomes 1 at 0200 (0100 EU)
				break;
			case DST_ENDS:
				// The *summer time* hour has to be the decision hour,
				// and we haven't yet made 'h' the summer time hour,
				// so compare it to one less than the decision hour.
				dst_offset = (h >= (decision_hour - 1))?0:1; // offset becomes 0 at 0200 (daylight) (0100 EU)
				break;
		}
		h += dst_offset;
		if (h >= 24) h -= 24;
	}

	unsigned char am = 0;
	if (ampm) {
		// Create AM or PM
		if (h == 0) { h = 12; am = 1; }
		else if (h < 12) { am = 1; }
		else if (h > 12) h -= 12;
	}

	disp_buf[DIGIT_1_SEC] = pgm_read_byte(&(character_set[s % 10]));
	disp_buf[DIGIT_10_SEC] = pgm_read_byte(&(character_set[s / 10]));
	disp_buf[DIGIT_1_MIN] = pgm_read_byte(&(character_set[m % 10]));
	disp_buf[DIGIT_10_MIN] = pgm_read_byte(&(character_set[m / 10]));
	disp_buf[DIGIT_1_HR] = pgm_read_byte(&(character_set[h % 10]));
	disp_buf[DIGIT_10_HR] = pgm_read_byte(&(character_set[h / 10]));
	// no, we do this in the ISR
	//disp_buf[DIGIT_100_MSEC] = pgm_read_byte(&(character_set[0]));
	disp_buf[DIGIT_MISC] = 0;
	if (ampm) {
		// If we are doing 12 hour display and if the 10 hours digit is 0, then blank it instead.
		if (h / 10 == 0) {
			disp_buf[DIGIT_10_HR] = 0; // blank it
		}
		disp_buf[DIGIT_MISC] |= am ? MASK_AM : MASK_PM;
	}
	if (colon_state == COLON_ON || ((colon_state == COLON_BLINK) && (s % 2 == 0))) {
		disp_buf[DIGIT_MISC] |= MASK_COLON_HM | MASK_COLON_MS;
	}
}

static const char *skip_commas(const char *ptr, const int num) {
	for(int i = 0; i < num; i++) {
		ptr = strchr(ptr, ',');
		if (ptr == NULL) return NULL; // not enough commas
		ptr++; // skip over it
	}
	return ptr;
}

static const char hexes[] PROGMEM = "0123456789abcdef";

static unsigned char hexChar(unsigned char c) {
	if (c >= 'A' && c <= 'F') c += ('a' - 'A'); // make lower case
	const char* outP = strchr_P(hexes, c);
	if (outP == NULL) return 0;
	return (unsigned char)(outP - hexes);
}

static inline void handleGPS() {
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
	  
	const char *ptr = (char *)rx_buf;
	if (!strncmp_P(ptr, PSTR("$GPRMC"), 6)) {
		// $GPRMC,172313.000,A,xxxx.xxxx,N,xxxxx.xxxx,W,0.01,180.80,260516,,,D*74\x0d\x0a
		ptr = skip_commas(ptr, 1);
		if (ptr == NULL) return; // not enough commas
		char h = (ptr[0] - '0') * 10 + (ptr[1] - '0');
		unsigned char min = (ptr[2] - '0') * 10 + (ptr[3] - '0');
		unsigned char s = (ptr[4] - '0') * 10 + (ptr[5] - '0');
		ptr = skip_commas(ptr, 8);
		if (ptr == NULL) return; // not enough commas
		unsigned char d = (ptr[0] - '0') * 10 + (ptr[1] - '0');
		unsigned char mon = (ptr[2] - '0') * 10 + (ptr[3] - '0');
		unsigned int y = (ptr[4] - '0') * 10 + (ptr[5] - '0');

		// Y2.1K bug here... We must turn the two digit year into
		// the actual A.D. year number. As time goes forward, in
		// principle, we could start deciding that "low" values
		// get 2100 added instead of 2000. You'd think that
		// way before then GPS will be obsolete, though.
		y += 2000;
		if (y < 2017) y += 100; // As I type this, it's A.D. 2017

		// The problem is that our D/M/Y is UTC, but DST decisions are made in the local
		// timezone. We can adjust the day against standard time midnight, and
		// that will be good enough. Don't worry that this can result in d being either 0
		// or past the last day of the month. Neither of those will match the "decision day"
		// for DST, which is the only day on which the day of the month is significant.
		if (h + tz_hour < 0) d--;
		if (h + tz_hour > 23) d++;
		unsigned char dst_flags = calculateDST(d, mon, y);
		handle_time(h, min, s, dst_flags);
	} else if (!strncmp_P(ptr, PSTR("$GPGSA"), 6)) {
		// $GPGSA,A,3,02,06,12,24,25,29,,,,,,,1.61,1.33,0.90*01
		ptr = skip_commas(ptr, 2);
		if (ptr == NULL) return; // not enough commas
		gps_locked = (*ptr == '3' || *ptr == '2');
	}
}

// serial receive interrupt.
ISR(USARTC0_RXC_vect) {
	unsigned char rx_char = USARTC0.DATA;
 
	if (nmea_ready) return; // ignore serial until the buffer is handled 
	if (rx_str_len == 0 && rx_char != '$') return; // wait for a "$" to start the line.
	rx_buf[rx_str_len] = rx_char;
	if (rx_char == 0x0d || rx_char == 0x0a) {
		rx_buf[rx_str_len] = 0; // null terminate
		nmea_ready = 1; // Mark it as ready
		return;
	}
	if (++rx_str_len == RX_BUF_LEN) {
		// The string is too long. Start over.
		rx_str_len = 0;
	}
}

static const unsigned char no_sig_data[] PROGMEM = {
	MASK_C | MASK_E | MASK_G, // n
	MASK_C | MASK_D | MASK_E | MASK_G, // o
	0,
	MASK_A | MASK_C | MASK_D | MASK_E | MASK_F | MASK_G, // G
	MASK_A | MASK_B | MASK_E | MASK_F | MASK_G, // P
	MASK_A | MASK_C | MASK_D | MASK_F | MASK_G, // S
	0,
	0
};

static void write_no_sig() {
	last_pps_tick_good = 0;
	tenth_ticks = 0;
	fake_blink = 0;
	memcpy_P((void*)disp_reg, no_sig_data, sizeof(no_sig_data));
}

static inline unsigned long timer_value() __attribute__ ((always_inline));
static inline unsigned long timer_value() {
	// We've configured event block 0-3 for timer C 4/5 capture.
	// CCA causes an interrupt, but CCB doesn't, so use a
	// synthetic capture to grab the current value. This avoids
	// having to deal with overflow propagation issues.
	EVSYS.STROBE = _BV(1); // event channel 1
	while(!((TCC4.INTFLAGS & TC4_CCBIF_bm)) && ((TCC5.INTFLAGS & TC5_CCBIF_bm))) ; // wait for both words
	unsigned long out = (((unsigned long)TCC5.CCB) << 16) | TCC4.CCB;
	TCC4.INTFLAGS = TC4_CCBIF_bm; // XXX why is this necessary?
	TCC5.INTFLAGS = TC5_CCBIF_bm;
	return out;
}

ISR(TCC5_CCA_vect) {
	while(!((TCC4.INTFLAGS & TC4_CCAIF_bm)) && ((TCC5.INTFLAGS & TC5_CCAIF_bm))) ; // wait for both words
	unsigned long this_tick = (((unsigned long)TCC5.CCA) << 16) | TCC4.CCA;
	TCC4.INTFLAGS = TC4_CCAIF_bm; // XXX why is this necessary?
	TCC5.INTFLAGS = TC5_CCAIF_bm;

	if (last_pps_tick_good) {
		// DIY GPS driven FLL for the 32 MHz oscillator.
		unsigned long pps_tick_count = this_tick - last_pps_tick;
		if (pps_tick_count < F_CPU) DFLLRC32M.CALA++; // too slow
		else if (pps_tick_count > F_CPU) DFLLRC32M.CALA--; // too fast
	}

	if (menu_pos == 0 && tenth_enable && last_pps_tick_good) {
		tenth_ticks = (this_tick - last_pps_tick) / 10;
		// For unknown reasons we seemingly sometimes get spurious
		// PPS interrupts. If the calculus leads us to believe a
		// a tenth of a second is less than 50 ms worth of system clock,
		// then it's not right - just skip it.
		if (tenth_ticks < FAST_PPS_TICKS) tenth_ticks = 0;
	} else {
		tenth_ticks = 0;
	}
	last_pps_tick_good = 1;
	last_pps_tick = this_tick;

	if (menu_pos) return;
	if (!gps_locked) {
		write_no_sig();
		return;
	}

	// If we're not going to show the tenths...
	if (tenth_ticks == 0) {
		disp_buf[DIGIT_100_MSEC] = 0; // blank
	} else {
		disp_buf[DIGIT_100_MSEC] = pgm_read_byte(&(character_set[0]));
		disp_buf[DIGIT_1_SEC] |= MASK_DP; // add a decimal point on seconds digit
	}

	disp_tenth = 0; // right now, 0 is showing.

	// Copy the display buffer data into the display.
	memcpy((void*)disp_reg, (const void *)disp_buf, sizeof(disp_reg));
}

// This is a precalculated array of 1 << n. The reason for it is that << results
// in a loop, and this needs to be fast.
static const unsigned char mask[] PROGMEM = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80 };

// This is the high priority display raster handler. We support multiple display brightness
// by splitting each digit's timeslice up into brightness periods. We turn the digit's
// segments on at the start and off early depending on the configured setting.
ISR(TCD5_OVF_vect) {
	static unsigned char current_digit;
	static unsigned char bright_step;
	TCD5.INTFLAGS = TC5_OVFIF_bm; // ack the interrupt
	if (++bright_step >= BRIGHTNESS_LEVELS) {
		bright_step = 0;
		// time to change digits
		if (++current_digit >= sizeof(mask)) current_digit = 0;
		DIGIT_VAL_REG = 0; // At max brightness, must turn off previous digit..
		for(int i = 0; i < 10; i++) // and waste a little bit of time (the driver chip needs at least 2 us)
			_NOP();
		DIGIT_SEL_REG = pgm_read_byte(&(mask[current_digit])); // This is (1 << current_digit)
		DIGIT_VAL_REG = disp_reg[current_digit]; // Light up the segments
	} else {
		if (bright_step > brightness)
			DIGIT_VAL_REG = 0; // turn the outputs off for the rest of this character's timeslot.
	}
}

static unsigned char check_buttons() {
	unsigned long now = timer_value();
	if (debounce_time != 0 && now - debounce_time < DEBOUNCE_TICKS) {
		// We don't pay any attention to the buttons during debounce time.
		return 0;
	} else {
		debounce_time = 0; // debounce is over
	}
	unsigned char status = PORT_SW & (SW_0_BIT | SW_1_BIT);
	status ^= (SW_0_BIT | SW_1_BIT); // invert the buttons - 0 means down.
	if (!((button_down == 0) ^ (status == 0))) return 0; // either no button is down, or a button is still down

	// Something *changed*, which means we must now start a debounce interval.
	debounce_time = now;
	if (!debounce_time) debounce_time++; // it's not allowed to be zero

	if (!button_down && status) {
		button_down = 1; // a button has been pushed
		return (status & SW_1_BIT)?SELECT:SET;
	}
	if (button_down && !status) {
		button_down = 0; // a button has been released
		return 0;
	}
	__builtin_unreachable(); // we'll never get here.
}

static void menu_render() {
	// blank the display
	memset((void*)disp_reg, 0, sizeof(disp_reg));
	switch(menu_pos) {
		case 0:
			// we're returning to time mode. Either leave it blank or indicate no signal.
			if (!gps_locked)
				write_no_sig();
			tenth_ticks = 0;
			break;
		case 1: // zone
        		disp_reg[0] = MASK_D | MASK_E | MASK_F | MASK_G; // t
        		disp_reg[1] = MASK_C | MASK_E | MASK_F | MASK_G; // h
			if (tz_hour < 0) {
        			disp_reg[3] = MASK_G; // -
			}
			disp_reg[4] = pgm_read_byte(&(character_set[abs(tz_hour) / 10]));
			disp_reg[5] = pgm_read_byte(&(character_set[abs(tz_hour) % 10]));
			break;
		case 2: // DST on/off
        		disp_reg[0] = MASK_B | MASK_C | MASK_D | MASK_E | MASK_G; // d
        		disp_reg[1] = MASK_A | MASK_C | MASK_D | MASK_F | MASK_G; // S
			switch(dst_mode) {
                                case DST_OFF:
                                        disp_reg[3] = MASK_C | MASK_D | MASK_E | MASK_G; // o
                                        disp_reg[4] = MASK_A | MASK_E | MASK_F | MASK_G; // F
                                        disp_reg[5] = MASK_A | MASK_E | MASK_F | MASK_G; // F
                                        break;
                                case DST_EU:
                                        disp_reg[3] = MASK_A | MASK_D | MASK_E | MASK_F | MASK_G; // E
                                        disp_reg[4] = MASK_B | MASK_C | MASK_D | MASK_E | MASK_F; // U
                                        break;
                                case DST_US:
                                        disp_reg[3] = MASK_B | MASK_C | MASK_D | MASK_E | MASK_F; // U
                                        disp_reg[4] = MASK_A | MASK_C | MASK_D | MASK_F | MASK_G; // S
                                        break;
                                case DST_AU:
                                        disp_reg[3] = MASK_A | MASK_B | MASK_C | MASK_E | MASK_F | MASK_G; // A
                                        disp_reg[4] = MASK_B | MASK_C | MASK_D | MASK_E | MASK_F; // U
                                        break;
                                case DST_NZ:
                                        disp_reg[3] = MASK_C | MASK_E | MASK_G; // n
                                        disp_reg[4] = MASK_A | MASK_B | MASK_D | MASK_E | MASK_G; // Z
                                        break;
                        }
			break;
		case 3: // 12/24 hour
        		disp_reg[1] = pgm_read_byte(&(character_set[ampm?1:2]));
        		disp_reg[2] = pgm_read_byte(&(character_set[ampm?2:4]));
        		disp_reg[4] = MASK_C | MASK_E | MASK_F | MASK_G; // h
        		disp_reg[5] = MASK_E | MASK_G; // r
			break;
		case 4: // tenths enabled
			disp_reg[0] = pgm_read_byte(&(character_set[1]));
			disp_reg[1] = pgm_read_byte(&(character_set[0]));
			if (tenth_enable) {
				disp_reg[3] = MASK_C | MASK_D | MASK_E | MASK_G; // o
				disp_reg[4] = MASK_C | MASK_E | MASK_G; // n
			} else {
				disp_reg[3] = MASK_C | MASK_D | MASK_E | MASK_G; // o
				disp_reg[4] = MASK_A | MASK_E | MASK_F | MASK_G; // F
				disp_reg[5] = MASK_A | MASK_E | MASK_F | MASK_G; // F
			}
			break;
		case 5: // colons enabled
        		disp_reg[0] = MASK_A | MASK_D | MASK_E | MASK_F; // C
        		disp_reg[1] = MASK_C | MASK_D | MASK_E | MASK_G; // o
        		disp_reg[2] = MASK_D | MASK_E | MASK_F; // L
        		disp_reg[3] = MASK_C | MASK_D | MASK_E | MASK_G; // o
			disp_reg[4] = MASK_C | MASK_E | MASK_G; // n
			disp_reg[5] = MASK_A | MASK_C | MASK_D | MASK_F | MASK_G; // S
			switch(colon_state) {
				case COLON_OFF: // nothing
					fake_blink = 0;
					disp_reg[7] = 0;
					break;
				case COLON_ON: // on solid
					fake_blink = 0;
					disp_reg[7] = MASK_COLON_HM | MASK_COLON_MS;
					break;
				case COLON_BLINK:
					// Don't bother setting disp_reg here. The fake blink handler will do it.
					fake_blink = timer_value();
					if (fake_blink == 0) fake_blink++;
					break;
			}
			break;
		case 6: // brightness
        		disp_reg[0] = MASK_C | MASK_D | MASK_E | MASK_F | MASK_G; // b
        		disp_reg[1] = MASK_E | MASK_G; // r
        		disp_reg[2] = MASK_B | MASK_C; // I
        		disp_reg[3] = MASK_A | MASK_C | MASK_D | MASK_E | MASK_F | MASK_G; // G
        		disp_reg[4] = MASK_C | MASK_E | MASK_F | MASK_G; // h
        		disp_reg[5] = MASK_D | MASK_E | MASK_F | MASK_G; // t
			break;
	}
}

static void menu_set() {
	switch(menu_pos) {
		case 0:
			// we're entering the menu system. Disable the tenth digit.
			tenth_ticks = 0;
			break;
		case 1:
			eeprom_write_byte(EE_TIMEZONE, tz_hour + 12);
			break;
		case 2:
			eeprom_write_byte(EE_DST_MODE, dst_mode);
			break;
		case 3:
			eeprom_write_byte(EE_AM_PM, ampm);
			break;
		case 4:
			eeprom_write_byte(EE_TENTHS, tenth_enable);
			break;
		case 5:
			fake_blink = 0; // We're done with that nonsense
			eeprom_write_byte(EE_COLONS, colon_state);
			break;
		case 6:
			eeprom_write_byte(EE_BRIGHTNESS, brightness);
			break;
	}
	if (++menu_pos > 6) menu_pos = 0;
	menu_render();
}

static void menu_select() {
	switch(menu_pos) {
		case 0: return; // ignore SET when just running
		case 1: // timezone
			if (++tz_hour >= 13) tz_hour = -12;
			break;
		case 2: // DST on/off
			if (++dst_mode > DST_MODE_MAX) dst_mode = 0;
			break;
		case 3: // 12/24 hour
			ampm = !ampm;
			break;
		case 4: // tenths enabled
			tenth_enable = !tenth_enable;
			break;
		case 5: // colons
			if (++colon_state > COLON_STATE_MAX) colon_state = 0;
			break;
		case 6: // brightness
			brightness = (brightness + 1) & 0x3; // 0-3
			break;
	}
	menu_render();
}

// main() never returns.
void __ATTR_NORETURN__ main(void) {

	// Run the CPU at 32 MHz.
	OSC.CTRL = OSC_RC32MEN_bm;
	while(!(OSC.STATUS & OSC_RC32MRDY_bm)) ; // wait for it.

	_PROTECTED_WRITE(CLK.CTRL, CLK_SCLKSEL_RC32M_gc); // switch to it
	OSC.CTRL &= ~(OSC_RC2MEN_bm); // we're done with the 2 MHz osc.

	//wdt_enable(WDTO_1S); // This is broken on XMegas.
	// This replacement code doesn't disable interrupts (but they're not on now anyway)
	_PROTECTED_WRITE(WDT.CTRL, WDT_PER_256CLK_gc | WDT_ENABLE_bm | WDT_CEN_bm);
	while(WDT.STATUS & WDT_SYNCBUSY_bm) ; // wait for it to take
	// We don't want a windowed watchdog.
	_PROTECTED_WRITE(WDT.WINCTRL, WDT_WCEN_bm);
	while(WDT.STATUS & WDT_SYNCBUSY_bm) ; // wait for it to take

	// Leave on only the parts of the chip we use.
	PR.PRGEN = PR_XCL_bm | PR_RTC_bm | PR_EDMA_bm;
	PR.PRPA = PR_DAC_bm | PR_ADC_bm | PR_AC_bm;
	PR.PRPC = PR_TWI_bm | PR_SPI_bm | PR_HIRES_bm;
	PR.PRPD = PR_USART0_bm;

	// Event 0 is PPS - it causes a timer capture.
	EVSYS.CH0MUX = EVSYS_CHMUX_PORTC_PIN0_gc;
	EVSYS.CH0CTRL = 0;
	// Event 4 is a carry from timer 4 to timer 5
	EVSYS.CH4MUX = EVSYS_CHMUX_TCC4_OVF_gc;
	EVSYS.CH4CTRL = 0;

	// Ports A and D are the mux for the display. Initialize all of them
	// output and low.
	DIGIT_VAL_REG = 0;
	DIGIT_SEL_REG = 0;
	PORTA.DIRSET = 0xff;
	PORTD.DIRSET = 0xff;

	PORTC.OUTSET = _BV(3); // TXD defaults to high, but we really don't use it anyway
	PORTC.DIRSET = _BV(3); // TXD is an output.

	// Send an event on the rising edge of PPS.
	PORTC.PIN0CTRL = PORT_ISC_RISING_gc;

	// Switches get pull-ups.
	PORTC.PIN4CTRL = PORT_OPC_PULLUP_gc;
	PORTC.PIN5CTRL = PORT_OPC_PULLUP_gc;

	rx_str_len = 0;
	nmea_ready = 0;

	// 9600 baud async serial, 8N1, low priority interrupt on receive
	USARTC0.CTRLA = USART_DRIE_bm | USART_RXCINTLVL_LO_gc;
	USARTC0.CTRLB = USART_RXEN_bm; // if we ever want to talk to GPS, we can turn on TX.
	USARTC0.CTRLC = USART_CHSIZE_8BIT_gc;
	USARTC0.CTRLD = 0;
	USARTC0.BAUDCTRLA = BSEL & 0xff;
	USARTC0.BAUDCTRLB = (BSEL >> 8) | (BSCALE << USART_BSCALE_gp);

	// TCC4 and 5 are a 32 bit cascaded counter with cascaded capture (on PPS).
	TCC4.CTRLA = TC45_CLKSEL_DIV1_gc; // 32 MHz timer clocking - 31.25 ns granularity
	TCC4.CTRLB = 0;
	TCC4.CTRLC = 0;
	TCC4.CTRLD = TC45_EVSEL_CH0_gc; // capture on event 0
	TCC4.CTRLE = TC45_CCBMODE_CAPT_gc | TC45_CCAMODE_CAPT_gc;
	TCC4.INTCTRLA = 0;
	TCC5.INTCTRLB = 0; // we're going to interrupt from TC5

	TCC5.CTRLA = TC45_CLKSEL_EVCH4_gc; // Clock from timer 4's overflow
	TCC5.CTRLB = 0;
	TCC5.CTRLC = 0;
	TCC5.CTRLD = TC5_EVDLY_bm | TC45_EVSEL_CH0_gc; // We're cascading 32 bits - we must delay capture events 1 cycle
	TCC5.CTRLE = TC45_CCBMODE_CAPT_gc | TC45_CCAMODE_CAPT_gc;
	TCC5.INTCTRLA = 0;
	TCC5.INTCTRLB = TC45_CCAINTLVL_MED_gc;

	// TCD5 is the timer for the display refresh. Its overflow triggers the rastering ISR.
	// A rastering rate of 10 kHz means a display latency of 100 microseconds, but it also
	// means that we're spending about half of the CPU doing it (~50 clocks spent in the ISR).
	TCD5.CTRLA = TC45_CLKSEL_DIV1_gc; // full speed clocking
	TCD5.CTRLB = 0;
	TCD5.CTRLC = 0;
	TCD5.CTRLD = 0;
	TCD5.CTRLE = 0;
	TCD5.INTCTRLA = TC45_OVFINTLVL_HI_gc;
	TCD5.INTCTRLB = 0;
	TCD5.PER = REFRESH_PERIOD;
	unsigned char ee_rd = eeprom_read_byte(EE_TIMEZONE);
	if (ee_rd == 0xff)
		tz_hour = -8;
	else
		tz_hour = ee_rd - 12;
	dst_mode = eeprom_read_byte(EE_DST_MODE);
	if (dst_mode > DST_MODE_MAX) dst_mode = DST_US;
	ampm = eeprom_read_byte(EE_AM_PM) != 0;

	tenth_enable = eeprom_read_byte(EE_TENTHS) != 0;
        colon_state = eeprom_read_byte(EE_COLONS);
	if (colon_state > COLON_STATE_MAX) colon_state = 1; // default to just on.

	gps_locked = 0;
	menu_pos = 0;
	debounce_time = 0;
	button_down = 0;
	last_pps_tick_good = 0;
	tenth_ticks = 0;
	disp_tenth = 0;
	fake_blink = 0;

	// Turn on just the display refresh interrupt to start with
	PMIC.CTRL = PMIC_HILVLEN_bm;
	sei();

	// Turn on the self-test for a second
	brightness = BRIGHTNESS_LEVELS - 1; // max
	memset((void*)disp_reg, 0xff, sizeof(disp_reg));

	// We want to wait for 1 second, but _delay_ms() assumes we get the whole
	// CPU. So we'll use the timer instead.
	unsigned long start = timer_value();
	while(timer_value() - start < F_CPU) wdt_reset();

	unsigned char b = eeprom_read_byte(EE_BRIGHTNESS);
	if (b >= BRIGHTNESS_LEVELS) b = BRIGHTNESS_LEVELS - 1; // default to max
	brightness = b;
	write_no_sig();

	// turn on the rest of the interrupt sources
	PMIC.CTRL |= PMIC_MEDLVLEN_bm | PMIC_LOLVLEN_bm;

	while(1) {
		wdt_reset();
		if (nmea_ready) {
			// We're doing this here to get the heavy processing
			// out of an ISR. It *is* a lot of work, after all.
			handleGPS();
			rx_str_len = 0; // clear the buffer
			nmea_ready = 0;
			continue;
		}
		unsigned long local_lpt, local_tt;
		unsigned char local_dt, local_lptg;
		// capture these values atomically so they can't change independently of each other.
		ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
			local_lptg = last_pps_tick_good;
			local_lpt = last_pps_tick;
			local_dt = disp_tenth;
			local_tt = tenth_ticks;
		}
		unsigned long now = timer_value();
		unsigned long current_tick = now - local_lpt;
		// If we've not seen a PPS pulse in a certain amount of time, then
		// without doing something like this, the wrong time would just get stuck.
		if (local_lptg && current_tick > LOST_PPS_TICKS) {
			write_no_sig();
			continue;
		}
		if (local_tt != 0) {
			unsigned int current_tenth = (unsigned int)((current_tick / local_tt) % 10);
			// We don't want to do the 9 -> 0 transition here. The PPS interrupt is absolutely
			// accurate. We don't need to worry about this code going 0->9 because if local_dt
			// was set to 0 by the capture ISR, then it HAD to also change local_lpt so that
			// current_tenth is now 0.
			if (local_dt != current_tenth && local_dt != 9) {
				// This is really only volatite during the 0 tenth ISR.
				disp_tenth = current_tenth;
				// Write the tenth-of-a-second digit, preserving the
				// decimal point state (just in case)
				// Do this atomically so the PPS ISR can't change it in the middle.
				// (should never happen because local_dt would be 9).
				ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
					disp_reg[DIGIT_100_MSEC] &= MASK_DP;
					disp_reg[DIGIT_100_MSEC] |= pgm_read_byte(&(character_set[current_tenth]));
				}
			}
		}
		if (fake_blink) {
			// This is necessary for the colon menu. We want to blink at 1 Hz (never mind that
			// in the actual clock, we blink at 1/2 Hz), and we have to do it whether GPS is
			// working or not (but it doesn't have to be accurate). We want it to be an even
			// multiple of the timer range so we don't have any discontinuities. 2^24 is 5%
			// more than 16E6 - close enough. We also want to start at the beginning of a
			// blink, so we'll offset from when we began fake_blinking. We also want to start
			// off (since it's menu position immediately follows 'full on').
			if (((now - fake_blink) >> 24) % 2) {
				disp_reg[DIGIT_MISC] |= MASK_COLON_HM | MASK_COLON_MS;
			} else {
				disp_reg[DIGIT_MISC] &= ~(MASK_COLON_HM | MASK_COLON_MS);
			}
		}
		unsigned char button = check_buttons();
		switch(button) {
			case SELECT:
				menu_select();
				break;
			case SET:
				menu_set();
				break;
		}
	}
	__builtin_unreachable();
}
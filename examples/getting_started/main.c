/* ----------------------------------------------------------------------------
 *         SAM Software Package License
 * ----------------------------------------------------------------------------
 * Copyright (c) 2015, Atmel Corporation
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the disclaimer below.
 *
 * Atmel's name may not be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * DISCLAIMER: THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ----------------------------------------------------------------------------
 */

/**
 *  \page getting-started Getting Started with sama5d4x Microcontrollers
 *
 *  \section Purpose
 *
 *  The Getting Started example will help new users get familiar with Atmel's
 *  sama5d4x microcontroller. This basic application shows the startup
 *  sequence of a chip and how to use its core peripherals.
 *
 *  \section Requirements
 *
 *  This package can be used with SAMA5D4-EK and SAMA5D4-XULT.
 *
 *  \section Description
 *
 *  The demonstration program makes two LEDs on the board blink at a fixed rate.
 *  This rate is generated by using Time tick timer. The blinking can be stopped
 *  using two buttons (one for each LED). If there is no enough buttons on board, please
 *  type "1" or "2" in the terminal application on PC to control the LEDs
 *  instead.
 *
 *  \section Usage
 *
 *  -# Build the program and download it inside the evaluation board. Please
 *     refer to the
 *     <a href="http://www.atmel.com/dyn/resources/prod_documents/6421B.pdf">
 *     SAM-BA User Guide</a>, the
 *     <a href="http://www.atmel.com/dyn/resources/prod_documents/doc6310.pdf">
 *     GNU-Based Software Development</a>
 *     application note or to the
 *     <a href="ftp://ftp.iar.se/WWWfiles/arm/Guides/EWARM_UserGuide.ENU.pdf">
 *     IAR EWARM User Guide</a>,
 *     depending on your chosen solution.
 *  -# On the computer, open and configure a terminal application
 *     (e.g. HyperTerminal on Microsoft Windows) with these settings:
 *    - 115200 bauds
 *    - 8 bits of data
 *    - No parity
 *    - 1 stop bit
 *    - No flow control
 *  -# Start the application.
 *  -# Two LEDs should start blinking on the board. In the terminal window, the
 *     following text should appear (values depend on the board and chip used):
 *     \code
 *      -- Getting Started Example xxx --
 *      -- SAMxxxxx-xx
 *      -- Compiled: xxx xx xxxx xx:xx:xx --
 *     \endcode
 *  -# Pressing and release button 1 or type "1" in the terminal application on
 *     PC should make the first LED stop & restart blinking.
 *     Pressing and release button 2 or type "2" in the terminal application on
 *     PC should make the other LED stop & restart blinking.
 *
 *  \section References
 *  - getting-started/main.c
 *  - pio.h
 *  - pio_it.h
 *  - led.h
 *  - trace.h
 */

/** \file
 *
 *  This file contains all the specific code for the getting-started example.
 *
 */

/*----------------------------------------------------------------------------
 *        Headers
 *----------------------------------------------------------------------------*/

#include "board.h"
#include "chip.h"
#include "trace.h"
#include "compiler.h"

#include "cortex-a/mmu.h"

#include "peripherals/aic.h"
#include "peripherals/pio.h"
#include "peripherals/pit.h"
#include "peripherals/pmc.h"
#include "peripherals/tc.h"
#include "peripherals/twid.h"
#include "peripherals/wdt.h"

#include "misc/console.h"
#include "misc/led.h"

#include "power/act8945a.h"

#include <stdbool.h>
#include <stdio.h>

/*----------------------------------------------------------------------------
 *        Local definitions
 *----------------------------------------------------------------------------*/

/** LED0 blink time, LED1 blink half this time, in ms */
#define BLINK_PERIOD        1000

/** Delay for pushbutton debouncing (in milliseconds). */
#define DEBOUNCE_TIME       500

/** Maximum number of handled led */
#define MAX_LEDS            5

/*----------------------------------------------------------------------------
 *        Local variables
 *----------------------------------------------------------------------------*/

#ifdef CONFIG_HAVE_PMIC_ACT8945A
struct _pin act8945a_pins[] = ACT8945A_PINS;
struct _twi_desc act8945a_twid = {
	.addr = ACT8945A_ADDR,
	.freq = ACT8945A_FREQ,
	.transfert_mode = TWID_MODE_POLLING
};
struct _act8945a act8945a = {
	.desc = {
		.pin_chglev = ACT8945A_PIN_CHGLEV,
		.pin_irq = ACT8945A_PIN_IRQ,
		.pin_lbo = ACT8945A_PIN_LBO
	}
};
#endif

#ifdef PINS_PUSHBUTTONS
/** Pushbutton \#1 pin instance. */
static const struct _pin button_pins[] = PINS_PUSHBUTTONS;
#endif

/* Only used to get the number of available leds */
static const struct _pin pinsLeds[] = PINS_LEDS;
/** Number of available leds */
static const uint32_t num_leds = ARRAY_SIZE(pinsLeds);
volatile bool led_status[MAX_LEDS];

/** Global timestamp in milliseconds since start of application */
volatile uint32_t dwTimeStamp = 0;

/*----------------------------------------------------------------------------
 *        Local functions
 *----------------------------------------------------------------------------*/

/**
 *  \brief Process Buttons Events
 *
 *  Change active states of LEDs when corresponding button events happened.
 */
static void process_button_evt(uint8_t bt)
{
	if (bt >= num_leds) {
		return;
	}
	led_status[bt] = !led_status[bt];
	if (bt == 0) {
		if (!led_status[bt]) {
			led_clear(bt);
		}
	} else if (bt < num_leds) {
		if (led_status[bt]) {
			led_set(bt);
		} else {
			led_clear(bt);
		}
	}
}

/**
 *  \brief Handler for Buttons rising edge interrupt.
 *
 *  Handle process led1 status change.
 */
static void pio_handler(uint32_t mask, uint32_t status, void* user_arg)
{
	int i = 0;

	/* unused */
	(void)user_arg;

	for (i = 0; i < ARRAY_SIZE(button_pins); ++i) {
		if (status & button_pins[i].mask)
			process_button_evt(i);
	}
}

/**
 *  \brief Handler for DBGU input.
 *
 *  Handle process LED1 or LED2 status change.
 */
static void console_handler(void)
{
	uint8_t key;
	if (!console_is_rx_ready())
		return;
	key = console_get_char();
	if (key >= '0' && key <= '9') {
		process_button_evt(key - '0');
	} else if (key == 's') {
		tc_stop(TC0, 0);
	} else if (key == 'b') {
		tc_start(TC0, 0);
	}
}

/**
 *  \brief Handler for PIT interrupt.
 */
static void pit_handler(void)
{
	uint32_t status;

	/* Read the PIT status register */
	status = pit_get_status() & PIT_SR_PITS;
	if (status != 0) {

		/* 1 = The Periodic Interval timer has reached PIV
		 * since the last read of PIT_PIVR. Read the PIVR to
		 * acknowledge interrupt and get number of ticks
		 * Returns the number of occurrences of periodic
		 * intervals since the last read of PIT_PIVR. */
		dwTimeStamp += (pit_get_pivr() >> 20);
	}
}

/**
 *  \brief Configure the periodic interval timer (PIT) to generate an interrupt
 *  every interrupt every millisecond
 */
static void configure_pit(void)
{
	/* Enable PIT controller */
	pmc_enable_peripheral(ID_PIT);

	/* Initialize the PIT to the desired frequency */
	pit_init(BLINK_PERIOD);

	/* Configure interrupt on PIT */
	aic_enable(ID_PIT);
	aic_set_source_vector(ID_PIT, pit_handler);

	pit_enable_it();

	/* Enable the pit */
	pit_enable();
}

/**
 *  \brief Configure the Pushbuttons
 *
 *  Configure the PIO as inputs and generate corresponding interrupt when
 *  pressed or released.
 */
static void configure_buttons(void)
{
	int i = 0;
	for (i = 0; i < ARRAY_SIZE(button_pins); ++i)
	{
		/* Configure pios as inputs. */
		pio_configure(&button_pins[i], 1);

		/* Adjust pio debounce filter parameters, uses 10 Hz filter. */
		pio_set_debounce_filter(&button_pins[i], 10);

		/* Initialize pios interrupt with its handlers, see
		 * PIO definition in board.h. */
		pio_configure_it(&button_pins[i]);
		pio_add_handler_to_group(button_pins[i].group,
				      button_pins[i].mask, pio_handler, NULL);

		/* Enable PIO line interrupts. */
		pio_enable_it(button_pins);
	}
}

/**
 *  \brief Configure LEDs
 *
 *  Configures LEDs \#1 and \#2 (cleared by default).
 */
static void configure_leds(void)
{
	int i = 0;
	for (i = 0; i < num_leds; ++i) {
		led_configure(i);
	}
}

/**
 *  Interrupt handler for TC0 interrupt. Toggles the state of LED\#2.
 */
static void tc_handler(void)
{
	uint32_t dummy, i;

	/* Clear status bit to acknowledge interrupt */
	dummy = tc_get_status(TC0, 0);
	(void) dummy;

	/** Toggle LEDs state. */
	for (i = 1; i < num_leds; ++i) {
		if (led_status[i]) {
			led_toggle(i);
			printf("%i ", (unsigned int)i);
		}
	}
}

/**
 *  Configure Timer Counter 0 to generate an interrupt every 250ms.
 */
static void configure_tc(void)
{
	/** Enable peripheral clock. */
	pmc_enable_peripheral(ID_TC0);

	/* Put the source vector */
	aic_set_source_vector(ID_TC0, tc_handler);

	/** Configure TC for a 4Hz frequency and trigger on RC compare. */
	tc_trigger_on_freq(TC0, 0, 4);

	/* Configure and enable interrupt on RC compare */
	tc_enable_it(TC0, 0, TC_IER_CPCS);
	aic_enable(ID_TC0);

	/* Start the counter if LED1 is enabled. */
	if (led_status[1]) {
		tc_start(TC0, 0);
	}
}

/**
 *  Waits for the given number of milliseconds (using the dwTimeStamp generated
 *  by the SAM3's microcontrollers's system tick).
 *  \param delay  Delay to wait for, in milliseconds.
 */
static void _Wait(unsigned long delay)
{
	uint32_t start = dwTimeStamp;
	uint32_t elapsed;

	do {
		elapsed = dwTimeStamp;
		elapsed -= start;
	} while (elapsed < delay);
}

/*----------------------------------------------------------------------------
 *        Global functions
 *----------------------------------------------------------------------------*/

/**
 *  \brief getting-started Application entry point.
 *
 *  \return Unused (ANSI-C compatibility).
 */
int main(void)
{
	int i = 0;
	led_status[0] = true;
	for (i = 1; i < num_leds; ++i) {
		led_status[i] = led_status[i-1];
	}

	/* Disable watchdog */
	wdt_disable();

	/* Disable all PIO interrupts */
	pio_reset_all_it();

	/* Initialize console */
	console_configure(CONSOLE_BAUDRATE);

	/* Output example information */
	printf("-- Getting Started Example %s --\n\r", SOFTPACK_VERSION);
	printf("-- %s\n\r", BOARD_NAME);
	printf("-- Compiled: %s %s --\n\r", __DATE__, __TIME__);

#ifdef CONFIG_HAVE_PMIC_ACT8945A
	pio_configure(act8945a_pins, ARRAY_SIZE(act8945a_pins));
	if (act8945a_configure(&act8945a, &act8945a_twid)) {
		act8945a_set_regulator_voltage(&act8945a, 6, 2500);
		act8945a_enable_regulator(&act8945a, 6, true);
	} else {
		printf("--E-- Error initializing ACT8945A PMIC\n\r");
	}
#endif

	/* Configure PIT. */
	printf("Configure PIT \n\r");
	configure_pit();

	/* Configure TC */
	printf("Configure TC.\n\r");
	configure_tc();

	/* PIO configuration for LEDs and Buttons. */
	printf("Configure LED PIOs.\n\r");
	configure_leds();
	printf("Configure buttons with debouncing.\n\r");
	configure_buttons();

	printf("Initializing console interrupts\r\n");
	aic_set_source_vector(CONSOLE_ID, console_handler);
	aic_enable(CONSOLE_ID);
	console_enable_interrupts(US_IER_RXRDY);

	printf("use push buttons or console key 0 to 9.\n\r");
	printf("Press the number of the led to make it "
	       "start or stop blinking.\n\r");
	printf("Press 's' to stop the TC and 'b' to start it\r\n");

	while (1) {

		/* Wait for LED to be active */
		while (!led_status[0]);

		/* Toggle LED state if active */
		if (led_status[0]) {
			led_toggle(0);
			printf("0 ");
		}

		/* Wait for 500ms */
		_Wait(500);
	}
}

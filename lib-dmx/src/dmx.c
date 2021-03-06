/**
 * @file dmx.c
 *
 * @brief This file implements the DMX512/RDM receive state-machine. It
 * uses the Fast Interrupt Request (FIQ) for accurate timing.
 * The Interrupt Request (IRQ) is used for sending DMX data.
 *
 */
/* Copyright (C) 2015, 2016 by Arjan van Vught mailto:info@raspberrypi-dmx.nl
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "arm/arm.h"
#include "arm/synchronize.h"
#include "arm/pl011.h"

#include "bcm2835.h"
#include "bcm2835_gpio.h"
#include "bcm2835_vc.h"

#include "irq_timer.h"

#include "gpio.h"
#include "util.h"
#include "dmx.h"
#include "rdm.h"
#include "rdm_e120.h"

///< State of receiving DMX/RDM Bytes
typedef enum {
	IDLE = 0,	///<
	BREAK,		///<
	MAB,		///<
	DMXDATA,	///<
	RDMDATA,	///<
	CHECKSUMH,	///<
	CHECKSUML,	///<
	RDMDISCFE,	///<
	RDMDISCEUID,///<
	RDMDISCECS	///<
} _dmx_state;

static volatile uint16_t dmx_data_buffer_index_head = (uint16_t) 0;				///<
static volatile uint16_t dmx_data_buffer_index_tail = (uint16_t) 0;				///<
static struct _dmx_data dmx_data[DMX_DATA_BUFFER_INDEX_ENTRIES] ALIGNED;		///<
static uint8_t dmx_data_previous[DMX_DATA_BUFFER_SIZE] ALIGNED;					///<
static uint8_t dmx_receive_state = IDLE;										///< Current state of DMX receive
static volatile uint16_t dmx_data_index = (uint16_t) 0;							///<
static uint32_t dmx_output_break_time = (uint32_t) DMX_TRANSMIT_BREAK_TIME_MIN;	///<
static uint32_t dmx_output_mab_time = (uint32_t) DMX_TRANSMIT_MAB_TIME_MIN;		///<
static uint32_t dmx_output_period = DMX_TRANSMIT_PERIOD_DEFAULT;				///<
static uint32_t dmx_output_period_requested = DMX_TRANSMIT_PERIOD_DEFAULT;		///<
static uint16_t dmx_send_data_length = (uint16_t) DMX_UNIVERSE_SIZE + 1;		///< SC + UNIVERSE SIZE
static uint8_t dmx_port_direction = DMX_PORT_DIRECTION_INP;						///<
static volatile uint32_t dmx_fiq_micros_current = (uint32_t) 0;					///< Timestamp FIQ
static volatile uint32_t dmx_fiq_micros_previous = (uint32_t) 0;				///< Timestamp previous FIQ
static volatile bool dmx_is_previous_break_dmx = false;							///< Is the previous break from a DMX packet?
static volatile uint32_t dmx_break_to_break_latest = (uint32_t) 0;				///<
static volatile uint32_t dmx_break_to_break_previous = (uint32_t) 0;			///<
static volatile uint32_t dmx_slots_in_packet_previous = (uint32_t) 0;			///<
static volatile uint8_t dmx_send_state = IDLE;									///<
static volatile bool dmx_send_always = false;									///<
//static volatile uint32_t dmx_irq_micros = 0;									///<
static volatile uint32_t dmx_send_break_micros = (uint32_t) 0;					///<

static volatile uint16_t rdm_data_buffer_index_head = (uint16_t) 0;				///<
static volatile uint16_t rdm_data_buffer_index_tail = (uint16_t) 0;				///<
static uint8_t rdm_data_buffer[RDM_DATA_BUFFER_INDEX_ENTRIES][RDM_DATA_BUFFER_SIZE] ALIGNED;///<
static volatile uint16_t rdm_checksum = (uint16_t) 0;							///<
static volatile uint32_t rdm_data_receive_end = (uint32_t) 0;					///<
static volatile uint8_t rdm_disc_index = (uint8_t) 0;							///<

static volatile uint32_t dmx_updates_per_seconde= (uint32_t) 0;					///<
static uint32_t dmx_packets_previous = (uint32_t) 0;							///<
static volatile struct _total_statistics total_statistics ALIGNED;				///<

/**
 * @ingroup dmx
 *
 * @return
 */
const volatile uint32_t dmx_get_updates_per_seconde(void) {
	dmb();
	return dmx_updates_per_seconde;
}

/**
 * @ingroup dmx
 *
 * @param period
 */
void dmx_set_output_period(const uint32_t period) {
	const uint32_t package_length_us = dmx_output_break_time + dmx_output_mab_time + (dmx_send_data_length * 44);

	dmx_output_period_requested = period;

	if (period != 0) {
		if (period < package_length_us) {
			dmx_output_period = (uint32_t) MAX(DMX_TRANSMIT_BREAK_TO_BREAK_TIME_MIN, package_length_us + 44);
		} else {
			dmx_output_period = period;
		}
	} else {
		dmx_output_period = (uint32_t) MAX(DMX_TRANSMIT_BREAK_TO_BREAK_TIME_MIN, package_length_us + 44);
	}
}

/**
 * @ingroup dmx
 *
 * @param send_data_length
 */
static void dmx_set_send_data_length(uint16_t send_data_length) {
	dmx_send_data_length = send_data_length;

	dmx_set_output_period(dmx_output_period_requested);
}

/**
 * @ingroup dmx
 *
 * @param data
 * @param length
 */
void dmx_set_send_data(const uint8_t *data, const uint16_t length) {
	(void *)_memcpy(dmx_data[0].data, data, (size_t)length);

	dmx_set_send_data_length(length);
}

/**
 * @ingroup dmx
 *
 */
void dmx_clear_data(void) {
	uint32_t i = sizeof(dmx_data) / sizeof(uint32_t);
	uint32_t *p = (uint32_t *)dmx_data;

	while (i-- != (uint32_t) 0) {
		*p++ = (uint32_t) 0;
	}
}

/**
 * @ingroup dmx
 *
 * @return
 */
const uint32_t dmx_get_output_period(void) {
	return dmx_output_period;
}

/**
 * @ingroup dmx
 *
 * @return
 */
const uint16_t dmx_get_send_data_length(void) {
	return dmx_send_data_length;
}

/**
 * @ingroup rdm
 *
 * @return
 */
const uint8_t *rdm_get_available(void)  {
	if (rdm_data_buffer_index_head == rdm_data_buffer_index_tail) {
		return NULL;
	} else {
		const uint8_t *p = &rdm_data_buffer[rdm_data_buffer_index_tail][0];
		rdm_data_buffer_index_tail = (rdm_data_buffer_index_tail + 1) & RDM_DATA_BUFFER_INDEX_MASK;
		return p;
	}
}

/**
 * @ingroup rdm
 *
 * @return
 */
const uint8_t *rdm_get_current_data(void) {
	return &rdm_data_buffer[rdm_data_buffer_index_tail][0];
}

/**
 * @ingroup dmx
 *
 * @return
 */
const uint8_t *dmx_get_available(void)  {
	dmb();
	if (dmx_data_buffer_index_head == dmx_data_buffer_index_tail) {
		return NULL;
	} else {
		const uint8_t *p = dmx_data[dmx_data_buffer_index_tail].data;
		dmx_data_buffer_index_tail = (dmx_data_buffer_index_tail + 1) & DMX_DATA_BUFFER_INDEX_MASK;
		return p;
	}
}

/**
 * @ingroup rdm
 *
 * @return
 */
const uint8_t *dmx_get_current_data(void) {
	return dmx_data[dmx_data_buffer_index_tail].data;
}

/**
 * @ingroup dmx
 *
 * @return
 */
const volatile uint8_t dmx_get_receive_state(void) {
	dmb();
	return dmx_receive_state;
}

/**
 * @ingroup dmx
 *
 * The DMX data is changed when slots in packets is changed,
 * or when the data itself is changed.
 *
 * @return
 */
const uint8_t *dmx_is_data_changed(void) {
	uint16_t i;
	uint8_t const *p = (uint8_t *)dmx_get_available();
	uint32_t *src = (uint32_t *)p;
	uint32_t *dst = (uint32_t *)dmx_data_previous;
	bool is_changed = false;

	if (src == NULL) {
		return NULL;
	}

	const struct _dmx_data *dmx_statistics = (struct _dmx_data *)p;

	if (dmx_statistics->statistics.slots_in_packet != dmx_slots_in_packet_previous) {
		dmx_slots_in_packet_previous = dmx_statistics->statistics.slots_in_packet;
		for (i = 0; i < DMX_DATA_BUFFER_SIZE / 4; i++) {
			*dst= *src;
			dst++;
			src++;
		}
		return p;
	}

	for (i = 0; i < DMX_DATA_BUFFER_SIZE / 4; i++) {
		if (*dst != *src) {
			*dst = *src;
			is_changed = true;
		}
		dst++;
		src++;
	}

	return (is_changed ? p : NULL);
}

/**
 * @ingroup dmx
 *
 * @return
 */
const _dmx_port_direction dmx_get_port_direction(void) {
	return dmx_port_direction;
}

/**
 * @ingroup dmx
 *
 * @return
 */
const uint32_t rdm_get_data_receive_end(void) {
	return rdm_data_receive_end;
}

/**
 * @ingroup dmx
 *
 * @return
 */
const uint32_t dmx_get_output_break_time(void) {
	return dmx_output_break_time;
}

/**
 * @ingroup dmx
 *
 * @param break_time
 */
void dmx_set_output_break_time(const uint32_t break_time) {
	dmx_output_break_time = MAX((uint32_t)DMX_TRANSMIT_BREAK_TIME_MIN, break_time);

	dmx_set_output_period(dmx_output_period_requested);
}

/**
 * @ingroup dmx
 *
 * @return
 */
const uint32_t dmx_get_output_mab_time(void) {
	return dmx_output_mab_time;
}

/**
 * @ingroup dmx
 *
 * @param mab_time
 */
void dmx_set_output_mab_time(const uint32_t mab_time) {
	dmx_output_mab_time = MAX((uint32_t)DMX_TRANSMIT_MAB_TIME_MIN, mab_time);

	dmx_set_output_period(dmx_output_period_requested);
}

/**
 * @ingroup dmx
 *
 */
void dmx_reset_total_statistics(void) {
	total_statistics.dmx_packets = (uint32_t) 0;
	total_statistics.rdm_packets = (uint32_t) 0;
}

/**
 * @ingroup dmx
 *
 * @return
 */
const volatile struct _total_statistics *dmx_get_total_statistics(void) {
	return &total_statistics;
}

/**
 * @ingroup dmx
 *
 * Interrupt handler for continues receiving DMX512 data.
 *
 */
static void __attribute__((interrupt("FIQ"))) fiq_dmx_in_handler(void) {
	dmb();

#ifdef LOGIC_ANALYZER
	bcm2835_gpio_set(GPIO_ANALYZER_CH1);
#endif

	dmx_fiq_micros_current = BCM2835_ST->CLO;

	const uint32_t dr = BCM2835_PL011->DR;

	if (dr & PL011_DR_BE) {
		dmx_receive_state = BREAK;
		dmx_break_to_break_latest = dmx_fiq_micros_current;
#ifdef LOGIC_ANALYZER
		bcm2835_gpio_set(GPIO_ANALYZER_CH2);	// BREAK
		bcm2835_gpio_clr(GPIO_ANALYZER_CH4);	// IDLE
#endif
	} else {
		const uint8_t data = dr & 0xFF;

		switch (dmx_receive_state) {
		case IDLE:
			if (data == 0xFE) {
				dmx_receive_state = RDMDISCFE;
				rdm_data_buffer[rdm_data_buffer_index_head][0] = 0xFE;
				dmx_data_index = 1;
			}
			break;
		case BREAK:
			switch (data) {
			case DMX512_START_CODE:
				dmx_receive_state = DMXDATA;
				dmx_data[dmx_data_buffer_index_head].data[0] = DMX512_START_CODE;
				dmx_data_index = 1;
				total_statistics.dmx_packets = total_statistics.dmx_packets + 1;
				if (dmx_is_previous_break_dmx) {
					dmx_data[dmx_data_buffer_index_head].statistics.break_to_break = dmx_break_to_break_latest - dmx_break_to_break_previous;
					dmx_break_to_break_previous = dmx_break_to_break_latest;

				} else {
					dmx_is_previous_break_dmx = true;
					dmx_break_to_break_previous = dmx_break_to_break_latest;
				}
#ifdef LOGIC_ANALYZER
				bcm2835_gpio_clr(GPIO_ANALYZER_CH2);	// BREAK
			    bcm2835_gpio_set(GPIO_ANALYZER_CH3);	// DMX DATA
#endif
				break;
			case E120_SC_RDM:
				dmx_receive_state = RDMDATA;
				rdm_data_buffer[rdm_data_buffer_index_head][0] = E120_SC_RDM;
				rdm_checksum = E120_SC_RDM;
				dmx_data_index = 1;
				total_statistics.rdm_packets = total_statistics.rdm_packets + 1;
				dmx_is_previous_break_dmx = false;
#ifdef LOGIC_ANALYZER
				bcm2835_gpio_clr(GPIO_ANALYZER_CH2);	// BREAK
			    bcm2835_gpio_set(GPIO_ANALYZER_CH3);	// DMX DATA
#endif
				break;
			default:
				dmx_receive_state = IDLE;
				dmx_is_previous_break_dmx = false;
#ifdef LOGIC_ANALYZER
				bcm2835_gpio_clr(GPIO_ANALYZER_CH2);	// BREAK
				bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
#endif
				break;
			}
			break;
		case DMXDATA:
			dmx_data[dmx_data_buffer_index_head].statistics.slot_to_slot = dmx_fiq_micros_current - dmx_fiq_micros_previous;
			if (dmx_data[dmx_data_buffer_index_head].statistics.slot_to_slot < 44) { // Broadcom BUG ? FIQ is late
				dmx_data[dmx_data_buffer_index_head].statistics.slot_to_slot = (uint32_t)44;
			}
			dmx_data[dmx_data_buffer_index_head].data[dmx_data_index++] = data;
		    BCM2835_ST->C1 = dmx_fiq_micros_current + dmx_data[0].statistics.slot_to_slot + (uint32_t)12;
			if (dmx_data_index > DMX_UNIVERSE_SIZE) {
				dmx_receive_state = IDLE;
				dmx_data[dmx_data_buffer_index_head].statistics.slots_in_packet = DMX_UNIVERSE_SIZE;
				dmx_data_buffer_index_head = (dmx_data_buffer_index_head + 1) & DMX_DATA_BUFFER_INDEX_MASK;
#ifdef LOGIC_ANALYZER
				bcm2835_gpio_clr(GPIO_ANALYZER_CH3);	// DMX DATA
				bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
#endif
			}
			break;
		case RDMDATA:
			if (dmx_data_index > RDM_DATA_BUFFER_SIZE) {
				dmx_receive_state = IDLE;
#ifdef LOGIC_ANALYZER
				bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
#endif
			} else {
				rdm_data_buffer[rdm_data_buffer_index_head][dmx_data_index++] = data;
				rdm_checksum += data;

				const struct _rdm_command *p = (struct _rdm_command *)(&rdm_data_buffer[rdm_data_buffer_index_head][0]);
				if (dmx_data_index == p->message_length) {
					dmx_receive_state = CHECKSUMH;
				}
			}
			break;
		case CHECKSUMH:
			rdm_data_buffer[rdm_data_buffer_index_head][dmx_data_index++] =	data;
			rdm_checksum -= data << 8;
			dmx_receive_state = CHECKSUML;
			break;
		case CHECKSUML:
			rdm_data_buffer[rdm_data_buffer_index_head][dmx_data_index++] = data;
			rdm_checksum -= data;
			const struct _rdm_command *p = (struct _rdm_command *)(&rdm_data_buffer[rdm_data_buffer_index_head][0]);
			if ((rdm_checksum == 0) && (p->sub_start_code == E120_SC_SUB_MESSAGE)) {
				rdm_data_buffer_index_head = (rdm_data_buffer_index_head + 1) & RDM_DATA_BUFFER_INDEX_MASK;
				rdm_data_receive_end = BCM2835_ST->CLO;
			}
			dmx_receive_state = IDLE;
#ifdef LOGIC_ANALYZER
			bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
#endif
			break;
		case RDMDISCFE:
			switch (data) {
			case 0xFE:
				rdm_data_buffer[rdm_data_buffer_index_head][dmx_data_index++] = 0xFE;
				break;
			case 0xAA:
				rdm_data_buffer[rdm_data_buffer_index_head][dmx_data_index++] = 0xAA;
				dmx_receive_state = RDMDISCEUID;
				rdm_disc_index = 0;
				break;
			default:
				dmx_receive_state = IDLE;
#ifdef LOGIC_ANALYZER
				bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
#endif
				break;
			}
			break;
		case RDMDISCEUID:
			rdm_data_buffer[rdm_data_buffer_index_head][dmx_data_index++] = data;
			rdm_disc_index++;
			if (rdm_disc_index == 2 * RDM_UID_SIZE) {
				dmx_receive_state = RDMDISCECS;
				rdm_disc_index = 0;
			}
			break;
		case RDMDISCECS:
			rdm_data_buffer[rdm_data_buffer_index_head][dmx_data_index++] = data;
			rdm_disc_index++;
			if (rdm_disc_index == 4) {
				rdm_data_buffer_index_head = (rdm_data_buffer_index_head + 1) & RDM_DATA_BUFFER_INDEX_MASK;
				dmx_receive_state = IDLE;
				rdm_data_receive_end = BCM2835_ST->CLO;
#ifdef LOGIC_ANALYZER
				bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
#endif
			}
			break;
		default:
			break;
		}
	}

	dmx_fiq_micros_previous = dmx_fiq_micros_current;

#ifdef LOGIC_ANALYZER
	bcm2835_gpio_clr(GPIO_ANALYZER_CH1);
#endif

	dmb();
}

static void irq_timer1_dmx_receive(const uint32_t clo) {
	if (dmx_receive_state == DMXDATA) {
		if (clo - dmx_fiq_micros_current > dmx_data[0].statistics.slot_to_slot) {
			dmx_receive_state = IDLE;
			dmx_data[dmx_data_buffer_index_head].statistics.slots_in_packet = dmx_data_index - 1;
			dmx_data_buffer_index_head = (dmx_data_buffer_index_head + 1) & DMX_DATA_BUFFER_INDEX_MASK;
#ifdef LOGIC_ANALYZER
			bcm2835_gpio_clr(GPIO_ANALYZER_CH3);	// DMX DATA
			bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
#endif
		} else {
			BCM2835_ST->C1 = clo + dmx_data[dmx_data_buffer_index_head].statistics.slot_to_slot;
		}
	}
}

static void irq_timer3_dmx_receive(const uint32_t clo) {
	BCM2835_ST->C3 = clo + (uint32_t) 1000000;
	dmx_updates_per_seconde = total_statistics.dmx_packets - dmx_packets_previous;
	dmx_packets_previous = total_statistics.dmx_packets;
}

static void irq_timer1_dmx_send(const uint32_t clo) {
	switch (dmx_send_state) {
	case IDLE:
		BCM2835_ST->C1 = clo + dmx_output_break_time;
		BCM2835_PL011->LCRH = PL011_LCRH_WLEN8 | PL011_LCRH_STP2 | PL011_LCRH_BRK;
		dmx_send_break_micros = clo;
		dmb();
		dmx_send_state = BREAK;
		break;
	case BREAK:
		BCM2835_ST->C1 = clo + dmx_output_mab_time;
		BCM2835_PL011->LCRH = PL011_LCRH_WLEN8 | PL011_LCRH_STP2;
		dmb();
		dmx_send_state = MAB;
		break;
		//udelay(dmx_output_mab_time);
		/* no break */
	case MAB:
		BCM2835_ST->C1 = dmx_send_break_micros + dmx_output_period;
		/* dmx_send_state = DMXDATA; */
		uint16_t i = 0;
		for (i = 0; i < dmx_send_data_length; i++) {
			while ((BCM2835_PL011->FR & PL011_FR_TXFF) != 0)
				;
			BCM2835_PL011->DR = dmx_data[0].data[i];
		}
		while ((BCM2835_PL011->FR & PL011_FR_BUSY) != 0)
			;
		dmb();
		dmx_send_state = IDLE;
		break;
	default:
		dmb();
		dmx_send_state = IDLE;
		break;
	}
}

/**
 * @ingroup dmx
 *
 */
static void dmx_start_data(void) {
	switch (dmx_port_direction) {
	case DMX_PORT_DIRECTION_OUTP:
		dmx_send_always = true;
		dmb();
		dmx_send_state = IDLE;

		irq_timer_set(IRQ_TIMER_1, irq_timer1_dmx_send);

		const uint32_t clo = BCM2835_ST->CLO;

		if (clo - dmx_send_break_micros > dmx_output_period)  {
			BCM2835_ST->C1 = clo + 4;
		} else {
			BCM2835_ST->C1 = dmx_output_period + dmx_send_break_micros + 4;
		}

		dmb();

		break;
	case DMX_PORT_DIRECTION_INP:
		dmb();
		dmx_receive_state = IDLE;

		irq_timer_set(IRQ_TIMER_1, irq_timer1_dmx_receive);
		irq_timer_set(IRQ_TIMER_3, irq_timer3_dmx_receive);

		BCM2835_ST->C3 = BCM2835_ST->CLO + (uint32_t) 1000000;

		dmb();

		__enable_fiq();

		dmb();
		break;
	default:
		break;
	}
}

/**
 * @ingroup dmx
 *
 * If \ref dmx_send_always is true, then the IRQ routine is outputting DMX512.
 * We need to wait until all data is sent. When finished the state machine is in state IDLE.
 * At this time we can set the flag \ref dmx_send_always to false.
 *
 * The receiving of DMX data is stopped by disabling the FIQ.
 *
 */
static void dmx_stop_data(void) {
	int i;

	if (dmx_send_always) {
		const uint32_t clo = BCM2835_ST->CLO;
		do {
			dmb();
			if (dmx_send_state == IDLE) {
				break;
			}
		} while (BCM2835_ST->CLO - clo < dmx_output_period);
		dmx_send_always = false;
		irq_timer_set(IRQ_TIMER_1, NULL);
	}

	__disable_fiq();

	dmb();
	dmx_receive_state = IDLE;

	for (i = 0; i < DMX_DATA_BUFFER_INDEX_ENTRIES; i++) {
		dmx_data[i].statistics.slots_in_packet = 0;
	}
}

/**
 * @ingroup dmx
 *
 * @param port_direction \ref _dmx_port_direction
 * @param enable_data
 */
void dmx_set_port_direction(const _dmx_port_direction port_direction, const bool enable_data) {
	dmx_stop_data();

	switch (port_direction) {
	case DMX_PORT_DIRECTION_OUTP:
		bcm2835_gpio_set(GPIO_DMX_DATA_DIRECTION);	// 0 = input, 1 = output
		dmx_port_direction = DMX_PORT_DIRECTION_OUTP;
		break;
	case DMX_PORT_DIRECTION_INP:
		bcm2835_gpio_clr(GPIO_DMX_DATA_DIRECTION);	// 0 = input, 1 = output
		dmx_port_direction = DMX_PORT_DIRECTION_INP;
		break;
	default:
		bcm2835_gpio_clr(GPIO_DMX_DATA_DIRECTION);	// 0 = input, 1 = output
		dmx_port_direction = DMX_PORT_DIRECTION_INP;
		break;
	}

	if (enable_data) {
		dmx_start_data();
	}
}

/**
 * @ingroup dmx
 *
 * Configure PL011 for DMX512 transmission. Enable the UART.
 *
 */
static void pl011_init(void) {
	uint32_t ibrd = 12;													// Default UART CLOCK 48Mhz

	// Work around BROADCOM firmware bug
	if (bcm2835_vc_get_clock_rate(BCM2835_VC_CLOCK_ID_UART) != 48000000) {
		(void) bcm2835_vc_set_clock_rate(BCM2835_VC_CLOCK_ID_UART, 4000000);// Set UART clock rate to 4000000 (4MHz)
		ibrd = 1;
	}

	BCM2835_PL011->CR = 0;												// Disable everything

	dmb();

    // Set the GPI0 pins to the Alt 0 function to enable PL011 access on them
    bcm2835_gpio_fsel(RPI_V2_GPIO_P1_08, BCM2835_GPIO_FSEL_ALT0);		// PL011_TXD
    bcm2835_gpio_fsel(RPI_V2_GPIO_P1_10, BCM2835_GPIO_FSEL_ALT0);		// PL011_RXD

    // Disable pull-up/down
    bcm2835_gpio_set_pud(RPI_V2_GPIO_P1_08, BCM2835_GPIO_PUD_OFF);
    bcm2835_gpio_set_pud(RPI_V2_GPIO_P1_10, BCM2835_GPIO_PUD_OFF);

    dmb();

	while ((BCM2835_PL011->FR & PL011_FR_BUSY) != 0)
		;																// Poll the "flags register" to wait for the UART to stop transmitting or receiving

	BCM2835_PL011->LCRH &= ~PL011_LCRH_FEN;								// Flush the transmit FIFO by marking FIFOs as disabled in the "line control register"
	BCM2835_PL011->ICR = 0x7FF;											// Clear all interrupt status
	BCM2835_PL011->IBRD = ibrd;											//
	BCM2835_PL011->FBRD = 0;											//
	BCM2835_PL011->LCRH = PL011_LCRH_WLEN8 | PL011_LCRH_STP2;			// Set 8, N, 2, FIFO disabled
	BCM2835_PL011->CR = 0x301;											// Enable UART

	BCM2835_PL011->IMSC = PL011_IMSC_RXIM;
	BCM2835_IRQ->FIQ_CONTROL = (uint32_t) BCM2835_FIQ_ENABLE | (uint32_t) INTERRUPT_VC_UART;

	dmb();

	arm_install_handler((unsigned)fiq_dmx_in_handler, ARM_VECTOR(ARM_VECTOR_FIQ));
}

/**
 * @ingroup dmx
 *
 */
void dmx_init(void) {
	bcm2835_gpio_fsel(GPIO_DMX_DATA_DIRECTION, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_clr(GPIO_DMX_DATA_DIRECTION);	// 0 = input, 1 = output

#ifdef LOGIC_ANALYZER
	bcm2835_gpio_fsel(GPIO_ANALYZER_CH1, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_fsel(GPIO_ANALYZER_CH2, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_fsel(GPIO_ANALYZER_CH3, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_fsel(GPIO_ANALYZER_CH4, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_fsel(GPIO_ANALYZER_CH5, BCM2835_GPIO_FSEL_OUTP);

	bcm2835_gpio_clr(GPIO_ANALYZER_CH1);	// FIQ
	bcm2835_gpio_clr(GPIO_ANALYZER_CH2);	// BREAK
	bcm2835_gpio_clr(GPIO_ANALYZER_CH3);	// DMX DATA
	bcm2835_gpio_set(GPIO_ANALYZER_CH4);	// IDLE
	bcm2835_gpio_clr(GPIO_ANALYZER_CH5);	// IRQ
#endif

	dmx_clear_data();

	dmx_data_buffer_index_head = (uint16_t) 0;
	dmx_data_buffer_index_tail = (uint16_t) 0;

	rdm_data_buffer_index_head = (uint16_t) 0;
	rdm_data_buffer_index_tail = (uint16_t) 0;

	dmx_receive_state = IDLE;

	dmx_send_state = IDLE;
	dmx_send_always = false;

	irq_timer_init();

	pl011_init();
}

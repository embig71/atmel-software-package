/* ----------------------------------------------------------------------------
 *         SAM Software Package License
 * ----------------------------------------------------------------------------
 * Copyright (c) 2016, Atmel Corporation
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

/*----------------------------------------------------------------------------
 *        Headers
 *----------------------------------------------------------------------------*/

#ifdef CONFIG_HAVE_FLEXCOM
#include "peripherals/flexcom.h"
#endif

#include "peripherals/aic.h"
#include "peripherals/pmc.h"
#include "peripherals/twid.h"
#include "peripherals/twi.h"
#include "peripherals/xdmad.h"
#include "misc/cache.h"

#include "trace.h"
#include "io.h"
#include "timer.h"

#include <assert.h>
#include <string.h>

/*----------------------------------------------------------------------------
 *        Definition
 *----------------------------------------------------------------------------*/

#define TWID_DMA_THRESHOLD      16
#define TWID_TIMEOUT            100

#define MAX_ADESC               8
struct _async_desc asyncdesc[MAX_ADESC];
static uint8_t  adesc_index = 0;

/*----------------------------------------------------------------------------
 *        Internal functions
 *----------------------------------------------------------------------------*/
static uint8_t _check_nack(Twi* addr);
static uint8_t _check_rx_time_out(struct _twi_desc* desc);
static uint8_t _check_tx_time_out(struct _twi_desc* desc);
static void _twid_handler(void);


static uint32_t _twid_wait_twi_transfer(struct _twi_desc* desc)
{
	struct _timeout timeout;
	timer_start_timeout(&timeout, desc->timeout);
	while(!twi_is_transfer_complete(desc->addr)){
		if (timer_timeout_reached(&timeout)) {
			trace_error("twid: Unable to complete transfert!\r\n");
			twid_configure(desc);
			return TWID_ERROR_TRANSFER;
		}
	}
	return TWID_SUCCESS;
}

static void _twid_xdmad_callback_wrapper(struct _xdmad_channel* channel,
					   void* args)
{
	trace_debug("TWID DMA Transfert Finished\r\n");
	struct _twi_desc* twid = (struct _twi_desc*) args;
	xdmad_free_channel(channel);
	if (twid->region_start && twid->region_length) {
		cache_invalidate_region(twid->region_start, twid->region_length);
	}
	if (twid && twid->callback)
		twid->callback(twid, twid->cb_args);

}

static void _twid_init_dma_read_channel(const struct _twi_desc* desc,
					  struct _xdmad_channel** channel,
					  struct _xdmad_cfg* cfg)
{
	assert(cfg);
	assert(channel);

	uint32_t id = get_twi_id_from_addr(desc->addr);
	assert(id < ID_PERIPH_COUNT);
	memset(cfg, 0x0, sizeof(*cfg));
	*channel = xdmad_allocate_channel(id, XDMAD_PERIPH_MEMORY);
	assert(*channel);
	xdmad_prepare_channel(*channel);
	cfg->cfg.uint32_value = XDMAC_CC_TYPE_PER_TRAN
		| XDMAC_CC_DSYNC_PER2MEM
		| XDMAC_CC_MEMSET_NORMAL_MODE
		| XDMAC_CC_CSIZE_CHK_1
		| XDMAC_CC_DWIDTH_BYTE
		| XDMAC_CC_DIF_AHB_IF0
		| XDMAC_CC_SIF_AHB_IF1
		| XDMAC_CC_SAM_FIXED_AM;
	cfg->src_addr = (void*)&desc->addr->TWI_RHR;
}

static void _twid_dma_read(const struct _twi_desc* desc,
			   struct _buffer* buffer)
{
	struct _xdmad_channel* channel = NULL;
	struct _xdmad_cfg cfg;

	_twid_init_dma_read_channel(desc, &channel, &cfg);
	cfg.cfg.bitfield.dam = XDMAC_CC_DAM_INCREMENTED_AM
		>> XDMAC_CC_DAM_Pos;
	cfg.dest_addr = buffer->data;
	cfg.ublock_size = buffer->size;
	cfg.block_size = 0;
	xdmad_configure_transfer(channel, &cfg, 0, 0);
	xdmad_set_callback(channel, _twid_xdmad_callback_wrapper, (void*)desc);
	cache_clean_region(desc->region_start, desc->region_length);
	xdmad_start_transfer(channel);
}

static void _twid_init_dma_write_channel(struct _twi_desc* desc,
					 struct _xdmad_channel** channel,
					 struct _xdmad_cfg* cfg)
{
	assert(cfg);
	assert(channel);

	uint32_t id = get_twi_id_from_addr(desc->addr);
	assert(id < ID_PERIPH_COUNT);
	memset(cfg, 0x0, sizeof(*cfg));
	*channel = xdmad_allocate_channel(XDMAD_PERIPH_MEMORY, id);
	assert(*channel);
	xdmad_prepare_channel(*channel);
	cfg->cfg.uint32_value = XDMAC_CC_TYPE_PER_TRAN
		| XDMAC_CC_DSYNC_MEM2PER
		| XDMAC_CC_MEMSET_NORMAL_MODE
		| XDMAC_CC_CSIZE_CHK_1
		| XDMAC_CC_DWIDTH_BYTE
		| XDMAC_CC_DIF_AHB_IF1
		| XDMAC_CC_SIF_AHB_IF0
		| XDMAC_CC_DAM_FIXED_AM;
	cfg->dest_addr = (void*)&desc->addr->TWI_THR;
}

static void _twid_dma_write(struct _twi_desc* desc, struct _buffer* buffer)
{
	struct _xdmad_channel* channel = NULL;
	struct _xdmad_cfg cfg;

	_twid_init_dma_write_channel(desc, &channel, &cfg);
	cfg.cfg.bitfield.sam = XDMAC_CC_SAM_INCREMENTED_AM >> XDMAC_CC_SAM_Pos;
	cfg.src_addr = buffer->data;
	cfg.ublock_size = buffer->size;
	cfg.block_size = 0;
	xdmad_configure_transfer(channel, &cfg, 0, 0);
	xdmad_set_callback(channel, _twid_xdmad_callback_wrapper, (void*)desc);
	cache_clean_region(desc->region_start, desc->region_length);
	xdmad_start_transfer(channel);
}

/*
 *
 */
static uint8_t _check_nack(Twi* addr)
{
	if(twi_get_status(addr) & TWI_SR_NACK) {
		trace_error("twid: command NACK\r\n");
		return TWID_ERROR_ACK;
	}
	else return TWID_SUCCESS;
}

/*
 *
 */
static uint8_t _check_rx_time_out(struct _twi_desc* desc)
{
	uint8_t status = TWID_SUCCESS;
	struct _timeout timeout;

	timer_start_timeout(&timeout, desc->timeout);
	while(!twi_is_byte_received(desc->addr)) {
		if (timer_timeout_reached(&timeout)) {
			trace_error("twid: Device doesn't answer (RX TIMEOUT)\r\n");
			status = TWID_ERROR_TIMEOUT;
			break;
		}
	}
	return status;
}

/*
 *
 */
static uint8_t _check_tx_time_out(struct _twi_desc* desc)
{
	uint8_t status = TWID_SUCCESS;
	struct _timeout timeout;

	timer_start_timeout(&timeout, desc->timeout);
	while(!twi_is_byte_sent(desc->addr)) {
		if (timer_timeout_reached(&timeout)) {
			trace_error("twid: Device doesn't answer (TX TIMEOUT)\r\n");
			status = TWID_ERROR_TIMEOUT;
			break;
		}
	}
	return status;
}

/*
 *
 */
static void _twid_handler(void)
{
	uint8_t i;
	uint32_t status = 0;
	Twi* addr;
	uint32_t id = aic_get_current_interrupt_identifier();

	for (i=0; i!=MAX_ADESC; i++) {
		if(asyncdesc[i].twi_id == id) {
			status = 1;
			break;
		}
	}

	if(!status) {
		/* asynchrone descriptor not found, disable interrupt */
		addr = get_twi_addr_from_id(id);
		twi_disable_it(addr, TWI_IDR_RXRDY | TWI_IDR_TXRDY);
	}
	else {

		struct _async_desc* adesc = &asyncdesc[i];
		addr = adesc->twi_desc.addr;
		status = twi_get_masked_status(addr);

		if (TWI_STATUS_RXRDY(status)) {
			adesc->pdata[adesc->transferred] = twi_read_byte(addr);
			adesc->transferred ++;

			/* check for transfer finish */
			if (adesc->transferred == adesc->size) {
				twi_disable_it(addr, TWI_IDR_RXRDY);
				twi_enable_it(addr, TWI_IER_TXCOMP);
			}
			/* Last byte? */
			else if (adesc->transferred == (adesc->size - 1)) {
				twi_send_stop_condition(addr);
			}
		}
		else if (TWI_STATUS_TXRDY(status)) {

			/* Transfer finished ? */
			if (adesc->transferred == adesc->size) {
				twi_disable_it(addr, TWI_IDR_TXRDY);
				twi_enable_it(addr, TWI_IER_TXCOMP);
				twi_send_stop_condition(addr);
			}
			/* Bytes remaining */
			else {
				twi_write_byte(addr, adesc->pdata[adesc->transferred]);
				adesc->transferred++;
			}
		}
		/* Transfer complete*/
		else if (TWI_STATUS_TXCOMP(status)) {
			aic_disable(id);
			twi_disable_it(addr, TWI_IDR_RXRDY | TWI_IDR_RXRDY);
			twi_disable_it(addr, TWI_IDR_TXCOMP);
			if (adesc->twi_desc.callback)
				adesc->twi_desc.callback(&adesc->twi_desc, adesc->twi_desc.cb_args);
			adesc->pdata = 0;
			adesc->twi_id = 0;
		}
	}
}

/*----------------------------------------------------------------------------
 *        External functions
 *----------------------------------------------------------------------------*/

void twid_configure(struct _twi_desc* desc)
{
	uint32_t id = get_twi_id_from_addr(desc->addr);
	assert(id < ID_PERIPH_COUNT);

	if (desc->timeout == 0)
		desc->timeout = TWID_TIMEOUT;

#ifdef CONFIG_HAVE_FLEXCOM
	Flexcom* flexcom = get_flexcom_addr_from_id(get_twi_id_from_addr(desc->addr));
	if (flexcom) {
		flexcom_select(flexcom, FLEX_MR_OPMODE_TWI);
	}
#endif

	pmc_enable_peripheral(id);
	twi_configure_master(desc->addr, desc->freq);

#ifdef CONFIG_HAVE_TWI_FIFO
	if (desc->transfert_mode == TWID_MODE_FIFO) {
		uint32_t fifo_depth = get_peripheral_fifo_depth(desc->addr);
		twi_fifo_configure(desc->addr, fifo_depth/2, fifo_depth/2,
				   TWI_FMR_RXRDYM_ONE_DATA | TWI_FMR_TXRDYM_ONE_DATA);
	}
#endif
}

/*
 *
 */

#ifdef CONFIG_SOC_SAMA5D4

static uint32_t _twid_poll_read(struct _twi_desc* desc, struct _buffer* buffer)
{
	int i = 0;
	Twi* addr = desc->addr;

	/* Enable read interrupt and start the transfer */
	twi_start_read(addr, desc->slave_addr, desc->iaddr, desc->isize);

	if( buffer->size != 1)
	{
		/* get buffer_size-1 data */
		for (i = 0; i < buffer->size-1; ++i) {
			if( _check_rx_time_out(desc) != TWID_SUCCESS )
				break;
			buffer->data[i] = twi_read_byte(addr);
			if( _check_nack(addr) != TWID_SUCCESS )
			   return TWID_ERROR_ACK ;
		}
	}
	/* Befor receive last data, send stop */
	twi_send_stop_condition(addr);

	if( _check_nack(addr) != TWID_SUCCESS )
		return TWID_ERROR_ACK ;
	if( _check_rx_time_out(desc) != TWID_SUCCESS )
		return TWID_ERROR_TIMEOUT;
	buffer->data[i] = twi_read_byte(addr);
	/* wait transfert to be finished */
	return _twid_wait_twi_transfer(desc);
}

static uint32_t _twid_poll_write(struct _twi_desc* desc, struct _buffer* buffer)
{
	int i = 0;
	Twi* addr = desc->addr;

	/* Start twi with send first byte */
	twi_start_write(addr, desc->slave_addr, desc->iaddr, desc->isize, buffer->data[0]);

	/* If only one byte send stop immediatly */
	if(buffer->size == 1) {
		twi_send_stop_condition(addr);
	}
	if( _check_nack(addr) != TWID_SUCCESS )
		return TWID_ERROR_ACK;

	for (i = 1; i < buffer->size; ++i) {
		if( _check_tx_time_out(desc) != TWID_SUCCESS )
			break;
		twi_write_byte(addr, buffer->data[i]);
		if( _check_nack(addr) != TWID_SUCCESS )
			return TWID_ERROR_ACK ;
	}
	/* Finally send stop if more than 1 byte to send */
	if(buffer->size != 1)
		twi_send_stop_condition(addr);

	/* wait transfert to be finished */
	return _twid_wait_twi_transfer(desc);
}

#else /* not a CONFIG_SOC_SAMA5D4 */

static uint32_t _twid_poll_read(struct _twi_desc* desc, struct _buffer* buffer)
{
	int i = 0;
	Twi* addr = desc->addr;

	#ifdef CONFIG_HAVE_TWI_ALTERNATE_CMD
	twi_init_read_transfert(desc->addr, desc->slave_addr, desc->iaddr,
							desc->isize, buffer->size);
	#endif

	if( _check_nack(addr) != TWID_SUCCESS )
		return TWID_ERROR_ACK ;

	for (i = 0; i < buffer->size; ++i) {
		if (_check_rx_time_out(desc) != TWID_SUCCESS)
				break;
		buffer->data[i] = twi_read_byte(desc->addr);
		if (_check_nack(addr) != TWID_SUCCESS)
			return TWID_ERROR_ACK ;
	}

	/* wait transfert to be finished */
	return _twid_wait_twi_transfer(desc);
}

static uint32_t _twid_poll_write(struct _twi_desc* desc, struct _buffer* buffer)
{
	int i = 0;
	Twi* addr = desc->addr;

	#ifdef CONFIG_HAVE_TWI_ALTERNATE_CMD
	twi_init_write_transfert(desc->addr, desc->slave_addr, desc->iaddr,
							 desc->isize, buffer->size);
	#endif

	if (_check_nack(addr) != TWID_SUCCESS)
		return TWID_ERROR_ACK ;

	for (i = 0; i < buffer->size; ++i) {
		if (_check_tx_time_out(desc) != TWID_SUCCESS)
			break;
		twi_write_byte(desc->addr, buffer->data[i]);
		if (_check_nack(addr) != TWID_SUCCESS)
			return TWID_ERROR_ACK ;
	}
	/* wait transfert to be finished */
	return _twid_wait_twi_transfer(desc);
}

#endif

/*
 *
 */
uint32_t twid_transfert(struct _twi_desc* desc, struct _buffer* rx, struct _buffer* tx, twid_callback_t cb, void* user_args)
{
	uint32_t status = TWID_SUCCESS;
	uint32_t id;
	uint8_t tmode;

	desc->callback = cb;
	desc->cb_args = user_args;

	if (mutex_try_lock(&desc->mutex)) {
		return TWID_ERROR_LOCK;
	}

	tmode = desc->transfert_mode;
	/* Check if only one car to send or receive */
	if( tmode == TWID_MODE_ASYNC ) {
		if( (tx->size == 1) || (rx->size == 1) )
		tmode = TWID_MODE_POLLING;
	}

	switch (tmode) {

	case TWID_MODE_ASYNC:

		/* Copy descriptor to async descriptor */
		asyncdesc[adesc_index].twi_desc = *desc;
		/* Init param used by interrupt handler*/
		asyncdesc[adesc_index].pdata = NULL;
		id = get_twi_id_from_addr(desc->addr);
		asyncdesc[adesc_index].twi_id = id;

		/* Set handler twi */
		aic_set_source_vector(id, _twid_handler);
		/* Enable interrupt twi */
		aic_enable(id);

		if (tx != NULL) {
			/* Set buffer data info to async descriptor */
			/* pnt+1 and size-1 because first data sent directly */
			asyncdesc[adesc_index].pdata = tx->data+1;
			asyncdesc[adesc_index].size = tx->size-1;
			/* Start twi with send first byte */
			twi_start_write(desc->addr, desc->slave_addr, desc->iaddr, desc->isize, tx->data[0]);
			twi_enable_it(desc->addr, TWI_IER_TXRDY);
		}
		else if (rx != NULL) {
			/* Set buffer data info to async descriptor */
			asyncdesc[adesc_index].pdata = rx->data;
			asyncdesc[adesc_index].size = rx->size;
			/* Enable read interrupt and start the transfer */
			twi_enable_it(desc->addr, TWI_IER_RXRDY);
			twi_start_read(desc->addr, desc->slave_addr, desc->iaddr, desc->isize);
		}
		else ;

		//while (asyncdesc[adesc_index].status == TWID_TRANSFER_IN_PROGRESS);

		adesc_index = (adesc_index+1)% MAX_ADESC;
		break;


	case TWID_MODE_POLLING:
		if (tx != NULL) {
			twi_enable_it(desc->addr, TWI_IER_TXRDY);
			status = _twid_poll_write(desc, tx);
			if (status != TWID_SUCCESS) break;
		}
		else if (rx != NULL) {
			twi_enable_it(desc->addr, TWI_IER_RXRDY);
			status = _twid_poll_read(desc, rx);
			if (status!= TWID_SUCCESS) break;
		}
		else ;
		if (cb) cb(desc, user_args);
		mutex_free(&desc->mutex);
		break;

	case TWID_MODE_DMA:
		if (!(rx != NULL || tx != NULL)) {
			status = TWID_ERROR_DUPLEX;
			break;
		}
		if (tx != NULL) {
			if (tx->size < TWID_DMA_THRESHOLD) {
				status = _twid_poll_write(desc, tx);
				if (status) break;
				if (cb) cb(desc, user_args);
				mutex_free(&desc->mutex);
			} else {

				#ifdef CONFIG_HAVE_TWI_ALTERNATE_CMD
				twi_init_write_transfert(desc->addr,
							 desc->slave_addr,
							 desc->iaddr,
							 desc->isize,
							 tx->size);
				#else

				#endif

				desc->region_start = tx->data;
				desc->region_length = tx->size;
				_twid_dma_write(desc, tx);
			}
		}
		else if (rx) {
			if (rx->size < TWID_DMA_THRESHOLD) {
				status = _twid_poll_read(desc, rx);
				if (status) break;
				if (cb) cb(desc, user_args);
				mutex_free(&desc->mutex);
			} else {

				#ifdef CONFIG_HAVE_TWI_ALTERNATE_CMD
				twi_init_read_transfert(desc->addr,
							desc->slave_addr,
							desc->iaddr,
							desc->isize,
							rx->size);
				#else

				#endif

				desc->region_start = rx->data;
				desc->region_length = rx->size;
				if(twi_get_status(desc->addr) & TWI_SR_NACK) {
					trace_error("twid: Acknolegment " "Error\r\n");
					status = TWID_ERROR_ACK;
					break;
				}
				_twid_dma_read(desc, rx);
			}
		}
		else ;
		break;

#ifdef CONFIG_HAVE_TWI_FIFO
	case TWID_MODE_FIFO:
		if (tx != NULL) {
			status = twi_write_stream(desc->addr, desc->slave_addr,
						  desc->iaddr, desc->isize,
						  tx->data, tx->size, desc->timeout);
			status = status ? TWID_SUCCESS : TWID_ERROR_ACK;
			if (status)
				break;
			status = _twid_wait_twi_transfer(desc);
			if (status)
				break;
		}
		if (rx != NULL) {
			status = twi_read_stream(desc->addr, desc->slave_addr,
						 desc->iaddr, desc->isize,
						 rx->data, rx->size, desc->timeout);
			status = status ? TWID_SUCCESS : TWID_ERROR_ACK;
			if (status)
				break;
			status = _twid_wait_twi_transfer(desc);
			if (status)
				break;
		}
		if (cb != NULL) cb(desc, user_args);
		mutex_free(&desc->mutex);
		break;
#endif

	default:
		trace_debug("Unknown mode");
	}

	if (status)
		mutex_free(&desc->mutex);

	return status;
}

void twid_finish_transfert_callback(struct _twi_desc* desc, void* user_args)
{
	(void)user_args;
	twid_finish_transfert(desc);
}

void twid_finish_transfert(struct _twi_desc* desc)
{
	mutex_free(&desc->mutex);
}

uint32_t twid_is_busy(const struct _twi_desc* desc)
{
	return mutex_is_locked(&desc->mutex);
}

void twid_wait_transfert(const struct _twi_desc* desc)
{
	while (mutex_is_locked(&desc->mutex));
}

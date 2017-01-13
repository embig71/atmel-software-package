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

/**
 * \file
 *
 * Interface for memories configuration on board.
 *
 */

#ifndef BOARD_SUPPORT_H
#define BOARD_SUPPORT_H

/*----------------------------------------------------------------------------
 *        Headers
 *----------------------------------------------------------------------------*/

#include <stdint.h>

/*----------------------------------------------------------------------------
 *        Functions
 *----------------------------------------------------------------------------*/

/* \brief Return a string containing the board name
 * \return the board name
 */
extern const char* get_board_name(void);

/**
 * \brief Configure the board master clock using PMC
 */
extern void board_cfg_clocks(void);

/**
 * \brief Performs the low-level initialization of the chip.
 *
 * This includes watchdog, master clock, NVIC, DDRAM and MPU.
 */
extern void board_cfg_lowlevel(bool clocks, bool ddram, bool mpu);

/**
 * \brief Configure the MATRIX for DDR
 */
extern void board_cfg_matrix_for_ddr(void);

/**
 * \brief Configures DDR (calls board_cfg_matrix_for_ddr)
 */
extern void board_cfg_ddram(void);

/**
 * \brief Configure the board console if any
 * \param baudrate Requested baudrate, or board default if 0
 */
extern void board_cfg_console(uint32_t baudrate);

/**
 * \brief Setup MPU for the board
 */
extern void board_cfg_mpu(void);

/**
 * \brief Configures the board.
 */
extern void board_init(void);

#endif  /* BOARD_SUPPORT_H */

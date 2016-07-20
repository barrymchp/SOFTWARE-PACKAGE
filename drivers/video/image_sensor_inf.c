/* ----------------------------------------------------------------------------
 *         SAM Software Package License
 * ----------------------------------------------------------------------------
 * Copyright (c) 2013, Atmel Corporation
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
#include "chip.h"

#include "video/image_sensor_inf.h"
#include "peripherals/twi.h"
#include "peripherals/twid.h"

#include <stdbool.h>
#include <stdio.h>
 
#include "timer.h"
#include "trace.h"
/*----------------------------------------------------------------------------
 *        Local variable
 *----------------------------------------------------------------------------*/

static const sensor_profile_t *sensor;

/*----------------------------------------------------------------------------
 *        Local functions
 *----------------------------------------------------------------------------*/
/**
 * \brief Read value from a register in an dedicated sensor device.
 * \param twid TWI interface
 * \param reg Register to be read
 * \param data Data read
 * \return SENSOR_OK if no error; otherwise SENSOR_TWI_ERROR
 */
static sensor_status_t sensor_twi_read_reg(struct _twi_desc* twid, uint16_t reg, uint8_t *data)
{
	uint8_t status;
	uint8_t reg8[2];
	struct _buffer buf[2] = {
		{ .attr = TWID_BUF_ATTR_START | TWID_BUF_ATTR_WRITE | TWID_BUF_ATTR_STOP },
		{ .attr = TWID_BUF_ATTR_START | TWID_BUF_ATTR_READ | TWID_BUF_ATTR_STOP },
	};

	twid->slave_addr = sensor->twi_slave_addr;
	twid->iaddr = 0;
	twid->isize = 0;

	reg8[0] = reg >> 8;
	reg8[1] = reg & 0xff;
	switch (sensor->twi_inf_mode){
	case SENSOR_TWI_REG_BYTE_DATA_BYTE:
		buf[0].data = reg8 + 1;
		buf[0].size = 1;
		buf[1].data = data;
		buf[1].size = 1;
		break;

	case SENSOR_TWI_REG_2BYTE_DATA_BYTE:
		buf[0].data = reg8;
		buf[0].size = 2;
		buf[1].data = data;
		buf[1].size = 1;
		break;

	case SENSOR_TWI_REG_BYTE_DATA_2BYTE:
		buf[0].data = reg8 + 1;
		buf[0].size = 1;
		buf[1].data = data;
		buf[1].size = 2;
		break;
	default:
		return SENSOR_TWI_ERROR;
	}

	status = twid_transfer(twid, &buf[0], NULL, NULL);
	while (twid_is_busy(twid));

	if (status)
		return SENSOR_TWI_ERROR;

	status = twid_transfer(twid, &buf[1], NULL, NULL);
	while (twid_is_busy(twid));

	if (status)
		return SENSOR_TWI_ERROR;

	return SENSOR_OK;
}

/**
 * \brief  Write a value to a register in an dedicated sensor device.
 * \param twid TWI interface
 * \param reg Register to be written
 * \param data Data written
 * \return SENSOR_OK if no error; otherwise SENSOR_TWI_ERROR
 */
static sensor_status_t sensor_twi_write_reg(struct _twi_desc* twid, uint16_t reg, uint8_t *data)
{
	uint8_t status;
	struct _buffer out = {
		.data = data,
		.attr = TWID_BUF_ATTR_START | TWID_BUF_ATTR_STOP | TWID_BUF_ATTR_WRITE,
	};

	twid->slave_addr = sensor->twi_slave_addr;
	twid->iaddr = reg;

	switch (sensor->twi_inf_mode){
	case SENSOR_TWI_REG_BYTE_DATA_BYTE:
		out.size = 1;
		twid->isize = 1;
		break;

	case SENSOR_TWI_REG_2BYTE_DATA_BYTE:
		out.size = 1;
		twid->isize = 2;
		break;

	case SENSOR_TWI_REG_BYTE_DATA_2BYTE:
		out.size = 2;
		twid->isize = 1;
		break;

	default:
		return SENSOR_TWI_ERROR;
	}

	status = twid_transfer(twid, &out, NULL, NULL);
	while (twid_is_busy(twid));

	if (status)
		return SENSOR_TWI_ERROR;

	return SENSOR_OK;
}

/**
 * \brief Read and check sensor product ID.
 * \param twid TWI interface
 * \param reg_h Register address for product ID high byte.
 * \param reg_l Register address for product ID low byte.
 * \param pid Product ID to be compared.
 * \param ver_mask version mask.
 * \return SENSOR_OK if no error; otherwise SENSOR_TWI_ERROR
 */
static sensor_status_t sensor_check_pid(struct _twi_desc *twid,
						uint16_t reg_h,
						uint16_t reg_l,
						uint16_t pid,
						uint16_t ver_mask)
{
	/* use uint32_t to force 4-byte alignment */
	uint32_t pid_high = 0;
	uint32_t pid_low = 0;

	if (sensor_twi_read_reg(twid, reg_h, (uint8_t*)&pid_high) != SENSOR_OK)
		return SENSOR_TWI_ERROR;
	pid_high &= 0xff;

	if (sensor_twi_read_reg(twid, reg_l, (uint8_t*)&pid_low) != SENSOR_OK)
		return SENSOR_TWI_ERROR;
	pid_low &= 0xff;
	
	trace_debug_wp("SENSOR PID = <%x, %x>\n\r",
	               (unsigned)pid_high, (unsigned)pid_low);
	
	if ((pid & ver_mask) == (((pid_high << 8) | pid_low) & ver_mask))
		return SENSOR_OK;
	else
		return SENSOR_ID_ERROR;
}

/*----------------------------------------------------------------------------
 *        Exported functions
 *----------------------------------------------------------------------------*/

/**
 * \brief  Initialize a list of registers.
 * The list of registers is terminated by the pair of values
 * \param twid TWI interface
 * \param reglist Register list to be written
 * \return SENSOR_OK if no error; otherwise SENSOR_TWI_ERROR
 */
sensor_status_t sensor_twi_write_regs(struct _twi_desc *twid, const sensor_reg_t *reglist)
{
	sensor_status_t status;
	const sensor_reg_t *next = reglist;

	while (!((next->reg == SENSOR_REG_TERM) && (next->val == SENSOR_VAL_TERM))) {
		status = sensor_twi_write_reg( twid, next->reg, (uint8_t *)(&next->val));
		timer_wait(10);
		if (status)
			return SENSOR_TWI_ERROR;
		next++;
	}
	return SENSOR_OK;
}

/**
 * \brief  read list of registers.
 * The list of registers is terminated by the pair of values
 * \param twid TWI interface
 * \param reglist Register list to be read
 * \return SENSOR_OK if no error; otherwise SENSOR_TWI_ERROR
 */
sensor_status_t sensor_twi_read_regs(struct _twi_desc *twid, const sensor_reg_t *reglist)
{
	sensor_status_t status;
	const sensor_reg_t *next = reglist;
	uint8_t val;
	while (!((next->reg == SENSOR_REG_TERM) && (next->val == SENSOR_VAL_TERM))) {
		status = sensor_twi_read_reg( twid, next->reg, (uint8_t *)&val);
		timer_wait(10);
		if (status)
			return SENSOR_TWI_ERROR;
		next++;
	}
	return SENSOR_OK;
}

/**
 * \brief Load and configure sensor setting with giving profile.
 * \param sensor_profile pointer to a profile instance.
 * \param resolution resolution request
 * \return SENSOR_OK if no error; otherwise return SENSOR_XXX_ERROR
 */
sensor_status_t sensor_setup(struct _twi_desc* twid,
				const sensor_profile_t *sensor_profile,
				sensor_output_resolution_t resolution,
				sensor_output_format_t format)
{
	uint8_t i;
	uint8_t found = 0;
	sensor_status_t status = SENSOR_OK;
	for (i = 0; i < SENSOR_SUPPORTED_OUTPUTS; i++) {
		if (sensor_profile->output_conf[i]->supported){
			if (sensor_profile->output_conf[i]->output_resolution == resolution) {
				if (sensor_profile->output_conf[i]->output_format == format) {
					found = 1;
					break;
				}
			}
		}
	}
	if (found == 0)
		return SENSOR_RESOLUTION_NOT_SUPPORTED;
	sensor = sensor_profile;

	status = sensor_check_pid(twid, sensor->pid_high_reg, sensor->pid_low_reg,
	                          (sensor->pid_high) << 8 | sensor->pid_low, sensor->version_mask);
	if (status != SENSOR_OK)
		return SENSOR_ID_ERROR;
	else 
		return sensor_twi_write_regs(twid, sensor->output_conf[i]->output_setting);
}


/**
 * \brief Retrieves sensor output bit width and size for giving resolution and format.
 * \param resolution Output resolution request.
 * \param format Output Format request.
 * \param bits   Output Format request.
 * \param width  pointer to image width to be read.
 * \param height  pointer to image height to be read.
 * \return SENSOR_OK if no error; otherwise return SENSOR_XXX_ERROR
 */
sensor_status_t sensor_get_output(sensor_output_resolution_t resolution,
                                  sensor_output_format_t format, sensor_output_bit_t *bits,
                                  uint32_t *width, uint32_t *height)
{
	uint8_t i;
	for (i = 0; i < SENSOR_SUPPORTED_OUTPUTS; i++) {
		if (sensor->output_conf[i]->supported){
			if (sensor->output_conf[i]->output_resolution == resolution) {
				if (sensor->output_conf[i]->output_format == format) {
					*bits = sensor->output_conf[i]->output_bit;
					*width = sensor->output_conf[i]->output_width;
					*height = sensor->output_conf[i]->output_height;
					return SENSOR_OK;
				}
			}
		}
	}
	return SENSOR_RESOLUTION_NOT_SUPPORTED;
}

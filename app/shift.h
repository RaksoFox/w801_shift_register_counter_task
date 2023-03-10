#ifndef SHIFT_H
#define SHIFT_H

#include "wm_type_def.h"

#include "wm_gpio.h"

#define LSBFIRST 1
#define MSBFIRST 0

/**
 * @brief          This function is used to shift bytes to external output.
 *
 * @param[in]      data_pin     name of data GPIO pin
 * @param[in]      clock_pin    name of clock GPIO pin
 * @param[in]      bit_order    bits order LSBFIRST or MSBFIRST
 * @param[in]      val          bits to send
 */
void shift_out(enum tls_io_name data_pin, enum tls_io_name clock_pin, uint8_t bit_order, uint8_t val);

#endif
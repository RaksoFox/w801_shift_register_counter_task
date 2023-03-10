#include <stdio.h>
#include <stdlib.h>
#include "shift.h"

void shift_out(enum tls_io_name data_pin, enum tls_io_name clock_pin, uint8_t bit_order, uint8_t val)
{
    for (u8 i = 0; i < 8; i++)
    {
        if (bit_order == LSBFIRST)
        {
            tls_gpio_write(data_pin, val & 0x01);
            val >>= 1;
        }
        else
        {
            tls_gpio_write(data_pin, (val & 0x80) != 0);
            val <<= 1;
        }
        tls_gpio_write(clock_pin, 1);
        tls_gpio_write(clock_pin, 0);
    }
}
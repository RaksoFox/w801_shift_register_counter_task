#include <stdio.h>
#include <stdlib.h>
#include "wm_gpio.h"
#include "wm_osal.h"

#include "wm_type_def.h"

#include "shift.h"

#define SHIFT_TASK_SIZE 2048
static OS_STK ShiftTaskStk[SHIFT_TASK_SIZE];
#define SHIFT_TASK_PRIO 32

#define PIN_LATCH WM_IO_PB_22
#define PIN_CLOCK WM_IO_PB_21
#define PIN_DATA WM_IO_PB_23

void shift_task(void *data)
{
	printf("Pin configuration\n");

	tls_gpio_cfg(PIN_LATCH, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
	tls_gpio_cfg(PIN_CLOCK, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
	tls_gpio_cfg(PIN_DATA, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);

	for (;;)
	{
		printf("Start counting\n");
		for (int nr = 0; nr < 256; nr++)
		{
			tls_gpio_write(PIN_LATCH, 1);
			shift_out(PIN_DATA, PIN_CLOCK, LSBFIRST, nr);
			tls_gpio_write(PIN_LATCH, 0);
			tls_os_time_delay(HZ / 2);
		}
	}
}

void UserMain(void)
{
	tls_os_task_create(NULL, NULL, shift_task, NULL, (void *)ShiftTaskStk, SHIFT_TASK_SIZE * sizeof(u32), SHIFT_TASK_PRIO, 0);
}

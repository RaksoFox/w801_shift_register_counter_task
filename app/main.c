#include <stdio.h>
#include <stdlib.h>
#include "wm_gpio.h"
#include "wm_osal.h"

#include "wm_type_def.h"

#include "shift.h"

#define SHIFT_TASK_SIZE 2048
static OS_STK ShiftTaskStk[SHIFT_TASK_SIZE];
#define SHIFT_TASK_PRIO 32

void shift_task(void *data)
{
	printf("Pin configuration\n");

	tls_gpio_cfg(WM_IO_PB_18, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
	tls_gpio_cfg(WM_IO_PB_25, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
	tls_gpio_cfg(WM_IO_PB_26, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);

	for (;;)
	{
		printf("Start counting");
		for (int nr = 0; nr < 256; nr++)
		{
			tls_gpio_write(WM_IO_PB_26, 1);
			shift_out(WM_IO_PB_18, WM_IO_PB_25, LSBFIRST, nr);
			tls_gpio_write(WM_IO_PB_26, 0);
			tls_os_time_delay(HZ / 2);
		}
	}
}

void UserMain(void)
{
	tls_os_task_create(NULL, NULL, shift_task, NULL, (void *)ShiftTaskStk, SHIFT_TASK_SIZE * sizeof(u32), SHIFT_TASK_PRIO, 0);
}

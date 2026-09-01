/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    MCXA176.c
 * @brief   Application entry point.
 */
#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "cmsis_os2.h"
#include "BSP_PERI_INPUTMUX.h"
#include "BSP_ADC1.h"
#include "BSP_GPIO1.h"
#include "../Sys/include/hal_timebase.h"
/* TODO: insert other include files here. */

static void AppMainThread(void *argument)
{
    (void)argument;

    for (;;)
    {
        PRINTF("Hello World\r\n");
        (void)osDelay(1000U);
    }
}

/*
 * @brief   Application entry point.
 */
int main(void) {

    /* Init board hardware. */
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    /* Route CTIMER4 Match 0 to ADC1 hardware Trigger 0 before timers start. */
    PERI_INPUTMUX_Init();
    BOARD_InitBootPeripherals();
    SYS_HAL_TimebaseInit();
    BSP_ADC1_Init();
    BSP_GPIO1_Init();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    /* Init FSL debug console. */
    BOARD_InitDebugConsole();
#endif

    (void)osKernelInitialize();
    if (osThreadNew(AppMainThread, NULL, NULL) == NULL)
    {
        for (;;)
        {
        }
    }
    (void)osKernelStart();

    for (;;)
    {
    }
    return 0 ;
}

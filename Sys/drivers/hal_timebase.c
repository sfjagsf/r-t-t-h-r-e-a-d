#include "../include/hal_timebase.h"

#include "fsl_ctimer.h"
#include "cmsis_os2.h"

static volatile uint32_t s_halTickMs;

static void SYS_HAL_TimebaseMatch0Callback(uint32_t flags)
{
    if ((flags & (uint32_t)kCTIMER_Match0Flag) != 0U)
    {
        HAL_IncTick();
    }
}

void SYS_HAL_TimebaseInit(void)
{
    static ctimer_callback_t callbacks[8] = {SYS_HAL_TimebaseMatch0Callback};

    CTIMER_RegisterCallBack(CTIMER0, callbacks, kCTIMER_MultipleCallback);
}

void HAL_IncTick(void)
{
    s_halTickMs++;
}

uint32_t HAL_GetTick(void)
{
    return s_halTickMs;
}

void HAL_Delay(uint32_t delayMs)
{
    uint32_t start;

    if (delayMs == 0U)
    {
        return;
    }

    if ((__get_IPSR() == 0U) && (osKernelGetState() == osKernelRunning))
    {
        uint32_t delayTicks = (uint32_t)(((uint64_t)delayMs * osKernelGetTickFreq() + 999U) / 1000U);

        (void)osDelay(delayTicks);
        return;
    }

    /* HAL_Delay must not be called from an ISR. */
    if (__get_IPSR() != 0U)
    {
        return;
    }

    start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < delayMs)
    {
    }
}

void HAL_SuspendTick(void)
{
    CTIMER_DisableInterrupts(CTIMER0, (uint32_t)kCTIMER_Match0InterruptEnable);
}

void HAL_ResumeTick(void)
{
    CTIMER_EnableInterrupts(CTIMER0, (uint32_t)kCTIMER_Match0InterruptEnable);
}

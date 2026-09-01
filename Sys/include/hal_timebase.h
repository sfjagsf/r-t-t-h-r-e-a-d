#ifndef SYS_HAL_TIMEBASE_H_
#define SYS_HAL_TIMEBASE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void SYS_HAL_TimebaseInit(void);

/* STM32 HAL-compatible timebase functions backed by CTIMER0 Match0. */
void HAL_IncTick(void);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t delayMs);
void HAL_SuspendTick(void);
void HAL_ResumeTick(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_HAL_TIMEBASE_H_ */

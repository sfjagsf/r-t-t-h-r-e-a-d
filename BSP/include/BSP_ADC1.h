/* Application-facing LPADC1 interface. */
#ifndef BSP_ADC1_H_
#define BSP_ADC1_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_ADC1_CHANNEL_COUNT (6U)

/* Raw LPADC FIFO words, in STM32-compatible scan order. */
extern volatile uint32_t g_adc1DmaBuffer[BSP_ADC1_CHANNEL_COUNT];

/* Configure the application trigger after generated peripheral init. */
void BSP_ADC1_Init(void);

/* Return true when a complete six-channel scan is available. */
bool BSP_ADC1_DataReady(void);

/* Copy the latest scan and clear its ready indication. */
bool BSP_ADC1_Read(uint16_t values[BSP_ADC1_CHANNEL_COUNT]);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ADC1_H_ */

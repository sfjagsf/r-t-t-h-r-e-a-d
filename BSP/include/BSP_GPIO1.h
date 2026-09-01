/* GPIO1 DMA-sampled input interface. */
#ifndef BSP_GPIO1_H_
#define BSP_GPIO1_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_GPIO1_SAMPLE_COUNT       (8U)
#define BSP_GPIO1_HIGH_THRESHOLD     (5U)
#define BSP_GPIO1_INPUT_MASK         (0xFFU)

/* Each bit maps directly to the matching GPIO1 pin: bit 0 is P1_0, ..., bit 7 is P1_7. */
#define BSP_GPIO1_PIN_0_MASK         (1UL << 0U)
#define BSP_GPIO1_PIN_1_MASK         (1UL << 1U)
#define BSP_GPIO1_PIN_2_MASK         (1UL << 2U)
#define BSP_GPIO1_PIN_3_MASK         (1UL << 3U)
#define BSP_GPIO1_PIN_4_MASK         (1UL << 4U)
#define BSP_GPIO1_PIN_5_MASK         (1UL << 5U)
#define BSP_GPIO1_PIN_6_MASK         (1UL << 6U)
#define BSP_GPIO1_PIN_7_MASK         (1UL << 7U)

/* DMA destination used by the generated DMA0 CH5 configuration. */
extern volatile uint32_t g_gpio1SampleBuf[BSP_GPIO1_SAMPLE_COUNT];

/* Enable the generated DMA CH5 interrupt after BOARD_InitBootPeripherals(). */
void BSP_GPIO1_Init(void);

/* True after a new 8-sample filtered result is available. */
bool BSP_GPIO1_DataReady(void);

/*
 * Copy the latest filtered GPIO1[7:0] levels and clear its new-data indication.
 * A high result bit means at least 5 of the 8 samples were high.
 */
bool BSP_GPIO1_Read(uint32_t *levels);

/* Read one filtered GPIO1 pin without consuming the new-data indication. */
bool BSP_GPIO1_GetLevel(uint8_t pin, bool *level);

#ifdef __cplusplus
}
#endif

#endif /* BSP_GPIO1_H_ */

/* GPIO1 eight-sample DMA majority filter. */
#include "BSP_GPIO1.h"

#include "fsl_common.h"
#include "peripherals.h"

AT_NONCACHEABLE_SECTION_ALIGN(volatile uint32_t g_gpio1SampleBuf[BSP_GPIO1_SAMPLE_COUNT], 32);

static volatile uint32_t s_gpio1FilteredLevels;
static volatile bool s_gpio1DataReady;

void BSP_GPIO1_Init(void)
{
    s_gpio1FilteredLevels = 0U;
    s_gpio1DataReady      = false;

    /* The generated DMA configuration installs the callback and priority only. */
    NVIC_ClearPendingIRQ(DMA0_DMA_CH_INT_DONE_5_IRQN);
    NVIC_SetPriority(DMA0_DMA_CH_INT_DONE_5_IRQN, DMA0_DMA_CH_INT_DONE_5_IRQ_PRIORITY);
    EnableIRQ(DMA0_DMA_CH_INT_DONE_5_IRQN);
}

bool BSP_GPIO1_DataReady(void)
{
    return s_gpio1DataReady;
}

bool BSP_GPIO1_Read(uint32_t *levels)
{
    uint32_t primask;

    if (levels == NULL)
    {
        return false;
    }

    primask = DisableGlobalIRQ();
    if (!s_gpio1DataReady)
    {
        EnableGlobalIRQ(primask);
        return false;
    }

    *levels            = s_gpio1FilteredLevels;
    s_gpio1DataReady   = false;
    EnableGlobalIRQ(primask);

    return true;
}

bool BSP_GPIO1_GetLevel(uint8_t pin, bool *level)
{
    uint32_t primask;
    uint32_t filteredLevels;

    if ((level == NULL) || (pin >= 8U))
    {
        return false;
    }

    primask = DisableGlobalIRQ();
    filteredLevels = s_gpio1FilteredLevels;
    EnableGlobalIRQ(primask);

    *level = ((filteredLevels & (1UL << pin)) != 0U);
    return true;
}

/* Strong definition overrides the weak startup handler. */
void DMA_CH5_IRQHandler(void)
{
    EDMA_HandleIRQ(&DMA0_CH1_Handle);
}

/* Registered by the generated DMA0 CH5 initialization. */
void GPIO1_SampleDmaCallback(edma_handle_t *handle, void *userData, bool transferDone, uint32_t tcds)
{
    uint32_t pin;
    uint32_t sample;
    uint32_t highCount;
    uint32_t filteredLevels = 0U;

    (void)handle;
    (void)userData;
    (void)tcds;

    if (!transferDone)
    {
        return;
    }

    for (pin = 0U; pin < 8U; ++pin)
    {
        highCount = 0U;
        for (sample = 0U; sample < BSP_GPIO1_SAMPLE_COUNT; ++sample)
        {
            if ((g_gpio1SampleBuf[sample] & (1UL << pin)) != 0U)
            {
                ++highCount;
            }
        }

        if (highCount >= BSP_GPIO1_HIGH_THRESHOLD)
        {
            filteredLevels |= (1UL << pin);
        }
    }

    s_gpio1FilteredLevels = filteredLevels & BSP_GPIO1_INPUT_MASK;
    s_gpio1DataReady      = true;
}

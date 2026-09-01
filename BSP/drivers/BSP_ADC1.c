/* Application ADC1/DMA layer for MCXA176. */
#include "BSP_ADC1.h"

#include <stddef.h>

#include "peripherals.h"
#include "fsl_lpadc.h"

/* Keep DMA data out of a cacheable region. */
AT_NONCACHEABLE_SECTION_ALIGN(volatile uint32_t g_adc1DmaBuffer[BSP_ADC1_CHANNEL_COUNT], 32);

static volatile uint16_t s_adc1Latest[BSP_ADC1_CHANNEL_COUNT];
static volatile bool s_adc1DataReady;

void BSP_ADC1_Init(void)
{
    const lpadc_conv_trigger_config_t trigger0 = {
        .targetCommandId = ADC1_ADC1_A0,
        .delayPower = 0U,
        .priority = 1U,
        .enableHardwareTrigger = true,
    };

    /* CTIMER4 MAT0 is routed to ADC1 Trigger 0 by INPUTMUX. */
    LPADC_SetConvTriggerConfig(ADC1_PERIPHERAL, 0U, &trigger0);
    NVIC_SetPriority(DMA0_DMA_CH_INT_DONE_2_IRQN, DMA0_DMA_CH_INT_DONE_2_IRQ_PRIORITY);
    EnableIRQ(DMA0_DMA_CH_INT_DONE_2_IRQN);
    s_adc1DataReady = false;
}

bool BSP_ADC1_DataReady(void)
{
    return s_adc1DataReady;
}

bool BSP_ADC1_Read(uint16_t values[BSP_ADC1_CHANNEL_COUNT])
{
    uint32_t i;

    if ((values == NULL) || !s_adc1DataReady)
    {
        return false;
    }

    for (i = 0U; i < BSP_ADC1_CHANNEL_COUNT; ++i)
    {
        values[i] = s_adc1Latest[i];
    }
    s_adc1DataReady = false;
    return true;
}

/* Strong definition overrides the weak startup handler. */
void DMA_CH2_IRQHandler(void)
{
    EDMA_HandleIRQ(&g_adc1DmaHandle);
}

/* Registered by the generated DMA initialization. */
void ADC1_DMA_Callback(edma_handle_t *handle, void *userData, bool transferDone, uint32_t tcds)
{
    uint32_t i;
    status_t status;

    (void)handle;
    (void)userData;
    (void)tcds;

    if (!transferDone)
    {
        return;
    }

    for (i = 0U; i < BSP_ADC1_CHANNEL_COUNT; ++i)
    {
        /* LPADC FIFO words contain the conversion result in the low 16 bits. */
        s_adc1Latest[i] = (uint16_t)(g_adc1DmaBuffer[i] & 0xFFFFU);
    }
    s_adc1DataReady = true;

    /* Re-arm the six-word transfer for the next CTIMER4 trigger. */
    status = EDMA_SubmitTransfer(&g_adc1DmaHandle, &DMA0_CH2_TRANSFER0_CONFIG);
    if (status == kStatus_Success)
    {
        EDMA_EnableChannelRequest(DMA0_DMA_BASEADDR, DMA0_CH2_DMA_CHANNEL);
    }
}

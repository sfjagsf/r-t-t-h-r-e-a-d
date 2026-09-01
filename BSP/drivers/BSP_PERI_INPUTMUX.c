/*
 * Application-specific INPUTMUX routes for MCXA176.
 */
#include "BSP_PERI_INPUTMUX.h"

#include "fsl_clock.h"
#include "fsl_reset.h"
#include "MCXA176.h"

/*
 * INPUTMUX ADC1_TRIG[0] source encoding from PERI_INPUTMUX.h:
 * 0x2B selects CTimer4_MAT0.
 */
#define PERI_INPUTMUX_ADC1_TRIG0_CTIMER4_MAT0 (0x2BU)

void PERI_INPUTMUX_Init(void) {
	CLOCK_EnableClock(kCLOCK_InputMux);
	(void) RESET_ClearPeripheralReset(kINPUTMUX0_RST_SHIFT_RSTn);

	INPUTMUX0->ADC1_TRIG[0] = PERI_INPUTMUX_ADC1_TRIG0_CTIMER4_MAT0;
}

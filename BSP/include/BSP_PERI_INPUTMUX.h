/*
 * Application-specific INPUTMUX routes for MCXA176.
 */
#ifndef BSP_PERI_INPUTMUX_H_
#define BSP_PERI_INPUTMUX_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Route CTIMER4 Match 0 to LPADC1 hardware Trigger 0. */
void PERI_INPUTMUX_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_PERI_INPUTMUX_H_ */

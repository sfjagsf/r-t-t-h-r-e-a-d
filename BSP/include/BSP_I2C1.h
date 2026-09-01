/* Application-facing LPI2C1 driver. */
#ifndef BSP_I2C1_H_
#define BSP_I2C1_H_

#include <stddef.h>
#include <stdint.h>

#include "fsl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The address is the unshifted 7-bit I2C address, unlike STM32 HAL's 8-bit form. */
status_t BSP_I2C1_Write(uint8_t deviceAddress, const uint8_t *data, size_t size);
status_t BSP_I2C1_Read(uint8_t deviceAddress, uint8_t *data, size_t size);
status_t BSP_I2C1_MemWrite(uint8_t deviceAddress,
                           uint32_t subaddress,
                           size_t subaddressSize,
                           const uint8_t *data,
                           size_t size);
status_t BSP_I2C1_MemRead(uint8_t deviceAddress,
                                   uint32_t subaddress,
                          size_t subaddressSize,
                          uint8_t *data,
                          size_t size);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2C1_H_ */

/* Blocking LPI2C1 operations corresponding to STM32 HAL I2C master calls. */
#include "BSP_I2C1.h"

#include "fsl_lpi2c.h"
#include "peripherals.h"

static status_t BSP_I2C1_Transfer(uint8_t deviceAddress,
                                  lpi2c_direction_t direction,
                                  uint32_t subaddress,
                                  size_t subaddressSize,
                                  void *data,
                                  size_t size)
{
    lpi2c_master_transfer_t transfer = {
        .flags = kLPI2C_TransferDefaultFlag,
        .slaveAddress = deviceAddress,
        .direction = direction,
        .subaddress = subaddress,
        .subaddressSize = subaddressSize,
        .data = data,
        .dataSize = size,
    };

    if ((subaddressSize > 4U) || ((size != 0U) && (data == NULL)))
    {
        return kStatus_InvalidArgument;
    }

    return LPI2C_MasterTransferBlocking(LPI2C1_PERIPHERAL, &transfer);
}

status_t BSP_I2C1_Write(uint8_t deviceAddress, const uint8_t *data, size_t size)
{
    return BSP_I2C1_Transfer(deviceAddress, kLPI2C_Write, 0U, 0U,
                             (void *)data, size);
}

status_t BSP_I2C1_Read(uint8_t deviceAddress, uint8_t *data, size_t size)
{
    return BSP_I2C1_Transfer(deviceAddress, kLPI2C_Read, 0U, 0U,
                             data, size);
}

status_t BSP_I2C1_MemWrite(uint8_t deviceAddress,
                           uint32_t subaddress,
                           size_t subaddressSize,
                           const uint8_t *data,
                           size_t size)
{
    return BSP_I2C1_Transfer(deviceAddress, kLPI2C_Write,
                             subaddress, subaddressSize, (void *)data, size);
}

status_t BSP_I2C1_MemRead(uint8_t deviceAddress,
                          uint32_t subaddress,
                          size_t subaddressSize,
                          uint8_t *data,
                          size_t size)
{
    return BSP_I2C1_Transfer(deviceAddress, kLPI2C_Read,
                             subaddress, subaddressSize, data, size);
}

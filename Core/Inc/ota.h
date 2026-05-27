#ifndef __OTA_H
#define __OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

/* ====================== OTA 初始化 ====================== */
/**
  * @brief  启动 OTA 接收（UART2 IDLE + DMA 模式）
  * @param  huart:   UART 句柄指针
  * @param  hdma_rx: DMA 接收句柄指针
  */
void ota_init(UART_HandleTypeDef *huart, DMA_HandleTypeDef *hdma_rx);

/* ====================== OTA 轮询 ====================== */
/**
  * @brief  在主循环中周期性调用，处理已接收的 OTA 帧
  * @param  huart:   UART 句柄指针
  * @param  hdma_rx: DMA 接收句柄指针
  */
void ota_poll(UART_HandleTypeDef *huart, DMA_HandleTypeDef *hdma_rx);

/**
  * @brief  查询 OTA 是否已激活（收到 ERASE 命令后进入工作状态）
  * @retval 0=空闲 1=工作中
  */
uint8_t ota_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H */

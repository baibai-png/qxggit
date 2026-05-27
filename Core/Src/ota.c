/* OTA (Over-The-Air) 固件升级模块
 * 协议: 帧同步 + CRC16 校验, YMODEM 风格帧结构
 * 帧格式: [0x5A 0xA5][CMD 1B][SEQ 1B][LEN 2B BE][PAYLOAD N][CRC16 2B BE]
 * 响应:   [0xA5 0x5A][STATUS 1B][INFO 1B][CHECKSUM 1B]
 */

#include "ota.h"

/* ====================== 协议常量 ====================== */
#define OTA_SYNC1              0x5A
#define OTA_SYNC2              0xA5
#define OTA_FRAME_HEADER_LEN   6
#define OTA_FRAME_CRC_LEN      2
#define OTA_MAX_DATA_LEN       1024
#define OTA_UART_TIMEOUT       100

/* 命令 */
#define OTA_CMD_ERASE          0x01
#define OTA_CMD_DATA           0x02
#define OTA_CMD_BOOT           0x03
#define OTA_CMD_INFO           0x04

/* 响应状态 */
#define OTA_ACK                0x90
#define OTA_NAK                0x91
#define OTA_BUSY               0x92
#define OTA_ERR_FLASH          0x93
#define OTA_ERR_LENGTH         0x94
#define OTA_ERR_SEQ            0x95

/* Flash 布局 */
#define FLASH_APP_START_ADDR   0x08008000
#define FLASH_APP_MAX_SIZE     (240 * 1024)

/* ====================== 模块内部变量 ====================== */
static uint8_t   ota_rx_buf[OTA_MAX_DATA_LEN + OTA_FRAME_HEADER_LEN + OTA_FRAME_CRC_LEN];
static volatile uint16_t ota_rx_len = 0;
static volatile uint8_t  ota_rx_done = 0;

static uint8_t   ota_state = 0;
static uint32_t  ota_total_size = 0;
static uint32_t  ota_written = 0;
static uint8_t   ota_expected_seq = 0;

/* ====================== CRC16 (XMODEM) ====================== */
static uint16_t ota_crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0;
  while (len--)
  {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; i++)
    {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc <<= 1;
    }
  }
  return crc;
}

/* ====================== Flash 操作 ====================== */
static uint8_t ota_flash_erase(uint32_t start_addr, uint32_t size)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t page_err = 0;

  uint32_t first_page = (start_addr - FLASH_BASE) / FLASH_PAGE_SIZE;
  uint32_t num_pages  = (size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

  if (HAL_FLASH_Unlock() != HAL_OK)
    return OTA_ERR_FLASH;

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks     = FLASH_BANK_1;
  erase.Page      = first_page;
  erase.NbPages   = num_pages;

  if (HAL_FLASHEx_Erase(&erase, &page_err) != HAL_OK)
  {
    HAL_FLASH_Lock();
    return OTA_ERR_FLASH;
  }

  HAL_FLASH_Lock();
  return OTA_ACK;
}

static uint8_t ota_flash_write(uint32_t addr, const uint8_t *data, uint16_t len)
{
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

  if (HAL_FLASH_Unlock() != HAL_OK)
    return OTA_ERR_FLASH;

  while (len >= 8)
  {
    uint64_t word = *(const uint64_t *)data;
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, word) != HAL_OK)
    {
      HAL_FLASH_Lock();
      return OTA_ERR_FLASH;
    }
    addr += 8;
    data += 8;
    len  -= 8;
  }

  if (len > 0)
  {
    uint64_t word = 0xFFFFFFFFFFFFFFFFULL;
    uint8_t *p = (uint8_t *)&word;
    for (uint16_t i = 0; i < len; i++)
      p[i] = data[i];

    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, word) != HAL_OK)
    {
      HAL_FLASH_Lock();
      return OTA_ERR_FLASH;
    }
  }

  HAL_FLASH_Lock();
  return OTA_ACK;
}

/* ====================== 响应发送 ====================== */
static void ota_send_response(UART_HandleTypeDef *huart, uint8_t status, uint8_t info)
{
  uint8_t resp[5] = { OTA_SYNC1, OTA_SYNC2, status, info, 0 };
  resp[4] = ota_crc16(resp, 4) & 0xFF;
  HAL_UART_Transmit(huart, resp, sizeof(resp), OTA_UART_TIMEOUT);
}

/* ====================== 帧处理状态机 ====================== */
static void ota_process_frame(UART_HandleTypeDef *huart, uint16_t len)
{
  uint8_t *frame = ota_rx_buf;

  if (len < OTA_FRAME_HEADER_LEN + OTA_FRAME_CRC_LEN)
  {
    ota_send_response(huart, OTA_ERR_LENGTH, 0);
    return;
  }

  if (frame[0] != OTA_SYNC1 || frame[1] != OTA_SYNC2)
  {
    ota_send_response(huart, OTA_NAK, 0);
    return;
  }

  uint8_t  cmd = frame[2];
  uint8_t  seq = frame[3];
  uint16_t data_len = ((uint16_t)frame[4] << 8) | frame[5];
  uint8_t *payload  = &frame[6];

  if (OTA_FRAME_HEADER_LEN + data_len + OTA_FRAME_CRC_LEN != len)
  {
    ota_send_response(huart, OTA_ERR_LENGTH, data_len);
    return;
  }

  uint16_t crc_rx   = ((uint16_t)payload[data_len] << 8) | payload[data_len + 1];
  uint16_t crc_calc = ota_crc16(&frame[2], 4 + data_len);
  if (crc_rx != crc_calc)
  {
    ota_send_response(huart, OTA_NAK, 0);
    return;
  }

  switch (cmd)
  {
    case OTA_CMD_ERASE:
    {
      if (data_len < 8)
      {
        ota_send_response(huart, OTA_ERR_LENGTH, 0);
        break;
      }
      uint32_t app_addr = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16)
                        | ((uint32_t)payload[2] << 8)  |  (uint32_t)payload[3];
      uint32_t app_size = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16)
                        | ((uint32_t)payload[6] << 8)  |  (uint32_t)payload[7];

      if (app_addr < FLASH_APP_START_ADDR || app_size > FLASH_APP_MAX_SIZE)
      {
        ota_send_response(huart, OTA_ERR_FLASH, 0);
        break;
      }

      ota_total_size   = app_size;
      ota_written      = 0;
      ota_expected_seq = 0;

      if (ota_flash_erase(app_addr, app_size) == OTA_ACK)
      {
        ota_state = 1;
        ota_send_response(huart, OTA_ACK, 0);
      }
      else
      {
        ota_send_response(huart, OTA_ERR_FLASH, 0);
      }
      break;
    }

    case OTA_CMD_DATA:
    {
      if (ota_state != 1)
      {
        ota_send_response(huart, OTA_BUSY, 0);
        break;
      }
      if (seq != ota_expected_seq)
      {
        ota_send_response(huart, OTA_ERR_SEQ, ota_expected_seq);
        break;
      }
      if (data_len == 0)
      {
        ota_send_response(huart, OTA_ERR_LENGTH, 0);
        break;
      }

      uint32_t addr = FLASH_APP_START_ADDR + ota_written;
      uint8_t ret = ota_flash_write(addr, payload, data_len);
      if (ret == OTA_ACK)
      {
        ota_written += data_len;
        ota_expected_seq++;
        ota_send_response(huart, OTA_ACK, ota_expected_seq - 1);
      }
      else
      {
        ota_send_response(huart, OTA_ERR_FLASH, 0);
      }
      break;
    }

    case OTA_CMD_BOOT:
    {
      if (ota_written < ota_total_size)
      {
        ota_send_response(huart, OTA_ERR_LENGTH, 0);
        break;
      }

      if (*(uint32_t *)FLASH_APP_START_ADDR == 0xFFFFFFFF)
      {
        ota_send_response(huart, OTA_ERR_FLASH, 0);
        break;
      }

      ota_send_response(huart, OTA_ACK, 0);
      HAL_Delay(50);

      __disable_irq();
      SCB->VTOR = FLASH_APP_START_ADDR;
      uint32_t msp = *(uint32_t *)FLASH_APP_START_ADDR;
      __set_MSP(msp);
      uint32_t reset_handler = *(uint32_t *)(FLASH_APP_START_ADDR + 4);
      ((void (*)(void))reset_handler)();
      break;
    }

    case OTA_CMD_INFO:
    {
      uint8_t info[10];
      info[0] = OTA_SYNC1;
      info[1] = OTA_SYNC2;
      info[2] = OTA_ACK;
      info[3] = (FLASH_PAGE_SIZE >> 8) & 0xFF;
      info[4] = FLASH_PAGE_SIZE & 0xFF;
      info[5] = (FLASH_APP_START_ADDR >> 24) & 0xFF;
      info[6] = (FLASH_APP_START_ADDR >> 16) & 0xFF;
      info[7] = (FLASH_APP_START_ADDR >> 8)  & 0xFF;
      info[8] = FLASH_APP_START_ADDR & 0xFF;
      info[9] = ota_crc16(info, 9) & 0xFF;
      HAL_UART_Transmit(huart, info, sizeof(info), OTA_UART_TIMEOUT);
      break;
    }

    default:
      ota_send_response(huart, OTA_NAK, 0);
      break;
  }
}

/* ====================== 状态查询 ====================== */
uint8_t ota_is_active(void)
{
  return (ota_state != 0) ? 1 : 0;
}

/* ====================== 公开接口 ====================== */

void ota_init(UART_HandleTypeDef *huart, DMA_HandleTypeDef *hdma_rx)
{
  __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);
  HAL_UARTEx_ReceiveToIdle_DMA(huart, ota_rx_buf, sizeof(ota_rx_buf));
  __HAL_DMA_DISABLE_IT(hdma_rx, DMA_IT_HT);
}

void ota_poll(UART_HandleTypeDef *huart, DMA_HandleTypeDef *hdma_rx)
{
  if (!ota_rx_done)
    return;

  __disable_irq();
  uint16_t len = ota_rx_len;
  ota_rx_len = 0;
  ota_rx_done = 0;
  __enable_irq();

  ota_process_frame(huart, len);

  HAL_UARTEx_ReceiveToIdle_DMA(huart, ota_rx_buf, sizeof(ota_rx_buf));
  __HAL_DMA_DISABLE_IT(hdma_rx, DMA_IT_HT);
}

/* ====================== HAL 回调 ====================== */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
  if (huart->Instance == USART2 && size > 0)
  {
    ota_rx_len  = size;
    ota_rx_done = 1;
  }
}

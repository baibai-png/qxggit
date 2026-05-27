/**
  * Bootloader — 独立启动程序，驻留在 Flash 0x08000000（32KB 保护区）
  *
  * 启动流程:
  *   上电 → 检测 OTA 入口条件 → [OTA 模式] 或 [跳转 App]
  *
  * OTA 入口条件（任一满足即进入 OTA 模式）:
  *   1. SRAM 中存在魔数 0x4F54414C（App 设置后复位）
  *   2. App 区域无效（首字为擦除态 0xFFFFFFFF）
  *   3. 上电后 OTA_ENTRY_TIMEOUT_MS 内收到上位机 OTA 命令
  *
  * 跳转条件:
  *   - App 区域有效 且 超时未收到 OTA 命令
  */

#include "stm32g4xx_hal.h"
#include "ota.h"
#include "bootloader.h"

/* ====================== 外设句柄 ====================== */
UART_HandleTypeDef  huart2;
DMA_HandleTypeDef   hdma_rx;
DMA_HandleTypeDef   hdma_tx;

/* ====================== 简易系统时钟（HSI 16MHz）====================== */
static void boot_clock_init(void)
{
  /* 使用默认 HSI 16MHz，HAL_Init 已配置 SYSCLK = HSI = 16MHz */
  /* APB1 = AHB = SYSCLK = 16MHz，满足 UART 115200 波特率需求 */
}

/* ====================== GPIO 初始化（PA3 LED + UART2 复用）====================== */
static void boot_gpio_init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* PA3 — 状态指示灯 */
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin   = GPIO_PIN_3;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET); /* 亮灯 = Bootloader 运行中 */

  /* PA2 — USART2_TX (AF7) */
  gpio.Pin       = GPIO_PIN_2;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF7_USART2;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* PB4 — USART2_RX (AF7) */
  gpio.Pin       = GPIO_PIN_4;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF7_USART2;
  HAL_GPIO_Init(GPIOB, &gpio);
}

/* ====================== UART2 初始化（115200-8-N-1, DMA）====================== */
static void boot_uart_init(void)
{
  __HAL_RCC_USART2_CLK_ENABLE();

  huart2.Instance          = USART2;
  huart2.Init.BaudRate     = 115200;
  huart2.Init.WordLength   = UART_WORDLENGTH_8B;
  huart2.Init.StopBits     = UART_STOPBITS_1;
  huart2.Init.Parity       = UART_PARITY_NONE;
  huart2.Init.Mode         = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  HAL_UART_Init(&huart2);

  /* DMA RX — Channel 1 */
  hdma_rx.Instance                 = DMA1_Channel1;
  hdma_rx.Init.Request             = DMA_REQUEST_USART2_RX;
  hdma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_rx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_rx.Init.Mode                = DMA_NORMAL;
  hdma_rx.Init.Priority            = DMA_PRIORITY_LOW;
  HAL_DMA_Init(&hdma_rx);
  __HAL_LINKDMA(&huart2, hdmarx, hdma_rx);

  /* DMA TX — Channel 2 */
  hdma_tx.Instance                 = DMA1_Channel2;
  hdma_tx.Init.Request             = DMA_REQUEST_USART2_TX;
  hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_tx.Init.Mode                = DMA_NORMAL;
  hdma_tx.Init.Priority            = DMA_PRIORITY_LOW;
  HAL_DMA_Init(&hdma_tx);
  __HAL_LINKDMA(&huart2, hdmatx, hdma_tx);

  /* NVIC */
  HAL_NVIC_SetPriority(USART2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

/* ====================== App 有效性检查 ====================== */
uint8_t boot_app_is_valid(void)
{
  uint32_t msp = *(uint32_t *)APP_FLASH_BASE;
  uint32_t reset_handler = *(uint32_t *)(APP_FLASH_BASE + 4);

  /* MSP 必须指向 RAM 区（0x20000000 ~ 0x20008000） */
  if (msp < 0x20000000 || msp > 0x20008000)
    return 0;

  /* Reset_Handler 必须在 App Flash 区 */
  if (reset_handler < APP_FLASH_BASE || reset_handler >= APP_FLASH_BASE + APP_FLASH_MAX_SIZE)
    return 0;

  /* 首字不能是擦除态 */
  if (msp == 0xFFFFFFFF)
    return 0;

  return 1;
}

/* ====================== 跳转到 App ====================== */
void boot_jump_to_app(void)
{
  /* 关闭 Bootloader 使用的外设（给 App 干净的环境） */
  HAL_UART_DeInit(&huart2);
  HAL_DMA_DeInit(&hdma_rx);
  HAL_DMA_DeInit(&hdma_tx);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET); /* 灭灯 */
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_3 | GPIO_PIN_2);
  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_4);

  __disable_irq();

  /* 清除 OTA 魔数 */
  *OTA_MAGIC_ADDR = 0;

  /* 设置 App 的向量表偏移 */
  SCB->VTOR = APP_FLASH_BASE;

  /* 加载 App 的初始栈指针和复位入口 */
  uint32_t app_sp = *(uint32_t *)APP_FLASH_BASE;
  uint32_t app_pc = *(uint32_t *)(APP_FLASH_BASE + 4);

  __set_MSP(app_sp);

  /* 跳到 App 的 Reset_Handler */
  ((void (*)(void))app_pc)();

  /* unreachable */
  while (1);
}

void boot_system_reset(void)
{
  NVIC_SystemReset();
}

void boot_enter_ota(void)
{
  *OTA_MAGIC_ADDR = OTA_MAGIC_VALUE;
  NVIC_SystemReset();
}

/* ====================== OTA 模式主循环 ====================== */
static void boot_ota_loop(uint32_t entry_reason)
{
  /* entry_reason: 0=超时等待 1=魔数触发 2=App无效 */

  /* 通知上位机进入 OTA 模式 */
  const char *msg = "BOOT:OTA_READY\r\n";
  HAL_UART_Transmit(&huart2, (uint8_t *)msg, 16, 100);

  ota_init(&huart2, &hdma_rx);

  uint32_t start_tick = HAL_GetTick();

  while (1)
  {
    ota_poll(&huart2, &hdma_rx);

    /* 心跳 LED：500ms 周期闪烁 */
    if ((HAL_GetTick() - start_tick) % 500 < 250)
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET);
    else
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET);
  }
}

/* ====================== Bootloader 入口 ====================== */
int main(void)
{
  HAL_Init();
  boot_clock_init();
  boot_gpio_init();
  boot_uart_init();

  /* ---- 判断进入方式 ---- */

  /* 条件1: SRAM 中存在 OTA 魔数（App 软件复位请求） */
  if (*OTA_MAGIC_ADDR == OTA_MAGIC_VALUE)
  {
    *OTA_MAGIC_ADDR = 0; /* 清除魔数 */
    boot_ota_loop(1);
  }

  /* 条件2: App 区域无有效固件 */
  if (!boot_app_is_valid())
  {
    HAL_UART_Transmit(&huart2, (uint8_t *)"BOOT:NO_APP\r\n", 14, 100);
    boot_ota_loop(2);
  }

  /* 条件3: 等待 OTA_ENTRY_TIMEOUT_MS，收到命令则进入 OTA */
  HAL_UART_Transmit(&huart2, (uint8_t *)"BOOT:WAIT_OTA...\r\n", 19, 100);

  ota_init(&huart2, &hdma_rx);
  uint32_t start = HAL_GetTick();

  while ((HAL_GetTick() - start) < OTA_ENTRY_TIMEOUT_MS)
  {
    ota_poll(&huart2, &hdma_rx);

    /* 如果 OTA 状态机已激活（收到 ERASE 命令），切到无限 OTA 循环 */
    if (ota_is_active())
      boot_ota_loop(0);
  }

  /* 超时 — 跳转 App */
  boot_jump_to_app();

  return 0;
}

/* ====================== Bootloader 所需的中断处理 ====================== */
/* 注意: App 使用 stm32g4xx_it.c 中的处理器，Bootloader 独立提供 */

void DMA1_Channel1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_rx);
}

void DMA1_Channel2_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_tx);
}

void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart2);
}

/* HAL 需要的 SysTick（Bootloader 不使用，提供空实现） */
void SysTick_Handler(void)
{
  HAL_IncTick();
}

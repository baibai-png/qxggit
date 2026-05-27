#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ====================== Flash 布局 ====================== */
#define BOOT_FLASH_BASE        0x08000000UL
#define BOOT_FLASH_SIZE        (32 * 1024)      /* Bootloader 占用 32KB */
#define APP_FLASH_BASE         0x08008000UL     /* App 起始地址 */
#define APP_FLASH_MAX_SIZE     (96 * 1024)      /* App 最大 96KB (128-32) */

/* ====================== 进入 OTA 模式的方式 ====================== */
#define OTA_MAGIC_ADDR         ((uint32_t *)0x20000000) /* SRAM 首字 */
#define OTA_MAGIC_VALUE        0x4F54414CUL    /* "OTAL" */
#define OTA_ENTRY_TIMEOUT_MS   3000            /* 上电等待 OTA 命令超时 */

/* ====================== Bootloader 公开接口 ====================== */

/**
  * @brief  检查 App 区域是否烧录了有效固件
  * @retval 0=无效 1=有效
  */
uint8_t boot_app_is_valid(void);

/**
  * @brief  跳转到 App（不返回）
  */
void boot_jump_to_app(void);

/**
  * @brief  执行系统复位
  */
void boot_system_reset(void);

/**
  * @brief  App 调用此函数进入 OTA 模式（写魔数 → 系统复位 → Bootloader 识别）
  */
void boot_enter_ota(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOOTLOADER_H */

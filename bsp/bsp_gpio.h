/* bsp/bsp_gpio.h */
#ifndef __BSP_GPIO_H
#define __BSP_GPIO_H

#include "stm32f10x.h"

/* LED 引脚（低电平亮） */
#define LED_SYS_PORT    GPIOB
#define LED_SYS_PIN     GPIO_Pin_12
#define LED_SYS_RCC     RCC_APB2Periph_GPIOB

#define LED_FW_PORT     GPIOA
#define LED_FW_PIN      GPIO_Pin_5
#define LED_FW_RCC      RCC_APB2Periph_GPIOA

#define LED_TX_PORT     GPIOA
#define LED_TX_PIN      GPIO_Pin_12
/* LED_TX_RCC 同 GPIOA，不重复开 */

#define LED_RX_PORT     GPIOC
#define LED_RX_PIN      GPIO_Pin_10
#define LED_RX_RCC      RCC_APB2Periph_GPIOC

/* 按键（上拉输入，低电平按下） */
#define KEY1_PORT       GPIOB
#define KEY1_PIN        GPIO_Pin_11
#define KEY1_RCC        RCC_APB2Periph_GPIOB

#define KEY2_PORT       GPIOE
#define KEY2_PIN        GPIO_Pin_14
#define KEY2_RCC        RCC_APB2Periph_GPIOE

#define KEY_ID_1        1u
#define KEY_ID_2        2u

/* 蜂鸣器 */
#define BUZZER_PORT     GPIOA
#define BUZZER_PIN      GPIO_Pin_3
/* BUZZER_RCC 同 GPIOA */

/* SD 卡片选 */
#define SD_CS_PORT      GPIOD
#define SD_CS_PIN       GPIO_Pin_8
#define SD_CS_RCC       RCC_APB2Periph_GPIOD

void    BSP_GPIO_Init(void);
void    BSP_LED_On(GPIO_TypeDef *port, uint16_t pin);
void    BSP_LED_Off(GPIO_TypeDef *port, uint16_t pin);
void    BSP_LED_Toggle(GPIO_TypeDef *port, uint16_t pin);
uint8_t BSP_Key_Read(uint8_t key_id);
void    BSP_Buzzer_On(void);
void    BSP_Buzzer_Off(void);
void    BSP_SD_CS_Low(void);
void    BSP_SD_CS_High(void);

#endif

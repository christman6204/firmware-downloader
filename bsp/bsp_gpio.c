/* bsp/bsp_gpio.c */
#include "bsp_gpio.h"

void BSP_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    /* 开启所有需要的 GPIO 时钟 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD |
                           RCC_APB2Periph_GPIOE, ENABLE);

    /* --- LED 引脚：推挽输出，默认高电平（灭） --- */
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;

    GPIO_InitStructure.GPIO_Pin = LED_FW_PIN | LED_TX_PIN | BUZZER_PIN; /* PA3,5,12 */
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_SetBits(GPIOA, LED_FW_PIN | LED_TX_PIN | BUZZER_PIN);

    GPIO_InitStructure.GPIO_Pin = LED_SYS_PIN;  /* PB12 */
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    GPIO_SetBits(GPIOB, LED_SYS_PIN);

    GPIO_InitStructure.GPIO_Pin = LED_RX_PIN;   /* PC10 */
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_SetBits(GPIOC, LED_RX_PIN);

    /* --- 按键引脚：上拉输入 --- */
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;

    GPIO_InitStructure.GPIO_Pin = KEY1_PIN;     /* PB11 */
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = KEY2_PIN;     /* PE14 */
    GPIO_Init(GPIOE, &GPIO_InitStructure);

    /* --- SD_CS：推挽输出，默认高（取消选中） --- */
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Pin   = SD_CS_PIN;  /* PD8 */
    GPIO_Init(GPIOD, &GPIO_InitStructure);
    GPIO_SetBits(GPIOD, SD_CS_PIN);

    /* 蜂鸣器为 PA3 高电平有效，与 LED（低电平有效）共组初始化时
       会被 SetBits 拉高 = 上电即响。所有引脚配置完成后，
       显式关闭蜂鸣器，确保上电静音。 */
    BSP_Buzzer_Off();
}

void BSP_LED_On(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_ResetBits(port, pin);   /* 低电平亮 */
}

void BSP_LED_Off(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_SetBits(port, pin);     /* 高电平灭 */
}

void BSP_LED_Toggle(GPIO_TypeDef *port, uint16_t pin)
{
    port->ODR ^= pin;
}

uint8_t BSP_Key_Read(uint8_t key_id)
{
    if (key_id == KEY_ID_1)
        return GPIO_ReadInputDataBit(KEY1_PORT, KEY1_PIN);  /* 0=按下 */
    if (key_id == KEY_ID_2)
        return GPIO_ReadInputDataBit(KEY2_PORT, KEY2_PIN);
    return 1u;
}

void BSP_Buzzer_On(void)  { GPIO_SetBits(BUZZER_PORT, BUZZER_PIN); }
void BSP_Buzzer_Off(void) { GPIO_ResetBits(BUZZER_PORT, BUZZER_PIN); }
void BSP_SD_CS_Low(void)  { GPIO_ResetBits(SD_CS_PORT, SD_CS_PIN); }
void BSP_SD_CS_High(void) { GPIO_SetBits(SD_CS_PORT, SD_CS_PIN); }

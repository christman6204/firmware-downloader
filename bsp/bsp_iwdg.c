/* bsp/bsp_iwdg.c */
#include "bsp_iwdg.h"
#include "stm32f10x_iwdg.h"

void BSP_IWDG_Init(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);   /* 40kHz/64 = 625Hz */
    IWDG_SetReload(2500);                     /* 2500/625 = 4s */
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void BSP_IWDG_Feed(void)
{
    IWDG_ReloadCounter();
}

#include "stm32f446xx.h"


struct Usart2{
    static USART_TypeDef *instance(){
        return USART2;
    };
    void clockEnable()
    {
        RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    };
};


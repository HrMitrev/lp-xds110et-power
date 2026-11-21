/*
 * startup_gcc.c - Startup code and MSP432E401Y interrupt vector table.
 *
 * Places the vector table at application flash base (0x00004000) matching
 * the exact MSP432E401Y IRQn layout (including CAN1 at IRQ 39 so USB0 sits
 * at vector index 58 / IRQ 42), initializes .data/.bss, enables FPU via SDK,
 * and transfers control to main().
 */

#include <stdint.h>

#include "driverlib/fpu.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"

/* Core exception handlers */
void reset_isr(void);
void nmi_handler(void) __attribute__((weak, alias("default_irq_handler")));
void hard_fault_handler(void) __attribute__((weak, alias("default_irq_handler")));
void mem_manage_handler(void) __attribute__((weak, alias("default_irq_handler")));
void bus_fault_handler(void) __attribute__((weak, alias("default_irq_handler")));
void usage_fault_handler(void) __attribute__((weak, alias("default_irq_handler")));
void svc_handler(void) __attribute__((weak, alias("default_irq_handler")));
void debug_mon_handler(void) __attribute__((weak, alias("default_irq_handler")));
void pendsv_handler(void) __attribute__((weak, alias("default_irq_handler")));
void systick_handler(void) __attribute__((weak, alias("default_irq_handler")));

/* Peripheral IRQ handlers (weakly aliased so subsystems override cleanly) */
void gpioa_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiob_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioc_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiod_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioe_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void ssi0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void pwm0_fault_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void pwm0_0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void pwm0_1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void pwm0_2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void qei0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc0ss0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc0ss1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc0ss2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc0ss3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void watchdog0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer0a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer0b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer1a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer1b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer2a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer2b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void comp0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void comp1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void comp2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void sysctl_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void flash_ctrl_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiof_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiog_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioh_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void ssi1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer3a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer3b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void can0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void can1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void emac0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void hib_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void usb0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void pwm0_3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void udma_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void udmaerr_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc1ss0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc1ss1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc1ss2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void adc1ss3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void epi0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioj_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiok_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiol_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void ssi2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void ssi3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart4_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart5_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart6_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void uart7_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer4a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer4b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer5a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer5b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void sysexc_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c4_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c5_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiom_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpion_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop4_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop5_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop6_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpiop7_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq0_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq1_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq2_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq3_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq4_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq5_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq6_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void gpioq7_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer6a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer6b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer7a_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void timer7b_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c6_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c7_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c8_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));
void i2c9_irq_handler(void) __attribute__((weak, alias("default_irq_handler")));

static void default_irq_handler(void);

extern int main(void);

/* Linker script symbols */
extern uint32_t __stack_top;
extern uint32_t __data_load__;
extern uint32_t __data_start__;
extern uint32_t __data_end__;
extern uint32_t __bss_start__;
extern uint32_t __bss_end__;

/* Interrupt vector table placed in .intvecs at Flash base (0x00004000) */
__attribute__((section(".intvecs"), used)) void (*const vector_table[])(void) = {
    (void (*)(void))((uint32_t)&__stack_top), /*   0: Initial Stack Pointer (0x20040000) */
    reset_isr,                                /*   1: Reset Handler */
    nmi_handler,                              /*   2: NMI Handler */
    hard_fault_handler,                       /*   3: Hard Fault Handler */
    mem_manage_handler,                       /*   4: MPU Fault Handler */
    bus_fault_handler,                        /*   5: Bus Fault Handler */
    usage_fault_handler,                      /*   6: Usage Fault Handler */
    0,                                        /*   7: Reserved */
    0,                                        /*   8: Reserved */
    0,                                        /*   9: Reserved */
    0,                                        /*  10: Reserved */
    svc_handler,                              /*  11: SVCall Handler */
    debug_mon_handler,                        /*  12: Debug Monitor Handler */
    0,                                        /*  13: Reserved */
    pendsv_handler,                           /*  14: PendSV Handler */
    systick_handler,                          /*  15: SysTick Handler */

    /* MSP432E401Y External Interrupts (IRQ 0 .. 110) */
    gpioa_irq_handler,      /*  16 (IRQ   0): GPIO Port A */
    gpiob_irq_handler,      /*  17 (IRQ   1): GPIO Port B */
    gpioc_irq_handler,      /*  18 (IRQ   2): GPIO Port C */
    gpiod_irq_handler,      /*  19 (IRQ   3): GPIO Port D */
    gpioe_irq_handler,      /*  20 (IRQ   4): GPIO Port E */
    uart0_irq_handler,      /*  21 (IRQ   5): UART0 */
    uart1_irq_handler,      /*  22 (IRQ   6): UART1 */
    ssi0_irq_handler,       /*  23 (IRQ   7): SSI0 */
    i2c0_irq_handler,       /*  24 (IRQ   8): I2C0 */
    pwm0_fault_irq_handler, /*  25 (IRQ   9): PWM0 Fault */
    pwm0_0_irq_handler,     /*  26 (IRQ  10): PWM0 Generator 0 */
    pwm0_1_irq_handler,     /*  27 (IRQ  11): PWM0 Generator 1 */
    pwm0_2_irq_handler,     /*  28 (IRQ  12): PWM0 Generator 2 */
    qei0_irq_handler,       /*  29 (IRQ  13): QEI0 */
    adc0ss0_irq_handler,    /*  30 (IRQ  14): ADC0 Sequence 0 */
    adc0ss1_irq_handler,    /*  31 (IRQ  15): ADC0 Sequence 1 */
    adc0ss2_irq_handler,    /*  32 (IRQ  16): ADC0 Sequence 2 */
    adc0ss3_irq_handler,    /*  33 (IRQ  17): ADC0 Sequence 3 */
    watchdog0_irq_handler,  /*  34 (IRQ  18): Watchdog Timer 0/1 */
    timer0a_irq_handler,    /*  35 (IRQ  19): Timer 0A */
    timer0b_irq_handler,    /*  36 (IRQ  20): Timer 0B */
    timer1a_irq_handler,    /*  37 (IRQ  21): Timer 1A */
    timer1b_irq_handler,    /*  38 (IRQ  22): Timer 1B */
    timer2a_irq_handler,    /*  39 (IRQ  23): Timer 2A */
    timer2b_irq_handler,    /*  40 (IRQ  24): Timer 2B */
    comp0_irq_handler,      /*  41 (IRQ  25): Analog Comparator 0 */
    comp1_irq_handler,      /*  42 (IRQ  26): Analog Comparator 1 */
    comp2_irq_handler,      /*  43 (IRQ  27): Analog Comparator 2 */
    sysctl_irq_handler,     /*  44 (IRQ  28): System Control */
    flash_ctrl_irq_handler, /*  45 (IRQ  29): Flash Memory Control */
    gpiof_irq_handler,      /*  46 (IRQ  30): GPIO Port F */
    gpiog_irq_handler,      /*  47 (IRQ  31): GPIO Port G */
    gpioh_irq_handler,      /*  48 (IRQ  32): GPIO Port H */
    uart2_irq_handler,      /*  49 (IRQ  33): UART2 */
    ssi1_irq_handler,       /*  50 (IRQ  34): SSI1 */
    timer3a_irq_handler,    /*  51 (IRQ  35): Timer 3A */
    timer3b_irq_handler,    /*  52 (IRQ  36): Timer 3B */
    i2c1_irq_handler,       /*  53 (IRQ  37): I2C1 */
    can0_irq_handler,       /*  54 (IRQ  38): CAN0 */
    can1_irq_handler,       /*  55 (IRQ  39): CAN1 */
    emac0_irq_handler,      /*  56 (IRQ  40): Ethernet MAC0 */
    hib_irq_handler,        /*  57 (IRQ  41): Hibernation Module */
    usb0_irq_handler,       /*  58 (IRQ  42): USB0 MAC */
    pwm0_3_irq_handler,     /*  59 (IRQ  43): PWM0 Generator 3 */
    udma_irq_handler,       /*  60 (IRQ  44): uDMA Software Transfer */
    udmaerr_irq_handler,    /*  61 (IRQ  45): uDMA Error */
    adc1ss0_irq_handler,    /*  62 (IRQ  46): ADC1 Sequence 0 */
    adc1ss1_irq_handler,    /*  63 (IRQ  47): ADC1 Sequence 1 */
    adc1ss2_irq_handler,    /*  64 (IRQ  48): ADC1 Sequence 2 */
    adc1ss3_irq_handler,    /*  65 (IRQ  49): ADC1 Sequence 3 */
    epi0_irq_handler,       /*  66 (IRQ  50): EPI0 */
    gpioj_irq_handler,      /*  67 (IRQ  51): GPIO Port J */
    gpiok_irq_handler,      /*  68 (IRQ  52): GPIO Port K */
    gpiol_irq_handler,      /*  69 (IRQ  53): GPIO Port L */
    ssi2_irq_handler,       /*  70 (IRQ  54): SSI2 */
    ssi3_irq_handler,       /*  71 (IRQ  55): SSI3 */
    uart3_irq_handler,      /*  72 (IRQ  56): UART3 */
    uart4_irq_handler,      /*  73 (IRQ  57): UART4 */
    uart5_irq_handler,      /*  74 (IRQ  58): UART5 */
    uart6_irq_handler,      /*  75 (IRQ  59): UART6 */
    uart7_irq_handler,      /*  76 (IRQ  60): UART7 */
    i2c2_irq_handler,       /*  77 (IRQ  61): I2C2 */
    i2c3_irq_handler,       /*  78 (IRQ  62): I2C3 */
    timer4a_irq_handler,    /*  79 (IRQ  63): Timer 4A */
    timer4b_irq_handler,    /*  80 (IRQ  64): Timer 4B */
    timer5a_irq_handler,    /*  81 (IRQ  65): Timer 5A */
    timer5b_irq_handler,    /*  82 (IRQ  66): Timer 5B */
    sysexc_irq_handler,     /*  83 (IRQ  67): Floating-Point / System Exception */
    0,                      /*  84 (IRQ  68): Reserved */
    0,                      /*  85 (IRQ  69): Reserved */
    i2c4_irq_handler,       /*  86 (IRQ  70): I2C4 */
    i2c5_irq_handler,       /*  87 (IRQ  71): I2C5 */
    gpiom_irq_handler,      /*  88 (IRQ  72): GPIO Port M */
    gpion_irq_handler,      /*  89 (IRQ  73): GPIO Port N */
    0,                      /*  90 (IRQ  74): Reserved */
    0,                      /*  91 (IRQ  75): Reserved (Tamper) */
    gpiop0_irq_handler,     /*  92 (IRQ  76): GPIO Port P (Summary or P0) */
    gpiop1_irq_handler,     /*  93 (IRQ  77): GPIO Port P1 */
    gpiop2_irq_handler,     /*  94 (IRQ  78): GPIO Port P2 */
    gpiop3_irq_handler,     /*  95 (IRQ  79): GPIO Port P3 */
    gpiop4_irq_handler,     /*  96 (IRQ  80): GPIO Port P4 */
    gpiop5_irq_handler,     /*  97 (IRQ  81): GPIO Port P5 */
    gpiop6_irq_handler,     /*  98 (IRQ  82): GPIO Port P6 */
    gpiop7_irq_handler,     /*  99 (IRQ  83): GPIO Port P7 */
    gpioq0_irq_handler,     /* 100 (IRQ  84): GPIO Port Q (Summary or Q0) */
    gpioq1_irq_handler,     /* 101 (IRQ  85): GPIO Port Q1 */
    gpioq2_irq_handler,     /* 102 (IRQ  86): GPIO Port Q2 */
    gpioq3_irq_handler,     /* 103 (IRQ  87): GPIO Port Q3 */
    gpioq4_irq_handler,     /* 104 (IRQ  88): GPIO Port Q4 */
    gpioq5_irq_handler,     /* 105 (IRQ  89): GPIO Port Q5 */
    gpioq6_irq_handler,     /* 106 (IRQ  90): GPIO Port Q6 */
    gpioq7_irq_handler,     /* 107 (IRQ  91): GPIO Port Q7 */
    0,                      /* 108 (IRQ  92): Reserved */
    0,                      /* 109 (IRQ  93): Reserved */
    0,                      /* 110 (IRQ  94): Reserved */
    0,                      /* 111 (IRQ  95): Reserved */
    0,                      /* 112 (IRQ  96): Reserved */
    0,                      /* 113 (IRQ  97): Reserved */
    timer6a_irq_handler,    /* 114 (IRQ  98): Timer 6A */
    timer6b_irq_handler,    /* 115 (IRQ  99): Timer 6B */
    timer7a_irq_handler,    /* 116 (IRQ 100): Timer 7A */
    timer7b_irq_handler,    /* 117 (IRQ 101): Timer 7B */
    i2c6_irq_handler,       /* 118 (IRQ 102): I2C6 */
    i2c7_irq_handler,       /* 119 (IRQ 103): I2C7 */
    0,                      /* 120 (IRQ 104): Reserved */
    0,                      /* 121 (IRQ 105): Reserved */
    0,                      /* 122 (IRQ 106): Reserved */
    0,                      /* 123 (IRQ 107): Reserved */
    0,                      /* 124 (IRQ 108): Reserved */
    i2c8_irq_handler,       /* 125 (IRQ 109): I2C8 */
    i2c9_irq_handler,       /* 126 (IRQ 110): I2C9 */
};

void reset_isr(void)
{
    uint32_t *src_ptr = &__data_load__;
    uint32_t *dst_ptr = &__data_start__;

    while (dst_ptr < &__data_end__) {
        *dst_ptr++ = *src_ptr++;
    }

    dst_ptr = &__bss_start__;
    while (dst_ptr < &__bss_end__) {
        *dst_ptr++ = 0U;
    }

    /* Enable Cortex-M4F FPU coprocessor using TI DriverLib */
    MAP_FPUEnable();
    MAP_FPULazyStackingEnable();

    main();

    while (1) {
    }
}

static void default_irq_handler(void)
{
    while (1) {
    }
}

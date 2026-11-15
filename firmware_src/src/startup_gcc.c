/*
 * startup_gcc.c - Startup code for MSP432E401Y using GNU Arm GCC
 *
 * Implements the interrupt vector table, data/bss segment initialization,
 * FPU coprocessor enablement, and transition to main().
 *
 * Architecture and startup sequence match the original XDS110 firmware binary.
 */

#include <stdint.h>

/* Forward declarations */
void ResetISR(void);
static void NmiSR(void);
static void FaultISR(void);
static void IntDefaultHandler(void);

extern int main(void);

/* Linker script symbols */
extern uint32_t __stack_top;
extern uint32_t __data_load__;
extern uint32_t __data_start__;
extern uint32_t __data_end__;
extern uint32_t __bss_start__;
extern uint32_t __bss_end__;

/* Interrupt vector table placed in .intvecs at Flash base (0x00004000) */
__attribute__((section(".intvecs"), used))
void (* const g_pfnVectors[])(void) =
{
    (void (*)(void))((uint32_t)&__stack_top), /* Initial Stack Pointer (Top of 256K SRAM: 0x20040000) */
    ResetISR,                                  /* Reset Handler */
    NmiSR,                                     /* NMI Handler */
    FaultISR,                                  /* Hard Fault Handler */
    IntDefaultHandler,                         /* MPU Fault Handler */
    IntDefaultHandler,                         /* Bus Fault Handler */
    IntDefaultHandler,                         /* Usage Fault Handler */
    0,                                         /* Reserved */
    0,                                         /* Reserved */
    0,                                         /* Reserved */
    0,                                         /* Reserved */
    IntDefaultHandler,                         /* SVCall Handler */
    IntDefaultHandler,                         /* Debug Monitor Handler */
    0,                                         /* Reserved */
    IntDefaultHandler,                         /* PendSV Handler */
    IntDefaultHandler,                         /* SysTick Handler */

    /* MSP432E4 Peripheral Interrupt Handlers */
    IntDefaultHandler,                         /* GPIO Port A */
    IntDefaultHandler,                         /* GPIO Port B */
    IntDefaultHandler,                         /* GPIO Port C */
    IntDefaultHandler,                         /* GPIO Port D */
    IntDefaultHandler,                         /* GPIO Port E */
    IntDefaultHandler,                         /* UART0 */
    IntDefaultHandler,                         /* UART1 */
    IntDefaultHandler,                         /* SSI0 */
    IntDefaultHandler,                         /* I2C0 */
    IntDefaultHandler,                         /* PWM0 Fault */
    IntDefaultHandler,                         /* PWM0 Generator 0 */
    IntDefaultHandler,                         /* PWM0 Generator 1 */
    IntDefaultHandler,                         /* PWM0 Generator 2 */
    IntDefaultHandler,                         /* QEI0 */
    IntDefaultHandler,                         /* ADC0 Sequence 0 */
    IntDefaultHandler,                         /* ADC0 Sequence 1 */
    IntDefaultHandler,                         /* ADC0 Sequence 2 */
    IntDefaultHandler,                         /* ADC0 Sequence 3 */
    IntDefaultHandler,                         /* Watchdog Timer */
    IntDefaultHandler,                         /* Timer 0A */
    IntDefaultHandler,                         /* Timer 0B */
    IntDefaultHandler,                         /* Timer 1A */
    IntDefaultHandler,                         /* Timer 1B */
    IntDefaultHandler,                         /* Timer 2A */
    IntDefaultHandler,                         /* Timer 2B */
    IntDefaultHandler,                         /* Comp 0 */
    IntDefaultHandler,                         /* Comp 1 */
    IntDefaultHandler,                         /* Comp 2 */
    IntDefaultHandler,                         /* System Control */
    IntDefaultHandler,                         /* Flash Control */
    IntDefaultHandler,                         /* GPIO Port F */
    IntDefaultHandler,                         /* GPIO Port G */
    IntDefaultHandler,                         /* GPIO Port H */
    IntDefaultHandler,                         /* UART2 */
    IntDefaultHandler,                         /* SSI1 */
    IntDefaultHandler,                         /* Timer 3A */
    IntDefaultHandler,                         /* Timer 3B */
    IntDefaultHandler,                         /* I2C1 */
    IntDefaultHandler,                         /* CAN0 */
    IntDefaultHandler,                         /* Ethernet */
    IntDefaultHandler,                         /* Hibernate */
    IntDefaultHandler,                         /* USB0 MAC */
    IntDefaultHandler,                         /* PWM0 Generator 3 */
    IntDefaultHandler,                         /* uDMA Software Transfer */
    IntDefaultHandler,                         /* uDMA Error */
    IntDefaultHandler,                         /* ADC1 Sequence 0 */
    IntDefaultHandler,                         /* ADC1 Sequence 1 */
    IntDefaultHandler,                         /* ADC1 Sequence 2 */
    IntDefaultHandler,                         /* ADC1 Sequence 3 */
    IntDefaultHandler,                         /* EPI0 */
    IntDefaultHandler,                         /* GPIO Port J */
    IntDefaultHandler,                         /* GPIO Port K */
    IntDefaultHandler,                         /* GPIO Port L */
    IntDefaultHandler,                         /* SSI2 */
    IntDefaultHandler,                         /* SSI3 */
    IntDefaultHandler,                         /* UART3 */
    IntDefaultHandler,                         /* UART4 */
    IntDefaultHandler,                         /* UART5 */
    IntDefaultHandler,                         /* UART6 */
    IntDefaultHandler,                         /* UART7 */
    IntDefaultHandler,                         /* I2C2 */
    IntDefaultHandler,                         /* I2C3 */
    IntDefaultHandler,                         /* Timer 4A */
    IntDefaultHandler,                         /* Timer 4B */
    IntDefaultHandler,                         /* Timer 5A */
    IntDefaultHandler,                         /* Timer 5B */
    IntDefaultHandler,                         /* FPU */
    0,                                         /* Reserved */
    0,                                         /* Reserved */
    IntDefaultHandler,                         /* I2C4 */
    IntDefaultHandler,                         /* I2C5 */
    IntDefaultHandler,                         /* GPIO Port M */
    IntDefaultHandler,                         /* GPIO Port N */
    0,                                         /* Reserved */
    IntDefaultHandler,                         /* Tamper */
    IntDefaultHandler,                         /* GPIO Port P (Summary or P0) */
    IntDefaultHandler,                         /* GPIO Port P1 */
    IntDefaultHandler,                         /* GPIO Port P2 */
    IntDefaultHandler,                         /* GPIO Port P3 */
    IntDefaultHandler,                         /* GPIO Port P4 */
    IntDefaultHandler,                         /* GPIO Port P5 */
    IntDefaultHandler,                         /* GPIO Port P6 */
    IntDefaultHandler,                         /* GPIO Port P7 */
    IntDefaultHandler,                         /* GPIO Port Q (Summary or Q0) */
    IntDefaultHandler,                         /* GPIO Port Q1 */
    IntDefaultHandler,                         /* GPIO Port Q2 */
    IntDefaultHandler,                         /* GPIO Port Q3 */
    IntDefaultHandler,                         /* GPIO Port Q4 */
    IntDefaultHandler,                         /* GPIO Port Q5 */
    IntDefaultHandler,                         /* GPIO Port Q6 */
    IntDefaultHandler,                         /* GPIO Port Q7 */
    IntDefaultHandler,                         /* GPIO Port R */
    IntDefaultHandler,                         /* GPIO Port S */
    IntDefaultHandler,                         /* SHA/MD5 0 */
    IntDefaultHandler,                         /* AES 0 */
    IntDefaultHandler,                         /* DES 0 */
    0,                                         /* Reserved */
    IntDefaultHandler,                         /* Timer 6A */
    IntDefaultHandler,                         /* Timer 6B */
    IntDefaultHandler,                         /* Timer 7A */
    IntDefaultHandler,                         /* Timer 7B */
    IntDefaultHandler,                         /* I2C6 */
    IntDefaultHandler,                         /* I2C7 */
    0,                                         /* Reserved */
    IntDefaultHandler,                         /* OneWire0 */
    0,                                         /* Reserved */
    0,                                         /* Reserved */
    0,                                         /* Reserved */
    IntDefaultHandler,                         /* I2C8 */
    IntDefaultHandler,                         /* I2C9 */
    IntDefaultHandler,                         /* GPIO Port T */
};

/*
 * Reset ISR: Called upon CPU reset or entry from the DFU bootloader.
 */
void ResetISR(void)
{
    uint32_t *pui32Src, *pui32Dest;

    /* Copy .data segment initializers from Flash to SRAM */
    pui32Src = &__data_load__;
    for (pui32Dest = &__data_start__; pui32Dest < &__data_end__; )
    {
        *pui32Dest++ = *pui32Src++;
    }

    /* Zero-fill the .bss segment using inline assembly (matches official binary) */
    __asm volatile(
        "    ldr     r0, =__bss_start__\n"
        "    ldr     r1, =__bss_end__\n"
        "    mov     r2, #0\n"
        "    .thumb_func\n"
        "zero_loop:\n"
        "    cmp     r0, r1\n"
        "    it      lt\n"
        "    strlt   r2, [r0], #4\n"
        "    blt     zero_loop"
    );

    /* Enable Floating Point Unit (CP10 and CP11 coprocessors in NVIC_CPAC) */
    *((volatile uint32_t *)0xE000ED88) |= (0x00F00000);

    /* Call the application main() entry point */
    main();

    /* Hang if main returns */
    while (1) {}
}

static void NmiSR(void)
{
    while (1) {}
}

static void FaultISR(void)
{
    while (1) {}
}

static void IntDefaultHandler(void)
{
    while (1) {}
}

/*
 * Flash and RAM map for the DWM3001CDK lock.
 *
 * THIS MAP IS NOT THIS PORT'S TO CHOOSE. It is byte-for-byte the map in
 * apps/dwm3001cdk-lock/pm_static.yml, which the Zephyr build pinned and which
 * every board that already exists has been flashed with. Read that file for why
 * each boundary is where it is; the reasoning is measured, long, and not worth
 * duplicating here. What matters at this end is that the two images agree, for
 * one reason above all others:
 *
 *   settings_storage at 0x7c000 MUST NOT MOVE. It holds the reader identity,
 *   the trust anchors and OpenThread's SRP key, and a board is reflashed with
 *   `west flash` and no --erase precisely so that those four pages survive. A
 *   FreeRTOS image that placed its store anywhere else would orphan every key
 *   provisioned against that board -- silently, and only discovered later.
 *
 * A flash map change is also the one thing an over-the-air update cannot
 * deliver: it needs a cable on every board in the field. So this agrees with
 * the oracle rather than optimising for itself.
 *
 *   0x00000  mcuboot           40,960 B  a separate image; see the note below
 *   0x0a000  mcuboot_pad          512 B  the signed image header imgtool writes
 *   0x0a200  app              433,664 B  THIS IMAGE
 *   0x74000  patch_staging     32,768 B  one inbound OTA delta patch
 *   0x7c000  settings_storage  16,384 B  the key-value store
 *
 * WHY THE BOOTLOADER BEING A ZEPHYR BUILD IS NOT A CONTRADICTION. MCUboot is a
 * separate image with its own reset vector, and nothing about it is linked
 * into this one. It hands over the core and is gone. Which RTOS built it is
 * invisible from here, and rebuilding it under FreeRTOS would buy nothing and
 * cost the delta-patch applier that already works. The port is Zephyr-free;
 * the bootloader in front of it is a prebuilt artifact, like the SoftDevice
 * Controller.
 */

/*
 * The origin is written out rather than derived from a symbol: a MEMORY block
 * is parsed before any symbol assignment, so it cannot read one. Bringing a
 * board up without a bootloader means editing these two numbers and knowing
 * why, which is the right amount of friction for a change that invalidates
 * every signed image.
 */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x0000a200, LENGTH = 0x69e00
  STORAGE (r) : ORIGIN = 0x0007c000, LENGTH = 0x04000
  RAM (rwx)   : ORIGIN = 0x20000000, LENGTH = 0x20000
}

__ultrawidelock_storage_start = ORIGIN(STORAGE);
__ultrawidelock_storage_end = ORIGIN(STORAGE) + LENGTH(STORAGE);

/*
 * Where an inbound OTA delta patch is written before a reboot into MCUboot.
 * Declared here rather than in C so that there is exactly one flash map and the
 * staging code cannot disagree with it.
 */
__ultrawidelock_patch_staging_start = 0x00074000;
__ultrawidelock_patch_staging_end = 0x0007c000;

/*
 * The vector table's address, which the startup code loads into VTOR. Taken
 * from the map rather than hardcoded, because the whole point of the two
 * symbols above is that one file owns these numbers.
 */
__ultrawidelock_vector_table = ORIGIN(FLASH);

/*
 * The main stack, used by every interrupt handler and by main() before the
 * scheduler starts. Task stacks are separate and come from their own storage.
 * MPSL's handlers run here at priority 0, so this is not a place to economise.
 */
__stack_size = DEFINED(__stack_size) ? __stack_size : 4096;

ENTRY(Reset_Handler)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)

        KEEP(*(.init))
        KEEP(*(.fini))

        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)

        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        *(.rodata*)

        KEEP(*(.eh_frame*))
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    . = ALIGN(4);
    __etext = .;

    .data : AT (__etext)
    {
        __data_start__ = .;
        *(vtable)
        *(.data*)

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN (__fini_array_end = .);

        KEEP(*(.jcr*))
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    /*
     * Deliberately not loaded and not zeroed. SEGGER RTT's control block lives
     * here so a debugger attached after a reset can still find a buffer that
     * survived it, and the fault path can leave a breadcrumb for the next boot.
     */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        *(.noinit*)
        . = ALIGN(4);
    } > RAM

    .heap (COPY) :
    {
        . = ALIGN(8);
        __HeapBase = .;
        __end__ = .;
        PROVIDE(end = .);
        KEEP(*(.heap*))
        __HeapLimit = .;
    } > RAM

    .stack (COPY) :
    {
        . = ALIGN(8);
        __StackLimit = .;
        . = . + __stack_size;
        . = ALIGN(8);
        __StackTop = .;
    } > RAM
    PROVIDE(__stack = __StackTop);

    /*
     * The stack is placed explicitly rather than floated to the top of RAM, so
     * that overflowing it runs into a linker error at build time instead of
     * into whatever happens to be below it at run time.
     */
    ASSERT(__StackTop <= ORIGIN(RAM) + LENGTH(RAM), "RAM overflowed: stack does not fit")
    ASSERT(__StackLimit >= __HeapLimit, "RAM overflowed: heap and stack overlap")
    ASSERT(__etext + (__data_end__ - __data_start__) <= ORIGIN(FLASH) + LENGTH(FLASH),
           "FLASH overflowed: image does not fit the MCUboot application slot at 0xa200")
}

/* AstraOS x86-64 Linker Script for Limine Bootloader */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kmain)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* Execute | Read */
    rodata  PT_LOAD    FLAGS((1 << 2));            /* Read */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)); /* Write | Read */
}

SECTIONS
{
    . = 0xffffffff80200000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(COMMONPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(COMMONPAGESIZE));

    .data : {
        *(.data .data.*)
        *(.limine_requests_start)
        *(.limine_requests)
        *(.limine_requests_end)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}

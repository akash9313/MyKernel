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

    __kernel_start = .;
    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    . = ALIGN(CONSTANT(COMMONPAGESIZE));
    __text_end = .;

    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    . = ALIGN(CONSTANT(COMMONPAGESIZE));
    __rodata_end = .;

    __data_start = .;
    .data : {
        *(.data .data.*)
        *(.limine_requests_start)
        *(.limine_requests)
        *(.limine_requests_end)
    } :data

    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data
    . = ALIGN(CONSTANT(COMMONPAGESIZE));
    __bss_end = .;
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}

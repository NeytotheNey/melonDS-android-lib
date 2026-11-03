/* Flat image at LOAD_ADDR (set with --defsym), .bss included as zeros so the
   direct boot's copy clears it. */
ENTRY(_start)
SECTIONS
{
    . = LOAD_ADDR;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.bss .bss.*) *(COMMON) . = ALIGN(4); }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}

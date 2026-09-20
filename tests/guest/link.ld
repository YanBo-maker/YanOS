ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : {
    *(.text.start)
    *(.text .text.*)
  }
  .rodata : {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  }
  .data : {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }
  .bss : {
    /* The whole writable state of a Guest lives here: every test buffer and
     * the stack. `begin_signature`/`end_signature` make it the signature
     * region the RISC-V validation flow (and sail's --test-signature) expects,
     * so a reference model and the DUT can be compared byte for byte. */
    begin_signature = .;
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    . = ALIGN(4);
    end_signature = .;
  }
  . = ALIGN(16);
  .tohost : { *(.tohost) }
  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.riscv.attributes)
  }
}

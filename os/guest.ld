/* Linker script for the production Guest terminal image (0022).
 *
 * Layout only: entry at _start from os/trap_entry.S, the low address the
 * platform loads images at, and the sections that file places (text, rodata,
 * data, bss/sbss/COMMON and the tohost word). It deliberately declares no
 * test-fixture symbols such as a signature region, so the production image
 * shares no artifact with the validation corpus.
 *
 * The host loader zero-fills .bss, so the script does not emit a zeroing loop.
 * The .tohost output section is the only place the runtime's finishing word
 * lives; yan_run finds it through the symbol table. */
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
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    . = ALIGN(4);
  }
  . = ALIGN(16);
  .tohost : { *(.tohost) }
  /* 0036-R3: one-shot bump arena, after every loadable section. The end is the
   * Guest's 32 MiB budget (0x80000000 + 32 MiB), well below the channel rings
   * at 0x83FFC000 when yan_run's default 64 MiB RAM is used. */
  . = ALIGN(16);
  __yan_arena_start = .;
  __yan_arena_end = 0x80000000 + 32M;
  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.riscv.attributes)
  }
}

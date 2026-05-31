/* JCY8001 OTA Bootloader: 16KB @ 0x08000000, 48KB RAM */
MEMORY {
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 16K
  RAM  (xrw) : ORIGIN = 0x20000000, LENGTH = 48K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) } > FLASH
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT> FLASH
  .bss  : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
}

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)
MEMORY
{
  /* 0x7A000..0x7EFFF boot code, 0x7F000 metadata. */
  FLASH (rx) : ORIGIN = 0x0007A000, LENGTH = 0x00005000
  RAM (rwx)  : ORIGIN = 0x20004C28, LENGTH = 0x0000B3B8
}
SECTIONS { }
SECTIONS
{
  .mem_section_dummy_ram : { } > RAM
  .fs_data : { PROVIDE(__start_fs_data = .); KEEP(*(.fs_data)) PROVIDE(__stop_fs_data = .); } > RAM
} INSERT AFTER .data;
SECTIONS
{
  .mem_section_dummy_rom : { } > FLASH
  .sdh_soc_observers : { PROVIDE(__start_sdh_soc_observers = .); KEEP(*(SORT(.sdh_soc_observers*))) PROVIDE(__stop_sdh_soc_observers = .); } > FLASH
  .sdh_ble_observers : { PROVIDE(__start_sdh_ble_observers = .); KEEP(*(SORT(.sdh_ble_observers*))) PROVIDE(__stop_sdh_ble_observers = .); } > FLASH
  .sdh_req_observers : { PROVIDE(__start_sdh_req_observers = .); KEEP(*(SORT(.sdh_req_observers*))) PROVIDE(__stop_sdh_req_observers = .); } > FLASH
  .sdh_state_observers : { PROVIDE(__start_sdh_state_observers = .); KEEP(*(SORT(.sdh_state_observers*))) PROVIDE(__stop_sdh_state_observers = .); } > FLASH
  .sdh_stack_observers : { PROVIDE(__start_sdh_stack_observers = .); KEEP(*(SORT(.sdh_stack_observers*))) PROVIDE(__stop_sdh_stack_observers = .); } > FLASH
} INSERT AFTER .text;
INCLUDE "nrf_common.ld"

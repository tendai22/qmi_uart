//
// conf.h ... hardware/software configuration parameters
//

#if !defined(__CONF_H)
//
// USE_STUB .. qmi_flash stubbing
//    use 128KB SRAM storage for pseudo FLASH
//
//#define USE_STUB

// -----------------------------------------------------------------------------
// UART
// -----------------------------------------------------------------------------

#define UART_ID uart0
#define BAUD_RATE 115200

#define UART_TX_PIN 0
#define UART_RX_PIN 1

//
// flash
//

#if defined(USE_STUB)
#define FLASH_SIZE          0x20000u    // 128kB
#else
#define FLASH_SIZE          0x400000u  // 4 MiB
#endif

#define SECTOR_SIZE         0x1000u    // 4 KiB
#define PAGE_SIZE           0x100u     // 256 bytes

#endif //__CONF_H
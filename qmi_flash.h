//
// qmi_flash.h
//

#if !defined(__QMI_FLASH_H)

extern void qmi_init(void);
extern int qmi_flash_test(void);
extern bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length);
extern bool qmi_flash_erase_4k(uint32_t address);
extern bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length);

#endif //__QMI_FLASH_H

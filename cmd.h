//
// cmd.h
//

#if !defined(__CMD_H)
// ------------------------------------------------------------
// Configuration
// ------------------------------------------------------------

#define BUFFER_SIZE         SECTOR_SIZE
#define LINE_SIZE           128

extern void command_loop(void);

#endif //__CMD_H

#ifndef ADB_SHELL_H
#define ADB_SHELL_H

#include <stdint.h>
#include <stddef.h>

void adb_shell_init(void);
void adb_shell_handle_open(uint32_t remote_id, const char *name);
void adb_shell_handle_wrte(uint32_t local_id, uint32_t remote_id, const uint8_t *data, size_t len);
void adb_shell_handle_clse(uint32_t local_id, uint32_t remote_id);
void adb_shell_handle_okay(uint32_t local_id, uint32_t remote_id);
void adb_shell_reset(void);

#endif

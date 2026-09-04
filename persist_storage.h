/* ============================================================================
 *  persist_storage.h -- ATA PIO disk + encrypted persistent volume
 *
 *  Security model (honest):
 *   - No network stack (remote online attacks out of scope by design)
 *   - Volume encrypted with AES-128-CTR; key from login password + salt
 *   - CRC32 integrity; fail-closed on mismatch / bad magic
 *   - Keys wiped from RAM after use
 *  This is strong for a hobby offline OS, not a formal FIPS evaluation.
 * ==========================================================================*/
#ifndef PERSIST_STORAGE_H
#define PERSIST_STORAGE_H

typedef unsigned char  p_u8;
typedef unsigned short p_u16;
typedef unsigned int   p_u32;
typedef int            p_bool;
#define P_TRUE  1
#define P_FALSE 0

/* Provided by kernel: serialize whole RAMFS into buf, return bytes used (0=fail) */
typedef p_u32 (*persist_export_fn)(void *buf, p_u32 cap);
/* Import blob into RAMFS; return non-zero on success */
typedef p_bool (*persist_import_fn)(const void *buf, p_u32 len);

void persist_init(persist_export_fn exp, persist_import_fn imp);

/* Probe ATA; returns 1 if a disk responds */
p_bool persist_disk_present(void);

/* Create empty encrypted volume (call once / after format) */
p_bool persist_format(const char *password);

/* Load volume using password; mounts into RAMFS via import callback */
p_bool persist_load(const char *password);

/* Encrypt+write current RAMFS to disk */
p_bool persist_save(const char *password);

p_bool persist_is_mounted(void);
const char *persist_last_error(void);

#endif

/* Persistent credentials + settings (survive reboot) */
p_bool persist_cred_save(const char *password, p_u32 wallpaper, p_u32 theme,
                         p_u32 security, p_u32 idle_sec);
p_bool persist_cred_load(p_u32 *wallpaper, p_u32 *theme, p_u32 *security, p_u32 *idle_sec);
p_bool persist_cred_verify(const char *password);
p_bool persist_cred_exists(void);

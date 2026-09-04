/* ============================================================================
 *  lockscreen.h -- Windows-style lock screen for Titan Kernel
 * ==========================================================================*/
#ifndef LOCKSCREEN_H
#define LOCKSCREEN_H

typedef unsigned char  lk_u8;
typedef unsigned short lk_u16;
typedef unsigned int   lk_u32;
typedef int            lk_bool;
#define LK_TRUE  1
#define LK_FALSE 0

#define LOCK_USER_MAX 24
#define LOCK_PASS_MAX 32

void     lock_init(void);
void     lock_activate(void);
lk_bool  lock_is_active(void);
lk_bool  lock_login_visible(void);
int      lock_focus(void);
lk_bool  lock_has_error(void);
const char *lock_username(void);
const char *lock_password(void);
lk_u32   lock_user_len(void);
lk_u32   lock_pass_len(void);

void     lock_on_click(void);
void     lock_on_key(char c);

typedef void (*lock_success_fn)(const char *password);
void     lock_set_success_hook(lock_success_fn fn);
typedef int (*lock_verify_fn)(const char *user, const char *password);
void     lock_set_verify_hook(lock_verify_fn fn);
void     lock_set_password_memory(const char *new_pass); /* update default in-RAM */

void     lock_format_time(char *buf, int buflen);
void     lock_format_date(char *buf, int buflen);

/* Security hardening */
void     lock_set_security_level(lk_u32 level);
lk_bool  lock_change_password(const char *old_pass, const char *new_pass);
lk_u32   lock_fail_count(void);
lk_bool  lock_is_locked_out(void);
lk_u32   lock_lockout_seconds_left(void);
void     lock_tick(lk_u32 now_ticks, lk_u32 hz);

#endif

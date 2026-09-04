/* ============================================================================
 *  lockscreen.c -- Lock + login lockout (Titan Kernel security step 1)
 * ==========================================================================*/
#include "lockscreen.h"
#include "timeanddate.h"
#include "ossystemsafety.h"

static lk_bool active = LK_TRUE;
static lk_bool login_vis = LK_FALSE;
static int     focus = 0;
static lk_bool err = LK_FALSE;

static char user[LOCK_USER_MAX + 1];
static char pass[LOCK_PASS_MAX + 1];
static lk_u32 user_len = 0;
static lk_u32 pass_len = 0;

static const char *USER_OK = "titan";
/* Empty by default — no hardcoded secret in the binary.
 * First-boot / no-disk: empty password is accepted only until a password is set.
 * After lock_set_password_memory() or lock_change_password(), non-empty is required. */
static char PASS_OK[LOCK_PASS_MAX + 1] = { 0 };
static lock_success_fn g_success = 0;
static lock_verify_fn g_verify = 0;

/* lockout: after N fails, block input for delay seconds */
static lk_u32 sec_level = 1;
static lk_u32 fail_count = 0;
static lk_u32 lockout_until_tick = 0;
static lk_u32 last_tick = 0;
static lk_u32 tick_hz = 100;

void lock_set_success_hook(lock_success_fn fn) { g_success = fn; }
void lock_set_verify_hook(lock_verify_fn fn) { g_verify = fn; }
void lock_set_password_memory(const char *new_pass) {
    if (!new_pass) {
        safety_wipe(PASS_OK, sizeof(PASS_OK));
        return;
    }
    lk_u32 n = 0;
    while (new_pass[n] && n < LOCK_PASS_MAX) { PASS_OK[n] = new_pass[n]; n++; }
    PASS_OK[n] = 0;
    /* wipe any leftover tail beyond the new length */
    if (n < LOCK_PASS_MAX)
        safety_wipe(PASS_OK + n + 1, LOCK_PASS_MAX - n);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

void lock_set_security_level(lk_u32 level) {
    sec_level = level;
}

lk_bool lock_change_password(const char *oldp, const char *newp) {
    if (!oldp || !newp || !newp[0]) return LK_FALSE;
    if (!str_eq(oldp, PASS_OK)) return LK_FALSE;
    /* reject identical new password (no-op that looks like success) */
    if (str_eq(oldp, newp)) return LK_FALSE;
    lk_u32 n = 0;
    while (newp[n] && n < LOCK_PASS_MAX) { PASS_OK[n] = newp[n]; n++; }
    PASS_OK[n] = 0;
    if (n < LOCK_PASS_MAX)
        safety_wipe(PASS_OK + n + 1, LOCK_PASS_MAX - n);
    return LK_TRUE;
}

lk_u32 lock_fail_count(void) { return fail_count; }

lk_bool lock_is_locked_out(void) {
    if (lockout_until_tick == 0) return LK_FALSE;
    if (last_tick >= lockout_until_tick) {
        lockout_until_tick = 0;
        return LK_FALSE;
    }
    return LK_TRUE;
}

lk_u32 lock_lockout_seconds_left(void) {
    if (!lock_is_locked_out()) return 0;
    lk_u32 left = lockout_until_tick - last_tick;
    if (tick_hz == 0) return 0;
    return (left + tick_hz - 1) / tick_hz;
}

void lock_tick(lk_u32 now_ticks, lk_u32 hz) {
    last_tick = now_ticks;
    if (hz) tick_hz = hz;
    if (lockout_until_tick && last_tick >= lockout_until_tick)
        lockout_until_tick = 0;
}

void lock_init(void) {
    active = LK_TRUE;
    login_vis = LK_FALSE;
    focus = 0;
    err = LK_FALSE;
    user_len = 0; pass_len = 0;
    user[0] = 0; pass[0] = 0;
    /* keep fail_count across soft re-lock so attacker cannot reset by locking */
}

void lock_activate(void) {
    active = LK_TRUE;
    login_vis = LK_FALSE;
    focus = 0;
    err = LK_FALSE;
    user_len = 0; pass_len = 0;
    user[0] = 0; pass[0] = 0;
    safety_wipe(pass, sizeof(pass));
    safety_wipe(user, sizeof(user));
}

lk_bool lock_is_active(void) { return active; }
lk_bool lock_login_visible(void) { return login_vis; }
int     lock_focus(void) { return focus; }
lk_bool lock_has_error(void) { return err; }
const char *lock_username(void) { return user; }
const char *lock_password(void) { return pass; }
lk_u32  lock_user_len(void) { return user_len; }
lk_u32  lock_pass_len(void) { return pass_len; }

void lock_on_click(void) {
    if (!active) return;
    if (lock_is_locked_out()) return;
    if (!login_vis) {
        login_vis = LK_TRUE;
        focus = 0;
        err = LK_FALSE;
        return;
    }
    err = LK_FALSE;
}

static void apply_fail_penalty(void) {
    fail_count++;
    /* thresholds depend on security level */
    lk_u32 max_before_lock = (sec_level >= 2) ? 3u : (sec_level >= 1) ? 5u : 8u;
    lk_u32 base_delay_sec = (sec_level >= 2) ? 15u : (sec_level >= 1) ? 8u : 3u;
    if (fail_count >= max_before_lock) {
        lk_u32 mult = fail_count - max_before_lock + 1;
        if (mult > 8) mult = 8;
        lk_u32 delay = base_delay_sec * mult;
        lockout_until_tick = last_tick + delay * tick_hz;
    }
}

static void try_login(void) {
    if (lock_is_locked_out()) return;
    user[user_len] = 0;
    pass[pass_len] = 0;
    {
        int ok = 0;
        if (g_verify)
            ok = g_verify(user, pass);
        else
            ok = str_eq(user, USER_OK) && str_eq(pass, PASS_OK);
        if (ok) {
            active = LK_FALSE;
            login_vis = LK_FALSE;
            err = LK_FALSE;
            fail_count = 0;
            lockout_until_tick = 0;
            if (g_success) g_success(pass);
            safety_wipe(pass, sizeof(pass));
            pass_len = 0;
        } else {
            err = LK_TRUE;
            safety_wipe(pass, sizeof(pass));
            pass_len = 0;
            focus = 1;
            apply_fail_penalty();
        }
    }
}

void lock_on_key(char c) {
    if (!active || !login_vis) return;
    if (lock_is_locked_out()) return;

    if (c == '\n') {
        if (focus == 0) { focus = 1; return; }
        try_login();
        return;
    }
    if (c == '\t') { focus = 1 - focus; err = LK_FALSE; return; }
    if (c == '\b') {
        if (focus == 0 && user_len > 0) { user_len--; user[user_len] = 0; }
        else if (focus == 1 && pass_len > 0) { pass_len--; pass[pass_len] = 0; }
        err = LK_FALSE;
        return;
    }
    if (c >= 32 && c < 127) {
        if (focus == 0 && user_len < LOCK_USER_MAX) {
            user[user_len++] = c; user[user_len] = 0;
        } else if (focus == 1 && pass_len < LOCK_PASS_MAX) {
            pass[pass_len++] = c; pass[pass_len] = 0;
        }
        err = LK_FALSE;
    }
}

void lock_format_time(char *buf, int buflen) {
    if (buflen < 6) { if (buflen > 0) buf[0] = 0; return; }
    td_u8 h, m, s, d, mo;
    td_u16 y;
    rtc_read_datetime(&h, &m, &s, &d, &mo, &y);
    buf[0] = '0' + (h / 10);
    buf[1] = '0' + (h % 10);
    buf[2] = ':';
    buf[3] = '0' + (m / 10);
    buf[4] = '0' + (m % 10);
    buf[5] = 0;
}

void lock_format_date(char *buf, int buflen) {
    if (buflen < 12) { if (buflen > 0) buf[0] = 0; return; }
    td_u8 h, m, s, d, mo;
    td_u16 y;
    rtc_read_datetime(&h, &m, &s, &d, &mo, &y);
    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    const char *mon = (mo >= 1 && mo <= 12) ? months[mo - 1] : "???";
    int i = 0;
    buf[i++] = '0' + (d / 10);
    buf[i++] = '0' + (d % 10);
    buf[i++] = ' ';
    buf[i++] = mon[0]; buf[i++] = mon[1]; buf[i++] = mon[2];
    buf[i++] = ' ';
    buf[i++] = '0' + ((y / 1000) % 10);
    buf[i++] = '0' + ((y / 100) % 10);
    buf[i++] = '0' + ((y / 10) % 10);
    buf[i++] = '0' + (y % 10);
    buf[i] = 0;
}

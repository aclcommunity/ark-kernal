/* ============================================================================
 *  timeanddate.c -- CMOS Real-Time Clock driver + Windows-style formatter
 *  Part of Titan Kernel v3 (Glass Edition). Freestanding, no libc.
 * ==========================================================================*/
#include "timeanddate.h"

static inline void td_outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline unsigned char td_inb(unsigned short port) {
    unsigned char r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

static unsigned char cmos_read(unsigned char reg) {
    td_outb(CMOS_INDEX, reg);
    return td_inb(CMOS_DATA);
}
static int cmos_update_in_progress(void) {
    td_outb(CMOS_INDEX, 0x0A);
    return td_inb(CMOS_DATA) & 0x80;
}
static unsigned char bcd_to_bin(unsigned char v) {
    return (unsigned char)((v & 0x0F) + (v >> 4) * 10);
}

void rtc_read_datetime(td_u8 *hour, td_u8 *min, td_u8 *sec,
                        td_u8 *day, td_u8 *month, td_u16 *year) {
    unsigned char s, m, h, d, mo, y;
    unsigned char s2, m2, h2, d2, mo2, y2;

    /* Standard OSDev double-read pattern: read all fields, wait for any
       in-progress update, read again, and only accept the result once two
       consecutive reads agree -- this avoids catching the RTC mid-tick. */
    for (;;) {
        while (cmos_update_in_progress()) {}
        s = cmos_read(0x00); m = cmos_read(0x02); h = cmos_read(0x04);
        d = cmos_read(0x07); mo = cmos_read(0x08); y = cmos_read(0x09);

        while (cmos_update_in_progress()) {}
        s2 = cmos_read(0x00); m2 = cmos_read(0x02); h2 = cmos_read(0x04);
        d2 = cmos_read(0x07); mo2 = cmos_read(0x08); y2 = cmos_read(0x09);

        if (s == s2 && m == m2 && h == h2 && d == d2 && mo == mo2 && y == y2) break;
    }

    unsigned char regB = cmos_read(0x0B);
    if (!(regB & 0x04)) { /* values are BCD, not straight binary -- convert */
        s  = bcd_to_bin(s);
        m  = bcd_to_bin(m);
        h  = (unsigned char)(bcd_to_bin(h & 0x7F) | (h & 0x80));
        d  = bcd_to_bin(d);
        mo = bcd_to_bin(mo);
        y  = bcd_to_bin(y);
    }
    if (!(regB & 0x02) && (h & 0x80)) { /* 12-hour mode with PM flag set */
        h = (unsigned char)(((h & 0x7F) + 12) % 24);
    }

    *hour = h; *min = m; *sec = s; *day = d; *month = mo;
    /* CMOS "century" register location varies by BIOS/emulator and QEMU's
       default doesn't reliably expose it, so we assume the 2000s -- fine
       for a hobby kernel; a real OS would consult the ACPI FADT century field. */
    *year = (td_u16)(2000 + y);
}

static void put2(char *buf, int *i, td_u8 v) {
    buf[(*i)++] = (char)('0' + (v / 10) % 10);
    buf[(*i)++] = (char)('0' + v % 10);
}

void format_datetime_windows(char *buf, td_u8 hour, td_u8 min, td_u8 sec,
                              td_u8 day, td_u8 month, td_u16 year) {
    int i = 0;
    put2(buf, &i, hour);  buf[i++] = ':';
    put2(buf, &i, min);   buf[i++] = ':';
    put2(buf, &i, sec);
    buf[i++] = '\n';
    put2(buf, &i, day);   buf[i++] = '/';
    put2(buf, &i, month); buf[i++] = '/';
    buf[i++] = (char)('0' + (year / 1000) % 10);
    buf[i++] = (char)('0' + (year / 100)  % 10);
    buf[i++] = (char)('0' + (year / 10)   % 10);
    buf[i++] = (char)('0' + year % 10);
    buf[i] = '\0';
}

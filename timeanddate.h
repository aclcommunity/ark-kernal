/* ============================================================================
 *  timeanddate.h -- Real-time clock (CMOS RTC) time/date module for Titan
 *
 *  Reads the motherboard's battery-backed CMOS RTC chip (the same chip
 *  every BIOS/UEFI reads its clock from) and formats it Windows-taskbar
 *  style: "HH:MM:SS" on one line, "DD/MM/YYYY" on the next.
 * ==========================================================================*/
#ifndef TIMEANDDATE_H
#define TIMEANDDATE_H

typedef unsigned char  td_u8;
typedef unsigned short td_u16;

/* Reads the current wall-clock time/date from the CMOS RTC. Blocks briefly
 * (a few microseconds) while the RTC's "update in progress" flag is set,
 * and double-reads to guard against catching the clock mid-tick. */
void rtc_read_datetime(td_u8 *hour, td_u8 *min, td_u8 *sec,
                        td_u8 *day, td_u8 *month, td_u16 *year);

/* Formats into buf as two lines separated by '\n':
 *   "HH:MM:SS\nDD/MM/YYYY"
 * buf must have room for at least 20 bytes. */
void format_datetime_windows(char *buf, td_u8 hour, td_u8 min, td_u8 sec,
                              td_u8 day, td_u8 month, td_u16 year);

#endif /* TIMEANDDATE_H */

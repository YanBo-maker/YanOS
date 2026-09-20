#ifndef YAN_OS_CONSOLE_H
#define YAN_OS_CONSOLE_H

/* YanOS console: the first real consumer of the UART character device.
 *
 * The driver contract in docs/specs/0015-uart-device.md is the reason this
 * interface has no blocking call. When no terminal backend is attached the
 * device reports CONNECTED=0, and a driver must not wait for a ready bit that
 * will never be set: every entry point below returns YAN_OS_UNAVAILABLE
 * instead, so the caller can keep running headless. */

typedef enum {
    /* The operation completed. */
    YAN_OS_OK = 0,
    /* No terminal backend is attached, or the device refused the byte right
     * now. The caller must not retry in a tight loop; it should carry on and
     * report the condition at a policy level. */
    YAN_OS_UNAVAILABLE,
    YAN_OS_INVALID_ARGUMENT
} YanOsResult;

/* True when a terminal backend is attached. Callers that want to avoid
 * composing output at all can check this once instead of testing every call. */
int yan_os_console_connected(void);

/* Write one character. Returns YAN_OS_UNAVAILABLE rather than blocking. */
YanOsResult yan_os_console_putc(char c);

/* Write a NUL-terminated string. Stops at the first refused character and
 * reports it; it does not spin and does not buffer. */
YanOsResult yan_os_console_puts(const char *text);

/* Read one line into `buffer`, terminated with NUL. `length` receives the
 * number of characters stored, excluding the terminator. Returns
 * YAN_OS_UNAVAILABLE immediately when no terminal is attached, so a headless
 * run does not hang here. */
YanOsResult yan_os_console_getline(char *buffer, unsigned capacity,
                                   unsigned *length);

#endif

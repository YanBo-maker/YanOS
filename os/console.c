#include "console.h"

#include "platform.h"

/* SKELETON. The interface in os/console.h is frozen once
 * docs/specs/0017-console-and-os-layout.md is locked; the line-editing
 * behaviour below is owed by the console implementation task. Everything here
 * currently reports "no terminal", which is the honest answer while the
 * driver does not exist yet. */

int yan_os_console_connected(void)
{
    return yan_os_uart_connected();
}

YanOsResult yan_os_console_putc(char c)
{
    (void)c;
    return YAN_OS_UNAVAILABLE;
}

YanOsResult yan_os_console_puts(const char *text)
{
    (void)text;
    return YAN_OS_UNAVAILABLE;
}

YanOsResult yan_os_console_getline(char *buffer, unsigned capacity,
                                   unsigned *length)
{
    (void)buffer;
    (void)capacity;
    (void)length;
    return YAN_OS_UNAVAILABLE;
}

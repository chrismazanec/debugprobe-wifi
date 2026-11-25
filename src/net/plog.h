/* Small RAM event log (boot stages, WiFi, clients). Printed on the CDC port
 * when it is opened and the target UART has never sent a byte, and by the
 * config console command "log". Never contains the WiFi password. */
#ifndef PLOG_H_
#define PLOG_H_
#include <stddef.h>
#include <stdint.h>

#define PLOG_SIZE 3072u

void plog_init(void);
/* printf-style; one line, "\r\n" added. Not from ISRs. Uses ~500 B of stack. */
void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copy the log (oldest first) into out; returns bytes copied. */
size_t plog_read(char *out, size_t n);
#endif

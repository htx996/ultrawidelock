/* SPDX-License-Identifier: ISC */

/*
 * The test-facing half of dgram_host.c: what a suite needs to drive the sealed
 * link that ultrawidelock_dgram.h itself must not expose. Nothing in ports/ or
 * modules/ includes this; a production file that needed it would be reaching
 * for the radio's internals.
 */
#ifndef DGRAM_HOST_H
#define DGRAM_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Forget every captured datagram, close the link, clear the injected results. */
void dgram_host_reset(void);

/**
 * Hand a datagram to whoever opened the link, exactly as the OpenThread backend
 * would. False if the link is closed or the length is one the real backend
 * would have dropped -- in which case the callback did NOT run.
 */
bool dgram_host_deliver(const void *data, size_t len);

/** How many datagrams were sent, including any past the capture ring's depth. */
size_t dgram_host_sent_count(void);

/** The port the link was opened on, or 0 if it was never opened. */
uint16_t dgram_host_port(void);

/**
 * Copy captured datagram @p index into @p out. Returns its length, or 0 if
 * there is no such datagram or it does not fit @p cap.
 */
size_t dgram_host_sent(size_t index, void *out, size_t cap);

/* One-shot results for the next open() and send(); cleared once consumed. */
extern int dgram_host_open_rc;
extern int dgram_host_send_rc;

#ifdef __cplusplus
}
#endif

#endif /* DGRAM_HOST_H */

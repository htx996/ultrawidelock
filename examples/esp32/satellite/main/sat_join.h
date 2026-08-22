/* SPDX-License-Identifier: ISC */

/**
 * @file sat_join.h — join a ranging session from raw parameters.
 *
 * ONE path, whatever delivered the parameters: the console command and the
 * sealed link both land here. The link is meant to REPLACE the typed command,
 * and a join that behaved differently depending on how it arrived would make
 * bench results say nothing about the wireless path.
 */

#ifndef SAT_JOIN_H
#define SAT_JOIN_H

#include <stddef.h>
#include <stdint.h>

#define SAT_URSK_LEN 32u
#define SAT_RCFG_LEN 17u

/**
 * Configure and start the responder.
 *
 * @param sid_out session id actually configured, for the caller's log.
 * @param why receives a caller-printable reason on failure.
 * @return 0, or negative.
 */
int sat_join_apply(const uint8_t *ursk, const uint8_t *rcfg, uint8_t channel,
		   uint8_t sync_code_index, uint32_t *sid_out, const char **why);

/** Parse exactly 2*len hex chars into @p out. @return 0, or -1. */
int sat_hex_parse(const char *s, uint8_t *out, size_t len);

/** Register the `sat` console commands. */
void sat_console_register(void);

#endif /* SAT_JOIN_H */

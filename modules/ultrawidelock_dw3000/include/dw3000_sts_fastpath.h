/* SPDX-License-Identifier: ISC */

#ifndef DW3000_STS_FASTPATH_H
#define DW3000_STS_FASTPATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Write the four contiguous DW3000 STS key words in one SPI transaction. */
void ultrawidelock_dw3000_write_sts_key_bulk(const uint32_t words[4]);

/** Write the four contiguous DW3000 STS IV words in one SPI transaction. */
void ultrawidelock_dw3000_write_sts_iv_bulk(const uint32_t words[4]);

#ifdef __cplusplus
}
#endif

#endif /* DW3000_STS_FASTPATH_H */

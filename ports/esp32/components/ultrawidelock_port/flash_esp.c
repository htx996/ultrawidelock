/* SPDX-License-Identifier: ISC */

/*
 * flash_esp.c - the ultrawidelock_flash.h backend for ESP-IDF.
 *
 * Mirror of ports/zephyr/osal/flash_zephyr.c and
 * ports/freertos-nrf52833/board/flash_freertos.c. The receiver
 * (modules/ultrawidelock_dfu/src/dfu_receiver.c) compiles against this header
 * and nothing else, which is why it can be shared verbatim across three OSes.
 *
 *
 * THE PREFIX, which is the only interesting thing in this file.
 *
 * The staging contract in ultrawidelock_dfu.h is a partition laid out as:
 *
 *     page 0                the 32-byte header, written by the application
 *     page 1                the step log, appended by the BOOTLOADER
 *     page 2 onward         the payload
 *
 * That shape exists for MCUboot, which reads the header on the next boot,
 * applies the delta onto the primary slot and uses the step log to survive a
 * power cut mid-apply. ESP-IDF has none of that: it has two whole application
 * slots and an otadata partition, and the "apply" is a single word saying which
 * slot to boot next.
 *
 * So an ESP32 wants the payload at offset 0 of an OTA partition -- that is
 * where the bootloader expects an image to start -- while the receiver insists
 * on writing it at 8192. Rather than teach the receiver a second layout (and
 * carry that fork through its 7,546 host checks), this backend presents a
 * VIRTUAL area:
 *
 *     virtual [0, 8192)         the two bookkeeping pages, backed here
 *     virtual [8192, 8192+N)    the OTA partition, at its own offset 0
 *
 * The receiver's arithmetic is then correct by construction: patch_max() comes
 * out as exactly the OTA partition size, and the image lands where the ROM
 * bootloader looks for it.
 *
 * Page 1 is NOT backed. Nothing on this port writes a step log, because
 * nothing on this port applies a delta -- so a write into it is a bug in a
 * caller that thinks it is talking to MCUboot, and it is refused rather than
 * quietly absorbed. Page 0 is backed by 32 bytes of RAM rather than flash,
 * because the header's only reader here is the commit hook, which is handed the
 * parsed struct directly.
 */

#include <string.h>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "ultrawidelock_flash.h"
#include "ultrawidelock_log.h"

/* No level argument: this file lives in ultrawidelock_port and is compiled
 * whether or not the DFU component is in the image, so it must not reach for
 * that component's CONFIG_ULTRAWIDELOCK_DFU_LOG_LEVEL. The ESP-IDF branch of
 * ultrawidelock_log.h ignores the level anyway -- esp_log filters at runtime. */
LOG_MODULE_REGISTER(ultrawidelock_flash);

/* Must equal ULTRAWIDELOCK_DFU_PATCH_OFFSET. Not included from
 * ultrawidelock_dfu.h on purpose: this file is the flash contract, not the DFU
 * one, and the port component does not depend on the DFU module. The static
 * assert in dfu_esp32.c is what keeps the two honest. */
#define PREFIX_LEN 8192u
#define HDR_LEN    32u
#define PAGE_SIZE  4096u

struct ultrawidelock_flash_area {
	const esp_partition_t *part;
	/* Non-zero for staging: the virtual bookkeeping pages ahead of the
	 * partition. Zero for primary, which is the partition as it is. */
	uint32_t prefix;
	uint8_t hdr[HDR_LEN];
};

static struct ultrawidelock_flash_area s_staging;
static struct ultrawidelock_flash_area s_primary;

int ultrawidelock_flash_open(enum ultrawidelock_flash_area_id id,
			     const struct ultrawidelock_flash_area **fa)
{
	struct ultrawidelock_flash_area *area;

	if (fa == NULL) {
		return -1;
	}

	switch (id) {
	case ULTRAWIDELOCK_FLASH_AREA_STAGING:
		area = &s_staging;
		if (area->part == NULL) {
			/* The slot we are NOT running from. Deterministic for a
			 * given boot, which matters because the receiver opens
			 * this once and caches it for the life of the process. */
			area->part = esp_ota_get_next_update_partition(NULL);
			area->prefix = PREFIX_LEN;
			memset(area->hdr, 0xff, sizeof(area->hdr));
		}
		break;
	case ULTRAWIDELOCK_FLASH_AREA_PRIMARY:
		area = &s_primary;
		if (area->part == NULL) {
			area->part = esp_ota_get_running_partition();
			area->prefix = 0;
		}
		break;
	default:
		return -1;
	}

	if (area->part == NULL) {
		/* A single-app partition table has no second slot. The board
		 * cannot be updated over the air at all, and saying so here is
		 * what turns that into a refusal rather than a corrupted slot. */
		LOG_WRN("no OTA partition; this image cannot stage an update");
		return -1;
	}

	*fa = area;
	return 0;
}

void ultrawidelock_flash_close(const struct ultrawidelock_flash_area *fa)
{
	/* esp_partition_t handles are owned by the partition table, not by us.
	 * The areas are static and re-opened by pointer, so there is nothing to
	 * release -- same as the Zephyr backend. */
	(void)fa;
}

size_t ultrawidelock_flash_size(const struct ultrawidelock_flash_area *fa)
{
	if (fa == NULL || fa->part == NULL) {
		return 0;
	}
	return (size_t)fa->part->size + fa->prefix;
}

/**
 * Split a virtual offset into (prefix?, partition offset).
 *
 * @return 0 when the range lies wholly in the partition and @p part_off is set,
 *         1 when it lies wholly in the prefix, negative when it is out of
 *         bounds or straddles the boundary. Straddling is refused rather than
 *         split: every caller writes in aligned runs well inside one region,
 *         so a straddle means an offset was computed wrong and silently
 *         handling it would hide that.
 */
static int split(const struct ultrawidelock_flash_area *fa, uint32_t off, size_t len,
		 uint32_t *part_off)
{
	size_t total = ultrawidelock_flash_size(fa);

	if (off > total || len > total - off) {
		return -1;
	}
	if (off >= fa->prefix) {
		*part_off = off - fa->prefix;
		return 0;
	}
	if (len > (size_t)(fa->prefix - off)) {
		return -2;
	}
	return 1;
}

int ultrawidelock_flash_read(const struct ultrawidelock_flash_area *fa, uint32_t off, void *dst,
			     size_t len)
{
	uint32_t part_off = 0;
	int where;

	if (fa == NULL || fa->part == NULL || dst == NULL) {
		return -1;
	}
	if (len == 0U) {
		return 0;
	}

	where = split(fa, off, len, &part_off);
	if (where < 0) {
		return -1;
	}
	if (where == 1) {
		/* Page 0 comes back as written; the rest of the prefix is the
		 * step log, which this port never writes, so it reads as erased
		 * flash would. */
		uint8_t *out = dst;

		for (size_t i = 0; i < len; i++) {
			uint32_t at = off + (uint32_t)i;

			out[i] = (at < HDR_LEN) ? fa->hdr[at] : 0xff;
		}
		return 0;
	}

	return (esp_partition_read(fa->part, part_off, dst, len) == ESP_OK) ? 0 : -1;
}

int ultrawidelock_flash_write(const struct ultrawidelock_flash_area *fa, uint32_t off,
			      const void *src, size_t len)
{
	uint32_t part_off = 0;
	int where;

	if (fa == NULL || fa->part == NULL || src == NULL) {
		return -1;
	}
	if (len == 0U) {
		return 0;
	}
	/* The contract's alignment rule, enforced rather than assumed -- the
	 * host backend enforces it too, and the receiver's write combiner exists
	 * only to satisfy it. A backend that quietly accepted an unaligned write
	 * would let that combiner rot until a real board rejected it. */
	if (((off | (uint32_t)len) & 3U) != 0U) {
		return -1;
	}

	where = split(fa, off, len, &part_off);
	if (where < 0) {
		return -1;
	}
	if (where == 1) {
		if (off + len > HDR_LEN) {
			/* Page 1 is MCUboot's step log. Nothing on ESP-IDF
			 * applies a delta, so nothing should be writing one. */
			LOG_WRN("write into the unbacked staging prefix at %u", (unsigned)off);
			return -1;
		}
		/* s_staging, not fa->hdr: fa is const, and this branch is only
		 * reachable for an area with a non-zero prefix, which is staging
		 * and nothing else. The switch in _open is what guarantees that. */
		memcpy(&s_staging.hdr[off], src, len);
		return 0;
	}

	return (esp_partition_write(fa->part, part_off, src, len) == ESP_OK) ? 0 : -1;
}

int ultrawidelock_flash_erase(const struct ultrawidelock_flash_area *fa, uint32_t off, size_t len)
{
	uint32_t part_off = 0;
	size_t total;

	if (fa == NULL || fa->part == NULL) {
		return -1;
	}
	if (len == 0U) {
		return 0;
	}
	if (((off | (uint32_t)len) & (PAGE_SIZE - 1U)) != 0U) {
		return -1;
	}

	total = ultrawidelock_flash_size(fa);
	if (off > total || len > total - off) {
		return -1;
	}

	/* Unlike read and write, an erase legitimately spans the boundary: the
	 * receiver erases the whole area in one call. Reset the prefix and pass
	 * the remainder down. */
	if (off < fa->prefix) {
		size_t in_prefix = (size_t)(fa->prefix - off);

		memset(s_staging.hdr, 0xff, sizeof(s_staging.hdr));
		if (len <= in_prefix) {
			return 0;
		}
		len -= in_prefix;
		part_off = 0;
	} else {
		part_off = off - fa->prefix;
	}

	return (esp_partition_erase_range(fa->part, part_off, len) == ESP_OK) ? 0 : -1;
}

void ultrawidelock_reboot(void)
{
	esp_restart();
}

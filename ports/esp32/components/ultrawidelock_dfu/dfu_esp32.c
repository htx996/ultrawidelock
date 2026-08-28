/* SPDX-License-Identifier: ISC */

/*
 * dfu_esp32.c - what the portable DFU receiver cannot know on ESP-IDF.
 *
 * The receiver (modules/ultrawidelock_dfu/src/dfu_receiver.c) is shared
 * verbatim with the DWM3001CDK and the standalone FreeRTOS port. It takes
 * frames, checks a P-256 signature over the header, writes the payload into
 * the staging area and reboots. On the CDK that is the whole job, because
 * MCUboot reads the staging partition on the next boot and applies it.
 *
 * ESP-IDF has no such bootloader. Its ROM bootloader reads `otadata` to decide
 * which of two application slots to boot, and only the application can write
 * otadata. So an image staged into the spare slot with nothing else done is
 * simply ignored: the board reboots into what it was already running, and the
 * update silently does not happen while every layer reports success.
 *
 * This file is the missing step. It installs the commit hook the receiver
 * calls once the bytes have proven themselves, and that hook points the
 * bootloader at the slot that was just written.
 *
 *
 * WHY A WHOLE IMAGE AND NOT A DELTA. The CDK carries one MCUboot slot in 512 KB
 * and cannot hold two, so what travels to it is an ~11 KB signed delta. The
 * ESP32 has two full slots by construction -- that is what ota_0/ota_1 are --
 * so a delta would buy nothing and cost a detools applier, a step log and a
 * power-cut resume path, all to avoid sending bytes down a radio that is
 * already going to send 2 MB of them. The container is the same either way;
 * ULTRAWIDELOCK_DFU_FLAG_FULL_IMAGE in the signed header is what says which.
 */

#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "ultrawidelock_dfu.h"
#include "ultrawidelock_dfu_esp32.h"
#include "ultrawidelock_dfu_rx.h"
#include "ultrawidelock_log.h"

LOG_MODULE_REGISTER(ultrawidelock_dfu_esp, CONFIG_ULTRAWIDELOCK_DFU_LOG_LEVEL);

/*
 * flash_esp.c presents the OTA partition behind a virtual prefix so that the
 * receiver's fixed 8192-byte payload offset lands at offset 0 of the partition,
 * which is where the ROM bootloader looks for an image. The two constants have
 * to agree exactly or the image is written 8 KB into the slot and the board
 * boots nothing. They live in different components -- the flash contract is not
 * the DFU contract -- so this is where they are tied together.
 */
_Static_assert(ULTRAWIDELOCK_DFU_PATCH_OFFSET == 8192u,
	       "flash_esp.c's PREFIX_LEN is 8192 and must equal the payload offset");

/**
 * Point the bootloader at the slot the receiver just filled.
 *
 * Runs from the receiver's COMMIT path, after magic, ABI, header CRC, payload
 * length and payload CRC-32 have all passed, and after the P-256 signature over
 * the header was verified back when the first 96 bytes arrived. Returning
 * non-zero fails the COMMIT and leaves nothing staged.
 */
static int esp_commit(const struct ultrawidelock_dfu_hdr *hdr)
{
	const esp_partition_t *target;
	esp_err_t err;

	/*
	 * A delta would be written into an OTA slot as though it were an image.
	 * esp_ota_set_boot_partition() would reject it a moment later for
	 * failing image validation, so this check is not what makes it safe --
	 * it is what makes the failure say the true thing. The flag is inside
	 * the signed 32 bytes, so nobody without the release key can flip it.
	 */
	if ((hdr->flags & ULTRAWIDELOCK_DFU_FLAG_FULL_IMAGE) == 0U) {
		LOG_WRN("refused: this is a delta, and this port applies whole images");
		return -1;
	}

	target = esp_ota_get_next_update_partition(NULL);
	if (target == NULL) {
		LOG_WRN("refused: no second OTA slot in this partition table");
		return -1;
	}

	/*
	 * Validates the image before it changes anything: magic byte, segment
	 * table, and the checksum the ROM bootloader itself will check. A slot
	 * full of plausible bytes that is not a bootable image is caught here,
	 * on the board that still has a working image, rather than at the next
	 * boot on a board that no longer does.
	 */
	err = esp_ota_set_boot_partition(target);
	if (err != ESP_OK) {
		LOG_WRN("refused: esp_ota_set_boot_partition rc=%d", (int)err);
		return -1;
	}

	LOG_INF("staged %u B into %s; next boot runs it", (unsigned)hdr->patch_len,
		target->label);
	return 0;
}

void ultrawidelock_dfu_esp32_init(void)
{
	ultrawidelock_dfu_set_commit_cb(esp_commit);
}

void ultrawidelock_dfu_esp32_open_window(uint32_t duration_ms)
{
	ultrawidelock_dfu_window_open(duration_ms);
}

int ultrawidelock_dfu_esp32_confirm(void)
{
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_img_states_t state;

	/*
	 * Only meaningful with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. Without
	 * it every image is already marked valid and this is a no-op, which is
	 * why it is safe to call unconditionally from app start-up.
	 *
	 * With it, an image that reaches this line has booted, brought up its
	 * radios and reached the point the caller considers healthy -- so it is
	 * allowed to keep running. An image that crashes before it gets here is
	 * rolled back to the previous slot by the bootloader on the next boot,
	 * which is the only thing standing between a bad update and a door lock
	 * that no longer answers.
	 */
	if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
		return 0;
	}
	if (state != ESP_OTA_IMG_PENDING_VERIFY) {
		return 0;
	}
	if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
		LOG_WRN("could not confirm this image; it will roll back at the next boot");
		return -1;
	}
	LOG_INF("this image is confirmed good; no rollback pending");
	return 0;
}

/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief Application half of the delta update: receive, verify, stage, reboot.
 *
 * Never applies anything. The patch is written into `patch_staging` and the
 * board is restarted; MCUboot does the work, because the application executes
 * from the slot the patch rewrites (see src/dfu_applier.c).
 *
 * WHAT ARRIVES, in order, as one byte stream over whatever transport:
 *
 *     0   32   struct ultrawidelock_dfu_hdr
 *    32   64   ECDSA-P256 signature, raw r||s, over those 32 bytes
 *    96   ..   the patch
 *
 * The header is written to flash LAST, after the patch's CRC checks out, so a
 * cut-off transfer leaves no valid magic and the next boot ignores it. The
 * signature is checked HERE, not in the bootloader (this image already links
 * PSA ECDSA-P256; MCUboot is the flash-starved one), and the floor sits under
 * both: CONFIG_BOOT_VALIDATE_SLOT0 re-verifies the RESULT before booting, so a
 * forged header can only destroy the installed image, never install code.
 */

#include <psa/crypto.h>
#include <errno.h>
#include <string.h>

#include "dfu_crc.h"
#include "ultrawidelock_bytes.h"
#include "ultrawidelock_dfu.h"
#include "ultrawidelock_dfu_rx.h"
#include "ultrawidelock_flash.h"
#include "ultrawidelock_log.h"
#include "ultrawidelock_osal.h"
#include "ultrawidelock_port.h"

LOG_MODULE_REGISTER(ultrawidelock_dfu, CONFIG_ULTRAWIDELOCK_DFU_LOG_LEVEL);

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/** Generated at build time from the MCUboot signing key. 0x04 || X || Y. */
extern const uint8_t ultrawidelock_dfu_pubkey[65];

/** Bytes of preamble ahead of the patch: the header and its signature. */
#define HEAD_LEN (ULTRAWIDELOCK_DFU_HDR_LEN + ULTRAWIDELOCK_DFU_SIG_LEN)

/** Flash write staging. Multiple of the 4-byte write block. */
#define WBUF_SZ 64

static struct {
	bool active;
	bool erased;
	bool staged;
	enum ultrawidelock_dfu_owner owner;
	uint32_t transfer_id;
	uint32_t total; /**< wire bytes the host promised */
	uint32_t got;   /**< wire bytes consumed so far */
	uint32_t patch_crc;
	uint32_t wpos; /**< next staging offset for patch data */
	uint8_t head[HEAD_LEN];
	uint8_t wbuf[WBUF_SZ];
	size_t wlen;
} s_rx;

static const struct ultrawidelock_flash_area *s_fa;
static ultrawidelock_mutex_t s_rx_lock;
#ifdef CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG
/* SMP client-space cursor and optional MCUboot wrapper geometry. */
static uint32_t s_cli_off;
static uint32_t s_skip;
static uint32_t s_inner;
#endif

/* Replying before rebooting is the whole reason this is deferred: a reboot
 * inside the frame handler drops the acknowledgement the host is waiting for,
 * and the host cannot then tell success from a dead board. */
static void reboot_fn(struct ultrawidelock_dwork *dwork)
{
	(void)dwork;
	LOG_INF("update staged, restarting into the bootloader");
	ultrawidelock_reboot();
}
static struct ultrawidelock_dwork s_reboot;

/* ---- window --------------------------------------------------------------- */

static void window_expire(struct ultrawidelock_dwork *dwork);
static struct ultrawidelock_dwork s_window;
static bool s_open;
static int64_t s_window_deadline_ms;
static ultrawidelock_dfu_window_cb s_window_cb;

/* Both delayable items are bound on the first window call, which every route
 * into the receiver goes through -- frames and SMP uploads are refused while
 * the window has never opened. */
static void works_bind(void)
{
	static bool bound;

	if (!bound) {
		ultrawidelock_mutex_init(&s_rx_lock);
		ultrawidelock_dwork_init(&s_reboot, reboot_fn);
		ultrawidelock_dwork_init(&s_window, window_expire);
		bound = true;
	}
}

/**
 * Register a callback to be invoked when the update window opens or closes.
 */
void ultrawidelock_dfu_set_window_cb(ultrawidelock_dfu_window_cb cb)
{
	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	s_window_cb = cb;
	ultrawidelock_mutex_unlock(&s_rx_lock);
}

/* One place, so that every route in -- the button, Apple Home, the bench SWD
 * write -- reaches the indicator without knowing it exists. */
static void window_notify(bool open)
{
	ultrawidelock_dfu_window_cb cb;

	ultrawidelock_mutex_lock(&s_rx_lock);
	cb = s_window_cb;
	ultrawidelock_mutex_unlock(&s_rx_lock);
	if (cb != NULL) {
		cb(open);
	}
}

/**
 * Mark the update window closed, reset RX state, and notify all listeners (typically the UI) that
 * the window is no longer open.
 */
static void rx_state_reset_locked(void)
{
	memset(&s_rx, 0, sizeof(s_rx));
}

#ifdef CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG
static void smp_cursor_reset_locked(void)
{
	s_cli_off = 0u;
	s_skip = 0u;
	s_inner = 0u;
}
#endif

static void rx_reset_all_locked(void)
{
	rx_state_reset_locked();
#ifdef CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG
	smp_cursor_reset_locked();
#endif
}

static void window_expire(struct ultrawidelock_dwork *dwork)
{
	int64_t remaining_ms;

	(void)dwork;
	ultrawidelock_mutex_lock(&s_rx_lock);
	if (!s_open) {
		ultrawidelock_mutex_unlock(&s_rx_lock);
		return;
	}
#if defined(ULTRAWIDELOCK_PORT_HOST)
	/* The deterministic host scheduler has its own virtual clock and removes a
	 * rescheduled item atomically, so an old callback cannot overlap here. */
	remaining_ms = 0;
#else
	remaining_ms = s_window_deadline_ms - ultrawidelock_uptime_ms();
#endif
	if (remaining_ms > 0) {
		/* A callback from the previous deadline can already be running when
		 * the owner extends the window. It must not close the new window. */
		(void)ultrawidelock_dwork_reschedule(
			&s_window, remaining_ms > INT32_MAX ? INT32_MAX : (int32_t)remaining_ms);
		ultrawidelock_mutex_unlock(&s_rx_lock);
		return;
	}
	LOG_INF("update window closed");
	s_open = false;
	s_window_deadline_ms = 0;
	rx_reset_all_locked();
	ultrawidelock_mutex_unlock(&s_rx_lock);
	window_notify(false);
}

/**
 * Open the update window for the given duration in milliseconds. Reschedule the close timer and
 * notify all window listeners.
 */
void ultrawidelock_dfu_window_open(uint32_t duration_ms)
{
	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	s_open = true;
	s_window_deadline_ms = ultrawidelock_uptime_ms() + duration_ms;
	(void)ultrawidelock_dwork_reschedule(
		&s_window, duration_ms > INT32_MAX ? INT32_MAX : (int32_t)duration_ms);
	ultrawidelock_mutex_unlock(&s_rx_lock);
	LOG_INF("update window open for %u ms", (unsigned)duration_ms);
	window_notify(true);
}

/**
 * Cancel the update window timer, mark it closed, reset RX state, and notify all listeners that the
 * window is no longer open.
 */
void ultrawidelock_dfu_window_close(void)
{
	works_bind();
	(void)ultrawidelock_dwork_cancel(&s_window);
	ultrawidelock_mutex_lock(&s_rx_lock);
	s_open = false;
	s_window_deadline_ms = 0;
	rx_reset_all_locked();
	ultrawidelock_mutex_unlock(&s_rx_lock);
	window_notify(false);
}

/**
 * Return true if the update window is currently open.
 */
bool ultrawidelock_dfu_window_is_open(void)
{
	bool open;

	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	open = s_open;
	ultrawidelock_mutex_unlock(&s_rx_lock);
	return open;
}

/* ---- staging flash -------------------------------------------------------- */

/**
 * Open the staging flash area if not already open. Return 0 on success or if already open; nonzero
 * on error.
 */
static int staging_open(void)
{
	if (s_fa != NULL) {
		return 0;
	}
	return ultrawidelock_flash_open(ULTRAWIDELOCK_FLASH_AREA_STAGING, &s_fa);
}

/** Room for patch bytes, after the header page and the step-log page. */
static uint32_t patch_max(void)
{
	return (uint32_t)ultrawidelock_flash_size(s_fa) - ULTRAWIDELOCK_DFU_PATCH_OFFSET;
}

/**
 * Flush buffered patch data to the staging flash area, padding to 4-byte alignment if final. Return
 * 0 on success, -1 on write error. Updates write position and shifts remaining bytes.
 */
static int wbuf_flush(bool final)
{
	size_t n = s_rx.wlen & ~(size_t)3;

	if (final) {
		while (s_rx.wlen & 3U) {
			s_rx.wbuf[s_rx.wlen++] = 0xff;
		}
		n = s_rx.wlen;
	}
	if (n == 0U) {
		return 0;
	}
	if (ultrawidelock_flash_write(s_fa, s_rx.wpos, s_rx.wbuf, n) != 0) {
		return -1;
	}

	s_rx.wpos += (uint32_t)n;
	s_rx.wlen -= n;
	if (s_rx.wlen > 0U) {
		memmove(s_rx.wbuf, s_rx.wbuf + n, s_rx.wlen);
	}
	return 0;
}

/**
 * Buffer patch data, updating the running CRC32, and flush to flash when the buffer is full. Return
 * 0 on success or nonzero on flush failure.
 */
static int patch_write(const uint8_t *data, size_t len)
{
	s_rx.patch_crc = ultrawidelock_crc32_update(s_rx.patch_crc, data, len);

	while (len > 0U) {
		size_t n = MIN(len, WBUF_SZ - s_rx.wlen);

		memcpy(s_rx.wbuf + s_rx.wlen, data, n);
		s_rx.wlen += n;
		data += n;
		len -= n;

		if (s_rx.wlen == WBUF_SZ && wbuf_flush(false) != 0) {
			return -1;
		}
	}
	return 0;
}

/* ---- authenticity --------------------------------------------------------- */

/**
 * Verify the DFU header signature using ECDSA-SHA256 with the built-in public key. Return true if
 * the signature is valid.
 */
static bool head_verifies(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_status_t st;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	st = psa_import_key(&attr, ultrawidelock_dfu_pubkey, sizeof(ultrawidelock_dfu_pubkey), &key);
	if (st != PSA_SUCCESS) {
		LOG_ERR("pubkey import failed (%d)", (int)st);
		return false;
	}

	st = psa_verify_message(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), s_rx.head, ULTRAWIDELOCK_DFU_HDR_LEN,
				s_rx.head + ULTRAWIDELOCK_DFU_HDR_LEN, ULTRAWIDELOCK_DFU_SIG_LEN);
	(void)psa_destroy_key(key);

	return st == PSA_SUCCESS;
}

/* ---- frame handling ------------------------------------------------------- */

/**
 * Reset the receiver state to empty: clear the RX struct.
 */
void ultrawidelock_dfu_rx_reset_all(void)
{
	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	rx_reset_all_locked();
	ultrawidelock_mutex_unlock(&s_rx_lock);
}

void ultrawidelock_dfu_rx_reset(enum ultrawidelock_dfu_owner owner)
{
	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	if (owner != ULTRAWIDELOCK_DFU_OWNER_NONE && s_rx.owner == owner) {
		rx_reset_all_locked();
	}
	ultrawidelock_mutex_unlock(&s_rx_lock);
}

static int rx_erase_locked(enum ultrawidelock_dfu_owner owner)
{
	if (!s_open) {
		return -EACCES;
	}
	if (s_rx.owner != ULTRAWIDELOCK_DFU_OWNER_NONE && s_rx.owner != owner) {
		return -EBUSY;
	}
	if (staging_open() != 0 ||
	    ultrawidelock_flash_erase(s_fa, 0, ultrawidelock_flash_size(s_fa)) != 0) {
		return -EIO;
	}
	rx_reset_all_locked();
	return 0;
}

int ultrawidelock_dfu_rx_erase(enum ultrawidelock_dfu_owner owner)
{
	int rc;

	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	rc = rx_erase_locked(owner);
	ultrawidelock_mutex_unlock(&s_rx_lock);
	return rc;
}

/**
 * Write a ULTRAWIDELOCK_DFU_RSP_OK response: set opcode to OK, append the byte count received as
	 * little-endian 32-bit. Return 9 (response size).
 */
static size_t reply_ok(uint8_t *rsp, uint32_t transfer_id)
{
	rsp[0] = ULTRAWIDELOCK_DFU_RSP_OK;
	sys_put_le32(transfer_id, &rsp[1]);
	sys_put_le32(s_rx.got, &rsp[5]);
	return 9u;
}

/**
 * Encode a two-byte error response with the given error code. Return the
 * response length (2 bytes).
 */
static size_t reply_err(uint8_t *rsp, enum ultrawidelock_dfu_err code)
{
	rsp[0] = ULTRAWIDELOCK_DFU_RSP_ERR;
	rsp[1] = (uint8_t)code;
	return 2u;
}

/* The three cores below carry the whole state machine, and they are shared:
 * the framed protocol wraps them in opcodes, and the SMP image group
 * (src/dfu_smp_img.c) drives the same three from CBOR. Neither transport holds
 * any transfer state of its own, so there is one place where a patch can be
 * accepted and one set of checks in front of it. ULTRAWIDELOCK_DFU_ERR_OK (0) means
 * accepted; every other value is refused. */
#define ULTRAWIDELOCK_DFU_ERR_OK ((enum ultrawidelock_dfu_err)0)

/**
 * Validate the patch size and prepare the receiver
 * to accept upload: initialize RX state with the write position at the patch offset and mark
 * reception active. Return an error code.
 */
static enum ultrawidelock_dfu_err begin_at(enum ultrawidelock_dfu_owner owner,
					    uint32_t transfer_id, uint32_t total)
{
	if (owner == ULTRAWIDELOCK_DFU_OWNER_NONE || transfer_id == 0U) {
		return ULTRAWIDELOCK_DFU_ERR_MALFORMED;
	}
	/*
	 * THIS CLAIM IS TAKEN BEFORE ANYTHING IS AUTHENTICATED, and that is an
	 * accepted risk rather than an oversight. The signature over the header is
	 * only checked once HEAD_LEN bytes have arrived, so whoever sends BEGIN
	 * first holds the receiver until they disconnect or the window shuts.
	 *
	 * Weighed and kept, on four counts:
	 *
	 *   - The window is owner-gated. s_open is false until SW2 or the Matter
	 *     commissioning window opens it (matter_commission.c), so there is
	 *     nothing to claim outside a gesture the owner just made.
	 *   - It cannot install anything. head_verifies() still checks the
	 *     ECDSA-P256 signature, and commit_now() re-checks magic, ABI, CRCs
	 *     and lengths. This is denial of an update, never a forged one.
	 *   - CONFIG_BT_MAX_CONN=1. A peer that can send BEGIN has already taken
	 *     the board's only connection slot, which denies the flasher, the
	 *     phone and everything else regardless of what DFU does. The claim
	 *     adds no exposure the connection slot has not already given away.
	 *   - The obvious fix is barred. Requiring an encrypted link would mean
	 *     BT_GATT_PERM_WRITE_ENCRYPT and therefore pairing, and the reader
	 *     deliberately never asks a phone to pair -- the live-iPhone unlock
	 *     depends on it (apps/dwm3001cdk-lock/prj.conf, CONFIG_BT_SMP block).
	 *
	 * The claim is released on transport disconnect via
	 * ultrawidelock_dfu_rx_reset(), and window_expire() clears everything when
	 * the window shuts, so it cannot outlive either.
	 *
	 * REVISIT IF CONFIG_BT_MAX_CONN EVER EXCEEDS 1. That is the load-bearing
	 * leg above: with two connection slots a second peer can claim the
	 * receiver without denying the first, and the argument stops holding. The
	 * fix that needs no pairing is to preempt a claim that has taken no DATA
	 * bytes after an idle timeout.
	 */
	if (s_rx.owner != ULTRAWIDELOCK_DFU_OWNER_NONE) {
		if (s_rx.owner == owner && s_rx.transfer_id == transfer_id && s_rx.total == total) {
			return ULTRAWIDELOCK_DFU_ERR_OK;
		}
		return ULTRAWIDELOCK_DFU_ERR_BUSY;
	}
	if (staging_open() != 0) {
		return ULTRAWIDELOCK_DFU_ERR_FLASH;
	}
	if (total <= HEAD_LEN || (total - HEAD_LEN) > patch_max()) {
		return ULTRAWIDELOCK_DFU_ERR_SIZE;
	}

	rx_state_reset_locked();
	s_rx.active = true;
	s_rx.owner = owner;
	s_rx.transfer_id = transfer_id;
	s_rx.total = total;
	s_rx.wpos = ULTRAWIDELOCK_DFU_PATCH_OFFSET;
	LOG_INF("update begun, %u B", (unsigned)total);
	return ULTRAWIDELOCK_DFU_ERR_OK;
}

static enum ultrawidelock_dfu_err feed_bytes(const uint8_t *p, size_t n)
{
	if (!s_rx.active) {
		return ULTRAWIDELOCK_DFU_ERR_SEQUENCE;
	}
	if (s_rx.got + n > s_rx.total) {
		return ULTRAWIDELOCK_DFU_ERR_SIZE;
	}

	/* Preamble first, then everything else is patch. */
	if (s_rx.got < HEAD_LEN) {
		size_t take = MIN(n, HEAD_LEN - s_rx.got);

		memcpy(s_rx.head + s_rx.got, p, take);
		s_rx.got += take;
		p += take;
		n -= take;

		if (s_rx.got == HEAD_LEN) {
			if (!head_verifies()) {
				LOG_WRN("rejected: header signature did not verify");
				return ULTRAWIDELOCK_DFU_ERR_AUTH;
			}

			/* Authenticate before the first destructive operation. A random
			 * nearby peer can no longer consume an erase cycle with BEGIN. */
			if (ultrawidelock_flash_erase(s_fa, 0, ultrawidelock_flash_size(s_fa)) != 0) {
				return ULTRAWIDELOCK_DFU_ERR_FLASH;
			}
			s_rx.erased = true;
		}
	}

	if (n > 0U) {
		if (patch_write(p, n) != 0) {
			return ULTRAWIDELOCK_DFU_ERR_FLASH;
		}
		s_rx.got += n;
	}

	return ULTRAWIDELOCK_DFU_ERR_OK;
}

/* @p reboot is false only for SMP, where the host sends its own reset command
 * afterwards and a board that restarted on its own would look like a failure. */
static enum ultrawidelock_dfu_err commit_now(bool reboot)
{
	struct ultrawidelock_dfu_hdr hdr;

	if (s_rx.staged) {
		/* A COMMIT whose ack was lost gets retried. The header is already
		 * written, so there is nothing to re-validate -- but returning OK
		 * without re-arming the reboot leaves the host waiting forever for
		 * an update it has in fact delivered. Scheduling is a no-op while
		 * the work is armed, so the original deadline still stands and a
		 * host repeating COMMIT cannot push the reboot out. */
		if (reboot) {
			(void)ultrawidelock_dwork_schedule(&s_reboot, 500);
		}
		return ULTRAWIDELOCK_DFU_ERR_OK;
	}
	if (!s_rx.active || !s_rx.erased || s_rx.got != s_rx.total) {
		return ULTRAWIDELOCK_DFU_ERR_SEQUENCE;
	}
	if (wbuf_flush(true) != 0) {
		return ULTRAWIDELOCK_DFU_ERR_FLASH;
	}

	memcpy(&hdr, s_rx.head, sizeof(hdr));

	if (hdr.magic != ULTRAWIDELOCK_DFU_MAGIC || hdr.abi_version != ULTRAWIDELOCK_DFU_ABI_VERSION ||
	    hdr.hdr_crc32 != ultrawidelock_crc32(s_rx.head, ULTRAWIDELOCK_DFU_HDR_CRC_LEN) ||
	    hdr.patch_len != (s_rx.total - HEAD_LEN) || hdr.patch_crc32 != s_rx.patch_crc) {
		LOG_WRN("rejected: staged bytes do not match the header");
		return ULTRAWIDELOCK_DFU_ERR_INTEGRITY;
	}

	/* The header goes in LAST. Until this write lands there is no magic in
	 * the staging partition and the bootloader has nothing to act on, so an
	 * interrupted transfer is indistinguishable from no transfer. */
	if (ultrawidelock_flash_write(s_fa, ULTRAWIDELOCK_DFU_HDR_OFFSET, s_rx.head,
			    ULTRAWIDELOCK_DFU_HDR_LEN) != 0) {
		return ULTRAWIDELOCK_DFU_ERR_FLASH;
	}

	s_rx.active = false;
	s_rx.staged = true;
	if (reboot) {
		(void)ultrawidelock_dwork_schedule(&s_reboot, 500);
	}
	return ULTRAWIDELOCK_DFU_ERR_OK;
}

static size_t do_begin(enum ultrawidelock_dfu_owner owner, const uint8_t *frame, size_t len,
		       uint8_t *rsp)
{
	enum ultrawidelock_dfu_err e;
	uint32_t transfer_id;

	if (len != 9u) {
		return reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_MALFORMED);
	}
	transfer_id = sys_get_le32(&frame[1]);
	e = begin_at(owner, transfer_id, sys_get_le32(&frame[5]));
	return (e == ULTRAWIDELOCK_DFU_ERR_OK) ? reply_ok(rsp, transfer_id) : reply_err(rsp, e);
}

static size_t do_data(enum ultrawidelock_dfu_owner owner, const uint8_t *frame, size_t len,
		      uint8_t *rsp)
{
	enum ultrawidelock_dfu_err e;
	uint32_t transfer_id;
	uint32_t offset;

	if (len < 9u) {
		return reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_MALFORMED);
	}
	transfer_id = sys_get_le32(&frame[1]);
	offset = sys_get_le32(&frame[5]);
	if (s_rx.owner != owner || s_rx.transfer_id != transfer_id) {
		return reply_err(rsp, s_rx.owner == ULTRAWIDELOCK_DFU_OWNER_NONE ?
					       ULTRAWIDELOCK_DFU_ERR_SEQUENCE : ULTRAWIDELOCK_DFU_ERR_BUSY);
	}
	/* Any stale or future offset is a non-destructive resynchronisation. This
	 * makes a retry after a lost GATT notification or CoC SDU idempotent. */
	if (offset != s_rx.got) {
		return reply_ok(rsp, transfer_id);
	}
	e = feed_bytes(&frame[9], len - 9u);
	if (e != ULTRAWIDELOCK_DFU_ERR_OK) {
		rx_reset_all_locked();
		return reply_err(rsp, e);
	}
	return reply_ok(rsp, transfer_id);
}

static size_t do_commit(enum ultrawidelock_dfu_owner owner, const uint8_t *frame, size_t len,
			uint8_t *rsp)
{
	enum ultrawidelock_dfu_err e;
	uint32_t transfer_id;

	if (len != 5u) {
		return reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_MALFORMED);
	}
	transfer_id = sys_get_le32(&frame[1]);
	if (s_rx.owner != owner || s_rx.transfer_id != transfer_id) {
		return reply_err(rsp, s_rx.owner == ULTRAWIDELOCK_DFU_OWNER_NONE ?
					       ULTRAWIDELOCK_DFU_ERR_SEQUENCE : ULTRAWIDELOCK_DFU_ERR_BUSY);
	}
	e = commit_now(true);
	if (e != ULTRAWIDELOCK_DFU_ERR_OK) {
		rx_reset_all_locked();
		return reply_err(rsp, e);
	}
	return reply_ok(rsp, transfer_id);
}

int ultrawidelock_dfu_rx_frame(enum ultrawidelock_dfu_owner owner, const uint8_t *frame, size_t len,
			      uint8_t *rsp, size_t *rsp_len)
{
	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	if (len < 1u) {
		*rsp_len = reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_MALFORMED);
		ultrawidelock_mutex_unlock(&s_rx_lock);
		return 0;
	}
	if (!s_open) {
		*rsp_len = reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_CLOSED);
		ultrawidelock_mutex_unlock(&s_rx_lock);
		return 0;
	}

	switch (frame[0]) {
	case ULTRAWIDELOCK_DFU_OP_BEGIN:
		*rsp_len = do_begin(owner, frame, len, rsp);
		break;
	case ULTRAWIDELOCK_DFU_OP_DATA:
		*rsp_len = do_data(owner, frame, len, rsp);
		break;
	case ULTRAWIDELOCK_DFU_OP_COMMIT:
		*rsp_len = do_commit(owner, frame, len, rsp);
		break;
	case ULTRAWIDELOCK_DFU_OP_ABORT:
		if (len != 5u || s_rx.owner != owner || s_rx.transfer_id != sys_get_le32(&frame[1])) {
			*rsp_len = reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_SEQUENCE);
			break;
		}
		{
			uint32_t transfer_id = s_rx.transfer_id;
			int rc = rx_erase_locked(owner);

			*rsp_len = (rc == 0) ? reply_ok(rsp, transfer_id) :
						 reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_FLASH);
		}
		break;
	default:
		*rsp_len = reply_err(rsp, ULTRAWIDELOCK_DFU_ERR_MALFORMED);
		break;
	}

	ultrawidelock_mutex_unlock(&s_rx_lock);
	return 0;
}

#ifdef CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG

/* MCUboot's image magic and the two header fields needed to measure a wrapper.
 * Also spelled out in src/dfu_smp_img.c, which walks the running image's TLVs;
 * this is MCUboot's fixed on-disk format and cannot drift. */
#define MCUBOOT_IMAGE_MAGIC 0x96f3b83du
#define MCUBOOT_HDR_MIN     16u

/* The CLIENT's cursor, which stops matching the patch cursor the moment a file
 * is wrapped. Everything reported back to the host is in this space; s_rx.got
 * remains the cursor into the patch itself. */
static int rx_upload_locked(uint32_t off, uint32_t total, const uint8_t *data, size_t len,
			    uint32_t *next)
{
	enum ultrawidelock_dfu_err e;

	if (!s_open) {
		return -EACCES;
	}
	if (s_rx.owner != ULTRAWIDELOCK_DFU_OWNER_NONE &&
	    s_rx.owner != ULTRAWIDELOCK_DFU_OWNER_SMP) {
		return -EBUSY;
	}
	if (off == 0U && s_rx.owner == ULTRAWIDELOCK_DFU_OWNER_SMP && s_cli_off > 0U) {
		*next = s_cli_off;
		return 0;
	}

	if (off == 0U) {
		/* A restarted upload is normal, not an error: mcumgr clients
			 * rewind to 0 after a dropped link. The cursor check above turns
			 * an already accepted first chunk into a resynchronisation. */
		s_skip = 0U;
		s_inner = total;

		/* A phone will not offer a file its own parser rejects, so a
		 * patch meant for one is dressed as an MCUboot image by
		 * `ultrawidelock_patch.py wrap`. Step over that wrapper. A raw .wdfu has
		 * no magic here and is taken as it is, so both files work and
		 * the native transport is untouched. */
		if (len >= MCUBOOT_HDR_MIN && sys_get_le32(data) == MCUBOOT_IMAGE_MAGIC) {
			uint32_t hdr_sz = sys_get_le16(&data[8]);
			uint32_t img_sz = sys_get_le32(&data[12]);

			if (hdr_sz < MCUBOOT_HDR_MIN || img_sz == 0U || hdr_sz > total ||
			    img_sz > total - hdr_sz) {
				e = ULTRAWIDELOCK_DFU_ERR_MALFORMED;
				goto refused;
			}
			s_skip = hdr_sz;
			s_inner = img_sz;
			LOG_INF("mcuboot wrapper: %u B header, %u B payload", (unsigned)hdr_sz,
				(unsigned)img_sz);
		}

		e = begin_at(ULTRAWIDELOCK_DFU_OWNER_SMP, 1U, s_inner);
		if (e != ULTRAWIDELOCK_DFU_ERR_OK) {
			goto refused;
		}
		s_cli_off = 0U;
	} else if (off != s_cli_off) {
		/* Do NOT fail. mcumgr's contract is that the device reports
		 * where it actually is and the host resends from there, which
		 * is what makes a dropped chunk recoverable without restarting
		 * the whole transfer. */
		*next = s_cli_off;
		return 0;
	}

	/* The wrapper's header. */
	if (s_cli_off < s_skip) {
		size_t drop = MIN(len, (size_t)(s_skip - s_cli_off));

		data += drop;
		len -= drop;
		s_cli_off += drop;
	}

	/* The patch. */
	if (len > 0U && s_rx.got < s_inner) {
		size_t take = MIN(len, (size_t)(s_inner - s_rx.got));

		e = feed_bytes(data, take);
		if (e != ULTRAWIDELOCK_DFU_ERR_OK) {
			goto refused;
		}
		len -= take;
		s_cli_off += take;
	}

	/* The wrapper's TLV trailer. Acknowledged and thrown away: the client
	 * has to be allowed to finish sending the file it holds, and nothing
	 * past the patch means anything to the bootloader. */
	s_cli_off += len;

	/* The last patch byte stages the header but does not restart the board.
	 * The host sends os_mgmt reset itself, and a board that rebooted early
	 * would drop the response and read as a failed upload. */
	if (s_rx.active && s_rx.got == s_inner) {
		e = commit_now(false);
		if (e != ULTRAWIDELOCK_DFU_ERR_OK) {
			goto refused;
		}
		LOG_INF("update staged, %u B; awaiting reset", (unsigned)s_inner);
	}

	*next = s_cli_off;
	return 0;

refused:
	s_cli_off = 0U;
	LOG_WRN("upload refused at %u B (err %d)", (unsigned)s_rx.got, (int)e);
	if (s_rx.owner == ULTRAWIDELOCK_DFU_OWNER_SMP) {
		rx_reset_all_locked();
	} else {
		/* A failed attempt may have parsed SMP wrapper geometry before it
		 * learned that another transport owns the receiver. Clear only SMP's
		 * cursors; the other owner's authenticated transfer remains intact. */
		smp_cursor_reset_locked();
	}
	return -EINVAL;
}

int ultrawidelock_dfu_rx_upload(uint32_t off, uint32_t total, const uint8_t *data, size_t len,
				uint32_t *next)
{
	int rc;

	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	rc = rx_upload_locked(off, total, data, len, next);
	ultrawidelock_mutex_unlock(&s_rx_lock);
	return rc;
}

/**
 * Return true if a valid patch header is present in the staging flash area; otherwise return false.
 * The header's magic and ABI version must both match.
 */
bool ultrawidelock_dfu_rx_staged(void)
{
	struct ultrawidelock_dfu_hdr hdr;
	bool staged;

	works_bind();
	ultrawidelock_mutex_lock(&s_rx_lock);
	if (staging_open() != 0) {
		ultrawidelock_mutex_unlock(&s_rx_lock);
		return false;
	}
	if (ultrawidelock_flash_read(s_fa, ULTRAWIDELOCK_DFU_HDR_OFFSET, &hdr, sizeof(hdr)) != 0) {
		ultrawidelock_mutex_unlock(&s_rx_lock);
		return false;
	}
	staged = hdr.magic == ULTRAWIDELOCK_DFU_MAGIC &&
		 hdr.abi_version == ULTRAWIDELOCK_DFU_ABI_VERSION;
	ultrawidelock_mutex_unlock(&s_rx_lock);
	return staged;
}

#endif /* CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG */

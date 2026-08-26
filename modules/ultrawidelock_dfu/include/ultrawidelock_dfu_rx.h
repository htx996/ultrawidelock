/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief Receives a delta patch into the staging partition, application side.
 *
 * Transport-independent on purpose. The DWM3001CDK feeds this from a second
 * L2CAP CoC beside the credential one, but nothing here knows that -- it takes
 * frames and returns replies, so the host tests can drive it without a radio.
 *
 * The bootloader half is @ref ultrawidelock_dfu.h. This side never applies anything: it
 * writes bytes, checks a signature, and reboots.
 */

#ifndef ULTRAWIDELOCK_DFU_RX_H_
#define ULTRAWIDELOCK_DFU_RX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Version-2 request opcodes, first byte of every frame from the host.
 *
 * These deliberately do not overlap the original transfer-blind 0x01..0x04
 * protocol. Old hosts and firmware therefore fail loudly instead of
 * misinterpreting a transfer id as a length.
 */
enum ultrawidelock_dfu_op {
	ULTRAWIDELOCK_DFU_OP_BEGIN = 0x11,  /**< u32 transfer id + u32 total follow */
	ULTRAWIDELOCK_DFU_OP_DATA = 0x12,   /**< u32 transfer id + u32 offset + bytes */
	ULTRAWIDELOCK_DFU_OP_COMMIT = 0x13, /**< u32 transfer id; reboots on success */
	ULTRAWIDELOCK_DFU_OP_ABORT = 0x14,  /**< u32 transfer id; erases staged data */
};

/** Reply opcodes, first byte of every frame back to the host. */
enum ultrawidelock_dfu_rsp {
	ULTRAWIDELOCK_DFU_RSP_OK = 0x81,  /**< u32 transfer id + u32 next offset follow */
	ULTRAWIDELOCK_DFU_RSP_ERR = 0x82, /**< one @ref ultrawidelock_dfu_err byte follows */
};

/**
 * Why a frame was refused.
 *
 * Deliberately coarse. A peer that has not been let in learns only that it was
 * refused, not how close it got.
 */
enum ultrawidelock_dfu_err {
	ULTRAWIDELOCK_DFU_ERR_CLOSED = 1,    /**< no update window is open */
	ULTRAWIDELOCK_DFU_ERR_SEQUENCE = 2,  /**< opcode does not fit the current state */
	ULTRAWIDELOCK_DFU_ERR_SIZE = 3,      /**< will not fit patch_staging */
	ULTRAWIDELOCK_DFU_ERR_AUTH = 4,      /**< header signature did not verify */
	ULTRAWIDELOCK_DFU_ERR_INTEGRITY = 5, /**< length or CRC disagreed at commit */
	ULTRAWIDELOCK_DFU_ERR_FLASH = 6,     /**< a write or erase failed */
	ULTRAWIDELOCK_DFU_ERR_MALFORMED = 7, /**< frame too short for its opcode */
	ULTRAWIDELOCK_DFU_ERR_BUSY = 8,      /**< another transport owns the receiver */
};

/** Largest reply this ever produces. */
#define ULTRAWIDELOCK_DFU_RSP_MAX 9u

/**
 * A receiver owner. Each transport gets a distinct value so a disconnect or
 * malformed request on one endpoint cannot discard another endpoint's upload.
 */
enum ultrawidelock_dfu_owner {
	ULTRAWIDELOCK_DFU_OWNER_NONE = 0,
	ULTRAWIDELOCK_DFU_OWNER_L2CAP = 1,
	ULTRAWIDELOCK_DFU_OWNER_GATT = 2,
	ULTRAWIDELOCK_DFU_OWNER_SMP = 3,
	ULTRAWIDELOCK_DFU_OWNER_TEST = 4,
};

/**
 * Open the update window for @p duration_ms.
 *
 * Until this is called nothing is accepted, and that IS the authorization
 * model. The patch is signed and MCUboot re-verifies the result, so no peer can
 * install code regardless; what the window prevents is an unauthenticated
 * peer in radio range burning flash cycles and forcing reboots. A door lock
 * that anyone nearby can reset in a loop is a real availability attack, and a
 * window the owner has to open is what stops it.
 *
 * Calling it again while open restarts the clock.
 */
void ultrawidelock_dfu_window_open(uint32_t duration_ms);

/** Close the window immediately and discard any transfer in progress. */
void ultrawidelock_dfu_window_close(void);

/** True while the window is open. Transports gate their accept() on this. */
bool ultrawidelock_dfu_window_is_open(void);

/**
 * Called whenever the window opens or closes.
 *
 * Registered rather than a weak symbol so that it survives LTO without
 * argument, and so a port with no indicator pays nothing.
 */
typedef void (*ultrawidelock_dfu_window_cb)(bool open);

/**
 * Watch the window, so the board can SHOW that it is open.
 *
 * There are three ways in -- SW2, Apple Home's pairing mode, and the bench
 * SWD write -- and none of them is visible from outside the board. An owner who
 * pressed the button has no way to tell whether the press registered, and the
 * five-minute window can expire while they are still looking for the phone.
 * One callback covers every path because they all end at
 * ultrawidelock_dfu_window_open().
 */
void ultrawidelock_dfu_set_window_cb(ultrawidelock_dfu_window_cb cb);

/**
 * Handle one frame.
 *
 * @param frame     request bytes, opcode first
 * @param len       length of @p frame
 * @param rsp       at least @ref ULTRAWIDELOCK_DFU_RSP_MAX bytes
 * @param rsp_len   set to the number of reply bytes produced
 *
 * @retval 0 always; failures are reported to the peer through @p rsp, because
 *           a transport has nothing useful to do with an error code.
 */
int ultrawidelock_dfu_rx_frame(enum ultrawidelock_dfu_owner owner, const uint8_t *frame, size_t len,
			      uint8_t *rsp, size_t *rsp_len);

struct ultrawidelock_dfu_hdr; /* ultrawidelock_dfu.h */

/**
 * Called at COMMIT, once the staged bytes have proven themselves.
 *
 * WHY THIS EXISTS. On the DWM3001CDK, COMMIT's job is finished when the header
 * lands: MCUboot reads it on the next boot and applies the patch, so the
 * receiver's whole contract with the bootloader is those 32 bytes on flash.
 *
 * The ESP32 has no such bootloader. Its two OTA slots are selected by an
 * otadata partition that only the application can write, so a staged image
 * that nobody points the bootloader at is simply ignored -- the board reboots
 * into what it was already running and the update silently does not happen.
 * Something has to run between "the bytes are good" and "reboot", and this is
 * it.
 *
 * Called AFTER the magic, ABI, header CRC, length and patch CRC have all
 * passed and after the header write, and BEFORE the reboot is armed. Returning
 * non-zero fails the COMMIT with @ref ULTRAWIDELOCK_DFU_ERR_FLASH and leaves
 * nothing staged, so a port that cannot arm its bootloader refuses the update
 * rather than rebooting into the old image and reporting success.
 *
 * @param hdr the verified header, including @ref ultrawidelock_dfu_hdr.flags --
 *            which is how a port tells a whole-image payload from a delta.
 * @return 0 to accept.
 */
typedef int (*ultrawidelock_dfu_commit_cb)(const struct ultrawidelock_dfu_hdr *hdr);

/**
 * Install the commit hook. NULL (the default) means "nothing to do", which is
 * every port with a bootloader that reads staging by itself.
 */
void ultrawidelock_dfu_set_commit_cb(ultrawidelock_dfu_commit_cb cb);

/** Drop the transfer only when @p owner currently owns it. */
void ultrawidelock_dfu_rx_reset(enum ultrawidelock_dfu_owner owner);

/** Drop all receiver state. Window close and tests use this global boundary. */
void ultrawidelock_dfu_rx_reset_all(void);

/** Erase staging only if it is unowned or owned by @p owner. */
int ultrawidelock_dfu_rx_erase(enum ultrawidelock_dfu_owner owner);

#ifdef CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG
/**
 * Take one SMP image-upload chunk.
 *
 * The same bytes and the same checks as @ref ultrawidelock_dfu_rx_frame, reached from
 * CBOR instead of opcodes, so that a stock mcumgr client (nRF Device Manager,
 * `mcumgr image upload`) can push an update. See src/dfu_smp_img.c.
 *
 * @param off    offset the host believes this chunk starts at
 * @param total  whole wire length; only read when @p off is 0
 * @param data   chunk bytes
 * @param len    length of @p data
 * @param[out] next  offset to send next. On a mismatched @p off this comes back
 *                   as the device's real position and the chunk is discarded --
 *                   a resync, which the protocol treats as success.
 *
 * @retval 0        chunk accepted, or a resync was requested
 * @retval -EACCES  no update window is open
 * @retval -EBUSY   another transport owns the receiver
 * @retval -EINVAL  refused; the transfer is discarded and must restart at 0
 */
int ultrawidelock_dfu_rx_upload(uint32_t off, uint32_t total, const uint8_t *data, size_t len,
		      uint32_t *next);

/** True when a complete, verified update is staged and waiting for a reboot. */
bool ultrawidelock_dfu_rx_staged(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_DFU_RX_H_ */

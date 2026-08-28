/* Host recording doubles for the DW3000 radio surface that ccc_shim_rx.c (the
 * Pre-POLL listener) drives. Every arm/off call is counted and sequenced so a
 * unit test can assert the listen-gate semantics: after ccc_prepoll_stop() no
 * callback path may reach dwt_rxenable, and the last radio call is the
 * forcetrxoff. Register reads return 0 (so callback paths take their ip==0 /
 * quiet branches unless a test injects status bits via dwt_cb_data_t). */
#include <string.h>

#include "deca_device_api.h"
#include "uwb_min.h"
#include "uwb_rxdiag.h"
#include "uwb_seam.h"

struct ultrawidelock_host_rx_rec ultrawidelock_host_rx = { .rxenable_ret = DWT_SUCCESS,
				       .starttx_ret = DWT_SUCCESS };

void ultrawidelock_host_rx_reset(void)
{
	memset(&ultrawidelock_host_rx, 0, sizeof(ultrawidelock_host_rx));
	ultrawidelock_host_rx.rxenable_ret = DWT_SUCCESS;
	ultrawidelock_host_rx.starttx_ret = DWT_SUCCESS;
}

/* The decadriver entries the seam helpers chain into; on the host they ARE the doubles. */
int32_t dwt_rxenable(int32_t mode)
{
	ultrawidelock_host_rx.rxenable_calls++;
	ultrawidelock_host_rx.last_rxenable_mode = mode;
	ultrawidelock_host_rx.last_rxenable_seq = ++ultrawidelock_host_rx.seq;
	return ultrawidelock_host_rx.rxenable_ret;
}

void dwt_forcetrxoff(void)
{
	ultrawidelock_host_rx.forcetrxoff_calls++;
	ultrawidelock_host_rx.last_forcetrxoff_seq = ++ultrawidelock_host_rx.seq;
}

int32_t dwt_starttx(int32_t mode)
{
	(void)mode;
	ultrawidelock_host_rx.starttx_calls++;
	ultrawidelock_host_rx.seq++;
	return ultrawidelock_host_rx.starttx_ret;
}

void dwt_setcallbacks(dwt_callbacks_s *callbacks)
{
	if (callbacks != NULL) {
		ultrawidelock_host_rx.cbs = *callbacks;
	}
}

int32_t dwt_configure(dwt_config_t *config)
{
	(void)config;
	return ultrawidelock_host_rx.configure_ret;
}

void dwt_configurestsmode(uint8_t stsMode)
{
	(void)stsMode;
}

/* Seam helpers uwb_rxdiag.c defines on target; the main binary links the
 * engine without the driver, so they are stubbed here instead. */
void ultrawidelock_uwb_set_callbacks(dwt_callbacks_s *callbacks)
{
	dwt_setcallbacks(callbacks);
}

int32_t ultrawidelock_uwb_configure_phy(dwt_config_t *config)
{
	return dwt_configure(config);
}

void dwt_setinterrupt(uint32_t bitmask_lo, uint32_t bitmask_hi, int options)
{
	(void)bitmask_lo;
	(void)bitmask_hi;
	(void)options;
}

void dwt_setrxtimeout(uint32_t time)
{
	(void)time;
}

void dwt_setdelayedtrxtime(uint32_t starttime)
{
	(void)starttime;
}

int32_t dwt_writetxdata(uint16_t txDataLength, uint8_t *txDataBytes, uint16_t txBufferOffset)
{
	(void)txDataLength;
	(void)txDataBytes;
	(void)txBufferOffset;
	return DWT_SUCCESS;
}

void dwt_writetxfctrl(uint16_t txFrameLength, uint16_t txBufferOffset, uint8_t ranging)
{
	(void)txFrameLength;
	(void)txBufferOffset;
	(void)ranging;
}

uint32_t dwt_read_reg(uint32_t addr)
{
	(void)addr;
	return 0u;
}

uint32_t dwt_readctrdbg(void)
{
	return 0u;
}

uint32_t dwt_readsystimestamphi32(void)
{
	return ultrawidelock_host_rx.systime;
}

uint32_t dwt_readsysstatuslo(void)
{
	return 0u;
}

/** Serialize a 40-bit timestamp LE into the 5-byte wire form ts5_to_u64 undoes. */
static void ts40_out(uint8_t *timestamp, uint64_t v)
{
	for (int i = 0; i < 5; i++) {
		timestamp[i] = (uint8_t)(v >> (8 * i));
	}
}

void dwt_readtxtimestamp(uint8_t *timestamp)
{
	ts40_out(timestamp, ultrawidelock_host_rx.tx_ts40);
}

void dwt_readrxtimestamp_ipatov(uint8_t *timestamp)
{
	ts40_out(timestamp, ultrawidelock_host_rx.rx_ts40);
}

void dwt_readrxdata(uint8_t *buffer, uint16_t length, uint16_t rxBufferOffset)
{
	(void)rxBufferOffset;
	memset(buffer, 0, length);
	if (length > sizeof(ultrawidelock_host_rx.rxdata)) {
		length = sizeof(ultrawidelock_host_rx.rxdata);
	}
	memcpy(buffer, ultrawidelock_host_rx.rxdata, length);
}

uint16_t dwt_getframelength(uint8_t *rng)
{
	if (rng != NULL) {
		*rng = 0u;
	}
	return ultrawidelock_host_rx.rxdata_len;
}

int dwt_readstsquality(int16_t *rxStsQualityIndex, int stsSegment)
{
	(void)stsSegment;
	if (rxStsQualityIndex != NULL) {
		*rxStsQualityIndex = ultrawidelock_host_rx.stsq_val;
	}
	return ultrawidelock_host_rx.stsq_ret;
}

/* ── modules/ultrawidelock_uwb/src/driver externs the listener links against ─────────── */

int uwb_min_radio_init(void)
{
	return ultrawidelock_host_rx.radio_init_ret; /* radio is "up" on the host by default */
}

uint32_t uwb_min_radio_generation(void)
{
	return 1u; /* single init, never reset, on the host */
}

/* Counted rather than ignored: ccc_prepoll_stop() is the only caller, and the
 * whole point of moving the sleep past that function's old early return was
 * that a boot-time probe-and-stop must still put the part down. A stub that
 * silently swallowed the call would let that regress unnoticed. */
void uwb_min_sleep(void)
{
	ultrawidelock_host_rx.sleep_calls++;
}

bool uwb_rxdiag_stream_get(void)
{
	return false;
}

bool uwb_rxdiag_rng_get(void)
{
	return false;
}

/* try_prepoll() decode-cost probe, defined by the excluded uwb_rxdiag.c on target. */
uint32_t g_ccc_dbg_decode;

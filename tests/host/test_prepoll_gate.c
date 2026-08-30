/** @file test_prepoll_gate.c — ccc_shim_rx listen-gate: stop kills every RX
 * self-rearm path, listen reopens it, and the radio's last act on stop is the
 * forcetrxoff (the serialization invariant the suspend fix relies on). The
 * captured DW3000 callbacks are invoked directly, playing the role of the
 * coop isr-workqueue thread. */
#include <errno.h>
#include <string.h>

#include <deca_device_api.h>

#include "ccc_shim.h"
#include "test.h"

/* Drive a captured RX callback the way dwt_isr would. */
static void rx_event(dwt_cb_t cb, uint32_t status)
{
	dwt_cb_data_t d;

	memset(&d, 0, sizeof(d));
	d.status = status;
	cb(&d);
}

void test_prepoll_gate(void)
{
	unsigned before;

	t_group("stop before any listen is a no-op (driver may be unprobed)");
	ultrawidelock_host_rx_reset();
	ccc_prepoll_stop();
	T_EQ("stop.cold.no_trxoff", ultrawidelock_host_rx.forcetrxoff_calls, 0);
	T_EQ("stop.cold.no_rxenable", ultrawidelock_host_rx.rxenable_calls, 0);
	/* A cold stop is a no-op ON THE BUS, not a no-op entirely: it must still
	 * ask for the sleep. ultrawidelock_ranging_init() probes the radio at boot and
	 * stops without ever listening, and that path is precisely how a board
	 * that no phone had come near still rested in IDLE_PLL at 18 mA from
	 * power-on. uwb_min_sleep() makes its own unprobed check, so the
	 * no-SPI-when-cold guarantee the two assertions above encode is kept. */
	T_EQ("stop.cold.sleeps", ultrawidelock_host_rx.sleep_calls, 1);

	t_group("listen arms RX and installs the self-rearm callbacks");
	ultrawidelock_host_rx_reset();
	T_EQ("listen.rc", ccc_prepoll_listen(9u, 9u), 0);
	T_EQ("listen.armed", ultrawidelock_host_rx.rxenable_calls, 1);
	T_EQ("listen.arm.immediate", ultrawidelock_host_rx.last_rxenable_mode,
	     DWT_START_RX_IMMEDIATE);
	T_OK("listen.cb.rxok", ultrawidelock_host_rx.cbs.cbRxOk != NULL);
	T_OK("listen.cb.rxto", ultrawidelock_host_rx.cbs.cbRxTo != NULL);
	T_OK("listen.cb.rxerr", ultrawidelock_host_rx.cbs.cbRxErr != NULL);
	T_OK("listen.cb.txdone", ultrawidelock_host_rx.cbs.cbTxDone != NULL);

	t_group("while up, every RX outcome self-rearms");
	before = ultrawidelock_host_rx.rxenable_calls;
	rx_event(ultrawidelock_host_rx.cbs.cbRxOk, 0u); /* quiet event -> SP0 fallback */
	T_EQ("up.rxok.rearms", ultrawidelock_host_rx.rxenable_calls, before + 1);
	rx_event(ultrawidelock_host_rx.cbs.cbRxTo, DWT_INT_RXFTO_BIT_MASK);
	T_EQ("up.rxto.rearms", ultrawidelock_host_rx.rxenable_calls, before + 2);
	rx_event(ultrawidelock_host_rx.cbs.cbRxErr, DWT_INT_RXFCE_BIT_MASK);
	T_EQ("up.rxerr.rearms", ultrawidelock_host_rx.rxenable_calls, before + 3);

	t_group("stop closes the gate: no callback path re-enables RX");
	before = ultrawidelock_host_rx.forcetrxoff_calls; /* listen()'s own trxoff is already in */
	ccc_prepoll_stop();
	T_EQ("stop.trxoff", ultrawidelock_host_rx.forcetrxoff_calls, before + 1);
	/* And the part goes down after the RX is killed, not instead of it. */
	T_EQ("stop.sleeps", ultrawidelock_host_rx.sleep_calls, 1);
	before = ultrawidelock_host_rx.rxenable_calls;
	rx_event(ultrawidelock_host_rx.cbs.cbRxOk, 0u);
	rx_event(ultrawidelock_host_rx.cbs.cbRxOk, DWT_INT_RXFCG_BIT_MASK |
					 DWT_INT_RXPHD_BIT_MASK |
					 DWT_INT_CIADONE_BIT_MASK);
	rx_event(ultrawidelock_host_rx.cbs.cbRxTo, DWT_INT_RXFTO_BIT_MASK);
	rx_event(ultrawidelock_host_rx.cbs.cbRxErr, DWT_INT_RXFCE_BIT_MASK);
	T_EQ("stop.rx.gated", ultrawidelock_host_rx.rxenable_calls, before);

	t_group("late TXFRS after stop cannot rearm the Final window");
	before = ultrawidelock_host_rx.rxenable_calls;
	rx_event(ultrawidelock_host_rx.cbs.cbTxDone, DWT_INT_TXFRS_BIT_MASK);
	T_EQ("stop.txdone.gated", ultrawidelock_host_rx.rxenable_calls, before);

	t_group("after stop the radio's last act is the forcetrxoff");
	T_OK("stop.trxoff.last",
	     ultrawidelock_host_rx.last_forcetrxoff_seq > ultrawidelock_host_rx.last_rxenable_seq);

	t_group("double stop is idempotent");
	before = ultrawidelock_host_rx.forcetrxoff_calls;
	ccc_prepoll_stop();
	T_EQ("stop.twice.no_trxoff", ultrawidelock_host_rx.forcetrxoff_calls, before);

	t_group("resume: listen reopens the gate and the rearm loop");
	T_EQ("resume.rc", ccc_prepoll_listen(9u, 9u), 0);
	before = ultrawidelock_host_rx.rxenable_calls;
	T_OK("resume.armed", before > 0);
	rx_event(ultrawidelock_host_rx.cbs.cbRxOk, 0u);
	T_EQ("resume.rxok.rearms", ultrawidelock_host_rx.rxenable_calls, before + 1);

	t_group("listen fails when the radio cannot init or configure");
	ccc_prepoll_stop();
	ultrawidelock_host_rx.radio_init_ret = -5;
	T_EQ("listen.radiofail", ccc_prepoll_listen(9u, 9u), -5);
	ultrawidelock_host_rx.radio_init_ret = 0;
	/* A different channel bypasses the PHY cache so dwt_configure runs. */
	ultrawidelock_host_rx.configure_ret = DWT_ERROR;
	T_EQ("listen.cfgfail", ccc_prepoll_listen(5u, 9u), -EIO);
	ultrawidelock_host_rx.configure_ret = DWT_SUCCESS;

	/* Leave the listener stopped so later suites see a quiet radio. */
	ccc_prepoll_stop();
}

/* SPDX-License-Identifier: ISC */

/*
 * DWM3001CDK standalone credential reader.
 *
 * One board: the nRF52833 runs the BLE peripheral and the credential reader engine,
 * and the DW3110 in the same DWM3001C module does the UWB ranging. No host MCU
 * board, no seated DWM3000EVB, no NFC (the CDK has none).
 *
 * Stage 0 keeps main deliberately thin. Its job is to hold the whole call graph
 * live so the linker cannot garbage-collect the engine and hand back a size
 * number that flatters us.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_device.h>

#include "ultrawidelock_approach.h"
#include "ultrawidelock_lat.h"
#include "ultrawidelock_prov.h" /* ultrawidelock_prov_erase, for the factory-reset button */
#include <ultrawidelock/reader.h>
#include <ultrawidelock/uwb.h>
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
#include "matter_commission.h"
#include "matter_fab_settings.h" /* matter_fab_erase, the Matter half of a reset */
#endif
#include "ml_feed.h" /* channel-classifier glue; plain feed when ML is off */
#include "status_led.h"
#include "uwb_cirdiag.h" /* latched Ipatov scalars, for the channel classifier */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR)
#include "ultrawidelock_satellite.h" /* second-anchor verdict; gates PREDICT only */
#endif
/* The sensors are ANCHOR's; the event they raise is the Matter node's. An
 * anchor build with no Matter in it has nowhere to put an alarm. */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR) && IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
#include "door_alarm.h" /* impact latch + swing angle -> DoorLockAlarm */
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
#include "ultrawidelock_latch.h" /* persistent inside veto layered over the side gate */
#include <zephyr/settings/settings.h>
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_WITNESS_LINK_OT)
#include "witness_link.h"
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_SIDE_GATE)
#include "ultrawidelock_side.h" /* fail-closed OUTSIDE-only passive unlock gate */
#include "ultrawidelock_side_log.h"
#include "side_feed.h"
#if defined(CONFIG_ULTRAWIDELOCK_CRED_LAB)
#include "ultrawidelock_log.h"
#include "ultrawidelock_port.h"
#endif
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM)
#include "ultrawidelock_slam.h"
#include "ultrawidelock_slam_hw.h"
#endif

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_DFU_RECEIVER)
/* src/dfu_ble_zephyr.c. One function, so it carries no header of its own. */
int dfu_ble_start(void);
#endif

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_HEAP_PROBE)
#include <mbedtls/memory_buffer_alloc.h>
#endif

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_HEAP_PROBE)
/* Reported at the grant, because by then the unlock has done every P-256 and
 * AES-GCM operation it is going to do. The peak is cumulative since boot, so it
 * covers BLE pairing and the credential exchange too, not only the ranging. */
static void heap_peak_log(const char *when)
{
	size_t used = 0;
	size_t blocks = 0;

	mbedtls_memory_buffer_alloc_max_get(&used, &blocks);
	LOG_INF("mbedtls heap peak @%s: %u B of %u (%u blocks)", when,
		(unsigned int)used, (unsigned int)CONFIG_MBEDTLS_HEAP_SIZE,
		(unsigned int)blocks);
}
#endif /* CONFIG_ULTRAWIDELOCK_HEAP_PROBE */

/* The per-frame UWB diagnostic trace (DIAGK) defaults ON for nRF targets but OFF
 * for the ESP32, because its printf on every ranging frame blocks the callback
 * long enough to miss the DW3110 delayed-TX slot deadline -- the ESP port disables
 * it for exactly that reason (bench-correlated late RESPONSE arms). What shell this
 * board has exists only in provisioning mode, where the radios never start, so
 * `uwbdiag off` can never be typed at a walk-up; force it off here before any
 * ranging starts. See modules/ultrawidelock_uwb/include/ultrawidelock_diag.h. */
extern volatile int ultrawidelock_uwb_diag_on;

/* Reader status housekeeping: the engine expects a periodic tick to age out a
 * stalled transaction and to drive the ranging power gate's decay. 250 ms is
 * the cadence the ESP32 port runs. */
#define ULTRAWIDELOCK_TICK_MS 250

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_SIDE_GATE)
/* Liveness watchdog on the witness feed, which is NOT the same question as
 * ultrawidelock_side_cfg::evidence_fresh_ms. That one asks "how old is the committed
 * decision at the moment a feed arrives"; this one asks "is anything still
 * feeding us at all". Sizing them the same breaks the gate: witnesses summarise
 * over WITNESS_WINDOW_MS (2000), so SF1 lands about every 2 s and a 1500 ms
 * watchdog would expire between every pair of feeds and hold the gate shut
 * forever. 5 s survives one dropped window plus margin, and still shuts the
 * gate within seconds of a witness link dying. Must stay above 2x the feed
 * period: shorten it if WITNESS_WINDOW_MS is ever cut. */
#define SIDE_FEED_WATCHDOG_MS 5000

/* A vetoed unlock is re-offered on every trusted range, so the deny line needs
 * a floor or it buries everything else on the console during a walk-up. */
#define SIDE_DENY_LOG_MS 1000
#endif

/* How long the ranging LED holds after the last range landed. Four ticks: long
 * enough that no rate iOS ranges at can make it stutter, short enough that a
 * phone put in a pocket drops the light while the person is still in the room. */
#define ULTRAWIDELOCK_LED_RANGE_HOLD_MS 1000

/* Wakes the grant loop the moment a range is latched, instead of on the next
 * 250 ms tick.
 *
 * The tick alone quantized every unlock and relock by 0-250 ms on top of the
 * structural ~1 s trust floor, and worse, 250 ms beats against the 192 ms
 * ranging block, so the delay a walk-up got was a lottery rather than a
 * constant. The tick REMAINS as the take timeout: it is still what ages out a
 * stalled transaction and decays the power gate when no range is arriving.
 *
 * Count limit 1 because the loop reads the generation counter, not a queue --
 * two latches between wakes still mean exactly one pass.
 *
 * Declared and initialised separately rather than with K_SEM_DEFINE: semgrep's
 * C parser cannot read that macro at file scope, and the security gate treats
 * an unparseable file as zero rule coverage rather than a clean result, which
 * would silently drop every rule that guards this file. */
static struct k_sem s_range_sig;

/**
 * Wake the grant loop on an accepted range latch. Runs on the UWB RX path, so it does nothing but
 * give the semaphore -- the float math in the approach controller stays on the main thread.
 */
static void on_range_latched(void)
{
	(void)ultrawidelock_lat_mark(ULTRAWIDELOCK_LAT_FIRST_RANGE);
	k_sem_give(&s_range_sig);
}

/* Provisioning mode: hold SW2 (the board's sw0 alias, P0.02) through reset.
 *
 * The reader identity is per-device data in the settings store, never a string
 * in the image, so it has to arrive at runtime. This board's only input path is
 * the USB device port wired straight to the nRF52833 -- RTT is output-only --
 * so provisioning mode brings up CDC-ACM and the `ultrawidelock` console on it.
 *
 * The radios stay down in this mode on purpose. It keeps USB's millisecond SOF
 * interrupts away from the DW3110's delayed-TX reply window (the timing that
 * commit 5b8d06b had to fight for on this single-core part), and it means the
 * console can never be reached while a walk-up is in flight. */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_PROV_CONSOLE)
/**
 * Check GPIO SW0 (active-low, pulled up in DTS) to see if provisioning is requested at boot.
 * Returns true if SW0 is ready and held (logical 1), false otherwise.
 */
static bool provisioning_requested(void)
{
	static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

	if (!gpio_is_ready_dt(&sw)) {
		return false;
	}
	if (gpio_pin_configure_dt(&sw, GPIO_INPUT) != 0) {
		return false;
	}
	/* Active low with a pull-up in the board DTS; _dt() returns logical level. */
	return gpio_pin_get_dt(&sw) == 1;
}

/* Runs the console and nothing else. Never returns: leaving this function would
 * start the radios in a mode the user did not ask for. */
static void provisioning_mode(void)
{
	int rc = usb_enable(NULL);

	/* Solid blue for as long as this mode lasts. Provisioning looks exactly
	 * like a hung boot from the outside otherwise -- the radios are down, so
	 * nothing else on the board moves. */
	status_led_signal(STATUS_LED_PROV_MODE, true);

	if (rc != 0) {
		/* Nothing to fall back to: without USB there is no input path at
		 * all on this board, so say so on RTT and stop. */
		LOG_ERR("provisioning mode: usb_enable rc=%d, no console available", rc);
	} else {
		LOG_INF("provisioning mode: USB console up, radios down");
	}

	while (1) {
		k_msleep(1000);
	}
}
#endif /* CONFIG_ULTRAWIDELOCK_PROV_CONSOLE */

/* Factory reset: hold SW2 (the sw0 alias, P0.02) through reset.
 *
 * WITHOUT THIS THE BOARD IS A BRICK AFTER A FAILED PAIRING, which is not a
 * bench annoyance but the ordinary failure. A commissioning that gets far
 * enough to install a fabric and then times out leaves the fabric stored; the
 * advert gate then offers credential 0xFFF2 instead of commissionable; the
 * controller can neither discover the node nor open a commissioning window on
 * an accessory it has already forgotten. On 2026-08-02 that state was reached
 * four times in one evening and cleared four times with a debugger. A user has
 * no debugger.
 *
 * Held through reset rather than long-pressed while running: it matches the
 * provisioning console's idiom on this same button, needs no timer, no
 * debounce and no thread on a part at 96% RAM, and cannot fire while a walk-up
 * is in flight.
 *
 * The Thread credentials are deliberately NOT erased -- see ultrawidelock_prov_erase().
 */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_FACTORY_RESET_BUTTON)
/**
 * Check GPIO SW0 (active-low, pulled up in DTS) at boot. If SW0 is held (logical 1), blink the lock
 * LED as user feedback, erase credential provisioning and Matter fabric (if
 * CONFIG_ULTRAWIDELOCK_MATTER_BLE is on), and log that the board is now commissionable on the next
 * boot. Returns silently if GPIO is not ready or if SW0 is not held.
 */
static void factory_reset_if_requested(void)
{
	static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

	if (!gpio_is_ready_dt(&sw) || gpio_pin_configure_dt(&sw, GPIO_INPUT) != 0) {
		return;
	}
	/* Active low with a pull-up in the board DTS; _dt() returns logical level. */
	if (gpio_pin_get_dt(&sw) != 1) {
		return;
	}

	LOG_WRN("SW2 held at boot: FACTORY RESET");
	/*
	 * The only feedback this board can give someone without a debugger. RTT
	 * says it too, but a user holding a button needs to see that the hold
	 * was long enough and registered. Blocking, and it goes through
	 * status_led.c because that owns the pin from SYS_INIT onwards -- driving
	 * it from here as well would put the blink and the heartbeat on the same
	 * lamp, 120 ms apart.
	 */
	status_led_boot_blink();

	(void)ultrawidelock_prov_erase();
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
	(void)matter_fab_erase();
#endif
	LOG_WRN("factory reset done; commissionable on the next boot");
}
#endif

/**
 * Entry point for the DWM3001CDK reader application. Initializes provisioning and factory-reset
 * paths, starts the credential BLE reader and optional Matter commissioning and DFU receiver, then
 * runs the approach controller loop. Feeds the controller trusted ranges on each new latch
 * generation and observes untrusted ranges for departure detection. Grants unlock on approach
 * prediction or threshold crossing, relocks on departure or abort, and exits with an error code if
 * reader startup fails.
 */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
/*
 * The inside veto. The side gate classifies each window; this decides what a
 * run of those classifications is allowed to authorise, and its resting state
 * is INSIDE. File scope because the settings handler has to reach it, and
 * static because that is the point -- the belief outlives the session, the
 * reboot, and the witnesses.
 */
static struct ultrawidelock_latch s_latch;
static bool s_latch_dirty;

/*
 * ONE credential, for now. The module tracks ULTRAWIDELOCK_LATCH_MAX_CREDS
 * records keyed by a derived non-identifying id, but the reader does not yet
 * hand the main loop anything that distinguishes one credential from another,
 * so every session maps to the same record.
 *
 * State the consequence rather than hiding it: with two enrolled phones, one
 * of them leaving clears the belief for both. That is a loss of strictness
 * against docs/inside-latch.md and it resolves in the UNSAFE direction, so it
 * is a gap, not a simplification. It closes when the credential plumbing
 * exposes a stable per-endpoint handle to hash here.
 */
#define LATCH_CRED_ID 0x00000001u

static int latch_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t blob[ULTRAWIDELOCK_LATCH_BLOB_LEN];
	ssize_t got;

	if (strcmp(name, "rec") != 0 || len > sizeof(blob)) {
		return -ENOENT;
	}
	got = read_cb(cb_arg, blob, sizeof(blob));
	if (got <= 0) {
		return -EINVAL;
	}
	/* A rejected blob leaves the latch initialised-and-empty, which reads
	 * INSIDE for everyone. Corruption must not be recoverable into a
	 * permissive state. */
	if (!ultrawidelock_latch_deserialize(&s_latch, NULL, blob, (size_t)got)) {
		LOG_WRN("latch records unreadable; every credential reads INSIDE");
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(uwl_latch, "uwl/latch", NULL, latch_settings_set, NULL, NULL);

static void latch_save(void)
{
	uint8_t blob[ULTRAWIDELOCK_LATCH_BLOB_LEN];
	size_t n;

	if (!s_latch_dirty) {
		return;
	}
	s_latch_dirty = false;
	n = ultrawidelock_latch_serialize(&s_latch, blob, sizeof(blob));
	if (n == 0u || settings_save_one("uwl/latch/rec", blob, n) != 0) {
		LOG_ERR("latch records could not be saved");
	}
}

/* Any door opening at all re-asserts INSIDE. Called from every grant path the
 * main loop can see, deliberate ones included: a phone that just walked
 * through the door is inside regardless of what opened it. */
static void latch_note_opened(int64_t now_ms)
{
	ultrawidelock_latch_note_grant(&s_latch, LATCH_CRED_ID, now_ms);
	s_latch_dirty = true;
	latch_save();
}
#endif

int main(void)
{
	/* Off before the radio comes up: keeps the ranging callbacks print-free so the
	 * delayed RESPONSE/FINAL TX can hit its microsecond turnaround. */
	ultrawidelock_uwb_diag_on = 0;

	/* ASCII only: the console is a byte stream, and a UTF-8 dash renders as
	 * mojibake in RTT Viewer. */
	LOG_INF("ultrawidelock reader: DWM3001CDK (nRF52833 + DW3110)");

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_PROV_CONSOLE)
	if (provisioning_requested()) {
		provisioning_mode(); /* never returns */
	}
#endif

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_FACTORY_RESET_BUTTON)
	factory_reset_if_requested();
#endif

	int rc = ultrawidelock_reader_start();

	if (rc != 0) {
		LOG_ERR("ultrawidelock_reader_start rc=%d", rc);
		/* main() is about to return and this board has no console, so a
		 * solid D12 is the only account anyone gets of why it does
		 * nothing. The LED tick lives on the system work queue and
		 * outlives this thread, so the light stays on. */
		status_led_signal(STATUS_LED_FAULT, true);
		return rc;
	}

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS) && defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG)
	/*
	 * The classifier is this image's consumer of the CIA latch, and nothing else
	 * arms it: the capture-cycle thread is CONFIG_ULTRAWIDELOCK_CIRDIAG_CAPTURE (not set
	 * here) and the console shell is a DK-only path. Safe before the radio is
	 * probed -- the chip-side CIA enable happens lazily inside the first armed
	 * reception. The first mlgate walk (2026-08-07) printed zero [ALAB] lines
	 * precisely because nothing did this.
	 */
	uwb_cirdiag_set_enabled(true);
#endif

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
	/* After the reader, because the reader owns BLE and the advertising set;
	 * this only attaches handlers to the 0xFFF6 transport that SYS_INIT
	 * already brought up. */
	(void)matter_commission_init();
#endif

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_DFU_RECEIVER)
	/* Also after the reader, and for the same reason: registering an L2CAP
	 * PSM needs the host up, and the reader is what enables it. The channel
	 * refuses every connection until SW2 opens a window, so registering it
	 * here costs nothing an idle board can be reached through. */
	(void)dfu_ble_start();
#endif

	/* Bridge the trusted UWB range stream to the Wallet grant.
	 *
	 * ultrawidelock_reader_start brings up BLE and the CCC/FiRa ranging engine, and the engine
	 * latches a trust-gated distance (fira_session) on every good block -- but nothing
	 * consumes it on its own. The shipped Matter lock wires this in its app_main
	 * (apps/esp32-matter-lock): trusted range -> approach controller -> on UNLOCK,
	 * ultrawidelock_reader_notify_unlock(true), which sends Reader Status = Unsecured and animates
	 * the phone. The standalone reader has to do the same or a perfectly good range never
	 * becomes an unlock. There is no bolt on this board: the grant IS the product.
	 *
	 * Static: the struct carries two 5-entry sample windows plus the filter and
	 * grew past trivial; the 4 KB main stack is not the place to discover that,
	 * and in .bss the cost shows up in the measured RAM budget instead. */
	static struct ultrawidelock_approach approach;
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR)
	/*
	 * Second-anchor geometry. Nothing feeds this yet -- the satellite
	 * transport is not wired up -- and that is a working
	 * state rather than a gap: with no report the verdict is UNKNOWN, UNKNOWN
	 * permits prediction, and the door behaves exactly as it does today.
	 */
	static struct ultrawidelock_satellite satellite;
	const struct ultrawidelock_fusion_cfg fusion_cfg = {
		.baseline_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_BASELINE_MM,
		.tol_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_TOL_MM,
		.deadband_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_DEADBAND_MM,
	};

	ultrawidelock_satellite_init(&satellite, &fusion_cfg, CONFIG_ULTRAWIDELOCK_ANCHOR_STALE_MS,
			   IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_SELF_INSIDE));
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR) && IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
	/* Door alarms. The ajar half waits on the same missing transport as the
	 * satellite verdict above; the forced-open half is live below. */
	door_alarm_init();
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_SIDE_GATE)
	/*
	 * Fail-closed side gate. BLE witness summaries arrive as SF1 lines on
	 * RTT down (lab) via side_feed_*; without them every evaluate stays
	 * UNKNOWN / quorum-fail and passive unlock is withheld. NFC Express
	 * Mode and Home commands are not routed through this switch.
	 */
	static struct ultrawidelock_side_filter side_filt;
	static struct ultrawidelock_side_decision side_dec;
	static struct ultrawidelock_side_cfg side_cfg;
	/* When the last feed landed. side_dec is a snapshot, and the filter can
	 * only age it while it is being fed -- so a feed that stops right after
	 * committing OUTSIDE would leave the grant live forever. Observed: a lab
	 * injector was killed and the lock still opened on the next walk-up. */
	int64_t side_feed_ms = 0;
	int64_t side_deny_log_ms = 0;

	ultrawidelock_side_defaults(&side_cfg);
	ultrawidelock_side_filter_init(&side_filt, &side_cfg);
	{
		/* Boot: no witnesses yet => UNKNOWN + quorum fail (fail-closed). */
		struct ultrawidelock_side_features absent;

		memset(&absent, 0, sizeof(absent));
		absent.now_ms = k_uptime_get();
		absent.uwb_range_mm = -1;
		absent.uwb_vel_mm_s = INT32_MIN;
		absent.ble_rssi_inside_dbm = INT16_MIN;
		absent.ble_rssi_outside_dbm = INT16_MIN;
		absent.ble_rssi_threshold_dbm = INT16_MIN;
		absent.uwb_peer_mm = -1;
		absent.classifier_ver = side_cfg.classifier_ver;
		absent.calibration_ver = side_cfg.calibration_ver;
		absent.flags = ULTRAWIDELOCK_SIDE_F_QUORUM_FAIL;
		side_dec = ultrawidelock_side_filter_feed(&side_filt, &absent);
	}
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
	{
		struct ultrawidelock_latch_cfg latch_cfg;

		ultrawidelock_latch_defaults(&latch_cfg);
		latch_cfg.entry_dwell_ms = CONFIG_ULTRAWIDELOCK_LATCH_ENTRY_DWELL_MS;
		latch_cfg.clear_windows = CONFIG_ULTRAWIDELOCK_LATCH_CLEAR_WINDOWS;
		latch_cfg.clear_min_mm = CONFIG_ULTRAWIDELOCK_LATCH_CLEAR_MIN_MM;
		latch_cfg.clear_valid_ms = CONFIG_ULTRAWIDELOCK_LATCH_CLEAR_VALID_MS;
		ultrawidelock_latch_init(&s_latch, &latch_cfg);
		/* Records land through the settings handler. A load that never
		 * happens leaves every credential reading INSIDE, which is the
		 * state an uncommissioned lock should be in. */
		(void)settings_load_subtree("uwl/latch");
	}
	static uint8_t latch_why;
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_WITNESS_LINK_OT)
	witness_link_init();
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM)
	static struct ultrawidelock_slam_state slam;
	const struct ultrawidelock_slam_cfg slam_cfg = {
		.debounce_ms = ULTRAWIDELOCK_SLAM_DEBOUNCE_MS_DEFAULT,
		.tamper_window_ms = ULTRAWIDELOCK_SLAM_TAMPER_WINDOW_MS_DEFAULT,
		.tamper_count = ULTRAWIDELOCK_SLAM_TAMPER_COUNT_DEFAULT,
	};

	ultrawidelock_slam_init(&slam);
	/* A board with no accelerometer, or one that will not answer, loses the
	 * tamper signal and keeps the lock. Nothing below depends on it. */
	if (ultrawidelock_slam_hw_init() != 0) {
		LOG_WRN("no impact sensor; tamper detection is off");
	}
#endif

	/* Factory defaults: unlock 100 cm, relock 250 cm, and a trajectory gate
	 * at 180 cm -- no auto-unlock until the credential has been seen that
	 * far out in this session, so a phone that was already at the door when
	 * ranging started does not open it. See ultrawidelock_approach_cfg::approach_cm. */
	ultrawidelock_approach_init(&approach, NULL);
	approach.cfg.near_dwell = CONFIG_ULTRAWIDELOCK_APPROACH_NEAR_DWELL;

	/* Same seam the ESP32 matter-lock uses (app_main.cpp on_uwb_range): the engine
	 * signals, this thread decides. Both lines run before the listener can fire --
	 * the semaphore has to exist before anything is allowed to give it, and the
	 * controller has to be initialised before a signal can reach it. */
	k_sem_init(&s_range_sig, 0, 1);
	ultrawidelock_uwb_set_range_listener(on_range_latched);

	uint32_t last_gen = ultrawidelock_uwb_range_generation();
	/* The last range OBSERVED for departure, trusted or not; see the loop. */
	uint32_t last_obs_gen = last_gen;
	/* A third epoch, for the activity LED alone. The two above are consumed
	 * at different moments on purpose -- that is what keeps a late-trusted
	 * latch from being counted twice and what stops the silence clock being
	 * refreshed -- so folding a light into either would change when a relock
	 * fires. This one is read-only with respect to the unlock logic. */
	uint32_t led_gen = last_gen;
	int64_t led_range_ms = 0;
	bool present = false;
	bool granted = false;
	/* Rising-edge detector for the credential session, which is what arms the
	 * trajectory gate. See ultrawidelock_approach_session_up(). */
	bool session_was_up = false;

	while (1) {
		int64_t now = k_uptime_get();
		uint32_t gen = ultrawidelock_uwb_range_generation();
		int32_t cm = 0;
		enum ultrawidelock_approach_action act;

		ultrawidelock_reader_status_tick(now);
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_WITNESS_LINK_OT)
		witness_link_tick(now);
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_SIDE_GATE)
		{
			struct ultrawidelock_side_features feat;

			side_feed_rtt_poll();
			if (side_feed_take(&feat)) {
				feat.now_ms = now;
				if (feat.obs_session_id == 0) {
					feat.obs_session_id = 1;
				}
				feat.classifier_ver = side_cfg.classifier_ver;
				feat.calibration_ver = side_cfg.calibration_ver;
				side_dec = ultrawidelock_side_filter_feed(&side_filt, &feat);
				side_feed_ms = now;
				LOG_INF("side feed: side=%u conf=%u flags=0x%02x oi_pkts=%u/%u",
					(unsigned)side_dec.side, side_dec.confidence,
					side_dec.flags, feat.ble_pkts_inside,
					feat.ble_pkts_outside);
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
				/*
				 * Only decisions the side gate would itself
				 * release may extend a run. The two gates run in
				 * series: this counts windows that already
				 * cleared confidence, quorum, freshness and the
				 * contradiction flags, so a degraded or stale
				 * window can contradict but never accumulate.
				 */
				ultrawidelock_latch_note_window(
					&s_latch, LATCH_CRED_ID,
					ultrawidelock_side_may_passive_unlock(&side_dec, &side_cfg)
						? side_dec.side
						: (side_dec.side ==
							   ULTRAWIDELOCK_SIDE_LABEL_INSIDE
							   ? ULTRAWIDELOCK_SIDE_LABEL_INSIDE
							   : ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN),
					feat.uwb_range_mm, now);
#endif
			}

			/* Age the snapshot on the loop clock, not on the arrival of
			 * the next feed. A witness link that dies must close the
			 * gate, not freeze it open at whatever it last said. */
			if (side_dec.side != ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN &&
			    (now - side_feed_ms) > (int64_t)SIDE_FEED_WATCHDOG_MS) {
				LOG_WRN("side evidence stale (%lld ms); closing gate",
					(long long)(now - side_feed_ms));
				side_dec.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
				side_dec.confidence = 0;
				side_dec.flags |= ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE;
			}

			/*
			 * REVOKE. The gate above only guards the moment of the
			 * grant; nothing withdrew one already given. That leaves
			 * the "walk in and stay" case with no way back to Secured:
			 * iOS stops ranging once the phone is still, and the
			 * departure path deliberately refuses to relock from a
			 * last measurement INSIDE the radius (ultrawidelock_approach.c,
			 * far_silence_ms) so it cannot throw the bolt at a
			 * stationary owner. MEASURED 2026-08-11: grant, then
			 * "UWB session IDLE" at ~112 cm, then nothing -- the door
			 * stayed open with the phone demonstrably indoors.
			 *
			 * Committed INSIDE is the one signal that says the person
			 * is through the door, and it is independent of ranging.
			 * No confidence floor: revoking is the safe direction, so
			 * evidence too weak to open on is still good enough to
			 * close on. UNKNOWN deliberately does NOT revoke -- losing
			 * sight of a phone is not evidence it went inside, and
			 * relocking on it would fire while the owner stands at an
			 * open door. A peer that actually leaves relocks through
			 * the session-ended path below.
			 */
			/*
			 * INSIDE_CONTRADICT counts as well as a committed
			 * INSIDE. It is set the moment one window favours
			 * inside while OUTSIDE is committed -- which is this
			 * case, one window into the walk-in, about 6 s before
			 * INSIDE could commit. Waiting for the commit means
			 * holding the door open across the whole crossing.
			 */
			const bool went_inside =
				side_dec.side == ULTRAWIDELOCK_SIDE_LABEL_INSIDE ||
				(side_dec.flags & ULTRAWIDELOCK_SIDE_F_INSIDE_CONTRADICT) != 0;

			if (granted && went_inside) {
				LOG_INF("passive unlock revoked: side=%u flags=0x%02x conf=%u",
					(unsigned)side_dec.side, side_dec.flags,
					side_dec.confidence);
				ultrawidelock_reader_notify_unlock(false);
				status_led_signal(STATUS_LED_UNLOCKED, false);
				granted = false;
				/* Same state repair as a refusal: the controller still
				 * believes the bolt it opened is open. Without this it
				 * never offers again until a departure past relock_cm,
				 * which is the very thing that is not coming. */
				ultrawidelock_approach_veto(&approach);
			}
		}
#endif

		/*
		 * D11: what the phone is doing. The loop wakes on the latch
		 * itself, so this lights within a round rather than on the next
		 * 250 ms tick. Any range counts, trusted or not -- the light
		 * says "the radios are working on someone", which is the
		 * question being asked when you are standing in front of a
		 * board that has not opened.
		 * ULTRAWIDELOCK_LED_RANGE_HOLD_MS outlives one round at every rate iOS
		 * uses, so a walk-up shows as a steady 4 Hz rather than a
		 * stutter, and it drops back to the 1 Hz session light about a
		 * second after the phone stops ranging (which a still phone
		 * does, with the session still up).
		 */
		if (gen != led_gen) {
			led_gen = gen;
			led_range_ms = now;
		}
		/* The != 0 is not redundant: uptime is a few tens of ms when this
		 * loop first runs, so without it every board reports ranging for
		 * its first second. */
		bool ranging = led_range_ms != 0 &&
			       (now - led_range_ms) < ULTRAWIDELOCK_LED_RANGE_HOLD_MS;

		status_led_signal(STATUS_LED_RANGING, ranging);
		const bool session_now = ultrawidelock_reader_session_active();

		status_led_signal(STATUS_LED_SESSION, session_now);
		if (session_now && !session_was_up) {
			/*
			 * A session cannot come up without the phone approaching: the
			 * BLE RSSI power gate holds ranging off until the connection
			 * crosses its open threshold. So this edge is the approach
			 * evidence approach_cm wants, and it is the only form of it
			 * this architecture produces -- UWB starts when the phone is
			 * already at the door, so a 180 cm range never arrives.
			 */
			ultrawidelock_approach_session_up(&approach);
		}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
		if (session_now != session_was_up) {
			/* Clear progress belongs to one approach: a run of
			 * agreeing windows must never be inherited by the next
			 * session, which may be a different phone entirely. */
			if (session_now) {
				ultrawidelock_latch_session_open(&s_latch, LATCH_CRED_ID);
			} else {
				ultrawidelock_latch_session_close(&s_latch);
			}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_WITNESS_LINK_OT)
			witness_link_session(session_now);
#endif
		}
#endif
		session_was_up = session_now;
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
		/* D12: an uncommissioned node cannot unlock anything, and it is
		 * indistinguishable from a working one until someone walks up. */
		status_led_signal(STATUS_LED_UNCOMMISSIONED, !matter_commission_has_fabric());
#endif

		/* Feed exactly one sample per NEWLY accepted trusted range (the generation epoch
		 * advances only on an accepted latch), mirroring the ESP lock's per-wake feed. A
		 * stale latch -- iOS stops ranging once the phone holds still -- keeps the old
		 * generation, so it drives a tick, not a fresh approach sample. */
		if (gen != last_gen && ultrawidelock_uwb_trusted_range_cm(&cm)) {
			last_gen = gen;
			last_obs_gen = gen;
			present = true;
			(void)ultrawidelock_lat_mark(ULTRAWIDELOCK_LAT_TRUSTED_RANGE);
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_WITNESS_LINK_OT)
			/* Only ranges the integrity consensus vouches for are
			 * published. The picker correlates advertiser RSSI
			 * against this, so feeding it an unvouched range would
			 * let a spoofed distance choose which advertiser the
			 * lock believes is the phone. */
			witness_link_set_range_mm(cm * 10);
#endif
			act = ml_feed_range(&approach, now, cm);
		} else {
			/*
			 * A fresh range the integrity consensus will not vouch
			 * for still says something -- about DEPARTURE only. Far
			 * ranges are the ones it declines, so without this the
			 * walk-away relock can never fire; see
			 * ultrawidelock_approach_observe_departure() for why reading an
			 * unvouched range is safe in that one direction.
			 *
			 * last_gen is deliberately NOT consumed here. Trust can
			 * arrive late for a latch already taken (the good-run
			 * counter builds across blocks), and the retry above is
			 * what catches it. A separate epoch keeps this from
			 * observing the same range twice, which would refresh
			 * the silence clock and stop it ever expiring.
			 */
			if (gen != last_obs_gen) {
				int32_t raw = 0;

				last_obs_gen = gen;
				if (ultrawidelock_uwb_last_range_cm(&raw)) {
					ultrawidelock_approach_observe_departure(&approach, now, raw);
				}
			}
			act = ultrawidelock_approach_tick(&approach, now);
		}

		ml_feed_vote_trace(&approach, now);
		if (act == ULTRAWIDELOCK_APPROACH_UNLOCK_PREDICT ||
		    act == ULTRAWIDELOCK_APPROACH_UNLOCK_THRESHOLD) {
			(void)ultrawidelock_lat_mark(ULTRAWIDELOCK_LAT_NEAR_DWELL);
		}

		switch (act) {
		case ULTRAWIDELOCK_APPROACH_UNLOCK_PREDICT:
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR) && !IS_ENABLED(CONFIG_ULTRAWIDELOCK_SIDE_GATE)
			/*
			 * Legacy two-anchor helper: gates PREDICTION only and
			 * fail-opens on UNKNOWN. Retained when ULTRAWIDELOCK_SIDE_GATE is
			 * off so existing ANCHOR=1 behaviour stays unchanged.
			 */
			if (!ultrawidelock_satellite_may_predict(&satellite, approach.last_cm * 10, now)) {
				LOG_INF("predict withheld: second anchor puts the phone outside");
				break;
			}
#endif
			/* fall through */
		case ULTRAWIDELOCK_APPROACH_UNLOCK_THRESHOLD:
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_SIDE_GATE)
			/*
			 * Safety gate for ALL passive approach unlocks. Requires
			 * confident OUTSIDE. UNKNOWN/INSIDE/THRESHOLD/stale/
			 * quorum-fail suppress the grant. Intentional paths
			 * (NFC Express, Home, mechanical) do not enter here.
			 */
			if (!ultrawidelock_side_may_passive_unlock(&side_dec, &side_cfg)) {
				/*
				 * Hand the unlock back. Both approach unlock paths
				 * clear their own `locked` before returning, so
				 * without this the controller thinks the bolt is
				 * open and never offers again -- one refusal, made
				 * before the witnesses had time to commit a side,
				 * killed auto-unlock for the whole approach.
				 */
				ultrawidelock_approach_veto(&approach);
				/* Retried on every trusted range now, so rate-limit
				 * the line or it buries the RTT console. */
				if ((now - side_deny_log_ms) >= SIDE_DENY_LOG_MS) {
					side_deny_log_ms = now;
					LOG_INF("passive unlock withheld: side=%u conf=%u flags=0x%02x",
						(unsigned)side_dec.side, side_dec.confidence,
						side_dec.flags);
				}
#if defined(CONFIG_ULTRAWIDELOCK_CRED_LAB)
				ultrawidelock_printf("[ALAB] t=%lld ev=side.deny side=%u conf=%u flags=%u\n",
					   ultrawidelock_uptime_us(), (unsigned)side_dec.side,
					   side_dec.confidence, side_dec.flags);
#endif
				break;
			}
#endif
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
			/*
			 * The inside veto, last and unconditional. Reached only
			 * for passive approach unlocks: NFC Express Mode, Apple
			 * Home commands and mechanical operation do not enter
			 * this switch and must never be gated here.
			 *
			 * Refusing hands the unlock back the same way the side
			 * gate does, so the approach controller keeps offering
			 * for the rest of the walk-up rather than being killed
			 * by one early refusal.
			 */
			if (!ultrawidelock_latch_may_passive_unlock(&s_latch, LATCH_CRED_ID, now,
								   &latch_why)) {
				ultrawidelock_approach_veto(&approach);
				if ((now - side_deny_log_ms) >= SIDE_DENY_LOG_MS) {
					side_deny_log_ms = now;
					LOG_INF("passive unlock withheld: inside latch (why=0x%02x)",
						latch_why);
				}
				break;
			}
#endif
			ultrawidelock_reader_notify_unlock(true); /* Reader Status -> Unsecured (animate) */
			status_led_signal(STATUS_LED_UNLOCKED, true);
			granted = true;
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
			/* The door is open, so assume the phone goes in. This
			 * is the pessimism the whole module rests on. */
			latch_note_opened(now);
#endif
			if (ultrawidelock_lat_mark(ULTRAWIDELOCK_LAT_BOLT_DRIVEN)) {
				/* This CDK has no motor. BOLT_DRIVEN means the software grant
				 * and its visible output have both been committed. */
				ultrawidelock_lat_report();
			}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_HEAP_PROBE)
			heap_peak_log("unlock");
#endif
			break;
		case ULTRAWIDELOCK_APPROACH_RELOCK_DEPART:
		case ULTRAWIDELOCK_APPROACH_RELOCK_ABORT:
			ultrawidelock_reader_notify_unlock(false); /* Reader Status -> Secured */
			status_led_signal(STATUS_LED_UNLOCKED, false);
			granted = false;
			break;
		default:
			break;
		}

		/* Departure: the peer's credential session ended (walked away / phone pocketed). iOS
		 * ranging silence alone does NOT mean departed (a still phone stops ranging too),
		 * so gate on the session, not on range age. Tell Wallet Secured once and reset. */
		if (present && !ultrawidelock_reader_session_active()) {
			/*
			 * Reaching here with the bolt still open means the silence
			 * relock in ultrawidelock_approach_tick() did NOT fire, and this is
			 * the only moment that proves it: the Secured is about to go
			 * out with no session left to carry it.
			 *
			 * last_cm is what the controller was actually FED, which is
			 * not what the status line prints. main.c feeds only a range
			 * ultrawidelock_uwb_trusted_range_cm() vouches for; the trace prints
			 * the raw latch either way, so a walk-away can show 390 cm
			 * on screen while the controller last saw 199 cm. Printing
			 * both the value and its age says which of the two gates
			 * held. MUST run before ultrawidelock_approach_gone(), which
			 * re-inits the struct and erases the evidence.
			 */
			if (granted) {
				LOG_WRN("departure fallback: last FED %d cm, %u ms ago "
					"(gate: >= %d cm held for %d ms)",
					approach.last_cm,
					approach.last_feed_ms != 0
						? (unsigned int)(now - approach.last_feed_ms)
						: 0u,
					approach.cfg.relock_cm, approach.cfg.far_silence_ms);
			}
			(void)ultrawidelock_approach_gone(&approach);
			if (granted) {
				ultrawidelock_reader_notify_unlock(false);
				status_led_signal(STATUS_LED_UNLOCKED, false);
				granted = false;
			}
			present = false;
		}

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM)
		/*
		 * The accelerometer's whole cost at runtime: one atomic read per
		 * tick. The GPIO callback that sets it does nothing but set it,
		 * so nothing here runs in interrupt context and nothing competes
		 * with the ranging arm deadline.
		 *
		 * Deliberately after the approach switch: a strike is a report
		 * about the DOOR, and must not be able to influence whether this
		 * tick unlocked. Tamper is a signal to surface, not an input to
		 * the grant.
		 */
		switch (ultrawidelock_slam_poll(&slam_cfg, &slam, ultrawidelock_slam_hw_take(), now)) {
		case ULTRAWIDELOCK_SLAM_TAMPER:
			LOG_WRN("tamper: repeated impacts on the door");
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
			/* A struck door while the bolt is thrown is what
			 * DoorForcedOpen means. The bolt test is applied by the
			 * Matter half, which owns that state. */
			door_alarm_tamper();
#endif
			break;
		case ULTRAWIDELOCK_SLAM_IMPACT:
			LOG_INF("impact");
			break;
		default:
			break;
		}
#endif

		/* Wake on the next latch, or on the housekeeping tick if none comes.
		 * A latch that lands while this pass is still running leaves the
		 * semaphore given, so the take returns at once and no range waits.
		 *
		 * The impact poll above deliberately sits BEFORE this: it now runs on
		 * every wake rather than on a fixed 250 ms cadence, so a busy walk-up
		 * polls it more often, never less. Its debounce is a time comparison,
		 * not a count of ticks, so a faster poll rate cannot make a single
		 * strike read as several. */
		(void)k_sem_take(&s_range_sig, K_MSEC(ULTRAWIDELOCK_TICK_MS));
	}
	return 0;
}

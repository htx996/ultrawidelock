/* SPDX-License-Identifier: ISC */

/**
 * @file main.c — nRF52840 BLE witness: hears the room, tells the lock, decides
 *       nothing.
 *
 * ONE IMAGE for every mounting position. The old firmware baked the role in at
 * build time (WITNESS_ROLE=inside|outside|threshold) and needed a `LEARN` pass
 * and an `ADDR` push from a host on every session. All three are gone:
 *
 *   - role, keys and the Thread dataset are provisioned once and persist
 *   - nothing is learned, because nothing is filtered: the witness reports the
 *     loudest advertisers it heard and the LOCK works out which is the phone
 *   - reports ride Thread, so no probe, no USB host, no per-session anything
 *
 * WHY THE FILTER IS GONE, and it is the load-bearing change. The lock cannot
 * tell a witness which advertiser to watch, because the lock does not know: it
 * holds the credential connection's InitA, generated for the initiating role,
 * while what this firmware hears comes from advertising sets with their own
 * addresses and rotation timers. Matching those two would fail in the ordinary
 * case. So the lock correlates advertiser RSSI against its own authenticated
 * UWB range instead (ultrawidelock_witness_pick.h), and this firmware's job is
 * to report honestly and rank sensibly.
 *
 * WHAT LEAVES THIS BOARD. Never an address. Each advertiser is labelled
 * trunc24(CMAC(group_key, addr)) under a key shared by the WITNESSES and not
 * held by the lock, so the same phone carries the same label at both witnesses
 * -- which is what lets inside be compared against outside -- while the label
 * is opaque to the lock and to anyone listening.
 *
 * AUTHORITY: none. Reports are sealed for integrity and freshness, not because
 * this board is trusted. Every rule is enforced at the lock; the worst a
 * compromised witness achieves is a door that will not open passively.
 *
 * Provisioning (once, over USB CDC, before the dongle goes on the wall):
 *   PROV <role> <link-key-hex32> <group-key-hex32> <dataset-hex>
 *   SHOW | WIPE | HELP
 * Steady state needs no host and no commands.
 *
 * LED: fast blink = unprovisioned; slow blink = provisioned, Thread not
 * attached; solid = attached and reporting.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#if IS_ENABLED(CONFIG_WITNESS_BOOT_TRACE)
void witness_boot_trace_main(void); /* src/boot_trace.c, bench only */
void witness_boot_trace_phase(unsigned int n); /* src/boot_trace.c, bench only */
#define TRACE_PHASE(n) witness_boot_trace_phase(n)
#else
#define TRACE_PHASE(n) ((void)0)
#endif

/*
 * The bring-up half only: a dataset, a device role and otIp6SetEnabled are
 * OpenThread's own lifecycle. Moving bytes is ultrawidelock_dgram.h's job, and
 * the socket calls left this file with it.
 */
#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <zephyr/net/openthread.h>

#include "ultrawidelock_dgram.h"
#include "ultrawidelock_kv.h"

#include <psa/crypto.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultrawidelock_witness_core.h"
#include "ultrawidelock_witness_msg.h"

LOG_MODULE_REGISTER(witness, LOG_LEVEL_INF);

#define WINDOW_MS      CONFIG_WITNESS_WINDOW_MS
#define WITNESS_PORT   CONFIG_WITNESS_PORT
/* The longest challenge: version, 8 nonce bytes, and the lock's picked-label
 * trailer. Named now that the datagram arrives as a pointer and a length rather
 * than as a local array whose sizeof said the same thing. */
#define CHALLENGE_MAX  12u
#define KEY_LEN        16u
#define CCM_TAG_LEN    8u
#define CCM_NONCE_LEN  13u
#define DATASET_MAX    254u
#define CMD_MAX        640u
#define MIN_PKTS       2u

/* Persisted once at provisioning; nothing here identifies a phone. */
static struct {
	uint8_t role; /* enum ultrawidelock_witness_role */
	uint8_t link_key[KEY_LEN];
	uint8_t group_key[KEY_LEN];
	uint8_t dataset[DATASET_MAX];
	uint8_t dataset_len;
	bool have_role;
	bool have_link;
	bool have_group;
} s_prov;

static struct ultrawidelock_witness_core s_core;
static struct k_mutex s_core_lock;
static uint32_t s_boot_id;
static uint32_t s_ctr;
static uint64_t s_nonce; /* newest challenge heard from the lock */
static uint32_t s_hint;  /* label the lock asked to always include; 0 = none */
static bool s_attached;

static struct gpio_dt_spec s_leds[2];
static uint8_t s_led_n;

static bool provisioned(void)
{
	return s_prov.have_role && s_prov.have_link && s_prov.have_group &&
	       s_prov.dataset_len > 0u;
}

/* ---- LEDs ------------------------------------------------------------- */

static void led_try_add(const struct gpio_dt_spec *led)
{
	if (s_led_n >= ARRAY_SIZE(s_leds) || !gpio_is_ready_dt(led)) {
		return;
	}
	if (gpio_pin_configure_dt(led, GPIO_OUTPUT_ACTIVE) != 0) {
		return;
	}
	s_leds[s_led_n++] = *led;
}

static void led_init(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
	{
		static const struct gpio_dt_spec l = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

		led_try_add(&l);
	}
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(led1_green), okay)
	{
		static const struct gpio_dt_spec l =
			GPIO_DT_SPEC_GET(DT_ALIAS(led1_green), gpios);

		led_try_add(&l);
	}
#elif DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay)
	{
		static const struct gpio_dt_spec l = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

		led_try_add(&l);
	}
#endif
}

static void led_set(int on)
{
	for (uint8_t i = 0; i < s_led_n; i++) {
		(void)gpio_pin_set_dt(&s_leds[i], on);
	}
}

/* Three states, distinguishable across a room: the installer has no console
 * once the dongle is on the wall, so the LED is the only diagnostic. */
static void led_tick(void)
{
	static int64_t last;
	static bool on;
	int64_t period;
	int64_t now = k_uptime_get();

	if (s_led_n == 0u) {
		return;
	}
	if (s_attached) {
		if (!on) {
			on = true;
			led_set(1);
		}
		return;
	}
	period = provisioned() ? 700 : 150;
	if ((now - last) < period) {
		return;
	}
	last = now;
	on = !on;
	led_set(on ? 1 : 0);
}

/* ---- crypto ----------------------------------------------------------- */

/*
 * The label. CMAC rather than a plain hash because the point is that only a
 * holder of the group key can compute it: a truncated unkeyed hash of an
 * address is trivially reversed by anyone who can enumerate addresses, and
 * addresses are what this whole design refuses to disclose.
 */
static bool label_of(const bt_addr_le_t *addr, uint32_t *out)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = PSA_KEY_ID_NULL;
	uint8_t mac[16];
	uint8_t in[7];
	size_t mac_len = 0;
	psa_status_t st;

	if (addr == NULL || out == NULL) {
		return false;
	}
	in[0] = addr->type;
	memcpy(&in[1], addr->a.val, 6);

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, KEY_LEN * 8u);
	if (psa_import_key(&attr, s_prov.group_key, KEY_LEN, &key) != PSA_SUCCESS) {
		return false;
	}
	st = psa_mac_compute(key, PSA_ALG_CMAC, in, sizeof(in), mac, sizeof(mac), &mac_len);
	(void)psa_destroy_key(key);
	if (st != PSA_SUCCESS || mac_len < 3u) {
		return false;
	}
	*out = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | (uint32_t)mac[2];
	return true;
}

/* Wire layout is nonce || ciphertext || tag; the lock reads the nonce off the
 * front. It is not secret -- it is a counter and a boot id -- and it must be
 * on the wire because the lock cannot reconstruct a counter it has not seen. */
static size_t seal(const uint8_t *plain, size_t plain_len, uint8_t *out, size_t cap)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CCM_TAG_LEN);
	size_t ct_len = 0;
	psa_status_t st;

	if (cap < CCM_NONCE_LEN + plain_len + CCM_TAG_LEN) {
		return 0;
	}
	memset(out, 0, CCM_NONCE_LEN);
	out[0] = s_prov.role;
	out[1] = (uint8_t)(s_boot_id >> 24);
	out[2] = (uint8_t)(s_boot_id >> 16);
	out[3] = (uint8_t)(s_boot_id >> 8);
	out[4] = (uint8_t)s_boot_id;
	out[5] = (uint8_t)(s_ctr >> 24);
	out[6] = (uint8_t)(s_ctr >> 16);
	out[7] = (uint8_t)(s_ctr >> 8);
	out[8] = (uint8_t)s_ctr;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, alg);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, KEY_LEN * 8u);
	if (psa_import_key(&attr, s_prov.link_key, KEY_LEN, &key) != PSA_SUCCESS) {
		return 0;
	}
	st = psa_aead_encrypt(key, alg, out, CCM_NONCE_LEN, NULL, 0, plain, plain_len,
			      out + CCM_NONCE_LEN, cap - CCM_NONCE_LEN, &ct_len);
	(void)psa_destroy_key(key);
	if (st != PSA_SUCCESS) {
		return 0;
	}
	return CCM_NONCE_LEN + ct_len;
}

/* ---- Thread ----------------------------------------------------------- */

static void report_send(const uint8_t *buf, size_t len)
{
	/*
	 * To the group. The witness is not told the lock's address at
	 * provisioning, so it does not have to be re-provisioned when the lock is
	 * replaced or its address changes; only a holder of the link key can
	 * produce a report, so the broadcast costs nothing but a frame.
	 *
	 * This used to reach OpenThread without taking openthread_mutex, alone
	 * among the three senders on this link. The seam takes it on every send,
	 * so the omission is fixed by moving rather than by remembering.
	 */
	(void)ultrawidelock_dgram_send(buf, len);
}

/* The challenge. Unauthenticated by design: it is a freshness beacon, not a
 * command, and echoing a wrong one costs a clear rather than granting one. */
static void link_rx(void *ctx, const uint8_t *body, size_t len)
{
	ARG_UNUSED(ctx);

	/* 9 B is the bare challenge; 12 B carries the lock's picked label as a
	 * trailer, to be kept in every report while the pick lasts (see
	 * ultrawidelock_witness_core_include). Either length is a valid
	 * challenge, and a bare one clears any standing hint -- the lock
	 * sends the trailer on every challenge for as long as it has a pick,
	 * so a missing trailer means the pick is gone, not lost in the air.
	 *
	 * The datagram arrives flattened, so the otMessage offset arithmetic this
	 * function used to open with is gone; the two exact lengths are this
	 * protocol's own rule and stay. */
	if (len != 9u && len != CHALLENGE_MAX) {
		return;
	}
	if (body[0] != ULTRAWIDELOCK_WITNESS_MSG_VER) {
		return;
	}
	s_nonce = 0u;
	for (int i = 0; i < 8; i++) {
		s_nonce = (s_nonce << 8) | body[1 + i];
	}
	s_hint = (len == CHALLENGE_MAX)
			 ? (((uint32_t)body[9] << 16) | ((uint32_t)body[10] << 8) | body[11])
			 : 0u;
}

static void ot_state_changed(otChangedFlags flags, void *context)
{
	otInstance *ot = (otInstance *)context;
	otDeviceRole role;

	if ((flags & OT_CHANGED_THREAD_ROLE) == 0u) {
		return;
	}
	role = otThreadGetDeviceRole(ot);
	s_attached = (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
		      role == OT_DEVICE_ROLE_LEADER);
	LOG_INF("thread role=%d attached=%d", (int)role, (int)s_attached);
}

static void thread_start(void)
{
	otInstance *ot = openthread_get_default_instance();
	otOperationalDatasetTlvs tlvs;

	if (ot == NULL || s_prov.dataset_len == 0u) {
		return;
	}
	memset(&tlvs, 0, sizeof(tlvs));
	tlvs.mLength = s_prov.dataset_len;
	memcpy(tlvs.mTlvs, s_prov.dataset, s_prov.dataset_len);

	openthread_mutex_lock();
	if (otDatasetSetActiveTlvs(ot, &tlvs) != OT_ERROR_NONE) {
		openthread_mutex_unlock();
		LOG_ERR("dataset rejected; not joining");
		return;
	}
	(void)otSetStateChangedCallback(ot, ot_state_changed, ot);
	(void)otIp6SetEnabled(ot, true);
	(void)otThreadSetEnabled(ot, true);
	openthread_mutex_unlock();

	/* Outside the lock, because the transport takes that lock itself and the
	 * caller holding it first would be holding it twice. */
	if (ultrawidelock_dgram_open(WITNESS_PORT, link_rx, NULL) != ULTRAWIDELOCK_DGRAM_OK) {
		LOG_ERR("could not open port %u", (unsigned)WITNESS_PORT);
	}
}

/* ---- scanning and windows --------------------------------------------- */

#if IS_ENABLED(CONFIG_BT)
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		    struct net_buf_simple *ad)
{
	uint32_t label;

	ARG_UNUSED(type);
	ARG_UNUSED(ad);

	/* The payload is deliberately not examined. The old firmware
	 * fingerprinted it, which is why it needed a LEARN pass and why an
	 * Apple payload rotating mid-window looked like a new device. */
	if (!provisioned() || !label_of(addr, &label)) {
		return;
	}
	k_mutex_lock(&s_core_lock, K_FOREVER);
	ultrawidelock_witness_core_note(&s_core, label, rssi);
	k_mutex_unlock(&s_core_lock);
}
#endif /* CONFIG_BT */

static void window_close_and_send(void)
{
	struct ultrawidelock_witness_msg wm;
	uint8_t plain[ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN];
	uint8_t sealed[CCM_NONCE_LEN + ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN + CCM_TAG_LEN];
	size_t plain_len, sealed_len;
	uint8_t n;

	memset(&wm, 0, sizeof(wm));
	wm.ver = ULTRAWIDELOCK_WITNESS_MSG_VER;
	wm.role = s_prov.role;
	wm.boot_id = s_boot_id;
	wm.ctr = ++s_ctr;
	wm.echo_nonce = s_nonce;
	wm.window_ms = WINDOW_MS;

	k_mutex_lock(&s_core_lock, K_FOREVER);
	n = ultrawidelock_witness_core_summarize(&s_core, &wm, MIN_PKTS);
	if (s_hint != 0u && ultrawidelock_witness_core_include(&s_core, &wm, s_hint)) {
		n = wm.n_tuples;
	}
	ultrawidelock_witness_core_open(&s_core);
	k_mutex_unlock(&s_core_lock);

	/*
	 * An empty window is still sent. Silence is evidence the lock needs:
	 * it is how a witness that is alive but hearing nothing is told apart
	 * from one that has died, and those two must not look the same to a
	 * gate that fails closed on staleness.
	 */
	plain_len = ultrawidelock_witness_msg_encode(&wm, plain, sizeof(plain));
	if (plain_len == 0u) {
		return;
	}
	sealed_len = seal(plain, plain_len, sealed, sizeof(sealed));
	if (sealed_len == 0u) {
		LOG_ERR("report could not be sealed");
		return;
	}
	report_send(sealed, sealed_len);
	LOG_DBG("report ctr=%u tuples=%u", (unsigned)wm.ctr, (unsigned)n);
}

/* ---- persistent provisioning ----------------------------------------- */

static void provisioning_load(void)
{
	size_t len = sizeof(s_prov.role);

	if (ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_ROLE,
				 &s_prov.role, &len) == ULTRAWIDELOCK_KV_OK &&
	    len == sizeof(s_prov.role)) {
		s_prov.have_role = true;
	}
	len = sizeof(s_prov.link_key);
	if (ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_LINK_KEY,
				 s_prov.link_key, &len) == ULTRAWIDELOCK_KV_OK &&
	    len == sizeof(s_prov.link_key)) {
		s_prov.have_link = true;
	}
	len = sizeof(s_prov.group_key);
	if (ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_GROUP_KEY,
				 s_prov.group_key, &len) == ULTRAWIDELOCK_KV_OK &&
	    len == sizeof(s_prov.group_key)) {
		s_prov.have_group = true;
	}
	len = sizeof(s_prov.dataset);
	if (ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_DATASET,
				 s_prov.dataset, &len) == ULTRAWIDELOCK_KV_OK &&
	    len > 0u && len <= sizeof(s_prov.dataset)) {
		s_prov.dataset_len = (uint8_t)len;
	}
}

static int unhex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = strlen(hex);
	size_t bytes = n / 2u;

	if ((n % 2u) != 0u || bytes == 0u || bytes > cap) {
		return -1;
	}
	for (size_t i = 0; i < bytes; i++) {
		char b[3] = {hex[2 * i], hex[2 * i + 1], '\0'};
		char *end;
		long v = strtol(b, &end, 16);

		if (*end != '\0' || v < 0 || v > 255) {
			return -1;
		}
		out[i] = (uint8_t)v;
	}
	return (int)bytes;
}

static uint8_t role_of(const char *s)
{
	if (strcmp(s, "inside") == 0) {
		return ULTRAWIDELOCK_WITNESS_ROLE_INSIDE;
	}
	if (strcmp(s, "outside") == 0) {
		return ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE;
	}
	if (strcmp(s, "threshold") == 0) {
		return ULTRAWIDELOCK_WITNESS_ROLE_THRESHOLD;
	}
	return ULTRAWIDELOCK_WITNESS_ROLE_UNKNOWN;
}

static const char *role_name(uint8_t r)
{
	switch (r) {
	case ULTRAWIDELOCK_WITNESS_ROLE_INSIDE:
		return "inside";
	case ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE:
		return "outside";
	case ULTRAWIDELOCK_WITNESS_ROLE_THRESHOLD:
		return "threshold";
	default:
		return "unset";
	}
}

static void cmd_prov(char *args)
{
	char *role_s = strtok(args, " \t");
	char *lk_s = strtok(NULL, " \t");
	char *gk_s = strtok(NULL, " \t");
	char *ds_s = strtok(NULL, " \t");
	uint8_t lk[KEY_LEN], gk[KEY_LEN], ds[DATASET_MAX];
	uint8_t role;
	int dsn;

	if (role_s == NULL || lk_s == NULL || gk_s == NULL || ds_s == NULL) {
		printk("PROV err=args\n");
		return;
	}
	role = role_of(role_s);
	if (role == ULTRAWIDELOCK_WITNESS_ROLE_UNKNOWN) {
		printk("PROV err=role\n");
		return;
	}
	if (unhex(lk_s, lk, KEY_LEN) != (int)KEY_LEN || unhex(gk_s, gk, KEY_LEN) != (int)KEY_LEN) {
		printk("PROV err=key\n");
		return;
	}
	dsn = unhex(ds_s, ds, DATASET_MAX);
	if (dsn <= 0) {
		printk("PROV err=dataset\n");
		return;
	}

	if (ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_ROLE,
				 &role, sizeof(role)) != ULTRAWIDELOCK_KV_OK ||
	    ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_LINK_KEY,
				 lk, sizeof(lk)) != ULTRAWIDELOCK_KV_OK ||
	    ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_GROUP_KEY,
				 gk, sizeof(gk)) != ULTRAWIDELOCK_KV_OK ||
	    ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_DATASET,
				 ds, (size_t)dsn) != ULTRAWIDELOCK_KV_OK) {
		printk("PROV err=store\n");
		return;
	}
	printk("PROV ok role=%s ds=%d reboot\n", role_name(role), dsn);
}

static void cmd_wipe(void)
{
	(void)ultrawidelock_kv_delete(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_ROLE);
	(void)ultrawidelock_kv_delete(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_LINK_KEY);
	(void)ultrawidelock_kv_delete(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_GROUP_KEY);
	(void)ultrawidelock_kv_delete(ULTRAWIDELOCK_KV_KEY_LINK_BLE_WITNESS_DATASET);
	printk("WIPE ok reboot\n");
}

static void handle_cmd(char *line)
{
	while (*line && isspace((unsigned char)*line)) {
		line++;
	}
	if (*line == '\0') {
		return;
	}
	if (strncmp(line, "PROV", 4) == 0) {
		cmd_prov(line + 4);
		return;
	}
	if (strncmp(line, "WIPE", 4) == 0) {
		cmd_wipe();
		return;
	}
	if (strncmp(line, "SHOW", 4) == 0) {
		/* Deliberately prints no key material: a provisioning console
		 * that echoes secrets turns one careless capture into a
		 * permanent compromise of the witness link. */
		printk("SHOW role=%s prov=%d ds=%u attached=%d ctr=%u\n",
		       role_name(s_prov.role), provisioned() ? 1 : 0,
		       (unsigned)s_prov.dataset_len, (int)s_attached, (unsigned)s_ctr);
		return;
	}
	printk("cmds: PROV <role> <lk-hex32> <gk-hex32> <ds-hex> | SHOW | WIPE | HELP\n");
}

/*
 * Console input over the UART IRQ into a message queue, rather than a blocking
 * console read: provisioning is a once-ever event and the reporting cadence
 * must not depend on whether a host is attached. Lines are assembled in the
 * ISR and consumed by the main loop.
 */
static struct k_msgq s_uart_msgq;
static char __aligned(4) s_uart_msgq_buf[CMD_MAX * 2];

static void uart_rx_feed(unsigned char c)
{
	static char line[CMD_MAX];
	static size_t n;

	if (c == '\r' || c == '\n') {
		line[n] = '\0';
		if (n > 0u) {
			(void)k_msgq_put(&s_uart_msgq, line, K_NO_WAIT);
		}
		n = 0u;
		return;
	}
	if (n < sizeof(line) - 1u) {
		line[n++] = (char)c;
	} else {
		/* Overlong line: drop it whole rather than acting on a
		 * truncated dataset, which would provision a witness onto a
		 * network it can never attach to. */
		n = 0u;
	}
}

static void uart_cb(const struct device *dev, void *user_data)
{
	unsigned char c;

	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
		return;
	}
	while (uart_fifo_read(dev, &c, 1) == 1) {
		uart_rx_feed(c);
	}
}

static void console_init_uart(const struct device *uart)
{
	if (uart == NULL || !device_is_ready(uart)) {
		return;
	}
	k_msgq_init(&s_uart_msgq, s_uart_msgq_buf, CMD_MAX,
		    sizeof(s_uart_msgq_buf) / CMD_MAX);
	uart_irq_callback_user_data_set(uart, uart_cb, NULL);
	uart_irq_rx_enable(uart);
}

static void console_poll(void)
{
	static char line[CMD_MAX];

	while (k_msgq_get(&s_uart_msgq, line, K_NO_WAIT) == 0) {
		handle_cmd(line);
	}
}

/* ---- entry ------------------------------------------------------------ */

int main(void)
{
#if IS_ENABLED(CONFIG_BT)
	static const struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
#endif
	int64_t next_window;

#if IS_ENABLED(CONFIG_WITNESS_BOOT_TRACE)
	/* Before anything else in main, and before led_init() takes the same
	 * pins: the question this answers is whether main was reached at all. */
	witness_boot_trace_main();
	k_sleep(K_SECONDS(3)); /* long enough to read the pattern by eye */
#endif
	k_mutex_init(&s_core_lock);
	led_init();
	console_init_uart(DEVICE_DT_GET(DT_CHOSEN(zephyr_console)));

	if (psa_crypto_init() != PSA_SUCCESS) {
		LOG_ERR("PSA init failed; this board cannot seal a report");
	}
	sys_rand_get(&s_boot_id, sizeof(s_boot_id));

	(void)ultrawidelock_kv_init();
	provisioning_load();
	/* OpenThread owns settings records outside this application's KV window.
	 * Keep loading those registered handlers; provisioning no longer registers
	 * one of its own. */
	(void)settings_load();

	printk("witness role=%s provisioned=%d\n", role_name(s_prov.role),
	       provisioned() ? 1 : 0);
	TRACE_PHASE(1);

#if !IS_ENABLED(CONFIG_OPENTHREAD_SYS_INIT)
	/* DIAGNOSTIC BUILD ONLY (overlay-otmain.conf). With OPENTHREAD_SYS_INIT
	 * off, nothing has created the OpenThread instance and main() must do
	 * it. That is the entire point: called from here the console already
	 * exists, so a stall inside otSysInit is a printed line rather than a
	 * dead board. The sleep is for the USB host, which needs a moment to
	 * enumerate and open the port before anything printed can be seen. */
	{
		int ot_rc;

		k_sleep(K_SECONDS(5));
		printk("openthread_init: calling\n");
		TRACE_PHASE(2);
		ot_rc = openthread_init();
		TRACE_PHASE(3);
		printk("openthread_init: returned %d\n", ot_rc);
	}
#endif

#if IS_ENABLED(CONFIG_BT)
	if (bt_enable(NULL) != 0) {
		LOG_ERR("bluetooth would not start");
	} else if (bt_le_scan_start(&scan_param, scan_cb) != 0) {
		LOG_ERR("scan would not start");
	}
#else
	/* DIAGNOSTIC BUILD ONLY (overlay-nobt.conf). A witness that does not
	 * scan cannot witness anything; this exists to answer one question --
	 * whether the boot survives without the BLE controller -- and the
	 * answer is read off the LED, not off any report. */
	LOG_WRN("built with CONFIG_BT=n: this image reports nothing");
#endif
	TRACE_PHASE(4);

	if (provisioned()) {
		thread_start();
	} else {
		/* No dataset, no keys: joining would scan a radio BLE is
		 * sharing, forever, for a network it cannot authenticate to. */
		LOG_WRN("not provisioned; scanning only, reporting nothing");
	}

	TRACE_PHASE(5);
	ultrawidelock_witness_core_open(&s_core);
	next_window = k_uptime_get() + WINDOW_MS;

	while (1) {
		int64_t now = k_uptime_get();

		console_poll();
		led_tick();
		if (provisioned() && now >= next_window) {
			next_window = now + WINDOW_MS;
			window_close_and_send();
		}
		k_sleep(K_MSEC(20));
	}
	return 0;
}

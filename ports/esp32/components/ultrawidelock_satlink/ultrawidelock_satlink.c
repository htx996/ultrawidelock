/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_satlink.c — the sealed anchor link's ESP-NOW carrier.
 *
 * Bytes and key storage. Every decision is ultrawidelock_link.c's, shared with
 * the Thread port, so the two carriers cannot come to different conclusions
 * about the same datagram.
 *
 * BROADCAST, NOT A UNICAST PEER. Both directions go to the ESP-NOW broadcast
 * address, matching the Thread port's mesh-local all-nodes for the same reason:
 * neither board is ever told the other's address, so replacing one, or reusing
 * these boards on a different door, costs no re-provisioning. Only a holder of
 * the link key can produce or read anything, so the broadcast costs a frame and
 * reveals a distance to nobody who could not already measure one.
 *
 * WHAT A LISTENER LEARNS ANYWAY. ESP-NOW frames carry a source MAC in the
 * clear, and the sealed payload's length says which of the two messages it is.
 * So an observer learns that two boards are exchanging something and how often.
 * That is the same exposure the Thread port has and is not what the seal is
 * for; the seal keeps the distance, the URSK and the schedule.
 */

#include "ultrawidelock_satlink.h"

#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "satlink";

/*
 * THE FRAME MUST FIT THE CARRIER, checked here where the carrier is chosen.
 * The largest thing this link sends is the WV4 handoff at
 * ULTRAWIDELOCK_LINK_MAX_FRAME bytes. If that ever outgrew ESP-NOW's payload,
 * the symptom would be a session handoff that silently never arrives -- a
 * satellite that ranges perfectly and joins nothing -- so it fails the build
 * instead.
 */
_Static_assert(ULTRAWIDELOCK_LINK_MAX_FRAME <= ESP_NOW_MAX_DATA_LEN,
	       "sealed frame is larger than one ESP-NOW payload");

/* NVS names. Both are capped at 15 characters by ESP-IDF and both are declared
 * in PORTING.md's storage table; the asserts below fail the build rather than
 * let a too-long name become a silent "never stored" at run time. */
#define SATLINK_NS  "satlink"
#define SATLINK_KEY "lk"
_Static_assert(sizeof(SATLINK_NS) - 1 <= NVS_NS_NAME_MAX_SIZE - 1,
	       "NVS namespace name is longer than NVS allows (NVS_NS_NAME_MAX_SIZE - 1)");
_Static_assert(sizeof(SATLINK_KEY) - 1 <= NVS_KEY_NAME_MAX_SIZE - 1,
	       "NVS key name is longer than NVS allows (NVS_KEY_NAME_MAX_SIZE - 1)");

static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static struct ultrawidelock_link s_link;
static bool s_up;
static ultrawidelock_satlink_report_cb s_report_cb;
static ultrawidelock_satlink_join_cb s_join_cb;

void ultrawidelock_satlink_set_report_cb(ultrawidelock_satlink_report_cb cb)
{
	s_report_cb = cb;
}

void ultrawidelock_satlink_set_join_cb(ultrawidelock_satlink_join_cb cb)
{
	s_join_cb = cb;
}

bool ultrawidelock_satlink_ready(void)
{
	return s_up && ultrawidelock_link_ready(&s_link);
}

/** Load the stored link key, if there is one. Absence is not an error. */
static void key_load(void)
{
	uint8_t key[ULTRAWIDELOCK_SEAL_KEY_LEN];
	size_t len = sizeof(key);
	nvs_handle_t h;

	if (nvs_open(SATLINK_NS, NVS_READONLY, &h) != ESP_OK) {
		return;
	}
	if (nvs_get_blob(h, SATLINK_KEY, key, &len) == ESP_OK) {
		(void)ultrawidelock_link_set_key(&s_link, key, len);
	}
	nvs_close(h);
	memset(key, 0, sizeof(key));
}

int ultrawidelock_satlink_set_key(const uint8_t *key, size_t len)
{
	nvs_handle_t h;
	esp_err_t e;

	/* Install first: a key the link refuses must never reach flash, or the
	 * next boot loads something that cannot seal and says nothing about it. */
	if (ultrawidelock_link_set_key(&s_link, key, len) != 0) {
		ESP_LOGE(TAG, "link key must be %u bytes", (unsigned)ULTRAWIDELOCK_SEAL_KEY_LEN);
		return -1;
	}
	if (nvs_open(SATLINK_NS, NVS_READWRITE, &h) != ESP_OK) {
		return -1;
	}
	e = nvs_set_blob(h, SATLINK_KEY, key, len);
	if (e == ESP_OK) {
		e = nvs_commit(h);
	}
	nvs_close(h);
	if (e != ESP_OK) {
		/* The key is live in RAM but will not survive a reboot. Say so:
		 * a link that works until the next power cut is worse to debug
		 * than one that never worked. */
		ESP_LOGE(TAG, "link key not persisted (%s); it will be lost on reboot",
			 esp_err_to_name(e));
		return -1;
	}
	return 0;
}

/** Hand one sealed frame to the carrier. */
static void tx(const uint8_t *buf, size_t len)
{
	esp_err_t e = esp_now_send(BCAST, buf, len);

	if (e != ESP_OK) {
		ESP_LOGW(TAG, "send failed (%s)", esp_err_to_name(e));
	}
}

/**
 * ESP-NOW receive. Runs on the Wi-Fi task, so it does the least it can: decide
 * and dispatch. The Thread port's equivalent runs on the OpenThread RX thread
 * and is arranged the same way.
 */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	struct ultrawidelock_anchor_msg am;
	struct ultrawidelock_join_msg jm;
	enum ultrawidelock_link_rx rx;

	(void)info;
	if (data == NULL || len <= 0) {
		return;
	}

	memset(&am, 0, sizeof(am));
	memset(&jm, 0, sizeof(jm));
	rx = ultrawidelock_link_consume(&s_link, data, (size_t)len,
					s_report_cb != NULL ? &am : NULL,
					s_join_cb != NULL ? &jm : NULL);

	switch (rx) {
	case ULTRAWIDELOCK_LINK_RX_REPORT:
		if (s_report_cb != NULL) {
			s_report_cb(am.role, am.peer_mm, am.ranging_block);
		}
		break;
	case ULTRAWIDELOCK_LINK_RX_JOIN:
		if (s_join_cb != NULL) {
			s_join_cb(jm.ursk, jm.rcfg, jm.channel, jm.sync_code_index);
		}
		break;
	case ULTRAWIDELOCK_LINK_RX_REPLAYED:
		/* Logged, because it is the one rejection that means something is
		 * being recorded and re-sent rather than merely misconfigured. */
		ESP_LOGW(TAG, "peer datagram replayed or stale; ignored");
		break;
	case ULTRAWIDELOCK_LINK_RX_UNSEALED:
		ESP_LOGW(TAG, "peer datagram failed the seal; ignored");
		break;
	case ULTRAWIDELOCK_LINK_RX_MALFORMED:
		/* Sealed under our key but the codec refused it: the two ends
		 * disagree about the format, which is a version skew, not an
		 * attack. Worth saying out loud -- both boards need reflashing. */
		ESP_LOGE(TAG, "peer datagram sealed but malformed: wire format skew");
		break;
	case ULTRAWIDELOCK_LINK_RX_CHALLENGE:
	case ULTRAWIDELOCK_LINK_RX_IGNORED:
	default:
		break;
	}

	/* jm carried a URSK if anything did. It does not outlive this frame. */
	memset(&jm, 0, sizeof(jm));
	memset(&am, 0, sizeof(am));
}

int ultrawidelock_satlink_init(uint8_t role)
{
	esp_now_peer_info_t peer;
	esp_err_t e;

	ultrawidelock_link_init(&s_link, role, esp_random());
	key_load();

	e = esp_netif_init();
	if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
		return e;
	}
	e = esp_event_loop_create_default();
	if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
		return e;
	}
	/*
	 * WHOEVER OWNS THE RADIO KEEPS IT. On the satellite this is the only
	 * user of Wi-Fi and the block below brings it up. On the Matter lock,
	 * Matter may already have initialised, configured and started it -- and
	 * forcing STA mode, RAM storage or a restart there would disconnect a
	 * commissioned node to deliver a distance report. ESP-NOW rides whatever
	 * interface is already up, so the right move is to touch nothing.
	 *
	 * esp_wifi_init returning ESP_ERR_INVALID_STATE is the signal that
	 * someone got here first; it is not an error and not our radio.
	 */
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	e = esp_wifi_init(&cfg);
	if (e == ESP_ERR_INVALID_STATE) {
		ESP_LOGI(TAG, "Wi-Fi already up; riding it rather than reconfiguring");
	} else if (e != ESP_OK) {
		return e;
	} else {
		/* Ours to configure. Storage in RAM so this leaves no Wi-Fi
		 * credentials in NVS beside the link key, and STA mode without
		 * ever associating: ESP-NOW needs the radio initialised, not a
		 * network. */
		(void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
		(void)esp_wifi_set_mode(WIFI_MODE_STA);
		e = esp_wifi_start();
		if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
			return e;
		}
	}

	e = esp_now_init();
	if (e != ESP_OK) {
		return e;
	}
	e = esp_now_register_recv_cb(on_recv);
	if (e != ESP_OK) {
		return e;
	}

	memset(&peer, 0, sizeof(peer));
	memcpy(peer.peer_addr, BCAST, ESP_NOW_ETH_ALEN);
	peer.channel = 0;  /* whatever channel the interface is on */
	peer.encrypt = false; /* our seal, not ESP-NOW's -- see the file header */
	e = esp_now_add_peer(&peer);
	if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
		return e;
	}

	s_up = true;
	if (!ultrawidelock_link_ready(&s_link)) {
		/* Loud on purpose. An anchor that ranges perfectly and reports
		 * nothing is indistinguishable from one that never booted. */
		ESP_LOGW(TAG, "no link key stored: ranging will work, reporting will not");
	} else {
		ESP_LOGI(TAG, "up, role %u", (unsigned)role);
	}
	return 0;
}

void ultrawidelock_satlink_report(int32_t peer_mm, uint32_t ranging_block)
{
	uint8_t frame[ULTRAWIDELOCK_LINK_MAX_FRAME];
	size_t n;

	if (!ultrawidelock_satlink_ready()) {
		return;
	}
	n = ultrawidelock_link_build_report(&s_link, peer_mm, ranging_block, frame, sizeof(frame));
	if (n != 0u) {
		tx(frame, n);
	}
}

void ultrawidelock_satlink_send_handoff(const uint8_t *ursk, const uint8_t *rcfg, uint8_t channel,
					uint8_t sync_code_index)
{
	uint8_t frame[ULTRAWIDELOCK_LINK_MAX_FRAME];
	size_t n;

	if (!ultrawidelock_satlink_ready()) {
		return;
	}
	n = ultrawidelock_link_build_join(&s_link, ursk, rcfg, channel, sync_code_index, frame,
					  sizeof(frame));
	if (n != 0u) {
		tx(frame, n);
	}
	/* The sealed handoff carries a URSK. Do not leave it on the stack. */
	memset(frame, 0, sizeof(frame));
}

void ultrawidelock_satlink_challenge(uint64_t nonce)
{
	uint8_t frame[ULTRAWIDELOCK_LINK_CHALLENGE_LEN];
	size_t n;

	if (!s_up) {
		return;
	}
	n = ultrawidelock_link_build_challenge(nonce, frame, sizeof(frame));
	if (n != 0u) {
		tx(frame, n);
	}
}

/* SPDX-License-Identifier: ISC */

/**
 * @file matter_thread_port.c — matter_thread.h on top of Zephyr's OpenThread.
 *
 * The dataset arrives from the commissioner as raw meshcop TLVs and
 * otDatasetSetActiveTlvs() takes raw meshcop TLVs, so nothing here has to
 * understand the format -- which is the point. This node parses exactly one
 * field out of it, the Extended PAN ID, and only so it can name the network
 * back to the commissioner.
 *
 * Built into every image. Without CONFIG_OPENTHREAD it refuses honestly
 * rather than disappearing: matter_clusters.c calls it unconditionally, and a
 * link error would be a worse way to learn that Thread was configured out.
 */
#include "matter_thread.h"

/* For MATTER_INSTANCE_NAME_LEN: the SRP instance name is sized by the thing
 * that produces it, matter_fabric_instance_name(). */
#include "matter_case.h"
#include "matter_clusters.h" /* MATTER_SUPPORTED_FABRICS */
#include "matter_fabric.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(matter_thread, CONFIG_ULTRAWIDELOCK_MATTER_BLE_LOG_LEVEL);

/* CONFIG_OPENTHREAD, not CONFIG_NET_L2_OPENTHREAD: every call below is either
 * OpenThread's own API or one of the four openthread_*() helpers that the
 * standalone module provides, so the Zephyr L2 was never what this needed. */
#if defined(CONFIG_OPENTHREAD)

#include <openthread.h>
#include <openthread/dataset.h>
#include <openthread/srp_client.h>
#include <openthread/thread.h>
#include <openthread/udp.h>

#include <psa/crypto.h>
#include <zephyr/settings/settings.h>

#include <stdio.h>
#include <string.h>

/** How often to look at the role while waiting. */
#define ATTACH_POLL_MS 250u

/**
 * A per-lifetime suffix on the SRP host name, and the reason it exists.
 *
 * Name ownership on the border router is first-come BY KEY. The SRP client's
 * ECDSA key lives in OpenThread's settings, a chip erase destroys it, and the
 * next boot then asks for a name the server still holds under the OLD key.
 * That is refused with OT_ERROR_DUPLICATED for as long as the KEY lease runs
 * -- 14 days at OpenThread's default -- and presents as a node that attaches to
 * Thread, never registers, and leaves the commissioner on "Adding to Home"
 * with nothing to say why. The bare EUI-64 is stable across an erase, which is
 * precisely what makes it collide with itself.
 *
 * This value dies in the same erase that takes the key, so a new key always
 * asks for a name nobody owns. The orphaned registration is left to expire on
 * its own: it costs a record on somebody else's server and nothing here.
 *
 * It is NOT under a tree the factory reset clears, so holding SW2 through
 * reset keeps the name it already published -- the key survives that too, so
 * there is nothing to dodge.
 */
#define SRP_HOST_ID_KEY "srp/hid"

/**
 * How long to ask the border router to hold the name against this key.
 *
 * OpenThread requests 14 days (OPENTHREAD_CONFIG_SRP_CLIENT_DEFAULT_KEY_LEASE),
 * which is how long a collision lasts if one ever happens anyway. An hour
 * bounds that, and costs a re-registration the client is already doing for the
 * 2-hour service lease. The server clamps this to its own limits and does not
 * report what it settled on, so it is a request, not a guarantee: the suffix
 * above is what PREVENTS a collision, this only shortens one.
 */
#define SRP_KEY_LEASE_S 3600u

int matter_thread_start(const uint8_t *dataset, size_t len)
{
	otInstance *ot = openthread_get_default_instance();
	otOperationalDatasetTlvs tlvs;
	otError err;

	if (ot == NULL || dataset == NULL || len == 0u || len > sizeof(tlvs.mTlvs)) {
		return MATTER_E_INVAL;
	}

	memcpy(tlvs.mTlvs, dataset, len);
	tlvs.mLength = (uint8_t)len;

	openthread_mutex_lock();
	err = otDatasetSetActiveTlvs(ot, &tlvs);
	openthread_mutex_unlock();
	if (err != OT_ERROR_NONE) {
		LOG_ERR("dataset rejected by OpenThread (%d)", err);
		return MATTER_E_INVAL;
	}

	/*
	 * openthread_run() takes the mutex itself, so it must be called with the
	 * lock released. It enables IPv6 and then the Thread interface, which is
	 * when attaching actually begins.
	 */
	if (openthread_run() != 0) {
		LOG_ERR("OpenThread refused to start");
		return MATTER_E_STATE;
	}

	LOG_INF("OpenThread started on the commissioner's dataset (%u B)", (unsigned int)len);
	return MATTER_OK;
}

int matter_thread_clear(void)
{
	otInstance *ot = openthread_get_default_instance();
	otOperationalDatasetTlvs empty = { 0 };
	otError thread_err;
	otError dataset_err;

	if (ot == NULL) {
		return MATTER_E_STATE;
	}

	/* Release SRP's references before disabling the interface it uses. */
	matter_thread_advertise_reset();
	openthread_mutex_lock();
	thread_err = otThreadSetEnabled(ot, false);
	dataset_err = otDatasetSetActiveTlvs(ot, &empty);
	openthread_mutex_unlock();

	if (thread_err != OT_ERROR_NONE || dataset_err != OT_ERROR_NONE) {
		LOG_ERR("failed to clear Thread state (%d, %d)", thread_err, dataset_err);
		return MATTER_E_STATE;
	}
	return MATTER_OK;
}

/**
 * Whether this node is already attached to the network @p xpanid names.
 * See matter_thread.h for why this exists.
 */
bool matter_thread_attached_to(const uint8_t *xpanid)
{
	otInstance *ot = openthread_get_default_instance();
	const otExtendedPanId *active;
	otDeviceRole role;
	bool same;

	if (ot == NULL || xpanid == NULL) {
		return false;
	}

	openthread_mutex_lock();
	role = otThreadGetDeviceRole(ot);
	active = otThreadGetExtendedPanId(ot);
	/* Compared under the lock: the pointer is into OpenThread's own state. */
	same = (active != NULL) && memcmp(active->m8, xpanid, MATTER_THREAD_XPANID_LEN) == 0;
	openthread_mutex_unlock();

	/* DETACHED and DISABLED both mean "not on it", whatever the stored id
	 * says; only the three attached roles count. */
	if (!same || (role != OT_DEVICE_ROLE_CHILD && role != OT_DEVICE_ROLE_ROUTER &&
		      role != OT_DEVICE_ROLE_LEADER)) {
		return false;
	}
	LOG_INF("already attached to the commissioner's Thread network; not restarting");
	return true;
}

/**
 * Is this an address something off the Thread mesh could route to?
 *
 * NOT a test for "is it global". A border router's off-mesh-routable prefix is
 * very often a unique-local one, indistinguishable from the mesh-local prefix
 * by its first byte -- an earlier version of this checked fc00::/7 and would
 * have reported a perfectly routable OMR address as unreachable. The only
 * sound test is against the mesh-local prefix this network actually uses.
 */
static bool addr_is_offmesh(otInstance *ot, const otNetifAddress *a)
{
	const otMeshLocalPrefix *ml = otThreadGetMeshLocalPrefix(ot);

	/* fe80::/10 */
	if (a->mAddress.mFields.m8[0] == 0xFEu && (a->mAddress.mFields.m8[1] & 0xC0u) == 0x80u) {
		return false;
	}
	if (ml != NULL && memcmp(a->mAddress.mFields.m8, ml->m8, OT_MESH_LOCAL_PREFIX_SIZE) == 0) {
		return false;
	}
	return true;
}

/**
 * Count the number of preferred off-mesh unicast addresses this node holds. Iterates over Thread
 * unicast addresses and counts those marked preferred that route to a destination not on the mesh.
 */
static int count_offmesh(otInstance *ot)
{
	const otNetifAddress *a;
	int n = 0;

	for (a = otIp6GetUnicastAddresses(ot); a != NULL; a = a->mNext) {
		if (a->mPreferred && addr_is_offmesh(ot, a)) {
			n++;
		}
	}
	return n;
}

/**
 * Every address this node holds, and whether any of them is reachable.
 *
 * A registered SRP name is not the same as a reachable node. Auto host address
 * mode publishes the PREFERRED unicast addresses, and falls back to the
 * mesh-local EID when there are none -- and a mesh-local address does not leave
 * the Thread mesh, so a commissioner on Wi-Fi resolves the name and then routes
 * nowhere. That failure is invisible from the SRP result, which is why it is
 * printed here instead of assumed.
 */
static void log_addresses(otInstance *ot)
{
	const otNetifAddress *a;
	char buf[OT_IP6_ADDRESS_STRING_SIZE];

	for (a = otIp6GetUnicastAddresses(ot); a != NULL; a = a->mNext) {
		otIp6AddressToString(&a->mAddress, buf, sizeof(buf));
		LOG_DBG("  addr %s  preferred=%d %s", buf, (int)a->mPreferred,
			addr_is_offmesh(ot, a) ? "(off-mesh)" : "(local)");
	}
	if (count_offmesh(ot) == 0) {
		LOG_WRN("NO off-mesh-routable address -- the border router is publishing no "
			"prefix this node can autoconfigure from");
	}
}

#if defined(CONFIG_ULTRAWIDELOCK_THREAD_DATASET_DUMP)
/*
 * The active dataset as hex, for `chip-tool pairing ble-thread`.
 *
 * WHY THIS IS HERE AT ALL. Commissioning a second Matter administrator onto
 * this node cannot happen over IP: the Thread receive path in
 * matter_commission.c answers CASE Sigma1/Sigma3 and drops everything else, so
 * PBKDFParamRequest never reaches the PASE responder and a controller that
 * found us over DNS-SD times out having sent five of them. BLE is the only
 * transport PASE runs on here, and chip-tool's BLE path insists on a dataset
 * argument. Passing anything other than the dataset already in force would move
 * the node to a different Thread network and take it out of its home, so the
 * node has to say which one that is.
 *
 * Printed in 32-byte lines because a full dataset is ~110 bytes and one log
 * message cannot carry 220 hex characters. Bare lines between the markers, no
 * prefix, so they concatenate without editing.
 */
void matter_thread_dump_active_dataset(void)
{
	otInstance *ot = openthread_get_default_instance();
	otOperationalDatasetTlvs tlvs;
	char line[65];
	otError err;

	if (ot == NULL) {
		return;
	}

	openthread_mutex_lock();
	err = otDatasetGetActiveTlvs(ot, &tlvs);
	openthread_mutex_unlock();

	if (err != OT_ERROR_NONE) {
		LOG_ERR("no active dataset to dump (%d)", err);
		return;
	}

	LOG_ERR("---- BEGIN THREAD DATASET (hex, %u B) -- CONTAINS THE NETWORK KEY ----",
		(unsigned int)tlvs.mLength);
	for (uint16_t off = 0u; off < tlvs.mLength; off += 32u) {
		uint16_t n = MIN((uint16_t)32u, (uint16_t)(tlvs.mLength - off));
		uint16_t i;

		for (i = 0u; i < n; i++) {
			(void)snprintf(&line[i * 2u], 3u, "%02x", tlvs.mTlvs[off + i]);
		}
		line[n * 2u] = '\0';
		LOG_ERR("%s", line);
	}
	LOG_ERR("---- END THREAD DATASET ----");
	LOG_ERR("join those lines; pass as hex:<joined> to chip-tool pairing ble-thread");
}
#else /* !CONFIG_ULTRAWIDELOCK_THREAD_DATASET_DUMP */

void matter_thread_dump_active_dataset(void)
{
}

#endif /* CONFIG_ULTRAWIDELOCK_THREAD_DATASET_DUMP */

int matter_thread_wait_attached(uint32_t timeout_ms)
{
	otInstance *ot = openthread_get_default_instance();
	uint32_t waited = 0u;
	bool announced = false;

	for (;;) {
		otDeviceRole role;
		int offmesh;

		openthread_mutex_lock();
		role = otThreadGetDeviceRole(ot);
		offmesh = count_offmesh(ot);
		openthread_mutex_unlock();

		if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
		    role == OT_DEVICE_ROLE_LEADER) {
			if (!announced) {
				LOG_INF("Thread attached after %u ms, role %d", waited, (int)role);
				announced = true;
			}
			/*
			 * Attached is NOT reachable. The off-mesh-routable
			 * address is autoconfigured from a prefix the border
			 * router publishes in network data, which arrives some
			 * time AFTER the attach -- and a node that registers SRP
			 * before it exists publishes only its mesh-local
			 * address, which no commissioner off the mesh can route
			 * to. Waiting here is what makes ConnectNetwork's
			 * Success mean something.
			 */
			if (offmesh > 0) {
				LOG_INF("reachable after %u ms (%d off-mesh address(es))", waited,
					offmesh);
				return MATTER_OK;
			}
		}
		if (waited >= timeout_ms) {
			if (announced) {
				LOG_WRN("attached but STILL no off-mesh address after %u ms",
					waited);
			} else {
				LOG_WRN("Thread still %s after %u ms",
					role == OT_DEVICE_ROLE_DETACHED ? "detached" : "disabled",
					waited);
			}
			openthread_mutex_lock();
			log_addresses(ot);
			openthread_mutex_unlock();
			return MATTER_E_TIMEOUT;
		}

		k_msleep(ATTACH_POLL_MS);
		waited += ATTACH_POLL_MS;
	}
}

/*
 * Everything the SRP client is handed must OUTLIVE the call. otSrpClientAddService()
 * links the service into a list it keeps and does not copy the strings, so a
 * stack buffer here would be a use-after-return that shows up as a garbled
 * service name on somebody else's border router.
 */
static char s_host_name[26]; /* 16 hex of EUI-64 + '-' + 8 hex of host id + NUL */
static char s_service_type[] = "_matter._tcp";
static char s_txt_sii[] = "500";
static char s_txt_sai[] = "300";

/**
 * One registration per fabric, because a node on two fabrics has two names.
 *
 * The instance name is derived from the compressed fabric id and this node's id
 * ON that fabric, so the second administrator resolving the first fabric's name
 * finds an address it cannot open a session to. A single slot here published
 * whichever fabric registered last and left the other unreachable.
 */
struct srp_reg {
	char instance_name[MATTER_INSTANCE_NAME_LEN];
	char subtype[19]; /* "_I" + 16 hex digits + NUL */
	const char *subtype_labels[2];
	otSrpClientService service;
	otDnsTxtEntry txt[2];
	enum {
		SRP_REG_FREE,
		SRP_REG_ADDING,
		SRP_REG_ACTIVE,
		SRP_REG_REMOVING,
		SRP_REG_FAILED,
	} state;
	bool desired;
};
static struct srp_reg s_regs[MATTER_SUPPORTED_FABRICS];

/** The SRP host is registered once, whatever number of services hang off it. */
static bool s_host_ready;

/**
 * Whether the commissionable service is registered.
 *
 * Declared up here with the host flag rather than beside the code that sets it,
 * because the two places that tear SRP down -- matter_thread_advertise_reset()
 * and the mid-retraction clear in srp_host_register() -- both drop this service
 * along with everything else and have to say so. A flag left true after its
 * service was removed makes the NEXT commissioning window silently invisible.
 */
static uint8_t s_comm_state;
static bool s_comm_desired;
static otSrpClientService s_comm_service;
static bool s_recovery_pending;
static uint32_t s_host_id;

/**
 * Settings callback to read a 32-bit host ID from persistent storage. Reads exactly
 * sizeof(uint32_t) bytes into the output parameter, returning 0 on all paths.
 */
static int host_id_read(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
			void *param)
{
	uint32_t *out = param;

	ARG_UNUSED(key);

	if (len == sizeof(*out)) {
		(void)read_cb(cb_arg, out, sizeof(*out));
	}
	return 0;
}

/**
 * The host-name suffix: read it, or mint one and keep it. See SRP_HOST_ID_KEY.
 *
 * Zero is the "not stored" marker, so it is never a valid id -- which costs one
 * value out of 2^32 and saves carrying a separate "have I got one" flag through
 * the settings backend.
 */
static uint32_t srp_host_id(void)
{
	if (s_host_id != 0u) {
		return s_host_id;
	}
	/* Idempotent, and this can run before anything else has needed
	 * settings. */
	(void)settings_subsys_init();
	(void)settings_load_subtree_direct(SRP_HOST_ID_KEY, host_id_read, &s_host_id);
	if (s_host_id == 0u) {
		/*
		 * PSA's, not otRandomNonCryptoGetUint32(): this runs on the
		 * Matter work queue without the OpenThread lock held. A CSPRNG
		 * for a name that gets published in the clear is not strictly
		 * required, but this codebase does not want two classes of
		 * random source, and a value
		 * drawn once per lifetime cannot be the wrong place to pay.
		 */
		do {
			if (psa_generate_random((uint8_t *)&s_host_id, sizeof(s_host_id)) != PSA_SUCCESS) {
				LOG_ERR("no RNG for the SRP host id");
				return 0u;
			}
		} while (s_host_id == 0u);
		if (settings_save_one(SRP_HOST_ID_KEY, &s_host_id, sizeof(s_host_id)) != 0) {
			/* Registration still works; it is the NEXT boot that
			 * would ask for a different name and orphan this one. */
			LOG_WRN("SRP host id not persisted");
		}
	}
	return s_host_id;
}

static otUdpSocket s_udp;
static bool s_udp_open;

/**
 * Whatever arrives on the operational port, reported and dropped.
 *
 * There is no CASE responder yet, so this cannot answer. What it can do is
 * prove the half that would otherwise be invisible: a datagram here means the
 * commissioner resolved this node's name through the border router and routed
 * to it. Without it, "stuck at connecting" cannot be told apart from a service
 * that was never registered.
 */
/** The peer of the datagram in flight; see matter_thread_peer_current(). */
static struct matter_thread_peer s_cur_peer;

/**
 * Copy the current inbound peer address and port to the caller's buffer. Called by Matter protocol
 * handlers to discover where a datagram came from. Returns without effect if out is NULL.
 */
void matter_thread_peer_current(struct matter_thread_peer *out)
{
	if (out == NULL) {
		return;
	}
	*out = s_cur_peer;
}

/**
 * Send a datagram to a peer over Thread UDP. Returns MATTER_E_STATE if the peer is invalid, the
 * address family is unsupported, the message buffer cannot be allocated, or the send fails;
 * MATTER_E_NOSPACE if the message buffer is full; MATTER_OK on success.
 */
int matter_thread_send_to(const struct matter_thread_peer *peer, const uint8_t *msg, size_t len)
{
	otInstance *ot = openthread_get_default_instance();
	otMessageInfo info;
	otMessage *out;

	if (peer == NULL || !peer->valid || msg == NULL || len == 0u || ot == NULL || !s_udp_open) {
		return MATTER_E_STATE;
	}

	memset(&info, 0, sizeof(info));
	memcpy(info.mPeerAddr.mFields.m8, peer->addr, sizeof(peer->addr));
	info.mPeerPort = peer->port;
	/*
	 * mSockAddr left unspecified so the stack picks a source address for
	 * this destination. The reply path can copy the one the request arrived
	 * on; there is no request here to copy from, and pinning the wrong
	 * source is how a datagram leaves and is never answered.
	 */

	out = otUdpNewMessage(ot, NULL);
	if (out == NULL) {
		LOG_ERR("no message buffer for an unsolicited send");
		return MATTER_E_NOSPACE;
	}
	if (otMessageAppend(out, msg, (uint16_t)len) != OT_ERROR_NONE ||
	    otUdpSend(ot, &s_udp, out, &info) != OT_ERROR_NONE) {
		/* otUdpSend takes ownership on success only. */
		otMessageFree(out);
		LOG_ERR("unsolicited %u B send failed", (unsigned int)len);
		return MATTER_E_STATE;
	}
	LOG_DBG("sent %u B unsolicited", (unsigned int)len);
	return MATTER_OK;
}

/**
 * OpenThread UDP RX callback. Reads the incoming datagram, logs its size, invokes
 * matter_thread_on_datagram to generate a reply, and sends the reply back to the peer. Uses static
 * buffers sized for Sigma3 (RX) and full subscription reports (reply) respectively. Temporarily
 * publishes the peer address so the Matter handler can discover where traffic arrived from.
 */
static void udp_rx(void *ctx, otMessage *msg, const otMessageInfo *info)
{
	/*
	 * Sized for a Sigma3, not a Sigma1. A Sigma1 is 196 bytes, but a Sigma3
	 * carries the initiator's whole certificate chain encrypted -- with an
	 * intermediate certificate present that is comfortably past 512, and the
	 * only symptom of an undersized buffer here is this function declining to
	 * look at the message that ends the handshake.
	 *
	 * Static because this runs on OpenThread's own thread, whose stack is one
	 * of the two things deliberately left un-shrunk.
	 */
	static uint8_t buf[MATTER_CASE_SIGMA3_MAX];
	/*
	 * NOT Sigma2. The largest thing this sends is a ReportData answering a
	 * subscription to the whole data model -- measured at 1479 B of payload
	 * on hardware against Sigma2's 494 -- plus both headers and the AEAD
	 * tag. Sized off the report buffer for that reason; while it was sized
	 * off Sigma2 the node built the report correctly and then could not
	 * copy it out, which reads in the log as "needs 1513 B, have 1024".
	 */
	static uint8_t reply[MATTER_THREAD_REPLY_MAX];
	otMessageInfo reply_info;
	otMessage *out;
	size_t reply_len;
	uint16_t len = otMessageGetLength(msg) - otMessageGetOffset(msg);

	ARG_UNUSED(ctx);

	LOG_DBG("UDP %u B on port %u from peer port %u", (unsigned int)len,
		(unsigned int)info->mSockPort, (unsigned int)info->mPeerPort);

	if (len > sizeof(buf)) {
		LOG_WRN("  too large to look at (%u B)", (unsigned int)len);
		return;
	}
	if (otMessageRead(msg, otMessageGetOffset(msg), buf, len) != len) {
		LOG_WRN("  could not be read out");
		return;
	}

	/*
	 * Published for the duration of the handler only. A subscription
	 * outlives the SubscribeRequest that created it and a report has to be
	 * addressed somewhere, so the handler takes a copy while this is live.
	 */
	memcpy(s_cur_peer.addr, info->mPeerAddr.mFields.m8, sizeof(s_cur_peer.addr));
	s_cur_peer.port = info->mPeerPort;
	s_cur_peer.valid = true;

	reply_len = matter_thread_on_datagram(buf, len, reply, sizeof(reply));

	s_cur_peer.valid = false;

	if (reply_len == 0u) {
		return;
	}

	/*
	 * Replying to where it came FROM, rather than to the address SRP
	 * published. They are the same today and need not be: a commissioner
	 * behind a border router reaches this node from whichever of its
	 * addresses routes, and answering anywhere else answers a different
	 * peer.
	 */
	memset(&reply_info, 0, sizeof(reply_info));
	reply_info.mPeerAddr = info->mPeerAddr;
	reply_info.mPeerPort = info->mPeerPort;
	reply_info.mSockAddr = info->mSockAddr;

	out = otUdpNewMessage(openthread_get_default_instance(), NULL);
	if (out == NULL) {
		LOG_ERR("  no message buffer for the reply");
		return;
	}
	if (otMessageAppend(out, reply, (uint16_t)reply_len) != OT_ERROR_NONE ||
	    otUdpSend(openthread_get_default_instance(), &s_udp, out, &reply_info) !=
		    OT_ERROR_NONE) {
		/* otUdpSend takes ownership on success only. */
		otMessageFree(out);
		LOG_ERR("  reply could not be sent");
		return;
	}
	LOG_DBG("  replied %u B", (unsigned int)reply_len);
}

/**
 * Every address this node holds, and whether any of them is reachable.
 *
 * A registered SRP name is not the same as a reachable node. Auto host address
 * mode publishes the PREFERRED unicast addresses, and falls back to the
 * mesh-local EID when there are none -- and a mesh-local address does not leave
 * the Thread mesh, so a commissioner on Wi-Fi resolves the name and then routes
 * nowhere. That failure is invisible from the SRP result, which is why it is
 * printed here instead of assumed.
 *
 * The address to look for is an off-mesh-routable one, which only exists if the
 * border router is publishing a prefix this node has picked up.
 */
#if defined(CONFIG_ULTRAWIDELOCK_SRP_DIAG)
/**
 * Whether auto-start ever FOUND a server, printed at the only moment that can change.
 *
 * srp_cb() below is the verdict on a registration that was sent. It says nothing
 * about one that was never sent, and the two failures are indistinguishable from
 * every other log this firmware prints: host and services sit at ToAdd either
 * way, and srp_cb() is simply never called, so the "SRP registration FAILED"
 * line reads as absent-because-fine rather than absent-because-nothing-happened.
 *
 * otSrpClientEnableAutoStartMode() picks its server out of Thread network data,
 * so network data changing is the only event that can turn "no server" into
 * "server". Hence OT_CHANGED_THREAD_NETDATA rather than a timer: a timer would
 * print the same answer repeatedly and still miss the transition.
 *
 * Runs on the OpenThread thread with the API lock already held, which is why
 * nothing here takes openthread_mutex_lock() -- the same reason thread_gate.c's
 * callback does not.
 */
static void srp_diag_state_changed(otChangedFlags flags, void *context)
{
	ARG_UNUSED(context);

	if ((flags & OT_CHANGED_THREAD_NETDATA) == 0u) {
		return;
	}

	otInstance *ot = openthread_get_default_instance();
	char buf[OT_IP6_ADDRESS_STRING_SIZE];
	const otSrpClientHostInfo *host;
	const otSockAddr *server;

	if (!otSrpClientIsRunning(ot)) {
		LOG_WRN("SRP diag: network data changed, client STILL NOT RUNNING -- "
			"auto-start has been offered no server by this network");
		return;
	}

	host = otSrpClientGetHostInfo(ot);
	server = otSrpClientGetServerAddress(ot);
	otIp6AddressToString(&server->mAddress, buf, sizeof(buf));
	LOG_INF("SRP diag: network data changed, client running against [%s]:%u, host state %d",
		buf, (unsigned int)server->mPort, host != NULL ? (int)host->mState : -1);
}

static struct openthread_state_changed_callback srp_diag_cb = {
	.otCallback = srp_diag_state_changed,
};

static bool s_srp_diag_registered;
#endif /* CONFIG_ULTRAWIDELOCK_SRP_DIAG */

static otError srp_host_register(otInstance *ot);
static void srp_host_name_build(otInstance *ot);
static void srp_recovery_work_fn(struct k_work *w);
#define SRP_DUPLICATE_RETRY_S 30u
static K_WORK_DELAYABLE_DEFINE(s_srp_recovery_work, srp_recovery_work_fn);

static void srp_mark_service(const otSrpClientService *service, uint8_t state)
{
	for (size_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		if (service == &s_regs[i].service) {
			s_regs[i].state = state;
			return;
		}
	}
	if (service == &s_comm_service) {
		s_comm_state = state;
	}
}

/** The SRP server's verdict, which otSrpClientAddService() cannot give. */
static void srp_cb(otError err, const otSrpClientHostInfo *host, const otSrpClientService *services,
		   const otSrpClientService *removed, void *ctx)
{
	ARG_UNUSED(ctx);
	const otSrpClientService *service;

	for (service = removed; service != NULL; service = service->mNext) {
		srp_mark_service(service, SRP_REG_FREE);
	}

	if (err == OT_ERROR_NONE) {
		for (service = services; service != NULL; service = service->mNext) {
			srp_mark_service(service, SRP_REG_ACTIVE);
		}
		LOG_INF("SRP registered: host state %d, service state %d",
			host != NULL ? (int)host->mState : -1,
			services != NULL ? (int)services->mState : -1);
	} else {
		for (service = services; service != NULL; service = service->mNext) {
			srp_mark_service(service, SRP_REG_FAILED);
		}
		LOG_ERR("SRP registration FAILED (%d) -- the commissioner cannot resolve this node",
			err);
		if (err == OT_ERROR_DUPLICATED && !s_recovery_pending) {
			s_recovery_pending = true;
			(void)k_work_reschedule(&s_srp_recovery_work,
						K_SECONDS(SRP_DUPLICATE_RETRY_S));
		}
	}
	log_addresses(openthread_get_default_instance());
}

/**
 * Release all SRP registrations for the host and services. Clears both the SRP client state and the
 * local registration cache, so the next advertise re-registers from scratch. Called when fabrics
 * are rolled back to avoid leaving dangling registrations under old names.
 */
void matter_thread_advertise_reset(void)
{
	otInstance *ot = openthread_get_default_instance();
	otError rm = OT_ERROR_INVALID_STATE;

	(void)k_work_cancel_delayable(&s_srp_recovery_work);
	s_recovery_pending = false;

	/*
	 * Rolling back the fabrics has to release their SRP registrations too.
	 *
	 * There is one slot per supported fabric and a slot is only reused when
	 * the instance name matches EXACTLY -- but the name is derived from the
	 * compressed fabric id and node id, so a new commissioner never matches
	 * the old one. A board that came up with restored fabrics therefore held
	 * both slots against names that no longer existed, and the next pairing
	 * died right after PASE with "no SRP slot left", which looks from the
	 * phone like an accessory stuck on "connecting".
	 *
	 * The host goes with them: the services hang off it, and the next
	 * advertise re-registers both from scratch.
	 */
	/*
	 * RETRACT, do not just forget. otSrpClientClearHostAndServices() is
	 * local-only: it drops this node's copy and never tells the server, so
	 * the border router kept advertising every abandoned instance until its
	 * lease ran out -- seven of them accumulated in one evening of failed
	 * pairings (dns-sd -B _matter._tcp, 2026-08-06), each one a name a
	 * commissioner can still resolve and then fail to reach.
	 *
	 * aRemoveKeyLease = true: the key lease is what reserves the name for a
	 * client that will come back, and these names are never coming back --
	 * the instance name is derived from a fabric that just ceased to exist.
	 *
	 * aSendUnregToServer = true: covers the case where this rollback lands
	 * before the registration was confirmed, which is precisely the failed
	 * pairing that leaves rubbish behind.
	 */
	if (ot != NULL) {
		openthread_mutex_lock();
		rm = otSrpClientRemoveHostAndServices(ot, true, true);

		if (rm != OT_ERROR_NONE) {
			/* ALREADY means nothing is registered, which is the
			 * outcome wanted anyway; anything else leaves the
			 * client mid-state, and the local clear is the only
			 * way back to a usable one. */
			LOG_INF("SRP remove not started (%d); clearing locally", rm);
			otSrpClientClearHostAndServices(ot);
		}
		openthread_mutex_unlock();
	}
	for (size_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		s_regs[i].desired = false;
		if (rm == OT_ERROR_NONE && s_regs[i].state != SRP_REG_FREE) {
			s_regs[i].state = SRP_REG_REMOVING;
		} else if (rm != OT_ERROR_NONE) {
			memset(&s_regs[i], 0, sizeof(s_regs[i]));
		}
	}
	s_host_ready = false;
	/* The commissionable service hung off that host too. */
	s_comm_desired = false;
	if (rm == OT_ERROR_NONE && s_comm_state != SRP_REG_FREE) {
		s_comm_state = SRP_REG_REMOVING;
	} else if (rm != OT_ERROR_NONE) {
		s_comm_state = SRP_REG_FREE;
		memset(&s_comm_service, 0, sizeof(s_comm_service));
	}
	LOG_INF("SRP registrations released");
}

/*
 * The host name, built outside the OpenThread lock because srp_host_id() reaches
 * into the settings backend and that is not somewhere to go holding it.
 *
 * The host name only has to be unique on the SRP server, and the EUI-64 already
 * is -- across boards. The suffix is what makes it unique across this board's
 * own erases; see SRP_HOST_ID_KEY.
 */
static void srp_host_name_build(otInstance *ot)
{
	otExtAddress eui;

	otPlatRadioGetIeeeEui64(ot, eui.m8);
	(void)snprintf(s_host_name, sizeof(s_host_name), "%02X%02X%02X%02X%02X%02X%02X%02X-%08X",
		       eui.m8[0], eui.m8[1], eui.m8[2], eui.m8[3], eui.m8[4], eui.m8[5], eui.m8[6],
		       eui.m8[7], (unsigned int)srp_host_id());
}

/*
 * The HOST is registered once; every service hangs off it, operational and
 * commissionable alike, and whichever registers first brings it up for the
 * other. Caller holds the OpenThread lock and has already built s_host_name.
 *
 * Calling otSrpClientSetHostName() again once the client is running returns
 * OT_ERROR_INVALID_STATE (13) and takes the whole registration down with it --
 * which is what refused the second fabric after its AddNOC was accepted, leaving
 * the new administrator a fabric it could not resolve.
 */
static otError srp_host_register(otInstance *ot)
{
	otError err;

	if (s_host_ready) {
		return OT_ERROR_NONE;
	}
	otSrpClientSetCallback(ot, srp_cb, NULL);
	/* Before the first registration: it is sent WITH the update, so setting
	 * it afterwards would leave the default in force until something else
	 * refreshed the lease. */
	otSrpClientSetKeyLeaseInterval(ot, SRP_KEY_LEASE_S);
	err = otSrpClientSetHostName(ot, s_host_name);
	if (err == OT_ERROR_INVALID_STATE) {
		/* The host is still retracting. Its service objects remain owned
		 * by OpenThread until the callback returns them in `removed`; do
		 * not clear or reuse those buffers underneath it. */
		LOG_WRN("host name refused while an SRP retraction is in flight");
	}
	if (err == OT_ERROR_NONE) {
		err = otSrpClientEnableAutoHostAddress(ot);
	}
	if (err == OT_ERROR_NONE) {
		s_host_ready = true;
	}
	return err;
}

static void srp_recovery_work_fn(struct k_work *w)
{
	otInstance *ot = openthread_get_default_instance();
	otError err = OT_ERROR_INVALID_STATE;

	ARG_UNUSED(w);
	if (ot == NULL) {
		s_recovery_pending = false;
		return;
	}

	/* Clear is synchronous and is the only point after which OpenThread no
	 * longer owns the service structures. The desired flags and buffers stay
	 * intact so every live registration can be rebuilt. */
	openthread_mutex_lock();
	otSrpClientClearHostAndServices(ot);
	s_host_ready = false;
	openthread_mutex_unlock();

	/* Keep the persisted host identity and SRP key. Matter operational
	 * instance names are deterministic and cannot be rotated, so changing
	 * only the host name cannot cure a service-name collision and can lose
	 * the key ownership that would let the server accept it after recovery. */
	srp_host_name_build(ot);

	openthread_mutex_lock();
	err = srp_host_register(ot);
	if (err == OT_ERROR_NONE) {
		for (size_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
			if (!s_regs[i].desired) {
				s_regs[i].state = SRP_REG_FREE;
				continue;
			}
			err = otSrpClientAddService(ot, &s_regs[i].service);
			s_regs[i].state =
				err == OT_ERROR_NONE ? SRP_REG_ADDING : SRP_REG_FAILED;
			if (err != OT_ERROR_NONE) {
				break;
			}
		}
	}
	if (err == OT_ERROR_NONE && s_comm_desired) {
		err = otSrpClientAddService(ot, &s_comm_service);
		s_comm_state = err == OT_ERROR_NONE ? SRP_REG_ADDING : SRP_REG_FAILED;
	}
	if (err == OT_ERROR_NONE) {
		otSrpClientEnableAutoStartMode(ot, NULL, NULL);
	}
	openthread_mutex_unlock();
	if (err == OT_ERROR_NONE) {
		s_recovery_pending = false;
		LOG_INF("SRP duplicate recovery retry submitted");
	} else {
		LOG_ERR("SRP duplicate recovery retry failed (%d); trying again", err);
		(void)k_work_reschedule(&s_srp_recovery_work,
					K_SECONDS(SRP_DUPLICATE_RETRY_S));
	}
}

int matter_thread_advertise(const char *instance_name, uint16_t port)
{
	otInstance *ot = openthread_get_default_instance();
	otSockAddr bind_addr;
	struct srp_reg *reg = NULL;
	size_t i;
	otError err;

	if (ot == NULL || instance_name == NULL) {
		return MATTER_E_INVAL;
	}
	if (strlen(instance_name) >= MATTER_INSTANCE_NAME_LEN) {
		return MATTER_E_INVAL;
	}
	/*
	 * One slot per name. Re-registering the SAME name is a no-op rather than
	 * an error: matter_clusters.c re-advertises every fabric whenever one is
	 * added, which is simpler than tracking what changed and means this is
	 * called with names already published.
	 */
	for (i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		if (s_regs[i].state != SRP_REG_FREE &&
		    strcmp(s_regs[i].instance_name, instance_name) == 0) {
			return s_regs[i].state == SRP_REG_ADDING ||
				       s_regs[i].state == SRP_REG_ACTIVE
				       ? MATTER_OK
				       : MATTER_E_STATE;
		}
	}
	for (i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		if (s_regs[i].state == SRP_REG_FREE) {
			reg = &s_regs[i];
			break;
		}
	}
	if (reg == NULL) {
		LOG_ERR("no SRP slot left for %s", instance_name);
		return MATTER_E_NOSPACE;
	}
	(void)snprintf(reg->instance_name, sizeof(reg->instance_name), "%s", instance_name);
	reg->desired = true;

	srp_host_name_build(ot);

	/*
	 * SII and SAI are the peer's retransmission timers, in milliseconds.
	 * 500/300 are the Matter defaults, republished because the places that
	 * describe this radio's availability -- this TXT record, the Sigma2
	 * session parameters in matter_case.c, and the link mode in
	 * overlay-thread.conf -- have to move together. This node was a
	 * 3,000 ms sleepy end device once, and advertising that poll was what
	 * kept peers from giving up on it; the radio is rx-on (MED) now, and a
	 * stale SII=3000 would do the opposite, pacing every retransmit to a
	 * sleep that no longer happens.
	 */
	reg->txt[0].mKey = "SII";
	reg->txt[0].mValue = (const uint8_t *)s_txt_sii;
	reg->txt[0].mValueLength = (uint16_t)strlen(s_txt_sii);
	reg->txt[1].mKey = "SAI";
	reg->txt[1].mValue = (const uint8_t *)s_txt_sai;
	reg->txt[1].mValueLength = (uint16_t)strlen(s_txt_sai);

	/*
	 * The compressed-fabric subtype, "_I" + the id in uppercase hex
	 * (lib/dnssd/ServiceNaming.cpp, MakeServiceSubtype, kCompressedFabricId).
	 * A commissioner that already knows the node id resolves the instance
	 * name directly and does not need this; one that BROWSES for every node
	 * on its fabric finds nothing without it, and which of the two Apple
	 * does is not something this end can see.
	 *
	 * The first 16 characters of the instance name are that id already.
	 */
	reg->subtype[0] = '_';
	reg->subtype[1] = 'I';
	memcpy(&reg->subtype[2], reg->instance_name, 16u);
	reg->subtype[18] = '\0';
	reg->subtype_labels[0] = reg->subtype;
	reg->subtype_labels[1] = NULL;

	memset(&reg->service, 0, sizeof(reg->service));
	reg->service.mName = s_service_type;
	reg->service.mInstanceName = reg->instance_name;
	reg->service.mSubTypeLabels = reg->subtype_labels;
	reg->service.mPort = port;
	reg->service.mTxtEntries = reg->txt;
	reg->service.mNumTxtEntries = 2u;

	openthread_mutex_lock();

	/*
	 * BIND BEFORE PUBLISHING. The commissioner starts resolving the moment
	 * the registration lands, and a name that resolves to a closed port is
	 * worse than one that has not appeared yet -- it turns a retry into a
	 * refusal.
	 */
	if (!s_udp_open) {
		memset(&bind_addr, 0, sizeof(bind_addr));
		bind_addr.mPort = port;
		if (otUdpOpen(ot, &s_udp, udp_rx, NULL) == OT_ERROR_NONE &&
		    otUdpBind(ot, &s_udp, &bind_addr, OT_NETIF_THREAD) == OT_ERROR_NONE) {
			s_udp_open = true;
			LOG_INF("listening on UDP %u", (unsigned int)port);
		} else {
			LOG_ERR("could not listen on UDP %u", (unsigned int)port);
		}
	}

	/*
	 * The HOST is registered once; the services hang off it. Calling
	 * otSrpClientSetHostName() again once the client is running returns
	 * OT_ERROR_INVALID_STATE (13) and takes the whole registration down with
	 * it -- which is what refused the second fabric after its AddNOC was
	 * accepted, leaving the new administrator a fabric it could not resolve.
	 */
	err = srp_host_register(ot);
	if (err == OT_ERROR_NONE) {
		err = otSrpClientAddService(ot, &reg->service);
	}
	if (err == OT_ERROR_NONE) {
		/* Finds the border router's SRP server itself, from the network
		 * data it already has as a child. Idempotent. */
		otSrpClientEnableAutoStartMode(ot, NULL, NULL);
#if defined(CONFIG_ULTRAWIDELOCK_SRP_DIAG)
		/* Registered here rather than at start-up so it cannot outlive
		 * the thing it reports on, and once because the register call
		 * appends to a list. */
		if (!s_srp_diag_registered) {
			s_srp_diag_registered =
				openthread_state_changed_callback_register(&srp_diag_cb) == 0;
		}
#endif
	}
	openthread_mutex_unlock();

	if (err != OT_ERROR_NONE) {
		LOG_ERR("SRP registration refused (%d)", err);
		memset(reg, 0, sizeof(*reg));
		return MATTER_E_STATE;
	}
	reg->state = SRP_REG_ADDING;

	LOG_INF("SRP: %s.%s._matter._tcp on %s.local port %u", reg->instance_name, reg->subtype,
		s_host_name, port);
	log_addresses(ot);
	return MATTER_OK;
}

/*
 * The commissionable registration, one at a time because only one window is ever
 * open. Same lifetime rule as the operational one: OpenThread links these into a
 * list and does not copy the strings, so every buffer here is static.
 */
static char s_comm_service_type[] = "_matterc._udp";
static char s_comm_instance[17]; /* 16 hex of a random 64-bit id + NUL */
static char s_comm_sub_short[5]; /* "_S" + up to 2 digits + NUL */
static char s_comm_sub_long[7];  /* "_L" + up to 4 digits + NUL */
static const char *s_comm_sub_labels[3];
static char s_comm_txt_d[5];   /* long discriminator, decimal */
static char s_comm_txt_cm[2];  /* commissioning mode */
static char s_comm_txt_vp[12]; /* "<vendor>+<product>", both decimal */
static otDnsTxtEntry s_comm_txt[5];

int matter_thread_advertise_commissionable(uint16_t discriminator, uint16_t port)
{
	otInstance *ot = openthread_get_default_instance();
	uint8_t rnd[8];
	otError err;

	if (ot == NULL) {
		return MATTER_E_STATE;
	}
	if (s_comm_state == SRP_REG_ADDING || s_comm_state == SRP_REG_ACTIVE) {
		/* One window at a time, and the discriminator is fixed for its
		 * duration. Re-registering the same name would be a no-op that
		 * costs an SRP update, so say nothing happened. */
		return MATTER_OK;
	}
	if (s_comm_state != SRP_REG_FREE) {
		return MATTER_E_STATE;
	}
	s_comm_desired = true;

	/*
	 * A fresh 64-bit instance name per window, per the spec's commissionable
	 * instance naming. It deliberately does NOT encode the fabric or the node
	 * id the way the operational name does: this service is offered to a
	 * controller that has no relationship with this node yet, and a stable
	 * name would let anyone browsing correlate windows across time.
	 */
	if (psa_generate_random(rnd, sizeof(rnd)) != PSA_SUCCESS) {
		LOG_ERR("no RNG for the commissionable instance name");
		return MATTER_E_STATE;
	}
	(void)snprintf(s_comm_instance, sizeof(s_comm_instance), "%02X%02X%02X%02X%02X%02X%02X%02X",
		       rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);

	/*
	 * Both subtypes, because controllers do not agree on which to browse.
	 * chip-tool asks for "_matterc._udp,_S<short>" where short is the TOP
	 * FOUR BITS of the 12-bit discriminator; others browse the long form.
	 * Publishing one and not the other is indistinguishable from being
	 * offline to whichever half asks the other way.
	 */
	(void)snprintf(s_comm_sub_short, sizeof(s_comm_sub_short), "_S%u",
		       (unsigned int)((discriminator >> 8) & 0x0Fu));
	(void)snprintf(s_comm_sub_long, sizeof(s_comm_sub_long), "_L%u",
		       (unsigned int)(discriminator & 0x0FFFu));
	s_comm_sub_labels[0] = s_comm_sub_short;
	s_comm_sub_labels[1] = s_comm_sub_long;
	s_comm_sub_labels[2] = NULL;

	/*
	 * D is the long discriminator, and it must agree with the BLE advert --
	 * a controller that finds this over DNS-SD and then falls back to BLE
	 * matches on the same value. CM=2 says the window was opened by a
	 * commissioner with its own verifier (enhanced), which is exactly the
	 * kind admin_arm() reports as kind 1.
	 */
	(void)snprintf(s_comm_txt_d, sizeof(s_comm_txt_d), "%u",
		       (unsigned int)(discriminator & 0x0FFFu));
	(void)snprintf(s_comm_txt_cm, sizeof(s_comm_txt_cm), "2");
	(void)snprintf(s_comm_txt_vp, sizeof(s_comm_txt_vp), "%u+%u",
		       (unsigned int)CONFIG_ULTRAWIDELOCK_MATTER_VENDOR_ID,
		       (unsigned int)CONFIG_ULTRAWIDELOCK_MATTER_PRODUCT_ID);
	s_comm_txt[0].mKey = "D";
	s_comm_txt[0].mValue = (const uint8_t *)s_comm_txt_d;
	s_comm_txt[0].mValueLength = (uint16_t)strlen(s_comm_txt_d);
	s_comm_txt[1].mKey = "CM";
	s_comm_txt[1].mValue = (const uint8_t *)s_comm_txt_cm;
	s_comm_txt[1].mValueLength = (uint16_t)strlen(s_comm_txt_cm);
	s_comm_txt[2].mKey = "VP";
	s_comm_txt[2].mValue = (const uint8_t *)s_comm_txt_vp;
	s_comm_txt[2].mValueLength = (uint16_t)strlen(s_comm_txt_vp);
	/* Keep commissionable and operational discovery aligned with the MED
	 * retransmission intervals described above. */
	s_comm_txt[3].mKey = "SII";
	s_comm_txt[3].mValue = (const uint8_t *)s_txt_sii;
	s_comm_txt[3].mValueLength = (uint16_t)strlen(s_txt_sii);
	s_comm_txt[4].mKey = "SAI";
	s_comm_txt[4].mValue = (const uint8_t *)s_txt_sai;
	s_comm_txt[4].mValueLength = (uint16_t)strlen(s_txt_sai);

	memset(&s_comm_service, 0, sizeof(s_comm_service));
	s_comm_service.mName = s_comm_service_type;
	s_comm_service.mInstanceName = s_comm_instance;
	s_comm_service.mSubTypeLabels = s_comm_sub_labels;
	s_comm_service.mPort = port;
	s_comm_service.mTxtEntries = s_comm_txt;
	s_comm_service.mNumTxtEntries = 5u;

	srp_host_name_build(ot);

	openthread_mutex_lock();
	err = srp_host_register(ot);
	if (err == OT_ERROR_NONE) {
		err = otSrpClientAddService(ot, &s_comm_service);
	}
	openthread_mutex_unlock();

	if (err != OT_ERROR_NONE) {
		LOG_ERR("commissionable SRP registration refused (%d)", err);
		s_comm_desired = false;
		s_comm_state = SRP_REG_FREE;
		memset(&s_comm_service, 0, sizeof(s_comm_service));
		return MATTER_E_STATE;
	}
	s_comm_state = SRP_REG_ADDING;
	LOG_INF("SRP: %s.%s/%s._matterc._udp port %u (D=%s)", s_comm_instance, s_comm_sub_short,
		s_comm_sub_long, port, s_comm_txt_d);
	return MATTER_OK;
}

int matter_thread_unadvertise(const char *instance_name)
{
	otInstance *ot = openthread_get_default_instance();
	struct srp_reg *reg = NULL;
	otError err;
	size_t i;

	if (instance_name == NULL) {
		return MATTER_E_INVAL;
	}
	for (i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		if (s_regs[i].state != SRP_REG_FREE &&
		    strcmp(s_regs[i].instance_name, instance_name) == 0) {
			reg = &s_regs[i];
			break;
		}
	}
	/* Already not there is the state being asked for. */
	if (reg == NULL) {
		return MATTER_OK;
	}
	if (ot == NULL) {
		return MATTER_E_STATE;
	}

	openthread_mutex_lock();
	err = otSrpClientRemoveService(ot, &reg->service);
	if (err != OT_ERROR_NONE) {
		otSrpClientClearService(ot, &reg->service);
	}
	openthread_mutex_unlock();

	reg->desired = false;
	if (err != OT_ERROR_NONE) {
		LOG_WRN("SRP removal of %s refused (%d); cleared locally", instance_name, err);
		memset(reg, 0, sizeof(*reg));
		return MATTER_E_STATE;
	}
	reg->state = SRP_REG_REMOVING;
	LOG_INF("SRP: operational service %s withdrawn", instance_name);
	return MATTER_OK;
}

int matter_thread_unadvertise_commissionable(void)
{
	otInstance *ot = openthread_get_default_instance();
	otError err;

	if (s_comm_state == SRP_REG_FREE) {
		return MATTER_OK;
	}
	if (ot == NULL) {
		return MATTER_E_STATE;
	}

	openthread_mutex_lock();
	err = otSrpClientRemoveService(ot, &s_comm_service);
	if (err != OT_ERROR_NONE) {
		otSrpClientClearService(ot, &s_comm_service);
	}
	openthread_mutex_unlock();

	s_comm_desired = false;
	if (err != OT_ERROR_NONE) {
		LOG_WRN("commissionable SRP removal refused (%d); cleared locally", err);
		s_comm_state = SRP_REG_FREE;
		memset(&s_comm_service, 0, sizeof(s_comm_service));
		return MATTER_E_STATE;
	}
	s_comm_state = SRP_REG_REMOVING;
	LOG_INF("SRP: commissionable service withdrawn");
	return MATTER_OK;
}

#else /* !CONFIG_OPENTHREAD */

/**
 * Advertise this node's services to SRP. Returns MATTER_E_STATE; Thread is not built into this
 * image.
 */
int matter_thread_advertise(const char *instance_name, uint16_t port)
{
	ARG_UNUSED(instance_name);
	ARG_UNUSED(port);

	return MATTER_E_STATE;
}

/**
 * Publish the commissionable service. Returns MATTER_E_STATE; Thread is not built into this image.
 */
int matter_thread_advertise_commissionable(uint16_t discriminator, uint16_t port)
{
	ARG_UNUSED(discriminator);
	ARG_UNUSED(port);

	return MATTER_E_STATE;
}

/**
 * Withdraw one operational service. Returns MATTER_OK; nothing was ever registered.
 */
int matter_thread_unadvertise(const char *instance_name)
{
	ARG_UNUSED(instance_name);

	return MATTER_OK;
}

/**
 * Withdraw the commissionable service. Returns MATTER_OK; nothing was ever registered.
 */
int matter_thread_unadvertise_commissionable(void)
{
	return MATTER_OK;
}

/**
 * Print the active dataset. No-op; Thread is not built into this image.
 */
void matter_thread_dump_active_dataset(void)
{
}

/**
 * Start Thread with the provided operational dataset. Returns MATTER_E_STATE; Thread is not built
 * into this image.
 */
int matter_thread_start(const uint8_t *dataset, size_t len)
{
	ARG_UNUSED(dataset);
	ARG_UNUSED(len);

	LOG_WRN("Thread dataset received, but this image has no Thread stack");
	return MATTER_E_STATE;
}

int matter_thread_clear(void)
{
	return MATTER_E_STATE;
}

/**
 * Is this node already on that network? Always false; there is no Thread stack in this image, so
 * it is on no network at all.
 */
bool matter_thread_attached_to(const uint8_t *xpanid)
{
	ARG_UNUSED(xpanid);

	return false;
}

/**
 * Stub: always returns MATTER_E_TIMEOUT. Thread attachment checking is not implemented on this
 * target.
 */
int matter_thread_wait_attached(uint32_t timeout_ms)
{
	ARG_UNUSED(timeout_ms);

	return MATTER_E_TIMEOUT;
}

#endif /* CONFIG_OPENTHREAD */

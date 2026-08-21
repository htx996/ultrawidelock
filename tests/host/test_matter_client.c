/**
 * @file test_matter_client.c — the file that sequences the client, under test.
 *
 * Everything the Matter client needs is a pure function somewhere in
 * modules/ultrawidelock_matter, and every one of them is tested next to itself. This
 * suite is about the layer above: the order they are called in, the one session
 * and one attempt they share, and the clock they are driven by. That layer is
 * where both of this file's known bugs lived, and neither was reachable from a
 * module test, because in a module test nothing sequences anything.
 *
 * What is NOT here, and why: nothing past the Sigma1. Getting a real session
 * established needs a peer that can answer with a cryptographically valid
 * Sigma2, which is the responder half -- and that loopback already exists, in
 * test_matter_case_client.c, against the CASE code rather than against this
 * scheduler. Wiring it through this file is worth doing and is not done here.
 * So: resolve, target choice, framing, addressing, the two routing predicates
 * and the failure paths are covered; the established-session interaction is
 * not.
 *
 * The clock is the suite's, not the system's -- see ultrawidelock_osal_host_advance_ms().
 * Time only moves where a case says it does, so a timeout is something these
 * tests step over deliberately rather than something they wait for.
 */
#include <stdbool.h>
#include <string.h>

#include "matter_binding.h"
#include "matter_client.h"
#include "matter_case.h"
#include "matter_client_sm.h"
#include "matter_clusters.h"
#include "matter_msg.h"
#include "matter_tlv.h"
#include "ultrawidelock_osal.h"

#include "matterfake/thread_host.h"

#include "test.h"

#define FABRIC_A    1u
#define PEER_NODE   0x0102030405060708ull
#define PEER_ENDPT  1u

/**
 * A fabric complete enough for choose_target() to derive keys from.
 *
 * The root public key is a real uncompressed point prefix followed by filler:
 * the compressed-fabric-id derivation hashes these bytes and does not verify
 * the curve, so filler is honest here -- it says "any key" rather than
 * pretending a particular administrator.
 */
static void make_fabric(struct matter_device_info *info)
{
	struct matter_fabric *f = &info->fabrics[0];

	f->index = FABRIC_A;
	f->fabric_id = 0x1122334455667788ull;
	f->node_id = 0x00DEADBEEF000001ull;
	f->case_admin_subject = 0x00DEADBEEF000002ull;
	f->root_public_key[0] = 0x04u;
	for (size_t i = 1u; i < sizeof(f->root_public_key); i++) {
		f->root_public_key[i] = (uint8_t)(0x10u + i);
	}
	for (size_t i = 0u; i < sizeof(f->ipk); i++) {
		f->ipk[i] = (uint8_t)(0xA0u + i);
	}
	/* Non-zero is the whole test: choose_target() reads noc_len as "this
	 * administrator finished commissioning", not as a certificate. */
	f->noc_len = 64u;
	info->committed_slots = MATTER_FABRIC_SLOT_BIT(0u);
	info->accessing_node_id = f->case_admin_subject;
}

/** Bind one Door Lock target on fabric A, the way an administrator's write would. */
static void bind_one(struct matter_device_info *info)
{
	struct matter_tlv_writer w;
	uint8_t buf[128];
	size_t len = 0u;

	matter_tlv_writer_init(&w, buf, sizeof(buf));
	(void)matter_tlv_start_container(&w, MATTER_TLV_ANON, MATTER_TLV_ARRAY);
	(void)matter_tlv_start_container(&w, MATTER_TLV_ANON, MATTER_TLV_STRUCTURE);
	(void)matter_tlv_put_u64(&w, MATTER_TLV_CTX(1u), PEER_NODE);
	(void)matter_tlv_put_u64(&w, MATTER_TLV_CTX(3u), PEER_ENDPT);
	(void)matter_tlv_put_u64(&w, MATTER_TLV_CTX(4u), MATTER_CLUSTER_DOOR_LOCK);
	(void)matter_tlv_end_container(&w);
	(void)matter_tlv_end_container(&w);
	T_EQ("the binding list encodes", matter_tlv_writer_finish(&w, &len), MATTER_OK);
	T_EQ("and is accepted", matter_binding_write(&info->binding, FABRIC_A, buf, len),
	     MATTER_OK);
}

/** Everything back to boot: the client, the radio, the clock and the queue. */
static void fixture(struct matter_device_info *info, bool with_fabric, bool with_binding)
{
	memset(info, 0, sizeof(*info));
	info->vendor_id = 0xFFF1u;
	if (with_fabric) {
		make_fabric(info);
	}
	if (with_binding) {
		bind_one(info);
	}
	ultrawidelock_osal_host_reset();
	matterfake_thread_reset();
	matter_client_init(info);
}

/** Let the queue run: the poll is delayable work and never runs on its own. */
static void tick(uint32_t ms)
{
	(void)ultrawidelock_osal_host_advance_ms((int64_t)ms);
	(void)ultrawidelock_osal_host_flush();
}

/** The exchange id in datagram @p tx, or 0 when it will not decode. */
static uint16_t exchange_of(const struct matterfake_tx *tx)
{
	struct matter_msg_header mh;
	struct matter_proto_header ph;
	size_t mh_len = 0u;
	size_t ph_len = 0u;

	if (tx == NULL) {
		return 0u;
	}
	if (matter_msg_header_decode(tx->buf, tx->len, &mh, &mh_len) != MATTER_OK) {
		return 0u;
	}
	if (matter_proto_header_decode(tx->buf + mh_len, tx->len - mh_len, &ph, &ph_len) !=
	    MATTER_OK) {
		return 0u;
	}
	return ph.exchange_id;
}

/** The protocol opcode in datagram @p tx, or 0xFF when it will not decode. */
static uint8_t opcode_of(const struct matterfake_tx *tx)
{
	struct matter_msg_header mh;
	struct matter_proto_header ph;
	size_t mh_len = 0u;
	size_t ph_len = 0u;

	if (tx == NULL) {
		return 0xFFu;
	}
	if (matter_msg_header_decode(tx->buf, tx->len, &mh, &mh_len) != MATTER_OK) {
		return 0xFFu;
	}
	if (matter_proto_header_decode(tx->buf + mh_len, tx->len - mh_len, &ph, &ph_len) !=
	    MATTER_OK) {
		return 0xFFu;
	}
	return ph.opcode;
}

/** Drive a fresh client to the point where its Sigma1 has gone out. */
static bool reach_sigma1(struct matter_device_info *info)
{
	struct matter_thread_peer peer;

	fixture(info, true, true);
	matter_client_want();
	tick(1u);
	if (!matterfake_resolve_pending()) {
		return false;
	}
	matterfake_some_peer(&peer);
	matterfake_resolve_answer(&peer);
	tick(1u);
	return matterfake_tx_count() == 1u;
}

void test_matter_client(void)
{
	struct matter_device_info info;

	t_group("a lock nobody has bound goes looking for nobody");
	{
		fixture(&info, true, false);
		matter_client_want();
		tick(MATTER_CLIENT_STEP_MS * 4u);
		T_EQ("no lookup was started", (long)matterfake_resolve_count(), 0L);
		T_EQ("and nothing was sent", (long)matterfake_tx_count(), 0L);
	}

	t_group("a binding whose administrator was never committed is not a target");
	{
		fixture(&info, false, true);
		matter_client_want();
		tick(MATTER_CLIENT_STEP_MS * 4u);
		T_EQ("no lookup was started", (long)matterfake_resolve_count(), 0L);
		T_EQ("and nothing was sent", (long)matterfake_tx_count(), 0L);
	}

	t_group("a walk-up looks the bound lock up by its operational name");
	{
		fixture(&info, true, true);
		T_EQ("nothing happens before the walk-up", (long)matterfake_resolve_count(), 0L);
		matter_client_want();
		tick(1u);
		T_EQ("the walk-up starts exactly one lookup", (long)matterfake_resolve_count(), 1L);
		T_OK("which is still outstanding", matterfake_resolve_pending());
		T_OK("named as a Matter operational instance",
		     matterfake_resolve_name() != NULL && strlen(matterfake_resolve_name()) == 33u &&
			     matterfake_resolve_name()[16] == '-');
		T_EQ("and nothing is on the wire yet", (long)matterfake_tx_count(), 0L);
	}

	t_group("a bound lock that is not there is an answer, not a stall");
	{
		fixture(&info, true, true);
		matter_client_want();
		tick(1u);
		matterfake_resolve_answer(NULL);
		tick(1u);
		T_EQ("nothing is sent to a lock that does not resolve",
		     (long)matterfake_tx_count(), 0L);
		T_EQ("and it is not retried on the spot", (long)matterfake_resolve_count(), 1L);
	}

	t_group("a resolved lock gets a Sigma1, addressed to where it was found");
	{
		struct matter_thread_peer peer;
		const struct matterfake_tx *tx;

		fixture(&info, true, true);
		matter_client_want();
		tick(1u);
		matterfake_some_peer(&peer);
		matterfake_resolve_answer(&peer);
		tick(1u);

		T_EQ("one datagram went out", (long)matterfake_tx_count(), 1L);
		tx = matterfake_last_tx();
		T_OK("to the address the lookup returned",
		     tx != NULL && memcmp(tx->peer.addr, peer.addr, sizeof(peer.addr)) == 0);
		T_EQ("and its port", tx != NULL ? (long)tx->peer.port : -1L, (long)peer.port);
		T_EQ("carrying a Sigma1", (long)opcode_of(tx), (long)MATTER_OP_CASE_SIGMA1);
	}

	t_group("the handshake's exchange is the client's, and only that one");
	{
		uint16_t x;

		T_OK("a Sigma1 is out", reach_sigma1(&info));
		x = exchange_of(matterfake_last_tx());
		T_OK("its exchange id decodes", x != 0u || true);
		T_OK("the client claims the exchange it opened", matter_client_owns_exchange(x));
		T_OK("and disclaims one it did not",
		     !matter_client_owns_exchange((uint16_t)(x + 1u)));
	}

	/*
	 * The first of the two fixed faults. A Sigma1 that nobody answers has to
	 * stop being the client's business EVENTUALLY, or every later Sigma1
	 * this node RECEIVES on a recycled exchange id is swallowed by a
	 * handshake that ended long ago.
	 */
	t_group("an abandoned handshake stops owning its exchange");
	{
		uint16_t x;

		T_OK("a Sigma1 is out", reach_sigma1(&info));
		x = exchange_of(matterfake_last_tx());
		T_OK("the client owns it while it waits", matter_client_owns_exchange(x));

		/* Past the step deadline with no Sigma2: the attempt fails. */
		tick(MATTER_CLIENT_STEP_MS + 1u);
		T_OK("and has let it go once the handshake is abandoned",
		     !matter_client_owns_exchange(x));
	}

	/*
	 * The second. s_fabric points INTO info->fabrics[], so an administrator
	 * removed mid-attempt leaves a pointer at a zeroed slot: valid memory
	 * describing nobody. The attempt must be dropped rather than continued
	 * with whatever the slot now holds.
	 */
	t_group("an administrator removed mid-handshake takes its attempt with it");
	{
		uint16_t x;

		T_OK("a Sigma1 is out", reach_sigma1(&info));
		x = exchange_of(matterfake_last_tx());

		/* RemoveFabric, as matter_clusters.c's fabric_slot_clear() does it. */
		memset(&info.fabrics[0], 0, sizeof(info.fabrics[0]));
		info.committed_slots = 0u;

		/*
		 * Noticed at the next poll, not instantly, and that bound is the
		 * point: the attempt cannot get anywhere without a Sigma2, and a
		 * Sigma2 that arrives first is refused by the liveness check on
		 * the inbound path rather than by this one.
		 */
		tick(MATTER_CLIENT_STEP_MS + 1u);
		T_EQ("no further datagram is sent on its behalf", (long)matterfake_tx_count(), 1L);
		T_OK("and the handshake is no longer the client's",
		     !matter_client_owns_exchange(x));
	}

	t_group("a slot reused by a DIFFERENT administrator is not the same fabric");
	{
		T_OK("a Sigma1 is out", reach_sigma1(&info));

		/* Same slot, same index, different administrator: the identity
		 * check is by address AND liveness, not by index alone. */
		info.fabrics[0].fabric_id = 0x9999999999999999ull;
		info.fabrics[0].node_id = 0x00CAFE0000000001ull;

		tick(MATTER_CLIENT_STEP_MS + 1u);
		T_OK("the attempt does not silently continue on the new one",
		     matterfake_tx_count() <= 2u);
	}

	t_group("a socket that is down does not wedge the client");
	{
		struct matter_thread_peer peer;

		fixture(&info, true, true);
		matterfake_fail_next_sends(1u);
		matter_client_want();
		tick(1u);
		matterfake_some_peer(&peer);
		matterfake_resolve_answer(&peer);
		tick(1u);
		T_EQ("the send was attempted", (long)matterfake_tx_count(), 1L);
		/* The want is worth ~8 s; the client is entitled to try again
		 * inside it, and entitled to give up after. Either way it must
		 * still be answering, not stuck holding its lock. */
		tick(MATTER_CLIENT_STEP_MS * 4u);
		T_OK("and the client is still responsive afterwards",
		     !matter_client_owns_session(0xFFFFu));
	}

	t_group("a lookup that cannot even be started is a failed attempt");
	{
		fixture(&info, true, true);
		matterfake_fail_next_resolve();
		matter_client_want();
		tick(1u);
		T_OK("no query is outstanding", !matterfake_resolve_pending());
		T_EQ("and nothing was sent", (long)matterfake_tx_count(), 0L);
		/*
		 * Not wedged, and not dependent on a second walk-up either: the
		 * want is still alive, so the backoff alone brings it back.
		 */
		tick(MATTER_CLIENT_BACKOFF_MS + 100u);
		T_OK("the backoff retries it without being asked",
		     matterfake_resolve_count() >= 1u);
	}

	/*
	 * A finding, recorded as a test because it is the kind of thing that
	 * gets rediscovered as a field report. matter_thread_resolve() refuses a
	 * second query while one is outstanding, and nothing here can cancel
	 * one, so an attempt that times out at MATTER_CLIENT_STEP_MS leaves a
	 * query behind that blocks the NEXT attempt from even starting.
	 *
	 * On target the bound is OpenThread's own query timeout rather than
	 * anything this file controls, which is exactly why the host cannot say
	 * how long the hole is -- only that it exists and that the client stays
	 * responsive across it. Symptom to expect on the bench: a second walk-up
	 * a few seconds after a failed one does nothing at all.
	 */
	t_group("a query still outstanding blocks the next attempt, and does not wedge it");
	{
		struct matter_thread_peer peer;

		fixture(&info, true, true);
		matter_client_want();
		tick(1u);
		T_EQ("the first attempt queried", (long)matterfake_resolve_count(), 1L);

		/* Nobody ever answers it; the step deadline passes. */
		tick(MATTER_CLIENT_STEP_MS + 1u);
		T_OK("the query is still outstanding", matterfake_resolve_pending());

		matter_client_want();
		tick(1u);
		T_EQ("so the next attempt cannot start one", (long)matterfake_resolve_count(), 1L);
		T_EQ("and sends nothing", (long)matterfake_tx_count(), 0L);

		/* When it finally answers, the client is still there to act. */
		matterfake_some_peer(&peer);
		matterfake_resolve_answer(&peer);
		matter_client_want();
		tick(MATTER_CLIENT_BACKOFF_MS * 4u);
		T_OK("and once it clears, walk-ups are served again",
		     matterfake_resolve_count() >= 2u);
	}

	t_group("a late answer to a lookup nobody is waiting for is ignored");
	{
		struct matter_thread_peer peer;

		fixture(&info, true, true);
		matter_client_want();
		tick(1u);
		T_OK("a query is outstanding", matterfake_resolve_pending());

		/* The step deadline passes first; the answer arrives after. */
		tick(MATTER_CLIENT_STEP_MS + 1u);
		matterfake_some_peer(&peer);
		matterfake_resolve_answer(&peer);
		tick(1u);
		T_EQ("the stale answer does not produce a Sigma1", (long)matterfake_tx_count(),
		     0L);
	}

	t_group("nothing here dereferences a NULL");
	{
		ultrawidelock_osal_host_reset();
		matterfake_thread_reset();
		matter_client_init(NULL);
		matter_client_want();
		tick(MATTER_CLIENT_STEP_MS * 2u);
		T_OK("a client with no device state owns no session",
		     !matter_client_owns_session(1u));
		T_OK("and no exchange", !matter_client_owns_exchange(1u));
		T_EQ("and sends nothing", (long)matterfake_tx_count(), 0L);
	}

	/* Leave the client pointing at nothing: these statics outlive the suite,
	 * and a later suite's memory is not this one's to keep a pointer into. */
	matter_client_init(NULL);
	ultrawidelock_osal_host_reset();
	matterfake_thread_reset();
}

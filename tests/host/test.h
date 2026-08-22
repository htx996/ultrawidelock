/**
 * @file test.h — shared host-test harness: assertions + suite registry.
 *
 * Self-validating helpers first (no golden needed): T_OK/T_EQ for booleans and
 * integers, t_vec round-trip compares. For exact-byte pins use t_vec/t_u32 with
 * an empty expect string to RECORD the value, then bake it in (behavior-lock).
 * Each module under test exposes one void test_<module>(void) suite; test_main.c
 * runs them all and prints an aggregate PASS/FAIL/RECORD.
 */
#ifndef ULTRAWIDELOCK_HOST_TEST_H
#define ULTRAWIDELOCK_HOST_TEST_H

#include <stddef.h>
#include <stdint.h>

/* Reporting counters (defined in test.c). */
extern int t_fail;
extern int t_pending;
extern int t_pass;

/* hex helpers */
void t_hex(char *dst, const uint8_t *b, size_t n);
int t_unhex(uint8_t *dst, const char *hex, size_t cap);

/* Group header inside a suite. */
void t_group(const char *name);

/* Silence stdout across a call that prints firmware diagnostics by the
 * thousand-line (the CIR ring drain, the accumulator probe). Those are driven
 * for their state changes, not their text, and printing them buries the suite's
 * own output. Keep assertions OUTSIDE the muted region: t_ok_/t_eqi_ print only
 * on failure, so a muted assertion fails invisibly. Nesting is a no-op. */
void t_mute(void);
void t_unmute(void);

/* Byte-vector check. expect==NULL or "" => RECORD (print value, mark pending). */
void t_vec(const char *name, const uint8_t *got, size_t len, const char *expect);
void t_u32(const char *name, uint32_t v, const char *expect);
void t_u16(const char *name, uint16_t v, const char *expect);

/* Self-validating assertions (no golden). */
void t_ok_(const char *name, int cond, const char *file, int line);
void t_eqi_(const char *name, long got, long want, const char *file, int line);
#define T_OK(name, cond)      t_ok_((name), (cond), __FILE__, __LINE__)
#define T_EQ(name, got, want) t_eqi_((name), (long)(got), (long)(want), __FILE__, __LINE__)

/* Module suites — one per file under test. */
void test_ultrawidelock_advertising(void);
void test_ultrawidelock_latch(void);
void test_ultrawidelock_witness_core(void);
void test_ultrawidelock_witness_msg(void);
void test_ultrawidelock_witness_pick(void);
void test_ultrawidelock_ble(void);
void test_ultrawidelock_nfc(void);
void test_pn532(void);
void test_ccc_kdf(void);
void test_ccc_mac(void);
void test_ccc_sts(void);
void test_ccc_shim(void);
void test_ccc_session(void);
void test_prepoll_schedule(void);
void test_ultrawidelock_uwb_msg_builder(void);
void test_ultrawidelock_uwb_msg_parser(void);
void test_ultrawidelock_uwb_adapter(void);
void test_ultrawidelock_uwb_msg(void);
void test_ultrawidelock_uwb_session(void);
void test_ultrawidelock_prov(void);
void test_ultrawidelock_hash(void);
void test_ultrawidelock_assert(void);
void test_ultrawidelock_device_uwb(void);
void test_cherry(void);
void test_fira(void);
void test_facade(void);
void test_prepoll_gate(void);
void test_prepoll_round(void);
void test_rssi_gate(void);
void test_flight_recorder(void);
void test_approach(void);
void test_grant(void);
void test_ultrawidelock_door(void);
void test_ultrawidelock_fusion(void);
void test_ultrawidelock_report(void);
void test_ultrawidelock_satellite(void);
void test_ultrawidelock_side(void);
void test_ultrawidelock_side_replay(void);
void test_ultrawidelock_slam(void);
void test_ultrawidelock_logfmt(void);
void test_trace(void);
void test_ccc_shim_wrap(void);
void test_matter_tlv(void);
void test_matter_msg(void);
void test_matter_mrp(void);
void test_matter_crypto(void);
void test_matter_btp(void);
void test_matter_pase(void);
void test_matter_spake2p(void);
void test_matter_pase_sm(void);
void test_matter_exchange(void);
void test_matter_im(void);
void test_matter_im_invoke(void);
void test_matter_im_write(void);
void test_matter_im_events(void);
void test_matter_attest(void);
void test_matter_fabric(void);
void test_matter_addnoc(void);
void test_matter_network(void);
void test_matter_case(void);
void test_matter_case_client(void);
void test_matter_client_sm(void);
void test_matter_im_client(void);
void test_matter_binding(void);
/** The file that sequences the Matter client: order, ownership, failure. */
void test_matter_client(void);
void test_matter_clusters(void);
void test_ultrawidelock_ml(void);
void test_ultrawidelock_port(void);

/* Driver-binary suites (built by run.sh as host_test_drv; see drvfake.h). */
void test_uwb_min(void);
void test_uwb_isr(void);
void test_uwb_rxdiag(void);
void test_uwb_cirdiag(void);
void test_uwb_selftest(void);
void test_ultrawidelock_shell(void);

/* PSA/mbedTLS-backend binary suites (host_test_psa; see tests/host/psafake/). */
void test_ccc_crypto_backends(void);
void test_ultrawidelock_prim_psa(void);

#endif /* ULTRAWIDELOCK_HOST_TEST_H */

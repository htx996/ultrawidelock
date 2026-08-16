/*
 * Host test for ports/zephyr/store/matter_fab_settings.c — the FIRST host
 * coverage any file under firmware has had.
 *
 * That gap is why this exists. Every defect this port has hit on hardware lived
 * in the Zephyr glue, never in the ultrawidelock_matter protocol modules (which are 14/14
 * covered), and the glue was never compiled by anything but a developer's local
 * `make cdk-*`. This compiles the REAL source unmodified against a fake settings
 * backend, so what passes is the code that ships.
 *
 * What it can and cannot prove: the fake is an in-RAM key/value store, so
 * nothing here says anything about NVS durability, wear or garbage collection.
 * What it does prove is the branch logic around failure — and one failure in
 * particular, the TORN WRITE, which is otherwise only reachable by cutting power
 * to a real board between two of seven settings_save_one() calls.
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/settings/settings.h>

#include "matter_fab_settings.h"
#include "settingsfake.h"
#include "test.h"

#define K_META "mf2/meta"
#define K_FAB0 "mf2/f0"
#define K_FAB1 "mf2/f1"
#define K_ACL0 "mf2/a0"
#define K_ACL1 "mf2/a1"
#define K_NET  "mf2/net"
#define K_ICAC "mf2/ic"

static void fill_identity(struct matter_device_info *info)
{
	memset(info, 0, sizeof(*info));

	info->fabrics[0].index = 1u;
	info->fabrics[0].have_root = true;
	info->fabrics[0].fabric_id = 0x1122334455667788ULL;
	info->fabrics[0].node_id = 0x99AABBCCDDEEFF00ULL;

	info->fabrics[1].index = 2u;
	info->fabrics[1].have_root = true;
	info->fabrics[1].fabric_id = 0xAAAABBBBCCCCDDDDULL;
	info->fabrics[1].node_id = 0x0011223344556677ULL;
	info->fabric_acls[0].len = 3u;
	memcpy(info->fabric_acls[0].data, "one", 3u);
	info->fabric_acls[1].len = 3u;
	memcpy(info->fabric_acls[1].data, "two", 3u);

	info->thread_dataset_len = 32u;
	for (size_t i = 0; i < info->thread_dataset_len; i++) {
		info->thread_dataset[i] = (uint8_t)(0x40u + i);
	}
	info->have_thread_xpanid = true;
	memcpy(info->thread_xpanid, "\xde\xad\x00\xbe\xef\x00\xca\xfe", 8);
}

static int store_identity(struct matter_device_info *info)
{
	struct matter_device_info empty;
	int rc;

	memset(&empty, 0, sizeof(empty));
	(void)matter_fab_load(&empty);
	info->attempt.have_thread_candidate = true;
	info->attempt.thread_dataset_len = info->thread_dataset_len;
	memcpy(info->attempt.thread_dataset, info->thread_dataset, info->thread_dataset_len);
	memcpy(info->attempt.thread_xpanid, info->thread_xpanid, MATTER_THREAD_XPANID_LEN);
	info->committed_slots = 0u;
	rc = matter_fab_commit(info, MATTER_FABRIC_STORE_COMMIT_ATTEMPT, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	info->committed_slots = MATTER_FABRIC_SLOT_BIT(0u);
	info->attempt.have_thread_candidate = false;
	rc = matter_fab_commit(info, MATTER_FABRIC_STORE_COMMIT_ATTEMPT, 1u, NULL, 0u);
	info->committed_slots |= MATTER_FABRIC_SLOT_BIT(1u);
	return rc;
}

void test_matter_fab_settings(void)
{
	struct matter_device_info stored;
	struct matter_device_info loaded;
	int rc;

	t_group("matter_fab_settings: round trip");
	settingsfake_reset();
	fill_identity(&stored);

	rc = store_identity(&stored);
	T_EQ("store succeeds", rc, 0);
	T_OK("epoch key written", settingsfake_has(K_META));
	T_OK("fabric 0 written", settingsfake_has(K_FAB0));
	T_OK("fabric 1 written", settingsfake_has(K_FAB1));
	T_OK("dataset written before the first fabric", settingsfake_has(K_NET));
	T_OK("ACLs are scoped per fabric", settingsfake_has(K_ACL0) && settingsfake_has(K_ACL1));

	memset(&loaded, 0, sizeof(loaded));
	rc = matter_fab_load(&loaded);
	T_EQ("load reports a restored record", rc, 0);
	T_EQ("fabric 0 index survives", loaded.fabrics[0].index, 1);
	T_EQ("fabric 1 index survives", loaded.fabrics[1].index, 2);
	T_EQ("dataset length survives", loaded.thread_dataset_len, 32);
	T_OK("dataset bytes survive",
	     memcmp(loaded.thread_dataset, stored.thread_dataset, 32) == 0);
	T_OK("xpanid survives", loaded.have_thread_xpanid &&
				       memcmp(loaded.thread_xpanid, stored.thread_xpanid, 8) == 0);
	T_OK("a restored record marks its fabrics committed", loaded.committed_slots != 0u);
	T_OK("fabric ACLs survive independently",
	     loaded.fabric_acls[0].len == 3u && loaded.fabric_acls[1].len == 3u &&
		     memcmp(loaded.fabric_acls[0].data, "one", 3u) == 0 &&
		     memcmp(loaded.fabric_acls[1].data, "two", 3u) == 0);

	t_group("matter_fab_settings: nothing stored");
	settingsfake_reset();
	memset(&loaded, 0, sizeof(loaded));
	rc = matter_fab_load(&loaded);
	T_EQ("load reports an empty store", rc, 1);
	T_OK("and claims no commissioning", loaded.committed_slots == 0u);

	/*
	 * THE ONE THAT MATTERS.
	 *
	 * Network and ACL prerequisites may land first, but the per-slot fabric
	 * record is the authority commit. Losing power before it cannot create a
	 * half identity.
	 */
	t_group("matter_fab_settings: torn write");
	settingsfake_reset();
	fill_identity(&stored);
	memset(&loaded, 0, sizeof(loaded));
	(void)matter_fab_load(&loaded);
	settingsfake_fail_saves_after(3); /* meta + network + ICAC tombstone land */

	stored.attempt.have_thread_candidate = true;
	stored.attempt.thread_dataset_len = stored.thread_dataset_len;
	memcpy(stored.attempt.thread_dataset, stored.thread_dataset, stored.thread_dataset_len);
	memcpy(stored.attempt.thread_xpanid, stored.thread_xpanid, MATTER_THREAD_XPANID_LEN);
	stored.committed_slots = 0u;
	rc = matter_fab_commit(&stored, MATTER_FABRIC_STORE_COMMIT_ATTEMPT, 0u, NULL, 0u);
	T_OK("a torn store reports failure", rc != 0);
	T_EQ("and stopped before the authority record", settingsfake_save_count(), 3);

	settingsfake_fail_saves_after(-1);
	memset(&loaded, 0, sizeof(loaded));
	rc = matter_fab_load(&loaded);
	T_OK("a torn record does NOT restore as commissioned", loaded.committed_slots == 0u);
	T_OK("and no fabric is presented", loaded.fabrics[0].index == 0u &&
						  loaded.fabrics[1].index == 0u);

	t_group("matter_fab_settings: corruption is isolated to one fabric");
	settingsfake_reset();
	fill_identity(&stored);
	T_EQ("two fabrics stored for corruption", store_identity(&stored), 0);
	T_OK("second authority record corrupted", settingsfake_corrupt(K_FAB1));
	memset(&loaded, 0, sizeof(loaded));
	T_EQ("the intact neighbour still loads", matter_fab_load(&loaded), 0);
	T_EQ("first fabric survives isolated corruption", loaded.fabrics[0].index, 1u);
	T_EQ("corrupt second fabric is discarded", loaded.fabrics[1].index, 0u);
	T_EQ("its ACL is not exposed without its identity", loaded.fabric_acls[1].len, 0u);

	t_group("matter_fab_settings: corrupt shared ICAC only discards its owner");
	settingsfake_reset();
	fill_identity(&stored);
	stored.fabrics[1].icac_len = 8u;
	stored.icac.owner_index = 2u;
	stored.icac.len = 8u;
	memcpy(stored.icac.buf, "testicac", 8u);
	T_EQ("two fabrics and an ICAC stored", store_identity(&stored), 0);
	T_OK("shared ICAC record corrupted", settingsfake_corrupt(K_ICAC));
	memset(&loaded, 0, sizeof(loaded));
	T_EQ("the independent neighbour still loads", matter_fab_load(&loaded), 0);
	T_EQ("fabric without an ICAC survives", loaded.fabrics[0].index, 1u);
	T_EQ("fabric whose ICAC is corrupt is discarded", loaded.fabrics[1].index, 0u);
	T_EQ("corrupt ICAC bytes are not exposed", loaded.icac.len, 0u);
	T_EQ("the discarded fabric's ACL is also cleared", loaded.fabric_acls[1].len, 0u);

	t_group("matter_fab_settings: targeted tombstone preserves neighbours");
	settingsfake_reset();
	fill_identity(&stored);
	T_EQ("two fabrics stored", store_identity(&stored), 0);
	T_EQ("targeted remove is durable",
	     matter_fab_commit(&stored, MATTER_FABRIC_STORE_REMOVE, 1u, NULL, 0u), 0);
	memset(&loaded, 0, sizeof(loaded));
	T_EQ("survivor loads", matter_fab_load(&loaded), 0);
	T_EQ("first fabric survives", loaded.fabrics[0].index, 1u);
	T_EQ("removed fabric stays absent", loaded.fabrics[1].index, 0u);
	T_EQ("removed ACL cannot leak into the empty slot", loaded.fabric_acls[1].len, 0u);

	t_group("matter_fab_settings: erase");
	settingsfake_reset();
	fill_identity(&stored);
	T_EQ("store for erase", store_identity(&stored), 0);
	T_OK("keys exist before erase", settingsfake_key_count() > 0);
	settingsfake_fail_saves_after(0);
	T_OK("a torn erase reports failure", matter_fab_erase() != 0);
	settingsfake_fail_saves_after(-1);
	memset(&loaded, 0, sizeof(loaded));
	T_EQ("a torn erase leaves the old epoch authoritative", matter_fab_load(&loaded), 0);
	T_EQ("and the old fabric remains intact", loaded.fabrics[0].index, 1u);
	T_EQ("erase succeeds", matter_fab_erase(), 0);
	T_OK("the epoch tombstone remains", settingsfake_has(K_META));

	memset(&loaded, 0, sizeof(loaded));
	T_EQ("load after erase reports an empty store", matter_fab_load(&loaded), 1);

	t_group("matter_fab_settings: v0.3 Matter identity is a clean break");
	settingsfake_reset();
	T_EQ("legacy record injected", settings_save_one("mfab/f0", &stored.fabrics[0],
						    sizeof(stored.fabrics[0])), 0);
	memset(&loaded, 0, sizeof(loaded));
	T_EQ("legacy identity is not loaded", matter_fab_load(&loaded), 1);
	T_EQ("no legacy fabric reaches RAM", loaded.fabrics[0].index, 0u);
	T_OK("legacy Matter key is reclaimed", !settingsfake_has("mfab/f0"));

	/*
	 * The two attributes a controller WRITES, which lived only in RAM until
	 * now: every reset silently undid whatever auto-lock timing and approach
	 * direction the home app had configured, and the app has no way to tell
	 * that apart from a lock that ignored the setting.
	 */
	t_group("matter_dl_attr: a write survives a reboot");
	settingsfake_reset();
	memset(&stored, 0, sizeof(stored));
	stored.auto_relock_time_s = 120u;
	stored.approach_direction = 0x05u;
	T_EQ("both changed values are stored", matter_dl_attr_store(&stored, 0u, 0x07u), 0);
	T_EQ("and each got its own key", settingsfake_key_count(), 2);
	T_OK("under a tree of their own, not the identity's",
	     settingsfake_has("mdl/art") && settingsfake_has("mdl/apd"));

	memset(&loaded, 0, sizeof(loaded));
	loaded.auto_relock_time_s = 999u;
	loaded.approach_direction = 0x07u;
	T_EQ("the load succeeds", matter_dl_attr_load(&loaded), 0);
	T_EQ("AutoRelockTime comes back", loaded.auto_relock_time_s, 120u);
	T_EQ("the approach direction comes back", loaded.approach_direction, 0x05u);

	t_group("matter_dl_attr: only what changed is written");
	settingsfake_reset();
	memset(&stored, 0, sizeof(stored));
	stored.auto_relock_time_s = 60u;
	stored.approach_direction = 0x07u;
	T_EQ("a store of two changes", matter_dl_attr_store(&stored, 0u, 0x00u), 0);
	T_EQ("costs two saves", settingsfake_save_count(), 2);
	T_EQ("re-writing the same values succeeds",
	     matter_dl_attr_store(&stored, 60u, 0x07u), 0);
	T_EQ("and costs no flash at all", settingsfake_save_count(), 2);

	stored.auto_relock_time_s = 0u;
	T_EQ("zero is a value, not an absence", matter_dl_attr_store(&stored, 60u, 0x07u), 0);
	T_EQ("so it is written", settingsfake_save_count(), 3);
	memset(&loaded, 0, sizeof(loaded));
	loaded.auto_relock_time_s = 999u;
	(void)matter_dl_attr_load(&loaded);
	T_EQ("and read back as zero", loaded.auto_relock_time_s, 0u);

	t_group("matter_dl_attr: nothing stored leaves the boot defaults alone");
	settingsfake_reset();
	memset(&loaded, 0, sizeof(loaded));
	loaded.auto_relock_time_s = 42u;
	loaded.approach_direction = 0x07u;
	T_EQ("an empty store still succeeds", matter_dl_attr_load(&loaded), 0);
	T_EQ("AutoRelockTime keeps the port's default", loaded.auto_relock_time_s, 42u);
	T_EQ("so does the approach direction", loaded.approach_direction, 0x07u);

	/*
	 * A store that refuses the write. The RAM value stands for this boot --
	 * the controller's write took effect and only its durability was lost --
	 * so the failure is reported and nothing is rolled back.
	 */
	t_group("matter_dl_attr: a full store is reported, not hidden");
	settingsfake_reset();
	memset(&stored, 0, sizeof(stored));
	stored.auto_relock_time_s = 30u;
	settingsfake_fail_saves_after(0);
	T_OK("a failed save is reported", matter_dl_attr_store(&stored, 0u, 0u) != 0);
	settingsfake_fail_saves_after(-1);
	T_EQ("and nothing was stored", settingsfake_key_count(), 0);

	t_group("matter_dl_attr: erase");
	settingsfake_reset();
	memset(&stored, 0, sizeof(stored));
	stored.auto_relock_time_s = 90u;
	stored.approach_direction = 0x01u;
	T_EQ("store for erase", matter_dl_attr_store(&stored, 0u, 0u), 0);
	T_EQ("erase succeeds", matter_dl_attr_erase(), 0);
	T_EQ("both keys are gone", settingsfake_key_count(), 0);

	/*
	 * And the two trees stay separate. A factory reset un-commissions the
	 * node; forgetting the owner's auto-lock timing is not part of that.
	 */
	t_group("matter_dl_attr: the identity erase leaves them alone");
	settingsfake_reset();
	fill_identity(&stored);
	stored.auto_relock_time_s = 75u;
	stored.approach_direction = 0x02u;
	T_EQ("identity stored", store_identity(&stored), 0);
	T_EQ("attributes stored", matter_dl_attr_store(&stored, 0u, 0u), 0);
	T_EQ("erasing the identity succeeds", matter_fab_erase(), 0);
	T_OK("and the attributes are still there",
	     settingsfake_has("mdl/art") && settingsfake_has("mdl/apd"));

	memset(&loaded, 0, sizeof(loaded));
	(void)matter_dl_attr_load(&loaded);
	T_EQ("they still read back", loaded.auto_relock_time_s, 75u);
	T_EQ("both of them", loaded.approach_direction, 0x02u);
}

int main(void)
{
	test_matter_fab_settings();

	if (t_fail > 0) {
		printf("  cdk-fab-settings: FAIL (%d of %d)\n", t_fail, t_fail + t_pass);
		return 1;
	}
	printf("  cdk-fab-settings: PASS (%d checks — fake settings store, no NVS durability)\n",
	       t_pass);
	return 0;
}

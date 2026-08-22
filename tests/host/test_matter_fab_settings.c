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
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/settings/settings.h>

#include "matter_fab_settings.h"
#include "settingsfake.h"
#include "test.h"
#include "ultrawidelock_prov.h"

/* The keys matter_fab_settings.c writes, in the order it writes them. */
#define K_VER   "mfab/ver"
#define K_FAB0  "mfab/f0"
#define K_FAB1  "mfab/f1"
#define K_TD    "mfab/td"
#define K_XP    "mfab/xp"
#define K_ICLEN "mfab/il"
#define K_ICAC  "mfab/ic"

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

	info->thread_dataset_len = 32u;
	for (size_t i = 0; i < info->thread_dataset_len; i++) {
		info->thread_dataset[i] = (uint8_t)(0x40u + i);
	}
	info->have_thread_xpanid = true;
	memcpy(info->thread_xpanid, "\xde\xad\x00\xbe\xef\x00\xca\xfe", 8);
}

void test_matter_fab_settings(void)
{
	struct matter_device_info stored;
	struct matter_device_info loaded;
	int rc;

	t_group("matter_fab_settings: round trip");
	settingsfake_reset();
	fill_identity(&stored);

	rc = matter_fab_store(&stored);
	T_EQ("store succeeds", rc, 0);
	T_OK("version key written", settingsfake_has(K_VER));
	T_OK("fabric 0 written", settingsfake_has(K_FAB0));
	T_OK("fabric 1 written", settingsfake_has(K_FAB1));
	T_OK("dataset written", settingsfake_has(K_TD));
	T_OK("xpanid written", settingsfake_has(K_XP));

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
	T_OK("a restored record means commissioning finished", loaded.commissioning_complete);

	t_group("matter_fab_settings: nothing stored");
	settingsfake_reset();
	memset(&loaded, 0, sizeof(loaded));
	rc = matter_fab_load(&loaded);
	T_EQ("load reports an empty store", rc, 1);
	T_OK("and claims no commissioning", !loaded.commissioning_complete);

	/*
	 * THE ONE THAT MATTERS.
	 *
	 * matter_fab_store() writes seven keys in sequence with no commit
	 * record. A reset between any two of them leaves KEY_VER present and
	 * matching -- it is written FIRST -- so the version guard in fab_set()
	 * passes and the load restores a fabric table that was never finished.
	 * matter_fab_load() then sets commissioning_complete = true on it,
	 * because "a stored record MEANS commissioning finished" is only true
	 * of a record that was fully written.
	 *
	 * The node then advertises operationally with half an identity and can
	 * never complete CASE -- which the file's own comment already calls
	 * worse than having nothing ("half an identity is worse than none").
	 *
	 * Coming up commissionable is the correct outcome: the pairing did not
	 * survive, and saying so costs a re-pair, while pretending otherwise
	 * costs a device that is bricked from the controller's point of view.
	 */
	t_group("matter_fab_settings: torn write");
	settingsfake_reset();
	fill_identity(&stored);
	settingsfake_fail_saves_after(2); /* version + fabric 0 land, then power dies */

	rc = matter_fab_store(&stored);
	T_OK("a torn store reports failure", rc != 0);
	T_EQ("and stopped after the writes that succeeded", settingsfake_save_count(), 2);

	settingsfake_fail_saves_after(-1);
	memset(&loaded, 0, sizeof(loaded));
	rc = matter_fab_load(&loaded);
	T_OK("a torn record does NOT restore as commissioned", !loaded.commissioning_complete);
	T_OK("and no fabric is presented", loaded.fabrics[0].index == 0u &&
						  loaded.fabrics[1].index == 0u);

	t_group("matter_fab_settings: erase");
	settingsfake_reset();
	fill_identity(&stored);
	T_EQ("store for erase", matter_fab_store(&stored), 0);
	T_OK("keys exist before erase", settingsfake_key_count() > 0);
	T_EQ("erase succeeds", matter_fab_erase(), 0);
	T_EQ("every key is gone", settingsfake_key_count(), 0);

	memset(&loaded, 0, sizeof(loaded));
	T_EQ("load after erase reports an empty store", matter_fab_load(&loaded), 1);

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
	T_EQ("identity stored", matter_fab_store(&stored), 0);
	T_EQ("attributes stored", matter_dl_attr_store(&stored, 0u, 0u), 0);
	T_EQ("erasing the identity succeeds", matter_fab_erase(), 0);
	T_OK("and the attributes are still there",
	     settingsfake_has("mdl/art") && settingsfake_has("mdl/apd"));

	memset(&loaded, 0, sizeof(loaded));
	(void)matter_dl_attr_load(&loaded);
	T_EQ("they still read back", loaded.auto_relock_time_s, 75u);
	T_EQ("both of them", loaded.approach_direction, 0x02u);
}

static void fill_provisioning(struct ultrawidelock_reader_identity *id,
			      struct ultrawidelock_trust_store *ts)
{
	memset(id, 0, sizeof(*id));
	memset(ts, 0, sizeof(*ts));
	for (size_t i = 0; i < sizeof(id->reader_id); i++) {
		id->reader_id[i] = (uint8_t)(0x10u + i);
		id->sign_priv[i] = (uint8_t)(0x80u + i);
	}
	memset(id->grk, 0x5a, sizeof(id->grk));
	id->is_dev = false;
	ts->count = 1u;
	ts->cred_pub[0][0] = 0x04u;
	memset(&ts->cred_pub[0][1], 0xa5, ULTRAWIDELOCK_CRED_PUB_LEN - 1u);
	ts->kp_valid = 1u;
	memset(ts->kpersistent[0], 0x3c, ULTRAWIDELOCK_KPERSISTENT_LEN);
	ts->cred_type[0] = 5u;
	ts->cred_index[0] = 0x1234u;
	ts->user_index[0] = 0x5678u;
}

static void test_ultrawidelock_prov_settings(void)
{
	struct ultrawidelock_reader_identity stored_id;
	struct ultrawidelock_reader_identity loaded_id;
	struct ultrawidelock_trust_store stored_ts;
	struct ultrawidelock_trust_store loaded_ts;
	uint8_t malformed[] = {'b', 'a', 'd'};
	uint8_t oversized[ULTRAWIDELOCK_PROV_BLOB_MAX + 1u] = {0};

	t_group("ultrawidelock_prov_settings: empty and sibling records");
	settingsfake_reset();
	memset(&loaded_id, 0, sizeof(loaded_id));
	memset(&loaded_ts, 0xff, sizeof(loaded_ts));
	T_EQ("empty store is distinct from storage failure",
	     ultrawidelock_prov_load(&loaded_id, &loaded_ts), ULTRAWIDELOCK_PROV_LOAD_EMPTY);
	T_OK("empty store exposes only marked recovery identity", loaded_id.is_dev);
	T_EQ("empty store exposes no trust anchors", loaded_ts.count, 0);
	T_EQ("unrelated sibling can be stored",
	     settings_save_one("ultrawidelock/other", malformed, sizeof(malformed)), 0);
	T_EQ("unrelated sibling is not treated as provisioning",
	     ultrawidelock_prov_load(&loaded_id, &loaded_ts), ULTRAWIDELOCK_PROV_LOAD_EMPTY);

	t_group("ultrawidelock_prov_settings: valid round trip");
	settingsfake_reset();
	fill_provisioning(&stored_id, &stored_ts);
	T_EQ("provisioning store succeeds", ultrawidelock_prov_store(&stored_id, &stored_ts), 0);
	memset(&loaded_id, 0, sizeof(loaded_id));
	memset(&loaded_ts, 0, sizeof(loaded_ts));
	T_EQ("stored provisioning loads", ultrawidelock_prov_load(&loaded_id, &loaded_ts), 0);
	T_OK("reader identity survives",
	     memcmp(loaded_id.reader_id, stored_id.reader_id, sizeof(stored_id.reader_id)) == 0 &&
		     memcmp(loaded_id.sign_priv, stored_id.sign_priv,
			    sizeof(stored_id.sign_priv)) == 0 &&
		     memcmp(loaded_id.grk, stored_id.grk, sizeof(stored_id.grk)) == 0 &&
		     !loaded_id.is_dev);
	T_OK("anchor and expedited key survive",
	     loaded_ts.count == 1u && loaded_ts.kp_valid == 1u &&
		     memcmp(loaded_ts.cred_pub[0], stored_ts.cred_pub[0],
			    ULTRAWIDELOCK_CRED_PUB_LEN) == 0 &&
		     memcmp(loaded_ts.kpersistent[0], stored_ts.kpersistent[0],
			    ULTRAWIDELOCK_KPERSISTENT_LEN) == 0);
	T_OK("Matter indices survive", loaded_ts.cred_type[0] == 5u &&
					 loaded_ts.cred_index[0] == 0x1234u &&
					 loaded_ts.user_index[0] == 0x5678u);

	t_group("ultrawidelock_prov_settings: corrupt storage fails closed");
	settingsfake_reset();
	T_EQ("malformed record can be injected",
	     settings_save_one("ultrawidelock/prov", malformed, sizeof(malformed)), 0);
	memset(&loaded_id, 0, sizeof(loaded_id));
	memset(&loaded_ts, 0xff, sizeof(loaded_ts));
	T_EQ("malformed record is an explicit error",
	     ultrawidelock_prov_load(&loaded_id, &loaded_ts), -EBADMSG);
	T_OK("malformed record cannot expose production identity", loaded_id.is_dev);
	T_EQ("malformed record cannot expose trust anchors", loaded_ts.count, 0);
	settingsfake_reset();
	T_EQ("oversized record can be injected",
	     settings_save_one("ultrawidelock/prov", oversized, sizeof(oversized)), 0);
	T_EQ("oversized record preserves handler errno",
	     ultrawidelock_prov_load(&loaded_id, &loaded_ts), -EINVAL);
	T_OK("oversized record remains fail-closed", loaded_id.is_dev && loaded_ts.count == 0u);

	t_group("ultrawidelock_prov_settings: write and erase failures surface");
	settingsfake_reset();
	settingsfake_fail_saves_after(0);
	T_EQ("full settings store is reported", ultrawidelock_prov_store(&stored_id, &stored_ts),
	     -ENOSPC);
	settingsfake_fail_saves_after(-1);
	T_EQ("erase succeeds", ultrawidelock_prov_erase(), 0);
	T_EQ("erase removes exact provisioning key", settingsfake_key_count(), 0);
}

void test_kv_zephyr(void); /* test_kv_zephyr.c, same stage, same fake */

int main(void)
{
	test_matter_fab_settings();
	test_ultrawidelock_prov_settings();
	test_kv_zephyr();

	if (t_fail > 0) {
		printf("  cdk-fab-settings: FAIL (%d of %d)\n", t_fail, t_fail + t_pass);
		return 1;
	}
	printf("  cdk-settings: PASS (%d checks — fake settings store, no NVS durability)\n",
	       t_pass);
	return 0;
}

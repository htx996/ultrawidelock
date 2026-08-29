#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
backend="$repo_root/ports/zephyr/ble/ultrawidelock_ble_zephyr.c"
app_conf="$repo_root/apps/dwm3001cdk-lock/prj.conf"
overlay_dir="$repo_root/apps/dwm3001cdk-lock/overlays"

need() {
	local pattern=$1
	local file=$2
	if ! grep -Eq "$pattern" "$file"; then
		echo "missing '$pattern' in ${file#"$repo_root"/}" >&2
		exit 1
	fi
}

reject() {
	local pattern=$1
	local file=$2
	if grep -Eq "$pattern" "$file"; then
		echo "forbidden '$pattern' in ${file#"$repo_root"/}" >&2
		exit 1
	fi
}

# Source contract: active legacy advertising is updated without a stop/start
# gap, refresh/recovery is delayable, and both negotiated results are visible.
need 'bt_le_adv_update_data' "$backend"
need 'K_WORK_DELAYABLE_DEFINE\(s_advertising_work' "$backend"
need 'ULTRAWIDELOCK_ADV_RETRY_MAX_MS' "$backend"
need '\.le_data_len_updated = on_le_data_len_updated' "$backend"
need '\.le_phy_updated = on_le_phy_updated' "$backend"
# bt_le_adv_stop was rejected outright, and the invariant behind that is kept:
# a PAYLOAD refresh must never stop the set. The dynamic tag rotates roughly
# every nine minutes and a stop/start there would blink the board out of
# existence while somebody could be walking up to it, so advertising_apply()
# updates payloads in place (the bt_le_adv_update_data need() above).
#
# What is now allowed is exactly one call, for a RATE change, which Zephyr's
# legacy advertising API cannot do any other way -- update_data changes bytes,
# not timing. It is fenced three ways and all three are checked below: at most
# one occurrence, only alongside the rate-change predicate, and only inside the
# CONFIG_ULTRAWIDELOCK_BLE_ADV_SLOW guard so a default build cannot reach it.
# Count CALLS, not mentions: the block comment above that call explains why it
# is the only one, and naming the symbol there must not trip its own guard.
# Continuation lines of a block comment start with '*', so drop those.
adv_stop_calls=$(grep -n 'bt_le_adv_stop' "$backend" | grep -cvE ':[[:space:]]*\*' || true)
if (( adv_stop_calls > 1 )); then
	echo "bt_le_adv_stop called more than once in ${backend#"$repo_root"/};" \
	     "only the advertising rate change may stop the set" >&2
	exit 1
fi
if (( adv_stop_calls > 0 )); then
	need 'adv_rate_change_due' "$backend"
	need 'defined\(CONFIG_ULTRAWIDELOCK_BLE_ADV_SLOW\)' "$backend"
fi
reject '\(time_t\)\(UINT32_MAX' "$backend"
if (( $(grep -Ec '\(uint64_t\)now <= UINT32_MAX - ULTRAWIDELOCK_ADV_TAG_VALID_S' \
	"$backend") != 2 )); then
	echo "both advertising time-validity checks must widen before comparing" >&2
	exit 1
fi

# Enabling USER_DATA_LEN_UPDATE would otherwise turn off Zephyr's previous
# automatic DLE policy. Shipping observes both callbacks but requests no PHY
# change and does not adopt a 527-byte Host ACL geometry.
need '^CONFIG_BT_USER_DATA_LEN_UPDATE=y$' "$app_conf"
need '^CONFIG_BT_AUTO_DATA_LEN_UPDATE=y$' "$app_conf"
need '^CONFIG_BT_USER_PHY_UPDATE=y$' "$app_conf"
need '^CONFIG_BT_BUF_ACL_RX_SIZE=255$' "$app_conf"
need '^CONFIG_BT_BUF_ACL_TX_SIZE=251$' "$app_conf"
need '^CONFIG_ULTRAWIDELOCK_RANGE_GATE_STRICT=y$' "$app_conf"
reject '^CONFIG_BT_BUF_ACL_(RX|TX)_SIZE=527$' "$overlay_dir"/bench-ble-*.conf

# Each experiment changes one variable; the combined arm is intentionally
# separate so isolated results keep attribution.
need '^CONFIG_BT_CTLR_DATA_LENGTH_MAX=251$' "$overlay_dir/bench-ble-dle251.conf"
need '^CONFIG_BT_AUTO_PHY_PERIPHERAL_NONE=y$' "$overlay_dir/bench-ble-dle251.conf"
need '^CONFIG_BT_CTLR_DATA_LENGTH_MAX=27$' "$overlay_dir/bench-ble-phy2m.conf"
need '^CONFIG_BT_AUTO_PHY_PERIPHERAL_2M=y$' "$overlay_dir/bench-ble-phy2m.conf"
need '^CONFIG_BT_CTLR_DATA_LENGTH_MAX=251$' \
	"$overlay_dir/bench-ble-dle251-phy2m.conf"
need '^CONFIG_BT_AUTO_PHY_PERIPHERAL_2M=y$' \
	"$overlay_dir/bench-ble-dle251-phy2m.conf"

# Optional generated-config matrix: shipping, DLE-only, PHY-only, combined.
if (( $# != 0 && $# != 4 )); then
	echo "usage: $0 [shipping.config dle.config phy.config combined.config]" >&2
	exit 2
fi

if (( $# == 4 )); then
	shipping=$1
	dle=$2
	phy=$3
	combined=$4

	need '^CONFIG_BT_USER_DATA_LEN_UPDATE=y$' "$shipping"
	need '^CONFIG_BT_AUTO_DATA_LEN_UPDATE=y$' "$shipping"
	need '^CONFIG_BT_USER_PHY_UPDATE=y$' "$shipping"
	need '^CONFIG_BT_CTLR_DATA_LENGTH_MAX=27$' "$shipping"
	need '^CONFIG_BT_AUTO_PHY_PERIPHERAL_NONE=y$' "$shipping"
	need '^CONFIG_ULTRAWIDELOCK_RANGE_GATE_STRICT=y$' "$shipping"

	need '^CONFIG_BT_CTLR_DATA_LENGTH_MAX=251$' "$dle"
	need '^CONFIG_BT_AUTO_PHY_PERIPHERAL_NONE=y$' "$dle"
	need '^CONFIG_BT_BUF_ACL_RX_SIZE=255$' "$dle"
	need '^CONFIG_BT_BUF_ACL_TX_SIZE=251$' "$dle"

	need '^CONFIG_BT_CTLR_DATA_LENGTH_MAX=27$' "$phy"
	need '^CONFIG_BT_AUTO_PHY_PERIPHERAL_2M=y$' "$phy"
	need '^CONFIG_BT_BUF_ACL_RX_SIZE=255$' "$phy"
	need '^CONFIG_BT_BUF_ACL_TX_SIZE=251$' "$phy"

	need '^CONFIG_BT_CTLR_DATA_LENGTH_MAX=251$' "$combined"
	need '^CONFIG_BT_AUTO_PHY_PERIPHERAL_2M=y$' "$combined"
	need '^CONFIG_BT_BUF_ACL_RX_SIZE=255$' "$combined"
	need '^CONFIG_BT_BUF_ACL_TX_SIZE=251$' "$combined"
fi

echo "BLE link/liveness checks passed"

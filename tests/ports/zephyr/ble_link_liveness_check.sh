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
reject 'bt_le_adv_stop' "$backend"
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

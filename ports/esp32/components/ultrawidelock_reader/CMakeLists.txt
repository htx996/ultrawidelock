# credential reader engine as an ESP-IDF component.
# The transaction driver, wire codec, trust store and ranging glue come from
# the role manifests in modules/ultrawidelock_cred/roles/; the same lists apps/dwm3001cdk-lock/
# and the host suite read. Only ultrawidelock_prov_nvs.c is ESP32-specific, and only
# because it logs through ESP_LOG* -- the blob itself goes through ultrawidelock_kv.h now,
# the same seam its Zephyr and FreeRTOS twins use.

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CRED "${REPO_ROOT}/modules/ultrawidelock_cred")

include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
ultrawidelock_role_sources("${CRED}/roles/reader.list" ULTRAWIDELOCK_READER_SRCS)
ultrawidelock_role_sources("${CRED}/roles/apdu.list" ULTRAWIDELOCK_APDU_SRCS)
ultrawidelock_role_sources("${CRED}/roles/reader_policy.list" ULTRAWIDELOCK_POLICY_SRCS)
# Step-up (Access Document) + presence-assertion pure codecs. Always compiled
# (pure functions, no side effects) but only invoked from ultrawidelock_reader.c under
# CONFIG_ULTRAWIDELOCK_CRED_STEPUP (default n); ultrawidelock_assert.c is backend-free by
# design and only the presence dongle app (CONFIG_ULTRAWIDELOCK_PRESENCE) calls it.
ultrawidelock_role_sources("${CRED}/roles/wire_codecs.list" ULTRAWIDELOCK_WIRE_CODEC_SRCS)

idf_component_register(
	SRCS
		${ULTRAWIDELOCK_READER_SRCS}
		${ULTRAWIDELOCK_APDU_SRCS}
		${ULTRAWIDELOCK_POLICY_SRCS}
		${ULTRAWIDELOCK_WIRE_CODEC_SRCS}
		# Single-consumer shared files, deliberately literal (allowlisted as
		# NOT_MANIFESTED in tests/tooling/port_purity_check.sh): the step-up
		# worker's engine half whose ESP worker body is gated to empty when
		# CONFIG_ULTRAWIDELOCK_CRED_STEPUP is off, and the ultrawidelock_assert P-256 shim —
		# the only one of the assert pair with a crypto dependency.
		"${CRED}/src/ultrawidelock_stepup.c"
		"${CRED}/src/ultrawidelock_assert_ec.c"
		"ultrawidelock_stepup_worker.c"
		"ultrawidelock_prov_nvs.c"
		# The `presence` console command. Lives here rather than in one app so
		# both the standalone reader and the Matter lock can serve presence: the
		# lock is the board a phone is actually provisioned against, so requiring
		# a separate dongle app would mean cloning an identity to use it at all.
		"presence_link.c"
	INCLUDE_DIRS "${CRED}/include" "."
	PRIV_INCLUDE_DIRS "${CRED}/src"
	# ultrawidelock_port.h / ultrawidelock_log.h (the platform contract ultrawidelock_reader.c and
	# ultrawidelock_ranging.c are written to) come via ultrawidelock_uwb's public include dirs
	# (modules/ultrawidelock_port/include). ultrawidelock_port is named for the symbols, not
	# the headers: ultrawidelock_prov_nvs.c stores its blob through ultrawidelock_kv.h, whose
	# backend on this port is that component's kv_nvs.c.
	REQUIRES ultrawidelock_ble ultrawidelock_uwb ultrawidelock_crypto ultrawidelock_port nvs_flash
)

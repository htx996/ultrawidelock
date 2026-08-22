# The sealed anchor link's ESP-NOW carrier. Bytes and key storage only: every
# decision belongs to modules/ultrawidelock_anchor (ultrawidelock_link.c), which
# the ultrawidelock_anchor component supplies and the Thread port shares, so the
# two carriers cannot come to different conclusions about the same datagram.
#
# No role manifest here: this file is ESP-IDF-specific by nature (esp_now,
# esp_wifi, nvs) and has exactly one consumer per role. Manifests exist to stop
# a SHARED source being listed twice; a platform file has nowhere else to go.

idf_component_register(
	SRCS "ultrawidelock_satlink.c"
	INCLUDE_DIRS "include"
	REQUIRES
		# the link core, the codec and the seal
		ultrawidelock_anchor
		# esp_wifi carries ESP-NOW; nvs_flash keeps the link key
		esp_wifi esp_event esp_netif nvs_flash
)

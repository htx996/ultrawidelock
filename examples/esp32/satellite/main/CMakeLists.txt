idf_component_register(
	SRCS "main.c" "sat_join.c" "sat_console.c"
	REQUIRES
		# the ranging engine (responder + CCC STS) and the DW3000 backend
		ultrawidelock_uwb
		# the sealed link: ESP-NOW carrier, and through it the link core,
		# the WV3/WV4 codec and the AES-CCM envelope
		ultrawidelock_satlink ultrawidelock_anchor ultrawidelock_crypto
		console nvs_flash
)

# The responder count and index come from the PROJECT CMakeLists, which is the
# one place they are chosen, and are handed to the engine component from there
# too. Passing them through rather than re-declaring them is what keeps this
# app's `rcfg[12]` check and the engine's SaltedHash agreeing about the same
# number -- and a disagreement there decodes nothing while looking like a dead
# radio.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
	SAT_NUM_RESPONDERS=${ULTRAWIDELOCK_NUM_RESPONDERS}
	SAT_RESPONDER_INDEX=${ULTRAWIDELOCK_RESPONDER_INDEX})

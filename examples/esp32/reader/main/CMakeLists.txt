# ultrawidelock_anchor: the two-anchor geometry, the WV3 codec and the sealed
# link's envelope. Required here before this app calls any of it, deliberately —
# it is what makes the ESP toolchain compile and link the shared tier on every
# build of this project, so a change that breaks the port fails here rather than
# on the bench. ESP-IDF links with --gc-sections, so what nothing calls costs
# nothing.
#
# ultrawidelock_satlink: the ESP-NOW carrier for that link. This app is where
# the lock half lands (stage E), and requiring it now is what compiles and
# links the carrier on every build of this project rather than only when the
# bench is set up.
idf_component_register(SRCS "main.c" "app_shell.c" REQUIRES ultrawidelock_uwb ultrawidelock_reader ultrawidelock_anchor ultrawidelock_satlink console esp_app_format nvs_flash)

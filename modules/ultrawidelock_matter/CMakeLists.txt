# Hand-written minimal Matter node as a Zephyr module.
# Contributes nothing unless CONFIG_ULTRAWIDELOCK_MATTER=y (every existing build stays
# byte-identical). Why this exists at all:
# CHIP does not fit the nRF52833 and rs-matter cannot supply a Door Lock
# cluster, so the DWM3001CDK can only self-provision from Apple Home if the
# node is written by hand.
#
# Platform-agnostic C11 with no Zephyr dependency, so tests/host compiles the
# same sources directly (tests/host/sources.sh).

if(CONFIG_ULTRAWIDELOCK_MATTER)

  set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

  zephyr_library_named(ultrawidelock_matter)
  zephyr_library_sources(
    ${SRC}/matter_tlv.c
    ${SRC}/matter_msg.c
    ${SRC}/matter_mrp.c
    ${SRC}/matter_crypto.c
    ${SRC}/matter_btp.c
    ${SRC}/matter_pase.c
    ${SRC}/matter_spake2p.c
    ${SRC}/matter_pase_sm.c
    ${SRC}/matter_exchange.c
    ${SRC}/matter_im.c
    ${SRC}/matter_clusters.c
    ${SRC}/matter_attest.c
    ${SRC}/matter_fabric.c
    ${SRC}/matter_case.c)

  # The CASE initiator, and nothing else, rides with the client build.
  #
  # A separate file behind its own symbol rather than more of matter_case.c,
  # because the point of the option is that an image without it is byte for
  # byte the image that existed before it -- and a preprocessor guard inside a
  # compiled file cannot promise that as plainly as not compiling the file.
  if(CONFIG_ULTRAWIDELOCK_MATTER_CLIENT)
    zephyr_library_sources(
      ${SRC}/matter_case_client.c
      ${SRC}/matter_client_sm.c
      ${SRC}/matter_im_client.c
      ${SRC}/matter_binding.c)
    # The client-role code that has no file of its own to live in: the Binding
    # cluster's surface inside matter_clusters.{c,h} -- the target list has to
    # be reachable from a controller and has to sit in struct
    # matter_device_info -- and the two initiator entry points in
    # matter_exchange.c. Neither can move to a separate file, so both are
    # guarded by the macro instead and preprocess away without it.
    #
    # Global rather than library-scoped, for the same reason as
    # MATTER_FEATURE_DL_ALARMS below: the app includes matter_clusters.h, and a
    # header that means one thing to the module and another to its caller is a
    # struct layout mismatch waiting to happen.
    zephyr_compile_definitions(MATTER_FEATURE_CLIENT=1)
  endif()

  # Same reasoning as the client macro above: it changes struct
  # matter_device_info, so the app and the module have to agree on it.
  if(CONFIG_ULTRAWIDELOCK_MATTER_MULTI_ADMIN)
    zephyr_compile_definitions(MATTER_FEATURE_MULTI_ADMIN=1)
  endif()

  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

  # The DoorLockAlarm event, and nothing else, rides with the anchor build.
  #
  # A compile definition rather than a Kconfig symbol because the sources it
  # reaches are platform-agnostic C11 that tests/host builds directly, with no
  # Kconfig anywhere near it -- see matter_clusters.h, which defaults the macro
  # to 0. Global rather than library-scoped: the app includes matter_clusters.h
  # to call the recorder, and a header that means one thing to the module and
  # another to its caller is a struct layout mismatch waiting to happen.
  #
  # CONFIG_ULTRAWIDELOCK_ANCHOR is the condition because the alarms this node
  # can raise are the impact latch (ULTRAWIDELOCK_ANCHOR_SLAM, itself inside
  # ANCHOR) and the door swing angle, and neither exists without it. Off, every
  # file here preprocesses to what it was before alarms existed, so the default
  # image stays byte-identical.
  if(CONFIG_ULTRAWIDELOCK_ANCHOR)
    zephyr_compile_definitions(MATTER_FEATURE_DL_ALARMS=1)
  endif()

  # matter_crypto.c uses ultrawidelock_hash.h for SHA-256/HKDF and the
  # crypto_aes_ecb_encrypt contract. Consumer role composition supplies the
  # portable hash and the AES adapter over the image's explicitly linked
  # ultrawidelock_prim provider, so this is an include path, not new code. It is
  # still a real dependency, which CONFIG_ULTRAWIDELOCK_MATTER records with
  # `depends on ULTRAWIDELOCK_CRED`.
  #
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../ultrawidelock_cred/include)

endif(CONFIG_ULTRAWIDELOCK_MATTER)

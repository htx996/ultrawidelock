# Sysbuild-level build guards: the APPROTECT lock, and the image-signing key.
# Both are settings that are cheap to get wrong and expensive to discover late,
# and both are invisible to apps/dwm3001cdk-lock/CMakeLists.txt for the same reason.
#
# CMakeLists.txt catches CONFIG_NRF_*APPROTECT_LOCK on the application image.
# It cannot see SB_CONFIG_*, because those are evaluated one level up, before
# any image is configured -- and sysbuild is exactly where an APPROTECT setting
# would now be written, since MCUboot arrived as a second image with a config
# file of its own. Loaded automatically: sysbuild includes <app>/sysbuild.cmake
# if it exists (zephyr/share/sysbuild/cmake/modules/sysbuild_extensions.cmake:929).
#
# Why this matters more than it looks: the lock re-asserts in SystemInit() on
# every boot, and the only exit is `nrfjprog --recover`, which mass-erases flash
# AND UICR. On this board that destroys settings_storage at 0x7c000 -- the Matter
# fabrics, the trust anchors, and the reader private key that every iPhone key
# was provisioned against. Not a brick; a permanent loss of the board's identity.
if(SB_CONFIG_APPROTECT_LOCK OR SB_CONFIG_SECURE_APPROTECT_LOCK)
	message(FATAL_ERROR
		"APPROTECT lock is enabled in sysbuild. Refusing to build.\n"
		"  SB_CONFIG_APPROTECT_LOCK        = ${SB_CONFIG_APPROTECT_LOCK}\n"
		"  SB_CONFIG_SECURE_APPROTECT_LOCK = ${SB_CONFIG_SECURE_APPROTECT_LOCK}\n"
		"See apps/dwm3001cdk-lock/sysbuild.conf for why this board never locks.")
endif()

# ---- the image-signing key ---------------------------------------------------
#
# MCUboot boots slot 0 only if it verifies against a public key compiled into the
# bootloader, so the private half is the entire answer to "what firmware will
# this lock run". Configure nothing and MCUboot signs with root-ec-p256.pem out
# of its OWN repository, which is published there: every stock MCUboot in the
# world accepts images signed with it. On a lock that is not a signing key, it
# is a formality.
#
# MCUboot does notice, at bootloader/mcuboot/boot/zephyr/CMakeLists.txt:449-452,
# and calls message(WARNING). That is precisely why it survived here: a warning
# in a ten-thousand-line build log is indistinguishable from no warning. These
# are fatal instead.
#
# The four refusals -- unset, a demo basename, a relative path, a path that does
# not exist -- and the list of demo names they are checked against live in
# scripts/check-signing-key.sh, not here. The nRF5340 DK reaches the same
# bootloader through a fetched upstream application that this repo never edits,
# so it cannot be given a sysbuild.cmake of its own and has to make the same
# decision from its build script. One file both callers run is the only way the
# two boards cannot drift apart on which keys are acceptable.
#
# SB_CONFIG_* is populated here: Kconfig must run before images are added (it is
# SB_CONFIG_BOOTLOADER_MCUBOOT that causes the MCUboot image to exist at all),
# and sysbuild.cmake is included only after they have been
# (sysbuild_extensions.cmake:906-931). An unset symbol expands to the empty
# string, which the script reads as "nothing configured" -- the case that would
# otherwise fall through to the demo key.
set(sign_key_checker "${CMAKE_CURRENT_LIST_DIR}/../../scripts/check-signing-key.sh")

execute_process(
	COMMAND "${sign_key_checker}" "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}"
	RESULT_VARIABLE sign_key_result
	ERROR_VARIABLE sign_key_error)

# A launch failure -- script missing, or its execute bit lost -- puts a message
# in RESULT_VARIABLE rather than an exit code, and leaves ERROR_VARIABLE empty.
# It still fails the build, which is the right way round, but say why instead of
# raising a blank error.
if(NOT sign_key_result EQUAL 0)
	if(sign_key_error STREQUAL "")
		message(FATAL_ERROR
			"Could not run the signing-key check, so this build cannot know which\n"
			"key MCUboot would trust. Refusing to build.\n"
			"  ${sign_key_checker}\n"
			"  ${sign_key_result}")
	endif()
	message(FATAL_ERROR "\n${sign_key_error}")
endif()

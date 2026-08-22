#!/usr/bin/env bash
#
# check-signing-key.sh — refuse to build a bootloader that anybody can sign for.
#
# WHAT IS BEING PREVENTED. MCUboot boots slot 0 only if the image verifies
# against a public key compiled into the bootloader, so the private half is the
# whole answer to "what firmware will this lock run". Configure nothing and
# MCUboot signs with root-ec-p256.pem out of its OWN repository, where that key
# is published. Every stock MCUboot in the world accepts images signed with it.
# On a lock that is not a signing key, it is a formality.
#
# MCUboot does notice, at bootloader/mcuboot/boot/zephyr/CMakeLists.txt:449-452,
# and calls message(WARNING). That is precisely why it survived on this port for
# as long as it did: a warning in a ten-thousand-line build log is
# indistinguishable from no warning. Here it is fatal.
#
#   scripts/check-signing-key.sh <path>      # validate one configured key
#   scripts/check-signing-key.sh --self-test # prove the refusals actually fire
#
# Exit 0 clean, 1 on a finding, 2 if the gate could not do its job.
#
# Both Zephyr ports call this, which is why it is a file rather than a paragraph
# repeated in each: apps/dwm3001cdk-lock/sysbuild.cmake for the DWM3001CDK, and
# scripts/nrf5340dk-build.sh for the nRF5340 DK. One list, one set of refusals,
# one place to edit when upstream adds an eighth demo key. The DK additionally
# reads the key back out of the built mcuboot .config, because a flag we passed
# is not the same fact as a flag the build honoured.

set -euo pipefail

# Same shape as scripts/check-approtect.sh and tests/tooling/uwb_seam_check.sh, the
# sibling gates this one joins.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
	R=$'\033[31m' G=$'\033[32m' Z=$'\033[0m'
else
	R='' G='' Z=''
fi

# MCUboot's own list, copied from lines 439-447 of that CMakeLists.txt and
# checked against the NCS v3.3.0 tree. Compared by BASENAME, so a demo key
# copied somewhere else under a new directory is still caught: the check is
# about which key, not which path.
DEMO_KEYS='root-ec-p256.pem
root-ec-p256-pkcs8.pem
root-ec-p384.pem
root-ec-p384-pkcs8.pem
root-ed25519.pem
root-rsa-2048.pem
root-rsa-3072.pem'

# Print a refusal: first line coloured, the rest indented, all to stderr so a
# caller that captures this can hand it straight to its own fatal error.
refuse() {
	printf '%s%s%s\n' "$R" "$1" "$Z" >&2
	shift
	local line
	for line in "$@"; do printf '  %s\n' "$line" >&2; done
	return 1
}

# Validate one configured signing-key path. Returns 1, with the reason on
# stderr, when the path would leave the bootloader trusting a published key.
check_key() {
	local path="$1" name
	name="${path##*/}"

	# Ordering is deliberate: the demo-name test runs before the absolute-path
	# test, so a RELATIVE demo path reports the useful failure rather than the
	# pedantic one.
	if [ -z "$path" ]; then
		refuse "No MCUboot signing key is configured, so this build would fall back to MCUboot's PUBLIC demo key. Refusing to build." \
			"Fix: make dfu-key"
		return 1
	fi

	if printf '%s\n' "$DEMO_KEYS" | grep -qxF -- "$name"; then
		refuse "That is MCUboot's published demo key, which is not a signing key. Refusing to build." \
			"key: $path" \
			"Fix: make dfu-key"
		return 1
	fi

	# A relative path is resolved against the MCUboot repository
	# (boot/zephyr/CMakeLists.txt:428) and lands back on the demo key without
	# saying so, which is the exact failure this file exists to stop.
	case "$path" in
	/*) ;;
	*)
		refuse "The signing key path must be ABSOLUTE. A relative one resolves inside the MCUboot repository and silently becomes the demo key. Refusing to build." \
			"key: $path" \
			"Fix: make dfu-key"
		return 1
		;;
	esac

	if [ ! -f "$path" ]; then
		refuse "The configured MCUboot signing key does not exist. Refusing to build." \
			"key: $path" \
			"Fix: make dfu-key"
		return 1
	fi

	return 0
}

# ---- self-test --------------------------------------------------------------
#
# A gate whose fixture is wrong passes while checking nothing. Plant every shape
# that must be refused and the one that must pass, and fail loudly on either.

# Script-scoped, not local to self_test: the EXIT trap runs after that function
# has returned, so a `local` would be out of scope by then and `set -u` would
# turn the cleanup itself into the script's exit status.
SELF_TEST_TMP=''
# Clean up the self-test fixture directory. Always succeeds, so an EXIT trap
# can never rewrite the exit status the gate meant to report.
cleanup() {
	[ -n "$SELF_TEST_TMP" ] && rm -rf "$SELF_TEST_TMP"
	return 0
}
trap cleanup EXIT

self_test() {
	local fails=0 tmp key n=0 d case_name arg
	tmp="$(mktemp -d)"
	SELF_TEST_TMP="$tmp"

	# Every demo basename, each at a path that is otherwise perfectly valid:
	# absolute, and present on disk. Only the NAME may cause the refusal.
	while IFS= read -r d; do
		: >"$tmp/$d"
		if check_key "$tmp/$d" 2>/dev/null; then
			printf '%s  self-test FAILED: accepted the demo key %s%s\n' "$R" "$d" "$Z" >&2
			fails=$((fails + 1))
		else
			n=$((n + 1))
		fi
	done <<<"$DEMO_KEYS"
	[ "$fails" -ne 0 ] || printf '%s  self-test: refuses all %d published demo keys%s\n' "$G" "$n" "$Z"

	for case_name in empty relative missing; do
		case "$case_name" in
		empty) arg='' ;;
		relative) arg='apps/dwm3001cdk-lock/keys/mcuboot_ec_p256.pem' ;;
		missing) arg="$tmp/not-generated-yet.pem" ;;
		esac
		if check_key "$arg" 2>/dev/null; then
			printf '%s  self-test FAILED: accepted a %s key path%s\n' "$R" "$case_name" "$Z" >&2
			fails=$((fails + 1))
		fi
	done
	[ "$fails" -ne 0 ] || printf '%s  self-test: refuses unset, relative and absent key paths%s\n' "$G" "$Z"

	# ...and still says yes to a real one. Without this the gate could pass by
	# refusing everything, which is the other way a check stops being a check.
	key="$tmp/mcuboot_ec_p256.pem"
	: >"$key"
	if check_key "$key" 2>/dev/null; then
		printf '%s  self-test: accepts an absolute, present, non-demo key%s\n' "$G" "$Z"
	else
		printf '%s  self-test FAILED: refused a valid per-checkout key%s\n' "$R" "$Z" >&2
		fails=$((fails + 1))
	fi

	if [ "$fails" -ne 0 ]; then
		printf '%scheck-signing-key: the gate itself is broken%s\n' "$R" "$Z" >&2
		return 2
	fi
	return 0
}

# An EMPTY argument is a legitimate input -- it is what "nothing configured"
# looks like to the callers -- so no-argument-at-all is the only usage error.
if [ "$#" -eq 0 ]; then
	printf 'usage: %s <key-path> | --self-test\n' "$0" >&2
	exit 2
fi

case "$1" in
--self-test) self_test ;;
*) check_key "$1" ;;
esac

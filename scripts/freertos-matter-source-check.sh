#!/bin/sh
# The shared Matter Thread transport is compiled unmodified. Assert it still
# fits the shim that carries it.
#
# ports/zephyr/matter/matter_thread_port.c is built into this image as-is,
# against ports/freertos-nrf52833/matter/matter_compat. That works because its
# Zephyr surface is small and fixed. Nothing enforces that on the other side:
# the file belongs to the Zephyr build, and a change there compiles fine THERE.
#
# Two ways it can break this image, and both are quiet:
#
#   1. It starts using a Zephyr facility the shim does not provide. That is a
#      compile error, which is loud, so it is not what this file is for.
#   2. It renames the settings path. The shim's table then has no entry, the
#      backend refuses at RUN TIME on a board, and the only symptom is an SRP
#      host name that changes every boot -- which presents as a node that
#      attaches to Thread and never registers.
#
# The second is what this checks, plus the openthread_*() set, because a new
# helper would compile only if it happened to match something already declared.
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
SRC="$ROOT/ports/zephyr/matter/matter_thread_port.c"
SETTINGS="$ROOT/ports/freertos-nrf52833/matter/matter_settings_freertos.c"

for f in "$SRC" "$SETTINGS"; do
    if [ ! -f "$f" ]; then
        printf '  FAIL no such file: %s\n' "${f#"$ROOT"/}" >&2
        printf '        This check names its own inputs. One has moved, and a\n' >&2
        printf '        file that is not there cannot be checked clean.\n' >&2
        exit 1
    fi
done

fail=0

# --- 1. the settings path both sides have to agree on -----------------------
#
# Read it from the shared file rather than restating it here. A copy would be a
# hand-maintained mirror, which is the shape that stops biting the moment
# someone forgets and reports success either way.
path=$(sed -n 's/^#define SRP_HOST_ID_KEY[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$SRC")
if [ -z "$path" ]; then
    printf '  FAIL could not read SRP_HOST_ID_KEY from %s\n' "${SRC#"$ROOT"/}" >&2
    printf '        The define changed shape. This check cannot compare what it\n' >&2
    printf '        cannot find, and refuses to pass by not finding it.\n' >&2
    exit 1
fi

if ! grep -q "{ \"$path\", ULTRAWIDELOCK_KV_KEY_MATTER_SRP_HOST_ID" "$SETTINGS"; then
    printf '  FAIL settings path "%s" has no entry in the port table.\n' "$path" >&2
    printf '        %s knows it as SRP_HOST_ID_KEY;\n' "${SRC#"$ROOT"/}" >&2
    printf '        %s does not.\n' "${SETTINGS#"$ROOT"/}" >&2
    printf '        Left alone this fails on a board, not here: the SRP host\n' >&2
    printf '        name would change every boot and the node would attach to\n' >&2
    printf '        Thread and never register.\n' >&2
    fail=1
fi

# --- 2. the openthread_*() helpers the shim declares ------------------------
KNOWN='openthread_get_default_instance openthread_mutex_lock openthread_mutex_unlock openthread_run openthread_state_changed_callback_register'

# Only the lines that are actually compiled: CONFIG_ULTRAWIDELOCK_SRP_DIAG is off in
# this image, and the one helper this port has no equivalent for lives inside
# it. Counting it would fail a build that never sees it.
used=$(sed '/^#if defined(CONFIG_ULTRAWIDELOCK_SRP_DIAG)/,/^#endif/d' "$SRC" |
       grep -oE '\bopenthread_[a-z_]+\(' | sed 's/($//;s/(//' | sort -u)

if [ -z "$used" ]; then
    printf '  FAIL found no openthread_*() calls in %s\n' "${SRC#"$ROOT"/}" >&2
    printf '        It uses several. A selector that matches nothing reads as a\n' >&2
    printf '        clean scan, so this is a broken check, not a clean file.\n' >&2
    exit 1
fi

for u in $used; do
    case " $KNOWN " in
        *" $u "*) ;;
        *)
            printf '  FAIL %s() is used by the shared transport and the shim does\n' "$u" >&2
            printf '        not provide it. Add it to matter_compat/openthread.h with\n' >&2
            printf '        the port equivalent, or explain why it is excluded.\n' >&2
            fail=1
            ;;
    esac
done

# The unsolicited sender runs outside OpenThread's own callback thread. Both
# Zephyr and the FreeRTOS shim require the shared OT mutex there, including the
# socket-open publication read. Keep this narrow tripwire because lock helpers
# used elsewhere would otherwise let a regression in this function pass.
send_to=$(sed -n '/^int matter_thread_send_to(/,/^}/p' "$SRC")
if ! printf '%s\n' "$send_to" | grep -q 'openthread_mutex_lock()' ||
   ! printf '%s\n' "$send_to" | grep -q 'openthread_mutex_unlock()' ||
   ! printf '%s\n' "$send_to" | grep -q 'if (!s_udp_open)'; then
    printf '  FAIL matter_thread_send_to() does not lock OT around s_udp_open and UDP send.\n' >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    printf '  matter: shared transport fits the shim (path "%s", %s helpers)\n' \
        "$path" "$(echo $used | wc -w | tr -d ' ')"
fi
exit "$fail"

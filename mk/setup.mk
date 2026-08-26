# mk/setup.mk — getting a machine ready: host gate tools, then the NCS toolchain
# and the fetched west workspace both Zephyr ports build against.

.PHONY: tools tools-install bootstrap ws-link ws-store dfu-key print-sign-key

##@ Setup
## tools: what the host suites need, what this machine has
##   Installs nothing. Exits nonzero when a gate tool is missing, so a suite
##   skipping quietly is never a surprise.
tools:
	@$(REPO_ROOT)/scripts/toolchain.sh

## tools-install: install the missing host tools  ·  prints the commands, asks first
##   macOS/Linux, via whichever of brew/apt/dnf/pacman/zypper is present. Nothing
##   runs before you agree; ASSUME_YES=1 answers in advance. Offers the bench
##   tools too, which no gate needs. The SDKs are not here: those are
##   `make bootstrap` (NCS) and `make esp-bootstrap` (ESP-IDF, esp-matter).
tools-install:
	@$(REPO_ROOT)/scripts/toolchain.sh install

## bootstrap: set this machine up for the repo  ·  the only command before build
##   Checks the host first, then installs the NCS toolchain and fetches the
##   workspace. Interrupt it whenever: every phase resumes on the next run.
##   The workspace lands in the machine's store, named for the pin and the patch
##   set it holds, and ./workspace becomes a link to it. A second checkout that
##   agrees on both links to the same tree: `make ws-link`, no fetch.
##   Options: ULTRAWIDELOCK_WS_STORE=<path>  put the store on another volume
##            ULTRAWIDELOCK_WS=<path>  one workspace at one path, no store at all
##            SETUP_AUTO=1 install missing nrfutil without asking (0 = never ask)
bootstrap:
	@$(NRF_ENV) ./scripts/bootstrap.sh

## ws-link: point THIS checkout at the workspace its branch needs
##   Idempotent, and a symlink in the common case: the store already holds a tree
##   for this pin and patch set. A patch set of this branch's own is a
##   copy-on-write clone of the nearest entry plus a re-patch, not a re-fetch.
##   To link a worktree whose branch predates this script, run it from one that
##   has it: scripts/ws-link.sh <path-to-worktree>
ws-link:
	@$(REPO_ROOT)/scripts/ws-link.sh

## ws-store: every workspace on this machine, and which checkouts link to them
ws-store:
	@$(REPO_ROOT)/scripts/ws-store.sh

## dfu-key: generate this checkout's MCUboot signing key  ·  once per clone
#   Refuses to overwrite: replacing the key strands every board carrying the old
#   public half. Options: SIGN_KEY=<path> (absolute)
dfu-key:
	@if [ -f '$(SIGN_KEY)' ]; then \
	  printf '  key exists, keeping it  ·  %s\n' '$(SIGN_KEY)'; exit 0; \
	fi; \
	mkdir -p '$(dir $(SIGN_KEY))'; \
	if command -v openssl >/dev/null 2>&1; then \
	  openssl ecparam -name prime256v1 -genkey -noout -out '$(SIGN_KEY)'; \
	else \
	  python3 -c 'import sys;from cryptography.hazmat.primitives.asymmetric import ec;from cryptography.hazmat.primitives import serialization as s;open(sys.argv[1],"wb").write(ec.generate_private_key(ec.SECP256R1()).private_bytes(s.Encoding.PEM,s.PrivateFormat.PKCS8,s.NoEncryption()))' '$(SIGN_KEY)'; \
	fi || { printf '  cannot generate a key  ·  need openssl, or python3 with the cryptography module\n' >&2; exit 1; }; \
	chmod 600 '$(SIGN_KEY)'; \
	printf '  generated  ·  %s\n  Gitignored. Back it up wherever your other secrets live.\n' '$(SIGN_KEY)'

# Where this checkout's key would be, for scripts that must check it before they
# touch a board. Bare path on stdout, no decoration: it is read, not displayed.
# Undocumented in `make help` on purpose -- SIGN_KEY has a legacy fallback, and
# this exists so a caller cannot get that resolution subtly wrong.
print-sign-key:
	@printf '%s\n' '$(SIGN_KEY)'

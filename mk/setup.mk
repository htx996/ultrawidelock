# mk/setup.mk — getting a machine ready: host gate tools, then the NCS toolchain
# and the fetched west workspace both Zephyr ports build against.

.PHONY: tools bootstrap dfu-key print-sign-key

##@ Setup
## tools: what the host suites need, what this machine has
tools:
	@$(REPO_ROOT)/scripts/toolchain.sh

## bootstrap: set this machine up for the repo  ·  the only command before build
bootstrap:
	@$(NRF_ENV) ./scripts/bootstrap.sh

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

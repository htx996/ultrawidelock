# PINless proximity unlock on macOS 26

## Scope and evidence

Read-only investigation of smart-card and proximity screen unlock on macOS
26.4.1, build 25E253, with the macOS 26.4 SDK. Evidence:

- public Apple documentation and SDK header contracts;
- a read-only inspection of the macOS authorization database;
- the diagnostic messages the system CryptoTokenKit authorization mechanism
  emits to the unified log;
- a read-only inspection of the system Auto Unlock implementation and its code
  signature entitlements; and
- a controlled test with a paired CryptoTokenKit identity whose signing
  operation was gated by a fresh UWB proximity proof.

Evidence labels:

- **VERIFIED (documentation):** stated by a public Apple source or SDK
  contract.
- **VERIFIED (observation):** reproduced on build 25E253 or found in its
  shipping binaries.
- **LIKELY, verify before using:** an explanation consistent with documented
  and observed facts, but not stated by Apple.
- **I don't know:** not settled by the available evidence.

## Bottom line

A paired CryptoTokenKit smart-card identity cannot provide truly PINless
LoginWindow or screen unlock through Apple's native smart-card mechanism.

A `TKTokenOperationConstraint` value of boolean `true` makes the private-key
operation available without token authentication, but does not remove the login
mechanism's separate requirement for a credential value in the authorization
context. The verified blocker is that nonempty authorization-context gate, not
an authorization decision made by the token. Constraint tuning, key attributes,
and provider activation operate below the gate.

```text
nonempty LoginWindow credential submission
              |
authorization context key "password"
              |
CryptoTokenKit:login / TKAuthMechanismLogin
              |
token slot 9A signature request
              |
fresh UWB proof and private-key operation
```

The UWB transaction begins only after credential submission. Changing the key
constraint or `isSuitableForLogin` cannot remove that gate.

## Distinct unlock paths

Separate architectures, not interchangeable authentication factors:

| Path | Credential-material model | Public third-party integration |
|---|---|---|
| Password | Apple's built-in login mechanisms consume a typed password. The `system.login.console` rule includes `builtin:authenticate`. | Not a token-provider extension surface; authorization plug-ins are a separate architecture. |
| Smart card | `CryptoTokenKit:login` consumes a submitted credential-context value, performs token login, and writes token login data back to that context. | A token extension can implement token operations, but not replace the system login mechanism. |
| Touch ID | Data Protection keys remain wrapped by a key held by the Touch ID subsystem in the Secure Enclave, and a successful match releases the unwrap key. | No public third-party biometric hook for LoginWindow. |
| Apple Watch Auto Unlock | A previously armed 32-byte secret unwraps a record containing the passcode-derived key. | No public third-party Auto Unlock API was found. The shipping implementation uses private entitlements. |

Two authorization rights are easy to confuse:

- `system.login.screensaver` resolves to `use-login-window-ui`.
- `system.login.screensaver.unlock` contains the single mechanism
  `CryptoTokenKit:login` and identifies itself as the screensaver-unlock rule.

Source:

- [Uses for Optic ID, Face ID, and Touch ID](https://support.apple.com/guide/security/secc5227ff3c/web)

## Apple-documented behavior

`TKTokenOperationConstraint` describes authentication for one operation on one
token object. Apple documents boolean `true` as allowing that operation without
authentication; the macOS 26.4 SDK states the same:

```text
$(xcrun --show-sdk-path)/
  System/Library/Frameworks/CryptoTokenKit.framework/Headers/TKToken.h:73
```

`beginAuthForOperation` is the documented way for an extension to establish
token authentication. A smart-card extension can return
`TKTokenSmartCardPINAuthOperation` to request a PIN and optionally supply a PIV
`VERIFY` APDU template:

```text
CryptoTokenKit.framework/Headers/TKToken.h:152
CryptoTokenKit.framework/Headers/TKSmartCardToken.h:16
```

`TKTokenKeychainKey.isSuitableForLogin` only marks a key eligible for system
login. Its declaration promises no control over LoginWindow's user interface or
authorization context:

```text
CryptoTokenKit.framework/Headers/TKTokenKeychainItem.h:116
```

Apple's published model for smart-card login is two-factor: possession of the
token plus knowledge of its password or PIN. Apple documents slot 9A for login
authentication and slot 9D for the key agreement that wraps or unwraps the
login keychain secret.

Sources:

- [TKTokenOperationConstraint](https://developer.apple.com/documentation/cryptotokenkit/tktokenoperationconstraint)
- [Authenticating users with a cryptographic token](https://developer.apple.com/documentation/cryptotokenkit/authenticating-users-with-a-cryptographic-token)
- [Supported smart-card functions on Mac](https://support.apple.com/guide/deployment/supported-smart-card-functions-on-mac-depc47f60521/web)

## Component that imposes the credential requirement

### Authorization database

On build 25E253, a read-only query of `system.login.screensaver.unlock`
returned an `evaluate-mechanisms` rule with one mechanism:

```text
CryptoTokenKit:login
```

The rule's own comment says not to modify it and identifies it as the
screensaver-unlock rule.

### CryptoTokenKit authorization mechanism

```text
/System/Library/CoreServices/SecurityAgentPlugins/
  CryptoTokenKit.bundle/Contents/MacOS/CryptoTokenKit
```

Three of its unified-log messages describe the path this report depends on:

```text
TKAuthMechanismLogin invoked
PIN not found in authorization context
Token login data set to the authorization context
```

The first marks entry. The second is what a PINless attempt produces, with the
authorization-context key `password` empty. The third appears only on success,
when token login data is written back under that key. The companion
`loginwindow` plug-in reports `Attempt to authenticate with a blank password`.
The predicate in [Smallest safe experiment](#smallest-safe-experiment) captures
the three CryptoTokenKit messages; the `loginwindow` one needs a wider filter.

The sequence establishes that `TKAuthMechanismLogin` requires the
authorization-context value to process the token identity, not that the
submitted characters reach the smart card or are validated as its PIN.

## Credential material, not just an authorization verdict

Two independently observed designs point to a credential-material requirement:

1. **VERIFIED (observation):** after successful token login,
   `TKAuthMechanismLogin` reports writing token login data back under the
   authorization-context key `password`.
2. **VERIFIED (documentation):** Apple Watch Auto Unlock returns credential
   material. The Mac generates a random 32-byte unlock secret, sends it through
   a Secure-Enclave-to-Secure-Enclave Station-to-Station tunnel, and wraps the
   passcode-derived key with it during a normal unlock. A later proximity
   transaction returns that secret so the Mac can decrypt the unlock record.

**LIKELY, verify before using:** the `password` context slot transports material
needed by the login session, not merely a boolean "authorized" result. That
explains both the pre-token nonempty check and the token-login write-back.

Apple's documented use of PIV slot 9D for login-keychain wrapping is consistent
with this model, and makes a key-management slot load-bearing even when slot 9A
is the authentication identity.

Falsifiable prediction: if an experiment satisfies only the nonempty context
gate with no valid token login material, the screen and login keychain may not
reach the same state. Screen-unlocked-but-keychain-locked supports the model; a
fully unlocked keychain means checking whether another mechanism supplied it.

Do not use `security show-keychain-info` for this test. Its manual page says it
shows keychain settings, not current lock state. The read-only status API is
`SecKeychainGetStatus`, and the SDK defines `kSecUnlockStateStatus` as the bit
indicating an unlocked keychain:

```text
Security.framework/Headers/SecKeychain.h:50
Security.framework/Headers/SecKeychain.h:456
```

The API is deprecated but remains available on macOS 26. Read-only probe, to
run only after the test unlock has completed:

```sh
/usr/bin/swift -suppress-warnings - <<'SWIFT'
import Foundation
import Security

let path = NSString(
    string: "~/Library/Keychains/login.keychain-db"
).expandingTildeInPath
var keychain: SecKeychain?
let opened = SecKeychainOpen(path, &keychain)
var status: SecKeychainStatus = 0
let result = opened == errSecSuccess
    ? SecKeychainGetStatus(keychain, &status)
    : opened

if result != errSecSuccess {
    print("status-error=\(result)")
} else {
    print((status & UInt32(kSecUnlockStateStatus)) != 0
        ? "unlocked"
        : "locked")
}
SWIFT
```

Source:

- [SecKeychainGetStatus](https://developer.apple.com/documentation/security/seckeychaingetstatus%28_%3A_%3A%29)

## What boolean `true` does and does not do

- **VERIFIED (documentation):** Boolean `true` removes authentication from the
  selected token operation.
- **VERIFIED (observation):** LoginWindow still requires credential submission
  before the slot 9A signature callback is reached.
- **VERIFIED (observation):** A successful fresh UWB proof can gate that
  signature and therefore gate the final unlock.
- **LIKELY, verify before using:** When an extension uses boolean `true`
  constraints and does not implement `beginAuthForOperation`, the typed value
  is probably serving only as a LoginWindow and authorization-context gate.
- **I don't know:** a private macOS path might still send a PIV `VERIFY` APDU
  outside the documented extension delegate flow. The fastest check is a
  payload-redacted APDU trace during one correct-PIN unlock.

Do not settle the unknown by entering an incorrect PIV PIN. A failed `VERIFY`
can consume a persistent retry attempt and eventually block the credential.

## Why Auto Unlock does not provide a public workaround

Apple documents a different proximity-unlock protocol:

- all automatic-unlock cases use a mutually authenticated
  Station-to-Station tunnel with long-term keys and per-request ephemeral keys;
- BLE establishes contact, then peer-to-peer Wi-Fi approximates distance;
- the target wraps its passcode-derived key with a random 32-byte unlock secret;
- the initiator returns that secret only when the devices are in range and
  policy checks pass; and
- a Mac must first be unlocked through another method after the Watch is placed
  on wrist and unlocked, because the unlock record must be armed.

Read-only inspection of `/usr/libexec/sharingd` on build 25E253 found
`SFAutoUnlockDevice`, Auto Unlock session classes, AWDL ranging markers, and
the service name `com.apple.private.alloy.continuity.unlock`. Its code
signature contains private capabilities including:

```text
com.apple.private.endpoint-security.submit.authentication.auto-unlock
com.apple.private.ids.messaging
    com.apple.private.alloy.continuity.unlock
com.apple.private.nearbyinteraction.auth-ranging
com.apple.private.nearbyinteraction.privileged
```

**VERIFIED (observation):** Apple's proximity implementation runs in a platform
binary with private authorization, messaging, and ranging capabilities.

**LIKELY, verify before using:** Auto Unlock submits proximity authentication
to the login system by an Apple-private route, not `CryptoTokenKit:login`. No
public entitlement or extension point matching these capabilities was found.

The public Nearby Interaction surface is not a substitute. The macOS 26.4 SDK
marks `NISession` unavailable on macOS:

```text
NearbyInteraction.framework/Headers/NISession.h:30
```

The project's 802.15.4z UWB STS ranging and Apple's BLE plus peer-to-peer Wi-Fi
distance check are not comparable from public evidence: the former exposes an
explicit secure-ranging primitive, while Apple documents an STS tunnel and a
distance policy but not enough PHY detail to rank relay resistance. Do not claim
one is stronger without a relay experiment and implementation detail for both.

Source:

- [Automatically unlock Apple devices](https://support.apple.com/guide/security/automatically-unlock-apple-devices-sec6ab47ebfc/web)

## Smallest safe experiment

Use a disposable test account or VM, retain password fallback, and use only the
correct test PIN. Before locking the screen, start this static-message filter:

```sh
PREDICATE='senderImagePath ENDSWITH "/CryptoTokenKit" AND ('\
'eventMessage == "TKAuthMechanismLogin invoked" OR '\
'eventMessage == "PIN not found in authorization context" OR '\
'eventMessage == "Token login data set to the authorization context")'

sudo /usr/bin/log stream \
  --style compact \
  --level debug \
  --predicate "$PREDICATE"
```

Then:

1. Lock the screen with the token connected.
2. Satisfy the UWB-side prerequisites, but leave the credential field empty for
   several seconds.
3. Submit the correct test PIN once.
4. Stop the log, compare timestamps with payload-redacted token events, and run
   the read-only keychain-status probe from the preceding section.

Interpretation:

- No token or UWB activity before submission indicates a pre-token UI gate.
- `PIN not found in authorization context` identifies the mechanism's
  missing-context path.
- A token-login marker followed by UWB and slot 9A signing shows that submission
  opened the context gate.

Unified logging alone does not prove whether macOS sent PIV `VERIFY`. For a
conclusive trace, record only APDU headers, lengths, status words, and
timestamps. Never log APDU payloads. The distinguishing commands are:

```text
00 20 00 80    PIV VERIFY
00 87 11 9A    P-256 GENERAL AUTHENTICATE with slot 9A
```

A slot 9A marker with no preceding `VERIFY` marker proves the typed value was
only a UI or authorization-context gate for that transaction. A passive USB
protocol analyzer gives the same evidence without changing firmware.

This run settles two independent questions:

- whether LoginWindow or the token path sends PIV `VERIFY`; and
- whether a successful screen unlock also leaves the login keychain unlocked.

## Alternatives

| Approach | Native/supported role | Typed input | Assessment |
|---|---|---:|---|
| PIV + PIN + fresh UWB | Native smart-card login | Yes | Supported baseline. UWB can add a fresh proximity condition to the private-key operation. |
| Custom smart-card token extension | Public CryptoTokenKit extension API | Yes | Enables custom APDUs and UWB-gated signing, but boolean constraints do not remove the LoginWindow credential gate. |
| Persistent CryptoTokenKit token | Post-login token access | Not applicable | Apple says persistent tokens are per-user and unavailable until after login, so they are unsuitable for validating login. |
| Apple Watch Auto Unlock | Apple-owned proximity unlock | No | Native PINless reference design, but the shipping implementation relies on private system capabilities and exposes no public third-party provider API. |
| Platform SSO | Managed identity-provider login | Depends on method | Apple's supported architecture for alternative enterprise login. It requires an IdP and device-management configuration. Its access-key and smart-card methods do not document direct use of a Home/Aliro UWB credential. |
| Composite CCID/HID device | USB transport, not a native passwordless API | No user typing | Can type a test credential after UWB for a disposable demonstration, but stores or handles a reusable secret and can type into the wrong field. Lab-only. |
| Authorization plug-in | Public authorization plug-in API | Can be none | Can make a custom authorization decision, but replacing Apple's screen-unlock right requires privileged installation and authorization-database changes. The shipping rule says not to modify it, and Apple does not document replacement as a stable product contract. |

Apple documents no smart-card preference that removes the LoginWindow PIN
field. Published preferences cover pairing, enforcement, allowed token
providers, certificate trust, logging, and token-removal behavior.

Additional sources:

- [CryptoTokenKit overview and persistent tokens](https://developer.apple.com/documentation/cryptotokenkit)
- [Advanced smart-card options on Mac](https://support.apple.com/en-ca/guide/deployment/dep7b2ede1e3/web)
- [Platform Single Sign-on](https://developer.apple.com/documentation/authenticationservices/platform-single-sign-on-sso)
- [WWDC25: What's new in Apple device management and identity](https://developer.apple.com/videos/play/wwdc2025/258/)
- [Extending authorization services with plug-ins](https://developer.apple.com/documentation/security/extending-authorization-services-with-plug-ins)

## Standards direction

The Connectivity Standards Alliance released Aliro 1.0 on February 26, 2026,
and lists Apple's platforms as certified for Aliro 1.0 on March 4, 2026. The
published material describes mobile credentials for physical access points,
with NFC, BLE, and BLE plus UWB transports. It does not advertise a macOS
desktop-login profile. That absence does not rule out a future profile, but
makes Aliro 1.0 a standards and vendor-engagement path, not a macOS login API.

Sources:

- [Introducing Aliro 1.0](https://csa-iot.org/newsroom/introducing-aliro-1-0-a-unified-standard-to-transform-the-access-control-ecosystem/)
- [Apple's Aliro-certified platforms](https://csa-iot.org/csa_product/apples-platforms/)

## Recommended engineering direction

1. Keep native PIV, the correct PIN, and fresh UWB for the supported secure
   implementation.
2. In one disposable-VM run, capture event-only APDU markers to establish
   whether the custom provider sends `VERIFY`, then read the login keychain's
   lock state.
3. Ask Apple Developer Technical Support whether any supported third-party
   path can supply login credential material in a proximity flow analogous to
   Auto Unlock's wrapped-PDK model.
4. Stop iterating on `isSuitableForLogin`, provider activation, or boolean
   constraints as a way to remove the LoginWindow field.
5. Treat composite HID as a disposable no-typing demonstration, and Platform
   SSO as a separate managed-enterprise architecture.

An authorization plug-in is justified only if the objective changes from
integrating with macOS smart-card login to replacing part of the macOS
authorization stack.

## Remaining unknowns

| Unknown | Authoritative resolution |
|---|---|
| Does a specific custom-provider transaction issue PIV `VERIFY`? | Record payload-redacted APDU headers during one correct-PIN unlock, or use a passive USB trace. |
| Does the `password` write-back feed login-keychain unwrapping? | Read the login keychain state after unlock with `SecKeychainGetStatus`; correlate it with token-login markers. Inspecting private mechanism data flow would settle causality. |
| Would any nonempty value satisfy only the context gate? | First prove that no `VERIFY` occurs. Then use non-retry test firmware or ask Apple Developer Technical Support. Do not test on a retry-limited token. |
| Is the interaction between a boolean `true` login-key constraint and the mandatory text field intentional? | Submit a minimal reproducer and the build-specific `TKAuthMechanismLogin` evidence to Apple Developer Technical Support. |
| Is there a sanctioned third-party equivalent to Auto Unlock's credential-material path? | Ask Apple Developer Technical Support specifically about supplying wrapped login material from a third-party proximity factor. |
| Can a persistent token participate in screen unlock after an existing login? | Ask Apple Developer Technical Support. Public documentation rules persistent tokens out for validating initial login, but does not fully describe this edge case. |
| Is replacing `system.login.screensaver.unlock` supportable for a shipped product? | Ask Apple Developer Technical Support. The plug-in API is public, but modifying this Apple-owned right is not documented as stable. |
| Can Platform SSO consume an Aliro/Home UWB credential? | Obtain an explicit Apple Platform SSO or Wallet Access Program contract. Current public documentation does not describe that integration. |

## Reproduction boundary

The authorization rules, log messages, `sharingd` markers, and code-signature
entitlements above were verified on macOS 26.4.1 build 25E253 with the macOS
26.4 SDK. Recheck after every macOS update. The boolean-constraint conclusion
follows the public SDK contract; `TKAuthMechanismLogin` and Auto Unlock
behavior is build-specific, so treat it as observation of one build.

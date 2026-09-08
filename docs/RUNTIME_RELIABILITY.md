# Runtime reliability changes (0.9.2)

## App lifetime and upgrade procedure

The App is a settings/status client, not a gesture service. The native code in Launcher restores its own complete configuration before handling gestures. No App foreground service, keep-alive, auto-start permission, or boot receiver is introduced.

After upgrading, reload **both SystemUI and Launcher** (rebooting is the simplest option), then open SwipeGate once and confirm the channel reports that the configuration has been applied **and persisted**. The old native/SystemUI code cannot complete the new authenticated protocol. Legacy versions did not persist the haptic setting, so that particular value must be synchronized once from the existing App preferences. After that successful sync, the App may be killed and Launcher/the phone may be restarted without reopening the App.

Clearing Launcher *data*, disabling the module, or a runtime injection failure is different from killing the settings App; this change cannot preserve configuration after its actual storage is deleted or make an uninjected module run.

## Persistent configuration

The preferred location is:

```
/data/user_de/<android-user-id>/com.miui.home/files/swipegate/runtime.conf
```

The credential-encrypted Launcher data directory is a fallback when the preferred directory is unavailable. The file is private (`0600`), versioned, bounded in size, and includes the threshold, log level, haptic toggle, and Break-open toggle as one snapshot. Writes use a private temporary file, file `fsync`, atomic `renameat`, and directory `fsync`; newly created parent directories are synchronized too. Symlinked private directories and non-regular/symlinked config files are not read.

When no new-format file exists, the old Launcher cache files for threshold, log level, and Break-open are migrated. A malformed new-format file selects safe stock defaults; it does not revive stale legacy settings. The haptic toggle defaults off during legacy migration because no trustworthy legacy value exists on disk.

Storage unavailability is retried natively with a five-second backoff. A received live configuration is not later overwritten by a deferred startup load. A write failure leaves the new in-memory setting active but returns `swipegate_config_persisted=false`, keeps the App request pending for its bounded retry window, and does not report durable success. An unchanged setting is not rewritten on each status query. A failed persistence operation is retried even for a duplicate request.

## Transport

App request IDs are public, monotonic boot-time IDs used for correlation **and ordering**, not authentication secrets. Retries retain an immutable configuration and request ID; a new edit receives a new ID. SystemUI and native reject stale requests so delayed broadcasts cannot roll configuration back after a newer edit was acknowledged.

SystemUI generates a separate random 128-bit return capability per request. It is carried only in Xiaomi's existing protected, Launcher-package-targeted `fsgesture` broadcast and echoed in the native reply targeted to SystemUI. It is never forwarded to the App or logged. The native side still verifies the HyperRT-supplied sender package is SystemUI. SystemUI requires the capability and, when Android exposes an actual sender identity, rejects a different real sender. An extras-supplied UID is not an authentication credential. This boundary assumes the existing protected/package-targeted carrier is enforced by the device; privileged/root attackers and code already inside these host processes are out of scope.

The SystemUI request expires after 12 seconds; repeat forwarding is limited to once per 750 ms. A verified durable reply is cached only for that bounded request, allowing recovery when the final SystemUI-to-App reply is lost. Negative persistence replies are not cached as successful completion.

The App polls only while an Activity is started, or while a one-shot request is pending within its six-second deadline. On background timeout it clears the request and removes the scheduled callback. Returning to the App starts a fresh request. There is no permanently running pulse thread. Transport errors remain distinct from native gesture-hook failure.

## Native hook hardening included here

All three control-channel inline hooks now use the existing protected inline-hook installer. The HyperRT GOT guard preserves the actual previous slot entry, verifies all slot chains agree, publishes the upstream entry before replacement, and uses compare-and-exchange rather than blindly overwriting a changed slot.

Pointer writes and original page permissions are rolled back on a partial failure. If rollback cannot be completed, the guard reports that a restart is required and refuses further hook installation in that process. It does **not** claim that a failed rollback restored memory. An upstream entry is retained even after clean rollback because a thread may still be executing the wrapper. Slot-array overflow, inconsistent upstream entries, and a direct self-chain are refused. This does not establish universal compatibility with independently racing hook frameworks.

## Tests and remaining validation

Run:

```
bash tests/run_host_tests.sh
```

The suite covers 12 native persistence scenarios, deterministic Java relay/lifecycle behavior (including first-message loss, final-reply loss, negative persistence ACK, forged/late replies, out-of-order edits, and background expiry), and GOT transaction failure injection. C++ helper tests run with AddressSanitizer and UndefinedBehaviorSanitizer by default. The changed native translation units also get a host syntax check. The Java Android/libxposed stubs are test-only and are not packaged into the APK.

These are **host tests**, not proof of an Android/HyperRT integration. Before merging/releasing, build the APK and verify on a device: fresh install/legacy migration, App force-stop after a durable ACK, Launcher restart, phone restart before/after unlock, permission/write failure, and coexistence with another gesture module in both load orders.

This change intentionally does not alter the primary gesture ABI, resolver candidate policy, main-hook automatic unhook/rehook lifecycle, per-frame haptic retry behavior, or introduce a process-crash safe mode. Those audit findings remain separate work; this branch must not be described as resolving every native lifecycle/ABI risk. The existing log-text-driven backend/state plumbing also remains.

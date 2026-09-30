# Application source dependency

The Luban-Lite application consumes this repository as a pinned Git submodule
at application/rt-thread/pocketjs-smoke/third_party/pocketjs. The source remote is
boa-z/pocketjs; the consuming SDK remains in boa-w/luban-lite-jc-d50t-rev.

The application's Kconfig sources sdk/Kconfig in this directory. Its SConscript
includes sdk/SConscript. Runtime C files and UI core bindings compile from their
source directories into ignored build directories. Rust archives link from
.pocket-build/d13x/rust-target/d13x-e907-ilp32d/release as explicit file inputs,
outside the SDK's whole-archive library wrapper. No global SDK package hook,
kernel patch or copied runtime source is required.

From the SDK root:

    git submodule update --init -- application/rt-thread/pocketjs-smoke/third_party/pocketjs
    python application/rt-thread/pocketjs-smoke/build.py -j8

The SDK gitlink pins the source revision. Updating it is an explicit integration
change; do not update a dependency to a moving branch tip during a firmware build.
The port branch is based on PocketJS d13x commit 678563a. The upstream main pin in
versions.toml is unchanged; pulling a port branch does not rebase the validated
engine onto a newer main release.

Nested checkout discovery takes precedence over the legacy machine-local path
in versions.toml. An explicit POCKETJS_AIC_SDK_ROOT still takes precedence. The
firmware entry rejects another SDK branch or another application's defconfig.
The snapshot generator refuses to overwrite a source submodule. Legacy external
snapshot integrations continue to use apply-sdk.py.

Host checks, from the PocketJS root:

    python hosts/rt-thread/ports/artinchip-d13x/tools/test-application-integration.py
    python hosts/rt-thread/ports/artinchip-d13x/tools/test-alloc-host.py
    cargo +nightly-2026-07-02 test --manifest-path hosts/rt-thread/native/ui-core/Cargo.toml

build-firmware.py runs the strict SDK and archive/link ABI checks. Host tests and
successful linking do not establish board acceptance. Keep board UART captures,
CAN traces and build receipts in ignored validation directories.

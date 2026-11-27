# Initial HarmonyOS Gadget target

This branch adds an ARM64 OHOS cross-build using the Native SDK selected by
`OHOS_NATIVE_HOME`. It reuses the Linux-compatible userspace backends with an
`ohos` subsystem; the device tested runs HarmonyOS / HongMeng Kernel, not Linux.
`Process.platform` currently reports `linux` because of this backend choice.

The source base is Frida 17.27.5 (`bbb7e2292f10eb33be8ed1c9bb1038a7cf1f77be`).
The releng, Gum, and Core submodule changes are kept in separate forks and
commits. Their revisions are pinned by this branch's gitlinks.

## Build

On Linux, inside an environment able to execute the OHOS SDK binaries:

```sh
git submodule update --init releng subprojects/frida-gum subprojects/frida-core
git -C releng submodule update --init meson tomlkit
export OHOS_NATIVE_HOME=/absolute/path/to/sdk/native
unset PKG_CONFIG_PATH
export PKG_CONFIG_LIBDIR=/dev/null
./configure --host=ohos-arm64 --without-prebuilds=sdk:host -- \
  -Dgadget=enabled -Dserver=disabled -Dportal=disabled -Dinject=disabled \
  -Dfrida_python=disabled -Dfrida_node=disabled -Dfrida_tools=disabled \
  -Dfrida_net=disabled -Dfrida_swift=disabled -Dfrida_qml=disabled \
  -Dfrida-gum:v8=disabled -Dfrida-gum:tests=disabled \
  -Dfrida-core:tests=disabled -Dfrida-core:local_backend=disabled \
  -Dfrida-core:fruity_backend=disabled -Dfrida-core:droidy_backend=disabled \
  -Dfrida-core:barebone_backend=disabled -Dfrida-core:compiler_backend=disabled \
  -Dfrida-core:connectivity=disabled -Dfrida-core:compat=disabled
make frida-gadget
```

The result is `build/subprojects/frida-core/lib/gadget/frida-gadget.so`.
Package it as `libfrida-gadget.so` in the App's ARM64 native library directory,
and rebuild and sign the HAP. It cannot be replaced by a downloaded Android or
generic Linux Gadget. The dependency wrap files fix source revisions; a native
Frida toolchain / SDK bundle supplies build-machine tools and dependencies.
The target dependencies are built from source. Clearing the host pkg-config
paths is necessary to avoid linking host libraries into the ARM64 target.

## Embed

Before the first `dlopen`, write the following JSON to an App-private file and
set `FRIDA_GADGET_CONFIG` to that file's absolute path. On OHOS this variable
overrides the usual configuration adjacent to the native library, because the
installed library directory is read-only. Treat configuration failures as a
failed startup; a successful `dlopen` alone does not prove a working Gadget.

```json
{
  "interaction": {
    "type": "listen",
    "address": "127.0.0.1",
    "port": 27042,
    "on_load": "resume"
  },
  "runtime": "qjs",
  "code_signing": "required"
}
```

Load only on an explicit debug entry point, with the App's INTERNET permission
declared. Keep the library loaded for the lifetime of the App. Forward the
loopback listener using HDC; a remote HDC server additionally needs a TCP tunnel
to its forwarded port. This is an ordinary Gadget connection, without LLDB.

The current OHOS payload establishes the real `required` policy before module
discovery and refuses a configuration that would downgrade it. This is the
first-batch implementation boundary, not a decision about future second-batch
capabilities. No executable-memory permission or signed-code patch is used to
bootstrap the session.

## Verified scope and limits

The initial target was built with Command Line Tools 26.0.0.851 / Native Clang
15.0.4 and tested in an ordinary debug-signed App on Mate 70,
OpenHarmony-6.1.1.120 / HongMeng Kernel 1.12.0 / ARM64.

Two clean App starts exercised QuickJS messages in both directions, RPC,
script unload and reload, script exceptions, session detach and reattach, and
App exit. Actual JS checks require `Process.codeSigningPolicy === 'required'`
and `/proc/self/status` reporting `TracerPid: 0` in each script generation.

`NativeCallback`, `CModule`, `Stalker.follow`, and `Stalker.addCallProbe` throw
explicit errors in this mode. `Interceptor.attach` is rejected by Gum's code
signing policy. General Native instrumentation, dynamic module notifications,
exception interception, and ArkTS / Cangjie bridges have not been established
by these session checks. Native and language capabilities require independent
acceptance. An x86_64 simulator is not supported by this target.

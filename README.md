# abr-disk-tools

Command line tools that back up and restore one disk at a time into the `.abr`
backup format -- one tool per virtual disk format, all built from this tree:

| Tool | Disks | Runs on | Details |
| --- | --- | --- | --- |
| `abr-vmdk-tool` | plain files; VMware VMDKs, local or on an ESXi host or a vCenter, through the VDDK or the vSphere SOAP API | Linux | [vmdk-tool/README.md](vmdk-tool/README.md), [manual](doc/abr-vmdk-tool.md) |
| `abr-vhdx-tool` | plain files, disks and volumes; Hyper-V VHD, VHDX and AVHDX, through the Windows virtual disk service | Windows 10 / Server 2016 and later | [vhdx-tool/README.md](vhdx-tool/README.md), [manual](doc/abr-vhdx-tool.md) |

They are the data movers a backup server invokes once per disk. Every tool has
the same commands, the same options apart from its hypervisor's own, and the
same `--json` results, and they read and write the same repositories.

## Layout

```
common/         the CLI every tool shares: options, --json, the copy loops,
                the chain merge, every command
vmdk-tool/, vhdx-tool/
                one folder per format: its options and help, and its disks
                opened for backup and restore
abr-backup-lib/ the .abr format and its file and S3 storage, the backuplib
                library (submodule, AltaStor/abr-backup-lib)
abr-disk-lib/   the disk backends behind IDiskReader/IDiskWriter (submodule,
                AltaStor/abr-disk-lib)
cmake/          target and toolchain selection, the tool mechanism
doc/            the manuals; testing on Hyper-V
test/hyperv/    the end-to-end test of abr-vhdx-tool
```

## Targets

One parameter says what the build is for; which tools come out follows from it.

| `ABR_TARGET` | Host | Compiler | Builds |
| --- | --- | --- | --- |
| `linux` | Linux | the host's | `abr-vmdk-tool` |
| `windows` | Windows | MSVC, or MinGW in an MSYS2 shell | `abr-vhdx-tool.exe` |
| `windows-cross` | Linux (anything but Windows) | MinGW-w64 | `abr-vhdx-tool.exe` |
| `auto` (default) | any | the host's own, or the `CMAKE_TOOLCHAIN_FILE` on the command line | whatever that targets |

A target the host cannot build is refused at configure time, with the one to
use instead. Each preset has a build directory of its own, so a Linux machine
builds both tools side by side:

```bash
cmake --preset linux         && cmake --build --preset linux
cmake --preset windows-cross && cmake --build --preset windows-cross
```

```
build-linux/bin/abr-vmdk-tool   build-linux/bin/abr-vmdk-tool.sh
build-windows-cross/bin/abr-vhdx-tool.exe
```

On Windows:

```bat
cmake --preset windows
cmake --build --preset windows
```

Without presets it is the same parameter:

```bash
cmake -S . -B build -G Ninja -DABR_TARGET=windows-cross
cmake --build build --parallel
```

`ABR_TARGET` picks the toolchain, which CMake reads once per build directory:
use a fresh directory per target, as the presets do.

## Prerequisites

Common to every build:

- CMake 3.20 or newer, and Ninja (or, on Windows, Visual Studio's generator)
- vcpkg, which builds the AWS SDK for C++ (S3) -- and, for Windows, OpenSSL,
  zstd and crc32c -- for the target from [vcpkg.json](vcpkg.json) during the
  configure. It is found at `$VCPKG_ROOT`, else `~/vcpkg`
  (`%USERPROFILE%\vcpkg` on Windows):

  ```bash
  git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
  ~/vcpkg/bootstrap-vcpkg.sh          # bootstrap-vcpkg.bat on Windows
  ```

- the `abr-backup-lib` and `abr-disk-lib` submodules, initialized:

  ```bash
  git submodule update --init --recursive
  ```

### Linux (`linux`): abr-vmdk-tool

- a C++20 compiler
- for the VDDK API, the VMware Virtual Disk Development Kit (VDDK) -- not fetched by anything
  here. Broadcom gates it behind a support account, so download the SDK tarball
  yourself and unpack it into `vddk/` beside the repository checkout, so that
  `../vddk/include/vixDiskLib.h` exists, or name it with `-DVDDK_DIR=`. Only the
  **headers** are needed to build; the library is loaded at run time (see
  [vmdk-tool/README.md](vmdk-tool/README.md#vddk-and-the-library-search-path)).
  Without them the tool is built with the SOAP API alone
- for the SOAP API, abr-disk-lib's `vsphere-soap-lib` submodule, which the
  `--recursive` above fetches, and Boost 1.75 or later (Asio, Beast, JSON) and
  pugixml, found installed (`CMAKE_PREFIX_PATH`) or else fetched by the
  configure
- `libcrc32c`, installed on the system or resolvable through vcpkg
- system development packages: `libssl-dev`, `libcurl4-openssl-dev`,
  `libc-ares-dev`, `zlib1g-dev`, `libzstd-dev`

### Linux to Windows (`windows-cross`): abr-vhdx-tool

- MinGW-w64 targeting the Universal C Runtime (UCRT), with the `posix`
  thread model. On Debian 13 or later (and any distribution that packages it
  under this name):

  ```bash
  sudo apt install g++-mingw-w64-ucrt64
  ```

  That compiler is `x86_64-w64-mingw32ucrt-g++`, the default
  `ABR_MINGW_TRIPLE`; vcpkg is handed the same one for the dependencies, since
  a tool has to be built by the same compiler as the libraries it links. The
  `.exe` then needs `ucrtbase.dll`, which every Windows from 10 and Server 2016
  on has, and not `msvcrt.dll`. The `g++-mingw-w64-x86-64-*` packages are the
  `msvcrt.dll` kind and are refused: the configure asks the compiler which
  runtime it targets before vcpkg builds anything. A UCRT toolchain installed
  elsewhere works too, as long as its directory is on `PATH`; one with the
  classic names, `x86_64-w64-mingw32-g++`, is named with
  `-DABR_MINGW_TRIPLE=x86_64-w64-mingw32`.

- `perl` and `make`, which the OpenSSL port builds with.

No Hyper-V SDK is needed. The virtual disk API (`virtdisk.h`) ships with the
Windows SDK and with MinGW-w64, and `virtdisk.dll` is part of every Windows.

### Windows (`windows`): abr-vhdx-tool

Either of:

- Visual Studio 2022 with the "Desktop development with C++" workload. Its
  developer prompt sets `VCPKG_ROOT` to the vcpkg it bundles, which will do.
- MSYS2's UCRT64 environment with `mingw-w64-ucrt-x86_64-toolchain`, `cmake`
  and `ninja`, plus vcpkg as above. Not MINGW64, whose compiler targets
  `msvcrt.dll`; the configure refuses it.

## Configure

Always configure out of source. Do not run `cmake .` in this directory: it
leaves a `CMakeCache.txt` in the tree that later configures silently reuse,
which is how stale dependency state survives a "clean" rebuild.

What the configure settles by itself, before anything else
([cmake/select-toolchain.cmake](cmake/select-toolchain.cmake)):

| Setting | Default |
| --- | --- |
| Compiler | MinGW-w64 ([cmake/mingw-w64-x86_64.cmake](cmake/mingw-w64-x86_64.cmake)) for `windows-cross`; the host's own otherwise |
| `CMAKE_TOOLCHAIN_FILE` | vcpkg's, from `$VCPKG_ROOT` or `~/vcpkg`, with the compiler toolchain chainloaded behind it |
| `VCPKG_TARGET_TRIPLET` | `x64-mingw-static` for MinGW, `x64-windows-static` for MSVC, vcpkg's default on Linux |
| `CMAKE_BUILD_TYPE` | `Debug` |

Both Windows triplets are static, so nothing but Windows DLLs ends up beside
an `.exe`; MSVC builds link the C runtime statically (`/MT`) to match. The
`x64-mingw-static` triplet is an overlay of vcpkg's own, in
[cmake/triplets](cmake/triplets): building the AWS SDK from Linux needs it told
the target platform, and given a few header names in the case the Windows SDK
spells them ([cmake/mingw-case-shims](cmake/mingw-case-shims/README.md)).

A MinGW build hardens its own code by itself: `-fstack-protector-strong`
everywhere, `-D_FORTIFY_SOURCE=2` outside `Debug` (unless the flags passed in
already name a level), and ASLR, 64-bit ASLR and DEP asked of the linker. A
package build can therefore drop its distribution's hardening flags for
MinGW -- it has to, since `-z relro` is ELF-only -- without losing them.

The first configure for a target builds its dependencies, which takes a while
(about ten minutes, most of it the AWS SDK); vcpkg caches the result, so later
configures take seconds.

The parameters:

| Parameter | Values | Default | |
| --- | --- | --- | --- |
| `ABR_TARGET` | `auto`, `linux`, `windows`, `windows-cross` | `auto` | See [Targets](#targets). |
| `ABR_ENABLED_TOOLS` | formats, separated by commas | empty | The tools to build, named outright: `vmdk`, or `vmdk,vhdx`. A tool not on the list is not built, and the `ABR_BUILD_<FMT>_TOOL` switches are not consulted; naming a tool the target does not support, or one that does not exist, is an error. Empty leaves it to those switches. |
| `ABR_VERSION` | one to four numbers, each 0..65535 | `1.0.0` | The numeric version: `project()`'s, and the `FILEVERSION`/`PRODUCTVERSION` of each `.exe`. A release line and a build number, `26.11.31`. |
| `ABR_VERSION_STRING` | text without quotes or backslashes | empty | The full version, `26.11~pre+31.g6a11c8981539`: what `--version` prints, and the `FileVersion`/`ProductVersion` strings of each `.exe`. Empty is `ABR_VERSION`. |
| `ABR_BUILD_VMDK_TOOL`, `ABR_BUILD_VHDX_TOOL` | `AUTO`, `ON`, `OFF` | `AUTO` | Whether to build that tool. `AUTO` is "when the target is one it supports"; `ON` for any other target is an error. |
| `ABR_MINGW_TRIPLE` | a target triple | `x86_64-w64-mingw32ucrt` | `windows-cross` only: the MinGW-w64 compiler, `<triple>-g++` on `PATH`. It has to target UCRT. |
| `VDDK_DIR` | a path | `../vddk`, beside the repository | The VDDK, for `abr-vmdk-tool`. Only `include/vixDiskLib.h` has to exist to build. |
| `ABR_DISK_WITH_VDDK`, `ABR_DISK_WITH_SOAP` | `AUTO`, `ON`, `OFF` | `AUTO` | The two ways `abr-vmdk-tool` reaches VMware, `--api vddk` and `--api soap`. `AUTO` builds the VDDK where its headers are and SOAP where its submodule is, on Linux; `ON` makes a missing one an error. The tool needs at least one. |
| `VSPHERE_SOAP_LIB_DIR` | a path | `abr-disk-lib/vsphere-soap-lib/` | Another checkout of vsphere-soap-lib, for a change to both at once. |
| `ABR_BACKUPLIB_DIR` | a path | `abr-backup-lib/` | Another checkout of the backup library, for a change to both at once. |
| `ABR_DISKLIB_DIR` | a path | `abr-disk-lib/` | Another checkout of the disk library, for a change to both at once. |
| `ABR_BACKUP_BUILD_UNIT_TESTS` | `ON`, `OFF` | `ON` native, `OFF` cross and MSVC | backuplib's unit tests. |
| `CMAKE_BUILD_TYPE` | | `Debug` | |
| `CMAKE_TOOLCHAIN_FILE` | a path | chosen as above | A compiler toolchain passed this way is still put behind vcpkg's when vcpkg is found. |

## Tests

The unit tests are backuplib's:

```bash
ctest --preset linux
```

They are listed by running the test binary during the build, which a cross
build cannot do, and they have not been built with MSVC yet, so they are off by
default in both of those builds. A native MinGW build (MSYS2) runs them. A
cross build can have them back with `-DABR_BACKUP_BUILD_UNIT_TESTS=ON
-DCMAKE_CROSSCOMPILING_EMULATOR=wine`. On Windows the four tests about POSIX
permission bits are skipped, since Windows has none. `btest`, backuplib's
black-box test tool, is built everywhere.

`abr-vhdx-tool` has an end-to-end test against a real Hyper-V host, run by
hand: [doc/testing.md](doc/testing.md). `abr-vmdk-tool` can be exercised
without a hypervisor on a VMDK made with `qemu-img`, through the launcher with
`ABR_VMDK_TOOL_VDDK_DIR` pointing at the VDDK.

`abr-disk-lib` has a self-test of its own, in its repository.

## Clean

`cmake --build <dir> --target clean` (the built-in target) removes the built
binaries but leaves the build directory and its configured cache in place. To
wipe the build directory entirely, so the next configure starts fresh:

```bash
cmake --build <dir> --target distclean
```

## Install

```bash
cmake --install build-linux --prefix /path/to/prefix
cmake --install build-windows-cross --prefix /path/to/prefix --strip
```

`abr-vmdk-tool` and its launcher `abr-vmdk-tool.sh` land in `<prefix>/bin`;
the launcher looks for the binary beside itself, so keep the two together.
`abr-vhdx-tool.exe` is the one file, also in `<prefix>/bin`; `--strip` takes a
release build from about 17 MB down to 12.

## Adding a format

A tool is a folder named `<format>-tool` and one line. For a format `foo`
whose backend `abr-disk-lib` already has:

1. `foo-tool/tool.cmake` -- read before the libraries are added. Ask for the
   backend (`set(ABR_DISK_WITH_FOO ON CACHE STRING "" FORCE)`) and check
   anything only this format needs, such as an SDK.
2. `foo-tool/CMakeLists.txt` -- read after them:

   ```cmake
   abr_add_tool(abr-foo-tool
       DESCRIPTION "AltaStor FOO backup utility"
       SOURCES disk_tool.cpp
   )
   ```

   which links `common/`, `abr-disk-lib` and `backuplib`, and on Windows adds
   the version resource and the manifest every tool carries.
3. `foo-tool/disk_tool.cpp` -- the format's options and help, a class derived
   from `Tool::DiskTool` that parses them and opens the format's disks for
   `backup` and `restore`, and the `dispatch` table handed to `Tool::run`.
   The two commands themselves are `common/`'s. Start from the nearer of
   `vmdk-tool/` and `vhdx-tool/`.
4. In the top-level `CMakeLists.txt`, the systems it is built for:

   ```cmake
   abr_tool(foo SYSTEMS Linux Windows)
   ```

   That reads `foo-tool/`, creates `ABR_BUILD_FOO_TOOL`, and makes `foo` a name
   `ABR_ENABLED_TOOLS` takes.
5. A manual in `doc/`, a `foo-tool/README.md`, and a row in the table at the top.

The backend itself -- the classes derived from `IDiskReader` and `IDiskWriter`
-- goes in `abr-disk-lib`, not here.

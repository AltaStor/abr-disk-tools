#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Launcher for abr-vmdk-tool.
#
# The binary needs two things from its surroundings that it cannot work out on
# its own: a library search path that reaches the VMware VDDK bundle (the tool
# dlopens libvixDiskLib by name, and the VDDK then dlopens its transport plugins
# out of the same directory) and a writable TMPDIR for those plugins.
# This script supplies both and then gets out of the way.
#
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The env overrides below are authoritative rather than merely preferred:
# an override that does not resolve is a mistake worth reporting, not a reason to
# quietly run some other copy.

# Installed layout puts the binary beside this script; a dev tree leaves it in
# the CMake build directory.
find_tool_binary() {
  local candidate=""
  if [[ -n "${ABR_VMDK_TOOL_BIN:-}" ]]; then
    if [[ ! -x "${ABR_VMDK_TOOL_BIN}" ]]; then
      echo "ABR_VMDK_TOOL_BIN is not an executable: ${ABR_VMDK_TOOL_BIN}" >&2
      return 1
    fi
    printf '%s\n' "${ABR_VMDK_TOOL_BIN}"
    return 0
  fi
  for candidate in "${SCRIPT_DIR}/abr-vmdk-tool" "${SCRIPT_DIR}/build/abr-vmdk-tool"; do
    if [[ -x "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  echo "abr-vmdk-tool binary not found next to ${SCRIPT_DIR}; build it or set ABR_VMDK_TOOL_BIN." >&2
  return 1
}

is_vddk_root() {
  [[ -d "$1" && -f "$1/lib64/libvixDiskLib.so" ]]
}

# ../vddk matches the installed tree; ../../vddk a build directory inside
# the repository checkout, where the binary lands in <build>/bin.
find_vddk_root() {
  local candidate=""
  if [[ -n "${ABR_VMDK_TOOL_VDDK_DIR:-}" ]]; then
    if ! is_vddk_root "${ABR_VMDK_TOOL_VDDK_DIR}"; then
      echo "ABR_VMDK_TOOL_VDDK_DIR has no lib64/libvixDiskLib.so: ${ABR_VMDK_TOOL_VDDK_DIR}" >&2
      return 2
    fi
    (cd "${ABR_VMDK_TOOL_VDDK_DIR}" && pwd)
    return 0
  fi
  for candidate in "${SCRIPT_DIR}/../vddk" "${SCRIPT_DIR}/../../vddk"; do
    if is_vddk_root "${candidate}"; then
      (cd "${candidate}" && pwd)
      return 0
    fi
  done
  return 1
}

choose_tmpdir() {
  local candidate=""
  local probe=""
  for candidate in "${ABR_VMDK_TOOL_TMPDIR:-}" "${TMPDIR:-}" /tmp /var/tmp "${SCRIPT_DIR}/.tmp"; do
    [[ -n "${candidate}" ]] || continue
    mkdir -p "${candidate}" >/dev/null 2>&1 || continue
    probe="$(mktemp "${candidate%/}/abr-vmdk-tool.XXXXXX" 2>/dev/null)" || continue
    rm -f "${probe}"
    printf '%s\n' "${candidate}"
    return 0
  done
  return 1
}

TOOL_BIN="$(find_tool_binary)" || exit 1

TMPDIR="$(choose_tmpdir)" || {
  echo "Unable to find a writable TMPDIR for abr-vmdk-tool." >&2
  exit 1
}
export TMPDIR

# A bundle that is merely absent is not fatal: copy, list, info, verify, remove
# and the meta_* commands never touch the VDDK, and the ones that do report its
# absence better than this script could. A bad override (status 2) is fatal.
VDDK_ROOT="$(find_vddk_root)" || {
  [[ $? -eq 2 ]] && exit 1
  VDDK_ROOT=""
}

# The library search path, in the order the loader will try it:
#
#   1. the bundle's lib64, when there is one. It has to lead: the VDDK's
#      plugins (libdiskLibPlugin.so and the dozen libraries behind it) live
#      nowhere else, and LD_LIBRARY_PATH is the only search path that reaches
#      them -- they carry no RPATH of their own.
#   2. a lib/ directory vendored beside the binary by the package build,
#      if the package build put one there.
#   3. /usr/local/lib, where libcrc32c.so.1 is installed.
#
# The bundle leading the path also puts its older libstdc++/libcrypto/libcurl
# ahead of the system copies, which would break the binary -- except that
# the binary carries a DT_RPATH naming the compiler's runtime directories, and
# RPATH is searched before LD_LIBRARY_PATH for everything the process loads.
LIB_PATH=""
for dir in "${VDDK_ROOT:+${VDDK_ROOT}/lib64}" "${SCRIPT_DIR}/lib" /usr/local/lib; do
  [[ -n "${dir}" && -d "${dir}" ]] || continue
  LIB_PATH="${LIB_PATH:+${LIB_PATH}:}${dir}"
done
# Guarded because an empty LIB_PATH would leave a leading ":" on the variable,
# and an empty entry there means the current directory.
if [[ -n "${LIB_PATH}" ]]; then
  export LD_LIBRARY_PATH="${LIB_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

exec "${TOOL_BIN}" "$@"

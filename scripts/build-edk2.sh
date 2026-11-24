#!/bin/sh
set -eu

usage() {
  echo "Usage: $0 <EDK2_WORKSPACE> <PACKAGES_PATH>" >&2
  exit 2
}

[ "$#" -eq 2 ] || usage
workspace=$1
packages=$2

cd "$workspace"
. edksetup.sh
export PACKAGES_PATH="$packages:$workspace"
build -p CephRbdPkg/CephRbdPkg.dsc -a X64 -b DEBUG -t GCC5

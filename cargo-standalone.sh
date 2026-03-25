#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

vendored_manifest="$repo_root/Cargo.toml"
standalone_manifest="$repo_root/Cargo.standalone.toml"

need_standalone=0
for dep in api cache compiler-llvm types wasix virtual-fs; do
  if [[ ! -f "$repo_root/../$dep/Cargo.toml" ]]; then
    need_standalone=1
    break
  fi
done

use_standalone=0
if [[ "${NAPI_WASMER_FORCE_STANDALONE:-0}" == "1" || $need_standalone -eq 1 ]]; then
  use_standalone=1
fi

if [[ $# -eq 0 ]]; then
  echo "usage: $0 [+toolchain] <cargo-subcommand> [args...]" >&2
  exit 1
fi

cargo_cmd=(cargo)
if [[ "${1:-}" == +* ]]; then
  cargo_cmd+=("$1")
  shift
fi

if [[ $# -eq 0 ]]; then
  echo "usage: $0 [+toolchain] <cargo-subcommand> [args...]" >&2
  exit 1
fi

subcommand="$1"
shift

if [[ "$subcommand" == -* ]]; then
  echo "usage: $0 [+toolchain] <cargo-subcommand> [args...]" >&2
  exit 1
fi

if [[ "${NAPI_WASMER_STANDALONE_ACTIVE:-0}" == "1" ]]; then
  exec "${cargo_cmd[@]}" "$subcommand" --manifest-path "$vendored_manifest" "$@"
fi

if [[ $use_standalone -eq 0 ]]; then
  exec "${cargo_cmd[@]}" "$subcommand" --manifest-path "$vendored_manifest" "$@"
fi

lock_dir="$repo_root/.cargo-standalone.lock"
lock_pid_file="$lock_dir/pid"
while ! mkdir "$lock_dir" 2>/dev/null; do
  if [[ -f "$lock_pid_file" ]]; then
    lock_pid="$(cat "$lock_pid_file")"
    if [[ -n "$lock_pid" ]] && ! kill -0 "$lock_pid" 2>/dev/null; then
      rm -f "$lock_pid_file"
      rmdir "$lock_dir" 2>/dev/null || true
      continue
    fi
  fi
  sleep 0.1
done

printf '%s\n' "$$" > "$lock_pid_file"

manifest_backup="$(mktemp "$repo_root/.cargo.toml.backup.XXXXXX")"
cp "$vendored_manifest" "$manifest_backup"

restore_manifest() {
  cp "$manifest_backup" "$vendored_manifest"
  rm -f "$manifest_backup"
  rm -f "$lock_pid_file"
  rmdir "$lock_dir" 2>/dev/null || true
}

trap restore_manifest EXIT

cp "$standalone_manifest" "$vendored_manifest"

NAPI_WASMER_STANDALONE_ACTIVE=1 \
  "${cargo_cmd[@]}" "$subcommand" --manifest-path "$vendored_manifest" "$@"

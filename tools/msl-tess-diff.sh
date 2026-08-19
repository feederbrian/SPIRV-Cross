#!/usr/bin/env bash
#
# msl-tess-diff.sh — produce paired MSL outputs for a TES SPIR-V blob,
# diffing the vertex form (`[[patch(...)]] vertex`) against the kernel
# form (`tess_evaluation_as_compute`). For SPIRV-W's MSL-diff study of
# the AppGL Phase 3B.2 emission.
#
# Usage:
#   msl-tess-diff.sh <input.spv> [output_dir]
#     <input.spv>   path to the TES SPIR-V dump
#     [output_dir]  destination (default: /tmp/msl-tess-diff)
#
# Outputs (in <output_dir>):
#   <basename>.vertex.msl           — emission with tess_evaluation_as_compute=false
#   <basename>.kernel.msl           — emission with tess_evaluation_as_compute=true
#   <basename>.vertex.validate.log  — stderr from `xcrun -sdk macosx metal -c` on vertex form
#   <basename>.kernel.validate.log  — stderr from `xcrun -sdk macosx metal -c` on kernel form
#   <basename>.diff                 — `diff -u` of the two MSL files
#   <basename>.summary.txt          — entry-point signature + struct names + arg list
#                                      + Metal-validate status, for at-a-glance triage
#
# Env:
#   SPIRV_CROSS         override binary (default: ./spirv-cross relative to this script)
#   PATCH_VERTICES      per-patch input vertex count for kernel form (default: 4)
#   MSL_VERSION         MSL version uint (default: 30000 → 3.0.0)
#   EXTRA_FLAGS         appended to BOTH invocations (e.g. "--msl-multi-patch-workgroup")
#   NO_METAL_VALIDATE   set non-empty to skip xcrun-metal compilation step
#   EMPTY_MSL_THRESHOLD byte-count floor below which a form is flagged
#                       as EMPTY_MSL instead of routed through xcrun
#                       (default: 100). SPIRV-Cross can return success
#                       while emitting empty/boilerplate-only MSL when
#                       entry-point detection or a silent emit_tessellation_*
#                       bail leaves the body unwritten; xcrun metal -c
#                       trivially succeeds on such files, so the
#                       harness flags them before invoking metal.
#
# Exit codes:
#   0  diff produced; both forms emitted and Metal-validated cleanly
#   1  bad usage / file missing
#   2  SPIRV-Cross emission failed for at least one form (output captured)
#   3  emission OK but Metal-validate failed for at least one form,
#      OR a form is flagged EMPTY_MSL (translator silently bailed).
#      Asymmetric results — one form validates, the other doesn't —
#      are themselves diagnostic; see <basename>.{vertex,kernel}.validate.log.
#
set -u

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 1
}

[[ $# -lt 1 || $# -gt 2 ]] && usage
INPUT="$1"
OUTDIR="${2:-/tmp/msl-tess-diff}"

[[ -f "$INPUT" ]] || { echo "msl-tess-diff: input not found: $INPUT" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPIRV_CROSS="${SPIRV_CROSS:-$SCRIPT_DIR/../spirv-cross}"
[[ -x "$SPIRV_CROSS" ]] || {
  echo "msl-tess-diff: spirv-cross not executable at $SPIRV_CROSS" >&2
  echo "  (build with 'cd third_party/SPIRV-Cross && make -j8' or set SPIRV_CROSS=...)" >&2
  exit 1
}

PATCH_VERTICES="${PATCH_VERTICES:-4}"
MSL_VERSION="${MSL_VERSION:-30000}"
EXTRA_FLAGS="${EXTRA_FLAGS:-}"

mkdir -p "$OUTDIR"
BASE="$(basename "${INPUT%.spv}")"
V_OUT="$OUTDIR/$BASE.vertex.msl"
K_OUT="$OUTDIR/$BASE.kernel.msl"
DIFF_OUT="$OUTDIR/$BASE.diff"
SUM_OUT="$OUTDIR/$BASE.summary.txt"

# Common flags — tess emission requires raw_buffer_tese_input + multi_patch_workgroup
# upstream conventions; AppGL also passes vertex_for_tessellation on the paired VS,
# but here we're only running the TES so we omit it.
COMMON=(--msl --msl-version "$MSL_VERSION" --msl-raw-buffer-tese-input --msl-multi-patch-workgroup)

# shellcheck disable=SC2206
EXTRA=($EXTRA_FLAGS)

emit() {
  local form="$1"; shift
  local out="$1"; shift
  local extra_form_flags=("$@")
  set +e
  "$SPIRV_CROSS" "${COMMON[@]}" ${EXTRA[@]+"${EXTRA[@]}"} ${extra_form_flags[@]+"${extra_form_flags[@]}"} "$INPUT" > "$out" 2> "$out.stderr"
  local rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    echo "msl-tess-diff: $form emission failed (rc=$rc) — see $out.stderr" >&2
    return $rc
  fi
  return 0
}

VOK=0; KOK=0
emit "vertex"  "$V_OUT" || VOK=$?
# `--msl-capture-output` MUST pair with `--msl-tess-evaluation-as-compute`.
# AppGL's runtime sets both options together (ShaderTranslator.cpp:466-467);
# without capture-output the kernel form emits invalid MSL — `kernel void`
# with `return main0_out{};` semantics — because the spvOut write-through
# reference and the void return-type are gated on capture_output_to_buffer.
# Don't add capture-output to the vertex form: that path goes to the
# rasterizer (`[[patch(...)]] vertex`) and capture-output would change its
# emission shape away from the form AppGL's runtime actually compares against.
emit "kernel"  "$K_OUT" \
  --msl-tess-evaluation-as-compute \
  --msl-capture-output \
  --msl-tese-input-patch-vertices "$PATCH_VERTICES" || KOK=$?

# Metal-validate each emitted MSL via `xcrun -sdk macosx metal -c`. Per
# SPIRV-W §7 invariant #4, every emission must validate. Asymmetric
# results (one form validates, the other doesn't) are themselves
# diagnostic — they distinguish "SPIRV-Cross emits MSL Metal rejects"
# (codegen bug) from "MSL is valid but PSO creation fails on
# usage-level constraints" (runtime/API bug).
EMPTY_MSL_THRESHOLD="${EMPTY_MSL_THRESHOLD:-100}"

metal_validate() {
  local msl="$1"
  local log="${msl%.msl}.validate.log"
  # Guard: SPIRV-Cross can return success while emitting empty / boilerplate-only
  # MSL when entry-point detection or a silent emit_tessellation_* bail leaves
  # the body unwritten. xcrun metal -c trivially succeeds on such files; flag
  # before invoking it so the diagnostic stays honest.
  local size
  size=$(wc -c < "$msl" 2>/dev/null | tr -d ' ')
  if [[ -z "$size" || "$size" -lt "$EMPTY_MSL_THRESHOLD" ]]; then
    echo "EMPTY_MSL ($size bytes < $EMPTY_MSL_THRESHOLD-byte threshold)" > "$log"
    echo "  — SPIRV-Cross likely silently bailed on this program shape." >> "$log"
    echo "  — Inspect stdout/stderr from emission step before relying on diff." >> "$log"
    return 99
  fi
  if [[ -n "${NO_METAL_VALIDATE:-}" ]]; then
    echo "SKIPPED (NO_METAL_VALIDATE set)" > "$log"
    return 0
  fi
  if ! command -v xcrun >/dev/null 2>&1; then
    echo "SKIPPED (xcrun not on PATH)" > "$log"
    return 0
  fi
  if xcrun -sdk macosx metal -c "$msl" -o /dev/null 2> "$log"; then
    echo "(OK) xcrun -sdk macosx metal -c $msl" >> "$log"
    return 0
  fi
  return 1
}

VVAL=0; KVAL=0
if [[ $VOK -eq 0 ]]; then
  metal_validate "$V_OUT" || VVAL=$?
fi
if [[ $KOK -eq 0 ]]; then
  metal_validate "$K_OUT" || KVAL=$?
fi

# Always produce the diff even if one side failed — a stderr-only kernel form
# vs a clean vertex form is still informative.
diff -u "$V_OUT" "$K_OUT" > "$DIFF_OUT" || true

# Summary: pull the entry-point signature + emitted struct names from each form.
{
  echo "=== msl-tess-diff: $BASE ==="
  echo "input=$INPUT"
  echo "patch_vertices=$PATCH_VERTICES msl_version=$MSL_VERSION extra=${EXTRA_FLAGS:-<none>}"
  echo
  echo "[vertex form: $V_OUT]"
  if [[ $VOK -eq 0 ]]; then
    grep -nE '^(struct|kernel|vertex|fragment) ' "$V_OUT" | head -20
    echo "(structs: $(grep -cE '^struct ' "$V_OUT")  entry: $(grep -cE '^(kernel|vertex|fragment) ' "$V_OUT"))"
    if [[ $VVAL -eq 0 ]]; then
      echo "metal -c: OK"
    elif [[ $VVAL -eq 99 ]]; then
      echo "metal -c: EMPTY_MSL — translator produced no usable MSL; see ${V_OUT%.msl}.validate.log"
    else
      echo "metal -c: FAIL (rc=$VVAL); see ${V_OUT%.msl}.validate.log:"
      head -5 "${V_OUT%.msl}.validate.log" 2>/dev/null | sed 's/^/    /'
    fi
  else
    echo "FAILED rc=$VOK; stderr:"
    head -5 "$V_OUT.stderr"
  fi
  echo
  echo "[kernel form: $K_OUT]"
  if [[ $KOK -eq 0 ]]; then
    grep -nE '^(struct|kernel|vertex|fragment) ' "$K_OUT" | head -20
    echo "(structs: $(grep -cE '^struct ' "$K_OUT")  entry: $(grep -cE '^(kernel|vertex|fragment) ' "$K_OUT"))"
    if [[ $KVAL -eq 0 ]]; then
      echo "metal -c: OK"
    elif [[ $KVAL -eq 99 ]]; then
      echo "metal -c: EMPTY_MSL — translator produced no usable MSL; see ${K_OUT%.msl}.validate.log"
    else
      echo "metal -c: FAIL (rc=$KVAL); see ${K_OUT%.msl}.validate.log:"
      head -5 "${K_OUT%.msl}.validate.log" 2>/dev/null | sed 's/^/    /'
    fi
  else
    echo "FAILED rc=$KOK; stderr:"
    head -5 "$K_OUT.stderr"
  fi
  echo
  echo "[diff: $DIFF_OUT]"
  echo "  $(wc -l < "$DIFF_OUT") diff lines, $(grep -cE '^\+[^+]' "$DIFF_OUT" || true) additions, $(grep -cE '^-[^-]' "$DIFF_OUT" || true) removals"
} > "$SUM_OUT"

cat "$SUM_OUT"
echo
echo "Wrote: $V_OUT"
echo "       $K_OUT"
echo "       $DIFF_OUT"
echo "       $SUM_OUT"

if [[ $VOK -ne 0 || $KOK -ne 0 ]]; then exit 2; fi
# Asymmetric Metal-validate result (one form validates, the other
# doesn't) is the signal AppGL-W's hypothesis-C-class link-failure
# investigation is keyed on; surface it in the exit code without
# hiding the success of emission itself.
if [[ $VVAL -ne 0 || $KVAL -ne 0 ]]; then exit 3; fi
exit 0

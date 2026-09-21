#!/usr/bin/env bash
# The string kernel of the lazy store and its exits (doc/lazy-store/04-
# derivation.md, sections 0.1 and 1.2).  Three invariants the compiler cannot
# hold, held here instead; exits 1 with the offending lines.  Run from any
# directory.  The checks are textual: a comment that names `execvp(` counts.
#
# 1. Outside the evaluator (src/libexpr, src/libflake) and its tests, text is
#    obtained only through the doors (`realise`, `realiseNoCtx`, `emit`,
#    `coerceAndEmit`) or a flushing coercion, never through `EvalState`'s byte
#    accessors `forceString`, `forceStringNoCtx`, `coerceToString`.  The
#    compiler confines a value's raw bytes to `Value`'s friends; it cannot
#    confine these members to a library.
# 2. Every C API entry point that receives an evaluator -- as a parameter, or
#    through a value handle's `->state` -- returns through `nix_c_boundary`,
#    which writes what evaluation left pending before control returns to C,
#    after a returned body and after a thrown one.  Exceptions, each with its
#    reason: the evaluator's lifecycle functions (no evaluator to finish, or
#    one being destroyed); `nix_get_string` and `nix_get_path_string`, which
#    are doors themselves; `nix_init_string`, which only allocates.
# 3. The end of a command that evaluated is a return or an exec; an exec runs
#    no destructor, so the exec helpers `execProgramInStore` and
#    `execvpAfterEvaluation` (src/nix/run.cc) demand `EvalState::finish()`'s
#    token, and in src/nix, src/libcmd, src/libmain, src/libexpr and
#    src/libflake no exec may appear outside those two helpers and the chroot
#    helper they exec.  Exempt: src/nix/man-pages.cc (no evaluator exists in
#    that process yet) and src/libmain/shared.cc (a forked pager child
#    replaces its own image; the parent keeps its evaluator).
set -euo pipefail
cd "$(dirname "$0")/.."
status=0

# The self-test: the three checks must report five planted violations in a
# scratch copy of the tree, or they have not been shown to run (a check that
# never fails on a plant is indistinguishable from one that is inert).  Runs
# after the checks unless CHECK_STRING_KERNEL_NO_SELF_TEST is set, which the
# recursive invocation sets.
self_test() {
    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    cp -R src maintainers "$tmp"/
    # 1. an accessor called outside the kernel
    sed -i.bak 's/evalState->realiseNoCtx(\*attr->value, attr->pos, "").toOwned()/evalState->forceStringNoCtx(*attr->value, attr->pos, "")/' "$tmp/src/nix/bundle.cc"
    # 2. a C entry point outside the boundary; a handle-borne force outside it; a K&R-braced entry point
    awk '{ if ($0 ~ /return nix_c_boundary\(context, state, \[&\] \{ state->state.forceValueDeep\(\*value->value\); \}\);/) { print "    state->state.forceValueDeep(*value->value);"; print "    return NIX_OK;" } else print }' "$tmp/src/libexpr-c/nix_api_expr.cc" > "$tmp/x" && mv "$tmp/x" "$tmp/src/libexpr-c/nix_api_expr.cc"
    awk '{ print; if ($0 ~ /^nix_err nix_init_bool\(nix_c_context \* context, nix_value \* value, bool b\)$/) { getline; print; print "    value->state->forceValue(*value->value, nix::noPos);" } }' "$tmp/src/libexpr-c/nix_api_value.cc" > "$tmp/x" && mv "$tmp/x" "$tmp/src/libexpr-c/nix_api_value.cc"
    printf '\nextern "C" nix_err nix_knr_planted(nix_c_context * context, EvalState * state) {\n    state->state.flushPendingWrites();\n    return NIX_OK;\n}\n' >> "$tmp/src/libexpr-c/nix_api_value.cc"
    # 3. a raw exec at the site the token protects
    sed -i.bak 's/execProgramInStore(store, UseLookupPath::DontUse, app.program.string(), allArgs, std::move(finished));/execvp("x", nullptr);/' "$tmp/src/nix/run.cc"
    local out rc=0
    out=$(CHECK_STRING_KERNEL_NO_SELF_TEST=1 bash "$tmp/maintainers/check-string-kernel.sh" 2>&1) || rc=$?
    local missing=""
    for expect in 'bundle.cc:' 'nix_value_force_deep receives' 'nix_init_bool receives' 'nix_knr_planted receives' 'run.cc:.*execvp\("x", nullptr\)'; do
        grep -qE "$expect" <<< "$out" || missing+=" [$expect]"
    done
    if [[ $rc -ne 1 || -n $missing ]]; then
        echo "ERROR: the self-test did not report every planted violation (exit $rc; missing:$missing):"
        echo "$out"
        return 1
    fi
}

# 1. Excluded on purpose: in nix_api_external.cc the C API's callback table,
#    `NixCExternalValueDesc`, has a field of the accessor's name, and the
#    external-value interface declares a method of it.
found=$(grep -rnE '(^|[^A-Za-z0-9_])(forceString|forceStringNoCtx|coerceToString)[[:space:]]*\(' src \
    --include='*.cc' --include='*.hh' --include='*.h' \
    | grep -vE '^src/(libexpr|libflake)/' \
    | grep -vE '^src/[^/]*-(tests|test-support)/' \
    | grep -vE '^src/libexpr-c/nix_api_external\.cc:[0-9]+:.*(desc\.coerceToString\(|virtual std::string coerceToString\(|ExternalValueBase::coerceToString\()' || true)
if [[ -n "$found" ]]; then
    echo "ERROR: the evaluator's byte accessors are called outside the string kernel;"
    echo "       outside src/libexpr and src/libflake use EvalState::realise, emit,"
    echo "       coerceAndEmit or a flushing coercion (doc/lazy-store/04-derivation.md 0.1):"
    echo "$found"
    status=1
fi

# 2. A function definition is a header at column 0 (its parameter lines may
#    continue indented), then `{` at column 0, then `}` at column 0.
boundary=$(awk '
    BEGIN {
        exempt["nix_eval_state_build"] = exempt["nix_state_create"] = exempt["nix_state_free"] = 1
        exempt["nix_get_string"] = exempt["nix_get_path_string"] = exempt["nix_init_string"] = 1
        exempt["nix_c_boundary"] = 1
    }
    function report() {
        if ((needs || touches) && !seen && !(name in exempt))
            print FILENAME ":" line ": " name " receives an evaluator but does not return through nix_c_boundary"
    }
    function begin_body() {
        inheader = 0; infn = 1; seen = 0; touches = 0; line = hline
        name = match(header, /nix_[a-z0-9_]+\(/) ? substr(header, RSTART, RLENGTH - 1) : "?"
        needs = (header ~ /EvalState[ \t]*\*/)
    }
    { code = $0; sub(/\/\/.*/, "", code) }
    /^[A-Za-z_]/ && !/^(static|template|inline|struct|class|namespace|using|typedef)/ && !/^extern "C" \{/ && !infn {
        header = $0; hline = FNR; inheader = 1
        if (code ~ /\)[ \t]*\{[ \t]*$/) begin_body()
        next
    }
    inheader && /^[ \t]/ { header = header " " $0; if (code ~ /\)[ \t]*\{[ \t]*$/) begin_body(); next }
    inheader && /^\{/ { begin_body(); next }
    inheader { inheader = 0 }
    infn && code ~ /nix_c_boundary\(/ { seen = 1 }
    infn && code ~ /->state->/ { touches = 1 }
    infn && /^\}/ { report(); infn = 0 }
' src/libexpr-c/*.cc src/libexpr-c/*.h src/libflake-c/*.cc src/libflake-c/*.h)
if [[ -n "$boundary" ]]; then
    echo "ERROR: C API entry points outside the boundary (doc/lazy-store/04-derivation.md 1.2, the C API row):"
    echo "$boundary"
    status=1
fi

# 3. In run.cc, an exec is permitted only inside the two helpers and the chroot
#    helper; elsewhere in the scanned directories, nowhere.
execs=$(grep -rnE '\b(exec(l|lp|le|v|vp|vpe|ve)|fexecve|execveat|posix_spawnp?|popen)\(' src/nix src/libcmd src/libmain src/libexpr src/libflake \
    --include='*.cc' --include='*.hh' \
    | grep -vE '^src/nix/run\.cc:' \
    | grep -vE '^src/nix/man-pages\.cc:' \
    | grep -vE '^src/libmain/shared\.cc:' || true)
execs_run=$(awk '
    BEGIN { permitted["execProgramInStore"] = permitted["execvpAfterEvaluation"] = permitted["chrootHelper"] = 1 }
    /^[A-Za-z_]/ && !/^(template|inline|struct|class|extern|namespace|using|typedef)/ && !infn {
        header = $0; inheader = 1; next
    }
    inheader && /^[ \t]/ { header = header " " $0; next }
    inheader && /^\{/ {
        inheader = 0; infn = 1
        name = match(header, /[A-Za-z_][A-Za-z0-9_]*\(/) ? substr(header, RSTART, RLENGTH - 1) : "?"
        next
    }
    inheader { inheader = 0 }
    /(^|[^A-Za-z0-9_])(exec(l|lp|le|v|vp|vpe|ve)|fexecve|execveat|posix_spawnp?|popen)\(/ && !(infn && (name in permitted)) { print FILENAME ":" FNR ": " $0 }
    infn && /^\}/ { infn = 0 }
' src/nix/run.cc)
if [[ -n "$execs$execs_run" ]]; then
    echo "ERROR: an exec where an evaluator may be alive; use execProgramInStore or execvpAfterEvaluation"
    echo "       with EvalState::finish() (doc/lazy-store/04-derivation.md 1.2, teardown):"
    echo "$execs$execs_run"
    status=1
fi

if [[ $status -eq 0 && -z ${CHECK_STRING_KERNEL_NO_SELF_TEST:-} ]]; then
    self_test || status=1
fi

exit $status

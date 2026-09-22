#!/usr/bin/env bash

source common.sh

TODO_NixOS

# nix shell is an alias for nix env shell. We'll use the shorter form in the rest of the test.
nix env shell -f shell-hello.nix hello -c hello | grep 'Hello World'

nix shell -f shell-hello.nix hello -c hello | grep 'Hello World'
nix shell -f shell-hello.nix hello -c hello NixOS | grep 'Hello NixOS'

# Test output selection.
nix shell -f shell-hello.nix hello^dev -c hello2 | grep 'Hello2'
nix shell -f shell-hello.nix 'hello^*' -c hello2 | grep 'Hello2'

# Test output paths that are a symlink.
nix shell -f shell-hello.nix hello-symlink -c hello | grep 'Hello World'

# Test that symlinks outside of the store don't work.
expect 1 nix shell -f shell-hello.nix forbidden-symlink -c hello 2>&1 | grepQuiet "is not in the Nix store"

# Test that we're not setting any more environment variables than necessary.
# For instance, we might set an environment variable temporarily to affect some
# initialization or whatnot, but this must not leak into the environment of the
# command being run.
env > "$TEST_ROOT/expected-env"
nix shell -f shell-hello.nix hello -c env > "$TEST_ROOT/actual-env"
# Remove/reset variables we expect to be different.
# - PATH is modified by nix shell
# - we unset TMPDIR on macOS if it contains /var/folders
# - _ is set by bash and is expectedf to differ because it contains the original command
# - __CF_USER_TEXT_ENCODING is set by macOS and is beyond our control
# - __LLVM_PROFILE_RT_INIT_ONCE - implementation detail of LLVM source code coverage collection
sed -i \
  -e 's/PATH=.*/PATH=.../' \
  -e 's/_=.*/_=.../' \
  -e '/^TMPDIR=\/var\/folders\/.*/d' \
  -e '/^__CF_USER_TEXT_ENCODING=.*$/d' \
  -e '/^__LLVM_PROFILE_RT_INIT_ONCE=.*$/d' \
  "$TEST_ROOT/expected-env" "$TEST_ROOT/actual-env"
sort "$TEST_ROOT/expected-env" > "$TEST_ROOT/expected-env.sorted"
sort "$TEST_ROOT/actual-env" > "$TEST_ROOT/actual-env.sorted"
diff "$TEST_ROOT/expected-env.sorted" "$TEST_ROOT/actual-env.sorted"

if isDaemonNewer "2.20.0pre20231220"; then
    # Test that command line attribute ordering is reflected in the PATH
    # https://github.com/NixOS/nix/issues/7905
    nix shell -f shell-hello.nix hello salve-mundi -c hello | grep 'Hello World'
    nix shell -f shell-hello.nix salve-mundi hello -c hello | grep 'Salve Mundi'
fi

requireSandboxSupport
requiresUnprivilegedUserNamespaces

chmod -R u+w "$TEST_ROOT/store0" || true
rm -rf "$TEST_ROOT/store0"

clearStore

path=$(nix eval --raw -f shell-hello.nix hello)

# Note: we need the sandbox paths to ensure that the shell is
# visible in the sandbox.
nix shell --sandbox-build-dir /build-tmp \
    --sandbox-paths '/nix? /bin? /lib? /lib64? /usr?' \
    --store "$TEST_ROOT/store0" -f shell-hello.nix hello -c hello | grep 'Hello World'

path2=$(nix shell --sandbox-paths '/nix? /bin? /lib? /lib64? /usr?' --store "$TEST_ROOT/store0" -f shell-hello.nix hello -c "$SHELL" -c 'type -p hello')

[[ "$path/bin/hello" = "$path2" ]]

[[ -e $TEST_ROOT/store0/nix/store/$(basename "$path")/bin/hello ]]

# Shell metadata comes from the execution store, even when evaluation sees
# an unrelated ambient occupant or uses a different store altogether.
shellStore="$TEST_ROOT/shell-store?store=$NIX_STORE_DIR"
shellEvalStore="$TEST_ROOT/shell-eval-store?store=$NIX_STORE_DIR"
shellSource="$TEST_ROOT/shell-source"
mkdir -p "$shellSource/bin-target/bin" "$shellSource/propagated/bin" "$shellSource/package/nix-support"
binTarget=$(nix store add-path --store "$shellStore" "$shellSource/bin-target")
propagated=$(nix store add-path --store "$shellStore" "$shellSource/propagated")
printf '%s\n' "$propagated" > "$shellSource/propagation"
propagation=$(nix store add-file --store "$shellStore" "$shellSource/propagation")
ln -s "$binTarget/bin" "$shellSource/package/bin"
ln -s "$propagation" "$shellSource/package/nix-support/propagated-user-env-packages"
package=$(nix store add-path --store "$shellStore" "$shellSource/package")
requireGit
initGitRepo "$shellSource/package"

for evalMode in execution separate; do
    evalFlags=()
    if [[ $evalMode == separate ]]; then
        evalFlags=(--eval-store "$shellEvalStore")
    fi
    for purity in impure pure; do
        purityFlags=(--option pure-eval true)
        if [[ $purity == impure ]]; then
            purityFlags=(--impure)
        fi
        for shadow in absent bin propagation; do
            if [[ $shadow != absent ]]; then
                mkdir -p "$package/nix-support"
                if [[ $shadow == bin ]]; then
                    ln -s "$TEST_ROOT/outside-the-store" "$package/bin"
                else
                    ln -s "$binTarget/bin" "$package/bin"
                    echo not-a-store-path > "$package/nix-support/propagated-user-env-packages"
                fi
            fi
            shellPath=$(nix shell --store "$shellStore" "${evalFlags[@]}" "${purityFlags[@]}" "$package" \
                -c "$SHELL" -c 'printf "%s" "$PATH"')
            [[ $shellPath == "$binTarget/bin:$propagated/bin:"* ]]
            rm -rf "$package"
        done
    done

    # A fetched outPath is initially only mounted in the evaluator. The
    # installable must realise it before the shell reads execution-store bytes.
    # A separate eval store transfers opaque inputs through its substituter.
    printf '%s\n' "$evalMode" > "$shellSource/package/marker"
    git -C "$shellSource/package" add -A
    git -C "$shellSource/package" commit -q -m "$evalMode"
    fetch="(builtins.fetchTree { type = \"git\"; url = \"file://$shellSource/package\"; })"
    # Printing only the basename drops the store-path context, so this fetch
    # leaves the Git input mounted but unmaterialised in both stores.
    sourceName=$(nix eval --store "$shellStore" "${evalFlags[@]}" --impure --raw --expr \
        "builtins.unsafeDiscardStringContext (baseNameOf $fetch.outPath)")
    [[ $sourceName == *-source ]]
    [[ ! -e $TEST_ROOT/shell-store/nix/store/$sourceName ]]
    [[ ! -e $TEST_ROOT/shell-eval-store/nix/store/$sourceName ]]
    shellPath=$(nix shell --store "$shellStore" "${evalFlags[@]}" \
        --substituters "$shellEvalStore" --impure --expr \
        "$fetch.outPath" \
        -c "$SHELL" -c 'printf "%s" "$PATH"')
    [[ $shellPath == "$binTarget/bin:$propagated/bin:"* ]]
    nix path-info --store "$shellStore" "$NIX_STORE_DIR/$sourceName" > /dev/null
    [[ $(cat "$TEST_ROOT/shell-store/nix/store/$sourceName/marker") == "$evalMode" ]]
done

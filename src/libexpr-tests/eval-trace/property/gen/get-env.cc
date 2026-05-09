#include "../expr-gen.hh"

#include <unistd.h>  // getpid()

namespace nix::eval_trace::test::proptest {

// ── GetEnvGen ────────────────────────────────────────────────────────

/// Counter for unique env var names within this process.
static std::atomic<int> envVarCounter{0};

rc::Gen<TestExpr> makeGetEnvGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),  // initial value
        [](std::string value) {
            // Build a unique env var name: NIX_PROP_<pid>_<counter>
            auto varName = "NIX_PROP_"
                + std::to_string(getpid()) + "_"
                + std::to_string(envVarCounter++);

            // Use ScopedEnvVar (RAII) stored in the DepSlot's envGuard field.
            // This ensures the env var is restored (or unset) on DepSlot
            // destruction — even if RC_ASSERT unwinds the stack mid-property.
            // Raw setenv() without cleanup leaks the variable into process
            // state across shrink candidates and between iterations.
            auto guard = std::make_shared<ScopedEnvVar>(varName, value);

            DepSlot slot;
            slot.kind = DepSlot::Kind::EnvVar;
            slot.envVarName = varName;
            slot.envGuard = std::move(guard);
            slot.currentValue = value;
            slot.setOriginal(value);

            return TestExpr{
                .nixCode = R"(builtins.getEnv ")" + varName + R"(")",
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest

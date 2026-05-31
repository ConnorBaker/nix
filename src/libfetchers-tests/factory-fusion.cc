/* Factory-fusion UNIT tests (§11 MorphismFusionAtFactory +
   IdentityShortCircuit). The factory helpers makeTranslate / makeRestrict /
   makeMask / makeSoundnessGuard / makeLayer must:
   - short-circuit identity elements (Translate(/), Restrict(∅), Mask(∅),
     SoundnessGuard(∅), Layer([]), Layer([a]));
   - fuse adjacent operators of the same type at construction so the
     resulting operator-stack depth never exceeds the recipe's intrinsic
     complexity.

   We test these via dynamic_cast on the returned ref to confirm the
   wrapper depth and shape. */

#include <exception> // IWYU pragma: keep
#include <gtest/gtest.h>

#include "nix/fetchers/mask-source-accessor.hh"
#include "nix/fetchers/restrict-source-accessor.hh"
#include "nix/fetchers/soundness-guard-source-accessor.hh"
#include "nix/fetchers/strip-prefix-source-accessor.hh"
#include "nix/fetchers/translate-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"

#include "operator-test-helpers.hh"

namespace nix {

namespace {

ref<MemorySourceAccessor> emptyMem()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    return m;
}

std::shared_ptr<const std::set<CanonPath>> paths(std::initializer_list<CanonPath> ps)
{
    return std::make_shared<const std::set<CanonPath>>(std::set<CanonPath>(ps.begin(), ps.end()));
}

std::shared_ptr<const std::set<CanonPath>> emptyPaths()
{
    return std::make_shared<const std::set<CanonPath>>();
}

} // namespace

/* ---------- Translate ---------- */

TEST(FactoryFusion, TranslateIdentityShortCircuit)
{
    /* L1b/L1c: Translate(/) ≡ Identity → returns base unchanged. */
    auto base = emptyMem();
    auto result = makeTranslate(base, CanonPath::root, testHelpers::makeOpErr());
    /* Same shared_ptr — base passed through unchanged. */
    EXPECT_EQ(result.get(), base.get());
}

TEST(FactoryFusion, TranslateMorphismFusion)
{
    /* L1d: Translate(p) ∘ Translate(q) → Translate(p/q). Single
       wrapper produced regardless of nesting depth. */
    auto base = emptyMem();
    auto inner = makeTranslate(base, CanonPath("/a"), testHelpers::makeOpErr());
    auto fused = makeTranslate(inner, CanonPath("/b"), testHelpers::makeOpErr());

    /* Verify: fused is exactly one TranslateSourceAccessor wrapping base
       directly, not a Translate-wrapping-Translate. */
    auto fusedTranslate = fused.dynamic_pointer_cast<TranslateSourceAccessor>();
    ASSERT_NE(fusedTranslate, nullptr);
    /* The next pointer should be base itself (not another Translate). */
    EXPECT_EQ(fusedTranslate->next.get(), base.get());
    /* Combined prefix is /a/b. */
    EXPECT_EQ(fusedTranslate->prefix, CanonPath("/a/b"));
}

/* ---------- Restrict ---------- */

TEST(FactoryFusion, RestrictIdentityShortCircuit)
{
    /* L2b: Restrict(∅) ≡ Identity (universe sentinel). */
    auto base = emptyMem();
    auto result = makeRestrict(base, emptyPaths(), testHelpers::makeOpErr());
    EXPECT_EQ(result.get(), base.get());
}

TEST(FactoryFusion, RestrictMorphismFusion)
{
    /* L2a: Restrict(S) ∘ Restrict(T) → Restrict(S ∩ T). */
    auto base = emptyMem();
    auto inner = makeRestrict(base, paths({CanonPath("/a"), CanonPath("/b")}), testHelpers::makeOpErr());
    auto fused = makeRestrict(inner, paths({CanonPath("/b"), CanonPath("/c")}), testHelpers::makeOpErr());

    auto fusedRestrict = fused.dynamic_pointer_cast<RestrictSourceAccessor>();
    ASSERT_NE(fusedRestrict, nullptr);
    /* Intersection: {/b}. */
    ASSERT_EQ(fusedRestrict->paths->size(), 1u);
    EXPECT_TRUE(fusedRestrict->paths->contains(CanonPath("/b")));
    /* Wrapping base directly, not wrapping the inner Restrict. */
    EXPECT_EQ(fusedRestrict->next.get(), base.get());
}

TEST(FactoryFusion, RestrictFusionEmptyIntersectionShortCircuits)
{
    /* If S ∩ T is empty, the fused Restrict(∅) ≡ Identity per L2b
       sentinel — the helper returns inner->next (i.e. base). */
    auto base = emptyMem();
    auto inner = makeRestrict(base, paths({CanonPath("/a")}), testHelpers::makeOpErr());
    auto fused = makeRestrict(inner, paths({CanonPath("/b")}), testHelpers::makeOpErr());
    EXPECT_EQ(fused.get(), base.get());
}

/* ---------- Mask ---------- */

TEST(FactoryFusion, MaskIdentityShortCircuit)
{
    /* L3b: Mask(∅) ≡ Identity. */
    auto base = emptyMem();
    auto result = makeMask(base, emptyPaths(), testHelpers::makeOpErr());
    EXPECT_EQ(result.get(), base.get());
}

TEST(FactoryFusion, MaskMorphismFusion)
{
    /* L3a: Mask(W1) ∘ Mask(W2) → Mask(W1 ∪ W2). */
    auto base = emptyMem();
    auto inner = makeMask(base, paths({CanonPath("/a")}), testHelpers::makeOpErr());
    auto fused = makeMask(inner, paths({CanonPath("/b")}), testHelpers::makeOpErr());

    auto fusedMask = fused.dynamic_pointer_cast<MaskSourceAccessor>();
    ASSERT_NE(fusedMask, nullptr);
    EXPECT_EQ(fusedMask->paths->size(), 2u);
    EXPECT_TRUE(fusedMask->paths->contains(CanonPath("/a")));
    EXPECT_TRUE(fusedMask->paths->contains(CanonPath("/b")));
    EXPECT_EQ(fusedMask->next.get(), base.get());
}

/* ---------- SoundnessGuard ---------- */

TEST(FactoryFusion, SoundnessGuardIdentityShortCircuit)
{
    auto base = emptyMem();
    auto result = makeSoundnessGuard(base, emptyPaths(), testHelpers::makeOpErr());
    EXPECT_EQ(result.get(), base.get());
}

TEST(FactoryFusion, SoundnessGuardMorphismFusion)
{
    /* L7f: SoundnessGuard ∘ SoundnessGuard → SoundnessGuard(union). */
    auto base = emptyMem();
    auto inner = makeSoundnessGuard(base, paths({CanonPath("/a")}), testHelpers::makeOpErr());
    auto fused = makeSoundnessGuard(inner, paths({CanonPath("/b")}), testHelpers::makeOpErr());

    auto fusedSG = fused.dynamic_pointer_cast<SoundnessGuardSourceAccessor>();
    ASSERT_NE(fusedSG, nullptr);
    EXPECT_EQ(fusedSG->paths->size(), 2u);
    EXPECT_EQ(fusedSG->next.get(), base.get());
}

/* ---------- StripPrefix ---------- */

TEST(FactoryFusion, StripPrefixIdentityShortCircuit)
{
    /* SP1: StripPrefix(/) ≡ Identity → returns base unchanged. */
    auto base = emptyMem();
    auto result = makeStripPrefix(base, CanonPath::root, testHelpers::makeOpErr());
    EXPECT_EQ(result.get(), base.get());
}

TEST(FactoryFusion, StripPrefixMorphismFusion)
{
    /* SP2: StripPrefix(p) ∘ StripPrefix(q) → StripPrefix(p/q). Single
       wrapper, regardless of nesting depth.

       CRUCIAL — the fused field order is the OPPOSITE of Translate's.
       For Translate, makeTranslate(inner=/a, /b) yields prefix /a/b
       (inner->prefix / prefix). For StripPrefix, the OUTER strip applies
       first, so makeStripPrefix(inner=/a, /b) yields prefix /b/a
       (prefix / inner->prefix). This structural test is the guard
       against an undetectable-by-types field-order swap (cf. the
       recipe::Overlay swap lesson). */
    auto base = emptyMem();
    auto inner = makeStripPrefix(base, CanonPath("/a"), testHelpers::makeOpErr());
    auto fused = makeStripPrefix(inner, CanonPath("/b"), testHelpers::makeOpErr());

    auto fusedStrip = fused.dynamic_pointer_cast<StripPrefixSourceAccessor>();
    ASSERT_NE(fusedStrip, nullptr);
    /* Depth 1: next is base itself, not another StripPrefix. */
    EXPECT_EQ(fusedStrip->next.get(), base.get());
    /* Outer(/b) / inner(/a) = /b/a — NOT /a/b. */
    EXPECT_EQ(fusedStrip->prefix, CanonPath("/b/a"));
}

/* ---------- Layer ---------- */

TEST(FactoryFusion, LayerEmptyIsEmpty)
{
    /* L6e: Layer([]) ≡ empty accessor. */
    auto layer = makeLayer({});
    /* Read of any path throws (empty accessor). */
    EXPECT_THROW(layer->readFile(CanonPath("/anything")), std::exception);
}

TEST(FactoryFusion, LayerSingletonShortCircuit)
{
    /* L6d: Layer([a]) ≡ a. */
    auto base = emptyMem();
    auto layer = makeLayer({base});
    EXPECT_EQ(layer.get(), base.get());
}

TEST(FactoryFusion, LayerAssociativityFlattensObservably)
{
    /* L6a: Layer flattens nested Layers at construction. The
       observable form: building `Layer([a, Layer([b, c])])` and
       `Layer([a, b, c])` produces the same readDirectory output and
       the same first-wins behaviour. */
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("from_a.txt"), "A");
    auto b = make_ref<MemorySourceAccessor>();
    b->addFile(CanonPath("from_b.txt"), "B");
    auto c = make_ref<MemorySourceAccessor>();
    c->addFile(CanonPath("from_c.txt"), "C");

    auto nested = makeLayer({a, makeLayer({b, c})});
    auto flat = makeLayer({a, b, c});

    auto nestedEntries = nested->readDirectory(CanonPath::root);
    auto flatEntries = flat->readDirectory(CanonPath::root);
    EXPECT_EQ(nestedEntries.size(), flatEntries.size());
    EXPECT_EQ(nested->readFile(CanonPath("/from_c.txt")), flat->readFile(CanonPath("/from_c.txt")));
}

} // namespace nix

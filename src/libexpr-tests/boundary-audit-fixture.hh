#pragma once
///@file
///
/// Boundary audit for Item 1 (`addPath` migration).
///
/// `addPath` cannot be activated to emit `SourceVirtual` placeholders
/// until every eval boundary that observes a string-with-context
/// materialises the placeholder before serialising. The boundaries
/// covered:
///
///   1. `nix eval --raw` (`coerceToString` → `writeFull`)
///   2. `nix eval --json` (`printValueAsJSON`)
///   3. `nix-instantiate --eval --xml` (`printValueAsXML`)
///   4. `nix-instantiate --eval` plain-mode (`printAmbiguous`)
///   5. `prim_import` (`realisePath` → `realiseContext`)
///   6. eval-cache write path (`AttrDb::setString` via
///      `AttrCursor::forceValue`)
///
/// Boundaries 1–4 share the same shape (serialiser dumps
/// unrewritten body bytes); the proposal lumps them as "the four
/// CLI output boundaries". Boundary 5 is the only one already
/// covered by `realiseContext`. Boundary 6 is the cross-process
/// persistence path.
///
/// This fixture mints a real `SourcePlaceholder` via
/// `MaterialisationScheduler::registerView`, builds a string `Value`
/// carrying the placeholder render plus a `SourceVirtual` context
/// elem, and exposes that value to per-boundary test files. Each
/// boundary test drives the boundary-specific serialiser and asserts
/// the placeholder text does NOT appear in the output (interpretation
/// (a) of "covered": output contains the resolved storePath, no
/// `~<hash>` visible).

#include <gtest/gtest.h>

#include <iostream>
#include <string>
#include <string_view>

#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/materialisation-scheduler.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/content-address.hh"
#include "nix/store/source-content-id.hh"
#include "nix/store/source-placeholder.hh"
#include "nix/store/store-open.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-view.hh"

namespace nix {

ref<SourceViewAccessor> sourceViewRoot(ref<SourceAccessor> base);

/**
 * Scaffolding shared by every boundary-audit test file. Subclasses
 * are expected to call `mintPlaceholder()` and then drive their
 * specific serialiser against the resulting Value.
 *
 * Uses a per-process temp `local?root=...` store rather than
 * `dummy://` because the audit drives placeholder materialisation,
 * which calls `addToStoreFromDump` on the store — `dummy://` does
 * not support that operation.
 */
class BoundaryAuditTest : public LibExprTest
{
public:
    BoundaryAuditTest()
        : LibExprTest(openStore("local?root=" + (createTempDir() / "store").string()), [](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            return settings;
        })
    {
    }

protected:
    /** Build a tiny in-memory source accessor with one file. */
    ref<MemorySourceAccessor> makeBackingAccessor()
    {
        auto mem = make_ref<MemorySourceAccessor>();
        mem->addFile(CanonPath{"file.txt"}, "boundary-audit fixture content");
        return mem;
    }

    /**
     * Mint a `SourcePlaceholder` by registering a `SourceViewAccessor`
     * over a known in-memory source. Returns the placeholder; the
     * caller can `placeholder.render()` to get the `~<base32>` form
     * to look for in serialised output.
     */
    SourcePlaceholder mintPlaceholder()
    {
        auto backing = makeBackingAccessor();
        auto view = sourceViewRoot(backing);

        /* The contentId is content-determined: same fingerprint
           inputs produce the same placeholder. We hash the backing
           accessor's content directly to get a deterministic
           shapeHash; for an audit any value works. */
        auto shapeHash = hashString(HashAlgorithm::SHA256, "boundary-audit-shape");
        auto contentId = SourceContentId::compute(
            "audit-fixture", shapeHash, ContentAddressMethod::Raw::NixArchive, StoreReferences{});

        return state.materialisationScheduler->registerView(
            MaterialisationScheduler::Registration{
                .contentId = contentId,
                .name = "audit-source",
                .method = ContentAddressMethod::Raw::NixArchive,
                .refs = StoreReferences{},
                .view = view,
            });
    }

    /**
     * Build a string `Value` whose body is the placeholder's render
     * text and whose context contains a `SourceVirtual` element
     * referring to that same placeholder. This mirrors what an
     * `addPath` migrated to emit placeholders would produce.
     */
    void mkPlaceholderString(Value & v, const SourcePlaceholder & placeholder, const std::string & name)
    {
        NixStringContext context;
        context.insert(
            NixStringContextElem{NixStringContextElem::SourceVirtual{
                .placeholder = placeholder,
                .name = name,
            }});
        v.mkString(placeholder.render(), context, state.mem);
    }

    /**
     * Predicate: does `s` contain any unresolved-placeholder text?
     *
     * Two leak shapes are possible:
     *
     *   - the body form `/<base32>` returned by
     *     `SourcePlaceholder::render()` — leaks if the boundary
     *     serialises the value body without first rewriting via
     *     `realiseContext`.
     *   - the context wire form `~<base32>:<name>` returned by
     *     `NixStringContextElem::SourceVirtual::to_string()` — leaks
     *     if the boundary serialises the context inline (e.g.
     *     `AttrDb::setString` joins context elements as
     *     `<elem.view()>`).
     *
     * Either form appearing in `s` counts as a leak.
     */
    struct LeakDetail
    {
        bool bodyLeak;
        bool contextLeak;
        std::string firstLeakedText;

        bool any() const
        {
            return bodyLeak || contextLeak;
        }
    };

    /**
     * Brittleness note (PROPOSAL.md §6.4.3 follow-up):
     * `detectLeak` does literal substring search for two forms only:
     *   - body form `/<base32>` from `placeholder.render()`
     *   - context wire form `~<base32>:<name>` from
     *     `NixStringContextElem::SourceVirtual::to_string()`.
     * It does NOT catch transformed placeholder text. If a future
     * boundary URL-escapes (`%2F<base32>`), JSON-string-escapes
     * (`/<base32>` → `\/<base32>` in some emitters), hex-decodes,
     * or otherwise mutates the placeholder bytes between rendering
     * and serialisation, the audit will silently pass a real leak.
     * No current §6.4.3 boundary applies such transforms — but the
     * detector is hard-coupled to today's render format. If the
     * placeholder format ever extends (longer hash, structured
     * suffix, alternative encoding), detectLeak must be updated to
     * a regex variant covering all renderings.
     */
    static LeakDetail detectLeak(std::string_view s, const SourcePlaceholder & placeholder, const std::string & name)
    {
        LeakDetail d{};
        auto bodyForm = placeholder.render();
        std::string contextForm = "~" + bodyForm.substr(1) + ":" + name;
        if (s.find(bodyForm) != std::string_view::npos) {
            d.bodyLeak = true;
            d.firstLeakedText = bodyForm;
        } else if (s.find(contextForm) != std::string_view::npos) {
            d.contextLeak = true;
            d.firstLeakedText = contextForm;
        }
        return d;
    }

    /**
     * Print a single human-friendly verdict line for a boundary,
     * keyed by the boundary name. Output:
     *
     *   PASS:  <boundary> — output ok, no placeholder text visible
     *   LEAK:  <boundary> — output contained `<text>`
     *   THROW: <boundary> — `<message>`
     *
     * Tests should call `recordPass`, `recordLeak`, or `recordThrow`
     * exactly once before letting the test body's `EXPECT_*`
     * assertions decide pass/fail.
     */
    static void recordPass(std::string_view boundary)
    {
        std::cerr << "PASS:  " << boundary << " — output ok, no placeholder text visible\n";
    }

    static void recordLeak(std::string_view boundary, const LeakDetail & d)
    {
        std::cerr << "LEAK:  " << boundary << " — output contained `" << d.firstLeakedText << "`"
                  << (d.bodyLeak ? " (body form)" : " (context form)") << "\n";
    }

    static void recordThrow(std::string_view boundary, std::string_view message)
    {
        std::cerr << "THROW: " << boundary << " — " << message << "\n";
    }
};

} // namespace nix

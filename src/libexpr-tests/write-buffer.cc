#include "nix/expr/write-buffer.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/print.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/value-to-xml.hh"
#include "nix/store/derivations.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/store/derivation/aterm.hh"
#include <rapidcheck/gtest.h>
#include "nix/store/tests/derivation.hh"
#include "nix/util/util.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/store/local-store.hh"
#include "nix/store/gc-store.hh"
#include "nix/util/archive.hh"
#include "nix/util/hash.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/object-hash.hh"

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <streambuf>
#include <typeinfo>
#include <set>

namespace nix {

/* The buffers these tests flush by hand never flush on their own. */
static constexpr uint64_t neverAutoFlush = std::numeric_limits<uint64_t>::max();

/* An evaluator over a writable in-memory store with deferred writes on.
   Note that the fixture's own `store` is a second, read-only store; the
   evaluator's is `state.store`. */
class WriteBufferTest : public LibExprTest
{
protected:
    WriteBufferTest()
        : LibExprTest(openStore("dummy://?read-only=false"), [](bool & readOnlyMode) {
            /* So that `fetchTree` and the flake builtins exist to be swept;
               registered at evaluator construction, hence here. */
            experimentalFeatureSettings.experimentalFeatures.get().insert(Xp::FetchTree);
            experimentalFeatureSettings.experimentalFeatures.get().insert(Xp::Flakes);
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            return settings;
        })
    {
        readOnlyMode = false;
    }

    std::string readStoreFile(const StorePath & path)
    {
        return state.store->requireStoreObjectAccessor(path)->readFile(CanonPath::root);
    }

    /** A derivation expression whose path depends on `name` only. */
    static std::string drvExpr(std::string_view name)
    {
        return "(derivation { name = \"" + std::string(name)
               + "\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; })";
    }

    /** The store path text a string value carries, read raw: a test may. */
    std::string pathText(std::string expr)
    {
        return std::string(RawValueBytes::view(eval(expr)));
    }
};

/* A sink that fails the test if bytes naming a watched store path reach it
   while that path is still pending: condition (C2) at the byte level, for
   whatever is printed into it. */
class WriteBufferEscapeCheckingSink : public std::streambuf
{
    EvalState & state;
    std::vector<std::string> watched;
    std::string received;

    void check()
    {
        for (auto & p : watched)
            if (received.find(p) != std::string::npos)
                EXPECT_FALSE(state.writeBuffer.contains(state.store->parseStorePath(p)))
                    << "the pending path " << p << " reached the sink";
    }

protected:
    std::streamsize xsputn(const char * s, std::streamsize n) override
    {
        received.append(s, n);
        check();
        return n;
    }

    int_type overflow(int_type c) override
    {
        if (c != traits_type::eof()) {
            received.push_back(traits_type::to_char_type(c));
            check();
        }
        return traits_type::not_eof(c);
    }

public:
    WriteBufferEscapeCheckingSink(EvalState & state, std::vector<std::string> watched)
        : state(state)
        , watched(std::move(watched))
    {
    }

    const std::string & bytes() const
    {
        return received;
    }
};

TEST_F(WriteBufferTest, textsAreWrittenOnFlushWithTheirReferences)
{
    WriteBuffer buffer(*state.store, neverAutoFlush);
    auto dep = buffer.addText("dep", "dep contents", {});
    auto top = buffer.addText("top", "refers to " + state.store->printStorePath(dep), {dep});
    EXPECT_EQ(buffer.size(), 2);
    EXPECT_TRUE(buffer.contains(dep));
    EXPECT_FALSE(state.store->isValidPath(dep));
    EXPECT_FALSE(state.store->isValidPath(top));

    buffer.flush();

    EXPECT_TRUE(buffer.empty());
    ASSERT_TRUE(state.store->isValidPath(dep));
    ASSERT_TRUE(state.store->isValidPath(top));
    auto info = state.store->queryPathInfo(top);
    EXPECT_EQ(info->references, StorePathSet{dep});
    EXPECT_FALSE(info->ultimate);
    ASSERT_TRUE(info->ca);
    EXPECT_EQ(info->ca->method, ContentAddressMethod::Raw::Text);
    EXPECT_EQ(readStoreFile(top), "refers to " + state.store->printStorePath(dep));
    EXPECT_EQ(readStoreFile(dep), "dep contents");
}

TEST_F(WriteBufferTest, addIsIdempotentAndFlushOfNothingIsNothing)
{
    WriteBuffer buffer(*state.store, neverAutoFlush);
    auto a = buffer.addText("a", "same", {});
    auto b = buffer.addText("a", "same", {});
    EXPECT_EQ(a, b);
    EXPECT_EQ(buffer.size(), 1);
    buffer.flush();
    EXPECT_TRUE(buffer.empty());
    buffer.flush();
    EXPECT_TRUE(state.store->isValidPath(a));
    /* Enqueueing something already in the store and flushing is harmless. */
    EXPECT_EQ(buffer.addText("a", "same", {}), a);
    buffer.flush();
    EXPECT_TRUE(state.store->isValidPath(a));
}

/* The buffer flushes itself once its pending content crosses a cap, so peak
   memory stays bounded when a pure output sink accumulates the whole value
   (doc/lazy-store/04-derivation.md, the write buffer).  Here the cap is tiny
   so a few small objects cross it; the default is 32 MiB. */
TEST_F(WriteBufferTest, crossingTheThresholdFlushesToBoundMemory)
{
    WriteBuffer buffer(*state.store, 3);
    auto a = buffer.addText("a", "0123456789", {});
    EXPECT_EQ(buffer.size(), 1);
    auto b = buffer.addText("b", "0123456789", {});
    EXPECT_EQ(buffer.size(), 2);
    /* The third addition reaches the cap of 3 and flushes everything pending. */
    auto c = buffer.addText("c", "0123456789", {});
    EXPECT_TRUE(buffer.empty());
    EXPECT_TRUE(state.store->isValidPath(a));
    EXPECT_TRUE(state.store->isValidPath(b));
    EXPECT_TRUE(state.store->isValidPath(c));
    /* The buffer keeps accumulating afterwards, from a reset counter. */
    auto d = buffer.addText("d", "short", {});
    EXPECT_EQ(buffer.size(), 1);
    buffer.flush();
    EXPECT_TRUE(state.store->isValidPath(d));
}

/* With the tightest cap every object flushes as it is enqueued, so a referrer
   is added only after its reference is already valid; the buffer must still
   record the reference (doc/lazy-store/05-validation.md, the differential harness). */
TEST_F(WriteBufferTest, autoFlushKeepsReferencesValidForLaterReferrers)
{
    WriteBuffer buffer(*state.store, 1);
    auto dep = buffer.addText("dep", "dep", {});
    EXPECT_TRUE(buffer.empty());
    ASSERT_TRUE(state.store->isValidPath(dep));
    auto top = buffer.addText("top", "top refers to dep", {dep});
    EXPECT_TRUE(buffer.empty());
    ASSERT_TRUE(state.store->isValidPath(top));
    EXPECT_EQ(state.store->queryPathInfo(top)->references, StorePathSet{dep});
}

TEST_F(WriteBufferTest, evaluationDefersADerivationUntilItsPathLeaves)
{
    auto v = eval("(derivation { name = \"foo\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; }).drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);

    NixStringContext context;
    auto drvPath = state.coerceToStorePath(noPos, v, context, "while testing");

    EXPECT_TRUE(state.writeBuffer.empty());
    ASSERT_TRUE(state.store->isValidPath(drvPath));
    auto drv = state.store->readDerivation(drvPath);
    EXPECT_EQ(drv.name, "foo");
    EXPECT_EQ(drvPath, computeStorePath(*store, drv));
}

TEST_F(WriteBufferTest, discardingContextLeavesTheObjectPending)
{
    /* Dropping provenance inside the evaluator writes nothing: the doors write
       totally and the reads write what they dereference, so the text is safe
       until it leaves. */
    auto v = eval("builtins.unsafeDiscardStringContext (builtins.toFile \"hello\" \"hi\")");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto s = state.realise(v);
    EXPECT_TRUE(state.writeBuffer.empty());
    auto path = state.store->parseStorePath(std::string(s.view()));
    ASSERT_TRUE(state.store->isValidPath(path));
    EXPECT_EQ(readStoreFile(path), "hi");
}

TEST_F(WriteBufferTest, referencedPendingObjectsAreWrittenTogether)
{
    auto v = eval(
        "(derivation { name = \"foo\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; script = builtins.toFile \"s\" \"echo\"; }).drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 2);
    NixStringContext context;
    auto drvPath = state.coerceToStorePath(noPos, v, context, "while testing");
    EXPECT_TRUE(state.writeBuffer.empty());
    auto drv = state.store->readDerivation(drvPath);
    ASSERT_EQ(drv.inputs.size(), 1);
    auto script = drv.inputs.begin()->getBaseStorePath();
    EXPECT_TRUE(state.store->isValidPath(script));
    EXPECT_EQ(readStoreFile(script), "echo");
}

TEST_F(WriteBufferTest, realiseWritesWhatTheContextNames)
{
    auto v = eval(drvExpr("a") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto s = state.realise(v);
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_EQ(s.view(), RawValueBytes::view(v));
    EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(std::string(s.view()))));
}

TEST_F(WriteBufferTest, realiseOfAContextFreeStringWritesEverythingPending)
{
    /* The door writes what is pending whatever the string's own provenance
       says: a context-free text may still name a pending path, and only a
       total write makes its bytes safe to let out. */
    auto v = eval("builtins.seq " + drvExpr("b") + ".drvPath \"plain\"");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto s = state.realise(v);
    EXPECT_EQ(s.view(), "plain");
    EXPECT_TRUE(state.writeBuffer.empty());
    /* Likewise bytes with an empty accumulated context. */
    eval(drvExpr("b2") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    (void) state.realise(std::string("bytes"));
    EXPECT_TRUE(state.writeBuffer.empty());
}

TEST_F(WriteBufferTest, finishWritesEverythingPending)
{
    /* The evaluator's last act before an exec, where no destructor runs. */
    auto v = eval(drvExpr("fin") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    [[maybe_unused]] auto finished = state.finish();
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(std::string(RawValueBytes::view(v)))));
}

TEST_F(WriteBufferTest, aContextFreeLiteralNamingAPendingPathCanBeRead)
{
    /* The reads' side of the same case: a dereference of a pending object's
       text, arriving without context, finds the object, as it would under the
       reference, which wrote it on creation.  Written when read and not
       before: any other path leaves the batch intact. */
    auto written = pathText("builtins.toFile \"written\" \"w\"");
    state.flushPendingWrites();
    auto text = pathText("builtins.toFile \"readme\" \"read me\"");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    /* Paths that name no pending object leave the batch alone: none, a
       malformed store path, a store path already written. */
    EXPECT_FALSE(eval("builtins.pathExists \"/no/such/path\"").boolean());
    EXPECT_FALSE(eval("builtins.pathExists \"" + state.store->storeDir + "/not-a-store-path\"").boolean());
    EXPECT_TRUE(eval("builtins.pathExists \"" + written + "\"").boolean());
    EXPECT_EQ(state.writeBuffer.size(), 1);
    EXPECT_TRUE(eval("builtins.pathExists \"" + text + "\"").boolean());
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_EQ(RawValueBytes::view(eval("builtins.readFile \"" + text + "\"")), "read me");
}

TEST_F(WriteBufferTest, aContextFreeLiteralNamingAPendingPathCannotEscape)
{
    /* The case an inventory of context-bearing eliminators cannot see: the
       text of a pending path arrives without context -- here as a literal,
       as it would from `builtins.readFile` of a lock file -- and is emitted
       while the object is still pending. */
    auto text = pathText(drvExpr("lit") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto lit = eval("\"" + text + "\"");
    EXPECT_FALSE(lit.context());
    auto s = state.realise(lit);
    EXPECT_EQ(s.view(), text);
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(text)));
}

TEST_F(WriteBufferTest, coerceAndEmitWrites)
{
    auto v = eval(drvExpr("c"));
    NixStringContext context;
    auto s = state.coerceAndEmit(noPos, v, context, "while testing");
    EXPECT_FALSE(context.empty());
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_TRUE(s.view().ends_with("-c"));
}

TEST_F(WriteBufferTest, serialisingIntoAValueWritesNothing)
{
    auto j = eval("builtins.toJSON " + drvExpr("d"));
    ASSERT_EQ(j.type(), nString);
    EXPECT_TRUE(j.context());
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto x = eval("builtins.toXML " + drvExpr("e"));
    ASSERT_EQ(x.type(), nString);
    EXPECT_TRUE(x.context());
    EXPECT_EQ(state.writeBuffer.size(), 2);
}

TEST_F(WriteBufferTest, contextDroppingBuiltinsLeaveTheObjectsPending)
{
    eval("builtins.match \"(.*)\" " + drvExpr("f") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    eval("builtins.split \"/\" " + drvExpr("g") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 2);
    eval("builtins.getContext " + drvExpr("h") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 3);
    (void) state.realise(std::string("a door"));
    EXPECT_TRUE(state.writeBuffer.empty());
}

TEST_F(WriteBufferTest, emitOfAValueWrites)
{
    auto v = eval(drvExpr("em") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto s = state.emit(v);
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(std::string(s.view()))));
}

TEST_F(WriteBufferTest, coercionsWriteWhatTheyDereferenceAndNothingForTheRest)
{
    /* The dereference rule at the coercions: a pending object drains the
       batch, a written one leaves it alone. */
    auto outW = eval(drvExpr("w") + ".outPath");
    auto drvW = eval(drvExpr("w") + ".drvPath");
    state.flushPendingWrites();
    eval(drvExpr("p") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    NixStringContext context;
    state.coerceToStorePath(noPos, drvW, context, "while testing");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    state.coerceToSingleDerivedPath(noPos, outW, "while testing");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto outP = eval(drvExpr("p") + ".outPath");
    state.coerceToSingleDerivedPath(noPos, outP, "while testing");
    EXPECT_TRUE(state.writeBuffer.empty());
}

TEST_F(WriteBufferTest, printedPathValueDoesNotEscape)
{
    auto text = pathText("builtins.toFile \"pv\" \"p\"");
    auto v = eval("/. + \"" + text + "\"");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    WriteBufferEscapeCheckingSink sink(state, {text});
    std::ostream out(&sink);
    printValue(state, out, v, PrintOptions{.force = true});
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_NE(sink.bytes().find(text), std::string::npos);
}

TEST_F(WriteBufferTest, printedInlineErrorDoesNotEscape)
{
    auto text = pathText(drvExpr("er") + ".drvPath");
    auto v = eval("{ a = throw \"" + text + "\"; }");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    WriteBufferEscapeCheckingSink sink(state, {text});
    std::ostream out(&sink);
    printValue(state, out, v, PrintOptions{.force = true, .errors = ErrorPrintBehavior::Print});
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_NE(sink.bytes().find(text), std::string::npos);
}

TEST_F(WriteBufferTest, realiseStringWritesEverythingPending)
{
    eval(drvExpr("rs") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    auto v = eval("\"plain\"");
    EXPECT_EQ(state.realiseString(v, nullptr), "plain");
    EXPECT_TRUE(state.writeBuffer.empty());
}

TEST_F(WriteBufferTest, evalCacheGettersAreDoors)
{
    auto text = pathText(drvExpr("ec") + ".drvPath");
    Value root = eval("{ s = \"" + text + "\"; l = [ \"" + text + "\" ]; }");
    auto cache = make_ref<eval_cache::EvalCache>(std::nullopt, state, [&]() { return &root; });
    EXPECT_EQ(state.writeBuffer.size(), 1);
    EXPECT_EQ(cache->getRoot()->getAttr("s")->getString(), text);
    EXPECT_TRUE(state.writeBuffer.empty());
    eval(drvExpr("ec2") + ".drvPath");
    EXPECT_EQ(state.writeBuffer.size(), 1);
    EXPECT_EQ(cache->getRoot()->getAttr("l")->getListOfStrings(), std::vector<std::string>{text});
    EXPECT_TRUE(state.writeBuffer.empty());
}

TEST_F(WriteBufferTest, valuePrinterRealisesBeforeEmitting)
{
    auto drvPath = pathText(drvExpr("j") + ".drvPath");
    auto file = pathText("builtins.toFile \"j\" \"text\"");
    EXPECT_EQ(state.writeBuffer.size(), 2);
    auto v = eval(
        "{ d = " + drvExpr("j") + "; t = builtins.toFile \"j\" \"text\"; s = \"x-${" + drvExpr("j")
        + ".drvPath}-y\"; }");

    WriteBufferEscapeCheckingSink sink(state, {drvPath, file});
    std::ostream out(&sink);
    NixStringContext context;
    printValue(state, out, v, PrintOptions{.force = true, .derivationPaths = true}, &context);

    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
    EXPECT_NE(sink.bytes().find(file), std::string::npos);
    EXPECT_FALSE(context.empty());
}

TEST_F(WriteBufferTest, ambiguousPrinterRealisesBeforeEmitting)
{
    auto drvPath = pathText(drvExpr("k") + ".drvPath");
    auto v = eval("{ p = " + drvExpr("k") + ".drvPath; t = builtins.toFile \"k\" \"text\"; }");
    state.forceValueDeep(v);
    EXPECT_EQ(state.writeBuffer.size(), 2);

    WriteBufferEscapeCheckingSink sink(state, {drvPath});
    std::ostream out(&sink);
    std::set<const void *> seen;
    NixStringContext context;
    printAmbiguous(state, v, out, &seen, &context);

    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
}

TEST_F(WriteBufferTest, jsonRenderedThenRealisedDoesNotEscape)
{
    auto drvPath = pathText(drvExpr("l") + ".drvPath");
    auto v = eval("{ p = " + drvExpr("l") + ".drvPath; t = builtins.toFile \"l\" \"text\"; }");
    EXPECT_EQ(state.writeBuffer.size(), 1);

    /* The serialiser is pure and hands out a string, which `emit` writes for. */
    NixStringContext context;
    auto json = renderValueAsJSON(state, true, v, noPos, context, false);
    EXPECT_NE(json.find(drvPath), std::string::npos);
    EXPECT_EQ(state.writeBuffer.size(), 2);

    WriteBufferEscapeCheckingSink sink(state, {drvPath});
    std::ostream out(&sink);
    out << state.emit(std::move(json), context);

    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
}

TEST_F(WriteBufferTest, xmlRenderedThenRealisedDoesNotEscape)
{
    auto drvPath = pathText(drvExpr("m") + ".drvPath");
    auto v = eval("{ d = " + drvExpr("m") + "; t = builtins.toFile \"m\" \"text\"; }");

    NixStringContext context;
    auto xml = renderValueAsXML(state, true, false, v, context, noPos);
    /* The serialiser is pure: the derivation element names the path, nothing is written yet. */
    EXPECT_NE(xml.find(drvPath), std::string::npos);
    EXPECT_FALSE(state.writeBuffer.empty());

    WriteBufferEscapeCheckingSink sink(state, {drvPath});
    std::ostream out(&sink);
    out << state.emit(std::move(xml), context);

    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
}

/* The law of the buffer (doc/lazy-store/01-specification.md, section 6): every
   builtin returns on the same inputs what it returns when those inputs are
   already written, and the store holds the same objects afterwards.  There is
   one codepath now (the evaluator always buffers), so the two sides are the
   same buffer at two flush timings: this fixture's evaluator accumulates,
   while the reference sets `deferred-store-writes-max-pending = 1` so every
   object is written the moment it is created, before the next builtin sees it,
   as an eager writer would.  A builtin that dereferences a name it was given
   (reads the store, hands it to a fetcher or a builder) without a flush fails
   on the accumulating side and nowhere in the functional suite, which found
   none of the two this check was written after. */
TEST_F(WriteBufferTest, everyBuiltinAgreesWithEagerEvaluationOnPendingInputs)
{
    EvalSettings eagerSettings{readOnlyMode};
    eagerSettings.nixPath = {};
    eagerSettings.deferredStoreWritesMaxPending = 1;
    auto eagerStore = openStore("dummy://?read-only=false");
    EvalState eager({}, eagerStore, fetchSettings, eagerSettings, nullptr);

    struct Outcome
    {
        bool ok;
        std::string text;

        bool operator==(const Outcome &) const = default;
    };

    auto evaluate = [&](EvalState & s, const std::string & expr) -> Outcome {
        try {
            Value v;
            s.eval(s.parseExprFromString(expr, s.rootPath(CanonPath::root)), v);
            s.forceValueDeep(v);
            std::ostringstream out;
            printValue(s, out, v, PrintOptions{.force = true, .derivationPaths = true});
            return {true, out.str()};
        } catch (Error & e) {
            return {false, std::string(typeid(e).name()) + ": " + e.msg()};
        } catch (std::exception & e) {
            return {false, std::string(typeid(e).name()) + ": " + e.what()};
        }
    };

    const std::string text = "(builtins.toFile \"sweep-text\" \"hello\")";
    const std::string drv = drvExpr("sweep") + ".drvPath";
    const std::string attrs = "{ path = " + text + "; url = " + text + "; name = \"sweep\"; type = \"path\"; }";
    const std::vector<std::string> inputs{text, drv, attrs};

    /* A result that is a store path names an object both stores must then
       hold or both lack. */
    auto storePathOf = [&](const Outcome & o) -> std::optional<StorePath> {
        if (!o.ok || o.text.size() < 3 || o.text.front() != '"' || o.text.back() != '"')
            return std::nullopt;
        auto body = o.text.substr(1, o.text.size() - 2);
        if (!state.store->isStorePath(body))
            return std::nullopt;
        return state.store->parseStorePath(body);
    };

    /* Not comparable between two evaluations, or not to be run here. */
    const std::set<std::string> skipped{"currentTime", "exec", "break"};

    std::vector<std::string> disagreements;
    size_t calls = 0;

    for (auto & attr : *state.getBuiltins().attrs()) {
        std::string name(state.symbols[attr.name]);
        state.forceValue(*attr.value, noPos);
        if (attr.value->type() != nFunction || skipped.contains(name))
            continue;
        for (auto & input : inputs)
            for (auto arity : {1, 2}) {
                std::string expr = "builtins." + name;
                for (int i = 0; i < arity; ++i)
                    expr += " " + input;
                auto deferred = evaluate(state, expr);
                auto reference = evaluate(eager, expr);
                calls++;
                if (deferred != reference)
                    disagreements.push_back(
                        expr + "\n    deferred:  " + deferred.text + "\n    reference: " + reference.text);
                /* Whatever the builtin did, the stores hold the same objects
                   once the deferring evaluator has written. */
                state.flushPendingWrites();
                if (auto path = storePathOf(deferred))
                    if (state.store->isValidPath(*path) != eagerStore->isValidPath(*path))
                        disagreements.push_back(expr + "\n    store validity of the result differs");
            }
    }

    EXPECT_GT(calls, 100);
    EXPECT_TRUE(disagreements.empty()) << disagreements.size() << " disagreement(s):\n"
                                       << concatStringsSep("\n", disagreements);
}

/* The two builtins the sweep was written after, named.  The path fetcher
   behind `fetchTree` reads the real filesystem, where an in-memory store's
   object never is, so here it fails in both modes alike; what can be seen
   is that the write preceded the fetcher's read.  The command-line probe of
   `05-validation.md` describes the harness that sees the whole. */
TEST_F(WriteBufferTest, dereferencingBuiltinsWriteFirst)
{
    auto sp = eval("builtins.storePath " + drvExpr("deref") + ".drvPath");
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_TRUE(sp.context());

    auto file = pathText("builtins.toFile \"deref-text\" \"hello\"");
    EXPECT_FALSE(state.writeBuffer.empty());
    EXPECT_THROW(
        eval("(builtins.fetchTree { type = \"path\"; path = builtins.toFile \"deref-text\" \"hello\"; }).outPath"),
        Error);
    EXPECT_TRUE(state.writeBuffer.empty());
    EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(file)));
}

/* Carried test support (`Arbitrary<Derivation>`, doc/lazy-store/04-derivation.md §4) with its consumer: whatever
   derivation evaluation could produce, the buffer writes what `writeDerivation` would have written, at the path
   `computeStorePath` gives, and the store reads it back. */
RC_GTEST_FIXTURE_PROP(WriteBufferTest, prop_derivations_round_trip_through_the_buffer, (const Derivation & drv))
{
    WriteBuffer buffer(*state.store, neverAutoFlush);
    auto path = buffer.addDerivation(drv);
    RC_ASSERT(path == computeStorePath(*state.store, drv));
    RC_ASSERT(buffer.contains(path));
    buffer.flush();
    RC_ASSERT(state.store->isValidPath(path));
    try {
        RC_ASSERT(unparse(state.store->readDerivation(path), *state.store) == unparse(drv, *state.store));
    } catch (Error & e) {
        RC_FAIL(e.msg() + "\nwhile reading back:\n" + unparse(drv, *state.store));
    }
}

/* Repair, the one flush behaviour the in-memory dummy store cannot show:
   it throws on repair (dummy-store.cc), so this uses a chroot LocalStore
   whose objects are real files.  An object is written, then deleted out of
   band so the database still calls it valid; a plain re-enqueue trusts that
   validity and rewrites nothing, and a re-enqueue with Repair rewrites it.
   The repair unit test left open in doc/lazy-store/04-derivation.md, section
   2, risks and open items. */
struct WriteBufferLocalStoreTest : LibStoreTest
{};

TEST_F(WriteBufferLocalStoreTest, repairRewritesAnObjectTheDatabaseStillCallsValid)
{
    auto tmpRoot = createTempDir();
    AutoDelete del(tmpRoot, true);
    createDirs(tmpRoot / "nix/store");
    auto store = openStore(fmt("local?root=%s", tmpRoot.string()));

    auto real = [&](const StorePath & p) { return (tmpRoot / "nix/store" / std::string(p.to_string())).string(); };

    WriteBuffer buffer(*store, neverAutoFlush);
    auto path = buffer.addText("repair-me", "original", {});
    buffer.flush();
    ASSERT_TRUE(store->isValidPath(path));
    ASSERT_EQ(readFile(real(path)), "original");

    /* Corrupt the object out of band: remove the file the store believes in.
       The database is untouched, so the path is still valid to a query. */
    deletePath(real(path));
    ASSERT_FALSE(pathExists(real(path)));
    ASSERT_TRUE(store->isValidPath(path));

    /* A plain re-enqueue trusts the database and writes nothing back. */
    EXPECT_EQ(buffer.addText("repair-me", "original", {}), path);
    buffer.flush();
    EXPECT_FALSE(pathExists(real(path)));

    /* A re-enqueue asking for repair rewrites the object. */
    EXPECT_EQ(buffer.addText("repair-me", "original", {}, Repair), path);
    buffer.flush();
    ASSERT_TRUE(pathExists(real(path)));
    EXPECT_EQ(readFile(real(path)), "original");
}

/* The references a pending object already has in the store are temp-rooted at
   enqueue, so a garbage collection that runs before the flush cannot delete
   them and leave the flush dangling.  This is the invariant the original
   branch got wrong (04-derivation.md §4 and section
   1.6).  Deterministic: the collection runs in this process, which holds the
   temp roots, over two unrooted objects, one of which a pending object names. */
TEST_F(WriteBufferLocalStoreTest, gcKeepsTheExistingReferencesOfPendingObjects)
{
    auto tmpRoot = createTempDir();
    AutoDelete del(tmpRoot, true);
    createDirs(tmpRoot / "nix/store");
    auto store = openStore(fmt("local?root=%s", tmpRoot.string()));
    auto * localStore = dynamic_cast<LocalStore *>(&*store);
    ASSERT_TRUE(localStore);

    /* A valid text object with no root of its own, registered directly (an
       object a previous command wrote and left unreferenced sits like this). */
    auto addUnrooted = [&](std::string_view name, std::string contents) -> StorePath {
        HashSink narSink(HashAlgorithm::SHA256);
        dumpString(contents, narSink);
        auto [narHash, narSize] = narSink.finish();
        /* The object hash of a plain file is its blob id; registration
           requires it. */
        auto info = ValidPathInfo::makeFromCA(
            *store,
            name,
            TextInfo{.hash = hashString(HashAlgorithm::SHA256, contents), .references = {}},
            ObjectHash::of(merkle::TreeEntry{.mode = merkle::Mode::Regular, .hash = merkle::blobId(contents)}));
        info.narSize = narSize;
        auto path = info.path;
        writeFile((tmpRoot / "nix/store" / std::string(path.to_string())).string(), contents);
        localStore->registerValidPaths({{path, std::move(info)}});
        return path;
    };

    auto referenced = addUnrooted("kept", "referenced source");
    auto dead = addUnrooted("dead", "unreferenced");
    ASSERT_TRUE(store->isValidPath(referenced));
    ASSERT_TRUE(store->isValidPath(dead));

    /* A pending object naming the first source temp-roots it at enqueue. */
    WriteBuffer buffer(*store, neverAutoFlush);
    auto d = buffer.addText("refs", "-> " + store->printStorePath(referenced), {referenced});
    ASSERT_TRUE(buffer.contains(d));

    /* Collect garbage while d is still pending. */
    GCOptions options;
    GCResults results;
    localStore->collectGarbage(options, results);

    EXPECT_TRUE(store->isValidPath(referenced)) << "a temp-rooted reference was collected";
    EXPECT_FALSE(store->isValidPath(dead)) << "a dead object survived; the collector did not run";

    /* The flush still writes d, its reference valid. */
    buffer.flush();
    ASSERT_TRUE(store->isValidPath(d));
    EXPECT_EQ(store->queryPathInfo(d)->references, StorePathSet{referenced});
}

} // namespace nix

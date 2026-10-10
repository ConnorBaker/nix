#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/print.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/value-to-xml.hh"
#include "nix/store/derivations.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/store/derivation/aterm.hh"
#include "nix/util/util.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/terminal.hh"
#include "nix/store/local-store.hh"
#include "nix/store/gc-store.hh"
#include "nix/util/archive.hh"
#include "nix/util/hash.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/tests/setting-scopes.hh"

#include <gtest/gtest.h>
#include <algorithm>
#include <sstream>
#include <streambuf>
#include <typeinfo>

namespace nix {

// Intercept the real publication boundary: roots must already exist here,
// before the backend is allowed to collect garbage or reject an upload.
struct TextPublicationStore : LocalStore
{
    using Store::invalidatePathInfoCacheFor;

    explicit TextPublicationStore(ref<const LocalStore::Config> config)
        : Store{*config}
        , LocalFSStore{*config}
        , LocalStore{config}
    {
    }

    std::optional<std::string> failingName;
    bool collectBeforeWrite = false;
    size_t uploads = 0;
    size_t rootCalls = 0;
    std::optional<StorePath> collectBeforeValidity;
    size_t validityCollections = 0;

    bool isValidPathUncached(const StorePath & path) override
    {
        if (collectBeforeValidity == path) {
            collectBeforeValidity.reset();
            ++validityCollections;
            GCOptions options;
            options.pathsToDelete = GCOptions::SpecificPaths{.paths = {path}};
            GCResults results;
            collectGarbage(options, results);
        }
        return LocalStore::isValidPathUncached(path);
    }

    void addTempRoot(const StorePath & path) override
    {
        rootCalls++;
        LocalStore::addTempRoot(path);
    }

    StorePath addToStoreFromDump(
        Source & source,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair) override
    {
        uploads++;
        if (collectBeforeWrite) {
            collectBeforeWrite = false;
            GCOptions options;
            GCResults results;
            collectGarbage(options, results);
        }
        if (failingName == name)
            throw Error("injected write failure for '%s'", name);
        return LocalStore::addToStoreFromDump(source, name, dumpMethod, hashMethod, hashAlgo, references, repair);
    }
};

struct TextStoreRoot
{
    AutoDelete root{canonPath(createTempDir(), true)};
    ref<TextPublicationStore> local =
        make_ref<TextPublicationStore>(make_ref<LocalStoreConfig>(root.path(), StoreConfig::Params{}));
};

// Base destruction keeps the store and its files alive through EvalState.
class TextStoreTest : protected TextStoreRoot, public LibExprTest
{
protected:
    TextStoreTest()
        : LibExprTest(local, [](bool & readOnlyMode) {
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

    static std::string drvExpr(std::string_view name)
    {
        return "(derivation { name = \"" + std::string(name)
               + "\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; })";
    }

    std::string pathText(std::string expr)
    {
        return std::string(eval(expr).string_view());
    }

    StorePath addUnrooted(
        std::string_view name,
        std::string contents,
        ContentAddressMethod method = ContentAddressMethod::Raw::Text,
        const StorePathSet & references = {})
    {
        HashSink narSink(HashAlgorithm::SHA256);
        dumpString(contents, narSink);
        auto [narHash, narSize] = narSink.finish();
        auto info = ValidPathInfo::makeFromCA(
            *local,
            name,
            ContentAddressWithReferences::fromParts(
                method,
                method == ContentAddressMethod::Raw::Git ? merkle::blobId(contents)
                : method == ContentAddressMethod::Raw::NixArchive ? narHash
                                                                  : hashString(HashAlgorithm::SHA256, contents),
                {.others = references}),
            ObjectHash::of(merkle::TreeEntry{.mode = merkle::Mode::Regular, .hash = merkle::blobId(contents)}));
        info.narSize = narSize;
        auto path = info.path;
        writeFile(local->toRealPath(path), contents);
        local->registerValidPaths({{path, std::move(info)}});
        return path;
    }
};

class StoreEscapeCheckingSink : public std::streambuf
{
    EvalState & state;
    std::vector<std::string> watched;
    std::string received;

    void check()
    {
        for (auto & p : watched)
            if (received.find(p) != std::string::npos) {
                EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(p)))
                    << "an absent path " << p << " reached the sink";
            }
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
    StoreEscapeCheckingSink(EvalState & state, std::vector<std::string> watched)
        : state(state)
        , watched(std::move(watched))
    {
    }

    const std::string & bytes() const
    {
        return received;
    }
};

TEST_F(TextStoreTest, derivationIsStoredBeforeItsPathIsReturned)
{
    auto v = eval("(derivation { name = \"foo\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; }).drvPath");

    ASSERT_TRUE(local->isValidPath(local->parseStorePath(v.string_view())));
    NixStringContext context;
    auto drvPath = state.coerceToStorePath(noPos, v, context, "while testing");

    ASSERT_TRUE(state.store->isValidPath(drvPath));
    auto drv = state.store->readDerivation(drvPath);
    EXPECT_EQ(drv.name, "foo");
    EXPECT_EQ(drvPath, computeStorePath(*store, drv));
}

TEST_F(TextStoreTest, discardingContextKeepsTheCreatedObject)
{

    auto v = eval("builtins.unsafeDiscardStringContext (builtins.toFile \"hello\" \"hi\")");
    EXPECT_FALSE(v.context());
    auto s = v.string_view();
    auto path = state.store->parseStorePath(std::string(s));
    ASSERT_TRUE(state.store->isValidPath(path));
    EXPECT_EQ(readStoreFile(path), "hi");
}

TEST_F(TextStoreTest, derivationReferencesAreStoredBeforePublication)
{
    auto v = eval(
        "(derivation { name = \"foo\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; script = builtins.toFile \"s\" \"echo\"; }).drvPath");
    NixStringContext context;
    auto drvPath = state.coerceToStorePath(noPos, v, context, "while testing");
    auto drv = state.store->readDerivation(drvPath);
    ASSERT_EQ(drv.inputs.size(), 1u);
    auto script = drv.inputs.begin()->getBaseStorePath();
    EXPECT_TRUE(state.store->isValidPath(script));
    EXPECT_EQ(readStoreFile(script), "echo");
}

TEST_F(TextStoreTest, contextFreeLiteralCanReadCreatedText)
{

    auto written = pathText("builtins.toFile \"written\" \"w\"");
    auto text = pathText("builtins.toFile \"readme\" \"read me\"");

    EXPECT_FALSE(eval("builtins.pathExists \"/no/such/path\"").boolean());
    EXPECT_FALSE(eval("builtins.pathExists \"" + state.store->storeDir + "/not-a-store-path\"").boolean());
    EXPECT_TRUE(eval("builtins.pathExists \"" + written + "\"").boolean());
    EXPECT_TRUE(eval("builtins.pathExists \"" + text + "\"").boolean());
    EXPECT_EQ((eval("builtins.readFile \"" + text + "\"")).string_view(), "read me");
}

TEST_F(TextStoreTest, contextFreeLiteralNamesAnExistingDerivation)
{

    auto text = pathText(drvExpr("lit") + ".drvPath");
    auto lit = eval("\"" + text + "\"");
    EXPECT_FALSE(lit.context());
    auto s = lit.string_view();
    EXPECT_EQ(s, text);
    EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(text)));
}

TEST_F(TextStoreTest, serialisingPreservesCreatedObjectsAndContext)
{
    auto j = eval("builtins.toJSON " + drvExpr("d"));
    ASSERT_EQ(j.type(), nString);
    EXPECT_TRUE(j.context());
    auto x = eval("builtins.toXML " + drvExpr("e"));
    ASSERT_EQ(x.type(), nString);
    EXPECT_TRUE(x.context());
    EXPECT_EQ(local->queryAllValidPaths().size(), 2u);
}

TEST_F(TextStoreTest, contextDroppingBuiltinsKeepCreatedObjects)
{
    eval("builtins.match \"(.*)\" " + drvExpr("f") + ".drvPath");
    eval("builtins.split \"/\" " + drvExpr("g") + ".drvPath");
    eval("builtins.getContext " + drvExpr("h") + ".drvPath");
    auto paths = local->queryAllValidPaths();
    ASSERT_EQ(paths.size(), 3u);
    for (const auto & path : paths)
        EXPECT_TRUE(path.isDerivation());
}

TEST_F(TextStoreTest, coercingDerivationOutputsDoesNotBuildThem)
{

    auto outW = eval(drvExpr("w") + ".outPath");
    auto drvW = eval(drvExpr("w") + ".drvPath");
    eval(drvExpr("p") + ".drvPath");
    NixStringContext context;
    state.coerceToStorePath(noPos, drvW, context, "while testing");
    state.coerceToSingleDerivedPath(noPos, outW, "while testing");
    auto outP = eval(drvExpr("p") + ".outPath");
    state.coerceToSingleDerivedPath(noPos, outP, "while testing");
    EXPECT_FALSE(local->isValidPath(local->parseStorePath(outW.string_view())));
    EXPECT_FALSE(local->isValidPath(local->parseStorePath(outP.string_view())));
    auto paths = local->queryAllValidPaths();
    ASSERT_EQ(paths.size(), 2u);
    for (const auto & path : paths)
        EXPECT_TRUE(path.isDerivation());
}

TEST_F(TextStoreTest, printedPathNamesAnExistingObject)
{
    auto text = pathText("builtins.toFile \"pv\" \"p\"");
    auto v = eval("/. + \"" + text + "\"");
    StoreEscapeCheckingSink sink(state, {text});
    std::ostream out(&sink);
    printValue(state, out, v, PrintOptions{.force = true});
    EXPECT_NE(sink.bytes().find(text), std::string::npos);
}

TEST_F(TextStoreTest, printedInlineErrorNamesAnExistingObject)
{
    auto text = pathText(drvExpr("er") + ".drvPath");
    auto v = eval("{ a = throw \"" + text + "\"; }");
    StoreEscapeCheckingSink sink(state, {text});
    std::ostream out(&sink);
    printValue(state, out, v, PrintOptions{.force = true, .errors = ErrorPrintBehavior::Print});
    EXPECT_NE(sink.bytes().find(text), std::string::npos);
}

TEST_F(TextStoreTest, evalCacheGettersReadExistingObjects)
{
    auto text = pathText(drvExpr("ec") + ".drvPath");
    Value root = eval("{ s = \"" + text + "\"; l = [ \"" + text + "\" ]; }");
    auto cache = make_ref<eval_cache::EvalCache>(std::nullopt, state, [&]() { return &root; });
    EXPECT_EQ(cache->getRoot()->getAttr("s")->getString(), text);
    eval(drvExpr("ec2") + ".drvPath");
    EXPECT_EQ(cache->getRoot()->getAttr("l")->getListOfStrings(), std::vector<std::string>{text});
}

TEST_F(TextStoreTest, valuePrinterPreservesContextAndExistingObjects)
{
    auto drvPath = pathText(drvExpr("j") + ".drvPath");
    auto file = pathText("builtins.toFile \"j\" \"text\"");
    auto v = eval(
        "{ d = " + drvExpr("j") + "; t = builtins.toFile \"j\" \"text\"; s = \"x-${" + drvExpr("j")
        + ".drvPath}-y\"; }");

    StoreEscapeCheckingSink sink(state, {drvPath, file});
    std::ostream out(&sink);
    NixStringContext context;
    printValue(state, out, v, PrintOptions{.force = true, .derivationPaths = true}, &context);

    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
    EXPECT_NE(sink.bytes().find(file), std::string::npos);
    EXPECT_FALSE(context.empty());
}

TEST_F(TextStoreTest, ambiguousPrinterNamesExistingObjects)
{
    auto drvPath = pathText(drvExpr("k") + ".drvPath");
    auto v = eval("{ p = " + drvExpr("k") + ".drvPath; t = builtins.toFile \"k\" \"text\"; }");
    state.forceValueDeep(v);

    StoreEscapeCheckingSink sink(state, {drvPath});
    std::ostream out(&sink);
    std::set<const void *> seen;
    NixStringContext context;
    printAmbiguous(state, v, out, &seen, &context);

    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
}

TEST_F(TextStoreTest, jsonNamesExistingObjectsBeforeDelivery)
{
    auto drvPath = pathText(drvExpr("l") + ".drvPath");
    auto v = eval("{ p = " + drvExpr("l") + ".drvPath; t = builtins.toFile \"l\" \"text\"; }");

    NixStringContext context;
    auto json = renderValueAsJSON(state, true, v, noPos, context, false);
    EXPECT_NE(json.find(drvPath), std::string::npos);

    StoreEscapeCheckingSink sink(state, {drvPath});
    std::ostream out(&sink);
    state.ensureLazyPathsCopied(context);
    out << json;

    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
}

TEST_F(TextStoreTest, xmlNamesExistingObjectsBeforeDelivery)
{
    auto drvPath = pathText(drvExpr("m") + ".drvPath");
    auto v = eval("{ d = " + drvExpr("m") + "; t = builtins.toFile \"m\" \"text\"; }");

    NixStringContext context;
    auto xml = renderValueAsXML(state, true, false, v, context, noPos);

    EXPECT_NE(xml.find(drvPath), std::string::npos);

    StoreEscapeCheckingSink sink(state, {drvPath});
    std::ostream out(&sink);
    state.ensureLazyPathsCopied(context);
    out << xml;

    EXPECT_NE(sink.bytes().find(drvPath), std::string::npos);
}

TEST_F(TextStoreTest, mountedReferencesPrecedeBothTextWriters)
{
    for (bool derivation : {false, true})
        for (bool large : {false, true}) {
            SCOPED_TRACE(fmt("derivation=%d large=%d", derivation, large));
            AutoDelete root{canonPath(createTempDir(), true)};
            auto local = make_ref<LocalStore>(make_ref<LocalStoreConfig>(root.path(), StoreConfig::Params{}));
            EvalState nested({}, local, fetchSettings, evalSettings);
            auto tree = make_ref<MemorySourceAccessor>();
            tree->addFile(CanonPath("file"), "mounted source");
            auto source = gitTreePath(*local, "mounted", merkle::objectHash(objectHashOf(*tree, CanonPath::root).root));
            nested.storeFS->mount(CanonPath(local->printStorePath(source)), tree);
            ASSERT_FALSE(local->isValidPath(source));
            auto expr = derivation ? "input: (derivation { name = \"mounted-drv\"; system = \"x86_64-linux\"; "
                                     "builder = \"/bin/sh\"; inherit input; }).drvPath"
                                   : "input: builtins.toFile \"mounted-text\" input";
            Value function, argument, value;
            nested.eval(nested.parseExprFromString(expr, nested.rootPath(CanonPath::root)), function);

            argument.mkString(
                std::string(large ? 32 * 1024 * 1024 : 5, 'x'),
                NixStringContext{NixStringContextElem::Opaque{.path = source}},
                nested.mem);
            nested.callFunction(function, argument, value, noPos);
            nested.forceValue(value, noPos);
            auto path = local->parseStorePath((value).string_view());
            ASSERT_TRUE(local->isValidPath(path));
            EXPECT_TRUE(local->isValidPath(source));
            EXPECT_EQ(local->queryPathInfo(path)->references, StorePathSet{source});
        }
}

TEST_F(TextStoreTest, validReferrersStillCheckTheirMountedSources)
{
    for (bool derivation : {false, true}) {
        SCOPED_TRACE(derivation);
        AutoDelete root{canonPath(createTempDir(), true)};
        auto local = make_ref<LocalStore>(make_ref<LocalStoreConfig>(root.path(), StoreConfig::Params{}));
        EvalState nested({}, local, fetchSettings, evalSettings);
        auto tree = make_ref<MemorySourceAccessor>();
        tree->addFile(CanonPath("file"), "before");
        auto source = gitTreePath(*local, "mounted", merkle::objectHash(objectHashOf(*tree, CanonPath::root).root));
        nested.storeFS->mount(CanonPath(local->printStorePath(source)), tree);
        auto expr = derivation ? "input: (derivation { name = \"valid-drv\"; system = \"x86_64-linux\"; "
                                 "builder = \"/bin/sh\"; inherit input; }).drvPath"
                               : "input: builtins.toFile \"valid-text\" input";
        Value function, argument;
        nested.eval(nested.parseExprFromString(expr, nested.rootPath(CanonPath::root)), function);
        argument.mkString("same text", NixStringContext{NixStringContextElem::Opaque{.path = source}}, nested.mem);
        auto evaluate = [&] {
            Value value;
            nested.callFunction(function, argument, value, noPos);
            nested.forceValue(value, noPos);
            return local->parseStorePath((value).string_view());
        };
        auto path = evaluate();
        ASSERT_TRUE(local->isValidPath(path));
        tree->addFile(CanonPath("file"), "after");
        try {
            evaluate();
            ADD_FAILURE() << "a valid referrer suppressed the changed mounted-source error";
        } catch (Error & error) {
            EXPECT_EQ(error.info().status, 102u);
            EXPECT_NE(error.message().find("contents have changed"), std::string::npos);
        }
        EXPECT_TRUE(local->isValidPath(path));
    }
}

TEST_F(TextStoreTest, unobservedObjectsRemainAndFailuresLeaveAReferenceClosedPrefix)
{
    auto text = pathText("builtins.toFile \"unobserved\" \"content\"");
    EXPECT_EQ(eval("builtins.seq " + drvExpr("unobserved") + ".drvPath \"plain\"").string_view(), "plain");
    EXPECT_THROW(eval("builtins.seq (builtins.toFile \"before-error\" \"written\") (throw \"stop\")"), Error);
    auto paths = local->queryAllValidPaths();
    EXPECT_GE(paths.size(), 2u);
    EXPECT_TRUE(paths.contains(local->parseStorePath(text)));
    for (const auto & path : paths)
        for (const auto & reference : local->queryPathInfo(path)->references)
            EXPECT_TRUE(paths.contains(reference));
}

TEST_F(TextStoreTest, readonlyComputesBothTextAddressesWithoutWritingOrRooting)
{
    auto saved = settings.readOnlyMode;
    Finally restore{[&] { settings.readOnlyMode = saved; }};
    settings.readOnlyMode = false;
    readOnlyMode = true;
    auto roots = local->rootCalls;
    auto text = pathText("builtins.toFile \"readonly\" \"content\"");
    auto drv = pathText(drvExpr("readonly") + ".drvPath");
    EXPECT_FALSE(local->isValidPath(local->parseStorePath(text)));
    EXPECT_FALSE(local->isValidPath(local->parseStorePath(drv)));
    EXPECT_TRUE(local->queryAllValidPaths().empty());
    EXPECT_EQ(local->uploads, 0u);
    EXPECT_EQ(local->rootCalls, roots);
}

TEST_F(TextStoreTest, repairRewritesMissingBytesAndValidReuseAvoidsUpload)
{
    for (bool derivation : {false, true}) {
        SCOPED_TRACE(derivation);
        auto expr = derivation ? drvExpr("repair") + ".drvPath" : "builtins.toFile \"repair\" \"original\"";
        auto path = local->parseStorePath(pathText(expr));
        auto original = readStoreFile(path);
        auto uploads = local->uploads;
        deletePath(local->toRealPath(path));
        ASSERT_TRUE(local->isValidPath(path));
        EXPECT_EQ(local->parseStorePath(pathText(expr)), path);
        EXPECT_FALSE(pathExists(local->toRealPath(path)));
        EXPECT_EQ(local->uploads, uploads);
        state.repair = Repair;
        EXPECT_EQ(local->parseStorePath(pathText(expr)), path);
        state.repair = NoRepair;
        EXPECT_EQ(readStoreFile(path), original);
        EXPECT_EQ(local->uploads, uploads + 1);
    }
}

TEST_F(TextStoreTest, publicationRootsExistingReferencesBeforeBackendCollection)
{
    auto referenced = addUnrooted("kept", "referenced source");
    auto dead = addUnrooted("dead", "unreferenced");
    local->collectBeforeWrite = true;
    auto path = local->addTextToStore("referrer", "references a live object", {referenced});
    EXPECT_TRUE(local->isValidPath(referenced));
    EXPECT_FALSE(local->isValidPath(dead));
    EXPECT_EQ(local->queryPathInfo(path)->references, StorePathSet{referenced});
}

TEST_F(TextStoreTest, anAlreadyValidReferrerRootsItsClosureWithoutUploading)
{
    auto referenced = addUnrooted("kept", "referenced source");
    auto dead = addUnrooted("dead", "unreferenced");
    // Register the referrer without a root, just as a prior process can leave it.
    const std::string contents = "referrer contents";
    auto info = ValidPathInfo::makeFromCA(
        *local,
        "referrer",
        TextInfo{.hash = hashString(HashAlgorithm::SHA256, contents), .references = {referenced}},
        ObjectHash::of(merkle::TreeEntry{.mode = merkle::Mode::Regular, .hash = merkle::blobId(contents)}));
    HashSink sink(HashAlgorithm::SHA256);
    dumpString(contents, sink);
    info.narSize = sink.finish().numBytesDigested;
    auto path = info.path;
    writeFile(local->toRealPath(path), contents);
    local->registerValidPaths({{path, std::move(info)}});
    auto uploads = local->uploads;
    EXPECT_EQ(local->addTextToStore("referrer", contents, {referenced}), path);
    EXPECT_EQ(local->uploads, uploads);
    GCOptions options;
    GCResults results;
    local->collectGarbage(options, results);
    EXPECT_TRUE(local->isValidPath(path));
    EXPECT_TRUE(local->isValidPath(referenced));
    EXPECT_FALSE(local->isValidPath(dead));
}

TEST_F(TextStoreTest, eagerWriteErrorsAreNotSuppressedAndRetryPreservesTheCompleteObjectSet)
{
    auto saved = settings.getWorkerSettings().keepGoing.get();
    Finally restore{[&] { settings.getWorkerSettings().keepGoing = saved; }};
    for (bool keepGoing : {false, true})
        for (auto repair : {NoRepair, Repair})
            for (bool chain : {false, true})
                for (size_t count : {1, 3})
                    for (size_t failure = 0; failure < count; ++failure) {
                        SCOPED_TRACE(
                            fmt("keepGoing=%d repair=%d chain=%d count=%d failure=%d",
                                keepGoing,
                                repair == Repair,
                                chain,
                                count,
                                failure));
                        settings.getWorkerSettings().keepGoing = keepGoing;
                        TextStoreRoot other;
                        EvalState nested({}, other.local, fetchSettings, evalSettings);
                        nested.repair = repair;
                        std::vector<StorePath> paths;
                        std::string expr = "let ", values;
                        for (size_t i = 0; i < count; ++i) {
                            StorePathSet refs;
                            if (chain && i)
                                refs.insert(paths.back());
                            auto name = fmt("object-%d", i), contents = fmt("contents-%d", i);
                            paths.push_back(other.local->makeFixedOutputPathFromCA(
                                name,
                                TextInfo{.hash = hashString(HashAlgorithm::SHA256, contents), .references = refs}));
                            auto input = fmt("\"%s\"", contents);
                            if (chain && i)
                                input = fmt("(builtins.appendContext %s (builtins.getContext v%d))", input, i - 1);
                            expr += fmt("v%d = builtins.toFile \"%s\" %s; ", i, name, input);
                            values += fmt(" v%d", i);
                        }
                        expr += "in builtins.deepSeq [" + values + "] true";
                        auto evaluate = [&] {
                            Value result;
                            nested.eval(nested.parseExprFromString(expr, nested.rootPath(CanonPath::root)), result);
                            nested.forceValue(result, noPos);
                            EXPECT_TRUE(result.boolean());
                        };
                        other.local->failingName = fmt("object-%d", failure);
                        try {
                            evaluate();
                            ADD_FAILURE() << "the backend write error was suppressed";
                        } catch (Error & error) {
                            EXPECT_NE(error.message().find("injected write failure"), std::string::npos);
                        }
                        auto survivors = other.local->queryAllValidPaths();
                        EXPECT_FALSE(survivors.contains(paths[failure]));
                        for (const auto & path : survivors)
                            for (const auto & reference : other.local->queryPathInfo(path)->references)
                                EXPECT_TRUE(survivors.contains(reference));
                        other.local->failingName.reset();
                        evaluate();
                        EXPECT_EQ(other.local->queryAllValidPaths(), StorePathSet(paths.begin(), paths.end()));
                        for (size_t i = 0; i < count; ++i) {
                            EXPECT_EQ(readFile(other.local->toRealPath(paths[i])), fmt("contents-%d", i));
                            StorePathSet refs;
                            if (chain && i)
                                refs.insert(paths[i - 1]);
                            EXPECT_EQ(other.local->queryPathInfo(paths[i])->references, refs);
                        }
                    }
}

// Compare producer expressions to materialized literals with the same context.
// Cover callable names outside the exclusion set and three selected input shapes.
// Reach each primitive's declared arity; lambdas retain the one/two-argument
// probes because their curried arity can depend on evaluation.
TEST_F(TextStoreTest, everyBuiltinAgreesOnProducedAndMaterializedInputs)
{
    struct Outcome
    {
        bool ok;
        std::string text;
        bool operator==(const Outcome &) const = default;
    };

    auto evaluate = [&](const std::string & expr) -> Outcome {
        try {
            auto value = eval(expr);
            state.forceValueDeep(value);
            std::ostringstream out;
            printValue(state, out, value, PrintOptions{.force = true, .derivationPaths = true});
            return {true, out.str()};
        } catch (Error & error) {
            // The input expressions deliberately have different syntax and
            // source positions. Compare the error itself, not those traces.
            const auto & info = error.info();
            return {
                false, fmt("%s: status=%u: %s", typeid(error).name(), info.status, filterANSIEscapes(info.msg.str()))};
        } catch (std::exception & error) {
            return {false, std::string(typeid(error).name()) + ": " + error.what()};
        }
    };
    const std::string text = "(builtins.toFile \"sweep-text\" \"hello\")";
    const std::string drv = drvExpr("sweep") + ".drvPath";
    auto textPath = pathText(text), drvPath = pathText(drv);
    auto materializedText = fmt("(builtins.appendContext \"%s\" { \"%s\" = { path = true; }; })", textPath, textPath);
    auto materializedDrv =
        fmt("(builtins.appendContext \"%s\" { \"%s\" = { allOutputs = true; }; })", drvPath, drvPath);
    auto attrs = [](const std::string & value) {
        return "{ path = " + value + "; url = " + value + "; name = \"sweep\"; type = \"path\"; }";
    };
    const std::vector<std::pair<std::string, std::string>> inputs{
        {text, materializedText}, {drv, materializedDrv}, {attrs(text), attrs(materializedText)}};
    const std::set<std::string> skipped{"currentTime", "exec", "break"};
    size_t calls = 0;
    for (auto & attr : *state.getBuiltins().attrs()) {
        std::string name(state.symbols[attr.name]);
        state.forceValue(*attr.value, noPos);
        if (attr.value->type() != nFunction || skipped.contains(name))
            continue;
        auto * primitive = attr.value;
        size_t supplied = 0;
        while (primitive->isPrimOpApp()) {
            ++supplied;
            primitive = primitive->primOpApp().left;
        }
        auto maxArity = primitive->isPrimOp() ? std::max<size_t>(2, primitive->primOp()->arity - supplied) : 2;
        for (const auto & [produced, materialized] : inputs)
            for (size_t arity = 1; arity <= maxArity; ++arity) {
                std::string lhs = "builtins." + name, rhs = lhs;
                for (size_t i = 0; i < arity; ++i) {
                    lhs += " " + produced;
                    rhs += " " + materialized;
                }
                SCOPED_TRACE(lhs);
                auto actual = evaluate(lhs), expected = evaluate(rhs);
                EXPECT_EQ(actual, expected) << actual.text << "\n" << expected.text;
                ++calls;
            }
    }
    EXPECT_GT(calls, 100u);
}

TEST_F(TextStoreTest, admittedReferencesSurviveNestedEagerWrites)
{
    for (bool explicitContext : {false, true}) {
        SCOPED_TRACE(explicitContext);
        auto suffix = explicitContext ? "context" : "path";
        auto reference = addUnrooted(fmt("reference-%s", suffix), "referenced source");
        auto dead = addUnrooted(fmt("dead-%s", suffix), "unreferenced");
        auto path = local->printStorePath(reference);
        auto admitted = explicitContext ? fmt("(builtins.appendContext \"ref\" { \"%s\" = { path = true; }; })", path)
                                        : fmt("(builtins.storePath \"%s\")", path);
        local->collectBeforeWrite = true;
        auto result = pathText(fmt(
            "builtins.toFile \"outer-%s\" (%s + (builtins.toFile \"inner-%s\" \"inner\"))", suffix, admitted, suffix));
        auto outer = local->parseStorePath(result);
        EXPECT_TRUE(local->isValidPath(reference));
        EXPECT_FALSE(local->isValidPath(dead));
        EXPECT_TRUE(local->queryPathInfo(outer)->references.contains(reference));
        for (const auto & ref : local->queryPathInfo(outer)->references)
            EXPECT_TRUE(local->isValidPath(ref));
    }
}

TEST_F(TextStoreTest, validatedContextKeysSurviveArbitraryFlagEvaluation)
{
    for (bool selected : {false, true}) {
        SCOPED_TRACE(selected);
        auto suffix = selected ? "selected" : "empty";
        auto reference = addUnrooted(fmt("flag-reference-%s", suffix), "validated source");
        auto dead = addUnrooted(fmt("flag-dead-%s", suffix), "unreferenced source");
        auto path = local->printStorePath(reference);
        local->collectBeforeWrite = true;
        auto result = eval(
            fmt("builtins.appendContext \"text\" { \"%s\" = { "
                "path = builtins.seq (builtins.toFile \"flag-inner-%s\" \"inner\") %s; "
                "allOutputs = false; outputs = []; }; }",
                path,
                suffix,
                selected ? "true" : "false"));
        EXPECT_EQ(bool(result.context()), selected);
        EXPECT_TRUE(local->isValidPath(reference));
        EXPECT_FALSE(local->isValidPath(dead));
    }
}

namespace {

struct AssertedPathSource : MemorySourceAccessor
{
    size_t reads = 0;

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        ++reads;
        MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
    }
};

} // namespace

/* A valid database row does not keep its file alive. The collector runs
   at a real validity observation, or after a warm-cache return, rather
   than checking the implementation's call sequence in a mock. */
TEST_F(TextStoreTest, assertedPathAddressesProtectWritableUseAndRespectRepairAndReadonly)
{
    auto savedMode = settings.readOnlyMode;
    auto savedCache = getEnv("NIX_CACHE_HOME");
    auto savedLsof = getEnv("_NIX_TEST_NO_LSOF");
    Finally restore{[&] {
        settings.readOnlyMode = savedMode;
        if (savedCache) setEnv("NIX_CACHE_HOME", savedCache->c_str()); else unsetenv("NIX_CACHE_HOME");
        if (savedLsof) setEnv("_NIX_TEST_NO_LSOF", savedLsof->c_str()); else unsetenv("_NIX_TEST_NO_LSOF");
    }};
    FreshCacheHome cache;
    setEnv("_NIX_TEST_NO_LSOF", "1");
    unsigned serial = 0;
    // Direct tree; NAR correspondence hit/miss; both assertions with
    // correspondence hit/miss; flat SHA256; NAR with references.
    for (unsigned route = 0; route < 7; ++route)
        for (unsigned contentsState = 0; contentsState < 3; ++contentsState)
            for (bool readonly : {false, true})
                for (bool repair : {false, true})
                    for (bool warm : {false, true}) {
                        SCOPED_TRACE(fmt("route=%d state=%d readonly=%d repair=%d warm=%d",
                                         route, contentsState, readonly, repair, warm));
                        settings.readOnlyMode = false;
                        auto name = fmt("asserted-%d", serial++);
                        auto contents = "good-" + name;
                        auto source = make_ref<AssertedPathSource>();
                        source->root = MemorySourceAccessor::File::Regular{false, contents};
                        auto treeHash = merkle::blobId(contents);
                        auto narHash = source->hashPath(CanonPath::root);
                        source->reads = 0;
                        bool flat = route == 5, referenced = route == 6;
                        StorePathSet refs;
                        std::string sourcePath = "/asserted-source-" + name;
                        if (referenced) {
                            refs.insert(addUnrooted(name + "-reference", "reference"));
                            sourcePath = local->printStorePath(addUnrooted(
                                name + "-source", contents, ContentAddressMethod::Raw::NixArchive, refs));
                        }
                        auto method = flat ? ContentAddressMethod::Raw::Flat
                                           : referenced ? ContentAddressMethod::Raw::NixArchive
                                                        : ContentAddressMethod::Raw::Git;
                        auto addressHash = flat ? hashString(HashAlgorithm::SHA256, contents)
                                                : referenced ? narHash : treeHash;
                        auto expected = local->makeFixedOutputPathFromCA(
                            name, ContentAddressWithReferences::fromParts(method, addressHash, {.others = refs}));
                        if (contentsState) {
                            ASSERT_EQ(addUnrooted(name, contents, method, refs), expected);
                            if (contentsState == 2)
                                writeFile(local->toRealPath(expected), "bad-" + name);
                        }
                        if (!warm) local->invalidatePathInfoCacheFor(expected);
                        else {
                            try { local->queryPathInfo(expected); }
                            catch (InvalidPath &) { }
                        }
                        fetchers::Settings localFetch;
                        if (route == 1 || route == 3)
                            recordTreeAddress(localFetch, narHash, treeHash);
                        readOnlyMode = settings.readOnlyMode = readonly;
                        EvalState nested({}, local, localFetch, evalSettings);
                        nested.repair = repair ? Repair : NoRepair;
                        if (!referenced)
                            nested.storeFS->mount(CanonPath(sourcePath), source);
                        std::string assertions;
                        if (route == 0 || route == 3 || route == 4)
                            assertions += fmt("treeHash = \"%s\";", treeHash.to_string(HashFormat::SRI, true));
                        if (route != 0)
                            assertions += fmt("sha256 = \"%s\";", (flat ? addressHash : narHash).to_string(HashFormat::SRI, true));
                        auto pathExpr = referenced ? fmt("builtins.storePath \"%s\"", sourcePath) : sourcePath;
                        auto expression = fmt("builtins.path { name = \"%s\"; path = %s; recursive = %s; %s }",
                                              name, pathExpr, flat ? "false" : "true", assertions);
                        auto rootCalls = local->rootCalls;
                        auto collections = local->validityCollections;
                        if (!readonly && !repair && !warm && route != 2)
                            local->collectBeforeValidity = expected;
                        Value result;
                        bool rejectsCorruptNAR = contentsState == 2 && !repair && route == 4;
                        try {
                            nested.eval(nested.parseExprFromString(expression, nested.rootPath(CanonPath::root)), result);
                            if (rejectsCorruptNAR) {
                                ADD_FAILURE() << "corrupt stored NAR satisfied both assertions";
                            } else {
                                EXPECT_EQ(result.string_view(), local->printStorePath(expected));
                            }
                        } catch (Error & error) {
                            if (rejectsCorruptNAR) {
                                EXPECT_EQ(error.info().status, 102u);
                            } else {
                                throw;
                            }
                        }
                        local->collectBeforeValidity.reset();
                        if (!readonly && !repair && !warm && route != 2) {
                            EXPECT_EQ(local->validityCollections, collections + 1);
                        }
                        if (readonly) {
                            EXPECT_EQ(local->rootCalls, rootCalls);
                        } else {
                            EXPECT_GT(local->rootCalls, rootCalls);
                        }
                        bool stored = contentsState || !readonly;
                        EXPECT_EQ(pathExists(local->toRealPath(expected)), stored);
                        if (stored) {
                            auto expectedBytes = contentsState == 2 && (readonly || !repair) ? "bad-" + name : contents;
                            EXPECT_EQ(readFile(local->toRealPath(expected)), expectedBytes);
                        }
                        if (!referenced) {
                            bool reobservesSource = repair || !contentsState || route == 2;
                            EXPECT_EQ(source->reads != 0, reobservesSource);
                        }
                        GCOptions options;
                        options.pathsToDelete = GCOptions::SpecificPaths{.paths = {expected}};
                        GCResults gc;
                        local->collectGarbage(options, gc);
                        EXPECT_EQ(pathExists(local->toRealPath(expected)), !readonly && stored);
                    }
}

TEST_F(TextStoreTest, assertedPathRepairAndReadonlyStillRejectBothHashMismatches)
{
    auto savedMode = settings.readOnlyMode;
    auto savedCache = getEnv("NIX_CACHE_HOME");
    Finally restore{[&] {
        settings.readOnlyMode = savedMode;
        if (savedCache) setEnv("NIX_CACHE_HOME", savedCache->c_str()); else unsetenv("NIX_CACHE_HOME");
    }};
    FreshCacheHome cache;
    fetchers::Settings localFetch;
    auto source = make_ref<AssertedPathSource>();
    source->root = MemorySourceAccessor::File::Regular{false, "good"};
    auto treeHash = merkle::blobId("good");
    auto narHash = source->hashPath(CanonPath::root);
    auto wrongTree = merkle::blobId("wrong");
    auto wrongNar = hashString(HashAlgorithm::SHA256, "wrong");
    for (bool readonly : {false, true})
        for (bool repair : {false, true})
            for (bool wrongTreeAssertion : {false, true}) {
                SCOPED_TRACE(fmt("readonly=%d repair=%d wrongTree=%d", readonly, repair, wrongTreeAssertion));
                settings.readOnlyMode = false;
                auto name = fmt("mismatch-%d-%d-%d", readonly, repair, wrongTreeAssertion);
                addUnrooted(name, "good", ContentAddressMethod::Raw::Git);
                readOnlyMode = settings.readOnlyMode = readonly;
                EvalState nested({}, local, localFetch, evalSettings);
                nested.repair = repair ? Repair : NoRepair;
                nested.storeFS->mount(CanonPath("/asserted-mismatch-source"), source);
                auto expression = fmt(
                    "builtins.path { name = \"%s\"; path = /asserted-mismatch-source; treeHash = \"%s\"; sha256 = \"%s\"; }",
                    name,
                    (wrongTreeAssertion ? wrongTree : treeHash).to_string(HashFormat::SRI, true),
                    (wrongTreeAssertion ? narHash : wrongNar).to_string(HashFormat::SRI, true));
                Value result;
                try {
                    nested.eval(nested.parseExprFromString(expression, nested.rootPath(CanonPath::root)), result);
                    ADD_FAILURE() << "invalid assertion was accepted";
                } catch (Error & error) {
                    EXPECT_EQ(error.info().status, 102u);
                }
            }
}

} // namespace nix

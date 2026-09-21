/**
 * The naming law for composite source accessors (doc/lazy-store/01-specification.md,
 * section 8): if two subtrees have names and the names are equal, the trees
 * are equal, hence their NARs are.  Checked here over random terms built
 * from the union and graft operations on leaves that name every subtree by
 * the hash of its NAR, as a git leaf names subtrees by object identifier.
 */
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/serialise.hh"
#include "nix/util/hash.hh"

#include "nix/util/tests/source-accessor-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <map>

#include <nlohmann/json.hpp>

namespace nix::source_accessor_naming {

using File = MemorySourceAccessor::File;

/**
 * A term of the accessor algebra: the accessor it denotes and a description
 * for counterexamples.
 */
struct Term
{
    ref<SourceAccessor> accessor;
    std::string desc;
};

inline std::ostream & operator<<(std::ostream & os, const Term & t)
{
    return os << t.desc;
}

/* Few names and few contents, so that random terms share subtrees and
   `genPath` (below) lands on existing entries. */
FileGenSpec fewNamesSpec()
{
    return {
        .names = {"a", "b", "c"},
        .contents = rc::gen::element(std::string(""), std::string("x"), std::string("yy")),
        .targets = rc::gen::element(std::string("t1"), std::string("t2")),
    };
}

/* A leaf whose root is a directory, so that grafts at paths below the root
   satisfy the graft precondition where the path is absent. */
rc::Gen<Term> genLeaf()
{
    return rc::gen::map(
        rc::gen::tuple(rc::gen::maybe(genFile(1, fewNamesSpec())), rc::gen::maybe(genFile(1, fewNamesSpec()))),
        [](auto t) {
            auto accessor = make_ref<MemorySourceAccessor>();
            File::Directory d;
            auto & [a, b] = t;
            if (a)
                d.entries.emplace("a", *a);
            if (b)
                d.entries.emplace("b", *b);
            accessor->root = File{std::move(d)};
            return Term{
                .accessor = make_ref<ContentNamedAccessor>(accessor),
                .desc = "leaf" + static_cast<nlohmann::json>(*accessor).dump(),
            };
        });
}

rc::Gen<CanonPath> genPath()
{
    return rc::gen::map(
        rc::gen::pair(
            rc::gen::element(std::string("a"), std::string("b"), std::string("m")),
            rc::gen::maybe(rc::gen::element(std::string("a"), std::string("n")))),
        [](std::pair<std::string, rc::Maybe<std::string>> p) {
            auto path = CanonPath::root / p.first;
            if (p.second)
                path.push(*p.second);
            return path;
        });
}

/* Terms over three of the four operations: leaves, the general union, and
   graft.  Subtree is exercised because the leaf names every subtree, not
   only its root, and `collectPaths` visits every path.  Filter is NOT here:
   it has its own naming rule (the inner name together with the accepted
   set), the filtering accessors live in libfetchers, and the rule is
   property-tested there (`src/libfetchers-tests/fetch-to-store.cc`,
   `prop_filtered_names_are_sound`).  The coherent union is a separate,
   targeted test below, since a random pair is almost never coherent. */
rc::Gen<Term> genTerm(int depth)
{
    if (depth == 0)
        return genLeaf();
    auto sub = genTerm(depth - 1);
    auto unionTerm = rc::gen::map(rc::gen::pair(sub, sub), [](std::pair<Term, Term> p) {
        return Term{
            .accessor = makeUnionSourceAccessor({p.first.accessor, p.second.accessor}),
            .desc = "union(" + p.first.desc + ", " + p.second.desc + ")",
        };
    });
    auto graftTerm = rc::gen::map(rc::gen::tuple(sub, genPath(), sub), [](std::tuple<Term, CanonPath, Term> t) {
        auto & [base, path, mounted] = t;
        /* The graft precondition: every proper ancestor of the mount point
           is a directory or absent in the base. */
        auto p = path;
        while (!p.isRoot()) {
            p.pop();
            if (auto st = base.accessor->maybeLstat(p); st && st->type != SourceAccessor::tDirectory)
                return base;
        }
        return Term{
            .accessor = makeMountedSourceAccessor({{CanonPath::root, base.accessor}, {path, mounted.accessor}}),
            .desc = "graft(" + base.desc + ", " + path.abs() + ", " + mounted.desc + ")",
        };
    });
    return rc::gen::oneOf(genLeaf(), unionTerm, graftTerm);
}

void collectPaths(SourceAccessor & accessor, const CanonPath & path, std::vector<CanonPath> & out)
{
    out.push_back(path);
    if (accessor.lstat(path).type == SourceAccessor::tDirectory)
        for (auto & [name, _] : accessor.readDirectory(path))
            collectPaths(accessor, path / name, out);
}

} // namespace nix::source_accessor_naming

namespace nix {

using namespace nix::source_accessor_naming;

/* Equal names imply equal trees, across two random terms and within each. */
RC_GTEST_PROP(SourceAccessorNaming, equalNamesImplyEqualTrees, ())
{
    auto a = *genTerm(2);
    auto b = *genTerm(2);

    std::map<std::pair<std::string, std::string>, std::string> seen;
    for (auto & term : {a, b}) {
        std::vector<CanonPath> paths;
        collectPaths(*term.accessor, CanonPath::root, paths);
        for (auto & path : paths) {
            try {
                auto [subpath, name] = term.accessor->getFingerprint(path);
                if (!name)
                    continue;
                auto key = std::pair{*name, subpath.abs()};
                auto contents = narOf(*term.accessor, path);
                auto [it, inserted] = seen.emplace(key, contents);
                if (!inserted)
                    RC_ASSERT(it->second == contents);
            } catch (Error & e) {
                RC_FAIL("at path " + path.abs() + " of " + term.desc + ": " + e.what());
            }
        }
    }
}

/* The coherent union (`ChildrenAgree`) names a subtree by the first child
   that has a name for it, which is sound only because the children agree
   wherever both have a node: the rule the evaluation root relies on
   (01-specification.md section 8.4).  A random pair is almost never
   coherent, so this is a constructed pair that shares a subtree and differs
   elsewhere; the assertion is the same homomorphism, plus that the name is
   the first child's. */
RC_GTEST_PROP(SourceAccessorNaming, coherentUnionNamesByTheFirstChildAndStaysSound, ())
{
    /* Two trees that agree on their overlap: both hold the same `shared`,
       and each holds a private entry. */
    auto shared = *genFile(2, fewNamesSpec());
    auto makeChild = [&](std::string priv) {
        auto mem = make_ref<MemorySourceAccessor>();
        mem->open(CanonPath("shared"), shared);
        mem->addFile(CanonPath(priv), priv + " contents");
        return make_ref<ContentNamedAccessor>(mem).cast<SourceAccessor>();
    };
    auto first = makeChild("a");
    auto second = makeChild("b");
    auto coherent = makeUnionSourceAccessor({first, second}, UnionCoherence::ChildrenAgree);

    /* At the shared subtree the union's name is the first child's, and both
       children name it the same (they agree), so the homomorphism holds. */
    auto atShared = coherent->getFingerprint(CanonPath("shared"));
    RC_ASSERT(atShared == first->getFingerprint(CanonPath("shared")));
    RC_ASSERT(atShared.second == second->getFingerprint(CanonPath("shared")).second);

    /* And equal names imply equal NARs across every path of the union. */
    std::vector<CanonPath> paths;
    collectPaths(*coherent, CanonPath::root, paths);
    std::map<std::pair<std::string, std::string>, std::string> seen;
    for (auto & path : paths) {
        auto [subpath, name] = coherent->getFingerprint(path);
        if (!name)
            continue;
        auto contents = narOf(*coherent, path);
        auto [it, inserted] = seen.emplace(std::pair{*name, subpath.abs()}, contents);
        if (!inserted)
            RC_ASSERT(it->second == contents);
    }
}

} // namespace nix

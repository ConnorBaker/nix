#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/print.hh"
#include "nix/util/signals.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"

namespace nix {

/* PROTOTYPE: additions to `seen`, in order, so that an unforced child's can
   be forgotten. */
static thread_local std::vector<const void *> seenAdded;

static bool markSeen(std::set<const void *> & seen, const void * p, bool unforce)
{
    if (!seen.insert(p).second)
        return false;
    if (unforce)
        seenAdded.push_back(p);
    return true;
}

// See: https://github.com/NixOS/nix/issues/9730
void printAmbiguous(
    EvalState & state,
    Value & v,
    std::ostream & str,
    std::set<const void *> * seen,
    NixStringContext * context,
    size_t depth,
    bool force,
    bool unforce)
{
    checkInterrupt();

    if (depth > state.settings.maxCallDepth)
        state.error<StackOverflowError>().atPos(v.determinePos(noPos)).debugThrow();

    /* PROTOTYPE: forcing while printing (`force`) is what `--strict` gets
       from `forceValueDeep` beforehand, but lets each element be unforced
       once printed (`unforce`). */
    if (force)
        state.forceValue(v, v.determinePos(noPos));

    /* With `unforce`, the children of an attribute set or list are unforced
       in batches. When a batch frees what they allocated, the addresses they
       added to `seen` are forgotten, since they may be reused; a batch that
       frees nothing (nested in another one) leaves them for the enclosing
       batch. */
    size_t seenMark = 0;
    UnforceBatch batch(state, unforce);
    if (batch.enabled())
        batch.onEnd = [&](bool freed) {
            if (freed)
                for (; seenAdded.size() > seenMark; seenAdded.pop_back())
                    if (seen && unforceMayHaveFreed(seenAdded.back()))
                        seen->erase(seenAdded.back());
        };
    auto child = [&](Value & c, std::set<const void *> * childSeen) {
        if (batch.enabled() && !batch.isOpen())
            seenMark = seenAdded.size();
        UnforceBatch::Element element(batch);
        printAmbiguous(state, c, str, childSeen, context, depth + 1, force, unforce);
    };
    switch (v.type()) {
    case nInt:
        str << v.integer();
        break;
    case nBool:
        printLiteralBool(str, v.boolean());
        break;
    case nString:
        printLiteralString(str, v.string_view());
        if (context)
            copyContext(v, *context);
        break;
    case nPath:
        str << v.path().to_string(); // !!! escaping?
        break;
    case nNull:
        str << "null";
        break;
    case nAttrs: {
        if (seen && !v.attrs()->empty() && !markSeen(*seen, v.attrs(), unforce))
            str << "«repeated»";
        else {
            str << "{ ";
            for (auto & i : v.attrs()->lexicographicOrder(state.symbols)) {
                str << state.symbols[i->name] << " = ";
                if (force) {
                    try {
                        child(*i->value, seen);
                    } catch (Error & e) {
                        state.addErrorTrace(e, i->pos, "while evaluating the attribute '%1%'", state.symbols[i->name]);
                        throw;
                    }
                } else
                    child(*i->value, seen);
                str << "; ";
            }
            str << "}";
        }
        break;
    }
    case nList:
        /* Use pointer to the Value instead of pointer to the elements, because
           that would need to explicitly handle the case of SmallList. */
        if (seen && v.listSize() && !markSeen(*seen, &v, unforce))
            str << "«repeated»";
        else {
            str << "[ ";
            for (auto v2 : v.listView()) {
                if (v2)
                    child(*v2, seen);
                else
                    str << "(nullptr)";
                str << " ";
            }
            str << "]";
        }
        break;
    case nThunk:
        if (!v.isBlackhole()) {
            str << "<CODE>";
        } else {
            // Although we know for sure that it's going to be an infinite recursion
            // when this value is accessed _in the current context_, it's likely
            // that the user will misinterpret a simpler «infinite recursion» output
            // as a definitive statement about the value, while in fact it may be
            // a valid value after `builtins.trace` and perhaps some other steps
            // have completed.
            str << "«potential infinite recursion»";
        }
        break;
    case nFailed:
        // Historically, a tried and then ignored value (e.g. through tryEval) was
        // reverted to the original thunk.
        str << "<CODE>";
        break;
    case nFunction:
        if (v.isLambda()) {
            str << "<LAMBDA>";
        } else if (v.isPrimOp()) {
            str << "<PRIMOP>";
        } else if (v.isPrimOpApp()) {
            str << "<PRIMOP-APP>";
        }
        break;
    case nExternal:
        str << *v.external();
        break;
    case nFloat:
        str << v.fpoint();
        break;
    default:
        printError("Nix evaluator internal error: printAmbiguous: invalid value type");
        unreachable();
    }
}

} // namespace nix

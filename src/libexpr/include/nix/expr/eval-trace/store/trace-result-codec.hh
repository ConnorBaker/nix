#pragma once
/// store/trace-result-codec.hh — Pure encode/decode for CachedResult.
///
/// Encodes a CachedResult to an EncodedResultPayload (persistence
/// wire format) and decodes a ResultPayload back into a CachedResult.
/// Symbol and vocab refs are threaded explicitly so neither function
/// needs a live TraceStore.
///
/// Step 5 of rearchitecture-proposal.md §14.

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/expr/eval-trace/store/trace-value-types.hh"

namespace nix {
class SymbolTable;
}

namespace nix::eval_trace {

/// Encode a CachedResult as an EncodedResultPayload. `vocab` is only
/// used for the FullAttrs variant (which maps symbols through the
/// attr-name vocabulary); other variants ignore it.
[[nodiscard]] EncodedResultPayload encodeCachedResult(const CachedResult & value, AttrVocabStore & vocab);

/// Decode a ResultPayload into a CachedResult. `vocab` + `symbols` are
/// used to re-resolve attr-name IDs into the current SymbolTable for
/// FullAttrs; other variants ignore them.
[[nodiscard]] CachedResult decodeCachedResult(const ResultPayload & payload, AttrVocabStore & vocab, SymbolTable & symbols);

} // namespace nix::eval_trace

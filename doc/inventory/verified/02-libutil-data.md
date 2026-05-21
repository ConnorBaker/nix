# Inventory — Shard 02: libutil data/encoding

## File: src/libutil/hash.cc

### Namespaces
- `nix` — implementation namespace for hashing types/free fns.
- `nix::{anonymous}` — anonymous namespace housing `DecodeNamePair` helper struct.
- `nlohmann` — adds `adl_serializer<Hash>` JSON glue.

### Classes / structs / enums
- `struct DecodeNamePair` — anon-namespace helper bundling `decltype(base16::decode) * decode` function pointer and `std::string_view encodingName` for error messages.
- `union Hash::Ctx` — out-of-line definition of the hash-context union (members: `blake3_hasher blake3`, `MD5_CTX md5`, `SHA_CTX sha1`, `SHA256_CTX sha256`, `SHA512_CTX sha512`).

### Functions
- `Hash::Hash(HashAlgorithm, const ExperimentalFeatureSettings &)` — constructor; gates BLAKE3 behind `Xp::BLAKE3Hashes`, sets `hashSize = regularHashSize(algo)`, asserts `hashSize <= maxHashSize`, `memset(hash, 0, maxHashSize)`.
- `bool Hash::operator==(const Hash &) const noexcept` — byte-wise hash equality (size-checked first).
- `std::strong_ordering Hash::operator<=>(const Hash &) const noexcept` — three-way compare on `hashSize`, then bytes, then `algo`.
- `std::string Hash::to_string(HashFormat, bool includeAlgo) const` — render hash in chosen encoding with optional algorithm prefix; SRI uses `<algo>-` separator, others use `<algo>:`.
- `static DecodeNamePair baseExplicit(HashFormat)` — map a non-SRI `HashFormat` to its decoder + display name; `unreachable()` for SRI.
- `static HashFormat baseFromSize(std::string_view rest, HashAlgorithm algo)` — infer encoding from encoded length; throws `BadHash` on mismatch.
- `static Hash parseLowLevel(std::string_view, HashAlgorithm, DecodeNamePair, const ExperimentalFeatureSettings &)` — common decode path used by all `parse*` entry points; default xpSettings = `experimentalFeatureSettings`.
- `static Hash Hash::parseSRI(std::string_view, const ExperimentalFeatureSettings &)` — parse `<algo>-<base64>` SRI form using `splitPrefixTo` on `'-'`.
- `static std::pair<Hash, HashFormat> parseAnyHelper(std::string_view rest, auto resolveAlgo)` — generic parse helper templated over how the algorithm is resolved; tries `:` then `-` separator, infers SRI from `-`.
- `static Hash Hash::parseAnyPrefixed(std::string_view)` — parse requiring a `<algo>:` or `<algo>-` prefix.
- `static Hash Hash::parseAny(std::string_view, std::optional<HashAlgorithm>)` — parse with optional explicit algo.
- `static std::pair<Hash, HashFormat> Hash::parseAnyReturningFormat(std::string_view, std::optional<HashAlgorithm>)` — like `parseAny` but also returns inferred format.
- `static Hash Hash::parseNonSRIUnprefixed(std::string_view, HashAlgorithm)` — parse non-SRI string with externally supplied algo (delegates to `parseExplicitFormatUnprefixed` after `baseFromSize`).
- `static Hash Hash::parseExplicitFormatUnprefixed(std::string_view, HashAlgorithm, HashFormat, const ExperimentalFeatureSettings &)` — parse with explicit algo and format.
- `static Hash Hash::random(HashAlgorithm)` — fill `hash` with `randombytes_buf` (libsodium).
- `Hash newHashAllowEmpty(std::string_view, std::optional<HashAlgorithm>)` — accept empty string as zero hash, warns; otherwise delegates to `Hash::parseAny`.
- `static void start(HashAlgorithm, Hash::Ctx &)` — dispatcher selecting `*_Init` / `blake3_hasher_init`.
- `void blake3_hasher_update_with_heuristics(blake3_hasher *, std::string_view)` — picks `blake3_hasher_update_tbb` vs `blake3_hasher_update` based on `blake3TbbThreshold`; only enabled with `BLAKE3_USE_TBB`.
- `static void update(HashAlgorithm, Hash::Ctx &, std::string_view)` — dispatcher selecting `*_Update`.
- `static void finish(HashAlgorithm, Hash::Ctx &, unsigned char *)` — dispatcher selecting `*_Final` / `blake3_hasher_finalize`.
- `Hash hashString(HashAlgorithm, std::string_view, const ExperimentalFeatureSettings &)` — hash an in-memory string via local `Hash::Ctx`.
- `Hash hashFile(HashAlgorithm, const std::filesystem::path &)` — read file into a `HashSink` via `readFile`.
- `HashSink::HashSink(HashAlgorithm)` — allocates a `Hash::Ctx` (heap), zeros bytes, calls `start`.
- `HashSink::~HashSink()` — sets `bufPos = 0` (skip flush) and `delete ctx`.
- `void HashSink::writeUnbuffered(std::string_view) override` — feed bytes through `update`, increment `bytes`.
- `HashResult HashSink::finish() override` — flush + final, returns `HashResult{hash, bytes}`.
- `HashResult HashSink::currentHash()` — clone ctx via copy, finalize without consuming.
- `Hash compressHash(const Hash &, unsigned int newSize)` — XOR-fold into a shorter digest (preserves algo).
- `std::optional<HashFormat> parseHashFormatOpt(std::string_view)` — names → `HashFormat`, with `base32` deprecation warning.
- `HashFormat parseHashFormat(std::string_view)` — throwing wrapper, throws `UsageError`.
- `std::string_view printHashFormat(HashFormat)` — reverse of parseHashFormat; asserts on illegal value.
- `std::optional<HashAlgorithm> parseHashAlgoOpt(std::string_view, const ExperimentalFeatureSettings &)` — names → `HashAlgorithm`, gates BLAKE3 via xpSettings.
- `HashAlgorithm parseHashAlgo(std::string_view, const ExperimentalFeatureSettings &)` — throwing wrapper, throws `UsageError`.
- `std::string_view printHashAlgo(HashAlgorithm)` — reverse of parseHashAlgo; asserts on illegal value.
- `Hash adl_serializer<Hash>::from_json(const json &, const ExperimentalFeatureSettings &)` — JSON in via SRI string (uses `getString` then `Hash::parseSRI`).
- `void adl_serializer<Hash>::to_json(json &, const Hash &)` — JSON out as SRI string with algo.

### Type aliases
- (None at namespace scope here.)

### Macros / globals
- `const StringSet hashAlgorithms` — `{"blake3", "md5", "sha1", "sha256", "sha512"}`.
- `const StringSet hashFormats` — `{"base64", "nix32", "base16", "sri"}`.
- `Hash Hash::dummy(HashAlgorithm::SHA256)` — static dummy SHA256 hash (zero-filled).
- `const size_t blake3TbbThreshold = 128000` — input-size threshold above which TBB-parallel BLAKE3 is used.

## File: src/libutil/include/nix/util/hash.hh

### Namespaces
- `nix` — primary declarations.
- `std` — explicit specialization of `std::hash<nix::Hash>`.
- `nlohmann` (via `JSON_IMPL_WITH_XP_FEATURES` macro) — opens for `adl_serializer<Hash>` specialization.

### Classes / structs / enums
- `enum struct HashAlgorithm : char` — values `MD5 = 42, SHA1, SHA256, SHA512, BLAKE3` (intentional `42` to "fight bug-compatibility with old versions of Nix").
- `enum struct HashFormat : int` — `Base64, Nix32, Base16, SRI`.
- `struct Hash` — holds `size_t hashSize`, `uint8_t hash[maxHashSize]`, `HashAlgorithm algo`; declares forward `union Ctx`; `constexpr static size_t maxHashSize = 64`; static `dummy`; numerous parse static methods, `to_string`, `gitRev`, `gitShortRev`, comparison operators, `random`.
- `struct HashResult` — `{ Hash hash; uint64_t numBytesDigested; }`.
- `struct AbstractHashSink : virtual Sink` — abstract base with `virtual HashResult finish() = 0`.
- `class HashSink : public BufferedSink, public AbstractHashSink` — buffered hashing sink with private `HashAlgorithm ha`, `Hash::Ctx * ctx`, `uint64_t bytes`; ctor + copy-ctor + dtor + `writeUnbuffered`, `finish`, `currentHash`.
- `template<> struct json_avoids_null<Hash> : std::true_type` — opt-in for null-safe `optional<Hash>`.
- `template<> struct std::hash<nix::Hash>` — provides `operator()` taking first sizeof(size_t) bytes via reinterpret_cast (asserts `hashSize > sizeof(size_t)`).

### Functions
- `MakeError(BadHash, Error)` — declares `class BadHash : public Error` via macro.
- `constexpr inline size_t regularHashSize(HashAlgorithm)` — switch returning 32/16/20/32/64 (BLAKE3/MD5/SHA1/SHA256/SHA512); asserts on default.
- `Hash newHashAllowEmpty(std::string_view, std::optional<HashAlgorithm>)`.
- `Hash hashString(HashAlgorithm, std::string_view, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `Hash hashFile(HashAlgorithm, const std::filesystem::path &)`.
- `Hash compressHash(const Hash &, unsigned int)`.
- `HashFormat parseHashFormat(std::string_view)`, `std::optional<HashFormat> parseHashFormatOpt(std::string_view)`, `std::string_view printHashFormat(HashFormat)`.
- `HashAlgorithm parseHashAlgo(std::string_view, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`, `std::optional<HashAlgorithm> parseHashAlgoOpt(std::string_view, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`, `std::string_view printHashAlgo(HashAlgorithm)`.
- `inline std::size_t nix::hash_value(const Hash &)` — Boost-style ADL hash that delegates to `std::hash<Hash>`.
- `Hash::gitRev() const` (inline) — base16, no algo prefix; `[[nodiscard]]`.
- `Hash::gitShortRev() const` (inline) — first 7 chars of base16; `[[nodiscard]]`.

### Type aliases
- (None new at namespace scope.)

### Macros / globals
- `extern const StringSet hashAlgorithms`, `extern const StringSet hashFormats`.
- `JSON_IMPL_WITH_XP_FEATURES(Hash)` — emits the `nlohmann::adl_serializer<Hash>` specialization with `xpSettings` parameter (defined in hash.cc).

## File: src/libutil/base-n.cc

### Namespaces
- `nix` — implementation.

### Classes / structs / enums
- (None.)

### Functions
- `std::string base16::encode(std::span<const std::byte>)` — hex encode (lowercase nibbles) using `base16Chars`.
- `std::string base16::decode(std::string_view)` — hex decode with case-insensitive `parseHexDigit` lambda; asserts even length.
- `std::string base64::encode(std::span<const std::byte>)` — RFC-4648 base64 with `=` padding to multiple of 4.
- `std::string base64::decode(std::string_view)` — base64 decode using a constexpr 256-entry inverse table built from `base64Chars`; ignores `\n`, stops on `=`.

### Type aliases
- (None.)

### Macros / globals
- `constexpr static const std::array<char, 16> base16Chars` — `"0123456789abcdef"` (built via `_arrayNoNull` literal).
- `constexpr static const std::array<char, 64> base64Chars` — full RFC base64 alphabet `A-Za-z0-9+/`.

## File: src/libutil/include/nix/util/base-n.hh

### Namespaces
- `nix`
- `nix::base16`
- `nix::base64`

### Classes / structs / enums
- (None — both encodings exposed as namespaces with free functions.)

### Functions
- `[[nodiscard]] constexpr static inline size_t base16::encodedLength(size_t origSize)` — `origSize * 2`.
- `std::string base16::encode(std::span<const std::byte>)`.
- `std::string base16::decode(std::string_view)`.
- `[[nodiscard]] constexpr static inline size_t base64::encodedLength(size_t origSize)` — `((4 * origSize / 3) + 3) & ~3`.
- `std::string base64::encode(std::span<const std::byte>)`.
- `std::string base64::decode(std::string_view)`.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/base-nix-32.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (Out-of-line member of `BaseNix32` only.)

### Functions
- `std::string BaseNix32::encode(std::span<const std::byte>)` — Nix's omitted-letters base32, MSB-first reverse iteration (writes `len-1..0` so encoded digits descend); empty input returns `{}`.
- `std::string BaseNix32::decode(std::string_view)` — inverse with growing `res` buffer via `resize`; throws `FormatError` on invalid char via `lookupReverse`.

### Type aliases / macros / globals
- `constexpr const std::array<unsigned char, 256> BaseNix32::reverseMap` — out-of-line definition of the inverse-lookup table; built at constant-eval time from `characters` and the `invalid` sentinel.

## File: src/libutil/include/nix/util/base-nix-32.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct BaseNix32` — value-namespace style struct with only static members:
  - `static constexpr std::array<char, 32> characters` = `"0123456789abcdfghijklmnpqrsvwxyz"_arrayNoNull` (omitted: E O U T).
  - `static const std::array<uint8_t, 256> reverseMap` (private).
  - `static constexpr const uint8_t invalid = 0xFF` (private).
  - `static inline std::optional<uint8_t> lookupReverse(char)`.
  - `[[nodiscard]] static constexpr inline size_t encodedLength(size_t)` — `(originalLength * 8 - 1) / 5 + 1`.
  - `static std::string encode(std::span<const std::byte>)`.
  - `static std::string decode(std::string_view)`.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/strings.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (None defined; uses local lambdas inside `shellSplitString`.)

### Functions
- Explicit-instantiation declarations (no new logic):
  - `tokenizeString` for `std::list<std::string>`, `StringSet`, `std::vector<std::string>`.
  - `splitString` for `std::list<std::string>`, `StringSet`, `std::vector<std::string>`.
  - `basicSplitString<std::list<OsString>, OsChar>` (Windows wide-char support).
  - `concatStringsSep` for `std::list<std::string>`, `StringSet`, `std::vector<std::string>`, `boost::container::small_vector<std::string,64>`.
  - `concatStringsSep` for fixed-size `std::string_view[2|3|4]` C arrays (via local typedefs `strings_2`, `strings_3`, `strings_4`).
  - `dropEmptyInitThenConcatStringsSep` for `std::list<std::string>`, `StringSet`, `std::vector<std::string>`.
- `std::list<std::string> shellSplitString(std::string_view)` — POSIX-shell-style splitter with quote/escape handling; uses local `pushCurrent`, `pushChar`, `pop`, `inDoubleQuotes`, `inSingleQuotes` lambdas.
- `std::string optionalBracket(std::string_view prefix, std::string_view content, std::string_view suffix)` — wrap content with brackets only when non-empty.
- `const char * requireCString(const std::string &)` — assert no embedded NULs and return `c_str()`; throws with substituted `␀` glyph on violation via `replaceStrings`.

### Type aliases
- `typedef std::string_view strings_2[2]` (file-local).
- `typedef std::string_view strings_3[3]` (file-local).
- `typedef std::string_view strings_4[4]` (file-local).

### Macros / globals
- (None; all template instantiations.)

## File: src/libutil/include/nix/util/strings.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct StringViewHash` — heterogeneous (transparent) hash functor for unordered containers; private `using HashType = std::hash<std::string_view>`; public `using is_transparent = void` and three `operator()` overloads (C-string, string_view, string).

### Functions (templates and free fns)
- `template<class C, class CharT=char> C basicTokenizeString(std::basic_string_view<CharT>, std::basic_string_view<CharT>)`.
- `template<class C> C tokenizeString(std::string_view, std::string_view = " \t\n\r")`.
- `extern template` decls of `tokenizeString` for list/set/vector of `std::string`.
- `template<class C, class CharT=char> C basicSplitString(...)`.
- `template<typename C> C splitString(...)`.
- `extern template` decls of `splitString` for list/set/vector.
- `template<class C> std::string concatStringsSep(std::string_view sep, const C & ss)`.
- `extern template` decls for `std::list`, `StringSet`, `std::vector`, `boost::small_vector<std::string,64>`.
- `template<class C, class F> std::string concatMapStringsSep(std::string_view, const C &, F)` — defined inline; collects mapped strings into a `boost::container::small_vector<std::string,64>` then concats.
- `template<class C> [[deprecated]] std::string dropEmptyInitThenConcatStringsSep(std::string_view, const C &)`; extern templates for list/set/vector.
- `std::list<std::string> shellSplitString(std::string_view)`.
- `std::string optionalBracket(std::string_view, std::string_view, std::string_view)`.
- `template<typename T> requires std::convertible_to<T, std::string_view> std::string optionalBracket(std::string_view, const std::optional<T> &, std::string_view)` — defined inline; overload for optional content (empty content → "").
- `const char * requireCString(const std::string &)`.

### Type aliases
- (Inside `StringViewHash`: `using HashType = std::hash<std::string_view>`, `using is_transparent = void`.)

### Macros / globals
- (None.)

## File: src/libutil/include/nix/util/strings-inline.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- (None.)

### Functions (templates)
- `template<class C, class CharT> C basicTokenizeString(std::basic_string_view<CharT>, std::basic_string_view<CharT>)` — definition.
- `template<class C> C tokenizeString(std::string_view, std::string_view)` — wrapper to `basicTokenizeString<C, char>`.
- `template<class C, class CharT> void basicSplitStringInto(C & accum, std::basic_string_view<CharT>, std::basic_string_view<CharT>)` — append-mode split into existing container.
- `template<typename C> void splitStringInto(C &, std::string_view, std::string_view)` — wrapper to `basicSplitStringInto<C, char>`.
- `template<class C, class CharT> C basicSplitString(std::basic_string_view<CharT>, std::basic_string_view<CharT>)` — return-value split (calls `basicSplitStringInto` into a fresh container).
- `template<class C> C splitString(std::string_view, std::string_view)` — wrapper.
- `template<class CharT, class C> std::basic_string<CharT> basicConcatStringsSep(std::basic_string_view<CharT>, const C & ss)` — pre-sized concat with separator (two-pass: size, then copy).
- `template<class C> std::string concatStringsSep(std::string_view, const C &)` — wrapper to `basicConcatStringsSep<char, C>`.
- `template<class C> std::string dropEmptyInitThenConcatStringsSep(std::string_view, const C & ss)` — legacy concat that emits separator before non-first non-empty result strings (effectively skips initial empties); contains a TODO commented-out `assert(!i.empty())` block.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/include/nix/util/split.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- (None.)

### Functions
- `static inline std::optional<std::string_view> splitPrefixTo(std::string_view & string, char separator)` — find first separator; mutates input to remainder; returns prefix view or `nullopt`.
- `static inline bool splitPrefix(std::string_view & string, std::string_view prefix)` — strip a literal prefix (using `hasPrefix`) and return success.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/include/nix/util/regex-combinators.hh

### Namespaces
- `nix::regex`.

### Classes / structs / enums
- (None.)

### Functions
- `static inline std::string either(std::string_view a, std::string_view b)` — `"a|b"` via `std::stringstream`.
- `static inline std::string group(std::string_view a)` — `"(a)"` via `std::stringstream`.
- `static inline std::string list(std::string_view a)` — `"a(,a)*"` via `std::stringstream`.

### Type aliases / macros / globals
- (None — file contains a TODO about constexpr string building.)

## File: src/libutil/url.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (None defined; out-of-line definitions of `ParsedURL::Authority` and `ParsedURL` member functions, plus `VerbatimURL`.)

### Functions
- `ParsedURL::Authority ParsedURL::Authority::parse(std::string_view encodedAuthority)` — wraps `boost::urls::parse_authority`; maps boost host-type enum to local `HostType`; rejects empty/zero ports via `port_number()`.
- `std::ostream & operator<<(std::ostream &, const ParsedURL::Authority &)` — render userinfo (with optional `:password@`), host (percent-encoded for Name; bracketed for IPv6/IPvFuture with `:` re-encoded), and port.
- `std::string ParsedURL::Authority::to_string() const` — uses `std::ostringstream` over `<<`.
- `static std::string percentEncodeCharSet(std::string_view, auto charSet)` — encode any char in `charSet` via `percentEncode`.
- `static ParsedURL fromBoostUrlView(boost::urls::url_view, bool lenient)` — common conversion from boost view to `ParsedURL`; rejects file:// with non-empty authority host; per-segment percent-decode of path.
- `ParsedURL parseURL(std::string_view, bool lenient)` — entry that optionally pre-fixes lenient query/fragment; catches `boost::system::system_error` → `BadURL`.
- `ParsedURL parseURLRelative(std::string_view, const ParsedURL & base)` — RFC-3986 §5 reference resolution against base; works around boost url quirks (Zone IDs not supported by `url`, fragment improperly preserved).
- `std::string percentDecode(std::string_view)` — uses `boost::urls::make_pct_string_view`; throws `BadURL` on error.
- `std::string percentEncode(std::string_view, std::string_view keep)` — uses `boost::urls::encode` with `unreserved_chars` + `keep` set.
- `StringMap decodeQuery(std::string_view query, bool lenient)` — uses `boost::urls::params_encoded_view`; warns on entries missing `=`.
- `std::string encodeUrlPath(std::span<const std::string>)` — per-segment `percentEncode` with `allowedInPath`, joined by `/`.
- `std::string encodeQuery(const StringMap &)` — joined `name=value` with `&`.
- `std::string renderUrlPathNoPctEncoding(std::span<const std::string>)` — `concatStringsSep("/", ...)`.
- `std::string ParsedURL::renderPath(bool encode) const`.
- `std::string ParsedURL::renderAuthorityAndPath() const` — asserts RFC3986 §3.3 path invariants.
- `std::string ParsedURL::to_string() const`.
- `std::ostream & operator<<(std::ostream &, const ParsedURL &)`.
- `ParsedURL ParsedURL::canonicalise()` — `.` / `..` stripping via `CanonPath` (re-splits the rendered path).
- `ParsedUrlScheme parseUrlScheme(std::string_view)` — split on `+`.
- `static std::optional<ParsedURL> tryParseScpStyle(std::string_view)` — git-compat SCP-style URL parsing including IPv6 brackets, schemes-known-to-git list (ssh/http/https/file/ftp/ftps/git plus their `git+` variants); warns about relative→absolute path conversion; uses local function-static `schemesSupportedByGit` `unordered_set` with `StringViewHash`.
- `ParsedURL fixGitURL(std::string)` — absolute-path/SCP/URL detection wrapper, drops `git+` prefix.
- `bool isValidSchemeName(std::string_view)` — RFC 3986 §3.1 regex match (lazy-init function-static `regex`).
- `std::vector<std::string> pathToUrlPath(const std::filesystem::path &)` — handles Windows drive letters and trailing slashes.
- `std::filesystem::path urlPathToPath(std::span<const std::string>)` — inverse, with `/` rejection, NUL-byte rejection (substituted `␀` glyph), and Windows UNC handling.
- `std::ostream & operator<<(std::ostream &, const VerbatimURL &)`.
- `std::optional<std::string> VerbatimURL::lastPathSegment() const` — last non-empty segment via `parsed().pathSegments(skipEmpty=true)` with `baseNameOf` fallback on `BadURL`.

### Type aliases
- (None new.)

### Macros / globals
- `std::regex refRegex(refRegexS, std::regex::ECMAScript)` — definition for header-declared `extern std::regex`.
- `std::regex revRegex(revRegexS, std::regex::ECMAScript)` — definition for header-declared `extern std::regex`.
- `static constexpr boost::urls::grammar::lut_chars extraAllowedCharsInFragment = " \"^"`.
- `static constexpr boost::urls::grammar::lut_chars extraAllowedCharsInQuery = " \""`.
- `const static std::string allowedInQuery = ":@/?"`.
- `const static std::string allowedInPath = ":@"`.

## File: src/libutil/include/nix/util/url.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct ParsedURL` — RFC-3986 representation:
  - nested `struct Authority`:
    - `enum class HostType { Name, IPv4, IPv6, IPvFuture }`.
    - static `Authority parse(std::string_view encodedAuthority)`.
    - `auto operator<=>(const Authority & other) const = default`.
    - `std::string to_string() const`.
    - `friend std::ostream & operator<<(std::ostream &, const Authority &)`.
    - members: `HostType hostType = HostType::Name`, `std::string host`, `std::optional<std::string> user`, `std::optional<std::string> password`, `std::optional<uint16_t> port`.
  - members: `std::string scheme`, `std::optional<Authority> authority`, `std::vector<std::string> path`, `StringMap query`, `std::string fragment`.
  - `std::string renderAuthorityAndPath() const`.
  - `std::string to_string() const`.
  - `std::string renderPath(bool encode = false) const`.
  - `auto operator<=>(const ParsedURL & other) const noexcept = default`.
  - `ParsedURL canonicalise()`.
  - `auto pathSegments(bool skipEmpty) const &` — inline ranges view filtering empty segments.
- `MakeError(BadURL, Error)` — exception type via macro.
- `struct ParsedUrlScheme` — `{ std::optional<std::string_view> application; std::string_view transport; }`.
- `struct VerbatimURL` — variant wrapper:
  - `using Raw = std::variant<std::string, ParsedURL>; Raw raw;`.
  - 3 ctors (string_view, string, ParsedURL).
  - inline `std::string to_string() const` via `std::visit + overloaded`.
  - inline `const ParsedURL parsed() const` — re-runs `parseURL` for the string variant (no caching).
  - inline `std::string_view scheme() const &` — via `splitPrefixTo(':')` for the string variant.
  - `std::optional<std::string> lastPathSegment() const`.

### Functions
- `std::ostream & operator<<(std::ostream &, const ParsedURL &)`.
- `std::string percentDecode(std::string_view)`, `std::string percentEncode(std::string_view, std::string_view keep = "")`.
- `std::string renderUrlPathNoPctEncoding(std::span<const std::string>)`, `std::string encodeUrlPath(std::span<const std::string>)`.
- `StringMap decodeQuery(std::string_view, bool lenient = false)`, `std::string encodeQuery(const StringMap &)`.
- `ParsedURL parseURL(std::string_view, bool lenient = false)`, `ParsedURL parseURLRelative(std::string_view, const ParsedURL & base)`.
- `ParsedUrlScheme parseUrlScheme(std::string_view)`.
- `ParsedURL fixGitURL(std::string)`.
- `bool isValidSchemeName(std::string_view)`.
- `std::vector<std::string> pathToUrlPath(const std::filesystem::path &)`, `std::filesystem::path urlPathToPath(std::span<const std::string>)`.
- `std::ostream & operator<<(std::ostream &, const VerbatimURL &)`.

### Type aliases
- `ParsedURL::Authority::HostType` — enum class (already noted).
- `VerbatimURL::Raw` (already noted).

### Macros / globals
- (None.)

## File: src/libutil/include/nix/util/url-parts.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- (None.)

### Functions
- (None — file is purely regex constants.)

### Type aliases
- (None.)

### Macros / globals
- `const static std::string pctEncoded` — `"(?:%[0-9a-fA-F][0-9a-fA-F])"`.
- `const static std::string unreservedRegex` — `"(?:[a-zA-Z0-9-._~])"`.
- `const static std::string subdelimsRegex` — `"(?:[!$&'\"()*+,;=])"`.
- `const static std::string pcharRegex` — composed of the above (`unreservedRegex|pctEncoded|subdelimsRegex|[:@]`).
- `const static std::string fragmentRegex` — `"(?:" + pcharRegex + "|[/? \"^])*"`.
- `const static std::string refRegexS` — `"[a-zA-Z0-9@][a-zA-Z0-9_.\\/@+-]*"` (Git ref, marked TODO incomplete).
- `extern std::regex refRegex` (defined in url.cc).
- `const static std::string revRegexS` — `"[0-9a-fA-F]{40}"` (40-char SHA-1).
- `extern std::regex revRegex` (defined in url.cc).
- `const static std::string refAndOrRevRegex` — combined `(rev)|(ref(/rev)?)`.

## File: src/libutil/git.cc

### Namespaces
- `nix::git` (with `using namespace nix; using namespace std::string_literals;` at file scope).

### Classes / structs / enums
- (None defined here.)

### Functions
- `std::optional<Mode> decodeMode(RawMode)` — only allow Directory/Executable/Regular/Symlink.
- `static std::string getStringUntil(Source &, char)` — read source byte-by-byte until terminator byte; terminator is consumed but not appended.
- `static std::string getString(Source &, int n)` — read exactly n bytes via `Source::operator()`.
- `void parseBlob(FileSystemObjectSink &, const CanonPath &, Source &, BlobMode, const ExperimentalFeatureSettings &)` — handle regular/executable/symlink blob payloads via `createRegularFile` (with `preallocateContents` + `drainInto`) or `createSymlink`; gates on `Xp::GitHashing`.
- `void parseTree(FileSystemObjectSink &, const CanonPath &, Source &, HashAlgorithm, fun<SinkHook>, const ExperimentalFeatureSettings &)` — read tree entries (octal mode, name, raw hash bytes) and dispatch hook; only SHA1/SHA256 supported.
- `ObjectType parseObjectType(Source &, const ExperimentalFeatureSettings &)` — read 5-byte `"blob "` or `"tree "` prefix; gates on `Xp::GitHashing`.
- `void parse(FileSystemObjectSink &, const CanonPath &, Source &, BlobMode, HashAlgorithm, fun<SinkHook>, const ExperimentalFeatureSettings &)` — top-level parse dispatcher.
- `std::optional<Mode> convertMode(SourceAccessor::Type)` — Source type → Git mode mapping; returns `nullopt` for char/block/socket/fifo, `unreachable()` for unknown.
- `void restore(FileSystemObjectSink &, Source &, HashAlgorithm, fun<RestoreHook>)` — replays git tree, copying via `RestoreHook` + `copyRecursive`; verifies modes match.
- `void dumpBlobPrefix(uint64_t size, Sink &, const ExperimentalFeatureSettings &)` — writes `"blob N\0"` via `fmt`.
- `void dumpTree(const Tree &, Sink &, const ExperimentalFeatureSettings &)` — write entries (`"%o name\0<binhash>"`) framed by `"tree N\0"`; strips trailing `/` from directory names.
- `Mode dump(const SourcePath &, Sink &, fun<DumpHook>, PathFilter &, const ExperimentalFeatureSettings &)` — recursive dump dispatching on file type.
- `TreeEntry dumpHash(HashAlgorithm, const SourcePath &, PathFilter &)` — recursive `dump` against `HashSink` to compute Git-style hashes via a recursive lambda hook.
- `std::optional<LsRemoteRefLine> parseLsRemoteLine(std::string_view)` — regex parse of one ls-remote line via function-static `std::regex line_regex`.

### Type aliases
- (None new.)

### Macros / globals
- (None.)

## File: src/libutil/include/nix/util/git.hh

### Namespaces
- `nix::git`.

### Classes / structs / enums
- `enum struct ObjectType { Blob, Tree, /* Commit, Tag commented out */ }`.
- `enum struct Mode : RawMode { Directory = 0040000, Regular = 0100644, Executable = 0100755, Symlink = 0120000 }`.
- `enum struct BlobMode : RawMode { Regular, Executable, Symlink }` — subset of Mode (values cast from Mode constants).
- `struct TreeEntry { Mode mode; Hash hash; bool operator==(const TreeEntry&) const = default; auto operator<=>(const TreeEntry&) const = default; }`.
- `struct LsRemoteRefLine` — `enum struct Kind { Symbolic, Object }`, `Kind kind`, `std::string target`, `std::optional<std::string> reference`.

### Functions
- `std::optional<Mode> decodeMode(RawMode)`.
- `ObjectType parseObjectType(Source &, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `void parseBlob(FileSystemObjectSink &, const CanonPath &, Source &, BlobMode, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `void parseTree(FileSystemObjectSink &, const CanonPath &, Source &, HashAlgorithm, fun<SinkHook>, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `void parse(FileSystemObjectSink &, const CanonPath &, Source &, BlobMode rootModeIfBlob, HashAlgorithm, fun<SinkHook>, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `std::optional<Mode> convertMode(SourceAccessor::Type)`.
- `void restore(FileSystemObjectSink &, Source &, HashAlgorithm, fun<RestoreHook>)`.
- `void dumpBlobPrefix(uint64_t, Sink &, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `void dumpTree(const Tree &, Sink &, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `Mode dump(const SourcePath &, Sink &, fun<DumpHook>, PathFilter & = defaultPathFilter, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
- `TreeEntry dumpHash(HashAlgorithm, const SourcePath &, PathFilter & = defaultPathFilter)`.
- `std::optional<LsRemoteRefLine> parseLsRemoteLine(std::string_view)`.

### Type aliases
- `using RawMode = uint32_t`.
- `using Tree = std::map<std::string, TreeEntry>` (directories' names end with `/` to sort correctly).
- `using SinkHook = void(const CanonPath & name, TreeEntry entry)` — function-type alias used with `fun<>`.
- `using RestoreHook = SourcePath(Hash)`.
- `using DumpHook = TreeEntry(const SourcePath & path)`.

### Macros / globals
- (None.)

## File: src/libutil/compression.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct ChunkedCompressionSink : CompressionSink` — base for sinks that prefer ≤32k chunks; `uint8_t outbuf[32 * 1024]`; pure-virtual `writeInternal` and override of `writeUnbuffered` which calls `writeInternal` on chunks (`CHUNK_SIZE = sizeof(outbuf) << 2 = 128 KiB`).
- `struct ArchiveDecompressionSource : Source` — wraps `TarArchive` (libarchive raw mode) for streaming decompression; holds `std::unique_ptr<TarArchive> archive`, `Source & src`, and optional `compressionMethod`; throws `EndOfFile` when libarchive returns 0; `CompressionError` when filter count < 2.
- `struct ArchiveCompressionSink : CompressionSink` — libarchive-write wrapper with private `open()` and static `callback_write` for libarchive's I/O hook; disables internal buffering and output padding; uses X-macro `NIX_FOR_EACH_LA_ALGO` to dispatch to `archive_write_add_filter_*`.
- `struct NoneSink : CompressionSink` — pass-through sink (warns if a non-default level is specified).
- `struct BrotliDecompressionSink : ChunkedCompressionSink` — uses `BrotliDecoderState`; `bool finished = false` flag.
- `struct BrotliCompressionSink : ChunkedCompressionSink` — uses `BrotliEncoderState`; member `uint8_t outbuf[BUFSIZ]` (note: shadows `ChunkedCompressionSink::outbuf`); `bool finished = false`.
- `struct ZstdMultiFrameCompressionSink : CompressionSink` — multi-frame zstd encoder cutting frames every `bytesPerFrame = 16 MiB`; uses `ZSTD_CCtx` (RAII via `unique_ptr` with `ZSTD_freeCCtx`) with explicit `setPledgedSrcSize` (writes `Frame_Content_Size` into header for parallel decoders); caps `nbWorkers` to ≤4; emits empty frame if no input.

### Functions
- `void ChunkedCompressionSink::writeUnbuffered(std::string_view) override`.
- `size_t ArchiveDecompressionSource::read(char *, size_t) override`.
- `ArchiveCompressionSink::ArchiveCompressionSink(Sink &, CompressionAlgo, bool parallel, int level)` — ctor.
- `ArchiveCompressionSink::~ArchiveCompressionSink() override`.
- `void ArchiveCompressionSink::finish() override`.
- `void ArchiveCompressionSink::check(int err, const std::string & reason)` — throws on `ARCHIVE_EOF` (`EndOfFile`) or non-`ARCHIVE_OK`.
- `void ArchiveCompressionSink::writeUnbuffered(std::string_view) override`.
- `void ArchiveCompressionSink::open()` — private.
- `static ssize_t ArchiveCompressionSink::callback_write(struct archive *, void *, const void *, size_t)`.
- `NoneSink::NoneSink(Sink &, int level)`.
- `void NoneSink::finish() override`.
- `void NoneSink::writeUnbuffered(std::string_view) override`.
- `BrotliDecompressionSink::BrotliDecompressionSink(Sink &)`.
- `BrotliDecompressionSink::~BrotliDecompressionSink()`.
- `void BrotliDecompressionSink::finish() override`.
- `void BrotliDecompressionSink::writeInternal(std::string_view) override`.
- `std::string decompress(const std::string & method, std::string_view in)` — convenience using `makeDecompressionSink` over `StringSink`.
- `std::unique_ptr<FinishSink> makeDecompressionSink(const std::string & method, Sink & nextSink)` — selects `NoneSink` (`none`/`""`/`identity`), `BrotliDecompressionSink` (`br`), or `sourceToSink` wrapping `ArchiveDecompressionSource`.
- `BrotliCompressionSink::BrotliCompressionSink(Sink &)`.
- `BrotliCompressionSink::~BrotliCompressionSink()`.
- `void BrotliCompressionSink::finish() override`.
- `void BrotliCompressionSink::writeInternal(std::string_view) override`.
- `ZstdMultiFrameCompressionSink::ZstdMultiFrameCompressionSink(Sink &, bool parallel, int level)`.
- `void ZstdMultiFrameCompressionSink::checkZstd(size_t)`.
- `void ZstdMultiFrameCompressionSink::emitFrame()`.
- `void ZstdMultiFrameCompressionSink::writeUnbuffered(std::string_view) override`.
- `void ZstdMultiFrameCompressionSink::finish() override`.
- `ref<CompressionSink> makeCompressionSink(CompressionAlgo, Sink &, bool parallel, int level)` — dispatch to `NoneSink`/`BrotliCompressionSink`/`ZstdMultiFrameCompressionSink`/`ArchiveCompressionSink` per algorithm via X-macro.
- `std::string compress(CompressionAlgo, std::string_view, bool parallel, int level)`.
- `std::string compress(CompressionAlgo, Source &, bool parallel, int level)`.

### Type aliases
- (None new.)

### Macros / globals
- `static const int COMPRESSION_LEVEL_DEFAULT = -1` — sentinel meaning "library default".
- `#define NIX_FOR_EACH_LA_ALGO(MACRO)` — X-macro listing libarchive-handled algos: bzip2/compress/grzip/gzip/lrzip/lz4/lzip/lzma/lzop/xz; deliberately excludes none/brotli/zstd.
- `#define NIX_DEF_LA_ALGO_CASE(algo)` — local helper used twice (in `ArchiveCompressionSink` ctor lambda and `makeCompressionSink`); each `#define` is `#undef`-ed locally.

## File: src/libutil/include/nix/util/compression.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct CompressionSink : BufferedSink, FinishSink` — composite sink with `using` directives lifting `BufferedSink::operator()`, `BufferedSink::writeUnbuffered`, `FinishSink::finish` into scope to disambiguate overload resolution.
- `MakeError(CompressionError, Error)` — exception type via macro.

### Functions
- `std::string decompress(const std::string & method, std::string_view in)`.
- `std::unique_ptr<FinishSink> makeDecompressionSink(const std::string &, Sink &)`.
- `std::string compress(CompressionAlgo, std::string_view, bool parallel = false, int level = -1)`.
- `std::string compress(CompressionAlgo, Source &, bool parallel = false, int level = -1)`.
- `ref<CompressionSink> makeCompressionSink(CompressionAlgo, Sink &, bool parallel = false, int level = -1)`.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/compression-algo.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (None.)

### Functions
- `CompressionAlgo parseCompressionAlgo(std::string_view, bool suggestions)` — uses macro-generated `lookupTable`; on miss optionally attaches `Suggestions::bestMatches` from a function-static `allNames` cache and throws `UnknownCompressionMethod`.
- `std::string showCompressionAlgo(CompressionAlgo)` — macro-generated reverse map; `unreachable()` on default.

### Type aliases / macros / globals
- `#define NIX_COMPRESSION_ALGO_FROM_STRING(name, value) {name, CompressionAlgo::value},` (function-local, `#undef`-ed).
- `#define NIX_COMPRESSION_ALGO_TO_STRING(name, value) case CompressionAlgo::value: return name;` (function-local, `#undef`-ed).
- `static const std::unordered_map<std::string_view, CompressionAlgo> lookupTable` — function-static.
- `static const StringSet allNames` — function-static; lazy-initialized only when `suggestions` is true.

## File: src/libutil/include/nix/util/compression-algo.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum class CompressionAlgo` — values generated by `NIX_FOR_EACH_COMPRESSION_ALGO`: `none, brotli, bzip2, compress, grzip, gzip, lrzip, lz4, lzip, lzma, lzop, xz, zstd`.
- `MakeError(UnknownCompressionMethod, Error)` — exception type.

### Functions
- `CompressionAlgo parseCompressionAlgo(std::string_view method, bool suggestions = false)`.
- `std::string showCompressionAlgo(CompressionAlgo)`.

### Type aliases / macros / globals
- `#define NIX_FOR_EACH_COMPRESSION_ALGO(MACRO)` — public X-macro listing 13 `(string-name, enum-value)` pairs.
- `#define NIX_DEFINE_COMPRESSION_ALGO(name, value) value,` (file-scope, `#undef`-ed) — used to produce the enum body.

## File: src/libutil/compression-settings.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (Out-of-line specializations only.)

### Functions
- `template<> CompressionAlgo BaseSetting<CompressionAlgo>::parse(const std::string &) const` — wraps `parseCompressionAlgo` with suggestions; rethrows as `UsageError` on `UnknownCompressionMethod`.
- `template<> std::optional<CompressionAlgo> BaseSetting<std::optional<CompressionAlgo>>::parse(const std::string &) const` — empty string → `std::nullopt`; same `UsageError` rethrow.
- `template<> std::string BaseSetting<CompressionAlgo>::to_string() const`.
- `template<> std::string BaseSetting<std::optional<CompressionAlgo>>::to_string() const` — empty string for `nullopt`.
- `NLOHMANN_JSON_SERIALIZE_ENUM(CompressionAlgo, ...)` — generated JSON ↔ enum mapping using the same X-macro.
- `template class BaseSetting<CompressionAlgo>` — explicit instantiation.
- `template class BaseSetting<std::optional<CompressionAlgo>>` — explicit instantiation.

### Type aliases
- `template<> struct BaseSetting<CompressionAlgo>::trait { static constexpr bool appendable = false; }` — disables append semantics.
- `template<> struct BaseSetting<std::optional<CompressionAlgo>>::trait` — same.

### Macros / globals
- `#define NIX_COMPRESSION_JSON(name, value) {CompressionAlgo::value, name},` (file-scope, `#undef`-ed).

## File: src/libutil/include/nix/util/compression-settings.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `template<> struct json_avoids_null<CompressionAlgo> : std::true_type {}` — declares enum is never serialized as null.

### Functions
- (None directly; via `NIX_DECLARE_CONFIG_SERIALISER` macro.)

### Type aliases
- (None.)

### Macros / globals
- `NIX_DECLARE_CONFIG_SERIALISER(CompressionAlgo)` — declares `BaseSetting<CompressionAlgo>` parse/to_string/trait machinery.
- `NIX_DECLARE_CONFIG_SERIALISER(std::optional<CompressionAlgo>)` — same for optional.

## File: src/libutil/xml-writer.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (Out-of-line `XMLWriter` definitions only.)

### Functions
- `XMLWriter::XMLWriter(bool indent, std::ostream &)` — emits XML prolog `<?xml version='1.0' encoding='utf-8'?>`; sets `closed = false`.
- `XMLWriter::~XMLWriter()` — calls `close()`.
- `void XMLWriter::close()` — closes pending elems and sets `closed = true`.
- `void XMLWriter::indent_(size_t depth)` — emits `depth * 2` spaces if `indent`.
- `void XMLWriter::openElement(std::string_view name, const XMLAttrs &)` — asserts `!closed`; pushes name onto `pendingElems`.
- `void XMLWriter::closeElement()` — asserts `!pendingElems.empty()`; pops; if empty after pop sets `closed = true`.
- `void XMLWriter::writeEmptyElement(std::string_view, const XMLAttrs &)`.
- `void XMLWriter::writeAttrs(const XMLAttrs &)` — escapes `"`, `<`, `>`, `&`, and `\n` (as `&#xA;` to prevent attribute normalisation per XML spec §3.3.3).

### Type aliases / macros / globals
- (None.)

## File: src/libutil/include/nix/util/xml-writer.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `class XMLWriter` — private `std::ostream & output`, `bool indent`, `bool closed`, `std::list<std::string> pendingElems`; ctor/dtor; public `close`, `openElement`, `closeElement`, `writeEmptyElement`; private `writeAttrs`, `indent_`.
- `class XMLOpenElement` — RAII wrapper; private `XMLWriter & writer`; ctor opens element; non-copyable, non-movable (deleted copy/move ctors and assignment ops); destructor calls `closeElement`.

### Functions
- (Member functions of the above, declared.)

### Type aliases
- `typedef std::map<std::string, std::string, std::less<>> XMLAttrs` — heterogeneous-comparable attributes map.

### Macros / globals
- (None.)

## File: src/libutil/json-utils.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (None defined.)

### Functions
- `const nlohmann::json & valueAt(const nlohmann::json::object_t &, std::string_view key)` — throwing key access; delegates to `optionalValueAt` and throws `Error` with object dump if absent.
- `const nlohmann::json * optionalValueAt(const nlohmann::json::object_t &, std::string_view key)` — wraps nlohmann's `get` (free function).
- `const nlohmann::json * getNullable(const nlohmann::json &)` — JSON `null` → `nullptr`, otherwise returns `&value`.
- `static const nlohmann::json & ensureType(const nlohmann::json &, nlohmann::json::value_type expectedType)` — type-check helper used by other `get*` fns; throws `Error` with type names + dump on mismatch.
- `const nlohmann::json::object_t & getObject(const nlohmann::json &)`.
- `const nlohmann::json::array_t & getArray(const nlohmann::json &)`.
- `const nlohmann::json::string_t & getString(const nlohmann::json &)`.
- `const nlohmann::json::number_unsigned_t & getUnsigned(const nlohmann::json &)` — handles type-name special-casing for integers (distinguishes signed/floating-point in error message).
- `const nlohmann::json::boolean_t & getBoolean(const nlohmann::json &)`.
- `Strings getStringList(const nlohmann::json &)` — uses `getArray` + per-element `getString`.
- `StringMap getStringMap(const nlohmann::json &)` — uses `getMap<std::string, std::less<>>(getObject(value), getString)`.
- `StringSet getStringSet(const nlohmann::json &)` — uses `getArray` + per-element `getString`.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/include/nix/util/json-utils.hh

### Namespaces
- `nix`.
- `nlohmann` — for `adl_serializer<std::optional<T>>`.

### Classes / structs / enums
- `enum struct ExperimentalFeature` — forward declaration.
- `template<typename T> struct nlohmann::adl_serializer<std::optional<T>>` — generic optional serializer enforcing `nix::json_avoids_null<T>::value` via `static_assert`; `null ↔ std::nullopt`.

### Functions
- `const nlohmann::json & valueAt(const nlohmann::json::object_t &, std::string_view)`.
- `const nlohmann::json * optionalValueAt(const nlohmann::json::object_t &, std::string_view)`.
- Two `= delete` overloads of the above for r-value `nlohmann::json::object_t &&` (prevent dangling refs).
- `const nlohmann::json * getNullable(const nlohmann::json &)`.
- `const nlohmann::json::object_t & getObject(const nlohmann::json &)`.
- `const nlohmann::json::array_t & getArray(const nlohmann::json &)`.
- `const nlohmann::json::string_t & getString(const nlohmann::json &)`.
- `const nlohmann::json::number_unsigned_t & getUnsigned(const nlohmann::json &)`.
- `template<typename T> auto getInteger(const nlohmann::json &) -> std::enable_if_t<std::is_signed_v<T> && std::is_integral_v<T>, T>` — bounds-checked conversion of signed/unsigned numeric JSON values to integral T (defined inline; throws on out-of-range or non-integer).
- `template<typename... Args> std::map<std::string, Args...> getMap(const json::object_t &, auto && f)` — generic map deserializer (defined inline).
- `const nlohmann::json::boolean_t & getBoolean(const nlohmann::json &)`, `Strings getStringList(...)`, `StringMap getStringMap(...)`, `StringSet getStringSet(...)`.
- `template<typename T> static inline std::optional<T> ptrToOwned(const nlohmann::json *)` — convert pointer-style result to owning optional.

### Type aliases
- (None new.)

### Macros / globals
- (None.)

## File: src/libutil/include/nix/util/json-impls.hh

### Namespaces
- (None at file scope; macros open `nlohmann`.)

### Classes / structs / enums
- (None.)

### Functions
- (None.)

### Type aliases
- (None.)

### Macros / globals
- `JSON_IMPL_INNER_TO(TYPE)` — declares `adl_serializer<TYPE>` with only `to_json`.
- `JSON_IMPL_INNER_FROM(TYPE)` — declares `adl_serializer<TYPE>` with only `from_json`.
- `JSON_IMPL_INNER(TYPE)` — declares `adl_serializer<TYPE>` with both `from_json` and `to_json`.
- `JSON_IMPL(TYPE)` — opens `namespace nlohmann { using namespace nix; template<> JSON_IMPL_INNER(TYPE); }`.
- `JSON_IMPL_WITH_XP_FEATURES(TYPE)` — like `JSON_IMPL` but `from_json` takes a defaulted `const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings`.

## File: src/libutil/include/nix/util/json-non-null.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `template<typename T> struct json_avoids_null` — forward declaration of trait.
- `template<typename T> struct json_avoids_null : std::bool_constant<std::is_integral<T>::value> {}` — default falls back to integral.
- `template<> struct json_avoids_null<std::nullptr_t> : std::false_type`.
- `template<> struct json_avoids_null<bool> : std::true_type`.
- `template<> struct json_avoids_null<std::string> : std::true_type`.
- `template<typename T> struct json_avoids_null<std::vector<T>> : std::true_type`.
- `template<typename T> struct json_avoids_null<std::list<T>> : std::true_type`.
- `template<typename T, typename Compare> struct json_avoids_null<std::set<T, Compare>> : std::true_type`.
- `template<typename K, typename V, typename Compare> struct json_avoids_null<std::map<K, V, Compare>> : std::true_type`.

### Functions / aliases / macros
- (None.)

## File: src/libutil/include/nix/util/abstract-setting-to-json.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- (None defined directly.)

### Functions
- `template<typename T> std::map<std::string, nlohmann::json> BaseSetting<T>::toJSONObject() const` — out-of-line template definition layered on `AbstractSetting::toJSONObject()`, adding `value`, `defaultValue`, `documentDefault` keys via `emplace`.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/signature/signer.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (Out-of-line `LocalSigner` defs only.)

### Functions
- `LocalSigner::LocalSigner(SecretKey && privateKey)` — initializes member `privateKey` from the parameter (note: no `std::move` in init list, so a copy occurs despite the rvalue-ref signature), derives `publicKey` via `privateKey.toPublicKey()`.
- `Signature LocalSigner::signDetached(std::string_view) const override`.
- `const PublicKey & LocalSigner::getPublicKey() override`.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/signature/local-keys.cc

### Namespaces
- `nix`.
- `nix::{anonymous}` — for `parseColonBase64` / `serializeColonBase64`.
- `nlohmann` — `adl_serializer<Signature>` JSON glue.

### Classes / structs / enums
- (None defined; out-of-line member definitions.)

### Functions
- `std::pair<std::string, std::string> parseColonBase64(std::string_view s, std::string_view typeName)` — anon helper splitting `<name>:<base64>`; throws `FormatError` if no colon, empty name, or empty data.
- `std::string serializeColonBase64(std::string_view name, std::string_view data)` — anon helper producing `name + ":" + base64::encode(...)`.
- `Signature Signature::parse(std::string_view)` — uses `parseColonBase64`.
- `std::string Signature::to_string() const` — uses `serializeColonBase64`.
- `template<typename Container> std::set<Signature> Signature::parseMany(const Container &)` — definition using `std::views::transform`; explicit instantiations for `Strings` and `StringSet`.
- `Strings Signature::toStrings(const std::set<Signature> &)`.
- `Key::Key(std::string_view s, bool sensitiveValue)` — common parse via `parseColonBase64`; on error adds trace optionally including the raw value when `!sensitiveValue`.
- `std::string Key::to_string() const`.
- `SecretKey::SecretKey(std::string_view)` — calls `Key{s, /*sensitiveValue=*/true}`; validates length against `crypto_sign_SECRETKEYBYTES`.
- `Signature SecretKey::signDetached(std::string_view) const` — calls `crypto_sign_detached`.
- `PublicKey SecretKey::toPublicKey() const` — derives public via `crypto_sign_ed25519_sk_to_pk`; uses private `PublicKey(name, key)` ctor (allowed via `friend`).
- `static SecretKey SecretKey::generate(std::string_view name)` — calls `crypto_sign_keypair` then private `SecretKey(name, key)` ctor.
- `PublicKey::PublicKey(std::string_view)` — calls `Key{s, /*sensitiveValue=*/false}`; validates length against `crypto_sign_PUBLICKEYBYTES`.
- `bool PublicKey::verifyDetached(std::string_view, const Signature &) const` — checks `sig.keyName == name` then forwards to `verifyDetachedAnon`.
- `bool PublicKey::verifyDetachedAnon(std::string_view, const Signature &) const` — calls `crypto_sign_verify_detached`; throws if `sig.sig.size() != crypto_sign_BYTES`.
- `bool verifyDetached(std::string_view, const Signature &, const PublicKeys &)` — looks up `sig.keyName`, dispatches to `verifyDetachedAnon`.
- `void adl_serializer<Signature>::to_json(json &, const Signature &)` — emits `{"keyName": s.keyName, "sig": <base64>}`.
- `Signature adl_serializer<Signature>::from_json(const json &)` — accepts plain string (calls `Signature::parse`) or object form with `{keyName, sig (base64)}`.

### Type aliases / macros / globals
- (None.)

## File: src/libutil/include/nix/util/signature/signer.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct Signer` — abstract base; `virtual ~Signer() = default`; `virtual Signature signDetached(std::string_view) const = 0`; `virtual const PublicKey & getPublicKey() = 0`.
- `struct LocalSigner : Signer` — holds private `SecretKey privateKey` and `PublicKey publicKey`; ctor takes `SecretKey &&`; overrides `signDetached` and `getPublicKey`.

### Functions
- (Member declarations only.)

### Type aliases
- `using Signers = std::map<std::string, Signer *>` — registry by name.

### Macros / globals
- (None.)

## File: src/libutil/include/nix/util/signature/local-keys.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct Signature` — `std::string keyName; std::string sig` (raw decoded bytes); `static Signature parse(std::string_view)`; `template<Container> static std::set<Signature> parseMany(const Container &)`; `std::string to_string() const`; `static Strings toStrings(const std::set<Signature> &)`; `auto operator<=>(const Signature &) const = default`.
- `struct Key` — base with `std::string name; std::string key`; public `to_string()`; protected ctors `Key(std::string_view s, bool sensitiveValue)` and `Key(std::string_view name, std::string && key)`.
- forward `struct PublicKey`.
- `struct SecretKey : Key` — public ctor from string, `signDetached`, `toPublicKey`, static `generate`; private ctor `SecretKey(std::string_view name, std::string && key)` used by `generate`.
- `struct PublicKey : Key` — public ctor from string, `verifyDetached`, `verifyDetachedAnon`; private ctor `PublicKey(std::string_view name, std::string && key)`; `friend struct SecretKey` (so `SecretKey::toPublicKey` can construct it).

### Functions
- `bool verifyDetached(std::string_view data, const Signature &, const PublicKeys &)` — multi-key verification.

### Type aliases
- `typedef std::map<std::string, PublicKey> PublicKeys`.

### Macros / globals
- `JSON_IMPL(nix::Signature)` — emits `nlohmann::adl_serializer<Signature>` declaration (placed at file scope outside `namespace nix`).

## Cross-file observations

- **Three sibling base-N encoders** with parallel APIs but living in slightly different shapes:
  - `nix::base16::{encode, decode, encodedLength}` — namespace functions (see `base-n.hh`/`base-n.cc`).
  - `nix::base64::{encode, decode, encodedLength}` — namespace functions (see `base-n.hh`/`base-n.cc`).
  - `nix::BaseNix32::{encode, decode, encodedLength, characters, reverseMap, lookupReverse, invalid}` — static-only struct (see `base-nix-32.hh`/`base-nix-32.cc`).
  All three encode `std::span<const std::byte> → std::string` and decode `std::string_view → std::string`. Only `BaseNix32` exposes a public alphabet/inverse-lookup; `base64::decode` builds its inverse table as a constexpr lambda and `base16::decode` uses an inline lambda, so each does its own thing. `hash.cc` (`baseExplicit`, `baseFromSize`) fans out to all three; `local-keys.cc` and `hash.cc` rerun the SRI/base64 path independently. A common `Codec` concept (`encode`/`decode`/`encodedLength`) and a shared "build inverse map" helper would consolidate these.

- **Hash format/algorithm parsing pairs** in `hash.cc` repeat a near-identical pattern: an `Opt` (returns `std::optional`) plus a throwing wrapper that re-throws as `UsageError` (`parseHashFormat`/`parseHashFormatOpt` and `parseHashAlgo`/`parseHashAlgoOpt`). The same pattern shows up in `compression-algo.cc` (`parseCompressionAlgo`, only-throwing). The `optional + throwing wrapper` idiom appears at least three times in this shard and is a candidate for a small generic helper that takes an X-macro list.

- **X-macro driven enum/string lookups** appear in two distinct flavors:
  - `compression-algo.hh` defines `NIX_FOR_EACH_COMPRESSION_ALGO`, used in `compression-algo.cc` (forward and reverse maps), `compression-settings.cc` (via `NLOHMANN_JSON_SERIALIZE_ENUM`), and (with a deliberate exclusion of none/brotli/zstd) in `compression.cc` via `NIX_FOR_EACH_LA_ALGO`. Three independent maps are generated from the same list, plus the private libarchive subset.
  - `hash.cc` does the same job by hand (no X-macro) for `HashAlgorithm` and `HashFormat`; `parseHashAlgoOpt`, `printHashAlgo`, `parseHashFormatOpt`, `printHashFormat` would compress nicely with the same X-macro idiom.

- **Colon-prefixed Base64 serialization** appears in three places:
  - `Hash::to_string` / `Hash::parseAny*` use `<algo>:<base*>` and `<algo>-<base64>` (SRI).
  - `Signature` and `Key` (in `signature/local-keys.cc`) use `<name>:<base64>` via the anon-namespace `parseColonBase64`/`serializeColonBase64` helpers. Those helpers are private and not shared with `hash.cc`, even though the parse/format work is essentially identical.

- **JSON glue scaffolding** is split across several headers:
  - `json-impls.hh` provides `JSON_IMPL` / `JSON_IMPL_WITH_XP_FEATURES` / `JSON_IMPL_INNER{,_TO,_FROM}` for declaring `adl_serializer` specializations.
  - `json-non-null.hh` provides the `json_avoids_null<T>` trait used by both `Hash` and `CompressionAlgo`.
  - `json-utils.hh` provides the `getX` accessors *and* the generic `adl_serializer<std::optional<T>>` that consumes `json_avoids_null` (with `static_assert` enforcement).
  - `abstract-setting-to-json.hh` is a one-function header providing `BaseSetting<T>::toJSONObject`.
  Each consumer (`hash.cc`, `compression-settings.cc`, `signature/local-keys.cc`) ends up writing similar `adl_serializer` boilerplate that could be templated on a "string-like value type".

- **Two unrelated regex stash points** for Git-style refs/revisions:
  - `url-parts.hh` declares `refRegexS`, `revRegexS`, `refAndOrRevRegex`, with `extern std::regex refRegex`/`revRegex` defined in `url.cc`.
  - `git.cc::parseLsRemoteLine` defines its own anonymous `std::regex line_regex` rather than using these constants. Note that `line_regex` matches a different shape (whole ls-remote line, not just a ref/rev).

- **Source-reading micro-helpers** `git.cc::getStringUntil` (byte-by-byte until terminator) and `getString(Source &, int)` are file-local statics that look like they should live alongside `Source` in `serialise.hh`. Similar manual `Source` reading is also done indirectly via `readFile` in `hash.cc::hashFile`.

- **CompressionSink hierarchy** repeats common boilerplate (`finish() override` calling `flush()` then doing trailing work) across `NoneSink`, `BrotliCompressionSink`, `BrotliDecompressionSink`, `ArchiveCompressionSink`, `ZstdMultiFrameCompressionSink`. The decompression and compression Brotli sinks share most of `writeInternal`'s structure (`next_in`/`avail_in`/`next_out` loop) but cannot trivially share code due to differing C-API surfaces. Note also that `BrotliCompressionSink` shadows `ChunkedCompressionSink::outbuf` with its own `outbuf[BUFSIZ]` member — an easy-to-miss trap.

- **Duplicate concat helpers**: `concatStringsSep` (header) and `basicConcatStringsSep` (inline header) are templated, yet `dropEmptyInitThenConcatStringsSep` is a separate copy with slightly different empty-handling (it skips a leading separator only when the accumulator is still empty, which behaves like skipping the initial empty element). The header marks it `[[deprecated]]`, but it's still instantiated for three container types (`std::list<std::string>`, `StringSet`, `std::vector<std::string>`). Removing the third concat function (or unifying via a predicate) is an obvious cleanup.

- **`ParsedURL` + `VerbatimURL`** both expose `to_string`, `scheme`, and last-segment helpers, but `VerbatimURL` re-runs `parseURL` on every call to `parsed()` (no caching). The `scheme()` method does its own colon-splitting via `splitPrefixTo` rather than going through `parseURL`. A cached `mutable std::optional<ParsedURL>` would consolidate the two paths.

- **`MakeError` macro** appears in five files in this shard: `hash.hh` (`BadHash`), `url.hh` (`BadURL`), `compression.hh` (`CompressionError`), `compression-algo.hh` (`UnknownCompressionMethod`), with the macro itself living elsewhere. Useful pattern; consistent.

- **String <-> typed-enum round-trips** for `HashAlgorithm`, `HashFormat`, `CompressionAlgo` plus the `NLOHMANN_JSON_SERIALIZE_ENUM` macro instance for `CompressionAlgo` show three different ways to do the same job (manual switch with `assert`/`unreachable`, X-macro generated, nlohmann macro). `hash.cc` would benefit from converting to either of the macro-driven approaches used in `compression-algo.cc` / `compression-settings.cc`.

- **Settings glue duplication**: `compression-settings.cc` provides `BaseSetting<CompressionAlgo>::parse` and a near-identical `BaseSetting<std::optional<CompressionAlgo>>::parse` with a pre-empty-check; both also need a `trait` specialization to disable `appendable` and a `to_string`. These four specializations are mostly cut-and-pasted from each other and could be macro-generated.

- **Two flavors of Source/path conversion in `url.cc`**: `pathToUrlPath` and `urlPathToPath` round-trip filesystem paths through URL path vectors with explicit Windows handling (drive letter, UNC paths). Similar logic is reproduced inside `fixGitURL` (the `is_absolute` branch only goes one direction). Worth noting because `tryParseScpStyle`'s SCP-bracket handling overlaps with `Authority::parse` IPv6 logic (which goes through `boost::urls::parse_authority`), so two parallel IPv6 paths exist in the same file.

- **Macros that open `nlohmann`**: `JSON_IMPL`, `JSON_IMPL_WITH_XP_FEATURES`, and `NIX_DECLARE_CONFIG_SERIALISER` (used in `compression-settings.hh`) all open a nested namespace block via the macro body. This keeps the header pleasant but means search tools won't see the `adl_serializer` specialization at the macro site.

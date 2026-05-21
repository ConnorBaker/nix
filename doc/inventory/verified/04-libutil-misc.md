# Inventory — Shard 04: libutil misc / platform

This shard covers libutil header-only utility abstractions (chunked-vector, lru-cache, pool, ref, sort, sync, finally, callback, fmt, etc.), miscellaneous .cc files, the vendored widechar_width.h, and all platform-specific implementations under unix/, linux/, freebsd/, and windows/.

## File: src/libutil/include/nix/util/bump-memory-resource.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `BumpMemoryResource` (class, derives from `std::pmr::memory_resource`) — thread-safe bump allocator that overcommits memory and frees on destruction.
  - private members: `void * base = nullptr`, `std::size_t capacity = 0`, `std::pmr::memory_resource * upstreamResource`, cache-line aligned `std::atomic<std::size_t> offset{}`.
  - protected `do_allocate(bytes, alignment)` — overrides `memory_resource::do_allocate`.
  - protected `do_deallocate(p, bytes, alignment)` — overrides; no-op until destructor.
  - protected `do_is_equal(other) const noexcept` — identity comparison (inline).
  - public static constexpr `defaultReserveSize` — 8 GiB on 64-bit, 64 MiB on 32-bit.
  - explicit constructor `(reserveSize = defaultReserveSize, upstream = std::pmr::new_delete_resource())`.
  - move/copy constructors and assignment operators deleted.
  - virtual destructor.

## File: src/libutil/bump-memory-resource.cc

### File-local helpers
- `canOvercommitLinux()` (static, Linux-only) — reads `/proc/sys/vm/overcommit_memory` and tests policy != 2; returns false on read failure.
- `checkRlimit(reserveSize)` (static, non-Windows) — checks `RLIMIT_AS` headroom (reserve must be <= 1/16 of soft limit).
- `canOvercommit(reserveSize)` (static, non-Windows) — composite predicate: 64-bit + Linux policy (cached) + rlimit headroom.

### Member functions defined here
- `BumpMemoryResource::BumpMemoryResource(reserveSize, upstream)` — uses `mmap` with `MAP_PRIVATE | (MAP_ANON or MAP_ANONYMOUS) | MAP_NORESERVE` when overcommit allowed; on Windows or when overcommit denied/mmap failed, leaves `base = nullptr` and falls back to upstream.
- `BumpMemoryResource::~BumpMemoryResource()` — `munmap` of base (POSIX only).
- `BumpMemoryResource::do_allocate(bytes, alignment)` — CAS bump-pointer over `offset` with `alignUp`, falls back to upstream on overflow or when `base == nullptr`.
- `BumpMemoryResource::do_deallocate(p, bytes, alignment)` — no-op (with comment explaining that upstream also defers).

## File: src/libutil/include/nix/util/compute-levels.hh

### Namespaces
- `nix`

### Functions
- `computeLevels()` — returns `StringSet` of x86_64 micro-architecture feature levels (v1..v4) supported by the CPU.

## File: src/libutil/compute-levels.cc

### Functions
- `computeLevels()` (when `HAVE_LIBCPUID`) — calls `cpu_identify` then collects `x86_64-v1`..`x86_64-v4` feature-level strings.
- `computeLevels()` (when `!HAVE_LIBCPUID`) — returns empty `StringSet`.

## File: src/libutil/include/nix/util/english.hh

### Namespaces
- `nix`

### Functions
- `pluralize(output, count, single, plural) -> std::ostream &` — writes `"1 single"` or `"{count} plural"` to output and returns it.

## File: src/libutil/english.cc

### Functions
- `pluralize(output, count, single, plural) -> std::ostream &` — implementation as described.

## File: src/libutil/include/nix/util/hilite.hh

### Namespaces
- `nix`

### Functions
- `hiliteMatches(s, matches, prefix, postfix) -> std::string` — wraps regex match ranges with prefix/postfix, merging overlapping matches into a single wrap.

## File: src/libutil/hilite.cc

### Functions
- `hiliteMatches(s, matches, prefix, postfix)` — fast-paths empty matches; sorts matches by `position()`; greedily merges contiguous/overlapping matches before emitting prefix/match/postfix; appends remaining tail.

## File: src/libutil/include/nix/util/pos-idx.hh

### Namespaces
- `nix`, `std`

### Classes / structs / enums
- `PosIdx` (class) — opaque wrapper over `uint32_t id`; befriends `LazyPosAccessors`, `PosTable`, `std::hash<PosIdx>`.
  - private explicit `PosIdx(uint32_t id)`.
  - public default constructor (`id = 0`).
  - explicit `operator bool() const` — true if `id > 0`.
  - `operator<=>(PosIdx other) const` — auto-deduced ordering on `id`.
  - `operator==(PosIdx other) const`.
  - `hash() const noexcept` — wraps `std::hash<uint32_t>`.

### Globals
- `noPos` (`inline PosIdx`) — sentinel "no position" (default-constructed).

### `std` specializations
- `std::hash<nix::PosIdx>` — `operator()(nix::PosIdx) const noexcept` forwards to `PosIdx::hash()`.

## File: src/libutil/include/nix/util/pos-table.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `PosTable` (class) — manages mapping from `PosIdx` (byte offset) back to `Pos` (line/column + origin).
  - nested public `Origin` (class; befriends `PosTable`) — wraps `Pos::Origin` plus offset & size.
    - private `uint32_t offset`.
    - private constructor `(Pos::Origin origin, uint32_t offset, size_t size)`.
    - public `const Pos::Origin origin`, `const size_t size`.
    - `offsetOf(PosIdx p) const -> uint32_t` — returns `p.id - 1 - offset`.
  - private type aliases: `Lines = std::vector<uint32_t>`, `LinesCache = LRUCache<uint32_t, Lines>`.
  - private members: `Sync<std::map<uint32_t, Origin>> origins_`, mutable `Sync<LinesCache> linesCache`.
  - private `resolve(PosIdx p) const -> const Origin *` — `upper_bound(idx)` then `prev` over the origins map.
  - public constructor `(linesCacheCapacity = 65536)`.
  - `addOrigin(Pos::Origin origin, size_t size) -> Origin` — registers a new source contiguously after previous origins; returns a stub `Origin{...,0}` on offset overflow.
  - `add(const Origin & origin, size_t offset) -> PosIdx` — converts (origin, offset) to a `PosIdx`; returns default `PosIdx()` if offset exceeds size.
  - `operator[](PosIdx p) const -> Pos` — expensive lookup that fills a per-origin line cache.
  - `originOf(PosIdx p) const -> Pos::Origin` — returns `std::monostate{}` if unresolved.
  - `clear()` — empties caches and origins (under separate locks).

## File: src/libutil/pos-table.cc

### Member functions
- `PosTable::operator[](PosIdx p) const` — uses `Pos::LinesIterator` to compute line offsets, populates `LRUCache` keyed by origin offset, then `upper_bound`s for line/column (1-based).

## File: src/libutil/include/nix/util/position.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `Pos` (struct) — line/column + Origin variant.
  - members: `uint32_t line = 0`, `uint32_t column = 0`, `Origin origin = std::monostate()`.
  - nested `Stdin` (struct) — `ref<std::string> source`; equality + spaceship by source contents.
  - nested `String` (struct) — `ref<std::string> source`; equality + spaceship by source contents.
  - typedef `Origin = std::variant<std::monostate, Stdin, String, SourcePath>`.
  - default and `(uint32_t line, uint32_t column, Origin)` constructors.
  - explicit `operator bool() const` — `line > 0`.
  - implicit conversion `operator std::shared_ptr<const Pos>() const`.
  - `getSource() const -> std::optional<std::string>`.
  - `print(std::ostream & out, bool showOrigin) const`.
  - `getCodeLines() const -> std::optional<LinesOfCode>`.
  - defaulted `operator==` and `operator<=>` (member).
  - `getSnippetUpTo(const Pos & end) const -> std::optional<std::string>`.
  - `getSourcePath() const -> std::optional<SourcePath>`.
  - nested `LinesIterator` (struct) — input iterator over lines, supports `\r`, `\n`, `\r\n`.
    - typedefs: `difference_type = size_t`, `value_type = std::string_view`, `reference = const std::string_view &`, `pointer = const std::string_view *`, `iterator_category = std::input_iterator_tag`.
    - default constructor (past-end).
    - explicit `LinesIterator(std::string_view input)`.
    - `operator++()`, `operator++(int)`, `operator*() const`, `operator->() const`, `operator==`, `operator!=`.
    - private `bump(bool atFirst)`.

### Functions
- `operator<<(std::ostream &, const Pos &)` — invokes `pos.print(str, /*showOrigin=*/true)`.

## File: src/libutil/position.cc

### Member functions
- `Pos::operator std::shared_ptr<const Pos>() const` — `make_shared<const Pos>(*this)`.
- `Pos::getCodeLines() const` — uses `LinesIterator` to extract `prevLineOfCode` / `errLineOfCode` / `nextLineOfCode`.
- `Pos::getSource() const` — visits `Origin` variant (`monostate -> nullopt`, `Stdin/String -> string(c_str)`, `SourcePath -> readFile()` swallowing exceptions).
- `Pos::getSourcePath() const` — `get_if<SourcePath>` on origin.
- `Pos::print(std::ostream &, bool showOrigin) const` — emits `«none»` / `«stdin»` / `«string»` / path, then `:line[:column]`.
- `Pos::LinesIterator::bump(bool atFirst)` — advances past `\r`, `\n`, or `\r\n`, then sets `curLine` to the next line view.
- `Pos::getSnippetUpTo(const Pos & end) const` — returns substring spanning `[this, end]` lines, asserting same origin.
- `operator<<(std::ostream &, const Pos &)`.

## File: src/libutil/include/nix/util/suggestions.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `Suggestion` (class) — `int distance`, `std::string suggestion` (both public).
  - `to_string() const -> std::string`.
  - defaulted `operator==` and `operator<=>`.
- `Suggestions` (class) — public `std::set<Suggestion> suggestions`.
  - `to_string() const -> std::string`.
  - `trim(int limit = 5, int maxDistance = 2) const -> Suggestions`.
  - static `bestMatches(const StringSet & allMatches, std::string_view query) -> Suggestions`.
  - `operator+=(const Suggestions & other) -> Suggestions &`.
- `OrSuggestions<T>` (class template) — variant of `T` or `Suggestions`.
  - public `using Raw = std::variant<T, Suggestions>;` and public `Raw raw`.
  - `operator->() -> T *`, `operator*() -> T &` (via `std::get<T>`), `operator bool() const noexcept` (holds T).
  - converting constructor `OrSuggestions(T t)`, default constructor (raw = empty `Suggestions`).
  - static `failed(const Suggestions & s) -> OrSuggestions<T>`, static `failed() -> OrSuggestions<T>`.
  - `getSuggestions() -> const Suggestions &` — returns static `noSuggestions` if T variant is held.

### Functions
- `levenshteinDistance(std::string_view first, std::string_view second) -> int`.
- `operator<<(std::ostream &, const Suggestion &) -> std::ostream &`.
- `operator<<(std::ostream &, const Suggestions &) -> std::ostream &`.

## File: src/libutil/suggestions.cc

### Functions
- `levenshteinDistance(...)` — two-row dynamic programming.
- `Suggestions::bestMatches(allMatches, query)` — computes Levenshtein distance against each candidate.
- `Suggestions::trim(limit, maxDistance)` — keeps the first `limit` entries with distance <= `maxDistance` (relies on set's ordering by distance).
- `Suggestion::to_string()` — wraps in `ANSI_WARNING`/`ANSI_NORMAL` after `filterANSIEscapes`.
- `Suggestions::to_string()` — empty/single/`"one of A, B, ... or Z"` form.
- `Suggestions::operator+=(other)` — set-insert merge.
- `operator<<(std::ostream &, const Suggestion &)`, `operator<<(std::ostream &, const Suggestions &)`.

## File: src/libutil/include/nix/util/table.hh

### Namespaces
- `nix`

### Type aliases
- `Table = std::vector<std::vector<std::string>>` (typedef).

### Functions
- `printTable(std::ostream & out, Table & table)` — column-aligned text table printer.

## File: src/libutil/table.cc

### Functions
- `printTable(out, table)` — computes per-column max widths, replaces `\n` with space, pads with two-space gutter; asserts that every row has the same column count.

## File: src/libutil/include/nix/util/terminal.hh

### Namespaces
- `nix`

### Functions
- `isTTY(Descriptor fd) -> bool` — terminal predicate by fd.
- `isTTY() -> bool` — composite predicate (cached): stderr is a TTY, `TERM != "dumb"`, no `NO_COLOR`/`NOCOLOR`.
- `filterANSIEscapes(s, filterAll = false, width = numeric_limits<unsigned>::max()) -> std::string` — printable truncation; preserves color CSI sequences when `filterAll == false`; expands tabs.
- `updateWindowSize()` — re-reads window size into the global cache.
- `getWindowSize() -> std::pair<unsigned short, unsigned short>` (rows, cols).
- `getPtsName(int fd) -> std::string` — pseudoterminal slave name (Unix only).

## File: src/libutil/terminal.cc

### File-local helpers (anonymous namespace)
- `charWidthUTF8Helper(s) -> std::pair<int width, size_t bytes>` — UTF-8 decode + `widechar_wcwidth`, with fallbacks for ambiguous (1) / widened-in-9 (2) / negative (0).

### Functions
- `isTTY(Descriptor fd)` — `isatty` on Unix, `GetConsoleMode` on Windows (with `isatty` macro = `_isatty`).
- `isTTY()` — cached static.
- `filterANSIEscapes(s, filterAll, width)` — handles CSI, OSC (terminated by ESC `\` or BEL), tabs (expanded modulo 8), `\r`/`\a` discarded, UTF-8 width.
- `windowSize` (static; intentionally leaked) — `Sync<std::pair<unsigned short, unsigned short>> *`.
- `updateWindowSize()` — `ioctl(2, TIOCGWINSZ)` / `GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE))`.
- `getWindowSize()` — copies under lock.
- `getPtsName(int fd)` (Unix-only) — `ptsname` (macOS, mutex-guarded) or `ptsname_r` with 64-byte buffer.

## File: src/libutil/include/nix/util/alignment.hh

### Namespaces
- `nix`

### Functions
- `alignUp<T>(T val, unsigned alignment) -> T` (template, requires `std::is_unsigned_v<T>`, `constexpr`) — aligns up to a power-of-two boundary; throws `Error` on overflow, asserts `std::has_single_bit(alignment)` and `alignment <= numeric_limits<T>::max()`.

## File: src/libutil/include/nix/util/array-from-string-literal.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `ArrayNoNullAdaptor<sizeWithNull>` (struct template) — public `std::array<char, sizeWithNull - 1> data` with `constexpr` constructor that strips the trailing null byte from a string literal (uses `std::copy_n`).

### Functions
- `operator""_arrayNoNull<ArrayNoNullAdaptor str>() -> std::array<char, ...>` (template UDL using a non-type template parameter) — returns `str.data`.

## File: src/libutil/include/nix/util/ansicolor.hh

### Namespaces
- `nix`

### Macros (preprocessor; defined inside namespace block)
- `ANSI_NORMAL` — `\e[0m`.
- `ANSI_BOLD` — `\e[1m`.
- `ANSI_FAINT` — `\e[2m`.
- `ANSI_ITALIC` — `\e[3m`.
- `ANSI_RED` — `\e[31;1m`.
- `ANSI_GREEN` — `\e[32;1m`.
- `ANSI_WARNING` — `\e[35;1m`.
- `ANSI_BLUE` — `\e[34;1m`.
- `ANSI_MAGENTA` — `\e[35;1m`.
- `ANSI_CYAN` — `\e[36;1m`.

## File: src/libutil/include/nix/util/callback.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `Callback<T>` (class template) — wraps a `nix::fun<void(std::future<T>)>` plus a `std::atomic_flag done = ATOMIC_FLAG_INIT`.
  - constructor `(nix::fun<void(std::future<T>)> fun)`.
  - move constructor `(Callback &&)` — `noexcept(std::is_nothrow_move_constructible_v<decltype(fun)>)`; preserves the `done` flag from the source.
  - `operator()(T && t) noexcept` — fulfils via `promise.set_value(std::move(t))` then invokes `fun(future)`; asserts not previously fired.
  - `rethrow(const std::exception_ptr & exc = std::current_exception()) noexcept` — fulfils via `promise.set_exception(exc)`; asserts not previously fired.

## File: src/libutil/include/nix/util/checked-arithmetic.hh

### Namespaces
- `nix::checked`

### Classes / structs / enums
- `DivideByZero` (class, derives from `std::exception`, private inheritance) — sentinel exception thrown by `valueWrapping` on divide-by-zero.
- `Checked<T>` (struct template, requires `std::integral`) — overflow-checked integer wrapper.
  - typedef `Inner = T`; public member `T value`.
  - default constructor; explicit value constructor.
  - defaulted copy/move constructors and copy assignment.
  - defaulted `operator<=>(Checked<T> const &) const` (returns `std::strong_ordering`).
  - `operator<=>(T const &) const -> std::strong_ordering`.
  - explicit `operator T() const`.
  - nested enum class `OverflowKind { NoOverflow, Overflow, DivByZero }`.
  - nested `Result` (class) — private `T value`, `OverflowKind overflowed_`.
    - constructors `(T value, bool overflowed)` (maps to `Overflow`/`NoOverflow`) and `(T value, OverflowKind)`.
    - `operator==(Result other) const`.
    - `valueChecked() const -> std::optional<T>` — `nullopt` on any overflow.
    - `valueWrapping() const -> T` — throws `DivideByZero` on `DivByZero`; otherwise returns `value`.
    - `overflowed() const -> bool`, `divideByZero() const -> bool`.
  - `operator+`, `operator-`, `operator*` — both `Checked<T>` and raw-`T` overloads; use `__builtin_add/sub/mul_overflow`.
  - `operator/` — both overloads; signed `MIN / -1` returns `{minV, true}`, divisor `0` returns `{0, DivByZero}`, otherwise wraps `value / other`.

### Functions
- `operator<<(std::ostream &, Checked<T>) -> std::ostream &` (template) — prints inner value.

## File: src/libutil/include/nix/util/chunked-vector.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `ChunkedVector<T, ChunkSize, MaxChunks>` (class template) — concurrent append/read indexable container with stable refs.
  - `static_assert` that `T` is not over-aligned for default `::operator new`.
  - private cache-line aligned `std::atomic<uint32_t> size_ = 0`.
  - private type alias `ChunksArray = std::array<std::atomic<T *>, MaxChunks>`.
  - private `std::unique_ptr<ChunksArray> chunks = std::make_unique<ChunksArray>()`.
  - private `allocChunk()` `[[gnu::noinline]]` — `::operator new(ChunkSize * sizeof(T))`.
  - private `ensureChunk(size_t chunkIdx)` `[[gnu::noinline]]` — `compare_exchange_strong`-installs a fresh chunk; reuses other thread's chunk on CAS loss.
  - private `numActiveChunks() const noexcept`.
  - default constructor.
  - copy/move constructors and assignments deleted.
  - destructor — destroys constructed elements per chunk (when not trivially destructible) and `::operator delete`s.
  - `size() const noexcept -> uint32_t` — relaxed load.
  - `add(Args && ...) -> std::pair<T &, uint32_t>` (variadic template) — relaxed `fetch_add`, placement-new at slot.
  - `operator[](uint32_t idx) const & noexcept -> const T &` — unchecked, acquire-load.
  - `forEach(Fn fn) const` (template) — non-concurrent iteration over all stored elements.

## File: src/libutil/include/nix/util/closure.hh

### Namespaces
- `nix`

### Type aliases
- `GetEdgesAsync<T> = fun<asio::awaitable<std::set<T>>(const T & elt)>`.

### Classes / structs / enums
- (anonymous, inside `computeClosure`) `State` (struct, derives from `std::enable_shared_from_this<State>`) — implements hand-rolled async graph traversal with bounded concurrency.
  - members: `Executor executor`, `GetEdgesAsync<T> getEdges`, `Handler handler`, `std::set<T> & res`, `asio::executor_work_guard<Executor> workGuard`, `std::size_t pending = 0`, `std::size_t inFlight = 0`, `std::size_t maxConcurrent = 1024`, `std::queue<T> todo`, `std::exception_ptr error`.
  - constructor `(Executor, GetEdgesAsync<T>, Handler, std::set<T> &)`.
  - methods: `complete(std::exception_ptr)`, `fail(std::exception_ptr)`, `enqueue(const std::set<T> &)`, `spawnWorker(const T &)`, `onWorkDone()`, `enqueue(const T &)`.

### Functions
- `computeClosure<T, CompletionToken>(std::set<T> startElts, std::set<T> & res, GetEdgesAsync<T> getEdges, CompletionToken token)` (template) — async closure traversal returning via `asio::async_initiate<CompletionToken, void(std::exception_ptr)>`.
- `computeClosure<T>(std::set<T> startElts, std::set<T> & res, GetEdgesAsync<T> getEdges)` (template, sync) — runs an `asio::io_context` until the async variant completes; rethrows exceptions captured in the completion handler.

## File: src/libutil/include/nix/util/comparator.hh

### Macros
- `GENERATE_ONE_CMP(PRE, RET, QUAL, COMPARATOR, MY_TYPE, ...)` — generates a single comparison operator that lexicographically compares tied fields via `std::tie`.
- `GENERATE_EQUAL(prefix, qualification, my_type, args...)` — wraps `GENERATE_ONE_CMP` for `==` returning `bool`.
- `GENERATE_SPACESHIP(prefix, ret, qualification, my_type, args...)` — wraps `GENERATE_ONE_CMP` for `<=>`.
- `GENERATE_CMP(args...)` — emits both `==` and `<=>`, no template prefix, `auto` return for spaceship.
- `GENERATE_CMP_EXT(prefix, ret, my_type, args...)` — out-of-class definitions with template prefix and explicit return type for spaceship.

## File: src/libutil/include/nix/util/deleter.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `Deleter<auto del>` (struct template) — wraps a free-function-pointer non-type template parameter for use with `std::unique_ptr`'s deleter slot. `template<typename T> void operator()(T * p) const` calls `del(p)`.

## File: src/libutil/include/nix/util/demangle.hh

### Namespaces
- `nix`

### Functions
- `demangle(const char * name) -> std::string` (inline) — wraps `abi::__cxa_demangle`; falls back to `std::string(name)` on failure; `std::free`s the demangled buffer on success.

## File: src/libutil/include/nix/util/finally.hh

### Classes / structs / enums (no namespace; top-level in global)
- `Finally<Fn>` (class template, `[[nodiscard("Finally values must be used")]]`) — runs `fun()` at scope exit unless moved-from.
  - private `Fn fun`, `bool movedFrom = false`.
  - constructor `(Fn fun)`.
  - copy constructor `(Finally &)` deleted.
  - move constructor `noexcept(std::is_nothrow_move_constructible_v<Fn>)` — sets `other.movedFrom = true`.
  - destructor `noexcept(false)` — protects against exceptions during stack unwinding via `std::uncaught_exceptions()`; asserts and rethrows if `fun` throws while another exception is in flight.

## File: src/libutil/include/nix/util/fmt.hh

### Namespaces
- `nix`

### Functions
- `formatHelper<F>(F & f)` (inline template, base case).
- `formatHelper<F, T, Args...>(F & f, const T & x, const Args &... args)` (inline template, recursive) — `static_assert`s neither `T` nor any of `Args` is `std::filesystem::path`.
- `setExceptions(boost::format & fmt)` (inline) — disables `too_many_args_bit` and `too_few_args_bit`.
- `fmt(const std::string & s)` (inline) — identity overload.
- `fmt(std::string_view s)` (inline) — copies to `std::string`.
- `fmt(const char * s)` (inline) — identity overload.
- `fmt<Args...>(const std::string & fs, const Args &... args)` (inline template) — applies `boost::format` with `setExceptions` and `formatHelper`.

### Classes / structs / enums
- `Magenta<T>` (struct template) — wraps a const reference (`const T & value`); printed in magenta via `operator<<`.
- `PathFmt` (struct) — `explicit PathFmt(const std::filesystem::path & p)` constructor that stores UTF-8 string in `value` (Windows uses `u8string` cast through `reinterpret_cast`); printed quoted.
- `Uncolored<T>` (struct template) — wraps a const reference (`const T & value`); printed without coloring (resets to normal).
- `HintFmt` (class) — colors interpolated args magenta by default.
  - private `boost::format fmt`.
  - constructor `(const std::string & literal)` — formats literally via `Uncolored`.
  - static `fromFormatString(const std::string & format) -> HintFmt`.
  - constructor `(const std::string & format, const Args &... args)` (template).
  - copy constructor.
  - constructor `(boost::format && fmt, const Args &... args)` (template) — rejects any `std::filesystem::path` arg via `static_assert`; calls `setExceptions` and `formatHelper(*this, args...)`.
  - `operator%(const std::filesystem::path &)` deleted.
  - `operator%(const T & value)` (template) — wraps `value` in `Magenta`.
  - `operator%(const Uncolored<T> & value)` (template) — passes `value.value` through.
  - defaulted copy assignment (`HintFmt & operator=(HintFmt const & rhs) = default`).
  - `str() const -> std::string`.

### Operators
- `operator<<(std::ostream &, const Magenta<T> &)` (template) — writes `ANSI_WARNING ... ANSI_NORMAL`.
- `operator<<(std::ostream &, const PathFmt &)` (inline) — writes `"value"` (quoted).
- `operator<<(std::ostream &, const Uncolored<T> &)` (template) — emits `ANSI_NORMAL` first.
- `operator<<(std::ostream &, const HintFmt &)` — declared (definition lives elsewhere).

## File: src/libutil/include/nix/util/fun.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `fun<Sig>` (class template, primary, undefined).
- `fun<Ret(Args...)>` (partial specialization) — non-nullable wrapper around `std::function<Ret(Args...)>`.
  - private `std::function<Ret(Args...)> f`.
  - private `assertCallable()` — throws `std::invalid_argument("null callable cast to fun")` if `f` is null.
  - typedef `result_type = Ret`.
  - generic converting constructor (template, requires not `fun`/`std::function<Ret(Args...)>`/`std::nullptr_t` and constructible).
  - explicit constructor `(const std::function<Ret(Args...)> &)`.
  - explicit constructor `(std::function<Ret(Args...)> &&)`.
  - `fun(std::nullptr_t) = delete`.
  - `operator()(Ts && ... args) const -> Ret` (template).
  - `get_fn() const & -> const std::function<Ret(Args...)> &`, `get_fn() && -> std::function<Ret(Args...)>`.

## File: src/libutil/include/nix/util/lru-cache.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `LRUCache<Key, Value, Compare = std::less<>>` (class template) — single-thread LRU.
  - private `size_t capacity`.
  - private nested `LRUIterator` (struct) — wraps `LRU::iterator it` for circular-dependency dance.
  - private type aliases: `Data = std::map<Key, std::pair<LRUIterator, Value>, Compare>`, `LRU = std::list<typename Data::iterator>`.
  - private members `Data data`, `LRU lru`.
  - private `promote(LRU::iterator it)` — splices node to end of `lru`.
  - public constructor `(size_t capacity)`.
  - `upsert<K>(const K & key, const Value & value)` (template) — evicts oldest at capacity.
  - `erase<K>(const K & key) -> bool` (template) — true if entry removed.
  - `get<K>(const K & key) -> std::optional<Value>` (template) — promotes on hit.
  - `getOrNullptr<K>(const K & key) -> Value *` (template) — promotes on hit; returns mutable pointer.
  - `size() const noexcept -> size_t`.
  - `clear() noexcept`.

## File: src/libutil/include/nix/util/memo.hh

### Namespaces
- `nix`

### Functions
- `memo<T>(fun<T()> f) -> fun<T()>` (template) — defines a private `State { fun<T()> compute; std::once_flag flag; std::optional<T> cached; }` shared via `std::shared_ptr`; first call computes via `std::call_once`, subsequent calls return the cached value; thread-safe; copies of the returned `fun` share the same `State`.

## File: src/libutil/include/nix/util/pool.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `Pool<R>` (class template) — bounded resource pool.
  - typedef `Factory = fun<ref<R>()>`.
  - typedef `Validator = fun<bool(const ref<R> &)>`.
  - private nested `State` (struct) — `size_t inUse = 0`, `size_t max`, `std::vector<ref<R>> idle`.
  - private members: `Factory factory`, `Validator validator`, `Sync<State> state`, `std::condition_variable wakeup`.
  - public constructor `(size_t max = numeric_limits<size_t>::max(), const Factory & factory = []{ return make_ref<R>(); }, const Validator & validator = [](ref<R>){ return true; })`.
  - `incCapacity()`, `decCapacity()`.
  - destructor — asserts `inUse == 0`; clears `idle`.
  - nested `Handle` (class) — RAII for borrowed instances.
    - private `Pool & pool`, `std::shared_ptr<R> r`, `bool bad = false` (befriends `Pool`).
    - private constructor `(Pool & pool, std::shared_ptr<R>)`.
    - move constructor `noexcept` (with `static_assert`s on `shared_ptr` noexcept guarantees).
    - copy constructor deleted.
    - destructor — pushes back to idle (unless `bad`), notifies one waiter.
    - `operator->() -> R *`, `operator*() -> R &`.
    - `markBad()`.
  - `get() -> Handle` — waits for slot, validates idle entries on stack-pop, falls back to `factory()` outside the lock with rollback on factory exceptions.
  - `count() -> size_t` (idle + inUse), `capacity() -> size_t`.
  - `flushBad()` — removes invalid idle entries (by re-validating each).
  - `clear() -> std::vector<ref<R>>` — exchanges idle with empty and returns previous.

## File: src/libutil/include/nix/util/ref.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `bad_ref_cast` (class, derives from `std::bad_cast`) — carries error message via `what()`; constructor `(std::string msg)`.
- `RefImplicitlyUpcastableTo<From, To>` (concept) — `std::is_convertible_v<From*, To*>`.
- `ref<T>` (class template) — non-nullable wrapper around `std::shared_ptr<T>`.
  - private `std::shared_ptr<T> p`.
  - private `assertNonNull()` — throws `std::invalid_argument("null pointer cast to ref")`.
  - typedef `element_type = T`.
  - explicit constructors `(const std::shared_ptr<T> &)`, `(std::shared_ptr<T> &&)`, `(T *)`.
  - `get() const -> T *`, `operator->() const -> T *`, `operator*() const -> T &`.
  - `get_ptr() const & -> std::shared_ptr<T>`, `get_ptr() && -> std::shared_ptr<T>`.
  - implicit conversion `operator std::shared_ptr<T>(this auto && self)` (deducing-this) via `get_ptr()`.
  - `cast<T2>() const -> ref<T2>` — `dynamic_pointer_cast` or throws `bad_ref_cast` with demangled type names.
  - `dynamic_pointer_cast<T2>() const -> std::shared_ptr<T2>`.
  - implicit covariance `operator ref<T2>() const` (template, requires `RefImplicitlyUpcastableTo<T, T2>`).
  - `operator==`, `operator!=`, `operator<=>`.
  - friends `make_ref<T2>(Args && ...)`.

### Functions
- `make_ref<T>(Args && ... args) -> ref<T>` (template, inline) — wraps `std::make_shared<T>`.

## File: src/libutil/include/nix/util/repair-flag.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `RepairFlag` (enum with bool underlying type via `: bool`) — values `NoRepair = false`, `Repair = true`. (Not `enum class` — note: it is unscoped.)

## File: src/libutil/include/nix/util/sort.hh

### Namespaces
- `nix`

### Functions
- `mergeSortedRunsInPlace<Iter, BufIter, Comparator>(Iter begin, Iter middle, Iter end, BufIter workingBegin, Comparator comp = {})` (template; `Iter` forward iterator, `BufIter` random-access) — stable in-place merge through a working buffer; safe for non-strict-weak comparators (uses `!comp(*right, *left)` to keep left-bias).
- `insertionsort<Iter, Comparator>(Iter begin, Iter end, Comparator comp = {})` (template; bidirectional iterator) — weak exception safety; not necessarily noexcept under throwing comparator/move-assign.
- `strictlyDecreasingPrefix<Iter, Comparator>(Iter begin, Iter end, const Comparator & comp = {})` (template; forward iterator).
- `strictlyDecreasingSuffix<Iter, Comparator>(Iter begin, Iter end, const Comparator & comp = {})` (template; bidirectional iterator).
- `weaklyIncreasingPrefix<Iter, Comparator>(Iter begin, Iter end, const Comparator & comp = {})` (template) — forwards to `strictlyDecreasingPrefix(..., std::not_fn(comp))`.
- `weaklyIncreasingSuffix<Iter, Comparator>(Iter begin, Iter end, const Comparator & comp = {})` (template) — forwards to `strictlyDecreasingSuffix(..., std::not_fn(comp))`.
- `peeksort<Iter, Comparator>(Iter begin, Iter end, Comparator comp = {})` (template, `Iter` random-access, requires `std::is_default_constructible_v<value_type>`) — stable natural mergesort with PeekSort dispatch (16-element insertion-sort cutoff); MIT-licensed adaptation of Sebastian Wild's powersort.

## File: src/libutil/include/nix/util/std-hash.hh

### Namespaces
- `nix`

### Functions
- `hash_combine(std::size_t & seed)` (inline, base case, no-op).
- `hash_combine<T, Rest...>(std::size_t & seed, const T & v, Rest... rest)` (inline template) — Boost-style combiner using the magic constant `0x9e3779b9`, recurses on remaining args.

## File: src/libutil/include/nix/util/topo-sort.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `Cycle<T>` (struct template) — public `T path`, `T parent` representing a detected cycle.

### Type aliases
- `TopoSortResult<T> = std::variant<std::vector<T>, Cycle<T>>`.

### Functions
- `topoSort<T, Compare, F>(std::set<T, Compare> items, const F & getChildren) -> TopoSortResult<T>` (template; `F` `std::invocable<const T &>` and must return `std::set<T, Compare>`) — DFS topological sort (using a hand-rolled `fun<...>` recursive lambda) that returns the reversed sorted vector or the first detected cycle; only follows children that are in the original `items` set.

## File: src/libutil/include/nix/util/variant-wrapper.hh

### Macros
- `FORCE_DEFAULT_CONSTRUCTORS(CLASS_NAME)` — forces defaulted copy/move/copy-assign constructors (including the rare `CLASS_NAME(CLASS_NAME &) = default;` non-const-ref overload).
- `MAKE_WRAPPER_CONSTRUCTOR(CLASS_NAME)` — emits `FORCE_DEFAULT_CONSTRUCTORS` plus a perfect-forwarding template constructor that initializes `raw` (excluding the single-argument self-conversion case).

## File: src/libutil/widecharwidth/widechar_width.h

Vendored third-party. Source: https://github.com/ridiculousfish/widecharwidth/ (Unicode 16.0.0). Wrapped in anonymous namespace.

### Top-level public symbols
- anonymous unnamed `enum` — special width values: `widechar_nonprint = -1`, `widechar_combining = -2`, `widechar_ambiguous = -3`, `widechar_private_use = -4`, `widechar_unassigned = -5`, `widechar_widened_in_9 = -6`, `widechar_non_character = -7`.
- `widechar_range` (struct) — `uint32_t lo, hi`.
- static range tables: `widechar_ascii_table`, `widechar_private_table`, `widechar_nonprint_table`, `widechar_combining_table`, `widechar_combiningletters_table`, `widechar_doublewide_table`, `widechar_ambiguous_table`, `widechar_unassigned_table`, `widechar_nonchar_table`, `widechar_widened_table`.
- `widechar_in_table<Collection>(const Collection & arr, uint32_t c) -> bool` (template) — binary-search predicate (`std::lower_bound`) over a sorted range table.
- `widechar_wcwidth(uint32_t c) -> int` — returns 1, 2, or one of the negative `widechar_*` enum values.

## File: src/libutil/unix/current-process.cc

### Namespaces
- `nix`

### Functions
- `getCpuUserTime() -> std::chrono::microseconds` — returns `RUSAGE_SELF` user time via `getrusage`; throws `SysError` on failure.

## File: src/libutil/unix/environment-variables.cc

### Globals
- `extern char ** environ __attribute__((weak));` (file scope, not in `nix::`).

### Namespaces
- `nix`

### Functions
- `setEnv(const char * name, const char * value) -> int` — wraps `::setenv(name, value, 1)`.
- `getEnvOs(const std::string & key) -> std::optional<std::string>` — forwards to `getEnv(key)` (Unix: `OsString == std::string`).
- `getEnvOs() -> OsStringMap` — walks `environ` to populate the map; skips entries without `=`.
- `getEnv() -> StringMap` — same as `getEnvOs()` (typed as `StringMap`).
- `setEnvOs(const OsString & name, const OsString & value) -> int` — wraps `setEnv(name.c_str(), value.c_str())`.
- `unsetEnvOs(const OsChar * name) -> int` — wraps `::unsetenv(name)`.

## File: src/libutil/unix/file-descriptor.cc

### Namespaces
- `nix` (with `nix::unix::*` definitions written as fully qualified `unix::` from inside `namespace nix`).

### Functions
- `getFileSize(Descriptor fd) -> std::make_unsigned_t<off_t>` — reads `st.st_size` via `nix::fstat`.
- `read(Descriptor fd, std::span<std::byte> buffer) -> size_t` — interruptible `::read`, retries on `EINTR`.
- `readOffset(Descriptor fd, off_t offset, std::span<std::byte> buffer) -> size_t` — interruptible `::pread`.
- `write(Descriptor fd, std::span<const std::byte> buffer, bool allowInterrupts) -> size_t` — `::write`; only interruptible when `allowInterrupts` is true.
- `dupDescriptor(Descriptor fd) -> AutoCloseFD` — uses `fcntl(F_DUPFD_CLOEXEC, 0)`.
- `Pipe::create(bool nonBlocking)` — uses `pipe2(O_CLOEXEC | (nonBlocking ? O_NONBLOCK : 0))` if available, else `pipe + closeOnExec + fcntl(F_SETFL, O_NONBLOCK)`.
- `unix_close_range(unsigned first, unsigned last, int flags) -> int` (static, Linux/FreeBSD only) — wraps `close_range` (or syscall fallback).
- `unix::closeExtraFDs()` — closes all FDs above stderr; tries `close_range`, then falls back to walking `/proc/self/fd` (Linux) or a `sysconf(_SC_OPEN_MAX)` loop.
- `unix::closeOnExec(int fd)` — sets `FD_CLOEXEC`.
- `syncDescriptor(Descriptor fd)` — `F_FULLFSYNC` on Apple, `fsync` elsewhere; throws `NativeSysError` on failure.
- `unix::SelfPipe::create()` — non-blocking pipe.
- `unix::SelfPipe::notify()` — writes one byte; tolerates `EAGAIN`/`EINTR`.
- `unix::SelfPipe::drain()` — drains 128-byte buffer until `EAGAIN`.

## File: src/libutil/unix/file-path.cc

### Namespaces
- `nix`

### Functions
- `toOwnedPath(PathView path) -> std::filesystem::path` — `PathView` -> `std::filesystem::path` via `std::string` round-trip (POSIX paths are bytes).

## File: src/libutil/unix/file-system-at.cc

### Namespaces
- `nix` (with nested `nix::linux` block on Linux)
- definitions of `nix::unix::fchmodatTryNoFollow` use the fully qualified `unix::` prefix from inside `namespace nix`.

### Macros
- `HAVE_OPENAT2` — `1` iff Linux + `__NR_openat2`.
- `HAVE_FCHMODAT2` — `1` iff Linux + `__NR_fchmodat2`.

### Functions
- `linux::openat2(Descriptor dirFd, const char * path, uint64_t flags, uint64_t mode, uint64_t resolve) -> std::optional<AutoCloseFD>` (Linux) — direct `__NR_openat2` syscall with caching of `ENOSYS` via `std::atomic_flag`; returns `nullopt` if syscall is unavailable.
- `unix::fchmodatTryNoFollow(Descriptor dirFd, const CanonPath & path, mode_t mode)` — tries `fchmodat2` syscall (Linux) with `ENOSYS` caching; falls back via `O_PATH | O_NOFOLLOW` and `/proc/self/fd/...` chmod (also caching missing `/proc`); final fallback to `fchmodat` (with `AT_SYMLINK_NOFOLLOW` only on non-Linux). Throws `SysError(EOPNOTSUPP, ...)` when target is a symlink. `warnOnce` if both fchmodat2 and `/proc` are unavailable on Linux.
- `openFileEnsureBeneathNoSymlinksIterative(Descriptor dirFd, const CanonPath & path, int flags, mode_t mode, std::function<void(AutoCloseFD, CanonPath)> dirFdCallback) -> AutoCloseFD` (static) — component-by-component `openat` walk with `O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`, plus `O_PATH` (Linux) and `O_RESOLVE_BENEATH` (FreeBSD); converts symlink encounters to `SymlinkNotAllowed`; invokes `dirFdCallback` on each intermediate directory.
- `openFileEnsureBeneathNoSymlinks(Descriptor dirFd, const CanonPath & path, int flags, mode_t mode, std::function<void(AutoCloseFD, CanonPath)> dirFdCallback) -> AutoCloseFD` — adjusts `O_PATH`-without-`O_DIRECTORY` to add `O_NOFOLLOW`, attempts `linux::openat2` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS` if available; otherwise delegates to the iterative walk.
- `readLinkAt(Descriptor dirFd, const CanonPath & path) -> OsString` — grows buffer (starting at `PATH_MAX/4`) until short read.
- `fstat(Descriptor fd) -> PosixStat` — wraps `::fstat`.
- `fstatat(Descriptor dirFd, const std::filesystem::path & path) -> PosixStat` — wraps `::fstatat` with `AT_SYMLINK_NOFOLLOW`.
- `maybeFstatat(Descriptor dirFd, const std::filesystem::path & path) -> std::optional<PosixStat>` — like `fstatat`, returns `nullopt` on `ENOENT`/`ENOTDIR`.

## File: src/libutil/unix/file-system.cc

### Namespaces
- `nix`

### Macros
- `MOUNTEDPATHS_PARAM` / `MOUNTEDPATHS_ARG` — file-local; expand to `, std::set<std::filesystem::path> & mountedPaths` / `, mountedPaths` on FreeBSD, empty otherwise.

### Functions
- `openDirectory(const std::filesystem::path & path, FinalSymlink finalSymlink) -> AutoCloseFD` — `O_RDONLY | O_DIRECTORY | O_CLOEXEC | (Follow ? 0 : O_NOFOLLOW)`.
- `openFileReadonly(const std::filesystem::path & path, FinalSymlink finalSymlink) -> AutoCloseFD` — `O_RDONLY | O_CLOEXEC` plus optional `O_NOFOLLOW`.
- `openNewFileForWrite(const std::filesystem::path & path, mode_t mode, OpenNewFileForWriteParams params) -> AutoCloseFD` — applies `params.writeOnly`, `truncateExisting` (`O_TRUNC` + optional `O_NOFOLLOW`), or `O_EXCL`.
- `descriptorToPath(Descriptor fd) -> std::filesystem::path` — readlinks `/proc/self/fd/<fd>` (Linux) or `fcntl(F_GETPATH)` (Apple) with `<stdin>`/`<stdout>`/`<stderr>` sentinels; falls back to `<fd N>`.
- `defaultTempDir() -> std::filesystem::path` — `getEnvOsNonEmpty("TMPDIR")` or `/tmp`.
- `lstat(const std::filesystem::path &) -> PosixStat`, `maybeLstat(const std::filesystem::path &) -> std::optional<PosixStat>` — wrappers; `maybeLstat` returns nullopt on `ENOENT`/`ENOTDIR`.
- `setWriteTime(const std::filesystem::path &, time_t accessedTime, time_t modificationTime, std::optional<bool> optIsSymlink)` — uses `utimensat(AT_FDCWD, ..., AT_SYMLINK_NOFOLLOW)` if available; else `lutimes`; else `utimes` (refusing symlinks via `is_symlink` check or supplied hint).
- `_deletePath(Descriptor parentfd, const std::filesystem::path & path, uint64_t & bytesFreed, std::exception_ptr & ex [, std::set<...> & mountedPaths])` (static) — recursive in-place delete; counts size bytes for nlink in {1, 2}; `fchmodatTryNoFollow` to make directories accessible; uses `unlinkat` with `AT_REMOVEDIR`. Avoids descending into mount points on FreeBSD.
- `_deletePath(const std::filesystem::path & path, uint64_t & bytesFreed [, std::set<...> & mountedPaths])` (static) — opens parent dir then delegates; rethrows accumulated exception.
- `deletePath(const std::filesystem::path & path)` — single-arg overload using local `bytesFreed`.
- `deletePath(const std::filesystem::path & path, uint64_t & bytesFreed)` — populates `bytesFreed`; on FreeBSD reads `getmntinfo(MNT_WAIT)` to skip mount points.
- `chown(const std::filesystem::path & path, uid_t owner, gid_t group)` — wraps `::chown`.

## File: src/libutil/unix/muxable-pipe.cc

### Namespaces
- `nix`

### Member functions
- `MuxablePipePollState::poll(std::optional<unsigned int> timeout)` — wraps `::poll(pollStatus.data(), pollStatus.size(), ...)`; returns silently on `EINTR`.
- `MuxablePipePollState::iterate(std::set<CommChannel> & channels, fun<void(Descriptor, std::string_view)> handleRead, fun<void(Descriptor)> handleEOF)` — reads up to 4 KiB per active fd (looking up via `fdToPollStatus`); treats `EIO` as pty close (EOF heuristic); erases EOF channels.

## File: src/libutil/unix/processes.cc

### Namespaces
- `nix`

### Type aliases (file-local)
- `ChildWrapperFunction = fun<void()>`.

### Member functions
- `Pid::Pid()` (default).
- `Pid::Pid(Pid && other) noexcept` — copies fields from `other` and calls `other.release()`.
- `Pid::Pid(pid_t pid)`.
- `Pid::~Pid()` — kills if alive (`allowInterrupts=false`); ignores exceptions via `ignoreExceptionInDestructor`.
- `Pid::operator=(pid_t pid)` (returns `void`) — kills previous if different; resets `killSignal` to `SIGKILL`.
- `Pid::operator pid_t()` (implicit; non-const).
- `Pid::kill(bool allowInterrupts) -> int` — sends `killSignal` (process group if `separatePG`); spawns watchdog thread for `SIGKILL` escalation after `killTimeout` (only when signal != `SIGKILL` and timeout > 0); tolerates `EPERM` from BSD/Apple zombie groups.
- `Pid::wait(bool allowInterrupts) -> int` — `waitpid` loop with `EINTR` retry.
- `Pid::setSeparatePG(bool)`, `Pid::setKillSignal(int)`, `Pid::setKillTimeout(std::chrono::milliseconds)`.
- `Pid::release() -> pid_t` — leaks pid (no kill on dtor); resets remaining fields by move-assigning a default-constructed `Pid()`.

### File-local helpers
- `doFork(bool allowVfork, ChildWrapperFunction & fun) -> pid_t` (`__attribute__((noinline))`, static) — `vfork()` (Linux + allowVfork) or `fork()`; in child runs `fun()` then `unreachable()`.
- `childEntry(void * arg) -> int` (Linux only, static) — clone entry trampoline that calls `fun()` and returns 1.

### Functions
- `killUser(uid_t uid)` — forks, `setuid`, then loops `kill(-1, SIGKILL)` (Apple uses `SYS_kill` syscall) until `ESRCH`/`EPERM`; throws on non-zero status.
- `startProcess(fun<void()> processMain, const ProcessOptions & options) -> pid_t` — supports `cloneFlags` (Linux) with `mmap`-allocated `MAP_STACK` (1 MiB; `Finally` for stack cleanup); honors `dieWithParent` via `prctl(PR_SET_PDEATHSIG, SIGKILL)` (Linux); replaces logger with simple one (unless vfork); converts thrown exception to stderr line and `exit(1)`/`_exit(1)`.
- `runProgram(std::filesystem::path program, bool lookupPath, const OsStrings & args, bool isInteractive) -> std::string` — convenience around `runProgram(RunOptions)`; throws `ExecError` on non-zero status.
- `runProgram2(const RunOptions & options)` — full lifecycle: pipe, fork (vfork allowed iff no env override), env replacement, dup2 stdout/stderr, optional `chdir`/`setgid`/`setgroups(0,0)`/`setuid`, `restoreProcessContext`, `execvp`/`execv`.
- `statusToString(int status) -> std::string` — exit code or signal description (uses `strsignal` if available); else `"died abnormally"` or `"succeeded"`.
- `statusOk(int status) -> bool` — `WIFEXITED && WEXITSTATUS == 0`.
- `execvpe(const char * file0, const char * const argv[], const char * const envp[]) -> int` — uses `ExecutablePath::load().findPath(file0)` then `execve` (with `const_cast` per POSIX spec note).

## File: src/libutil/unix/signals.cc

### Namespaces
- `nix` (with `nix::unix::*` definitions written as fully qualified `unix::` from inside `namespace nix`).

### Globals (file-local definitions)
- `unix::_isInterrupted` — `std::atomic<bool>`, definition (defined `false`).
- `unix::interruptCheck` — `thread_local std::function<bool()>` (definition).
- `savedSignalMask` (static `sigset_t`).
- `savedSignalMaskIsSet` (static `bool`).

### Classes / structs
- `InterruptCallbacks` (file-local struct) — `typedef int64_t Token;` `Token nextToken = 0;` `std::map<Token, fun<void()>> callbacks;`.
- `InterruptCallbackImpl` (file-local, derives from `InterruptCallback`) — RAII de-registration; explicitly deletes copy/move constructors/assignment; destructor erases the registered token under lock.

### Functions
- `unix::_interrupted()` — throws `Interrupted("interrupted by the user")` unless inside another exception (`std::uncaught_exceptions()`).
- `getInterruptCallbacks() -> Sync<InterruptCallbacks> &` (static, file-local) — Construct-On-First-Use leaked singleton.
- `signalHandlerThread(sigset_t set)` (static, file-local) — `sigwait` loop dispatching `SIGINT/SIGTERM/SIGHUP -> triggerInterrupt`, `SIGWINCH -> updateWindowSize` (note: `SIGPIPE` is blocked but not handled).
- `unix::triggerInterrupt()` — sets `_isInterrupted` and invokes registered callbacks under iteration that releases lock between calls.
- `unix::saveSignalMask()` — captures current `sigprocmask`.
- `unix::startSignalHandlerThread()` — calls `updateWindowSize`, `saveSignalMask`, blocks SIGINT/SIGTERM/SIGHUP/SIGPIPE/SIGWINCH, then detaches `signalHandlerThread` thread.
- `unix::restoreSignals()` — restores saved mask if set.
- `createInterruptCallback(fun<void()> callback) -> std::unique_ptr<InterruptCallback>` — registers callback under unique token, returns RAII handle.

## File: src/libutil/unix/users.cc

### Namespaces
- `nix`

### Functions
- `getUserName() -> std::string` — `getpwuid(geteuid())->pw_name` or `$USER`; throws if both empty.
- `getHomeOf(uid_t userId) -> std::filesystem::path` — `getpwuid_r` with 16384-byte buffer; throws if no entry / no `pw_dir`.
- `getHome() -> std::filesystem::path` — cached: prefers `$HOME` if owned by current user (via `maybeStat` + `geteuid` check); falls back to `getHomeOf(geteuid())`; warns once on `$HOME` ownership mismatch or stat failure.
- `isRootUser() -> bool` — `getuid() == 0`.

## File: src/libutil/unix/xdg-dirs.cc

### Namespaces
- `nix::unix::xdg`

### Functions
- `getCacheHome() -> std::filesystem::path` — `$XDG_CACHE_HOME` or `getHome() / ".cache"`.
- `getConfigHome() -> std::filesystem::path` — `$XDG_CONFIG_HOME` or `getHome() / ".config"`.
- `getConfigDirs() -> std::vector<std::filesystem::path>` — colon-split `$XDG_CONFIG_DIRS` or `/etc/xdg`.
- `getDataHome() -> std::filesystem::path` — `$XDG_DATA_HOME` or `getHome() / ".local" / "share"`.
- `getStateHome() -> std::filesystem::path` — `$XDG_STATE_HOME` or `getHome() / ".local" / "state"`.

## File: src/libutil/unix/xdg-dirs.hh

Private header (not installed). Declares the same five functions in `nix::unix::xdg` (`getCacheHome`, `getConfigHome`, `getConfigDirs`, `getDataHome`, `getStateHome`) — all returning `std::filesystem::path` except `getConfigDirs` which returns `std::vector<std::filesystem::path>`.

## File: src/libutil/unix/include/nix/util/monitor-fd.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `MonitorFdHup` (class) — RAII watcher that triggers `unix::triggerInterrupt()` when an fd hangs up.
  - private members `std::thread thread`, `Pipe notifyPipe`.
  - private `runThread(int watchFd, int notifyFd)` (inline, defined in this header) — kqueue-based on Apple (uses `EVFILT_READ` for both fds; treats `EV_EOF` on `watchFd` as hangup -> trigger interrupt); poll-based elsewhere (waits for `POLLHUP`).
  - constructor `MonitorFdHup(int fd)` (inline) — creates pipe, spawns thread.
  - copy/move constructors and assignments deleted.
  - destructor — closes write side of notify pipe, joins thread.

## File: src/libutil/unix/include/nix/util/signals-impl.hh

### Namespaces
- `nix`, `nix::unix`

### Globals (extern declarations)
- `unix::_isInterrupted` — `std::atomic<bool>`.
- `unix::interruptCheck` — `thread_local std::function<bool()>`.

### Functions
- `unix::_interrupted()`, `unix::startSignalHandlerThread()`, `unix::saveSignalMask()`, `unix::restoreSignals()`, `unix::triggerInterrupt()` — declarations.
- `setInterrupted(bool)` (inline `static`) — sets `unix::_isInterrupted`.
- `getInterrupted() -> bool` (inline `static`) — reads atomic.
- `isInterrupted() -> bool` (inline `static`) — composite check using `interruptCheck`.
- `checkInterrupt()` (inline) — throws via `unix::_interrupted()` if interrupted.

### Classes / structs / enums
- `ReceiveInterrupts` (struct) — public `pthread_t target`, public `std::unique_ptr<InterruptCallback> callback`; default constructor captures `pthread_self()` and registers a callback that `pthread_kill`s the captured thread with `NIX_SIG_MULTI_INT`.

## File: src/libutil/linux/cgroup.cc

### Namespaces
- `nix::linux`

### Functions
- `getCgroupFS() -> std::optional<std::filesystem::path>` — caches result; reads `/proc/mounts` via `getmntent`, returns first `cgroup2` mount.
- `getCgroups(const std::filesystem::path & cgroupFile) -> StringMap` — parses `N:controllers:path` lines via `std::regex`, stripping `name=` prefix.
- `getCgroupStats(const std::filesystem::path & cgroup) -> CgroupStats` — reads `cpu.stat`, extracts `user_usec` / `system_usec` into microseconds.
- `destroyCgroup(const std::filesystem::path & cgroup, bool returnStats) -> CgroupStats` (static) — recursive; uses `cgroup.kill` if available; otherwise `SIGKILL`s every PID in `cgroup.procs` (and recursively in subdirectories), backing off with `2^min(round, 10)` ms sleeps (max 20 rounds, prints stray builder cmdlines once each, logs warnings after 100 ms); finally `rmdir`s the cgroup.
- `destroyCgroup(const std::filesystem::path & cgroup) -> CgroupStats` — wraps with `returnStats = true`.
- `getCurrentCgroup() -> CanonPath` — derives unified cgroup path from `/proc/self/cgroup` (uses key `""` from the parsed map).
- `getRootCgroup() -> CanonPath` — cached `getCurrentCgroup()`.

## File: src/libutil/linux/include/nix/util/cgroup.hh

### Namespaces
- `nix::linux`

### Classes / structs / enums
- `CgroupStats` (struct) — public `std::optional<std::chrono::microseconds> cpuUser, cpuSystem;`.

### Functions
- `getCgroupFS() -> std::optional<std::filesystem::path>` — declaration.
- `getCgroups(const std::filesystem::path & cgroupFile) -> StringMap` — declaration.
- `getCgroupStats(const std::filesystem::path & cgroup) -> CgroupStats` — declaration.
- `destroyCgroup(const std::filesystem::path & cgroup) -> CgroupStats` — declaration.
- `getCurrentCgroup() -> CanonPath`, `getRootCgroup() -> CanonPath` — declarations.

## File: src/libutil/linux/linux-namespaces.cc

### Namespaces
- `nix`

### Globals (file-local)
- `fdSavedMountNamespace` (static `AutoCloseFD`).
- `fdSavedRoot` (static `AutoCloseFD`).

### Functions
- `userNamespacesSupported() -> bool` — cached predicate; checks `/proc/self/ns/user`, `/proc/sys/user/max_user_namespaces`, `/proc/sys/kernel/unprivileged_userns_clone`, then probes `CLONE_NEWUSER` via `startProcess`/`pid.wait`.
- `mountAndPidNamespacesSupported() -> bool` — cached; spawns child with `CLONE_NEWNS | CLONE_NEWPID` (+ `CLONE_NEWUSER` if available) and tries to remount `/proc` (after first making `/` private-recursive).
- `saveMountNamespace()` — `std::call_once`: opens `/proc/self/ns/mnt` and `/proc/self/root` for later restoration (root open is best-effort, no throw).
- `restoreMountNamespace()` — `setns(CLONE_NEWNS)` + `fchdir(savedRoot)` + `chroot(".")` + restore CWD; logs error on failure (`debug`, swallows).
- `tryUnshareFilesystem()` — `unshare(CLONE_FS)`; throws unless `EPERM` or `ENOSYS`.

## File: src/libutil/linux/include/nix/util/linux-namespaces.hh

### Namespaces
- `nix`

### Functions
- `saveMountNamespace()`, `restoreMountNamespace()`, `tryUnshareFilesystem()`, `userNamespacesSupported() -> bool`, `mountAndPidNamespacesSupported() -> bool` — declarations.

## File: src/libutil/freebsd/freebsd-jail.cc

Body wrapped in `#ifdef __FreeBSD__`.

### Namespaces
- `nix`

### Member functions
- `AutoRemoveJail::AutoRemoveJail(int jid)`.
- `AutoRemoveJail::remove()` — `jail_remove(jid)` (unless invalid), then `cancel()`, then unmounts each `childrenMounts` (with single retry-on-EBUSY after 1 s sleep); throws `SysError` if any unmount failed.
- `AutoRemoveJail::~AutoRemoveJail()` — calls `remove()` swallowing exceptions via `ignoreExceptionInDestructor`.
- `AutoRemoveJail::cancel() noexcept` — sets `jid = INVALID_JAIL`.

## File: src/libutil/freebsd/include/nix/util/freebsd-jail.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `AutoRemoveJail` (class) — RAII jail handle.
  - private `static constexpr int INVALID_JAIL = -1;`.
  - public `int jid = INVALID_JAIL`, `std::vector<std::filesystem::path> childrenMounts`.
  - default constructor and `(int jid)` constructor.
  - copy constructor and assignment deleted.
  - move constructor `noexcept` (copies `jid`, calls `other.cancel()`).
  - move assignment `noexcept` (uses free `swap`).
  - friend `swap(AutoRemoveJail &, AutoRemoveJail &) noexcept` (free function via ADL; only swaps `jid`).
  - implicit `operator int() const`.
  - destructor.
  - `remove()` — declaration.
  - `cancel() noexcept` — declaration.

## File: src/libutil/windows/current-process.cc

### Namespaces
- `nix`

### Functions
- `getCpuUserTime() -> std::chrono::microseconds` — wraps `GetProcessTimes(GetCurrentProcess(), ...)` and converts FILETIME 100-ns ticks to microseconds; throws `windows::WinError` on failure.

## File: src/libutil/windows/environment-variables.cc

### Namespaces
- `nix`

### Functions
- `getEnvOs(const OsString & key) -> std::optional<OsString>` — wraps `GetEnvironmentVariableW` (size probe + read, with explicit non-uniform-init buffer).
- `getEnvOs() -> OsStringMap` — walks `GetEnvironmentStringsW` block (RAII via `std::unique_ptr` with `FreeEnvironmentStringsW` deleter), splitting at `=`.
- `getEnv() -> StringMap` — converts `OsStringMap` via `os_string_to_string`.
- `unsetenv(const char * name) -> int` — wraps `SetEnvironmentVariableA(name, nullptr)`; returns negated bool.
- `unsetEnvOs(const OsChar * name) -> int` — wraps `SetEnvironmentVariableW(name, nullptr)`; returns negated bool.
- `setEnv(const char * name, const char * value) -> int` — wraps `SetEnvironmentVariableA`; returns negated bool.
- `setEnvOs(const OsString & name, const OsString & value) -> int` — wraps `SetEnvironmentVariableW`; returns negated bool.

## File: src/libutil/windows/file-descriptor.cc

### Namespaces
- `nix`

### Functions
- `getFileSize(Descriptor fd) -> std::make_unsigned_t<off_t>` — `GetFileSizeEx`.
- `read(Descriptor fd, std::span<std::byte> buffer) -> size_t` — `checkInterrupt` then `ReadFile`; treats `ERROR_BROKEN_PIPE` as EOF.
- `readOffset(Descriptor fd, off_t offset, std::span<std::byte> buffer) -> size_t` — `OVERLAPPED`-based `ReadFile` with `Offset`/`OffsetHigh` split.
- `write(Descriptor fd, std::span<const std::byte> buffer, bool allowInterrupts) -> size_t` — `WriteFile`; only checks interrupts when `allowInterrupts` true.
- `dupDescriptor(Descriptor fd) -> AutoCloseFD` — `DuplicateHandle(GetCurrentProcess(), ..., DUPLICATE_SAME_ACCESS)`.
- `Pipe::create()` — `CreatePipe` with `bInheritHandle = TRUE` security attributes (no `nonBlocking` argument; differs from Unix overload).
- `lseek(HANDLE h, off_t offset, int whence) -> off_t` — translates POSIX `whence` to `SetFilePointerEx` method; sets `errno` from `GetLastError` on failure (returning `-1`).
- `syncDescriptor(Descriptor fd)` — `FlushFileBuffers`.

## File: src/libutil/windows/file-path.cc

### Namespaces
- `nix`

### Functions
- `maybePath(PathView path) -> std::optional<std::filesystem::path>` — accepts `<drive>:<sep>...` (prefixed with `\\?\`) or `\\?\<drive>:<sep>...` / `\\.\<drive>:<sep>...`; normalizes `/` to `\\`; otherwise returns empty `optional`.
- `toOwnedPath(PathView path) -> std::filesystem::path` — wraps `maybePath`; on failure writes message to `std::wcerr` and `_exit(111)` (FIXME comment notes the lack of regular error handling).

## File: src/libutil/windows/file-system-at.cc

### Namespaces
- `nix`, `nix::windows`

### Classes / structs / enums (in `nix::windows` anonymous namespace)
- `ReparseDataBuffer` (struct) — Windows DDK reparse-data layout (vendored, justified by LLVM use).
  - `unsigned long ReparseTag`, `unsigned short ReparseDataLength`, `unsigned short Reserved`.
  - union of `SymbolicLinkReparseBuffer` (with `Flags`), `MountPointReparseBuffer`, `GenericReparseBuffer`.

### Functions
- `windows::ntOpenAt(Descriptor dirFd, std::wstring_view pathComponent, ACCESS_MASK desiredAccess, ULONG createOptions, ULONG createDisposition = FILE_OPEN) -> AutoCloseFD` (file-local in anonymous namespace) — wraps `NtCreateFile` with `OBJECT_ATTRIBUTES.RootDirectory = dirFd` and `FILE_SYNCHRONOUS_IO_NONALERT`; throws `WinError(RtlNtStatusToDosError(...))` on failure.
- `windows::openSymlinkAt(Descriptor dirFd, const CanonPath & path) -> AutoCloseFD` (file-local) — opens with `FILE_READ_ATTRIBUTES | SYNCHRONIZE` and `FILE_OPEN_REPARSE_POINT`.
- `windows::readSymlinkTarget(HANDLE linkHandle) -> OsString` (file-local) — issues `FSCTL_GET_REPARSE_POINT` via `DeviceIoControl`, validates `ReparseTag == IO_REPARSE_TAG_SYMLINK`, extracts `PrintName` (or `SubstituteName` if `PrintNameLength == 0`).
- `windows::isReparsePoint(HANDLE handle) -> bool` (file-local) — `GetFileInformationByHandleEx(FileBasicInfo)` and tests `FILE_ATTRIBUTE_REPARSE_POINT`.
- `fstat(Descriptor fd) -> PosixStat` — `GetFileInformationByHandle` -> `windows::statFromFileInfo` (with `nNumberOfLinks`).
- `openFileEnsureBeneathNoSymlinks(Descriptor dirFd, const CanonPath & path, ACCESS_MASK desiredAccess, ULONG createOptions, ULONG createDisposition, std::function<void(AutoCloseFD, CanonPath)> dirFdCallback) -> AutoCloseFD` — component-by-component `ntOpenAt` walk (with `FILE_TRAVERSE | SYNCHRONIZE`, `FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT`); throws `SymlinkNotAllowed` on reparse points (also probed on `ERROR_CANT_ACCESS_FILE`/`ERROR_ACCESS_DENIED`); final open uses `createOptions | FILE_OPEN_REPARSE_POINT` and a post-open reparse-point check. `dirFdCallback` is currently unused (FIXME).
- `readLinkAt(Descriptor dirFd, const CanonPath & path) -> OsString` — opens via `windows::openSymlinkAt`, reads via `windows::readSymlinkTarget`.

## File: src/libutil/windows/file-system.cc

### Namespaces
- `nix` (with `nix::windows::*` definitions written as fully qualified `windows::` from inside `namespace nix`).

### Static asserts
- `(S_IFLNK & S_IFMT) == S_IFLNK`.
- `S_IFLNK != S_IFDIR`, `S_IFLNK != S_IFREG`, `S_IFLNK != S_IFCHR` — guards polyfill for `S_IFLNK`.

### Functions
- `setWriteTime(const std::filesystem::path & path, time_t accessedTime, time_t modificationTime, std::optional<bool> optIsSymlink)` — currently stubbed; logs a `warn` (FIXME notes future use of `std::filesystem::last_write_time`).
- `openDirectory(const std::filesystem::path & path, FinalSymlink finalSymlink) -> AutoCloseFD` — `CreateFileW(GENERIC_READ, ..., OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | optional FILE_FLAG_OPEN_REPARSE_POINT)`.
- `openFileReadonly(const std::filesystem::path & path, FinalSymlink finalSymlink) -> AutoCloseFD` — `CreateFileW(GENERIC_READ, ...)` plus optional `FILE_FLAG_OPEN_REPARSE_POINT`.
- `openNewFileForWrite(const std::filesystem::path & path, mode_t mode, OpenNewFileForWriteParams params) -> AutoCloseFD` — `CreateFileW(GENERIC_WRITE | optional GENERIC_READ, ..., CREATE_ALWAYS or CREATE_NEW)`. (Reparse points TODO.)
- `defaultTempDir() -> std::filesystem::path` — `GetTempPathW(MAX_PATH+1, ...)`.
- `deletePath(const std::filesystem::path & path)` — `std::filesystem::remove_all` swallowing `no_such_file_or_directory`.
- `deletePath(const std::filesystem::path & path, uint64_t & bytesFreed)` — sets `bytesFreed = 0` and delegates.
- `descriptorToPath(Descriptor handle) -> std::filesystem::path` — `GetFinalPathNameByHandleW(FILE_NAME_OPENED)` with size retry; `<stdin>`/`<stdout>`/`<stderr>` sentinels via `GetStdHandle`; falls back to `boost::wformat("<unnnamed handle %X>")`.
- `windows::fileTimeToUnixTime(const FILETIME & ft) -> time_t` — `file_clock::time_point` -> `system_clock` cast via `std::chrono::clock_cast` and `to_time_t`.
- `windows::statFromFileInfo(PosixStat & st, DWORD dwFileAttributes, const FILETIME & ftCreationTime, const FILETIME & ftLastAccessTime, const FILETIME & ftLastWriteTime, DWORD nFileSizeHigh, DWORD nFileSizeLow, DWORD nNumberOfLinks)` — fills `PosixStat` from Win32 attributes (encodes reparse points as `S_IFLNK | 0777`, dirs as `S_IFDIR | 0755`, regular files as `S_IFREG | 0644`); zeroes `st_uid`/`st_gid`.
- `statFromFileInfo(const WIN32_FILE_ATTRIBUTE_DATA &) -> PosixStat` (static, file-local) — adapter overload for `GetFileAttributesExW`-style data; calls `windows::statFromFileInfo` without `nNumberOfLinks` (defaults to zero).
- `lstat(const std::filesystem::path & path) -> PosixStat` — `GetFileAttributesExW`; throws `WinError` on failure.
- `maybeLstat(const std::filesystem::path & path) -> std::optional<PosixStat>` — like `lstat`, returns `nullopt` on `ERROR_FILE_NOT_FOUND`/`ERROR_PATH_NOT_FOUND`.

## File: src/libutil/windows/known-folders.cc

### Namespaces
- `nix::windows::known_folders`

### Functions
- `getKnownFolder(REFKNOWNFOLDERID rfid) -> std::filesystem::path` (static, file-local) — wraps `SHGetKnownFolderPath` with `Finally` for `CoTaskMemFree` cleanup; throws `WinError(static_cast<DWORD>(res))` on failure.
- `getLocalAppData() -> std::filesystem::path` — cached `FOLDERID_LocalAppData`.
- `getRoamingAppData() -> std::filesystem::path` — cached `FOLDERID_RoamingAppData`.
- `getProgramData() -> std::filesystem::path` — cached `FOLDERID_ProgramData`.

## File: src/libutil/windows/muxable-pipe.cc

### Namespaces
- `nix`

### Member functions
- `MuxablePipePollState::poll(HANDLE ioport, std::optional<unsigned int> timeout)` — `GetQueuedCompletionStatusEx(..., 0x20, &removed, ...)`; tolerates `WAIT_TIMEOUT` (asserts `removed == 0`); otherwise asserts `0 < removed <= 0x20`.
- `MuxablePipePollState::iterate(std::set<CommChannel> & channels, fun<void(Descriptor, std::string_view)> handleRead, fun<void(Descriptor)> handleEOF)` — matches IOCP entries to channel keys via `key ^ 0x5555`; reissues `ReadFile` (overlapped); recognizes `gotEOF` and `ERROR_BROKEN_PIPE`; tolerates `ERROR_IO_PENDING`.

## File: src/libutil/windows/os-string.cc

### Namespaces
- `nix`

### Functions
- `os_string_to_string(OsStringView s) -> std::string` — UTF-16 -> UTF-8 via `std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>`.
- `os_string_to_string(OsString s) -> std::string` — overload delegating to `OsStringView`.
- `string_to_os_string(std::string_view s) -> OsString` — UTF-8 -> UTF-16.
- `string_to_os_string(std::string s) -> OsString` — overload delegating to `string_view`.
- `tokenizeString<C>(OsStringView s, OsStringView separators) -> C` (Windows-only template; wraps `basicTokenizeString<C, OsChar>`) — explicit instantiations for `std::list<OsString>` and `std::vector<OsString>`.

## File: src/libutil/windows/processes.cc

### Namespaces
- `nix`

### Member functions
- `Pid::Pid()` (default).
- `Pid::Pid(Pid && other) noexcept` — moves the underlying `AutoCloseFD`; does not explicitly clear `other.pid` (the move of `AutoCloseFD` handles that).
- `Pid::Pid(AutoCloseFD pid)` — moves in.
- `Pid::~Pid()` — calls `kill()` if `pid.get() != INVALID_DESCRIPTOR`.
- `Pid::operator=(AutoCloseFD pid)` (returns `void`) — kills previous if different handle.
- `Pid::kill(bool allowInterrupts) -> int` — `TerminateProcess(pid, 1)` (logs error on failure), then `wait`.
- `Pid::wait(bool allowInterrupts) -> int` — `WaitForSingleObject(INFINITE)` + `GetExitCodeProcess`; closes handle. `allowInterrupts` is unused (TODO comment matches Unix shape).

### Functions
- `runProgram(std::filesystem::path program, bool lookupPath, const OsStrings & args, bool isInteractive) -> std::string` — same shape as Unix.
- `getProgramInterpreter(const std::filesystem::path & program) -> std::optional<std::filesystem::path>` — short-circuits `.exe`, `.cmd`, `.bat`; otherwise throws `UnimplementedError` (shebang reading TODO).
- `setFDInheritable(AutoCloseFD & fd, bool inherit)` — `SetHandleInformation(HANDLE_FLAG_INHERIT, ...)`.
- `nullFD() -> AutoCloseFD` — opens `NUL` device with read/write/share rights and marks inheritable.
- `windowsEscape(const OsString & str, bool cmd) -> OsString` — adapted from MSDN twistylittlepassages; throws `UnimplementedError` if `cmd == true`.
- `spawnProcess(const std::filesystem::path & realProgram, const RunOptions & options, Pipe & out) -> Pid` — `CreateProcessW(... CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED)`; on success creates a `JobObject`, assigns the child to it, then `ResumeThread` (job object ensures children die with parent). Build environment line via `getEnvOs` plus overrides; cmdline via `windowsEscape`.
- `runProgram2(const RunOptions & options)` — pipe + `getProgramInterpreter` + `spawnProcess` + drain + wait; throws `ExecError` on non-zero status.
- `statusToString(int status) -> std::string` — `"with exit code N"` or `"succeeded"`.
- `statusOk(int status) -> bool` — `status == 0`.
- `execvpe(const wchar_t * file0, const wchar_t * const argv[], const wchar_t * const envp[]) -> int` — `_wexecve` after `ExecutablePath::load().findPath`.

## File: src/libutil/windows/users.cc

### Namespaces
- `nix`

### Functions
- `getUserName() -> std::string` — `GetUserNameA(nullptr, &size)` size probe (expecting `ERROR_INSUFFICIENT_BUFFER`) followed by the actual call.
- `getHome() -> std::filesystem::path` — cached: `$USERPROFILE` or `C:\Users\Default`, run through `canonPath`.
- `isRootUser() -> bool` — always `false`.

## File: src/libutil/windows/windows-async-pipe.cc

### Namespaces
- `nix::windows`

### Member functions
- `AsyncPipe::createAsyncPipe(HANDLE iocp)` — initializes a 4 KiB `buffer` and zeroed `OVERLAPPED`; creates a uniquely named `\\.\pipe\nix-<pid>-<this>` named pipe (`PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED`); registers with completion port using key `(readSide.get()) ^ 0x5555`; calls `ConnectNamedPipe` (overlapped, tolerating `ERROR_IO_PENDING`); opens the write side via `CreateFileA` with `bInheritHandle = TRUE`.
- `AsyncPipe::close()` — closes both sides.

## File: src/libutil/windows/windows-environment.cc

### Namespaces
- `nix::windows`

### Functions
- `isWine() -> bool` — checks for `wine_get_version` exported by `ntdll.dll` (via `GetModuleHandle` + `GetProcAddress`).

## File: src/libutil/windows/windows-error.cc

### Namespaces
- `nix::windows`

### Member functions
- `WinError::renderError(DWORD lastError) -> std::string` — uses `FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS)`; cleans up via `LocalFree`; falls back to `"CODE=N"`.

## File: src/libutil/windows/include/nix/util/windows-async-pipe.hh

### Namespaces
- `nix::windows`

### Classes / structs / enums
- `AsyncPipe` (class) — IOCP-friendly named-pipe wrapper.
  - public members `AutoCloseFD writeSide, readSide`, `OVERLAPPED overlapped`, `DWORD got`, `std::vector<unsigned char> buffer`.
  - `createAsyncPipe(HANDLE iocp)` — declaration.
  - `close()` — declaration.

## File: src/libutil/windows/include/nix/util/windows-environment.hh

### Namespaces
- `nix::windows`

### Functions
- `isWine() -> bool` — declaration.

## File: src/libutil/windows/include/nix/util/windows-known-folders.hh

### Namespaces
- `nix::windows::known_folders`

### Functions
- `getLocalAppData() -> std::filesystem::path`, `getRoamingAppData() -> std::filesystem::path`, `getProgramData() -> std::filesystem::path` — declarations.

## File: src/libutil/windows/include/nix/util/signals-impl.hh

### Namespaces
- `nix`

### Forward declarations
- `class InterruptCallback;`

### Functions (no-op stubs)
- `setInterrupted(bool)` (inline `static`) — no-op.
- `getInterrupted() -> bool` (inline `static`) — returns `false`.
- `isInterrupted() -> bool` (inline `static`) — returns `false`.
- `checkInterrupt()` (inline) — no-op.
- `createInterruptCallback(fun<void()> callback) -> std::unique_ptr<InterruptCallback>` (inline) — returns null `unique_ptr`.

### Classes / structs / enums
- `ReceiveInterrupts` (struct) — empty stub with explicit destructor (avoids dead-code warning).

## Cross-file observations

The shard splits along a clear platform-abstraction axis: a public header in `src/libutil/include/nix/util/` (or a Unix/Windows/Linux/FreeBSD-private header next to the `.cc`) declares the contract, and one `.cc` per platform provides the implementation. The function signatures published by these headers are platform-conditional in several places (Windows takes wide strings as `OsString`, Unix takes byte strings; Windows `Pipe::create()` has no `nonBlocking` parameter; `MuxablePipePollState::poll` takes an extra `HANDLE ioport` on Windows; `Pid` holds a `pid_t` on Unix and an `AutoCloseFD` on Windows), so "parallel implementation" frequently means "same logical interface, slightly different signature".

### Logical interfaces with parallel platform implementations

- **`getCpuUserTime() -> std::chrono::microseconds`** — declared in `include/nix/util/current-process.hh` (verified via cross-namespace usage); defined in `unix/current-process.cc` (uses `getrusage(RUSAGE_SELF)`) and `windows/current-process.cc` (uses `GetProcessTimes(GetCurrentProcess(), ...)`).
- **environment variables** — declared in `include/nix/util/environment-variables.hh`; defined in `unix/environment-variables.cc` (POSIX `setenv`/`unsetenv`/`environ`) and `windows/environment-variables.cc` (`SetEnvironmentVariableW`/`GetEnvironmentVariableW`/`GetEnvironmentStringsW`). Common surface: `setEnv(const char *, const char *) -> int`, `setEnvOs(const OsString &, const OsString &) -> int`, `unsetEnvOs(const OsChar *) -> int`, `getEnv() -> StringMap`, `getEnvOs() -> OsStringMap`. Diverging surface: `getEnvOs(key)` takes `const std::string &` returning `std::optional<std::string>` on Unix vs `const OsString &` returning `std::optional<OsString>` on Windows; the `extern char ** environ` weak symbol is Unix-only; the standalone `int unsetenv(const char *)` and `Windows`'s negated-bool return convention are Windows-only quirks. On Unix `OsString == std::string`; on Windows it is `std::wstring`.
- **file descriptor I/O** — declared in `include/nix/util/file-descriptor.hh`; defined in `unix/file-descriptor.cc` (POSIX `read`/`pread`/`write`/`pipe2`/`F_DUPFD_CLOEXEC`/`fsync`/`F_FULLFSYNC`) and `windows/file-descriptor.cc` (`ReadFile`/`WriteFile` with `OVERLAPPED`, `DuplicateHandle`, `CreatePipe`, `FlushFileBuffers`). The Windows variant additionally provides `lseek` (translating POSIX semantics onto `SetFilePointerEx`); Unix has no extra `lseek` shim because POSIX already provides it. `Pipe::create` differs in arity (Unix takes `bool nonBlocking`, Windows takes none); the Unix-only `unix::closeExtraFDs`, `unix::closeOnExec`, `unix::SelfPipe::*` interfaces have no Windows analog. `read`/`readOffset`/`write` have identical signatures; `getFileSize` returns `make_unsigned_t<off_t>` on both.
- **`toOwnedPath(PathView) -> std::filesystem::path`** — `unix/file-path.cc` is a trivial copy; `windows/file-path.cc` validates and prefixes drive paths with `\\?\` plus the additional `maybePath` validator and an `_exit(111)` on invalid input.
- **`fstat` / `*at` family** — declared in `include/nix/util/file-system-at.hh`; `unix/file-system-at.cc` (uses `openat`/`fstatat`/`readlinkat` with optional Linux `openat2` and `fchmodat2` syscalls plus `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`) versus `windows/file-system-at.cc` (uses `NtCreateFile` + `OBJECT_ATTRIBUTES.RootDirectory` + `FILE_OPEN_REPARSE_POINT`, plus a vendored DDK `ReparseDataBuffer` struct in an anonymous namespace). The signature of `openFileEnsureBeneathNoSymlinks` differs: Unix takes `(int flags, mode_t mode, ...)` while Windows takes `(ACCESS_MASK desiredAccess, ULONG createOptions, ULONG createDisposition, ...)`. `fstat`, `readLinkAt`, `fstatat` are Unix-only declarations; `maybeFstatat` is Unix-only; `windows::fileTimeToUnixTime` and the `windows::statFromFileInfo` adapter are Windows-only helpers.
- **file-system operations** — declared in `include/nix/util/file-system.hh`; `unix/file-system.cc` uses POSIX `open` plus `utimensat`/`lutimes`/`utimes` and a hand-rolled recursive deleter that avoids mount points on FreeBSD; `windows/file-system.cc` uses `CreateFileW`, `GetFinalPathNameByHandleW`, `GetFileAttributesExW` with a stubbed `setWriteTime` (warns) and a `std::filesystem::remove_all`-based `deletePath`. `chown` is Unix-only; the `_deletePath` static helpers (with a FreeBSD `mountedPaths` parameter) are Unix-only.
- **`MuxablePipePollState`** (declared in `include/nix/util/muxable-pipe.hh`) — `unix/muxable-pipe.cc` uses `::poll` over `pollStatus`; `windows/muxable-pipe.cc` uses `GetQueuedCompletionStatusEx` over an internal `oentries[0x20]` IOCP buffer. The Unix `poll` overload takes only a timeout; the Windows overload takes an IOCP `HANDLE ioport` plus timeout. The struct's data members are conditionally compiled (`pollStatus`/`fdToPollStatus` on Unix vs `oentries`/`removed`/`gotEOF` on Windows) and `MuxablePipe` itself is `Pipe` on Unix vs `windows::AsyncPipe` on Windows.
- **`Pid` class + `runProgram` family + `statusToString`/`statusOk`/`execvpe`** — `unix/processes.cc` (forks, vfork, clone, waitpid, kill timeouts, signal handling) versus `windows/processes.cc` (CreateProcessW + JobObject + WaitForSingleObject; arg quoting via `windowsEscape`; `nullFD()` for stdin redirection). `Pid` holds `pid_t` on Unix and `AutoCloseFD` (process handle) on Windows. `Pid::release()`, `Pid::setSeparatePG`/`setKillSignal`/`setKillTimeout`, and `killUser`/`startProcess` are Unix-only; `setFDInheritable`/`spawnProcess`/`getProgramInterpreter`/`nullFD`/`windowsEscape` are Windows-only. `execvpe` differs in argv encoding: `const char *` on Unix vs `const wchar_t *` on Windows.
- **`getUserName()` / `getHome()` / `isRootUser()`** — declared in `include/nix/util/users.hh`; `unix/users.cc` uses `getpwuid_r`/`geteuid`; `windows/users.cc` uses `GetUserNameA` and `$USERPROFILE`. Unix-only `getHomeOf(uid_t)` provides per-user lookup. Windows always returns `isRootUser() == false`.
- **signal handling** (`checkInterrupt`, `setInterrupted`, `isInterrupted`, `createInterruptCallback`, `ReceiveInterrupts`) — declared with full machinery in `unix/include/nix/util/signals-impl.hh` (interrupt thread, sigwait, callback table) and reduced to no-op stubs in `windows/include/nix/util/signals-impl.hh`. Only Unix has `triggerInterrupt`, `startSignalHandlerThread`, `saveSignalMask`, `restoreSignals`. `ReceiveInterrupts` carries `pthread_t` + `unique_ptr<InterruptCallback>` members on Unix and is empty on Windows. The interrupt callback registry implementation lives in `unix/signals.cc`; Windows has nothing equivalent.

### Logical interfaces with no platform parallel

- **`MonitorFdHup`** (Unix only, `unix/include/nix/util/monitor-fd.hh` — implementation is inline in this header) — kqueue on Apple (uses `EVFILT_READ` for both watch fd and notify pipe), `poll(POLLHUP)` elsewhere. Triggers `unix::triggerInterrupt()` on hangup.
- **xdg dirs** (Unix only, `unix/xdg-dirs.{cc,hh}` in `nix::unix::xdg`) — counterpart on Windows is the `windows/known-folders.cc` interface using `SHGetKnownFolderPath`. Different APIs (XDG env vars + `~/...` fallbacks vs Win32 KNOWNFOLDERID), both providing per-user/per-machine config/data/state directories.
- **cgroups** (Linux only, `linux/cgroup.{cc,hh}` in `nix::linux`) — exposes `getCgroupFS`, `getCgroups`, `getCgroupStats`, `destroyCgroup`, `getCurrentCgroup`, `getRootCgroup`. Uses `getmntent`, `cpu.stat`, `cgroup.kill`, and recursive SIGKILL fallback with backoff.
- **mount/PID/user namespaces** (Linux only, `linux/linux-namespaces.{cc,hh}` in `nix::`) — `userNamespacesSupported`, `mountAndPidNamespacesSupported`, `saveMountNamespace`, `restoreMountNamespace`, `tryUnshareFilesystem`. Note the implementation lives in `nix::` (not `nix::linux::`), even though the build only compiles it on Linux.
- **FreeBSD jails** (FreeBSD only, `freebsd/freebsd-jail.{cc,hh}`) — `AutoRemoveJail` RAII type wrapping `jail_remove` and child mount unmounting.
- **Windows-only utilities** — `AsyncPipe` (named-pipe IOCP wrapper), `windows::isWine()`, `windows::WinError::renderError`, `os_string_to_string`/`string_to_os_string`, `tokenizeString` for `OsString`, the `windows::known_folders` namespace, `windowsEscape`, the `lseek(HANDLE, off_t, int)` shim.

### Pure header-only abstractions

These do not have platform splits (`.hh`-only, no `.cc`):
`alignment.hh`, `array-from-string-literal.hh`, `ansicolor.hh`, `callback.hh`, `checked-arithmetic.hh`, `chunked-vector.hh`, `closure.hh`, `comparator.hh`, `deleter.hh`, `demangle.hh`, `finally.hh`, `fmt.hh` (some functions inline; `operator<<(HintFmt)` declared but defined elsewhere), `fun.hh`, `lru-cache.hh`, `memo.hh`, `pool.hh`, `ref.hh`, `repair-flag.hh`, `sort.hh`, `std-hash.hh`, `topo-sort.hh`, `variant-wrapper.hh`. The vendored `widecharwidth/widechar_width.h` is also header-only and wrapped in an anonymous namespace.

The Unix-only `unix/include/nix/util/monitor-fd.hh` is also effectively header-only — its `runThread`, constructor, and (apple/non-apple) implementation methods are all `inline` in the header and depend on `unix::triggerInterrupt` from `signals-impl.hh`.

### Recurring patterns

- **Cache-line alignment** via `std::hardware_destructive_interference_size` in both `BumpMemoryResource::offset` and `ChunkedVector::size_` to avoid false sharing in concurrent bump/append operations.
- **`Sync<T>` (declared elsewhere)** is used as the generic mutex wrapper across platform-private state: `PosTable::origins_`, `PosTable::linesCache`, `Pool<R>::state`, the leaked `windowSize` in `terminal.cc`, the leaked `interruptCallbacks` singleton in `unix/signals.cc`. The pattern of intentionally leaking these `Sync`-protected globals (Construct On First Use) recurs to avoid static-destruction-order issues — the comment in `terminal.cc` explicitly notes this is to avoid `~ProgressBar()` calling `getWindowSize()` after `windowSize` has been destroyed.
- **`std::atomic_flag`-cached syscall availability**: both `linux::openat2` and `unix::fchmodatTryNoFollow` cache `ENOSYS`/`/proc` unavailability via `std::atomic_flag`, and a separate `std::atomic<bool> warned` (with `warnOnce`) is used for the "kernel doesn't support fchmodat2 and procfs isn't mounted" warning.
- **`noexcept` move constructors** throughout (`Callback`, `Finally`, `Pool::Handle`, `AutoRemoveJail`, `Pid`) propagate `is_nothrow_move_constructible_v` of inner types where applicable.
- **Error policy**: most platform-specific calls throw `SysError` (Unix `errno`) or `windows::WinError` (`GetLastError`); some operations have `maybe*` variants returning `std::optional` for `ENOENT`/`ENOTDIR` (`maybeLstat`, `maybeFstatat`) or `ERROR_FILE_NOT_FOUND`/`ERROR_PATH_NOT_FOUND` on Windows.
- **Hand-rolled non-nullable wrappers** — `nix::ref<T>` (over `std::shared_ptr<T>`) and `nix::fun<Sig>` (over `std::function<Sig>`) both throw `std::invalid_argument` on null construction and form a layered abstraction (e.g., `Pool<R>::Factory = fun<ref<R>()>`).
- **Caching idioms** — `static auto res = []() { ... }()` (function-local `static` with immediately-invoked lambda) is used pervasively for cached computations: `getCgroupFS`, `userNamespacesSupported`, `mountAndPidNamespacesSupported`, `getRootCgroup`, `getHome` (both Unix and Windows), `isTTY()`, the `windowSize` global in `terminal.cc`, the `getInterruptCallbacks` Construct-On-First-Use idiom.

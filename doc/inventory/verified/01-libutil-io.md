# Inventory — Shard 01: libutil IO (verified)

## File: src/libutil/archive.cc

### Namespaces
- `nix` — primary namespace; uses `static` linkage internally rather than an anonymous namespace block

### Classes / structs
- `ArchiveSettings` (struct, : `Config`) — settings holder declaring the `use-case-hack` boolean (default true on Apple, false elsewhere)
- `CaseInsensitiveCompare` (struct) — strict-weak ordering comparator using `strcasecmp`, used for the case-collision detection map during NAR parse

### Functions / member functions
- `SourceAccessor::dumpPath(const CanonPath & path, Sink & sink, PathFilter & filter)` (member fn) — writes the NAR magic prefix and recursively serialises the tree using an explicit-this lambda bounded by `narMaxDepth`; on macOS removes the case-hack suffix from directory entries
- `dumpPathAndGetMtime(const std::filesystem::path & path, Sink & sink, PathFilter & filter)` (free fn) — wraps `makeFSSourceAccessor` with mtime tracking enabled and dumps; returns the recorded last-modified time
- `dumpPath(const std::filesystem::path & path, Sink & sink, PathFilter & filter)` (free fn) — same without mtime tracking
- `dumpString(std::string_view s, Sink & sink)` (free fn) — emits a NAR for one regular file with the given contents
- `badArchive(std::string_view s, const Args &... args)` (template fn, file-static) — helper that builds a `SerialisationError` with a `"bad archive: "` prefix
- `parseContents(CreateRegularFileSink & sink, Source & source)` (static fn) — read file size, copy bytes (or skip if `skipContents`), then consume padding
- `parse(FileSystemObjectSink & sink, Source & source, const CanonPath & path, size_t depth)` (static fn) — recursive-descent NAR parser; enforces depth, sorted-entry, name-validity, case-hack-collision invariants
- `parseDump(FileSystemObjectSink & sink, Source & source)` (free fn) — read NAR magic and dispatch to `parse` at root
- `restorePath(const std::filesystem::path & path, Source & source, bool startFsync)` (free fn) — instantiate a `RestoreSink` and parse a NAR into it
- `copyNAR(Source & source, Sink & sink)` (free fn) — parse a NAR using `NullFileSystemObjectSink` while teeing raw bytes to the output sink

### Type aliases
- (none)

### Macros / globals
- `archiveSettings` (static `ArchiveSettings`) — singleton settings instance
- `rArchiveSettings` (static `GlobalConfig::Register`) — registers `archiveSettings` with the global config
- `narMaxDepth` (static constexpr `size_t`, value 64) — directory nesting bound for dump/parse to keep stack usage in coroutine frames bounded
- `defaultPathFilter` (`PathFilter`) — global lambda that always returns true; declared in `file-system.hh`, defined here

## File: src/libutil/include/nix/util/archive.hh

### Namespaces
- `nix` — primary namespace

### Functions (declarations)
- `dumpPath(const std::filesystem::path & path, Sink & sink, PathFilter & filter = defaultPathFilter)` — serialise FS path as NAR
- `dumpPathAndGetMtime(const std::filesystem::path & path, Sink & sink, PathFilter & filter = defaultPathFilter)` — same plus return mtime
- `dumpString(std::string_view s, Sink & sink)` — emit a NAR with a single regular file
- `parseDump(FileSystemObjectSink & sink, Source & source)` — read NAR magic and parse into a sink
- `restorePath(const std::filesystem::path & path, Source & source, bool startFsync = false)` — restore a NAR into the filesystem
- `copyNAR(Source & source, Sink & sink)` — pass a NAR through, validating its structure

### Type aliases
- (none)

### Macros / globals
- `narVersionMagic1` (`inline constexpr std::string_view`, "nix-archive-1") — NAR magic header
- `caseHackSuffix` (`inline constexpr std::string_view`, "~nix~case~hack~") — collision suffix used when `use-case-hack` is on

## File: src/libutil/serialise.cc

### Namespaces
- `nix`

### Classes / structs
- `SourceToSink` (struct, local to `sourceToSink`; : `FinishSink`) — runs `reader(Source&)` as a `boost::coroutines2` push coroutine, feeding incoming chunks via a `LambdaSource`
- `SinkToSource` (struct, local to `sinkToSource`; : `Source`) — runs `writer(Sink&)` as a `boost::coroutines2` pull coroutine and yields produced chunks

### Functions / member functions
- `BufferedSink::operator()(std::string_view data)` — append into internal buffer, bypass for large writes, flush when full
- `BufferedSink::flush()` — flush the internal buffer via `writeUnbuffered`
- `FdSink::~FdSink()` — flushes; swallows exceptions
- `FdSink::writeUnbuffered(std::string_view data)` — `writeFull` to the descriptor and tally bytes; marks not-good on `SystemError`
- `FdSink::good()` — returns the `_good` flag
- `Source::operator()(char * data, size_t len)` — read until exactly `len` bytes are filled
- `Source::operator()(std::string_view data)` — overload casting away const for the same loop
- `Source::drainInto(Sink & sink)` — copy all bytes until EOF using an 8 KiB stack buffer
- `Source::drainInto(Sink & sink, uint64_t len)` — copy exactly `len` bytes using a 64 KiB stack buffer with interrupt checks
- `Source::drain()` — drain into a `StringSink` and return the string
- `Source::skip(size_t len)` — read-and-discard `len` bytes via 8 KiB buffer
- `BufferedSource::read(char * data, size_t len)` — fill from buffer, refilling via `readUnbuffered` as needed
- `BufferedSource::readLine(bool eofOk, char terminator)` — buffered line reader using `memchr`
- `BufferedSource::hasData()` — `bufPosOut < bufPosIn`
- `FdSource::readUnbuffered(char * data, size_t len)` — call `nix::read`; throw `EndOfFile` on 0
- `FdSource::good()` — returns `_good`
- `FdSource::hasData()` — non-blocking poll/select to check for available data
- `FdSource::restart()` — `lseek` to start (seekable only)
- `FdSource::skip(size_t len)` — discard buffer bytes, then `lseek SEEK_CUR` if seekable, else fall back to `BufferedSource::skip`
- `StringSource::read(char * data, size_t len)` — `string_view::copy` from `pos`; throw `EndOfFile` past end
- `StringSource::skip(size_t len)` — advance `pos`; throw `EndOfFile` past end
- `sourceToSink(fun<void(Source &)> reader)` (free fn) — constructs a `SourceToSink` coroutine adapter
- `sinkToSource(fun<void(Sink &)> writer, fun<void()> eof)` (free fn) — constructs a `SinkToSource` coroutine adapter
- `writePadding(size_t len, Sink & sink)` (free fn) — pad to 8-byte boundary with zero bytes
- `writeString(std::string_view data, Sink & sink)` (free fn) — write length, bytes, padding
- `operator<<(Sink &, std::string_view)` — calls `writeString`
- `writeStrings<T>(const T & ss, Sink & sink)` (template fn) — write count + each string
- `operator<<(Sink &, const Strings &)` — calls `writeStrings<Strings>`
- `operator<<(Sink &, const StringSet &)` — calls `writeStrings<StringSet>`
- `operator<<(Sink &, const Error & ex)` — wire-encode an `Error` (level, name placeholder, message, traces)
- `readPadding(size_t len, Source & source)` (free fn) — verify and discard zero padding
- `readString(char * buf, size_t max, Source & source)` (free fn) — read length-prefixed string into buffer
- `readString(Source & source, size_t max)` (free fn) — same but returning `std::string`
- `operator>>(Source &, std::string &)` — read string from source
- `readStrings<T>(Source & source)` (template fn, with explicit `Strings`/`StringSet` instantiations) — read count + each string into `T`
- `readError(Source & source)` (free fn) — wire-decode a serialised `Error` (level, name, message, traces); rejects positions
- `StringSink::operator()(std::string_view data)` — append to internal `s`
- `ChainSource::read(char * data, size_t len)` — read from `source1` until EOF then from `source2`

### Type aliases
- (none)

### Macros / globals
- (none)

## File: src/libutil/include/nix/util/serialise.hh

### Namespaces
- `boost::context` (forward decl `struct stack_context`)
- `nix`

### Classes / structs
- `Sink` (struct) — abstract destination of binary data; pure virtual `operator()(string_view)`, virtual `good()` defaulting to true
- `NullSink` (struct, : `Sink`) — discards all data
- `FinishSink` (struct, virtual : `Sink`) — adds pure virtual `finish()` hook
- `BufferedSink` (struct, virtual : `Sink`) — owns a buffer; implements `operator()` and `flush()`; pure virtual `writeUnbuffered`
- `Source` (struct) — abstract byte source; pure virtual `read`, defines `operator()` overloads, `drainInto` overloads, `drain`, `skip`, virtual `good`
- `BufferedSource` (struct, virtual : `Source`) — buffering; `readLine`, `hasData`, pure virtual `readUnbuffered`
- `RestartableSource` (struct, virtual : `Source`) — adds pure virtual `restart()`
- `FdSink` (struct, : `BufferedSink`) — writes to a `Descriptor`; tracks `written`; deletes copy; move asn flushes
- `FdSource` (struct, : `BufferedSource, RestartableSource`) — reads from a `Descriptor`; tracks `read`, EOF message, seekability flag
- `StringSink` (struct, : `Sink`) — appends data to a `std::string s`; default/reserved-size/move-from-string ctors
- `StringSource` (struct, : `RestartableSource`) — reads from a `std::string` or `string_view`; carries optional shared owner; inline `restart()` resets `pos`
- `TeeSink` (struct, : `Sink`) — duplicates writes to two sinks
- `TeeSource` (struct, : `Source`) — copies bytes read into a `Sink`
- `SizedSource` (struct, : `Source`) — bounds reads to `remain`; `drainAll` discards the remainder
- `LengthSink` (struct, : `Sink`) — counts bytes written into `length`
- `LengthSource` (struct, : `Source`) — counts bytes read into `total`
- `LambdaSink` (struct, : `Sink`) — wraps a `fun<void(string_view)>` plus optional cleanup `fun<>`
- `LambdaSource` (struct, : `Source`) — wraps a `fun<size_t(char *, size_t)>`
- `ChainSource` (struct, : `Source`) — read from `source1` until EOF, then `source2`
- `StreamToSourceAdapter` (struct, : `Source`) — wraps a `std::shared_ptr<std::basic_istream<char>>`
- `FramedSource` (struct, : `Source`) — reads length-prefixed frames; destructor drains remaining frames; deleted copy/move
- `FramedSink` (struct, : `BufferedSink`) — writes length-prefixed frames; destructor writes terminator and flushes; calls a `checkError` callback before each write
- `EnsureRead` (struct, : `Source`) — wraps a source so that on destruction at least `bytesExpected` have been consumed; `finish()` skips the rest

### Functions / member functions (declarations and inline defs)
- `BufferedSink::BufferedSink(size_t bufSize = 32 * 1024)` (inline) — initialises buffer state
- `Source::operator()(char *, size_t)` and `(std::string_view)` — fill exactly len
- `Source::read` — pure virtual
- `Source::good` — virtual default true
- `Source::drainInto(Sink &)` and `(Sink &, uint64_t)` (latter virtual) — drain helpers
- `Source::drain()` — drain into a string
- `Source::skip(size_t)` — virtual default impl
- `BufferedSource::read`, `readLine`, `hasData` — buffered impls
- `RestartableSource::restart()` — pure virtual
- `FdSink` ctors (default-fd, fd), deleted copy, defaulted move ctor, custom move-assignment that flushes
- `FdSink::writeUnbuffered`, `FdSink::good`
- `FdSource` ctors (default, fd), deleted copy, defaulted move
- `FdSource::good`, `restart`, `hasData`, `skip`, `readUnbuffered`
- `StringSink` ctors (default, reserved size, from rvalue string) and `operator()`
- `StringSource` ctors (rvalue string, string_view, const string&), `read`, `skip`, `restart` (inline)
- `TeeSink::operator()` (inline), `TeeSource::read` (inline)
- `SizedSource::read` (inline), `SizedSource::drainAll` (inline)
- `LengthSink::operator()` (inline), `LengthSource::read` (inline)
- `LambdaSink` ctor with optional cleanup, deleted copy/move, dtor calling `cleanupFun`, `operator()` (all inline)
- `LambdaSource::read` (inline)
- `ChainSource::read`
- `sourceToSink(fun<void(Source &)> reader)` — coroutine adapter
- `sinkToSource(fun<void(Sink &)> writer, fun<void()> eof = ...throw EndOfFile)` — coroutine adapter
- `writePadding(size_t, Sink &)`, `writeString(string_view, Sink &)`
- `operator<<(Sink &, uint64_t)` (inline) — little-endian 8-byte integer write
- `operator<<(Sink &, const Error &)`, `operator<<(Sink &, std::string_view)`, `operator<<(Sink &, const Strings &)`, `operator<<(Sink &, const StringSet &)`
- `readNum<T>(Source &)` (inline template) — read 8 LE bytes, range-check, cast
- `readInt(Source &)` (inline) — `readNum<unsigned int>`
- `readLongLong(Source &)` (inline) — `readNum<uint64_t>`
- `readPadding(size_t, Source &)`, `readString(...)` overloads
- `readStrings<T>(Source &)` (template)
- `operator>>(Source &, std::string &)`
- `operator>>(Source &, T &)` (inline template) — `readNum<T>`
- `operator>>(Source &, bool &)` (inline template) — read as `uint64_t`
- `readError(Source &)`
- `StreamToSourceAdapter::read` (inline)
- `FramedSource` ctor, dtor, deleted copy/move, `read` (all inline)
- `FramedSink` ctor, dtor, deleted copy/move, `writeUnbuffered` (all inline)
- `EnsureRead` ctor, dtor, `finish`, `read`, `good`, `skip` (all inline)

### Type aliases
- (none direct in this file)

### Macros / globals
- `MakeError(SerialisationError, Error)` — defines exception type `SerialisationError` deriving from `Error`

## File: src/libutil/file-system.cc

### Namespaces
- `nix`

### Classes / structs
- (no new types declared; member fns of `DirectoryIterator` and `AutoDelete` defined here)

### Functions / member functions
- `DirectoryIterator::DirectoryIterator(const std::filesystem::path & p)` — ctor; rethrow `filesystem_error` as `SystemError`
- `DirectoryIterator::operator++()` — advance with `error_code`; rethrow as `SysError`
- `absPath(const std::filesystem::path & path0, const std::filesystem::path * dir, bool resolveSymlinks)` (free fn) — make absolute relative to `dir` or cwd, then `canonPath`
- `canonPath(const std::filesystem::path & path, bool resolveSymlinks)` (free fn) — canonicalise via `canonPathInner<OsPathTrait<char>>` with optional symlink resolution and 1024-symlink follow limit
- `baseNameOf(std::string_view path)` (free fn) — return last path component, accounting for trailing separators
- `isInDir(const std::filesystem::path & path, const std::filesystem::path & dir)` (free fn) — `lexically_relative`-based descendant check
- `isDirOrInDir(const std::filesystem::path & path, const std::filesystem::path & dir)` (free fn) — `path == dir || isInDir(...)`
- `stat(const std::filesystem::path &)` (free fn) — wraps `STAT` (Windows `_wstat64` or POSIX `stat`)
- `maybeStat(const std::filesystem::path &)` (free fn) — `nullopt` on `ENOENT`/`ENOTDIR`, else throws or returns
- `pathExists(const std::filesystem::path &)` (free fn) — implemented via `maybeLstat`
- `pathAccessible(const std::filesystem::path &)` (free fn) — wraps `pathExists`, swallows EPERM as false
- `readLink(const std::filesystem::path &)` (free fn) — `std::filesystem::read_symlink` rethrown as `SystemError`
- `readFile(const std::filesystem::path &)` (free fn) — open RO, then drain via `readFile(fd)`
- `readFile(const std::filesystem::path &, Sink &, bool memory_map)` (free fn) — try `boost::iostreams::mapped_file_source` first, fall back to `drainFD`
- `writeFile(const std::filesystem::path &, std::string_view, mode_t, FsSync, FinalSymlink)` (free fn) — open new file, call fd-overload, close to propagate exceptions
- `writeFile(Descriptor, std::string_view, FsSync, const std::filesystem::path *)` (free fn) — `writeFull` plus optional `syncDescriptor`
- `writeFile(const std::filesystem::path &, Source &, mode_t, FsSync, FinalSymlink)` (free fn) — stream-copy from a `Source` into a fresh file with optional fsync + `syncParent`
- `syncParent(const std::filesystem::path &)` (free fn) — open the parent directory and fsync
- `recursiveSync(const std::filesystem::path &)` (free fn) — DFS over a tree fsyncing files and finally directories (in reverse traversal order)
- `createDir(const std::filesystem::path &, mode_t mode)` (free fn) — wrap `mkdir`
- `createDirs(const std::filesystem::path &)` (free fn) — wrap `std::filesystem::create_directories`, rethrow as `SystemError`
- `AutoDelete::AutoDelete()` — disarmed default
- `AutoDelete::AutoDelete(const std::filesystem::path &, bool recursive)` — armed
- `AutoDelete::deletePath()` — delete path (recursive or single) and disarm
- `AutoDelete::~AutoDelete()` — calls `deletePath`, swallowing exceptions
- `AutoDelete::cancel() noexcept` — disarm
- `createTempDir(const std::filesystem::path & tmpRoot, const std::string & prefix, mode_t mode)` (free fn) — loop until `mkdir` succeeds; on FreeBSD chowns to egid to work around BSD setgid inheritance
- `createAnonymousTempFile()` (free fn) — Linux `O_TMPFILE` fast path with fallback to `createTempFile`+`tryUnlink`; Windows uses `FILE_FLAG_DELETE_ON_CLOSE`
- `createTempFile(const std::filesystem::path & prefix)` (free fn) — `mkstemp` and set CLOEXEC on Unix
- `makeTempPath(const std::filesystem::path & root, const std::string & suffix)` (free fn) — `<tmproot>/<suffix>-<pid>-<atomic counter>` (counter seeded from `random_device`)
- `createSymlink(const std::filesystem::path & target, const std::filesystem::path & link)` (free fn) — wraps `std::filesystem::create_symlink`
- `replaceSymlink(const std::filesystem::path & target, const std::filesystem::path & link)` (free fn) — atomic-replace via temp + rename, retrying on `file_exists`
- `setWriteTime(const std::filesystem::path &, const PosixStat & st)` (free fn) — convenience overload calling the 3-arg `setWriteTime(path, atime, mtime, isLink)` (latter defined per-platform elsewhere)
- `copyFile(const std::filesystem::path & from, const std::filesystem::path & to, bool andDelete, bool contents)` (free fn) — recursive copy (file/symlink/directory); optional delete-after-copy and forced-regular-file variant
- `moveFile(const std::filesystem::path &, const std::filesystem::path &)` (free fn) — `std::filesystem::rename` first, fall back to copy-via-temp-dir on `EXDEV`
- `isExecutableFileAmbient(const std::filesystem::path & exe)` (free fn) — `is_regular_file` && `access(exe, X_OK)`
- `chmod(const std::filesystem::path & path, mode_t mode)` (free fn) — wraps platform chmod (`_wchmod` on Windows)
- `unlinkIfExists(const std::filesystem::path &)` (free fn) — UNLINK_PROC, ignore ENOENT
- `unlink(const std::filesystem::path &)` (free fn) — UNLINK_PROC, throw on failure
- `tryUnlink(const std::filesystem::path &)` (free fn) — UNLINK_PROC, ignore errors
- `chmodIfNeeded(const std::filesystem::path &, mode_t mode, mode_t mask)` (free fn) — chmod only if mode bits differ under mask

### Type aliases
- (none)

### Macros / globals
- `STAT` (`#define`, file-local) — `_wstat64` on Windows, `stat` elsewhere; `#undef`'d after use
- `UNLINK_PROC` (`#define`, file-local) — `::_wunlink` on Windows, `::unlink` elsewhere; `#undef`'d after use

## File: src/libutil/include/nix/util/file-system.hh

### Namespaces
- `nix`
- `nix::windows` (Windows only)

### Classes / structs
- `OpenNewFileForWriteParams` (struct) — bitfield flags `truncateExisting:1`, `followSymlinksOnTruncate:1`, `writeOnly:1` (default true)
- `AutoDelete` (class) — RAII path deleter (default ctor, path/recursive ctor, `deletePath`, `cancel`, dtor, move ctor and move assignment via `swap`, deleted copy, friend `swap`, accessor `path()`/`view()`, conversions to `path`/`PathView`)
- `DirectoryIterator` (class) — input-iterator wrapping `std::filesystem::directory_iterator` translating filesystem errors to `SysError`; supports range-for over a single instance via `begin()`/`end()`; postfix `++(int)` and equality comparison

### Enums
- `FinalSymlink` (`enum struct`, underlying `bool`) — `DontFollow = false`, `Follow = true`
- `FsSync` (`enum struct`) — `Yes`, `No`

### Functions (declarations)
- `absPath(const path &, const path * dir = nullptr, bool resolveSymlinks = false)`
- `canonPath(const path &, bool resolveSymlinks = false)`
- `baseNameOf(std::string_view path)`
- `isInDir(const path &, const path &)`, `isDirOrInDir(const path &, const path &)`
- `windows::fileTimeToUnixTime(const FILETIME &)` — Windows only
- `windows::statFromFileInfo(PosixStat &, DWORD, const FILETIME &, ..., DWORD nNumberOfLinks = 1)` — Windows only; populate a `PosixStat` from FILE_INFO components
- `lstat(const path &)`, `stat(const path &)`, `maybeLstat(const path &)`, `maybeStat(const path &)`
- `pathExists(const path &)`, `pathAccessible(const path &)`, `readLink(const path &)`
- `descriptorToPath(Descriptor)` — for error messages only (TOCTOU note in docstring)
- `openDirectory(const path &, FinalSymlink = Follow)`
- `openFileReadonly(const path &, FinalSymlink = Follow)`
- `openNewFileForWrite(const path &, mode_t, OpenNewFileForWriteParams)`
- `readFile(const path &)` and `readFile(const path &, Sink &, bool memory_map = true)`
- `writeFile(const path &, std::string_view, mode_t = 0666, FsSync = No, FinalSymlink = Follow)`
- `writeFile(const path &, Source &, mode_t = 0666, FsSync = No, FinalSymlink = Follow)`
- `writeFile(Descriptor, std::string_view, FsSync = No, const path * origPath = nullptr)`
- `syncParent(const path &)`, `recursiveSync(const path &)`
- `deletePath(const path &)` and `deletePath(const path &, uint64_t & bytesFreed)` — declared here, implemented elsewhere
- `createDirs(const path &)`, `createDir(const path &, mode_t = 0755)`
- `setWriteTime(const path &, time_t accessedTime, time_t modificationTime, std::optional<bool> isSymlink = std::nullopt)`
- `setWriteTime(const path &, const PosixStat &)`
- `createSymlink(const path & target, const path & link)`, `replaceSymlink(const path & target, const path & link)`
- `moveFile(const path & src, const path & dst)`, `copyFile(const path & from, const path & to, bool andDelete, bool contents = false)`
- `createTempDir(const path & tmpRoot = "", const std::string & prefix = "nix", mode_t = 0755)`
- `createAnonymousTempFile()`, `createTempFile(const path & prefix = "nix")`, `defaultTempDir()`, `makeTempPath(const path & root, const std::string & suffix = ".tmp")`
- `isExecutableFileAmbient(const path & exe)`
- `chmodIfNeeded(const path &, mode_t mode, mode_t mask = S_IRWXU | S_IRWXG | S_IRWXO)`, `chmod(const path &, mode_t)`
- `chown(const path &, uid_t owner, gid_t group)` (non-Windows)
- `unlink(const path &)`, `unlinkIfExists(const path &)`, `tryUnlink(const path &)`

### Type aliases
- `PosixStat` (`using`) — `struct ::__stat64` on Windows / `struct ::stat` elsewhere
- `AutoCloseDir` (`typedef`) — `std::unique_ptr<DIR, Deleter<closedir>>`
- `PathFilter` (`typedef`) — `fun<bool(const std::string & path)>`

### Macros / globals
- `S_IFLNK` (`#define`, MinGW polyfill, value `0120000`) — only when not predefined; errors out if not on Windows
- `S_ISLNK(m)` (`#define`, MinGW polyfill) — only when not predefined; errors out if not on Windows
- `defaultPathFilter` (extern `PathFilter`) — defined in `archive.cc`, declared here

## File: src/libutil/include/nix/util/file-system-at.hh

### Namespaces
- `nix`
- `nix::linux` (Linux only)
- `nix::unix` (non-Windows only)

### Functions (declarations)
- `fstat(Descriptor fd)` — get `PosixStat` from open handle
- `maybeFstatat(Descriptor dirFd, const std::filesystem::path &)` (Unix only) — `nullopt` if missing
- `fstatat(Descriptor dirFd, const std::filesystem::path &)` (Unix only) — throwing variant
- `readLinkAt(Descriptor dirFd, const CanonPath & path)` — fd-relative `readlink`, returns `OsString`
- `openFileEnsureBeneathNoSymlinks(Descriptor dirFd, const CanonPath & path, ...)` — open ensuring path stays beneath `dirFd` and contains no interior symlinks; platform-conditional parameter list (Windows: `ACCESS_MASK desiredAccess, ULONG createOptions, ULONG createDisposition = FILE_OPEN`; Unix: `int flags, mode_t mode = 0`); optional `dirFdCallback` taking ownership of intermediate directory fds
- `linux::openat2(Descriptor dirFd, const char * path, uint64_t flags, uint64_t mode, uint64_t resolve)` — wrapper around the Linux 5.6 syscall, returns `nullopt` if unsupported
- `unix::fchmodatTryNoFollow(Descriptor dirFd, const CanonPath & path, mode_t mode)` — chmod relative to dirFd; falls back to `fchmodat` without `AT_SYMLINK_NOFOLLOW` if unsupported

### Macros / globals
- `NIX_ERR_OPEN_SYMLINK` (`#define`) — `EMLINK` on FreeBSD, `ELOOP` elsewhere on Unix; not defined on Windows

## File: src/libutil/include/nix/util/file-path.hh

### Namespaces
- `nix`

### Classes / structs
- `PathView` (struct, : `OsStringView`) — stop-gap `path_view`; brings in `string_view` ctors plus ctors from `std::filesystem::path` and `OsString`; provides const and non-const `native()` accessors

### Type aliases
- `Paths` (`typedef`) — `std::list<std::filesystem::path>`
- `PathSet` (`typedef`) — `std::set<std::filesystem::path>`

### Functions (declarations)
- `maybePath(PathView path)` — `std::optional<std::filesystem::path>`; defined elsewhere
- `toOwnedPath(PathView path)` — `std::filesystem::path`; defined elsewhere

### Templates
- `json_avoids_null<std::filesystem::path>` (template specialisation) — `std::true_type`

## File: src/libutil/include/nix/util/file-path-impl.hh

### Namespaces
- `nix`

### Classes / structs
- `UnixPathTrait` (struct) — `CharT=char`, `String=std::string`, `StringView=std::string_view`, `preferredSep='/'`, static `isPathSep`, `findPathSep`, `rfindPathSep`, `rootNameLen` returns 0
- `WindowsPathTrait<CharT0>` (struct template) — character-type-parameterised Windows variant; `preferredSep='\\'`, `isPathSep` accepts both `/` and `\\`, `rootNameLen` recognises drive letters

### Type aliases / templates
- `OsPathTrait<CharT>` (template alias) — selects `WindowsPathTrait<CharT>` on Windows, `UnixPathTrait` elsewhere

### Functions / templates
- `canonPathInner<PathDict>(StringView remaining, hookComponent)` (template fn) — pure path canonicalisation algorithm: strips `.`/`..`/empty components, supports a per-component hook (used for symlink resolution); reserves 256 chars

## File: src/libutil/include/nix/util/os-string.hh

### Namespaces
- `nix`

### Type aliases
- `OsChar` (`using`) — `wchar_t` on Windows, `char` elsewhere
- `OsString` (`using`) — `std::basic_string<OsChar>`
- `OsStringView` (`using`) — `std::basic_string_view<OsChar>`
- `OsStringMap` (`using`) — `std::map<OsString, OsString, std::less<>>`
- `OsStrings` (`using`) — `std::list<OsString>`

### Functions
- `os_string_to_string(OsStringView)` and `os_string_to_string(OsString)` — UTF-16 → UTF-8 on Windows; identity inline on Unix
- `string_to_os_string(std::string_view)` and `string_to_os_string(std::string)` — inverse; identity inline on Unix
- `toOsStrings(std::list<std::string>)` (inline) — convert; on Unix moves the input list directly
- `tokenizeString<C>(OsStringView, OsStringView separators = OS_STR(" \t\n\r"))` (template, Windows only; explicit instantiation declarations for `std::list<OsString>` and `std::vector<OsString>`)

### Macros / globals
- `OS_STR(s)` (`#define`) — Unix: identity; Windows: prepends `L` for wide string literals

## File: src/libutil/include/nix/util/socket.hh

### Namespaces
- `nix`

### Type aliases
- `Socket` (`using`) — `SOCKET` on Windows, `int` elsewhere

### Functions
- `toSocket(Descriptor)` (`static inline`) — `reinterpret_cast` HANDLE→SOCKET on Windows, identity elsewhere
- `fromSocket(Socket)` (`static inline`) — inverse

### Macros / globals
- `SHUT_WR` (`#define`, Windows only) — alias for `SD_SEND`
- `SHUT_RDWR` (`#define`, Windows only) — alias for `SD_BOTH`

## File: src/libutil/file-descriptor.cc

### Namespaces
- `nix`
- (anonymous) — internal helpers

### Classes / structs / enums
- `PollDirection` (`enum class`, anonymous ns) — `In`, `Out`

### Functions / member functions
- `retryOnBlock<F>(Descriptor fd, PollDirection dir, F && f)` (template fn, anonymous ns) — Unix: poll-and-retry on `EAGAIN`/`EWOULDBLOCK`; Windows: forwards once
- `readFull(Descriptor fd, char * buf, size_t count)` (free fn) — read until exactly `count` bytes (or `EndOfFile`)
- `readLine(Descriptor fd, bool eofOk, char terminator)` (free fn) — byte-at-a-time line read; maps `EIO` on PTY masters to EOF
- `writeFull(Descriptor fd, std::string_view s, bool allowInterrupts)` (free fn) — repeated `write` until empty
- `writeLine(Descriptor fd, std::string s)` (free fn) — append `'\n'` and `writeFull`
- `readFile(Descriptor fd)` (free fn) — `getFileSize` then `drainFD` (with `expected = false` since proc files report size 0)
- `drainFD(Descriptor fd, Sink & sink, DrainFdSinkOpts)` (free fn) — read until EOF with optional non-blocking mode and exact-size mode; uses 64 KiB stack buffer
- `drainFD(Descriptor fd, DrainFdOpts)` (free fn) — string-returning adapter
- `copyFdRange(Descriptor fd, off_t offset, size_t nbytes, Sink & sink)` (free fn) — `readOffset`-loop over a fixed range; throws `EndOfFile` on early EOF
- `AutoCloseFD::AutoCloseFD()` (default), `(Descriptor)`, move ctor (`noexcept`), move assignment (closes existing fd first), `~AutoCloseFD()` (close, swallow)
- `AutoCloseFD::get()` const, `close()`, `startFsync()` const (Linux: `sync_file_range` SYNC_FILE_RANGE_WRITE), `operator bool()`, `release()`
- `Pipe::close()` — close both ends

### Macros / globals
- (none)

## File: src/libutil/include/nix/util/file-descriptor.hh

### Namespaces
- `nix`
- `nix::unix` (non-Windows only)

### Classes / structs
- `DrainFdSinkOpts` (struct) — `std::optional<std::make_unsigned_t<off_t>> expectedSize`; on Unix `bool block = true`
- `DrainFdOpts` (struct) — `std::make_unsigned_t<off_t> size = 0`, `bool expected = false`; on Unix `bool block = true`
- `AutoCloseFD` (class) — RAII descriptor; deleted copy; move ctor `noexcept`/move assignment; `get`, `release`, `close`, `operator bool`, inline `fsync`, `startFsync`
- `Pipe` (class) — owns `AutoCloseFD readSide, writeSide`; `create(bool nonBlocking = false)` (default arg only on Unix), `close`
- `unix::SelfPipe` (struct, non-Windows) — owns a `Pipe`; `create`, `notify` (non-blocking), `drain`

### Type aliases
- `Descriptor` (`using`) — `HANDLE` on Windows, `int` elsewhere

### Functions (declarations + inline)
- `toDescriptor(int fd)` (`static inline`) — Windows `_get_osfhandle`; identity elsewhere
- `readFile(Descriptor)` (declared)
- `read(Descriptor, std::span<std::byte>)` (declared) — platform-specific read
- `write(Descriptor, std::span<const std::byte>, bool allowInterrupts)` (declared) — platform-specific write
- `getFileSize(Descriptor)` — `fstat`/`GetFileSizeEx` wrapper, returns `std::make_unsigned_t<off_t>`
- `readOffset(Descriptor, off_t, std::span<std::byte>)` — `pread`-style
- `copyFdRange(Descriptor, off_t, size_t, Sink &)`
- `readFull`, `writeFull` (with `allowInterrupts = true` default), `readLine`, `writeLine`
- `syncDescriptor(Descriptor)` — blocking fsync
- `drainFD(...)` overloads (string and Sink form)
- `getStandardInput`, `getStandardOutput`, `getStandardError` (`inline` `[[gnu::always_inline]]`) — `STDIN/OUT/ERR_FILENO` or `GetStdHandle(...)`
- `dupDescriptor(Descriptor)` — duplicate
- `unix::closeExtraFDs()`, `unix::closeOnExec(Descriptor)`
- `lseek(Descriptor, off_t, int)` (Windows only)

### Macros / globals
- `INVALID_DESCRIPTOR` (`const Descriptor`) — `INVALID_HANDLE_VALUE` on Windows, `-1` elsewhere
- `MakeError(EndOfFile, Error)` — defines `EndOfFile` exception

## File: src/libutil/posix-source-accessor.cc

### Namespaces
- `nix`
- (anonymous, contains POSIX/Windows accessor classes)

### Classes / structs
- `PosixFileSourceAccessor` (class, anonymous ns, Unix; : `detail::PosixSourceAccessorBase`) — accessor for a single regular file backed by an open `AutoCloseFD`; memoises `PosixStat`; friend of `PosixDirectorySourceAccessor`
- `PosixDirectorySourceAccessor` (class, anonymous ns, Unix; : `detail::PosixSourceAccessorBase`) — accessor for a directory tree backed by `AutoCloseFD dirFd`; optional LRU cache of intermediate dir-fds (`Sync<LRUCache<CanonPath, ref<AutoCloseFD>>>`); global cache registry; global fd-count throttle; deleted copy/move
- `WindowsSourceAccessor` (class, anonymous ns, Windows; : `detail::PosixSourceAccessorBase`) — Windows variant; uses a global `boost::concurrent_flat_map` for stat caching; in-tree TODO comment notes it should be split into its own file
- `SymlinkSourceAccessor` (class, local to `makeFSSourceAccessor`; : `MemorySourceAccessor`) — represents a single-symlink filesystem when `FinalSymlink::DontFollow` and the root is a symlink; overrides `getLastModified`, `getPhysicalPath`, `showPath`

### Functions / member functions
- `sourceAccessorStatFromPosixStat(const PosixStat &)` (static fn) — translate `S_IS*` mode bits to a `SourceAccessor::Stat` (size + executable bit only for regular files)
- `getGlobalDirFdCacheLimit()` (static fn, Unix) — `min(4096, RLIMIT_NOFILE / 8)`
- `PosixDirectorySourceAccessor::getGlobalFdLimit()` (static method) — memoised `getGlobalDirFdCacheLimit()`
- `PosixDirectorySourceAccessor::registerAccessor(ref<...>)` (static method) — registers in `globalDirFdCacheRegistry` and prunes expired weak ptrs
- `PosixDirectorySourceAccessor::maybeEvictFromGlobalCaches()` (static method) — when global fd count over limit, walk registry clearing dir-fd caches
- `PosixDirectorySourceAccessor::insertIntoDirFdCache(key, fd)` (private member) — upsert and adjust the global counter
- `PosixDirectorySourceAccessor::openParentAndUpsert(path, ignoreMissing)` (private member) — open parent, cache its fd, optionally tolerating missing intermediates
- `PosixDirectorySourceAccessor::openParent(path)` (private member) — climb cached chain to find deepest cached intermediate, open the rest with `openFileEnsureBeneathNoSymlinks`
- `PosixDirectorySourceAccessor::makeDirFdCallback()` (private member) — produces the callback used by the open helper to populate the cache
- `PosixDirectorySourceAccessor::openSubdirectory(path)` (private member) — open the directory itself (with `openat(dirFd, ".", ...)` for thread-safety on root)
- `PosixDirectorySourceAccessor::PosixDirectorySourceAccessor(...)` ctor + dtor (clears cache via `invalidateCache`)
- `PosixDirectorySourceAccessor::invalidateCache()` (override)
- `PosixDirectorySourceAccessor::readFile`, `maybeLstat`, `readDirectory` (entries variant), `readDirectory` (callback variant — opens a subaccessor at `dirPath` and invokes the callback), `readLink`, `getPhysicalPath`, `showPath` (overrides)
- `PosixFileSourceAccessor::PosixFileSourceAccessor(AutoCloseFD, path, trackLastModified, st)` ctor — asserts regular + absolute path; `setPathDisplay`; `maybeUpdateMtime`
- `PosixFileSourceAccessor::readFile` (uses `copyFdRange`), `pathExists`, `maybeLstat`, `readDirectory` (always throws `NotADirectory`), `readLink` (always throws `NotASymlink`), `getPhysicalPath`, `showPath` (overrides)
- `WindowsSourceAccessor::WindowsSourceAccessor()` and `(std::filesystem::path && root, bool trackLastModified = false)` ctors
- `WindowsSourceAccessor::makeAbsPath(path)` (private) — prepend root or use absolute
- `WindowsSourceAccessor::readFile` (asserts no symlinks; `FdSource::drainInto`), `pathExists`, `cachedLstat` (private; size-bounded `boost::concurrent_flat_map` at 16384 entries), `maybeLstat`, `readDirectory`, `readLink`, `getPhysicalPath`, `assertNoSymlinks`
- `getFSSourceAccessor()` (free fn, Unix) — singleton root-fs accessor `makeFSSourceAccessor("/")`
- `makeFSSourceAccessor(std::filesystem::path root, bool trackLastModified, FinalSymlink finalSymlink)` (free fn) — Unix: open root and dispatch on file type to `PosixFileSourceAccessor`/`PosixDirectorySourceAccessor`/(when final-symlink rejected) inline `SymlinkSourceAccessor`; Windows: returns a `WindowsSourceAccessor`; registers `PosixDirectorySourceAccessor` instances with the global registry

### Type aliases
- `Cache` (Windows-only, file-scope at namespace `nix`) — `boost::concurrent_flat_map<std::string, std::optional<PosixStat>>`

### Macros / globals
- `cache` (static `Cache`, Windows only) — global Windows lstat cache
- `PosixDirectorySourceAccessor::globalDirFdCount` (`static inline std::atomic<unsigned>`) — process-wide cached-dir-fd count
- `PosixDirectorySourceAccessor::globalDirFdCacheRegistry` (`static inline Sync<std::list<std::weak_ptr<PosixDirectorySourceAccessor>>>`) — registry of accessors with caches

## File: src/libutil/include/nix/util/posix-source-accessor.hh

### Namespaces
- `nix`
- `nix::detail`

### Classes / structs
- `detail::PosixSourceAccessorBase` (class, virtual : `SourceAccessor`) — protected base with `const bool trackLastModified`, `time_t mtime`, helper `maybeUpdateMtime(time_t)`, override of `getLastModified()`; ctor takes `bool trackLastModified`

### Forward declarations
- `nix::SourcePath` (struct) — forward-declared

## File: src/libutil/source-accessor.cc

### Namespaces
- `nix`

### Classes / structs
- (none new)

### Functions / member functions
- `SourceAccessor::Stat::isNotNARSerialisable()` — true for non-{regular,symlink,directory}
- `SourceAccessor::Stat::typeString()` — returns string label per `Type` enum value (e.g. `"regular"`, `"character device"`, etc.)
- `SourceAccessor::SourceAccessor()` — ctor; assigns `++nextNumber` to `number`, sets `displayPrefix = "«unknown»"`
- `SourceAccessor::pathExists(const CanonPath &)` — default impl via `maybeLstat`
- `SourceAccessor::readFile(const CanonPath &)` (string overload) — uses Sink overload + `s.reserve(_size)` via sizeCallback
- `SourceAccessor::readFile(const CanonPath &, Sink &, fun<void(uint64_t)>)` — default impl that reads into a `std::string` and writes through the sink
- `SourceAccessor::hashPath(path, filter, ha)` — `HashSink + dumpPath`
- `SourceAccessor::lstat(const CanonPath &)` — default impl: `maybeLstat` else throw `FileNotFound`
- `SourceAccessor::setPathDisplay(prefix, suffix)` — set the labelling fields
- `SourceAccessor::showPath(const CanonPath &)` — concat prefix + abs + suffix
- `SourceAccessor::resolveSymlinks(path, mode)` — manual walker with 1024-link bound, supporting `Ancestors` and `Full` modes

### Macros / globals
- `nextNumber` (`static std::atomic<size_t>`) — global counter for accessor IDs

## File: src/libutil/include/nix/util/source-accessor.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `SymlinkResolution` (`enum class`) — `Ancestors`, `Full`
- `SourceAccessor` (struct, : `std::enable_shared_from_this<SourceAccessor>`) — abstract read-only filesystem
  - public fields: `const size_t number`, `displayPrefix`, `displaySuffix`, `std::optional<std::string> fingerprint`
  - nested `enum Type { tRegular, tSymlink, tDirectory, tChar, tBlock, tSocket, tFifo, tUnknown }`
  - nested struct `Stat { Type type = tUnknown; std::optional<uint64_t> fileSize; bool isExecutable = false; std::optional<uint64_t> narOffset; isNotNARSerialisable(); typeString(); }`
  - typedef `DirEntry = std::optional<Type>`
  - typedef `DirEntries = std::map<std::string, DirEntry>`
  - virtual: `readFile(path, Sink&, sizeCallback)`, `pathExists(path)`, `lstat(path)`, `maybeLstat(path)` (=0), `readDirectory(path)` (=0), `readDirectory(path, callback)` (default delegates to `callback(*this, dirPath)`), `readLink(path)` (=0), `dumpPath(path, sink, filter)`, `getPhysicalPath(path)` (default nullopt), `showPath(path)`, `getFingerprint(path)` (default returns `{path, fingerprint}`), `getLastModified()` (default nullopt), `invalidateCache()`
  - non-virtual: `readFile(path)` (string), `hashPath(path, filter, ha)`, `operator==`, `operator<=>` (default-derived from `number`), `setPathDisplay`, `resolveSymlinks(path, mode)`
- `SymlinkNotAllowed` (struct, final, : `CloneableError<SymlinkNotAllowed, Error>`) — exception carrying a `CanonPath path`; default ctor formats a message; templated ctor with custom message format

### Functions
- `makeEmptySourceAccessor()` (free fn) — returns empty accessor; defined in `memory-source-accessor.cc`
- `getFSSourceAccessor()` — see `posix-source-accessor.cc`
- `makeFSSourceAccessor(root, trackLastModified, finalSymlink = DontFollow)` — see `posix-source-accessor.cc`
- `makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> &&)` — defined in `union-source-accessor.cc`
- `makeCachingSourceAccessor(ref<SourceAccessor>)` — defined in `caching-source-accessor.cc`

### Macros / globals
- `MakeError(SourceAccessorError, Error)`
- `MakeError(FileNotFound, SourceAccessorError)`
- `MakeError(NotASymlink, SourceAccessorError)`
- `MakeError(NotADirectory, SourceAccessorError)`
- `MakeError(NotARegularFile, SourceAccessorError)`
- `MakeError(RestrictedPathError, Error)`

## File: src/libutil/source-path.cc

### Namespaces
- `nix`

### Functions / member functions
- `SourcePath::baseName() const` — delegates to `path.baseName()` with default `"source"`
- `SourcePath::parent() const` — `path.parent()` (asserts non-null)
- `SourcePath::readFile() const` — `accessor->readFile(path)` (string overload)
- `SourcePath::pathExists() const`, `lstat() const`, `maybeLstat() const`, `readDirectory() const`, `readLink() const` — all delegate to `accessor`
- `SourcePath::dumpPath(Sink &, PathFilter &) const` — delegate
- `SourcePath::getPhysicalPath() const` — delegate
- `SourcePath::to_string() const` — `accessor->showPath(path)`
- `SourcePath::operator/(const CanonPath &) const` and `operator/(std::string_view) const` — produce extended `SourcePath`
- `SourcePath::operator==(const SourcePath &) const noexcept` and `operator<=>(...)` — compare via `std::tie(*accessor, path)`
- `operator<<(std::ostream &, const SourcePath &)` (free fn)

## File: src/libutil/include/nix/util/source-path.hh

### Namespaces
- `nix`
- `std` (specialisation of `std::hash`)

### Classes / structs
- `SourcePath` (struct) — `ref<SourceAccessor> accessor`, `CanonPath path`; ctor with default-root path; `readFile(Sink &, sizeCallback)` inline overload; `resolveSymlinks(mode = Full)` inline; friend of `std::hash<nix::SourcePath>`

### Functions
- `operator<<(std::ostream &, const SourcePath &)` (declaration)
- `hash_value(const SourcePath &)` (`inline`) — boost-style hash combining `accessor->number` and `path`
- `std::hash<nix::SourcePath>::operator()` — uses `hash_value`; declares `is_avalanching = std::true_type`

## File: src/libutil/canon-path.cc

### Namespaces
- `nix`

### Functions / member functions
- `absPathPure(std::string_view path)` (static fn) — calls `canonPathInner<UnixPathTrait>` with no-op hook
- `ensureNoNullBytes(std::string_view s)` (static fn) — throw `BadCanonPath` (with `␀`-replaced display string) if any NUL byte present
- `CanonPath::fromFilename(std::string_view segment)` (static method) — verify the segment is exactly one path component (single-iterator distance after canonicalisation)
- `CanonPath::CanonPath(std::string_view raw)` — canonicalise `"/" + raw`; reject NUL bytes
- `CanonPath::CanonPath(const char * raw)` — same but skips the explicit NUL-byte check (the C string already terminates)
- `CanonPath::CanonPath(std::string_view raw, const CanonPath & root)` — canonicalise relative to `root`; reject NUL bytes
- `CanonPath::CanonPath(const std::vector<std::string> & elems)` — push each element onto root
- `CanonPath::parent() const` — return parent `CanonPath` or `nullopt` at root
- `CanonPath::pop()` — drop last component (assert non-root)
- `CanonPath::isWithin(const CanonPath & parent) const` — descendant predicate
- `CanonPath::removePrefix(const CanonPath & prefix) const` — return suffix after the prefix
- `CanonPath::extend(const CanonPath & x)` — append another path
- `CanonPath::operator/(const CanonPath &) const` and `operator/(std::string_view) const` — non-mutating concatenation
- `CanonPath::push(std::string_view)` — append a single component (asserts no slash, not `.`/`..`); rejects NUL bytes
- `CanonPath::isAllowed(const std::set<CanonPath> & allowed) const` — true if `this` matches an allowed path or any of its parents (or vice versa for parent-of-allowed access)
- `CanonPath::makeRelative(const CanonPath & path) const` — produce relative representation of `path` from `this` (using `..` segments as needed)
- `operator<<(std::ostream &, const CanonPath &)` (free fn)

### Macros / globals
- `CanonPath::root` (static member, defined here) — `CanonPath("/")`

## File: src/libutil/include/nix/util/canon-path.hh

### Namespaces
- `nix`
- `std` (specialisation of `std::hash<nix::CanonPath>`)

### Classes / structs
- `CanonPath` (class) — owns `std::string path`; static `forward_range`-conforming
  - nested `struct unchecked_t {}` plus `constexpr CanonPath(unchecked_t, std::string)` ctor
  - nested `class Iterator` (forward iterator over slash-separated components) — uses internal `PointerProxy` for `operator->`
    - `class PointerProxy` (private, in `Iterator`) — wraps a `std::string_view` with `operator->`
  - public statics: `root` (defined in .cc), `fromFilename`
  - public ctors and members: `CanonPath(string_view)`, `(const char *)`, `(string_view, const CanonPath &)`, `(unchecked_t, string)`, `(vector<string> &)`; `isRoot`, `operator string_view`, `abs`, `absOrEmpty`, `c_str`, `rel`, `rel_c_str`, `begin`/`end`, `parent`, `pop`, `dirOf`, `baseName`, `operator==`/`!=`/`<=>` (sorts `/` before any other byte so directories appear before children), `isWithin`, `removePrefix`, `extend`, `operator/` (CanonPath/string_view), `push`, `isAllowed`, `makeRelative`
  - friend `hash_value(const CanonPath &)`

### Functions
- `hash_value(const CanonPath &)` (`inline`) — `boost::hash<std::string_view>` over the absolute path
- `operator<<(std::ostream &, const CanonPath &)` (declaration)
- `std::hash<nix::CanonPath>::operator()` — calls `hash_value`; declares `is_avalanching = std::true_type`

### Macros / globals
- `MakeError(BadCanonPath, Error)`

## File: src/libutil/mounted-source-accessor.cc

### Namespaces
- `nix`

### Classes / structs
- `MountedSourceAccessorImpl` (struct, : `MountedSourceAccessor`) — `boost::concurrent_flat_map<CanonPath, ref<SourceAccessor>> mounts`
  - ctor: clears `displayPrefix`; assert `_mounts.contains(CanonPath::root)`; mount each
  - overrides `readFile`, `lstat`, `maybeLstat`, `readDirectory`, `readLink`, `showPath`, `invalidateCache`, `getPhysicalPath`, `mount`, `getMount`, `getFingerprint` (own fingerprint short-circuits, else delegates)
  - private `resolve(CanonPath path)` — climb path until a mount matches, returns `(accessor, subpath)`

### Functions
- `makeMountedSourceAccessor(std::map<CanonPath, ref<SourceAccessor>> mounts)` — factory returning `ref<MountedSourceAccessor>`

## File: src/libutil/include/nix/util/mounted-source-accessor.hh

### Namespaces
- `nix`

### Classes / structs
- `MountedSourceAccessor` (struct, : `SourceAccessor`) — abstract; pure virtual `mount(mountPoint, accessor)` and `getMount(mountPoint)` (returns `shared_ptr` or nullptr)

### Functions
- `makeMountedSourceAccessor(std::map<CanonPath, ref<SourceAccessor>> mounts)` (declaration)

## File: src/libutil/union-source-accessor.cc

### Namespaces
- `nix`

### Classes / structs
- `UnionSourceAccessor` (struct, : `SourceAccessor`) — `std::vector<ref<SourceAccessor>> accessors`; tries each in order
  - ctor: clears `displayPrefix`
  - overrides `readFile`, `maybeLstat`, `readDirectory` (unions entries; earlier wins; throws `FileNotFound` only if no accessor has the path), `readLink`, `showPath`, `invalidateCache`, `getPhysicalPath`, `getFingerprint`
  - `showPath` returns the first accessor's `showPath` (loop body unconditionally returns on first iteration)

### Functions
- `makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> &&)` — factory

## File: src/libutil/caching-source-accessor.cc

### Namespaces
- `nix`

### Classes / structs
- `CachingSourceAccessor` (class, : `SourceAccessor`) — wraps a `ref<SourceAccessor> next` with two `boost::concurrent_flat_map` caches: `lstatCache` and `readLinkCache`
  - never evicts (comment: evaluator handles invalidation; `invalidateCache` clears both caches and forwards)
  - overrides `readFile` (passthrough), `maybeLstat`, `lstat`, `readDirectory` (both overloads passthrough), `readLink`, `showPath` (passthrough), `invalidateCache`, `getPhysicalPath`, `getFingerprint` (passthrough)

### Functions
- `makeCachingSourceAccessor(ref<SourceAccessor> next)` — factory

## File: src/libutil/memory-source-accessor.cc

### Namespaces
- `nix`

### Classes / structs
- `CreateMemoryRegularFile` (struct, : `CreateRegularFileSink`) — captures a `File::Regular &`; implements `operator()`, `isExecutable`, `preallocateContents` for in-memory population

### Functions / member functions
- `MemorySourceAccessor::open(const CanonPath &, std::optional<File> create)` — locate or create a `File*` along the path; throws `SymlinkNotAllowed`/`NotADirectory` on intermediate file conflicts; returns nullptr when not creating and missing
- `MemorySourceAccessor::readFile(path, sink, sizeCallback)` — visit the variant; reads regular contents through a `StringSource::drainInto`
- `MemorySourceAccessor::pathExists(path)` — `open(path, nullopt)`
- `MemorySourceAccessor::File::lstat()` (template specialisation, out-of-line) — visit variant to produce `Stat`
- `MemorySourceAccessor::maybeLstat(path)` — `open` + `lstat` or nullopt
- `MemorySourceAccessor::readDirectory(path)` — visit; throws `NotADirectory` for regulars and `SymlinkNotAllowed` for symlinks
- `MemorySourceAccessor::readLink(path)` — visit; throws `NotASymlink` if not a symlink
- `MemorySourceAccessor::addFile(path, contents)` — create root directory if missing, create regular file, set contents, return `SourcePath`
- `MemorySink::createDirectory(path)` — `dst.open(path, Directory{})`
- `MemorySink::createRegularFile(path, func)` — `dst.open(path, Regular{})` then invoke `func` with a `CreateMemoryRegularFile`
- `MemorySink::createSymlink(path, target)` — `dst.open(path, Symlink{})` then assign target
- `CreateMemoryRegularFile::isExecutable()` — set `regularFile.executable = true`
- `CreateMemoryRegularFile::preallocateContents(uint64_t len)` — `reserve` (with overflow check)
- `CreateMemoryRegularFile::operator()(std::string_view data)` — `regularFile.contents += data`
- `makeEmptySourceAccessor()` — singleton (lambda-init) returning a `MemorySourceAccessor` with a root directory and empty `displayPrefix`

### Type aliases
- `using File = MemorySourceAccessor::File` (file-scope) — short alias used in `MemorySink::createDirectory`/etc.

## File: src/libutil/include/nix/util/memory-source-accessor.hh

### Namespaces
- `nix`
- `nix::fso`
- `nlohmann` (template specialisations)

### Classes / structs / enums (in `nix::fso`)
- `Regular<RegularContents>` (struct template) — `bool executable = false`, `RegularContents contents`; defaulted `operator<=>`
- `DirectoryT<Child>` (struct template) — `using Name = std::string`, `std::map<Name, Child, std::less<>> entries`; out-of-line defaulted `operator==` and `operator<=>`
- `Symlink` (struct) — `std::string target`; defaulted `operator<=>`
- `Opaque` (struct) — empty placeholder; defaulted `operator<=>`
- `VariantT<RegularContents, recur>` (struct template) — wraps `std::variant<Regular, Directory, Symlink>` (where `Directory = DirectoryT<conditional_t<recur, VariantT, Opaque>>`); `MAKE_WRAPPER_CONSTRUCTOR(VariantT)`; member `lstat() const`; out-of-line defaulted `operator==`/`<=>`
- (out-of-line operator==/<=> definitions for `DirectoryT` and `VariantT`)

### Classes / structs (in `nix`)
- `MemorySourceAccessor` (struct, virtual : `SourceAccessor`) — type alias `File = fso::VariantT<std::string, true>`; `std::optional<File> root`; defaulted `operator==`; non-default `operator<` based on root; overrides `readFile`, `pathExists`, `maybeLstat`, `readDirectory`, `readLink`; `using SourceAccessor::readFile` to expose the string overload; helper `open(path, create)`, `addFile(path, contents)`
- `MemorySink` (struct, : `FileSystemObjectSink`) — references a `MemorySourceAccessor & dst`; overrides `createDirectory`, `createRegularFile`, `createSymlink`

### Templates
- `json_avoids_null<...>` (true_type) specialisations for `MemorySourceAccessor::File::Regular`, `MemorySourceAccessor::File::Directory`, `MemorySourceAccessor::File::Symlink`, `MemorySourceAccessor::File`, `MemorySourceAccessor`
- `nlohmann::adl_serializer` declarations (via `JSON_IMPL_INNER`/`JSON_IMPL` macros) for `fso::Regular<RC>`, `fso::DirectoryT<Child>`, `fso::Symlink`, `fso::Opaque`, `fso::VariantT<RC, recur>`, `MemorySourceAccessor`

### Macros / globals
- `ARG` (`#define`/`#undef` blocks) — local helper used to template `JSON_IMPL_INNER` invocations for `Regular<RegularContents>`, `DirectoryT<Child>`, and `VariantT<RegularContents, recur>`
- `JSON_IMPL(MemorySourceAccessor)` — macro emitting standard JSON adl_serializer scaffolding

## File: src/libutil/memory-source-accessor/json.cc

### Namespaces
- `nlohmann`

### Templates / functions
- `adl_serializer<MemorySourceAccessor::File::Regular>::from_json/to_json` (specialisation pair) — `(executable, contents)` round-trip
- `adl_serializer<NarListing::Regular>::from_json/to_json` (specialisation pair) — handles `executable`/`size`/`narOffset`; rule that `narOffset == 0` becomes nullopt on decode
- `adl_serializer<fso::DirectoryT<Child>>::to_json/from_json` (template) — `entries` round-trip
- `adl_serializer<fso::Symlink>::from_json/to_json` — `target` round-trip
- `adl_serializer<fso::Opaque>::from_json/to_json` — empty-object round-trip
- `adl_serializer<fso::VariantT<RegularContents, recur>>::to_json/from_json` (template) — variant tagged by `"type"` field of `regular`/`directory`/`symlink`
- `adl_serializer<MemorySourceAccessor>::from_json/to_json` — wrap/unwrap `root`
- Explicit instantiations: `adl_serializer<MemorySourceAccessor::File>`, `adl_serializer<NarListing>`, `adl_serializer<ShallowNarListing>`

## File: src/libutil/fs-sink.cc

### Namespaces
- `nix`

### Classes / structs
- `RestoreSinkSettings` (struct, : `Config`) — declares `preallocate-contents` boolean setting
- `RestoreRegularFile` (struct, : `CreateRegularFileSink, FdSink`) — owns an `AutoCloseFD fd`, optional `startFsync` flag
  - dtor: flushes the FdSink before closing, then triggers async fsync if requested
  - overrides `isExecutable` (Unix: `fchmod` adds execute bits) and `preallocateContents` (Unix: `posix_fallocate`, gated by setting and `HAVE_POSIX_FALLOCATE`)

### Functions / member functions
- `copyRecursive(SourceAccessor &, const CanonPath & from, FileSystemObjectSink & sink, const CanonPath & to)` (free fn) — recursive copy via the `SourceAccessor` and `FileSystemObjectSink` virtual interfaces; handles regular/symlink/directory; throws on non-regular FSO types
- `append(const std::filesystem::path & src, const CanonPath & path)` (static fn) — concat helper for restoring under `dstPath`
- `getParentFdAndName(Descriptor dirFd, const std::filesystem::path & dstPath, const CanonPath & path)` (static fn, Unix) — return `(AutoCloseFD, Descriptor, CanonPath)` triple suitable for *at calls; opens a transient parent dir if needed; the returned `CanonPath` is always a single filename
- `RestoreSink::createDirectory(const CanonPath &, DirectoryCreatedCallback)` — create then open a fresh dirFd, recurse via callback (root is special-cased to avoid double-create)
- `RestoreSink::createDirectory(const CanonPath &)` — Unix: `mkdirat` then open the root dirFd if not yet set; Windows: `std::filesystem::create_directory`
- `RestoreSink::createRegularFile(path, func)` — open new file with `O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC` (prevents symlink follow), then invoke `func` with the `RestoreRegularFile` and flush
- `RestoreRegularFile::isExecutable` (Unix-only effective body) — `fchmod` adds `S_IXUSR|S_IXGRP|S_IXOTH`
- `RestoreRegularFile::preallocateContents(len)` — `posix_fallocate` when `HAVE_POSIX_FALLOCATE` and the setting enabled; ignores `EINVAL/EOPNOTSUPP/ENOSYS`
- `RestoreSink::createSymlink(path, target)` — Unix: `symlinkat`; Windows: `nix::createSymlink`
- `RegularFileSink::createRegularFile(path, func)` — invokes `func` with a tiny inline local `CRF` that forwards bytes to `back.sink`
- `NullFileSystemObjectSink::createRegularFile(path, func)` — supplies an inline anonymous `CreateRegularFileSink` with `skipContents = true` (still calls `func` to advance the parser)

### Macros / globals
- `restoreSinkSettings` (static `RestoreSinkSettings`) — singleton settings instance
- `r1` (static `GlobalConfig::Register`) — registers `restoreSinkSettings`

## File: src/libutil/include/nix/util/fs-sink.hh

### Namespaces
- `nix`

### Classes / structs
- `CreateRegularFileSink` (struct, virtual : `Sink`) — abstract; `bool skipContents = false`, pure virtual `isExecutable`, virtual `preallocateContents(uint64_t)` (default no-op)
- `FileSystemObjectSink` (struct) — abstract; pure virtual `createDirectory(path)`, virtual `createDirectory(path, DirectoryCreatedCallback)` (default delegates to non-callback variant on `*this`), pure virtual `createRegularFile(path, fun<...>)`, pure virtual `createSymlink(path, target)`
  - typedef `DirectoryCreatedCallback = fun<void(FileSystemObjectSink &, const CanonPath &)>`
- `ExtendedFileSystemObjectSink` (struct, virtual : `FileSystemObjectSink`) — adds pure virtual `createHardlink(path, target)`
- `NullFileSystemObjectSink` (struct, : `FileSystemObjectSink`) — directory and symlink overrides are no-ops; regular file declared (defined in .cc)
- `RestoreSink` (struct, : `FileSystemObjectSink`) — fields `std::filesystem::path dstPath`, `AutoCloseFD dirFd`, `bool startFsync = false`; `explicit RestoreSink(bool startFsync)`; overrides directory (both variants), regular, symlink
- `RegularFileSink` (struct, : `FileSystemObjectSink`) — restore a single top-level regular file into a `Sink &`; `bool regular = true`; `createDirectory`/`createSymlink` set `regular = false`; `createRegularFile` declared (defined in .cc)

### Functions
- `copyRecursive(SourceAccessor &, const CanonPath &, FileSystemObjectSink &, const CanonPath &)` (declaration)

## File: src/libutil/nar-accessor.cc

### Namespaces
- `nix`

### Classes / structs
- `NarAccessorImpl` (struct, : `NarAccessor`) — owns a `NarListing root` and optional `std::function<void(uint64_t, uint64_t, Sink &)> getNarBytes`
  - 3 ctors: from string-NAR (parses listing + remembers contents for byte fetches), from existing listing (no fetcher), from listing + fetcher
  - `find(path)`: descend tree by path component, returning pointer or nullptr
  - `get(path)`: throwing variant
  - overrides: `getListing()`, `maybeLstat`, `readDirectory` (entries with no type info), `readFile` (uses `getNarBytes`), `readLink`

### Functions
- `makeNarAccessor(std::string && nar)`, `makeNarAccessor(NarListing)`, `makeLazyNarAccessor(NarListing, GetNarBytes)` — three factories
- `seekableGetNarBytes(const std::filesystem::path &)` — open file once, return closure that owns the fd via `make_ref<AutoCloseFD>` and uses the inner fd-based closure
- `seekableGetNarBytes(Descriptor)` — return closure invoking `copyFdRange` (with off_t/size_t overflow checks)

## File: src/libutil/include/nix/util/nar-accessor.hh

### Namespaces
- `nix`

### Classes / structs
- `NarAccessor` (struct, : `SourceAccessor`) — abstract; pure virtual `getListing()` returning `const NarListing &`

### Type aliases
- `GetNarBytes` (`using`) — `fun<void(uint64_t, uint64_t, Sink &)>`

### Functions
- `makeNarAccessor(std::string && nar)`, `makeNarAccessor(NarListing)`, `makeLazyNarAccessor(NarListing, GetNarBytes)`
- `seekableGetNarBytes(const std::filesystem::path &)`, `seekableGetNarBytes(Descriptor)`

## File: src/libutil/nar-cache.cc

### Namespaces
- `nix`

### Functions / member functions
- `NarCache::NarCache(std::optional<std::filesystem::path> cacheDir_)` — store cacheDir; `createDirs(*cacheDir)` if set
- `NarCache::getOrInsert(const Hash &, fun<void(Sink &)> populate)` — in-memory cache hit fast path; otherwise check on-disk cache (`<hash>.nar` and `<hash>.ls`) using `makeLazyNarAccessor` over a seekable fetch; falls back to populating + writing both files (best-effort, ignores write exceptions except interrupts); inserts resulting accessor into in-memory cache

## File: src/libutil/include/nix/util/nar-cache.hh

### Namespaces
- `nix`

### Classes / structs
- `NarCache` (class) — `std::optional<std::filesystem::path> cacheDir`, `std::map<Hash, ref<SourceAccessor>> nars`; ctor + `getOrInsert(narHash, populate)`

## File: src/libutil/nar-listing.cc

### Namespaces
- `nix`

### Classes / structs
- `NarMemberConstructor` (struct, local to `parseNarListing`; : `CreateRegularFileSink`) — populates a `NarListing::Regular` from byte stream callbacks; `isExecutable` sets executable; `preallocateContents(size)` records `fileSize` and current `pos` as `narOffset`; `operator()` is empty (skipContents = true is set by caller)
- `NarIndexer` (struct, local to `parseNarListing`; : `FileSystemObjectSink, Source`) — stack-based parent tracker that records each FSO into a tree while also tracking byte position from the underlying source
  - fields `std::optional<NarListing> root`, `Source & source`, `std::stack<NarListing *> parents`, `uint64_t pos = 0`
  - `createMember(path, member)` — pop levels until depth matches, then place under parent (or root)
  - overrides `createDirectory`, `createRegularFile` (sets `skipContents = true` on the constructor sink), `createSymlink` (all populate the tree without storing contents)
  - overrides `Source::read` and `skip` to bump `pos` (byte offset within the NAR)

### Functions
- `parseNarListing(Source & source)` (free fn) — instantiate `NarIndexer`, call `parseDump(indexer, indexer)`, return moved root listing
- `ListNarResult<deep>` (template alias, file-scope) — `std::conditional_t<deep, NarListing, ShallowNarListing>`
- `listNarImpl<deep>(SourceAccessor &, const CanonPath &)` (template fn, static) — recursive (deep) or shallow traversal of an accessor producing a NarListing; throws on unsupported FSO types
- `listNarDeep(SourceAccessor &, const CanonPath &)` (free fn) — `listNarImpl<true>`
- `listNarShallow(SourceAccessor &, const CanonPath &)` (free fn) — `listNarImpl<false>`

## File: src/libutil/include/nix/util/nar-listing.hh

### Namespaces
- `nix`

### Classes / structs
- `NarListingRegularFile` (struct) — `std::optional<uint64_t> fileSize`, `std::optional<uint64_t> narOffset` (only set when non-zero per docstring); defaulted `operator<=>`

### Type aliases
- `NarListing` (`using`) — `fso::VariantT<NarListingRegularFile, true>`
- `ShallowNarListing` (`using`) — `fso::VariantT<NarListingRegularFile, false>`

### Functions
- `parseNarListing(Source &)`, `listNarDeep(SourceAccessor &, const CanonPath &)`, `listNarShallow(SourceAccessor &, const CanonPath &)`

## File: src/libutil/tarfile.cc

### Namespaces
- `nix`
- (anonymous; libarchive callbacks)

### Functions / member functions (anonymous ns)
- `callback_open(struct archive *, void *)` — returns `ARCHIVE_OK`
- `callback_read(struct archive *, void * _self, const void ** buffer)` — pull bytes from `TarArchive::source` into the buffer; on `EndOfFile` returns 0; on other exceptions sets archive error and returns -1
- `callback_close(struct archive *, void *)` — returns `ARCHIVE_OK`
- `checkLibArchive(archive *, int err, const std::string & reason)` — translate `ARCHIVE_EOF` to `EndOfFile` exception, other non-OK to `Error` with `archive_error_string`
- `defaultBufferSize` (`constexpr auto`, 65536) — default libarchive read buffer size

### Functions / member functions (`nix` ns)
- `TarArchive::check(int err, const std::string & reason)` — call `checkLibArchive` against `this->archive`
- `getArchiveFilterCodeByName(const std::string & method)` (free fn) — round-trip through a temporary write archive to convert filter name → code
- `enableSupportedFormats(struct archive *)` (static fn) — enable tar+zip+empty formats
- `TarArchive::TarArchive(Source &, bool raw, std::optional<std::string> compression_method)` — set up filters, formats, mac-ext disable, `archive_read_open` with the callbacks
- `TarArchive::TarArchive(const std::filesystem::path &)` — open from filename via `archive_read_open_filename` (16384-byte buffer)
- `TarArchive::close()` — `archive_read_close`
- `TarArchive::~TarArchive()` — `archive_read_free` if archive is non-null
- `extract_archive(TarArchive &, const std::filesystem::path & destDir)` (static fn) — iterate entries, rewriting paths to be relative to `destDir`, force at-least-0500 mode bits on directories, rewrite hardlink targets, and call `archive_read_extract` with `ARCHIVE_EXTRACT_TIME | _SECURE_SYMLINKS | _SECURE_NODOTDOT`
- `unpackTarfile(const std::filesystem::path & tarFile, const std::filesystem::path & destDir)` (free fn) — `createDirs(destDir)` + `extract_archive`
- `unpackTarfileToSink(TarArchive &, ExtendedFileSystemObjectSink &)` (free fn) — iterate entries dispatching to the sink (`createHardlink`/`createDirectory`/`createRegularFile`/`createSymlink`); accumulates max mtime; uses 128 KiB heap buffer; returns the max mtime as `time_t`

### Macros / globals
- `NIX_LIBARCHIVE_NATIVE_PATH_FUNC(func)` (`#define`) — selects between `func` (Unix) and `func##_w` (Windows); `#undef`'d after use

## File: src/libutil/include/nix/util/tarfile.hh

### Namespaces
- `nix`

### Classes / structs
- `TarArchive` (struct) — `struct archive * archive`, `Source * source`, `std::vector<unsigned char> buffer`; `check(int, const std::string & = "failed to extract archive (%s)")`, two ctors (path / `Source &` with raw flag and optional method), deleted copy, defaulted move ctor and move assignment, `close`, dtor

### Functions
- `getArchiveFilterCodeByName(const std::string &)`
- `unpackTarfile(const std::filesystem::path &, const std::filesystem::path &)`
- `unpackTarfileToSink(TarArchive &, ExtendedFileSystemObjectSink &)` returning `time_t`

## File: src/libutil/file-content-address.cc

### Namespaces
- `nix`

### Functions
- `parseFileSerialisationMethodOpt(std::string_view input)` (static fn) — `flat`/`nar` → enum or nullopt
- `parseFileSerialisationMethod(std::string_view)` (free fn) — same but throws `UsageError` on unknown
- `parseFileIngestionMethod(std::string_view)` (free fn) — also recognises `git`; otherwise delegates to serialisation parse and `static_cast`s
- `renderFileSerialisationMethod(FileSerialisationMethod)` — name string
- `renderFileIngestionMethod(FileIngestionMethod)` — same plus `git`
- `dumpPath(const SourcePath &, Sink &, FileSerialisationMethod, PathFilter &)` — switch on method: flat → `path.readFile(sink)`, nar → `path.dumpPath(sink, filter)`
- `restorePath(const std::filesystem::path &, Source &, FileSerialisationMethod, bool startFsync)` — switch on method: flat → `writeFile(...)` with FsSync derived from `startFsync` and `FinalSymlink::DontFollow`, nar → `restorePath(path, source, startFsync)`
- `hashPath(const SourcePath &, FileSerialisationMethod, HashAlgorithm, PathFilter &)` — `HashSink + dumpPath`, returns full `HashResult`
- `hashPath(const SourcePath &, FileIngestionMethod, HashAlgorithm, PathFilter &)` — overload returning `(Hash, std::optional<uint64_t>)`; for git uses `git::dumpHash` (no size returned)

## File: src/libutil/include/nix/util/file-content-address.hh

### Namespaces
- `nix`

### Forward declarations
- `nix::SourcePath` (struct)

### Enums
- `FileSerialisationMethod` (`enum struct`, underlying `uint8_t`) — `Flat`, `NixArchive`
- `FileIngestionMethod` (`enum struct`, underlying `uint8_t`) — `Flat`, `NixArchive`, `Git` (the first two values are bit-compatible with `FileSerialisationMethod` so `static_cast` round-trips them)

### Functions
- `parseFileSerialisationMethod(std::string_view)`, `renderFileSerialisationMethod(FileSerialisationMethod)`
- `parseFileIngestionMethod(std::string_view)`, `renderFileIngestionMethod(FileIngestionMethod)`
- `dumpPath(const SourcePath &, Sink &, FileSerialisationMethod, PathFilter & = defaultPathFilter)`
- `restorePath(const std::filesystem::path &, Source &, FileSerialisationMethod, bool startFsync = false)`
- `hashPath(const SourcePath &, FileSerialisationMethod, HashAlgorithm, PathFilter & = defaultPathFilter)` — returns `HashResult`
- `hashPath(const SourcePath &, FileIngestionMethod, HashAlgorithm, PathFilter & = defaultPathFilter)` — returns `(Hash, std::optional<uint64_t>)`

## Cross-file observations

Several patterns repeat within this shard and may be worth flagging for a deduplication/refactor pass. Each observation has been re-checked against the source.

1. `dumpPath`/`restorePath` overload sets are scattered across multiple compilation units. `archive.hh/.cc` exposes `dumpPath(path, Sink, PathFilter)` plus `dumpPathAndGetMtime`; `source-path.hh/.cc` adds member `SourcePath::dumpPath(Sink, PathFilter)` (delegate); `source-accessor.hh/.cc` adds `SourceAccessor::dumpPath(...)` (the actual NAR algorithm); `file-content-address.hh/.cc` adds method-dispatched `dumpPath(SourcePath, Sink, FileSerialisationMethod, PathFilter)`. Same shape for `restorePath`. Clarifying the relationship between accessor-method, path-helper, and method-dispatched variants in one place would help.

2. The Source/Sink wrapper hierarchy in `serialise.hh` has many small one-shot adapters with similar structure (`TeeSink`/`TeeSource`, `LengthSink`/`LengthSource`, `LambdaSink`/`LambdaSource`, `SizedSource`, `EnsureRead`, `ChainSource`). They are written one-by-one; a base or generator could reduce repetition.

3. Multiple wrapping source accessors clear `displayPrefix` in their constructor (`UnionSourceAccessor`, `MountedSourceAccessorImpl`, `CachingSourceAccessor`) and re-implement near-identical `showPath` / `getPhysicalPath` / `invalidateCache` chains. A common `WrappingSourceAccessor` base could absorb this. (Note: `CachingSourceAccessor::getFingerprint` is a pure passthrough, while `MountedSourceAccessorImpl` and `UnionSourceAccessor` apply an "own fingerprint else delegate" pattern — slightly different shapes but related.)

4. `MountedSourceAccessorImpl::getFingerprint` and `UnionSourceAccessor::getFingerprint` each implement the "if I have my own fingerprint use that; else delegate to the wrapped accessor" pattern; `CachingSourceAccessor::getFingerprint` is a plain passthrough. A virtual mixin could centralise the "own-or-delegate" variant.

5. There are three NAR-tree "indexers"/walkers with very similar structure: `NarAccessorImpl::find/get` (in `nar-accessor.cc`), `NarIndexer::createMember` (in `nar-listing.cc`), and `MemorySourceAccessor::open` (in `memory-source-accessor.cc`). All walk a path, descend into the `Directory` variant, and either find or insert. A generic helper over `fso::VariantT` could replace all three.

6. JSON `adl_serializer` specialisations live in three places: `memory-source-accessor.hh` (declares them via `JSON_IMPL_INNER`/`JSON_IMPL`), `memory-source-accessor/json.cc` (defines them, including the `NarListing::Regular` specialisation which leverages the generic templates), and `file-path.hh` (`json_avoids_null<filesystem::path>`). The header-of-record for each serialiser deserves a docstring.

7. The NAR-magic constant `narVersionMagic1` lives only in `archive.hh`; `archive.cc` uses it both as size bound for the version read and as identity check. Good. The depth bound `narMaxDepth` (currently `static constexpr` inside `archive.cc`) could be moved to `archive.hh` so callers can reason about it.

8. `archive.cc` and `nar-listing.cc` independently implement nearly identical switches over file-system-object types (regular/directory/symlink/...); `tarfile.cc` does the same over `archive_entry_filetype`. A unifying visitor over the tri-typed FSO tag would reduce repetition.

9. `PosixDirectorySourceAccessor` (`posix-source-accessor.cc`) maintains a process-global directory-fd-count throttle via `static inline std::atomic<unsigned> globalDirFdCount` and a registry of `weak_ptr<PosixDirectorySourceAccessor>`. This is the only such mechanism in the shard; if other accessor types start caching fds (e.g. for tarballs), a shared throttle could be extracted.

10. `posix-source-accessor.cc`, `mounted-source-accessor.cc`, and `caching-source-accessor.cc` all use `boost::concurrent_flat_map` (with the `getConcurrent` accessor pattern for read-mostly lookups) — three independent uses of the same idiom; a thin nix-side wrapper might reduce boilerplate.

11. `WindowsSourceAccessor` lives inside `posix-source-accessor.cc` with an in-tree `// @todo Should be moved into a separate file.` comment — a known refactor opportunity flagged in source.

12. Both `tarfile.cc` and `archive.cc` write near-identical loops over `archive_entry_filetype`/`Stat::type` switching on regular/symlink/directory. They are conceptually parallel (and `tarfile.cc::extract_archive` and `tarfile.cc::unpackTarfileToSink` themselves share structure with an in-tree `// FIXME: merge with extract_archive` comment).

13. `serialise.cc`'s `sourceToSink` and `sinkToSource` both build local `SourceToSink`/`SinkToSource` types using boost coroutines; the two adapters share infrastructure (`protected_fixedsize_stack`, lambda-driven `LambdaSink`/`LambdaSource` glue) and could share a base if/when boost coroutines are abstracted.

14. The `MakeError(...)` macro is used in `serialise.hh` (`SerialisationError`), `file-descriptor.hh` (`EndOfFile`), `source-accessor.hh` (`SourceAccessorError`, `FileNotFound`, `NotASymlink`, `NotADirectory`, `NotARegularFile`, `RestrictedPathError`), and `canon-path.hh` (`BadCanonPath`). `SymlinkNotAllowed` (in `source-accessor.hh`) uses `CloneableError` instead — a mix of two error-base systems within one shard.

15. The NAR depth bound `narMaxDepth = 64` (in `archive.cc`) is enforced both during dumping and during parsing, but with subtly different error messages and code paths (one uses `Error("path '%s' exceeds maximum NAR directory depth of %d", ...)`, the other `badArchive("NAR directory nesting exceeds maximum depth of %d", ...)`). The two predicates could share a helper.

16. `RestorePath`-style sinks are scattered: `RestoreSink` in `fs-sink.hh/.cc` writes filesystem paths; `MemorySink` in `memory-source-accessor.hh/.cc` writes an in-memory tree; both implement essentially the same `FileSystemObjectSink` interface plus their own `CreateRegularFileSink` subclass for byte streams (`RestoreRegularFile` and `CreateMemoryRegularFile`). A factoring of the bytes-callback boilerplate is plausible.

17. `getParentFdAndName` in `fs-sink.cc` and `PosixDirectorySourceAccessor::openParent` in `posix-source-accessor.cc` both walk the directory tree to find a parent fd, but with different caching strategies. A shared "open-and-resolve-parent" helper could unify them once both use `openat2`-style semantics.


#include "nix/fetchers/git-promisor.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/ssh.hh"
#include "nix/util/error.hh"
#include "nix/util/serialise.hh"
#include "nix/util/sync.hh"
#include "nix/util/url.hh"
#include "nix/util/util.hh"

#include <git2/errors.h>
#include <git2/global.h>
#include <git2/indexer.h>
#include <git2/odb.h>
#include <git2/repository.h>
#include <git2/sys/odb_backend.h>

#include <atomic>
#include <cstdio>

namespace nix {

namespace pkt {

void appendData(std::string & buf, std::string_view payload)
{
    auto total = payload.size() + kHeaderLen;
    if (total > 0xffff)
        throw Error("pkt-line payload too large (%d bytes)", payload.size());
    char hdr[5];
    std::snprintf(hdr, sizeof(hdr), "%04zx", total);
    buf.append(hdr, kHeaderLen);
    buf.append(payload);
}

void appendFlush(std::string & buf)
{
    buf.append("0000", kHeaderLen);
}

void appendDelim(std::string & buf)
{
    buf.append("0001", kHeaderLen);
}

static unsigned parseHex4(std::string_view s)
{
    if (s.size() < 4)
        throw Error("pkt-line truncated header");
    unsigned v = 0;
    for (size_t i = 0; i < 4; ++i) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F')
            d = 10 + c - 'A';
        else
            throw Error("pkt-line bad hex char '%c'", c);
        v = (v << 4) | d;
    }
    return v;
}

Line readLine(std::string_view & cursor)
{
    if (cursor.size() < kHeaderLen)
        throw Error("pkt-line stream truncated");
    auto len = parseHex4(cursor.substr(0, kHeaderLen));
    cursor.remove_prefix(kHeaderLen);
    if (len == 0)
        return {Line::Kind::Flush, {}};
    if (len == 1)
        return {Line::Kind::Delim, {}};
    if (len == 2)
        return {Line::Kind::ResponseEnd, {}};
    if (len < 4)
        throw Error("pkt-line bad length %u", len);
    auto payloadLen = len - kHeaderLen;
    if (cursor.size() < payloadLen)
        throw Error("pkt-line payload truncated (need %d, have %d)", payloadLen, cursor.size());
    auto payload = cursor.substr(0, payloadLen);
    cursor.remove_prefix(payloadLen);
    return {Line::Kind::Data, payload};
}

/** Read one pkt-line from a streaming `Source` (exact reads; throws at
 *  EOF). `owned` holds the payload bytes backing the returned
 *  `Line::payload` view for `Kind::Data`. Used by the ssh transport,
 *  where the response arrives incrementally over a pipe rather than as
 *  one in-memory buffer. */
Line readLine(Source & source, std::string & owned)
{
    char hdr[kHeaderLen];
    source(hdr, kHeaderLen);
    auto len = parseHex4(std::string_view(hdr, kHeaderLen));
    if (len == 0)
        return {Line::Kind::Flush, {}};
    if (len == 1)
        return {Line::Kind::Delim, {}};
    if (len == 2)
        return {Line::Kind::ResponseEnd, {}};
    if (len < 4)
        throw Error("pkt-line bad length %u", len);
    auto payloadLen = len - kHeaderLen;
    owned.resize(payloadLen);
    source(owned.data(), payloadLen);
    return {Line::Kind::Data, std::string_view(owned)};
}

} // namespace pkt

namespace {

/* ---------- protocol-v2 capability advertisement ----------
 *
 * `GET <url>/info/refs?service=git-upload-pack` with the v2 header
 * returns:
 *
 *     pkt "# service=git-upload-pack\n" (optional, flushed)
 *     pkt "version 2\n"
 *     pkt "agent=...\n"
 *     pkt "fetch=...\n"  ← look here for "filter"
 *     pkt "ls-refs=...\n"
 *     ...
 *     flush
 */

struct V2Caps
{
    bool v2 = false;
    bool fetchSupportsFilter = false;
};

/* Fold one advertisement data-line into `caps`. Shared by the HTTP and
   ssh advertisement readers — the capability lines are identical
   regardless of transport. */
void accumulateCap(V2Caps & caps, std::string_view p)
{
    if (!p.empty() && p.back() == '\n')
        p.remove_suffix(1);
    if (p == "version 2")
        caps.v2 = true;
    else if (p.starts_with("fetch")) {
        /* "fetch" or "fetch=...features..."; check for filter in the
           feature list. */
        if (p.find("filter") != std::string_view::npos)
            caps.fetchSupportsFilter = true;
    }
}

V2Caps probeV2(FileTransfer & ft, const std::string & url)
{
    FileTransferRequest req(url + "/info/refs?service=git-upload-pack");
    req.headers.push_back({"Git-Protocol", "version=2"});
    auto resp = ft.download(req);

    std::string_view cursor = resp.data;
    /* Some servers prepend a "# service=git-upload-pack" sentinel
       packet then a flush; tolerate both shapes. */
    bool sawSentinel = false;
    V2Caps caps;
    while (!cursor.empty()) {
        auto line = pkt::readLine(cursor);
        switch (line.kind) {
        case pkt::Line::Kind::Flush:
            if (!sawSentinel) {
                sawSentinel = true;
                continue;
            }
            return caps;
        case pkt::Line::Kind::Delim:
        case pkt::Line::Kind::ResponseEnd:
            continue;
        case pkt::Line::Kind::Data:
            accumulateCap(caps, line.payload);
            break;
        }
    }
    return caps;
}

/* Read + parse a v2 capability advertisement off a streaming `Source`
   (the ssh transport). The advertisement is a run of data pkt-lines
   terminated by a single flush-pkt; over ssh there is no
   "# service=..." sentinel (that's an HTTP smart-protocol artifact),
   so the first flush ends the advertisement. */
V2Caps readAdvertisement(Source & source)
{
    V2Caps caps;
    std::string owned;
    while (true) {
        auto line = pkt::readLine(source, owned);
        switch (line.kind) {
        case pkt::Line::Kind::Flush:
            return caps;
        case pkt::Line::Kind::Delim:
        case pkt::Line::Kind::ResponseEnd:
            continue;
        case pkt::Line::Kind::Data:
            accumulateCap(caps, line.payload);
            break;
        }
    }
}

/* ---------- sideband demultiplexer ----------
 *
 * Inside the packfile section the server sideband-multiplexes:
 *   payload[0] == 0x01 → pack data
 *   payload[0] == 0x02 → progress (text)
 *   payload[0] == 0x03 → error (text)
 */

void demuxAndIndex(std::string_view body, git_indexer * idx, git_indexer_progress * stats)
{
    std::string_view cursor = body;
    bool inPackfileSection = false;
    while (!cursor.empty()) {
        auto line = pkt::readLine(cursor);
        switch (line.kind) {
        case pkt::Line::Kind::Flush:
            return;
        case pkt::Line::Kind::Delim:
        case pkt::Line::Kind::ResponseEnd:
            continue;
        case pkt::Line::Kind::Data: {
            std::string_view p = line.payload;
            /* Skip section headers like "packfile\n", "shallow-info\n",
               "wanted-refs\n", "acknowledgments\n". */
            if (!inPackfileSection) {
                if (p == "packfile\n") {
                    inPackfileSection = true;
                    continue;
                }
                /* Sections we don't care about — skip until we see
                   packfile or flush. */
                continue;
            }
            if (p.empty())
                continue;
            uint8_t channel = static_cast<uint8_t>(p.front());
            std::string_view payload = p.substr(1);
            switch (channel) {
            case 1: /* pack data */
                if (git_indexer_append(idx, payload.data(), payload.size(), stats))
                    throw Error("git_indexer_append: %s", git_error_last()->message);
                break;
            case 2: /* progress */
                debug("git pack progress: %s", payload);
                break;
            case 3: /* error */
                throw Error("git server error: %s", payload);
            default:
                throw Error("unknown sideband channel %d", channel);
            }
            break;
        }
        }
    }
}

/* ---------- transport-agnostic core ----------
 *
 * The protocol-v2 `command=fetch` exchange is identical regardless of
 * transport: the same pkt-line request body, the same sideband-muxed
 * packfile response indexed into the on-disk ODB. Only how the bytes
 * travel (HTTPS POST vs ssh stdio pipe) differs. These helpers hold
 * the common parts; the two `*Provider` structs supply the transport.
 */

/* Build the v2 `command=fetch want <oid>... [filter blob:none] done`
   pkt-line request body. */
std::string buildFetchRequest(std::span<const Hash> wants, FetchFilter filter)
{
    std::string body;
    pkt::appendData(body, "command=fetch\n");
    pkt::appendData(body, "agent=nix-promisor\n");
    pkt::appendDelim(body);
    switch (filter) {
    case FetchFilter::Blobless:
        pkt::appendData(body, "filter blob:none\n");
        break;
    case FetchFilter::None:
        break;
    }
    for (auto & oid : wants)
        pkt::appendData(body, "want " + oid.gitRev() + "\n");
    pkt::appendData(body, "done\n");
    pkt::appendFlush(body);
    return body;
}

/* Index a sideband-muxed packfile response into `repoPath`'s ODB. The
   indexer writes pack/<sha1>.{pack,idx} under <repoPath>/objects/pack —
   exactly where libgit2's pack backend looks. Callers must
   `git_odb_refresh` afterwards so subsequent reads see the new pack. */
void indexPackResponse(std::string_view response, const std::filesystem::path & repoPath)
{
    std::filesystem::path packDir = repoPath / "objects" / "pack";
    std::filesystem::create_directories(packDir);

    git_indexer * indexer = nullptr;
    git_indexer_progress stats{};
    if (git_indexer_new(&indexer, packDir.string().c_str(), 0, nullptr, nullptr))
        throw Error("git_indexer_new: %s", git_error_last()->message);

    struct IndexerGuard
    {
        git_indexer * p;

        ~IndexerGuard()
        {
            if (p)
                git_indexer_free(p);
        }
    } guard{indexer};

    demuxAndIndex(response, indexer, &stats);

    if (git_indexer_commit(indexer, &stats))
        throw Error("git_indexer_commit: %s", git_error_last()->message);
}

/* ---------- HTTP(S) transport ---------- */

struct HttpGitPromisorProvider : GitPromisorProvider
{
    std::string url;
    std::filesystem::path repoPath;

    HttpGitPromisorProvider(std::string u, std::filesystem::path p)
        : url(std::move(u))
        , repoPath(std::move(p))
    {
    }

    /* The raw capability probe. This is the PRIMITIVE that backs the
       per-URL `gitV2CapsCache` (its default probe fn constructs a
       provider and calls this), so it must NOT itself consult the
       cache — that would recurse. No per-instance memo: a provider is
       short-lived (the gate builds a throwaway one; the attached one
       calls this at most once), so a member cache never served a second
       call. Callers who want a cached answer use `gitV2CapsCache`. */
    bool supportsFilteredFetch() override
    {
        auto caps = probeV2(*getFileTransfer(), url);
        return caps.v2 && caps.fetchSupportsFilter;
    }

    void ensureObjects(std::span<const Hash> wants, FetchFilter filter) override
    {
        if (wants.empty())
            return;
        /* Defensive guard: read the SHARED per-URL cache (already
           populated by the git.cc gate / attach probe), not a fresh
           `probeV2` round-trip. Safe — this is the attached provider, a
           distinct object from the cache's throwaway probe instance, so
           there is no recursion. */
        if (!fetchers::gitV2CapsCache(url).supportsFilter)
            throw Error("remote '%s' does not advertise protocol v2 with fetch filters", url);

        debug("lazy-fetch: backfilling %d missing git object(s) over HTTP from '%s'", wants.size(), url);

        auto body = buildFetchRequest(wants, filter);

        StringSource src(body);
        FileTransferRequest req(url + "/git-upload-pack");
        req.method = HttpMethod::Post;
        req.mimeType = "application/x-git-upload-pack-request";
        req.headers.push_back({"Git-Protocol", "version=2"});
        req.headers.push_back({"Accept", "application/x-git-upload-pack-result"});
        req.data.emplace(src);
        auto resp = getFileTransfer()->upload(req);

        indexPackResponse(resp.data, repoPath);
        debug("lazy-fetch: HTTP backfill of %d object(s) from '%s' indexed into the ODB", wants.size(), url);
    }
};

/* ---------- ssh transport ----------
 *
 * Unlike HTTP (stateless POST), the ssh transport drives one stateful
 * `git-upload-pack` over an stdio pipe. Two consequences vs the HTTP
 * impl:
 *
 *  - We must CONSUME the server's v2 capability advertisement (sent
 *    first, before it reads our command) — that's also where the
 *    capability probe gets its answer, so `ensureObjects` learns
 *    `supportsFilter` for free from the live connection.
 *
 *  - We transmit `GIT_PROTOCOL=version=2` OUT-OF-BAND via ssh
 *    `-oSetEnv=GIT_PROTOCOL=version=2` (see `sshEnvArgs`), keeping the
 *    remote command a BARE `git-upload-pack '<path>'` (see
 *    `gitUploadPackCommand`). The command MUST stay bare: every Git
 *    forge restricts the ssh login to `git-shell`, which allowlists
 *    only `git-upload-pack`/`git-receive-pack`/`git-upload-archive` as
 *    the first word — any prefix (e.g. an inline `env VAR=… `) is
 *    rejected (this was a field failure against GitHub Enterprise;
 *    pinned by `GitUploadPackCommand.IsBareNoPrefix`).
 *
 *    CRITICAL: the `-oSetEnv` must be applied to the ssh CONTROL-MASTER,
 *    not just the per-session command — so it is passed as the
 *    `SSHMaster` `extraSshArgs` (see `master()`), NOT as `startCommand`'s
 *    per-session `extraSshArgs`. OpenSSH fixes environment forwarding at
 *    master-creation time; a session multiplexed over an existing master
 *    inherits the master's env decisions and SILENTLY DROPS a per-session
 *    `-oSetEnv`. (This differs from how git negotiates v2 over ssh: git
 *    uses a one-shot, NON-multiplexed ssh, so its per-session `SendEnv`
 *    is honoured; we multiplex via `SSHMaster`, where only the master's
 *    options take effect.) If the server's sshd lacks
 *    `AcceptEnv GIT_PROTOCOL` the var is still dropped and the exchange
 *    degrades to v0 (no `filter` cap → probe false → full fetch), which
 *    is safe.
 *
 * Auth, host-key checking, `~/.ssh/config`, ssh-agent, and connection
 * sharing are all delegated to Nix's `SSHMaster` (the same path the
 * ssh stores use), so a user who relies on git+ssh for authentication
 * gets exactly their normal ssh behaviour. `SSHMaster` honours
 * `NIX_SSHOPTS`; it does NOT read git's `GIT_SSH_COMMAND` /
 * `core.sshCommand` (a deliberate scope choice — see PROPOSAL §6.2.1).
 */

struct SshGitPromisorProvider : GitPromisorProvider
{
    ParsedURL::Authority authority;
    std::string remotePath;
    std::filesystem::path repoPath;

    /* One multiplexed ssh control-master for the whole life of this
       provider (PF-3). The provider is long-lived per-repo (held in
       `GitAccessorOptions::provider`), so every backfill — incl. the
       per-blob `readBlob` fallback that, on a prefetch miss, can fire
       once per file — rides ONE connection instead of paying a fresh
       TCP+auth+`git-upload-pack` handshake each time. Lazily created on
       first use (a probe of an unsupported remote shouldn't spawn a
       master). `SSHMaster` is non-movable (holds a `Sync`), so hold it
       by `shared_ptr` behind a `Sync`. */
    Sync<std::shared_ptr<SSHMaster>> master_;

    SshGitPromisorProvider(ParsedURL::Authority authority, std::string remotePath, std::filesystem::path repoPath)
        : authority(std::move(authority))
        , remotePath(std::move(remotePath))
        , repoPath(std::move(repoPath))
    {
    }

    /* The bare `git-upload-pack '<path>'` command + the ssh `SetEnv`
       option, both via the exported (unit-tested) helpers so the
       forge-safety property is pinned in one place. */
    OsStrings remoteCommand() const
    {
        return {OS_STR("git-upload-pack"), string_to_os_string(gitUploadPackCommandArg(remotePath))};
    }

    static OsStrings sshEnvArgs()
    {
        return {string_to_os_string(gitProtocolSetEnvArg())};
    }

    SSHMaster & master()
    {
        auto m(master_.lock());
        if (!*m) {
            debug(
                "lazy-fetch: opening multiplexed ssh control-master for '%s' (reused by all later backfills)",
                authority.to_string());
            *m = std::make_shared<SSHMaster>(
                authority,
                /*keyFile=*/std::nullopt,
                /*sshPublicHostKey=*/"",
                /*useMaster=*/true,
                /*compress=*/false,
                /*logFD=*/INVALID_DESCRIPTOR,
                /* extraSshArgs: GIT_PROTOCOL must be set on the MASTER, not just
                   per-session. OpenSSH fixes env-forwarding at master-creation
                   time, so a session multiplexed over the master silently drops
                   a per-session `-oSetEnv`; passing it here makes the v2
                   advertisement (and thus `filter` detection) actually work. */
                /*extraSshArgs=*/sshEnvArgs());
        } else
            debug("lazy-fetch: reusing multiplexed ssh control-master for '%s'", authority.to_string());
        return **m;
    }

    /* The raw capability probe — the primitive backing `gitV2CapsCache`
       for ssh URLs (so it must not consult the cache: recursion). No
       per-instance memo: a provider is short-lived and `ensureObjects`
       re-reads the advertisement on its own live connection anyway. */
    bool supportsFilteredFetch() override
    {
        /* Open a connection, read + parse the advertisement, then drop
           the connection (we send no command). */
        try {
            /* No per-session env args: GIT_PROTOCOL is set on the master (see
               `master()`), which is the only place OpenSSH honours it for a
               multiplexed connection. */
            auto conn = master().startCommand(remoteCommand());
            FdSource source(conn->out.get());
            auto adv = readAdvertisement(source);
            return adv.v2 && adv.fetchSupportsFilter;
        } catch (Error & e) {
            debug("ssh partial-clone capability probe for '%s' failed: %s", authority.to_string(), e.what());
            return false;
        }
    }

    void ensureObjects(std::span<const Hash> wants, FetchFilter filter) override
    {
        if (wants.empty())
            return;

        debug(
            "lazy-fetch: backfilling %d missing git object(s) over ssh from '%s'",
            wants.size(),
            authority.to_string());

        auto conn = master().startCommand(remoteCommand()); // GIT_PROTOCOL is on the master

        /* ONE `FdSource` spans the whole connection: it buffers ahead,
           so the advertisement read and the pack drain must come from
           the same source or buffered bytes past the advertisement's
           flush would be lost. */
        FdSource source(conn->out.get());

        /* Phase 1: the server greets with its v2 advertisement before
           reading our command. Consume + verify it carries `filter`
           (this live read is authoritative — no separate cap cache). */
        auto adv = readAdvertisement(source);
        if (!adv.v2 || !adv.fetchSupportsFilter)
            throw Error(
                "ssh remote '%s' does not advertise protocol v2 with fetch filters", authority.to_string());

        /* Phase 2: send our `command=fetch` request, close our write
           side so the server sees EOF on input, then slurp the
           sideband-muxed packfile response from the SAME source. */
        auto body = buildFetchRequest(wants, filter);
        writeFull(conn->in.get(), body);
        conn->in.close();

        std::string response = source.drain();
        indexPackResponse(response, repoPath);
        debug(
            "lazy-fetch: ssh backfill of %d object(s) from '%s' indexed into the ODB",
            wants.size(),
            authority.to_string());
    }
};

} // anonymous namespace

std::string gitUploadPackCommandArg(std::string_view remotePath)
{
    /* Single-quote as Git does: wrap in '…', and turn any embedded
       single quote into the `'\''` close-reopen sequence. */
    return "'" + replaceStrings(std::string(remotePath), "'", "'\\''") + "'";
}

std::string gitUploadPackCommand(std::string_view remotePath)
{
    /* BARE command — no prefix. See header: a prefix is rejected by the
       `git-shell` allowlist every forge uses. */
    return "git-upload-pack " + gitUploadPackCommandArg(remotePath);
}

std::string gitProtocolSetEnvArg()
{
    return "-oSetEnv=GIT_PROTOCOL=version=2";
}

ref<GitPromisorProvider> makeGitPromisorProvider(std::string url, std::filesystem::path repoPath)
{
    /* Pick the transport by scheme — TOTAL dispatch. `ssh://` (and
       scp-style URLs, which `fixGitURL` normalises to `ssh`) use the
       `SSHMaster` pipe transport; `http`/`https` use the `FileTransfer`
       POST transport. Any other scheme throws rather than silently
       defaulting to HTTP: the `git-lazy-fetch` *creation* gate in
       `git.cc` only marks http/https/ssh remotes, but the provider
       *attach* path runs for ANY repo that is already a partial clone —
       including ones created externally by stock
       `git clone --filter=blob:none` over `git://`/`file://`. Handing
       such a URL to the HTTP transport would mis-probe; an explicit
       throw lets the attach site warn and skip cleanly (see git.cc). */
    auto parsed = parseURL(url);
    if (parsed.scheme == "ssh") {
        if (!parsed.authority)
            throw Error("ssh URL '%s' has no host", url);
        return make_ref<SshGitPromisorProvider>(*parsed.authority, parsed.renderPath(), std::move(repoPath));
    }
    if (parsed.scheme == "http" || parsed.scheme == "https")
        return make_ref<HttpGitPromisorProvider>(std::move(url), std::move(repoPath));
    throw Error(
        "unsupported lazy-fetch transport scheme '%s' for partial-clone remote '%s' "
        "(only http, https, and ssh are supported)",
        parsed.scheme,
        url);
}

} // namespace nix

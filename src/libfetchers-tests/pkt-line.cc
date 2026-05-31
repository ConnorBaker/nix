/* Track J: Git protocol-v2 wire framing.
 *
 * pkt-line and pkt-line-control are pure parsing/serialising logic;
 * they're testable without any network. We exercise:
 *
 *   - Round-trip: encode → decode produces the same data.
 *   - Reserved sizes: 0000/0001/0002 decode as Flush/Delim/ResponseEnd
 *     (no data payload), and the public encoders write them.
 *   - Negative cases: truncated headers, bad hex, declared length <
 *     header, length exceeding 0xffff.
 *   - Buffer advancement: reading consumes exactly the right number
 *     of bytes from the cursor.
 */

#include "nix/fetchers/git-promisor.hh"
#include "nix/util/error.hh"
#include "nix/util/serialise.hh"

#include <gtest/gtest.h>

namespace nix {

TEST(PktLine, RoundTripData)
{
    std::string buf;
    pkt::appendData(buf, "hello\n");
    pkt::appendData(buf, "world");
    pkt::appendFlush(buf);

    std::string_view cursor = buf;

    auto a = pkt::readLine(cursor);
    EXPECT_EQ(a.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(a.payload, "hello\n");

    auto b = pkt::readLine(cursor);
    EXPECT_EQ(b.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(b.payload, "world");

    auto c = pkt::readLine(cursor);
    EXPECT_EQ(c.kind, pkt::Line::Kind::Flush);
    EXPECT_TRUE(cursor.empty());
}

TEST(PktLine, FlushDelimResponseEnd)
{
    /* Hand-crafted: the wire forms of the three reserved sizes. */
    std::string buf;
    buf.append("0000"); // flush
    buf.append("0001"); // delim
    buf.append("0002"); // response-end

    std::string_view cursor = buf;
    EXPECT_EQ(pkt::readLine(cursor).kind, pkt::Line::Kind::Flush);
    EXPECT_EQ(pkt::readLine(cursor).kind, pkt::Line::Kind::Delim);
    EXPECT_EQ(pkt::readLine(cursor).kind, pkt::Line::Kind::ResponseEnd);
}

TEST(PktLine, EncodingMatchesFourHexDigits)
{
    /* Spec: <4 hex digits> encodes total packet length including the
       header. "abc" payload + 4 byte header = 7 → "0007abc". */
    std::string buf;
    pkt::appendData(buf, "abc");
    EXPECT_EQ(buf, "0007abc");
}

TEST(PktLine, EmptyPayloadEncodesAs0004)
{
    /* An empty data payload is still distinct from flush. */
    std::string buf;
    pkt::appendData(buf, "");
    EXPECT_EQ(buf, "0004");

    std::string_view cursor = buf;
    auto line = pkt::readLine(cursor);
    EXPECT_EQ(line.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(line.payload, "");
}

TEST(PktLine, TruncatedHeaderThrows)
{
    std::string_view cursor = "00";
    EXPECT_THROW(pkt::readLine(cursor), Error);

    cursor = "";
    EXPECT_THROW(pkt::readLine(cursor), Error);
}

TEST(PktLine, BadHexThrows)
{
    /* Header has non-hex characters. */
    std::string_view cursor = "zzzz";
    EXPECT_THROW(pkt::readLine(cursor), Error);
}

TEST(PktLine, TruncatedPayloadThrows)
{
    /* Header claims 9 bytes but only 4 are present. */
    std::string_view cursor = "0009ab";
    EXPECT_THROW(pkt::readLine(cursor), Error);
}

TEST(PktLine, RejectsLengthBelowHeader)
{
    /* Length 0003 is <4 (header size) and not one of the reserved
       sizes. The Git wire spec leaves this undefined; we throw. */
    std::string_view cursor = "0003";
    EXPECT_THROW(pkt::readLine(cursor), Error);
}

TEST(PktLine, RejectsOversizedPayload)
{
    /* Payloads must fit in a 4-hex-digit length field plus the
       4-byte header — i.e. payload <= 0xffff - 4. Anything longer is
       a programming error. */
    std::string buf;
    std::string huge(0x10000, 'x');
    EXPECT_THROW(pkt::appendData(buf, huge), Error);
}

TEST(PktLine, MultiPayloadCursorAdvancesExactly)
{
    /* Verify the cursor doesn't skip past or overshoot bytes. */
    std::string buf;
    pkt::appendData(buf, "a");
    pkt::appendData(buf, "bb");
    pkt::appendDelim(buf);
    pkt::appendData(buf, "ccc");

    auto totalSize = buf.size();
    std::string_view cursor = buf;

    /* Each readLine should consume `header + payload` bytes. */
    pkt::readLine(cursor); // 0005a → 5 bytes
    EXPECT_EQ(totalSize - cursor.size(), 5u);
    pkt::readLine(cursor); // 0006bb → 6 bytes
    EXPECT_EQ(totalSize - cursor.size(), 11u);
    pkt::readLine(cursor); // 0001 (delim) → 4 bytes
    EXPECT_EQ(totalSize - cursor.size(), 15u);
    pkt::readLine(cursor); // 0007ccc → 7 bytes
    EXPECT_EQ(totalSize - cursor.size(), 22u);
    EXPECT_TRUE(cursor.empty());
}

TEST(PktLine, ProtocolV2FetchRequestShape)
{
    /* The shape `GitPromisorProvider::ensureObjects` builds:
     *   command=fetch
     *   agent=...
     *   <delim>
     *   filter blob:none
     *   want <oid>
     *   done
     *   <flush>
     */
    std::string body;
    pkt::appendData(body, "command=fetch\n");
    pkt::appendData(body, "agent=test\n");
    pkt::appendDelim(body);
    pkt::appendData(body, "filter blob:none\n");
    pkt::appendData(body, "want abc123\n");
    pkt::appendData(body, "done\n");
    pkt::appendFlush(body);

    std::string_view cursor = body;
    auto a = pkt::readLine(cursor);
    EXPECT_EQ(a.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(a.payload, "command=fetch\n");

    auto b = pkt::readLine(cursor);
    EXPECT_EQ(b.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(b.payload, "agent=test\n");

    auto delim = pkt::readLine(cursor);
    EXPECT_EQ(delim.kind, pkt::Line::Kind::Delim);

    auto filter = pkt::readLine(cursor);
    EXPECT_EQ(filter.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(filter.payload, "filter blob:none\n");

    auto want = pkt::readLine(cursor);
    EXPECT_EQ(want.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(want.payload, "want abc123\n");

    auto done = pkt::readLine(cursor);
    EXPECT_EQ(done.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(done.payload, "done\n");

    auto flush = pkt::readLine(cursor);
    EXPECT_EQ(flush.kind, pkt::Line::Kind::Flush);
    EXPECT_TRUE(cursor.empty());
}

/* ---------- streaming reader (the ssh transport's primitive) ----------
 *
 * The ssh transport reads pkt-lines incrementally off a pipe `Source`
 * rather than from one in-memory buffer. These exercise the
 * `readLine(Source&, std::string&)` overload against a `StringSource`,
 * which delivers bytes via the same exact-read interface a pipe does.
 */

TEST(PktLineStream, RoundTripDataDelimFlush)
{
    std::string buf;
    pkt::appendData(buf, "command=fetch\n");
    pkt::appendDelim(buf);
    pkt::appendData(buf, "want abc\n");
    pkt::appendFlush(buf);

    StringSource source(buf);
    std::string owned;

    auto a = pkt::readLine(source, owned);
    EXPECT_EQ(a.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(a.payload, "command=fetch\n");

    auto d = pkt::readLine(source, owned);
    EXPECT_EQ(d.kind, pkt::Line::Kind::Delim);

    auto w = pkt::readLine(source, owned);
    EXPECT_EQ(w.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(w.payload, "want abc\n");

    auto f = pkt::readLine(source, owned);
    EXPECT_EQ(f.kind, pkt::Line::Kind::Flush);
}

TEST(PktLineStream, ParsesV2AdvertisementShape)
{
    /* The advertisement a v2 `git-upload-pack` greets us with over ssh,
       terminated by a flush. The ssh provider scans the data lines for
       `version 2` and a `fetch=...filter` capability. */
    std::string buf;
    pkt::appendData(buf, "version 2\n");
    pkt::appendData(buf, "agent=git/2.53.0\n");
    pkt::appendData(buf, "ls-refs=unborn\n");
    pkt::appendData(buf, "fetch=shallow wait-for-done filter\n");
    pkt::appendData(buf, "object-format=sha1\n");
    pkt::appendFlush(buf);

    StringSource source(buf);
    std::string owned;
    bool sawV2 = false, sawFilter = false;
    while (true) {
        auto line = pkt::readLine(source, owned);
        if (line.kind == pkt::Line::Kind::Flush)
            break;
        if (line.kind != pkt::Line::Kind::Data)
            continue;
        std::string_view p = line.payload;
        if (!p.empty() && p.back() == '\n')
            p.remove_suffix(1);
        if (p == "version 2")
            sawV2 = true;
        else if (p.starts_with("fetch") && p.find("filter") != std::string_view::npos)
            sawFilter = true;
    }
    EXPECT_TRUE(sawV2);
    EXPECT_TRUE(sawFilter);
}

TEST(PktLineStream, EofMidPacketThrows)
{
    /* Header declares 9 bytes total (5 payload) but the stream ends
       after 2 payload bytes: the exact-read must throw, not hang or
       silently truncate. */
    std::string buf = "0009ab";
    StringSource source(buf);
    std::string owned;
    EXPECT_THROW(pkt::readLine(source, owned), Error);
}

TEST(PktLineStream, OwnedBufferBacksPayload)
{
    /* For Kind::Data the payload view must point into `owned` (the
       caller-supplied backing store), so it stays valid after readLine
       returns. */
    std::string buf;
    pkt::appendData(buf, "payload-bytes");

    StringSource source(buf);
    std::string owned;
    auto line = pkt::readLine(source, owned);
    EXPECT_EQ(line.kind, pkt::Line::Kind::Data);
    EXPECT_EQ(line.payload, "payload-bytes");
    EXPECT_EQ(owned, "payload-bytes");
    EXPECT_EQ(line.payload.data(), owned.data());
}

} // namespace nix

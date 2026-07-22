#include "global.h"
#include "test/test.h"
#include "string_util.h"
#include "peepo_overworld.h"
#include "constants/characters.h"

// Regression coverage for the peer-name sanitizer (PKT_SETNAME ingest). A peer's
// display name is rendered through StringExpandPlaceholders (the in-game nameplate),
// so any 0xF7+ text-engine control byte that survives ingest can stall dialog or,
// via PLACEHOLDER_BEGIN, recurse the expander into a hard-lock for everyone who sees
// the name. SanitizePeepoName must strip those, stop at an early EOS, truncate to the
// buffer, and never leave the name blank.

TEST("Peer name keeps printable glyphs and drops control codes")
{
    u8 dst[PEEPO_NAME_LEN];
    static const u8 src[] = { CHAR_A, PLACEHOLDER_BEGIN, CHAR_B, EXT_CTRL_CODE_BEGIN, CHAR_DYNAMIC };
    static const u8 expected[] = { CHAR_A, CHAR_B, EOS };
    SanitizePeepoName(dst, src, sizeof(src));
    EXPECT_EQ(StringCompare(dst, expected), 0);
}

TEST("Peer name drops the placeholder byte that hard-locks the expander")
{
    u8 dst[PEEPO_NAME_LEN];
    u32 i;
    // The exact 3-byte payload that recursed StringExpandPlaceholders.
    static const u8 src[] = { CHAR_A, PLACEHOLDER_BEGIN, 0x02 };
    SanitizePeepoName(dst, src, sizeof(src));
    for (i = 0; dst[i] != EOS; i++)
        EXPECT(dst[i] != PLACEHOLDER_BEGIN);
}

TEST("Peer name of all control codes falls back to a default")
{
    u8 dst[PEEPO_NAME_LEN];
    static const u8 src[] = { CHAR_DYNAMIC, EXT_CTRL_CODE_BEGIN, PLACEHOLDER_BEGIN };
    static const u8 expected[] = _("Player");
    SanitizePeepoName(dst, src, sizeof(src));
    EXPECT_NE(dst[0], EOS);                      // never blank
    EXPECT_EQ(StringCompare(dst, expected), 0);  // the fallback default
}

TEST("Peer name ingest stops at an early EOS")
{
    u8 dst[PEEPO_NAME_LEN];
    static const u8 src[] = { CHAR_A, EOS, CHAR_B };
    static const u8 expected[] = { CHAR_A, EOS };
    SanitizePeepoName(dst, src, sizeof(src));
    EXPECT_EQ(StringCompare(dst, expected), 0);
}

TEST("Peer name is truncated to the buffer")
{
    u8 dst[PEEPO_NAME_LEN];
    u8 src[PEEPO_NAME_LEN + 8];
    u32 i;
    for (i = 0; i < sizeof(src); i++)
        src[i] = CHAR_A;
    SanitizePeepoName(dst, src, sizeof(src));
    EXPECT_EQ(StringLength(dst), PEEPO_NAME_LEN - 1);
    EXPECT_EQ(dst[PEEPO_NAME_LEN - 1], EOS);
}

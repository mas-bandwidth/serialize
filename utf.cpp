/*
    serialize

    Copyright © 2016 - 2026, Más Bandwidth LLC.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
    Tests for serialize_utf.h, the wstring migration helper: UTF-16 and UTF-32 to UTF-8 and back.

    Both widths are tested explicitly on every platform through the uint16_t and uint32_t entry
    points, and the wchar_t entry points are tested at whatever width this platform's wchar_t
    has, so the Windows legs cover 2 byte wchar_t and every other leg covers 4. The texts are the
    real-text vectors of conformance/string.txt, as explicit bytes and code points so the source
    file encoding can never change them.
*/

#include "serialize.h"
#include "serialize_utf.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check( bool condition, const char * what )
{
    if ( !condition )
    {
        printf( "    FAILED: %s\n", what );
        failures++;
    }
}

struct Text
{
    const char * name;
    const char * utf8;
    uint32_t code_points[16];
    int count;
};

static const Text texts[] =
{
    { "chinese weiqi",              "\xE5\x9B\xB4\xE6\xA3\x8B",                         { 0x56F4, 0x68CB }, 2 },
    { "japanese go stones",         "\xE7\xA2\x81\xE7\x9F\xB3",                         { 0x7881, 0x77F3 }, 2 },
    { "japanese black and white",   "\xE9\xBB\x92/\xE7\x99\xBD",                        { 0x9ED2, 0x2F, 0x767D }, 3 },
    { "korean baduk",               "\xEB\xB0\x94\xEB\x91\x91",                         { 0xBC14, 0xB451 }, 2 },
    { "arabic chess",               "\xD8\xA7\xD9\x84\xD8\xB4\xD8\xB7\xD8\xB1\xD9\x86\xD8\xAC", { 0x0627, 0x0644, 0x0634, 0x0637, 0x0631, 0x0646, 0x062C }, 7 },
    { "combining acute",            "e\xCC\x81",                                        { 0x65, 0x0301 }, 2 },
    { "precomposed e acute",        "\xC3\xA9",                                         { 0x00E9 }, 1 },
    { "emoji",                      "\xF0\x9F\x98\x80",                                 { 0x1F600 }, 1 },
    { "cjk extension b",            "\xF0\xA0\xAE\x9F",                                 { 0x20B9F }, 1 },
    { "highest code point",         "\xF4\x8F\xBF\xBF",                                 { 0x10FFFF }, 1 },
    { "byte order mark kept",       "\xEF\xBB\xBFgo",                                   { 0xFEFF, 0x67, 0x6F }, 3 },
    { "noncharacter kept",          "\xEF\xBF\xBF",                                     { 0xFFFF }, 1 },
    { "ascii",                      "golden",                                           { 0x67, 0x6F, 0x6C, 0x64, 0x65, 0x6E }, 6 },
    { "empty",                      "",                                                 { 0 }, 0 },
};

static int to_utf16( const uint32_t * code_points, int count, uint16_t * units )
{
    int length = 0;
    for ( int i = 0; i < count; i++ )
    {
        const uint32_t code = code_points[i];
        if ( code >= 0x10000 )
        {
            units[length++] = (uint16_t) ( 0xD800 + ( ( code - 0x10000 ) >> 10 ) );
            units[length++] = (uint16_t) ( 0xDC00 + ( ( code - 0x10000 ) & 0x3FF ) );
        }
        else
        {
            units[length++] = (uint16_t) code;
        }
    }
    return length;
}

static void test_round_trips()
{
    for ( int t = 0; t < (int) ( sizeof( texts ) / sizeof( texts[0] ) ); t++ )
    {
        const Text & text = texts[t];
        const int utf8_bytes = (int) strlen( text.utf8 );
        char utf8[64];

        // UTF-32 both ways
        check( serialize_utf32_to_utf8( text.code_points, text.count, utf8, sizeof( utf8 ) ) == utf8_bytes, text.name );
        check( strcmp( utf8, text.utf8 ) == 0, text.name );
        uint32_t utf32[32];
        check( serialize_utf8_to_utf32( text.utf8, utf8_bytes, utf32, 32 ) == text.count, text.name );
        check( memcmp( utf32, text.code_points, (size_t) text.count * sizeof( uint32_t ) ) == 0 && utf32[text.count] == 0, text.name );

        // UTF-16 both ways
        uint16_t expected16[32];
        const int units = to_utf16( text.code_points, text.count, expected16 );
        check( serialize_utf16_to_utf8( expected16, units, utf8, sizeof( utf8 ) ) == utf8_bytes, text.name );
        check( strcmp( utf8, text.utf8 ) == 0, text.name );
        uint16_t utf16[32];
        check( serialize_utf8_to_utf16( text.utf8, utf8_bytes, utf16, 32 ) == units, text.name );
        check( memcmp( utf16, expected16, (size_t) units * sizeof( uint16_t ) ) == 0 && utf16[units] == 0, text.name );

        // wchar_t at this platform's width
        wchar_t wide[32];
        int wide_units = 0;
        if ( sizeof( wchar_t ) >= 4 )
        {
            for ( int i = 0; i < text.count; i++ )
                wide[wide_units++] = (wchar_t) text.code_points[i];
        }
        else
        {
            for ( int i = 0; i < units; i++ )
                wide[wide_units++] = (wchar_t) expected16[i];
        }
        wide[wide_units] = 0;
        check( serialize_wide_to_utf8( wide, utf8, sizeof( utf8 ) ) == utf8_bytes, text.name );
        check( strcmp( utf8, text.utf8 ) == 0, text.name );
        wchar_t wide_back[32];
        check( serialize_utf8_to_wide( text.utf8, wide_back, 32 ) == wide_units, text.name );
        check( memcmp( wide_back, wide, (size_t) ( wide_units + 1 ) * sizeof( wchar_t ) ) == 0, text.name );

        // and the UTF-8 is a string the wire accepts, byte for byte
        uint8_t buffer[128];
        memset( buffer, 0, sizeof( buffer ) );
        serialize::WriteStream writeStream( buffer, 64 );
        char sent[64];
        memcpy( sent, text.utf8, (size_t) utf8_bytes + 1 );
        check( serialize::serialize_string_internal( writeStream, sent, 64 ), text.name );
        writeStream.Flush();
        serialize::ReadStream readStream( buffer, writeStream.GetBytesProcessed() );
        char received[64];
        check( serialize::serialize_string_internal( readStream, received, 64 ), text.name );
        check( strcmp( received, text.utf8 ) == 0, text.name );
        check( readStream.SerializeEnd(), text.name );
    }
}

static void test_lone_surrogates_refused()
{
    char utf8[64];

    // UTF-16: a high without its low, at the end and before an ordinary unit; a low with no high;
    // two highs; a low before a high
    const uint16_t high_at_end[] = { 0x0041, 0xD83D };
    const uint16_t high_then_ascii[] = { 0xD83D, 0x0041 };
    const uint16_t low_alone[] = { 0xDE00 };
    const uint16_t two_highs[] = { 0xD83D, 0xD83D, 0xDE00 };
    const uint16_t reversed_pair[] = { 0xDE00, 0xD83D };
    check( serialize_utf16_to_utf8( high_at_end, 2, utf8, sizeof( utf8 ) ) == -1 && utf8[0] == '\0', "utf16 high surrogate at the end" );
    check( serialize_utf16_to_utf8( high_then_ascii, 2, utf8, sizeof( utf8 ) ) == -1, "utf16 high surrogate before ascii" );
    check( serialize_utf16_to_utf8( low_alone, 1, utf8, sizeof( utf8 ) ) == -1, "utf16 low surrogate alone" );
    check( serialize_utf16_to_utf8( two_highs, 3, utf8, sizeof( utf8 ) ) == -1, "utf16 high surrogate followed by a high" );
    check( serialize_utf16_to_utf8( reversed_pair, 2, utf8, sizeof( utf8 ) ) == -1, "utf16 low before high" );

    // REVERSED WITNESS: the pair itself, in order, is real text and converts
    const uint16_t pair[] = { 0xD83D, 0xDE00 };
    check( serialize_utf16_to_utf8( pair, 2, utf8, sizeof( utf8 ) ) == 4 && strcmp( utf8, "\xF0\x9F\x98\x80" ) == 0, "utf16 surrogate pair converts" );

    // UTF-32: a surrogate code point is never a character, at either end of the block
    const uint32_t surrogate_low_end[] = { 0xD800 };
    const uint32_t surrogate_high_end[] = { 0xDFFF };
    const uint32_t above_unicode[] = { 0x110000 };
    check( serialize_utf32_to_utf8( surrogate_low_end, 1, utf8, sizeof( utf8 ) ) == -1, "utf32 D800" );
    check( serialize_utf32_to_utf8( surrogate_high_end, 1, utf8, sizeof( utf8 ) ) == -1, "utf32 DFFF" );
    check( serialize_utf32_to_utf8( above_unicode, 1, utf8, sizeof( utf8 ) ) == -1, "utf32 above 10FFFF" );
    const uint32_t neighbours[] = { 0xD7FF, 0xE000 };
    check( serialize_utf32_to_utf8( neighbours, 2, utf8, sizeof( utf8 ) ) == 6, "utf32 the neighbours of the surrogate block convert" );

    // REVERSED WITNESS: a converter without the rule would emit ED A0 BD for the lone high
    // surrogate, the encoded surrogate the wire refuses. serialize_string's reader refuses those
    // bytes as invalid_utf8, which is why the helper refuses before they are ever written
    {
        uint8_t buffer[64];
        memset( buffer, 0, sizeof( buffer ) );
        serialize::WriteStream writeStream( buffer, 32 );
        int32_t length = 3;
        writeStream.SerializeInteger( length, 0, 15 );
        uint8_t naive[3] = { 0xED, 0xA0, 0xBD };
        writeStream.SerializeBytes( naive, 3 );
        writeStream.Flush();
        serialize::ReadStream readStream( buffer, writeStream.GetBytesProcessed() );
        char received[16];
        check( serialize::serialize_string_internal( readStream, received, 16 ) == false, "the wire refuses an encoded surrogate" );
        check( readStream.GetRefusal() == serialize::REFUSAL_INVALID_UTF8, "as invalid_utf8" );
    }

    // UTF-8 in: encoded surrogates, overlong forms and truncations are refused by both decoders
    uint16_t utf16[16];
    uint32_t utf32[16];
    const char * malformed[] = { "\xED\xA0\x80", "\xED\xBF\xBF", "\xC0\xAF", "\xE0\x80\xAF", "\xF4\x90\x80\x80", "\xE5\x9B", "\x80", "\xFF" };
    for ( int i = 0; i < (int) ( sizeof( malformed ) / sizeof( malformed[0] ) ); i++ )
    {
        const int bytes = (int) strlen( malformed[i] );
        check( serialize_utf8_to_utf16( malformed[i], bytes, utf16, 16 ) == -1, "malformed utf8 to utf16" );
        check( serialize_utf8_to_utf32( malformed[i], bytes, utf32, 16 ) == -1, "malformed utf8 to utf32" );
    }

    // wchar_t at this platform's width: a lone surrogate in the platform's own representation
    {
        wchar_t lone[3] = { (wchar_t) 0x0041, (wchar_t) 0xD800, 0 };
        check( serialize_wide_to_utf8( lone, utf8, sizeof( utf8 ) ) == -1, "wchar_t lone surrogate" );
    }
}

static void test_limits()
{
    // a zero character inside counted input is refused: serialize_string refuses a NUL among its bytes
    char utf8[8];
    const uint32_t with_nul[] = { 0x41, 0, 0x42 };
    check( serialize_utf32_to_utf8( with_nul, 3, utf8, sizeof( utf8 ) ) == -1, "utf32 NUL inside the input" );
    uint32_t utf32[8];
    check( serialize_utf8_to_utf32( "A\0B", 3, utf32, 8 ) == -1, "utf8 NUL inside the input" );

    // no room: refused, never truncated mid character
    const uint32_t emoji[] = { 0x1F600 };
    char small[4];
    check( serialize_utf32_to_utf8( emoji, 1, small, sizeof( small ) ) == -1 && small[0] == '\0', "four bytes and a terminator need five" );
    char exact[5];
    check( serialize_utf32_to_utf8( emoji, 1, exact, sizeof( exact ) ) == 4, "five is enough" );
    uint16_t pair[2];
    check( serialize_utf8_to_utf16( "\xF0\x9F\x98\x80", 4, pair, 2 ) == -1, "a pair and a terminator need three units" );
}

int main()
{
    printf( "\nserialize_utf.h\n\n" );

    test_round_trips();
    test_lone_surrogates_refused();
    test_limits();

    if ( failures > 0 )
    {
        printf( "\n*** %d UTF TESTS FAILED ***\n\n", failures );
        return 1;
    }

    printf( "*** ALL UTF TESTS PASS (wchar_t is %d bytes here) ***\n\n", (int) sizeof( wchar_t ) );
    return 0;
}

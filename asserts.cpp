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
    The write side debug assertions, tested by observing them fire.

    A serialize macro that narrows the caller's value before asserting on it cannot report
    the input the assertion exists to diagnose: the assertion sees a value already truncated
    to the narrower width, and 2^32 + 5 truncates to 5, which is in range by construction.
    Every macro that narrows now validates the caller's original expression first, and this
    file is where that ordering is proved.

    serialize_assert is defined here, before serialize.h, which the header honors. A
    recording definition makes "the assertion fired" an ordinary observable result rather
    than a process death: it runs identically on all three platforms in the matrix and in
    both build configurations, where an aborting assertion would need a death test whose
    shape differs on each of them, and it also names WHICH assertion fired.

    InitializePadded's too-small destination is a memory claim, not a decoded-value one.
    The unit test can only exercise the refusal under NDEBUG, because a live assert aborts
    first. Recording here lets both halves run in every configuration, including the
    sanitizer job.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int assert_fires = 0;
static const char * assert_condition = "";

#define serialize_assert( condition )                                                       \
    do                                                                                      \
    {                                                                                       \
        if ( !( condition ) )                                                               \
        {                                                                                   \
            /* the FIRST assertion to fire is the diagnostic one: a case that gets past  */ \
            /* it goes on to trip the downstream assertions the narrowing used to hide   */ \
            if ( assert_fires == 0 )                                                        \
            {                                                                               \
                assert_condition = #condition;                                              \
            }                                                                               \
            assert_fires++;                                                                 \
        }                                                                                   \
    }                                                                                       \
    while ( 0 )

#include "serialize.h"

static int failures = 0;

static void check( bool condition, const char * what )
{
    printf( "    %-62s %s\n", what, condition ? "pass" : "FAILED" );
    if ( !condition )
    {
        failures++;
    }
}

// the assertion counter is global to the translation unit, so each case starts from zero
static void reset()
{
    assert_fires = 0;
    assert_condition = "";
}

static void test_value_in_int_relative_domain()
{
    printf( "  serialize::value_in_int_relative_domain\n" );

    check( serialize::value_in_int_relative_domain( 0 ), "zero is in the domain" );
    check( serialize::value_in_int_relative_domain( 2147483647 ), "the top of the domain is in it" );
    check( !serialize::value_in_int_relative_domain( -1 ), "a negative value is out of the domain" );
    check( !serialize::value_in_int_relative_domain( int64_t( 2147483648LL ) ), "2^31 is out of the domain" );

    // the point of the helper: a wide value whose low 32 bits are in the domain
    check( !serialize::value_in_int_relative_domain( int64_t( 4294967301LL ) ), "2^32 + 5 is out of the domain, not 5" );
    check( !serialize::value_in_int_relative_domain( uint64_t( 1 ) << 63 ), "2^63 is out of the domain" );
    check( !serialize::value_in_int_relative_domain( uint32_t( 4294967295u ) ), "an unsigned value above the domain is out of it" );
}

static void test_write_int_relative_asserts_on_the_callers_value()
{
    printf( "  write_int_relative\n" );

    uint8_t buffer[64];

    // a wide current whose low 32 bits are in the domain and above previous. Narrowed to int
    // first, this is 5, which is a legal current for a previous of 4 -- and that is what the
    // helper's assertion used to be handed.
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 4294967301LL;                   // 2^32 + 5
        const int previous = 4;
        write_int_relative( stream, previous, current );
        check( assert_fires > 0, "a wide out of domain current trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }

    // the negative control: the same call one step inside the domain must not assert
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 5;
        const int previous = 4;
        write_int_relative( stream, previous, current );
        check( assert_fires == 0, "an in domain current does not trip it" );
    }

    // a current at the top of the domain is legal, and 2^31 is one step outside it
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 2147483647LL;
        const int previous = 2147483646;
        write_int_relative( stream, previous, current );
        check( assert_fires == 0, "a current at the top of the domain does not trip it" );
    }

    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 2147483648LL;
        const int previous = 2147483646;
        write_int_relative( stream, previous, current );
        check( assert_fires > 0, "a current one step above the domain trips it" );
        printf( "      assertion: %s\n", assert_condition );
    }
}

#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )

template <typename Stream> bool serialize_compile_time_bounded( Stream & stream, int64_t & value )
{
    serialize_int_compile_time( stream, value, 0, 1000 );
    return true;
}

static void test_serialize_int_compile_time_asserts_on_the_callers_value()
{
    printf( "  serialize_int_compile_time\n" );

    uint8_t buffer[64];

    // 2^32 narrows to 0, which is in [0,1000]: the assertion inside SerializeIntConst is
    // handed a value the narrowing already made legal
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        int64_t value = 4294967296LL;
        serialize_compile_time_bounded( stream, value );
        check( assert_fires > 0, "a wide out of range value trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }

    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        int64_t value = 500;
        serialize_compile_time_bounded( stream, value );
        check( assert_fires == 0, "an in range value does not trip it" );
    }
}

#endif // #if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )

static void test_initialize_padded_refuses_a_short_destination()
{
    printf( "  ReadStream::InitializePadded\n" );

    uint8_t payload[11];
    memset( payload, 0xAB, sizeof( payload ) );

    {
        reset();
        uint8_t dest[16];
        memset( dest, 0xFF, sizeof( dest ) );
        serialize::ReadStream stream;
        bool ok = stream.InitializePadded( dest, (int64_t) sizeof( dest ), payload, (int64_t) sizeof( payload ) );
        check( assert_fires > 0, "a destination that cannot hold payload+8 trips the assertion" );
        check( ok == false, "and refuses in every build" );
        check( dest[0] == (uint8_t) 0xFF, "copying nothing" );
        uint32_t after = 0xFFFFFFFFu;
        check( stream.SerializeBits( after, 8 ) == false, "the stream is latched" );
        check( after == 0xFFFFFFFFu, "a refused read leaves the caller's value" );
        printf( "      assertion: %s\n", assert_condition );
    }

    {
        reset();
        uint8_t dest[19];
        memset( dest, 0xFF, sizeof( dest ) );
        serialize::ReadStream stream;
        bool ok = stream.InitializePadded( dest, (int64_t) sizeof( dest ), payload, (int64_t) sizeof( payload ) );
        check( assert_fires == 0, "exactly payload+8 does not trip it" );
        check( ok == true, "and is accepted" );
        check( dest[0] == (uint8_t) 0xAB, "the payload is copied" );
        check( dest[11] == 0, "the slack is zeroed" );
    }
}

static void write_float_bits( uint32_t bits )
{
    uint8_t buffer[64];
    serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
    float value;
    memcpy( &value, &bits, 4 );
    serialize::serialize_float_internal( stream, value );
}

static void write_double_bits( uint64_t bits )
{
    uint8_t buffer[64];
    serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
    double value;
    memcpy( &value, &bits, 8 );
    serialize::serialize_double_internal( stream, value );
}

static void test_float_and_double_writes_assert_finite()
{
    printf( "  serialize_float, serialize_double, write_float, write_double\n" );

    // STANDARD.md, "Floating Point": the format carries finite values only. Writing NaN or an
    // infinity is the caller's contract violation, asserted like an out of range int
    const uint32_t refused_floats[] = { 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFFFFFFFu };
    for ( int i = 0; i < (int) ( sizeof( refused_floats ) / sizeof( refused_floats[0] ) ); i++ )
    {
        reset();
        write_float_bits( refused_floats[i] );
        check( assert_fires > 0, "a non-finite float trips the assertion" );
    }
    printf( "      assertion: %s\n", assert_condition );

    const uint64_t refused_doubles[] = { 0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x7FF8000000000000ULL, 0x7FF0000000000001ULL };
    for ( int i = 0; i < (int) ( sizeof( refused_doubles ) / sizeof( refused_doubles[0] ) ); i++ )
    {
        reset();
        write_double_bits( refused_doubles[i] );
        check( assert_fires > 0, "a non-finite double trips the assertion" );
    }
    printf( "      assertion: %s\n", assert_condition );

    // the negative control: -0, the subnormals and the largest finite values are finite
    const uint32_t kept_floats[] = { 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu };
    for ( int i = 0; i < (int) ( sizeof( kept_floats ) / sizeof( kept_floats[0] ) ); i++ )
    {
        reset();
        write_float_bits( kept_floats[i] );
        check( assert_fires == 0, "a finite float, -0 and subnormals included, does not trip it" );
    }
    const uint64_t kept_doubles[] = { 0x8000000000000000ULL, 0x0000000000000001ULL, 0x7FEFFFFFFFFFFFFFULL };
    for ( int i = 0; i < (int) ( sizeof( kept_doubles ) / sizeof( kept_doubles[0] ) ); i++ )
    {
        reset();
        write_double_bits( kept_doubles[i] );
        check( assert_fires == 0, "a finite double does not trip it" );
    }

    // write_float narrows its argument to float first, and a finite double above FLT_MAX
    // narrows to an infinity: the assertion sees what would be sent
    {
        reset();
        uint8_t buffer[64];
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const double huge = 1.0e300;
        write_float( stream, huge );
        check( assert_fires > 0, "write_float of a double that narrows to infinity trips it" );
    }
    {
        reset();
        uint8_t buffer[64];
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        write_float( stream, 1.5 );
        write_double( stream, -0.0 );
        check( assert_fires == 0, "write_float and write_double of finite values do not trip it" );
    }
    {
        reset();
        uint8_t buffer[64];
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        uint64_t nan_bits = 0x7FF8000000000000ULL;
        double nan_value;
        memcpy( &nan_value, &nan_bits, 8 );
        write_double( stream, nan_value );
        check( assert_fires > 0, "write_double of NaN trips it" );
    }
}

static void test_compressed_float_declaration_top_must_decode_finite()
{
    printf( "  serialize_compressed_float declaration\n" );

    // [3 * 2^103, FLT_MAX] at resolution 2^104: delta and values are finite, and the top
    // integer decodes to min + delta, which rounds to +inf. A non-conforming declaration
    const float min = 3.0f * 10141204801825835211973625643008.0f;
    const float max = 340282346638528859811704183484516925440.0f;
    const float res = 20282409603651670423947251286016.0f;
    {
        reset();
        uint8_t buffer[64];
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        float value = 1.0e38f;
        serialize::serialize_compressed_float_internal( stream, value, min, max, res );
        check( assert_fires > 0, "writing through a declaration whose top decodes to infinity trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }
    {
        reset();
        uint8_t buffer[64];
        serialize::ReadStream stream( buffer, 8 );
        float value = 0.0f;
        serialize::serialize_compressed_float_internal( stream, value, min, max, res );
        check( assert_fires == 0, "reading through it does not: the reader refuses at runtime instead" );
    }
    {
        reset();
        uint8_t buffer[64];
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        float value = 1.0f;
        const float delta = max - min;
        serialize::serialize_compressed_float_precomputed_internal( stream, value, 16777214u, 24, delta, min );
        check( assert_fires > 0, "and so does writing through its precomputed constants" );
    }
    {
        reset();
        uint8_t buffer[64];
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        float value = 3.0e38f;
        serialize::serialize_compressed_float_internal( stream, value, 0.0f, 3.0e38f, 1.0e30f );
        check( assert_fires == 0, "writing the top of a wide declaration that decodes finite does not trip it" );
    }
}

// serialize_reject is a read side refusal. On a write or measure stream there is nothing to
// refuse, so calling it is caller error, asserted like any other write side misuse; with the
// recording assert the macro still returns false, which is what a release build does.

template <typename Stream> static bool run_reject( Stream & stream )
{
    serialize_reject( stream );
}

static void test_reject_asserts_on_write_and_measure()
{
    printf( "  serialize_reject\n" );

    uint8_t buffer[64];
    memset( buffer, 0, sizeof( buffer ) );
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const bool result = run_reject( stream );
        check( assert_fires > 0, "rejecting on a write stream trips the assertion" );
        check( result == false, "and the macro still returns false" );
        printf( "      assertion: %s\n", assert_condition );
    }
    {
        reset();
        serialize::MeasureStream stream;
        const bool result = run_reject( stream );
        check( assert_fires > 0, "rejecting on a measure stream trips the assertion" );
        check( result == false, "and the macro still returns false" );
    }
    {
        reset();
        serialize::ReadStream stream( buffer, 8 );
        const bool result = run_reject( stream );
        check( assert_fires == 0, "rejecting on a read stream does not: it is a refusal" );
        check( result == false && stream.GetRefusal() == serialize::REFUSAL_REJECTED, "and it refuses as rejected" );
    }
}

// IMPLEMENTING.md U5: serialize_int's bounds are int32. A bound outside int32 is caller error,
// caught on the caller's original expression, never narrowed into a silently different field

template <typename Stream> static bool run_int( Stream & stream, int64_t value, int64_t min, int64_t max )
{
    serialize_int( stream, value, min, max );
    return true;
}

static void test_int_bounds_are_int32()
{
    printf( "  serialize_int bounds\n" );

    uint8_t buffer[64];
    memset( buffer, 0, sizeof( buffer ) );
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        run_int( stream, 5, 0, int64_t( 4294967301LL ) );                     // 2^32 + 5: narrowed, a legal max of 5
        check( assert_fires > 0, "a max past int32 trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }
    {
        reset();
        serialize::MeasureStream stream;
        run_int( stream, 0, int64_t( -2147483647LL ) - 2, 10 );
        check( assert_fires > 0, "a min below int32 trips it on a measure too" );
    }
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        run_int( stream, 0, int64_t( -2147483647LL ) - 1, int64_t( 2147483647LL ) );
        check( assert_fires == 0, "the full int32 range does not" );
    }
}

// IMPLEMENTING.md U10: a write checks room for the whole operation before it writes any of it.
// With the recording assertion the operation carries on, so the FIRST assertion to fire is the
// one that names the whole operation, ahead of any piece of it

template <typename Stream> static bool run_string( Stream & stream, char * string, int buffer_size )
{
    serialize_string( stream, string, buffer_size );
    return true;
}

static void test_write_checks_room_for_the_whole_operation()
{
    printf( "  whole operation room\n" );

    uint8_t buffer[64];
    memset( buffer, 0, sizeof( buffer ) );
    {
        reset();
        serialize::WriteStream stream( buffer, 8 );                             // 64 bits
        uint32_t lead = 0;
        stream.SerializeBits( lead, 3 );
        char text[16] = "abcdefgh";                                             // 4 + 1 + 64 bits after the 3, 61 left
        run_string( stream, text, 16 );
        check( assert_fires > 0, "a string whose payload does not fit trips the assertion" );
        check( strstr( assert_condition, "serialize_write_has_room" ) != NULL, "and the first to fire is the whole operation check" );
        printf( "      assertion: %s\n", assert_condition );
    }
    {
        reset();
        serialize::WriteStream stream( buffer, 8 );
        uint32_t lead = 0;
        stream.SerializeBits( lead, 3 );
        char text[16] = "abcdef";                                               // 3 + 4 + 1 + 48 + ... fits
        run_string( stream, text, 16 );
        check( assert_fires == 0, "a string that fits does not" );
    }
    {
        reset();
        serialize::WriteStream stream( buffer, 8 );
        uint32_t lead = 0;
        stream.SerializeBits( lead, 1 );
        uint8_t data[8] = { 0 };
        stream.SerializeBytes( data, 8 );                                       // 1 + 7 + 64 bits of a 64 bit buffer
        check( assert_fires > 0, "bytes whose payload does not fit after the align trip the assertion" );
        check( strstr( assert_condition, "GetAlignBits" ) != NULL, "and the first to fire counts the align with the payload" );
        printf( "      assertion: %s\n", assert_condition );
    }
}

// IMPLEMENTING.md U9 and U14: serialize_fail on a write or measure stream returns false and never
// asserts: it is the one routing every refusal takes, and only a read stream has anything to latch

static void test_fail_on_write_and_measure_never_asserts()
{
    printf( "  serialize_fail on write and measure\n" );

    uint8_t buffer[64];
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        check( serialize::serialize_fail( stream, serialize::REFUSAL_PAST_END ) == false && assert_fires == 0, "a write stream returns false without asserting" );
    }
    {
        reset();
        serialize::MeasureStream stream;
        check( serialize::serialize_fail( stream, serialize::REFUSAL_REJECTED ) == false && assert_fires == 0, "a measure stream returns false without asserting" );
    }
}

// The reference audit's narrowing reproducer (audit/int64_narrowing_assert.cpp): the int64 and
// int128 write assertions used to compare int64_t( value ), after the narrowing, so an unsigned
// 2^64 - 1 became -1, landed inside [-1, 0], and went on the wire as -1 with no assertion. The
// assertion now judges the caller's value as passed, sign included

template <typename Stream> static bool run_int64( Stream & stream, uint64_t & value )
{
    serialize_int64( stream, value, -1, 0 );
    return true;
}

template <typename Stream> static bool run_write_int64( Stream & stream, uint64_t value )
{
    write_int64( stream, value, -1, 0 );
    return true;
}

template <typename Stream> static bool run_int_wide( Stream & stream, uint64_t & value )
{
    serialize_int( stream, value, -1, 0 );
    return true;
}

template <typename Stream> static bool run_int128( Stream & stream, const serialize::uint128_t & value )
{
    write_int128( stream, value, serialize::int128_t( -1 ), serialize::int128_t( 0 ) );     // write only: an unsigned 128 bit destination has no read form on the emulated type
    return true;
}

template <typename Stream> static bool run_write_int64_unsigned_bounds( Stream & stream, int64_t value, uint64_t min, uint64_t max )
{
    write_int64( stream, value, min, max );
    return true;
}

template <typename Stream> static bool run_int64_unsigned_bounds( Stream & stream, int64_t & value, uint64_t min, uint64_t max )
{
    serialize_int64( stream, value, min, max );
    return true;
}

#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
template <typename Stream> static bool run_int64_compile_time( Stream & stream, uint64_t & value )
{
    serialize_int64_compile_time( stream, value, -1, 0 );
    return true;
}
#endif // #if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )

static void test_int64_asserts_before_narrowing()
{
    printf( "  int64 and int128 range assertions before narrowing\n" );

    uint8_t buffer[64];
    memset( buffer, 0, sizeof( buffer ) );
    const uint64_t all_ones = ~uint64_t( 0 );
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        uint64_t value = all_ones;
        run_int64( stream, value );
        check( assert_fires > 0, "serialize_int64: an unsigned 2^64 - 1 in [-1, 0] trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        run_write_int64( stream, all_ones );
        check( assert_fires > 0, "write_int64: the same" );
    }
    {
        reset();
        serialize::MeasureStream stream;
        uint64_t value = all_ones;
        run_int64( stream, value );
        check( assert_fires > 0, "and on a measure stream" );
    }
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        uint64_t value = all_ones;
        run_int_wide( stream, value );
        check( assert_fires > 0, "serialize_int: an unsigned 2^64 - 1 in [-1, 0] trips it too" );
    }
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        serialize::uint128_t value = ~serialize::uint128_t( 0 );
        run_int128( stream, value );
        check( assert_fires > 0, "write_int128: an unsigned 2^128 - 1 in [-1, 0] trips the assertion" );
    }
#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        uint64_t value = all_ones;
        run_int64_compile_time( stream, value );
        check( assert_fires > 0, "serialize_int64_compile_time: the same" );
    }
#endif // #if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        uint64_t value = 0;
        run_int64( stream, value );
        check( assert_fires == 0, "an unsigned 0 in [-1, 0] does not" );
    }

    // U16: a bound that arrives as an unsigned 64 bit value above INT64_MAX does not fit, though it
    // converts to a legal negative int64
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        run_write_int64_unsigned_bounds( stream, 0, all_ones, 0 );
        check( assert_fires > 0, "write_int64: an unsigned 2^64 - 1 min trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }
    {
        reset();
        serialize::MeasureStream stream;
        int64_t value = 0;
        run_int64_unsigned_bounds( stream, value, all_ones, 0 );
        check( assert_fires > 0, "serialize_int64 on a measure: the same" );
    }
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        int64_t value = 0;
        run_int64_unsigned_bounds( stream, value, 0, 1 );
        check( assert_fires == 0, "unsigned bounds that fit do not" );
    }
}

// IMPLEMENTING.md U15: a split write form makes every write-misuse check its unified form
// makes, through the same mechanism. Each case runs the same misuse through the serialize_
// macro and through its write_ twin, and both must trip an assertion.

template <typename Stream> static bool unified_bits( Stream & stream, uint64_t value, int bits ) { serialize_bits( stream, value, bits ); return true; }
template <typename Stream> static bool unified_uint8( Stream & stream, uint32_t value ) { serialize_uint8( stream, value ); return true; }
template <typename Stream> static bool unified_int( Stream & stream, int32_t value, int32_t min, int32_t max ) { serialize_int( stream, value, min, max ); return true; }
template <typename Stream> static bool unified_int64( Stream & stream, int64_t value, int64_t min, int64_t max ) { serialize_int64( stream, value, min, max ); return true; }
template <typename Stream> static bool unified_int128( Stream & stream, serialize::int128_t value, serialize::int128_t min, serialize::int128_t max ) { serialize_int128( stream, value, min, max ); return true; }
template <typename Stream> static bool unified_double( Stream & stream, double value ) { serialize_double( stream, value ); return true; }
template <typename Stream> static bool unified_string( Stream & stream, char * value, int buffer_size ) { serialize_string( stream, value, buffer_size ); return true; }
template <typename Stream> static bool unified_int_relative( Stream & stream, int previous, int current ) { serialize_int_relative( stream, previous, current ); return true; }
template <typename Stream> static bool unified_int_wide_bounds( Stream & stream, int32_t value, int64_t min, int64_t max ) { serialize_int( stream, value, min, max ); return true; }
template <typename Stream> static bool unified_float( Stream & stream, float value ) { serialize_float( stream, value ); return true; }
template <typename Stream> static bool unified_fixed( Stream & stream, int64_t value ) { serialize_fixed( stream, value, 48, 16, -10, 10 ); return true; }
template <typename Stream> static bool unified_bytes( Stream & stream, uint8_t * data, int bytes ) { serialize_bytes( stream, data, bytes ); return true; }

static void split_bits( serialize::WriteStream & stream, uint64_t value, int bits ) { write_bits( stream, value, bits ); }
static void split_uint8( serialize::WriteStream & stream, uint32_t value ) { write_uint8( stream, value ); }
static void split_int( serialize::WriteStream & stream, int32_t value, int32_t min, int32_t max ) { write_int( stream, value, min, max ); }
static void split_int64( serialize::WriteStream & stream, int64_t value, int64_t min, int64_t max ) { write_int64( stream, value, min, max ); }
static void split_int128( serialize::WriteStream & stream, serialize::int128_t value, serialize::int128_t min, serialize::int128_t max ) { write_int128( stream, value, min, max ); }
static void split_double( serialize::WriteStream & stream, double value ) { write_double( stream, value ); }
static void split_string( serialize::WriteStream & stream, char * value, int buffer_size ) { write_string( stream, value, buffer_size ); }
static void split_int_relative( serialize::WriteStream & stream, int previous, int current ) { write_int_relative( stream, previous, current ); }
static void split_int_wide_bounds( serialize::WriteStream & stream, int32_t value, int64_t min, int64_t max ) { write_int( stream, value, min, max ); }
static void split_float( serialize::WriteStream & stream, float value ) { write_float( stream, value ); }
static void split_fixed( serialize::WriteStream & stream, int64_t value ) { write_fixed( stream, value, 48, 16, -10, 10 ); }
static void split_bytes( serialize::WriteStream & stream, uint8_t * data, int bytes ) { write_bytes( stream, data, bytes ); }

static uint8_t g_twin_buffer[256];

static serialize::WriteStream & fresh_twin_stream( serialize::WriteStream & stream )
{
    memset( g_twin_buffer, 0, sizeof( g_twin_buffer ) );
    stream.Initialize( g_twin_buffer, (int64_t) sizeof( g_twin_buffer ) );
    reset();
    return stream;
}

// a stream over 8 bytes of the 256 byte twin buffer: a write that the room assertion catches
// still lands inside the real allocation, because the recording assert does not stop it
static serialize::WriteStream & short_twin_stream( serialize::WriteStream & stream )
{
    memset( g_twin_buffer, 0, sizeof( g_twin_buffer ) );
    stream.Initialize( g_twin_buffer, 8 );
    reset();
    return stream;
}

// both the serialize_ macro and its write_ twin must trip an assertion for the same misuse
static void check_twins( const char * what, int unified_fires, int split_fires )
{
    check( unified_fires > 0 && split_fires > 0, what );
}

static void test_write_forms_catch_misuse_as_their_twins()
{
    printf( "  write_ forms against their serialize_ twins (U15)\n" );

    serialize::WriteStream stream;
    int unified_fires = 0;
    double infinity = 1.0;
    for ( int i = 0; i < 2000; i++ )
        infinity *= 10.0;
    char long_string[16];
    memset( long_string, 0, sizeof( long_string ) );
    memcpy( long_string, "too long", 8 );

    fresh_twin_stream( stream ); (void) unified_bits( stream, 4, 2 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_bits( stream, 4, 2 );
    check_twins( "bits: a value wider than its field", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_bits( stream, uint64_t( 1 ) << 40, 40 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_bits( stream, uint64_t( 1 ) << 40, 40 );
    check_twins( "bits: a 64 bit value wider than its field", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_uint8( stream, 256 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_uint8( stream, 256 );
    check_twins( "uint8: 256", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_int( stream, 11, 0, 10 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_int( stream, 11, 0, 10 );
    check_twins( "int: a value above max", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_int( stream, 5, 10, 0 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_int( stream, 5, 10, 0 );
    check_twins( "int: min above max", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_int64( stream, -11, -10, 10 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_int64( stream, -11, -10, 10 );
    check_twins( "int64: a value below min", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_int128( stream, serialize::int128_t( 11 ), serialize::int128_t( 0 ), serialize::int128_t( 10 ) ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_int128( stream, serialize::int128_t( 11 ), serialize::int128_t( 0 ), serialize::int128_t( 10 ) );
    check_twins( "int128: a value above max", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_double( stream, infinity ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_double( stream, infinity );
    check_twins( "double: an infinity", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_string( stream, long_string, 4 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_string( stream, long_string, 4 );
    check_twins( "string: longer than buffer_size - 1", unified_fires, assert_fires );

    fresh_twin_stream( stream ); (void) unified_int_relative( stream, 100, 100 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_int_relative( stream, 100, 100 );
    check_twins( "int_relative: current not above previous", unified_fires, assert_fires );

    // U4: a string that is not well formed UTF-8
    char bad_utf8[16];
    memset( bad_utf8, 0, sizeof( bad_utf8 ) );
    memcpy( bad_utf8, "a\xC3\x28z", 4 );
    fresh_twin_stream( stream ); (void) unified_string( stream, bad_utf8, 16 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_string( stream, bad_utf8, 16 );
    check_twins( "string: not well formed UTF-8 (U4)", unified_fires, assert_fires );

    // U4, the NUL half: a C++ string's length is strlen, so an interior NUL cannot reach the
    // wire from either form (IMPLEMENTING.md U4, C++ note). Both forms write the prefix before
    // it, the same bytes, and neither has anything to assert
    {
        char with_nul[16];
        memset( with_nul, 0, sizeof( with_nul ) );
        memcpy( with_nul, "ab\0cd", 5 );
        fresh_twin_stream( stream ); (void) unified_string( stream, with_nul, 16 ); stream.Flush();
        unified_fires = assert_fires;
        uint8_t unified_bytes_out[8];
        memcpy( unified_bytes_out, g_twin_buffer, sizeof( unified_bytes_out ) );
        fresh_twin_stream( stream ); split_string( stream, with_nul, 16 ); stream.Flush();
        check( unified_fires == 0 && assert_fires == 0 && memcmp( unified_bytes_out, g_twin_buffer, sizeof( unified_bytes_out ) ) == 0,
               "string: an interior NUL stops both forms alike (U4, by construction)" );
    }

    // U5: int bounds outside int32, caught on the caller's original bounds
    fresh_twin_stream( stream ); (void) unified_int_wide_bounds( stream, 0, -( int64_t( 1 ) << 40 ), 10 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_int_wide_bounds( stream, 0, -( int64_t( 1 ) << 40 ), 10 );
    check_twins( "int: a bound outside int32 (U5)", unified_fires, assert_fires );

    // U10: no room for the whole operation
    {
        uint8_t payload[8] = { 0 };
        uint32_t lead = 0;
        short_twin_stream( stream ); stream.SerializeBits( lead, 1 );
        (void) unified_bytes( stream, payload, 8 ); unified_fires = assert_fires;
        short_twin_stream( stream ); stream.SerializeBits( lead, 1 );
        split_bytes( stream, payload, 8 );
        check_twins( "bytes: no room for the align and payload (U10)", unified_fires, assert_fires );
    }
    {
        char text[16];
        memset( text, 0, sizeof( text ) );
        memcpy( text, "abcdefgh", 8 );
        short_twin_stream( stream ); (void) unified_string( stream, text, 16 ); unified_fires = assert_fires;
        short_twin_stream( stream ); split_string( stream, text, 16 );
        check_twins( "string: no room for the length, align and payload (U10)", unified_fires, assert_fires );
    }

    // U16: a bad bit count
    fresh_twin_stream( stream ); (void) unified_bits( stream, 0, 0 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_bits( stream, 0, 0 );
    check_twins( "bits: a bit count of 0 (U16)", unified_fires, assert_fires );

    // a non-finite float
    float float_infinity = 1.0f;
    for ( int i = 0; i < 200; i++ )
        float_infinity *= 10.0f;
    fresh_twin_stream( stream ); (void) unified_float( stream, float_infinity ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_float( stream, float_infinity );
    check_twins( "float: an infinity", unified_fires, assert_fires );

    // fixed point out of its range: 11.0 in Q48.16 over [-10,10]
    fresh_twin_stream( stream ); (void) unified_fixed( stream, int64_t( 11 ) * 65536 ); unified_fires = assert_fires;
    fresh_twin_stream( stream ); split_fixed( stream, int64_t( 11 ) * 65536 );
    check_twins( "fixed: a value above max", unified_fires, assert_fires );

}

int main()
{
    printf( "\nwrite side assertions\n\n" );

    test_int64_asserts_before_narrowing();

    test_reject_asserts_on_write_and_measure();
    test_int_bounds_are_int32();
    test_write_checks_room_for_the_whole_operation();
    test_fail_on_write_and_measure_never_asserts();
    test_write_forms_catch_misuse_as_their_twins();

    test_value_in_int_relative_domain();
    test_write_int_relative_asserts_on_the_callers_value();
#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
    test_serialize_int_compile_time_asserts_on_the_callers_value();
#endif
    test_initialize_padded_refuses_a_short_destination();
    test_float_and_double_writes_assert_finite();
    test_compressed_float_declaration_top_must_decode_finite();

    if ( failures > 0 )
    {
        printf( "\n*** %d ASSERTION TESTS FAILED ***\n\n", failures );
        return 1;
    }

    printf( "\n*** ALL ASSERTION TESTS PASS ***\n\n" );

    return 0;
}

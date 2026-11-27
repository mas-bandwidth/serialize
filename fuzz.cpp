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
    libFuzzer harness for serialize.

    Every input runs four passes:

    1. Hostile read (FuzzRead). ReadStream is the trust boundary of this library: it must survive
       arbitrary hostile bytes, failing reads by returning false, never by corrupting memory or
       tripping an assert. Build with asserts enabled (Debug config) plus ASan/UBSan so all three
       failure modes are caught.

    2. The same hostile bytes through InitializePadded, from a source allocation with no slack.
       The source is heap allocated at exactly the payload size so ASan fences the byte after it.

    3. Hostile read with sentinel destinations (FuzzReadSentinel). The same hostile bytes, with every
       scalar read wrapped so its refusal comes back as a value: a refused read must leave its
       destination exactly as the caller left it (STANDARD.md), and failure is terminal, so every
       later read refuses, consumes no bits and writes nothing. Run with two different sentinels.

    4. Differential round trip (FuzzRoundTrip). Values generated from the input are written with
       WriteStream, read back with ReadStream, and compared. Any write/read asymmetry traps.
       MeasureStream runs the same ops and must never measure fewer bits than were written.

    The first bytes of the fuzz input are an op program selecting which serialize_* calls run and
    with what parameters. The remaining bytes are the hostile bitstream for pass 1 and the value
    pool for pass 2. This lets coverage-guided fuzzing explore interleavings of every primitive.
*/

#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>

// the pass running now, so a failed check names the pass that caught it (the replay controls match on it)
static const char * fuzz_pass = "none";

// the replay driver exits with a status so ctest can match the message; libFuzzer wants the trap
#ifdef SERIALIZE_FUZZ_REPLAY
#define fuzz_trap() exit( 86 )
#else
#define fuzz_trap() __builtin_trap()
#endif

#define fuzz_check( condition ) do { if ( !(condition) ) { fprintf( stderr, "fuzz_check failed: pass %s, line %d\n", fuzz_pass, __LINE__ ); fuzz_trap(); } } while (0)

// NEGATIVE CONTROLS. Built with SERIALIZE_FUZZ_CONTROL=n, exactly one check of this harness is
// sabotaged, and the replay driver below must then trap in the pass that owns that check (CMake
// registers each as a test that passes only when the output names that pass). A harness that has
// never been seen to fail is a claim, not a measurement. 0 is the real harness.
//   1 sentinel   a refused read is made to write its destination
//   2 hostile    a range check is tightened below the range the reader accepts
//   3 padded     InitializePadded's success is expected to be false
//   4 roundtrip  a round trip value is expected to differ from what was written
//   5 canary     a byte past the tight write buffer is overwritten
//   6 trailing   the trailing data refusal is expected to be an accept
#ifndef SERIALIZE_FUZZ_CONTROL
#define SERIALIZE_FUZZ_CONTROL 0
#endif

// hands out bytes from the fuzz input, wrapping around when exhausted. both round trip passes
// regenerate the identical value sequence, so nothing needs to be stored between passes.

struct ValuePool
{
    const uint8_t * data;
    size_t size;
    size_t index;

    ValuePool( const uint8_t * data_in, size_t size_in ) : data( data_in ), size( size_in ), index( 0 ) {}

    uint8_t NextByte()
    {
        if ( size == 0 )
            return 0;
        if ( index >= size )
            index = 0;
        return data[index++];
    }

    uint32_t NextUint32()
    {
        uint32_t value = 0;
        for ( int i = 0; i < 4; i++ )
            value = ( value << 8 ) | NextByte();
        return value;
    }

    uint64_t NextUint64()
    {
        return ( uint64_t( NextUint32() ) << 32 ) | NextUint32();
    }
};

// the compressed float declarations the harness drives: bands with a negative low bound, zero
// based bands, resolutions from coarse to fine, and one whose resolution is coarser than the whole
// band ({ 0, 1, 2 }: fewer than one step, which the params clamp up to one)
struct FuzzCompressedFloatDeclaration
{
    float min;
    float max;
    float res;
};

static const FuzzCompressedFloatDeclaration fuzz_compressed_float_declarations[] =
{
    { -10.0f,  +10.0f,  0.01f },
    { -1.0f,   +1.0f,   0.001f },
    { 0.0f,    +100.0f, 1.0f },
    { -100.0f, +100.0f, 0.5f },
    { 0.0f,    +10.0f,  3.0f },
    { 0.0f,    +1.0f,   0.0001f },
    { 0.0f,    +1.0f,   2.0f },
};

static const int fuzz_num_compressed_float_declarations =
    (int) ( sizeof( fuzz_compressed_float_declarations ) / sizeof( fuzz_compressed_float_declarations[0] ) );

// the bytes and string directional macros do not cross type check (the write form takes a const
// buffer, the read form a mutable one), so the direction selects a specialization instead
template <bool IsWriting> struct FuzzDirectionalBytes;

template <> struct FuzzDirectionalBytes<true>
{
    template <typename Stream> static bool Run( Stream & stream, ValuePool & pool )
    {
        uint8_t expected_bytes[32];
        for ( int j = 0; j < 16; j++ )
        {
            expected_bytes[j] = pool.NextByte();
        }
        write_bytes( stream, expected_bytes, 16 );

        char expected_string[32];
        const int string_length = pool.NextByte() % ( sizeof( expected_string ) - 1 );
        for ( int j = 0; j < string_length; j++ )
        {
            const uint8_t c = pool.NextByte() & 0x7F;
            expected_string[j] = ( c != 0 ) ? (char) c : ' ';
        }
        expected_string[string_length] = '\0';
        write_string( stream, expected_string, (int) sizeof( expected_string ) );
        return true;
    }
};

template <> struct FuzzDirectionalBytes<false>
{
    template <typename Stream> static bool Run( Stream & stream, ValuePool & pool )
    {
        uint8_t expected_bytes[32];
        for ( int j = 0; j < 16; j++ )
        {
            expected_bytes[j] = pool.NextByte();
        }
        uint8_t bytes_value[32];
        read_bytes( stream, bytes_value, 16 );
        fuzz_check( memcmp( bytes_value, expected_bytes, 16 ) == 0 );

        char expected_string[32];
        const int string_length = pool.NextByte() % ( sizeof( expected_string ) - 1 );
        for ( int j = 0; j < string_length; j++ )
        {
            const uint8_t c = pool.NextByte() & 0x7F;
            expected_string[j] = ( c != 0 ) ? (char) c : ' ';
        }
        expected_string[string_length] = '\0';
        char string_value[32];
        read_string( stream, string_value, (int) sizeof( string_value ) );
        fuzz_check( strcmp( string_value, expected_string ) == 0 );
        return true;
    }
};

// the directional read_* macros, in their intended direction, with the same in-contract checks the
// unified forms get in pass 1
template <typename Stream> bool FuzzDirectionalRead( Stream & stream )
{
    int32_t int_value = 0;
    read_int( stream, int_value, -100, +100 );
    fuzz_check( int_value >= -100 && int_value <= +100 );

    int64_t int64_value = 0;
    read_int64( stream, int64_value, -1000000, +1000000 );
    fuzz_check( int64_value >= -1000000 && int64_value <= +1000000 );

    uint32_t bits_value = 0;
    read_bits( stream, bits_value, 17 );
    fuzz_check( bits_value < ( 1U << 17 ) );

    uint64_t bits64_value = 0;
    read_bits( stream, bits64_value, 40 );
    fuzz_check( bits64_value < ( uint64_t( 1 ) << 40 ) );

    uint8_t uint8_value = 0;
    read_uint8( stream, uint8_value );
    (void) uint8_value;

    uint16_t uint16_value = 0;
    read_uint16( stream, uint16_value );
    (void) uint16_value;

    uint32_t uint32_value = 0;
    read_uint32( stream, uint32_value );
    (void) uint32_value;

    uint64_t uint64_value = 0;
    read_uint64( stream, uint64_value );
    (void) uint64_value;

    int32_t fixed_value = 0;
    read_fixed( stream, fixed_value, 16, 16, -1000, +1000 );
    fuzz_check( fixed_value >= -1000 * 65536 && fixed_value <= 1000 * 65536 );

    bool bool_value = false;
    read_bool( stream, bool_value );
    (void) bool_value;

    float float_value = 0.0f;
    read_float( stream, float_value );
    uint32_t float_bits = 0;
    memcpy( &float_bits, &float_value, 4 );
    fuzz_check( serialize::float_bits_are_finite( float_bits ) );

    double double_value = 0.0;
    read_double( stream, double_value );
    uint64_t double_bits = 0;
    memcpy( &double_bits, &double_value, 8 );
    fuzz_check( serialize::double_bits_are_finite( double_bits ) );

    const serialize::int128_t bound128 = serialize::int128_t( 1 ) << 40;
    serialize::int128_t int128_value = 0;
    read_int128( stream, int128_value, -bound128, +bound128 );
    fuzz_check( int128_value >= -bound128 && int128_value <= +bound128 );

    serialize::uint128_t uint128_value = 0;
    read_uint128( stream, uint128_value );

    uint8_t bytes_value[32];
    read_bytes( stream, bytes_value, 16 );

    char string_value[32];
    read_string( stream, string_value, (int) sizeof( string_value ) );
    fuzz_check( strlen( string_value ) < sizeof( string_value ) );

    int previous = 0;
    int current = 0;
    read_int_relative( stream, previous, current );
    fuzz_check( current > previous );

    read_align( stream );

    return true;
}

// the directional read_* and write_* macros, selected by the stream direction. Stream::IsWriting is
// a compile time constant, so the unused half folds away but both halves are type checked.
template <typename Stream> bool FuzzDirectionalRoundTrip( Stream & stream, ValuePool & pool )
{
    const int32_t expected_int = -100 + int32_t( pool.NextUint32() % 201 );
    int32_t int_value = Stream::IsWriting ? expected_int : 0;
    if ( Stream::IsWriting )
    {
        write_int( stream, int_value, -100, +100 );
    }
    else
    {
        read_int( stream, int_value, -100, +100 );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( int_value == expected_int );
    }

    const int64_t expected_int64 = -1000000 + int64_t( pool.NextUint32() % 2000001 );
    int64_t int64_value = Stream::IsWriting ? expected_int64 : 0;
    if ( Stream::IsWriting )
    {
        write_int64( stream, int64_value, -1000000, +1000000 );
    }
    else
    {
        read_int64( stream, int64_value, -1000000, +1000000 );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( int64_value == expected_int64 );
    }

    const uint32_t expected_bits = pool.NextUint32() & 0x1FFFF;
    uint32_t bits_value = Stream::IsWriting ? expected_bits : 0;
    if ( Stream::IsWriting )
    {
        write_bits( stream, bits_value, 17 );
    }
    else
    {
        read_bits( stream, bits_value, 17 );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( bits_value == expected_bits );
    }

    const uint64_t expected_bits64 = pool.NextUint64() & 0xFFFFFFFFFFULL;
    uint64_t bits64_value = Stream::IsWriting ? expected_bits64 : 0;
    if ( Stream::IsWriting )
    {
        write_bits( stream, bits64_value, 40 );
    }
    else
    {
        read_bits( stream, bits64_value, 40 );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( bits64_value == expected_bits64 );
    }

    const uint8_t expected_uint8 = pool.NextByte();
    uint8_t uint8_value = Stream::IsWriting ? expected_uint8 : (uint8_t) 0;
    if ( Stream::IsWriting )
    {
        write_uint8( stream, uint8_value );
    }
    else
    {
        read_uint8( stream, uint8_value );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( uint8_value == expected_uint8 );
    }

    const uint16_t expected_uint16 = (uint16_t) pool.NextUint32();
    uint16_t uint16_value = Stream::IsWriting ? expected_uint16 : (uint16_t) 0;
    if ( Stream::IsWriting )
    {
        write_uint16( stream, uint16_value );
    }
    else
    {
        read_uint16( stream, uint16_value );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( uint16_value == expected_uint16 );
    }

    const uint32_t expected_uint32 = pool.NextUint32();
    uint32_t uint32_value = Stream::IsWriting ? expected_uint32 : 0;
    if ( Stream::IsWriting )
    {
        write_uint32( stream, uint32_value );
    }
    else
    {
        read_uint32( stream, uint32_value );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( uint32_value == expected_uint32 );
    }

    const uint64_t expected_uint64 = pool.NextUint64();
    uint64_t uint64_value = Stream::IsWriting ? expected_uint64 : 0;
    if ( Stream::IsWriting )
    {
        write_uint64( stream, uint64_value );
    }
    else
    {
        read_uint64( stream, uint64_value );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( uint64_value == expected_uint64 );
    }

    const uint32_t fixed_range = 2000U * 65536;
    const int32_t expected_fixed = int32_t( uint32_t( -1000 * 65536 ) + pool.NextUint32() % ( fixed_range + 1 ) );
    int32_t fixed_value = Stream::IsWriting ? expected_fixed : 0;
    if ( Stream::IsWriting )
    {
        write_fixed( stream, fixed_value, 16, 16, -1000, +1000 );
    }
    else
    {
        read_fixed( stream, fixed_value, 16, 16, -1000, +1000 );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( fixed_value == expected_fixed );
    }

    const bool expected_bool = ( pool.NextByte() & 1 ) != 0;
    bool bool_value = Stream::IsWriting ? expected_bool : false;
    if ( Stream::IsWriting )
    {
        write_bool( stream, bool_value );
    }
    else
    {
        read_bool( stream, bool_value );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( bool_value == expected_bool );
    }

    const uint32_t float_raw = pool.NextUint32();
    const uint32_t float_expected_bits = serialize::float_bits_are_finite( float_raw ) ? float_raw : ( float_raw & 0x807FFFFF );
    float float_value = 0.0f;
    if ( Stream::IsWriting )
    {
        memcpy( &float_value, &float_expected_bits, 4 );
        write_float( stream, float_value );
    }
    else
    {
        read_float( stream, float_value );
        uint32_t float_bits = 0;
        memcpy( &float_bits, &float_value, 4 );
        fuzz_check( float_bits == float_expected_bits );
    }

    const uint64_t double_raw = pool.NextUint64();
    const uint64_t double_expected_bits = serialize::double_bits_are_finite( double_raw ) ? double_raw : ( double_raw & 0x800FFFFFFFFFFFFFULL );
    double double_value = 0.0;
    if ( Stream::IsWriting )
    {
        memcpy( &double_value, &double_expected_bits, 8 );
        write_double( stream, double_value );
    }
    else
    {
        read_double( stream, double_value );
        uint64_t double_bits = 0;
        memcpy( &double_bits, &double_value, 8 );
        fuzz_check( double_bits == double_expected_bits );
    }

    const serialize::int128_t bound128 = serialize::int128_t( 1 ) << 40;
    const serialize::int128_t expected_int128 = serialize::int128_t( int64_t( pool.NextUint64() & 0x1FFFFFFFFFFULL ) ) - bound128;
    serialize::int128_t int128_value = Stream::IsWriting ? expected_int128 : serialize::int128_t( 0 );
    if ( Stream::IsWriting )
    {
        write_int128( stream, int128_value, -bound128, +bound128 );
    }
    else
    {
        read_int128( stream, int128_value, -bound128, +bound128 );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( int128_value == expected_int128 );
    }

    const serialize::uint128_t expected_uint128 = ( serialize::uint128_t( pool.NextUint64() ) << 64 ) | pool.NextUint64();
    serialize::uint128_t uint128_value = Stream::IsWriting ? expected_uint128 : serialize::uint128_t( 0 );
    if ( Stream::IsWriting )
    {
        write_uint128( stream, uint128_value );
    }
    else
    {
        read_uint128( stream, uint128_value );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( uint128_value == expected_uint128 );
    }

    if ( !FuzzDirectionalBytes<( Stream::IsWriting != 0 )>::Run( stream, pool ) )
    {
        return false;
    }

    const int previous = 0;
    const int expected_current = 1 + int( pool.NextUint32() % 1000000 );
    int current = Stream::IsWriting ? expected_current : 0;
    if ( Stream::IsWriting )
    {
        write_int_relative( stream, previous, current );
    }
    else
    {
        read_int_relative( stream, previous, current );
    }
    if ( Stream::IsReading )
    {
        fuzz_check( current == expected_current );
    }

    if ( Stream::IsWriting )
    {
        write_align( stream );
    }
    else
    {
        read_align( stream );
    }

    return true;
}

// pass 1: hostile read. arbitrary bytes go in, reads must fail cleanly or produce in-contract values.

template <typename Stream> bool FuzzRead( Stream & stream, const uint8_t * ops, int numOps )
{
    uint8_t bytes[256];
    char string[256];

    for ( int i = 0; i < numOps; i++ )
    {
        const int select = ops[i] & 15;
        const int param = ops[i] >> 4;              // [0,15]

        switch ( select )
        {
            case 0:
            {
                uint32_t value = 0;
                const int bits = param + 1;         // [1,16]
                serialize_bits( stream, value, bits );
                fuzz_check( value < ( 1U << bits ) );
            }
            break;

            case 1:
            {
                uint32_t value = 0;
                const int bits = param + 17;        // [17,32]
                serialize_bits( stream, value, bits );
                fuzz_check( bits == 32 || value < ( 1U << bits ) );
            }
            break;

            case 2:
            {
                int32_t value = 0;
                serialize_int( stream, value, -100, +100 );
#if SERIALIZE_FUZZ_CONTROL == 2
                fuzz_check( value >= -100 && value <= +50 );                  // NEGATIVE CONTROL
#else
                fuzz_check( value >= -100 && value <= +100 );
#endif
            }
            break;

            case 3:
            {
                int32_t value = 0;
                serialize_int( stream, value, INT32_MIN, INT32_MAX );
                int64_t value64 = 0;
                serialize_int64( stream, value64, INT64_MIN, INT64_MAX );
                const int64_t bound = int64_t( param + 1 ) << 35;               // spans of varying width exercise bits_required64
                int64_t ranged64 = 0;
                serialize_int64( stream, ranged64, -bound, +bound );
                fuzz_check( ranged64 >= -bound && ranged64 <= +bound );

                // fixed point: bounds are compile time, so the configurations are fixed. values read
                // off hostile bytes must decode within the raw bounds or fail the read.
                int32_t fixed32 = 0;
                serialize_fixed( stream, fixed32, 16, 16, -1000, +1000 );
                fuzz_check( fixed32 >= -1000 * 65536 && fixed32 <= 1000 * 65536 );

                int64_t fixed64 = 0;
                serialize_fixed( stream, fixed64, 48, 16, -100000000000LL, +100000000000LL );
                fuzz_check( fixed64 >= -100000000000LL * 65536 && fixed64 <= 100000000000LL * 65536 );

                serialize::int128_t wide = 0;                                   // exists on every platform: native or emulated
                serialize_fixed( stream, wide, 112, 16, -1152921504606846976LL, +1152921504606846976LL );        // ±2^60 units: a raw range past 64 bits
                fuzz_check( wide >= serialize::int128_t( -1152921504606846976LL ) * serialize::int128_t( 65536 ) );
                fuzz_check( wide <= serialize::int128_t( +1152921504606846976LL ) * serialize::int128_t( 65536 ) );

                // ranged 128 bit integers, with the bound width varying by param so the run walks
                // every group structure: one group, two, three and four. hostile bytes must decode
                // inside the bounds or fail the read — never clamp, never wrap.
                const serialize::int128_t bound128 = serialize::int128_t( 1 ) << ( 20 + ( param % 100 ) );
                serialize::int128_t ranged128 = 0;
                serialize_int128( stream, ranged128, -bound128, +bound128 );
                fuzz_check( ranged128 >= -bound128 && ranged128 <= +bound128 );

                serialize::int128_t full128 = 0;
                serialize_int128( stream, full128, serialize::int128_t( serialize::uint128_t( 1 ) << 127 ), serialize::int128_t( ~( serialize::uint128_t( 1 ) << 127 ) ) );

#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
                int32_t compile_time_int = 0;
                switch ( param & 3 )
                {
                    case 0: serialize_int_compile_time( stream, compile_time_int, -100, +100 ); break;
                    case 1: serialize_int_compile_time( stream, compile_time_int, 0, 65535 ); break;
                    case 2: serialize_int_compile_time( stream, compile_time_int, INT32_MIN, INT32_MAX ); break;
                    default: serialize_int_compile_time( stream, compile_time_int, -2000000000, +2000000000 ); break;
                }

                int64_t compile_time_int64 = 0;
                serialize_int64_compile_time( stream, compile_time_int64, -5000000000LL, +5000000000LL );
                fuzz_check( compile_time_int64 >= -5000000000LL && compile_time_int64 <= +5000000000LL );

                uint32_t compile_time_bits = 0;
                switch ( param & 3 )
                {
                    case 0: serialize_bits_compile_time( stream, compile_time_bits, 5 ); break;
                    case 1: serialize_bits_compile_time( stream, compile_time_bits, 17 ); break;
                    case 2: serialize_bits_compile_time( stream, compile_time_bits, 32 ); break;
                    default: serialize_bits_compile_time( stream, compile_time_bits, 1 ); break;
                }

                uint64_t compile_time_bits64 = 0;
                serialize_bits64_compile_time( stream, compile_time_bits64, 40 );

                bool compile_time_bool = false;
                serialize_bool_compile_time( stream, compile_time_bool );
#endif
            }
            break;

            case 4:
            {
                bool value = false;
                serialize_bool( stream, value );
            }
            break;

            case 5:
            {
                // finite values only: whatever the bytes, an accepted float is never NaN or infinite
                float value = 0.0f;
                serialize_float( stream, value );
                uint32_t value_bits = 0;
                memcpy( &value_bits, &value, 4 );
                fuzz_check( serialize::float_bits_are_finite( value_bits ) );
            }
            break;

            case 6:
            {
                double value = 0.0;
                serialize_double( stream, value );
                uint64_t value_bits = 0;
                memcpy( &value_bits, &value, 8 );
                fuzz_check( serialize::double_bits_are_finite( value_bits ) );
            }
            break;

            case 7:
            {
                const int declaration_index = param % fuzz_num_compressed_float_declarations;
                const FuzzCompressedFloatDeclaration & declaration = fuzz_compressed_float_declarations[declaration_index];

                uint32_t max_integer_value = 0;
                int bits = 0;
                float delta = 0.0f;
                serialize::serialize_compressed_float_params( declaration.min, declaration.max, declaration.res, max_integer_value, bits, delta );

                float value = 0.0f;
                if ( ( param & 1 ) != 0 )
                {
                    serialize_compressed_float_precomputed( stream, value, max_integer_value, bits, delta, declaration.min );
                }
                else
                {
                    serialize_compressed_float( stream, value, declaration.min, declaration.max, declaration.res );
                }

                uint32_t value_bits = 0;
                memcpy( &value_bits, &value, 4 );
                fuzz_check( serialize::float_bits_are_finite( value_bits ) );
                fuzz_check( value >= declaration.min );
                fuzz_check( value <= declaration.min + delta );
            }
            break;

            case 8:
            {
                uint64_t value = 0;
                serialize_uint64( stream, value );
                serialize::uint128_t value128 = 0;                      // exists on every platform: native or emulated
                serialize_uint128( stream, value128 );
            }
            break;

            case 9:
            {
                const int numBytes = param * 16 + 1;                    // [1,241]
                serialize_bytes( stream, bytes, numBytes );
            }
            break;

            case 10:
            {
                serialize_string( stream, string, (int) sizeof( string ) );
                fuzz_check( strlen( string ) < sizeof( string ) );
            }
            break;

            case 11:
            {
                // the end of the object: an accepted end leaves nothing in the stream
                serialize_end( stream );
                fuzz_check( stream.GetBitsProcessed() % 8 == 0 );
                uint32_t after = 0;
                fuzz_check( stream.SerializeBits( after, 1 ) == false );
                return false;
            }

            case 12:
            {
                serialize_align( stream );
            }
            break;

            case 13:
            {
                int previous = param * 1000;                    // int_relative's domain starts at 0: a negative previous is caller error
                int current = 0;
                serialize_int_relative( stream, previous, current );
            }
            break;

            case 14:
            {
                const int32_t max = ( param + 1 ) * 1000;               // ranges of varying width exercise bits_required
                int32_t value = 0;
                serialize_int( stream, value, 0, max );
                fuzz_check( value >= 0 && value <= max );
            }
            break;

            case 15:
            {
                uint8_t value8 = 0;
                serialize_uint8( stream, value8 );
                uint16_t value16 = 0;
                serialize_uint16( stream, value16 );
                uint32_t value32 = 0;
                serialize_uint32( stream, value32 );

                if ( !FuzzDirectionalRead( stream ) )
                {
                    return false;
                }
            }
            break;
        }
    }

    return true;
}

// pass 4: differential round trip. run once with WriteStream (and MeasureStream), then again with
// ReadStream over the bytes just written. every case must consume the same pool bytes when writing
// and reading, so the read pass regenerates the exact values the write pass produced.

template <typename Stream> bool FuzzRoundTrip( Stream & stream, const uint8_t * ops, int numOps, ValuePool & pool )
{
    for ( int i = 0; i < numOps; i++ )
    {
        const int select = ops[i] & 15;
        const int param = ops[i] >> 4;              // [0,15]

        switch ( select )
        {
            case 0:
            case 1:
            {
                const int bits = ( select == 0 ) ? param + 1 : param + 17;                      // [1,32]
                const uint32_t mask = ( bits == 32 ) ? 0xFFFFFFFF : ( ( 1U << bits ) - 1 );
                const uint32_t expected = pool.NextUint32() & mask;
                uint32_t value = Stream::IsWriting ? expected : 0;
                serialize_bits( stream, value, bits );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 2:
            {
                const int32_t expected = -100 + int32_t( pool.NextUint32() % 201 );
                int32_t value = Stream::IsWriting ? expected : 0;
                serialize_int( stream, value, -100, +100 );
                if ( Stream::IsReading )
                {
#if SERIALIZE_FUZZ_CONTROL == 4
                    fuzz_check( value == expected + 1 );                      // NEGATIVE CONTROL
#else
                    fuzz_check( value == expected );
#endif
                }
            }
            break;

            case 3:
            {
                const int32_t expected = (int32_t) pool.NextUint32();
                int32_t value = Stream::IsWriting ? expected : 0;
                serialize_int( stream, value, INT32_MIN, INT32_MAX );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }

                const int64_t expected64 = (int64_t) pool.NextUint64();
                int64_t value64 = Stream::IsWriting ? expected64 : 0;
                serialize_int64( stream, value64, INT64_MIN, INT64_MAX );
                if ( Stream::IsReading )
                {
                    fuzz_check( value64 == expected64 );
                }

                const int64_t bound = int64_t( param + 1 ) << 35;               // spans of varying width exercise bits_required64
                const uint64_t span = uint64_t( bound ) * 2 + 1;
                const int64_t expected_ranged = -bound + int64_t( pool.NextUint64() % span );
                int64_t ranged64 = Stream::IsWriting ? expected_ranged : 0;
                serialize_int64( stream, ranged64, -bound, +bound );
                if ( Stream::IsReading )
                {
                    fuzz_check( ranged64 == expected_ranged );
                }

                // fixed point: raw values generated uniformly inside the raw bounds must round trip exactly
                const uint32_t fixed32_range = 2000U * 65536;                   // bounds [-1000,+1000] whole units in Q16.16
                const int32_t expected_fixed32 = int32_t( uint32_t( -1000 * 65536 ) + pool.NextUint32() % ( fixed32_range + 1 ) );
                int32_t fixed32 = Stream::IsWriting ? expected_fixed32 : 0;
                serialize_fixed( stream, fixed32, 16, 16, -1000, +1000 );
                if ( Stream::IsReading )
                {
                    fuzz_check( fixed32 == expected_fixed32 );
                }

                const uint64_t fixed64_range = uint64_t( 200000000000ULL ) * 65536;     // bounds [-1e11,+1e11] whole units in Q48.16
                const int64_t expected_fixed64 = int64_t( uint64_t( -100000000000LL * 65536 ) + pool.NextUint64() % ( fixed64_range + 1 ) );
                int64_t fixed64 = Stream::IsWriting ? expected_fixed64 : 0;
                serialize_fixed( stream, fixed64, 48, 16, -100000000000LL, +100000000000LL );
                if ( Stream::IsReading )
                {
                    fuzz_check( fixed64 == expected_fixed64 );
                }

                // wide fixed point: bounds ±2^60 whole units in Q112.16 give a raw range of 2^77,
                // exercising the multi group path with values that cannot fit in 64 bits. the wide
                // types exist on every platform: native __int128 or the emulated pair.
                const serialize::uint128_t wide_range = serialize::uint128_t( 1152921504606846976ULL ) << 17;    // ( 2 * 2^60 ) << 16
                const uint64_t wide_pool_high = pool.NextUint64();
                const uint64_t wide_pool_low = pool.NextUint64();
                const serialize::uint128_t wide_pool = ( serialize::uint128_t( wide_pool_high ) << 64 ) | wide_pool_low;
                const serialize::uint128_t wide_raw_min = serialize::uint128_t( serialize::int128_t( -1152921504606846976LL ) ) << 16;
                const serialize::int128_t expected_wide = serialize::int128_t( wide_raw_min + wide_pool % ( wide_range + serialize::uint128_t( 1 ) ) );
                serialize::int128_t wide = Stream::IsWriting ? expected_wide : serialize::int128_t( 0 );
                serialize_fixed( stream, wide, 112, 16, -1152921504606846976LL, +1152921504606846976LL );
                if ( Stream::IsReading )
                {
                    fuzz_check( wide == expected_wide );
                }

#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
                {
                    int32_t expected_compile_time_int = 0;
                    int32_t compile_time_int = 0;
                    switch ( param & 3 )
                    {
                        case 0:
                        {
                            expected_compile_time_int = -100 + int32_t( pool.NextUint32() % 201 );
                            compile_time_int = Stream::IsWriting ? expected_compile_time_int : 0;
                            serialize_int_compile_time( stream, compile_time_int, -100, +100 );
                        }
                        break;
                        case 1:
                        {
                            expected_compile_time_int = int32_t( pool.NextUint32() % 65536 );
                            compile_time_int = Stream::IsWriting ? expected_compile_time_int : 0;
                            serialize_int_compile_time( stream, compile_time_int, 0, 65535 );
                        }
                        break;
                        case 2:
                        {
                            expected_compile_time_int = int32_t( pool.NextUint32() );
                            compile_time_int = Stream::IsWriting ? expected_compile_time_int : 0;
                            serialize_int_compile_time( stream, compile_time_int, INT32_MIN, INT32_MAX );
                        }
                        break;
                        default:
                        {
                            expected_compile_time_int = int32_t( -2000000000LL + int64_t( pool.NextUint32() % 4000000001U ) );
                            compile_time_int = Stream::IsWriting ? expected_compile_time_int : 0;
                            serialize_int_compile_time( stream, compile_time_int, -2000000000, +2000000000 );
                        }
                        break;
                    }
                    if ( Stream::IsReading )
                    {
                        fuzz_check( compile_time_int == expected_compile_time_int );
                    }

                    const int64_t expected_compile_time_int64 = -5000000000LL + int64_t( pool.NextUint64() % 10000000001ULL );
                    int64_t compile_time_int64 = Stream::IsWriting ? expected_compile_time_int64 : 0;
                    serialize_int64_compile_time( stream, compile_time_int64, -5000000000LL, +5000000000LL );
                    if ( Stream::IsReading )
                    {
                        fuzz_check( compile_time_int64 == expected_compile_time_int64 );
                    }

                    uint32_t expected_compile_time_bits = 0;
                    uint32_t compile_time_bits = 0;
                    switch ( param & 3 )
                    {
                        case 0:
                            expected_compile_time_bits = pool.NextUint32() & 0x1F;
                            compile_time_bits = Stream::IsWriting ? expected_compile_time_bits : 0;
                            serialize_bits_compile_time( stream, compile_time_bits, 5 );
                            break;
                        case 1:
                            expected_compile_time_bits = pool.NextUint32() & 0x1FFFF;
                            compile_time_bits = Stream::IsWriting ? expected_compile_time_bits : 0;
                            serialize_bits_compile_time( stream, compile_time_bits, 17 );
                            break;
                        case 2:
                            expected_compile_time_bits = pool.NextUint32();
                            compile_time_bits = Stream::IsWriting ? expected_compile_time_bits : 0;
                            serialize_bits_compile_time( stream, compile_time_bits, 32 );
                            break;
                        default:
                            expected_compile_time_bits = pool.NextUint32() & 0x1;
                            compile_time_bits = Stream::IsWriting ? expected_compile_time_bits : 0;
                            serialize_bits_compile_time( stream, compile_time_bits, 1 );
                            break;
                    }
                    if ( Stream::IsReading )
                    {
                        fuzz_check( compile_time_bits == expected_compile_time_bits );
                    }

                    const uint64_t expected_compile_time_bits64 = pool.NextUint64() & 0xFFFFFFFFFFULL;
                    uint64_t compile_time_bits64 = Stream::IsWriting ? expected_compile_time_bits64 : 0;
                    serialize_bits64_compile_time( stream, compile_time_bits64, 40 );
                    if ( Stream::IsReading )
                    {
                        fuzz_check( compile_time_bits64 == expected_compile_time_bits64 );
                    }

                    const bool expected_compile_time_bool = ( pool.NextByte() & 1 ) != 0;
                    bool compile_time_bool = Stream::IsWriting ? expected_compile_time_bool : false;
                    serialize_bool_compile_time( stream, compile_time_bool );
                    if ( Stream::IsReading )
                    {
                        fuzz_check( compile_time_bool == expected_compile_time_bool );
                    }
                }
#endif
            }
            break;

            case 4:
            {
                const bool expected = ( pool.NextByte() & 1 ) != 0;
                bool value = Stream::IsWriting ? expected : false;
                serialize_bool( stream, value );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 5:
            {
                // arbitrary FINITE bit patterns, -0 and subnormals included: floats round trip bit
                // exact. NaN and infinity are non-conforming on write and assert in this harness, so
                // a non-finite pattern has its exponent cleared, which keeps the sign and mantissa
                // entropy as a subnormal. Hostile non-finite bytes reach the read path in pass 1
                const uint32_t raw_bits = pool.NextUint32();
                const uint32_t expected_bits = serialize::float_bits_are_finite( raw_bits ) ? raw_bits : ( raw_bits & 0x807FFFFF );
                float value = 0.0f;
                if ( Stream::IsWriting )
                {
                    memcpy( &value, &expected_bits, 4 );
                }
                serialize_float( stream, value );
                if ( Stream::IsReading )
                {
                    uint32_t value_bits = 0;
                    memcpy( &value_bits, &value, 4 );
                    fuzz_check( value_bits == expected_bits );
                }
            }
            break;

            case 6:
            {
                const uint64_t raw_bits = pool.NextUint64();
                const uint64_t expected_bits = serialize::double_bits_are_finite( raw_bits ) ? raw_bits : ( raw_bits & 0x800FFFFFFFFFFFFFULL );
                double value = 0.0;
                if ( Stream::IsWriting )
                {
                    memcpy( &value, &expected_bits, 8 );
                }
                serialize_double( stream, value );
                if ( Stream::IsReading )
                {
                    uint64_t value_bits = 0;
                    memcpy( &value_bits, &value, 8 );
                    fuzz_check( value_bits == expected_bits );
                }
            }
            break;

            case 7:
            {
                // arbitrary FINITE bit patterns: out of range values must clamp into [min,max] on
                // write, and finite in range values must round trip within the resolution. non-finite
                // values are non-conforming on write (STANDARD.md, adopted 2026-08-15) and assert out
                // in debug, so the writer is fed only finite input: clearing the exponent of a
                // non-finite pattern yields a subnormal, keeping the sign and mantissa entropy.
                // hostile non-finite coverage belongs to the read path, which stage one exercises.
                const int declaration_index = param % fuzz_num_compressed_float_declarations;
                const FuzzCompressedFloatDeclaration & declaration = fuzz_compressed_float_declarations[declaration_index];

                uint32_t max_integer_value = 0;
                int bits = 0;
                float delta = 0.0f;
                serialize::serialize_compressed_float_params( declaration.min, declaration.max, declaration.res, max_integer_value, bits, delta );

                const uint32_t raw_bits = pool.NextUint32();
                const bool raw_finite = ( raw_bits & 0x7FFFFFFF ) < 0x7F800000;
                const uint32_t expected_bits = raw_finite ? raw_bits : ( raw_bits & 0x807FFFFF );
                float expected = 0.0f;
                memcpy( &expected, &expected_bits, 4 );
                float value = Stream::IsWriting ? expected : 0.0f;
                if ( ( param & 1 ) != 0 )
                {
                    serialize_compressed_float_precomputed( stream, value, max_integer_value, bits, delta, declaration.min );
                }
                else
                {
                    serialize_compressed_float( stream, value, declaration.min, declaration.max, declaration.res );
                }
                if ( Stream::IsReading )
                {
                    uint32_t value_bits = 0;
                    memcpy( &value_bits, &value, 4 );
                    fuzz_check( serialize::float_bits_are_finite( value_bits ) );
                    fuzz_check( value >= declaration.min );
                    fuzz_check( value <= declaration.min + delta );
                    const bool finite = ( expected_bits & 0x7FFFFFFF ) < 0x7F800000;            // bit test: immune to fast math
                    if ( finite && expected >= declaration.min && expected <= declaration.max )
                    {
                        const float tolerance = declaration.res + ( declaration.max - declaration.min ) * 1e-6f;
                        fuzz_check( fabs( value - expected ) <= tolerance );
                    }
                }
            }
            break;

            case 8:
            {
                const uint64_t expected = pool.NextUint64();
                uint64_t value = Stream::IsWriting ? expected : 0;
                serialize_uint64( stream, value );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }

                const uint64_t expected128_high = pool.NextUint64();
                const uint64_t expected128_low = pool.NextUint64();
                const serialize::uint128_t expected128 = ( serialize::uint128_t( expected128_high ) << 64 ) | expected128_low;
                serialize::uint128_t value128 = Stream::IsWriting ? expected128 : serialize::uint128_t( 0 );
                serialize_uint128( stream, value128 );
                if ( Stream::IsReading )
                {
                    fuzz_check( value128 == expected128 );
                }
            }
            break;

            case 9:
            {
                const int numBytes = param * 16 + 1;                    // [1,241]
                uint8_t expected[256];
                for ( int j = 0; j < numBytes; j++ )
                    expected[j] = pool.NextByte();
                uint8_t value[256];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, (size_t) numBytes );
                }
                serialize_bytes( stream, value, numBytes );
                if ( Stream::IsReading )
                {
                    fuzz_check( memcmp( value, expected, (size_t) numBytes ) == 0 );
                }
            }
            break;

            case 10:
            {
                char expected[32];
                const int length = pool.NextByte() % ( sizeof( expected ) - 1 );
                for ( int j = 0; j < length; j++ )
                {
                    // masked to ASCII: the string payload is well-formed UTF-8 by the
                    // writer's contract (debug-asserted, and asserts are live in this
                    // harness), and the reader REFUSES malformed payloads (STANDARD.md,
                    // adopted 2026-08-15), so the differential round trip must generate
                    // conforming content. Arbitrary bytes still reach the READ path via
                    // FuzzRead.
                    const uint8_t c = pool.NextByte() & 0x7F;
                    expected[j] = ( c != 0 ) ? (char) c : ' ';
                }
                expected[length] = '\0';
                char value[32];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, (size_t) length + 1 );
                }
                serialize_string( stream, value, (int) sizeof( value ) );
                if ( Stream::IsReading )
                {
                    fuzz_check( strcmp( value, expected ) == 0 );
                }
            }
            break;

            case 11:
            {
                // a string of real text: random Unicode scalar values, encoded as UTF-8 here, so
                // every length of sequence and every plane reaches the writer and must come back
                // byte for byte. NUL and the surrogate block are not scalar text and are skipped
                char expected[40];                                      // seven four byte characters at most, and the terminator
                int length = 0;
                const int characters = pool.NextByte() % 8;
                for ( int j = 0; j < characters; j++ )
                {
                    uint32_t code = pool.NextUint32() % 0x110000;
                    if ( code == 0 || ( code >= 0xD800 && code <= 0xDFFF ) )
                        code = 0x7881;                                  // 碁
                    if ( code < 0x80 )
                    {
                        expected[length++] = (char) code;
                    }
                    else if ( code < 0x800 )
                    {
                        expected[length++] = (char) ( 0xC0 | ( code >> 6 ) );
                        expected[length++] = (char) ( 0x80 | ( code & 0x3F ) );
                    }
                    else if ( code < 0x10000 )
                    {
                        expected[length++] = (char) ( 0xE0 | ( code >> 12 ) );
                        expected[length++] = (char) ( 0x80 | ( ( code >> 6 ) & 0x3F ) );
                        expected[length++] = (char) ( 0x80 | ( code & 0x3F ) );
                    }
                    else
                    {
                        expected[length++] = (char) ( 0xF0 | ( code >> 18 ) );
                        expected[length++] = (char) ( 0x80 | ( ( code >> 12 ) & 0x3F ) );
                        expected[length++] = (char) ( 0x80 | ( ( code >> 6 ) & 0x3F ) );
                        expected[length++] = (char) ( 0x80 | ( code & 0x3F ) );
                    }
                }
                expected[length] = '\0';
                char value[40];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, (size_t) length + 1 );
                }
                serialize_string( stream, value, (int) sizeof( value ) );
                if ( Stream::IsReading )
                {
                    fuzz_check( strcmp( value, expected ) == 0 );
                }
            }
            break;

            case 12:
            {
                serialize_align( stream );
            }
            break;

            case 13:
            {
                const int previous = param * 1000;              // int_relative's domain starts at 0: a negative previous is caller error
                const int expected = previous + 1 + int( pool.NextUint32() % 1000000 );         // strictly greater than previous
                int value = Stream::IsWriting ? expected : 0;
                serialize_int_relative( stream, previous, value );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 14:
            {
                const int32_t max = ( param + 1 ) * 1000;               // ranges of varying width exercise bits_required
                const int32_t expected = int32_t( pool.NextUint32() % uint32_t( max + 1 ) );
                int32_t value = Stream::IsWriting ? expected : 0;
                serialize_int( stream, value, 0, max );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 15:
            {
                const uint8_t expected8 = pool.NextByte();
                const uint16_t expected16 = (uint16_t) pool.NextUint32();
                const uint32_t expected32 = pool.NextUint32();
                uint8_t value8 = Stream::IsWriting ? expected8 : (uint8_t) 0;
                uint16_t value16 = Stream::IsWriting ? expected16 : (uint16_t) 0;
                uint32_t value32 = Stream::IsWriting ? expected32 : 0;
                serialize_uint8( stream, value8 );
                serialize_uint16( stream, value16 );
                serialize_uint32( stream, value32 );
                if ( Stream::IsReading )
                {
                    fuzz_check( value8 == expected8 );
                    fuzz_check( value16 == expected16 );
                    fuzz_check( value32 == expected32 );
                }

                if ( !FuzzDirectionalRoundTrip( stream, pool ) )
                {
                    return false;
                }
            }
            break;
        }
    }

    return true;
}

// pass 3: hostile read with sentinel destinations. STANDARD.md, "A refused read must leave its
// destination unwritten": when a read of a scalar fails, the caller's value is exactly what it was
// before the call, and failure is terminal: every later read refuses, consumes no bits and writes
// nothing. Pass 1 stops at the first refusal, because the serialize_* macros return false, and it
// cannot see a destination. This pass wraps every scalar read in a function of its own, so the
// refusal comes back as a value and the destination can be compared against a sentinel; then it
// keeps reading, to prove that terminality holds for every operation. bytes and string leave their
// buffer unspecified after a refusal (the same rule), so they are driven for the terminality check
// only. Run twice per input with different sentinels, so a write that happens to equal one
// sentinel is caught by the other.

template <typename Stream> bool SentinelBits32( Stream & stream, uint32_t & value, int bits ) { serialize_bits( stream, value, bits ); return true; }
template <typename Stream> bool SentinelInt( Stream & stream, int32_t & value, int32_t min, int32_t max ) { serialize_int( stream, value, min, max ); return true; }
template <typename Stream> bool SentinelInt32Full( Stream & stream, int32_t & value ) { serialize_int( stream, value, INT32_MIN, INT32_MAX ); return true; }
template <typename Stream> bool SentinelInt64( Stream & stream, int64_t & value, int64_t min, int64_t max ) { serialize_int64( stream, value, min, max ); return true; }
template <typename Stream> bool SentinelFixed32( Stream & stream, int32_t & value ) { serialize_fixed( stream, value, 16, 16, -1000, +1000 ); return true; }
template <typename Stream> bool SentinelFixed64( Stream & stream, int64_t & value ) { serialize_fixed( stream, value, 48, 16, -100000000000LL, +100000000000LL ); return true; }
template <typename Stream> bool SentinelFixed128( Stream & stream, serialize::int128_t & value ) { serialize_fixed( stream, value, 112, 16, -1152921504606846976LL, +1152921504606846976LL ); return true; }
template <typename Stream> bool SentinelInt128( Stream & stream, serialize::int128_t & value, serialize::int128_t min, serialize::int128_t max ) { serialize_int128( stream, value, min, max ); return true; }
template <typename Stream> bool SentinelBool( Stream & stream, bool & value ) { serialize_bool( stream, value ); return true; }
template <typename Stream> bool SentinelFloat( Stream & stream, float & value ) { serialize_float( stream, value ); return true; }
template <typename Stream> bool SentinelDouble( Stream & stream, double & value ) { serialize_double( stream, value ); return true; }
template <typename Stream> bool SentinelCompressedFloat( Stream & stream, float & value, int declaration_index ) { const FuzzCompressedFloatDeclaration & declaration = fuzz_compressed_float_declarations[declaration_index]; serialize_compressed_float( stream, value, declaration.min, declaration.max, declaration.res ); return true; }
template <typename Stream> bool SentinelCompressedFloatPrecomputed( Stream & stream, float & value, int declaration_index ) { const FuzzCompressedFloatDeclaration & declaration = fuzz_compressed_float_declarations[declaration_index]; uint32_t max_integer_value = 0; int bits = 0; float delta = 0.0f; serialize::serialize_compressed_float_params( declaration.min, declaration.max, declaration.res, max_integer_value, bits, delta ); serialize_compressed_float_precomputed( stream, value, max_integer_value, bits, delta, declaration.min ); return true; }
template <typename Stream> bool SentinelUint64( Stream & stream, uint64_t & value ) { serialize_uint64( stream, value ); return true; }
template <typename Stream> bool SentinelUint128( Stream & stream, serialize::uint128_t & value ) { serialize_uint128( stream, value ); return true; }
template <typename Stream> bool SentinelBits32Narrow( Stream & stream, uint8_t & value ) { serialize_uint8( stream, value ); return true; }
template <typename Stream> bool SentinelBits16Narrow( Stream & stream, uint16_t & value ) { serialize_uint16( stream, value ); return true; }
template <typename Stream> bool SentinelBytes( Stream & stream, uint8_t * data, int numBytes ) { serialize_bytes( stream, data, numBytes ); return true; }
template <typename Stream> bool SentinelString( Stream & stream, char * string, int size ) { serialize_string( stream, string, size ); return true; }
template <typename Stream> bool SentinelEnd( Stream & stream ) { serialize_end( stream ); return true; }
template <typename Stream> bool SentinelAlign( Stream & stream ) { serialize_align( stream ); return true; }
template <typename Stream> bool SentinelIntRelative( Stream & stream, int previous, int & current ) { serialize_int_relative( stream, previous, current ); return true; }
template <typename Stream> bool SentinelReadIntDirectional( Stream & stream, int32_t & value ) { read_int( stream, value, -100, +100 ); return true; }
template <typename Stream> bool SentinelReadBitsDirectional( Stream & stream, uint32_t & value ) { read_bits( stream, value, 17 ); return true; }
template <typename Stream> bool SentinelReadUint8Directional( Stream & stream, uint8_t & value ) { read_uint8( stream, value ); return true; }
template <typename Stream> bool SentinelReadFloatDirectional( Stream & stream, float & value ) { read_float( stream, value ); return true; }
template <typename Stream> bool SentinelReadIntRelativeDirectional( Stream & stream, int previous, int & current ) { read_int_relative( stream, previous, current ); return true; }
#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
template <typename Stream> bool SentinelIntCompileTime( Stream & stream, int32_t & value ) { serialize_int_compile_time( stream, value, -100, +100 ); return true; }
template <typename Stream> bool SentinelInt64CompileTime( Stream & stream, int64_t & value ) { serialize_int64_compile_time( stream, value, -5000000000LL, +5000000000LL ); return true; }
template <typename Stream> bool SentinelBitsCompileTime( Stream & stream, uint32_t & value ) { serialize_bits_compile_time( stream, value, 17 ); return true; }
template <typename Stream> bool SentinelBits64CompileTime( Stream & stream, uint64_t & value ) { serialize_bits64_compile_time( stream, value, 40 ); return true; }
template <typename Stream> bool SentinelBoolCompileTime( Stream & stream, bool & value ) { serialize_bool_compile_time( stream, value ); return true; }
#endif

struct SentinelRun
{
    serialize::ReadStream * stream;
    serialize::Refusal refusal;         // the first reason, once the stream has refused
    int64_t bits;                       // bits processed at the first refusal
    int pattern;                        // the sentinel byte pattern
    bool flag;                          // the sentinel for a bool

    // the checks every read gets, whatever it reads. ok is what the read returned
    void Check( bool ok, bool unchanged )
    {
        if ( refusal != serialize::REFUSAL_NONE )
        {
            fuzz_check( !ok );                                              // terminal: refused, whatever the operation
            fuzz_check( stream->GetRefusal() == refusal );                  // the first reason wins
            fuzz_check( stream->GetBitsProcessed() == bits );               // and consumed nothing
            fuzz_check( unchanged );
            return;
        }
        if ( ok )
        {
            fuzz_check( stream->GetRefusal() == serialize::REFUSAL_NONE );
            return;
        }
        fuzz_check( stream->GetRefusal() != serialize::REFUSAL_NONE );     // a refused read stream never reports none
        fuzz_check( unchanged );                                            // and the destination is exactly what it was
        refusal = stream->GetRefusal();
        bits = stream->GetBitsProcessed();
    }

    template <typename T> void Arm( T & value )
    {
        memset( &value, pattern, sizeof( T ) );
    }

    template <typename T> bool Untouched( const T & value ) const
    {
        const uint8_t * bytes = (const uint8_t *) &value;
        for ( size_t i = 0; i < sizeof( T ); i++ )
        {
            if ( bytes[i] != (uint8_t) pattern )
                return false;
        }
        return true;
    }

    // a refused read must not have written. The control makes one do exactly that
    template <typename T> void Sabotage( bool ok, T & value )
    {
#if SERIALIZE_FUZZ_CONTROL == 1
        if ( !ok )
            memset( &value, pattern ^ 0xFF, sizeof( T ) );                  // NEGATIVE CONTROL
#else
        (void) ok; (void) value;
#endif
    }
};

#define sentinel_read( run, type, call )                                    \
    do                                                                      \
    {                                                                       \
        type destination;                                                   \
        run.Arm( destination );                                             \
        const bool ok = call;                                               \
        run.Sabotage( ok, destination );                                    \
        run.Check( ok, run.Untouched( destination ) );                      \
    } while (0)

static void FuzzReadSentinel( serialize::ReadStream & stream, const uint8_t * ops, int numOps, int pattern )
{
    SentinelRun run;
    run.stream = &stream;
    run.refusal = serialize::REFUSAL_NONE;
    run.bits = 0;
    run.pattern = pattern;
    run.flag = ( pattern & 1 ) != 0;

    uint8_t bytes[256];
    char string[256];

    for ( int i = 0; i < numOps; i++ )
    {
        const int select = ops[i] & 15;
        const int param = ops[i] >> 4;

        switch ( select )
        {
            case 0:
            {
                sentinel_read( run, uint32_t, SentinelBits32( stream, destination, param + 1 ) );
            }
            break;

            case 1:
            {
                sentinel_read( run, uint32_t, SentinelBits32( stream, destination, param + 17 ) );
            }
            break;

            case 2:
            {
                sentinel_read( run, int32_t, SentinelInt( stream, destination, -100, +100 ) );
            }
            break;

            case 3:
            {
                sentinel_read( run, int32_t, SentinelInt32Full( stream, destination ) );
                const int64_t bound = int64_t( param + 1 ) << 35;
                sentinel_read( run, int64_t, SentinelInt64( stream, destination, INT64_MIN, INT64_MAX ) );
                sentinel_read( run, int64_t, SentinelInt64( stream, destination, -bound, +bound ) );
                sentinel_read( run, int32_t, SentinelFixed32( stream, destination ) );
                sentinel_read( run, int64_t, SentinelFixed64( stream, destination ) );
                sentinel_read( run, serialize::int128_t, SentinelFixed128( stream, destination ) );
                const serialize::int128_t bound128 = serialize::int128_t( 1 ) << ( 20 + ( param % 100 ) );
                sentinel_read( run, serialize::int128_t, SentinelInt128( stream, destination, -bound128, +bound128 ) );
                sentinel_read( run, serialize::int128_t, SentinelInt128( stream, destination, serialize::int128_t( serialize::uint128_t( 1 ) << 127 ), serialize::int128_t( ~( serialize::uint128_t( 1 ) << 127 ) ) ) );
#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
                sentinel_read( run, int32_t, SentinelIntCompileTime( stream, destination ) );
                sentinel_read( run, int64_t, SentinelInt64CompileTime( stream, destination ) );
                sentinel_read( run, uint32_t, SentinelBitsCompileTime( stream, destination ) );
                sentinel_read( run, uint64_t, SentinelBits64CompileTime( stream, destination ) );
                sentinel_read( run, bool, SentinelBoolCompileTime( stream, destination ) );
#endif
            }
            break;

            case 4:
            {
                bool destination = run.flag;
                const bool ok = SentinelBool( stream, destination );
#if SERIALIZE_FUZZ_CONTROL == 1
                if ( !ok )
                    destination = !run.flag;                                // NEGATIVE CONTROL
#endif
                run.Check( ok, destination == run.flag );
            }
            break;

            case 5:
            {
                sentinel_read( run, float, SentinelFloat( stream, destination ) );
            }
            break;

            case 6:
            {
                sentinel_read( run, double, SentinelDouble( stream, destination ) );
            }
            break;

            case 7:
            {
                const int declaration_index = param % fuzz_num_compressed_float_declarations;
                sentinel_read( run, float, SentinelCompressedFloat( stream, destination, declaration_index ) );
                sentinel_read( run, float, SentinelCompressedFloatPrecomputed( stream, destination, declaration_index ) );
            }
            break;

            case 8:
            {
                sentinel_read( run, uint64_t, SentinelUint64( stream, destination ) );
                sentinel_read( run, serialize::uint128_t, SentinelUint128( stream, destination ) );
            }
            break;

            case 9:
            {
                const bool ok = SentinelBytes( stream, bytes, param * 16 + 1 );
                run.Check( ok, true );                                      // the buffer is unspecified after a refusal
            }
            break;

            case 10:
            {
                const bool ok = SentinelString( stream, string, (int) sizeof( string ) );
                run.Check( ok, true );
            }
            break;

            case 11:
            {
                const bool ok = SentinelEnd( stream );
                run.Check( ok, true );
            }
            break;

            case 12:
            {
                const bool ok = SentinelAlign( stream );
                run.Check( ok, true );
            }
            break;

            case 13:
            {
                sentinel_read( run, int, SentinelIntRelative( stream, param * 1000, destination ) );
            }
            break;

            case 14:
            {
                sentinel_read( run, int32_t, SentinelInt( stream, destination, 0, ( param + 1 ) * 1000 ) );
            }
            break;

            case 15:
            {
                sentinel_read( run, uint8_t, SentinelBits32Narrow( stream, destination ) );
                sentinel_read( run, uint16_t, SentinelBits16Narrow( stream, destination ) );
                sentinel_read( run, uint32_t, SentinelBits32( stream, destination, 32 ) );
                sentinel_read( run, int32_t, SentinelReadIntDirectional( stream, destination ) );
                sentinel_read( run, uint32_t, SentinelReadBitsDirectional( stream, destination ) );
                sentinel_read( run, uint8_t, SentinelReadUint8Directional( stream, destination ) );
                sentinel_read( run, float, SentinelReadFloatDirectional( stream, destination ) );
                sentinel_read( run, int, SentinelReadIntRelativeDirectional( stream, param * 1000, destination ) );
            }
            break;
        }
    }
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    const int NumOps = 32;

    if ( size <= NumOps || size > NumOps + 4096 )
    {
        return 0;
    }

    const uint8_t * ops = data;
    const uint8_t * payload = data + NumOps;
    const size_t payloadBytes = size - NumOps;

    // pass 1: hostile read of arbitrary bytes
    fuzz_pass = "hostile";
    {
        // the buffer allocation must extend at least 8 bytes past the data, because the bit reader loads 64 bit windows.
        // The slack is all ones, not zeros: bytes past the end are loaded but must never be interpreted, and a
        // reader that leaked them into a value or a decision would pass on zeros (end's padding check included)
        std::vector<uint8_t> buffer( payloadBytes + 8, 0xFF );
        memcpy( buffer.data(), payload, payloadBytes );

        serialize::ReadStream stream( buffer.data(), (int) payloadBytes );

        FuzzRead( stream, ops, NumOps );
    }

    // pass 2: the same hostile bytes, through InitializePadded, from a source
    // allocation with no slack. The destination is not zeroed first, so a byte
    // the reader can touch that the wrapper failed to write shows up as a
    // finding rather than a silent zero.
    {
        uint8_t * exact = (uint8_t *) malloc( payloadBytes );
        uint8_t * padded = (uint8_t *) malloc( payloadBytes + 8 );
        if ( exact == NULL || padded == NULL )
        {
            free( exact );
            free( padded );
            return 0;
        }
        memcpy( exact, payload, payloadBytes );
        serialize::ReadStream stream;
        fuzz_pass = "padded";
#if SERIALIZE_FUZZ_CONTROL == 3
        fuzz_check( stream.InitializePadded( padded, (int64_t) ( payloadBytes + 8 ), exact, (int64_t) payloadBytes ) == false );      // NEGATIVE CONTROL
#else
        fuzz_check( stream.InitializePadded( padded, (int64_t) ( payloadBytes + 8 ), exact, (int64_t) payloadBytes ) == true );
#endif
        FuzzRead( stream, ops, NumOps );
        free( exact );
        free( padded );
    }

    // pass 3: hostile read with sentinel destinations, twice with different sentinels
    fuzz_pass = "sentinel";
    for ( int pattern = 0x5A; pattern != 0; pattern = ( pattern == 0x5A ) ? 0xA5 : 0 )
    {
        std::vector<uint8_t> buffer( payloadBytes + 8, 0xFF );
        memcpy( buffer.data(), payload, payloadBytes );
        serialize::ReadStream stream( buffer.data(), (int) payloadBytes );
        FuzzReadSentinel( stream, ops, NumOps, pattern );
    }

    // pass 4: differential round trip of values generated from the same bytes
    fuzz_pass = "roundtrip";
    {
        // worst case is ~260 bytes per op (a 241 byte serialize_bytes plus alignment), so 32 ops fit comfortably
        const int WriteBufferSize = 16 * 1024;
        alignas( 4 ) uint8_t writeBuffer[WriteBufferSize];
        memset( writeBuffer, 0, sizeof( writeBuffer ) );

        serialize::WriteStream writeStream( writeBuffer, WriteBufferSize );
        ValuePool writePool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( writeStream, ops, NumOps, writePool ) == true );             // writing in-range values must always succeed
        writeStream.Flush();

        // the same writes into a buffer that is exactly the smallest multiple of 8 holding them, followed by
        // a canary. The qword flush must stay inside the buffer and produce the same bytes. ASan cannot see a
        // partial-granule overrun past a tight allocation (CLAUDE.md), so the canary is the check
        {
            const int64_t written = writeStream.GetBytesProcessed();
            int64_t tightBytes = ( ( written + 7 ) / 8 ) * 8;
            if ( tightBytes == 0 )
            {
                tightBytes = 8;
            }
            const int CanaryBytes = 16;
            std::vector<uint8_t> tight( (size_t) ( tightBytes + CanaryBytes ), 0xA5 );
            memset( tight.data() + tightBytes, 0xAA, CanaryBytes );
            serialize::WriteStream tightStream( tight.data(), tightBytes );
            ValuePool tightPool( payload, payloadBytes );
            fuzz_check( FuzzRoundTrip( tightStream, ops, NumOps, tightPool ) == true );
            tightStream.Flush();
            fuzz_check( tightStream.GetBytesProcessed() == written );
            fuzz_check( written == 0 || memcmp( tight.data(), writeBuffer, (size_t) written ) == 0 );
#if SERIALIZE_FUZZ_CONTROL == 5
            tight[(size_t) tightBytes] = 0;                                 // NEGATIVE CONTROL
#endif
            fuzz_pass = "canary";
            for ( int i = 0; i < CanaryBytes; i++ )
            {
                fuzz_check( tight[(size_t) ( tightBytes + i )] == 0xAA );
            }
        }

        fuzz_pass = "roundtrip";
        serialize::MeasureStream measureStream;
        ValuePool measurePool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( measureStream, ops, NumOps, measurePool ) == true );
        fuzz_check( measureStream.GetBitsProcessed() >= writeStream.GetBitsProcessed() );       // measure must be conservative

        // the 16KB writeBuffer extends well past the written bytes, satisfying the reader's 8-bytes-past allocation contract
        serialize::ReadStream readStream( writeBuffer, writeStream.GetBytesProcessed() );
        ValuePool readPool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( readStream, ops, NumOps, readPool ) == true );               // reading back our own data must always succeed
        fuzz_check( readStream.SerializeEnd() == true );                                        // and it ends exactly where the writer stopped

        // one byte more than the writer reported is trailing data, refused at the end
        serialize::ReadStream longerStream( writeBuffer, writeStream.GetBytesProcessed() + 1 );
        ValuePool longerPool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( longerStream, ops, NumOps, longerPool ) == true );
        fuzz_pass = "trailing";
#if SERIALIZE_FUZZ_CONTROL == 6
        fuzz_check( longerStream.SerializeEnd() == true );                                      // NEGATIVE CONTROL
#else
        fuzz_check( longerStream.SerializeEnd() == false );
#endif
        fuzz_check( longerStream.GetRefusal() == serialize::REFUSAL_TRAILING_DATA );
    }

    return 0;
}

#ifdef SERIALIZE_FUZZ_REPLAY

// The replay driver: the same target, run over a fixed set of generated inputs with no libFuzzer,
// so ctest can run it anywhere clang or gcc builds and so the negative controls above have a
// deterministic input to fail on. The generator is a fixed xorshift (no clock, no files). The first
// inputs are the edge shapes (all zero, all ones), the rest are random bytes of varying length, and
// every input is also run with each op byte cycled through the sixteen operations so that every
// operation is reached whatever the generator drew.

static uint64_t replay_state = 0x9E3779B97F4A7C15ULL;

static uint32_t ReplayNext()
{
    replay_state ^= replay_state << 13;
    replay_state ^= replay_state >> 7;
    replay_state ^= replay_state << 17;
    return (uint32_t) ( replay_state >> 16 );
}

int main( int argc, char ** argv )
{
    int count = 3000;
    if ( argc > 1 )
        count = atoi( argv[1] );

    const int NumOps = 32;
    std::vector<uint8_t> input;

    for ( int n = 0; n < count; n++ )
    {
        const int payload = ( n % 7 == 0 ) ? 1 + int( ReplayNext() % 8 ) : 1 + int( ReplayNext() % 600 );
        input.resize( (size_t) ( NumOps + payload ) );
        for ( size_t i = 0; i < input.size(); i++ )
        {
            input[i] = (uint8_t) ReplayNext();
        }
        if ( n % 11 == 0 )
        {
            for ( int i = 0; i < payload; i++ )
                input[(size_t) ( NumOps + i )] = ( n % 2 ) ? 0xFF : 0x00;
        }
        // cycle the op selectors so every operation appears, the parameters stay random
        for ( int i = 0; i < NumOps; i++ )
        {
            if ( n % 3 == 0 )
                input[(size_t) i] = (uint8_t) ( ( input[(size_t) i] & 0xF0 ) | ( ( i + n ) & 15 ) );
        }
        LLVMFuzzerTestOneInput( input.data(), input.size() );
    }

    printf( "replayed %d inputs\n", count );
    return 0;
}

#endif // #ifdef SERIALIZE_FUZZ_REPLAY

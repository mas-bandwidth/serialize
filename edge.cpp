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
    The buffer edges, differentially. Adopted from the 2026-10-27 reference audit
    (audit/writer_reader_edge.cpp).

    The qword flush writer and the branchless 64 bit window reader at the very end of tight
    buffers. The writer gets a buffer sized to the smallest multiple of 8 that holds the data
    (sometimes 8 more), pre-filled with garbage and followed by a canary, and its bytes are
    compared with a bit at a time reference writer: same bytes, nothing past the data but zero
    or the untouched garbage, the canary intact. ASan cannot see a partial-granule overrun past
    a tight allocation, so the canary is the check. The reader gets an allocation of exactly
    bytes + 8, the slack filled with 0x00 in one pass and 0xFF in the other: every decoded value
    and every accept or refuse decision must match, because the slack is loaded and never
    interpreted.

    A unit test: a fixed seed, no clock, no sockets.

    Declared sizes that do not fit the cursor are checked here too. The counts
    are declared on a small local array and are never allocated: a negative
    count, the largest count the cursor accepts, and one above it.
*/

// SERIALIZE_ENABLE_TESTS makes the emulated 128 bit pair available beside native
// __int128, so the wide declaration cases below cover both carriers in one build.
#define SERIALIZE_ENABLE_TESTS 1
#include "serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static uint64_t rng_state;

static uint64_t rnd()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

struct RefWriter
{
    std::vector<uint8_t> bytes;
    int64_t bits;

    RefWriter() : bits( 0 ) {}

    void Bit( int b )
    {
        if ( ( bits >> 3 ) >= (int64_t) bytes.size() )
            bytes.push_back( 0 );
        if ( b )
            bytes[(size_t) ( bits >> 3 )] |= uint8_t( 1 << ( bits & 7 ) );
        bits++;
    }

    void Bits( uint32_t v, int n ) { for ( int i = 0; i < n; i++ ) Bit( ( v >> i ) & 1 ); }
    void Align() { while ( bits & 7 ) Bit( 0 ); }
    void Bytes( const uint8_t * d, int n ) { Align(); for ( int i = 0; i < n; i++ ) Bits( d[i], 8 ); }
};

enum { OP_BITS, OP_ALIGN, OP_BYTES, OP_INT64 };

struct Op
{
    int kind;
    uint32_t value;
    int bits;
    uint8_t data[64];
    int length;
    int64_t v64;
};


static bool cursor_refused( serialize::ReadStream & stream )
{
    uint32_t dest = 0xA5A5A5A5u;
    uint8_t bytes[4];
    memset( bytes, 0xAB, sizeof( bytes ) );
    if ( stream.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        return false;
    // empty, then poisoned: the cursor is one, not a wrapped product
    if ( stream.GetBitsProcessed() != int64_t( 1 ) )
        return false;
    if ( stream.SerializeBits( dest, 8 ) )
        return false;
    if ( dest != 0xA5A5A5A5u )
        return false;
    if ( stream.SerializeBytes( bytes, 4 ) )
        return false;
    for ( int i = 0; i < 4; i++ )
    {
        if ( bytes[i] != 0xAB )
            return false;
    }
    if ( stream.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        return false;
    return true;
}

template <typename Stream, typename Value, typename Min, typename Max>
static bool run_serialize_int64_wide( Stream & stream, Value & value, Min min, Max max )
{
    serialize_int64( stream, value, min, max );
    return true;
}

template <typename Stream, typename Value, typename Min, typename Max>
static bool run_read_int64_wide( Stream & stream, Value & value, Min min, Max max )
{
    read_int64( stream, value, min, max );
    return true;
}

template <typename Stream, typename Value, typename Min, typename Max>
static bool run_serialize_int_wide( Stream & stream, Value & value, Min min, Max max )
{
    serialize_int( stream, value, min, max );
    return true;
}

template <typename Stream, typename Value, typename Min, typename Max>
static bool run_read_int_wide( Stream & stream, Value & value, Min min, Max max )
{
    read_int( stream, value, min, max );
    return true;
}

template <typename Stream, typename Value>
static bool run_serialize_int_relative_wide( Stream & stream, Value previous, Value & current )
{
    serialize_int_relative( stream, previous, current );
    return true;
}

template <typename Stream, typename Value>
static bool run_read_int_relative_wide( Stream & stream, Value previous, Value & current )
{
    read_int_relative( stream, previous, current );
    return true;
}

// A 128 bit declaration judged in a narrower width: bounds of 2^64 + 5 to
// 2^64 + 10 narrow to [5, 10] as int64 and to [5, 10] as int32, and a relative
// previous of 2^64 + 5 narrows to 5. Every form must refuse the declaration on
// read as value_out_of_range before any bits move, leaving the sentinel
// destination unchanged, for native and emulated unsigned 128 bit carriers,
// through the unified and the split macros. A wide bound that does fit the
// declared width (0 to 8 here) is compared in its own width and accepted.
static int wide_declaration_edges()
{
    uint8_t zero_packet[24];
    memset( zero_packet, 0, sizeof( zero_packet ) );
    uint8_t one_bit_packet[24];
    memset( one_bit_packet, 0, sizeof( one_bit_packet ) );
    one_bit_packet[0] = 0x01;   // the one bit tier: current = previous + 1

    const int64_t sentinel64 = int64_t( -1122334455667788LL );
    const int32_t sentinel32 = int32_t( -1122334455 );

#if defined( __SIZEOF_INT128__ )
    {
        const serialize::uint128_t wide_min = ( serialize::uint128_t( 1 ) << 64 ) + serialize::uint128_t( 5 );
        const serialize::uint128_t wide_max = ( serialize::uint128_t( 1 ) << 64 ) + serialize::uint128_t( 10 );

        serialize::ReadStream s1( zero_packet, 0 );
        int64_t v64 = sentinel64;
        if ( run_serialize_int64_wide( s1, v64, wide_min, wide_max ) || v64 != sentinel64 ||
             s1.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  native serialize_int64 accepted 128 bit bounds past int64\n" );
            return 1;
        }
        serialize::ReadStream s2( zero_packet, 0 );
        v64 = sentinel64;
        if ( run_read_int64_wide( s2, v64, wide_min, wide_max ) || v64 != sentinel64 ||
             s2.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  native read_int64 accepted 128 bit bounds past int64\n" );
            return 1;
        }
        serialize::ReadStream s3( zero_packet, 0 );
        int32_t v32 = sentinel32;
        if ( run_serialize_int_wide( s3, v32, wide_min, wide_max ) || v32 != sentinel32 ||
             s3.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  native serialize_int accepted 128 bit bounds past int32\n" );
            return 1;
        }
        serialize::ReadStream s4( zero_packet, 0 );
        v32 = sentinel32;
        if ( run_read_int_wide( s4, v32, wide_min, wide_max ) || v32 != sentinel32 ||
             s4.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  native read_int accepted 128 bit bounds past int32\n" );
            return 1;
        }
        serialize::ReadStream s5( zero_packet, 0 );
        serialize::uint128_t current = serialize::uint128_t( 777 );
        if ( run_serialize_int_relative_wide( s5, wide_min, current ) || current != serialize::uint128_t( 777 ) ||
             s5.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  native serialize_int_relative accepted a 128 bit previous past its domain\n" );
            return 1;
        }
        serialize::ReadStream s6( one_bit_packet, 8 );
        current = serialize::uint128_t( 777 );
        if ( run_read_int_relative_wide( s6, wide_min, current ) || current != serialize::uint128_t( 777 ) ||
             s6.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  native read_int_relative accepted a 128 bit previous past its domain\n" );
            return 1;
        }
        // a wide bound that fits the declared width is accepted in its own width
        serialize::ReadStream s7( zero_packet, 8 );
        v64 = sentinel64;
        if ( !run_serialize_int64_wide( s7, v64, serialize::uint128_t( 0 ), serialize::uint128_t( 8 ) ) || v64 != 0 ||
             s7.GetRefusal() != serialize::REFUSAL_NONE )
        {
            printf( "  native serialize_int64 refused 128 bit bounds that fit int64\n" );
            return 1;
        }
    }
#endif // #if defined( __SIZEOF_INT128__ )

// The emulated-carrier cases below cannot compile against a header without
// the wide-declaration fix (the narrower macros cast the emulated carrier to
// int64_t), so a base check of this file uses the native-only form:
// compile with SERIALIZE_WIDE_NATIVE_ONLY defined to keep only the native
// cases above, which compile at base and fail there with
// "native serialize_int64 accepted 128 bit bounds past int64".
#if defined( SERIALIZE_UINT128_DEFINED ) && !defined( SERIALIZE_WIDE_NATIVE_ONLY )
    {
        serialize_uint128_t wide_min( 0 );
        wide_min.lo = 5;
        wide_min.hi = 1;
        serialize_uint128_t wide_max( 0 );
        wide_max.lo = 10;
        wide_max.hi = 1;

        serialize::ReadStream s1( zero_packet, 0 );
        int64_t v64 = sentinel64;
        if ( run_serialize_int64_wide( s1, v64, wide_min, wide_max ) || v64 != sentinel64 ||
             s1.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  emulated serialize_int64 accepted 128 bit bounds past int64\n" );
            return 1;
        }
        serialize::ReadStream s2( zero_packet, 0 );
        v64 = sentinel64;
        if ( run_read_int64_wide( s2, v64, wide_min, wide_max ) || v64 != sentinel64 ||
             s2.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  emulated read_int64 accepted 128 bit bounds past int64\n" );
            return 1;
        }
        serialize::ReadStream s3( zero_packet, 0 );
        int32_t v32 = sentinel32;
        if ( run_serialize_int_wide( s3, v32, wide_min, wide_max ) || v32 != sentinel32 ||
             s3.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  emulated serialize_int accepted 128 bit bounds past int32\n" );
            return 1;
        }
        serialize::ReadStream s4( zero_packet, 0 );
        v32 = sentinel32;
        if ( run_read_int_wide( s4, v32, wide_min, wide_max ) || v32 != sentinel32 ||
             s4.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  emulated read_int accepted 128 bit bounds past int32\n" );
            return 1;
        }
        // the relative previous as the signed emulated carrier holding 2^64 + 5: the
        // unsigned emulated type converts only to uint64_t, so it cannot name a
        // relative previous through the macro and is judged at the helper instead
        if ( serialize::value_in_int_relative_domain( wide_min ) )
        {
            printf( "  emulated unsigned 128 bit previous judged inside the relative domain\n" );
            return 1;
        }
        serialize_int128_t wide_previous( wide_min );
        serialize::ReadStream s5( zero_packet, 0 );
        serialize_int128_t current( 777 );
        if ( run_serialize_int_relative_wide( s5, wide_previous, current ) || current != serialize_int128_t( 777 ) ||
             s5.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  emulated serialize_int_relative accepted a 128 bit previous past its domain\n" );
            return 1;
        }
        serialize::ReadStream s6( one_bit_packet, 8 );
        current = serialize_int128_t( 777 );
        if ( run_read_int_relative_wide( s6, wide_previous, current ) || current != serialize_int128_t( 777 ) ||
             s6.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
        {
            printf( "  emulated read_int_relative accepted a 128 bit previous past its domain\n" );
            return 1;
        }
        serialize::ReadStream s7( zero_packet, 8 );
        v64 = sentinel64;
        if ( !run_serialize_int64_wide( s7, v64, serialize_uint128_t( 0 ), serialize_uint128_t( 8 ) ) || v64 != 0 ||
             s7.GetRefusal() != serialize::REFUSAL_NONE )
        {
            printf( "  emulated serialize_int64 refused 128 bit bounds that fit int64\n" );
            return 1;
        }
    }
#endif // #if defined( SERIALIZE_UINT128_DEFINED ) && !defined( SERIALIZE_WIDE_NATIVE_ONLY )

    printf( "    wide declarations: 128 bit bounds and previous judged in their own width     pass\n" );
    return 0;
}

static int declared_size_edges()
{
    uint8_t local[16];
    memset( local, 0x5A, sizeof( local ) );

    // the largest multiple of 8 that is <= INT64_MAX - 8, divided by 8
    const int64_t min_count = int64_t( -9223372036854775807LL ) - 1;
    const int64_t max_count = ( int64_t( 9223372036854775807LL ) - 15 ) / 8;
    const int64_t bad[3] = { min_count, int64_t( -1 ), max_count + 1 };

    for ( int i = 0; i < 3; i++ )
    {
        const int64_t count = bad[i];
        serialize::BitReader raw( local, count );
        if ( raw.GetBitsRead() != int64_t( 0 ) || raw.GetBitsRemaining() != int64_t( 0 ) )
        {
            printf( "  bit reader kept count %lld\n", (long long) count );
            return 1;
        }
        raw.Initialize( local, count );
        if ( raw.GetBitsRead() != int64_t( 0 ) || raw.GetBitsRemaining() != int64_t( 0 ) )
        {
            printf( "  bit reader re-init kept count %lld\n", (long long) count );
            return 1;
        }

        serialize::ReadStream stream( local, count );
        if ( !cursor_refused( stream ) )
        {
            printf( "  stream count %lld refused as %d cursor %lld\n",
                    (long long) count, (int) stream.GetRefusal(), (long long) stream.GetBitsProcessed() );
            return 1;
        }
        stream.Initialize( local, int64_t( 8 ) );
        if ( stream.GetRefusal() != serialize::REFUSAL_NONE || stream.GetBitsRemaining() != int64_t( 64 ) )
        {
            printf( "  re-init to 8 bytes did not clear count %lld\n", (long long) count );
            return 1;
        }
        stream.Initialize( local, count );
        if ( !cursor_refused( stream ) )
        {
            printf( "  re-init of count %lld did not refuse\n", (long long) count );
            return 1;
        }
    }

    serialize::BitReader raw_max( local, max_count );
    if ( raw_max.GetBitsRead() != int64_t( 0 ) || raw_max.GetBitsRemaining() != max_count * 8 )
    {
        printf( "  largest count was not accepted by the bit reader\n" );
        return 1;
    }
    raw_max.Initialize( local, max_count );
    if ( raw_max.GetBitsRemaining() != max_count * 8 )
    {
        printf( "  largest count was not accepted on re-init\n" );
        return 1;
    }

    serialize::ReadStream ok( local, max_count );
    if ( ok.GetRefusal() != serialize::REFUSAL_NONE )
    {
        printf( "  largest count refused as %d\n", (int) ok.GetRefusal() );
        return 1;
    }
    if ( ok.GetBitsProcessed() != int64_t( 0 ) || ok.GetBitsRemaining() != max_count * 8 )
    {
        printf( "  largest count cursor %lld remaining %lld\n",
                (long long) ok.GetBitsProcessed(), (long long) ok.GetBitsRemaining() );
        return 1;
    }
    ok.Initialize( local, min_count );
    if ( !cursor_refused( ok ) )
    {
        printf( "  largest stream did not refuse a negative re-init\n" );
        return 1;
    }
    ok.Initialize( local, max_count );
    if ( ok.GetRefusal() != serialize::REFUSAL_NONE || ok.GetBitsRemaining() != max_count * 8 )
    {
        printf( "  largest count did not clear the refusal\n" );
        return 1;
    }

    // poison the largest accepted cursor and round it up to a byte. the +7 must fit
    serialize::ReadStream topped( local, max_count );
    uint32_t dest = 0xA5A5A5A5u;
    if ( topped.SerializeBits( dest, 0 ) )
    {
        printf( "  a zero bit count was accepted\n" );
        return 1;
    }
    if ( dest != 0xA5A5A5A5u )
    {
        printf( "  a refused declaration wrote the destination\n" );
        return 1;
    }
    if ( topped.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
    {
        printf( "  a zero bit count refused as %d\n", (int) topped.GetRefusal() );
        return 1;
    }
    const int64_t max_bits = max_count * 8;
    if ( topped.GetBitsProcessed() != max_bits + 1 )
    {
        printf( "  poisoned cursor %lld\n", (long long) topped.GetBitsProcessed() );
        return 1;
    }
    if ( topped.GetBytesProcessed() != ( max_bits + 1 + 7 ) / 8 )
    {
        printf( "  byte rounding %lld\n", (long long) topped.GetBytesProcessed() );
        return 1;
    }
    if ( topped.GetBitsRemaining() != int64_t( -1 ) )
    {
        printf( "  poisoned remaining %lld\n", (long long) topped.GetBitsRemaining() );
        return 1;
    }
    if ( topped.SerializeBits( dest, 8 ) || dest != 0xA5A5A5A5u )
    {
        printf( "  a later read wrote or was accepted\n" );
        return 1;
    }
    if ( topped.GetRefusal() != serialize::REFUSAL_VALUE_OUT_OF_RANGE )
    {
        printf( "  the first reason was relabelled to %d\n", (int) topped.GetRefusal() );
        return 1;
    }

    serialize::ReadStream zero( local, 0 );
    if ( zero.GetRefusal() != serialize::REFUSAL_NONE || zero.GetBitsRemaining() != int64_t( 0 ) )
    {
        printf( "  a zero count was refused\n" );
        return 1;
    }

    printf( "    declared sizes: negative, too large, and the largest that fits     pass\n" );
    return 0;
}

int main()
{
    if ( wide_declaration_edges() != 0 )
        return 1;

    if ( declared_size_edges() != 0 )
        return 1;

    printf( "\nbuffer edges\n\n" );

    const int64_t int64_min = int64_t( -9223372036854775807LL ) - 1;
    const int64_t int64_max = int64_t( 9223372036854775807LL );

    rng_state = 88172645463325252ULL;
    int failures = 0;
    const int Iterations = 20000;
    for ( int iteration = 0; iteration < Iterations; iteration++ )
    {
        const int numOps = 1 + int( rnd() % 12 );
        std::vector<Op> ops( (size_t) numOps );
        RefWriter ref;
        for ( size_t o = 0; o < ops.size(); o++ )
        {
            Op & op = ops[o];
            memset( &op, 0, sizeof( Op ) );
            op.kind = int( rnd() % 4 );
            switch ( op.kind )
            {
                case OP_BITS:
                    op.bits = 1 + int( rnd() % 32 );
                    op.value = uint32_t( rnd() ) & uint32_t( ( uint64_t( 1 ) << op.bits ) - 1 );
                    ref.Bits( op.value, op.bits );
                    break;
                case OP_ALIGN:
                    ref.Align();
                    break;
                case OP_BYTES:
                    op.length = int( rnd() % 40 );
                    for ( int i = 0; i < op.length; i++ )
                        op.data[i] = uint8_t( rnd() );
                    ref.Bytes( op.data, op.length );
                    break;
                case OP_INT64:
                {
                    op.v64 = int64_t( rnd() );
                    const uint64_t u = uint64_t( op.v64 ) - uint64_t( int64_min );
                    ref.Bits( uint32_t( u ), 32 );
                    ref.Bits( uint32_t( u >> 32 ), 32 );
                    break;
                }
            }
        }

        const int64_t dataBytes = ( ref.bits + 7 ) / 8;
        const int64_t bufferBytes = ( ( dataBytes + 7 ) / 8 ) * 8 + ( ( rnd() & 3 ) == 0 || dataBytes == 0 ? 8 : 0 );
        const int canary = 16;
        uint8_t * buffer = (uint8_t*) malloc( (size_t) ( bufferBytes + canary ) );
        const uint8_t garbage = uint8_t( 0x80 | rnd() );
        memset( buffer, garbage, (size_t) bufferBytes );
        memset( buffer + bufferBytes, 0xAA, canary );

        serialize::WriteStream w( buffer, bufferBytes );
        for ( size_t o = 0; o < ops.size(); o++ )
        {
            Op & op = ops[o];
            switch ( op.kind )
            {
                case OP_BITS: w.SerializeBits( op.value, op.bits ); break;
                case OP_ALIGN: w.SerializeAlign(); break;
                case OP_BYTES: w.SerializeBytes( op.data, op.length ); break;
                case OP_INT64: w.SerializeInteger64( op.v64, int64_min, int64_max ); break;
            }
        }
        w.Flush();

        bool bad = false;
        if ( w.GetBytesProcessed() != dataBytes )
        {
            printf( "  bytes processed %lld, the reference wrote %lld\n", (long long) w.GetBytesProcessed(), (long long) dataBytes );
            bad = true;
        }
        if ( dataBytes > 0 && memcmp( buffer, &ref.bytes[0], (size_t) dataBytes ) != 0 )
        {
            printf( "  wire bytes differ from the reference writer\n" );
            bad = true;
        }
        for ( int i = 0; i < canary; i++ )
        {
            if ( buffer[bufferBytes + i] != 0xAA )
            {
                printf( "  canary overwritten at +%d\n", i );
                bad = true;
                break;
            }
        }
        for ( int64_t i = dataBytes; i < bufferBytes; i++ )
        {
            if ( buffer[i] != 0 && buffer[i] != garbage )
            {
                printf( "  byte past the data written non-zero at %lld\n", (long long) i );
                bad = true;
                break;
            }
        }

        // read back with exactly 8 bytes of slack, filled with 0x00 then 0xFF
        std::vector<uint64_t> results[2];
        bool ok[2];
        for ( int pass = 0; pass < 2; pass++ )
        {
            uint8_t * packet = (uint8_t*) malloc( (size_t) ( dataBytes + 8 ) );
            if ( dataBytes > 0 )
                memcpy( packet, buffer, (size_t) dataBytes );
            memset( packet + dataBytes, pass ? 0xFF : 0x00, 8 );
            serialize::ReadStream r( packet, dataBytes );
            ok[pass] = true;
            for ( size_t o = 0; o < ops.size(); o++ )
            {
                const Op & op = ops[o];
                uint64_t got = 0;
                switch ( op.kind )
                {
                    case OP_BITS:
                    {
                        uint32_t v = 0;
                        ok[pass] = r.SerializeBits( v, op.bits ) && ok[pass];
                        got = v;
                        if ( v != op.value ) { printf( "  read bits mismatch\n" ); bad = true; }
                        break;
                    }
                    case OP_ALIGN:
                        ok[pass] = r.SerializeAlign() && ok[pass];
                        break;
                    case OP_BYTES:
                    {
                        uint8_t d[64];
                        ok[pass] = r.SerializeBytes( d, op.length ) && ok[pass];
                        if ( op.length > 0 && memcmp( d, op.data, (size_t) op.length ) != 0 ) { printf( "  read bytes mismatch\n" ); bad = true; }
                        break;
                    }
                    case OP_INT64:
                    {
                        int64_t v = 0;
                        ok[pass] = r.SerializeInteger64( v, int64_min, int64_max ) && ok[pass];
                        got = uint64_t( v );
                        if ( v != op.v64 ) { printf( "  read int64 mismatch\n" ); bad = true; }
                        break;
                    }
                }
                results[pass].push_back( got );
            }
            ok[pass] = r.SerializeEnd() && ok[pass];
            // one more read past the end must be refused, whatever the slack holds
            uint32_t extra = 0;
            if ( r.SerializeBits( extra, 1 ) ) { printf( "  a read past the end was accepted\n" ); bad = true; }
            free( packet );
        }
        if ( !ok[0] || !ok[1] ) { printf( "  round trip refused (%d,%d)\n", ok[0], ok[1] ); bad = true; }
        if ( results[0] != results[1] ) { printf( "  the slack changed a decoded value\n" ); bad = true; }
        free( buffer );
        if ( bad )
        {
            failures++;
            printf( "  iteration %d failed\n", iteration );
            if ( failures > 5 )
                break;
        }
    }

    if ( failures > 0 )
    {
        printf( "\n*** %d BUFFER EDGE ITERATIONS FAILED ***\n\n", failures );
        return 1;
    }

    printf( "    %d iterations: tight write buffers, canaries, 0x00 and 0xFF slack     pass\n", Iterations );
    printf( "\n*** ALL BUFFER EDGE TESTS PASS ***\n\n" );
    return 0;
}

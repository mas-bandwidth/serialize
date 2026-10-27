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
*/

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

int main()
{
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

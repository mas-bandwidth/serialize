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

    Every input runs two passes:

    1. Hostile read (FuzzRead). ReadStream is the trust boundary of this library: it must survive
       arbitrary hostile bytes, failing reads by returning false, never by corrupting memory or
       tripping an assert. Build with asserts enabled (Debug config) plus ASan/UBSan so all three
       failure modes are caught.

    2. Differential round trip (FuzzRoundTrip). Values generated from the input are written with
       WriteStream, read back with ReadStream, and compared. Any write/read asymmetry traps.
       MeasureStream runs the same ops and must never measure fewer bits than were written.

    The first bytes of the fuzz input are an op program selecting which serialize_* calls run and
    with what parameters. The remaining bytes are the hostile bitstream for pass 1 and the value
    pool for pass 2. This lets coverage-guided fuzzing explore interleavings of every primitive.
*/

#include "serialize.h"

#include <vector>

#define fuzz_check( condition ) do { if ( !(condition) ) { __builtin_trap(); } } while (0)

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

// pass 1: hostile read. arbitrary bytes go in, reads must fail cleanly or produce in-contract values.

template <typename Stream> bool FuzzRead( Stream & stream, const uint8_t * ops, int numOps )
{
    uint8_t bytes[256];
    char string[256];
    wchar_t wstring[64];

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
                fuzz_check( value >= -100 && value <= +100 );
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
                float value = 0.0f;
                serialize_float( stream, value );
            }
            break;

            case 6:
            {
                double value = 0.0;
                serialize_double( stream, value );
            }
            break;

            case 7:
            {
                float value = 0.0f;
                serialize_compressed_float( stream, value, -10.0f, +10.0f, 0.01f );
            }
            break;

            case 8:
            {
                uint64_t value = 0;
                serialize_uint64( stream, value );
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
                serialize_wstring( stream, wstring, (int) ( sizeof( wstring ) / sizeof( wchar_t ) ) );
                fuzz_check( wcslen( wstring ) < sizeof( wstring ) / sizeof( wchar_t ) );
            }
            break;

            case 12:
            {
                serialize_align( stream );
            }
            break;

            case 13:
            {
                int previous = param * 1000 - 8000;
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
            }
            break;
        }
    }

    return true;
}

// pass 2: differential round trip. run once with WriteStream (and MeasureStream), then again with
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
                    fuzz_check( value == expected );
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
                // arbitrary bit patterns, including nan and inf: floats must round trip bit exact
                const uint32_t expected_bits = pool.NextUint32();
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
                const uint64_t expected_bits = pool.NextUint64();
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
                // arbitrary bit patterns again: out of range, nan and inf values must clamp into
                // [min,max] on write, and finite in range values must round trip within the resolution
                const uint32_t expected_bits = pool.NextUint32();
                float expected = 0.0f;
                memcpy( &expected, &expected_bits, 4 );
                float value = Stream::IsWriting ? expected : 0.0f;
                serialize_compressed_float( stream, value, -10.0f, +10.0f, 0.01f );
                if ( Stream::IsReading )
                {
                    fuzz_check( value >= -10.001f && value <= +10.001f );
                    const bool finite = ( expected_bits & 0x7FFFFFFF ) < 0x7F800000;            // bit test: immune to fast math
                    if ( finite && expected >= -10.0f && expected <= +10.0f )
                    {
                        fuzz_check( fabs( value - expected ) <= 0.011f );
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
                    memcpy( value, expected, numBytes );
                }
                serialize_bytes( stream, value, numBytes );
                if ( Stream::IsReading )
                {
                    fuzz_check( memcmp( value, expected, numBytes ) == 0 );
                }
            }
            break;

            case 10:
            {
                char expected[32];
                const int length = pool.NextByte() % ( sizeof( expected ) - 1 );
                for ( int j = 0; j < length; j++ )
                {
                    const uint8_t c = pool.NextByte();
                    expected[j] = ( c != 0 ) ? (char) c : ' ';
                }
                expected[length] = '\0';
                char value[32];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, length + 1 );
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
                wchar_t expected[8];
                const int length = pool.NextByte() % ( sizeof( expected ) / sizeof( wchar_t ) - 1 );
                for ( int j = 0; j < length; j++ )
                    expected[j] = (wchar_t) ( pool.NextUint32() % 0xFFFF + 1 );                 // [1,0xFFFF]: valid for 2 and 4 byte wchar_t
                expected[length] = L'\0';
                wchar_t value[8];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, ( length + 1 ) * sizeof( wchar_t ) );
                }
                serialize_wstring( stream, value, (int) ( sizeof( value ) / sizeof( wchar_t ) ) );
                if ( Stream::IsReading )
                {
                    fuzz_check( wcscmp( value, expected ) == 0 );
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
                const int previous = param * 1000 - 8000;
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
            }
            break;
        }
    }

    return true;
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
    {
        // the buffer allocation must round up to a multiple of 4, because the bit reader reads dwords from memory
        std::vector<uint8_t> buffer( ( payloadBytes + 3 ) & ~size_t(3), 0 );
        memcpy( buffer.data(), payload, payloadBytes );

        serialize::ReadStream stream( buffer.data(), (int) payloadBytes );

        FuzzRead( stream, ops, NumOps );
    }

    // pass 2: differential round trip of values generated from the same bytes
    {
        // worst case is ~260 bytes per op (a 241 byte serialize_bytes plus alignment), so 32 ops fit comfortably
        const int WriteBufferSize = 16 * 1024;
        alignas( 4 ) uint8_t writeBuffer[WriteBufferSize];
        memset( writeBuffer, 0, sizeof( writeBuffer ) );

        serialize::WriteStream writeStream( writeBuffer, WriteBufferSize );
        ValuePool writePool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( writeStream, ops, NumOps, writePool ) == true );             // writing in-range values must always succeed
        writeStream.Flush();

        serialize::MeasureStream measureStream;
        ValuePool measurePool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( measureStream, ops, NumOps, measurePool ) == true );
        fuzz_check( measureStream.GetBitsProcessed() >= writeStream.GetBitsProcessed() );       // measure must be conservative

        serialize::ReadStream readStream( writeBuffer, writeStream.GetBytesProcessed() );
        ValuePool readPool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( readStream, ops, NumOps, readPool ) == true );               // reading back our own data must always succeed
    }

    return 0;
}

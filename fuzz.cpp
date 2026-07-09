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
    libFuzzer harness for the read side of serialize.

    ReadStream is the trust boundary of this library: it must survive arbitrary hostile bytes,
    failing reads by returning false, never by corrupting memory or tripping an assert.
    Build with asserts enabled (Debug config) plus ASan/UBSan so all three failure modes are caught.

    The first bytes of the fuzz input are an op program selecting which serialize_* reads to run
    and with what parameters, the remaining bytes are the bitstream those reads consume.
    This lets coverage-guided fuzzing explore interleavings of every read primitive.
*/

#include "serialize.h"

#include <vector>

#define fuzz_check( condition ) do { if ( !(condition) ) { __builtin_trap(); } } while (0)

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
                fuzz_check( bits == 32 || value < ( 1U << bits ) );
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
                uint16_t sequence = (uint16_t) ( param * 4096 );
                uint16_t ack = 0;
                if ( !serialize::serialize_ack_relative_internal( stream, sequence, ack ) )
                {
                    return false;
                }
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

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    const int NumOps = 32;

    if ( size <= NumOps || size > NumOps + 4096 )
    {
        return 0;
    }

    const uint8_t * ops = data;

    const size_t streamBytes = size - NumOps;

    // the buffer allocation must round up to a multiple of 4, because the bit reader reads dwords from memory
    std::vector<uint8_t> buffer( ( streamBytes + 3 ) & ~size_t(3), 0 );
    memcpy( buffer.data(), data + NumOps, streamBytes );

    serialize::ReadStream stream( buffer.data(), (int) streamBytes );

    FuzzRead( stream, ops, NumOps );

    return 0;
}

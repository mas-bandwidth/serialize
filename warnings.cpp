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
    A consumer translation unit, compiled in CI with -Wall -Wextra -Wconversion -Wsign-conversion
    -Wcast-qual -Wundef as errors (the `strict warnings` job), so serialize.h stays clean under the flags a
    strict consumer builds with. It is never run. It touches every serialize macro, read, write and measure,
    with destination variables of every width and signedness, because the macros expand at the caller and
    a conversion inside one is reported at the caller's line.

    The header is also compiled with SERIALIZE_ENABLE_TESTS (test.cpp, same flags): the embedded tests are
    part of the header.
*/

#include "serialize.h"

struct Inner
{
    int x;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, x, 0, 100 );
        return true;
    }
};

struct Packet
{
    int a;
    int64_t i;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    int8_t s8;
    int16_t s16;
    uint32_t bits5;
    uint64_t bits40;
    bool b;
    float f;
    float cf;
    double d;
    int32_t q32;
    int64_t q64;
    serialize::int128_t wide;
    serialize::uint128_t uwide;
    int previous;
    int current;
    uint8_t bytes[16];
    char string[32];
    Inner inner;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, a, -100, 100 );
        serialize_int( stream, s8, -100, 100 );
        serialize_int( stream, s16, -1000, 1000 );
        serialize_int( stream, u32, 0, 1000 );
        serialize_int64( stream, i, -5000000000LL, 5000000000LL );
        serialize_int128( stream, wide, -5, 5 );
        serialize_bits( stream, bits5, 5 );
        serialize_bits( stream, bits40, 40 );
        serialize_bits( stream, u8, 3 );
        serialize_bool( stream, b );
        serialize_uint8( stream, u8 );
        serialize_uint16( stream, u16 );
        serialize_uint32( stream, u32 );
        serialize_uint64( stream, u64 );
        serialize_uint128( stream, uwide );
        serialize_float( stream, f );
        serialize_double( stream, d );
        serialize_compressed_float( stream, cf, 0.0f, 100.0f, 0.01f );
        serialize_compressed_float( stream, cf, 0, 100, 0.01 );
        serialize_fixed( stream, q32, 16, 16, -100, 100 );
        serialize_fixed( stream, q64, 48, 16, -1000, 1000 );
        serialize_fixed( stream, wide, 112, 16, -1000, 1000 );
        serialize_int_relative( stream, previous, current );
        serialize_align( stream );
        serialize_bytes( stream, bytes, 16 );
        serialize_string( stream, string, 32 );
        serialize_object( stream, inner );
        serialize_end( stream );
        return true;
    }
};

bool write_packet( Packet & packet, uint8_t * buffer, int bytes )
{
    serialize::WriteStream stream( buffer, bytes );
    const bool result = packet.Serialize( stream );
    stream.Flush();
    return result;
}

bool read_packet( Packet & packet, const uint8_t * buffer, int bytes )
{
    serialize::ReadStream stream( buffer, bytes );
    return packet.Serialize( stream );
}

bool measure_packet( Packet & packet )
{
    serialize::MeasureStream stream;
    return packet.Serialize( stream );
}

// the separate read_ and write_ spellings

bool write_separately( Packet & packet, serialize::WriteStream & stream )
{
    write_int( stream, packet.a, -100, 100 );
    write_int64( stream, packet.i, -5000000000LL, 5000000000LL );
    write_int128( stream, packet.wide, -5, 5 );
    write_bits( stream, packet.bits5, 5 );
    write_bits( stream, packet.bits40, 40 );
    write_bool( stream, packet.b );
    write_uint8( stream, packet.u8 );
    write_uint16( stream, packet.u16 );
    write_uint32( stream, packet.u32 );
    write_uint64( stream, packet.u64 );
    write_float( stream, packet.f );
    write_double( stream, packet.d );
    write_align( stream );
    write_bytes( stream, packet.bytes, 16 );
    write_string( stream, packet.string, 32 );
    return true;
}

bool read_separately( Packet & packet, serialize::ReadStream & stream )
{
    read_int( stream, packet.a, -100, 100 );
    read_int64( stream, packet.i, -5000000000LL, 5000000000LL );
    read_int128( stream, packet.wide, -5, 5 );
    read_bits( stream, packet.bits5, 5 );
    read_bits( stream, packet.bits40, 40 );
    read_bool( stream, packet.b );
    read_uint8( stream, packet.u8 );
    read_uint16( stream, packet.u16 );
    read_uint32( stream, packet.u32 );
    read_uint64( stream, packet.u64 );
    read_float( stream, packet.f );
    read_double( stream, packet.d );
    read_align( stream );
    read_bytes( stream, packet.bytes, 16 );
    read_string( stream, packet.string, 32 );
    return true;
}

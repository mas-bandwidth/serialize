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
    serialize benchmark.

    Measures throughput of the raw bitpacker (BitWriter/BitReader) with mixed bit widths,
    and of the stream + serialize macro path with a representative packet.

    Also measures matched pairs of the runtime macros against the compile time parameter
    surface (serialize_*_compile_time), to answer whether moving min/max/bits into template
    arguments buys anything the optimizer wasn't already doing.

    Each benchmark runs several trials and reports the best, to shave off scheduler noise.
    Only release build numbers are meaningful.
*/

#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <chrono>

static volatile uint64_t g_sink = 0;            // defeats dead code elimination of computed values

// Tells the compiler the memory at data is observed, so stores to it cannot be dead code eliminated.
// Without this, gcc proves nothing reads the serialized buffer inside the loop and deletes the
// serialization work entirely, reporting fictional throughput. The empty asm with a memory clobber
// is the standard escape (as used by google benchmark); it emits no instructions.

#if defined(_MSC_VER)
#include <intrin.h>
inline void bench_escape( const void * data )
{
    (void) data;
    _ReadWriteBarrier();
}
#else // #if defined(_MSC_VER)
inline void bench_escape( const void * data )
{
    asm volatile( "" : : "g"( data ) : "memory" );
}
#endif // #if defined(_MSC_VER)

inline double time_now()
{
    return std::chrono::duration<double>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

const int NumTrials = 5;

// ------------------------------------------------------------------------------------------

const int BitpackerBufferSize = 64 * 1024;
const int BitpackerNumPasses = 4096;
const int NumWidths = 16;

static const int bench_widths[NumWidths] = { 1, 32, 7, 13, 3, 25, 8, 19, 4, 28, 11, 16, 2, 30, 6, 22 };        // 227 bits per group

static uint32_t bench_values[NumWidths];

void bench_bitpacker( uint8_t * buffer )
{
    for ( int i = 0; i < NumWidths; i++ )
    {
        const uint32_t mask = ( bench_widths[i] == 32 ) ? 0xFFFFFFFF : ( ( 1u << bench_widths[i] ) - 1 );
        bench_values[i] = ( 0x9E3779B9u * uint32_t( i + 1 ) ) & mask;
    }

    double best_write = 1e30;
    double best_read = 1e30;

    uint64_t bytes_per_pass = 0;

    for ( int trial = 0; trial < NumTrials; trial++ )
    {
        double start = time_now();
        for ( int pass = 0; pass < BitpackerNumPasses; pass++ )
        {
            serialize::BitWriter writer( buffer, BitpackerBufferSize );
            while ( writer.GetBitsAvailable() >= 256 )
            {
                for ( int i = 0; i < NumWidths; i++ )
                    writer.WriteBits( bench_values[i], bench_widths[i] );
            }
            writer.FlushBits();
            bench_escape( buffer );
            bytes_per_pass = (uint64_t) writer.GetBytesWritten();
            g_sink = g_sink + bytes_per_pass;
        }
        double time = time_now() - start;
        if ( time < best_write )
            best_write = time;

        start = time_now();
        for ( int pass = 0; pass < BitpackerNumPasses; pass++ )
        {
            serialize::BitReader reader( buffer, BitpackerBufferSize );
            uint64_t sum = 0;
            while ( reader.GetBitsRemaining() >= 256 )
            {
                for ( int i = 0; i < NumWidths; i++ )
                    sum += reader.ReadBits( bench_widths[i] );
            }
            g_sink = g_sink + sum;
        }
        time = time_now() - start;
        if ( time < best_read )
            best_read = time;
    }

    const double total_mb = double( bytes_per_pass ) * BitpackerNumPasses / ( 1024.0 * 1024.0 );

    printf( "bitpacker write:  %8.1f MB/s\n", total_mb / best_write );
    printf( "bitpacker read:   %8.1f MB/s\n", total_mb / best_read );
}

// ------------------------------------------------------------------------------------------

struct BenchPacket
{
    int32_t a, b, c;
    uint32_t bits7, bits13, bits23;
    bool flag;
    float x, y, z;
    uint64_t big;
    uint8_t blob[17];

    void Init()
    {
        a = -37;
        b = 12345;
        c = 987654;
        bits7 = 97;
        bits13 = 5000;
        bits23 = 1234567;
        flag = true;
        x = 1.5f;
        y = -3.25f;
        z = 100.125f;
        big = 0x123456789ABCDEF0ULL;
        for ( int i = 0; i < (int) sizeof( blob ); i++ )
            blob[i] = (uint8_t) ( i * 31 );
    }

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, a, -100, +100 );
        serialize_int( stream, b, 0, 65535 );
        serialize_int( stream, c, -1000000, +1000000 );
        serialize_bits( stream, bits7, 7 );
        serialize_bits( stream, bits13, 13 );
        serialize_bits( stream, bits23, 23 );
        serialize_bool( stream, flag );
        serialize_float( stream, x );
        serialize_float( stream, y );
        serialize_float( stream, z );
        serialize_uint64( stream, big );
        serialize_bytes( stream, blob, (int) sizeof( blob ) );
        return true;
    }
};

const int StreamNumPackets = 1000000;
const int NumVariants = 64;

// Most packet fields must vary per iteration, driven by a serially dependent generator the compiler
// cannot fold. Varying just one field is not enough: all field bit widths are constant, so gcc
// precomputes the scratch words for the loop-invariant fields at compile time and only patches in
// the varying bits, reporting fictional write throughput. The LCG costs a couple of cycles.

inline uint64_t bench_vary_packet( BenchPacket & packet, uint64_t rng )
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    packet.a = int32_t( ( rng >> 8 ) & 63 ) - 32;                   // [-32,31] within [-100,+100]
    packet.b = uint32_t( rng >> 16 ) & 65535;                       // [0,65535]
    packet.c = int32_t( ( rng >> 24 ) & 0xFFFFF ) - 500000;         // [-500000,548575] within [-1000000,+1000000]
    packet.bits7 = uint32_t( rng ) & 127;
    packet.bits13 = uint32_t( rng >> 3 ) & 8191;
    packet.bits23 = uint32_t( rng >> 5 ) & 8388607;
    packet.flag = ( rng & 1 ) != 0;
    packet.x = float( uint32_t( rng ) & 0xFFFF );
    packet.big = rng;
    packet.blob[0] = uint8_t( rng >> 32 );
    return rng;
}

void bench_stream()
{
    uint8_t buffer[256];
    memset( buffer, 0, sizeof( buffer ) );

    BenchPacket packet;
    packet.Init();

    uint8_t variant_buffers[NumVariants][256];
    int bytes_per_packet = 0;
    {
        uint64_t rng = 1;
        for ( int k = 0; k < NumVariants; k++ )
        {
            memset( variant_buffers[k], 0, sizeof( variant_buffers[k] ) );
            rng = bench_vary_packet( packet, rng );
            serialize::WriteStream stream( variant_buffers[k], (int) sizeof( variant_buffers[k] ) );
            if ( !packet.Serialize( stream ) )
                exit( 1 );
            stream.Flush();
            bytes_per_packet = stream.GetBytesProcessed();
        }
    }

    double best_write = 1e30;
    double best_read = 1e30;
    double best_measure = 1e30;

    for ( int trial = 0; trial < NumTrials; trial++ )
    {
        uint64_t rng = 1;

        double start = time_now();
        for ( int i = 0; i < StreamNumPackets; i++ )
        {
            rng = bench_vary_packet( packet, rng );
            serialize::WriteStream stream( buffer, (int) sizeof( buffer ) );
            if ( !packet.Serialize( stream ) )
                exit( 1 );
            stream.Flush();
            bench_escape( buffer );
            g_sink = g_sink + (uint64_t) stream.GetBytesProcessed();
        }
        double time = time_now() - start;
        if ( time < best_write )
            best_write = time;

        start = time_now();
        for ( int i = 0; i < StreamNumPackets; i++ )
        {
            serialize::ReadStream stream( variant_buffers[i & ( NumVariants - 1 )], bytes_per_packet );
            BenchPacket read_packet;
            if ( !read_packet.Serialize( stream ) )
                exit( 1 );
            bench_escape( &read_packet );               // every decoded field is observed, so the full decode must happen
            g_sink = g_sink + (uint64_t) read_packet.b;
        }
        time = time_now() - start;
        if ( time < best_read )
            best_read = time;

        // note: measure folds to near-constants at compile time by design, so this mostly
        // measures loop overhead. that measure is almost free is the property worth tracking.
        start = time_now();
        for ( int i = 0; i < StreamNumPackets; i++ )
        {
            rng = bench_vary_packet( packet, rng );
            serialize::MeasureStream stream;
            if ( !packet.Serialize( stream ) )
                exit( 1 );
            g_sink = g_sink + (uint64_t) stream.GetBitsProcessed();
        }
        time = time_now() - start;
        if ( time < best_measure )
            best_measure = time;
    }

    const double total_mb = double( bytes_per_packet ) * StreamNumPackets / ( 1024.0 * 1024.0 );
    const double packets = double( StreamNumPackets ) / 1000000.0;

    printf( "stream write:     %8.1f MB/s  (%.1f M packets/s)\n", total_mb / best_write, packets / best_write );
    printf( "stream read:      %8.1f MB/s  (%.1f M packets/s)\n", total_mb / best_read, packets / best_read );
    printf( "stream measure:   %19.1f M packets/s\n", packets / best_measure );
}

// ------------------------------------------------------------------------------------------

/*
    string + wstring rows (mas-bandwidth/schema#64). Measure-first: no string or wstring row
    existed anywhere in the family's benches, and rows land before/with any string or wstring
    change — you can't improve what you don't measure.

    Corpus composition, stated per BENCH-STANDARD §1.7 (bulk share by bits, declared in the
    definition so the audit never has to be re-derived):

      - string row: pinned-length 24 byte UTF-8 payload behind a 6 bit length field.
        200 wire bits = 25 bytes; 192 of them ride serialize_bytes, so the row is 96% bulk
        by bits. That is the point of the row — it measures the bulk string path (length
        dispatch + align + memcpy + the read-side interior-NUL scan and UTF-8 validation) —
        and per §1.7 it must never lead a headline table without this bulk share captioned.

      - wstring row: pinned-length 24 unit UTF-16 payload behind a 6 bit length field.
        774 wire bits = 97 bytes; 0% bulk by bits, because the wstring wire format is one
        32 bit group per UTF-16 code unit, each an individual serialize_bits dispatch, not
        a bulk byte copy. The row measures that per-unit dispatch plus the read-side
        validation the wire contract demands (group range, surrogate pairing, interior NUL).

    Lengths are pinned so every variant buffer is byte-identical in size (24 matches the
    ~24 byte average chat string in the §1.7 audit); content varies per iteration through
    the same serially dependent LCG as the other rows, so the payload cannot be folded.
    Additive only per §1.7: no existing row is touched. Iteration counts are sized so each
    leg exceeds the 200 ms floor (§2.1) on the Apple Silicon reference machine.
*/

const int StringBufferSize = 64;            // bounds the length field: serialize_int( length, 0, 63 ) = 6 bits
const int StringPinnedLength = 24;          // bytes (string) or UTF-16 units (wstring), pinned

const int StringNumPackets = 32000000;
const int WStringNumPackets = 16000000;

struct BenchStringPacket
{
    char text[StringBufferSize];

    void Init()
    {
        for ( int i = 0; i < StringPinnedLength; i++ )
            text[i] = (char) ( 'a' + ( i * 7 ) % 26 );
        text[StringPinnedLength] = '\0';
    }

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_string( stream, text, StringBufferSize );
        return true;
    }
};

inline uint64_t bench_vary_string( BenchStringPacket & packet, uint64_t rng )
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    // four positions per iteration, printable ASCII, so the payload stays valid UTF-8 with no interior NUL
    packet.text[ ( rng >> 3 ) % StringPinnedLength ] = (char) ( 'a' + ( rng >> 8 ) % 26 );
    packet.text[ ( rng >> 13 ) % StringPinnedLength ] = (char) ( 'A' + ( rng >> 19 ) % 26 );
    packet.text[ ( rng >> 27 ) % StringPinnedLength ] = (char) ( '0' + ( rng >> 33 ) % 10 );
    packet.text[ ( rng >> 41 ) % StringPinnedLength ] = (char) ( 'a' + ( rng >> 47 ) % 26 );
    return rng;
}

struct BenchWStringPacket
{
    wchar_t text[StringBufferSize];

    void Init()
    {
        for ( int i = 0; i < StringPinnedLength; i++ )
            text[i] = (wchar_t) ( 0x4E00 + i );         // BMP, one code unit each: no surrogates, no NUL
        text[StringPinnedLength] = L'\0';
    }

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_wstring( stream, text, StringBufferSize );
        return true;
    }
};

inline uint64_t bench_vary_wstring( BenchWStringPacket & packet, uint64_t rng )
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    // four positions per iteration, pinned to the CJK block: BMP code points, one unit each, no surrogates, no NUL
    packet.text[ ( rng >> 3 ) % StringPinnedLength ] = (wchar_t) ( 0x4E00 + ( ( rng >> 8 ) & 0xFFF ) );
    packet.text[ ( rng >> 13 ) % StringPinnedLength ] = (wchar_t) ( 0x4E00 + ( ( rng >> 19 ) & 0xFFF ) );
    packet.text[ ( rng >> 27 ) % StringPinnedLength ] = (wchar_t) ( 0x4E00 + ( ( rng >> 33 ) & 0xFFF ) );
    packet.text[ ( rng >> 41 ) % StringPinnedLength ] = (wchar_t) ( 0x4E00 + ( ( rng >> 47 ) & 0xFFF ) );
    return rng;
}

// Same trial structure, escape barriers and variant-buffer read scheme as bench_stream, so the
// string rows are comparable with the stream rows above them. Packet provides Init (the pinned
// content the LCG then perturbs); pinned lengths make every variant the same wire size, which
// the setup loop verifies rather than assumes.

template <typename Packet> void bench_string_shape( const char * write_label, const char * read_label, int num_packets, uint64_t (*vary)( Packet &, uint64_t ) )
{
    uint8_t buffer[256];
    memset( buffer, 0, sizeof( buffer ) );

    Packet packet;
    packet.Init();

    uint8_t variant_buffers[NumVariants][256];
    int bytes_per_packet = 0;
    {
        uint64_t rng = 1;
        for ( int k = 0; k < NumVariants; k++ )
        {
            memset( variant_buffers[k], 0, sizeof( variant_buffers[k] ) );
            rng = vary( packet, rng );
            serialize::WriteStream stream( variant_buffers[k], (int) sizeof( variant_buffers[k] ) );
            if ( !packet.Serialize( stream ) )
                exit( 1 );
            stream.Flush();
            if ( bytes_per_packet != 0 && stream.GetBytesProcessed() != bytes_per_packet )
                exit( 1 );                              // the pinned length must make every variant the same size
            bytes_per_packet = stream.GetBytesProcessed();
        }
    }

    double best_write = 1e30;
    double best_read = 1e30;

    for ( int trial = 0; trial < NumTrials; trial++ )
    {
        uint64_t rng = 1;

        double start = time_now();
        for ( int i = 0; i < num_packets; i++ )
        {
            rng = vary( packet, rng );
            serialize::WriteStream stream( buffer, (int) sizeof( buffer ) );
            if ( !packet.Serialize( stream ) )
                exit( 1 );
            stream.Flush();
            bench_escape( buffer );
            g_sink = g_sink + (uint64_t) stream.GetBytesProcessed();
        }
        double time = time_now() - start;
        if ( time < best_write )
            best_write = time;

        start = time_now();
        for ( int i = 0; i < num_packets; i++ )
        {
            serialize::ReadStream stream( variant_buffers[i & ( NumVariants - 1 )], bytes_per_packet );
            Packet read_packet;
            if ( !read_packet.Serialize( stream ) )
                exit( 1 );
            bench_escape( &read_packet );               // every decoded unit is observed, so the full decode + validation must happen
            g_sink = g_sink + (uint64_t) *(const uint8_t*) &read_packet;
        }
        time = time_now() - start;
        if ( time < best_read )
            best_read = time;
    }

    const double total_mb = double( bytes_per_packet ) * num_packets / ( 1024.0 * 1024.0 );
    const double packets = double( num_packets ) / 1000000.0;

    printf( "%s %8.1f MB/s  (%.1f M packets/s)\n", write_label, total_mb / best_write, packets / best_write );
    printf( "%s %8.1f MB/s  (%.1f M packets/s)\n", read_label, total_mb / best_read, packets / best_read );
}

void bench_strings()
{
    bench_string_shape<BenchStringPacket>( "string write:    ", "string read:     ", StringNumPackets, bench_vary_string );
    bench_string_shape<BenchWStringPacket>( "wstring write:   ", "wstring read:    ", WStringNumPackets, bench_vary_wstring );
}

// ------------------------------------------------------------------------------------------

// Matched pairs: the same packet serialized through the runtime macros and through the compile
// time parameter surface. Same data, same serially dependent LCG variation pattern, same escape
// barriers, same trial structure, so any difference is the forms themselves, not the harness.
// Each shape derives its two packet forms from one fields struct, so a single vary function
// drives both sides of a pair with identical values.

template <typename Fields, typename Packet> void bench_packet_shape( const char * label, uint64_t (*vary)( Fields &, uint64_t ) )
{
    uint8_t buffer[256];
    memset( buffer, 0, sizeof( buffer ) );

    Packet packet = Packet();

    // pre-write variant packets for the read benchmark, using the same LCG sequence as the write loop
    uint8_t variant_buffers[NumVariants][256];
    int bytes_per_packet = 0;
    {
        uint64_t rng = 1;
        for ( int k = 0; k < NumVariants; k++ )
        {
            memset( variant_buffers[k], 0, sizeof( variant_buffers[k] ) );
            rng = vary( packet, rng );
            serialize::WriteStream stream( variant_buffers[k], (int) sizeof( variant_buffers[k] ) );
            if ( !packet.Serialize( stream ) )
                exit( 1 );
            stream.Flush();
            bytes_per_packet = stream.GetBytesProcessed();
        }
    }

    double best_write = 1e30;
    double best_read = 1e30;

    for ( int trial = 0; trial < NumTrials; trial++ )
    {
        uint64_t rng = 1;

        double start = time_now();
        for ( int i = 0; i < StreamNumPackets; i++ )
        {
            rng = vary( packet, rng );
            serialize::WriteStream stream( buffer, (int) sizeof( buffer ) );
            if ( !packet.Serialize( stream ) )
                exit( 1 );
            stream.Flush();
            bench_escape( buffer );
            g_sink = g_sink + (uint64_t) stream.GetBytesProcessed();
        }
        double time = time_now() - start;
        if ( time < best_write )
            best_write = time;

        start = time_now();
        for ( int i = 0; i < StreamNumPackets; i++ )
        {
            serialize::ReadStream stream( variant_buffers[i & ( NumVariants - 1 )], bytes_per_packet );
            Packet read_packet;
            if ( !read_packet.Serialize( stream ) )
                exit( 1 );
            bench_escape( &read_packet );               // every decoded field is observed, so the full decode must happen
            g_sink = g_sink + (uint64_t) *(const uint8_t*) &read_packet;
        }
        time = time_now() - start;
        if ( time < best_read )
            best_read = time;
    }

    const double packets = double( StreamNumPackets ) / 1000000.0;

    printf( "%s  write: %6.1f M packets/s   read: %6.1f M packets/s\n", label, packets / best_write, packets / best_read );
}

// pair 1: a realistic packet of ~10 bounded ints, runtime serialize_int vs serialize_int_compile_time

struct BenchIntFields
{
    int32_t f0, f1, f2, f3, f4, f5, f6, f7, f8, f9;
};

inline uint64_t bench_vary_int_fields( BenchIntFields & f, uint64_t rng )
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    f.f0 = int32_t( ( rng >> 8 ) & 63 ) - 32;                       // within [-100,+100]
    f.f1 = int32_t( uint32_t( rng >> 16 ) & 65535 );                // [0,65535]
    f.f2 = int32_t( ( rng >> 24 ) & 0xFFFFF ) - 500000;             // within [-1000000,+1000000]
    f.f3 = int32_t( uint32_t( rng >> 2 ) & 3 );                     // [0,3]
    f.f4 = int32_t( ( rng >> 11 ) & 15 ) - 8;                       // within [-15,+15]
    f.f5 = int32_t( uint32_t( rng >> 22 ) & 511 );                  // within [0,1000]
    f.f6 = int32_t( ( rng >> 33 ) & 2047 ) - 1024;                  // within [-2048,+2047]
    f.f7 = int32_t( uint32_t( rng >> 40 ) & 255 );                  // [0,255]
    f.f8 = int32_t( ( rng >> 30 ) & 0xFFFFF ) - 500000;             // within [-600000,+600000]
    f.f9 = int32_t( uint32_t( rng >> 57 ) & 63 );                   // within [0,100]
    return rng;
}

struct BenchIntPacketRuntime : public BenchIntFields
{
    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, f0, -100, +100 );
        serialize_int( stream, f1, 0, 65535 );
        serialize_int( stream, f2, -1000000, +1000000 );
        serialize_int( stream, f3, 0, 3 );
        serialize_int( stream, f4, -15, +15 );
        serialize_int( stream, f5, 0, 1000 );
        serialize_int( stream, f6, -2048, +2047 );
        serialize_int( stream, f7, 0, 255 );
        serialize_int( stream, f8, -600000, +600000 );
        serialize_int( stream, f9, 0, 100 );
        return true;
    }
};

struct BenchIntPacketCompileTime : public BenchIntFields
{
    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int_compile_time( stream, f0, -100, +100 );
        serialize_int_compile_time( stream, f1, 0, 65535 );
        serialize_int_compile_time( stream, f2, -1000000, +1000000 );
        serialize_int_compile_time( stream, f3, 0, 3 );
        serialize_int_compile_time( stream, f4, -15, +15 );
        serialize_int_compile_time( stream, f5, 0, 1000 );
        serialize_int_compile_time( stream, f6, -2048, +2047 );
        serialize_int_compile_time( stream, f7, 0, 255 );
        serialize_int_compile_time( stream, f8, -600000, +600000 );
        serialize_int_compile_time( stream, f9, 0, 100 );
        return true;
    }
};

// pair 2: mixed bit widths including one wider than 32 bits, runtime serialize_bits vs serialize_bits_compile_time

struct BenchBitsFields
{
    uint32_t b7, b13, b23, b3, b32, b11, b19;
    uint64_t b48;
};

inline uint64_t bench_vary_bits_fields( BenchBitsFields & f, uint64_t rng )
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    f.b7 = uint32_t( rng ) & 127;
    f.b13 = uint32_t( rng >> 3 ) & 8191;
    f.b23 = uint32_t( rng >> 5 ) & 8388607;
    f.b3 = uint32_t( rng >> 29 ) & 7;
    f.b32 = uint32_t( rng >> 16 );
    f.b11 = uint32_t( rng >> 37 ) & 2047;
    f.b19 = uint32_t( rng >> 44 ) & 524287;
    f.b48 = rng & 0xFFFFFFFFFFFFULL;
    return rng;
}

struct BenchBitsPacketRuntime : public BenchBitsFields
{
    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_bits( stream, b7, 7 );
        serialize_bits( stream, b13, 13 );
        serialize_bits( stream, b23, 23 );
        serialize_bits( stream, b3, 3 );
        serialize_bits( stream, b32, 32 );
        serialize_bits( stream, b11, 11 );
        serialize_bits( stream, b19, 19 );
        serialize_bits( stream, b48, 48 );
        return true;
    }
};

struct BenchBitsPacketCompileTime : public BenchBitsFields
{
    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_bits_compile_time( stream, b7, 7 );
        serialize_bits_compile_time( stream, b13, 13 );
        serialize_bits_compile_time( stream, b23, 23 );
        serialize_bits_compile_time( stream, b3, 3 );
        serialize_bits_compile_time( stream, b32, 32 );
        serialize_bits_compile_time( stream, b11, 11 );
        serialize_bits_compile_time( stream, b19, 19 );
        serialize_bits64_compile_time( stream, b48, 48 );
        return true;
    }
};

// pair 3: a "generated packet" shape mixing bounded ints, bits and bools, the way schema generated code looks

struct BenchGenFields
{
    int32_t sequence;       // [0,65535]
    uint32_t ack_bits;      // 32 bits
    uint32_t entity_id;     // 12 bits
    int32_t pos_x, pos_y, pos_z;    // [-16384,+16383]
    uint32_t yaw;           // 9 bits
    bool moving;
    bool firing;
    uint64_t timestamp;     // 48 bits
    int32_t weapon;         // [0,15]
};

inline uint64_t bench_vary_gen_fields( BenchGenFields & f, uint64_t rng )
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    f.sequence = int32_t( uint32_t( rng >> 8 ) & 65535 );
    f.ack_bits = uint32_t( rng >> 16 );
    f.entity_id = uint32_t( rng ) & 4095;
    f.pos_x = int32_t( ( rng >> 20 ) & 32767 ) - 16384;
    f.pos_y = int32_t( ( rng >> 25 ) & 32767 ) - 16384;
    f.pos_z = int32_t( ( rng >> 30 ) & 32767 ) - 16384;
    f.yaw = uint32_t( rng >> 3 ) & 511;
    f.moving = ( rng & 1 ) != 0;
    f.firing = ( rng & 2 ) != 0;
    f.timestamp = rng & 0xFFFFFFFFFFFFULL;
    f.weapon = int32_t( uint32_t( rng >> 60 ) & 15 );
    return rng;
}

struct BenchGenPacketRuntime : public BenchGenFields
{
    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, sequence, 0, 65535 );
        serialize_bits( stream, ack_bits, 32 );
        serialize_bits( stream, entity_id, 12 );
        serialize_int( stream, pos_x, -16384, +16383 );
        serialize_int( stream, pos_y, -16384, +16383 );
        serialize_int( stream, pos_z, -16384, +16383 );
        serialize_bits( stream, yaw, 9 );
        serialize_bool( stream, moving );
        serialize_bool( stream, firing );
        serialize_bits( stream, timestamp, 48 );
        serialize_int( stream, weapon, 0, 15 );
        return true;
    }
};

struct BenchGenPacketCompileTime : public BenchGenFields
{
    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int_compile_time( stream, sequence, 0, 65535 );
        serialize_bits_compile_time( stream, ack_bits, 32 );
        serialize_bits_compile_time( stream, entity_id, 12 );
        serialize_int_compile_time( stream, pos_x, -16384, +16383 );
        serialize_int_compile_time( stream, pos_y, -16384, +16383 );
        serialize_int_compile_time( stream, pos_z, -16384, +16383 );
        serialize_bits_compile_time( stream, yaw, 9 );
        serialize_bool_compile_time( stream, moving );
        serialize_bool_compile_time( stream, firing );
        serialize_bits64_compile_time( stream, timestamp, 48 );
        serialize_int_compile_time( stream, weapon, 0, 15 );
        return true;
    }
};

void bench_compile_time_pairs()
{
    bench_packet_shape<BenchIntFields, BenchIntPacketRuntime>          ( "int packet   (runtime):     ", bench_vary_int_fields );
    bench_packet_shape<BenchIntFields, BenchIntPacketCompileTime>      ( "int packet   (compile time):", bench_vary_int_fields );
    bench_packet_shape<BenchBitsFields, BenchBitsPacketRuntime>        ( "bits packet  (runtime):     ", bench_vary_bits_fields );
    bench_packet_shape<BenchBitsFields, BenchBitsPacketCompileTime>    ( "bits packet  (compile time):", bench_vary_bits_fields );
    bench_packet_shape<BenchGenFields, BenchGenPacketRuntime>          ( "mixed packet (runtime):     ", bench_vary_gen_fields );
    bench_packet_shape<BenchGenFields, BenchGenPacketCompileTime>      ( "mixed packet (compile time):", bench_vary_gen_fields );
}

// ------------------------------------------------------------------------------------------

int main()
{
    printf( "\n[serialize benchmark]\n\n" );

#ifdef SERIALIZE_DEBUG
    printf( "WARNING: this is a debug build. only release build numbers are meaningful!\n\n" );
#endif

    uint8_t * buffer = (uint8_t*) malloc( BitpackerBufferSize + 8 );        // + 8: read allocations extend 8 bytes past the data

    bench_bitpacker( buffer );

    bench_stream();

    bench_strings();

    printf( "\n" );

    bench_compile_time_pairs();

    free( buffer );

    printf( "\n" );

    return 0;
}

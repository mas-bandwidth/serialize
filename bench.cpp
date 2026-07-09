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

    Each benchmark runs several trials and reports the best, to shave off scheduler noise.
    Only release build numbers are meaningful.
*/

#include "serialize.h"

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

int main()
{
    printf( "\n[serialize benchmark]\n\n" );

#ifdef SERIALIZE_DEBUG
    printf( "WARNING: this is a debug build. only release build numbers are meaningful!\n\n" );
#endif

    uint8_t * buffer = (uint8_t*) malloc( BitpackerBufferSize );

    bench_bitpacker( buffer );

    bench_stream();

    free( buffer );

    printf( "\n" );

    return 0;
}

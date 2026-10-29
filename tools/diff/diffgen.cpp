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
    The differential generator: live outputs of the C++ reference, in the corpus's own format.

    The committed corpus is never generated from an implementation (STANDARD.md): it is what the
    document says, written by hand, and it is the instrument. This tool is a different instrument and
    is never added to the corpus. It asks a different question: given inputs nobody wrote by hand, does
    every port decide exactly as the reference does today? Each night the workflow
    .github/workflows/nightly-diff.yml generates a fresh set from a seed, then holds every port's
    own conformance runner to it. A port that disagrees with the reference on a generated sequence is
    either a port bug or a reference bug; the report names the vector and the seed, and the hand
    written corpus is what decides which.

    Every generated record is a `sequence` (STANDARD.md, "A sequence vector states more than one
    operation"), so no port needs a runner it does not already have:

      * a random program of up to eight operations, with declarations drawn from the shapes the
        corpus uses (ranged ints at every width class, the fixed point declarations the runners
        carry, quantized floats, strings, bytes, alignments, the end of the object);
      * the stream for it is written by the reference writer from random in-range values, so the
        accepted path is exercised at depth and not only by luck;
      * about half the streams are then damaged: bits flipped, bytes dropped, bytes appended, the
        last byte forced to all ones. The reference reader's verdict on the stream is the expectation:
        the decoded values and the bits consumed when it accepts, the refusal reason when it refuses;
      * an accepted record also carries `writer canonical` when the reference writer re-emits exactly
        the stream's bytes from the decoded values.

    The generator runs each record through the same reader, writer and terminality checks the C++
    conformance runner applies to the corpus before printing it, so a record the reference itself
    would fail never leaves this tool.

    usage: diffgen <seed> <count> <output file>

    The generator is deterministic in its arguments: no clock, no environment.

    It includes conformance.cpp for the step machine, the one execution path the corpus uses, so the
    two cannot drift apart. conformance.cpp's own main is renamed away.
*/

#define main conformance_main
#include "../../conformance.cpp"
#undef main

static uint64_t rng_state = 1;

static uint64_t rng_next()
{
    // xorshift64*
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static uint64_t rng_below( uint64_t n )
{
    return n == 0 ? 0 : ( rng_next() >> 11 ) % n;
}

static bool rng_chance( int percent )
{
    return rng_below( 100 ) < (uint64_t) percent;
}

static serialize::uint128_t rng_128()
{
    return ( serialize::uint128_t( rng_next() ) << 64 ) | serialize::uint128_t( rng_next() );
}

// a value in [min,max], biased toward the edges, where the refusals and the tiers live

static serialize::int128_t rng_in_range( serialize::int128_t min, serialize::int128_t max )
{
    if ( min == max )
        return min;
    const uint64_t pick = rng_below( 10 );
    if ( pick == 0 ) return min;
    if ( pick == 1 ) return max;
    if ( pick == 2 ) return min + 1;
    if ( pick == 3 ) return max - 1;
    // span + 1 wraps to zero for the full 128 bit range, which means "any pattern"
    const serialize::uint128_t span = serialize::uint128_t( max ) - serialize::uint128_t( min ) + serialize::uint128_t( 1 );
    serialize::uint128_t offset = rng_128();
    if ( span != 0 )
        offset = offset % span;
    return serialize::int128_t( serialize::uint128_t( min ) + offset );
}

static void format_128( serialize::int128_t value, char * out )
{
    // signed decimal would need a 128 bit divide on emulated platforms; the corpus accepts 0x, and the
    // text is parsed back as the same two's complement pattern
    char hex[40];
    render_hex128( serialize::uint128_t( value ), hex );
    strcpy( out, hex );
}

struct Bound
{
    const char * text;
    serialize::int128_t value;
};

static serialize::int128_t pow2( int n )
{
    return serialize::int128_t( 1 ) << n;
}

struct Program
{
    int numSteps;
    char words[MaxSteps][MaxLine];
};

// the fixed point declarations the runners carry, as step words, with the raw value range of each

struct FixedShape
{
    const char * words;
    FixedDeclaration declaration;
    int fractionBits;
    int64_t min;                // whole units, where they fit 64 bits
    int64_t max;
};

static const FixedShape fixedShapes[] =
{
    { "fixed 8 8 -100 100",          FIXED_Q8_8_M100_100,          8,  -100, 100 },
    { "fixed 32 0 0 7",              FIXED_Q32_0_0_7,              0,  0, 7 },
    { "fixed 32 0 0 5",              FIXED_Q32_0_0_5,              0,  0, 5 },
    { "fixed 16 16 7 7",             FIXED_Q16_16_7_7,             16, 7, 7 },
    { "fixed 16 16 -32768 32767",    FIXED_Q16_16_M32768_32767,    16, -32768, 32767 },
    { "fixed 16 16 -2000 2000",      FIXED_Q16_16_M2000_2000,      16, -2000, 2000 },
    { "fixed 16 16 0 30000",         FIXED_Q16_16_0_30000,         16, 0, 30000 },
    { "fixed 48 16 0 131072",        FIXED_Q48_16_0_131072,        16, 0, 131072 },
    { "fixed 48 16 -100000 100000",  FIXED_Q48_16_M100000_100000,  16, -100000, 100000 },
    { "fixed 112 16 -9 -9",          FIXED_Q112_16_M9_M9,          16, -9, -9 },
};

static const int numFixedShapes = (int) ( sizeof( fixedShapes ) / sizeof( fixedShapes[0] ) );

static void generate_program( Program & program )
{
    program.numSteps = 1 + (int) rng_below( 8 );

    for ( int i = 0; i < program.numSteps; i++ )
    {
        char * word = program.words[i];
        const bool last = ( i == program.numSteps - 1 );

        switch ( rng_below( 16 ) )
        {
            case 0:
            case 1:
            {
                const int widths[] = { 1, 2, 3, 5, 7, 8, 9, 13, 16, 17, 24, 31, 32, 33, 48, 63, 64 };
                snprintf( word, MaxLine, "bits %d", widths[rng_below( sizeof( widths ) / sizeof( widths[0] ) )] );
            }
            break;

            case 2:  strcpy( word, "bool" ); break;
            case 3:  strcpy( word, "align" ); break;
            case 4:  strcpy( word, "float" ); break;
            case 5:  strcpy( word, "double" ); break;
            case 6:  strcpy( word, "uint128" ); break;

            case 7:
            {
                static const int32_t pairs[][2] =
                {
                    { -100, 100 }, { 0, 7 }, { 0, 8 }, { 0, 255 }, { 0, 256 }, { -1000, 1000 }, { 42, 42 },
                    { 0, 1000000 }, { INT32_MIN, INT32_MAX }, { 0, INT32_MAX }, { INT32_MIN, 0 }, { -5, 10 }
                };
                const int pick = (int) rng_below( sizeof( pairs ) / sizeof( pairs[0] ) );
                snprintf( word, MaxLine, "int %d %d", (int) pairs[pick][0], (int) pairs[pick][1] );
            }
            break;

            case 8:
            {
                static const int64_t pairs[][2] =
                {
                    { 0, 1 }, { -1, 1 }, { 0, 4294967295LL }, { 0, 4294967296LL }, { -1000000000000LL, 1000000000000LL },
                    { INT64_MIN, INT64_MAX }, { 0, INT64_MAX }, { INT64_MIN, 0 }, { 7, 7 }, { 0, 1099511627775LL }
                };
                const int pick = (int) rng_below( sizeof( pairs ) / sizeof( pairs[0] ) );
                snprintf( word, MaxLine, "int64 %lld %lld", (long long) pairs[pick][0], (long long) pairs[pick][1] );
            }
            break;

            case 9:
            {
                static const int bounds[] = { 0, 8, 31, 63, 64, 65, 100, 126 };       // 2^n spans, group boundaries included
                const int n = bounds[rng_below( sizeof( bounds ) / sizeof( bounds[0] ) )];
                char low[40];
                char high[40];
                if ( rng_chance( 30 ) )
                {
                    format_128( serialize::int128_t( serialize::uint128_t( 1 ) << 127 ), low );
                    format_128( serialize::int128_t( ~( serialize::uint128_t( 1 ) << 127 ) ), high );
                }
                else
                {
                    format_128( -pow2( n ), low );
                    format_128( pow2( n ), high );
                }
                snprintf( word, MaxLine, "int128 %s %s", low, high );
            }
            break;

            case 10:
            {
                const int64_t previous = rng_chance( 50 ) ? (int64_t) rng_below( 300 ) : (int64_t) rng_below( 0x7FFFFFFFULL - 1 );
                snprintf( word, MaxLine, "int_relative %lld", (long long) previous );
            }
            break;

            case 11:
            {
                static const char * declarations[] =
                {
                    "compressed_float 0 10 0.1", "compressed_float -10 10 0.01", "compressed_float 0 1 0.001",
                    "compressed_float -1000 1000 0.5", "compressed_float 0 100000 1", "compressed_float -1 1 0.25"
                };
                strcpy( word, declarations[rng_below( sizeof( declarations ) / sizeof( declarations[0] ) )] );
            }
            break;

            case 12:
            {
                strcpy( word, fixedShapes[rng_below( (uint64_t) numFixedShapes )].words );
            }
            break;

            case 13:
            {
                snprintf( word, MaxLine, "bytes %d", 1 + (int) rng_below( 24 ) );
            }
            break;

            case 14:
            {
                snprintf( word, MaxLine, "string %d", 2 + (int) rng_below( 30 ) );
            }
            break;

            default:
            {
                if ( last )
                    strcpy( word, "end" );
                else
                    strcpy( word, "bool" );
            }
            break;
        }

        // end is only meaningful as the last operation: the object is over
        if ( strcmp( word, "end" ) == 0 && !last )
        {
            strcpy( word, "bits 3" );
        }
    }
}

// random text that the strict codec accepts and that fits in `capacity` bytes with its NUL

static void random_text( uint8_t * out, int capacity )
{
    int length = 0;
    const int target = (int) rng_below( (uint64_t) capacity );        // [0,capacity-1] bytes
    while ( length < target )
    {
        uint32_t code;
        switch ( rng_below( 6 ) )
        {
            case 0:  code = 0x20 + (uint32_t) rng_below( 0x5F ); break;
            case 1:  code = 0x80 + (uint32_t) rng_below( 0x780 ); break;
            case 2:  code = 0x800 + (uint32_t) rng_below( 0xD000 - 0x800 ); break;
            case 3:  code = 0xE000 + (uint32_t) rng_below( 0x2000 ); break;
            case 4:  code = 0x10000 + (uint32_t) rng_below( 0x10000 ); break;
            default: code = 0x61 + (uint32_t) rng_below( 26 ); break;
        }
        uint8_t encoded[4];
        int width;
        if ( code < 0x80 )          { encoded[0] = (uint8_t) code; width = 1; }
        else if ( code < 0x800 )    { encoded[0] = (uint8_t) ( 0xC0 | ( code >> 6 ) ); encoded[1] = (uint8_t) ( 0x80 | ( code & 0x3F ) ); width = 2; }
        else if ( code < 0x10000 )  { encoded[0] = (uint8_t) ( 0xE0 | ( code >> 12 ) ); encoded[1] = (uint8_t) ( 0x80 | ( ( code >> 6 ) & 0x3F ) ); encoded[2] = (uint8_t) ( 0x80 | ( code & 0x3F ) ); width = 3; }
        else                        { encoded[0] = (uint8_t) ( 0xF0 | ( code >> 18 ) ); encoded[1] = (uint8_t) ( 0x80 | ( ( code >> 12 ) & 0x3F ) ); encoded[2] = (uint8_t) ( 0x80 | ( ( code >> 6 ) & 0x3F ) ); encoded[3] = (uint8_t) ( 0x80 | ( code & 0x3F ) ); width = 4; }
        if ( length + width > capacity - 1 )
            break;
        memcpy( out + length, encoded, (size_t) width );
        length += width;
    }
    out[length] = '\0';
}

static uint32_t random_finite_float_bits()
{
    uint32_t bits = (uint32_t) rng_next();
    if ( !serialize::float_bits_are_finite( bits ) )
        bits &= 0x807FFFFF;
    return bits;
}

static uint64_t random_finite_double_bits()
{
    uint64_t bits = rng_next();
    if ( !serialize::double_bits_are_finite( bits ) )
        bits &= 0x800FFFFFFFFFFFFFULL;
    return bits;
}

// give a parsed step a value the writer will accept

static bool seed_step_value( Step & step )
{
    switch ( step.kind )
    {
        case STEP_BITS:
        {
            const uint64_t mask = step.width >= 64 ? ~0ULL : ( ( 1ULL << step.width ) - 1 );
            step.bits = serialize::uint128_t( rng_next() & mask );
        }
        return true;

        case STEP_BOOL:         step.boolean = rng_chance( 50 ); return true;
        case STEP_UINT128:      step.bits = rng_128(); return true;
        case STEP_ALIGN:        return true;
        case STEP_END:          return true;

        case STEP_INT:
        case STEP_INT64:
        case STEP_INT128:
            step.number = rng_in_range( step.min, step.max );
            return true;

        case STEP_INT_RELATIVE:
        {
            const int64_t room = (int64_t) INT32_MAX - (int64_t) step.previous;
            if ( room < 1 )
                return false;
            const int64_t delta = rng_chance( 70 ) ? 1 + (int64_t) rng_below( (uint64_t) ( room < 300 ? room : 300 ) ) : 1 + (int64_t) rng_below( (uint64_t) room );
            step.number = serialize::int128_t( (int64_t) step.previous + delta );
        }
        return true;

        case STEP_FLOAT:        step.bits = serialize::uint128_t( random_finite_float_bits() ); return true;
        case STEP_DOUBLE:       step.bits = serialize::uint128_t( random_finite_double_bits() ); return true;

        case STEP_COMPRESSED_FLOAT:
        {
            // a value anywhere in [min,max], between quanta as often as on one
            const double t = double( rng_below( 1000001 ) ) / 1000000.0;
            const float value = (float) ( (double) step.fmin + t * ( (double) step.fmax - (double) step.fmin ) );
            uint32_t pattern;
            memcpy( &pattern, &value, 4 );
            step.bits = serialize::uint128_t( pattern );
        }
        return true;

        case STEP_BYTES:
            for ( int64_t i = 0; i < step.width; i++ )
                step.buffer[i] = (uint8_t) rng_next();
            return true;

        case STEP_STRING:
            random_text( step.buffer, (int) step.width );
            return true;

        case STEP_FIXED:
        {
            for ( int i = 0; i < numFixedShapes; i++ )
            {
                if ( fixedShapes[i].declaration == step.fixedDeclaration )
                {
                    const serialize::int128_t low = serialize::int128_t( fixedShapes[i].min ) * pow2( fixedShapes[i].fractionBits );
                    const serialize::int128_t high = serialize::int128_t( fixedShapes[i].max ) * pow2( fixedShapes[i].fractionBits );
                    step.number = rng_in_range( low, high );
                    return true;
                }
            }
        }
        return false;

        default:
            return false;
    }
}

static void damage( uint8_t * bytes, int & numBytes )
{
    const uint64_t kind = rng_below( 6 );
    if ( kind == 0 && numBytes > 0 )
    {
        // one to three single bit flips
        const int flips = 1 + (int) rng_below( 3 );
        for ( int i = 0; i < flips; i++ )
            bytes[rng_below( (uint64_t) numBytes )] ^= (uint8_t) ( 1 << rng_below( 8 ) );
    }
    else if ( kind == 1 && numBytes > 0 )
    {
        numBytes -= 1 + (int) rng_below( (uint64_t) ( numBytes < 4 ? numBytes : 4 ) );        // truncated
    }
    else if ( kind == 2 && numBytes < 200 )
    {
        bytes[numBytes++] = (uint8_t) rng_next();                                             // trailing data
    }
    else if ( kind == 3 && numBytes > 0 )
    {
        bytes[rng_below( (uint64_t) numBytes )] = 0xFF;                                       // an all ones byte
    }
    else if ( kind == 4 && numBytes > 0 )
    {
        bytes[rng_below( (uint64_t) numBytes )] = (uint8_t) rng_next();                       // a random byte
    }
    else if ( numBytes > 0 )
    {
        bytes[numBytes - 1] ^= 0x80;                                                          // the last bit written
    }
}

static void print_record( FILE * out, const char * name, const Program & program, const uint8_t * bytes, int numBytes,
                          bool accepted, const char * refusal, const Step * steps, int64_t consumed, bool canonical )
{
    fprintf( out, "operation sequence\nname %s\n", name );
    for ( int i = 0; i < program.numSteps; i++ )
        fprintf( out, "param step = %s\n", program.words[i] );

    char hex[MaxLine];
    bytes_to_hex( bytes, numBytes, hex, MaxLine );
    fprintf( out, "bytes %s\n", hex );

    if ( accepted )
    {
        fprintf( out, "expect value = " );
        for ( int i = 0; i < program.numSteps; i++ )
        {
            char rendered[MaxLine];
            if ( steps[i].kind == STEP_ALIGN || steps[i].kind == STEP_END )
                strcpy( rendered, "-" );
            else
                render_step_value( steps[i], rendered, MaxLine );
            fprintf( out, "%s%s", i == 0 ? "" : " | ", rendered );
        }
        fprintf( out, "\nconsumed %lld\n", (long long) consumed );
        if ( canonical )
            fprintf( out, "writer canonical\n" );
    }
    else
    {
        fprintf( out, "expect refused %s\n", refusal );
    }
    fprintf( out, "\n" );
}

int main( int argc, char ** argv )
{
    if ( argc != 4 )
    {
        printf( "usage: diffgen <seed> <count> <output file>\n" );
        return 1;
    }

    const unsigned long long seed = strtoull( argv[1], NULL, 10 );
    const int count = atoi( argv[2] );
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
    if ( rng_state == 0 )
        rng_state = 1;
    for ( int i = 0; i < 16; i++ )
        rng_next();

    FILE * out = fopen( argv[3], "w" );
    if ( !out )
    {
        printf( "diffgen: cannot open %s\n", argv[3] );
        return 1;
    }

    fprintf( out, "# serialize differential vectors, seed %llu, %d records.\n", seed, count );
    fprintf( out, "#\n" );
    fprintf( out, "# GENERATED by tools/diff/diffgen.cpp from the C++ reference's own output. This is not the\n" );
    fprintf( out, "# corpus and never joins it: the corpus is never generated from an implementation (STANDARD.md).\n" );
    fprintf( out, "# It is the nightly differential's input: every port's own conformance runner is held to what the\n" );
    fprintf( out, "# reference decides today, on sequences nobody wrote by hand.\n\n" );

    int written = 0;
    int accepted = 0;
    int refused = 0;
    int canonicalCount = 0;
    int attempts = 0;

    while ( written < count && attempts < count * 20 )
    {
        attempts++;

        Program program;
        generate_program( program );

        // 1. the writer: random in-range values, written by the reference
        Vector vector;
        vector_reset( vector, "diffgen" );
        strcpy( vector.operation, "sequence" );
        vector.numSteps = program.numSteps;
        for ( int i = 0; i < program.numSteps; i++ )
            strcpy( vector.steps[i], program.words[i] );

        static Step steps[MaxSteps];
        int numSteps = 0;
        if ( !build_steps( vector, steps, numSteps ) )
            continue;

        bool seeded = true;
        for ( int i = 0; i < numSteps; i++ )
        {
            if ( !seed_step_value( steps[i] ) )
                seeded = false;
        }
        if ( !seeded )
            continue;

        uint8_t scratch[MaxBytes + 64];
        memset( scratch, SlackFill, sizeof( scratch ) );
        const int64_t capacity = (int64_t) sizeof( scratch ) & ~(int64_t) 7;
        serialize::WriteStream writeStream( scratch, capacity );
        if ( !run_steps( writeStream, steps, numSteps ) )
            continue;
        writeStream.Flush();
        const int64_t writtenBytes = writeStream.GetBytesProcessed();
        if ( writtenBytes > 200 )
            continue;

        uint8_t bytes[MaxBytes];
        int numBytes = (int) writtenBytes;
        memcpy( bytes, writeStream.GetData(), (size_t) numBytes );

        // 2. damage, about half of them
        if ( rng_chance( 55 ) )
            damage( bytes, numBytes );

        // 3. the reference reader's verdict
        memcpy( vector.bytes, bytes, (size_t) numBytes );
        vector.numBytes = numBytes;

        static Step readSteps[MaxSteps];
        int numReadSteps = 0;
        if ( !build_steps( vector, readSteps, numReadSteps ) )
            continue;

        StreamBuffer buffer;
        stream_buffer_init( buffer, vector );
        serialize::ReadStream stream( buffer.data, buffer.bytes );
        const serialize::uint128_t sentinelBits = serialize::uint128_t( 0xCAFEF00DULL );
        const serialize::int128_t sentinelNumber = serialize::int128_t( -1234567 );
        for ( int i = 0; i < numReadSteps; i++ )
        {
            readSteps[i].bits = sentinelBits;
            readSteps[i].number = sentinelNumber;
            readSteps[i].boolean = true;
        }
        g_failedStep = NULL;
        int stoppedAt = -1;
        const bool ok = run_steps( stream, readSteps, numReadSteps, &stoppedAt );

        char name[MaxLine];
        snprintf( name, MaxLine, "diff-%llu-%d", seed, written );

        if ( ok )
        {
            const int64_t consumed = stream.GetBitsProcessed();

            // an accepted record is canonical when the reference writer re-emits exactly its bytes
            uint8_t again[MaxBytes + 64];
            memset( again, SlackFill, sizeof( again ) );
            serialize::WriteStream rewrite( again, capacity );
            static Step rewriteSteps[MaxSteps];
            memcpy( rewriteSteps, readSteps, sizeof( Step ) * (size_t) numReadSteps );
            bool canonical = run_steps( rewrite, rewriteSteps, numReadSteps );
            if ( canonical )
            {
                rewrite.Flush();
                canonical = rewrite.GetBytesProcessed() == numBytes && ( numBytes == 0 || memcmp( rewrite.GetData(), bytes, (size_t) numBytes ) == 0 );
            }

            print_record( out, name, program, bytes, numBytes, true, NULL, readSteps, consumed, canonical );
            accepted++;
            if ( canonical )
                canonicalCount++;
        }
        else
        {
            print_record( out, name, program, bytes, numBytes, false, serialize::refusal_name( stream.GetRefusal() ), readSteps, 0, false );
            refused++;
        }
        written++;
    }

    fclose( out );
    printf( "diffgen: seed %llu: %d records (%d accepted, %d of them canonical; %d refused) -> %s\n",
            seed, written, accepted, canonicalCount, refused, argv[3] );
    return written == count ? 0 : 1;
}

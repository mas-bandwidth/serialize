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
    Runs the shared conformance corpus through this library's reader, writer and measure.

    The corpus is the conformance/ directory: one file per covered operation, holding the
    accepted and refused vectors STANDARD.md's rules require. It is the conformance instrument every
    implementation in the family runs, and it is deliberately not generated from this code — a
    suite that regenerates its own expectations proves only that a port agrees with itself.

    Each vector states an operation, its parameters, the stream bytes, and either the value a
    conforming reader decodes together with the bits it consumes, or the word `refused`. An
    accepted vector must yield exactly that value and consume exactly that many bits; a refused
    vector must be refused, and must leave the caller's scalar destination unwritten, which is the
    obligation Reader Obligations states for every refusal. STANDARD.md leaves a caller-owned
    BUFFER unspecified after a refusal — bytes, string and wstring — so this runner checks the
    destination only for the scalar operations, exactly as far as the document reaches.

    A vector carrying `writer = canonical` additionally pins the bytes a conforming writer emits
    for its value: the runner writes the value back and requires the emission to be the vector's
    bytes exactly, flush included, which is where the trailing-bits writer obligation bites. A
    vector without the mark binds the reader only.

    A sequence vector carrying `measure_at_least` pins the floor a conforming measure may report,
    which STANDARD.md makes a bound rather than an equality.

    THE BUFFER CONTRACT. STANDARD.md, "Past-end memory is an implementation contract": the C++
    implementation loads 64-bit windows at byte granularity and therefore requires its caller to
    allocate at least 8 bytes beyond the data. Every stream this runner presents carries that
    slack, and the slack bytes are set to a non-zero pattern rather than zero, so a vector that
    only passes because uninterpreted bytes past the end read as zero fails here.

    A corpus file whose operation this runner cannot drive is a gap in the runner, not a pass:
    such a vector FAILS. So does a vector naming a parameter the runner does not understand, and
    a fixed point vector naming a Q format declaration the runner does not carry — the fixed
    point declaration is a compile time constant of its call site, so a runner supports a fixed
    set of them and must say so out loud rather than skip.

    A refused vector must be refused, must leave the caller's scalar destination unwritten, and
    must leave the stream TERMINAL. Terminality is checked by behavior rather than by an
    accessor, so the check ports to every implementation in the family: a further read on the
    same stream must also fail, consume no bits and write nothing.

    The vector files are named on the command line, and CMake discovers them: the glob is in
    CMakeLists.txt, with CONFIGURE_DEPENDS so a vendored file that no one named still runs.
    STANDARD.md, "The vector format", specifies the syntax.
*/

#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------
// vector file parsing

const int MaxLine = 1024;
const int MaxBytes = 256;
const int MaxSlack = 8;                 // the buffer contract: at least 8 bytes past the data
const uint8_t SlackFill = 0xA5;         // non-zero, so a read that strays past the end is visible
const int MaxParams = 8;
const int MaxSteps = 8;

enum ExpectKind
{
    EXPECT_REFUSED,
    EXPECT_VALUE,
    EXPECT_BITS                         // compared as a bit pattern, never as a value
};

struct Param
{
    char name[MaxLine];
    char value[MaxLine];
};

struct Vector
{
    char file[MaxLine];
    char operation[MaxLine];
    char name[MaxLine];
    char expect[MaxLine];
    Param params[MaxParams];
    int numParams;
    char steps[MaxSteps][MaxLine];
    int numSteps;
    uint8_t bytes[MaxBytes + MaxSlack];
    int numBytes;
    int64_t consumed;
    bool hasConsumed;
    int64_t measureAtLeast;
    bool hasMeasure;
    bool writerCanonical;
    ExpectKind expectKind;
};

static void vector_reset( Vector & vector, const char * file )
{
    memset( &vector, 0, sizeof( Vector ) );
    strncpy( vector.file, file, MaxLine - 1 );
    vector.expectKind = EXPECT_VALUE;
}

static bool vector_empty( const Vector & vector )
{
    return vector.operation[0] == '\0';
}

// strips surrounding whitespace, in place

static char * trim( char * text )
{
    while ( *text == ' ' || *text == '\t' )
    {
        text++;
    }
    char * end = text + strlen( text );
    while ( end > text && ( end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n' ) )
    {
        end--;
    }
    *end = '\0';
    return text;
}

static bool parse_hex_digit( char c, int & out )
{
    if ( c >= '0' && c <= '9' ) { out = c - '0'; return true; }
    if ( c >= 'a' && c <= 'f' ) { out = 10 + c - 'a'; return true; }
    if ( c >= 'A' && c <= 'F' ) { out = 10 + c - 'A'; return true; }
    return false;
}

// hexadecimal byte pairs, whitespace separated, into a byte array

static bool parse_hex_bytes( const char * text, uint8_t * out, int maxBytes, int & numBytes )
{
    numBytes = 0;
    while ( *text )
    {
        if ( *text == ' ' || *text == '\t' )
        {
            text++;
            continue;
        }
        int high = 0;
        int low = 0;
        if ( !parse_hex_digit( text[0], high ) || !parse_hex_digit( text[1], low ) )
        {
            return false;
        }
        if ( numBytes >= maxBytes )
        {
            return false;
        }
        out[numBytes++] = (uint8_t) ( high * 16 + low );
        text += 2;
    }
    return true;
}

/*
    Numbers in a vector are signed decimal or 0x hexadecimal, parsed to 128 bits, because a
    vector's value can be wider than any built in strtol and the corpus states wide bounds as
    hexadecimal where the decimal would be unreadable.
*/

static bool parse_number( const char * text, serialize::int128_t & out )
{
    bool negative = false;
    if ( *text == '-' ) { negative = true; text++; }
    else if ( *text == '+' ) { text++; }
    if ( *text == '\0' )
    {
        return false;
    }

    // The accumulation runs in the UNSIGNED domain and the sign is applied there too. The corpus
    // states 128 bit bounds at both extremes -- the full signed range's minimum, and the unsigned
    // maximum as a decimal -- and accumulating either of those in a signed type overflows, which
    // is undefined behaviour rather than the wrap the value needs. Unsigned arithmetic wraps by
    // definition, so the digits land where they should and the two's complement reading happens
    // once, at the end.
    serialize::uint128_t value = 0;
    if ( text[0] == '0' && ( text[1] == 'x' || text[1] == 'X' ) )
    {
        text += 2;
        if ( *text == '\0' )
        {
            return false;
        }
        for ( ; *text; text++ )
        {
            int digit = 0;
            if ( !parse_hex_digit( *text, digit ) )
            {
                return false;
            }
            value = value * serialize::uint128_t( 16 ) + serialize::uint128_t( digit );
        }
    }
    else
    {
        for ( ; *text; text++ )
        {
            if ( *text < '0' || *text > '9' )
            {
                return false;
            }
            value = value * serialize::uint128_t( 10 ) + serialize::uint128_t( *text - '0' );
        }
    }
    if ( negative )
    {
        value = serialize::uint128_t( 0 ) - value;
    }
    out = serialize::int128_t( value );
    return true;
}

// ---------------------------------------------------------------------------------------
// failure reporting

static int failures = 0;
static int checked = 0;
static int writerChecked = 0;
static int measureChecked = 0;

static void fail( const Vector & vector, const char * detail )
{
    printf( "  FAIL %s: %s [%s]\n", vector.name, detail, vector.file );
    failures++;
}

// Failure is terminal (STANDARD.md, Reader Obligations), and a refused vector is where that
// rule is testable: the stream is checked by behavior rather than by an accessor, so the same
// check ports to every implementation in the family. A further read must fail, consume no bits
// and leave its destination alone.

static void fail_unless_stream_is_terminal( const Vector & vector, serialize::ReadStream & stream )
{
    uint32_t after = 0xFFFFFFFF;
    const int64_t bitsBefore = stream.GetBitsProcessed();
    if ( stream.SerializeBits( after, 8 ) )
    {
        fail( vector, "the stream accepted a read after the refusal: failure is not terminal" );
        return;
    }
    if ( after != 0xFFFFFFFF )
    {
        fail( vector, "the read after the refusal wrote to its destination" );
        return;
    }
    if ( stream.GetBitsProcessed() != bitsBefore )
    {
        fail( vector, "the read after the refusal consumed bits" );
    }
}

// ---------------------------------------------------------------------------------------
// parameter access. A parameter the runner does not understand is a failure rather than a
// silent default: a vector whose declaration is not the one being exercised proves nothing.

static const char * param_string( const Vector & vector, const char * name )
{
    for ( int i = 0; i < vector.numParams; i++ )
    {
        if ( strcmp( vector.params[i].name, name ) == 0 )
        {
            return vector.params[i].value;
        }
    }
    return NULL;
}

static bool param_number( const Vector & vector, const char * name, serialize::int128_t & out )
{
    const char * text = param_string( vector, name );
    if ( !text )
    {
        return false;
    }
    return parse_number( text, out );
}

static bool param_int( const Vector & vector, const char * name, int64_t & out )
{
    serialize::int128_t value = 0;
    if ( !param_number( vector, name, value ) )
    {
        return false;
    }
    out = (int64_t) value;
    return true;
}

static bool param_float( const Vector & vector, const char * name, float & out )
{
    const char * text = param_string( vector, name );
    if ( !text )
    {
        return false;
    }
    char * end = NULL;
    out = (float) strtod( text, &end );
    return end != text && *end == '\0';
}

// ---------------------------------------------------------------------------------------
// stream construction. Every stream the runner hands the reader carries the slack the buffer
// contract requires, filled with a non-zero pattern so that a decode which depends on memory
// past the end cannot pass by reading zeros.

struct StreamBuffer
{
    uint8_t data[MaxBytes + MaxSlack];
    int64_t bytes;
};

static void stream_buffer_init( StreamBuffer & buffer, const Vector & vector )
{
    memset( buffer.data, SlackFill, sizeof( buffer.data ) );
    memcpy( buffer.data, vector.bytes, (size_t) vector.numBytes );
    buffer.bytes = vector.numBytes;
}

// ---------------------------------------------------------------------------------------
// the operations under test, called through the public macros so the vectors exercise the
// surface a consumer uses rather than the stream methods underneath it

template <typename Stream> bool op_bits( Stream & stream, uint64_t & value, int bits )
{
    serialize_bits( stream, value, bits );
    return true;
}

template <typename Stream> bool op_bool( Stream & stream, bool & value )
{
    serialize_bool( stream, value );
    return true;
}

template <typename Stream> bool op_uint128( Stream & stream, serialize::uint128_t & value )
{
    serialize_uint128( stream, value );
    return true;
}

template <typename Stream> bool op_align( Stream & stream )
{
    serialize_align( stream );
    return true;
}

template <typename Stream> bool op_int( Stream & stream, int32_t & value, int32_t min, int32_t max )
{
    serialize_int( stream, value, min, max );
    return true;
}

template <typename Stream> bool op_int64( Stream & stream, int64_t & value, int64_t min, int64_t max )
{
    serialize_int64( stream, value, min, max );
    return true;
}

template <typename Stream> bool op_int128( Stream & stream, serialize::int128_t & value, serialize::int128_t min, serialize::int128_t max )
{
    serialize_int128( stream, value, min, max );
    return true;
}

template <typename Stream> bool op_int_relative( Stream & stream, int32_t previous, int32_t & current )
{
    serialize_int_relative( stream, previous, current );
    return true;
}

template <typename Stream> bool op_float( Stream & stream, float & value )
{
    serialize_float( stream, value );
    return true;
}

template <typename Stream> bool op_double( Stream & stream, double & value )
{
    serialize_double( stream, value );
    return true;
}

template <typename Stream> bool op_compressed_float( Stream & stream, float & value, float min, float max, float res )
{
    serialize_compressed_float( stream, value, min, max, res );
    return true;
}

template <typename Stream> bool op_bytes( Stream & stream, uint8_t * data, int count )
{
    serialize_bytes( stream, data, count );
    return true;
}

template <typename Stream> bool op_string( Stream & stream, char * string, int bufferSize )
{
    serialize_string( stream, string, bufferSize );
    return true;
}

template <typename Stream> bool op_wstring( Stream & stream, wchar_t * string, int bufferSize )
{
    serialize_wstring( stream, string, bufferSize );
    return true;
}

// ---------------------------------------------------------------------------------------
// fixed point. Every parameter is a compile time constant of the call site, so the runner
// carries a table of declarations and a vector naming one that is not in it FAILS rather than
// passes. Adding a declaration to the corpus means adding a row here.

template <int I, int F, int64_t Lo, int64_t Hi, typename Storage, typename Stream>
bool op_fixed( Stream & stream, Storage & value )
{
    serialize_fixed( stream, value, I, F, Lo, Hi );
    return true;
}

enum FixedDeclaration
{
    FIXED_NONE,
    FIXED_Q8_8_M100_100,
    FIXED_Q32_0_0_7,
    FIXED_Q32_0_0_5,
    FIXED_Q16_16_7_7,
    FIXED_Q16_16_M32768_32767,
    FIXED_Q48_16_0_131072,
    FIXED_Q112_16_M9_M9,
    FIXED_Q112_16_0_2P58,
    FIXED_Q112_16_M2P57_2P57,
    FIXED_Q64_64_0_0,
    FIXED_Q64_64_3_3,
    FIXED_Q64_64_0_INT64MAX
};

static FixedDeclaration fixed_declaration( int64_t integerBits, int64_t fractionBits, serialize::int128_t min, serialize::int128_t max )
{
    const serialize::int128_t two58 = serialize::int128_t( 288230376151711744LL );
    const serialize::int128_t two57 = serialize::int128_t( 144115188075855872LL );
    if ( integerBits == 8   && fractionBits == 8  && min == -100 && max == 100 )     return FIXED_Q8_8_M100_100;
    if ( integerBits == 32  && fractionBits == 0  && min == 0    && max == 7 )       return FIXED_Q32_0_0_7;
    if ( integerBits == 32  && fractionBits == 0  && min == 0    && max == 5 )       return FIXED_Q32_0_0_5;
    if ( integerBits == 16  && fractionBits == 16 && min == 7    && max == 7 )       return FIXED_Q16_16_7_7;
    if ( integerBits == 16  && fractionBits == 16 && min == -32768 && max == 32767 ) return FIXED_Q16_16_M32768_32767;
    if ( integerBits == 48  && fractionBits == 16 && min == 0    && max == 131072 )  return FIXED_Q48_16_0_131072;
    if ( integerBits == 112 && fractionBits == 16 && min == -9   && max == -9 )      return FIXED_Q112_16_M9_M9;
    if ( integerBits == 112 && fractionBits == 16 && min == 0    && max == two58 )   return FIXED_Q112_16_0_2P58;
    if ( integerBits == 112 && fractionBits == 16 && min == -two57 && max == two57 ) return FIXED_Q112_16_M2P57_2P57;
    if ( integerBits == 64  && fractionBits == 64 && min == 0    && max == 0 )       return FIXED_Q64_64_0_0;
    if ( integerBits == 64  && fractionBits == 64 && min == 3    && max == 3 )       return FIXED_Q64_64_3_3;
    if ( integerBits == 64  && fractionBits == 64 && min == 0    && max == serialize::int128_t( 9223372036854775807LL ) ) return FIXED_Q64_64_0_INT64MAX;
    return FIXED_NONE;
}

// The raw value travels through the runner as a 128 bit integer whatever the declaration's
// storage width, so one code path drives every row of the table above.

template <typename Stream>
static bool run_fixed_declaration( Stream & stream, FixedDeclaration declaration, serialize::int128_t & raw )
{
    switch ( declaration )
    {
        case FIXED_Q8_8_M100_100:
        {
            int16_t value = (int16_t) (int64_t) raw;
            if ( !op_fixed<8, 8, -100, 100, int16_t>( stream, value ) ) return false;
            raw = serialize::int128_t( (int64_t) value );
            return true;
        }
        case FIXED_Q32_0_0_7:
        {
            int32_t value = (int32_t) (int64_t) raw;
            if ( !op_fixed<32, 0, 0, 7, int32_t>( stream, value ) ) return false;
            raw = serialize::int128_t( (int64_t) value );
            return true;
        }
        case FIXED_Q32_0_0_5:
        {
            int32_t value = (int32_t) (int64_t) raw;
            if ( !op_fixed<32, 0, 0, 5, int32_t>( stream, value ) ) return false;
            raw = serialize::int128_t( (int64_t) value );
            return true;
        }
        case FIXED_Q16_16_7_7:
        {
            int32_t value = (int32_t) (int64_t) raw;
            if ( !op_fixed<16, 16, 7, 7, int32_t>( stream, value ) ) return false;
            raw = serialize::int128_t( (int64_t) value );
            return true;
        }
        case FIXED_Q16_16_M32768_32767:
        {
            int32_t value = (int32_t) (int64_t) raw;
            if ( !op_fixed<16, 16, -32768, 32767, int32_t>( stream, value ) ) return false;
            raw = serialize::int128_t( (int64_t) value );
            return true;
        }
        case FIXED_Q48_16_0_131072:
        {
            int64_t value = (int64_t) raw;
            if ( !op_fixed<48, 16, 0, 131072, int64_t>( stream, value ) ) return false;
            raw = serialize::int128_t( value );
            return true;
        }
        case FIXED_Q112_16_M9_M9:
        {
            serialize::int128_t value = raw;
            if ( !op_fixed<112, 16, -9, -9, serialize::int128_t>( stream, value ) ) return false;
            raw = value;
            return true;
        }
        case FIXED_Q112_16_0_2P58:
        {
            serialize::int128_t value = raw;
            if ( !op_fixed<112, 16, 0, 288230376151711744LL, serialize::int128_t>( stream, value ) ) return false;
            raw = value;
            return true;
        }
        case FIXED_Q112_16_M2P57_2P57:
        {
            serialize::int128_t value = raw;
            if ( !op_fixed<112, 16, -144115188075855872LL, 144115188075855872LL, serialize::int128_t>( stream, value ) ) return false;
            raw = value;
            return true;
        }
        case FIXED_Q64_64_0_0:
        {
            serialize::int128_t value = raw;
            if ( !op_fixed<64, 64, 0, 0, serialize::int128_t>( stream, value ) ) return false;
            raw = value;
            return true;
        }
        case FIXED_Q64_64_3_3:
        {
            serialize::int128_t value = raw;
            if ( !op_fixed<64, 64, 3, 3, serialize::int128_t>( stream, value ) ) return false;
            raw = value;
            return true;
        }
        case FIXED_Q64_64_0_INT64MAX:
        {
            serialize::int128_t value = raw;
            if ( !op_fixed<64, 64, 0, 9223372036854775807LL, serialize::int128_t>( stream, value ) ) return false;
            raw = value;
            return true;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------
// the step machine, which drives both the single operation files and the sequence files. A
// single operation vector is a one step sequence whose step is built from the record's own
// parameters, so there is exactly one execution path and the sequence files cannot drift away
// from the operation files.

enum StepKind
{
    STEP_BITS,
    STEP_BOOL,
    STEP_UINT128,
    STEP_ALIGN,
    STEP_INT,
    STEP_INT64,
    STEP_INT128,
    STEP_INT_RELATIVE,
    STEP_FLOAT,
    STEP_DOUBLE,
    STEP_COMPRESSED_FLOAT,
    STEP_BYTES,
    STEP_STRING,
    STEP_WSTRING,
    STEP_FIXED,
    STEP_OBJECT                             // opens a nested object over the steps that follow
};

struct Step
{
    StepKind kind;
    int64_t width;                          // bits, count, buffer_size or preceding_bits
    serialize::int128_t min;
    serialize::int128_t max;
    float fmin;
    float fmax;
    float fres;
    FixedDeclaration fixedDeclaration;
    int32_t previous;

    // outputs
    serialize::uint128_t bits;              // the decoded value, as a bit pattern where that is what is pinned
    serialize::int128_t number;
    bool boolean;
    uint8_t buffer[MaxBytes + 1];
    int bufferBytes;
    wchar_t wbuffer[MaxBytes + 1];
    int wbufferUnits;
};

/*
    Runs one step against any stream. The destination sentinel rule lives at the call site: for
    the scalar operations the caller seeds the destination and checks it afterwards, and for the
    caller-owned buffers it does not, because STANDARD.md leaves those unspecified after a refusal.
*/

template <typename Stream> static bool run_step( Stream & stream, Step & step )
{
    switch ( step.kind )
    {
        case STEP_BITS:
        {
            uint64_t value = uint64_t( step.bits );
            const bool ok = op_bits( stream, value, (int) step.width );
            step.bits = serialize::uint128_t( value );
            return ok;
        }
        case STEP_BOOL:
            return op_bool( stream, step.boolean );

        case STEP_UINT128:
        {
            serialize::uint128_t value = step.bits;
            const bool ok = op_uint128( stream, value );
            step.bits = value;
            return ok;
        }
        case STEP_ALIGN:
            return op_align( stream );

        case STEP_INT:
        {
            int32_t value = (int32_t) (int64_t) step.number;
            const bool ok = op_int( stream, value, (int32_t) (int64_t) step.min, (int32_t) (int64_t) step.max );
            step.number = serialize::int128_t( (int64_t) value );
            return ok;
        }
        case STEP_INT64:
        {
            int64_t value = (int64_t) step.number;
            const bool ok = op_int64( stream, value, (int64_t) step.min, (int64_t) step.max );
            step.number = serialize::int128_t( value );
            return ok;
        }
        case STEP_INT128:
        {
            serialize::int128_t value = step.number;
            const bool ok = op_int128( stream, value, step.min, step.max );
            step.number = value;
            return ok;
        }
        case STEP_INT_RELATIVE:
        {
            int32_t value = (int32_t) (int64_t) step.number;
            const bool ok = op_int_relative( stream, step.previous, value );
            step.number = serialize::int128_t( (int64_t) value );
            return ok;
        }
        case STEP_FLOAT:
        {
            uint32_t pattern = (uint32_t) uint64_t( step.bits );
            float value;
            memcpy( &value, &pattern, 4 );
            const bool ok = op_float( stream, value );
            memcpy( &pattern, &value, 4 );
            step.bits = serialize::uint128_t( uint64_t( pattern ) );
            return ok;
        }
        case STEP_DOUBLE:
        {
            uint64_t pattern = uint64_t( step.bits );
            double value;
            memcpy( &value, &pattern, 8 );
            const bool ok = op_double( stream, value );
            memcpy( &pattern, &value, 8 );
            step.bits = serialize::uint128_t( pattern );
            return ok;
        }
        case STEP_COMPRESSED_FLOAT:
        {
            uint32_t pattern = (uint32_t) uint64_t( step.bits );
            float value;
            memcpy( &value, &pattern, 4 );
            const bool ok = op_compressed_float( stream, value, step.fmin, step.fmax, step.fres );
            memcpy( &pattern, &value, 4 );
            step.bits = serialize::uint128_t( uint64_t( pattern ) );
            return ok;
        }
        case STEP_BYTES:
            return op_bytes( stream, step.buffer, (int) step.width );

        case STEP_STRING:
            return op_string( stream, (char*) step.buffer, (int) step.width );

        case STEP_WSTRING:
            return op_wstring( stream, step.wbuffer, (int) step.width );

        case STEP_FIXED:
            return run_fixed_declaration( stream, step.fixedDeclaration, step.number );

        case STEP_OBJECT:
            // nesting is driven by run_steps, which owns the step range an object wraps; a bare
            // object step reaching here is a runner bug rather than a vector one
            return false;
    }
    return false;
}

// the field of a step that holds the value, which is the destination
// "a refused primitive read must leave its destination unwritten" reaches

static bool step_value_is_a_bit_pattern( StepKind kind )
{
    return kind == STEP_BITS || kind == STEP_UINT128 || kind == STEP_FLOAT || kind == STEP_DOUBLE || kind == STEP_COMPRESSED_FLOAT;
}

static bool step_value_is_a_number( StepKind kind )
{
    return kind == STEP_INT || kind == STEP_INT64 || kind == STEP_INT128 || kind == STEP_INT_RELATIVE || kind == STEP_FIXED;
}

/*
    STANDARD.md, "object": serialize_object invokes the object's own serialize function inline
    and contributes NO BYTES OF ITS OWN — it is composition, not an encoding, with no framing,
    length prefix or alignment inserted around it. A step spelled `object <n>` wraps the next n
    steps in a nested object, so a vector can state the same operations twice, once nested and
    once flat, and require identical bytes.

    The nested object is driven through the public serialize_object macro rather than by calling
    the steps directly, so what the vectors exercise is the composition the macro performs.
*/

template <typename Stream> static bool run_steps( Stream & stream, Step * steps, int count, int * stoppedAt = NULL );

static Step * g_failedStep = NULL;      // the step a run stopped on, for the destination check

struct NestedObject
{
    Step * steps;
    int count;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        return run_steps( stream, steps, count );
    }
};

template <typename Stream> static bool run_nested_object( Stream & stream, NestedObject & object )
{
    serialize_object( stream, object );
    return true;
}

// advances past the steps a nested object owns, so a top level walk sees one step per object

static int step_span( const Step * steps, int index )
{
    if ( steps[index].kind == STEP_OBJECT )
    {
        return 1 + (int) steps[index].width;
    }
    return 1;
}

template <typename Stream> static bool run_steps( Stream & stream, Step * steps, int count, int * stoppedAt )
{
    for ( int i = 0; i < count; i += step_span( steps, i ) )
    {
        if ( steps[i].kind == STEP_OBJECT )
        {
            NestedObject object;
            object.steps = steps + i + 1;
            object.count = (int) steps[i].width;
            if ( !run_nested_object( stream, object ) )
            {
                if ( stoppedAt ) *stoppedAt = i;
                return false;
            }
            continue;
        }
        if ( !run_step( stream, steps[i] ) )
        {
            g_failedStep = &steps[i];
            if ( stoppedAt ) *stoppedAt = i;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// building steps

static bool step_from_words( const Vector & vector, const char * text, Step & step )
{
    memset( &step, 0, sizeof( Step ) );
    step.fixedDeclaration = FIXED_NONE;

    char work[MaxLine];
    strncpy( work, text, MaxLine - 1 );
    work[MaxLine - 1] = '\0';

    const char * words[8];
    int numWords = 0;
    char * cursor = work;
    while ( *cursor && numWords < 8 )
    {
        while ( *cursor == ' ' || *cursor == '\t' ) cursor++;
        if ( *cursor == '\0' ) break;
        words[numWords++] = cursor;
        while ( *cursor && *cursor != ' ' && *cursor != '\t' ) cursor++;
        if ( *cursor ) *cursor++ = '\0';
    }
    if ( numWords == 0 )
    {
        return false;
    }

    serialize::int128_t a = 0;
    serialize::int128_t b = 0;

    if ( strcmp( words[0], "bits" ) == 0 && numWords == 2 && parse_number( words[1], a ) )
    {
        step.kind = STEP_BITS;
        step.width = (int64_t) a;
        return true;
    }
    if ( strcmp( words[0], "bool" ) == 0 && numWords == 1 )
    {
        step.kind = STEP_BOOL;
        return true;
    }
    if ( strcmp( words[0], "object" ) == 0 && numWords == 2 && parse_number( words[1], a ) )
    {
        step.kind = STEP_OBJECT;
        step.width = (int64_t) a;
        return true;
    }
    if ( strcmp( words[0], "align" ) == 0 && numWords == 1 )
    {
        step.kind = STEP_ALIGN;
        return true;
    }
    if ( strcmp( words[0], "float" ) == 0 && numWords == 1 )
    {
        step.kind = STEP_FLOAT;
        return true;
    }
    if ( strcmp( words[0], "bytes" ) == 0 && numWords == 2 && parse_number( words[1], a ) )
    {
        step.kind = STEP_BYTES;
        step.width = (int64_t) a;
        return true;
    }
    if ( strcmp( words[0], "string" ) == 0 && numWords == 2 && parse_number( words[1], a ) )
    {
        step.kind = STEP_STRING;
        step.width = (int64_t) a;
        return true;
    }
    if ( strcmp( words[0], "wstring" ) == 0 && numWords == 2 && parse_number( words[1], a ) )
    {
        step.kind = STEP_WSTRING;
        step.width = (int64_t) a;
        return true;
    }
    if ( strcmp( words[0], "int" ) == 0 && numWords == 3 && parse_number( words[1], a ) && parse_number( words[2], b ) )
    {
        step.kind = STEP_INT;
        step.min = a;
        step.max = b;
        return true;
    }
    if ( strcmp( words[0], "fixed" ) == 0 && numWords == 5 )
    {
        serialize::int128_t ib = 0, fb = 0, lo = 0, hi = 0;
        if ( !parse_number( words[1], ib ) || !parse_number( words[2], fb ) || !parse_number( words[3], lo ) || !parse_number( words[4], hi ) )
        {
            return false;
        }
        step.kind = STEP_FIXED;
        step.fixedDeclaration = fixed_declaration( (int64_t) ib, (int64_t) fb, lo, hi );
        if ( step.fixedDeclaration == FIXED_NONE )
        {
            printf( "  FAIL %s: no runner for this fixed point declaration [%s]\n", vector.name, vector.file );
            failures++;
            return false;
        }
        return true;
    }
    return false;
}

/*
    Builds the step list for a vector. A single operation vector becomes a one or two step
    sequence: the operations whose interesting behaviour only exists at a non-zero bit index
    take a `preceding_bits` parameter, which becomes a leading bits step.
*/

static bool build_steps( const Vector & vector, Step * steps, int & numSteps )
{
    numSteps = 0;

    if ( strcmp( vector.operation, "sequence" ) == 0 )
    {
        for ( int i = 0; i < vector.numSteps; i++ )
        {
            if ( numSteps >= MaxSteps ) return false;
            if ( !step_from_words( vector, vector.steps[i], steps[numSteps] ) ) return false;
            numSteps++;
        }
        return numSteps > 0;
    }

    int64_t precedingBits = 0;
    if ( param_int( vector, "preceding_bits", precedingBits ) && precedingBits > 0 )
    {
        memset( &steps[numSteps], 0, sizeof( Step ) );
        steps[numSteps].kind = STEP_BITS;
        steps[numSteps].width = precedingBits;
        numSteps++;
    }

    Step & step = steps[numSteps];
    memset( &step, 0, sizeof( Step ) );
    step.fixedDeclaration = FIXED_NONE;

    int64_t width = 0;
    serialize::int128_t min = 0;
    serialize::int128_t max = 0;

    if ( strcmp( vector.operation, "bits" ) == 0 )
    {
        if ( !param_int( vector, "bits", width ) ) return false;
        step.kind = STEP_BITS;
        step.width = width;
    }
    else if ( strcmp( vector.operation, "bool" ) == 0 )
    {
        step.kind = STEP_BOOL;
    }
    else if ( strcmp( vector.operation, "uint128" ) == 0 )
    {
        step.kind = STEP_UINT128;
    }
    else if ( strcmp( vector.operation, "align" ) == 0 )
    {
        step.kind = STEP_ALIGN;
    }
    else if ( strcmp( vector.operation, "int" ) == 0 )
    {
        if ( !param_number( vector, "min", min ) || !param_number( vector, "max", max ) ) return false;
        step.kind = STEP_INT;
        step.min = min;
        step.max = max;
    }
    else if ( strcmp( vector.operation, "int64" ) == 0 )
    {
        if ( !param_number( vector, "min", min ) || !param_number( vector, "max", max ) ) return false;
        step.kind = STEP_INT64;
        step.min = min;
        step.max = max;
    }
    else if ( strcmp( vector.operation, "int128" ) == 0 )
    {
        if ( !param_number( vector, "min", min ) || !param_number( vector, "max", max ) ) return false;
        step.kind = STEP_INT128;
        step.min = min;
        step.max = max;
    }
    else if ( strcmp( vector.operation, "int_relative" ) == 0 )
    {
        int64_t previous = 0;
        if ( !param_int( vector, "previous", previous ) ) return false;
        step.kind = STEP_INT_RELATIVE;
        step.previous = (int32_t) previous;
    }
    else if ( strcmp( vector.operation, "float" ) == 0 )
    {
        step.kind = STEP_FLOAT;
    }
    else if ( strcmp( vector.operation, "double" ) == 0 )
    {
        step.kind = STEP_DOUBLE;
    }
    else if ( strcmp( vector.operation, "compressed_float" ) == 0 )
    {
        if ( !param_float( vector, "min", step.fmin ) || !param_float( vector, "max", step.fmax ) || !param_float( vector, "res", step.fres ) ) return false;
        step.kind = STEP_COMPRESSED_FLOAT;
    }
    else if ( strcmp( vector.operation, "bytes" ) == 0 )
    {
        if ( !param_int( vector, "count", width ) ) return false;
        step.kind = STEP_BYTES;
        step.width = width;
    }
    else if ( strcmp( vector.operation, "string" ) == 0 )
    {
        if ( !param_int( vector, "buffer_size", width ) ) return false;
        step.kind = STEP_STRING;
        step.width = width;
    }
    else if ( strcmp( vector.operation, "wstring" ) == 0 )
    {
        if ( !param_int( vector, "buffer_size", width ) ) return false;
        step.kind = STEP_WSTRING;
        step.width = width;
    }
    else if ( strcmp( vector.operation, "fixed" ) == 0 )
    {
        int64_t integerBits = 0;
        int64_t fractionBits = 0;
        if ( !param_int( vector, "integer_bits", integerBits ) || !param_int( vector, "fraction_bits", fractionBits ) ) return false;
        if ( !param_number( vector, "min", min ) || !param_number( vector, "max", max ) ) return false;
        step.kind = STEP_FIXED;
        step.fixedDeclaration = fixed_declaration( integerBits, fractionBits, min, max );
        if ( step.fixedDeclaration == FIXED_NONE )
        {
            printf( "  FAIL %s: no runner for this fixed point declaration [%s]\n", vector.name, vector.file );
            failures++;
            return false;
        }
    }
    else
    {
        return false;
    }

    numSteps++;
    return true;
}

// ---------------------------------------------------------------------------------------
// expectations

static void bytes_to_hex( const uint8_t * data, int count, char * out, int outSize )
{
    int written = 0;
    for ( int i = 0; i < count && written + 3 < outSize; i++ )
    {
        const char * digits = "0123456789ABCDEF";
        if ( written > 0 ) out[written++] = ' ';
        out[written++] = digits[( data[i] >> 4 ) & 0xF];
        out[written++] = digits[data[i] & 0xF];
    }
    out[written] = '\0';
}

static void units_to_hex( const wchar_t * units, int count, char * out, int outSize )
{
    static const char * digits = "0123456789ABCDEF";
    int written = 0;
    for ( int i = 0; i < count; i++ )
    {
        // STANDARD.md, "wstring": each 32 bit group carries one UTF-16 CODE UNIT, and a 4 byte
        // wchar_t platform "converts at the boundary — splits astral code points into surrogate
        // pairs on write, recombines on read". The corpus states units, so a code point that
        // came back recombined is split again here and both platforms compare the same text.
        unsigned int pair[2];
        int numUnits = 1;
        const unsigned int codePoint = (unsigned int) units[i];
        if ( codePoint > 0xFFFF )
        {
            const unsigned int offset = codePoint - 0x10000;
            pair[0] = 0xD800 + ( offset >> 10 );
            pair[1] = 0xDC00 + ( offset & 0x3FF );
            numUnits = 2;
        }
        else
        {
            pair[0] = codePoint;
        }
        for ( int u = 0; u < numUnits; u++ )
        {
            if ( written + 6 >= outSize ) break;
            if ( written > 0 ) out[written++] = ' ';
            out[written++] = digits[( pair[u] >> 12 ) & 0xF];
            out[written++] = digits[( pair[u] >> 8 ) & 0xF];
            out[written++] = digits[( pair[u] >> 4 ) & 0xF];
            out[written++] = digits[pair[u] & 0xF];
        }
    }
    out[written] = '\0';
}

/*
    A parameter this runner does not understand is a FAILURE and not a silent default: a vector
    whose declaration is not the one being exercised proves nothing, and a corpus that grows a
    parameter must grow a runner to read it. Each operation states the parameters it consumes.
*/

static bool operation_takes_param( const char * operation, const char * name )
{
    if ( strcmp( name, "step" ) == 0 )              return strcmp( operation, "sequence" ) == 0;
    if ( strcmp( name, "preceding_bits" ) == 0 )    return strcmp( operation, "align" ) == 0 || strcmp( operation, "bytes" ) == 0;
    if ( strcmp( name, "bits" ) == 0 )              return strcmp( operation, "bits" ) == 0;
    if ( strcmp( name, "count" ) == 0 )             return strcmp( operation, "bytes" ) == 0;
    if ( strcmp( name, "buffer_size" ) == 0 )       return strcmp( operation, "string" ) == 0 || strcmp( operation, "wstring" ) == 0;
    if ( strcmp( name, "previous" ) == 0 )          return strcmp( operation, "int_relative" ) == 0;
    if ( strcmp( name, "res" ) == 0 )               return strcmp( operation, "compressed_float" ) == 0;
    if ( strcmp( name, "integer_bits" ) == 0 || strcmp( name, "fraction_bits" ) == 0 )
                                                    return strcmp( operation, "fixed" ) == 0;
    if ( strcmp( name, "min" ) == 0 || strcmp( name, "max" ) == 0 )
    {
        return strcmp( operation, "int" ) == 0 || strcmp( operation, "int64" ) == 0 || strcmp( operation, "int128" ) == 0
            || strcmp( operation, "fixed" ) == 0 || strcmp( operation, "compressed_float" ) == 0;
    }
    return false;
}

/*
    Renders a step's decoded value for a failure message, and decides whether it matches the
    corpus.

    Numeric values — every integer width, and the float, double and compressed_float bit
    patterns — are compared as 128 bit PATTERNS: the step's value is taken as its two's
    complement 128 bit form and the corpus expectation is parsed to the same form, so a
    hexadecimal expectation and its decimal twin are one expectation, and NOTHING here goes
    through a float. That last part is the document's requirement, not a convenience:
    STANDARD.md says conformance vectors for float and double "must compare BIT PATTERNS, NOT
    VALUES: NaN compares unequal to itself, -0.0 == 0.0, and a tolerance comparison cannot see a
    quieted signaling bit, so a value-space comparison here proves nothing."

    The remaining kinds have textual spellings the corpus states directly: `true` or `false`,
    hexadecimal byte pairs for bytes and string payloads, and four digit code units for wstring.
*/

static void render_hex128( serialize::uint128_t bits, char * out )
{
    static const char * digits = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for ( int i = 0; i < 32; i++ )
    {
        const int shift = 124 - i * 4;
        const uint64_t nibble = uint64_t( bits >> shift ) & 0xF;
        out[2 + i] = digits[nibble];
    }
    out[34] = '\0';
}

static bool step_pattern( const Step & step, serialize::uint128_t & out )
{
    if ( step_value_is_a_bit_pattern( step.kind ) )
    {
        out = step.bits;
        return true;
    }
    if ( step_value_is_a_number( step.kind ) )
    {
        out = serialize::uint128_t( step.number );
        return true;
    }
    return false;
}

static void render_step_value( const Step & step, char * out, int outSize )
{
    serialize::uint128_t pattern = 0;
    if ( step_pattern( step, pattern ) )
    {
        render_hex128( pattern, out );
        return;
    }

    switch ( step.kind )
    {
        case STEP_OBJECT:
        case STEP_ALIGN:
            // neither has a value of its own; for align the corpus states the padding it
            // consumed, which a conforming read always finds zero
            strncpy( out, "0", (size_t) outSize - 1 );
            out[outSize - 1] = '\0';
            return;

        case STEP_BOOL:
            strncpy( out, step.boolean ? "true" : "false", (size_t) outSize - 1 );
            out[outSize - 1] = '\0';
            return;

        case STEP_BYTES:
            bytes_to_hex( step.buffer, (int) step.width, out, outSize );
            return;

        case STEP_STRING:
            bytes_to_hex( step.buffer, (int) strlen( (const char*) step.buffer ), out, outSize );
            return;

        case STEP_WSTRING:
        {
            int units = 0;
            while ( step.wbuffer[units] != 0 ) units++;
            units_to_hex( step.wbuffer, units, out, outSize );
            return;
        }
        default:
            strncpy( out, "?", (size_t) outSize - 1 );
            out[outSize - 1] = '\0';
            return;
    }
}

static bool expectation_matches( const Step & step, const char * expected )
{
    serialize::uint128_t pattern = 0;
    if ( step_pattern( step, pattern ) )
    {
        serialize::int128_t wanted = 0;
        if ( !parse_number( expected, wanted ) )
        {
            return false;
        }
        return pattern == serialize::uint128_t( wanted );
    }

    char rendered[MaxLine];
    render_step_value( step, rendered, MaxLine );
    return strcmp( rendered, expected ) == 0;
}

// splits an expect list into per step entries, on " | "

static int split_expect( const char * text, char entries[MaxSteps][MaxLine] )
{
    int count = 0;
    const char * cursor = text;
    while ( count < MaxSteps )
    {
        const char * bar = strstr( cursor, "|" );
        char piece[MaxLine];
        if ( bar )
        {
            const size_t length = (size_t) ( bar - cursor );
            memcpy( piece, cursor, length < MaxLine - 1 ? length : MaxLine - 1 );
            piece[length < MaxLine - 1 ? length : MaxLine - 1] = '\0';
        }
        else
        {
            strncpy( piece, cursor, MaxLine - 1 );
            piece[MaxLine - 1] = '\0';
        }
        char * trimmed = trim( piece );
        strncpy( entries[count], trimmed, MaxLine - 1 );
        entries[count][MaxLine - 1] = '\0';
        count++;
        if ( !bar ) break;
        cursor = bar + 1;
    }
    return count;
}

// ---------------------------------------------------------------------------------------
// running one vector

static void run_reader( const Vector & vector, Step * steps, int numSteps )
{
    StreamBuffer buffer;
    stream_buffer_init( buffer, vector );

    serialize::ReadStream stream( buffer.data, buffer.bytes );

    // the sentinel is only meaningful for the scalar operations, which is exactly where
    // STANDARD.md's "a refused primitive read must leave its destination unwritten" reaches
    // the sentinels must survive the narrowing this runner performs on the way to each
    // operation's own width — 32 bits for float and for the ranged int — or a destination the
    // library correctly left alone still reads as written
    const serialize::uint128_t sentinelBits = serialize::uint128_t( 0xCAFEF00DULL );
    const serialize::int128_t sentinelNumber = serialize::int128_t( -1234567 );
    for ( int i = 0; i < numSteps; i++ )
    {
        steps[i].bits = sentinelBits;
        steps[i].number = sentinelNumber;
        steps[i].boolean = true;      // a refused bool read must leave this alone
    }

    g_failedStep = NULL;
    int stoppedAt = -1;
    const bool accepted = run_steps( stream, steps, numSteps, &stoppedAt );

    if ( vector.expectKind == EXPECT_REFUSED )
    {
        if ( accepted )
        {
            fail( vector, "the read succeeded, the corpus requires refusal" );
            return;
        }

        // STANDARD.md, "A refused primitive read must leave its destination unwritten". The rule
        // reaches the scalars only: a read into a caller-owned buffer — bytes, string and
        // wstring — leaves that buffer's contents unspecified after a refusal, and the document
        // says so in as many words, so those kinds are not checked here.
        if ( g_failedStep )
        {
            const Step & step = *g_failedStep;
            if ( ( step_value_is_a_bit_pattern( step.kind ) && !( step.bits == sentinelBits ) ) ||
                 ( step_value_is_a_number( step.kind ) && !( step.number == sentinelNumber ) ) ||
                 ( step.kind == STEP_BOOL && step.boolean != true ) )
            {
                fail( vector, "the refused read wrote to the destination" );
                return;
            }
        }

        // Failure is terminal, and a sequence states its own successors: every step after the
        // failing one must fail too, however many readable bits the stream still holds. The
        // vectors are built so a reader without the latch passes the successor, and one of them
        // makes the successor a DEGENERATE RANGE — a read that consumes no bits and would
        // otherwise always succeed, which is the case an implementation checking the length
        // before the width gets wrong.
        for ( int i = stoppedAt + step_span( steps, stoppedAt ); i < numSteps; i += step_span( steps, i ) )
        {
            if ( run_steps( stream, steps + i, step_span( steps, i ) ) )
            {
                printf( "  FAIL %s: step %d succeeded after step %d was refused; failure must be terminal [%s]\n",
                        vector.name, i + 1, stoppedAt + 1, vector.file );
                failures++;
                return;
            }
        }

        // and the same rule against a read the vector does not name, so every refused vector
        // carries the terminality check and not only the sequences that spell a successor
        fail_unless_stream_is_terminal( vector, stream );
        return;
    }

    const int failedStep = accepted ? -1 : 0;
    if ( failedStep >= 0 )
    {
        fail( vector, "the read was refused, the corpus requires it to be accepted" );
        return;
    }

    char entries[MaxSteps][MaxLine];
    const int numEntries = split_expect( vector.expect, entries );

    // one expect entry per step, objects and aligns included, which state `-`. A leading
    // preceding_bits step carries no expectation of its own: it exists to place the stream, and
    // the record states only the operation under test.
    const int offset = numSteps - numEntries;
    if ( offset < 0 )
    {
        fail( vector, "the expect list states more values than the vector has steps" );
        return;
    }

    for ( int i = 0; i < numEntries; i++ )
    {
        if ( strcmp( entries[i], "-" ) == 0 )
        {
            continue;
        }
        if ( !expectation_matches( steps[offset + i], entries[i] ) )
        {
            char rendered[MaxLine];
            render_step_value( steps[offset + i], rendered, MaxLine );
            printf( "  FAIL %s: step %d decoded %s, the corpus states %s [%s]\n",
                    vector.name, offset + i + 1, rendered, entries[i], vector.file );
            failures++;
            return;
        }
    }

    if ( vector.hasConsumed && stream.GetBitsProcessed() != vector.consumed )
    {
        printf( "  FAIL %s: consumed %d bits, the corpus states %d [%s]\n",
                vector.name, (int) stream.GetBitsProcessed(), (int) vector.consumed, vector.file );
        failures++;
    }
}

/*
    The writer leg. A vector marked `writer = canonical` states the bytes a conforming writer
    emits for its value, so the runner writes the decoded steps back and compares. The
    comparison covers the whole stream, which is what pins the trailing-bits obligation: the
    unused bits of the final byte must be zero, and a writer leaking anything into them
    produces a byte the vector does not carry.
*/

static void run_writer( const Vector & vector, Step * steps, int numSteps )
{
    writerChecked++;

    uint8_t scratch[MaxBytes + 64];
    memset( scratch, SlackFill, sizeof( scratch ) );

    // the bit writer stores qwords, so the buffer length must be a multiple of 8
    const int64_t capacity = (int64_t) sizeof( scratch ) & ~(int64_t) 7;
    serialize::WriteStream stream( scratch, capacity );

    if ( !run_steps( stream, steps, numSteps ) )
    {
        fail( vector, "the writer refused a canonical vector" );
        return;
    }
    stream.Flush();

    const int64_t written = stream.GetBytesProcessed();
    if ( written != vector.numBytes )
    {
        printf( "  FAIL %s: the writer emitted %d bytes, the corpus states %d [%s]\n",
                vector.name, (int) written, vector.numBytes, vector.file );
        failures++;
        return;
    }
    if ( written > 0 && memcmp( stream.GetData(), vector.bytes, (size_t) written ) != 0 )
    {
        char got[MaxLine];
        char want[MaxLine];
        bytes_to_hex( stream.GetData(), (int) written, got, MaxLine );
        bytes_to_hex( vector.bytes, vector.numBytes, want, MaxLine );
        printf( "  FAIL %s: the writer emitted %s, the corpus states %s [%s]\n",
                vector.name, got, want, vector.file );
        failures++;
    }
}

/*
    The measure leg. STANDARD.md makes a measure a BOUND and not the packet size — "it need not
    be exact, and cannot be" — so the corpus states a floor and the check is an inequality. A
    measure that computes alignment from a running bit index starting at zero under-counts every
    unaligned start and falls below the floor, which is the non-conforming accounting the
    document names.
*/

static void run_measure( const Vector & vector, Step * steps, int numSteps )
{
    measureChecked++;

    serialize::MeasureStream stream;
    if ( !run_steps( stream, steps, numSteps ) )
    {
        fail( vector, "the measure refused a step; a measure refuses nothing at runtime" );
        return;
    }

    if ( stream.GetBitsProcessed() < vector.measureAtLeast )
    {
        printf( "  FAIL %s: measured %d bits, the corpus requires at least %d [%s]\n",
                vector.name, (int) stream.GetBitsProcessed(), (int) vector.measureAtLeast, vector.file );
        failures++;
    }
}

static void run_vector( const Vector & vector )
{
    checked++;

    for ( int i = 0; i < vector.numParams; i++ )
    {
        if ( !operation_takes_param( vector.operation, vector.params[i].name ) )
        {
            printf( "  FAIL %s: no runner for parameter '%s' on operation '%s' [%s]\n",
                    vector.name, vector.params[i].name, vector.operation, vector.file );
            failures++;
            return;
        }
    }
    if ( vector.numSteps > 0 && strcmp( vector.operation, "sequence" ) != 0 )
    {
        fail( vector, "steps are only meaningful on a sequence" );
        return;
    }

    Step steps[MaxSteps];
    int numSteps = 0;
    if ( !build_steps( vector, steps, numSteps ) )
    {
        // a corpus file this runner does not know how to drive is a gap in the runner, not a pass
        fail( vector, "no runner for this operation, or for one of its parameters" );
        return;
    }

    const int failuresBefore = failures;
    run_reader( vector, steps, numSteps );

    // the writer and the measure are handed the values the reader decoded, so running them after
    // a reader failure reports a second failure about a value that was never decoded. One vector,
    // one diagnosis.
    if ( vector.expectKind != EXPECT_REFUSED && failures == failuresBefore )
    {
        // the reader leg leaves the decoded values in the steps, which is what the writer and
        // the measure are handed: a canonical vector's round trip is decode then re-emit
        if ( vector.writerCanonical )
        {
            run_writer( vector, steps, numSteps );
        }
        if ( vector.hasMeasure )
        {
            run_measure( vector, steps, numSteps );
        }
    }
}

// ---------------------------------------------------------------------------------------

static bool run_file( const char * path )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
    {
        printf( "  FAIL could not open %s\n", path );
        failures++;
        return false;
    }

    Vector vector;
    vector_reset( vector, path );

    char line[MaxLine];
    while ( fgets( line, sizeof( line ), file ) )
    {
        if ( line[0] == '#' )
        {
            continue;                   // a comment begins at the start of a line and nowhere else
        }
        char * text = trim( line );
        if ( *text == '\0' )
        {
            if ( !vector_empty( vector ) )
            {
                run_vector( vector );
                vector_reset( vector, path );
            }
            continue;
        }

        char * space = strchr( text, ' ' );
        const char * key = text;
        char * value = (char*) "";
        if ( space )
        {
            *space = '\0';
            value = trim( space + 1 );
        }

        if ( strcmp( key, "operation" ) == 0 )
        {
            strncpy( vector.operation, value, MaxLine - 1 );
        }
        else if ( strcmp( key, "name" ) == 0 )
        {
            strncpy( vector.name, value, MaxLine - 1 );
        }
        else if ( strcmp( key, "param" ) == 0 )
        {
            char * equals = strchr( value, '=' );
            if ( !equals )
            {
                printf( "  FAIL %s: malformed param line\n", path );
                failures++;
                continue;
            }
            *equals = '\0';
            const char * paramName = trim( value );
            const char * paramValue = trim( equals + 1 );
            if ( strcmp( paramName, "step" ) == 0 )
            {
                if ( vector.numSteps >= MaxSteps )
                {
                    printf( "  FAIL %s: too many steps\n", path );
                    failures++;
                    continue;
                }
                strncpy( vector.steps[vector.numSteps], paramValue, MaxLine - 1 );
                vector.numSteps++;
            }
            else if ( vector.numParams < MaxParams )
            {
                strncpy( vector.params[vector.numParams].name, paramName, MaxLine - 1 );
                strncpy( vector.params[vector.numParams].value, paramValue, MaxLine - 1 );
                vector.numParams++;
            }
            else
            {
                printf( "  FAIL %s: too many parameters\n", path );
                failures++;
            }
        }
        else if ( strcmp( key, "bytes" ) == 0 )
        {
            if ( !parse_hex_bytes( value, vector.bytes, MaxBytes, vector.numBytes ) )
            {
                printf( "  FAIL %s: malformed bytes line\n", path );
                failures++;
            }
        }
        else if ( strcmp( key, "expect" ) == 0 )
        {
            if ( strcmp( value, "refused" ) == 0 )
            {
                vector.expectKind = EXPECT_REFUSED;
            }
            else
            {
                char * equals = strchr( value, '=' );
                if ( !equals )
                {
                    printf( "  FAIL %s: malformed expect line\n", path );
                    failures++;
                    continue;
                }
                *equals = '\0';
                const char * expectKey = trim( value );
                if ( strcmp( expectKey, "bits" ) == 0 )
                {
                    vector.expectKind = EXPECT_BITS;
                }
                else if ( strcmp( expectKey, "value" ) == 0 )
                {
                    vector.expectKind = EXPECT_VALUE;
                }
                else
                {
                    printf( "  FAIL %s: unknown expect kind '%s'\n", path, expectKey );
                    failures++;
                    continue;
                }
                strncpy( vector.expect, trim( equals + 1 ), MaxLine - 1 );
            }
        }
        else if ( strcmp( key, "consumed" ) == 0 )
        {
            vector.consumed = (int64_t) strtol( value, NULL, 10 );
            vector.hasConsumed = true;
        }
        else if ( strcmp( key, "measure_at_least" ) == 0 )
        {
            vector.measureAtLeast = (int64_t) strtol( value, NULL, 10 );
            vector.hasMeasure = true;
        }
        else if ( strcmp( key, "writer" ) == 0 )
        {
            if ( strcmp( value, "canonical" ) != 0 )
            {
                printf( "  FAIL %s: unknown writer mode '%s'\n", path, value );
                failures++;
                continue;
            }
            vector.writerCanonical = true;
        }
        else
        {
            printf( "  FAIL %s: unknown key '%s'\n", path, key );
            failures++;
        }
    }

    if ( !vector_empty( vector ) )
    {
        run_vector( vector );
    }

    fclose( file );
    return true;
}

int main( int argc, char ** argv )
{
    if ( argc < 2 )
    {
        printf( "usage: conformance <vector file> [vector file ...]\n" );
        return 1;
    }

    printf( "\nrunning the conformance corpus\n\n" );

    for ( int i = 1; i < argc; i++ )
    {
        run_file( argv[i] );
    }

    printf( "%d vectors from %d file(s): %d writer checks, %d measure checks, %d failure(s)\n",
            checked, argc - 1, writerChecked, measureChecked, failures );

    if ( checked == 0 )
    {
        printf( "\nno vectors ran. the corpus is the conformance instrument: an empty run is a failure.\n\n" );
        return 1;
    }

    if ( failures > 0 )
    {
        printf( "\nthis implementation and the shared corpus disagree. THE IMPLEMENTATION IS THE BUG.\n\n" );
        return 1;
    }

    printf( "\n*** EVERY CONFORMANCE VECTOR PASSES ***\n\n" );

    return 0;
}

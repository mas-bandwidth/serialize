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
    Runs the shared conformance corpus through this library's reader.

    The corpus is the conformance/ directory: one file per operation, holding the accepted and
    refused vectors STANDARD.md's rules require. It is the conformance instrument every
    implementation in the family runs, and it is deliberately not generated from this code — a
    suite that regenerates its own expectations proves only that a port agrees with itself.

    Each vector states an operation, its parameters, the stream bytes, and either the value a
    conforming reader decodes together with the bits it consumes, or the word `refused`. An
    accepted vector must yield exactly that value and consume exactly that many bits; a refused
    vector must be refused, and must leave the caller's scalar destination unwritten, which is the
    obligation Reader Obligations states for every refusal.

    The vector files are named on the command line. STANDARD.md, "The vector format", specifies
    the syntax.
*/

#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------
// vector file parsing

const int MaxLine = 512;
const int MaxBytes = 256;

struct Vector
{
    char file[MaxLine];
    char operation[MaxLine];
    char name[MaxLine];
    char previous[MaxLine];
    char min[MaxLine];
    char max[MaxLine];
    char expect[MaxLine];
    uint8_t bytes[MaxBytes + 8];        // + 8: read buffer allocations extend 8 bytes past the data
    int numBytes;
    int64_t consumed;
    bool hasConsumed;
    bool refused;
};

static void vector_reset( Vector & vector, const char * file )
{
    memset( &vector, 0, sizeof( Vector ) );
    strncpy( vector.file, file, MaxLine - 1 );
}

static bool vector_empty( const Vector & vector )
{
    return vector.operation[0] == '\0';
}

// strips a trailing comment and surrounding whitespace, in place

static char * trim( char * text )
{
    char * hash = strchr( text, '#' );
    if ( hash )
    {
        *hash = '\0';
    }
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

static bool parse_bytes( const char * text, Vector & vector )
{
    vector.numBytes = 0;
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
        if ( vector.numBytes >= MaxBytes )
        {
            return false;
        }
        vector.bytes[vector.numBytes++] = (uint8_t) ( high * 16 + low );
        text += 2;
    }
    return true;
}

// decimal to 128 bit, because a vector's value can be wider than any built in strtol

static bool parse_int128( const char * text, serialize::int128_t & out )
{
    bool negative = false;
    if ( *text == '-' ) { negative = true; text++; }
    else if ( *text == '+' ) { text++; }
    if ( *text == '\0' )
    {
        return false;
    }
    serialize::int128_t value = 0;
    for ( ; *text; text++ )
    {
        if ( *text < '0' || *text > '9' )
        {
            return false;
        }
        value = value * 10 + serialize::int128_t( *text - '0' );
    }
    out = negative ? -value : value;
    return true;
}

static bool parse_int32( const char * text, int32_t & out )
{
    char * end = NULL;
    const long value = strtol( text, &end, 10 );
    if ( end == text || *end != '\0' )
    {
        return false;
    }
    out = (int32_t) value;
    return true;
}

// ---------------------------------------------------------------------------------------
// the operations under test, called through the public macros so the vectors exercise the
// surface a consumer uses rather than the stream methods underneath it

template <typename Stream> bool conformance_int_relative( Stream & stream, int32_t previous, int32_t & current )
{
    serialize_int_relative( stream, previous, current );
    return true;
}

template <typename Stream> bool conformance_int128( Stream & stream, serialize::int128_t & value, serialize::int128_t min, serialize::int128_t max )
{
    serialize_int128( stream, value, min, max );
    return true;
}

// ---------------------------------------------------------------------------------------
// running one vector

static int failures = 0;
static int checked = 0;

static void fail( const Vector & vector, const char * detail )
{
    printf( "  FAIL %s: %s [%s]\n", vector.name, detail, vector.file );
    failures++;
}

// consumed is stated on accepted reads only: after a refusal the stream position is not part of
// the contract, so no vector states it and no implementation is judged on it

static void fail_on_bits_consumed( const Vector & vector, int64_t bitsProcessed )
{
    if ( vector.hasConsumed && bitsProcessed != vector.consumed )
    {
        printf( "  FAIL %s: consumed %d bits, the corpus states %d [%s]\n", vector.name, (int) bitsProcessed, (int) vector.consumed, vector.file );
        failures++;
    }
}

static void print_int128( const char * label, serialize::int128_t value )
{
    const serialize::uint128_t bits = serialize::uint128_t( value );
    printf( "%s0x%08X%08X%08X%08X",
            label,
            (unsigned int) ( uint64_t( bits >> 96 ) & 0xFFFFFFFF ),
            (unsigned int) ( uint64_t( bits >> 64 ) & 0xFFFFFFFF ),
            (unsigned int) ( uint64_t( bits >> 32 ) & 0xFFFFFFFF ),
            (unsigned int) ( uint64_t( bits ) & 0xFFFFFFFF ) );
}

static void run_int_relative( const Vector & vector )
{
    int32_t previous = 0;
    if ( !parse_int32( vector.previous, previous ) )
    {
        fail( vector, "could not parse the previous parameter" );
        return;
    }

    serialize::ReadStream stream( vector.bytes, vector.numBytes );

    const int32_t sentinel = -12345;                // outside the domain, so no accepted vector can produce it
    int32_t current = sentinel;
    const bool accepted = conformance_int_relative( stream, previous, current );

    if ( vector.refused )
    {
        if ( accepted )
        {
            fail( vector, "the read succeeded, the corpus requires refusal" );
        }
        else if ( current != sentinel )
        {
            fail( vector, "the refused read wrote to the destination" );
        }
        return;
    }

    if ( !accepted )
    {
        fail( vector, "the read was refused, the corpus requires it to be accepted" );
        return;
    }

    int32_t expected = 0;
    if ( !parse_int32( vector.expect, expected ) )
    {
        fail( vector, "could not parse the expected value" );
        return;
    }
    if ( current != expected )
    {
        printf( "  FAIL %s: decoded %d, the corpus states %d [%s]\n", vector.name, (int) current, (int) expected, vector.file );
        failures++;
        return;
    }
    fail_on_bits_consumed( vector, stream.GetBitsProcessed() );
}

static void run_int128( const Vector & vector )
{
    serialize::int128_t min = 0;
    serialize::int128_t max = 0;
    if ( !parse_int128( vector.min, min ) || !parse_int128( vector.max, max ) )
    {
        fail( vector, "could not parse the min or max parameter" );
        return;
    }

    serialize::ReadStream stream( vector.bytes, vector.numBytes );

    const serialize::int128_t sentinel = serialize::int128_t( -12345 );
    serialize::int128_t value = sentinel;
    const bool accepted = conformance_int128( stream, value, min, max );

    if ( vector.refused )
    {
        if ( accepted )
        {
            fail( vector, "the read succeeded, the corpus requires refusal" );
        }
        else if ( !( value == sentinel ) )
        {
            fail( vector, "the refused read wrote to the destination" );
        }
        return;
    }

    if ( !accepted )
    {
        fail( vector, "the read was refused, the corpus requires it to be accepted" );
        return;
    }

    serialize::int128_t expected = 0;
    if ( !parse_int128( vector.expect, expected ) )
    {
        fail( vector, "could not parse the expected value" );
        return;
    }
    if ( !( value == expected ) )
    {
        printf( "  FAIL %s: decoded ", vector.name );
        print_int128( "", value );
        printf( ", the corpus states %s [%s]\n", vector.expect, vector.file );
        failures++;
        return;
    }
    fail_on_bits_consumed( vector, stream.GetBitsProcessed() );
}

static void run_vector( const Vector & vector )
{
    checked++;
    if ( strcmp( vector.operation, "int_relative" ) == 0 )
    {
        run_int_relative( vector );
    }
    else if ( strcmp( vector.operation, "int128" ) == 0 )
    {
        run_int128( vector );
    }
    else
    {
        // a corpus file this runner does not know how to drive is a gap in the runner, not a pass
        fail( vector, "no runner for this operation" );
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
            if ( strcmp( paramName, "previous" ) == 0 )
            {
                strncpy( vector.previous, paramValue, MaxLine - 1 );
            }
            else if ( strcmp( paramName, "min" ) == 0 )
            {
                strncpy( vector.min, paramValue, MaxLine - 1 );
            }
            else if ( strcmp( paramName, "max" ) == 0 )
            {
                strncpy( vector.max, paramValue, MaxLine - 1 );
            }
            else
            {
                printf( "  FAIL %s: no runner for parameter '%s'\n", path, paramName );
                failures++;
            }
        }
        else if ( strcmp( key, "bytes" ) == 0 )
        {
            if ( !parse_bytes( value, vector ) )
            {
                printf( "  FAIL %s: malformed bytes line\n", path );
                failures++;
            }
        }
        else if ( strcmp( key, "expect" ) == 0 )
        {
            if ( strcmp( value, "refused" ) == 0 )
            {
                vector.refused = true;
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
                strncpy( vector.expect, trim( equals + 1 ), MaxLine - 1 );
            }
        }
        else if ( strcmp( key, "consumed" ) == 0 )
        {
            vector.consumed = (int64_t) strtol( value, NULL, 10 );
            vector.hasConsumed = true;
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

    printf( "%d vectors from %d file(s), %d failure(s)\n", checked, argc - 1, failures );

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

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
    The write side debug assertions, tested by observing them fire.

    A serialize macro that narrows the caller's value before asserting on it cannot report
    the input the assertion exists to diagnose: the assertion sees a value already truncated
    to the narrower width, and 2^32 + 5 truncates to 5, which is in range by construction.
    Every macro that narrows now validates the caller's original expression first, and this
    file is where that ordering is proved.

    serialize_assert is defined here, before serialize.h, which the header honors. A
    recording definition makes "the assertion fired" an ordinary observable result rather
    than a process death: it runs identically on all three platforms in the matrix and in
    both build configurations, where an aborting assertion would need a death test whose
    shape differs on each of them, and it also names WHICH assertion fired.
*/

#include <stdio.h>
#include <stdint.h>

static int assert_fires = 0;
static const char * assert_condition = "";

#define serialize_assert( condition )                                                       \
    do                                                                                      \
    {                                                                                       \
        if ( !( condition ) )                                                               \
        {                                                                                   \
            /* the FIRST assertion to fire is the diagnostic one: a case that gets past  */ \
            /* it goes on to trip the downstream assertions the narrowing used to hide   */ \
            if ( assert_fires == 0 )                                                        \
            {                                                                               \
                assert_condition = #condition;                                              \
            }                                                                               \
            assert_fires++;                                                                 \
        }                                                                                   \
    }                                                                                       \
    while ( 0 )

#include "serialize.h"

static int failures = 0;

static void check( bool condition, const char * what )
{
    printf( "    %-62s %s\n", what, condition ? "pass" : "FAILED" );
    if ( !condition )
    {
        failures++;
    }
}

// the assertion counter is global to the translation unit, so each case starts from zero
static void reset()
{
    assert_fires = 0;
    assert_condition = "";
}

static void test_value_in_int_relative_domain()
{
    printf( "  serialize::value_in_int_relative_domain\n" );

    check( serialize::value_in_int_relative_domain( 0 ), "zero is in the domain" );
    check( serialize::value_in_int_relative_domain( 2147483647 ), "the top of the domain is in it" );
    check( !serialize::value_in_int_relative_domain( -1 ), "a negative value is out of the domain" );
    check( !serialize::value_in_int_relative_domain( int64_t( 2147483648LL ) ), "2^31 is out of the domain" );

    // the point of the helper: a wide value whose low 32 bits are in the domain
    check( !serialize::value_in_int_relative_domain( int64_t( 4294967301LL ) ), "2^32 + 5 is out of the domain, not 5" );
    check( !serialize::value_in_int_relative_domain( uint64_t( 1 ) << 63 ), "2^63 is out of the domain" );
    check( !serialize::value_in_int_relative_domain( uint32_t( 4294967295u ) ), "an unsigned value above the domain is out of it" );
}

static void test_write_int_relative_asserts_on_the_callers_value()
{
    printf( "  write_int_relative\n" );

    uint8_t buffer[64];

    // a wide current whose low 32 bits are in the domain and above previous. Narrowed to int
    // first, this is 5, which is a legal current for a previous of 4 -- and that is what the
    // helper's assertion used to be handed.
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 4294967301LL;                   // 2^32 + 5
        const int previous = 4;
        write_int_relative( stream, previous, current );
        check( assert_fires > 0, "a wide out of domain current trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }

    // the negative control: the same call one step inside the domain must not assert
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 5;
        const int previous = 4;
        write_int_relative( stream, previous, current );
        check( assert_fires == 0, "an in domain current does not trip it" );
    }

    // a current at the top of the domain is legal, and 2^31 is one step outside it
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 2147483647LL;
        const int previous = 2147483646;
        write_int_relative( stream, previous, current );
        check( assert_fires == 0, "a current at the top of the domain does not trip it" );
    }

    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        const int64_t current = 2147483648LL;
        const int previous = 2147483646;
        write_int_relative( stream, previous, current );
        check( assert_fires > 0, "a current one step above the domain trips it" );
        printf( "      assertion: %s\n", assert_condition );
    }
}

#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )

template <typename Stream> bool serialize_compile_time_bounded( Stream & stream, int64_t & value )
{
    serialize_int_compile_time( stream, value, 0, 1000 );
    return true;
}

static void test_serialize_int_compile_time_asserts_on_the_callers_value()
{
    printf( "  serialize_int_compile_time\n" );

    uint8_t buffer[64];

    // 2^32 narrows to 0, which is in [0,1000]: the assertion inside SerializeIntConst is
    // handed a value the narrowing already made legal
    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        int64_t value = 4294967296LL;
        serialize_compile_time_bounded( stream, value );
        check( assert_fires > 0, "a wide out of range value trips the assertion" );
        printf( "      assertion: %s\n", assert_condition );
    }

    {
        reset();
        serialize::WriteStream stream( buffer, (int64_t) sizeof( buffer ) );
        int64_t value = 500;
        serialize_compile_time_bounded( stream, value );
        check( assert_fires == 0, "an in range value does not trip it" );
    }
}

#endif // #if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )

int main()
{
    printf( "\nwrite side assertions\n\n" );

    test_value_in_int_relative_domain();
    test_write_int_relative_asserts_on_the_callers_value();
#if defined( SERIALIZE_HAS_COMPILE_TIME_SURFACE )
    test_serialize_int_compile_time_asserts_on_the_callers_value();
#endif

    if ( failures > 0 )
    {
        printf( "\n*** %d ASSERTION TESTS FAILED ***\n\n", failures );
        return 1;
    }

    printf( "\n*** ALL ASSERTION TESTS PASS ***\n\n" );

    return 0;
}

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
    serialize_utf.h — wide text to UTF-8 and back, for code that used serialize_wstring.

    serialize 2.0 removed wstring. The wire carries text one way, as serialize_string: UTF-8
    bytes, validated. A program that holds its text as wchar_t, UTF-16 or UTF-32 converts at its
    own boundary, with these functions, and serializes the UTF-8:

        char utf8[64];
        if ( serialize_wide_to_utf8( name, utf8, sizeof( utf8 ) ) < 0 )
            return false;                               // a lone surrogate, or no room
        serialize_string( stream, utf8, sizeof( utf8 ) );

    and on read, serialize_string first, then serialize_utf8_to_wide.

    This header is NOT part of the format and NOT part of serialize.h's stream API. Nothing here
    touches a stream, and serialize.h does not include it. It applies the rule serialize_string
    applies on the wire: the text is Unicode scalar values. A lone surrogate is refused, in UTF-16
    an unpaired high or low unit and in UTF-32 a code point in D800-DFFF, and so is anything above
    10FFFF and a zero character inside the counted input, because serialize_string refuses a NUL
    among its bytes. Every real character, in any plane, converts.

    Every function returns the number of units written, not counting the terminator it always
    writes, or -1 on refusal. On refusal the output holds an empty string when it has room for
    one. Sizes count units of the output type, terminator included.
*/

#ifndef SERIALIZE_UTF_H
#define SERIALIZE_UTF_H

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

// appends one code point as UTF-8. returns the new length, or -1 if it does not fit with room for the terminator

inline int serialize_utf8_append( char * output, int length, int output_size, uint32_t code )
{
    const int needed = ( code < 0x80 ) ? 1 : ( code < 0x800 ) ? 2 : ( code < 0x10000 ) ? 3 : 4;
    if ( length + needed + 1 > output_size )
        return -1;
    if ( needed == 1 )
    {
        output[length++] = (char) code;
    }
    else if ( needed == 2 )
    {
        output[length++] = (char) ( 0xC0 | ( code >> 6 ) );
        output[length++] = (char) ( 0x80 | ( code & 0x3F ) );
    }
    else if ( needed == 3 )
    {
        output[length++] = (char) ( 0xE0 | ( code >> 12 ) );
        output[length++] = (char) ( 0x80 | ( ( code >> 6 ) & 0x3F ) );
        output[length++] = (char) ( 0x80 | ( code & 0x3F ) );
    }
    else
    {
        output[length++] = (char) ( 0xF0 | ( code >> 18 ) );
        output[length++] = (char) ( 0x80 | ( ( code >> 12 ) & 0x3F ) );
        output[length++] = (char) ( 0x80 | ( ( code >> 6 ) & 0x3F ) );
        output[length++] = (char) ( 0x80 | ( code & 0x3F ) );
    }
    return length;
}

// the empty string on refusal, where there is room for one

template <typename T> inline int serialize_utf_refuse( T * output, int output_size )
{
    if ( output && output_size > 0 )
        output[0] = 0;
    return -1;
}

/**
    Decode one code point from UTF-8, well formed under Unicode Table 3-7: no overlong forms, no
    encoded surrogates, nothing above 10FFFF, no truncated sequence. The same rule serialize_string
    applies on read.
    @returns The number of bytes consumed, or 0 if the bytes at input are not well formed.
 */

inline int serialize_utf8_decode( const char * input, int input_bytes, uint32_t & code )
{
    const uint8_t * bytes = (const uint8_t*) input;
    const uint32_t lead = bytes[0];
    if ( lead < 0x80 )
    {
        code = lead;
        return 1;
    }
    int count = 0;
    uint32_t minimum = 0;
    if ( lead >= 0xC2 && lead <= 0xDF )         { count = 2; minimum = 0x80;    code = lead & 0x1F; }
    else if ( lead >= 0xE0 && lead <= 0xEF )    { count = 3; minimum = 0x800;   code = lead & 0x0F; }
    else if ( lead >= 0xF0 && lead <= 0xF4 )    { count = 4; minimum = 0x10000; code = lead & 0x07; }
    else
        return 0;                               // a continuation byte, C0, C1, or F5 and above
    if ( count > input_bytes )
        return 0;
    for ( int i = 1; i < count; i++ )
    {
        if ( ( bytes[i] & 0xC0 ) != 0x80 )
            return 0;
        code = ( code << 6 ) | ( bytes[i] & 0x3F );
    }
    if ( code < minimum )                       // overlong
        return 0;
    if ( code >= 0xD800 && code <= 0xDFFF )     // an encoded surrogate is not a scalar value
        return 0;
    if ( code > 0x10FFFF )
        return 0;
    return count;
}

/**
    UTF-16 to UTF-8. Surrogate pairs combine into their code point; an unpaired surrogate is refused.
    @param input The UTF-16 code units.
    @param input_units How many units to convert. A zero unit among them is refused.
    @param output The UTF-8 destination, always terminated.
    @param output_size The destination size in bytes, terminator included.
    @returns The bytes written, not counting the terminator, or -1 on refusal.
 */

inline int serialize_utf16_to_utf8( const uint16_t * input, int input_units, char * output, int output_size )
{
    if ( !output || output_size < 1 || input_units < 0 )
        return serialize_utf_refuse( output, output_size );
    int length = 0;
    for ( int i = 0; i < input_units; i++ )
    {
        uint32_t code = input[i];
        if ( code == 0 )
            return serialize_utf_refuse( output, output_size );
        if ( code >= 0xDC00 && code <= 0xDFFF )
            return serialize_utf_refuse( output, output_size );            // a low surrogate with no high before it
        if ( code >= 0xD800 && code <= 0xDBFF )
        {
            if ( i + 1 >= input_units || input[i+1] < 0xDC00 || input[i+1] > 0xDFFF )
                return serialize_utf_refuse( output, output_size );        // a high surrogate without its low
            code = 0x10000 + ( ( code - 0xD800 ) << 10 ) + ( input[i+1] - 0xDC00 );
            i++;
        }
        length = serialize_utf8_append( output, length, output_size, code );
        if ( length < 0 )
            return serialize_utf_refuse( output, output_size );
    }
    output[length] = '\0';
    return length;
}

/**
    UTF-32 to UTF-8. A surrogate code point (D800-DFFF) or a value above 10FFFF is refused.
    @returns The bytes written, not counting the terminator, or -1 on refusal.
 */

inline int serialize_utf32_to_utf8( const uint32_t * input, int input_units, char * output, int output_size )
{
    if ( !output || output_size < 1 || input_units < 0 )
        return serialize_utf_refuse( output, output_size );
    int length = 0;
    for ( int i = 0; i < input_units; i++ )
    {
        const uint32_t code = input[i];
        if ( code == 0 || ( code >= 0xD800 && code <= 0xDFFF ) || code > 0x10FFFF )
            return serialize_utf_refuse( output, output_size );
        length = serialize_utf8_append( output, length, output_size, code );
        if ( length < 0 )
            return serialize_utf_refuse( output, output_size );
    }
    output[length] = '\0';
    return length;
}

/**
    UTF-8 to UTF-16. A code point above FFFF becomes its surrogate pair.
    @param input The UTF-8 bytes.
    @param input_bytes How many bytes to convert. Malformed UTF-8, or a zero byte among them, is refused.
    @param output The UTF-16 destination, always terminated.
    @param output_units The destination size in units, terminator included.
    @returns The units written, not counting the terminator, or -1 on refusal.
 */

inline int serialize_utf8_to_utf16( const char * input, int input_bytes, uint16_t * output, int output_units )
{
    if ( !output || output_units < 1 || input_bytes < 0 )
        return serialize_utf_refuse( output, output_units );
    int length = 0;
    int i = 0;
    while ( i < input_bytes )
    {
        uint32_t code = 0;
        const int consumed = serialize_utf8_decode( input + i, input_bytes - i, code );
        if ( consumed == 0 || code == 0 )
            return serialize_utf_refuse( output, output_units );
        const int needed = ( code >= 0x10000 ) ? 2 : 1;
        if ( length + needed + 1 > output_units )
            return serialize_utf_refuse( output, output_units );
        if ( needed == 2 )
        {
            output[length++] = (uint16_t) ( 0xD800 + ( ( code - 0x10000 ) >> 10 ) );
            output[length++] = (uint16_t) ( 0xDC00 + ( ( code - 0x10000 ) & 0x3FF ) );
        }
        else
        {
            output[length++] = (uint16_t) code;
        }
        i += consumed;
    }
    output[length] = 0;
    return length;
}

/**
    UTF-8 to UTF-32.
    @returns The units written, not counting the terminator, or -1 on refusal.
 */

inline int serialize_utf8_to_utf32( const char * input, int input_bytes, uint32_t * output, int output_units )
{
    if ( !output || output_units < 1 || input_bytes < 0 )
        return serialize_utf_refuse( output, output_units );
    int length = 0;
    int i = 0;
    while ( i < input_bytes )
    {
        uint32_t code = 0;
        const int consumed = serialize_utf8_decode( input + i, input_bytes - i, code );
        if ( consumed == 0 || code == 0 )
            return serialize_utf_refuse( output, output_units );
        if ( length + 2 > output_units )
            return serialize_utf_refuse( output, output_units );
        output[length++] = code;
        i += consumed;
    }
    output[length] = 0;
    return length;
}

/**
    A null terminated wchar_t string to UTF-8. wchar_t is UTF-16 where it is 2 bytes (Windows) and
    UTF-32 where it is 4 (everywhere else), and each width is converted by its own rule.
    @returns The bytes written, not counting the terminator, or -1 on refusal.
 */

inline int serialize_wide_to_utf8( const wchar_t * input, char * output, int output_size )
{
    if ( !input )
        return serialize_utf_refuse( output, output_size );
    const size_t length_units = wcslen( input );
    if ( length_units > 0x7FFFFFFF )
        return serialize_utf_refuse( output, output_size );            // too long to count in an int
    const int units = (int) length_units;
    // through a local rather than tested inline, so MSVC /W4 does not flag the constant conditional
    const bool wide_wchar = sizeof( wchar_t ) >= 4;
    if ( wide_wchar )
    {
        int length = 0;
        if ( !output || output_size < 1 )
            return serialize_utf_refuse( output, output_size );
        for ( int i = 0; i < units; i++ )
        {
            const uint32_t code = (uint32_t) input[i];
            if ( ( code >= 0xD800 && code <= 0xDFFF ) || code > 0x10FFFF )
                return serialize_utf_refuse( output, output_size );
            length = serialize_utf8_append( output, length, output_size, code );
            if ( length < 0 )
                return serialize_utf_refuse( output, output_size );
        }
        output[length] = '\0';
        return length;
    }
    int length = 0;
    if ( !output || output_size < 1 )
        return serialize_utf_refuse( output, output_size );
    for ( int i = 0; i < units; i++ )
    {
        uint32_t code = (uint32_t) (uint16_t) input[i];
        if ( code >= 0xDC00 && code <= 0xDFFF )
            return serialize_utf_refuse( output, output_size );
        if ( code >= 0xD800 && code <= 0xDBFF )
        {
            const uint32_t next = ( i + 1 < units ) ? (uint32_t) (uint16_t) input[i+1] : 0;
            if ( next < 0xDC00 || next > 0xDFFF )
                return serialize_utf_refuse( output, output_size );
            code = 0x10000 + ( ( code - 0xD800 ) << 10 ) + ( next - 0xDC00 );
            i++;
        }
        length = serialize_utf8_append( output, length, output_size, code );
        if ( length < 0 )
            return serialize_utf_refuse( output, output_size );
    }
    output[length] = '\0';
    return length;
}

/**
    A null terminated UTF-8 string to wchar_t: UTF-16 units where wchar_t is 2 bytes, code points where it is 4.
    @param output_size The destination size in wchar_t, terminator included.
    @returns The wchar_t written, not counting the terminator, or -1 on refusal.
 */

inline int serialize_utf8_to_wide( const char * input, wchar_t * output, int output_size )
{
    if ( !input || !output || output_size < 1 )
        return serialize_utf_refuse( output, output_size );
    int input_bytes = 0;
    while ( input[input_bytes] != '\0' )
    {
        if ( input_bytes == 0x7FFFFFFF )
            return serialize_utf_refuse( output, output_size );        // too long to count in an int
        input_bytes++;
    }
    const bool wide_wchar = sizeof( wchar_t ) >= 4;
    int length = 0;
    int i = 0;
    while ( i < input_bytes )
    {
        uint32_t code = 0;
        const int consumed = serialize_utf8_decode( input + i, input_bytes - i, code );
        if ( consumed == 0 )
            return serialize_utf_refuse( output, output_size );
        const int needed = ( !wide_wchar && code >= 0x10000 ) ? 2 : 1;
        if ( length + needed + 1 > output_size )
            return serialize_utf_refuse( output, output_size );
        if ( needed == 2 )
        {
            output[length++] = (wchar_t) ( 0xD800 + ( ( code - 0x10000 ) >> 10 ) );
            output[length++] = (wchar_t) ( 0xDC00 + ( ( code - 0x10000 ) & 0x3FF ) );
        }
        else
        {
            output[length++] = (wchar_t) code;
        }
        i += consumed;
    }
    output[length] = 0;
    return length;
}

#endif // #ifndef SERIALIZE_UTF_H

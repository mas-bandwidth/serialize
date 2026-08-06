# Introduction

[![CI](https://github.com/mas-bandwidth/serialize/actions/workflows/ci.yml/badge.svg)](https://github.com/mas-bandwidth/serialize/actions/workflows/ci.yml)

**serialize** is a simple bitpacking serializer for C++.

![image](https://github.com/mas-bandwidth/serialize/assets/696656/dc36cc53-3382-4a63-888e-6dbb53dda92d)

It has the following features:

* Serialize a bool with only one bit
* Serialize any integer value from [1,64] bits writing only that number of bits to the buffer
* Serialize signed integer values with [min,max] writing only the required bits to the buffer
* Serialize floats, doubles, compressed floats, strings, byte arrays, and integers relative to another integer
* Serialize fixed point values with a compile time Q format and [min,max] bounds in whole units, writing only the required bits — round trips are exact, unlike compressed floats. Wide formats like Q112.16 work on every platform
* Serialize 128 bit unsigned integers on every platform: native __int128 where the compiler has it, an emulated signed/unsigned pair where it doesn't, byte-identical on the wire
* Alignment support so you can align your bitstream to a byte boundary whenever you want
* Optional template-based serialization so you can write one function that handles both read and write

# Usage

You can use the bitpacker directly:

```c++
const int BufferSize = 256;

uint8_t buffer[BufferSize];

serialize::BitWriter writer( buffer, BufferSize );

writer.WriteBits( 0, 1 );
writer.WriteBits( 1, 1 );
writer.WriteBits( 10, 8 );
writer.WriteBits( 255, 8 );
writer.WriteBits( 1000, 10 );
writer.WriteBits( 50000, 16 );
writer.WriteBits( 9999999, 32 );
writer.FlushBits();

const int bytesWritten = writer.GetBytesWritten();

serialize::BitReader reader( buffer, bytesWritten );

uint32_t a = reader.ReadBits( 1 );
uint32_t b = reader.ReadBits( 1 );
uint32_t c = reader.ReadBits( 8 );
uint32_t d = reader.ReadBits( 8 );
uint32_t e = reader.ReadBits( 10 );
uint32_t f = reader.ReadBits( 16 );
uint32_t g = reader.ReadBits( 32 );
```

Or you can write serialize methods for your types:

```c++
struct Vector
{
    float x,y,z;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_float( stream, x );
        serialize_float( stream, y );
        serialize_float( stream, z );
        return true;
    }
};

struct Quaternion
{
    float x,y,z,w;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_float( stream, x );
        serialize_float( stream, y );
        serialize_float( stream, z );
        serialize_float( stream, w );
        return true;
    }
};

struct RigidBody
{
    Vector position;
    Quaternion orientation;
    Vector linearVelocity;
    Vector angularVelocity;
    bool atRest;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_object( stream, position );
        serialize_object( stream, orientation );
        serialize_bool( stream, atRest );
        if ( !atRest )
        {
            serialize_object( stream, linearVelocity );
            serialize_object( stream, angularVelocity );
        }
        else if ( Stream::IsReading )
        {
            linearVelocity.x = linearVelocity.y = linearVelocity.z = 0.0;
            angularVelocity.x = angularVelocity.y = angularVelocity.z = 0.0;
        }
        return true;
    }
};
```

Fixed point values serialize exactly. The Q format and the bounds are compile time constants, and only the bits the range requires go on the wire:

```c++
struct Player
{
    int64_t position_x;                     // Q48.16 fixed point, in ±8192 whole units
    int64_t position_y;
    int64_t position_z;
    serialize::uint128_t entity_id;         // 128 bit globally unique id

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_fixed( stream, position_x, 48, 16, -8192, +8192 );
        serialize_fixed( stream, position_y, 48, 16, -8192, +8192 );
        serialize_fixed( stream, position_z, 48, 16, -8192, +8192 );
        serialize_uint128( stream, entity_id );
        return true;
    }
};
```

See [example.cpp](example.cpp) for more.

# Limitations

* Write buffer sizes must be a multiple of 8 bytes, because the bit writer flushes qwords to memory. Bytes past the end of the written data are only ever written as zeros. Buffers do not need any particular alignment: all memory access goes through memcpy.
* Read buffer sizes may be any number of bytes, but the underlying allocation must extend at least 8 bytes past the end of the packet data, because the bit reader loads 64 bit windows at byte granularity. The bytes past the end are loaded but never interpreted.
* Buffer sizes are effectively unlimited, because bit counts are stored in 64 bit signed integers.
* Wide strings are serialized as 32 bits per character, so streams are compatible between platforms with 2 and 4 byte wchar_t, but code points above 0xFFFF are not translated between UTF-16 and UTF-32 platforms.

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/netcode) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

If you find this software useful, [please consider sponsoring it](https://github.com/sponsors/mas-bandwidth). Thanks!

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).

## Crediting

If you use this library in a product, please credit it in your product credits:

> serialize - Glenn Fiedler and Rowan Claude

The license doesn't require this. It's an official request, and honoring it is appreciated. Fair credit keeps open source honest.

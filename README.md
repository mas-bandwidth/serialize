[![Build status](https://github.com/mas-bandwidth/serialize/workflows/CI/badge.svg)](https://github.com/mas-bandwidth/serialize/actions?query=workflow%3ACI)

# Introduction

**serialize** is a simple bitpacking serializer for C++.

![image](https://github.com/mas-bandwidth/serialize/assets/696656/dc36cc53-3382-4a63-888e-6dbb53dda92d)

It has the following features:

* Serialize a bool with only one bit
* Serialize any integer value from [1,64] bits writing only that number of bits to the buffer
* Serialize signed integer values with [min,max] writing only the required bits to the buffer
* Serialize floats, doubles, compressed floats, strings, byte arrays, and integers relative to another integer
* Alignment support so you can align your bitstream to a byte boundary whenever you want
* Template-based serialization system lets you write one function that does both read and write

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

See [example.cpp](example.cpp) for more examples.

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/netcode) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

If you find this software useful, [please consider sponsoring it](https://github.com/sponsors/mas-bandwidth). Thanks!

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).

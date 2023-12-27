[![Build status](https://github.com/mas-bandwidth/serialize/workflows/CI/badge.svg)](https://github.com/mas-bandwidth/serialize/actions?query=workflow%3ACI)

# Introduction

**serialize** is a simple bitpacking serializer for C++.

It has the following features:

* Serialize a bool with only one bit
* Serialize any integer value from [1,64] bits writing only that number of bits to the buffer
* Serialize signed integer values with [min,max] writing only the required bits to the buffer
* Serialize floats, doubles, compressed floats, strings, byte arrays, and integers relative to another integer
* Alignment support so you can align your bitstream to a byte boundary whenever you want
* Template based serialization system so you can write a unified serialize function instead of separate read and write functions

# Usage

You can use the bitwriter and bitreader classes directly:

```c++
    const int BufferSize = 256;

    uint8_t buffer[BufferSize];

    serialize::BitWriter writer( buffer, BufferSize );

    serialize_check( writer.GetData() == buffer );
    serialize_check( writer.GetBitsWritten() == 0 );
    serialize_check( writer.GetBytesWritten() == 0 );
    serialize_check( writer.GetBitsAvailable() == BufferSize * 8 );

    writer.WriteBits( 0, 1 );
    writer.WriteBits( 1, 1 );
    writer.WriteBits( 10, 8 );
    writer.WriteBits( 255, 8 );
    writer.WriteBits( 1000, 10 );
    writer.WriteBits( 50000, 16 );
    writer.WriteBits( 9999999, 32 );
    writer.FlushBits();

    const int bitsWritten = 1 + 1 + 8 + 8 + 10 + 16 + 32;

    serialize_check( writer.GetBytesWritten() == 10 );
    serialize_check( writer.GetBitsWritten() == bitsWritten );
    serialize_check( writer.GetBitsAvailable() == BufferSize * 8 - bitsWritten );

    const int bytesWritten = writer.GetBytesWritten();

    serialize_check( bytesWritten == 10 );

    memset( buffer + bytesWritten, 0, BufferSize - bytesWritten );

    serialize::BitReader reader( buffer, bytesWritten );

    serialize_check( reader.GetBitsRead() == 0 );
    serialize_check( reader.GetBitsRemaining() == bytesWritten * 8 );

    uint32_t a = reader.ReadBits( 1 );
    uint32_t b = reader.ReadBits( 1 );
    uint32_t c = reader.ReadBits( 8 );
    uint32_t d = reader.ReadBits( 8 );
    uint32_t e = reader.ReadBits( 10 );
    uint32_t f = reader.ReadBits( 16 );
    uint32_t g = reader.ReadBits( 32 );
```

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/netcode) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).

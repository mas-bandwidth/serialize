/*
    serialize

    Copyright © 2016 - 2024, Mas Bandwidth LLC.

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

#include "serialize.h"

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

enum AddressType
{
    Address_None,
    Address_IPv4,
    Address_IPv6,
    NumAddressTypes
};

struct Address
{
    uint8_t type;
    union {
        uint8_t  ipv4[4];
        uint16_t ipv6[8];
    };
    uint16_t port;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, type, Address_None, NumAddressTypes - 1 );
       
        if ( type == Address_IPv4 )
        {
            for ( int i = 0; i < 4; i++ )
            {
                serialize_bits( stream, ipv4[i], 8 );
            }
        }
        else if ( type == Address_IPv6 )
        {
            for ( int i = 0; i < 8; i++ )
            {
                serialize_bits( stream, ipv6[i], 16 );
            }
        }
       
        serialize_bits( stream, port, 16 );
       
        return true;
    }
};

const int MaxProperties = 1024;

enum PropertyType
{
    BoolProperty,
    ByteProperty,
    ShortProperty,
    IntProperty,
    LongProperty,
    FloatProperty,
    DoubleProperty,
    NumPropertyTypes
};

struct PropertyValue
{
    uint8_t type;

    union
    {
        bool bool_value;
        uint8_t byte_value;
        uint16_t short_value;
        uint32_t int_value;
        uint64_t long_value;
        float float_value;
        double double_value;
    };

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, type, 0, NumPropertyTypes - 1 );

        switch ( type )
        {
            case BoolProperty: 
            {
                serialize_bool( stream, bool_value );
            }
            break;

            case ByteProperty: 
            {
                serialize_bits( stream, byte_value, 8 );
            }
            break;

            case ShortProperty: 
            {
                serialize_bits( stream, short_value, 16 );
            }
            break;

            case IntProperty:
            {
                serialize_bits( stream, int_value, 32 );
            }
            break;

            case LongProperty:
            {
                serialize_bits( stream, int_value, 64 );
            }
            break;

            case FloatProperty:
            {
                serialize_float( stream, float_value );
            }
            break;

            case DoubleProperty:
            {
                serialize_double( stream, double_value );
            }
            break;
        }
        return true;
    }
};

enum PacketTypes
{
    A,
    B,
    C,
    D,
    E,
    F,
    NumPacketTypes,
};

const int MaxObjects = 256;

struct PacketA
{
    int numObjects;
    RigidBody object[MaxObjects];

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, numObjects, 0, MaxObjects );
        for ( int i = 0; i < numObjects; i++ )
        {
            serialize_object( stream, object[i] );
        }
        return true;
    }
};

struct PacketB
{
    float x,y,z;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_compressed_float( stream, x, -1.0, +1.0f, 0.001f );
        serialize_compressed_float( stream, y, -1.0, +1.0f, 0.001f );
        serialize_compressed_float( stream, z, -1.0, +1.0f, 0.001f );
        return true;
    }
};

struct PacketC
{
    int numProperties;
    int propertyIndex[MaxProperties];
    PropertyValue propertyValue[MaxProperties];

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, numProperties, 0, MaxProperties );
        int lastPropertyIndex = -1;
        for ( int i = 0; i < numProperties; i++ )
        {
            serialize_int_relative( stream, lastPropertyIndex, propertyIndex[i] );
            lastPropertyIndex = propertyIndex[i];
            serialize_object( stream, propertyValue[i] );
        }
        return true;
    }
};

const int MaxClients = 8;
const int MaxPlayerNameLength = 64;
const int PlayerDataBytes = 1024;

struct PacketD
{
    int clientIndex;
    uint64_t clientId;
    char playerName[MaxPlayerNameLength];
    bool hasPlayerData;
    uint8_t playerData[PlayerDataBytes];

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, clientIndex, 0, MaxClients - 1 );
        serialize_bits( stream, clientId, 64 );
        serialize_string( stream, playerName, MaxPlayerNameLength );
        serialize_bool( stream, hasPlayerData );
        if ( hasPlayerData )
        {
            serialize_bytes( stream, playerData, PlayerDataBytes );
        }
        return true;
    }
};

struct PacketE
{
    bool i,j,k;
    bool x,y,z;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_bool( stream, i );
        serialize_bool( stream, j );
        serialize_bool( stream, k );

        if ( i )
        {
            serialize_bool( stream, x );
            serialize_bool( stream, y );
            serialize_bool( stream, z );
        }

        serialize_align( stream );

        return true;
    }
};

struct Packet
{
    uint8_t packetType;
    union {
        PacketA a;
        PacketB b;
        PacketC c;
        PacketD d;
        PacketE e;
    };

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, packetType, 0, NumPacketTypes - 1 );
        switch ( packetType )
        {
            case A:
            {
                serialize_object( stream, a );
            }
            break;

            case B:
            {
                serialize_object( stream, b );
            }
            break;

            case C:
            {
                serialize_object( stream, c );
            }
            break;

            case D:
            {
                serialize_object( stream, d );
            }
            break;

            case E:
            {
                serialize_object( stream, e );
            }
            break;
        }

        uint32_t check_value = 0;

        if ( Stream::IsWriting )
        {
            check_value = 0x12345678;
        }

        serialize_bits( stream, check_value, 32 );

        if ( Stream::IsReading )
        {
            if ( check_value != 0x12345678 )
            {
                printf( "error: serialize check failed\n" );
                exit( 1 );
            }
        }

        return true;
    }
};

#include <time.h>

int main()
{
    printf( "\nserialize example\n\n" );

    srand( time(NULL) );

    for ( int i = 0; i < 10000; i++ )
    {
        Packet input;

        memset( &input, 0, sizeof(input) );

        input.packetType = rand() % NumPacketTypes;

        switch ( input.packetType )
        {
            case A:
            {
                input.a.numObjects = rand() % ( MaxObjects + 1 );

                for ( int j = 0; j < input.a.numObjects; j++ )
                {
                    input.a.object[j].position.x = ( rand() % 100 ) - 50;
                    input.a.object[j].position.y = ( rand() % 100 ) - 50;
                    input.a.object[j].position.z = ( rand() % 100 ) - 50;
                    
                    input.a.object[j].orientation.x = 1;
                    input.a.object[j].orientation.y = 0;
                    input.a.object[j].orientation.z = 0;
                    input.a.object[j].orientation.w = 0;

                    input.a.object[j].atRest = ( rand() % 2 ) == 0;
                    
                    if ( !input.a.object[j].atRest )
                    {
                        input.a.object[j].linearVelocity.x = 1.0f;
                        input.a.object[j].linearVelocity.y = 2.0f;
                        input.a.object[j].linearVelocity.z = 3.0f;
              
                        input.a.object[j].angularVelocity.x = 1.0f;
                        input.a.object[j].angularVelocity.y = 2.0f;
                        input.a.object[j].angularVelocity.z = 3.0f;
                    }
                }
            }
            break;

            case B: 
            {
                input.b.x = ( ( rand() % 1000000 ) / 1000000.0f ) - 0.5f;
                input.b.y = ( ( rand() % 1000000 ) / 1000000.0f ) - 0.5f;
                input.b.z = ( ( rand() % 1000000 ) / 1000000.0f ) - 0.5f;
            }
            break;

            case C:
            {
                input.c.numProperties = rand() % ( MaxProperties + 1 );

                int propertyIndex = 0;

                for ( int j = 0; j < input.c.numProperties; j++ )
                {
                    input.c.propertyIndex[j] = propertyIndex;
                    propertyIndex += 1 + rand() % 10;
                }

                for ( int j = 0; j < input.c.numProperties; j++ )
                {
                    int propertyType = rand() % NumPropertyTypes;

                    switch ( propertyType )
                    {
                        case BoolProperty:
                        {
                            input.c.propertyValue[j].bool_value = ( rand() % 2 ) == 0;
                        }
                        break;

                        case ByteProperty:
                        {
                            input.c.propertyValue[j].byte_value = rand() % 256;
                        }
                        break;

                        case ShortProperty:
                        {
                            input.c.propertyValue[j].short_value = rand() % 655356;
                        }
                        break;

                        case IntProperty:
                        {
                            input.c.propertyValue[j].int_value = uint32_t( rand() );
                        }
                        break;

                        case LongProperty:
                        {
                            input.c.propertyValue[j].long_value = uint64_t( rand() );
                            input.c.propertyValue[j].long_value <<= 32;
                            input.c.propertyValue[j].long_value |= uint64_t( rand() );
                        }
                        break;

                        case FloatProperty:
                        {
                            input.c.propertyValue[j].float_value = uint32_t( rand() % 10000000 ) / 1000.0f;
                        }
                        break;

                        case DoubleProperty:
                        {
                            input.c.propertyValue[j].double_value = uint32_t( rand() % 10000000 ) / 1000.0;
                        }
                        break;
                    }

                    input.c.propertyValue[j].type = propertyType;
                }
            }
            break;

            case D:
            {
                input.d.clientIndex = rand() % MaxClients;
                input.d.clientId = ( uint64_t( rand() ) << 32 ) | uint64_t( rand() );
                serialize_copy_string( input.d.playerName, "Hingle McCringleberry", sizeof(input.d.playerName) );
                input.d.hasPlayerData = ( rand() % 2 ) == 0;
                for ( int j = 0; j < PlayerDataBytes; j++ )
                {
                    input.d.playerData[j] = rand() % 256;
                }
            }
            break;

            case E:
            {
                input.e.i = ( ( rand() % 2 ) == 0 ) ? true : false;
                input.e.j = ( ( rand() % 2 ) == 0 ) ? true : false;
                input.e.k = ( ( rand() % 2 ) == 0 ) ? true : false;
                input.e.x = ( ( rand() % 2 ) == 0 ) ? true : false;
                input.e.y = ( ( rand() % 2 ) == 0 ) ? true : false;
                input.e.z = ( ( rand() % 2 ) == 0 ) ? true : false;
            }
            break;
        }

        uint8_t buffer[100*1024];

        serialize::WriteStream writeStream( buffer, sizeof(buffer) );
        if ( !input.Serialize( writeStream ) )
        {
            printf( "error: serialize write failed\n" );
            exit( 1 );
        }
        writeStream.Flush();

        const int bytesWritten = writeStream.GetBytesProcessed();

        Packet output;
        serialize::ReadStream readStream( buffer, bytesWritten );
        if ( !output.Serialize( readStream ) )
        {
            printf( "error: serialize read failed\n" );
            exit( 1 );
        }

        const int bytesRead = readStream.GetBytesProcessed();

        const char * packetTypeString[] = {
            "packet type a",
            "packet type b",
            "packet type c",
            "packet type d",
            "packet type e",
            "packet type f",
        };

        printf( "%d: %s - wrote %d bytes, read %d bytes\n", i, packetTypeString[input.packetType], bytesWritten, bytesRead );

        serialize_assert( bytesWritten == bytesRead );
    }

    printf( "\nSuccess!\n\n" );

    return 0;
}

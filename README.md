[![Build status](https://github.com/mas-bandwidth/serialize/workflows/CI/badge.svg)](https://github.com/mas-bandwidth/serialize/actions?query=workflow%3ACI)

# Introduction

**serialize** is a simple bitpacking serializer for C++.

It has the following features:

* Serialize a bool with only one bit
* Serialize any integer value from [1,64] bits writing only that number of bits to the buffer
* Template based serialization system so you can write a unified serialize function instead of separate read and write
* Serialize signed integer values with [min,max] range, varints, relative integer encoding, zig-zag encoding and compressed float values

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/netcode) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).

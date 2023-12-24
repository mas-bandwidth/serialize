[![Build status](https://github.com/mas-bandwidth/serialize/workflows/CI/badge.svg)](https://github.com/mas-bandwidth/serialize/actions?query=workflow%3ACI)

# Introduction

**serialize** is a simple bitpacking serializer for C++.

You can now write a bool with only one bit, or serialize integer values from [0,64] bits while taking up only that much space in the output buffer.

A template based serialization system is included on top of the bitpacker, so you can have a unified serialize function that performs both read and write.

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/netcode) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).

# Texture Channel Packer

A small Windows desktop tool for packing image channels into a single RGBA texture. Import up to four images, choose a source channel for each image, assign it to a target channel, preview the result, and export PNG or TGA.

## Requirements

- Windows 10 or later
- CMake 3.16 or newer
- MinGW-w64 with `g++` available on `PATH`
- OpenGL support

The build downloads GLFW and Dear ImGui through CMake FetchContent. The stb image headers used by the application are included in this repository.

## Build

From the project directory:

```powershell
cmake -S . -B build/Debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug --parallel
```

For a release build:

```powershell
cmake -S . -B build/Release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel
```

Run the executable from the corresponding build directory:

```powershell
.\build\Debug\TextureChannelPacker.exe
```

You can also use the included VS Code tasks after configuring CMake Tools with a MinGW toolchain on `PATH`.

## Supported formats

- Import: formats supported by stb_image
- Export: PNG and TGA

## Dependencies and licenses

- [GLFW](https://github.com/glfw/glfw), zlib/libpng license
- [Dear ImGui](https://github.com/ocornut/imgui), MIT license
- [stb](https://github.com/nothings/stb), public domain or MIT license

See the upstream projects for their complete license text.

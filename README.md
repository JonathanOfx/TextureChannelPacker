# Texture Channel Packer

A small windows app to shuffle and pack RGBA channels of up to 4 textures into a single texture.

<img width="1279" height="847" alt="ChannelPackerTool_V01" src="https://github.com/user-attachments/assets/eddab6f3-b777-423f-a9f7-48530e678bcd" />

## Supported formats

- Import: formats supported by stb_image
- Export: PNG and TGA


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

## Dependencies and licenses

- [GLFW](https://github.com/glfw/glfw), zlib/libpng license
- [Dear ImGui](https://github.com/ocornut/imgui), MIT license
- [stb](https://github.com/nothings/stb), public domain or MIT license

See the upstream projects for their complete license text.

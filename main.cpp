
// STB image library implementation for loading and writing images.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// OpenGL and texture handling includes and definitions.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ImGui and OpenGL includes for the user interface and rendering.
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// GLFW include for window and context management.
#include <GLFW/glfw3.h>

// Windows-specific includes for file dialogs and shell operations.
#ifdef _WIN32
#include <shobjidl.h>
#include <shellapi.h>
#endif

// Standard library includes for math, filesystem operations, I/O, strings, algorithms, and dynamic arrays.
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

// Alias for the filesystem namespace to simplify path handling.
namespace fs = std::filesystem;

// Enumerates the four color channels that can be moved from source image data into the packed preview.
enum class Channel
{
    R,
    G,
    B,
    A
};

// Represents a single imported texture and its generated GPU texture copies.
struct TextureEntry
{
    fs::path path;
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
    Channel sourceChannel = Channel::R;
    Channel targetChannel = Channel::R;
    GLuint imageTexture = 0;
    GLuint channelTexture = 0;
};

// Main runtime state for the UI, imported texture list, preview texture, and resolved pack status.
struct AppState
{
    std::vector<TextureEntry> textures;
    std::vector<unsigned char> packedPixels;
    GLuint previewTexture = 0;
    int previewWidth = 0;
    int previewHeight = 0;
    bool channelVisible[4] = { true, true, true, true };
    std::string status = "Ready";
    std::string resolutionMismatch = "";
    std::string importLimitWarning = "";
};

static AppState* gApp = nullptr;
constexpr size_t MAX_IMPORTED_TEXTURES = 4;

// Mirrors the GLFW error callback for the application window and OpenGL setup.
static void glfwErrorCallback(int error, const char* description)
{
    std::cerr << "GLFW error " << error << ": " << description << '\n';
}

// Returns the single-letter UI label shown for each selectable channel in the source and target rows.
static const char* ChannelLabel(Channel channel)
{
    switch (channel)
    {
    case Channel::R: return "R";
    case Channel::G: return "G";
    case Channel::B: return "B";
    case Channel::A: return "A";
    default: return "R";
    }
}

// Maps each channel enum member to the display color used by UI bars and texture labels.
static ImVec4 ChannelColor(Channel channel)
{
    switch (channel)
    {
    case Channel::R: return ImVec4(0.82f, 0.16f, 0.16f, 1.0f);
    case Channel::G: return ImVec4(0.18f, 0.72f, 0.20f, 1.0f);
    case Channel::B: return ImVec4(0.12f, 0.45f, 0.88f, 1.0f);
    case Channel::A: return ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    default: return ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    }
}

// Paints a colored name bar for a texture row so the active target channel is visually identifiable.
static void DrawTextureNameBar(TextureEntry& tex)
{
    ImVec2 start = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    ImU32 barColor = ImColor(ChannelColor(tex.targetChannel));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float labelHeight = ImGui::GetTextLineHeightWithSpacing();

    drawList->AddRectFilled(start, ImVec2(start.x + avail.x, start.y + labelHeight + 8.0f), barColor, 4.0f);

    ImGui::SetCursorScreenPos(ImVec2(start.x + 8.0f, start.y + 4.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::TextUnformatted(tex.name.c_str());
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + labelHeight + 8.0f));
}

// Uploads a raw RGBA pixel buffer into an OpenGL texture for display and preview rendering.
static GLuint CreateTextureFromPixels(const std::vector<unsigned char>& pixels, int width, int height)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return texture;
}

// Generates the grayscale single-channel texture view for the selected source channel.
static void BuildChannelTexture(TextureEntry& entry)
{
    if (entry.imageTexture == 0)
        return;

    int source = static_cast<int>(entry.sourceChannel);
    std::vector<unsigned char> grayscale(entry.width * entry.height * 4);
    for (int y = 0; y < entry.height; ++y)
    {
        for (int x = 0; x < entry.width; ++x)
        {
            int idx = (y * entry.width + x) * 4;
            unsigned char value = entry.pixels[idx + source];
            grayscale[idx + 0] = value;
            grayscale[idx + 1] = value;
            grayscale[idx + 2] = value;
            grayscale[idx + 3] = 255;
        }
    }

    if (entry.channelTexture != 0)
    {
        glDeleteTextures(1, &entry.channelTexture);
    }

    entry.channelTexture = CreateTextureFromPixels(grayscale, entry.width, entry.height);
}

// Loads a disk image into memory and creates both the source texture preview and the channel texture preview.
static bool LoadTextureFromPath(const fs::path& path, TextureEntry& entry)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!data)
    {
        return false;
    }

    entry.path = path;
    entry.name = path.filename().string();
    entry.width = width;
    entry.height = height;
    entry.pixels.assign(data, data + width * height * 4);
    stbi_image_free(data);

    entry.imageTexture = CreateTextureFromPixels(entry.pixels, width, height);
    BuildChannelTexture(entry);
    return true;
}

// Builds the exact packed RGBA image in CPU memory from the selected source/target channel assignments.
static std::vector<unsigned char> BuildPackedPixels(const AppState& app, int& packedWidth, int& packedHeight, bool respectVisibility = true)
{
    if (app.textures.empty())
    {
        packedWidth = 0;
        packedHeight = 0;
        return {};
    }

    packedWidth = app.textures[0].width;
    packedHeight = app.textures[0].height;

    for (const auto& tex : app.textures)
    {
        if (tex.width != packedWidth || tex.height != packedHeight)
        {
            packedWidth = 0;
            packedHeight = 0;
            return {};
        }
    }

    std::vector<unsigned char> packed(packedWidth * packedHeight * 4, 0);
    for (int i = 0; i < packedWidth * packedHeight; ++i)
    {
        packed[i * 4 + 0] = 0;
        packed[i * 4 + 1] = 0;
        packed[i * 4 + 2] = 0;
        packed[i * 4 + 3] = 255;
    }

    for (const auto& tex : app.textures)
    {
        int source = static_cast<int>(tex.sourceChannel);
        int target = static_cast<int>(tex.targetChannel);
        if (source < 0 || source > 3 || target < 0 || target > 3)
            continue;

        if (respectVisibility && !app.channelVisible[target])
            continue;

        for (int y = 0; y < tex.height; ++y)
        {
            for (int x = 0; x < tex.width; ++x)
            {
                int base = (y * tex.width + x) * 4;
                int outBase = (y * packedWidth + x) * 4;

                // Each selected texture contributes its chosen source channel into the selected target channel.
                // If multiple textures target the same channel, the later entry intentionally overwrites the earlier one.
                packed[outBase + target] = tex.pixels[base + source];
            }
        }
    }

    return packed;
}

// Constructs the packed RGBA preview texture by copying selected source-channel bytes into the visible target-channel slots.
static void RegeneratePackedPreview(AppState& app)
{
    if (app.previewTexture != 0)
    {
        glDeleteTextures(1, &app.previewTexture);
        app.previewTexture = 0;
    }

    app.previewWidth = 0;
    app.previewHeight = 0;
    app.packedPixels.clear();

    if (app.textures.empty())
    {
        app.status = "Ready";
        return;
    }

    int packedWidth = 0;
    int packedHeight = 0;
    app.packedPixels = BuildPackedPixels(app, packedWidth, packedHeight);

    if (packedWidth == 0 || packedHeight == 0)
    {
        app.status = "Textures must be same size";
        return;
    }

    app.previewTexture = CreateTextureFromPixels(app.packedPixels, packedWidth, packedHeight);
    app.previewWidth = packedWidth;
    app.previewHeight = packedHeight;
    app.status = "Packed preview updated";
}

// Validates and records an imported texture path, checking resolution consistency against the first texture already loaded.
static bool TryImportTextureFromPath(const fs::path& path, AppState& app)
{
    if (!fs::exists(path) || !fs::is_regular_file(path))
    {
        app.status = "Could not load texture: " + path.filename().string();
        return false;
    }

    TextureEntry entry;
    entry.path = path;
    entry.name = path.filename().string();

    if (!LoadTextureFromPath(path, entry))
    {
        app.status = "Could not load texture: " + path.filename().string();
        return false;
    }

    bool alreadyLoaded = false;
    for (const auto& existing : app.textures)
    {
        if (existing.path == entry.path)
        {
            app.status = "Texture already loaded";
            alreadyLoaded = true;
            break;
        }
    }

    if (alreadyLoaded)
        return false;

    if (app.textures.size() >= MAX_IMPORTED_TEXTURES)
    {
        app.importLimitWarning = "You have reached the maximum of " + std::to_string(MAX_IMPORTED_TEXTURES) + " textures. Remove one before importing another.";
        app.status = "Texture limit reached (max " + std::to_string(MAX_IMPORTED_TEXTURES) + ")";
        return false;
    }

    if (!app.textures.empty())
    {
        const auto& first = app.textures[0];
        if (entry.width != first.width || entry.height != first.height)
        {
            app.resolutionMismatch = "Error: Mismatched texture size detected, import failed.";
            app.status = "Texture resolution mismatch";
            return false;
        }
    }

    app.textures.push_back(entry);
    app.status = "Loaded " + entry.name;
    RegeneratePackedPreview(app);
    return true;
}

static void ExportPackedTexture(AppState& app, const std::string& filepath);

// Opens the native Windows file save dialog so the user can choose a .png or .tga file name for the packed output.
static void ExportPackedTextureToDialog(AppState& app)
{
    if (app.previewTexture == 0 || app.previewWidth == 0 || app.previewHeight == 0)
    {
        app.status = "Nothing to export - generate a preview first";
        return;
    }

#ifdef _WIN32
    IFileSaveDialog* pFileSave = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, (void**)&pFileSave);
    if (FAILED(hr))
    {
        app.status = "Failed to create save dialog";
        return;
    }

    pFileSave->SetTitle(L"Export Packed Texture");

    COMDLG_FILTERSPEC rgSpec[] = { { L"All Supported", L"*.png;*.tga" }, { L"PNG Image", L"*.png" }, { L"TGA Image", L"*.tga" } };
    pFileSave->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
    pFileSave->SetFileTypeIndex(1);

    hr = pFileSave->Show(nullptr);
    if (SUCCEEDED(hr))
    {
        IShellItem* pItem = nullptr;
        hr = pFileSave->GetResult(&pItem);
        if (SUCCEEDED(hr))
        {
            PWSTR pszFilePath = nullptr;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
            if (SUCCEEDED(hr))
            {
                std::wstring widePath(pszFilePath);
                std::string path(widePath.begin(), widePath.end());
                ExportPackedTexture(app, path);
                CoTaskMemFree(pszFilePath);
            }
            pItem->Release();
        }
    }
    pFileSave->Release();
#endif
}

// Writes the exact packed CPU buffer to disk using the extension in the selected file path.
static void ExportPackedTexture(AppState& app, const std::string& filepath)
{
    if (app.textures.empty() || app.previewWidth == 0 || app.previewHeight == 0)
    {
        app.status = "Nothing to export";
        return;
    }

    std::string resolvedPath = filepath;
    std::string lowerPath = resolvedPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);

    bool hasExtension = lowerPath.find(".png") != std::string::npos || lowerPath.find(".tga") != std::string::npos;
    if (!hasExtension)
    {
        // Default to PNG when the user leaves the extension off.
        resolvedPath += ".png";
        lowerPath = resolvedPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
    }

    // Export always includes every channel regardless of the preview's "View channels" toggles.
    int packedWidth = 0;
    int packedHeight = 0;
    std::vector<unsigned char> pixels = BuildPackedPixels(app, packedWidth, packedHeight, false);
    if (packedWidth == 0 || packedHeight == 0 || pixels.empty())
    {
        app.status = "Nothing to export";
        return;
    }

    int result = 0;
    if (lowerPath.find(".tga") != std::string::npos)
    {
        result = stbi_write_tga(resolvedPath.c_str(), packedWidth, packedHeight, 4, pixels.data());
    }
    else
    {
        result = stbi_write_png(resolvedPath.c_str(), packedWidth, packedHeight, 4, pixels.data(), packedWidth * 4);
    }

    if (result != 0)
    {
        app.status = "Export successful: " + resolvedPath;
    }
    else
    {
        app.status = "Export failed";
    }
}

// Uses the native Windows file open picker to choose a texture file to load into the imported texture list.
static void ImportTextureFromDialog(AppState& app)
{
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        app.status = "Could not initialize file dialog";
        return;
    }

    IFileOpenDialog* dialog = nullptr;
    HRESULT createHr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(createHr))
    {
        app.status = "Could not open file dialog";
        CoUninitialize();
        return;
    }

    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

    if (SUCCEEDED(dialog->Show(nullptr)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR filePath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath)))
            {
                fs::path selected(filePath);
                TryImportTextureFromPath(selected, app);
                CoTaskMemFree(filePath);
            }
            item->Release();
        }
    }

    dialog->Release();
    CoUninitialize();
#else
    app.status = "File picker only available on Windows";
#endif
}

// Handles dropping one or more files into the GLFW window and routes each path through the same import path.
static void DropFiles(GLFWwindow* window, int count, const char** paths)
{
    if (!gApp)
        return;

    for (int i = 0; i < count; ++i)
    {
        fs::path dropPath(paths[i]);
        TryImportTextureFromPath(dropPath, *gApp);
    }
}

// Removes a texture entry and frees its OpenGL texture resources before refreshing the packed preview.
static void RemoveTexture(AppState& app, size_t index)
{
    if (index >= app.textures.size())
        return;

    TextureEntry& tex = app.textures[index];
    if (tex.imageTexture != 0)
        glDeleteTextures(1, &tex.imageTexture);
    if (tex.channelTexture != 0)
        glDeleteTextures(1, &tex.channelTexture);

    app.textures.erase(app.textures.begin() + static_cast<std::ptrdiff_t>(index));
    RegeneratePackedPreview(app);
}

// Updates the selected source or target channel and regenerates the packed preview right away.
static void SetTextureChannel(TextureEntry& tex, AppState& app, bool source, Channel candidate)
{
    if (source)
    {
        tex.sourceChannel = candidate;
        BuildChannelTexture(tex);
    }
    else
    {
        tex.targetChannel = candidate;
    }

    RegeneratePackedPreview(app);
}

// Draws the source or target channel selector row for an imported texture and records the selected button rect for arrows.
static void DrawChannelButtonRow(TextureEntry& tex, AppState& app, bool source, ImRect* selectedRect = nullptr, ImRect* rowRect = nullptr)
{
    ImGui::PushID(source ? "source" : "target");

    ImRect rects[4];
    for (int c = 0; c < 4; ++c)
    {
        Channel candidate = static_cast<Channel>(c);
        bool selected = source ? tex.sourceChannel == candidate : tex.targetChannel == candidate;

        ImGui::PushID(c);
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.4f, 0.7f, 0.95f, 1.0f) : ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(0.55f, 0.78f, 1.0f, 1.0f) : ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, selected ? ImVec4(0.25f, 0.60f, 0.85f, 1.0f) : ImVec4(0.18f, 0.18f, 0.18f, 1.0f));

        if (ImGui::Button(ChannelLabel(candidate)))
        {
            SetTextureChannel(tex, app, source, candidate);
        }

        rects[c] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

        ImGui::PopStyleColor(3);
        ImGui::PopID();
        if (c < 3)
            ImGui::SameLine();
    }

    // Resolve against the post-click channel so a same-frame click never leaves the rect at its default (0,0) value.
    if (selectedRect)
    {
        int finalIndex = static_cast<int>(source ? tex.sourceChannel : tex.targetChannel);
        *selectedRect = rects[finalIndex];
    }

    if (rowRect)
        *rowRect = ImRect(rects[0].Min, rects[3].Max);

    ImGui::PopID();
}

// Draws the visual transfer arrow connecting the selected source channel row to the target channel row.
static void DrawChannelTransferArrow(const ImRect& sourceRect, const ImRect& targetRect)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    ImVec2 start;
    start.x = (sourceRect.Min.x + sourceRect.Max.x) * 0.5f;
    start.y = sourceRect.Max.y + 2.0f;

    ImVec2 end;
    end.x = (targetRect.Min.x + targetRect.Max.x) * 0.5f;
    end.y = targetRect.Min.y - 2.0f;

    drawList->AddLine(start, end, ImColor(1.0f, 1.0f, 1.0f, 1.0f), 1.5f);

    float dx = end.x - start.x;
    float dy = end.y - start.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f)
    {
        return;
    }

    float ux = dx / len;
    float uy = dy / len;

    ImVec2 left;
    left.x = end.x - ux * 7.0f - uy * 3.0f;
    left.y = end.y - uy * 7.0f + ux * 3.0f;

    ImVec2 right;
    right.x = end.x - ux * 7.0f + uy * 3.0f;
    right.y = end.y - uy * 7.0f - ux * 3.0f;

    drawList->AddTriangleFilled(end, left, right, ImColor(1.0f, 1.0f, 1.0f, 1.0f));
}

// Shows a centered modal popup when a drag/drop or import file breaks the texture-size consistency requirement.
static void DrawResolutionMismatchPopup(AppState& app)
{
    if (app.resolutionMismatch.empty())
        return;

    ImGui::OpenPopup("Texture size error");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 0), ImVec2(420, 200));

    if (ImGui::BeginPopupModal("Texture size error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s", app.resolutionMismatch.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(80, 0)))
        {
            app.resolutionMismatch.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void DrawImportLimitPopup(AppState& app)
{
    if (app.importLimitWarning.empty())
        return;

    ImGui::OpenPopup("Texture limit reached");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0), ImVec2(420, 200));

    if (ImGui::BeginPopupModal("Texture limit reached", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s", app.importLimitWarning.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(80, 0)))
        {
            app.importLimitWarning.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Opens the given URL in the user's default web browser.
static void OpenUrlInBrowser(const char* url)
{
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

// Draws a square "?" button matching the standard button height that opens the given URL in a browser when clicked.
static void DrawHelpMarker(const char* url)
{
    float size = ImGui::GetFrameHeight();
    if (ImGui::Button("?", ImVec2(size, size)))
    {
        OpenUrlInBrowser(url);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Open help: %s", url);
    }
}

// Renders the imported texture panel, including the import/export/help buttons and texture rows.
static void DrawTextureList(AppState& app)
{
    DrawResolutionMismatchPopup(app);
    DrawImportLimitPopup(app);

    ImGui::BeginChild("TextureStream", ImVec2(460, 800), true);
    
    ImGui::Text("Textures");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 154.0f);
    if (ImGui::Button("Import"))
    {
        ImportTextureFromDialog(app);
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("Export"))
    {
        ExportPackedTextureToDialog(app);
    }
    ImGui::SameLine(0.0f, 8.0f);
    DrawHelpMarker("https://github.com/JonathanOfx/TextureChannelPacker");

    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::Separator();

    float availableHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("TextureContent", ImVec2(0.0f, availableHeight), false, ImGuiWindowFlags_NoMove);

    if (app.textures.empty())
    {
        ImVec2 area = ImGui::GetContentRegionAvail();
        float lineHeight = ImGui::GetTextLineHeight();
        float textWidth = ImGui::CalcTextSize("Drag and drop a texture").x;
        float centerX = area.x * 0.5f - textWidth * 0.5f;
        float centerY = area.y * 0.5f - lineHeight * 0.5f;

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + centerY);
        ImGui::SetCursorPosX(centerX);
        ImGui::TextUnformatted("Drag and drop a texture");
    }
    else
    {
        size_t i = 0;
        while (i < app.textures.size())
        {
            auto& tex = app.textures[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::BeginGroup();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));

            DrawTextureNameBar(tex);

            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 78.0f);
            bool removed = false;
            if (ImGui::Button("Remove"))
            {
                RemoveTexture(app, i);
                removed = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::Separator();

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            
            // Removal erases the vector entry, so avoid touching now-invalid `tex` for the rest of this row.
            if (!removed)
            {
                ImVec2 rowTop = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(rowTop.x, rowTop.y + 5.0f));

                ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(tex.imageTexture)), ImVec2(96, 96));
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(128, 128, 128, 255));
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(tex.channelTexture)), ImVec2(96, 96));
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(128, 128, 128, 255));

                ImGui::SameLine(0.0f, 30.0f);
                // Reset Y so only the previews shifted down, not the Source/Target group.
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop.y));
                ImGui::BeginGroup();

                // Add a small vertical spacing before the source group
                ImGui::Dummy(ImVec2(0.0f, 0.0f));

                ImGui::BeginGroup();
                // Reserve the label's line without drawing it yet, since its centered X depends on the row rect below.
                ImVec2 sourceLabelPos = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
                ImRect sourceRect;
                ImRect sourceRowRect;
                DrawChannelButtonRow(tex, app, true, &sourceRect, &sourceRowRect);
                float sourceLabelWidth = ImGui::CalcTextSize("Source").x;
                float sourceLabelX = sourceRowRect.Min.x + (sourceRowRect.GetWidth() - sourceLabelWidth) * 0.5f;
                ImGui::GetWindowDrawList()->AddText(ImVec2(sourceLabelX, sourceLabelPos.y), ImGui::GetColorU32(ImGuiCol_Text), "Source");
                ImGui::EndGroup();

                ImGui::Dummy(ImVec2(0.0f, 18.0f));

                ImGui::BeginGroup();
                ImRect targetRect;
                ImRect targetRowRect;
                DrawChannelButtonRow(tex, app, false, &targetRect, &targetRowRect);
                float targetLabelWidth = ImGui::CalcTextSize("Target").x;
                float targetLabelX = targetRowRect.Min.x + (targetRowRect.GetWidth() - targetLabelWidth) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(targetLabelX, ImGui::GetCursorScreenPos().y));
                ImGui::Text("Target");
                ImGui::EndGroup();

                DrawChannelTransferArrow(sourceRect, targetRect);
                ImGui::EndGroup();
            }

            ImGui::EndGroup();

            ImGui::Dummy(ImVec2(0.0f, 4.0f));

            ImGui::Separator();
            ImGui::PopID();

            if (removed)
            {
                continue;
            }

            ++i;
        }
    }

    ImGui::EndChild();

    ImGui::EndChild();
}


// Renders the preview header and scrollable image panel for the packed texture result.
static void DrawPackPreview(AppState& app)
{
    ImGui::BeginChild("Preview", ImVec2(800, 800), true);

    ImGui::BeginChild("PreviewTopBar", ImVec2(0.0f,24.0f), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Text("Channel Packed Preview");
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(12.0f, 0.0f));
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(10.0f, 0.0f));
    ImGui::SameLine();
    ImGui::Text("View channels");
    ImGui::SameLine();
    for (int c = 0; c < 4; ++c)
    {
        Channel channel = static_cast<Channel>(c);
        const char* label = ChannelLabel(channel);
        bool visible = app.channelVisible[c];
        if (ImGui::Checkbox(label, &visible))
        {
            app.channelVisible[c] = visible;
            RegeneratePackedPreview(app);
        }
        ImGui::SameLine();
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 170.0f);
    ImGui::Text("Resolution: %dx%d", app.previewWidth, app.previewHeight);
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("PreviewImageScroll", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar);
    if (app.previewTexture != 0)
    {
        float availableWidth = ImGui::GetContentRegionAvail().x;
        float scale = availableWidth / static_cast<float>(app.previewWidth);
        float drawWidth = availableWidth;
        float drawHeight = static_cast<float>(app.previewHeight) * scale;

        ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(app.previewTexture)),
                     ImVec2(drawWidth, drawHeight));
    }
    ImGui::EndChild();

    ImGui::EndChild();
}

// Bootstraps the GLFW window, ImGui UI, and texture packing app state, then starts the render loop.
int main()
{
    glfwSetErrorCallback(glfwErrorCallback);

    if (!glfwInit())
    {
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1285, 815, "Texture Channel Packer v" APP_VERSION, nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    {
        int iconWidth = 0, iconHeight = 0, iconChannels = 0;
        unsigned char* iconPixels = stbi_load("TexturePackerIcon.png", &iconWidth, &iconHeight, &iconChannels, 4);
        if (iconPixels)
        {
            GLFWimage icon{ iconWidth, iconHeight, iconPixels };
            glfwSetWindowIcon(window, 1, &icon);
            stbi_image_free(iconPixels);
        }
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    AppState app;
    gApp = &app;
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, DropFiles);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1280, 900));
        ImGui::Begin("Texture Channel Packer", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        DrawTextureList(app);
        ImGui::SameLine();
        DrawPackPreview(app);

        ImGui::End();

        ImGui::Render();

        int displayW = 0;
        int displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    if (app.previewTexture != 0)
        glDeleteTextures(1, &app.previewTexture);

    for (auto& tex : app.textures)
    {
        if (tex.imageTexture != 0)
            glDeleteTextures(1, &tex.imageTexture);
        if (tex.channelTexture != 0)
            glDeleteTextures(1, &tex.channelTexture);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

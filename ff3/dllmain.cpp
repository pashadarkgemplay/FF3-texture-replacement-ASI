#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "libMinHook.x86.lib")
#include <windows.h>
#include <GL/gl.h>
#include <string>
#include <filesystem>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <fstream>
#include "MinHook.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
namespace fs = std::filesystem;
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5   0x8363
#define GL_BGRA_EXT  0x80E1
typedef void (WINAPI* glTexImage2D_t)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid* pixels);
typedef void (WINAPI* glTexSubImage2D_t)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels);
glTexImage2D_t o_glTexImage2D = nullptr;
glTexSubImage2D_t o_glTexSubImage2D = nullptr;
std::unordered_set<unsigned int> dumpedHashes;
std::mutex hashMutex;
const std::string logFileName = "dumped_textures.txt";
unsigned int CalculateCRC32(const unsigned char* data, size_t size) {
    unsigned int crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return ~crc;
}
void LoadHashDatabase() {
    std::ifstream file(logFileName);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            unsigned int hash = std::stoul(line, nullptr, 16);
            dumpedHashes.insert(hash);
        }
        catch (...) {}
    }
    file.close();
}
void SaveHashToDatabase(unsigned int hash) {
    std::lock_guard<std::mutex> lock(hashMutex);
    dumpedHashes.insert(hash);
    std::ofstream file(logFileName, std::ios::app);
    if (file.is_open()) {
        file << "0x" << std::hex << hash << "\n";
        file.close();
    }
}
bool IsAlreadyDumped(unsigned int hash) {
    std::lock_guard<std::mutex> lock(hashMutex);
    return dumpedHashes.find(hash) != dumpedHashes.end();
}
bool ConvertToRGBA32(GLenum format, GLenum type, GLsizei width, GLsizei height, const void* pixels, std::vector<unsigned char>& outBuffer) {
    size_t numPixels = width * height;
    outBuffer.resize(numPixels * 4);
    if (type == GL_UNSIGNED_BYTE) {
        if (format == GL_RGBA) {
            memcpy(outBuffer.data(), pixels, numPixels * 4);
            return true;
        }
        else if (format == GL_RGB) {
            const unsigned char* src = (const unsigned char*)pixels;
            for (size_t i = 0; i < numPixels; ++i) {
                outBuffer[i * 4 + 0] = src[i * 3 + 0];
                outBuffer[i * 4 + 1] = src[i * 3 + 1];
                outBuffer[i * 4 + 2] = src[i * 3 + 2];
                outBuffer[i * 4 + 3] = 255;
            }
            return true;
        }
        else if (format == GL_BGRA_EXT) {
            const unsigned char* src = (const unsigned char*)pixels;
            for (size_t i = 0; i < numPixels; ++i) {
                outBuffer[i * 4 + 0] = src[i * 4 + 2];
                outBuffer[i * 4 + 1] = src[i * 4 + 1];
                outBuffer[i * 4 + 2] = src[i * 4 + 0];
                outBuffer[i * 4 + 3] = src[i * 4 + 3];
            }
            return true;
        }
    }
    else if (type == GL_UNSIGNED_SHORT_5_6_5) {
        const unsigned short* src = (const unsigned short*)pixels;
        for (size_t i = 0; i < numPixels; ++i) {
            unsigned short p = src[i];
            outBuffer[i * 4 + 0] = ((p >> 11) & 0x1F) * 255 / 31;
            outBuffer[i * 4 + 1] = ((p >> 5) & 0x3F) * 255 / 63;
            outBuffer[i * 4 + 2] = (p & 0x1F) * 255 / 31;
            outBuffer[i * 4 + 3] = 255;
        }
        return true;
    }
    else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        const unsigned short* src = (const unsigned short*)pixels;
        for (size_t i = 0; i < numPixels; ++i) {
            unsigned short p = src[i];
            outBuffer[i * 4 + 0] = ((p >> 12) & 0x0F) * 17;
            outBuffer[i * 4 + 1] = ((p >> 8) & 0x0F) * 17;
            outBuffer[i * 4 + 2] = ((p >> 4) & 0x0F) * 17;
            outBuffer[i * 4 + 3] = (p & 0x0F) * 17;
        }
        return true;
    }
    else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
        const unsigned short* src = (const unsigned short*)pixels;
        for (size_t i = 0; i < numPixels; ++i) {
            unsigned short p = src[i];
            outBuffer[i * 4 + 0] = ((p >> 11) & 0x1F) * 255 / 31;
            outBuffer[i * 4 + 1] = ((p >> 6) & 0x1F) * 255 / 31;
            outBuffer[i * 4 + 2] = ((p >> 1) & 0x1F) * 255 / 31;
            outBuffer[i * 4 + 3] = (p & 1) ? 255 : 0;
        }
        return true;
    }
    return false;
}

bool TryLoadReplacement(unsigned int hash, int& outWidth, int& outHeight, unsigned char*& outPixels) {
    std::stringstream filePath;
    filePath << "pixelpasha/0x" << std::hex << hash << ".png";
    if (fs::exists(filePath.str())) {
        int channels = 0;
        stbi_set_flip_vertically_on_load(0);
        outPixels = stbi_load(filePath.str().c_str(), &outWidth, &outHeight, &channels, 4);
        return outPixels != nullptr;
    }
    return false;
}
void SaveTexture(const std::string& folderPrefix, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels, unsigned int hash) {
    std::vector<unsigned char> rgbaBuffer;
    if (ConvertToRGBA32(format, type, width, height, pixels, rgbaBuffer)) {
        std::stringstream dirPath;
        dirPath << folderPrefix << "/" << width << "x" << height;
        try {
            fs::create_directories(dirPath.str());
        }
        catch (...) {}
        std::stringstream filePath;
        filePath << dirPath.str() << "/0x" << std::hex << hash << ".png";
        if (!fs::exists(filePath.str())) {
            stbi_flip_vertically_on_write(0);
            stbi_write_png(filePath.str().c_str(), width, height, 4, rgbaBuffer.data(), width * 4);
        }
    }
}
void WINAPI Detour_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid* pixels) {
    if (target == GL_TEXTURE_2D && pixels != nullptr && width > 0 && height > 0) {
        std::vector<unsigned char> rgbaBuffer;
        if (ConvertToRGBA32(format, type, width, height, pixels, rgbaBuffer)) {
            unsigned int hash = CalculateCRC32(rgbaBuffer.data(), rgbaBuffer.size());
            int modWidth = 0, modHeight = 0;
            unsigned char* modPixels = nullptr;

            if (TryLoadReplacement(hash, modWidth, modHeight, modPixels)) {
                o_glTexImage2D(target, level, GL_RGBA, modWidth, modHeight, border, GL_RGBA, GL_UNSIGNED_BYTE, modPixels);
                stbi_image_free(modPixels);
                return;
            }

            o_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);

            if (!IsAlreadyDumped(hash)) {
                SaveTexture("dumps", width, height, format, type, pixels, hash);
                SaveHashToDatabase(hash);
            }
            return;
        }
    }
    o_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
void WINAPI Detour_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels) {
    if (target == GL_TEXTURE_2D && pixels != nullptr && width > 0 && height > 0) {
        std::vector<unsigned char> rgbaBuffer;
        if (ConvertToRGBA32(format, type, width, height, pixels, rgbaBuffer)) {
            unsigned int hash = CalculateCRC32(rgbaBuffer.data(), rgbaBuffer.size());
            int modWidth = 0, modHeight = 0;
            unsigned char* modPixels = nullptr;

            if (TryLoadReplacement(hash, modWidth, modHeight, modPixels)) {
                o_glTexSubImage2D(target, level, xoffset, yoffset, modWidth, modHeight, GL_RGBA, GL_UNSIGNED_BYTE, modPixels);
                stbi_image_free(modPixels);
                return;
            }

            o_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);

            if (!IsAlreadyDumped(hash)) {
                SaveTexture("dumps/sub", width, height, format, type, pixels, hash);
                SaveHashToDatabase(hash);
            }
            return;
        }
    }
    o_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}
void InitHooks() {
    fs::create_directories("pixelpasha");
    LoadHashDatabase();

    if (MH_Initialize() != MH_OK) return;

    HMODULE hOpenGL = GetModuleHandleA("opengl32.dll");
    if (hOpenGL) {
        void* p_glTexImage2D = (void*)GetProcAddress(hOpenGL, "glTexImage2D");
        void* p_glTexSubImage2D = (void*)GetProcAddress(hOpenGL, "glTexSubImage2D");

        if (p_glTexImage2D) {
            MH_CreateHook(p_glTexImage2D, &Detour_glTexImage2D, reinterpret_cast<LPVOID*>(&o_glTexImage2D));
        }
        if (p_glTexSubImage2D) {
            MH_CreateHook(p_glTexSubImage2D, &Detour_glTexSubImage2D, reinterpret_cast<LPVOID*>(&o_glTexSubImage2D));
        }

        MH_EnableHook(MH_ALL_HOOKS);
    }
}
void UninitHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)InitHooks, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        UninitHooks();
        break;
    }
    return TRUE;
}
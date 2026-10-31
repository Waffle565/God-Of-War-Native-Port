// GOW-Port: replacement texture files -> Metal textures (gs_metal_texrepl_store.h). DDS block-compressed data is uploaded
// as is (Apple GPUs sample BC1-BC7 natively), with the file's own mip chain; PNG is read with ImageIO, kept in straight
// alpha (the GS alpha range: 0x80 = opaque, as PCSX2 dumps and loads it) and gets a generated mip chain.
#include "gs_metal_texrepl_store.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    uint32_t rd32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    constexpr uint32_t fourcc(char a, char b, char c, char d)
    {
        return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
    }

    id<MTLTexture> loadDds(id<MTLDevice> device, NSData *file, size_t &bytes)
    {
        const uint8_t *d = static_cast<const uint8_t *>(file.bytes);
        const size_t size = file.length;
        if (size < 128 || rd32(d) != fourcc('D', 'D', 'S', ' ') || rd32(d + 4) != 124u)
            return nil;
        const uint32_t height = rd32(d + 12), width = rd32(d + 16), flags = rd32(d + 8);
        uint32_t levels = (flags & 0x20000u) ? std::max(1u, rd32(d + 28)) : 1u;
        const uint32_t pfFlags = rd32(d + 80), code = rd32(d + 84), rgbBits = rd32(d + 88), rMask = rd32(d + 92);
        size_t offset = 128;
        MTLPixelFormat format = MTLPixelFormatInvalid;
        uint32_t blockBytes = 0; // 0: uncompressed 4 bytes per pixel
        if ((pfFlags & 4u) && code == fourcc('D', 'X', '1', '0'))
        {
            if (size < 148)
                return nil;
            offset = 148;
            switch (rd32(d + 128)) // DXGI_FORMAT
            {
            case 71: case 72: format = MTLPixelFormatBC1_RGBA; blockBytes = 8; break;
            case 74: case 75: format = MTLPixelFormatBC2_RGBA; blockBytes = 16; break;
            case 77: case 78: format = MTLPixelFormatBC3_RGBA; blockBytes = 16; break;
            case 98: case 99: format = MTLPixelFormatBC7_RGBAUnorm; blockBytes = 16; break;
            case 28: case 29: format = MTLPixelFormatRGBA8Unorm; break;
            case 87: case 91: format = MTLPixelFormatBGRA8Unorm; break;
            default: return nil;
            }
        }
        else if (pfFlags & 4u)
        {
            if (code == fourcc('D', 'X', 'T', '1')) format = MTLPixelFormatBC1_RGBA, blockBytes = 8;
            else if (code == fourcc('D', 'X', 'T', '3')) format = MTLPixelFormatBC2_RGBA, blockBytes = 16;
            else if (code == fourcc('D', 'X', 'T', '5')) format = MTLPixelFormatBC3_RGBA, blockBytes = 16;
            else return nil;
        }
        else if ((pfFlags & 0x40u) && rgbBits == 32u)
            format = rMask == 0x000000FFu ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatBGRA8Unorm;
        else
            return nil;
        if (width == 0u || height == 0u || width > 16384u || height > 16384u)
            return nil;
        uint32_t maxLevels = 1;
        while ((std::max(width, height) >> maxLevels) != 0u)
            ++maxLevels;
        levels = std::min(levels, maxLevels);
        // a truncated chain keeps only the levels the file holds: the texture must not claim levels it never gets
        {
            size_t at = offset;
            uint32_t present = 0;
            for (; present < levels; ++present)
            {
                const uint32_t w = std::max(1u, width >> present), h = std::max(1u, height >> present);
                const size_t pitch = blockBytes ? static_cast<size_t>((w + 3u) / 4u) * blockBytes : static_cast<size_t>(w) * 4u;
                const size_t rows = blockBytes ? (h + 3u) / 4u : h;
                if (at + pitch * rows > size)
                    break;
                at += pitch * rows;
            }
            if (present == 0u)
                return nil;
            levels = present;
        }
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        desc.mipmapLevelCount = levels;
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (!tex)
            return nil;
        bytes = 0;
        for (uint32_t level = 0; level < levels; ++level)
        {
            const uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
            const size_t pitch = blockBytes ? static_cast<size_t>((w + 3u) / 4u) * blockBytes : static_cast<size_t>(w) * 4u;
            const size_t rows = blockBytes ? (h + 3u) / 4u : h;
            if (offset + pitch * rows > size)
                return nil; // cannot happen: the levels were counted above
            [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level withBytes:d + offset bytesPerRow:pitch];
            offset += pitch * rows;
            bytes += pitch * rows;
        }
        return tex;
    }

    // straight-alpha RGBA8 of an image: the raw PNG bytes when they already are that, else drawn premultiplied and divided
    bool rgbaOf(CGImageRef image, std::vector<uint8_t> &out, uint32_t &w, uint32_t &h)
    {
        w = static_cast<uint32_t>(CGImageGetWidth(image));
        h = static_cast<uint32_t>(CGImageGetHeight(image));
        if (w == 0u || h == 0u || w > 16384u || h > 16384u)
            return false;
        out.assign(static_cast<size_t>(w) * h * 4u, 0u);
        const CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
        const CGBitmapInfo order = CGImageGetBitmapInfo(image) & kCGBitmapByteOrderMask;
        if (CGImageGetBitsPerComponent(image) == 8u && CGImageGetBitsPerPixel(image) == 32u && (alpha == kCGImageAlphaLast || alpha == kCGImageAlphaNoneSkipLast) &&
            (order == kCGBitmapByteOrderDefault || order == kCGBitmapByteOrder32Big))
        {
            CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
            if (!data)
                return false;
            const size_t pitch = CGImageGetBytesPerRow(image);
            const uint8_t *src = CFDataGetBytePtr(data);
            const bool ok = static_cast<size_t>(CFDataGetLength(data)) >= pitch * (h - 1u) + w * 4u;
            for (uint32_t y = 0; ok && y < h; ++y)
                std::memcpy(out.data() + static_cast<size_t>(y) * w * 4u, src + y * pitch, w * 4u);
            if (ok && alpha == kCGImageAlphaNoneSkipLast)
                for (size_t i = 3; i < out.size(); i += 4)
                    out[i] = 0xFF;
            CFRelease(data);
            return ok;
        }
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(out.data(), w, h, 8, w * 4u, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(space);
        if (!ctx)
            return false;
        CGContextSetBlendMode(ctx, kCGBlendModeCopy);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
        CGContextRelease(ctx);
        for (size_t i = 0; i < out.size(); i += 4)
            if (const uint32_t a = out[i + 3]; a != 0u && a != 255u)
                for (int c = 0; c < 3; ++c)
                    out[i + c] = static_cast<uint8_t>(std::min(255u, (out[i + c] * 255u + a / 2u) / a));
        return true;
    }

    id<MTLTexture> loadPng(id<MTLDevice> device, id<MTLCommandQueue> queue, NSData *file, size_t &bytes)
    {
        CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)file, nullptr);
        if (!source)
            return nil;
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        CFRelease(source);
        if (!image)
            return nil;
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        const bool ok = rgbaOf(image, rgba, w, h);
        CGImageRelease(image);
        if (!ok)
            return nil;
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:YES];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (!tex)
            return nil;
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:w * 4u];
        if (tex.mipmapLevelCount > 1 && queue)
        {
            id<MTLCommandBuffer> cmd = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
        }
        bytes = static_cast<size_t>(w) * h * 4u * 4u / 3u;
        return tex;
    }
}

id<MTLTexture> gsrepl::loadReplacement(id<MTLDevice> device, id<MTLCommandQueue> queue, const std::string &path, size_t &bytes)
{
    @autoreleasepool
    {
        NSData *file = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()] options:NSDataReadingMappedIfSafe error:nil];
        if (!file || file.length < 8)
            return nil;
        const uint8_t *d = static_cast<const uint8_t *>(file.bytes);
        if (rd32(d) == fourcc('D', 'D', 'S', ' '))
            return loadDds(device, file, bytes);
        return loadPng(device, queue, file, bytes);
    }
}

bool gsrepl::writePng(const std::string &path, const uint32_t *rgba, uint32_t width, uint32_t height)
{
    @autoreleasepool
    {
        CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(rgba), static_cast<CFIndex>(width) * height * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4u, space, kCGImageAlphaLast | kCGBitmapByteOrder32Big, provider, nullptr, false,
                                         kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        if (!image)
            return false;
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
        bool ok = false;
        if (dst)
        {
            CGImageDestinationAddImage(dst, image, nullptr);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CGImageRelease(image);
        return ok;
    }
}

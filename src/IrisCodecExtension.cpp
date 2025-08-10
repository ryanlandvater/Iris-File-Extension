/**
 * @file IrisCodecExtension.cpp
 * @author Ryan Landvater (RyanLandvater@gmail.com)
 * @brief  Iris Codec API Documentation.
 * @version 2025.1.0
 * @date 2024-01-11
 *
 * @copyright Copyright (c) 2023-25 Iris Developers
 *
 * Use of Iris Codec and the Iris File Extension (.iris) follows the
 * CC BY-ND 4.0 License outlined in the Iris Digital Slide Extension File Structure Techinical Specification
 * https://creativecommons.org/licenses/by-nd/4.0/
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the “Software”), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Use of this code constitutes your implicit agreement to these requirements.
 *
 */
// ~~~~~~~~~~~ INDEPENDENT INCLUDES ~~~~~~~~~~~~ //
// **NOTE:** Unlike most files in the Iris Codec, this is meant
// to be completely independently implementable by other authors
// As a result, we will NOT use the singular
// #include "IrisCodecPriv.hpp"
// But instead include all required elements manually
#include <bit> // NOTE: Bit requires compiling against C++20
#include <memory>
#include <math.h>
#include <float.h>
#include <iostream>
#include <assert.h>
#include "IrisTypes.hpp"
#include "IrisBuffer.hpp"
#include "IrisCodecTypes.hpp"
#include "IrisCodecExtension.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#ifdef _MSC_VER
static_assert(sizeof(short)     == 2);
static_assert(sizeof(long)      == 4);
static_assert(sizeof(long long) == 8);
#define __builtin_bswap16(X)    _byteswap_ushort(X)
#define __builtin_bswap32(X)    _byteswap_ulong(X)
#define __builtin_bswap64(X)    _byteswap_uint64(X)
#else
static_assert(__cplusplus >= 202002L, "Enable C++20 or greater in your Make system");
#endif

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
#ifndef FLT16_MIN
#define _Float16            float // use 32-bit if no 16
#endif
#ifndef U16_CAST
#define U16_CAST(X)         static_cast<uint16_t>(X)
#endif
#ifndef U32_CAST
#define U32_CAST(X)         static_cast<uint32_t>(X)
#endif
#ifndef F16_CAST
#define F16_CAST(X)         static_cast<_Float16>(X)
#endif
#ifndef F32_CAST
#define F32_CAST(X)         static_cast<float>(X)
#endif
#ifndef F64_CAST
#define F64_CAST(X)         static_cast<double>(X)
#endif
#define U8_MASK                     0x000000FF
#define U16_MASK                    0x0000FFFF
#define U24_MASK                    0x00FFFFFF
#define U32_MASK            0x00000000FFFFFFFF
#define U40_MASK            0x000000FFFFFFFFFF
#define U48_MASK            0x0000FFFFFFFFFFFF
#define U56_MASK            0x00FFFFFFFFFFFFFF
#define U64_MASK            0xFFFFFFFFFFFFFFFF
#define UINT24_MAX          16777215U
#define UINT40_MAX          1099511627775ULL
#define UINT48_MAX          281474976710655ULL
#define UINT52_MAX          4503599627370495ULL
#define UINT56_MAX          72057594037927935ULL
#define F16_IEEE_base       1024.f
#define F16_MANTESSA        0x03FF
#define F16_EXP             0x7C00
#define F16_NEG             0x8000
#define F32_IEEE_base       8388608.f
#define F32_MANTESSA        0x007FFFFF
#define F32_EXP             0x7F800000
#define F32_NEG             0x80000000
#define F64_IEEE_base       4.5035996e+15
#define F64_MANTESSA        0x000FFFFFFFFFFFFF
#define F64_EXP             0x7FF0000000000000
#define F64_NEG             0x8000000000000000
#define BYTE_PTR(X)         static_cast<BYTE* const>(X)
#define STORE_U8(P,V)       *static_cast<unsigned char*>(P) = static_cast<unsigned char>(V);
#define IRIS_EXTENSION_1_0  0x00010000
#define IRIS_EXTENSION_2_0  0x00020000
constexpr bool little_endian = std::endian::native == std::endian::little;
constexpr bool is_ieee754    = std::numeric_limits<float>::is_iec559;
template<typename T>
inline T load_unaligned (const void* ptr) {
static_assert(std::is_trivially_copyable_v<T>,"load_unaligned requires trivially copyable type");
T val; memcpy(&val, ptr, sizeof(T)); return val;}
template<typename T>
inline void store_unaligned (void*const ptr, T val) {
static_assert(std::is_trivially_copyable_v<T>,"store_unaligned requires trivially copyable type");
memcpy(ptr, &val, sizeof(T));}
inline _Float16 F16_CONVERT_NON_IEEE (uint32_t val);
inline uint16_t F16_CONVERT_NON_IEEE (_Float16 val);
inline float    F32_CONVERT_NON_IEEE (uint32_t val);
inline uint32_t F32_CONVERT_NON_IEEE (float val);
inline double   F64_CONVERT_NON_IEEE (uint64_t val);
inline uint64_t F64_CONVERT_NON_IEEE (double val);
inline uint8_t  LOAD_U8      (const void* ptr){return *static_cast<const uint8_t*>(ptr);}
inline uint64_t __LE_LOAD_U64(const void* ptr){return load_unaligned<uint64_t>(ptr);}
inline uint64_t __BE_LOAD_U64(const void* ptr){return __builtin_bswap64(__LE_LOAD_U64(ptr));}
inline uint64_t __LE_LOAD_U40(const void* ptr){return load_unaligned<uint64_t>(ptr)&U40_MASK;}
inline uint64_t __BE_LOAD_U40(const void* ptr){return __builtin_bswap64(__LE_LOAD_U64(ptr))&U40_MASK;}
inline uint32_t __LE_LOAD_U32(const void* ptr){return load_unaligned<uint32_t>(ptr);}
inline uint32_t __BE_LOAD_U32(const void* ptr){return __builtin_bswap32(__LE_LOAD_U32(ptr));}
inline uint32_t __LE_LOAD_U24(const void* ptr){return load_unaligned<uint32_t>(ptr)&U24_MASK;}
inline uint32_t __BE_LOAD_U24(const void* ptr){return __builtin_bswap32(__LE_LOAD_U32(ptr))&U40_MASK;}
inline uint16_t __LE_LOAD_U16(const void* ptr){return load_unaligned<uint16_t>(ptr);}
inline uint16_t __BE_LOAD_U16(const void* ptr){return __builtin_bswap16(__LE_LOAD_U16(ptr));}
inline float __LE_LOAD_F32_IE3(const void* ptr){return std::bit_cast<float>(__LE_LOAD_U32(ptr));}
inline float __BE_LOAD_F32_IE3(const void* ptr){return std::bit_cast<float>(__BE_LOAD_U32(ptr));}
inline float __LE_LOAD_F32_NON(const void* ptr){return F32_CONVERT_NON_IEEE(__LE_LOAD_U32(ptr));}
inline float __BE_LOAD_F32_NON(const void* ptr){return F32_CONVERT_NON_IEEE(__BE_LOAD_U32(ptr));}
inline double __LE_LOAD_F64_IE3(const void* ptr){return std::bit_cast<double>(__LE_LOAD_U64(ptr));}
inline double __BE_LOAD_F64_IE3(const void* ptr){return std::bit_cast<double>(__BE_LOAD_U64(ptr));}
inline double __LE_LOAD_F64_NON(const void* ptr){return F64_CONVERT_NON_IEEE(__LE_LOAD_U64(ptr));}
inline double __BE_LOAD_F64_NON(const void* ptr){return F64_CONVERT_NON_IEEE(__BE_LOAD_U64(ptr));}
inline void __LE_STORE_U64(void* ptr, uint64_t v){store_unaligned<uint64_t>(ptr,v);}
inline void __BE_STORE_U64(void* ptr, uint64_t v){store_unaligned<uint64_t>(ptr,__builtin_bswap64(v));}
inline void __LE_STORE_U40(void* ptr, uint64_t v){memcpy(ptr, &v, 5);}
inline void __BE_STORE_U40(void* ptr, uint64_t v){auto bs=__builtin_bswap64(v);memcpy(ptr,&bs,5);}
inline void __LE_STORE_U32(void* ptr, uint32_t v){store_unaligned<uint32_t>(ptr,v);}
inline void __BE_STORE_U32(void* ptr, uint32_t v){store_unaligned<uint32_t>(ptr,__builtin_bswap32(v));}
inline void __LE_STORE_U24(void* ptr, uint32_t v){memcpy(ptr, &v, 3);}
inline void __BE_STORE_U24(void* ptr, uint32_t v){auto bs=__builtin_bswap32(v);memcpy(ptr,&bs,3);}
inline void __LE_STORE_U16(void* ptr, uint16_t v){store_unaligned<uint16_t>(ptr,v);}
inline void __BE_STORE_U16(void* ptr, uint16_t v){store_unaligned<uint16_t>(ptr,__builtin_bswap16(v));}
inline void __LE_STORE_F32_IE3(void* ptr,float v){__LE_STORE_U32(ptr, std::bit_cast<uint32_t>(v));}
inline void __BE_STORE_F32_IE3(void* ptr,float v){__BE_STORE_U32(ptr, std::bit_cast<uint32_t>(v));}
inline void __LE_STORE_F32_NON(void* ptr,float v){__LE_STORE_U32(ptr, F32_CONVERT_NON_IEEE(v));}
inline void __BE_STORE_F32_NON(void* ptr,float v){__BE_STORE_U32(ptr, F32_CONVERT_NON_IEEE(v));}
inline void __LE_STORE_F64_IE3(void*ptr,double v){__LE_STORE_U64(ptr, std::bit_cast<uint64_t>(v));}
inline void __BE_STORE_F64_IE3(void*ptr,double v){__BE_STORE_U64(ptr, std::bit_cast<uint64_t>(v));}
inline void __LE_STORE_F64_NON(void*ptr,double v){__LE_STORE_U64(ptr, F64_CONVERT_NON_IEEE(v));}
inline void __BE_STORE_F64_NON(void*ptr,double v){__BE_STORE_U64(ptr, F64_CONVERT_NON_IEEE(v));}
static std::function<float(const void*)>    __LE_LOAD_F32   = is_ieee754    ? __LE_LOAD_F32_IE3  : __LE_LOAD_F32_NON;
static std::function<float(const void*)>    __BE_LOAD_F32   = is_ieee754    ? __BE_LOAD_F32_IE3  : __BE_LOAD_F32_NON;
static std::function<void(void*,float)>     __LE_STORE_F32  = is_ieee754    ? __LE_STORE_F32_IE3 : __LE_STORE_F32_NON;
static std::function<void(void*,float)>     __BE_STORE_F32  = is_ieee754    ? __BE_STORE_F32_IE3 : __BE_STORE_F32_NON;
static std::function<uint64_t(const void*)> LOAD_U64        = little_endian ? __LE_LOAD_U64  : __BE_LOAD_U64;
static std::function<uint64_t(const void*)> LOAD_U40        = little_endian ? __LE_LOAD_U40  : __BE_LOAD_U40;
static std::function<uint32_t(const void*)> LOAD_U32        = little_endian ? __LE_LOAD_U32  : __BE_LOAD_U32;
static std::function<uint32_t(const void*)> LOAD_U24        = little_endian ? __LE_LOAD_U24  : __BE_LOAD_U24;
static std::function<uint16_t(const void*)> LOAD_U16        = little_endian ? __LE_LOAD_U16  : __BE_LOAD_U16;
static std::function<float(const void*)>    LOAD_F32        = little_endian ? __LE_LOAD_F32  : __BE_LOAD_F32;
static std::function<void(void*,uint64_t)>  STORE_U64       = little_endian ? __LE_STORE_U64 : __BE_STORE_U64;
static std::function<void(void*,uint64_t)>  STORE_U40       = little_endian ? __LE_STORE_U40 : __BE_STORE_U40;
static std::function<void(void*,uint32_t)>  STORE_U32       = little_endian ? __LE_STORE_U32 : __BE_STORE_U32;
static std::function<void(void*,uint32_t)>  STORE_U24       = little_endian ? __LE_STORE_U24 : __BE_STORE_U24;
static std::function<void(void*,uint16_t)>  STORE_U16       = little_endian ? __LE_STORE_U16 : __BE_STORE_U16;
static std::function<void(void*,float)>     STORE_F32       = little_endian ? __LE_STORE_F32 : __BE_STORE_F32;
// Convenience functions that will convert from a serialized 16-bit IEEE 754-2008 half-float
// to whatever internal representation of half precision the system uses
inline _Float16 F16_CONVERT_NON_IEEE (uint16_t val)
{
    float       result = ldexp(1.0+F16_CAST(val & F16_MANTESSA)/F16_IEEE_base,
                               ((val & F16_EXP)>>10) - 15);
    return      val & F16_NEG ? -result : result;
}
inline uint16_t F16_CONVERT_NON_IEEE (_Float16 val)
{
    if (val == 0u)          return 0;
    if (isinf(val))         return 0x7C00;
    if (isnan(val))         return 0x7E00;
    uint32_t    neg         = val < 0 ? F16_NEG : 0;
    int         exp         = 0;
    uint32_t    mantessa    = U16_CAST(round(ldexp(frexp(abs(val), &exp)*2-1.0f, 10)));
    return neg | (((static_cast<int16_t>(exp)+14) << 10)&F16_EXP) | (mantessa&F16_MANTESSA);
}
// Convenience function that will convert from a serialized 32-bit IEEE 754 floating point
// to whatever internal representation of floating points a non-iec559 system uses
inline float F32_CONVERT_NON_IEEE (uint32_t val)
{
    float       result = ldexp(1.0+F32_CAST(val & F32_MANTESSA)/F32_IEEE_base,
                               ((val & F32_EXP)>>23) - 127);
    return      val & F32_NEG ? -result : result;
}
// Convenience function that will convert from whatever internal representation of floating points
// a non-iec559 system uses to a serialized IEEE 754 compliant bit sequence
inline uint32_t F32_CONVERT_NON_IEEE (float val)
{
    if (val == 0u)          return 0;
    if (isinf(val))         return 0x7F800000;
    if (isnan(val))         return 0x7FC00000;
    uint32_t    neg         = val < 0 ? F32_NEG : 0;
    int         exp         = 0;
    uint32_t    mantessa    = U32_CAST(round(ldexp(frexp(abs(val), &exp)*2-1.0f, 23)));
    return neg | (((static_cast<int32_t>(exp)+126) << 23)&F32_EXP) | (mantessa&F32_MANTESSA);
}
// Convenience function that will convert from a serialized 64-bit IEEE 754 double precision
// to whatever internal representation of floating points a non-iec559 system uses
inline double F64_CONVERT_NON_IEEE (uint64_t val)
{
    double      result = ldexp(1.0+static_cast<double>(val & F64_MANTESSA)/F64_IEEE_base,
                               ((val & F64_EXP)>>52) - 1023);
    return      val & F64_NEG ? -result : result;;
}
// Convenience function that will convert from whatever internal representation of double precision
// a non-iec559 system uses to a serialized IEEE 754 compliant bit sequence
inline uint64_t F64_CONVERT_NON_IEEE (double val)
{
    if (val == 0u)          return 0;
    if (isinf(val))         return 0x7FF8000000000000;
    if (isnan(val))         return 0x7FFC000000000000;
    uint64_t    neg         = val < 0 ? F64_NEG : 0;
    int         exp         = 0;
    uint64_t    mantessa    = static_cast<uint64_t>(round(ldexp(frexp(abs(val), &exp)*2-1.0f, 52)));
    return neg | (((static_cast<int64_t>(exp)+1022) << 52)&F64_EXP) | (mantessa&F64_MANTESSA);
}
static constexpr char hex_array [] = {
    '0','1','2','3','4','5','6','7','8','9',
    'A','B','C','D','E','F'};
inline std::string to_hex_string(uint8_t _i)
{
    return std::string {
        '0','x',
        hex_array[_i>>4 &0x0F],
        hex_array[_i    &0x0F],
    };
}
inline std::string to_hex_string(uint16_t _i)
{
    return std::string {
        '0','x',
        hex_array[_i>>12&0x0F],
        hex_array[_i>>8 &0x0F],
        hex_array[_i>>4 &0x0F],
        hex_array[_i    &0x0F],
    };
}
inline std::string to_hex_string(uint32_t _i)
{
    return std::string {
        '0','x',
        hex_array[_i>>28&0x0F],
        hex_array[_i>>24&0x0F],
        hex_array[_i>>20&0x0F],
        hex_array[_i>>16&0x0F],
        hex_array[_i>>12&0x0F],
        hex_array[_i>>8 &0x0F],
        hex_array[_i>>4 &0x0F],
        hex_array[_i    &0x0F],
    };
}
namespace IrisCodec {
constexpr uint32_t IFE_VERSION = IRIS_EXTENSION_MAJOR<<16|IRIS_EXTENSION_MINOR;

#ifndef __EMSCRIPTEN__
bool is_Iris_Codec_file (BYTE* const __base, size_t __size)
{
    using namespace Serialization;
    // There's a great chance that if these pass, it's an Iris file.
    if (LOAD_U32(__base + FILE_HEADER::MAGIC_BYTES_OFFSET) != MAGIC_BYTES) return false;
    if (LOAD_U16(__base + FILE_HEADER::RECOVERY) != RECOVER_HEADER) return false;
    return true;
}
Result validate_file_structure(BYTE *const __base, size_t __size) noexcept
{
//    using namespace Serialization;
    Result result;
    auto __FILE_HEADER  = Serialization::FILE_HEADER    (__size);
    result = __FILE_HEADER.validate_full                (__base);
    if (result != IRIS_SUCCESS) return result;
    
    auto TILE_TABLE = __FILE_HEADER.get_tile_table      (__base);
    result = TILE_TABLE.validate_full                   (__base);
    if (result != IRIS_SUCCESS) return result;
    
    auto METADATA   = __FILE_HEADER.get_metadata        (__base);
    result = METADATA.validate_full                     (__base);
    if (result != IRIS_SUCCESS) return result;

    return IRIS_SUCCESS;
}
Abstraction::File  abstract_file_structure (BYTE* const __base, size_t __size) {
    using namespace Abstraction;

    Abstraction::File abstraction;
    Iris::Result result;
    auto FILE_HEADER        = Serialization::FILE_HEADER(__size);
    
    abstraction.header      = FILE_HEADER.read_header   (__base);
    auto TILE_TABLE         = FILE_HEADER.get_tile_table(__base);
    abstraction.tileTable   = TILE_TABLE.read_tile_table(__base);
    auto METADATA           = FILE_HEADER.get_metadata  (__base);
    abstraction.metadata    = METADATA.read_metadata    (__base);
    
    auto& metadata          = abstraction.metadata;
    if (METADATA.attributes                             (__base))
    {
        auto ATTRIBUES      = METADATA.get_attributes   (__base);
        metadata.attributes = ATTRIBUES.read_attributes (__base);
    }
    if (METADATA.image_array                            (__base))
    {
        auto IMAGES         = METADATA.get_image_array  (__base);
        abstraction.images  = IMAGES.read_assoc_images  (__base);
        for (auto&& image : abstraction.images)
            metadata.associatedImages.insert(image.first);
    }
    if (METADATA.color_profile                          (__base))
    {
        auto ICC_PROFILE    = METADATA.get_color_profile(__base);
        metadata.ICC_profile= ICC_PROFILE.read_profile  (__base);
    }
    if (METADATA.annotations                            (__base))
    {
        auto ANNOTATIONS    = METADATA.get_annotations  (__base);
        abstraction.annotations =
        ANNOTATIONS.read_annotations                    (__base);
        for (auto&& note : abstraction.annotations)
            metadata.annotations.insert (note.first);
    }
    
    return abstraction;
}
Abstraction::FileMap  generate_file_map (BYTE* const __base, size_t __size) {
    using namespace Serialization;
    using namespace Abstraction;

    Abstraction::FileMap map;
    auto __FILE_HEADER  = FILE_HEADER                   (__size);
    __FILE_HEADER.validate_header                       (__base);
    map [__FILE_HEADER.__offset] = {
        .type           = MAP_ENTRY_FILE_HEADER,
        .datablock      = __FILE_HEADER,
        .size           = __FILE_HEADER.size            (__base)
    };
    auto file_header    = __FILE_HEADER.read_header     (__base);
    auto __TILE_TABLE   = __FILE_HEADER.get_tile_table  (__base);
    map [__TILE_TABLE.__offset] = {
        .type           = MAP_ENTRY_TILE_TABLE,
        .datablock      = __TILE_TABLE,
        .size           = __TILE_TABLE.size(),
    };
    auto __EXTENTS      = __TILE_TABLE.get_layer_extents(__base);
    map [__EXTENTS.__offset] = {
        .type           = MAP_ENTRY_LAYER_EXTENTS,
        .datablock      = __EXTENTS,
        .size           = __EXTENTS.size                (__base)
    };
    auto __TILES        = __TILE_TABLE.get_tile_offsets (__base);
    map [__TILES.__offset] = {
        .type           = MAP_ENTRY_TILE_OFFSETS,
        .datablock      = __TILES,
        .size           = __TILES.size                  (__base)
    };
    
    // This is the part that hurts: blocking in all the tiles
    auto table          = __TILE_TABLE.read_tile_table(__base);
    for (auto&& layer : table.layers)
        for (auto&& offset : layer)
            map[offset.offset] = {
                .type       = MAP_ENTRY_TILE_DATA,
                .datablock  = DATA_BLOCK
                (offset.offset,
                 file_header.fileSize,
                 file_header.extVersion),
                .size       = offset.size
            };
    
    auto __METADATA     = __FILE_HEADER.get_metadata    (__base);
    map [__METADATA.__offset] = {
        .type           = MAP_ENTRY_METADATA,
        .datablock      = __METADATA,
        .size           = __METADATA.size()
    };
    if (__METADATA.attributes                           (__base))
    {
        auto __ATTR     = __METADATA.get_attributes     (__base);
        map [__ATTR.__offset] = {
            .type       = MAP_ENTRY_ATTRIBUTES,
            .datablock  = __ATTR,
            .size       = __ATTR.size()
        };
    }
    if (__METADATA.image_array                          (__base))
    {
        auto __ARRAY    = __METADATA.get_image_array    (__base);
        map [__ARRAY.__offset] = {
            .type       = MAP_ENTRY_ASSOCIATED_IMAGES,
            .datablock  = __ARRAY,
            .size       = __ARRAY.size                  (__base)
        };
        std::vector<IMAGE_BYTES> __IMAGE_BYTES;
        __ARRAY.read_assoc_images                       (__base, &__IMAGE_BYTES);
        for (auto&& BYTES:__IMAGE_BYTES) {
            map[BYTES.__offset] = {
                .type   = MAP_ENTRY_ASSOCIATED_IMAGE_BYTES,
                .datablock = BYTES,
                .size   = BYTES.size                    (__base)
            };
        }
    }
    if (__METADATA.color_profile(__base))
    {
        auto __ICC      = __METADATA.get_color_profile  (__base);
        map [__ICC.__offset] = {
            .type       = MAP_ENTRY_ICC_PROFILE,
            .datablock  = __ICC,
            .size       = __ICC.size                    (__base)
        };
    }
    if (__METADATA.annotations                          (__base))
    {
        auto __ANNOT    = __METADATA.get_annotations    (__base);
        map[__ANNOT.__offset] = {
            .type       = MAP_ENTRY_ANNOTATIONS,
            .datablock  = __ANNOT,
            .size       = __ANNOT.size                  (__base)
        };
        std::vector<ANNOTATION_BYTES> __ANNOTATION_BYTES;
        __ANNOT.read_annotations(__base, &__ANNOTATION_BYTES);
        for (auto&& BYTES:__ANNOTATION_BYTES) {
            map[BYTES.__offset] = {
                .type   = MAP_ENTRY_ANNOTATION_BYTES,
                .datablock = BYTES,
                .size   = BYTES.size                    (__base)
            };
        }
        if (__ANNOT.groups                              (__base))
        {
            auto __GRPS = __ANNOT.get_group_sizes       (__base);
            map[__GRPS.__offset] = {
                .type       = MAP_ENTRY_ANNOTATION_GROUP_SIZES,
                .datablock  = __GRPS,
                .size       = __GRPS.size               (__base)
            };
            auto __GRPB = __ANNOT.get_group_bytes       (__base);
            map[__GRPB.__offset] = {
                .type       = MAP_ENTRY_ANNOTATION_GROUP_BYTES,
                .datablock  = __GRPB,
                .size       = __GRPB.size               (__base)
            };
        }
    }
    
    return map;
}
#elif /* WEB ASSEMBLY */ defined __EMSCRIPTEN__
constexpr size_t __ptr_size = sizeof(char*);
struct __Response {
    const size_t len;
    const BYTE* const data;
    explicit __Response (const char* url, const char* src, size_t bytes) :
    len (__ptr_size+bytes),
    data([url,src,bytes]()->BYTE*{
        auto data = (BYTE*)malloc(__ptr_size+bytes);
        if (!data) throw std::runtime_error
            ("Failed to allocate memory for data block response");
        memcpy(data, &url, __ptr_size);
        memcpy(data + __ptr_size, src, bytes);
        return data;
    }()){}
    ~__Response() {
        if (data) free (const_cast<BYTE*>(data));
    }
    const char* url () const {
        return *reinterpret_cast<const char * const *>(data);
    }
};
inline const char* REINTERPRET_ARRAY_START (const BYTE* const array_ptr)
{
    return *reinterpret_cast<const char* const *>(array_ptr);
}
/**
 * @brief Asynchronously fetches a byte range from a URL using JavaScript's fetch API,
 * but appears synchronous to the C++ code due to Asyncify.
 *
 * This function uses EM_ASYNC_JS to bridge the C++ and JavaScript environments. It
 * performs a network fetch request for a specific byte range of a file. The function
 * then copies the fetched data into a newly allocated block of memory on the
 * WebAssembly heap and returns a pointer to this memory.
 *
 * The C++ execution is paused (yielding control to the JavaScript event loop) until
 * the fetch operation completes.
 *
 * @param url_ptr A pointer to a C-style string containing the URL of the file to fetch.
 * @param range_header_ptr A pointer to a C-style string with the "Range" header
 * (e.g., "bytes=1000-1023") to specify the iris datablock byte segment to fetch.
 * @param data_size_ptr A pointer to an integer where the size of the fetched data
 * (in bytes) will be stored.
 * @param status_ptr A pointer to an integer where the HTTP status code of the
 * response (e.g., 200 for OK, 206 for Partial Content) will be stored.
 * @return A pointer to the newly allocated WebAssembly memory block containing the
 * fetched data. Returns a null pointer (0) on failure. The caller is
 * responsible for freeing this memory with `free()` when it's no longer
 * needed.
 */
EM_ASYNC_JS (int, fetch_data_async,
(const char* url_ptr, const char* range_header_ptr, int* data_size_ptr, int* status_ptr), {
  const url_js = UTF8ToString(url_ptr);
  const range_header_js = UTF8ToString(range_header_ptr);

  const request = new Request(url_js, {
    headers: {'Range': range_header_js}
  });

  try {
    const response = await fetch(request);

    // Write the HTTP status code back to the C++ pointer
    HEAP32[status_ptr >> 2] = response.status;

    // If the response is not ok, handle the error and return
    if (!response.ok) {
        HEAP32[data_size_ptr >> 2] = 0;
        return 0;
    }

    const buffer = await response.arrayBuffer();
    const dataSize = buffer.byteLength;
    const dataPtr = _malloc(dataSize);

    HEAPU8.set(new Uint8Array(buffer), dataPtr);

    // Write the data size back to the C++ pointer
    HEAP32[data_size_ptr >> 2] = dataSize;

    return dataPtr;
  } catch (error) {
    // Handle network errors
    console.error("Fetch failed:", error);
    HEAP32[data_size_ptr >> 2] = 0;
    HEAP32[status_ptr >> 2] = 0; // Use a distinct status for network errors
    return 0;
  }
});
inline Response FETCH_DATABLOCK (const char* __url, size_t start, size_t len)
{
    std::string range_header = "bytes="
    + std::to_string(start) + "-"
    + std::to_string(start+len-1);
    
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
    // EXTERNAL JAVASCRIPT CODE (SEE EM_ASYNC_JS ABOVE)
    // Execution is paused (yielding control to the JavaScript event loop)
    // until the fetch operation completes.
    int data_size = 0;
    int status_code = 0;
    char* payload = reinterpret_cast<char*>
    (fetch_data_async(__url, range_header.c_str(), &data_size, &status_code));
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
    
    Response response = nullptr;
    if (payload && status_code == 206) {
        response = std::make_shared<__Response>
        (__url, payload, data_size);
    } else {
        std::cerr   << "[Error] Failed to fetch datablock "
                    << "(HTTP status "<< status_code << ")\n";
    }
    if (payload) free (payload);
    return response;
}
bool is_Iris_Codec_file (const std::string url, size_t __size)
{
    using namespace Serialization;
    
    auto response = FETCH_DATABLOCK(url.c_str(), 0, FILE_HEADER::HEADER_SIZE);
    
    // There's a great chance that if these pass, it's an Iris file.
    if (!response) return false;
    auto __data = response->data + __ptr_size;
    if (LOAD_U32(__data + FILE_HEADER::MAGIC_BYTES_OFFSET) != MAGIC_BYTES) return false;
    if (LOAD_U16(__data + FILE_HEADER::RECOVERY) != RECOVER_HEADER) return false;
    return true;
}
Result validate_file_structure(const std::string url, size_t __size) noexcept
{
    using namespace Serialization;
    Result result;
    
    auto response = FETCH_DATABLOCK(url.c_str(), 0, FILE_HEADER::HEADER_SIZE);
    if (!response) return Result
        (IRIS_FAILURE,
         "Failed to fetch Iris file header from remote endpoint ("+url+")");
    const BYTE* __base = response->data;
    
    auto __FILE_HEADER  = Serialization::FILE_HEADER    (__size);
    result = __FILE_HEADER.validate_full                (__base);
    if (result != IRIS_SUCCESS) return result;

    auto TILE_TABLE = __FILE_HEADER.get_tile_table      (__base);
    result = TILE_TABLE.validate_full                   (__base);
    if (result != IRIS_SUCCESS) return result;

    auto METADATA   = __FILE_HEADER.get_metadata        (__base);
    result = METADATA.validate_full                     (__base);
    if (result != IRIS_SUCCESS) return result;

    return IRIS_SUCCESS;
}
Abstraction::File  abstract_file_structure (const std::string url, size_t __size) {
    using namespace Abstraction;
    using namespace Serialization;
    
    Abstraction::File abstraction;
    Iris::Result result;
    
    auto response = FETCH_DATABLOCK(url.c_str(), 0, FILE_HEADER::HEADER_SIZE);
    if (!response) throw std::runtime_error
        ("Failed to fetch Iris file header from remote endpoint ("+url+")");
    const BYTE* __base = response->data;
    
    auto FILE_HEADER        = Serialization::FILE_HEADER(__size);
    abstraction.header      = FILE_HEADER.read_header   (__base);
    auto TILE_TABLE         = FILE_HEADER.get_tile_table(__base);
    abstraction.tileTable   = TILE_TABLE.read_tile_table(__base);
    auto METADATA           = FILE_HEADER.get_metadata  (__base);
    abstraction.metadata    = METADATA.read_metadata    (__base);

    auto& metadata          = abstraction.metadata;
    if (METADATA.attributes                             (__base))
    {
        auto ATTRIBUES      = METADATA.get_attributes   (__base);
        metadata.attributes = ATTRIBUES.read_attributes (__base);
    }
    if (METADATA.image_array                            (__base))
    {
        auto IMAGES         = METADATA.get_image_array  (__base);
        abstraction.images  = IMAGES.read_assoc_images  (__base);
        for (auto&& image : abstraction.images)
            metadata.associatedImages.insert(image.first);
    }
    if (METADATA.color_profile                          (__base))
    {
        auto ICC_PROFILE    = METADATA.get_color_profile(__base);
        metadata.ICC_profile= ICC_PROFILE.read_profile  (__base);
    }
    if (METADATA.annotations                            (__base))
    {
        auto ANNOTATIONS    = METADATA.get_annotations  (__base);
        abstraction.annotations =
        ANNOTATIONS.read_annotations                    (__base);
        for (auto&& note : abstraction.annotations)
            metadata.annotations.insert (note.first);
    }
    return abstraction;
}
#endif
namespace Serialization {
inline bool VALIDATE_ENCODING_TYPE (Encoding encoding, uint32_t __version) {
    switch (encoding) {
        case TILE_ENCODING_IRIS:
        case TILE_ENCODING_JPEG:
        case TILE_ENCODING_AVIF:    return true;
        default:                    break;
    }
    
    if (__version > IRIS_EXTENSION_1_0); else return false;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return false;
}
inline bool VALIDATE_PIXEL_FORMAT (Format format, uint32_t __version) {
    switch (format) {
        case Iris::FORMAT_B8G8R8:
        case Iris::FORMAT_R8G8B8:
        case Iris::FORMAT_B8G8R8A8:
        case Iris::FORMAT_R8G8B8A8: return true;
        default:                    break;
    }
    
    if (__version > IRIS_EXTENSION_1_0); else return false;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return false;
    
}
inline bool VALIDATE_METADATA_TYPE (MetadataType type, uint32_t __version)
{
    switch (type) {
        case METADATA_I2S:
        case METADATA_DICOM:    return true;
        default:                break;
    }
    
    if (__version > IRIS_EXTENSION_1_0); else return false;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return false;
}
inline bool VALIDATE_IMAGE_ENCODING_TYPE (ImageEncoding encoding, uint32_t __version)
{
    switch (encoding) {
        case IMAGE_ENCODING_PNG:
        case IMAGE_ENCODING_JPEG:
        case IMAGE_ENCODING_AVIF:   return true;
        default:                    break;
    }
     
    if (__version > IRIS_EXTENSION_1_0); else return false;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return false;
}
inline bool VALIDATE_ANNOTATION_TYPE (AnnotationTypes type, uint32_t __version)
{
    switch (type) {
        case Iris::ANNOTATION_PNG:
        case Iris::ANNOTATION_JPEG:
        case Iris::ANNOTATION_SVG:
        case Iris::ANNOTATION_TEXT: return true;
        default:                    break;
    }
    
    if (__version > IRIS_EXTENSION_1_0); else return false;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return false;
    
}
// MARK: - DATA_BLOCK
#ifndef __EMSCRIPTEN__
 DATA_BLOCK::DATA_BLOCK (Offset offset, Size file_size, uint32_t IFE_version) :
__offset    (offset),
__size      (file_size),
__version   (IFE_version)
{
    
}
#elif defined __EMSCRIPTEN__
DATA_BLOCK::DATA_BLOCK (Offset offset, Size file_size, uint32_t IFE_version) :
__remote    (offset),
__size      (file_size),
__version   (IFE_version)
{
    
}
#endif
 DATA_BLOCK::operator bool() const
{
    return __offset != NULL_OFFSET && __offset < __size;
}
Result  DATA_BLOCK::validate_offset (const BYTE *const __base, const char* __type, enum RECOVERY __recovery) const noexcept
{
    const auto _type = std::string(__type);
    if (!*this) return Result
        (IRIS_VALIDATION_FAILURE, "Invalid "+_type+" object. The "+_type+" was not created with a valid offset value.");
#ifndef __EMSCRIPTEN__
    if (LOAD_U64 (__base + __offset + VALIDATION) != __offset) return Result
        (IRIS_VALIDATION_FAILURE, _type+" failed offset validation. The VALIDATION value (" +
         std::to_string(LOAD_U64 (__base + __offset + VALIDATION)) +
         ") is not the offset location ("+
         std::to_string(__offset)+")");
#elif defined __EMSCRIPTEN__
    if (LOAD_U64(__base + __offset + VALIDATION) != __remote) return Result
        (IRIS_VALIDATION_FAILURE, _type+" failed offset validation. The VALIDATION value (" +
         std::to_string(LOAD_U64 (__base + __offset + VALIDATION)) +
         ") is not the offset location ("+
         std::to_string(__remote)+")");
#endif
    if (LOAD_U16 (__base + __offset + RECOVERY) != __recovery) return Result
        (IRIS_VALIDATION_FAILURE, "RECOVER_"+_type+" ("+
         to_hex_string(__recovery)+
         ") tag failed validation. The tag value is ("+
         to_hex_string(LOAD_U16 (__base + __offset + RECOVERY))+")");
    
    return IRIS_SUCCESS;
}
// MARK: - FILE HEADER
FILE_HEADER::FILE_HEADER (Size __file_size) noexcept :
DATA_BLOCK (HEADER_OFFSET, __file_size, UINT32_MAX)
{
    
}
Size FILE_HEADER::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<FILE_HEADER&>(*this).check_and_fetch_remote(__base);
#endif
    validate_header(__base);
    uint32_t version =  LOAD_U16(__base + __offset + EXTENSION_MAJOR) << 16 |
                        LOAD_U16(__base + __offset + EXTENSION_MINOR);
    Size size = HEADER_V1_0_SIZE;
    if (version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result FILE_HEADER::validate_header(const BYTE* __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<FILE_HEADER&>(*this).check_and_fetch_remote(__base);
#endif
    if (!*this) return Result
        (IRIS_VALIDATION_FAILURE,"Invalid file header size. The header must be created with the OS returned file size.");
    if (LOAD_U32(__base + __offset + MAGIC_BYTES_OFFSET) != MAGIC_BYTES) return Result
        (IRIS_FAILURE,"Iris File Magic Number failed validation");
    if (LOAD_U16(__base + __offset + RECOVERY) != RECOVER_HEADER) return Result
        (IRIS_VALIDATION_FAILURE,"RECOVER_HEADER ("+
         std::to_string(RECOVER_HEADER)+
         ") tag failed validation. The tag value is ("+
         std::to_string(LOAD_U16 (__base + RECOVERY))+")");
    
    size_t size = LOAD_U64(__base + __offset + FILE_SIZE);
    if (size != __size) return Result
        (IRIS_VALIDATION_FAILURE,"The internally stored Iris file size (" +
         std::to_string(size) +
         " bytes) differs from that provided by the operating system (" +
         std::to_string(__size) +
         " bytes). This failure requires file recovery.");
    
    Result result;
    uint16_t major = LOAD_U32(__base + __offset + EXTENSION_MAJOR);
    uint16_t minor = LOAD_U32(__base + __offset + EXTENSION_MINOR);
    if (major > IRIS_EXTENSION_MAJOR || minor > IRIS_EXTENSION_MINOR) {
        result.flag = (ResultFlag) (result.flag | IRIS_WARNING_VALIDATION);
        result.message = "This Iris Extension Version ("+
        std::to_string(IRIS_EXTENSION_MAJOR) + "." +
        std::to_string(IRIS_EXTENSION_MINOR) +
        ") is is less than the extension version used to generate the slide file ("+
        std::to_string(major) + "." +
        std::to_string(minor) +
        "). This may limit additional features encoded in the file by restricting decoding to only those present in the current encoder version " +
        std::to_string(IRIS_EXTENSION_MAJOR) + "." +
        std::to_string(IRIS_EXTENSION_MINOR) +
        ". Please upgrade your implementation of the standard to access the full set of parameters encoded in this slide file.";
    }
    
    return IRIS_SUCCESS;
}
Result FILE_HEADER::validate_full (const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<FILE_HEADER&>(*this).check_and_fetch_remote(__base);
#endif
    auto result = validate_header (__base);
    if (result & IRIS_FAILURE) return result;
    else if (result & IRIS_WARNING) printf
        ("Tile table validation WARNING: %s", result.message.c_str());
    
    uint32_t version =  LOAD_U16(__base + __offset + EXTENSION_MAJOR) << 16 |
                        LOAD_U16(__base + __offset + EXTENSION_MINOR);
    
    Offset offset;
    offset = LOAD_U64(__base + __offset + TILE_TABLE_OFFSET);
    auto __TILE_TABLE = TILE_TABLE(offset, __size, version);
    result = __TILE_TABLE.validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    offset = LOAD_U64(__base + __offset + METADATA_OFFSET);
    auto __METADATA = METADATA(offset, __size, version);
    result = __METADATA.validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    if (version > IRIS_EXTENSION_1_0); else return result;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return result;
}
Header FILE_HEADER::read_header(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<FILE_HEADER&>(*this).check_and_fetch_remote(__base);
#endif
    Header header;
    Result result = validate_header(__base);
    if (result & IRIS_FAILURE)
        throw std::runtime_error(result.message);
    
    header.fileSize     = LOAD_U64(__base + __offset + FILE_HEADER::FILE_SIZE);
    header.extVersion   = LOAD_U16(__base + __offset + EXTENSION_MAJOR) << 16 |
                          LOAD_U16(__base + __offset + EXTENSION_MINOR);
    header.revision     = LOAD_U32(__base + __offset + FILE_REVISION);
    if (header.extVersion > IRIS_EXTENSION_1_0); else return header;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return header;
}
TILE_TABLE FILE_HEADER::get_tile_table (const BYTE* const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<FILE_HEADER&>(*this).check_and_fetch_remote(__base);
#endif
    const auto header       = read_header(__base);
    if (header.extVersion == 0) throw std::runtime_error
        ("Failed to retrieve tile table. Invalid file header");
    const auto __TILE_TABLE = TILE_TABLE
    (LOAD_U64(__base + __offset + TILE_TABLE_OFFSET), __size, header.extVersion);
    
    const auto result = __TILE_TABLE.validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed to retrieve tile table: " + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve tile table WARNING: %s", result.message.c_str());
    
    return __TILE_TABLE;
}
METADATA FILE_HEADER::get_metadata (const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<FILE_HEADER&>(*this).check_and_fetch_remote(__base);
#endif
    const auto header       = read_header(__base);
    if (header.extVersion == 0) throw std::runtime_error
        ("Failed to retrieve clinical metadata. Invalid file header");
    const auto __METADATA   = METADATA
    (LOAD_U64(__base + __offset + METADATA_OFFSET), __size, header.extVersion);
    
    const auto result = __METADATA.validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed to validate clinical metadata: " + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve clinical metadata WARNING: %s", result.message.c_str());
    
    return __METADATA;
}
#ifdef __EMSCRIPTEN__
void FILE_HEADER::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote,HEADER_SIZE);
        __offset        = __ptr_size;
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
void STORE_FILE_HEADER (BYTE *const __base, const HeaderCreateInfo &__CI)
{
    if (!__CI.fileSize) throw std::runtime_error
        ("Failed STORE_FILE_HEADER validation -- no file size provided. Per the IFE specification Section (2.3.1), the file size shall be encoded as a unsigned 64-bit integer identical to the operating system query for the file size in bytes.");
    Result result;
    DATA_BLOCK blk_validation (NULL_OFFSET, __CI.fileSize, IFE_VERSION);
    
    // Perform a FULL validation of the file structure: Tile Table
    blk_validation.__offset = __CI.tileTableOffset;
    result = static_cast<TILE_TABLE&>(blk_validation).validate_full(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed STORE_FILE_HEADER full validation check -- "
         + result.message
         + "\nPer the IFE specification Section (2.3.1), the tile table offset shall contain the file offset location of a valid tile table header (defined in subsection 2.3.2).");
    if (result & IRIS_WARNING)
        printf("STORE_FILE_HEADER validation WARNING: %s", result.message.c_str());
    
    // Perform a FULL validation of the file structure: Tile Table
    blk_validation.__offset = __CI.metadataOffset;
    result =  static_cast<METADATA&>(blk_validation).validate_full(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed STORE_FILE_HEADER full validation check -- " +
         result.message +
         "\nPer the IFE specification Section (2.3.1), the clinical metadata offset shall contain the file offset location of a valid metadata header (defined in subsection 2.3.3)");
    if (result & IRIS_WARNING)
        printf("STORE_FILE_HEADER clinical metadata validation WARNING: %s", result.message.c_str());
    
    STORE_U32   (__base + FILE_HEADER::MAGIC_BYTES_OFFSET,  MAGIC_BYTES);
    STORE_U16   (__base + FILE_HEADER::RECOVERY,            RECOVER_HEADER);
    STORE_U64   (__base + FILE_HEADER::FILE_SIZE,           __CI.fileSize);
    STORE_U16   (__base + FILE_HEADER::EXTENSION_MAJOR,     IRIS_EXTENSION_MAJOR);
    STORE_U16   (__base + FILE_HEADER::EXTENSION_MINOR,     IRIS_EXTENSION_MINOR);
    STORE_U32   (__base + FILE_HEADER::FILE_REVISION,       __CI.revision);
    STORE_U64   (__base + FILE_HEADER::TILE_TABLE_OFFSET,   __CI.tileTableOffset);
    STORE_U64   (__base + FILE_HEADER::METADATA_OFFSET,     __CI.metadataOffset);
}
#endif

// MARK: - TILE TABLE
TILE_TABLE::TILE_TABLE (Offset __TileTable_Offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(__TileTable_Offset, file_size, version)
{
    
}
Size TILE_TABLE::size() const
{
    Size size = HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result TILE_TABLE::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<TILE_TABLE&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result TILE_TABLE::validate_full(const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<TILE_TABLE&>(*this).check_and_fetch_remote(__base);
#endif
    Result result = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    else if (result & IRIS_WARNING) printf
        ("Tile table validation WARNING: %s", result.message.c_str());
    
    const auto __ptr = __base + __offset;
    if (VALIDATE_ENCODING_TYPE((Encoding)LOAD_U8(__ptr + ENCODING), __version) == false) return Result
        (IRIS_VALIDATION_FAILURE,"Undefined tile encoding value (" +
         to_hex_string((Encoding)LOAD_U8(__ptr + ENCODING))+
         ") decoded from tile table. Per the IFE specification Section 2.3.2, enumeration shall refer to the algorithm / specification used to compress the slide tile data and be one of the enumerated values (Enumeration 2.2.3), excluding the undefined value (0)");
    
    if (VALIDATE_PIXEL_FORMAT((Format)LOAD_U8(__ptr + FORMAT), __version) == false) return Result
        (IRIS_VALIDATION_FAILURE,"Undefined tile pixel format (" +
         to_hex_string((Format)LOAD_U8(__ptr + FORMAT))+
         ") decoded from tile table. Per the IFE specification Section 2.3.2, the format shall describe the pixel channel ordering and bits consumed per channel per the accepted norm using one of the defined enumerated values (Enumeration 2.2.4), excluding the undefined value (0).");

    auto offset = LOAD_U64(__ptr + LAYER_EXTENTS_OFFSET);
    const auto __LAYER_EXTENTS = LAYER_EXTENTS(offset, __size, __version);
    result = __LAYER_EXTENTS.validate_full (__base);
    if (result & IRIS_VALIDATION_FAILURE) return result;
    
    offset = LOAD_U64(__ptr + TILE_OFFSETS_OFFSET);
    const auto __TILE_OFFSETS = TILE_OFFSETS(offset, __size, __version);
    result = __TILE_OFFSETS.validate_full (__base);
    if (result & IRIS_VALIDATION_FAILURE) return result;
    
    return IRIS_SUCCESS;
}
TileTable TILE_TABLE::read_tile_table(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<TILE_TABLE&>(*this).check_and_fetch_remote(__base);
#endif
    TileTable tile_table;
    
    const auto  __ptr           = __base + __offset;
    tile_table.encoding         = (Encoding)LOAD_U8(__ptr + ENCODING);
    if (VALIDATE_ENCODING_TYPE(tile_table.encoding, __version) == false)
        throw std::runtime_error
        ("Undefined tile encoding value (" +
         std::to_string((Encoding)LOAD_U8(__ptr + ENCODING))+
         ") decoded from tile table.");
    
    tile_table.format           = (Format)LOAD_U8(__ptr + FORMAT);
    if (VALIDATE_PIXEL_FORMAT(tile_table.format, __version) == false)
        throw std::runtime_error
        ("Undefined tile pixel format (" +
         std::to_string((Format)LOAD_U8(__ptr + FORMAT))+
         ") decoded from tile table.");
    
    tile_table.extent.width     = LOAD_U32(__ptr + X_EXTENT);
    tile_table.extent.height    = LOAD_U32(__ptr + Y_EXTENT);
    
    // Pull the layer extents from the file
    LAYER_EXTENTS EXTENTS       = get_layer_extents(__base);
    tile_table.extent.layers    = EXTENTS.read_layer_extents(__base);
    
    // Then populate the offset array with the tile byte offset info
    TILE_OFFSETS  OFFSETS       = get_tile_offsets(__base);
    OFFSETS.read_tile_offsets(__base, tile_table);
    
    
    if (__version > IRIS_EXTENSION_1_0); else return tile_table;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return tile_table;
}
TILE_OFFSETS TILE_TABLE::get_tile_offsets(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<TILE_TABLE&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __TILE_OFFSETS = TILE_OFFSETS
    (LOAD_U64(__base + __offset + TILE_OFFSETS_OFFSET), __size, __version);
    
    const auto result = __TILE_OFFSETS.validate_offset(__base);
    if (result & IRIS_VALIDATION_FAILURE) throw std::runtime_error
        ("Failed to retrieve tile offset array:" + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve tile offset array WARNING: %s", result.message.c_str());
    
    return __TILE_OFFSETS;
}
LAYER_EXTENTS TILE_TABLE::get_layer_extents(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<TILE_TABLE&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __LAYER_EXTENTS = LAYER_EXTENTS
    (LOAD_U64(__base + __offset + LAYER_EXTENTS_OFFSET), __size, __version);
    
    const auto result = __LAYER_EXTENTS.validate_offset(__base);
    if (result & IRIS_VALIDATION_FAILURE) throw std::runtime_error
        ("Failed to retrieve layer extents array:" + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve layer extents array WARNING: %s", result.message.c_str());
    
    return __LAYER_EXTENTS;
}
#ifdef __EMSCRIPTEN__
void TILE_TABLE::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
void STORE_TILE_TABLE (BYTE *const __base, const TileTableCreateInfo &__CI)
{
    if (__CI.tileTableOffset == NULL_OFFSET) throw std::runtime_error
        ("Failed STORE_TILE_TABLE header -- invalid tileTableOffset in TileTableCreateInfo.");
    
    #if IrisCodecExtensionValidateEncoding
    if (VALIDATE_ENCODING_TYPE(__CI.encoding, IFE_VERSION) == false) throw std::runtime_error
        ("Undefined Tile Table tile encoding value ("+to_hex_string(__CI.encoding) +
         ") in TileTableCreateInfo. Per the IFE specification Section 2.3.2, the enumeration shall refer to the algorithm / specification used to compress the slide tile data and be one of the enumerated values (Enumeration 2.2.3), excluding the undefined value (0)");
    
    if (VALIDATE_PIXEL_FORMAT(__CI.format, IFE_VERSION) == false &&
        __CI.format!=FORMAT_UNDEFINED) throw std::runtime_error
        ("Undefined Tile Table tile format value ("+to_hex_string(__CI.format) +
         ") in TileTableCreateInfo. Per the IFE specification Section 2.3.2, format shall describe the pixel channel ordering and bits consumed per channel per the accepted norm using one of the defined enumerated values (Enumeration 2.2.4), or may encode the undefined value (0)");
    else if (__CI.format == FORMAT_UNDEFINED) printf
        ("WARNING Tile Table tile format value set to FORMAT_UNDEFINED (0x00). Per the IFE specification Section 2.3.2, while this is permitted, encoding the source pixel format is recommended by the standards committee.");

    Result result;
    DATA_BLOCK blk_validation (NULL_OFFSET, UINT64_MAX, IFE_VERSION);
    
    blk_validation.__offset = __CI.tilesOffset;
    result = static_cast<TILE_OFFSETS&>(blk_validation).validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed STORE_TILE_TABLE header -- Invalid TileTableCreateInfo tilesOffset ("+
         result.message +
         ").\nPer the IFE specification Section 2.3.2, the tile offsets shall contain a valid offset to the tile offsets array (Section 2.4.2) containing the byte offsets and sizes of each encoded tile");
    
    blk_validation.__offset = __CI.layerExtentsOffset;
    result = static_cast<LAYER_EXTENTS&>(blk_validation).validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed STORE_TILE_TABLE header -- Invalid TileTableCreateInfo layerExtentsOffset ("+
         result.message +
         ").\nPer the IFE specification  Section 2.3.2, layer extents shall contain a valid offset to the layer extents array (Section 2.4.2) containing the number of tiles and scale of each layer");
    #endif
    
    const auto __ptr = __base + __CI.tileTableOffset;
    STORE_U64   (__ptr + TILE_TABLE::VALIDATION,            __CI.tileTableOffset);
    STORE_U16   (__ptr + TILE_TABLE::RECOVERY,              RECOVER_TILE_TABLE);
    STORE_U8    (__ptr + TILE_TABLE::ENCODING,              __CI.encoding);
    STORE_U8    (__ptr + TILE_TABLE::FORMAT,                __CI.format);
    STORE_U64   (__ptr + TILE_TABLE::CIPHER_OFFSET,         NULL_OFFSET);
    STORE_U64   (__ptr + TILE_TABLE::TILE_OFFSETS_OFFSET,   __CI.tilesOffset);
    STORE_U64   (__ptr + TILE_TABLE::LAYER_EXTENTS_OFFSET,  __CI.layerExtentsOffset);
    STORE_U32   (__ptr + TILE_TABLE::X_EXTENT,              __CI.widthPixels);
    STORE_U32   (__ptr + TILE_TABLE::Y_EXTENT,              __CI.heightPixels);
    
}
#endif
// MARK: - METADATA
METADATA::METADATA  (Offset __metadata_offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(__metadata_offset, file_size, version)
{
    
}
Size METADATA::size() const
{
    Size size = HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result METADATA::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result METADATA::validate_full(const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto result = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    const auto __ptr = __base + __offset;
    if (attributes(__base)) {
        auto __ATTRIBUTES = ATTRIBUTES
        (LOAD_U64(__ptr + ATTRIBUTES_OFFSET), __size, __version);
        result = __ATTRIBUTES.validate_full(__base);
        if (result & IRIS_FAILURE) return result;
    }
    
    if (image_array(__base)) {
        auto __IMAGES = IMAGE_ARRAY
        (LOAD_U64(__ptr + IMAGES_OFFSET), __size, __version);
        result = __IMAGES.validate_full(__base);
        if (result & IRIS_FAILURE) return result;
    }
    
    if (color_profile(__base)) {
        auto _ICC = ICC_PROFILE
        (LOAD_U64(__ptr + ICC_COLOR_OFFSET), __size, __version);
        result = _ICC.validate_full(__base);
        if (result & IRIS_FAILURE) return result;
    }
    
    if (annotations(__base)) {
        auto __ANNOTATIONS = ANNOTATIONS
        (LOAD_U64(__ptr + IMAGES_OFFSET), __size, __version);
        result = __ANNOTATIONS.validate_full(__base);
        if (result & IRIS_FAILURE) return result;
    }
    
    if (__version > IRIS_EXTENSION_1_0); else return result;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return result;
}
Metadata METADATA::read_metadata (const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    Metadata metadata;
    // Validate the offset of this metadata object
    // Return a blank metadata block on failure
    auto result = validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error(result.message);
    
    const auto  __ptr           = __base + __offset;
    metadata.codec.major        = LOAD_U16(__ptr + CODEC_MAJOR);
    metadata.codec.minor        = LOAD_U16(__ptr + CODEC_MINOR);
    metadata.codec.build        = LOAD_U16(__ptr + CODEC_BUILD);
    metadata.micronsPerPixel    = LOAD_F32(__ptr + MICRONS_PIXEL);
    metadata.magnification      = LOAD_F32(__ptr + MAGNIFICATION);
    
    if (__version > IRIS_EXTENSION_1_0); else return metadata;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return metadata;
}
bool METADATA::attributes(const BYTE *const __base) const {
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto offset = LOAD_U64(__base + __offset + ATTRIBUTES_OFFSET);
    return offset != NULL_OFFSET && offset < __size;
}
ATTRIBUTES METADATA::get_attributes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto __ATTRIBUTES = ATTRIBUTES
    (LOAD_U64(__base + __offset + ATTRIBUTES_OFFSET), __size, __version);
    
    const auto result = __ATTRIBUTES.validate_offset(__base);
    if (result & IRIS_VALIDATION_FAILURE) throw std::runtime_error
        ("Failed to retrieve attributes data-block:" + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve attributes data-block WARNING: %s", result.message.c_str());
    
    return __ATTRIBUTES;
}
bool METADATA::image_array(const BYTE *const __base) const {
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto offset = LOAD_U64(__base + __offset + IMAGES_OFFSET);
    return offset != NULL_OFFSET && offset < __size;
}
IMAGE_ARRAY METADATA::get_image_array(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto __IMAGES = IMAGE_ARRAY
    (LOAD_U64(__base + __offset + IMAGES_OFFSET), __size, __version);
    
    const auto result = __IMAGES.validate_offset(__base);
    if (result & IRIS_VALIDATION_FAILURE) throw std::runtime_error
        ("Failed to retrieve associated images array:" + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve associated images array WARNING: %s", result.message.c_str());
    
    return __IMAGES;
}
bool METADATA::color_profile(const BYTE *const __base) const {
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto offset = LOAD_U64(__base + __offset + ICC_COLOR_OFFSET);
    return offset != NULL_OFFSET && offset < __size;
}
ICC_PROFILE METADATA::get_color_profile(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto __ICC = ICC_PROFILE
    (LOAD_U64(__base + __offset + ICC_COLOR_OFFSET), __size, __version);
    
    const auto result = __ICC.validate_offset(__base);
    if (result & IRIS_VALIDATION_FAILURE) throw std::runtime_error
        ("Failed to retrieve ICC profile buffer:" + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve ICC profile buffer WARNING: %s", result.message.c_str());
    
    return __ICC;
}
bool METADATA::annotations(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto offset = LOAD_U64(__base + __offset + ANNOTATIONS_OFFSET);
    return offset != NULL_OFFSET && offset < __size;
}
ANNOTATIONS METADATA::get_annotations(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<METADATA&>(*this).check_and_fetch_remote(__base);
#endif
    auto __ANNOTATIONS = ANNOTATIONS
    (LOAD_U64(__base + __offset + ANNOTATIONS_OFFSET), __size, __version);
    
    const auto result = __ANNOTATIONS.validate_offset(__base);
    if (result & IRIS_VALIDATION_FAILURE) throw std::runtime_error
        ("Failed to retrieve annotations array:" + result.message);
    else if (result & IRIS_WARNING) printf
        ("Retrieve annotations array WARNING: %s", result.message.c_str());
    
    return __ANNOTATIONS;
}
#ifdef __EMSCRIPTEN__
void METADATA::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
void STORE_METADATA (BYTE *const __base, const MetadataCreateInfo &__CI)
{
    
    if (__CI.metadataOffset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store METADATA header -- invalid (NULL_OFFSET) metadataOffset in MetadataCreateInfo.\n");
    
    #if IrisCodecExtensionValidateEncoding
    Result result;
    DATA_BLOCK blk_validation (NULL_OFFSET, UINT64_MAX, IFE_VERSION);
    if (__CI.attributes != NULL_OFFSET) {
        blk_validation.__offset = __CI.attributes;
        result = static_cast<ATTRIBUTES&>(blk_validation).validate_offset(__base);
        if (result & IRIS_FAILURE) throw std::runtime_error
            ("Failed STORE_METADATA -- Invalid attributes header offset (" +
             result.message +
             "). Per the IFE specification section 2.3.4, Attributes (offset) should point to a valid attribute header (Section 2.3.5) or shall be NULL_OFFSET if no attributes are encoded.");
    }
    if (__CI.images != NULL_OFFSET) {
        blk_validation.__offset = __CI.images;
        result = static_cast<IMAGE_ARRAY&>(blk_validation).validate_offset(__base);
        if (result & IRIS_FAILURE) throw std::runtime_error
            ("Failed STORE_METADATA -- Invalid ancillary images array offset (" +
             result.message +
             "). Per the IFE specification section 2.3.4, Images should point to any associated images in an images array (Section 2.4.6) or shall be NULL_OFFSET if no associated images are encoded.");
    }
    if (__CI.ICC_profile != NULL_OFFSET) {
        blk_validation.__offset = __CI.ICC_profile;
        result = static_cast<ICC_PROFILE&>(blk_validation).validate_offset(__base);
        if (result & IRIS_FAILURE) throw std::runtime_error
            ("Failed STORE_METADATA -- Invalid ICC profile byte array offset (" +
             result.message +
             "). Per the IFE specification section 2.3.4, ICC color space may point to a byte array object of type ICC color space (Section 2.4.8) or shall be NULL_OFFSET if no ICC color space is encoded.");
    }
    if (__CI.annotations != NULL_OFFSET) {
        blk_validation.__offset = __CI.annotations;
        result = static_cast<ANNOTATIONS&>(blk_validation).validate_offset(__base);
        if (result & IRIS_FAILURE) throw std::runtime_error
            ("Failed STORE_METADATA -- Invalid slide annotations array offset (" +
             result.message +
             "). Per the IFE specification section 2.3.4, Annotations may point to a byte array object of type Annotations (Section 2.4.9) or shall be NULL_OFFSET if no annotations are present.");
    }
    if (__CI.micronsPerPixel == 0.f) printf
        ("WARNING: MetadataCreateInfo passed to STORE_METADATA has a micronsPerPixel parameter value of zero (0.f). Per the IFE specification section 2.3.4, microns per pixel should encode a floating point coefficient that describes the number of microns of physical space each pixel of the highest resolution layer occupies but may encode a value of zero (0.f) if no value is available");
    if (__CI.magnification == 0.f) printf
        ("WARNING: MetadataCreateInfo passed to STORE_METADATA has a Magnification parameter value of zero (0.f). Per the IFE specification section 2.3.4, magnification should encode a floating point coefficient that converts layer scale to optical magnification corresponding to physical microscopes but may encode a value of zero (0.f) if no value is available.");
    #endif
    
    Offset offset   = __CI.metadataOffset;
    auto   __ptr    = __base + offset;
    STORE_U64   (__ptr + METADATA::VALIDATION,          __CI.metadataOffset);
    STORE_U16   (__ptr + METADATA::RECOVERY,            RECOVER_METADATA);
    STORE_U16   (__ptr + METADATA::CODEC_MAJOR,         __CI.codecVersion.major);
    STORE_U16   (__ptr + METADATA::CODEC_MINOR,         __CI.codecVersion.minor);
    STORE_U16   (__ptr + METADATA::CODEC_BUILD,         __CI.codecVersion.build);
    STORE_U64   (__ptr + METADATA::ATTRIBUTES_OFFSET,   __CI.attributes);
    STORE_U64   (__ptr + METADATA::IMAGES_OFFSET,       __CI.images);
    STORE_U64   (__ptr + METADATA::ICC_COLOR_OFFSET,    __CI.ICC_profile);
    STORE_U64   (__ptr + METADATA::ANNOTATIONS_OFFSET,  __CI.annotations);
    STORE_F32   (__ptr + METADATA::MICRONS_PIXEL,       __CI.micronsPerPixel);
    STORE_F32   (__ptr + METADATA::MAGNIFICATION,       __CI.magnification);
}
#endif

// MARK: - ATTRIBUTES
ATTRIBUTES::ATTRIBUTES  (Offset attributes, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(attributes, file_size, version)
{
    
}
Size ATTRIBUTES::size() const
{
    Size size = HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ATTRIBUTES::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result ATTRIBUTES::validate_full(const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    const auto  __ptr   = __base + __offset;
    if (VALIDATE_METADATA_TYPE((MetadataType)LOAD_U8(__ptr + FORMAT), __version) == false)
        return Result
        (IRIS_FAILURE, "Undefined tile metadata format ("+
         std::to_string((Format)LOAD_U8(__ptr + FORMAT)) +
         ") decoded from attributes header. Per the IFE specification Section 2.3.5, The metadata format shall refer to the metadata specification format by which the file metadata was encoded and shall be one of the metadata formats (Enumeration 2.2.5), excluding the undefined value (0).");
    
    Size expected_bytes;
    const auto __LENGTHS = ATTRIBUTES_SIZES
    (LOAD_U64(__base + __offset + LENGTHS_OFFSET),__size, __version);
    result = __LENGTHS.validate_full(__base, expected_bytes);
    if (result & IRIS_FAILURE) return result;
    
    const auto __BYTES = ATTRIBUTES_BYTES
    (LOAD_U64(__base + __offset + BYTE_ARRAY_OFFSET),__size, __version);
    result = __BYTES.validate_full(__base, expected_bytes);
    if (result & IRIS_FAILURE) return result;
    
    return result;
}
Attributes ATTRIBUTES::read_attributes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES&>(*this).check_and_fetch_remote(__base);
#endif
    Attributes attributes;
    
    const auto  __ptr   = __base + __offset;
    attributes.type     = (MetadataType)LOAD_U8(__ptr + ATTRIBUTES::FORMAT);
    if (VALIDATE_METADATA_TYPE(attributes.type, __version) == false)
        throw std::runtime_error ("Undefined attributes encoding format ("+
                                  std::to_string(attributes.type) +
                                  ") decoded from attributes table.");
    
    attributes.version      = LOAD_U16(__ptr + ATTRIBUTES::VERSION);
    
    const auto SIZES        = get_sizes(__base);
    const auto size_array   = SIZES.read_sizes(__base);
    
    const auto BYTES        = get_bytes(__base);
    BYTES.read_bytes        (__base, size_array, attributes);
    
    if (__version > IRIS_EXTENSION_1_0); else return attributes;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return attributes;
}
ATTRIBUTES_SIZES ATTRIBUTES::get_sizes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES&>(*this).check_and_fetch_remote(__base);
#endif
    auto __ATTRIBUTES_SIZES = ATTRIBUTES_SIZES
    (LOAD_U64(__base + __offset + LENGTHS_OFFSET), __size, __version);
    
    auto result = __ATTRIBUTES_SIZES.validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error(result.message);
    return __ATTRIBUTES_SIZES;
}
ATTRIBUTES_BYTES ATTRIBUTES::get_bytes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES&>(*this).check_and_fetch_remote(__base);
#endif
    auto __ATTRIBUTES_BYTES = ATTRIBUTES_BYTES
    (LOAD_U64(__base + __offset + BYTE_ARRAY_OFFSET), __size, __version);
    
    auto result = __ATTRIBUTES_BYTES.validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error(result.message);
    return __ATTRIBUTES_BYTES;
}
#ifdef __EMSCRIPTEN__
void ATTRIBUTES::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
void STORE_ATTRIBUTES (BYTE *const __base, const AttributesCreateInfo &info)
{
    if (info.attributesOffset == NULL_OFFSET) throw std::runtime_error
        ("failed to store attributes header -- invalid attributes offset");
    
    #if IrisCodecExtensionValidateEncoding
    if (VALIDATE_METADATA_TYPE(info.type, IFE_VERSION) == false) throw std::runtime_error
        ("failed to store metadata attributes -- undefined type");
    if (info.type == METADATA_DICOM && !info.version) throw std::runtime_error
        ("Attributes contains invalid type. IFE specification states that DICOM attributes must adhere to the DICOM PS3.3 and include the version year. A version of 0 indicates free-text attributes and requires METADATA_FREE_TEXT type.");
    
    Result result;
    DATA_BLOCK blk_validation (NULL_OFFSET, UINT64_MAX, IFE_VERSION);
    
    blk_validation.__offset = info.sizes;
    result = static_cast<ATTRIBUTES_SIZES&>(blk_validation).validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed STORE_ATTRIBUTES -- Invalid attributes sizes array offset (" +
         result.message +
         "). Per the IFE specification section 2.3.5, the attributes sizes offset shall encode a valid offset to the attribute size array (Section 2.4.4)");
    
    blk_validation.__offset = info.bytes;
    result = static_cast<ATTRIBUTES_BYTES&>(blk_validation).validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error
        ("Failed STORE_ATTRIBUTES -- " +
         result.message +
         ". Per the IFE specification section 2.3.5, the attributes bytes offset shall encode a valid offset to the attributes byte array (Section 2.4.5)");
    #endif
    
    const auto __ptr = __base + info.attributesOffset;
    STORE_U64(__ptr + ATTRIBUTES::VALIDATION,           info.attributesOffset);
    STORE_U16(__ptr + ATTRIBUTES::RECOVERY,             RECOVER_ATTRIBUTES);
    STORE_U8 (__ptr + ATTRIBUTES::FORMAT,               info.type);
    STORE_U16(__ptr + ATTRIBUTES::VERSION,              info.version);
    STORE_U64(__ptr + ATTRIBUTES::LENGTHS_OFFSET,       info.sizes);
    STORE_U64(__ptr + ATTRIBUTES::BYTE_ARRAY_OFFSET,    info.bytes);
}
#endif

// MARK: - LAYER EXTENT
LAYER_EXTENTS::LAYER_EXTENTS (Offset __TileTable_Offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(__TileTable_Offset, file_size, version)
{
    
}
Size LAYER_EXTENTS::size (const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<LAYER_EXTENTS&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    auto STEP           = LOAD_U16(__ptr + ENTRY_SIZE);
    auto ENTRIES        = LOAD_U32(__ptr + ENTRY_NUMBER);

    Size size = HEADER_V1_0_SIZE + ENTRIES*STEP;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result LAYER_EXTENTS::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<LAYER_EXTENTS&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result LAYER_EXTENTS::validate_full(const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<LAYER_EXTENTS&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if (result != IRIS_SUCCESS) return result;
    
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);

    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto VALIDATE_EXTENTS;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 LAYER_extentS PARAMETERS
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    VALIDATE_EXTENTS:
    if (start + ENTRIES*STEP > __size) return Result
        (IRIS_FAILURE,
         "LAYER_EXTENTS failed validation -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + ENTRIES*STEP)+
         "bytes) extends beyond the end of the file.");
    
    const BYTE* __array       = __base + start;
    float prior_scale   = 0.f;
    for (int LI = 0; LI < ENTRIES; ++LI, __array+=STEP) {
        if (LOAD_U32(__array + LAYER_EXTENT::X_TILES) < 1) return Result
            (IRIS_FAILURE,"LAYER_EXTENTS ["+std::to_string(LI)+"] failed validation. Per the IFE specifciation Section 2.4.1, the X-tiles shall encode the number of 256 pixel tiles in the horizontal direction and shall be greater than zero");
        if (LOAD_U32(__array + LAYER_EXTENT::Y_TILES) < 1) return Result
            (IRIS_FAILURE,"LAYER_EXTENTS ["+std::to_string(LI)+"] failed validation. Per the IFE specifciation Section 2.4.1, the Y-tiles shall encode the number of 256 pixel tiles in the vertical direction and shall be greater than zero");
        if (!(LOAD_F32(__array + LAYER_EXTENT::SCALE) > prior_scale)) return Result
            (IRIS_FAILURE,"LAYER_EXTENTS ["+std::to_string(LI)+"] failed validation. Per the IFE specifciation Section 2.4.1, the scale of a layer shall have a value greater than zero (0.f) and any subsequent layer shall have a scale that is greater than the previous scale");
        prior_scale = LOAD_F32(__array + LAYER_EXTENT::SCALE);
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2 LAYER_extent (no S) PARAMETERS
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    }
    return IRIS_SUCCESS;
}
LayerExtents LAYER_EXTENTS::read_layer_extents(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<LAYER_EXTENTS&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);

    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_EXTENTS;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 LAYER_extentS PARAMETERS
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_EXTENTS:
    if (start + ENTRIES*STEP > __size)
        throw std::runtime_error
        ("LAYER_EXTENTS::read_layer_extents failed -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + ENTRIES*STEP)+
         "bytes) extends beyond the end of the file.");
    
    const BYTE* __array       = __base + start;
    LayerExtents extents (ENTRIES);
    for (int LI = 0; LI < ENTRIES; ++LI, __array+=STEP) {
        auto& extent    = extents[LI];
        extent.xTiles   = LOAD_U32(__array + LAYER_EXTENT::X_TILES);
        extent.yTiles   = LOAD_U32(__array + LAYER_EXTENT::Y_TILES);
        extent.scale    = LOAD_F32(__array + LAYER_EXTENT::SCALE);
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2 LAYER_extent (no S) PARAMETERS
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    }
    
    // Calculate downsampling
    float max_scale = extents.back().scale;
    for (auto r_it = extents.rbegin(); r_it != extents.rend(); ++r_it)
        r_it->downsample = max_scale / r_it->scale;
    
    return extents;
}
#ifdef __EMSCRIPTEN__
void LAYER_EXTENTS::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url, __remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
inline Size STORE_EXTENT (BYTE* const __base, Offset offset, const LayerExtent &extent)
{
    using __LE = LAYER_EXTENT;
    STORE_U32(__base + offset + __LE::X_TILES, extent.xTiles);
    STORE_U32(__base + offset + __LE::Y_TILES, extent.yTiles);
    STORE_F32(__base + offset + __LE::SCALE,   extent.scale);
    return LAYER_EXTENT::SIZE;
}
Size SIZE_EXTENTS (const LayerExtents &__extents)
{
    return LAYER_EXTENTS::HEADER_SIZE + __extents.size() * LAYER_EXTENT::SIZE;
}
void STORE_EXTENTS(BYTE *const __base, Offset offset, const LayerExtents &extents)
{
    if (extents.size() > UINT32_MAX) throw std::runtime_error
        ("Failed to store layer extent sizes -- extents array length ("+
         std::to_string(extents.size())+
         ") exceeds 32-bit size limit. Per the IFE specification Section 2.4.1, the number of layers shall be less than the 32-bit max value.");
    
    STORE_U64 (__base + offset + LAYER_EXTENTS::VALIDATION, offset);
    STORE_U16 (__base + offset + LAYER_EXTENTS::RECOVERY,   RECOVER_LAYER_EXTENTS);
    STORE_U16 (__base + offset + LAYER_EXTENTS::ENTRY_SIZE, LAYER_EXTENT::SIZE);
    STORE_U32 (__base + offset + LAYER_EXTENTS::ENTRY_NUMBER, U32_CAST(extents.size()));
    offset      += LAYER_EXTENTS::HEADER_SIZE;
    for (auto&& layer : extents) {
        STORE_EXTENT(__base, offset, layer);
        offset += LAYER_EXTENT::SIZE;
    }
}
#endif
// MARK: - TILE OFFSETS
TILE_OFFSETS::TILE_OFFSETS  (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size TILE_OFFSETS::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<TILE_OFFSETS&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size           = HEADER_V1_0_SIZE + ENTRIES*STEP;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result TILE_OFFSETS::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<TILE_OFFSETS&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result TILE_OFFSETS::validate_full (const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<TILE_OFFSETS&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if(result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto VALIDATE_TILE_OFFSETS;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    VALIDATE_TILE_OFFSETS:
    if (start + ENTRIES*STEP > __size) return Result
        (IRIS_FAILURE,
         "TILE_OFFSETS failed validation -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + ENTRIES*STEP)+
         "bytes) extends beyond the end of the file.");
    
    const BYTE* __array = __base + start;
    for (auto TI = 0; TI < ENTRIES; ++TI, __array+=STEP)
        if (LOAD_U40(__array + TILE_OFFSET::OFFSET) +
            LOAD_U24(__array + TILE_OFFSET::TILE_SIZE) > __size) return Result
            (IRIS_FAILURE,
             "TILE_OFFSETS validation failed -- global tile entry (" +
             std::to_string(TI) +
             ") failed with the tile data block (offset + size size) extending out of the file bounds ("+
             std::to_string(__size)
             +"bytes).");
    
    return IRIS_SUCCESS;
}
void TILE_OFFSETS::read_tile_offsets(const BYTE *const __base, TileTable& table) const
{
#ifdef __EMSCRIPTEN__
    const_cast<TILE_OFFSETS&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);

    auto total_tiles = 0;
    for (auto&& layer : table.extent.layers)
        total_tiles += layer.xTiles * layer.yTiles;
    if (total_tiles != ENTRIES) throw std::runtime_error
        (std::string ("Failed TILE_OFFSETS::read_tile_offsets -- Tile numbers in tile table extents ")+
         std::to_string(total_tiles)+
         " does not match total entries in the tile offset array "+
         std::to_string(ENTRIES));
    
    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_OFFSETS;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_OFFSETS:
    if (start + ENTRIES*STEP > __size)
        throw std::runtime_error
        ("TILE_OFFSETS::read_tile_offsets failed -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + ENTRIES*STEP)+
         "bytes) extends beyond the end of the file.");
    
    const BYTE* __array   = __base + start;
    table.layers = TileTable::Layers (table.extent.layers.size());
    for (int LI = 0; LI < table.layers.size(); ++LI) {
        auto& LE        = table.extent.layers[LI];
        auto& layer     = table.layers[LI];
        auto  tiles     = LE.xTiles*LE.yTiles;
        layer = TileTable::Layer(tiles);
        for (auto TI = 0; TI < tiles; ++TI, __array+=STEP) {
            auto&tile   = layer[TI];
            
            // Load the tile table offset and size
            tile.offset     = LOAD_U40(__array + TILE_OFFSET::OFFSET);
            tile.size       = LOAD_U24(__array + TILE_OFFSET::TILE_SIZE);
            
            // Perform Offset Checks: Sparse Tile? Out of Bounds?
            if (tile.offset == NULL_TILE) {
                tile.offset = NULL_OFFSET;
                tile.size   = 0;
            } else if (tile.offset + tile.size > __size) throw std::runtime_error
                ("read_tile_offsets returned tile data offset value out of file bounds.");
            
            if (__version > IRIS_EXTENSION_1_0); else continue;
            
            // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
            // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
            // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
            
        }
    }
    return;
}
#ifdef __EMSCRIPTEN__
void TILE_OFFSETS::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url, __remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
Size SIZE_TILE_OFFSETS (const TileTable::Layers &__offsets)
{
    Size size = TILE_OFFSETS::HEADER_SIZE;
    for (auto&& layer : __offsets)
        size += layer.size() * TILE_OFFSET::SIZE;
    return size;
}
void STORE_TILE_OFFSETS (BYTE* const __base, Offset offset, const TileTable::Layers &__offsets)
{
    uint32_t total_tiles = 0;
    for (auto&& layer : __offsets) total_tiles += U32_CAST(layer.size());
    
    STORE_U64(__base + offset + TILE_OFFSETS::VALIDATION,   offset);
    STORE_U16(__base + offset + TILE_OFFSETS::RECOVERY,     RECOVER_TILE_OFFSETS);
    STORE_U16(__base + offset + TILE_OFFSETS::ENTRY_SIZE,   TILE_OFFSET::SIZE);
    STORE_U32(__base + offset + TILE_OFFSETS::ENTRY_NUMBER, total_tiles);
    offset += TILE_OFFSETS::HEADER_SIZE;
    for (auto&& layer : __offsets)
        for (auto&& tile : layer) {
            if (tile.offset > UINT40_MAX) throw std::runtime_error("tile offset above 40-bit numerical limit");
            if (tile.size   > UINT24_MAX) throw std::runtime_error("tile size above 24-bit numerical limit");
            STORE_U40   (__base + offset + TILE_OFFSET::OFFSET,     tile.offset);
            STORE_U24   (__base + offset + TILE_OFFSET::TILE_SIZE,  tile.size);
            offset += TILE_OFFSET::SIZE;
        }
}
#endif

// MARK: - ATTRIBUTES SIZES
ATTRIBUTES_SIZES::ATTRIBUTES_SIZES  (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size ATTRIBUTES_SIZES::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size           = HEADER_V1_0_SIZE + ENTRIES*STEP;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2 VALIDATIONS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ATTRIBUTES_SIZES::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result ATTRIBUTES_SIZES::validate_full(const BYTE *const __base, Size& expected_bytes) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if(result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Offset start = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_SIZES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_SIZES:
    if (start + ENTRIES * STEP > __size) return Result
        (IRIS_FAILURE,"ATTRIBUTES_SIZES failed validation -- sizes array block (location "+
         std::to_string(start)+ " - " +
         std::to_string(start + ENTRIES * STEP)+
         " bytes) extends beyond the end of file.");
    
    const BYTE* __array   = __base + start;
    expected_bytes  = 0;
    for (int EI = 0; EI < ENTRIES; ++EI, __array+=STEP) {
        expected_bytes += LOAD_U16(__array + ATTRIBUTE_SIZE::KEY_SIZE);
        expected_bytes += LOAD_U32(__array + ATTRIBUTE_SIZE::VALUE_SIZE);
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        
    }
    return IRIS_SUCCESS;
}
ATTRIBUTES_SIZES::SizeArray ATTRIBUTES_SIZES::read_sizes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Offset start = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_SIZES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_SIZES:
    SizeArray sizes (ENTRIES);
    if (start + ENTRIES * STEP > __size) throw std::runtime_error
        ("ANNOTATION_GROUP_SIZES failed -- sizes array block (location "+
         std::to_string(start)+ " - " +
         std::to_string(start + ENTRIES * STEP)+
         " bytes) extends beyond the end of file.");
    
    const BYTE* __array   = __base + start;
    for (int EI = 0; EI < ENTRIES; ++EI, __array+=STEP) {
        sizes[EI] = {
            LOAD_U16(__array + ATTRIBUTE_SIZE::KEY_SIZE),
            LOAD_U32(__array + ATTRIBUTE_SIZE::VALUE_SIZE)
        };
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        
    }
    return sizes;
}
#ifdef __EMSCRIPTEN__
void ATTRIBUTES_SIZES::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url, __remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
Size SIZE_ATTRIBUTES_SIZES (const Attributes& attributes)
{
    return ATTRIBUTES_SIZES::HEADER_SIZE +
    ATTRIBUTE_SIZE::SIZE * attributes.size();
}
void STORE_ATTRIBUTES_SIZES (BYTE* const __base, Offset offset, const Attributes& attributes)
{
    if (offset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store attributes sizes -- NULL_OFFSET provided as location");
    
    #if IrisCodecExtensionValidateEncoding
    switch (attributes.type) {
        case METADATA_I2S:
            for (auto&& attribute : attributes) {
                if (attribute.first.size() > UINT16_MAX) throw std::runtime_error
                    ("Failed to store attributes sizes -- attribute key \""+
                     attribute.first+"\" exceeds key 16-bit size limit");
                if (attribute.second.size() > UINT32_MAX) throw std::runtime_error
                    ("Failed to store attributes sizes -- attribute value length ("+
                     std::to_string(attribute.second.size())+
                     " bytes) exceeds key 32-bit size limit");
            } break;
        case METADATA_DICOM:
            for (auto&& attribute : attributes) {
                if (attribute.second.size() > UINT32_MAX) throw std::runtime_error
                    ("Failed to store attributes sizes -- attribute value length ("+
                     std::to_string(attribute.second.size())+
                     " bytes) exceeds key 32-bit size limit");
            } break;
        case METADATA_UNDEFINED:
            throw std::runtime_error("Failed to store attributes sizes -- undefined metadata attribute type");
    }
    #endif
    
    auto __ptr = __base + offset;
    STORE_U64(__ptr + ATTRIBUTES_SIZES::VALIDATION, offset);
    STORE_U16(__ptr + ATTRIBUTES_SIZES::RECOVERY, RECOVER_ATTRIBUTES_SIZES);
    STORE_U16(__ptr + ATTRIBUTES_SIZES::ENTRY_SIZE, ATTRIBUTE_SIZE::SIZE);
    STORE_U32(__ptr + ATTRIBUTES_SIZES::ENTRY_NUMBER, U32_CAST(attributes.size()));
    __ptr += ATTRIBUTES_SIZES::HEADER_SIZE;
    
    for (auto&& attribute : attributes) {
        STORE_U16(__ptr + ATTRIBUTE_SIZE::KEY_SIZE, U16_CAST(attribute.first.size()));
        STORE_U32(__ptr + ATTRIBUTE_SIZE::VALUE_SIZE, U32_CAST(attribute.second.size()));
        __ptr += ATTRIBUTE_SIZE::SIZE;
    }
}
#endif
// MARK: - ATTRIBUTES BYTES
ATTRIBUTES_BYTES::ATTRIBUTES_BYTES  (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size ATTRIBUTES_BYTES::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size = HEADER_V1_0_SIZE + BYTES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ATTRIBUTES_BYTES::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result ATTRIBUTES_BYTES::validate_full(const BYTE *const __base, Size expected) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if(result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    if (BYTES != expected) return Result
        (IRIS_FAILURE,"ATTRIBUTES_BYTES failed validation -- expected bytes ("+
         std::to_string(expected)+
         ") from ATTRIBUTES_SIZES array does not match the byte size of the ATTRIBUTES_BYTES block (" +
         std::to_string(BYTES) +
         ")");
    if (__offset + BYTES > __size) return Result
        (IRIS_FAILURE,"ATTRIBUTES_BYTES failed validation -- full attributes byte array block (location "+
         std::to_string(__offset)+ " - " +
         std::to_string(__offset + LOAD_U32(__ptr + ENTRY_NUMBER))+
         ") extends beyond end of file.");
    
    return IRIS_SUCCESS;
}
void ATTRIBUTES_BYTES::read_bytes(const BYTE *const __base, const SizeArray &sizes, Attributes &attributes) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ATTRIBUTES_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    {   // Validate sizes array for bounds check
        Size total_size = 0;
        for (auto&& size : sizes)
            total_size+= size.first + size.second;
        if (total_size != BYTES) throw std::runtime_error
            ("ATTRIBUTES_BYTES failed validation -- expected bytes ("+
             std::to_string(total_size)+
             ") from ATTRIBUTES_SIZES array does not match the byte size of the ATTRIBUTES_BYTES block (" +
             std::to_string(BYTES) +
             ")");
    }
    
    Offset start = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_STRINGS;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_STRINGS:
    if (start + BYTES > __size) throw std::runtime_error
        ("Failed ATTRIBUTES_BYTES::read_bytes -- out of bounds. Byte array offset and size (" +
         std::to_string(start+BYTES) +
         ") exceeds file size "+
         std::to_string(__size)+" bytes.");
    
    attributes.clear();
    const BYTE* __array = __base + start;
    for (auto&& size : sizes) {
        Size total_bytes = size.first + size.second;
        attributes[std::string((char*)__array,size.first)] =
        std::u8string((char8_t*)__array+size.first,size.second);
        __array += total_bytes;
    }
    return;
}
Size SIZE_ATTRIBUTES_BYTES (const Attributes& attributes)
{
    Size size = ATTRIBUTES_BYTES::HEADER_SIZE;
    for (auto&& attribute : attributes) {
        size += attribute.first.size();
        size += attribute.second.size();
    } return size;
}
#ifdef __EMSCRIPTEN__
void ATTRIBUTES_BYTES::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url, __remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
void STORE_ATTRIBUTES_BYTES (BYTE* const __base, Offset offset, const Attributes& attributes)
{
    #if IrisCodecExtensionValidateEncoding
    if (offset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store attributes bytes -- NULL_OFFSET provided as location");
    
    switch (attributes.type) {
        case METADATA_I2S:
        case METADATA_DICOM: break;
        case METADATA_UNDEFINED:
            throw std::runtime_error("Failed to store attributes sizes -- undefined metadata attribute type");
    }
    #endif
    
    auto __ptr = __base + offset;
    Size size  = 0;
    STORE_U64(__ptr + ATTRIBUTES_BYTES::VALIDATION, offset);
    STORE_U16(__ptr + ATTRIBUTES_BYTES::RECOVERY, RECOVER_ATTRIBUTES_BYTES);
    __ptr += ATTRIBUTES_BYTES::HEADER_SIZE;
    
    for (auto&& attribute : attributes) {
        uint16_t key_size = U16_CAST(attribute.first.size());
        std::memcpy(__ptr, attribute.first.data(), key_size);
        __ptr += key_size;
        size  += key_size;
        
        uint32_t value_size = U32_CAST(attribute.second.size());
        std::memcpy(__ptr, attribute.second.data(), value_size);
        __ptr += value_size;
        size  += value_size;
    }
    
    if (size > UINT32_MAX) throw std::runtime_error
        ("Failed to store attributes bytes -- attribute bytes array length ("+
         std::to_string(size)+
         " bytes) exceeds key 32-bit size limit.\n Per the IFE specification Section 2.4.10, The number entry shall encode the total byte size of the annotation byte array and shall not exceed the 32-bit integer max value (4.29 GB).");
    STORE_U32(__base + offset + ATTRIBUTES_BYTES::ENTRY_NUMBER, U32_CAST(size));
}
#endif
// MARK: - IMAGES_ARRAY
IMAGE_ARRAY::IMAGE_ARRAY  (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size IMAGE_ARRAY::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_ARRAY&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size = HEADER_V1_0_SIZE + STEP * ENTRIES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result IMAGE_ARRAY::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_ARRAY&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result IMAGE_ARRAY::validate_full (const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_ARRAY&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);

    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto VALIDATE_IMAGES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    VALIDATE_IMAGES:
    const BYTE* __array   = __base + start;
    for (int II = 0; II < ENTRIES; ++II, __array+=STEP) {
        
        auto __IMAGE_BYTES =
        IMAGE_BYTES(LOAD_U64(__array+IMAGE_ENTRY::BYTES_OFFSET),__size, __version);
        __IMAGE_BYTES.validate_offset(__base);
        __IMAGE_BYTES.validate_full(__base);
        
        if (!VALIDATE_IMAGE_ENCODING_TYPE
            ((ImageEncoding)LOAD_U8(__array + IMAGE_ENTRY::ENCODING),__version)) return Result
            (IRIS_FAILURE,"Undefined tile associated image encoding ("+
             std::to_string(LOAD_U8(__array + IMAGE_ENTRY::ENCODING))+
             ") decoded from associated image array. Per the IFE specification Section 2.4.6, the encoding parameter shall describe the compression codec used to generate the compressed image byte stream and shall be one of the defined enumerated values (Enumeration 2.2.7), excluding the undefined value (0)");
        
        if (!VALIDATE_PIXEL_FORMAT
            ((Format)LOAD_U8(__array + IMAGE_ENTRY::FORMAT),__version)) return Result
            (IRIS_FAILURE,"Undefined tile associated image pixel format ("+
             std::to_string((Format)LOAD_U8(__array + IMAGE_ENTRY::FORMAT))+
             ") decoded from associated image array. Per the IFE specification Section 2.4.6,  format parameter shall describe the pixel channel ordering and bits consumed per channel using one of the defined enumerated values (Enumeration 2.2.4), excluding the undefined value (0)");
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    }
    return result;
}
Abstraction::AssociatedImages IMAGE_ARRAY::read_assoc_images (const BYTE *const __base, BYTES_ARRAY* __image_bytes) const
{
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_ARRAY&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_IMAGES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    
    READ_IMAGES:
    Abstraction::AssociatedImages images;
    BYTES_ARRAY bytes_array;
    if (start + ENTRIES*STEP > __size)
        throw std::runtime_error
        ("IMAGE_ARRAY::read_images failed -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + ENTRIES*STEP)+
         "bytes) extends beyond the end of the file.");
    
    const BYTE* __array   = __base + start;
    for (int II = 0; II < ENTRIES; ++II, __array+=STEP) {
        
        auto bytes_offset   = LOAD_U64(__array+IMAGE_ENTRY::BYTES_OFFSET);
        if (bytes_offset == NULL_OFFSET) throw std::runtime_error
            ("Failed IMAGES_ARRAY::read_assoc_images -- image entry contains invalid offset");
        if (bytes_offset > __size) throw std::runtime_error
            ("Failed IMAGES_ARRAY::read_images -- image entry out of file bounds read");
        
        auto __IMAGE_BYTES  = IMAGE_BYTES(bytes_offset, __size, __version);
        __IMAGE_BYTES.validate_offset(__base);
        
        Abstraction::AssociatedImage image;
        auto title = __IMAGE_BYTES.read_image_bytes(__base, image);
        if (images.contains(title)) { printf
            ("WARNING: duplicate associated image title (%s) returned; skipping duplicate. Per the IFE Specification Sections 2.4.6-2.4.7, each image title within the associated images array shall be referenced by unique ASCII encoded labels.", title.c_str());
            continue;
        }
        if (image.byteSize == 0 || image.byteSize > UINT32_MAX) throw std::runtime_error
            ("Failed IMAGES_ARRAY::read_assoc_images -- image byte size ("+
             std::to_string(image.byteSize) +
             ") invalid. Per the IFE specification Section 2.4.7, the image size shall encode a size, in bytes, greater than zero bytes but less than the 32-bit max (4.29 GB) of a valid encoded image byte stream.");
        
        images[title]       = image;
        auto& info          = images[title].info;
        info.width          = LOAD_U32(__array + IMAGE_ENTRY::WIDTH);
        info.height         = LOAD_U32(__array + IMAGE_ENTRY::HEIGHT);
        info.encoding       = (ImageEncoding)LOAD_U8(__array + IMAGE_ENTRY::ENCODING);
        if (VALIDATE_IMAGE_ENCODING_TYPE(info.encoding, __version) == false)
            throw std::runtime_error ("Undefined associated image encoding ("+
                                      std::to_string(info.encoding) +
                                      ") decoded from tile table.");
        info.sourceFormat   = (Format)LOAD_U8(__array + IMAGE_ENTRY::FORMAT);
        if (VALIDATE_PIXEL_FORMAT(info.sourceFormat, __version) == false)
            throw std::runtime_error ("Undefined associated image source format ("+
                                      std::to_string(info.sourceFormat) +
                                      ") decoded from tile table.");
        info.orientation    = (ImageOrientation)(LOAD_U16(__array + IMAGE_ENTRY::ORIENTATION)%360);
        // NOTE: We will NOT validate here; while this is an enumeration
        // uint16_t values outside of the stated enumerations are permitted
        // per the IFE Specification.
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    }
    // If an array of the image bytes was requested
    if (__image_bytes) *__image_bytes = bytes_array;
    
    // Return the images
    return images;
}
#ifdef __EMSCRIPTEN__
void IMAGE_ARRAY::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url, __remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
Size SIZE_IMAGES_ARRAY (AssociatedImageCreateInfo& info)
{
    return IMAGE_ARRAY::HEADER_SIZE +
    IMAGE_ENTRY::SIZE * info.images.size();
}
void STORE_IMAGES_ARRAY (BYTE* const __base, const AssociatedImageCreateInfo& info)
{
    #if IrisCodecExtensionValidateEncoding
    if (info.offset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store associated images array -- NULL_OFFSET provided as location");
    if (info.images.size() > UINT32_MAX) throw std::runtime_error
        ("Failed to store associated images array -- array too large (" +
         std::to_string(info.images.size())+
         "). Per the IFE specification Section 2.4.6, the number of associated / ancillary images must be less than the 32-bit max value.");
    #endif
    
    
    auto __ptr  = __base + info.offset;
    STORE_U64(__ptr + IMAGE_ARRAY::VALIDATION,     info.offset);
    STORE_U16(__ptr + IMAGE_ARRAY::RECOVERY,       RECOVER_ASSOCIATED_IMAGES);
    STORE_U16(__ptr + IMAGE_ARRAY::ENTRY_SIZE,     IMAGE_ENTRY::SIZE);
    STORE_U32(__ptr + IMAGE_ARRAY::ENTRY_NUMBER,   U32_CAST(info.images.size()));
    __ptr += IMAGE_ARRAY::HEADER_SIZE;
    
    for (auto&& image : info.images) {
        #if IrisCodecExtensionValidateEncoding
        if (image.offset == NULL_OFFSET) throw std::runtime_error
            ("Failed to store associated image -- NULL_OFFSET provided as location");
        if (image.info.width == 0 || image.info.width > UINT32_MAX) throw std::runtime_error
            ("Failed to store associated image -- invalid width (" +
             std::to_string(image.info.width)+
             " px). Per the IFE specification Section 2.4.6, width parameter shall encode the horizontal pixel extent of the encoded image and shall be greater than zero but less than the 32-bit max value.");
        if (image.info.height == 0 || image.info.height > UINT32_MAX) throw std::runtime_error
            ("Failed to store associated image -- invalid height (" +
             std::to_string(image.info.width)+
             " px). Per the IFE specification Section 2.4.6, height parameter shall encode the horizontal pixel extent of the encoded image and shall be greater than zero but less than the 32-bit max value.");
        if (VALIDATE_IMAGE_ENCODING_TYPE(image.info.encoding, IFE_VERSION) == false) throw std::runtime_error
            ("Failed to store associated image -- undefined compression encoding (" +
             std::to_string(image.info.encoding) +
             "). Per the IFE specification Section 2.4.6, The encoding parameter shall describe the compression codec used to generate the compressed image byte stream and shall be one of the defined enumerated values (Enumeration 2.2.6), excluding the undefined value (0).");
        if (VALIDATE_PIXEL_FORMAT(image.info.sourceFormat, IFE_VERSION) == false) throw std::runtime_error
            ("Failed to store associated image -- undefined source pixel format (" +
             std::to_string(image.info.sourceFormat) +
             "). Per the IFE specification Section 2.4.6, The format parameter shall describe the pixel channel ordering and bits consumed per channel using one of the defined enumerated values (Enumeration 2.2.3), excluding the undefined value (0).");
        #endif
        STORE_U64(__ptr + IMAGE_ENTRY::BYTES_OFFSET,    image.offset);
        STORE_U32(__ptr + IMAGE_ENTRY::WIDTH,           image.info.width);
        STORE_U32(__ptr + IMAGE_ENTRY::HEIGHT,          image.info.height);
        STORE_U8 (__ptr + IMAGE_ENTRY::ENCODING,        image.info.encoding);
        STORE_U8 (__ptr + IMAGE_ENTRY::FORMAT,          image.info.sourceFormat);
        STORE_U16(__ptr + IMAGE_ENTRY::ORIENTATION,     image.info.orientation);
        __ptr += IMAGE_ENTRY::SIZE;
    }
}
#endif
// MARK: - IMAGE_BYTES
IMAGE_BYTES::IMAGE_BYTES  (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size IMAGE_BYTES::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto TITLE    = LOAD_U16(__ptr + TITLE_SIZE);
    const auto BYTES    = LOAD_U32(__ptr + IMAGE_SIZE);
    
    Size size = HEADER_V1_0_SIZE + TITLE * BYTES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result IMAGE_BYTES::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result IMAGE_BYTES::validate_full (const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto TITLE    = LOAD_U16(__ptr + TITLE_SIZE);
    const auto BYTES    = LOAD_U32(__ptr + IMAGE_SIZE);
    if (TITLE == 0 || TITLE > UINT16_MAX) return Iris::Result
        (IRIS_VALIDATION_FAILURE,"Associated image title failed validation due to length. Per IFE Section 2.4.7, title size shall encode a size, in bytes, greater than zero but shorter in length than the 16-bit max of a valid and unique image title / label");
    if (BYTES == 0 || BYTES > UINT32_MAX) return Iris::Result
        (IRIS_VALIDATION_FAILURE,"Associated image bytes failed validation due to length. Per IFE Section 2.4.7, image size shall encode a size, in bytes, greater than zero bytes but less than the 32-bit max (4.29 GB) of a valid encoded image byte stream");
    if (__offset + TITLE + BYTES > __size) return Result
        (IRIS_FAILURE,"Associated image IMAGE_BYTES failed validation -- image bytes array block (location "+
         std::to_string(__offset)+ " - " +
         std::to_string(__offset+TITLE+BYTES)+
         " bytes) extends beyond the end of file.");
    
    return result;
}
std::string IMAGE_BYTES::read_image_bytes(const BYTE *const __base, Abstraction::AssociatedImage &image) const
{
#ifdef __EMSCRIPTEN__
    const_cast<IMAGE_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto TITLE    = LOAD_U16(__ptr + TITLE_SIZE);
    image.byteSize      = LOAD_U32(__ptr + IMAGE_SIZE);
    
    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_BYTES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_BYTES:
    const BYTE* __bytes   = __base + start;
    auto title      = std::string((char*)__bytes, TITLE);
    image.offset    = start + TITLE;
    if (TITLE == 0 || TITLE > UINT16_MAX) throw std::runtime_error
        ("Associated image title failed validation due to length. Per IFE Section 2.4.7, title size shall encode a size, in bytes, greater than zero but shorter in length than the 16-bit max of a valid and unique image title / label");
    if (image.byteSize == 0 || image.byteSize > UINT32_MAX) throw std::runtime_error
        ("Associated image bytes failed validation due to length. Per IFE Section 2.4.7, image size shall encode a size, in bytes, greater than zero bytes but less than the 32-bit max (4.29 GB) of a valid encoded image byte stream");
    if (image.offset + image.byteSize > __size) throw std::runtime_error
        ("Read_image_bytes failed validation -- image bytes block ("+
         std::to_string(image.offset) + "-" +
         std::to_string(image.offset+image.byteSize)+
         "bytes) extends beyond the end of the file.");
    
    if (__version > IRIS_EXTENSION_1_0); else return title;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return title;
}
#ifdef __EMSCRIPTEN__
void IMAGE_BYTES::check_and_fetch_remote(const BYTE *const &base)
{
if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url,__remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
Size SIZE_IMAGES_BYTES(const ImageBytesCreateInfo &image)
{
    return IMAGE_BYTES::HEADER_SIZE +
    image.title.size() + image.dataBytes;
}
void STORE_IMAGES_BYTES(BYTE *const __base, const ImageBytesCreateInfo& info)
{
    #if IrisCodecExtensionValidateEncoding
    if (info.offset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store associated image bytes -- NULL_OFFSET provided as location");
    if (!info.title.size()) throw std::runtime_error
        ("Failed to store associated image bytes -- No title/label given to the associated image. Per the IFE specification Section 2.4.7, an associated image shall contain a valid and unique title/label.");
    if (info.title.size() > UINT16_MAX) throw std::runtime_error
        ("Failed to store associated image bytes -- Title/label too long. Per the IFE specification Section 2.4.7, an associated image title shall be encoded in ASCII and be shorter in length than the 16-bit max.");
    if (!info.data || !info.dataBytes) throw std::runtime_error
        ("Failed to store associated image bytes -- No image data was provided. Per the IFE specification Section 2.4.7, an associated image bytestream shall comprise a valid array of compressed image bytes.");
    if (info.dataBytes > UINT32_MAX) throw std::runtime_error
        ("Failed to store associated image bytes -- Image too large. Per the IFE specification Section 2.4.7, an associated image bytestream shall be less than the 32-bit max (4.29 GB)");
    #endif
    
    auto __ptr = __base + info.offset;
    STORE_U64(__ptr + IMAGE_BYTES::VALIDATION, info.offset);
    STORE_U16(__ptr + IMAGE_BYTES::RECOVERY, RECOVER_ASSOCIATED_IMAGE_BYTES);
    STORE_U16(__ptr + IMAGE_BYTES::TITLE_SIZE, U16_CAST(info.title.size()));
    STORE_U32(__ptr + IMAGE_BYTES::IMAGE_SIZE, U32_CAST(info.dataBytes));
    
    __ptr += IMAGE_BYTES::HEADER_SIZE;
    std::memcpy(__ptr, info.title.data(), info.title.size());
    __ptr += info.title.size();
    std::memcpy(__ptr, info.data, info.dataBytes);
    return;
}
#endif
// MARK: - ICC COLOR PROFILE
ICC_PROFILE::ICC_PROFILE (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size ICC_PROFILE::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ICC_PROFILE&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size = HEADER_V1_0_SIZE + BYTES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ICC_PROFILE::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ICC_PROFILE&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result ICC_PROFILE::validate_full(const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<ICC_PROFILE&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    Offset offset       = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto VALIDATE_BYTES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    VALIDATE_BYTES:
    if (offset + BYTES > __size) return Result
        (IRIS_FAILURE,"ICC_PROFILE failed validation -- bytes block ("+
         std::to_string(offset) + "-" +
         std::to_string(offset + BYTES)+
         "bytes) extends beyond the end of the file.");
    return result;
}
std::string ICC_PROFILE::read_profile (const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ICC_PROFILE&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);

    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_BYTES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_BYTES:
    if (start + BYTES > __size)
        throw std::runtime_error
        ("ICC_PROFILE::read_profile failed -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + BYTES)+
         "bytes) extends beyond the end of the file.");
    
    const BYTE* __bytes = __base + start;
    return std::string((char*)__bytes, BYTES);
}
#ifdef __EMSCRIPTEN__
void ICC_PROFILE::check_and_fetch_remote(const BYTE *const &base)
{
if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url,__remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
Size SIZE_ICC_COLOR_PROFILE(const std::string &color_profile)
{
    return ICC_PROFILE::HEADER_SIZE + color_profile.size();
}
void STORE_ICC_COLOR_PROFILE(BYTE *const __base, Offset offset, const std::string &color_profile)
{
    #if IrisCodecExtensionValidateEncoding
    if (offset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store associated image bytes -- NULL_OFFSET provided as location");
    if (color_profile.size() > UINT32_MAX) throw std::runtime_error
        ("Failed to store associated image bytes -- profile too long. Per the IFE specification Section 2.4.8, an ICC color profile shall be shorter in length than the 32-bit max (4.29GB).");
    #endif
    
    auto __ptr = __base + offset;
    STORE_U64(__ptr + ICC_PROFILE::VALIDATION, offset);
    STORE_U16(__ptr + ICC_PROFILE::RECOVERY, RECOVER_ICC_PROFILE);
    STORE_U16(__ptr + ICC_PROFILE::ENTRY_NUMBER, U32_CAST(color_profile.size()));
    memcpy(__ptr + ICC_PROFILE::HEADER_SIZE, color_profile.data(), color_profile.size());
    return;
}
#endif
// MARK: - ANNOTATIONS
ANNOTATIONS::ANNOTATIONS  (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size ANNOTATIONS::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATIONS&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size = HEADER_V1_0_SIZE + STEP * ENTRIES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ANNOTATIONS::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATIONS&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result ANNOTATIONS::validate_full (const BYTE *const __base) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATIONS&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    Offset start        = 0;
    
    
    if (groups(__base)) {
        Size expected_bytes;
        auto __GROUP_SIZES = GROUP_SIZES
        (LOAD_U64(__base + GROUP_SIZES_OFFSET), __size, __version);
        result = __GROUP_SIZES.validate_full(__base, expected_bytes);
        if (result & IRIS_FAILURE) return result;
        
        
        auto __GROUP_BYTES = GROUP_BYTES
        (LOAD_U64(__base + GROUP_BYTES_OFFSET), __size, __version);
        result = __GROUP_BYTES.validate_full(__base, expected_bytes);
        if (result & IRIS_FAILURE) return result;
    }
    
    start = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto VALIDATE_ANNOTATIONS;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    VALIDATE_ANNOTATIONS:
    std::unordered_set<uint32_t> __a;
    if (start + ENTRIES*STEP > __size) return Result
        (IRIS_FAILURE,"ANNOTATIONS::read_annotations failed validation -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + ENTRIES*STEP)+
         "bytes) extends beyond the end of the file.");
    
    const BYTE* __array   = __base + start;
    for (int AI = 0; AI < ENTRIES; ++AI, __array+=STEP) {
        auto bytes_offset   = LOAD_U64(__array+IMAGE_ENTRY::BYTES_OFFSET);
        if (bytes_offset == NULL_OFFSET) return Result
            (IRIS_FAILURE, "Failed ANNOTATION_ARRAY::read_annotations -- annotation entry contains invalid offset. Per the IFE Specification, the bytes offset shall be a valid offset location that point to the corresponding attribute object's attributes bytes array (Section 2.4.5).");
        if (bytes_offset > __size) return Result
            (IRIS_FAILURE,"Failed ANNOTATION_ARRAY::read_annotations -- annotation entry contains an offset that is out of file bounds(" +
             std::to_string(bytes_offset)+
             "). Per the IFE Specification, the bytes offset shall be a valid offset location that point to the corresponding attribute object's attributes bytes array (Section 2.4.5).");
        
        auto __BYTES  = ANNOTATION_BYTES(bytes_offset, __size, __version);
        __BYTES.validate_offset(__base);
        
        auto identifier = LOAD_U24(__ptr + ANNOTATION_ENTRY::IDENTIFIER);
        if (__a.contains(identifier)) { printf
            ("WARNING: duplicate annotation identifier (%X) returned. Per the IFE Specification Section 2.4.9, each annotation within the annotations array shall be referenced by a unique 24-bit identifier.",
             identifier);
        }
        
        if (VALIDATE_ANNOTATION_TYPE
            ((AnnotationTypes)LOAD_U8(__ptr + ANNOTATION_ENTRY::FORMAT),
             __version) == false) return Result
            (IRIS_FAILURE,"Undefined tile pixel format ("+
             std::to_string(LOAD_U8(__ptr + ANNOTATION_ENTRY::FORMAT)) +
             ") decoded from tile table.");
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        
    }
    return result;
}
Abstraction::Annotations ANNOTATIONS::read_annotations(const BYTE *const __base, BYTES_ARRAY* __bytes_array) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATIONS&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_ANNOTATIONS;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_ANNOTATIONS:
    Abstraction::Annotations annotations;
    const BYTE* __array   = __base + start;
    if (start + ENTRIES*STEP > __size)
        throw std::runtime_error
        ("ANNOTATIONS::read_annotations failed -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start + ENTRIES*STEP)+
         "bytes) extends beyond the end of the file.");
    
    for (int AI = 0; AI < ENTRIES; ++AI, __array+=STEP) {
        
        auto bytes_offset   = LOAD_U64(__array+IMAGE_ENTRY::BYTES_OFFSET);
        if (bytes_offset == NULL_OFFSET) throw std::runtime_error
            ("Failed ANNOTATION_ARRAY::read_annotations -- annotation entry contains invalid offset");
        if (bytes_offset > __size) throw std::runtime_error
            ("Failed ANNOTATION_ARRAY::read_annotations -- annotation entry out of file bounds read");
        
        auto __BYTES  = ANNOTATION_BYTES(bytes_offset, __size, __version);
        __BYTES.validate_offset(__base);
        
        auto identifier = LOAD_U24(__ptr + ANNOTATION_ENTRY::IDENTIFIER);
        if (annotations.contains(identifier)) { printf
            ("WARNING: duplicate annotation identifier (%X) returned; skipping duplicate. Per the IFE Specification Section 2.4.9, each annotation within the annotations array shall be referenced by a unique 24-bit identifier.", identifier);
        }
        
        Abstraction::Annotation annotation;
        __BYTES.read_bytes(__base, annotation);
        annotation.type      = (AnnotationTypes)LOAD_U8(__ptr + ANNOTATION_ENTRY::FORMAT);
        if (VALIDATE_ANNOTATION_TYPE(annotation.type, __version) == false)
            throw std::runtime_error ("Undefined tile pixel format ("+
                                      std::to_string(annotation.type) +
                                      ") decoded from tile table.");
        annotation.xLocation = LOAD_F32(__ptr + ANNOTATION_ENTRY::X_LOCATION);
        annotation.yLocation = LOAD_F32(__ptr + ANNOTATION_ENTRY::Y_LOCATION);
        annotation.xSize     = LOAD_F32(__ptr + ANNOTATION_ENTRY::X_SIZE);
        annotation.ySize     = LOAD_F32(__ptr + ANNOTATION_ENTRY::Y_SIZE);
        annotation.width     = LOAD_U32(__ptr + ANNOTATION_ENTRY::WIDTH);
        annotation.height    = LOAD_U32(__ptr + ANNOTATION_ENTRY::HEIGHT);
        annotation.parent    = LOAD_U24(__ptr + ANNOTATION_ENTRY::PARENT);
        
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        
    }
    
    if (groups(__base))
    {
        auto SIZES          = get_group_sizes(__base);
        auto size_array     = SIZES.read_group_sizes(__base);
        
        auto BYTES          = get_group_bytes(__base);
        BYTES.read_bytes    (__base, size_array, annotations);
    }
    
    return annotations;
}
bool ANNOTATIONS::groups(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATIONS&>(*this).check_and_fetch_remote(__base);
#endif
    auto SIZES_OFFSET = LOAD_U64(__base + __offset + GROUP_SIZES_OFFSET);
    auto BYTES_OFFSET = LOAD_U64(__base + __offset + GROUP_BYTES_OFFSET);
    return  SIZES_OFFSET != NULL_OFFSET && SIZES_OFFSET < __size &&
            BYTES_OFFSET != NULL_OFFSET && BYTES_OFFSET < __size;
}
ANNOTATION_GROUP_SIZES ANNOTATIONS::get_group_sizes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATIONS&>(*this).check_and_fetch_remote(__base);
#endif
    auto __GROUP_SIZES = ANNOTATION_GROUP_SIZES
    (LOAD_U64(__base + __offset + GROUP_SIZES_OFFSET), __size, __version);
    
    auto result = __GROUP_SIZES.validate_offset(__base);
    if (result & IRIS_FAILURE) throw std::runtime_error(result.message);
    return __GROUP_SIZES;
}
ANNOTATION_GROUP_BYTES ANNOTATIONS::get_group_bytes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATIONS&>(*this).check_and_fetch_remote(__base);
#endif
    auto offset = LOAD_U64(__base + __offset + GROUP_BYTES_OFFSET);
    if (offset == NULL_OFFSET || offset > __size) throw std::runtime_error
        ("Invalid tile table offset value for ANNOTATION_GROUP_BYTES array.");
    auto __BYTES = ANNOTATION_GROUP_BYTES(offset, __size, __version);
    
    __BYTES.validate_offset(__base);
    return __BYTES;
}
#ifdef __EMSCRIPTEN__
void ANNOTATIONS::check_and_fetch_remote(const BYTE *const &base)
{
if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url,__remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
Size SIZE_ANNOTATION_ARRAY(const AnnotationArrayCreateInfo &info)
{
    #if IrisCodecExtensionValidateEncoding
    Size size = ANNOTATIONS::HEADER_SIZE;
    for (auto&& annotation : info.annotations)
        if (annotation.identifier == Abstraction::Annotation::NULL_ID) printf
                ("WARNING: Annotation does not contain an identifier. Per the IFE Specification, Section 2.4.9, each annotation within the annotations array shall be referenced by a unique 24-bit identifier.");
        else size += ANNOTATION_ENTRY::SIZE;
    return size;
    #else
    return ANNOTATION_ARRAY::HEADER_SIZE +
    ANNOTATION_ENTRY::SIZE * info.annotations.size();
    #endif
}
void STORE_ANNOTATION_ARRAY(BYTE *const __base, const AnnotationArrayCreateInfo &info)
{
    using Annotation = Abstraction::Annotation;
    #if IrisCodecExtensionValidateEncoding
    if (info.offset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store associated annotations array -- NULL_OFFSET provided as location");
    if (info.annotations.size() > UINT32_MAX) throw std::runtime_error
        ("Failed to store annotations array -- array too large (" +
         std::to_string(info.annotations.size())+
         "). Per the IFE specification Section 2.4.9, the number of associated / ancillary images must be less than the 32-bit max value.");
    #endif
    
    auto __ptr  = __base + info.offset;
    STORE_U64(__ptr + ANNOTATIONS::VALIDATION,     info.offset);
    STORE_U16(__ptr + ANNOTATIONS::RECOVERY,       RECOVER_ANNOTATIONS);
    STORE_U16(__ptr + ANNOTATIONS::ENTRY_SIZE,     ANNOTATION_ENTRY::SIZE);
    __ptr += ANNOTATIONS::HEADER_SIZE;
    
    int entries = 0;
    for (auto&& annotation : info.annotations) {
        
        #if IrisCodecExtensionValidateEncoding
        if (annotation.identifier >= Annotation::NULL_ID) { printf
            ("WARNING: Annotation does not contain a valid identifier. Per the IFE Specification, Section 2.4.9, each annotation within the annotations array shall be referenced by a unique 24-bit identifier.");
            continue;
        }
        if (annotation.bytesOffset == NULL_OFFSET) { printf
            ("WARNING: Annotation (ID %d) does not contain a valid annotation byte array offset. Per the IFE Specification, Section 2.4.9, each annotation within the annotations array should have a valid byte stream encoding the visual object.", annotation.identifier);
            continue;
        } switch (annotation.type) {
            case Iris::ANNOTATION_PNG:
            case Iris::ANNOTATION_JPEG:
            case Iris::ANNOTATION_SVG:
            case Iris::ANNOTATION_TEXT: break;
            case Iris::ANNOTATION_UNDEFINED: printf
                ("WARNING: Annotation (ID %d) does not contain a valid annotation type. Per the IFE Specification, Section 2.4.9, each annotation within the annotations array should be one of the valid formats (Enumeration 2.2.6).", annotation.identifier);
                    continue;
        }
        if (annotation.parent > Annotation::NULL_ID) { printf
            ("WARNING: Annotation (ID %d) parent identifier is out of valid 24-bit range. Per the IFE Specification, Section 2.4.9, each annotation within the annotations array shall be referenced by a unique 24-bit identifier. The invalid parent identifier has been replaced with NULL_ID", annotation.identifier);
            const_cast<uint32_t&>(annotation.parent) = Annotation::NULL_ID;
        }
        #endif
        
        STORE_U24(__ptr + ANNOTATION_ENTRY::IDENTIFIER,     annotation.identifier);
        STORE_U64(__ptr + ANNOTATION_ENTRY::BYTES_OFFSET,   annotation.bytesOffset);
        STORE_U8 (__ptr + ANNOTATION_ENTRY::FORMAT,         annotation.type);
        STORE_F32(__ptr + ANNOTATION_ENTRY::X_LOCATION,     annotation.xLocation);
        STORE_F32(__ptr + ANNOTATION_ENTRY::Y_LOCATION,     annotation.yLocation);
        STORE_F32(__ptr + ANNOTATION_ENTRY::X_SIZE,         annotation.xSize);
        STORE_F32(__ptr + ANNOTATION_ENTRY::Y_SIZE,         annotation.ySize);
        STORE_U32(__ptr + ANNOTATION_ENTRY::WIDTH,          annotation.width);
        STORE_U32(__ptr + ANNOTATION_ENTRY::HEIGHT,         annotation.height);
        STORE_U24(__ptr + ANNOTATION_ENTRY::PARENT,         annotation.parent);
        __ptr   += ANNOTATION_ENTRY::SIZE;
        entries += 1;
    }
    // Store the actual number of entries encoded (excluding those that failed validation).
    STORE_U32(__base + info.offset + ANNOTATIONS::ENTRY_NUMBER, U32_CAST(entries));
}
#endif
// MARK: - ANNOTATION BYTES
ANNOTATION_BYTES::ANNOTATION_BYTES  (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size ANNOTATION_BYTES::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size = HEADER_V1_0_SIZE + BYTES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ANNOTATION_BYTES::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
void ANNOTATION_BYTES::read_bytes(const BYTE *const __base, Annotation &annotation) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr        = __base + __offset;
    annotation.byteSize     = LOAD_U32(__ptr + ENTRY_NUMBER);

    Offset start            = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_BYTES;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_BYTES:
    if (start + annotation.byteSize > __size)
        throw std::runtime_error
        ("ANNOTATION_BYTES::read_bytes failed validation -- bytes block ("+
         std::to_string(start) + "-" +
         std::to_string(start+annotation.byteSize)+
         "bytes) extends beyond the end of the file.");
        
    annotation.offset   = start;
    return;
}
#ifdef __EMSCRIPTEN__
void ANNOTATION_BYTES::check_and_fetch_remote(const BYTE *const &base)
{
if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url,__remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#else
Size SIZE_ANNOTATION_BYTES(const IrisCodec::Annotation &annotation)
{
    return ANNOTATION_BYTES::HEADER_SIZE + annotation.data->size();
}
void STORE_ANNOTATION_BYTES(BYTE *const __base, Offset offset, const IrisCodec::Annotation &annotation)
{
    auto& bytes = annotation.data;
    #if IrisCodecExtensionValidateEncoding
    if (offset == NULL_OFFSET) throw std::runtime_error
        ("Failed to store annotation bytes -- NULL_OFFSET provided as location");
    switch (annotation.type) {
        case Iris::ANNOTATION_PNG:
        case Iris::ANNOTATION_JPEG:
        case Iris::ANNOTATION_SVG:
        case Iris::ANNOTATION_TEXT: break;
        default: throw std::runtime_error
            ("Failed to store annotation bytes -- Undefined annotation type value (" +
             std::to_string(annotation.type) +
             "). Per the IFE specification Section 2.4.9, the format enumeration shall refer to the decoding algorithm used to convert the raw byte stream into a visual annotation object and shall be one of the enumerated values (Enumeration 2.2.6), excluding the undefined value (0)");
    }
    if (bytes->size() > UINT32_MAX) throw std::runtime_error
        ("Failed to store annotation bytes -- data block too large ("+
         std::to_string(bytes->size()) +
         " bytes). Per the IFE specification Section 2.4.9, the byte array shall contain less bytes than the 32-bit max value (4.29 GB).");
    #endif
    
    auto __ptr = __base + offset;
    STORE_U64(__ptr + ANNOTATION_BYTES::VALIDATION, offset);
    STORE_U16(__ptr + ANNOTATION_BYTES::RECOVERY, RECOVER_ANNOTATION_BYTES);
    STORE_U32(__ptr + ANNOTATION_BYTES::ENTRY_NUMBER, U32_CAST(bytes->size()));
    
    __ptr += IMAGE_BYTES::HEADER_SIZE;
    memcpy(__ptr, bytes->data(), bytes->size());
    return;
}
#endif
// MARK: - ANNOTATION_GROUPS
ANNOTATION_GROUP_SIZES::ANNOTATION_GROUP_SIZES (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK (offset, file_size, version)
{
    
}
Size ANNOTATION_GROUP_SIZES::size (const BYTE* const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size = HEADER_V1_0_SIZE + STEP * ENTRIES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ANNOTATION_GROUP_SIZES::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result ANNOTATION_GROUP_SIZES::validate_full (const BYTE *const __base, Size& expected_bytes) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    auto result         = validate_offset(__base);
    if (result & IRIS_FAILURE) return result;
    
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto VALIDATE_GROUPS;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    VALIDATE_GROUPS:
    if (start + ENTRIES * STEP > __size) return Result
        (IRIS_FAILURE, "ANNOTATION_GROUP_SIZES failed validation -- sizes array block (location "+
         std::to_string(start)+ " - " +
         std::to_string(start + ENTRIES * STEP)+
         " bytes) extends beyond the end of file.");
    
    const BYTE* __array   = __base + start;
    expected_bytes  = 0;
    for (auto GI = 0; GI < ENTRIES; ++GI, __array+=STEP) {
        expected_bytes += LOAD_U16(__ptr + ANNOTATION_GROUP_SIZE::LABEL_SIZE);
        expected_bytes += LOAD_U24(__ptr + ANNOTATION_GROUP_SIZE::ENTRIES_NUMBER)
        * TYPE_SIZE_UINT24 /*byte size of Annotation Identifier*/;
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        
    }
    return result;
}
ANNOTATION_GROUP_SIZES::GroupSizes ANNOTATION_GROUP_SIZES::read_group_sizes(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_SIZES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto STEP     = LOAD_U16(__ptr + ENTRY_SIZE);
    const auto ENTRIES  = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Offset start        = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_GROUP_SIZES;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_GROUP_SIZES:
    ANNOTATION_GROUP_SIZES::GroupSizes sizes (ENTRIES);
    if (start + ENTRIES * STEP > __size) throw std::runtime_error
        ("ANNOTATION_GROUP_SIZES failed -- sizes array block (location "+
         std::to_string(start)+ " - " +
         std::to_string(start + ENTRIES * STEP)+
         " bytes) extends beyond the end of file.");
    
    const BYTE* __array   = __base + start;
    for (auto GI = 0; GI < ENTRIES; ++GI, __array+=STEP) {
        sizes[GI] = {
            LOAD_U16(__ptr + ANNOTATION_GROUP_SIZE::LABEL_SIZE),
            LOAD_U32(__ptr + ANNOTATION_GROUP_SIZE::ENTRIES_NUMBER),
        };
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        
    }
    return sizes;
}
#ifdef __EMSCRIPTEN__
void ANNOTATION_GROUP_SIZES::check_and_fetch_remote(const BYTE *const &base)
{
if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url,__remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#endif
// MARK: - ANNOTATION_GROUP_BYTES
ANNOTATION_GROUP_BYTES::ANNOTATION_GROUP_BYTES (Offset offset, Size file_size, uint32_t version) noexcept :
DATA_BLOCK(offset, file_size, version)
{
    
}
Size ANNOTATION_GROUP_BYTES::size(const BYTE *const __base) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    Size size = HEADER_V1_0_SIZE + BYTES;
    if (__version > IRIS_EXTENSION_1_0); else return size;
    
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    return size;
}
Result ANNOTATION_GROUP_BYTES::validate_offset(const BYTE *const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    return DATA_BLOCK::validate_offset(__base, type, recovery);
}
Result ANNOTATION_GROUP_BYTES::validate_full (const BYTE *const __base, Size expected_bytes) const noexcept
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    if (BYTES != expected_bytes) return Result
        (IRIS_FAILURE,"ANNOTATION_GROUP_BYTES failed validation -- expected bytes ("+
         std::to_string(expected_bytes)+
         ") from ANNOTATIONS array does not match the byte size of the ANNOTATION_GROUP_BYTES block (" +
         std::to_string(BYTES) +
         ")");
    if (__offset + BYTES > __size) return Result
        (IRIS_FAILURE, "ANNOTATION_GROUP_BYTES failed validation -- full attributes byte array block (location "+
         std::to_string(__offset)+ " - " +
         std::to_string(__offset + LOAD_U32(__ptr + ENTRY_NUMBER))+
         ") extends beyond end of file.");
    
    return IRIS_SUCCESS;
}
void ANNOTATION_GROUP_BYTES::read_bytes (const BYTE *const __base, const GroupSizes &sizes, Annotations &annotations) const
{
#ifdef __EMSCRIPTEN__
    const_cast<ANNOTATION_GROUP_BYTES&>(*this).check_and_fetch_remote(__base);
#endif
    const auto __ptr    = __base + __offset;
    const auto BYTES    = LOAD_U32(__ptr + ENTRY_NUMBER);
    
    {   // Validate for bounds check
        Size total_size = 0;
        for (auto&& size : sizes)
            total_size+= size.first + size.second * TYPE_SIZE_UINT24;
        if (total_size != BYTES)
            throw std::runtime_error
            ("ANNOTATION_GROUP_BYTES::read_bytes failed -- expected bytes ("+
             std::to_string(total_size)+
             ") from ANNOTATION_GROUP_SIZES array does not match the byte size of the ANNOTATION_GROUP_BYTES block (" +
             std::to_string(BYTES) +
             "). Did you validate?");
    }
    
    Offset start = __offset + HEADER_V1_0_SIZE;
    if (__version > IRIS_EXTENSION_1_0); else goto READ_GROUP_OFFSETS;

    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    
    READ_GROUP_OFFSETS:
    if (start + BYTES > __size) throw std::runtime_error
        ("Failed ANNOTATION_GROUP_BYTES::read_bytes -- out of bounds. Byte array block (location "+
         std::to_string(start)+ " - " +
         std::to_string(start + BYTES)+
         " bytes) extends beyond the end of file. Did you validate?");
    annotations.groups.clear();
    
    const BYTE* __array = __base + start;
    for (auto&& size : sizes) {
        annotations.groups[std::string((char*)__array,size.first)] =
        Abstraction::AnnotationGroup {
            .offset = start,
            .number = size.second
        };
        Size total_bytes = size.first + size.second * TYPE_SIZE_UINT24;
        start  += total_bytes;
        __array += total_bytes;
        
        if (__version > IRIS_EXTENSION_1_0); else continue;
        
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        // VERSION CONTROL: VERSION 2+ PARAMETERS ARE ADDED HERE
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
        
    }
}
#ifdef __EMSCRIPTEN__
void ANNOTATION_GROUP_BYTES::check_and_fetch_remote(const BYTE *const &base)
{
    if (!__response) {
        const char* url = REINTERPRET_ARRAY_START(base);
        __response      = FETCH_DATABLOCK(url,__remote, HEADER_SIZE);
        __offset        = __ptr_size;
        __response      = FETCH_DATABLOCK(url,__remote, size(base));
    } const_cast<const BYTE*&>(base) = __response->data;
}
#endif
} // END SERIALIZATION
} // END IRIS CODEC

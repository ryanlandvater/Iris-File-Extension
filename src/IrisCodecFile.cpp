//
//  IrisCodecFile.cpp
//  Iris
//
//  Created by Ryan Landvater on 1/10/24.
//
#include <system_error>
#if IRIS_INTERNAL
    #include "IrisCodecPriv.hpp"
#else
    #include <iostream>
    #include <filesystem>
    #include "IrisCodecTypes.hpp"
    #include "IrisCodecFile.hpp"
    #include <assert.h>
#endif
namespace IrisCodec {
inline void         GENERATE_TEMP_FILE          (const File& file);
inline size_t       GET_FILE_SIZE               (const File& file);
inline void         PERFORM_FILE_MAPPING        (const File& file);
inline size_t       RESIZE_FILE                 (const File& file, size_t bytes);
inline bool         LOCK_FILE                   (const File& file, bool exclusive, bool wait);
inline void         UNLOCK_FILE                 (const File& file);

// MARK: - WINDOWS FILE IO Implementations
#if _WIN32
#include <io.h>
size_t get_page_size() {
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    return sys_info.dwPageSize;
};
const size_t PAGE_SIZE = get_page_size();
inline void GENERATE_TEMP_FILE (File& file) {
    int result  = -1;
}
inline size_t GET_FILE_SIZE (File& file) {
    auto& handle = file->handle;
    int result = -1;

    // If no handle provided, return
    if (handle == NULL)
        throw std::system_error(errno, std::generic_category(),
            "Cannot determine file size. Invalid file handle");

    // Seek the end of the file.
    result = fseek(handle, 0L, SEEK_END);
    if (result == -1)
        throw std::system_error(errno, std::generic_category(),
            "Cannot determine file size. Failed to seek file end");

    // Get the file size
    size_t size = ftell(handle);

    result = fseek(handle, 0L, SEEK_SET);
    if (result == -1)
        throw std::system_error(errno, std::generic_category(),
            "Failed to return to file start during file size determination");

    file->size = size;
    return size;
}
inline void PERFORM_FILE_MAPPING (File& file) {
    // Of note, we will NEVER EVER map with execetion privilages for safety. EVER.

    // Get the WIN32 file handle
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file->handle));
    if (handle == NULL)
        throw std::system_error(errno, std::generic_category(),
            "failed to get a WIN32 file handle to map.");

    file->map = CreateFileMapping(
        handle,         // Windows file handle
        NULL,           // Default security access descriptor
        file->writeAccess ? PAGE_READWRITE : PAGE_READONLY,
        0,              // High 32 bits of 64 bit file size (zero for full file)
        0,              // Low 32 bits of 64 bit file size (zero for full file)
        NULL);
    if (file->map == NULL)
        throw std::system_error(errno, std::generic_category(),
            "failed to create file mapping object.");

    // Generate a view of the file
    file->ptr = (BYTE*) MapViewOfFile(
        file->map,      // File Mapping object handle
        file->writeAccess ? FILE_MAP_WRITE : FILE_MAP_READ,
        0, 0,           // We will always map from the beginning of the file
        0);             // And map the entire file (zero for full extent)

    // If no pointer was returned, the
    if (file->ptr == NULL)
        throw std::system_error(errno, std::generic_category(),
            "failed create mapped file view.");
}
inline static void UNMAP_FILE(HANDLE& map, BYTE*& ptr) {
    if (ptr == nullptr) return;
    if (FlushViewOfFile(ptr, 0) == false)
        throw std::system_error(errno, std::generic_category(),
            "Failed to fush mapped file data contents");
    if (UnmapViewOfFile(ptr) == false) {
        throw std::system_error(errno, std::generic_category(),
            "Failed to unmap file");
        ptr = nullptr;
    }
    if (map == INVALID_HANDLE_VALUE) return;
    if (CloseHandle(map) == false) {
        throw std::system_error(errno, std::generic_category(),
            "Failed to close file mapping object");
        map = INVALID_HANDLE_VALUE;
    }
}
inline size_t RESIZE_FILE (const File& file, size_t bytes) {
    auto& map = file->map;
    auto& ptr = file->ptr;
    auto& size = file->size;

    // Return if no change needed
    if (size == bytes) return size;

    // Get the WIN32 file handle
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file->handle));
    if (handle == INVALID_HANDLE_VALUE)
        throw std::system_error(errno, std::generic_category(),
            "failed to get a WIN32 file handle to map.");

    // Reinterpret the size_t variable as a Large_Integer (ugh Windows)
    LARGE_INTEGER& LI_bytes = reinterpret_cast<LARGE_INTEGER&>(bytes);

    UNMAP_FILE(map, ptr);

    // Set the new file size by moving the pointer to the new size
    // and then resizing the file to the location of the pointer
    if (SetFilePointerEx(
        handle,     // Move the file pointer for the file handle
        LI_bytes,   // A long-integer version of the new size in bytes
        NULL,       // From the beginning of the file (FILE_BEGIN)
        FILE_BEGIN ) == 0)
        throw std::system_error(errno, std::generic_category(),
            "Failed to move file pointer to new location in file");
    if (SetEndOfFile(handle) == false)
        throw std::system_error(errno, std::generic_category(),
            "Failed to ftruncate-resize file");

    file->map = CreateFileMapping(
        handle,         // Windows file handle
        NULL,           // Default security access descriptor
        file->writeAccess,   // Use above access flags
        0,              // High 32 bits of 64 bit file size (zero for full file)
        0,              // Low 32 bits of 64 bit file size (zero for full file)
        NULL);
    if (file->map == INVALID_HANDLE_VALUE)
        throw std::system_error(errno, std::generic_category(),
            "failed to create file mapping object.");

    // Generate a view of the file
    file->ptr = (BYTE*)MapViewOfFile(
        file->map,      // File Mapping object handle
        file->writeAccess ? FILE_MAP_WRITE : FILE_MAP_READ,
        0, 0,           // We will always map from the beginning of the file
        0);             // And map the entire file (zero for full extent)

    // If no pointer was returned, the
    if (file->ptr == NULL)
        throw std::system_error(errno, std::generic_category(),
            "failed create mapped file view.");

    size = bytes;
    return bytes;
}
inline bool LOCK_FILE(File& file, bool exclusive, bool wait)
{
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file->handle));

    int flags = 0;
    flags |= exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    flags |= wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY;

    // We will need to fix this in the future whenever we want to call async file locks...
    assert(wait == false && "Async callback has not be established for windows. OVERLAPPED hEvent cannot be 0 for assync");

    OVERLAPPED overlapped {
        .hEvent = 0
    };

    return LockFileEx(
        handle,
        flags,
        0,
        file->size & UINT32_MAX,
        file->size >> 32,
        &overlapped);

}
inline void UNLOCK_FILE(File& file)
{
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file->handle));

    OVERLAPPED overlapped{
        .hEvent = 0
    };

    if (UnlockFileEx(
        handle,
        0,
        file->size & UINT32_MAX,
        file->size >> 32,
        &overlapped) == false)
        throw std::system_error( errno, std::generic_category(),
            "Failed to unlock a locked file.");
}
#else
// MARK: - POSIX COMPLIENT IMPLEMENTATIONS
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <unistd.h>
const size_t PAGE_SIZE = getpagesize();
inline void GENERATE_TEMP_FILE (const File& file)
{
    // Create the file path template (the 'X' values will be modified).
    int result          = -1;
    auto& file_path     = const_cast<std::string&>(file->path);
    file_path           = std::filesystem::temp_directory_path().string() +
                          std::string("IrisCodecTemporaryFile_XXXXXX");
    
    // Ask the file system to create a unique temporary file
    int file_descriptor = mkstemp(const_cast<char*>(file_path.c_str()));
    if (file_descriptor == -1)
        throw std::system_error(errno,std::generic_category(),
                                "Failed to create an cache file.");
    
    // Unlink the file from the file system so that when the program
    // closes, the file will be immediately released. 
    result = unlink(file_path.c_str());
    if (result == -1)
        assert(false && "failed to unlink the file from the filesystem");
    
    // Open stream access to the file
    file->handle       = fdopen(file_descriptor, "wb+");
}
inline size_t GET_FILE_SIZE (const File& file)
{
    auto& handle = file->handle;
    int result   = -1;
    
    // If no handle provided, return
    if (handle == NULL)
        throw std::system_error(errno,std::generic_category(),
                                "Cannot determine file size. Invalid file handle");
    
    // Seek the end of the file.
    result = fseek(handle, 0L, SEEK_END);
    if (result == -1)
        throw std::system_error(errno,std::generic_category(),
                                "Cannot determine file size. Failed to seek file end");
    
    // Get the file size
    size_t size = ftell(handle);
    
    result = fseek(handle, 0L, SEEK_SET);
    if (result == -1)
        throw std::system_error(errno,std::generic_category(),
                                "Failed to return to file start during file size determination");
    
    file->size = size;
    return size;
}
inline size_t RESIZE_FILE (const File& file, size_t bytes)
{
    auto& handle    = file->handle;
    auto& ptr       = file->ptr;
    auto& size      = file->size;
    int result      = -1;
    
    // Return if no change needed
    if (size == bytes) return size;
    
    // Set the new file size
    result = ftruncate(fileno(handle), bytes);
    if (result == -1)
        throw std::system_error(errno,std::generic_category(),
                                "Failed to ftruncate-resize file");
    
    // If we are expanding the file, write a byte to trigger
    // the OS to recognize the new file size.
    if (bytes > size) {
        fseek(handle, 0L, SEEK_END);
        fwrite("", sizeof(char), 1, handle);
        fseek(handle, 0L, SEEK_SET);
    }
    
    // If the file is already mapped, we must update the mapping.
    if (ptr) {
        // Of note, we will NEVER EVER PROT_EXEC for safety. EVER.
        int access_flags    = file->writeAccess ? PROT_READ | PROT_WRITE : PROT_READ;
        
        // Get the file descriptor
        int posix_file      = fileno(file->handle);
        if (posix_file == -1)
            throw std::system_error(errno,std::generic_category(),
                                    "failed to get a posix file descriptor to map.");
        
        // Remap the new file size
        munmap                      (file->ptr, file->size);
        auto new_ptr        = mmap  (ptr, bytes,     // No initial ptr, but map all bytes
                                     access_flags,   // Access map based on write vs read/write
                                     MAP_SHARED,     // Allow other processes to see updates
                                     posix_file,0);  // Get the file descriptor, no offset
        if (new_ptr == NULL)
            throw std::system_error(errno,std::generic_category(),
                                    "Failed to remap resized file");
        
        // Assign the new ptr if it is new
        if (new_ptr != ptr) file->ptr = static_cast<BYTE*>(new_ptr);
    }
    
    size = bytes;
    return bytes;
}
inline void PERFORM_FILE_MAPPING (const File& file)
{
    // Of note, we will NEVER EVER PROT_EXEC for safety. EVER.
    int access_flags = file->writeAccess ? PROT_READ | PROT_WRITE : PROT_READ;
    
    // Get the file descriptor
    int posix_file   = fileno(file->handle);
    if (posix_file == -1)
        throw std::system_error(errno,std::generic_category(),
                                "failed to get a posix file descriptor to map.");
    
    // Map the file into memory using MMAP
    file->ptr = static_cast<BYTE*>(mmap(NULL, file->size,  // No initial ptr, but map all bytes
                                        access_flags,      // Access map based on write vs read/write
                                        MAP_SHARED,        // Allow other processes to see updates
                                        posix_file,0));    // Get the file descriptor, no offset
    
    // If no pointer was returned, the
    if (file->ptr == NULL)
        throw std::system_error(errno,std::generic_category(),
                                "failed to map the file.");

}
inline bool LOCK_FILE (const File& file, bool exclusive, bool wait)
{
    int posix_file  = fileno(file->handle);
    int flags       = 0;
    flags          |= exclusive ? LOCK_EX : LOCK_SH;
    flags          |= wait ? 0 : LOCK_NB; // If no wait, no thread block (NB)
    
    // If Flock fails, return false. Maybe add info later?
    int lock_sucscess = flock(posix_file, flags);
    if (lock_sucscess == -1)
        return false;
    
    // Lock achieved.
    return true;
}
inline void UNLOCK_FILE (const File& file)
{
    int posix_file      = fileno(file->handle);
    int unlock_success  = flock(posix_file, LOCK_UN);
    if (unlock_success == -1)
        throw std::system_error(errno,std::generic_category(),
                                "failed to unlock the file");
}
inline void UNMAP_FILE (BYTE*& ptr, size_t bytes)
{
    // If there is a ptr, unmap it
    if (ptr) munmap(ptr, bytes);
    
    // Then nullify the stale ptr
    ptr = NULL;
}
#endif // END POSIX COMPLIENT

__INTERNAL__File::__INTERNAL__File  (const FileOpenInfo& info) :
    path                            (info.filePath),
    writeAccess                     (info.writeAccess)
{
    
}
__INTERNAL__File::__INTERNAL__File  (const FileCreateInfo& info) :
    path                            (info.filePath),
    writeAccess                     (true)
{

}
__INTERNAL__File::__INTERNAL__File(const CacheCreateInfo& info)
{

}
__INTERNAL__File::~__INTERNAL__File ()
{
    // If the file is mapped, unmap
    #if _WIN32
    UNMAP_FILE(map, ptr);
    #else
    UNMAP_FILE      (ptr, size);
    #endif
    
    // Close the file
    if (handle) fclose (handle);
}
File create_file (const FileCreateInfo &create_info)
{
    try {
        File file = std::make_shared<__INTERNAL__File>(create_info);
        
        if (create_info.initial_size == 0)
            throw std::runtime_error("There must be an initial file size to map");
        
        // Create a file for reading and writing in binary format.
        #if _WIN32 
        fopen_s(&file->handle, file->path.data(), "wb+");
        #else
        file->handle = fopen(file->path.data(), "wb+");
        #endif
        if (!file->handle)
            throw std::runtime_error("Failed to create the file");
        
        // Size the initial file in memory
        RESIZE_FILE (file, create_info.initial_size);
        
        // Map the file into memory
        PERFORM_FILE_MAPPING (file);
        
        // Return the newly mapped file
        return file;
        
    } catch (std::system_error &e) {
        std::cerr   << "Failed to create file"
                    << create_info.filePath << ": "
                    << e.what() << " - "
                    << e.code().message() << "\n";
        return NULL;
    } catch (std::runtime_error &e) {
        std::cerr   << "Failed to create file"
                    << create_info.filePath << ": "
                    << e.what() << "\n";
        return NULL;
    }   return NULL;
}

/// Open a file for read or read-write access.
File    open_file (const FileOpenInfo &open_info)
{
    try {
        File file = std::make_shared<__INTERNAL__File>(open_info);


        // Open the file for reading or reading and writing depending on write access
        #if _WIN32
        file->handle = fopen(file->path.data(), open_info.writeAccess ? "rbR+" : "rbR");
        #else   
        file->handle = fopen(file->path.data(), open_info.writeAccess ? "rb+" : "rb");
        #endif
        if (!file->handle)
            throw std::runtime_error("Failed to open the file");

        // Get the file size.
        GET_FILE_SIZE(file);
        
        // Map the file into memory
        PERFORM_FILE_MAPPING(file);
        
        // Return the newly mapped file
        return file;
        
    } catch (std::system_error &e) {
        std::cerr   << "Failed to open file"
                    << open_info.filePath << ": "
                    << e.what() << " - "
                    << e.code().message() << "\n";
        
        // Return a nullptr
        return NULL;
    } catch (std::runtime_error &e) {
        std::cerr   << "Failed to open file"
                    << open_info.filePath << ": "
                    << e.what() << "\n";
        return NULL;
    }   return NULL;
}

/// Create a new file-system temporary file for temporary archiving of slide data to disk.
File create_cache_file (const CacheCreateInfo &create_info)
{
    try {
        File file = std::make_shared<__INTERNAL__File>(create_info);
        
        // Generate the unlinked and unique temporary cache file.
        GENERATE_TEMP_FILE(file);
        
        // Set the initial cache file to about 500 MB at the page break.
        RESIZE_FILE(file, ((size_t)5E8&~(PAGE_SIZE-1))+PAGE_SIZE);

        // Map the file into memory
        PERFORM_FILE_MAPPING(file);
        
        // Return the new cache file
        return file;
        
    } catch (std::system_error &e) {
        std::cerr   << "Failed to create an cache file: "
                    << e.what() << " - "
                    << e.code().message() << "\n";
        
        // Return a nullptr
        return NULL;
    } catch (std::runtime_error &e) {
        std::cerr   << "Failed to create file: "
                    << e.what() << "\n";
        return NULL;
    }   return NULL;
}

Result resize_file (const File &file, const struct FileResizeInfo &info)
{
    // Check if page alignment was requested
    auto size = info.pageAlign ?
    // if page align, drop the size to the closest page break
    // and then add one page tobe at least the request size
    (info.size & ~(PAGE_SIZE-1)) + PAGE_SIZE :
    // Else, just do the info.size
    info.size;
    
    try {
        RESIZE_FILE (file, size);
        return IRIS_SUCCESS;
    } catch (std::system_error&e) {
        return {
            IRIS_FAILURE,
            e.what()
        };
    } return IRIS_FAILURE;
}
} // END IRIS CODEC NAMESPACE

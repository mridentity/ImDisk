/*
Server end for ImDisk Virtual Disk Driver proxy operation.

Copyright (C) 2005-2023 Olof Lagerkvist.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
*/

//#define DEBUG

#define WIN32_LEAN_AND_MEAN
#define __USE_UNIX98

#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32

#pragma warning(disable: 4201)

#include <windows.h>
#include <winsock2.h>
#include <winioctl.h>
#include <io.h>

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")

#ifdef _M_ARM
#ifdef CharToOemA
#undef CharToOemA
#endif
#define CharToOemA(s,t)
#endif

__inline
BOOL
OemPrintF(FILE *Stream, LPCSTR Message, ...)
{
    va_list param_list;
    LPSTR lpBuf = NULL;

    va_start(param_list, Message);

    if (!FormatMessageA(78 |
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_STRING, Message, 0, 0,
        (LPSTR)&lpBuf, 0, &param_list))
        return FALSE;

    CharToOemA(lpBuf, lpBuf);
    fprintf(Stream, "%s\n", lpBuf);
    LocalFree(lpBuf);
    return TRUE;
}

__inline
void
syslog(FILE *Stream, LPCSTR Message, ...)
{
    va_list param_list;
    LPSTR MsgBuf = NULL;
    LPSTR msgptr = NULL;
    LPSTR alloc_msg = NULL;

    va_start(param_list, Message);

    msgptr = strstr(Message, "%m");

    if (msgptr != NULL)
    {
        size_t msgpos = msgptr - Message;

        DWORD winerrno = GetLastError();

        if (winerrno == NO_ERROR)
        {
            winerrno = 10000 + errno;
        }

        alloc_msg = _strdup(Message);

        if (alloc_msg == NULL)
        {
            return;
        }

        msgptr = alloc_msg + msgpos;

        *msgptr = 0;

        if (FormatMessage(FORMAT_MESSAGE_MAX_WIDTH_MASK |
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, winerrno, 0, (LPSTR)&MsgBuf, 0, NULL))
        {
            CharToOemA(MsgBuf, MsgBuf);
        }
        else
        {
            MsgBuf = NULL;
        }

        Message = alloc_msg;
    }

    if (MsgBuf != NULL)
    {
        vfprintf(Stream, Message, param_list);
        fprintf(Stream, "%s\n", MsgBuf);

        LocalFree(MsgBuf);
    }
    else
    {
        vfprintf(Stream, Message, param_list);
    }

    if (alloc_msg != NULL)
    {
        free(alloc_msg);
    }

    fflush(Stream);
    return;
}

#define LOG_ERR       stderr

#define OBJNAME_SIZE  260

LONG
WINAPI
ExceptionFilter(
LPEXCEPTION_POINTERS ExceptionInfo);

HANDLE shm_server_mutex = NULL;
HANDLE shm_request_event = NULL;
HANDLE shm_response_event = NULL;

OVERLAPPED drv_memory_io;
OVERLAPPED drv_request_io;

#else  // Unix

#include <syslog.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#endif

#include "../inc/imdproxy.h"
#include "devio_types.h"
#include "safeio.h"
#include "devio.h"

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

#ifndef O_FSYNC
#define O_FSYNC 0
#endif

#define DEF_BUFFER_SIZE ((int)((sizeof(void*) << 3) << 20))

#define DEF_REQUIRED_ALIGNMENT 1

#if defined(DEBUG) || defined(_DEBUG) || defined(DBG) || defined(SYSLOG)
#define dbglog(x) syslog x
#else
#define dbglog(x)
#endif

int64_t GetBigEndian64(int8_t *storage)
{
    int i;
    int64_t number = 0;
    for (i = 0; i < sizeof(int64_t); i++)
    {
        number |= (int64_t)storage[i] << ((sizeof(int64_t) - i - 1) << 3);
    }
    return number;
}

uint32_t GetLittleEndian32U(uint8_t *storage)
{
    int i;
    uint32_t number = 0;
    for (i = 0; i < sizeof(uint32_t); i++)
    {
        number |= (uint32_t)storage[i] << (i << 3);
    }
    return number;
}

int image_fd = -1;
void *libhandle = NULL;
SOCKET sd = INVALID_SOCKET;
int shm_mode = 0;
char *shm_readptr = 0;
char *shm_writeptr = 0;
char *shm_view = NULL;
char *buf = NULL;
char *buf2 = NULL;
safeio_size_t buffer_size = DEF_BUFFER_SIZE;
off_t_64 image_offset = 0;
IMDPROXY_INFO_RESP devio_info = { 0 };
char dll_mode = 0;
char drv_mode = 0;
char vhd_mode = 0;
char auto_vhd_detect = 1;

struct _VHD_INFO
{
    struct _VHD_FOOTER
    {
        uint8_t Cookie[8];
        uint32_t Features;
        uint32_t FileFormatVersion;
        int64_t DataOffset;
        uint32_t TimeStamp;
        uint32_t CreatorApplication;
        uint32_t CreatorVersion;
        uint32_t CreatorHostOS;
        int64_t OriginalSize;
        int8_t CurrentSize[sizeof(int64_t)];
        uint32_t DiskGeometry;
        uint32_t DiskType;
        uint32_t Checksum;
        uint8_t UniqueID[16];
        uint8_t SavedState;
        uint8_t Padding[427];
    } Footer;

    struct _VHD_HEADER
    {
        uint8_t Cookie[8];
        int64_t DataOffset;
        int8_t TableOffset[sizeof(int64_t)];
        uint32_t HeaderVersion;
        uint32_t MaxTableEntries;
        uint32_t BlockSize;
        uint32_t Checksum;
        uint8_t ParentUniqueID[16];
        uint32_t ParentTimeStamp;
        uint32_t Reserved1;
        uint16_t ParentName[256];
        struct _VHD_PARENT_LOCATOR
        {
            uint32_t PlatformCode;
            uint32_t PlatformDataSpace;
            uint32_t PlatformDataLength;
            uint32_t Reserved1;
            int64_t PlatformDataOffset;
        } ParentLocator[8];
        uint8_t Padding[256];
    } Header;

} vhd_info = { { { 0 } } };

safeio_size_t block_size = 0;
safeio_size_t sector_size = 512;
off_t_64 table_offset = 0;
int16_t block_shift = 0;
int16_t sector_shift = 0;
off_t_64 current_size = 0;

dllread_proc dll_read = NULL;
dllwrite_proc dll_write = NULL;
dllclose_proc dll_close = NULL;
dllopen_proc dll_open = NULL;

safeio_ssize_t
physical_read(void *io_ptr, safeio_size_t size, off_t_64 offset)
{
    if (dll_mode)
        return dll_read(libhandle, io_ptr, size, offset);
    else
        return pread(image_fd, io_ptr, size, offset);
}

safeio_ssize_t
physical_write(void *io_ptr, safeio_size_t size, off_t_64 offset)
{
    if (dll_mode)
        return dll_write(libhandle, io_ptr, size, offset);
    else
        return pwrite(image_fd, io_ptr, size, offset);
}

int
physical_close(int fd)
{
    if (dll_mode)
        return dll_close(libhandle);
    else
        return _close(fd);
}

#ifdef _WIN32

int alloc_drv_buffer()
{
    HANDLE hFileMap = NULL;
    safeio_size_t detected_buffer_size;
    MEMORY_BASIC_INFORMATION memory_info;
    ULARGE_INTEGER map_size = { 0 };

    printf("Allocating new buffer: " SIZ_FMT " bytes.\n", buffer_size);

    map_size.QuadPart = (ULONGLONG)buffer_size + IMDPROXY_HEADER_SIZE;

    hFileMap = CreateFileMapping(INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE | SEC_COMMIT,
        map_size.HighPart,
        map_size.LowPart,
        NULL);

    if (hFileMap == NULL)
    {
        syslog(LOG_ERR, "CreateFileMapping() failed: %m\n");
        return 2;
    }

    shm_view = (char*)MapViewOfFile(hFileMap, FILE_MAP_WRITE, 0, 0, 0);

    if (shm_view == NULL)
    {
        syslog(LOG_ERR, "MapViewOfFile() failed: %m\n");
        return 2;
    }

    CloseHandle(hFileMap);

    buf = shm_view + IMDPROXY_HEADER_SIZE;

    if (!VirtualQuery(shm_view, &memory_info, sizeof(memory_info)))
    {
        syslog(LOG_ERR, "VirtualQuery() failed: %m\n");
        return 2;
    }

    memset(shm_view, 0, memory_info.RegionSize);

    detected_buffer_size = (safeio_size_t)
        (memory_info.RegionSize - IMDPROXY_HEADER_SIZE);

    if (buffer_size != detected_buffer_size)
    {
        buffer_size = detected_buffer_size;
        if (buf2 != NULL)
        {
            free(buf2);
            buf2 = (char*)malloc(buffer_size);
            if (buf2 == NULL)
            {
                syslog(LOG_ERR, "malloc() failed: %m\n");
                return 2;
            }
        }
    }

    ResetEvent(drv_memory_io.hEvent);

    if (!DeviceIoControl((HANDLE)sd, IOCTL_DEVIODRV_LOCK_MEMORY, &shm_view, sizeof(void*), shm_view, buffer_size + IMDPROXY_HEADER_SIZE, NULL, &drv_memory_io))
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            dbglog((LOG_ERR, "Memory successfully locked.\n"));
        }
        else
        {
            syslog(LOG_ERR, "Lock memory request failed: %m\n");
            return 3;
        }
    }

    return 0;
}

int
shm_read(void *io_ptr, safeio_size_t size)
{
    if (io_ptr == buf)
        if (size <= buffer_size)
            return 1;
        else
            return 0;

    if (shm_readptr == NULL)
        if (drv_mode)
            shm_readptr = shm_view + sizeof(IMDPROXY_DEVIODRV_BUFFER_HEADER);
        else
            shm_readptr = shm_view;

    if ((long long)size > (long long)(buf - shm_readptr))
        return 0;

    memcpy(io_ptr, shm_readptr, size);
    shm_readptr = shm_readptr + size;

    return 1;
}

int
shm_write(const void *io_ptr, safeio_size_t size)
{
    if (io_ptr == buf)
        if (size <= buffer_size)
            return 1;
        else
            return 0;

    if (shm_writeptr == NULL)
        if (drv_mode)
            shm_writeptr = shm_view + sizeof(IMDPROXY_DEVIODRV_BUFFER_HEADER);
        else
            shm_writeptr = shm_view;

    if ((long long)size > (long long)(buf - shm_writeptr))
        return 0;

    memcpy(shm_writeptr, io_ptr, size);
    shm_writeptr = shm_writeptr + size;

    return 1;
}

int
shm_flush()
{
    shm_readptr = NULL;
    shm_writeptr = NULL;

    if (!SetEvent(shm_response_event))
    {
        syslog(LOG_ERR, "SetEvent() failed: %m\n");
        return 0;
    }

    if (WaitForSingleObject(shm_request_event, INFINITE) != WAIT_OBJECT_0)
        return 0;

    return 1;
}

int
drv_flush()
{
    DWORD dw;

    shm_readptr = NULL;
    shm_writeptr = NULL;

    dbglog((LOG_ERR, "Calling DeviceIoControl for exchanging requests.\n"));

    ResetEvent(drv_request_io.hEvent);

    while (!DeviceIoControl((HANDLE)sd, IOCTL_DEVIODRV_EXCHANGE_IO, &shm_view, sizeof(void*), NULL, 0, &dw, &drv_request_io))
    {
        DWORD err = GetLastError();

        if (err == ERROR_IO_PENDING)
        {
            dbglog((LOG_ERR, "Waiting for request to complete.\n"));

            if (GetOverlappedResult((HANDLE)sd, &drv_request_io, &dw, TRUE))
            {
                dbglog((LOG_ERR, "Request complete.\n"));

                break;
            }

            err = GetLastError();
        }

        dbglog((LOG_ERR, "Request failed: %i %m", err));

        if (err == ERROR_INSUFFICIENT_BUFFER)
        {
            // need larger buffer (for write request, detected in driver)
            dbglog((LOG_ERR, "Larger buffer needed.\n"));

            if (!GetOverlappedResult((HANDLE)sd, &drv_memory_io, &dw, TRUE) &&
                GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            {
                syslog(LOG_ERR, "Error waiting for memory unlock: %i %m", GetLastError());
            }

            UnmapViewOfFile(shm_view);

            shm_view = NULL;

            buffer_size <<= 1;

            if (alloc_drv_buffer() == 0)
            {
                continue;
            }
            else
            {
                return 0;
            }
        }
        else if (err == ERROR_DEV_NOT_EXIST)
        {
            return 1;
        }
        else
        {
            syslog(LOG_ERR, "DeviceIoControl() failed: %m\n", err);
            return 0;
        }
    }

    return 1;
}

#else  // Unix

int
shm_read(void *io_ptr, safeio_size_t size)
{
    return 0;
}

int
shm_write(const void *io_ptr, safeio_size_t size)
{
    return 0;
}

int
shm_flush()
{
    return 0;
}

int
drv_flush()
{
  return 0;
}

#endif

void
buf_realloc(ULONGLONG new_size)
{
    if (shm_mode)
        return;

    if (new_size > (((safeio_size_t)-1) >> 1))
    {
        new_size = (((safeio_size_t)-1) >> 1);
    }

    buffer_size = (safeio_size_t)new_size;

    dbglog((LOG_ERR, "Read request " SLL_FMT " bytes, reallocating buffer.\n",
        (off_t_64)buffer_size));

#ifdef _WIN32
    if (drv_mode)
    {
        DWORD dw;

        char* existing_buf = buf;
        char* existing_buf2 = buf2;
        char* existing_shm_view = shm_view;
        safeio_size_t existing_buffer_size = buffer_size;

        if (!GetOverlappedResult((HANDLE)sd, &drv_memory_io, &dw, TRUE) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            syslog(LOG_ERR, "Error waiting for memory unlock: %i %m", GetLastError());
        }

        if (alloc_drv_buffer() == 0)
        {
            memcpy(shm_view, existing_shm_view, IMDPROXY_HEADER_SIZE);
            UnmapViewOfFile(existing_shm_view);
            free(existing_buf2);
        }
        else
        {
            shm_view = existing_shm_view;
            buf = existing_buf;
            buf2 = existing_buf2;
            buffer_size = existing_buffer_size;
        }
    }
    else
#endif
    {
        char *new_buf = (char*)malloc(buffer_size);
        char *new_buf2 = (char*)malloc(buffer_size);
        if (new_buf == NULL || new_buf2 == NULL)
        {
            syslog(LOG_ERR, "Failed allocating new buffer: %m\n");

            if (new_buf != NULL)
                free(new_buf);
            if (new_buf2 != NULL)
                free(new_buf2);
        }
        else
        {
            free(buf);
            buf = new_buf;
            free(buf2);
            buf2 = new_buf2;
        }
    }
}

int
comm_flush()
{
    if (shm_mode)
        return shm_flush();
    else if (drv_mode)
        return drv_flush();
    else
        return 1;
}

int
comm_read(void *io_ptr, safeio_size_t size)
{
    if (shm_mode || drv_mode)
        return shm_read(io_ptr, size);
    else
        return safe_read(sd, io_ptr, size);
}

int
comm_write(const void *io_ptr, safeio_size_t size)
{
    if (shm_mode || drv_mode)
        return shm_write(io_ptr, size);
    else
        return safe_write(sd, io_ptr, size);
}

int
send_info()
{
    if (!comm_write(&devio_info, sizeof devio_info))
        return 0;

    if (!comm_flush())
    {
        syslog(LOG_ERR, "Error flushing comm data: %m\n");

        return 0;
    }

    return 1;
}

safeio_ssize_t
vhd_read(char *io_ptr, safeio_size_t size, off_t_64 offset)
{
    off_t_64 block_number;
    off_t_64 data_offset;
    safeio_size_t in_block_offset;
    uint32_t block_offset;
    safeio_size_t first_size = size;
    off_t_64 second_offset = 0;
    safeio_size_t second_size = 0;
    safeio_ssize_t readdone;

    dbglog((LOG_ERR, "vhd_read: Request " SLL_FMT " bytes at " SLL_FMT ".\n",
        (off_t_64)size, (off_t_64)offset));

    if (offset + size > current_size)
        return 0;

    block_number = (safeio_size_t)(offset >> block_shift);
    data_offset = table_offset + (block_number << 2);
    in_block_offset = (safeio_size_t)offset & (block_size - 1);
    if (first_size + in_block_offset > block_size)
    {
        first_size = block_size - in_block_offset;
        second_size = size - first_size;
        second_offset = offset + first_size;
    }

    readdone = physical_read(&block_offset, sizeof(block_offset), data_offset);
    if (readdone != sizeof(block_offset))
    {
        syslog(LOG_ERR, "vhd_read: Error reading block table: %m\n");

        if (errno == 0)
            errno = E2BIG;

        return (safeio_ssize_t)-1;
    }

    memset(io_ptr, 0, size);

    if (block_offset == 0xFFFFFFFF)
        readdone = first_size;
    else
    {
        block_offset = ntohl(block_offset);

        data_offset =
            (((off_t_64)block_offset) << sector_shift) + sector_size +
            in_block_offset;

        readdone = physical_read(io_ptr, (safeio_size_t)first_size, data_offset);
        readdone = physical_read(io_ptr, (safeio_size_t)first_size, data_offset);
        if (readdone == -1)
            return (safeio_ssize_t)-1;
    }

    if (second_size > 0)
    {
        safeio_size_t second_read =
            vhd_read(io_ptr + first_size, second_size, second_offset);

        if (second_read == -1)
            return (safeio_ssize_t)-1;

        readdone += second_read;
    }

    return readdone;
}

safeio_ssize_t
vhd_write(char *io_ptr, safeio_size_t size, off_t_64 offset)
{
    off_t_64 block_number;
    off_t_64 data_offset;
    safeio_size_t in_block_offset;
    uint32_t block_offset;
    safeio_size_t first_size = size;
    off_t_64 second_offset = 0;
    safeio_size_t second_size = 0;
    safeio_ssize_t readdone;
    safeio_ssize_t writedone;
    off_t_64 bitmap_offset;
    safeio_size_t bitmap_datasize;
    safeio_size_t first_size_nqwords;

    dbglog((LOG_ERR, "vhd_write: Request " SLL_FMT " bytes at " SLL_FMT ".\n",
        (off_t_64)size, (off_t_64)offset));

    if (offset + size > current_size)
        return 0;

    block_number = offset >> block_shift;
    data_offset = table_offset + (block_number << 2);
    in_block_offset = (safeio_size_t)offset & (block_size - 1);
    if (first_size + in_block_offset > block_size)
    {
        first_size = block_size - in_block_offset;
        second_size = size - first_size;
        second_offset = offset + first_size;
    }
    first_size_nqwords = (first_size + 7) >> 3;

    readdone = physical_read(&block_offset, sizeof(block_offset), data_offset);
    if (readdone != sizeof(block_offset))
    {
        syslog(LOG_ERR, "vhd_write: Error reading block table: %m\n");

        if (errno == 0)
            errno = E2BIG;

        return (safeio_ssize_t)-1;
    }

    // Alocate a new block if not already defined
    if (block_offset == 0xFFFFFFFF)
    {
        off_t_64 block_offset_bytes;
        char *new_block_buf;
        long long *buf_ptr;

        // First check if new block is all zeroes, in that case don't allocate
        // a new block in the vhd file
        for (buf_ptr = (long long*)io_ptr;
            (buf_ptr < (long long*)io_ptr + first_size_nqwords) ? 0 :
            (*buf_ptr == 0);
        buf_ptr++);
        if (buf_ptr >= (long long*)io_ptr + first_size_nqwords)
        {
            dbglog((LOG_ERR, "vhd_write: New empty block not added to vhd file "
                "backing " SLL_FMT " bytes at " SLL_FMT ".\n",
                (off_t_64)first_size, (off_t_64)offset));

            writedone = first_size;

            if (second_size > 0)
            {
                safeio_size_t second_write =
                    vhd_write(io_ptr + first_size, second_size, second_offset);

                if (second_write == -1)
                    return (safeio_ssize_t)-1;

                writedone += second_write;
            }

            return writedone;
        }

        dbglog((LOG_ERR, "vhd_write: Adding new block to vhd file backing "
            SLL_FMT " bytes at " SLL_FMT ".\n",
            (off_t_64)first_size, (off_t_64)offset));

        new_block_buf = (char *)
            malloc((size_t)sector_size + block_size + sizeof(vhd_info.Footer));
        if (new_block_buf == NULL)
        {
            syslog(LOG_ERR, "vhd_write: Error allocating memory buffer for new "
                "block: %m\n");

            return (safeio_ssize_t)-1;
        }

        // New block is placed where the footer currently is
        block_offset_bytes =
            _lseeki64(image_fd, -(off_t_64) sizeof(vhd_info.Footer), SEEK_END);
        if (block_offset_bytes == -1)
        {
            syslog(LOG_ERR, "vhd_write: Error moving file pointer to last "
                "block: %m\n");

            free(new_block_buf);
            return (safeio_ssize_t)-1;
        }

        // Store pointer to new block start sector in BAT
        block_offset = htonl((uint32_t)(block_offset_bytes >> sector_shift));
        readdone =
            physical_write(&block_offset, sizeof(block_offset), data_offset);
        if (readdone != sizeof(block_offset))
        {
            syslog(LOG_ERR, "vhd_write: Error updating BAT: %m\n");

            free(new_block_buf);

            if (errno == 0)
                errno = E2BIG;

            return (safeio_ssize_t)-1;
        }

        // Initialize new block with zeroes followed by the new footer
        memset(new_block_buf, 0, (size_t)sector_size + block_size);
        memcpy(new_block_buf + sector_size + block_size, &vhd_info.Footer,
            sizeof(vhd_info.Footer));

        readdone =
            physical_write(new_block_buf,
                (size_t)sector_size + block_size + sizeof(vhd_info.Footer),
                block_offset_bytes);

        if (readdone != (safeio_ssize_t)(sector_size + block_size) +
            (safeio_ssize_t)sizeof(vhd_info.Footer))
        {
            syslog(LOG_ERR, "vhd_write: Error writing new block: %m\n");

            free(new_block_buf);

            if (errno == 0)
                errno = E2BIG;

            return (safeio_ssize_t)-1;
        }

        free(new_block_buf);
    }

    // Calculate where actual data should be written
    block_offset = ntohl(block_offset);
    data_offset = (((off_t_64)block_offset) << sector_shift) + sector_size +
        in_block_offset;

    // Write data
    writedone = physical_write(io_ptr, (safeio_size_t)first_size, data_offset);
    if (writedone == -1)
        return (safeio_ssize_t)-1;

    // Calculate where and how many bytes in allocation bitmap we need to update
    bitmap_offset = ((off_t_64)block_offset << sector_shift) +
        (in_block_offset >> sector_shift >> 3);

    bitmap_datasize =
        (((first_size + sector_size - 1) >> sector_shift) + 7) >> 3;

    // Set bits as 'allocated'
    memset(buf2, 0xFF, bitmap_datasize);

    // Update allocation bitmap
    readdone = physical_write(buf2, bitmap_datasize, bitmap_offset);
    if (readdone != (safeio_ssize_t)bitmap_datasize)
    {
        syslog(LOG_ERR, "vhd_write: Error updating block bitmap: %m\n");

        if (errno == 0)
            errno = E2BIG;

        return (safeio_ssize_t)-1;
    }

    if (second_size > 0)
    {
        safeio_size_t second_write =
            vhd_write(io_ptr + first_size, second_size, second_offset);

        if (second_write == -1)
            return (safeio_ssize_t)-1;

        writedone += second_write;
    }

    return writedone;
}

safeio_ssize_t
logical_read(char *io_ptr, safeio_size_t size, off_t_64 offset)
{
    if (vhd_mode)
        return vhd_read(io_ptr, size, offset);
    else
        return physical_read(io_ptr, size, offset);
}

safeio_ssize_t
logical_write(char *io_ptr, safeio_size_t size, off_t_64 offset)
{
    if (vhd_mode)
        return vhd_write(io_ptr, size, offset);
    else
        return physical_write(io_ptr, size, offset);
}

int
read_data()
{
    IMDPROXY_READ_REQ req_block = { 0 };
    IMDPROXY_READ_RESP resp_block = { 0 };
    safeio_size_t size;
    safeio_ssize_t readdone;

    if (!comm_read(&req_block.offset,
        sizeof(req_block) - sizeof(req_block.request_code)))
    {
        syslog(LOG_ERR, "Error reading request header.\n");
        return 0;
    }

    if (req_block.length > buffer_size) // we will need larger buffer to complete this request
    {
        buf_realloc(req_block.length);
    }

    size = (safeio_size_t)
        (req_block.length < buffer_size ? req_block.length : buffer_size);

    dbglog((LOG_ERR, "read request " ULL_FMT " bytes at " ULL_FMT " + "
        ULL_FMT " = " ULL_FMT ".\n",
        req_block.length, req_block.offset, image_offset,
        req_block.offset + image_offset));

    memset(buf, 0, size);

    readdone =
        logical_read(buf, (safeio_size_t)size, (off_t_64)(image_offset + req_block.offset));

    if (readdone == -1)
    {
        resp_block.errorno = errno;
        resp_block.length = 0;
        syslog(LOG_ERR, "Device read: %m\n");
    }
    else
    {
        resp_block.errorno = 0;
        resp_block.length = size;

        if (req_block.length != readdone)
        {
            syslog(LOG_ERR,
                "Partial read at " SLL_FMT ": Got " SLL_FMT ", req " ULL_FMT ".\n",
                (int64_t)(image_offset + req_block.offset), (int64_t)readdone, req_block.length);
        }
    }

    dbglog((LOG_ERR, "read done reporting/sending " ULL_FMT " bytes.\n",
        resp_block.length));

    if (!comm_write(&resp_block, sizeof resp_block))
    {
        syslog(LOG_ERR, "Warning: I/O stream inconsistency.\n");
        return 0;
    }

    if (resp_block.errorno == 0)
        if (!comm_write(buf, (safeio_size_t)resp_block.length))
        {
            syslog(LOG_ERR, "Error sending read response to caller.\n");
            return 0;
        }

    if (!comm_flush())
    {
        syslog(LOG_ERR, "Error flushing comm data: %m\n");
        return 0;
    }

    return 1;
}

int
write_data()
{
    IMDPROXY_WRITE_REQ req_block = { 0 };
    IMDPROXY_WRITE_RESP resp_block = { 0 };

    if (!comm_read(&req_block.offset,
        sizeof(req_block) - sizeof(req_block.request_code)))
        return 0;

    dbglog((LOG_ERR, "write request " ULL_FMT " bytes at " ULL_FMT " + "
        ULL_FMT " = " ULL_FMT ".\n",
        req_block.length, req_block.offset, image_offset,
        req_block.offset + image_offset));

    if (req_block.length > buffer_size)
    {
        syslog(LOG_ERR, "Too big block write requested: %u bytes.\n",
            (int)req_block.length);
        return 0;
    }

    if (!comm_read(buf, (safeio_size_t)req_block.length))
    {
        syslog(LOG_ERR, "Warning: I/O stream inconsistency.\n");

        return 0;
    }

    if (devio_info.flags & IMDPROXY_FLAG_RO)
    {
        resp_block.errorno = EBADF;
        resp_block.length = 0;
        syslog(LOG_ERR, "Device write attempt on read-only device.\n");
    }
    else
    {
        safeio_ssize_t writedone = logical_write(buf, (safeio_size_t)req_block.length,
            (off_t_64)(image_offset + req_block.offset));
        if (writedone == -1)
        {
            resp_block.errorno = errno;
            resp_block.length = writedone;
#ifdef _WIN32
            perror("Device write");
#else
            syslog(LOG_ERR, "Device write: %m\n");
#endif
        }
        else
        {
            resp_block.errorno = 0;
            resp_block.length = writedone;
        }

        if (req_block.length != resp_block.length)
        {
            if (writedone < 0)
            {
                syslog(LOG_ERR, "Write error (code " ULL_FMT ") at " ULL_FMT ": Req " ULL_FMT ".\n",
                    resp_block.errorno, image_offset + req_block.offset, req_block.length);
            }
            else
            {
                syslog(LOG_ERR, "Partial write at " ULL_FMT ": Got " ULL_FMT ", req " ULL_FMT ".\n",
                    resp_block.errorno, image_offset + req_block.offset, req_block.length);
            }
        }

        dbglog((LOG_ERR, "write done reporting/sending " ULL_FMT " bytes.\n",
            resp_block.length));
    }

    if (!comm_write(&resp_block, sizeof resp_block))
    {
        syslog(LOG_ERR, "Error sending write response to caller.\n");

        return 0;
    }

    if (!comm_flush())
    {
        syslog(LOG_ERR, "Error flushing comm data: %m\n");
        return 0;
    }

    return 1;
}

int
do_comm(char *comm_device);

int
main(int argc, char **argv)
{
    safeio_ssize_t readdone;
    int partition_number = 0;
    char mbr[512];
    int retval;
    char *comm_device = NULL;

#ifdef _WIN32
    WSADATA wsadata;

    SetUnhandledExceptionFilter(ExceptionFilter);

    (void)WSAStartup(0x0101, &wsadata);
#endif

    if (argc > 1 && _stricmp(argv[1], "--dll") == 0)
    {
        fprintf(stderr,
            "devio with custom DLL support\n"
            "Copyright (C) 2005-2023 Olof Lagerkvist.\n"
            "\n"
            "Usage for unmanaged C/C++ DLL files:\n"
            "devio --dll=dllfile;procedure other_devio_parameters ...\n"
            "\n"
            "dllfile     Name of custom DLL file to use for device I/O.\n"
            "\n"
            "procedure   Name of procedure in DLL file to use for opening device. This\n"
            "            procedure must follow the dllopen_proc typedef as specified in\n"
            "devio.h.\n"
            "\n"
            "Declaration for dllopen is:\n"
            "void * __cdecl dllopen(const char *str,\n"
            "                       int read_only,\n"
            "                       dllread_proc *dllread,\n"
            "                       dllwrite_proc *dllwrite,\n"
            "                       dllclose_proc *dllclose,\n"
            "                       __int64 *size)\n"
            "\n"
            "str         Device name to open as specified at devio command line.\n"
            "\n"
            "read_only   A non-zero value requests a device to be opened in read only mode.\n"
            "\n"
            "dllread     Pointer to memory where dllopen should store address to a function\n"
            "            that is used when reading from device.\n"
            "\n"
            "dllwrite    Pointer to memory where dllopen should store address to a function\n"
            "            that is used when writing to device. Address is ignored by devio\n"
            "            if device is opened for read only.\n"
            "\n"
            "dllclose    Pointer to memory where dllopen should store address to a function\n"
            "            that is used when closing device.\n"
            "\n"
            "size        Pointer to memory where dllopen should store detected size of\n"
            "            successfully opened device. This is optional.\n"
            "\n"
            "Types for dllread_proc, dllwrite_proc, dllclose_proc are declared in devio.h.\n"
            "\n"
            "Return value from dllopen is typed as void * to be able to hold as much data\n"
            "            for some kind of reference as current architecture allows. Devio\n"
            "            practically ignores this value, it is just sent in later calls to\n"
            "            dllread/dllwrite/dllclose. The only thing that devio checks is that\n"
            "            this value is not (void *)-1. That case is treated as an error\n"
            "            return.\n"
            "\n"
            "Value returned by dllopen will be passed by devio to to dllread, dllwrite and\n"
            "dllclose functions.\n"
            "\n"
            "Usage for .NET managed class library files:\n"
            "devio --dll=iobridge.dll;dllopen other_devio_parameters ...\n"
            "\n"
            "Parameter --dll=iobridge.dll;dllopen means to use iobridge.dll which is a\n"
            "mixed managed/unmanaged DLL that serves as a bridge to transfer requests to a\n"
            ".NET managed class library.\n"
            "\n"
            "The diskdev parameter to devio has somewhat special meaning in this case.\n"
            "Syntax of diskdev parameter is treated as follows:\n"
            "classlibraryfile::classname::procedure::devicename\n"
            "\n"
            "classlibraryfile\n"
            "            Name of .NET managed class library DLL file.\n"
            "\n"
            "classname::procedure\n"
            "            Name of class (managed type) and a static method in that class\n"
            "            to be used to open a Stream object to be used for I/O requests.\n"
            "\n"
            "devicename  User specified data, such as a device name, file name or similar,\n"
            "            that is sent as first parameter to above specified procedure.\n"
            "\n"
            "Declaration for classname::procedure:\n"
            "public static System.IO.Stream open_stream(String devicename, bool read_only)\n"
            "\n"
            "devicename  Device name to open as specified as part of diskdev parameter in\n"
            "            devio command line, as specified above.\n"
            "\n"
            "read_only   Value of true requests a device to be opened in read only mode.\n"
            "\n"
            "Return value from method needs to be a valid seekable stream object of a type\n"
            "that derives from System.IO.Stream class. Devio will use Read(), Write() and\n"
            "Close() methods as well as Position and Length properties on opened Stream\n"
            "object.\n");

        return -1;
    }

    if (argc >= 3  && _strnicmp(argv[1], "--dll=", 6) == 0)
    {
#ifdef _WIN32
        char *dllargs = argv[1] + 6;

        char *dllfile = strtok(dllargs, ";");
        char *dllfunc = strtok(NULL, "");

        HMODULE hDLL;

        hDLL = LoadLibrary(dllfile);
        if (hDLL == NULL)
        {
            syslog(stderr, "Error loading %s: %m\n", dllfile);
            return 1;
        }

        dll_open = (dllopen_proc)GetProcAddress(hDLL, dllfunc);
        if (dll_open == NULL)
        {
            syslog(stderr, "Cannot find procedure %s in %s: %m\n",
                dllfunc,
                dllfile);
            return 1;
        }

        dll_mode = 1;

        argc--;
        argv++;
#else
        fprintf(stderr, "Custom DLL mode only supported on Windows.\n");
        return -1;
#endif
    }

    if (argc >= 4 && strcmp(argv[1], "--drv") == 0)
    {
        drv_mode = 1;
        argv++;
        argc--;
    }

    if (argc >= 4 && strcmp(argv[1], "--novhd") == 0)
    {
        auto_vhd_detect = 0;
        argv++;
        argc--;
    }

    if (argc >= 4 && strcmp(argv[1], "-r") == 0)
    {
        devio_info.flags |= IMDPROXY_FLAG_RO;
        argv++;
        argc--;
    }

    if (argc < 3 || argc > 7)
    {
        fprintf(stderr,
            "devio - Device I/O Service ver " DEVIO_VERSION "\n"
            "With support for Microsoft VHD format, custom DLL files, shared memory proxy\n"
            "operation and also for use with DevIO Client Driver, if installed.\n"
            "Copyright (C) 2005-2023 Olof Lagerkvist.\n"
            "\n"
            "Usage:\n"
            "devio [-r] tcp-port|commdev diskdev [blocks] [offset] [alignm] [buffersize]\n"
            "devio [-r] tcp-port|commdev diskdev [partitionnumber] [alignm] [buffersize]\n"
            "\n"
            "-r      Open image file in read-only mode.\n"
            "\n"
            "tcp-port can be any free tcp port where this service should listen for incoming\n"
            "client connections.\n"
            "\n"
            "commdev is a path to a communications port, named pipe or similar where this\n"
            "service should listen for incoming client connections.\n"
            "\n"
            "commdev can also start with shm: followed by an section object name for using\n"
            "shared memory communication. Alternatively, drv: followed by a name for using\n"
            "DevIO Client Driver to expose a device object connected to this devio instance.\n"
            "\n"
            "Default number of blocks is 0. When running on Windows the program will try to\n"
            "get the size of the image file or partition automatically, otherwise the client\n"
            "must know the exact size without help from this service.\n"
            "\n"
            "Default number of blocks for dynamically expanding VHD image files are read\n"
            "automatically from VHD header structure within image file.\n"
            "\n"
            "Default alignment is %u bytes.\n"
            "Default buffer size is %i bytes.\n"
            "\n"
            "For syntax help with custom I/O DLL under Windows, type:\n"
            "devio --dll\n",
            DEF_REQUIRED_ALIGNMENT,
            DEF_BUFFER_SIZE);
        return -1;
    }

    comm_device = argv[1];

    if (dll_mode)
    {
        if (devio_info.flags & IMDPROXY_FLAG_RO)
            libhandle = dll_open(argv[2], 1, &dll_read, &dll_write, &dll_close,
            (off_t_64*)&devio_info.file_size);
        else
            libhandle = dll_open(argv[2], 0, &dll_read, &dll_write, &dll_close,
            (off_t_64*)&devio_info.file_size);

        if (libhandle == NULL)
        {
            syslog(LOG_ERR, "Library call failed to open '%s': %m\n", argv[2]);
            return 1;
        }
    }
    else
    {
        if (devio_info.flags & IMDPROXY_FLAG_RO)
            image_fd = _open(argv[2], O_BINARY | O_DIRECT | O_FSYNC | O_RDONLY);
        else
            image_fd = _open(argv[2], O_BINARY | O_DIRECT | O_FSYNC | O_RDWR);

        if (image_fd == -1)
        {
            syslog(LOG_ERR, "Failed to open '%s': %m\n", argv[2]);
            return 1;
        }
    }

    printf("Successfully opened '%s'.\n", argv[2]);

    // Autodetect Microsoft .vhd files
    readdone = physical_read(&vhd_info, (safeio_size_t) sizeof(vhd_info), 0);

    if (auto_vhd_detect &&
        (readdone == sizeof(vhd_info)) &&
        (strncmp((char*)vhd_info.Header.Cookie, "cxsparse", 8) == 0) &&
        (strncmp((char*)vhd_info.Footer.Cookie, "conectix", 8) == 0) &&
        vhd_info.Footer.DiskType == 0x03000000UL)
    {
        void *geometry = &vhd_info.Footer.DiskGeometry;

        // VHD I/O uses a secondary buffer
        buf2 = (char*)malloc(buffer_size);
        if (buf2 == NULL)
        {
            syslog(LOG_ERR, "malloc() failed: %m\n");
            return 2;
        }

        puts("Detected dynamically expanding Microsoft VHD image file format.");

        // Calculate vhd shifts
        current_size = GetBigEndian64(vhd_info.Footer.CurrentSize);
        table_offset = GetBigEndian64(vhd_info.Header.TableOffset);

        sector_size = 512;

        block_size = ntohl(vhd_info.Header.BlockSize);

        for (block_shift = 0;
            (block_shift < 64) &&
            ((((safeio_size_t)1) << block_shift) != block_size);
        block_shift++);

        devio_info.file_size = current_size;

        vhd_mode = 1;

        printf("VHD block size: %u bytes. C/H/S geometry: %u/%u/%u.\n",
            (unsigned int)block_size,
            (unsigned int)ntohs(*(u_short*)geometry),
            (unsigned int)((u_char*)geometry)[2],
            (unsigned int)((u_char*)geometry)[3]);
    }

    for (sector_shift = 0;
        (sector_shift < 64) &&
        ((((safeio_size_t)1) << sector_shift) != sector_size);
        sector_shift++);

    if (argc > 3)
    {
        ULONGLONG spec_size = 0;
        char suf = 0;

        if (sscanf(argv[3], ULL_FMT "%c", &spec_size, &suf) == 2)
        {
            switch (suf)
            {
            case 'T':
                spec_size <<= 10;
            case 'G':
                spec_size <<= 10;
            case 'M':
                spec_size <<= 10;
            case 'K':
                spec_size <<= 10;
            case 'B':
                break;
            case 't':
                spec_size *= 1000;
            case 'g':
                spec_size *= 1000;
            case 'm':
                spec_size *= 1000;
            case 'k':
                spec_size *= 1000;
            case 'b':
                break;
            default:
                syslog(LOG_ERR, "Unsupported size suffix: %c\n", suf);
            }

            devio_info.file_size = spec_size;
        }
        else if (spec_size < 512)
        {
            partition_number = (int)spec_size;
        }
        else
        {
            devio_info.file_size = spec_size << 9;
        }
    }
    else
    {
        partition_number = 1;
    }

#ifdef _WIN32

    if (devio_info.file_size == 0)
    {
        if (dll_mode)
            syslog(LOG_ERR, "DLL did not return size of image/partition.\n");
        else
        {
            HANDLE h = (HANDLE)_get_osfhandle(image_fd);
            BY_HANDLE_FILE_INFORMATION by_handle_file_info;

            if ((!GetFileInformationByHandle(h, &by_handle_file_info) &&
                (by_handle_file_info.nFileSizeLow = GetFileSize(h,
                    &by_handle_file_info.nFileSizeHigh)) == INVALID_FILE_SIZE) &&
                GetLastError() != NO_ERROR)
            {
                // If not regular disk file, try to lock volume using FSCTL operation.

                DWORD dw;
                FlushFileBuffers(h);
                if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0,
                    &dw, NULL))
                {
                    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL,
                        0, &dw, NULL))
                    {
                        syslog(LOG_ERR, "Cannot dismount filesystem on %s.\n",
                            argv[2]);

                        if (~devio_info.flags & IMDPROXY_FLAG_RO)
                            return 9;
                    }
                }
                else
                {
                    switch (GetLastError())
                    {
                    case ERROR_NOT_SUPPORTED:
                    case ERROR_INVALID_FUNCTION:
                    case ERROR_INVALID_HANDLE:
                    case ERROR_INVALID_PARAMETER:
                        break;

                    default:
                        syslog(LOG_ERR, "Cannot dismount filesystem on %s.\n",
                            argv[2]);

                        if (~devio_info.flags & IMDPROXY_FLAG_RO)
                            return 9;
                    }
                }

                if (devio_info.file_size == 0)
                {
                    PARTITION_INFORMATION partition_info = { 0 };

                    if (!DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                        &partition_info, sizeof(partition_info),
                        &dw, NULL))
                    {
                        syslog(LOG_ERR,
                            "Cannot determine size of disk volume.\n");
                    }
                    else
                    {
                        devio_info.file_size =
                            partition_info.PartitionLength.QuadPart;
                    }
                }
            }
            else
            {
                LARGE_INTEGER file_size = { 0 };
                file_size.HighPart = by_handle_file_info.nFileSizeHigh;
                file_size.LowPart = by_handle_file_info.nFileSizeLow;

                devio_info.file_size = file_size.QuadPart;
            }
        }
    }
#else
    if (devio_info.file_size == 0)
    {
        struct stat file_stat = { 0 };
        if (fstat(image_fd, &file_stat) == 0)
            devio_info.file_size = file_stat.st_size;
        else
            syslog(LOG_ERR, "Cannot determine size of image/partition: %m\n");
    }
#endif

    if (current_size == 0)
        current_size = devio_info.file_size;

    if (devio_info.file_size != 0)
    {
        printf("Image size used: " ULL_FMT " bytes.\n", devio_info.file_size);
    }

    if (partition_number >= 1 && partition_number < 512)
    {
        if (logical_read(mbr, 512, 0) < 512)
        {
            syslog(LOG_ERR, "Error reading device: %m\n");
        }
        else if ((*(u_char*)(mbr + 0x01FE) == 0x55) &&
            (*(u_char*)(mbr + 0x01FF) == 0xAA) &&
            ((*(u_char*)(mbr + 0x01BE) & 0x7F) == 0) &&
            ((*(u_char*)(mbr + 0x01CE) & 0x7F) == 0) &&
            ((*(u_char*)(mbr + 0x01DE) & 0x7F) == 0) &&
            ((*(u_char*)(mbr + 0x01EE) & 0x7F) == 0))
        {
            size_t i = 0;
            int c = 0;

            puts("Detected a master boot record at sector 0.");

            image_offset = 0;

            for (i = 0; i < 4; i++)
            {
                char type = *(mbr + 512 - 66 + (i << 4) + 4);

                if (type == 0)
                {
                    continue;
                }

                if (type == 0x05 || type == 0x0F)
                {
                    char read_next_ebr = TRUE;

                    off_t_64 first_ebr_offset =
                        ((off_t_64)GetLittleEndian32U((uint8_t*)mbr + 512 - 66 + (i << 4) + 8))
                        << sector_shift;

                    image_offset = first_ebr_offset;

                    while (read_next_ebr)
                    {
                        char ebr[512];
                        size_t e;

                        read_next_ebr = FALSE;

                        printf("Reading extended partition table at " SLL_FMT
                            ".\n", (int64_t)image_offset);

                        if (logical_read(ebr, 512, image_offset) == 512 &&
                            (*(u_char*)(ebr + 0x01FE) == 0x55) &&
                            (*(u_char*)(ebr + 0x01FF) == 0xAA) &&
                            ((*(u_char*)(ebr + 0x01BE) & 0x7F) == 0) &&
                            ((*(u_char*)(ebr + 0x01CE) & 0x7F) == 0) &&
                            ((*(u_char*)(ebr + 0x01DE) & 0x7F) == 0) &&
                            ((*(u_char*)(ebr + 0x01EE) & 0x7F) == 0))
                        {
                            puts("Found valid extended partition table.");
                        }
                        else
                        {
                            puts("Invalid extended partition table.");
                            break;
                        }

                        for (e = 0; e < 4; e++)
                        {
                            type = *(ebr + 512 - 66 + (e << 4) + 4);

                            if (type == 0)
                            {
                                continue;
                            }

                            if (type == 0x05 || type == 0x0F)
                            {
                                image_offset =
                                    first_ebr_offset + (((off_t_64)
                                        GetLittleEndian32U((uint8_t*)ebr + 512 - 66 + (e << 4) + 8)) << sector_shift);

                                read_next_ebr = TRUE;

                                break;
                            }

                            ++c;

                            if (c == partition_number)
                            {
                                image_offset += ((off_t_64)
                                    GetLittleEndian32U((uint8_t*)ebr + 512 - 66 + (e << 4) + 8))
                                    << sector_shift;
                                devio_info.file_size = ((off_t_64)
                                    GetLittleEndian32U((uint8_t*)ebr + 512 - 66 + (e << 4) + 12))
                                    << sector_shift;

                                break;
                            }
                        }
                    }
                }
                else
                {
                    ++c;

                    if (c == partition_number)
                    {
                        image_offset = ((off_t_64)
                            GetLittleEndian32U((uint8_t*)mbr + 512 - 66 + (i << 4) + 8))
                            << sector_shift;
                        devio_info.file_size = ((off_t_64)
                            GetLittleEndian32U((uint8_t*)mbr + 512 - 66 + (i << 4) + 12))
                            << sector_shift;

                        break;
                    }
                }
            }

            if ((devio_info.file_size == 0) ||
                ((current_size != 0) &&
                (image_offset + (off_t_64)devio_info.file_size > current_size)))
            {
                syslog(LOG_ERR,
                    "Partition %i not found.\n", partition_number);
                return 1;
            }

            printf("Using partition %i.\n", partition_number);
        }
        else
            puts("No master boot record detected. Using entire image.");
    }

    if (image_offset == 0 && argc > 4)
    {
        int64_t offset64 = image_offset;

        char suf = 0;
        if (sscanf(argv[4], SLL_FMT "%c", &offset64, &suf) == 2)
            switch (suf)
            {
            case 'T':
                offset64 <<= 10;
            case 'G':
                offset64 <<= 10;
            case 'M':
                offset64 <<= 10;
            case 'K':
                offset64 <<= 10;
            case 'B':
                break;
            case 't':
                offset64 *= 1000;
            case 'g':
                offset64 *= 1000;
            case 'm':
                offset64 *= 1000;
            case 'k':
                offset64 *= 1000;
            case 'b':
                break;
            default:
                syslog(LOG_ERR, "Unsupported size suffix: %c\n", suf);
            }
        else
            offset64 <<= 9;

        if ((((int64_t)(-1) - (off_t_64)(-1)) & offset64) != 0)
        {
            syslog(LOG_ERR, "Offset too big for this system.\n");
        }

        image_offset = (off_t_64)offset64;

        argc--;
        argv++;
    }

    if (argc > 4)
    {
        if (sscanf(argv[4], ULL_FMT, &devio_info.req_alignment) != 1)
        {
            syslog(LOG_ERR, "Invalid alignment specification: '%s'\n",
                argv[4]);

            return -1;
        }
    }
    else
    {
        devio_info.req_alignment = DEF_REQUIRED_ALIGNMENT;
    }

    if (argc > 5)
    {
        char suf = 0;
        if (sscanf(argv[5], SIZ_FMT "%c", &buffer_size, &suf) == 2)
        {
            switch (suf)
            {
            case 'T':
                buffer_size <<= 10;
            case 'G':
                buffer_size <<= 10;
            case 'M':
                buffer_size <<= 10;
            case 'K':
                buffer_size <<= 10;
            case 'B':
                break;
            case 't':
                buffer_size *= 1000;
            case 'g':
                buffer_size *= 1000;
            case 'm':
                buffer_size *= 1000;
            case 'k':
                buffer_size *= 1000;
            case 'b':
                break;
            default:
                syslog(LOG_ERR, "Unsupported size suffix: %c\n", suf);
            }
        }

        /*
        buffer_size = 0;
        sscanf(argv[5], "%u", &buffer_size);
        */
    }

    printf("Total size: " SLL_FMT " bytes. Using " ULL_FMT " bytes from offset "
        SLL_FMT ".\n"
        "Required alignment: " ULL_FMT " bytes.\n"
        "Buffer size: " SIZ_FMT " bytes.\n",
        (int64_t)current_size,
        devio_info.file_size,
        (int64_t)image_offset,
        devio_info.req_alignment,
        buffer_size);

    retval = do_comm(comm_device);

    printf("Image close result: %i\n", physical_close(image_fd));

    return retval;
}

#ifdef _WIN32
int
do_comm_shm(char *comm_device)
{
    HANDLE hFileMap = NULL;
    MEMORY_BASIC_INFORMATION memory_info = { 0 };
    safeio_size_t detected_buffer_size = 0;
    ULARGE_INTEGER map_size = { 0 };
    char *objname = (char*)malloc(OBJNAME_SIZE);
    char *namespace_prefix;
    HANDLE h = CreateFile("\\\\?\\Global", 0, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if ((h == INVALID_HANDLE_VALUE) && (GetLastError() == ERROR_FILE_NOT_FOUND))
        namespace_prefix = "";
    else
        namespace_prefix = "Global\\";

    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);

    if (objname == NULL)
    {
        syslog(LOG_ERR, "Memory allocation failed: %m\n");
        return -1;
    }

    puts("Shared memory operation.");

    _snprintf(objname, OBJNAME_SIZE,
        "%s%s", namespace_prefix, comm_device);
    objname[OBJNAME_SIZE - 1] = 0;

    map_size.QuadPart = (ULONGLONG)buffer_size + IMDPROXY_HEADER_SIZE;

    hFileMap = CreateFileMapping(INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE | SEC_COMMIT,
        map_size.HighPart,
        map_size.LowPart,
        objname);

    if (hFileMap == NULL)
    {
        syslog(LOG_ERR, "CreateFileMapping() failed: %m\n");
        return 2;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        syslog(LOG_ERR, "A service with this name is already running.\n");
        return 2;
    }

    shm_view = (char*)MapViewOfFile(hFileMap, FILE_MAP_WRITE, 0, 0, 0);

    if (shm_view == NULL)
    {
        syslog(LOG_ERR, "MapViewOfFile() failed: %m\n");
        return 2;
    }

    buf = shm_view + IMDPROXY_HEADER_SIZE;

    if (!VirtualQuery(shm_view, &memory_info, sizeof(memory_info)))
    {
        syslog(LOG_ERR, "VirtualQuery() failed: %m\n");
        return 2;
    }

    detected_buffer_size = (safeio_size_t)
        (memory_info.RegionSize - IMDPROXY_HEADER_SIZE);

    if (buffer_size != detected_buffer_size)
    {
        buffer_size = detected_buffer_size;
        if (buf2 != NULL)
        {
            free(buf2);
            buf2 = (char*)malloc(buffer_size);
            if (buf2 == NULL)
            {
                syslog(LOG_ERR, "malloc() failed: %m\n");
                return 2;
            }
        }
    }

    _snprintf(objname, OBJNAME_SIZE,
        "%s%s_Server", namespace_prefix, comm_device);
    objname[OBJNAME_SIZE - 1] = 0;
    shm_server_mutex = CreateMutex(NULL, FALSE, objname);

    if (shm_server_mutex == NULL)
    {
        syslog(LOG_ERR, "CreateMutex() failed: %m\n");
        return 2;
    }

    if (WaitForSingleObject(shm_server_mutex, 0) != WAIT_OBJECT_0)
    {
        syslog(LOG_ERR, "A service with this name is already running.\n");
        return 2;
    }

    _snprintf(objname, OBJNAME_SIZE,
        "%s%s_Request", namespace_prefix, comm_device);
    objname[OBJNAME_SIZE - 1] = 0;
    shm_request_event = CreateEvent(NULL, FALSE, FALSE, objname);

    if (shm_request_event == NULL)
    {
        syslog(LOG_ERR, "CreateEvent() failed: %m\n");
        return 2;
    }

    _snprintf(objname, OBJNAME_SIZE,
        "%s%s_Response", namespace_prefix, comm_device);
    objname[OBJNAME_SIZE - 1] = 0;
    shm_response_event = CreateEvent(NULL, FALSE, FALSE, objname);

    if (shm_response_event == NULL)
    {
        syslog(LOG_ERR, "CreateEvent() failed: %m\n");
        return 2;
    }

    free(objname);
    objname = NULL;

    shm_mode = 1;

    printf("Waiting for connection on object %s. Press Ctrl+C to cancel.\n",
        comm_device);

    if (WaitForSingleObject(shm_request_event, INFINITE) != WAIT_OBJECT_0)
    {
        syslog(LOG_ERR, "Wait failed: %m.\n");
        return 2;
    }

    printf("Connection on object %s.\n",
        comm_device);

    return 0;
}

int
do_comm_drv(char *comm_device)
{
    int rc;

    char *objname = (char*)malloc(OBJNAME_SIZE);
    if (objname == NULL)
    {
        syslog(LOG_ERR, "Memory allocation failed: %m");
        return -1;
    }

    drv_memory_io.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    drv_request_io.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (drv_memory_io.hEvent == NULL || drv_request_io.hEvent == NULL)
    {
        syslog(LOG_ERR, "Event object create failed: %m");
        return -1;
    }

    puts("Driver mode.");

    _snprintf(objname, OBJNAME_SIZE,
        "%ws\\%s", DEVIODRV_DEVICE_DOSDEV_NAME, comm_device);
    objname[OBJNAME_SIZE - 1] = 0;

    sd = (SOCKET)CreateFile(objname, GENERIC_ALL, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);

    if (sd == INVALID_SOCKET)
    {
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            syslog(LOG_ERR, "A service with this name is already running.\n");
        }
        else
        {
            syslog(LOG_ERR, "Error opening '%s': %m", objname);
        }

        return 2;
    }

    free(objname);
    objname = NULL;

    rc = alloc_drv_buffer();
    if (rc > 0)
    {
        return rc;
    }

    drv_mode = 1;

    printf("Waiting for client connection on object %s. Press Ctrl+C to cancel.\n",
        comm_device);

    ((PIMDPROXY_DEVIODRV_BUFFER_HEADER)shm_view)->request_code = IMDPROXY_REQ_INFO;

    if (!send_info())
    {
        syslog(LOG_ERR, "Wait failed: %m.\n");
        return 2;
    }

    printf("Connection on object %s.\n",
        comm_device);

    return 0;
}
#endif

int
do_comm(char *comm_device)
{
    ULONGLONG req = 0;
    u_short port = (u_short)strtoul(comm_device, NULL, 0);

    if (_strnicmp(comm_device, "shm:", 4) == 0)
    {
#ifdef _WIN32
        int shmresult = do_comm_shm(comm_device + 4);
        if (shmresult != 0)
            return shmresult;
#else
        fprintf(stderr, "Shared memory operation only supported on Windows.\n");
        return 2;
#endif
    }
    else if (_strnicmp(comm_device, "drv:", 4) == 0)
    {
#ifdef _WIN32
        int drvresult = do_comm_drv(comm_device + 4);
        if (drvresult != 0)
            return drvresult;
#else
        fprintf(stderr, "Driver operation only supported on Windows.\n");
        return 2;
#endif
    }
    else
    {
        buf = (char*)malloc(buffer_size);
        if (buf == NULL)
        {
            syslog(LOG_ERR, "malloc() failed: %m\n");
            return 2;
        }
    }

    if (shm_mode || drv_mode)
    {
    }
    else if (port != 0)
    {
        struct sockaddr_in saddr = { 0 };
        socklen_t i;
        SOCKET ssd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ssd == -1)
        {
            syslog(LOG_ERR, "socket() failed: %m\n");
            return 2;
        }

        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = INADDR_ANY;
        saddr.sin_port = htons(port);

        if (bind(ssd, (struct sockaddr*) &saddr, sizeof saddr) == -1)
        {
            syslog(LOG_ERR, "bind() failed port %u: %m\n", (unsigned int)port);
            return 2;
        }

        i = sizeof saddr;
        if (getsockname(ssd, (struct sockaddr*) &saddr, &i) == -1)
        {
            syslog(LOG_ERR, "getsockname() failed: %m\n");
            return 2;
        }

        if (listen(ssd, 1) == -1)
        {
            syslog(LOG_ERR, "listen() failed for %s:%u: %m\n",
                inet_ntoa(saddr.sin_addr),
                (unsigned int)ntohs(saddr.sin_port));
            return 2;
        }

        printf("Waiting for connection on port %u. Press Ctrl+C to cancel.\n",
            (unsigned int)ntohs(saddr.sin_port));

        i = sizeof saddr;
        sd = accept(ssd, (struct sockaddr*) &saddr, &i);
        if (sd == -1)
        {
            syslog(LOG_ERR, "accept() failed port %u: %m\n",
                (unsigned int)port);
            return 2;
        }

        closesocket(ssd);

        printf("Got connection from %s:%u.\n",
            inet_ntoa(saddr.sin_addr),
            (unsigned int)ntohs(saddr.sin_port));

        i = 1;
        if (setsockopt(sd, IPPROTO_TCP, TCP_NODELAY, (const char*)&i, sizeof i))
            syslog(LOG_ERR, "setsockopt(..., TCP_NODELAY): %m\n");
    }
    else if (strcmp(comm_device, "-") == 0)
    {
#ifdef _WIN32
        sd = (SOCKET)GetStdHandle(STD_INPUT_HANDLE);
#else
        sd = 0;
#endif

        dbglog((LOG_ERR, "Using stdin as comm device.\n"));
    }
    else
    {
#ifdef _WIN32
        sd = (SOCKET)CreateFile(comm_device,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
#else
        sd = open(comm_device, O_BINARY | O_RDWR | O_DIRECT | O_FSYNC);
#endif
        if (sd == -1)
        {
            syslog(LOG_ERR, "File open failed on '%s': %m\n", comm_device);
            return 1;
        }

        printf("Waiting for I/O requests on device '%s'.\n", comm_device);
    }

    for (;;)
    {
        if (!comm_read(&req, sizeof(req)))
        {
            puts("Connection closed.");
            return 0;
        }

        switch (req)
        {
        case IMDPROXY_REQ_INFO:
            if (!send_info())
                return 1;
            break;

        case IMDPROXY_REQ_READ:
            if (!read_data())
                return 1;
            break;

        case IMDPROXY_REQ_WRITE:
            if (!write_data())
                return 1;
            break;

        default:
            req = ENODEV;
            if (!comm_write(&req, sizeof req))
            {
                syslog(LOG_ERR, "stdout: %m\n");
                return 1;
            }
        }
    }
}

#ifdef _WIN32

LONG
WINAPI
ExceptionFilter(LPEXCEPTION_POINTERS ExceptionInfo)
{
    LPSTR MsgBuf = NULL;
    DWORD i;

    if (FormatMessage(78 |
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_HMODULE |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandle("ntdll.dll"),
        ExceptionInfo->ExceptionRecord->ExceptionCode, 0,
        (LPSTR)&MsgBuf, 0, NULL))
    {
        CharToOemA(MsgBuf, MsgBuf);
    }
    else
    {
        MsgBuf = NULL;
    }

    if (MsgBuf != NULL)
    {
        fprintf(stderr, "\n"
            "%s\n", MsgBuf);

        LocalFree(MsgBuf);
    }

    fprintf(stderr,
        "\n"
        "Fatal error - unhandled exception.\n"
        "\n"
        "Exception 0x%X at address 0x%p\n",
        ExceptionInfo->ExceptionRecord->ExceptionCode,
        ExceptionInfo->ExceptionRecord->ExceptionAddress);

    for (i = 0;
        i < ExceptionInfo->ExceptionRecord->NumberParameters;
        i++)
    {
        fprintf(stderr,
            "Parameter %u: 0x%p\n",
            i + 1,
            (LPVOID)ExceptionInfo->ExceptionRecord->ExceptionInformation[i]);
    }

    _flushall();
    ExitProcess((UINT)-1);
}

#endif

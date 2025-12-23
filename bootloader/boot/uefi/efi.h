#ifndef _EFI_H
#define _EFI_H

#include <stdint.h>
#include <stddef.h>

//types
typedef uint64_t UINTN;
typedef int64_t  INTN;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t  UINT8;
typedef int32_t  INT32;
typedef int16_t  INT16;
typedef uint16_t CHAR16;
typedef char     CHAR8;
typedef void     VOID;
typedef UINTN    EFI_STATUS;
typedef VOID*    EFI_HANDLE;
typedef VOID*    EFI_EVENT;
typedef UINT64   EFI_PHYSICAL_ADDRESS;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif
typedef UINT8 BOOLEAN;

typedef struct {
    UINT16 Year;       //1900 - 9999
    UINT8  Month;      //1 - 12
    UINT8  Day;        //1 - 31
    UINT8  Hour;       //0 - 23
    UINT8  Minute;     //0 - 59
    UINT8  Second;     //0 - 59
    UINT8  Pad1;
    UINT32 Nanosecond; //0 - 999,999,999
    INT16  TimeZone;   //-1440 to 1440 or 2047
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct {
    UINT32 Resolution;
    UINT32 Accuracy;
    BOOLEAN SetsToZero;
} EFI_TIME_CAPABILITIES;

#define EFIAPI __attribute__((ms_abi))
#define IN
#define OUT
#define OPTIONAL
#define CONST const

//status codes

#define EFI_SUCCESS              0ULL
#define EFI_ERROR(x)             ((INTN)(x) < 0)
#define EFI_NOT_READY            (0x8000000000000000ULL | 6)
#define EFI_NOT_FOUND            (0x8000000000000000ULL | 14)
#define EFI_BUFFER_TOO_SMALL     (0x8000000000000000ULL | 5)
#define EFI_UNSUPPORTED          (0x8000000000000000ULL | 3)
#define EFI_ABORTED              (0x8000000000000000ULL | 21)
#define EFI_LOAD_ERROR           (0x8000000000000000ULL | 1)

//allocation types for AllocatePages
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

//GUIDs

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

#define EFI_FILE_INFO_GUID \
    {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

#define ACPI_20_TABLE_GUID \
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}

#define ACPI_TABLE_GUID \
    {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

//console protocols

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN CHAR16 *String);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID *Reset;
    EFI_TEXT_STRING OutputString;
    VOID *TestString;
    VOID *QueryMode;
    VOID *SetMode;
    VOID *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    VOID *SetCursorPosition;
    VOID *EnableCursor;
    VOID *Mode;
};

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    OUT EFI_INPUT_KEY *Key);

struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET Reset;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    EFI_EVENT WaitForKey;
};

//GOP

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    VOID *QueryMode;
    VOID *SetMode;
    VOID *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

//filesystem

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

#define EFI_FILE_READ_ONLY  0x0000000000000001ULL
#define EFI_FILE_DIRECTORY  0x0000000000000010ULL

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    VOID   *CreateTime;
    VOID   *LastAccessTime;
    VOID   *ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    IN EFI_FILE_PROTOCOL *This,
    OUT EFI_FILE_PROTOCOL **NewHandle,
    IN CHAR16 *FileName,
    IN UINT64 OpenMode,
    IN UINT64 Attributes);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(IN EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    IN EFI_FILE_PROTOCOL *This,
    IN OUT UINTN *BufferSize,
    OUT VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    IN EFI_FILE_PROTOCOL *This,
    IN EFI_GUID *InformationType,
    IN OUT UINTN *BufferSize,
    OUT VOID *Buffer);

struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    VOID *Delete;
    EFI_FILE_READ Read;
    VOID *Write;
    VOID *GetPosition;
    VOID *SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    VOID *SetInfo;
    VOID *Flush;
};

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    OUT EFI_FILE_PROTOCOL **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
};

//loaded image protocol
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    VOID *SystemTable;
    EFI_HANDLE DeviceHandle;
    VOID *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

//memory map
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

//boot services
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    IN EFI_ALLOCATE_TYPE Type,
    IN EFI_MEMORY_TYPE MemoryType,
    IN UINTN Pages,
    IN OUT EFI_PHYSICAL_ADDRESS *Memory);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    IN EFI_PHYSICAL_ADDRESS Memory,
    IN UINTN Pages);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    IN EFI_MEMORY_TYPE PoolType,
    IN UINTN Size,
    OUT VOID **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(IN VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    IN OUT UINTN *MemoryMapSize,
    OUT EFI_MEMORY_DESCRIPTOR *MemoryMap,
    OUT UINTN *MapKey,
    OUT UINTN *DescriptorSize,
    OUT UINT32 *DescriptorVersion);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    IN EFI_HANDLE ImageHandle,
    IN UINTN MapKey);

typedef EFI_STATUS (EFIAPI *EFI_STALL)(IN UINTN Microseconds);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    IN EFI_HANDLE Handle,
    IN EFI_GUID *Protocol,
    OUT VOID **Interface);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    IN EFI_GUID *Protocol,
    IN VOID *Registration OPTIONAL,
    OUT VOID **Interface);

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
    //services
    VOID *RaiseTPL;                    //0
    VOID *RestoreTPL;                  //1
    EFI_ALLOCATE_PAGES AllocatePages;  //2
    EFI_FREE_PAGES FreePages;          //3
    EFI_GET_MEMORY_MAP GetMemoryMap;   //4
    EFI_ALLOCATE_POOL AllocatePool;    //5
    EFI_FREE_POOL FreePool;            //6
    VOID *CreateEvent;                 //7
    VOID *SetTimer;                    //8
    VOID *WaitForEvent;                //9
    VOID *SignalEvent;                 //10
    VOID *CloseEvent;                  //11
    VOID *CheckEvent;                  //12
    VOID *InstallProtocolInterface;    //13
    VOID *ReinstallProtocolInterface;  //14
    VOID *UninstallProtocolInterface;  //15
    EFI_HANDLE_PROTOCOL HandleProtocol;//16
    VOID *Reserved2;                   //17
    VOID *RegisterProtocolNotify;      //18
    VOID *LocateHandle;                //19
    VOID *LocateDevicePath;            //20
    VOID *InstallConfigurationTable;   //21
    VOID *LoadImage;                   //22
    VOID *StartImage;                  //23
    VOID *Exit;                        //24
    VOID *UnloadImage;                 //25
    EFI_EXIT_BOOT_SERVICES ExitBootServices; //26
    VOID *GetNextMonotonicCount;       //27
    EFI_STALL Stall;                   //28
    VOID *SetWatchdogTimer;            //29
    VOID *ConnectController;           //30
    VOID *DisconnectController;        //31
    VOID *OpenProtocol;                //32
    VOID *CloseProtocol;               //33
    VOID *OpenProtocolInformation;     //34
    VOID *ProtocolsPerHandle;          //35
    VOID *LocateHandleBuffer;          //36
    EFI_LOCATE_PROTOCOL LocateProtocol;//37
} EFI_BOOT_SERVICES;

typedef EFI_STATUS (EFIAPI *EFI_GET_TIME)(
    OUT EFI_TIME *Time,
    OUT EFI_TIME_CAPABILITIES *Capabilities OPTIONAL
);

//runtime services
typedef EFI_STATUS (EFIAPI *EFI_SET_VIRTUAL_ADDRESS_MAP)(
    IN UINTN MemoryMapSize,
    IN UINTN DescriptorSize,
    IN UINT32 DescriptorVersion,
    IN EFI_MEMORY_DESCRIPTOR *VirtualMap);

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
    //services
    EFI_GET_TIME GetTime;               //0
    VOID *SetTime;                      //1
    VOID *GetWakeupTime;                //2
    VOID *SetWakeupTime;                //3
    EFI_SET_VIRTUAL_ADDRESS_MAP SetVirtualAddressMap; //4
    VOID *ConvertPointer;               //5
    VOID *GetVariable;                  //6
    VOID *GetNextVariableName;          //7
    VOID *SetVariable;                  //8
    VOID *GetNextHighMonotonicCount;    //9
    VOID *ResetSystem;                  //10
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    VOID *VendorTable;
} EFI_CONFIGURATION_TABLE;

//system table

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif

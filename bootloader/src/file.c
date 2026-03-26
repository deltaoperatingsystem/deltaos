#include "file.h"

EFI_STATUS file_load(
    EFI_HANDLE image_handle,
    EFI_BOOT_SERVICES *bs,
    CHAR16 *path, 
    void **data, 
    uint64_t *size
) {
    EFI_STATUS status;
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fi_guid = EFI_FILE_INFO_GUID;
    
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file;
    
    status = bs->HandleProtocol(image_handle, &lip_guid, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) return status;
    
    status = bs->HandleProtocol(loaded_image->DeviceHandle, &fs_guid, (VOID **)&fs);
    if (EFI_ERROR(status)) return status;
    
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) return status;
    
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        root->Close(root);
        return status;
    }
    
    VOID *info_buf = NULL;
    UINTN info_size = 0;

    //query the required size first so we don't depend on a fixed metadata buffer
    status = file->GetInfo(file, &fi_guid, &info_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        file->Close(file);
        root->Close(root);
        return EFI_LOAD_ERROR;
    }

    status = bs->AllocatePool(EfiLoaderData, info_size, &info_buf);
    if (EFI_ERROR(status)) {
        file->Close(file);
        root->Close(root);
        return status;
    }

    status = file->GetInfo(file, &fi_guid, &info_size, info_buf);
    if (EFI_ERROR(status)) {
        bs->FreePool(info_buf);
        file->Close(file);
        root->Close(root);
        return status;
    }

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    *size = info->FileSize;

    status = bs->AllocatePool(EfiLoaderData, *size, data);
    if (EFI_ERROR(status)) {
        bs->FreePool(info_buf);
        file->Close(file);
        root->Close(root);
        return status;
    }

    UINTN read_size = *size;
    status = file->Read(file, &read_size, *data);
    if (EFI_ERROR(status) || read_size != *size) {
        bs->FreePool(*data);
        bs->FreePool(info_buf);
        file->Close(file);
        root->Close(root);
        return EFI_LOAD_ERROR;
    }

    bs->FreePool(info_buf);
    file->Close(file);
    root->Close(root);

    return EFI_SUCCESS;
}

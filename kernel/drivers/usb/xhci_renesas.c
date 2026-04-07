#include <drivers/usb/xhci.h>
#include <drivers/usb/usb.h>
#include <drivers/pci.h>
#include <fs/fs.h>
#include <fs/tmpfs.h>
#include <obj/handle.h>
#include <mm/kheap.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/time.h>
#include <errno.h>

#define RENESAS_FW_PATH             "/system/firmware/renesas_usb_fw.mem"
#define RENESAS_FW_MIN_SIZE         0x1000
#define RENESAS_FW_MAX_SIZE         0x10000
#define RENESAS_FW_VERSION          0x6C
#define RENESAS_FW_STATUS           0xF4
#define RENESAS_FW_STATUS_MSB       0xF5
#define RENESAS_ROM_STATUS          0xF6
#define RENESAS_ROM_STATUS_MSB      0xF7
#define RENESAS_DATA0               0xF8
#define RENESAS_DATA1               0xFC

#define RENESAS_FW_STATUS_DOWNLOAD_ENABLE (1 << 0)
#define RENESAS_FW_STATUS_LOCK            (1 << 1)
#define RENESAS_FW_STATUS_RESULT_MASK     (0x7 << 4)
#define RENESAS_FW_STATUS_SUCCESS         (1 << 4)
#define RENESAS_FW_STATUS_ERROR           (1 << 5)
#define RENESAS_FW_STATUS_SET_DATA0       (1 << 0)
#define RENESAS_FW_STATUS_SET_DATA1       (1 << 1)

#define RENESAS_ROM_STATUS_ACCESS         (1 << 0)
#define RENESAS_ROM_STATUS_ERASE          (1 << 1)
#define RENESAS_ROM_STATUS_RELOAD         (1 << 2)
#define RENESAS_ROM_STATUS_RESULT_MASK    (0x7 << 4)
#define RENESAS_ROM_STATUS_SUCCESS        (1 << 4)
#define RENESAS_ROM_STATUS_ERROR          (1 << 5)
#define RENESAS_ROM_STATUS_SET_DATA0      (1 << 0)
#define RENESAS_ROM_STATUS_SET_DATA1      (1 << 1)
#define RENESAS_ROM_STATUS_ROM_EXISTS     (1 << 15)

#define RENESAS_ROM_ERASE_MAGIC           0x5A65726F
#define RENESAS_ROM_WRITE_MAGIC           0x53524F4D
#define RENESAS_RETRY                     50000
#define RENESAS_CHIP_ERASE_RETRY          500000
#define RENESAS_DELAY                     10

static bool xhci_renesas_fw_probe(pci_device_t *pci, uint32 *fw_version,
                                  uint16 *fw_status, uint16 *rom_status) {
    if (!pci || pci->vendor_id != 0x1033) return false;

    if (fw_version) {
        *fw_version = pci_config_read(pci->bus, pci->dev, pci->func, RENESAS_FW_VERSION, 4);
    }
    if (fw_status) {
        *fw_status = (uint16)pci_config_read(pci->bus, pci->dev, pci->func, RENESAS_FW_STATUS, 2);
    }
    if (rom_status) {
        *rom_status = (uint16)pci_config_read(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 2);
    }
    return true;
}

static bool xhci_renesas_check_rom(pci_device_t *pci) {
    uint16 rom_status = 0;
    if (!xhci_renesas_fw_probe(pci, NULL, NULL, &rom_status)) {
        return false;
    }
    return (rom_status & RENESAS_ROM_STATUS_ROM_EXISTS) != 0;
}

static int xhci_renesas_check_rom_state(pci_device_t *pci) {
    uint32 fw_version = 0;
    uint16 fw_status = 0;
    uint16 rom_status = 0;

    if (!xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
        return -1;
    }

    if ((rom_status & RENESAS_ROM_STATUS_ROM_EXISTS) &&
        ((rom_status & RENESAS_ROM_STATUS_RESULT_MASK) == RENESAS_ROM_STATUS_SUCCESS)) {
        return 0;
    }

    (void)fw_version;

    if (fw_status & RENESAS_FW_STATUS_LOCK) {
        if (fw_status & RENESAS_FW_STATUS_SUCCESS) return 0;
        return -EIO;
    }

    if (fw_status & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) {
        return -EIO;
    }

    switch (fw_status & RENESAS_FW_STATUS_RESULT_MASK) {
        case 0:
            return 1;
        case RENESAS_FW_STATUS_SUCCESS:
            return 0;
        case RENESAS_FW_STATUS_ERROR:
            return -ENODEV;
        default:
            return -EINVAL;
    }
}

static int xhci_renesas_check_running(pci_device_t *pci) {
    int rom = xhci_renesas_check_rom_state(pci);
    if (rom == 0) return 0;
    if (rom < 0 && rom != 1) return rom;

    uint32 fw_version = 0;
    uint16 fw_status = 0;
    uint16 rom_status = 0;
    if (!xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
        return -1;
    }

    (void)fw_version;
    (void)rom_status;

    if (fw_status & RENESAS_FW_STATUS_LOCK) {
        if (fw_status & RENESAS_FW_STATUS_SUCCESS) return 0;
        return -EIO;
    }

    if (fw_status & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) {
        return -EIO;
    }

    switch (fw_status & RENESAS_FW_STATUS_RESULT_MASK) {
        case 0:
            return 1;
        case RENESAS_FW_STATUS_SUCCESS:
            return 0;
        case RENESAS_FW_STATUS_ERROR:
            return -ENODEV;
        default:
            return -EINVAL;
    }
}

static int xhci_renesas_fw_verify_blob(const uint8 *fw, size len) {
    if (!fw || len < RENESAS_FW_MIN_SIZE || len >= RENESAS_FW_MAX_SIZE) {
        return -EINVAL;
    }

    if ((uint16)fw[0] != 0xAA || (uint16)fw[1] != 0x55) {
        return -EINVAL;
    }

    uint16 version_ptr = (uint16)fw[4] | ((uint16)fw[5] << 8);
    if ((size)version_ptr + 2 >= len) {
        return -EINVAL;
    }

    return 0;
}

static int xhci_renesas_fw_read_blob(const char *path, uint8 **fw_out, size *fw_len) {
    if (!path || !fw_out || !fw_len) return -EINVAL;

    stat_t st;
    if (handle_stat(path, &st) < 0 || st.type != FS_TYPE_FILE) {
        return -ENOENT;
    }

    if (st.size < RENESAS_FW_MIN_SIZE || st.size >= RENESAS_FW_MAX_SIZE ||
        (st.size & 3) != 0) {
        return -EINVAL;
    }

    object_t *file = tmpfs_open(path);
    if (!file) return -ENOENT;

    uint8 *buf = kmalloc(st.size);
    if (!buf) {
        object_release(file);
        return -ENOMEM;
    }

    ssize got = object_read(file, buf, st.size, 0);
    object_release(file);

    if (got != (ssize)st.size) {
        kfree(buf);
        return -EIO;
    }

    *fw_out = buf;
    *fw_len = st.size;
    return 0;
}

static int xhci_renesas_fw_download_image(pci_device_t *pci, const uint32 *fw,
                                          size step, bool rom) {
    uint32 status_reg = rom ? RENESAS_ROM_STATUS_MSB : RENESAS_FW_STATUS_MSB;
    bool data1 = (step & 1) != 0;
    uint32 bit = data1 ? RENESAS_FW_STATUS_SET_DATA1 : RENESAS_FW_STATUS_SET_DATA0;

    for (size i = 0; i < 50000; i++) {
        uint8 fw_status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                                 (uint16)status_reg, 1);
        if (!(fw_status & (uint8)bit)) break;
        usleep(10);
    }

    uint32 data_reg = data1 ? RENESAS_DATA1 : RENESAS_DATA0;
    uint32 data = fw[step];
    pci_config_write(pci->bus, pci->dev, pci->func, (uint16)data_reg, 4, data);
    usleep(100);
    pci_config_write(pci->bus, pci->dev, pci->func, (uint16)status_reg, 1, bit);

    return 0;
}

static int xhci_renesas_fw_download(pci_device_t *pci, const uint8 *fw, size len) {
    if (!pci || !fw || len < RENESAS_FW_MIN_SIZE) return -EINVAL;

    const uint32 *fw_words = (const uint32 *)fw;
    size word_count = len / 4;

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_FW_STATUS, 1,
                     RENESAS_FW_STATUS_DOWNLOAD_ENABLE);

    for (size i = 0; i < word_count; i++) {
        int err = xhci_renesas_fw_download_image(pci, fw_words, i, false);
        if (err) return err;
    }

    for (size i = 0; i < 50000; i++) {
        uint8 fw_status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                                 RENESAS_FW_STATUS_MSB, 1);
        if (!(fw_status & (RENESAS_FW_STATUS_SET_DATA0 | RENESAS_FW_STATUS_SET_DATA1)))
            break;
        usleep(10);
    }

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_FW_STATUS, 1, 0);

    for (size i = 0; i < 1000; i++) {
        uint16 fw_status = (uint16)pci_config_read(pci->bus, pci->dev, pci->func,
                                                   RENESAS_FW_STATUS, 1);
        if ((fw_status & RENESAS_FW_STATUS_LOCK) &&
            (fw_status & RENESAS_FW_STATUS_SUCCESS)) {
            return 0;
        }
        if ((fw_status & RENESAS_FW_STATUS_RESULT_MASK) == RENESAS_FW_STATUS_SUCCESS) {
            return 0;
        }
        usleep(100);
    }

    return -ETIMEDOUT;
}

static void xhci_renesas_rom_erase(pci_device_t *pci) {
    if (!pci) return;

    printf("[xhci] Renesas ROM erase\n");
    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_DATA0, 4,
                     RENESAS_ROM_ERASE_MAGIC);

    uint8 status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                          RENESAS_ROM_STATUS, 1);
    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1,
                     status | RENESAS_ROM_STATUS_ERASE);

    sleep(20);
    for (size i = 0; i < RENESAS_CHIP_ERASE_RETRY; i++) {
        status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                        RENESAS_ROM_STATUS, 1);
        if (!(status & RENESAS_ROM_STATUS_ERASE)) break;
        usleep(RENESAS_DELAY);
    }
}

static bool xhci_renesas_setup_rom(pci_device_t *pci, const uint8 *fw, size len) {
    if (!pci || !fw || len < RENESAS_FW_MIN_SIZE) return false;

    const uint32 *fw_words = (const uint32 *)fw;
    size word_count = len / 4;

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_DATA0, 4,
                     RENESAS_ROM_WRITE_MAGIC);
    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1,
                     RENESAS_ROM_STATUS_ACCESS);

    uint8 status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                          RENESAS_ROM_STATUS, 1);
    if (status & RENESAS_ROM_STATUS_RESULT_MASK) {
        pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
        return false;
    }

    for (size i = 0; i < word_count; i++) {
        if (xhci_renesas_fw_download_image(pci, fw_words, i, true) < 0) {
            pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
            return false;
        }
    }

    for (size i = 0; i < RENESAS_RETRY; i++) {
        status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                        RENESAS_ROM_STATUS_MSB, 1);
        if (!(status & (RENESAS_ROM_STATUS_SET_DATA0 | RENESAS_ROM_STATUS_SET_DATA1)))
            break;
        usleep(RENESAS_DELAY);
    }

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
    usleep(10);

    for (size i = 0; i < RENESAS_RETRY; i++) {
        status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                        RENESAS_ROM_STATUS, 1);
        status &= RENESAS_ROM_STATUS_RESULT_MASK;
        if (status == RENESAS_ROM_STATUS_SUCCESS) {
            pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1,
                             RENESAS_ROM_STATUS_RELOAD);
            for (size j = 0; j < RENESAS_RETRY; j++) {
                status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                                RENESAS_ROM_STATUS, 1);
                if (!(status & RENESAS_ROM_STATUS_RELOAD)) {
                    return true;
                }
                usleep(RENESAS_DELAY);
            }
            return false;
        }
        usleep(RENESAS_DELAY);
    }

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
    return false;
}

void xhci_renesas_fw_load(pci_device_t *pci) {
    if (!pci || pci->vendor_id != 0x1033 || pci->device_id != 0x0194) {
        return;
    }

    int running = xhci_renesas_check_running(pci);
    if (running == 0) {
        uint32 fw_version = 0;
        uint16 fw_status = 0;
        uint16 rom_status = 0;
        if (xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
            if ((rom_status & RENESAS_ROM_STATUS_ROM_EXISTS) &&
                ((rom_status & RENESAS_ROM_STATUS_RESULT_MASK) ==
                 RENESAS_ROM_STATUS_SUCCESS)) {
                printf("[xhci] Renesas ROM ready (ver=0x%08X rom=0x%04X)\n",
                       fw_version, rom_status);
            } else {
                printf("[xhci] Renesas firmware ready (ver=0x%08X fw=0x%04X rom=0x%04X)\n",
                       fw_version, fw_status, rom_status);
            }
        }
        return;
    }

    if (running < 0 && running != 1) {
        return;
    }

    uint8 *fw = NULL;
    size fw_len = 0;
    int err = xhci_renesas_fw_read_blob(RENESAS_FW_PATH, &fw, &fw_len);
    if (err) {
        printf("[xhci] Renesas firmware blob unavailable at %s (%d), continuing\n",
               RENESAS_FW_PATH, err);
        return;
    }

    err = xhci_renesas_fw_verify_blob(fw, fw_len);
    if (err) {
        printf("[xhci] Renesas firmware blob invalid (%d), continuing\n", err);
        kfree(fw);
        return;
    }

    if (xhci_renesas_check_rom(pci)) {
        xhci_renesas_rom_erase(pci);
        if (xhci_renesas_setup_rom(pci, fw, fw_len)) {
            uint32 fw_version = 0;
            uint16 fw_status = 0;
            uint16 rom_status = 0;
            if (xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
                printf("[xhci] Renesas ROM programmed (ver=0x%08X rom=0x%04X)\n",
                       fw_version, rom_status);
            }
            kfree(fw);
            return;
        }

        printf("[xhci] Renesas ROM programming failed, falling back to FW load\n");
    }

    uint32 fw_version = 0;
    uint16 fw_status = 0;
    uint16 rom_status = 0;
    if (xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
        printf("[xhci] loading Renesas firmware (ver=0x%08X fw=0x%04X rom=0x%04X)\n",
               fw_version, fw_status, rom_status);
    }

    err = xhci_renesas_fw_download(pci, fw, fw_len);
    kfree(fw);
    if (err) {
        printf("[xhci] Renesas firmware load failed (%d), continuing\n", err);
        return;
    }
}

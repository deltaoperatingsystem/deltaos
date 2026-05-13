#include <drivers/sb16.h>
#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/interrupts.h>
#include <arch/mmu.h>
#include <drivers/init.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <lib/io.h>
#include <lib/mem.h>
#include <lib/spinlock.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <proc/bottom_half.h>
#include <sys/audio.h>
#include <sys/opl.h>

//base ports to probe (0x220 is the most common default)
static const uint16 SB16_BASE_PORTS[] = { 0x220, 0x240, 0x260, 0x280 };

//runtime-detected resources from mixer
static uint16 sb16_base = 0;
static uint8  sb16_irq_num = 0;
static uint8  sb16_dma16 = 0;
static uint8  sb16_dma8 = 0;

//fallbacks used if mixer reports invalid values
#define SB16_DEFAULT_BASE  0x220
#define SB16_DEFAULT_IRQ   5
#define SB16_DEFAULT_DMA16 5
#define SB16_DEFAULT_DMA8  1

//port offsets relative to sb16_base
#define DSP_MIXER       (sb16_base + 0x4)
#define DSP_MIXER_DATA  (sb16_base + 0x5)
#define DSP_RESET       (sb16_base + 0x6)
#define DSP_READ        (sb16_base + 0xA)
#define DSP_WRITE       (sb16_base + 0xC)
#define DSP_READ_STAT   (sb16_base + 0xE)
#define DSP_INT_ACK_8   (sb16_base + 0xE)
#define DSP_INT_ACK_16  (sb16_base + 0xF)

//DSP commands
#define DSP_SET_OUTPUT_RATE   0x41
#define DSP_SPEAKER_ON        0xD1
#define DSP_DMA16_PAUSE       0xD5
#define DSP_DMA16_EXIT        0xD9   //exit auto-init mode
#define DSP_DMA16_AUTOINIT    0xB6   //start 16-bit auto-init playback
#define DSP_MODE_SIGNED_MONO  0x10   //mode byte for the B6 command

//OPL ports
#define OPL_REG_PORT      0x388
#define OPL_DATA_PORT     0x389
#define OPL_REG_PORT_OPL3 0x38A
#define OPL_DATA_PORT_OPL3 0x38B

//OPL registers
#define OPL_REG_WAVEFORM_ENABLE 0x01
#define OPL_REG_TIMER1          0x02
#define OPL_REG_TIMER_CTRL      0x04
#define OPL_REG_FM_MODE         0x08
#define OPL_REG_NEW             0x105   //OPL3 enable bit in second bank
#define OPL_REGS_TREMOLO        0x20
#define OPL_REGS_LEVEL          0x40
#define OPL_REGS_ATTACK         0x60
#define OPL_REGS_SUSTAIN        0x80
#define OPL_REGS_WAVEFORM       0xE0

//mixer registers for IRQ/DMA routing
#define MIXER_IRQ_SELECT      0x80
#define MIXER_DMA_SELECT      0x81

//double-buffer sizing (16-bit ISA DMA cannot cross 128KB physical boundary)
#define DMA16_BUFFER_BYTES    4096
#define DMA16_HALF_BYTES      (DMA16_BUFFER_BYTES / 2)
#define DMA16_TOTAL_SAMPLES   (DMA16_BUFFER_BYTES / sizeof(int16))
#define DMA16_HALF_SAMPLES    (DMA16_HALF_BYTES / sizeof(int16))
#define DMA16_BOUNDARY        0x20000

//ring buffer for userspace writes feeding the DMA refill
#define SB16_QUEUE_SAMPLES    4096
#define SB16_RATE_MIN         5000
#define SB16_RATE_MAX         44100

//driver lock (sb16_irq runs from interrupt context)
static spinlock_irq_t sb16_lock = SPINLOCK_IRQ_INIT;

static void sb16_bottom_half(void *arg);
static void sb16_irq(void);
static bool sb16_start_stream_locked(void);
static bottom_half_handle_t sb16_bh = BOTTOM_HALF_INVALID_HANDLE;

//default format ensures writes work before SET_FORMAT ioctl
static audio_format_t current_format = {
    .channels = 1,
    .format = AUDIO_FORMAT_PCM_16BIT,
    .sample_rate = 11025
};

//device state flags
static bool sb16_present = false;
static bool sb16_runtime_enabled = false;
static bool sb16_irq_registered = false;
static uint32 sb16_opl_chip = OPL_CHIP_NONE;
static bool sb16_stream_running = false;
static bool sb16_reconfigure_pending = false;
static uint32 sb16_pending_halves = 0;
static uint32 sb16_next_refill_half = 0;

//physical DMA buffer allocations
static void *sb16_dma_alloc_base = NULL;
static size sb16_dma_alloc_pages = 0;
static uintptr sb16_dma_phys = 0;
static int16 *sb16_dma_virt = NULL;

//producer/consumer ring for playback
static int16 sb16_queue[SB16_QUEUE_SAMPLES];
static uint32 sb16_queue_read = 0;
static uint32 sb16_queue_write = 0;
static uint32 sb16_queue_count = 0;


static void dsp_write(uint8 value) {
    while (inb(DSP_WRITE) & 0x80) {
        arch_pause();
    }
    outb(DSP_WRITE, value);
}

static uint8 dsp_read(void) {
    while (!(inb(DSP_READ_STAT) & 0x80)) {
        arch_pause();
    }
    return inb(DSP_READ);
}

static void mixer_write(uint8 reg, uint8 value) {
    outb(DSP_MIXER, reg);
    outb(DSP_MIXER_DATA, value);
}

static uint8 mixer_read(uint8 reg) {
    outb(DSP_MIXER, reg);
    return inb(DSP_MIXER_DATA);
}

static uint8 opl_read_port(uint16 port) {
    return inb(port);
}

static uint8 opl_read_status(void) {
    return opl_read_port(OPL_REG_PORT);
}

static void opl_write_port(uint16 port, uint8 value) {
    outb(port, value);
}

//write OPL register with ISA delays (~3.3us after select, ~23us after write)
//bit 8 selects OPL3 bank. init_stage reads status port to avoid clone bugs
static void opl_write_register_locked(uint16 reg, uint8 value, bool init_stage) {
    int i;

    if (reg & 0x100) {
        opl_write_port(OPL_REG_PORT_OPL3, reg & 0xff);
    } else {
        opl_write_port(OPL_REG_PORT, reg & 0xff);
    }

    //~3.3us delay (6 ISA reads ~= 6us)
    for (i = 0; i < 6; ++i) {
        if (init_stage) {
            (void)opl_read_port(OPL_REG_PORT);
        } else {
            (void)opl_read_port(OPL_DATA_PORT);
        }
    }

    opl_write_port((reg & 0x100) ? OPL_DATA_PORT_OPL3 : OPL_DATA_PORT, value);

    //~23us delay
    for (i = 0; i < 24; ++i) {
        (void)opl_read_status();
    }
}

//probe OPL chip via timer test (returns 0xC0 on success)
//distinguish OPL3/OPL2 via register port
static uint32 opl_detect_locked(void) {
    int result1;
    int result2;
    int i;

    opl_write_register_locked(OPL_REG_TIMER_CTRL, 0x60, true);
    opl_write_register_locked(OPL_REG_TIMER_CTRL, 0x80, true);
    result1 = opl_read_status();

    opl_write_register_locked(OPL_REG_TIMER1, 0xff, true);
    opl_write_register_locked(OPL_REG_TIMER_CTRL, 0x21, true);

    for (i = 0; i < 200; ++i) {
        (void)opl_read_status();
    }
    for (volatile int delay = 0; delay < 200000; ++delay) {
        arch_pause();
    }

    result2 = opl_read_status();

    opl_write_register_locked(OPL_REG_TIMER_CTRL, 0x60, true);
    opl_write_register_locked(OPL_REG_TIMER_CTRL, 0x80, true);

    if ((result1 & 0xe0) == 0x00 && (result2 & 0xe0) == 0xc0) {
        if (opl_read_port(OPL_REG_PORT) == 0x00) {
            return OPL_CHIP_OPL3;
        }
        return OPL_CHIP_OPL2;
    }

    return OPL_CHIP_NONE;
}

//silence operators and clear settings (repeats on bank 2 for OPL3)
static void opl_init_registers_locked(bool opl3) {
    uint16 reg;

    for (reg = OPL_REGS_LEVEL; reg <= OPL_REGS_LEVEL + 21; ++reg) {
        opl_write_register_locked(reg, 0x3f, true);
    }

    for (reg = OPL_REGS_ATTACK; reg <= OPL_REGS_WAVEFORM + 21; ++reg) {
        opl_write_register_locked(reg, 0x00, true);
    }

    for (reg = 1; reg < OPL_REGS_LEVEL; ++reg) {
        opl_write_register_locked(reg, 0x00, true);
    }

    opl_write_register_locked(OPL_REG_TIMER_CTRL, 0x60, true);
    opl_write_register_locked(OPL_REG_TIMER_CTRL, 0x80, true);
    opl_write_register_locked(OPL_REG_WAVEFORM_ENABLE, 0x20, true);

    if (opl3) {
        opl_write_register_locked(OPL_REG_NEW, 0x01, true);

        for (reg = OPL_REGS_LEVEL; reg <= OPL_REGS_LEVEL + 21; ++reg) {
            opl_write_register_locked(reg | 0x100, 0x3f, true);
        }

        for (reg = OPL_REGS_ATTACK; reg <= OPL_REGS_WAVEFORM + 21; ++reg) {
            opl_write_register_locked(reg | 0x100, 0x00, true);
        }

        for (reg = 1; reg < OPL_REGS_LEVEL; ++reg) {
            opl_write_register_locked(reg | 0x100, 0x00, true);
        }
    }

    opl_write_register_locked(OPL_REG_FM_MODE, 0x40, true);

    if (opl3) {
        opl_write_register_locked(OPL_REG_NEW, 0x01, true);
    }
}

//reset DSP: pulse high >=3us, drop low, wait for 0xAA response
static bool dsp_reset(void) {
    outb(DSP_RESET, 1);
    for (volatile int i = 0; i < 10000; ++i) {}
    outb(DSP_RESET, 0);

    for (int i = 0; i < 1000; ++i) {
        if ((inb(DSP_READ_STAT) & 0x80) && inb(DSP_READ) == 0xAA) {
            return true;
        }
        for (volatile int j = 0; j < 1000; ++j) {}
    }

    return false;
}

//program 16-bit ISA DMA controller (channels 5-7)
static void dma16_program(uintptr phys_addr, size total_bytes) {
    //16-bit DMA uses word-aligned addresses and counts
    uint32 dma_addr_words = (uint32)(phys_addr >> 1);
    uint16 dma_count_words = (uint16)((total_bytes / 2) - 1);
    uint8 channel = sb16_dma16 & 0x3;

    static const uint16 dma16_addr_port[4]  = { 0x00, 0xC4, 0xC8, 0xCC };
    static const uint16 dma16_count_port[4] = { 0x00, 0xC6, 0xCA, 0xCE };
    static const uint16 dma16_page_port[4]  = { 0x00, 0x8B, 0x89, 0x8A };

    //mask channel and set mode (single-cycle, auto-init, read from memory)
    outb(0xD4, 0x04 | channel);
    outb(0xD6, 0x58 | channel);

    //reset flip-flop so we write low byte then high byte reliably
    outb(0xD8, 0x00);
    outb(dma16_addr_port[channel], dma_addr_words & 0xFF);
    outb(dma16_addr_port[channel], (dma_addr_words >> 8) & 0xFF);

    //page register takes the actual physical high bits, not shifted
    outb(dma16_page_port[channel], (phys_addr >> 16) & 0xFF);

    outb(0xD8, 0x00);
    outb(dma16_count_port[channel], dma_count_words & 0xFF);
    outb(dma16_count_port[channel], (dma_count_words >> 8) & 0xFF);

    //unmask channel
    outb(0xD4, channel);
}

//allocate physical buffer ensuring it doesn't cross a 128KB boundary
static bool sb16_alloc_dma_buffer(void) {
    uintptr base;
    uintptr aligned;
    size bytes;

    if (sb16_dma_virt != NULL) {
        return true;
    }

    bytes = DMA16_BOUNDARY + DMA16_BUFFER_BYTES;
    sb16_dma_alloc_pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    sb16_dma_alloc_base = pmm_alloc_zone(sb16_dma_alloc_pages, 0xFFFFFF);
    if (!sb16_dma_alloc_base) {
        printf("[sb16] failed to allocate DMA window under 16MB\n");
        return false;
    }

    base = (uintptr)sb16_dma_alloc_base;
    aligned = (base + DMA16_BOUNDARY - 1) & ~(uintptr)(DMA16_BOUNDARY - 1);
    if (aligned + DMA16_BUFFER_BYTES > base + (sb16_dma_alloc_pages * PAGE_SIZE)) {
        printf("[sb16] DMA alignment window calculation failed\n");
        pmm_free(sb16_dma_alloc_base, sb16_dma_alloc_pages);
        sb16_dma_alloc_base = NULL;
        sb16_dma_alloc_pages = 0;
        return false;
    }

    sb16_dma_phys = aligned;
    sb16_dma_virt = (int16 *)P2V((void *)aligned);
    memset(sb16_dma_virt, 0, DMA16_BUFFER_BYTES);
    return true;
}

//start playback: lazy alloc, kick auto-init if idle and we have data
static bool sb16_start_stream(void) {
    irq_state_t flags;
    bool started = false;

    if (!sb16_alloc_dma_buffer()) {
        return false;
    }

    flags = spinlock_irq_acquire(&sb16_lock);
    if (!sb16_reconfigure_pending && !sb16_stream_running && sb16_queue_count > 0) {
        started = sb16_start_stream_locked();
    }
    spinlock_irq_release(&sb16_lock, flags);
    return started;
}

//stop current auto-init transfer and reset bookkeeping
//must be called with sb16_lock held
static void sb16_stop_stream_locked(void) {
    if (!sb16_stream_running) {
        return;
    }

    dsp_write(DSP_DMA16_PAUSE);
    dsp_write(DSP_DMA16_EXIT);
    sb16_stream_running = false;
    sb16_pending_halves = 0;
    sb16_next_refill_half = 0;
}

//copy samples from ring to DMA buffer half (pads with silence if empty)
//must be called with sb16_lock held
static void sb16_fill_half_locked(uint32 half_index) {
    int16 *dst;
    uint32 copied;

    dst = sb16_dma_virt + (half_index * DMA16_HALF_SAMPLES);
    copied = 0;

    while (copied < DMA16_HALF_SAMPLES && sb16_queue_count > 0) {
        dst[copied++] = sb16_queue[sb16_queue_read];
        sb16_queue_read = (sb16_queue_read + 1) % SB16_QUEUE_SAMPLES;
        --sb16_queue_count;
    }

    while (copied < DMA16_HALF_SAMPLES) {
        dst[copied++] = 0;
    }
}

//program DSP for auto-init 16-bit signed mono playback
//must be called with sb16_lock held
static bool sb16_start_stream_locked(void) {
    uint16 block_words;

    if (!sb16_alloc_dma_buffer()) {
        return false;
    }

    //pre-fill both halves so we get clean audio from the very first IRQ
    memset(sb16_dma_virt, 0, DMA16_BUFFER_BYTES);
    sb16_fill_half_locked(0);
    sb16_fill_half_locked(1);

    //the DSP expects block size in words minus 1
    //since we want an IRQ at each half boundary, the block size IS the half size
    block_words = DMA16_HALF_SAMPLES - 1;

    //program the sample rate (big-endian: high byte first)
    dsp_write(DSP_SET_OUTPUT_RATE);
    dsp_write((current_format.sample_rate >> 8) & 0xFF);
    dsp_write(current_format.sample_rate & 0xFF);

    //point the ISA DMA controller at our buffer
    dma16_program(sb16_dma_phys, DMA16_BUFFER_BYTES);

    //tell the DSP to begin auto-init 16-bit signed mono playback
    dsp_write(DSP_DMA16_AUTOINIT);
    dsp_write(DSP_MODE_SIGNED_MONO);
    dsp_write(block_words & 0xFF);
    dsp_write((block_words >> 8) & 0xFF);

    sb16_stream_running = true;
    sb16_pending_halves = 0;
    sb16_next_refill_half = 0;
    return true;
}

//enqueue u8 PCM and convert to s16 (drops oldest samples on overflow)
//must be called with sb16_lock held
static void sb16_queue_write_u8_locked(const uint8 *src, uint32 count) {
    //if a single write is larger than the whole queue, keep just the tail
    if (count >= SB16_QUEUE_SAMPLES) {
        src += count - SB16_QUEUE_SAMPLES;
        count = SB16_QUEUE_SAMPLES;
        sb16_queue_read = 0;
        sb16_queue_write = 0;
        sb16_queue_count = 0;
    }

    //evict oldest samples until the new batch fits
    while (sb16_queue_count + count > SB16_QUEUE_SAMPLES) {
        sb16_queue_read = (sb16_queue_read + 1) % SB16_QUEUE_SAMPLES;
        --sb16_queue_count;
    }

    //u8 -> s16: subtract DC bias (128) and shift up to fill the 16-bit range
    for (uint32 i = 0; i < count; ++i) {
        sb16_queue[sb16_queue_write] = ((int16)src[i] - 128) << 8;
        sb16_queue_write = (sb16_queue_write + 1) % SB16_QUEUE_SAMPLES;
    }
    sb16_queue_count += count;
}

//enqueue s16 little-endian PCM (drops oldest samples on overflow)
//must be called with sb16_lock held
static void sb16_queue_write_s16_locked(const uint8 *src, uint32 bytes) {
    uint32 count = bytes / 2;

    //if a single write is larger than the whole queue, keep just the tail
    if (count >= SB16_QUEUE_SAMPLES) {
        src += (count - SB16_QUEUE_SAMPLES) * 2U;
        count = SB16_QUEUE_SAMPLES;
        bytes = count * 2U;
        sb16_queue_read = 0;
        sb16_queue_write = 0;
        sb16_queue_count = 0;
    }

    //evict oldest samples until the new batch fits
    while (sb16_queue_count + count > SB16_QUEUE_SAMPLES) {
        sb16_queue_read = (sb16_queue_read + 1) % SB16_QUEUE_SAMPLES;
        --sb16_queue_count;
    }

    //assemble little-endian s16 sample from byte stream
    for (uint32 i = 0; i < bytes; i += 2) {
        int16 sample = (int16)((uint16)src[i] | ((uint16)src[i + 1] << 8));
        sb16_queue[sb16_queue_write] = sample;
        sb16_queue_write = (sb16_queue_write + 1) % SB16_QUEUE_SAMPLES;
    }
    sb16_queue_count += count;
}

//deferred work to refill pending DMA halves
//runs from bottom-half context so memcpy/locking is safe
static void sb16_bottom_half(void *arg) {
    irq_state_t flags;

    (void)arg;

    flags = spinlock_irq_acquire(&sb16_lock);

    while (sb16_pending_halves > 0 && sb16_dma_virt != NULL) {
        sb16_fill_half_locked(sb16_next_refill_half);
        sb16_next_refill_half ^= 1U;
        --sb16_pending_halves;
    }

    spinlock_irq_release(&sb16_lock, flags);
}

//hardware ISR: ack device, bump pending count, schedule bottom half
static void sb16_irq(void) {
    bool scheduled = false;
    irq_state_t flags;

    //acknowledge the 16-bit playback interrupt source used by our auto-init stream
    (void)inb(DSP_INT_ACK_16);

    flags = spinlock_irq_acquire(&sb16_lock);
    
    //cap pending_halves at 2 (the buffer only has two halves)
    if (sb16_stream_running && sb16_pending_halves < 2) {
        ++sb16_pending_halves;
        scheduled = true;
    }
    spinlock_irq_release(&sb16_lock, flags);

    //queue refill work to happen outside of interrupt context
    if (scheduled && sb16_bh != BOTTOM_HALF_INVALID_HANDLE) {
        bottom_half_raise(sb16_bh);
    }
}

//$devices/dsp write callback (enqueues PCM and kicks auto-init DMA)
static ssize sb16_write(object_t *obj, const void *buf, size len, size offset) {
    (void)obj;
    (void)offset;
    const uint8 *src = (const uint8 *)buf;
    uint32 sample_bytes;
    audio_format_t format;
    bool need_start = false;
    irq_state_t flags;

    //fail fast if device is missing or buffer is empty
    if (!sb16_present || !buf || len == 0) {
        return sb16_present ? 0 : -1;
    }

    //refuse writes until the bottom-half scheduler is online
    if (!sb16_runtime_enabled) {
        return -1;
    }

    flags = spinlock_irq_acquire(&sb16_lock);

    //format changed via ioctl, tear down the old stream and drop stale samples
    if (sb16_reconfigure_pending) {
        sb16_stop_stream_locked();
        sb16_queue_read = 0;
        sb16_queue_write = 0;
        sb16_queue_count = 0;
        if (sb16_dma_virt != NULL) {
            memset(sb16_dma_virt, 0, DMA16_BUFFER_BYTES);
        }
        sb16_reconfigure_pending = false;
    }

    format = current_format;

    //we only support mono right now, reject stereo to avoid garbled audio
    if (format.channels != 1) {
        spinlock_irq_release(&sb16_lock, flags);
        return -1;
    }

    //truncate to a whole number of samples to prevent alignment issues
    sample_bytes = format.format == AUDIO_FORMAT_PCM_16BIT ? 2U : 1U;
    len -= len % sample_bytes;
    if (len == 0) {
        spinlock_irq_release(&sb16_lock, flags);
        return 0;
    }

    //enqueue and convert to the DSP's native 16-bit format
    if (format.format == AUDIO_FORMAT_PCM_8BIT) {
        sb16_queue_write_u8_locked(src, (uint32)len);
    } else if (format.format == AUDIO_FORMAT_PCM_16BIT) {
        sb16_queue_write_s16_locked(src, (uint32)len);
    } else {
        spinlock_irq_release(&sb16_lock, flags);
        return -1;
    }

    //if the stream was idle, we need to kickstart the DMA engine
    need_start = len > 0 && !sb16_stream_running;
    spinlock_irq_release(&sb16_lock, flags);

    //start playback or nudge the bottom half to refill the active buffer
    if (need_start) {
        if (!sb16_start_stream()) {
            printf("[sb16] failed to start auto-init stream\n");
        }
    } else if (len > 0) {
        if (sb16_bh != BOTTOM_HALF_INVALID_HANDLE) {
            bottom_half_raise(sb16_bh);
        }
    }

    return (ssize)len;
}

//$devices/dsp get_info callback (gets/sets audio format)
static intptr sb16_get_info(object_t *obj, uint32 topic, void *buf, size len) {
    (void)obj;

    switch (topic) {
        case AUDIO_IOCTL_SET_FORMAT: {
            audio_format_t *fmt = (audio_format_t *)buf;
            irq_state_t flags;

            if (len < sizeof(audio_format_t) || !fmt) return -1;
            if (fmt->channels != 1) return -1;
            if (fmt->format != AUDIO_FORMAT_PCM_8BIT
             && fmt->format != AUDIO_FORMAT_PCM_16BIT) return -1;
            if (fmt->sample_rate < SB16_RATE_MIN || fmt->sample_rate > SB16_RATE_MAX) return -1;

            flags = spinlock_irq_acquire(&sb16_lock);
            current_format = *fmt;
            sb16_reconfigure_pending = true;
            spinlock_irq_release(&sb16_lock, flags);
            return 0;
        }

        case AUDIO_IOCTL_GET_FORMAT: {
            audio_format_t *fmt = (audio_format_t *)buf;
            irq_state_t flags;

            if (len < sizeof(audio_format_t) || !fmt) return -1;

            flags = spinlock_irq_acquire(&sb16_lock);
            *fmt = current_format;
            spinlock_irq_release(&sb16_lock, flags);
            return sizeof(audio_format_t);
        }

        default:
            return -1;
    }
}

//$devices/opl write callback (applies array of reg/value pairs)
static ssize opl_write(object_t *obj, const void *buf, size len, size offset) {
    const opl_write_t *writes;
    irq_state_t flags;
    size count;
    size i;

    (void)obj;
    (void)offset;

    if (sb16_opl_chip == OPL_CHIP_NONE || buf == NULL) {
        return sb16_opl_chip == OPL_CHIP_NONE ? -1 : 0;
    }

    count = len / sizeof(opl_write_t);
    if (count == 0) {
        return 0;
    }

    writes = (const opl_write_t *)buf;

    flags = spinlock_irq_acquire(&sb16_lock);
    for (i = 0; i < count; ++i) {
        opl_write_register_locked(writes[i].reg, writes[i].value, false);
    }
    spinlock_irq_release(&sb16_lock, flags);

    return (ssize)(count * sizeof(opl_write_t));
}

//$devices/opl get_info callback (GET_INFO, INIT, RESET)
static intptr opl_get_info(object_t *obj, uint32 topic, void *buf, size len) {
    irq_state_t flags;

    (void)obj;

    switch (topic) {
        case OPL_IOCTL_GET_INFO: {
            opl_info_t *info = (opl_info_t *)buf;

            if (buf == NULL || len < sizeof(*info)) {
                return -1;
            }

            info->chip_type = sb16_opl_chip;
            info->num_voices = sb16_opl_chip == OPL_CHIP_OPL3 ? 18 : 9;
            return sizeof(*info);
        }

        case OPL_IOCTL_INIT: {
            opl_init_t *init = (opl_init_t *)buf;

            if (buf == NULL || len < sizeof(*init) || sb16_opl_chip == OPL_CHIP_NONE) {
                return -1;
            }

            flags = spinlock_irq_acquire(&sb16_lock);
            opl_init_registers_locked(init->opl3_mode != 0 && sb16_opl_chip == OPL_CHIP_OPL3);
            spinlock_irq_release(&sb16_lock, flags);
            return 0;
        }

        case OPL_IOCTL_RESET:
            if (sb16_opl_chip == OPL_CHIP_NONE) {
                return -1;
            }

            flags = spinlock_irq_acquire(&sb16_lock);
            opl_init_registers_locked(sb16_opl_chip == OPL_CHIP_OPL3);
            spinlock_irq_release(&sb16_lock, flags);
            return 0;

        default:
            return -1;
    }
}

static object_ops_t sb16_ops = {
    .read = NULL,
    .write = sb16_write,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL,
    .stat = NULL,
    .get_info = sb16_get_info
};

static object_ops_t opl_ops = {
    .read = NULL,
    .write = opl_write,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL,
    .stat = NULL,
    .get_info = opl_get_info
};

//probe standard SB16 ports for a DSP reset response
static bool sb16_probe_base(void) {
    for (size i = 0; i < sizeof(SB16_BASE_PORTS) / sizeof(SB16_BASE_PORTS[0]); ++i) {
        sb16_base = SB16_BASE_PORTS[i];
        if (dsp_reset()) {
            return true;
        }
    }
    sb16_base = 0;
    return false;
}

//decode mixer IRQ select register
static uint8 decode_irq_select(uint8 value) {
    if (value & 0x01) return 2;
    if (value & 0x02) return 5;
    if (value & 0x04) return 7;
    if (value & 0x08) return 10;
    return 0;
}

//decode mixer DMA select register
static void decode_dma_select(uint8 value, uint8 *dma8_out, uint8 *dma16_out) {
    *dma8_out = 0;
    *dma16_out = 0;

    if (value & 0x01) *dma8_out = 0;
    else if (value & 0x02) *dma8_out = 1;
    else if (value & 0x08) *dma8_out = 3;

    if (value & 0x20) *dma16_out = 5;
    else if (value & 0x40) *dma16_out = 6;
    else if (value & 0x80) *dma16_out = 7;
}

//early driver init
void sb16_init(void) {
    object_t *dsp_obj;
    object_t *opl_obj;
    irq_state_t flags;

    //probe the standard base ports to find the DSP
    if (!sb16_probe_base()) {
        printf("[sb16] no Sound Blaster found at any standard port\n");
        return;
    }

    printf("[sb16] DSP responded at base 0x%x\n", sb16_base);

    //check if the DSP is modern enough for 16-bit auto-init
    dsp_write(0xE1);
    uint8 major = dsp_read();
    uint8 minor = dsp_read();
    printf("[sb16] Found Sound Blaster DSP v%u.%u\n", major, minor);

    if (major < 4) {
        printf("[sb16] DSP version too old for 16-bit auto-init playback\n");
        return;
    }

    //read whatever the firmware/jumpers configured
    uint8 irq_reg = mixer_read(MIXER_IRQ_SELECT);
    uint8 dma_reg = mixer_read(MIXER_DMA_SELECT);

    sb16_irq_num = decode_irq_select(irq_reg);
    decode_dma_select(dma_reg, &sb16_dma8, &sb16_dma16);

    //fallback to defaults if the mixer reports garbage (common in clones)
    if (sb16_irq_num == 0) {
        printf("[sb16] mixer IRQ register invalid (0x%02x), falling back to IRQ %u\n",
               irq_reg, SB16_DEFAULT_IRQ);
        sb16_irq_num = SB16_DEFAULT_IRQ;
        
        //write back the fallback so the card actually routes there
        mixer_write(MIXER_IRQ_SELECT, 0x02);
    }

    if (sb16_dma16 == 0) {
        printf("[sb16] mixer DMA register invalid (0x%02x), falling back to 16-bit DMA %u / 8-bit DMA %u\n",
               dma_reg, SB16_DEFAULT_DMA16, SB16_DEFAULT_DMA8);
        sb16_dma16 = SB16_DEFAULT_DMA16;
        sb16_dma8  = SB16_DEFAULT_DMA8;
        mixer_write(MIXER_DMA_SELECT, 0x22);
    }

    //set all mixer volumes to maximum
    mixer_write(0x30, 0xFF);
    mixer_write(0x31, 0xFF);
    mixer_write(0x32, 0xFF);
    mixer_write(0x33, 0xFF);
    mixer_write(0x34, 0xFF);
    mixer_write(0x35, 0xFF);

    printf("[sb16] Using IRQ %u, 16-bit DMA %u, 8-bit DMA %u (mixer irq=0x%02x dma=0x%02x)\n",
           sb16_irq_num, sb16_dma16, sb16_dma8,
           mixer_read(MIXER_IRQ_SELECT), mixer_read(MIXER_DMA_SELECT));

    //probe for FM synth chip (OPL2/OPL3)
    flags = spinlock_irq_acquire(&sb16_lock);
    sb16_opl_chip = opl_detect_locked();
    spinlock_irq_release(&sb16_lock, flags);
    
    if (sb16_opl_chip == OPL_CHIP_OPL3) {
        printf("[sb16] OPL3 detected and available via $devices/opl\n");
    } else if (sb16_opl_chip == OPL_CHIP_OPL2) {
        printf("[sb16] OPL2 detected and available via $devices/opl\n");
    } else {
        printf("[sb16] no OPL device detected\n");
    }

    //turn on the speaker output
    dsp_write(DSP_SPEAKER_ON);

    //register the interrupt but leave it masked until the scheduler is up
    if (interrupt_register(sb16_irq_num, sb16_irq) == 0) {
        sb16_irq_registered = true;
    } else {
        printf("[sb16] failed to register IRQ %u handler\n", sb16_irq_num);
        return;
    }

    interrupt_mask(sb16_irq_num);

    dsp_obj = object_create(OBJECT_DEVICE, &sb16_ops, NULL);
    if (!dsp_obj) {
        printf("[sb16] Failed to create DSP object\n");
        return;
    }

    ns_register("$devices/dsp", dsp_obj);
    object_deref(dsp_obj);

    if (sb16_opl_chip != OPL_CHIP_NONE) {
        opl_obj = object_create(OBJECT_DEVICE, &opl_ops, NULL);
        if (!opl_obj) {
            printf("[sb16] Failed to create OPL object\n");
        } else {
            ns_register("$devices/opl", opl_obj);
            object_deref(opl_obj);
        }
    }

    sb16_present = true;
    printf("[sb16] Registered $devices/dsp, IRQ-driven playback start deferred until scheduler is running\n");
}

//deferred runtime startup once scheduler/bottom-halves are running
void sb16_start(void) {
    irq_state_t flags;

    flags = spinlock_irq_acquire(&sb16_lock);
    if (!sb16_present || sb16_runtime_enabled) {
        spinlock_irq_release(&sb16_lock, flags);
        return;
    }
    sb16_runtime_enabled = true;
    spinlock_irq_release(&sb16_lock, flags);

    if (sb16_bh == BOTTOM_HALF_INVALID_HANDLE) {
        sb16_bh = bottom_half_register(sb16_bottom_half, NULL);
    }
    if (sb16_irq_registered) {
        interrupt_unmask(sb16_irq_num);
    }

    printf("[sb16] bottom-half playback enabled\n");
}

DECLARE_DRIVER(sb16_init, INIT_LEVEL_DEVICE);
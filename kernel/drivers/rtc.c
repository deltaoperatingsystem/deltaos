#include <drivers/rtc.h>
#include <arch/amd64/io.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <lib/io.h>
#include <lib/spinlock.h>
#include <drivers/init.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static spinlock_irq_t rtc_lock = {0};

#define RTC_REG_SECOND 0x00
#define RTC_REG_MINUTE 0x02
#define RTC_REG_HOUR   0x04
#define RTC_REG_DAY    0x07
#define RTC_REG_MONTH  0x08
#define RTC_REG_YEAR   0x09
#define RTC_REG_STAT_A 0x0A
#define RTC_REG_STAT_B 0x0B

static uint8 rtc_read_reg(uint8 reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int rtc_is_updating() {
    return rtc_read_reg(RTC_REG_STAT_A) & 0x80;
}

static uint8 bcd2bin(uint8 bcd) {
    return ((bcd / 16) * 10) + (bcd & 0x0F);
}

void rtc_get_time(rtc_time_t *time) {
    spinlock_irq_acquire(&rtc_lock);
    
    while (rtc_is_updating());

    time->second = rtc_read_reg(RTC_REG_SECOND);
    time->minute = rtc_read_reg(RTC_REG_MINUTE);
    time->hour = rtc_read_reg(RTC_REG_HOUR);
    time->day = rtc_read_reg(RTC_REG_DAY);
    time->month = rtc_read_reg(RTC_REG_MONTH);
    time->year = rtc_read_reg(RTC_REG_YEAR);

    uint8 registerB = rtc_read_reg(RTC_REG_STAT_B);

    //convert BCD to binary if necessary
    if (!(registerB & 0x04)) {
        time->second = bcd2bin(time->second);
        time->minute = bcd2bin(time->minute);
        time->hour = ((time->hour & 0x0F) + (((time->hour & 0x70) / 16) * 10)) | (time->hour & 0x80);
        time->day = bcd2bin(time->day);
        time->month = bcd2bin(time->month);
        time->year = bcd2bin(time->year);
    }

    //convert 12h to 24h if necessary
    if (!(registerB & 0x02) && (time->hour & 0x80)) {
        time->hour = ((time->hour & 0x7F) + 12) % 24;
    }

    time->year += 2000;
    
    spinlock_irq_release(&rtc_lock);
}

//object ops for RTC - read returns time as binary struct
static ssize rtc_obj_read(object_t *obj, void *buf, size len, size offset) {
    (void)obj;
    (void)offset;
    if (len < sizeof(rtc_time_t)) return -1;
    
    rtc_get_time((rtc_time_t *)buf);
    return sizeof(rtc_time_t);
}

static object_ops_t rtc_object_ops = {
    .read = rtc_obj_read,
    .write = NULL,
    .close = NULL
};

static object_t *rtc_object = NULL;

void rtc_init() {
    rtc_object = object_create(OBJECT_DEVICE, &rtc_object_ops, NULL);
    if (rtc_object) {
        ns_register("$devices/rtc", rtc_object);
    }
    puts("[rtc] initialized\n");
}

DECLARE_DRIVER(rtc_init, INIT_LEVEL_DEVICE);

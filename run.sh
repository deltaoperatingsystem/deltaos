#!/usr/bin/env bash
set -e

#config
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
DISK_IMG="hda.img"
DISK_SIZE_MB=64
NVME_IMG="nvme.img"
NVME_SIZE_MB=128
FAT32_IMG="$ROOT_DIR/fat32.img"
EFI_BINARY="$ROOT_DIR/bootloader/BOOTX64.EFI"
OVMF_CODE=
QEMU_NET_MODE="${QEMU_NET_MODE:-passt}"
QEMU_HOSTFWD="${QEMU_HOSTFWD:-tcp::8080-:80}"
QEMU_BRIDGE="${QEMU_BRIDGE:-}"
QEMU_TAP_IFACE="${QEMU_TAP_IFACE:-tap0}"
QEMU_KERNEL_IRQCHIP="${QEMU_KERNEL_IRQCHIP:-}"

print_step() {
    echo -e "==> $1"
}

find_qemu_bridge_helper() {
    local candidates=(
        "/usr/lib/qemu/qemu-bridge-helper"
        "/usr/libexec/qemu-bridge-helper"
    )

    for helper in "${candidates[@]}"; do
        [[ -x "$helper" ]] && { echo "$helper"; return 0; }
    done
    return 1
}

detect_bridge_name() {
    if [[ -n "$QEMU_BRIDGE" && -d "/sys/class/net/$QEMU_BRIDGE" ]]; then
        echo "$QEMU_BRIDGE"
        return 0
    fi

    local candidates=(virbr0 br0)
    for bridge in "${candidates[@]}"; do
        [[ -d "/sys/class/net/$bridge" ]] && { echo "$bridge"; return 0; }
    done
    return 1
}

qemu_supports_passt_netdev() {
    command -v passt >/dev/null 2>&1 && qemu-system-x86_64 -netdev help 2>/dev/null | grep -q '^passt$'
}

append_rtl8139_netdev() {
    local -n qemu_args_out=$1
    local netdev_id=$2
    local netdev_spec=$3

    qemu_args_out+=(
        -netdev "$netdev_spec"
        -device "rtl8139,netdev=$netdev_id"
    )
}

append_network_args() {
    local -n qemu_args_ref=$1
    local helper bridge

    case "$QEMU_NET_MODE" in
        (tap)
            print_step "using tap networking on $QEMU_TAP_IFACE"
            qemu_args_ref+=(
                -netdev "tap,id=net0,ifname=$QEMU_TAP_IFACE,script=no,downscript=no"
                -device rtl8139,netdev=net0
            )
            echo "make sure the tap device is up and bridged/routed on the host"
            return 0
            ;;
        (bridge)
            helper=$(find_qemu_bridge_helper || true)
            bridge=$(detect_bridge_name || true)
            if [[ -n "$helper" && -n "$bridge" ]]; then
                print_step "using bridged networking on $bridge"
                qemu_args_ref+=(
                    -netdev "bridge,id=net0,br=$bridge,helper=$helper"
                    -device rtl8139,netdev=net0
                )
                echo "host can reach guest directly on bridge $bridge (including IPv6 link-local)"
                return 0
            fi
            {
                echo "error: bridge networking requested but no usable bridge/helper was found."
                echo "set QEMU_BRIDGE=<bridge> or install/configure qemu-bridge-helper."
                exit 1
            }
            ;;
    esac

    if [[ -n "$QEMU_HOSTFWD" && "$QEMU_NET_MODE" != "tap" && "$QEMU_NET_MODE" != "bridge" ]]; then
        if [[ "$QEMU_NET_MODE" == "passt" || "$QEMU_NET_MODE" == "auto" ]] && qemu_supports_passt_netdev; then
            print_step "using passt primary NIC plus user-mode forwarding NIC"
            append_rtl8139_netdev qemu_args_ref net0 passt,id=net0,ipv6=on
            append_rtl8139_netdev qemu_args_ref net1 "user,id=net1,hostfwd=$QEMU_HOSTFWD,ipv6=on"
            echo "primary NIC: passt"
            echo "forwarding host port(s) via $QEMU_HOSTFWD on secondary NIC"
            return 0
        fi

        print_step "using QEMU user networking for host forwarding"
        append_rtl8139_netdev qemu_args_ref net0 "user,id=net0,hostfwd=$QEMU_HOSTFWD,ipv6=on"
        echo "forwarding host port(s) via $QEMU_HOSTFWD"
        return 0
    fi

    if [[ "$QEMU_NET_MODE" == "passt" || "$QEMU_NET_MODE" == "user" || "$QEMU_NET_MODE" == "auto" ]]; then
        if qemu_supports_passt_netdev; then
            print_step "using QEMU passt networking"
            append_rtl8139_netdev qemu_args_ref net0 passt,id=net0,ipv6=on
            return 0
        fi

        print_step "using QEMU user networking (slirp fallback)"
        append_rtl8139_netdev qemu_args_ref net0 user,id=net0,ipv6=on
        echo "passt is unavailable, falling back to slirp"
        return 0
    fi

    print_step "using QEMU user networking (slirp fallback)"
    append_rtl8139_netdev qemu_args_ref net0 user,id=net0,ipv6=on
    echo "    classic user-mode networking"
}

prepare_boot_config() {
    echo "$ROOT_DIR/bootloader/boot/delboot.cfg"
}

configure_tap_ipv6() {
    local out_if
    out_if=$(ip -6 route show default | awk 'NR==1 {print $5; exit}')
    if [[ -z "$out_if" ]]; then
        echo "error: could not determine default IPv6 uplink interface"
        exit 1
    fi

    print_step "configuring TAP IPv6 NAT66 via $out_if"
    if ! ip link show dev "$QEMU_TAP_IFACE" >/dev/null 2>&1; then
        sudo ip tuntap add dev "$QEMU_TAP_IFACE" mode tap user "$USER"
    fi
    sudo ip link set "$QEMU_TAP_IFACE" up
    sudo ip addr replace 192.168.76.1/24 dev "$QEMU_TAP_IFACE"
    sudo ip -6 addr replace fd42:76:76::1/64 dev "$QEMU_TAP_IFACE"
    sudo sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null
    sudo sysctl -w net.ipv6.conf.default.forwarding=1 >/dev/null
    sudo sysctl -w "net.ipv6.conf.$QEMU_TAP_IFACE.forwarding=1" >/dev/null || true
    sudo ip6tables -t nat -C POSTROUTING -s fd42:76:76::/64 -o "$out_if" -j MASQUERADE 2>/dev/null \
        || sudo ip6tables -t nat -A POSTROUTING -s fd42:76:76::/64 -o "$out_if" -j MASQUERADE
    sudo ip6tables -C FORWARD -i "$QEMU_TAP_IFACE" -o "$out_if" -j ACCEPT 2>/dev/null \
        || sudo ip6tables -A FORWARD -i "$QEMU_TAP_IFACE" -o "$out_if" -j ACCEPT
    sudo ip6tables -C FORWARD -i "$out_if" -o "$QEMU_TAP_IFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null \
        || sudo ip6tables -A FORWARD -i "$out_if" -o "$QEMU_TAP_IFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT
}

cleanup_tap_ipv6() {
    print_step "cleaning up TAP IPv6 NAT66"
    local out_if
    out_if=$(ip -6 route show default | awk 'NR==1 {print $5; exit}')
    
    sudo ip6tables -t nat -D POSTROUTING -s fd42:76:76::/64 -o "$out_if" -j MASQUERADE 2>/dev/null || true
    sudo ip6tables -D FORWARD -i "$QEMU_TAP_IFACE" -o "$out_if" -j ACCEPT 2>/dev/null || true
    sudo ip6tables -D FORWARD -i "$out_if" -o "$QEMU_TAP_IFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
    
    sudo ip addr del 192.168.76.1/24 dev "$QEMU_TAP_IFACE" 2>/dev/null || true
    sudo ip -6 addr del fd42:76:76::1/64 dev "$QEMU_TAP_IFACE" 2>/dev/null || true
    sudo ip link set "$QEMU_TAP_IFACE" down 2>/dev/null || true
    sudo ip tuntap del dev "$QEMU_TAP_IFACE" mode tap 2>/dev/null || true
}

#create GPT disk image and ESP
create_disk_image() {
    print_step "creating disk image"
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=$DISK_SIZE_MB status=none

    #create sample NVmE image if it doesn't exist
    if [[ ! -f "$NVME_IMG" ]]; then
        print_step "creating sample NVMe image"
        dd if=/dev/zero of="$NVME_IMG" bs=1M count=$NVME_SIZE_MB status=none
        sgdisk --clear --new=1:2048:65535 --change-name=1:"TestPart1" --new=2:65536:0 --change-name=2:"TestPart2" "$NVME_IMG"
    fi

    #partition table creation
    sgdisk --clear --new=1:2048:0 --typecode=1:EF00 --change-name=1:"EFI System" "$DISK_IMG"


    #calculate offset for mtools
    local PART_OFFSET=$((2048 * 512))

    #format partition as fat32
    mformat -F -i "$DISK_IMG"@@${PART_OFFSET} ::

    #setup directory structure and copy binaries
    mmd -i "$DISK_IMG"@@${PART_OFFSET} ::/EFI
    mmd -i "$DISK_IMG"@@${PART_OFFSET} ::/EFI/BOOT

    if [[ -f "$EFI_BINARY" ]]; then
        mcopy -i "$DISK_IMG"@@${PART_OFFSET} "$EFI_BINARY" ::/EFI/BOOT/BOOTX64.EFI
    else
        echo "error: $EFI_BINARY missing."
        exit 1
    fi

    local boot_cfg
    boot_cfg=$(prepare_boot_config)
    [[ -f "$boot_cfg" ]] && mcopy -i "$DISK_IMG"@@${PART_OFFSET} "$boot_cfg" ::/EFI/BOOT/delboot.cfg
    [[ -f "$ROOT_DIR/kernel/delta.elf" ]] && mcopy -i "$DISK_IMG"@@${PART_OFFSET} "$ROOT_DIR/kernel/delta.elf" ::/EFI/BOOT/kernel.bin
    [[ -f "$ROOT_DIR/initrd.da" ]] && mcopy -i "$DISK_IMG"@@${PART_OFFSET} "$ROOT_DIR/initrd.da" ::/EFI/BOOT/initrd.da
}

#execute qemu with ovmf
run_qemu() {
    print_step "launching qemu"
    local machine_spec="q35"
    if [[ "$QEMU_KERNEL_IRQCHIP" == "off" ]]; then
        machine_spec="q35,kernel_irqchip=off"
        echo "    kernel IRQ chip disabled"
    fi
    local accel_spec="-enable-kvm"
    if [[ "$QEMU_KERNEL_IRQCHIP" == "off" ]]; then
        accel_spec="-accel tcg"
        echo "    using TCG because KVM does not support userspace APIC here"
    fi
    local QEMU_ARGS=(
        -machine "$machine_spec"
        -cpu qemu64
        -m 256M
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        -drive "file=$DISK_IMG,format=raw"
        -drive "file=$NVME_IMG,format=raw,if=none,id=nvm"
        -device nvme,serial=deadbeef,drive=nvm
        -drive "file=$FAT32_IMG,format=raw,if=none,id=fatdisk"
        -device nvme,serial=feedbeef,drive=fatdisk
        -chardev stdio,id=char0,logfile=../serial.log,signal=off -serial chardev:char0
        -no-reboot
        -no-shutdown
    )
    QEMU_ARGS+=("$accel_spec")

    if [[ "$QEMU_NET_MODE" == "tap" ]]; then
        configure_tap_ipv6
        trap cleanup_tap_ipv6 EXIT
    fi

    append_network_args QEMU_ARGS

    #handle writable variables if available
    if [[ -n "$OVMF_VARS" ]]; then
        [[ ! -f "$ROOT_DIR/ovmf_vars.fd" ]] && cp "$OVMF_VARS" "$ROOT_DIR/ovmf_vars.fd"
        QEMU_ARGS+=(-drive "if=pflash,format=raw,file=$ROOT_DIR/ovmf_vars.fd")
    fi

    GDK_BACKEND=x11 qemu-system-x86_64 "${QEMU_ARGS[@]}"
}

find_ovmf() {
    local ovmf_dirs=(
        "/usr/share/ovmf/x64"
        "/usr/share/edk2/x64"
        "/usr/share/edk2-ovmf/x64"
        "/usr/share/edk2"
        "/usr/share/ovmf"
        "/usr/share/edk2-ovmf"
    )

    for dir in "${ovmf_dirs[@]}"; do
        local found
        found=$(find "$dir" -maxdepth 2 -type f -name "OVMF_CODE.4m.fd" 2>/dev/null)
        if [[ -n "$found" ]]; then
            OVMF_CODE="$found"
            return
        fi
    done
}


main() {
    #dependency check
    for cmd in qemu-system-x86_64 sgdisk mformat mmd mcopy; do
        if ! command -v "$cmd" &>/dev/null  ; then
            echo "error: $cmd is not installed."
            exit 1
        fi
    done

    #compile if source is newer than binary
    make -C "$ROOT_DIR" > /dev/null

    find_ovmf
    create_disk_image
    run_qemu
}

main "$@"

#!/bin/sh

WXR_USB_ARCHIVE_BOARD='buffalo_wxr-5950ax12_usb'
WXR_USB_ARCHIVE_DIR="sysupgrade-$WXR_USB_ARCHIVE_BOARD"
WXR_USB_NAND_ARCHIVE_DIR='sysupgrade-buffalo_wxr-5950ax12'
WXR_USB_WORK_DIR='/tmp/wxr-usb-sysupgrade'
WXR_USB_FIT="$WXR_USB_WORK_DIR/production.itb"
WXR_USB_ROOTFS="$WXR_USB_WORK_DIR/root.squashfs"
WXR_USB_CONTRACT="$WXR_USB_WORK_DIR/contract.dtb"
WXR_USB_OVERLAY_MOUNT="$WXR_USB_WORK_DIR/overlay"

WXR_USB_RECOVERY_START=2048
WXR_USB_RECOVERY_SECTORS=262144
WXR_USB_PRODUCTION_START=264192
WXR_USB_PRODUCTION_SECTORS=262144
WXR_USB_ROOTFS_START=526336
WXR_USB_ROOTFS_SECTORS=524288
WXR_USB_DATA_START=1050624
WXR_USB_DATA_MIN_SECTORS=902466
WXR_USB_PRODUCTION_BYTES=134217728
WXR_USB_ROOTFS_BYTES_MAX=268435456

WXR_USB_RECOVERY_DEV=''
WXR_USB_PRODUCTION_DEV=''
WXR_USB_ROOTFS_DEV=''
WXR_USB_DATA_DEV=''
WXR_USB_ROOTFS_BYTES=''
WXR_USB_ROOTFS_SHA256=''

wxr_usb_running_medium() {
	local argument
	local cmdline
	local match_count=0
	local partname
	local partuuid
	local root_count=0
	local root_partuuid=''
	local uevent

	read -r cmdline < /proc/cmdline

	for argument in $cmdline; do
		case "$argument" in
		root=PARTUUID=*)
			root_partuuid=${argument#root=PARTUUID=}
			root_count=$((root_count + 1))
			;;
		esac
	done

	if [ "$root_count" -eq 0 ]; then
		echo nand
		return 0
	fi

	if [ "$root_count" -ne 1 ]; then
		echo "WXR USB sysupgrade: kernel command line has multiple root PARTUUID values" >&2
		return 1
	fi

	root_partuuid=$(printf '%s' "$root_partuuid" | /bin/busybox tr '[:upper:]' '[:lower:]')

	for uevent in /sys/class/block/*/uevent; do
		[ -r "$uevent" ] || continue

		partuuid=$(awk -F= '$1 == "PARTUUID" { print tolower($2) }' "$uevent")
		partname=$(awk -F= '$1 == "PARTNAME" { print $2 }' "$uevent")

		if [ "$partuuid" = "$root_partuuid" ] &&
			[ "$partname" = wxr_rootfs ]; then
			match_count=$((match_count + 1))
		fi
	done

	case "$match_count" in
	0)
		echo nand
		;;
	1)
		echo usb
		;;
	*)
		echo "WXR USB sysupgrade: root PARTUUID matches multiple wxr_rootfs partitions" >&2
		return 1
		;;
	esac
}

wxr_usb_image_kind() {
	local image=$1
	local members

	members=$(tar -tf "$image" 2>/dev/null) || {
		echo unknown
		return 0
	}

	if printf '%s\n' "$members" |
		grep -Fxq "$WXR_USB_ARCHIVE_DIR/CONTROL"; then
		echo usb
	elif printf '%s\n' "$members" |
		grep -Fxq "$WXR_USB_NAND_ARCHIVE_DIR/CONTROL"; then
		echo nand
	else
		echo unknown
	fi
}

wxr_usb_resolve_layout() {
	local argument
	local cmdline
	local data_count=0
	local node
	local node_parent
	local node_sysfs
	local parent_node
	local parent_sysfs
	local partname
	local partn
	local partuuid
	local production_count=0
	local recovery_count=0
	local root_count=0
	local root_match_count=0
	local root_node=''
	local root_partuuid=''
	local root_sysfs
	local rootfs_count=0
	local sectors
	local start
	local uevent

	WXR_USB_RECOVERY_DEV=''
	WXR_USB_PRODUCTION_DEV=''
	WXR_USB_ROOTFS_DEV=''
	WXR_USB_DATA_DEV=''

	read -r cmdline < /proc/cmdline

	for argument in $cmdline; do
		case "$argument" in
		root=PARTUUID=*)
			root_partuuid=${argument#root=PARTUUID=}
			root_count=$((root_count + 1))
			;;
		esac
	done

	if [ "$root_count" -ne 1 ]; then
		echo "WXR USB sysupgrade: one root PARTUUID is required" >&2
		return 1
	fi

	root_partuuid=$(printf '%s' "$root_partuuid" | /bin/busybox tr '[:upper:]' '[:lower:]')

	for uevent in /sys/class/block/*/uevent; do
		[ -r "$uevent" ] || continue

		partuuid=$(awk -F= '$1 == "PARTUUID" { print tolower($2) }' "$uevent")
		partname=$(awk -F= '$1 == "PARTNAME" { print $2 }' "$uevent")

		if [ "$partuuid" = "$root_partuuid" ] &&
			[ "$partname" = wxr_rootfs ]; then
			root_node=${uevent%/uevent}
			root_node=${root_node##*/}
			root_match_count=$((root_match_count + 1))
		fi
	done

	if [ "$root_match_count" -ne 1 ]; then
		echo "WXR USB sysupgrade: root PARTUUID does not identify one wxr_rootfs partition" >&2
		return 1
	fi

	root_sysfs=$(readlink -f "/sys/class/block/$root_node") || return 1
	parent_sysfs=${root_sysfs%/*}
	parent_node=${parent_sysfs##*/}

	if [ ! -b "/dev/$parent_node" ]; then
		echo "WXR USB sysupgrade: parent disk /dev/$parent_node is unavailable" >&2
		return 1
	fi

	for uevent in /sys/class/block/*/uevent; do
		[ -r "$uevent" ] || continue

		node_sysfs=$(readlink -f "${uevent%/uevent}") || continue
		node_parent=${node_sysfs%/*}
		[ "$node_parent" = "$parent_sysfs" ] || continue

		partn=$(awk -F= '$1 == "PARTN" { print $2 }' "$uevent")
		[ -n "$partn" ] || continue

		node=${uevent%/uevent}
		node=${node##*/}
		partname=$(awk -F= '$1 == "PARTNAME" { print $2 }' "$uevent")
		read -r start < "/sys/class/block/$node/start"
		read -r sectors < "/sys/class/block/$node/size"

		case "$partn:$partname" in
		1:wxr_recovery)
			recovery_count=$((recovery_count + 1))
			[ "$start" -eq "$WXR_USB_RECOVERY_START" ] &&
				[ "$sectors" -eq "$WXR_USB_RECOVERY_SECTORS" ] || {
				echo "WXR USB sysupgrade: wxr_recovery geometry is incorrect" >&2
				return 1
			}
			WXR_USB_RECOVERY_DEV="/dev/$node"
			;;
		2:wxr_production)
			production_count=$((production_count + 1))
			[ "$start" -eq "$WXR_USB_PRODUCTION_START" ] &&
				[ "$sectors" -eq "$WXR_USB_PRODUCTION_SECTORS" ] || {
				echo "WXR USB sysupgrade: wxr_production geometry is incorrect" >&2
				return 1
			}
			WXR_USB_PRODUCTION_DEV="/dev/$node"
			;;
		3:wxr_rootfs)
			rootfs_count=$((rootfs_count + 1))
			[ "$start" -eq "$WXR_USB_ROOTFS_START" ] &&
				[ "$sectors" -eq "$WXR_USB_ROOTFS_SECTORS" ] || {
				echo "WXR USB sysupgrade: wxr_rootfs geometry is incorrect" >&2
				return 1
			}
			WXR_USB_ROOTFS_DEV="/dev/$node"
			;;
		4:rootfs_data)
			data_count=$((data_count + 1))
			[ "$start" -eq "$WXR_USB_DATA_START" ] &&
				[ "$sectors" -ge "$WXR_USB_DATA_MIN_SECTORS" ] || {
				echo "WXR USB sysupgrade: rootfs_data geometry is incorrect" >&2
				return 1
			}
			WXR_USB_DATA_DEV="/dev/$node"
			;;
		*)
			echo "WXR USB sysupgrade: parent disk has an unexpected partition contract" >&2
			return 1
			;;
		esac
	done

	if [ "$recovery_count" -ne 1 ] ||
		[ "$production_count" -ne 1 ] ||
		[ "$rootfs_count" -ne 1 ] ||
		[ "$data_count" -ne 1 ]; then
		echo "WXR USB sysupgrade: parent disk does not contain one complete WXR USB layout" >&2
		return 1
	fi

	if [ "$WXR_USB_ROOTFS_DEV" != "/dev/$root_node" ]; then
		echo "WXR USB sysupgrade: running root is not the resolved wxr_rootfs partition" >&2
		return 1
	fi

	for node in \
		"$WXR_USB_RECOVERY_DEV" \
		"$WXR_USB_PRODUCTION_DEV" \
		"$WXR_USB_ROOTFS_DEV" \
		"$WXR_USB_DATA_DEV"; do
		if [ ! -b "$node" ]; then
			echo "WXR USB sysupgrade: target partition $node is unavailable" >&2
			return 1
		fi
	done
}

wxr_usb_validate_fit() {
	local actual_hash
	local byte
	local component_file
	local component_name
	local component_node
	local contract_version
	local expected_hash
	local fit=$1
	local fit_size
	local hash_bytes
	local image_nodes
	local position
	local role
	local rootfs_words
	local word

	fit_size=$(wc -c < "$fit") || return 1

	if [ "$fit_size" -le 0 ] ||
		[ "$fit_size" -gt "$WXR_USB_PRODUCTION_BYTES" ]; then
		echo "WXR USB sysupgrade: production FIT does not fit in wxr_production" >&2
		return 1
	fi

	image_nodes=$(fdtget -l "$fit" /images) || {
		echo "WXR USB sysupgrade: production FIT is not a readable device tree" >&2
		return 1
	}

	if [ "$image_nodes" != "$(printf 'kernel@1\nfdt@1\ncontract@1')" ]; then
		echo "WXR USB sysupgrade: production FIT has unexpected image nodes" >&2
		return 1
	fi

	[ "$(fdtget -t s "$fit" /configurations default)" = config@hk01 ] &&
		[ "$(fdtget -t s "$fit" '/configurations/config@hk01' kernel)" = kernel@1 ] &&
		[ "$(fdtget -t s "$fit" '/configurations/config@hk01' fdt)" = fdt@1 ] || {
		echo "WXR USB sysupgrade: production FIT configuration is incorrect" >&2
		return 1
	}

	[ "$(fdtget -t s "$fit" '/images/kernel@1' type)" = kernel ] &&
		[ "$(fdtget -t s "$fit" '/images/kernel@1' arch)" = arm64 ] &&
		[ "$(fdtget -t s "$fit" '/images/kernel@1' os)" = linux ] &&
		[ "$(fdtget -t s "$fit" '/images/kernel@1' compression)" = none ] &&
		[ "$(fdtget -t x "$fit" '/images/kernel@1' load)" = 41000000 ] &&
		[ "$(fdtget -t x "$fit" '/images/kernel@1' entry)" = 41000000 ] || {
		echo "WXR USB sysupgrade: production FIT kernel contract is incorrect" >&2
		return 1
	}

	[ "$(fdtget -t s "$fit" '/images/fdt@1' type)" = flat_dt ] &&
		[ "$(fdtget -t s "$fit" '/images/fdt@1' arch)" = arm64 ] &&
		[ "$(fdtget -t s "$fit" '/images/fdt@1' compression)" = none ] || {
		echo "WXR USB sysupgrade: production FIT device-tree contract is incorrect" >&2
		return 1
	}

	[ "$(fdtget -t s "$fit" '/images/contract@1' type)" = firmware ] &&
		[ "$(fdtget -t s "$fit" '/images/contract@1' arch)" = arm64 ] &&
		[ "$(fdtget -t s "$fit" '/images/contract@1' compression)" = none ] || {
		echo "WXR USB sysupgrade: production FIT metadata contract is incorrect" >&2
		return 1
	}

	for position in 0 1 2; do
		case "$position" in
		0)
			component_name=kernel
			component_node=kernel@1
			;;
		1)
			component_name=fdt
			component_node=fdt@1
			;;
		2)
			component_name=contract
			component_node=contract@1
			;;
		esac

		component_file="$WXR_USB_WORK_DIR/$component_name.bin"
		[ "$(fdtget -t s "$fit" "/images/$component_node/hash@1" algo)" = sha256 ] || {
			echo "WXR USB sysupgrade: $component_name hash algorithm is incorrect" >&2
			return 1
		}

		dumpimage -T flat_dt -p "$position" -o "$component_file" "$fit" >/dev/null || {
			echo "WXR USB sysupgrade: cannot extract FIT $component_name" >&2
			return 1
		}

		hash_bytes=$(fdtget -t bx "$fit" "/images/$component_node/hash@1" value) || return 1
		expected_hash=''
		for byte in $hash_bytes; do
			expected_hash="$expected_hash$(printf '%02x' "$((0x$byte))")"
		done

		actual_hash=$(/bin/busybox sha256sum "$component_file") || return 1
		actual_hash=${actual_hash%% *}

		if [ "$actual_hash" != "$expected_hash" ]; then
			echo "WXR USB sysupgrade: $component_name SHA-256 mismatch" >&2
			return 1
		fi
	done

	cp "$WXR_USB_WORK_DIR/contract.bin" "$WXR_USB_CONTRACT" || return 1

	[ "$(fdtget -t s "$WXR_USB_CONTRACT" / compatible)" = \
		openwrt,wxr-5950ax12-boot-contract ] || {
		echo "WXR USB sysupgrade: unknown boot contract" >&2
		return 1
	}

	contract_version=$(fdtget -t x "$WXR_USB_CONTRACT" / openwrt,boot-contract-version) || return 1
	if [ "$contract_version" != 1 ]; then
		echo "WXR USB sysupgrade: unsupported boot contract version" >&2
		return 1
	fi

	role=$(fdtget -t s "$WXR_USB_CONTRACT" / openwrt,image-role) || return 1
	if [ "$role" != usb-production ]; then
		echo "WXR USB sysupgrade: FIT role is not usb-production" >&2
		return 1
	fi

	rootfs_words=$(fdtget -t x "$WXR_USB_CONTRACT" / openwrt,rootfs-bytes) || return 1
	set -- $rootfs_words
	if [ "$#" -ne 2 ]; then
		echo "WXR USB sysupgrade: rootfs length contract is malformed" >&2
		return 1
	fi
	WXR_USB_ROOTFS_BYTES=$((0x$1 * 4294967296 + 0x$2))

	if [ "$WXR_USB_ROOTFS_BYTES" -le 0 ] ||
		[ "$WXR_USB_ROOTFS_BYTES" -gt "$WXR_USB_ROOTFS_BYTES_MAX" ]; then
		echo "WXR USB sysupgrade: rootfs length contract exceeds wxr_rootfs" >&2
		return 1
	fi

	hash_bytes=$(fdtget -t bx "$WXR_USB_CONTRACT" / openwrt,rootfs-sha256) || return 1
	WXR_USB_ROOTFS_SHA256=''
	for byte in $hash_bytes; do
		WXR_USB_ROOTFS_SHA256="$WXR_USB_ROOTFS_SHA256$(printf '%02x' "$((0x$byte))")"
	done

	if [ "${#WXR_USB_ROOTFS_SHA256}" -ne 64 ]; then
		echo "WXR USB sysupgrade: rootfs SHA-256 contract is malformed" >&2
		return 1
	fi
}

wxr_usb_validate_image() {
	local calculated_hash
	local control
	local expected_members
	local image=$1
	local listing
	local member
	local member_count
	local members
	local rootfs_file_size
	local squashfs_bytes

	rm -rf "$WXR_USB_WORK_DIR"
	mkdir -p "$WXR_USB_WORK_DIR" || return 1

	members=$(tar -tf "$image") || {
		echo "WXR USB sysupgrade: archive cannot be read" >&2
		return 1
	}

	expected_members=$(printf '%s\n' \
		"$WXR_USB_ARCHIVE_DIR/" \
		"$WXR_USB_ARCHIVE_DIR/CONTROL" \
		"$WXR_USB_ARCHIVE_DIR/kernel" \
		"$WXR_USB_ARCHIVE_DIR/root")

	if [ "$members" != "$expected_members" ]; then
		echo "WXR USB sysupgrade: archive members do not match the USB contract" >&2
		return 1
	fi

	listing=$(tar -tvf "$image") || return 1
	for member in CONTROL kernel root; do
		member_count=$(printf '%s\n' "$listing" |
			awk -v path="$WXR_USB_ARCHIVE_DIR/$member" \
			'$NF == path && substr($1, 1, 1) == "-" { count++ } END { print count + 0 }')
		if [ "$member_count" -ne 1 ]; then
			echo "WXR USB sysupgrade: $member is not one regular archive member" >&2
			return 1
		fi
	done

	control=$(tar -xOf "$image" "$WXR_USB_ARCHIVE_DIR/CONTROL") || return 1
	if [ "$control" != "BOARD=$WXR_USB_ARCHIVE_BOARD" ]; then
		echo "WXR USB sysupgrade: CONTROL board is incorrect" >&2
		return 1
	fi

	tar -xOf "$image" "$WXR_USB_ARCHIVE_DIR/kernel" > "$WXR_USB_FIT" || return 1
	tar -xOf "$image" "$WXR_USB_ARCHIVE_DIR/root" > "$WXR_USB_ROOTFS" || return 1

	[ -s "$WXR_USB_FIT" ] && [ -s "$WXR_USB_ROOTFS" ] || {
		echo "WXR USB sysupgrade: archive payload is empty" >&2
		return 1
	}

	wxr_usb_validate_fit "$WXR_USB_FIT" || return 1

	set -- $(hexdump -n 4 -e '4/1 "%u "' "$WXR_USB_ROOTFS")
	if [ "$#" -ne 4 ] ||
		[ "$1" -ne 104 ] || [ "$2" -ne 115 ] ||
		[ "$3" -ne 113 ] || [ "$4" -ne 115 ]; then
		echo "WXR USB sysupgrade: root payload is not little-endian SquashFS" >&2
		return 1
	fi

	set -- $(hexdump -s 40 -n 8 -e '8/1 "%u "' "$WXR_USB_ROOTFS")
	if [ "$#" -ne 8 ]; then
		echo "WXR USB sysupgrade: SquashFS bytes_used cannot be read" >&2
		return 1
	fi

	squashfs_bytes=$(($1 + ($2 << 8) + ($3 << 16) + ($4 << 24) +
		($5 << 32) + ($6 << 40) + ($7 << 48) + ($8 << 56)))
	rootfs_file_size=$(wc -c < "$WXR_USB_ROOTFS") || return 1

	if [ "$rootfs_file_size" -gt "$WXR_USB_ROOTFS_BYTES_MAX" ]; then
		echo "WXR USB sysupgrade: root payload exceeds wxr_rootfs" >&2
		return 1
	fi

	if [ "$squashfs_bytes" -ne "$WXR_USB_ROOTFS_BYTES" ] ||
		[ "$squashfs_bytes" -gt "$rootfs_file_size" ] ||
		[ "$squashfs_bytes" -gt "$WXR_USB_ROOTFS_BYTES_MAX" ]; then
		echo "WXR USB sysupgrade: SquashFS length differs from the FIT contract" >&2
		return 1
	fi

	calculated_hash=$(head -c "$WXR_USB_ROOTFS_BYTES" "$WXR_USB_ROOTFS" |
		/bin/busybox sha256sum) || return 1
	calculated_hash=${calculated_hash%% *}

	if [ "$calculated_hash" != "$WXR_USB_ROOTFS_SHA256" ]; then
		echo "WXR USB sysupgrade: root payload SHA-256 mismatch" >&2
		return 1
	fi
}

wxr_usb_rebuild_overlay() {
	local backup_file=${BACKUP_FILE:-sysupgrade.tgz}
	local upgrade_backup=${UPGRADE_BACKUP:-}

	rm -rf "$WXR_USB_OVERLAY_MOUNT"
	mkdir -p "$WXR_USB_OVERLAY_MOUNT" || return 1

	mkfs.ext4 -F -L rootfs_data "$WXR_USB_DATA_DEV" || {
		echo "WXR USB sysupgrade: cannot format rootfs_data" >&2
		return 1
	}

	mount -t ext4 "$WXR_USB_DATA_DEV" "$WXR_USB_OVERLAY_MOUNT" || {
		echo "WXR USB sysupgrade: cannot mount new rootfs_data" >&2
		return 1
	}

	if [ -n "$upgrade_backup" ]; then
		if ! cp "$upgrade_backup" "$WXR_USB_OVERLAY_MOUNT/$backup_file" ||
			! chmod 600 "$WXR_USB_OVERLAY_MOUNT/$backup_file"; then
			sync
			umount "$WXR_USB_OVERLAY_MOUNT"
			echo "WXR USB sysupgrade: cannot restore config backup" >&2
			return 1
		fi
	fi

	sync
	if ! umount "$WXR_USB_OVERLAY_MOUNT"; then
		echo "WXR USB sysupgrade: cannot unmount rootfs_data" >&2
		return 1
	fi
	rmdir "$WXR_USB_OVERLAY_MOUNT"
}

wxr_usb_do_upgrade() {
	local calculated_hash
	local expected_hash
	local fit_readback="$WXR_USB_WORK_DIR/production.readback.itb"
	local fit_size
	local image=$1
	local medium

	medium=$(wxr_usb_running_medium) || return 1
	if [ "$medium" != usb ]; then
		echo "WXR USB sysupgrade: system is not running from USB production" >&2
		return 1
	fi

	wxr_usb_resolve_layout || return 1
	wxr_usb_validate_image "$image" || return 1

	if awk -v rootfs="$WXR_USB_ROOTFS_DEV" -v data="$WXR_USB_DATA_DEV" \
		'$1 == rootfs || $1 == data { mounted = 1 } END { exit !mounted }' \
		/proc/mounts; then
		echo "WXR USB sysupgrade: target partitions are still mounted" >&2
		return 1
	fi

	v "Writing USB rootfs partition"
	dd if="$WXR_USB_ROOTFS" of="$WXR_USB_ROOTFS_DEV" bs=1M || return 1
	sync

	calculated_hash=$(head -c "$WXR_USB_ROOTFS_BYTES" "$WXR_USB_ROOTFS_DEV" |
		/bin/busybox sha256sum) || return 1
	calculated_hash=${calculated_hash%% *}
	if [ "$calculated_hash" != "$WXR_USB_ROOTFS_SHA256" ]; then
		echo "WXR USB sysupgrade: wxr_rootfs readback SHA-256 mismatch" >&2
		return 1
	fi

	v "Recreating USB rootfs_data partition"
	wxr_usb_rebuild_overlay || return 1

	fit_size=$(wc -c < "$WXR_USB_FIT") || return 1
	expected_hash=$(/bin/busybox sha256sum "$WXR_USB_FIT") || return 1
	expected_hash=${expected_hash%% *}

	v "Writing USB production FIT"
	dd if="$WXR_USB_FIT" of="$WXR_USB_PRODUCTION_DEV" bs=1M || return 1
	sync

	head -c "$fit_size" "$WXR_USB_PRODUCTION_DEV" > "$fit_readback" || return 1
	calculated_hash=$(/bin/busybox sha256sum "$fit_readback") || return 1
	calculated_hash=${calculated_hash%% *}
	if [ "$calculated_hash" != "$expected_hash" ]; then
		echo "WXR USB sysupgrade: wxr_production readback SHA-256 mismatch" >&2
		return 1
	fi

	wxr_usb_validate_fit "$fit_readback" || {
		echo "WXR USB sysupgrade: wxr_production readback FIT is invalid" >&2
		return 1
	}
}

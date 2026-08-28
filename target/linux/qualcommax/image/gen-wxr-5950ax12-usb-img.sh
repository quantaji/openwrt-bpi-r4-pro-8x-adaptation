#!/bin/sh

set -eu

IMAGE_SIZE=999999488
RECOVERY_PARTITION_SIZE=134217728
PRODUCTION_PARTITION_SIZE=134217728
ROOTFS_PARTITION_SIZE=268435456

EXPECTED_RECOVERY_OFFSET=1048576
EXPECTED_PRODUCTION_OFFSET=135266304
EXPECTED_ROOTFS_OFFSET=269484032
EXPECTED_OVERLAY_OFFSET=537919488
EXPECTED_OVERLAY_SIZE=462062592

die()
{
	echo "gen-wxr-5950ax12-usb-img: $*" >&2
	exit 1
}

file_size()
{
	stat -c '%s' "$1"
}

[ "$#" -eq 5 ] ||
	die "usage: PTGEN OUTPUT RECOVERY_FIT PRODUCTION_FIT ROOTFS"

ptgen=$1
output=$2
recovery_fit=$3
production_fit=$4
rootfs=$5

[ -x "$ptgen" ] || die "ptgen is not executable"
[ -s "$recovery_fit" ] || die "recovery FIT is missing or empty"
[ -s "$production_fit" ] || die "production FIT is missing or empty"
[ -s "$rootfs" ] || die "rootfs is missing or empty"

recovery_size=$(file_size "$recovery_fit")
production_size=$(file_size "$production_fit")
rootfs_size=$(file_size "$rootfs")

[ "$recovery_size" -le "$RECOVERY_PARTITION_SIZE" ] ||
	die "recovery FIT exceeds p1"

[ "$production_size" -le "$PRODUCTION_PARTITION_SIZE" ] ||
	die "production FIT exceeds p2"

[ "$rootfs_size" -le "$ROOTFS_PARTITION_SIZE" ] ||
	die "rootfs exceeds p3"

set -- $(od -An -tu1 -N4 "$rootfs")

[ "$#" -eq 4 ] ||
	die "cannot read SquashFS magic"

[ "$1" -eq 104 ] &&
	[ "$2" -eq 115 ] &&
	[ "$3" -eq 113 ] &&
	[ "$4" -eq 115 ] ||
	die "rootfs is not little-endian SquashFS"

set -- $(od -An -tu1 -j40 -N8 "$rootfs")

[ "$#" -eq 8 ] ||
	die "cannot read SquashFS bytes_used"

rootfs_bytes_used=$((
	$1 +
	($2 << 8) +
	($3 << 16) +
	($4 << 24) +
	($5 << 32) +
	($6 << 40) +
	($7 << 48) +
	($8 << 56)
))

[ "$rootfs_bytes_used" -gt 0 ] ||
	die "SquashFS bytes_used is zero"

[ "$rootfs_bytes_used" -le "$rootfs_size" ] ||
	die "SquashFS bytes_used exceeds the rootfs file"

[ "$rootfs_bytes_used" -le "$ROOTFS_PARTITION_SIZE" ] ||
	die "SquashFS bytes_used exceeds p3"

partition_data=$(
	"$ptgen" \
		-g \
		-D \
		-a 0 \
		-l 1024 \
		-d 976562K \
		-o "$output" \
		-t 2e -N wxr_recovery -p 128M@1M \
		-t 2e -N wxr_production -p 128M \
		-t 83 -N wxr_rootfs -p 256M \
		-t 83 -N rootfs_data -p 451233K
)

# ptgen prints offset and size for each partition.
set -- $partition_data

[ "$#" -eq 8 ] ||
	die "ptgen did not return four partition ranges"

[ "$1" -eq "$EXPECTED_RECOVERY_OFFSET" ] &&
	[ "$2" -eq "$RECOVERY_PARTITION_SIZE" ] ||
	die "unexpected p1 geometry"

[ "$3" -eq "$EXPECTED_PRODUCTION_OFFSET" ] &&
	[ "$4" -eq "$PRODUCTION_PARTITION_SIZE" ] ||
	die "unexpected p2 geometry"

[ "$5" -eq "$EXPECTED_ROOTFS_OFFSET" ] &&
	[ "$6" -eq "$ROOTFS_PARTITION_SIZE" ] ||
	die "unexpected p3 geometry"

[ "$7" -eq "$EXPECTED_OVERLAY_OFFSET" ] &&
	[ "$8" -eq "$EXPECTED_OVERLAY_SIZE" ] ||
	die "unexpected p4 geometry"

[ "$(file_size "$output")" -eq "$IMAGE_SIZE" ] ||
	die "ptgen produced an unexpected image size"

dd \
	if="$recovery_fit" \
	of="$output" \
	bs=512 \
	seek=$((EXPECTED_RECOVERY_OFFSET / 512)) \
	conv=notrunc \
	status=none

dd \
	if="$production_fit" \
	of="$output" \
	bs=512 \
	seek=$((EXPECTED_PRODUCTION_OFFSET / 512)) \
	conv=notrunc \
	status=none

dd \
	if="$rootfs" \
	of="$output" \
	bs=512 \
	seek=$((EXPECTED_ROOTFS_OFFSET / 512)) \
	conv=notrunc \
	status=none

[ "$(file_size "$output")" -eq "$IMAGE_SIZE" ] ||
	die "writing partition contents changed the image size"

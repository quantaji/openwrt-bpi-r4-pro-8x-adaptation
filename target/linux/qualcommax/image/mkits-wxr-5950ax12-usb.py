#!/usr/bin/env python3

import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ARM64_HEADER_SIZE = 64
ARM64_MAGIC_OFFSET = 56
ARM64_MAGIC = b"ARM\x64"
KERNEL_LOAD_ADDRESS = 0x41000000
FIT_INPUT_ADDRESS = 0x44000000
MAX_KERNEL_SIZE = 48 * 1024 * 1024
MAX_FIT_SIZE = 64 * 1024 * 1024
ROOTFS_PARTITION_SIZE = 256 * 1024 * 1024
SQUASHFS_BYTES_USED_OFFSET = 40
SQUASHFS_HEADER_SIZE = 48
SQUASHFS_MAGIC = b"hsqs"
HASH_CHUNK_SIZE = 1024 * 1024


def inspect_kernel(kernel_path):
	kernel_size = kernel_path.stat().st_size

	with kernel_path.open("rb") as kernel_file:
		header = kernel_file.read(ARM64_HEADER_SIZE)

	if len(header) != ARM64_HEADER_SIZE:
		raise ValueError("ARM64 Image header is incomplete")

	if header[ARM64_MAGIC_OFFSET:ARM64_MAGIC_OFFSET + 4] != ARM64_MAGIC:
		raise ValueError("kernel is not an ARM64 Image")

	text_offset, image_size = struct.unpack_from("<QQ", header, 8)

	if text_offset != 0:
		raise ValueError("ARM64 Image text_offset must be zero")

	if kernel_size == 0 or image_size == 0:
		raise ValueError("ARM64 Image size must be nonzero")

	effective_size = max(kernel_size, image_size)

	if kernel_size > MAX_KERNEL_SIZE or image_size > MAX_KERNEL_SIZE:
		raise ValueError("ARM64 Image exceeds the 48 MiB contract")

	if KERNEL_LOAD_ADDRESS + effective_size > FIT_INPUT_ADDRESS:
		raise ValueError("ARM64 Image overlaps the FIT input window")


def inspect_rootfs(rootfs_path):
	rootfs_size = rootfs_path.stat().st_size

	if rootfs_size > ROOTFS_PARTITION_SIZE:
		raise ValueError("rootfs file exceeds the 256 MiB partition")

	with rootfs_path.open("rb") as rootfs_file:
		header = rootfs_file.read(SQUASHFS_HEADER_SIZE)

		if len(header) != SQUASHFS_HEADER_SIZE:
			raise ValueError("SquashFS header is incomplete")

		if header[:4] != SQUASHFS_MAGIC:
			raise ValueError("rootfs is not little-endian SquashFS")

		bytes_used = struct.unpack_from(
			"<Q", header, SQUASHFS_BYTES_USED_OFFSET
		)[0]

		if bytes_used == 0:
			raise ValueError("SquashFS bytes_used must be nonzero")

		if bytes_used > rootfs_size:
			raise ValueError("SquashFS bytes_used exceeds the rootfs file")

		if bytes_used > ROOTFS_PARTITION_SIZE:
			raise ValueError("SquashFS bytes_used exceeds the p3 contract")

		digest = hashlib.sha256()
		remaining = bytes_used
		rootfs_file.seek(0)

		while remaining:
			chunk = rootfs_file.read(min(HASH_CHUNK_SIZE, remaining))

			if not chunk:
				raise ValueError(
					"rootfs ended during SHA-256 calculation"
				)

			digest.update(chunk)
			remaining -= len(chunk)

	return bytes_used, digest.digest()


def quote_dts_path(path):
	return str(path.resolve()).replace("\\", "\\\\").replace('"', '\\"')


def write_build_sources(
	role,
	kernel_path,
	dtb_path,
	contract_dts_path,
	contract_dtb_path,
	its_path,
	rootfs_contract,
):
	rootfs_properties = ""

	if rootfs_contract is not None:
		bytes_used, digest = rootfs_contract
		digest_bytes = " ".join(f"{byte:02x}" for byte in digest)
		rootfs_properties = f"""
\topenwrt,rootfs-bytes = /bits/ 64 <0x{bytes_used:016x}>;
\topenwrt,rootfs-sha256 = [{digest_bytes}];
"""

	contract_dts = f"""/dts-v1/;

/ {{
\tcompatible = "openwrt,wxr-5950ax12-boot-contract";
\topenwrt,boot-contract-version = <1>;
\topenwrt,image-role = "{role}";
{rootfs_properties}}};
"""

	its = f"""/dts-v1/;

/ {{
\tdescription = "Buffalo WXR-5950AX12 USB boot FIT";
\t#address-cells = <1>;

\timages {{
\t\tkernel@1 {{
\t\t\tdescription = "OpenWrt ARM64 Image";
\t\t\tdata = /incbin/("{quote_dts_path(kernel_path)}");
\t\t\ttype = "kernel";
\t\t\tarch = "arm64";
\t\t\tos = "linux";
\t\t\tcompression = "none";
\t\t\tload = <0x41000000>;
\t\t\tentry = <0x41000000>;

\t\t\thash@1 {{
\t\t\t\talgo = "sha256";
\t\t\t}};
\t\t}};

\t\tfdt@1 {{
\t\t\tdescription = "Buffalo WXR-5950AX12 USB device tree";
\t\t\tdata = /incbin/("{quote_dts_path(dtb_path)}");
\t\t\ttype = "flat_dt";
\t\t\tarch = "arm64";
\t\t\tcompression = "none";

\t\t\thash@1 {{
\t\t\t\talgo = "sha256";
\t\t\t}};
\t\t}};

\t\tcontract@1 {{
\t\t\tdescription = "WXR-5950AX12 USB boot contract";
\t\t\tdata = /incbin/("{quote_dts_path(contract_dtb_path)}");
\t\t\ttype = "firmware";
\t\t\tarch = "arm64";
\t\t\tcompression = "none";

\t\t\thash@1 {{
\t\t\t\talgo = "sha256";
\t\t\t}};
\t\t}};
\t}};

\tconfigurations {{
\t\tdefault = "config@hk01";

\t\tconfig@hk01 {{
\t\t\tdescription = "Buffalo WXR-5950AX12 USB boot";
\t\t\tkernel = "kernel@1";
\t\t\tfdt = "fdt@1";
\t\t}};
\t}};
}};
"""

	contract_dts_path.write_text(contract_dts, encoding="utf-8")
	its_path.write_text(its, encoding="utf-8")


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument(
		"--role",
		required=True,
		choices=("recovery", "usb-production"),
	)
	parser.add_argument("--kernel", required=True, type=Path)
	parser.add_argument("--dtb", required=True, type=Path)
	parser.add_argument("--rootfs", type=Path)
	parser.add_argument("--dtc", required=True, type=Path)
	parser.add_argument("--mkimage", required=True, type=Path)
	parser.add_argument("--output", required=True, type=Path)
	args = parser.parse_args()

	if args.role == "recovery" and args.rootfs is not None:
		raise ValueError("recovery FIT must not carry a rootfs contract")

	if args.role == "usb-production" and args.rootfs is None:
		raise ValueError("usb-production FIT requires a rootfs")

	inspect_kernel(args.kernel)

	rootfs_contract = None

	if args.rootfs is not None:
		rootfs_contract = inspect_rootfs(args.rootfs)

	with tempfile.TemporaryDirectory(
		prefix=f"{args.output.name}.",
		dir=args.output.parent,
	) as temporary_directory:
		temporary_path = Path(temporary_directory)
		contract_dts_path = temporary_path / "contract.dts"
		contract_dtb_path = temporary_path / "contract.dtb"
		its_path = temporary_path / "image.its"
		fit_path = temporary_path / "image.itb"

		write_build_sources(
			args.role,
			args.kernel,
			args.dtb,
			contract_dts_path,
			contract_dtb_path,
			its_path,
			rootfs_contract,
		)

		subprocess.run(
			(
				str(args.dtc),
				"-I", "dts",
				"-O", "dtb",
				"-o", str(contract_dtb_path),
				str(contract_dts_path),
			),
			check=True,
		)

		mkimage_environment = os.environ.copy()
		mkimage_environment["PATH"] = (
			f"{args.dtc.parent}:"
			f"{mkimage_environment.get('PATH', '')}"
		)

		subprocess.run(
			(
				str(args.mkimage),
				"-f", str(its_path),
				str(fit_path),
			),
			check=True,
			env=mkimage_environment,
		)

		fit_size = fit_path.stat().st_size

		if fit_size == 0 or fit_size > MAX_FIT_SIZE:
			raise ValueError("FIT size is outside the 64 MiB contract")

		os.replace(fit_path, args.output)


if __name__ == "__main__":
	try:
		main()
	except (OSError, ValueError, subprocess.CalledProcessError) as error:
		print(f"mkits-wxr-5950ax12-usb: {error}", file=sys.stderr)
		sys.exit(1)

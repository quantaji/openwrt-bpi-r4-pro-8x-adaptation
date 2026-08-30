# OpenWrt Custom Playground

A collection of my adaptation of OpenWRT.

<pre>
README                                           orphan documentation branch; no OpenWrt parent

OpenWrt upstream
├── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/main">main</a>
│   ├── BPI-R4 Pro 8X shared base                shared base, not a branch
│   │   ├── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/as21-mxl-debugging">as21-mxl-debugging</a>                   preserves the AS21 and MxL diagnostic matrix
│   │   └── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/bpi-r4-pro-8x-after-mxl-update">bpi-r4-pro-8x-after-mxl-update</a>       continues the complete board integration
│   └── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/buffalo-wxr-5950ax12-playground">buffalo-wxr-5950ax12-playground</a>          USB boot, sysupgrade, and canonical recovery FIT
│       └── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/nss-upstream-adaptation">nss-upstream-adaptation</a>             planned NSS upstream adaptation work
│
└── OpenWrt v25.12.4
    └── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/codex/bpi-r4-pro-8x-v25.12.4">codex/bpi-r4-pro-8x-v25.12.4</a>             original BPI-R4 Pro 8X bring-up and MxL PPE work
        ├── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/as21xx-upstream-style-vendor-migration">as21xx-upstream-style-vendor-migration</a>  upstream-style AS21xx vendor-behavior migration
        │   └── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/as21xx-debugfs-diagnostics">as21xx-debugfs-diagnostics</a>       temporary diagnostics layered on the migration
        └── <a href="https://github.com/quantaji/openwrt-custom-playground/tree/mxl-old-fw-legacy-pause-failed-experiment">mxl-old-fw-legacy-pause-failed-experiment</a>  preserved failed old-firmware pause experiment
</pre>

The regular `main`, `master`, and `openwrt-*` branches mirror upstream source
lines. They are reference branches rather than project-specific work branches.

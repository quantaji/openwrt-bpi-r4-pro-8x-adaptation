# IPQ8074 NSS firmware

These are the unmodified local `retail_router0.bin` and `retail_router1.bin`
images for NSS.HK.11.4.0.5-6-R. Both cores are required by the selected QDX
configuration. Source checkout and package payload include both originals;
there is no firmware download step at build time or runtime.

## Source and identity

- Repository: https://github.com/qosmio/qca-sdk-nss-fw
- Source commit: `e4726900138b3e77fbbeaa32e58aa40a66d6b01a`
- Archive: `QCA_Networking_2021.SPF_11.4/CS/IPQ8074.ATH.11.4/BIN-NSS.HK.11.4.0.5-6-R.tar.bz2`
- Archive encoding: XZ-compressed tar, despite its `.tar.bz2` suffix.
- Archive SHA256: `a34c67fb53082454d3510cd3663f745146a11805dce9393952b8a14642c70cb8`
- License source: the same source repository's `LICENSE.md`, reproduced below.

| File | Bytes | SHA256 |
|---|---:|---|
| `retail_router0.bin` | 835960 | `1c3badac4694554a89f556d7a9c22310cb8cab8e9d263a12e99e19ed734e26cd` |
| `retail_router1.bin` | 292296 | `67ab46b29b62441e8de8be89137dba323ab4e9c4f152f875055b822a81348c6a` |

The package installs both files with mode 0644 in `/lib/firmware/qdx/11.4/`
and this notice in `/usr/share/doc/qdx-nss-firmware/`. Check installed image
hashes and permissions when validating a build. The driver uses these exact
versioned paths with `request_firmware_direct()`.

## Original firmware license

The following text is reproduced without modification from the source
repository. The driver source license does not change the firmware license.

Copyright (c) 2013-2019 Qualcomm Innovation Center, Inc.

All rights reserved.

Subject to the terms and conditions set forth below, Qualcomm
Innovation Center, Inc. (“QuIC) hereby grants to you a nonexclusive,
limited license under QuIC’s copyrights to reproduce and redistribute
this software in binary forms, without modification, for use solely in
conjunction with a Qualcomm Technologies, Inc. chipset, provided that
the following conditions are met:

•	Redistributions must reproduce the above copyright notice, this list
of conditions, and the following disclaimer in the documentation
and/or other materials provided with the distribution.

•	Neither the name of Qualcomm  Innovation Center, Inc. nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

•	No reverse engineering, decompiling, decrypting, or disassembling of
this software is permitted.

•	Subjecting this software to any third party license terms (e.g.
open source license terms), by any means, is prohibited.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. NO LICENSES OR OTHER RIGHTS,
WHETHER EXPRESS, IMPLIED, BASED ON ESTOPPEL OR OTHERWISE, ARE GRANTED
TO ANY PARTY'S PATENTS, PATENT APPLICATIONS, OR PATENTABLE INVENTIONS
BY VIRTUE OF THIS LICENSE OR THE DELIVERY OR PROVISION BY QUALCOMM
INNOVATION CENTER, INC. OF THE SOFTWARE.
IN NO EVENT SHALL THE COPYRIGHT OWNER OR ANY CONTRIBUTOR BE LIABLE FOR
ANY INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND REGARDLESS OF ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF OR RESULTING FROM THE USE OF THE
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGES. IN ANY
EVENT, THE TOTAL AGGREGATE LIABILITY THAT MAY BE IMPOSED ON QUALCOMM
INNOVATION CENTER, INC. FOR ANY DIRECT DAMAGES ARISING UNDER OR RESULTING FROM
THIS AGREEMENT OR IN CONNECTION WITH ANY USE OF THE SOFTWARE SHALL NOT
EXCEED A TOTAL AMOUNT OF US$5.00.

IF ANY OF THE ABOVE PROVISIONS ARE HELD TO BE VOID, INVALID,
UNENFORCEABLE, OR ILLEGAL, THE OTHER PROVISIONS SHALL CONTINUE IN FULL
FORCE AND EFFECT.

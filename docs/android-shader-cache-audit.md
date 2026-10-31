# Android shader cache audit (Lane 5)

Audit of the device cache at
`/data/local/tmp/totk-android/lanes/shader/shader-cache` and the UI dumps under
`/data/local/tmp/totk-android/lanes/shader/shader-dump*` / `missdump*`.

Reproduce with:

```sh
python3 games/totk/android/tools/audit_shader_cache.py \
  --cache /tmp/totk-lane5/cache-cs \
  --manifest /tmp/totk-lane5/cache-manifest.txt \
  --dump /tmp/totk-lane5/shader-dump-ui \
  --dump /tmp/totk-lane5/shader-dump-ui3 \
  --dump /tmp/totk-lane5/missdump \
  --dump /tmp/totk-lane5/missdump5 \
  --dump /tmp/totk-lane5/missdump8 \
  --stage cs \
  --json /tmp/totk-lane5/audit-cs.json \
  --markdown /tmp/totk-lane5/audit-cs.md
```

`--manifest` is a `find` listing of the full cache (`vs ps cs/*.spv`) so the
missing-key check covers stages whose `.res.json` were not pulled.  The cache
itself is never modified by the audit.

## 1. Cache shape

| tree | canonical `.spv` | variants | total `.spv` | `.res.json` |
| --- | --- | --- | --- | --- |
| live cache | 3,960 (vs 2,256 / ps 1,629 / cs 75) | 1,859 (vs 829 / ps 1,026 / cs 4) | 5,819 | 5,810 |
| compute stage (`cs`) | 75 | 4 | 79 | 79 |

The "9015" figure in the task brief is the sum of several work trees
(`shader-cache` 5,819 + `shader-cache-canon` 2,698 + `flat-spv` 440 + probe
mirrors).  The live `shader-cache` root used by the runtime contains 5,819
`.spv` files.  Nine canonical modules have no `.res.json` (vs 8, ps 1); they
still create a `VkShaderModule` but cannot provide descriptor reflection.

All 5,819 files passed `vkCreateShaderModule` on the Adreno 830; see
`docs/android-shader-cache-flow.md` section 7.2 for the probe output.

## 2. Compute (`cs`) reflection

Summary of the 75 canonical compute modules:

| metric | count |
| --- | --- |
| canonical modules | 75 |
| variants | 4 |
| UBO declarations | 146 |
| SSBO declarations | 566 |
| sampled textures | 58 |
| storage images | **0** |
| modules with any storage image | **0** |
| modules with SSBO + sampled texture | 40 |
| modules with SSBO only | 37 |
| modules with sampled texture only | 1 (`c7830fac`); `441394b7` and `6258b3e6` are SSBO-only |

No compute module in the cache declares a storage image.  The UI compute path
is therefore a UBO/SSBO/sampled-texture path; it builds UI data, it does not
write pixels through `imageStore`.

### 2.1 `d946082d...` (the UI compute module)

`d946082d551ed0a7b0cb7cbeb5f0a391b2af716a4df50f599404b983662e8632`:
**2 UBO, 8 SSBO, 1 sampled texture, 0 storage images**.

| kind | SPIR-V binding | NVN slot | details |
| --- | --- | --- | --- |
| UBO | 10 | cbuf 0 | |
| UBO | 11 | cbuf 1 | |
| SSBO | 2 | sbuf 0 | cb_slot 0, cb_offset 196, flags 1 |
| SSBO | 3 | sbuf 1 | cb_slot 0, cb_offset 156, flags 0 |
| SSBO | 4 | sbuf 2 | cb_slot 0, cb_offset 164, flags 0 |
| SSBO | 5 | sbuf 3 | cb_slot 0, cb_offset 228, flags 0 |
| SSBO | 6 | sbuf 4 | cb_slot 0, cb_offset 224, flags 0 |
| SSBO | 7 | sbuf 5 | cb_slot 0, cb_offset 220, flags 0 |
| SSBO | 8 | sbuf 6 | cb_slot 0, cb_offset 204, flags 1 |
| SSBO | 9 | sbuf 7 | cb_slot 0, cb_offset 208, flags 1 |
| sampled texture | 1 | tex 18 | `handle=18`, `type=4`, `array=1`; the variant suffix is `t18_3d` |
| storage image | - | - | none |

The texture slot decodes from the NVN handle exactly as
`prepare_shader_cache.res_texture_slot()` does: `off0 = handle & 0x3fff`,
`byte_off = off0 * 4`; a handle in `0x168..0x568` maps to
`(byte_off - 0x168) / 8`, otherwise `off0 < 128` maps to `off0`.  For
`handle=18` this is slot 18, matching the `<key>-t18_3d` variant.

The canonical and variant entries have identical reflection.  The variant
`.res.json` carries the translator's internal `key` spelling
`d946...--t18_3d`; the file on disk is `d946...-t18_3d.res.json`.

### 2.2 All canonical compute modules

Columns: `cbuf`/`sbuf` are `SPIR-V binding -> NVN slot`; a contiguous run is
shown as `2..9->0..7`.  `tex` is `binding -> NVN texture slot`.  Every row has
0 storage images.

| key | UBO | SSBO | sampled | role | cbuf | sbuf | tex |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `0007b9b6d5e3c98b0e4fea85a236a4813b80cd369891fd8a5047b1c9cf84b616` | 3 | 1 | 0 | compute-buffer | 2->3 3->4 4->0 | 1->0 | - |
| `0361b332f3ce94dae7e36c72edfc9498a65d51df7b04fa8ab17334507add6c32` | 2 | 12 | 0 | compute-buffer | 13->0 14->1 | 1..12->0..11 | - |
| `0c87321b295e2056f6d8e4aa82bf68dec245c8486afd67ad28227ffcf8ba9351` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `0c9caba9fa7d33ad35e9a34202af1dd231847a96f985966a66d6fcfbc4537a33` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `0e387987e6f4992ca8d4996600f84c827384bcff04096d3201147ff4281de2b1` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `0f6f591041bc86de3b4c06c83223f4a86a85fcb01a86fc85d56734017f239df4` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `0fd391ee524c8c8df1c2be0aad96126433458d24424577e4744c6260eb8cbb3b` | 2 | 9 | 1 | compute-sample+buffer | 11->0 12->1 | 2..10->0..8 | 1->18 |
| `10c6a8745bec071cb01cd36f7b6fe6c60663c641740bf8077a5fc9c46eb941e4` | 2 | 1 | 0 | compute-buffer | 2->3 3->1 | 1->0 | - |
| `153b6080430817646fec90e1e5bfcf9df57efa653b36c78268984cde08b4b685` | 2 | 6 | 1 | compute-sample+buffer | 8->0 9->1 | 2..7->0..5 | 1->18 |
| `1a9c8ab4eb6aa8cddb6bf3c01977a7e25b47f3f6bfdfb6876c44a74a65809c0c` | 2 | 8 | 3 | compute-sample+buffer | 12->0 13->1 | 4..11->0..7 | 1->18 2->32 3->42 |
| `1aade0eba6fd2576726075a8871af29e6df920fc1261e6e9bab54e6f351b7c58` | 1 | 7 | 0 | compute-buffer | 8->0 | 1..7->0..6 | - |
| `1d447ebcff426e9134c1175b7fbe8d27288e3c0755c5a67fb7085e0f8667444c` | 2 | 2 | 0 | compute-buffer | 3->0 4->3 | 1->0 2->1 | - |
| `1d73b6ff34074d837b5c105fa3c9a57d04c4908faff6c6ce723a93825d1bff4a` | 3 | 9 | 1 | compute-sample+buffer | 11->0 12->2 13->1 | 2..10->0..8 | 1->18 |
| `20d9bc0b602567e28d2d2bd4ce9544650f33706bc36bd1ee3945a0ec2096fdf8` | 2 | 9 | 1 | compute-sample+buffer | 11->0 12->1 | 2..10->0..8 | 1->18 |
| `21be4f32d3e39ccecd3d28265616d397510adc244af22cf8c0dbacce8f30da5b` | 2 | 6 | 0 | compute-buffer | 7->0 8->1 | 1..6->0..5 | - |
| `25c44c9858bde833c52db405b5e6d2d247dc3f0eee314c3610578751ce99ab56` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `26259bb46be4930be5e1b331b7a82e9b26b1e529756f194b8ff530b3c4786c5a` | 2 | 10 | 1 | compute-sample+buffer | 12->0 13->1 | 2..11->0..9 | 1->20 |
| `2a66228d48a647f7ecf3724e457c26815c00a2692c0cc749ca77ff0026c8a961` | 2 | 9 | 1 | compute-sample+buffer | 11->0 12->1 | 2..10->0..8 | 1->18 |
| `2b7ef0a5be96cd0f9e31eafa6dd519f97df821f1e7694c267dc754be35a10a2c` | 2 | 9 | 1 | compute-sample+buffer | 11->0 12->1 | 2..10->0..8 | 1->18 |
| `2bb2438aa301c9e23d09cb6f0618c5a5203868257aec4009a25f9e71722b66fa` | 2 | 11 | 3 | compute-sample+buffer | 15->0 16->1 | 4..14->0..10 | 1->18 2->32 3->42 |
| `2dbc25056c9ecd028535ca0b8b72723ff24bb2685a171fe1ce044901e23c119b` | 2 | 6 | 0 | compute-buffer | 7->0 8->1 | 1..6->0..5 | - |
| `31b3a819284c6e36250fe384b4af668c7c4c0473a8964d3a1b04601256e9e8a7` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `3220d0bb370d999176f22dcecda15210da798c671494c889fb4148ba5dfb6aa8` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `341a0021aa713960830d8d0962118e63ed09d25750c9e454a0488052d2bb3e3a` | 2 | 11 | 0 | compute-buffer | 12->0 13->1 | 1..11->0..10 | - |
| `3892b6868f06c11e22c952a7cf504037dbeedced69be28caff030a175ac67abc` | 2 | 9 | 0 | compute-buffer | 10->0 11->1 | 1..9->0..8 | - |
| `39ad801037b27798143620d13493bc7cc1d7aa342efbf9070bf1a2e4da5d3275` | 2 | 12 | 2 | compute-sample+buffer | 15->0 16->1 | 3..14->0..11 | 1->32 2->42 |
| `432f415838ca2a544a0e64b6f79a510bf353b88e834d1c8faeefcf38d1a2f564` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `441394b7d5190cfe2f5ff222bab15a73e723bb7d14cf06499841c6e917f1c731` | 0 | 1 | 0 | compute-buffer | - | 1->0 | - |
| `48e7cb41a927e17b15524f09acf94acc869963e6c82856048aa3b417c493786d` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `4acfa5bc7cbce897a931d2ca92a20ec792e4b052a65de85cbee71792beb82bad` | 1 | 4 | 0 | compute-buffer | 5->0 | 1..4->0..3 | - |
| `4bfe0838ecc4ac32bf0ff5599142a6e483c34f4994af90b11bfa31a6078d2394` | 2 | 10 | 1 | compute-sample+buffer | 12->0 13->1 | 2..11->0..9 | 1->18 |
| `5830726ffb8e1e9cd3638996034cbba8c2eee3d1990f8220b7affaf2f9a8ddc2` | 2 | 10 | 1 | compute-sample+buffer | 12->0 13->1 | 2..11->0..9 | 1->18 |
| `5987a1ee3042c621f2d27e4ddb91341d931b781ea82afaac2ec92c8a11bb9ec3` | 2 | 1 | 0 | compute-buffer | 2->0 3->1 | 1->0 | - |
| `5aa34654957442964fa794fd581ab29dc1069bad9f02775788580c6da673368d` | 2 | 9 | 1 | compute-sample+buffer | 11->0 12->1 | 2..10->0..8 | 1->18 |
| `6258b3e6a2a6dbf28f7c39dca543033a7368b3a972234b1bc4c0ca5cb93c2780` | 0 | 1 | 0 | compute-buffer | - | 1->0 | - |
| `65d5662baba025d8e892f2b8b011eb981ddb98acb8e849adb6912fa60e51779f` | 2 | 9 | 0 | compute-buffer | 10->0 11->1 | 1..9->0..8 | - |
| `70fb1c9bee41c75207bf7fa1c6bf44e0569990c05711bcfee23d29615338e33f` | 2 | 9 | 0 | compute-buffer | 10->0 11->1 | 1..9->0..8 | - |
| `727341b9b66dc3f4a97e6bc691f74ab0a9d214167ed35a0ac9aa6d3084618f65` | 2 | 10 | 2 | compute-sample+buffer | 13->0 14->1 | 3..12->0..9 | 1->32 2->42 |
| `73e65f7ef16d1e60b1fb007aa29eb6a8ca48c2372feb401cadd8610d05abe3ca` | 2 | 10 | 2 | compute-sample+buffer | 13->0 14->1 | 3..12->0..9 | 1->22 2->32 |
| `80ff876ee44991ba07964634b93af8050ecd96fbd77113af0ac7b8d4fe6a93db` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `81ad85bc8530ad5066995759d74cea2b35685d0dbba91941f27abbf2cac23cf4` | 2 | 10 | 2 | compute-sample+buffer | 13->0 14->1 | 3..12->0..9 | 1->18 2->20 |
| `84fd96f0f4fad477da64369cb757c5b35e4e94676e5d53d22896bbd8100acbc8` | 2 | 9 | 3 | compute-sample+buffer | 13->0 14->1 | 4..12->0..8 | 1->18 2->22 3->42 |
| `85501247a0c60d10a6b91ca36678f4d8ca3a6db0452b886411a973ca0a6ef856` | 2 | 5 | 0 | compute-buffer | 6->0 7->1 | 1..5->0..4 | - |
| `8b45432c6beb4b5feaa6e9bac0736ff65dff0c1d9afbb5af40f731cda9d013e7` | 2 | 9 | 0 | compute-buffer | 10->0 11->1 | 1..9->0..8 | - |
| `8e62b35b119ba6a0fff646efae71d84f2058ca8e23edf0f4a8a3208c5e93af59` | 2 | 1 | 0 | compute-buffer | 2->3 3->0 | 1->0 | - |
| `91215cc24ab4f6d5e2a25e8b87b17f2ee3232b110866b39354567f15ae58783f` | 1 | 1 | 0 | compute-buffer | 2->0 | 1->0 | - |
| `9d4e8a086716348543b527345c2f4c108ea06f062da219904706ce84729302ca` | 2 | 11 | 1 | compute-sample+buffer | 13->0 14->1 | 2..12->0..10 | 1->20 |
| `9f3d1abe969c17515c4c2868fe7a4751a389a8a0a88774b0118434311e38df79` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `a17b0649345384e54a67bbddaec555a4464c030baabe8fa08eb979240feb668a` | 1 | 4 | 1 | compute-sample+buffer | 6->0 | 2..5->0..3 | 1->20 |
| `a6d534f31d47429d99d3224252862fc9424df712a557bde0e52c1ffdaa1fd6f7` | 2 | 6 | 1 | compute-sample+buffer | 8->0 9->1 | 2..7->0..5 | 1->18 |
| `ac493944e3a2b278612639b380696a9c22bee8451c512961d9cfd27db4934857` | 2 | 10 | 0 | compute-buffer | 11->0 12->1 | 1..10->0..9 | - |
| `ae6345d168ef518c2b7f5b0ece956e0cac8afa07df097736a7adda2b80a2dcfc` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `ae725063235b30c7b07e97e081a274e9718def6f816a1c5f9ea57ddd67a78039` | 1 | 1 | 0 | compute-buffer | 3->3 | 2->0 | - |
| `b4f9a69289a48ecceae0ee573726055a002eb7b0ce04535bae7220d70b3f84ca` | 2 | 9 | 0 | compute-buffer | 10->0 11->1 | 1..9->0..8 | - |
| `b5d3654ef41c60ebc7660f37c8d4bb4bbfb377b3f2266e241bdc6bc8a68cdc04` | 2 | 11 | 3 | compute-sample+buffer | 15->0 16->1 | 4..14->0..10 | 1->18 2->22 3->42 |
| `b88fb53a6722f87aa3be8b2a7f7d701841cea3b7ff441836179bc0b422ce5950` | 2 | 8 | 2 | compute-sample+buffer | 11->0 12->1 | 3..10->0..7 | 1->22 2->42 |
| `bc103cbf9596da72f6d89cc5f3a47ab15f294f45613782fba6ce7c5bbe8368dc` | 2 | 12 | 4 | compute-sample+buffer | 17->0 18->1 | 5..16->0..11 | 1->20 2->22 3->32 4->42 |
| `bc291d0e41a4ad0b06cf237df35379de85f79aa235cf4f0ac5889e1bb4fa04fd` | 2 | 10 | 0 | compute-buffer | 11->0 12->1 | 1..10->0..9 | - |
| `bd4bf756dcb82c78cc11971f85cc3be61b9e01fed1b6bcdbd7525c16e43db82e` | 2 | 9 | 0 | compute-buffer | 10->0 11->1 | 1..9->0..8 | - |
| `be7b289c560f8c56c7d1cc11b46ebc0dea30054a10731595a676edf639d17d33` | 2 | 9 | 1 | compute-sample+buffer | 11->0 12->1 | 2..10->0..8 | 1->18 |
| `c7830fac6bbcb5b71185855386386311d704f2d90ada33dec30d584777fcd6ad` | 0 | 0 | 1 | compute-sample | - | - | 1->8 |
| `cc4541044755b85b914f8c68eef1cbb11f7cef762bc701f82ef39f2e0339a93d` | 2 | 5 | 0 | compute-buffer | 6->0 7->1 | 1..5->0..4 | - |
| `ce4993a01b953efd33eb3c62c817b4067bd5549b986599ccc94739786633204e` | 2 | 1 | 1 | compute-sample+buffer | 3->0 4->3 | 2->0 | 1->8 |
| `d946082d551ed0a7b0cb7cbeb5f0a391b2af716a4df50f599404b983662e8632` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->18 |
| `db0984679ece41d3f04f350409c4d089d25b2547ffe71d301e8b7f49a9c55fac` | 1 | 1 | 0 | compute-buffer | 2->3 | 1->0 | - |
| `ddfbef8c960282d3fbdb08b303abfc15caf3b9ecdea2c8e5b6674e2ecca27713` | 2 | 7 | 1 | compute-sample+buffer | 9->0 10->1 | 2..8->0..6 | 1->18 |
| `de4e7a4915b5d3c797f3a5ad33ddaaab9105e409ed0b5c5992fb13e9e1e148be` | 2 | 11 | 0 | compute-buffer | 12->0 13->1 | 1..11->0..10 | - |
| `df4e53fe03b1c5739a5fc61494fb4f8989122ab96368e4f502559bb7bae3bd76` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `e3395bc35410e5e5645bbb3cf0dd306a898532ab17a90695612a94c29f1f1444` | 2 | 7 | 1 | compute-sample+buffer | 9->0 10->1 | 2..8->0..6 | 1->18 |
| `e63bf8f464e6ed00024c69c71df82f10754f6b1e9bba51a988327d121f63340f` | 2 | 9 | 0 | compute-buffer | 10->0 11->1 | 1..9->0..8 | - |
| `eb6b12581a845793bd98e553778f43028bcacfb2f0e0e310b563c3a10867abcb` | 2 | 8 | 1 | compute-sample+buffer | 10->0 11->1 | 2..9->0..7 | 1->20 |
| `f0bfc813b82e905c689e61d26c24cb9bd40c5c01c07233ff8b2b1d92f760c5ae` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `f5483dd06e51ccda436ae896279eae36c3b420f43dc50eda23e8f107286399b9` | 2 | 5 | 0 | compute-buffer | 6->0 7->1 | 1..5->0..4 | - |
| `f913fcdcf69d92db256bd9c49426424448ac10bc1c20c9ae2ad7fda73cc0bf3f` | 2 | 8 | 0 | compute-buffer | 9->0 10->1 | 1..8->0..7 | - |
| `fc7ac155afd3ecd4c5cc60392af27974c4a18aacd9093557826239661cb0b44a` | 2 | 9 | 1 | compute-sample+buffer | 11->0 12->1 | 2..10->0..8 | 1->18 |

Variants (all four are present with matching canonical parents):

| variant file | reflection |
| --- | --- |
| `5830726ffb8e...-t18_3d` | same as `5830726ffb8e...` (2/10/1, tex 1->18) |
| `c7830fac6bbc...-t8_2d` | same as `c7830fac6bbc...` (0/0/1, tex 1->8) |
| `ce4993a01b95...-t8_2d` | same as `ce4993a01b95...` (2/1/1, tex 1->8) |
| `d946082d551e...-t18_3d` | same as `d946082d551e...` (2/8/1, tex 1->18) |

## 3. UI coverage and missing keys

Dumps inspected:

| dump | vs | ps | cs | total |
| --- | --- | --- | --- | --- |
| `shader-dump-ui` | 114 | 87 | 2 | 203 |
| `shader-dump-ui3` | 145 | 163 | 12 | 320 |
| `missdump` | 1,629 | 969 | 60 | 2,658 |
| `missdump5` | 1 | 0 | 0 | 1 |
| `missdump8` | 8 | 8 | 0 | 16 |
| unique keys | 1,897 | 1,227 | 74 | 3,198 |

Result: **no missing keys**.  Every dumped key is present as a canonical
`<stage>/<key>.spv` in the live cache.  `d946...` is the only canonical `cs`
module in the cache that was not captured in the dump directories still on
disk; it came from an earlier dump and is present with its `t18_3d` variant.

Historical `shader cache MISS` lines in the lead logs:

- 1,488 MISS lines, 280 unique log lines and 113 unique `(stage, key)` pairs
  (vs 63 / ps 49 / cs 1; the only compute miss ever logged is `d946...`);
- all 113 unique keys are now present in the cache;

The one compute miss line was:

```text
[totk-nvn] shader cache MISS #25 cs d946082d551ed0a7b0cb7cbeb5f0a391b2af716a4df50f599404b983662e8632 code=13312 root=/data/local/tmp/totk-android/lanes/shader/shader-cache
```

It is now a hit; the canonical module and its `t18_3d` variant are both in
`cs/`.
- the latest UI run (`/data/local/tmp/totk-android/lanes/lead/ui4.log`) has
  16 `shader cache hit` lines and 0 MISS lines.

So the cache covers the UI path's requested modules.  The remaining UI
blocker is not a cache miss.

## 4. Copy / clear / blit

The UI path also issues `nvnCommandBufferClearTexture`,
`nvnCommandBufferCopyBufferToTexture`, `nvnCommandBufferCopyTextureToTexture`
and `nvnCommandBufferBlit`.  These are **not shader modules** and there are no
cache keys for them:

- no `cs`, `vs` or `ps` `.res.json` in the cache declares a storage image
  (`images` is empty for all 5,810 reflections);
- the four NVN entry points are still `nvn_zero_call()` stubs in
  `games/totk/android/nvn_android.c` (lines ~9927-9958), so they never reach
  the backend;
- the UI compute modules are buffer/sampled-texture modules, so they cannot
  write the `0x3f` target directly.

Filling the cache cannot make copy/clear/blit work.  That is a Lane 2
(`nvn_android.c`) blocker: implement the shims on top of the frozen
`gfx_backend_texture_copy/clear` API from Lane 1, then re-run the UI capture.
Reported here; this lane did not edit `nvn_android.c`.

## 5. Audit tool exit status

The command in the header exits `0` for the current cache:

```text
modules=79 canonical=75 variants=4 valid=79 reflections=79
storage_image_modules=0 missing=0 problems=0
  cs: canonical=75 variants=4 ubo=146 ssbo=566 sampled=58 images=0
```

A non-zero exit means a missing canonical key, a bad SPIR-V header, a missing
`.res.json` (unless `--no-verify`) or a `res.json.key` mismatch.  The tool never
writes to the cache.

# M5 浜ゆ帴锛氫粠杩欓噷寮€濮?

**鏈€鍚庢洿鏂帮細2026-10-26**锛堢姸鎬佹湁鍙樺寲鍏堟敼杩欓噷锛屽啀鏀?tasklist锛?

---

## 1. 涓€鍒嗛挓鐜扮姸

| 椤?| 鍊?|
|---|---|
| **鍞竴娲诲姩绾?* | 鍒嗘敮 `feature/m5-lifecycle`锛屽伐浣滄爲 `.worktrees/m5-lifecycle`锛圚EAD 浠?`git log` 涓哄噯锛屾湰鏂囦笉閽?SHA锛?|
| 鍩虹嚎 | `main` = `cb3df08`锛堝共鍑€ M4锛夛紝**淇濇寔涓嶅姩** |
| 鍗曠洏锛圙2/G3锛?| T1-01 / T1-02 / T1-03 / T1-04 / T1-11 **PASSED**锛?*T1-05 鍙垽 PASSED**锛坄cli_e2e -Repeat 20 鈫?20/20`锛屾湰鏂?搂3.14锛?|
| 澶氱洏锛圙4锛?| T2-01鈥2-03 **鏁寸珷寤跺悗**锛?026-10-17 鐢ㄦ埛瑁佸畾锛?|
| 浜у搧闈㈠鏈鸿鏁?| 瀹屾暣 154 鏉?鈫?**151/154**锛涗袱涓槸瀹㈡満鏃?Visual Studio 鐨?structural锛屼竴涓槸鎵瑰唴闂村彂锛堝崟璺?3/3 閫氳繃锛夈€傝 搂3.14 鈶?|
| **涓嬩竴浠朵簨** | T1-06锛堜腑鏂彁浜ゅ垽瀹氬彲杈撅級銆俿top/restart 鐨勬敹灏惧凡鍦?搂3.14 瀹屾垚 |

**椤哄簭**锛歍1-05 鏀跺熬锛?*宸插畬鎴愶紝搂3.14**锛夆啋 T1-06 鈫?T1-07 鈫?T1-08 鈫?T1-09 鈫?T1-10 鈫?T3锛圙UI锛夆啋 T4锛堜骇鍝佸寲锛夈€?
T2 鍦?G4 鎭㈠鍓嶄笉鍔ㄣ€?

---

## 2. 鏂囨。鍦板浘锛堟寜"浣犳兂鐭ラ亾浠€涔?锛?

| 鎯崇煡閬?| 璇昏繖浠?|
|---|---|
| **鐜板湪鍦ㄥ摢銆佷笅涓€姝ュ仛浠€涔?* | **鏈枃** |
| 姣忎釜澧為噺鐨勭姸鎬?| `docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`锛?*鍞竴鐘舵€佹潵婧?*锛?|
| 椤哄簭銆佷緷璧栥€侀獙鏀跺彛寰勩€侀闄┿€佷綍鏃跺仠涓嬮棶 | `docs/superpowers/plans/2026-10-15-m5-master-plan.md` |
| 璁捐涓庝笉鍙橀噺 | `docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md` |
| 鏌愪釜澧為噺鍑粈涔堢畻閫氳繃 | `docs/validation/`锛堟寜澧為噺鍚嶅鍙凤級 |
| **宸茬粡浠樿繃瀛﹁垂鐨勫潙** | `docs/operations/m5-lessons.md`锛?*鍔ㄦ墜鍓嶆壂涓€閬?*锛?|
| 2026-10-26 閭ｆ璇勫鐨勭粨璁轰笌淇硶 | `docs/operations/m5-review-2026-10-26.md`锛堟椂鐐瑰揩鐓э紝鍐欐椂鍥哄畾锛?|
| VM 鍥炶矾鐨勪笁鏉￠€氶亾涓庝竴娆℃€ч厤缃?| `docs/operations/m5-vm-harness.md` |
| 寮曟搸灞傦紙M3/M4锛夋€庝箞璺?| `docs/operations/m3-persistence.md`銆乣docs/testing/m3-e2e.md` |

**鏂囨。绾緥**锛氱姸鎬佸彧鍐欏湪 tasklist锛涙湰鏂囧啓"涓嬩竴姝?锛沵aster-plan 涓嶅啀鍐?褰撳墠浣嶇疆"銆?
浠讳綍涓€澶勪笌鍓嶄袱澶勫啿绐侊紝浠?tasklist 鈫?鏈枃 鈫?master-plan 鐨勯『搴忓彇鑸嶃€?

---

## 3. 褰撳墠澧為噺 T1-05锛歚stop` 宸蹭慨锛岄獙鏀惰 `create` 鐨勬柊澶辫触鎸′綇

### 3.1 宸茬粡閫氱殑閭ｄ竴娈碉紙瀹㈡満璇绘暟锛?026-10-23锛岃姹?`32d4ba1c71b54d72a87c4d4e6cd33203`锛?

```text
disk.create:     queued 鈫?running 鈫?鈥?鈫?publish-ready 鈫?assign-mount 鈫?readiness-io
disk.checkpoint: queued 鈫?running 鈫?succeeded (terminal)
```

`create` 鍏ㄩ摼銆乣list`銆乣show`銆佽鍐欍€乣checkpoint` 閮介€氳繃浜嗏€斺€?*杩欎簺姝ゅ墠浠庢湭璺戦€氳繃**銆?
璇︾粏杩囩▼涓庡叚涓鎺ㄧ炕鐨勫亣璁捐 `docs/validation/m5-t1-05-cli.md` 搂8鈥撀?0銆?

### 3.2 鍘熸潵鏄繖閲屽崱浣忥紙2026-10-26 宸蹭慨锛宍d5b9ad7`锛?

```text
execute_operation failed: code=22022 step=service_core.checkpoint_epoch
  detail=The final checkpoint state is invalid
```

浣嶇疆锛歚src/management/service_core.cpp:5753`锛屽湪 checkpoint 纭鍥炶皟閲?
鐢?`complete_command(pending_plan, succeeded_clean|succeeded_dirty)` 鍘?*瀹屾垚鏁翠釜 stop 璁″垝**銆?

璇讳唬鐮佺‘璁ょ殑涓や釜浜嬪疄锛?

1. **`stop` 涓嶆帴鍙?`succeeded_dirty`銆?*
   `src/management/state_machine.cpp:740` 鐨?`stop` 鍒嗘敮鍙
   `succeeded_clean` / `transient_failure` / `persistence_blocked` /
   `commit_indeterminate` / `committed_needs_reconciliation_*`锛?
   `succeeded_dirty` 钀藉埌 `break` 鈬?`invalid_completion` 鈬?灏辨槸涓婇潰杩欐潯 22022銆?
2. **杩欎竴姝ユ湰鏉ヤ篃涓嶈瀹屾垚鏁翠釜鍛戒护銆?*
   stop 璁″垝鍦?checkpoint 涔嬪悗杩樻湁 `remove_mount` / `remove_exact_device` /
   `stop_runtime_and_release_lock`锛涚粓鐐瑰湪 `finalize_operation`锛坄service_core.cpp:6795`锛夈€?

### 3.3 涓嬩竴姝ワ紙鎸夐『搴忥紝涓嶈璺筹級

1. **鏍瑰洜宸叉煡鏄庯紙2026-10-26锛夛細dirty epoch 鏄竴鏉′袱绔兘绌虹潃鐨勭閬擄紝涓嶆槸鍒ゆ嵁鍐欓敊銆?*
   - worker 渚х殑 `cut_dirty_epoch` 鍙栫殑鏄?`maintenance_stats().generation`
     锛坆acking generation锛宍m5_worker_runtime.cpp:519`锛夛紱
   - service 渚х殑 `live_dirty_epoch` 鏄?*鍐欐鐨?0**锛坄service_worker_control.cpp` 鐨?
     `publish_checkpoint_epoch`锛夛紱
   - `WorkerRuntimeStatus.dirty_epoch` 鍏ㄤ粨**娌℃湁鐢熶骇鑰?*锛屾亽涓?0銆?
   鈬?`clean = (0 == generation)`锛歡eneration 0 鏃舵垚绔嬶紙create 鐨勭涓€澶?checkpoint锛夛紝
   涔嬪悗姘镐笉鎴愮珛锛坰top锛夈€?*杩欐槸纭畾鎬х殑锛屼笉鏄棿鍙戙€?*
   瀹屾暣璇佹嵁閾句笌浜旀淇硶瑙?[`m5-review-2026-10-26.md`](m5-review-2026-10-26.md) 搂1銆?
   **涓嶈鍐嶅線"姣旇緝椤哄簭"涓婅皟鍙?*鈥斺€擿dbb25a3` 宸茬粡璇佹槑閭ｆ潯璺彧鑳借 create 鐪嬭捣鏉ュソ杞€?
2. **`normal_stop` 鏃朵笉瑕佸湪杩欎竴姝?`complete_command`**锛氬彧鍙戝竷"checkpoint 宸茬‘璁?鐨?
   durable 鐘舵€侊紙`phase = stable`銆乣disposition = committed`銆?
   `confirmed_generation`銆乣current_dirty_epoch`锛夛紝璁?remove 绯诲垪缁х画锛?
   鐢?`finalize_operation` 鐢?stop 鐨勭湡瀹?outcome 鏀跺熬銆?
3. **涓嶈鎶?鎺ュ彈 dirty"褰撴垚淇硶锛?026-10-26 鏇存锛?*锛氬啓鍐荤粨鎺掑湪 checkpoint 涔嬪墠锛?
   "鎻愪氦鍚庤繕鏈夎剰鍧?鎰忓懗鐫€**鍐荤粨婕忎簡鍐?*鈥斺€旈偅鏄己闄凤紝鎶ユ垚鍔熺瓑浜庢妸鏁版嵁涓㈠け浼鎴愭甯告敹灏俱€?
   宸叉寜姝ゅ疄鐜板苟鍔犳ā鍨嬬敤渚嬶細clean 鎴愬姛銆乨irty 涓?applied銆?
4. **楠屾敹**锛堝鏈猴級锛歚disk stop` 鈫?鐩樼娑堝け 鈫?閲嶅惎鏈嶅姟 鈫?`disk start` 鎭㈠鍒版渶鍚?
   generation 鈫?`disk forget`銆備慨澶嶇被缁撹**鑷冲皯 `-Repeat 20`**銆?

### 3.4 椤哄甫璁颁笅鐨勪袱涓彲鐢ㄦ€х己鍙ｏ紙涓嶉樆濉?T1-05锛?

- `disk show/stop/checkpoint` 鐨?`<disk>` 鍙傛暟鍙 display name 鎴?uuid锛?*涓嶈鐩樼**锛?
  鑰岀敤鎴蜂細鑷劧鎯崇敤鐩樼锛堟湰杞帓鏌ュ氨鎾炰笂杩?`profile_not_found`锛夛紱
- 澶辫触鍘熷洜鐨勫彲杈炬€у凡琛ワ紙`m5-t1-05-cli.md` 搂5.2锛夛紝浣?`OperationRecord` 浠嶅彧瀛樻爣璁颁覆銆?

### 3.5 2026-10-26 鏅氾細create 閫氫簡锛宻top 涔熼€氫簡锛涚幇鍦ㄦ尅鍦?`forget`

**create 鐨勬敞鍐屽け璐ュ凡淇紙`2e8cf32`锛?*锛歚ensure_profile_unique` 浼氭墦寮€**姣忎竴涓叾瀹?profile** 鐨?
bundle 鍘绘瘮 file identity锛屽苟鎶婃墦寮€澶辫触鍘熸牱涓婃姏銆傚鏈轰笂鏈変竴涓?01:59 鐣欎笅鐨?profile
锛坄c084bc42`锛宐undle 宸茶涓婁竴杞?teardown 鍒犳帀锛夛紝浜庢槸**涔嬪悗姣忎竴娆?create 閮藉け璐?*锛?
鑰岄敊璇噷鎶ョ殑鏄?*鏂?bundle** 鐨勮矾寰勩€傚洓娆¤繍琛岀殑澶辫触璺緞瀹屽叏鐩稿悓锛坄b6707f16鈥锛夛紝
鑰屾柊 bundle 褰撴椂纭疄瀛樺湪銆佺敤鎴疯繘绋嬬敤鍚屾牱鐨?flags 鑳芥墦寮€鈥斺€旇繖灏辨槸鍒ゆ嵁銆?
鐜板湪鏀规垚锛氭墦涓嶅紑鐨勫叾瀹?bundle 涓嶅彲鑳界瓑浜庢鍦ㄦ敞鍐岀殑閭ｄ釜锛坕dentity 鐩哥瓑瑕佷袱杈归兘鎵撳紑锛夛紝
鍓嶅洓椤规鏌ワ紙鍚嶅瓧 / image / 璺緞 / 鎸傝浇鐐癸級宸茬粡瑕嗙洊鐪熷疄鍐茬獊锛屾墍浠?`continue`銆?

**璇诲埌鐨勯摼璺紙鍚屼竴娆¤繍琛岋紝`disk.list` 鍋滃悗浠嶅彲瑙侊級**锛?

```text
create 鈫?list 鈫?show 鈫?璇诲啓 73 瀛楄妭 鈫?checkpoint succeeded 鈫?stop succeeded
鍋滃悗锛歏: ready stopped/configured runtime=Stopped persistence=Clean generation=4
```

**鐜板湪鎸″湪 `disk forget`**锛?

```text
kind=7 command=disk.forget phase=terminal
  message=terminal failure without a worker-action error; marker=unsafe
```

`ServiceWorkerControl` **娌℃湁瀹炵幇** `prove_forget_safe`锛岃蛋鐨勬槸 `IWorkerControl` 鐨勯粯璁ゅ疄鐜?
锛坄service_core.cpp:1390`锛岀洿鎺ヨ繑鍥?"The worker control does not expose forget safety proof"锛夈€?
鑰?worker 鍗忚閲?*娌℃湁浠讳綍 forget 鐩稿叧鐨勫懡浠?*锛坄m5_worker_protocol.h` 鍏ㄦ枃鏃?forget锛夈€?
鈬?**`disk forget` 鏄湭瀹炵幇锛屼笉鏄洖褰?*锛沠ail-closed 鐨勮涔夎瀹冪湅璧锋潵"瀹夊叏鍦版嫆缁?锛?
鎵€浠ヤ竴鐩存病琚彂鐜般€?

**涓轰粈涔堜笉鑳界畝鍗曞湪鏈嶅姟閲岃ˉ**锛歚myrd_management` 鍙摼 profile/product_ipc/contracts/
worker_protocol/common锛坄src/CMakeLists.txt:372`锛夛紝鎷夸笉鍒?writer lock锛坧ersistence锛夊拰
AIM 璁惧锛坉river锛夈€俻roof 鐨勪簲椤归噷鏈変袱椤瑰繀椤荤敱 worker 鏉ョ瓟 鈬?瑕佷箞鍔犱竴涓?worker 鍛戒护锛?
瑕佷箞鍏堝仛 T1-06 鐨勫彧璇绘鏌?worker銆?

**stop 鐨勯噸澶嶈鏁帮紙2026-10-26 鏅氾級**锛歚m5.production_execution_backend`锛堝惈 stop 璺緞锛塦-Repeat 20` 鈫?**20/20 閫氳繃**锛岃姹?`a21cf2191a9d4b10bab4727e43a7737a`銆傝繖鏄?stop 淇鐨勭ǔ瀹氳瘉鎹紱`cli_e2e` 鏁翠綋浠嶅崱鍦?forget銆?

**涓嬩竴姝ワ紙鎸夐『搴忥級**锛氣憼 缁?worker 鍗忚鍔犱竴涓彧璇荤殑 forget 璇佹槑鍛戒护锛坆undle 瀛樺湪 + file identity銆?
writer lock 鍙彇銆佸尮閰嶈澶囦笉瀛樺湪锛夛紱鈶?鏈嶅姟渚у疄鐜?`prove_forget_safe` 骞舵帴鍒?tombstone 鏍￠獙锛?
鈶?瀹㈡満璺?`-Repeat 20`銆?
---

### 3.6 2026-10-26 鏅氾細forget 閫氫簡锛岀幇鍦ㄦ尅鍦?閲嶅惎"

**forget 宸插疄鐜帮紙`c1ac06f`锛?*锛歸orker 鍦?`inspect_bundle` 閲岃瘉鏄?writer lock 鍙彇 + 娌℃湁鍚屽昂瀵?
浠ｇ悊璁惧"锛屾湇鍔′晶 `prove_forget_safe` 鐢ㄤ竴娆?inspection 鎷胯繖涓ら」骞剁瓑杩涚▼閫€鍑恒€?
瀹㈡満璇绘暟锛歚disk.forget: succeeded (phase terminal)`銆?

**e2e 鐜板湪杩囦簡鏁存潯鏁版嵁闈?*锛歝reate 鈫?list/show 鈫?璇诲啓 鈫?checkpoint 鈫?stop 鈫?forget 鍏ㄩ€氳繃锛?
澶辫触鐐圭Щ鍒?**installed service** 闃舵锛?

```text
installed service state: STOPPED   WIN32_EXIT_CODE 1066
sc.exe stop MyRamDiskService failed (1062): 鏈嶅姟灏氭湭鍚姩
```

**鏍瑰洜锛堝凡澶嶇幇锛?*锛氫竴娆?forget 涔嬪悗 `C:\ProgramData\MyRamDisk\profiles\<id>\` 鐣欎笅**绌虹洰褰?*锛?
`profiles\workers\<id>.workers` 涔熺暀鐫€锛?*涓嬩竴娆℃湇鍔″惎鍔ㄧ洿鎺ュけ璐?*锛?

```text
myrd-service: start failed: op=profile_registration.load
  msg=The requested profile is not registered native=0
```

绌虹洰褰?`list_profiles` 浼氳烦杩囷紝鎵€浠ュ珜鐤戝湪 worker binding / 鍚姩鍗忚皟璺緞銆?
**涓嬩竴姝?*锛氣憼 forget 杩炵洰褰曚笌 binding 涓€璧锋竻鎺夛紙娌绘簮澶达級锛涒憽 鍚姩璺緞鎶?宸?forget 鐨?profile"
褰撲綔鍙洖鏀舵畫鐣欒€屼笉鏄嚧鍛介敊璇紙宕╂簝涓€斾篃瀹夊叏锛夛紱鈶?鐒跺悗璺?`cli_e2e -Repeat 20`銆?

---


### 3.7 2026-10-26 鏅氾細cli_e2e 棣栨鏁村閫氳繃锛涢噸澶?20 娆℃毚闇?绗?5 娆″紑濮嬪崱鍦?reconciling"

**鍗曟閫氳繃锛堢涓€娆★級**锛歚integration.m5.cli_e2e` 鈫?**1 of 1 passed**锛岃姹?
`25e8077d130d42fdbe8c12d8e6b40509`銆傝鐩?create 鈫?list/show 鈫?璇诲啓 鈫?checkpoint 鈫?
stop 鈫?娉ㄥ唽浠嶅湪 鈫?forget 鈫?installed service锛圫CM 瀹夎/搴旂瓟/鍋滄/鍒犻櫎锛夈€?

**`-Repeat 20`锛?/20**锛堣姹?`8446da51fcaf43c0a36c0d0e240f73cf`锛夈€傚墠 4 娆￠€氳繃锛?
浠庣 5 娆¤捣鍏ㄩ儴澶辫触锛屽け璐ヨ鏁颁竴鑷达細

```text
FAIL: the operation did not reach a terminal state within 300 s (last state: reconciling)
WARNING: 1 worker process(es) survived the run
```

鍗?**stop 杩涘叆 reconciling 鍚?300 s 娌℃湁鍑烘潵**锛屽苟涓旂暀涓嬩竴涓?worker 杩涚▼锛涙鍚庢瘡娆¤繍琛岄兘琚?
杩欎釜娈嬬暀鐘舵€佹尅浣忋€傝繖鏄?*绱Н鍨嬬己闄?*锛屽崟娆￠€氳繃骞朵笉鑳戒唬琛ㄧǔ瀹氥€?

**宸茬粡淇帀鐨勬祴璇曚晶缂洪櫡**锛堣繖浜涙浘鎶婁骇鍝佺己闄蜂吉瑁呮垚娴嬭瘯澶辫触锛屾垨鍙嶄箣锛夛細
1. 瀹夎鏈嶅姟鍓嶆湭鍋滃墠鍙板疄渚嬶紙绠￠亾鍐茬獊锛孲CM 7024锛夛紱
2. stop 鐢?`--wait`锛堣蛋宸茬煡鍧忔帀鐨?`operation wait`锛夛紱
3. 鐢?`disk list` 閲?*绗竴涓?UUID** 褰撲綔鏈疆 profile锛堝湪鏈夋畫鐣欐椂鎵撻敊鐩橈級銆?

**宸茬粡淇帀鐨勪骇鍝佺己闄?*锛氬鍎?profile 鎷栨 create锛坄2e8cf32`锛夈€佸惎鍔ㄦ湡缁堟€佹搷浣滅己 profile 鍗宠嚧鍛?
锛坄6d70940`锛屾寜 profile 璞佸厤锛夈€乫orget 瀹屾垚鏃舵嫆缁?bundle 宸蹭笉瀛樺湪"锛坄c11f6d2`锛夈€?
forget 鏈韩鏈疄鐜帮紙`c1ac06f`锛夈€?

**涓嬩竴姝?*锛氭煡绗?5 娆￠偅浠?`last-service.log`/`last-trace.log` 閲?stop 涓轰粈涔堝仠鍦?reconciling銆?
閭ｄ釜 worker 涓轰粈涔堟病閫€锛堝緢鍙兘鏄澶囬€€褰圭獥鍙ｅ唴鐨勯樆濉烇紝鎴?reconciliation 鐨勫垽瀹氭潯浠舵案杩滀笉婊¤冻锛夈€?

---

### 3.8 鍐冲畾锛歴top 鐨?鏈€缁?checkpoint 涓?dirty"瑕佹湁鐣屽妫€锛屼笉 park

**璇绘暟**锛歚cli_e2e -Repeat 20` = **18/20**锛堣姹?`98e28d87d1ff46ea9a8905aa755cc935`锛夛紝
涓や釜澶辫触锛?2銆?14锛夊悓鍥狅細live stop 鍋滃湪 `reconciling` 300 s锛岀暀涓€涓?worker銆?

**鏍瑰洜鏄袱涓畧鍗簰鐩哥煕鐩?*锛?
1. stop 鐨勬渶缁?checkpoint 鍒?dirty 鈬?`resolving` + needs-attention锛?
2. `remove_and_stop` 鎷掔粷 dirty runtime锛堟€曟媶鍗告椂涓㈡湭鎻愪氦鐨勫啓锛夛紱
3. 鎷掔粷鍚?`park_stop_reconciliation` 鈬?`reconciling`锛岃€屽畠鍙兘闈犱笅涓€娆℃湇鍔″惎鍔ㄦ帹杩涖€?

**鍐冲畾锛堥€夐」 2锛屾湁鐣屽妫€锛?*锛?
鏈€缁?checkpoint 鍒?dirty 鏃讹紝**鍐?flush + checkpoint 涓€娆★紙鏈€澶?N=2锛?*锛?
骞插噣浜嗙户缁媶鍗革紱浠?dirty 鎵嶄互鍏峰悕澶辫触 + needs-attention **缁堟€?*鏀跺熬锛?*涓?park**銆?
涓嶆媶鎺変换浣曞畧鍗紝涔熶笉鏀瑰彉"鍏佽涓?checkpoint 涔嬪悗鐨勫啓"杩欐潯鏃㈡湁濂戠害銆?

**瀹炵幇钀界偣锛堟寜椤哄簭锛?*锛?
1. `service_core.cpp` 鐨?stop checkpoint 鍒嗘敮锛氭妸銆宒irty 鈬?resolving銆嶆敼鎴?
   銆宒irty 鈬?鍐嶆墽琛屼竴娆?`flush_exact_volume` + checkpoint锛涗粛 dirty 鎵?resolving銆嶏紱
2. `park_stop_reconciliation` 鐨勮皟鐢ㄧ偣锛氭敼涓虹粓鎬佸け璐ワ紙`OperationState::failed`锛?
   + 鍏峰悕鏍囪锛堝 `stop-final-checkpoint-dirty`锛夛紝涓嶅啀杩涘叆 `reconciling`锛?
3. 楠屾敹锛氬厛 `-Repeat 3`锛堝揩锛夛紝鍐?`-Repeat 20`銆?0/20 鎵嶇畻鐩爣杈炬垚銆?

**涓嶈**鍋氱殑浜嬶細涓嶈涓轰簡閫氳繃鑰岃 `remove_and_stop` 鎺ュ彈 dirty鈥斺€旈偅鏄敮涓€鍦ㄩ槻"鎷嗙洏鍗充涪鏁版嵁"鐨勫畧鍗€?

---

### 3.9 2026-10-26锛歚cli_e2e -Repeat 20 鈫?20/20`锛堢涓€娆★級

```text
integration.m5.cli_e2e  -Repeat 20  鈫? 20 of 20 passed
璇锋眰 6d8dd60cf5064f6d85a21a3ef7065112
```

瑕嗙洊锛歝reate 鈫?list/show 鈫?璇诲啓 鈫?checkpoint 鈫?stop 鈫?娉ㄥ唽浠嶅湪 鈫?forget 鈫?
installed service锛圫CM 瀹夎/搴旂瓟/鍋滄/鍒犻櫎锛夈€?

**绗簩娆″悓鏍?20/20**锛堣姹?`3c6697405728401fa19ab5d75e3bb93b`锛夆噿 **杩炵画涓ゆ 20/20锛?
鍚堣 40 娆¤繛缁€氳繃**銆?

**浠嶇劧璇氬疄璁板綍**锛氬湪杩欎箣鍓嶇殑涓よ疆鏄?18/20锛堝け璐ュ仠鍦?`reconciling`锛夛紝鑰屼笖浠?18/20 鍒?
20/20 涔嬮棿**鍙敼浜嗘祴璇曠殑璇婃柇杈撳嚭銆佹病鏈夋敼浜у搧浠ｇ爜**銆傛墍浠ラ偅涓棿鍙戠己闄?*娌℃湁琚牴娌?*锛?
鍙槸鍦ㄨ繖 40 娆￠噷娌″啀鍑虹幇銆傚畠鐜板湪鐨勫垽鎹凡缁忓氨浣嶏細涓嬫涓€鏃﹀鐜帮紝澶辫触淇℃伅浼氱洿鎺ュ甫鍑?
`service.log` 灏鹃儴涓?`step=`銆?

**杩欒疆娌℃湁鏂扮殑浜у搧鏀瑰姩**锛屽彧缁?`Wait-ForTerminal` 鍔犱簡"澶辫触鏃舵墦鍗版湇鍔℃棩蹇楀熬閮?
锛坄reconciling` 琚笁绉嶅け璐ュ叡鐢紝涓嶆墦鍗板氨鍙兘璇?瀹冩寕浜?锛夈€備篃灏辨槸璇达細18/20 鈫?20/20
涔嬮棿娌℃湁浠ｇ爜鍙樺寲锛?*杩欎釜闂村彂缂洪櫡浠嶆湭鏍规不**锛屽彧鏄繖娆℃病鍑虹幇銆?

---

### 3.10 **宸插畬鎴愶紙瑙?搂3.13锛?*锛歚disk start`锛坰top 涔嬪悗閲嶅惎锛夌殑鎺ョ嚎璁板綍鈥斺€斾繚鐣欎綔鍘熺悊鍙傝€冿紝涓嶈鍐嶆寜瀹冩柦宸?

**鐜拌薄**锛歚disk.start 鈫?create-runtime` 澶辫触锛宍code=21000 step=select_volume_for_disk`锛?
`seen=[C?@0 -?@0 -?@1 -?@0]`鈥斺€斿嵎鍦ㄧ鐩?1 涓娿€?*娌℃湁鐩樼**锛岃€岃鍑芥暟绗竴閬撹繃婊ゆ槸鐩樼銆?

**鏍瑰洜**锛歚find_recovered_volume` 鐨?GUID 鍒嗘敮锛坄windows_volume.cpp:685`锛夋槸瀵圭殑锛?
浣?*涓婃父浠庢潵娌″杺杩?GUID**锛屾墍浠ュ畠姘歌繙鏄?skip銆?

**宸茬粡鍋氬畬鐨勪竴灞?*锛坄798b0d1`锛夛細`WorkerLaunchSpec` 鏈?`volume_guid` 瀛楁 + 缂栬В鐮侊紝
`unit.m5.worker_protocol` 閫氳繃銆?

**杩樺墿浜斿锛堟寜椤哄簭锛?*锛?

1. `src/host/m5_worker_runtime.h`锛歚WorkerRuntimeConfig` 鍔?`std::wstring volume_guid;`
2. `src/host/m5_worker_main.cpp`锛堢害 175-180 琛岋紝`config.bundle = 鈥 閭ｄ竴娈碉級锛?
   `config.volume_guid = spec.value().volume_guid;`
3. `src/host/m5_worker_runtime.cpp`锛堢害 313-320 琛岀殑 `ProductionRuntimeAdapter::Config{鈥`锛夛細
   鎶?`volume_guid` 浼犲埌 `expected_volume_guid`銆?*娉ㄦ剰杩欐槸鑱氬悎鍒濆鍖栵紝瀛楁椤哄簭鏁忔劅**銆?
4. `src/management/service_worker_control.cpp` 鐨?`make_launch_request(const ProfileRecord&)`锛?
   浠?durable 璁板綍濉?`request.volume_guid`銆傝褰曢噷鐨勫瓧娈垫槸
   `std::optional<product::Uuid> volume_guid`锛坄profile_types.h:116`锛夛紝**鍙湁澶х被
   `{xxxxxxxx-鈥`锛屾病鏈夊墠缂€**銆?
5. **鏍煎紡闄烽槺锛堜細鐧借窇涓€杞紝鍔″繀娉ㄦ剰锛?*锛歚find_recovered_volume` 閲岀殑姣旇緝鏄?
   `_wcsicmp` + 鍙幓鎺夊熬閮ㄧ殑 `\`锛岃€屾灇涓惧嚭鏉ョ殑 `volume.volume_guid` 褰㈢姸鏄?
   `\\?\Volume{guid}\`銆傛墍浠ョ 4 姝ュ繀椤绘妸瀹冩嫾鎴?**`\\?\Volume{<uuid>}\`** 鍐嶄笂绾匡紝
   鍚﹀垯 GUID 鍒嗘敮浼氬尮閰嶅埌 0 涓€侀€€鍥炴寜鐩樼鎵俱€佺棁鐘朵笌鐜板湪涓€妯′竴鏍枫€?

**绗?13 杞ˉ鍏咃細瀹為檯鏄叚澶勶紝涓嶆槸浜斿銆?* 鏈嶅姟渚х殑璇锋眰缁撴瀯涔熻甯︿笂瀹冿紝鍚﹀垯 spec 姘歌繙鏄┖锛?

| # | 浣嶇疆 | 鍐呭 |
|---|---|---|
| 0 | `src/management/service_worker_control.h` | `WorkerLaunchRequest` 鍔?`std::wstring volume_guid` |
| 0b | `service_worker_control.cpp:118` / `:150` | 涓や釜 `make_launch_request` 閮藉～锛歞urable 閭ｆ潯浠?`profile.volume_guid` 鍙栵紝create/import 閭ｆ潯鐣欑┖ |
| 1 | `src/host/windows_worker_sessions.cpp:59` | `spec.volume_guid = request.volume_guid;` |

**宸叉湁鍏堜緥鍙洿鎺ユ妱**锛歚service_worker_control.cpp:467` 灏辨湁
`expected_volume = profile.volume_guid.value().to_string();`鈥斺€斿悓涓€涓?durable 瀛楁銆?
鍚屼竴绉嶅彇娉曘€傚埆蹇樹簡绗?5 鏉＄殑 `\\?\Volume{鈥\` 鍓嶇紑銆?

**楠屾敹**锛氬共鍑€瀹㈡満鍗曟 `cli_e2e`锛堝惈 restart锛夆啋 `-Repeat 20`銆?

---

### 3.11 2026-10-26 鏀跺熬锛歳estart 鐨勭幇鐘讹紙浜ゆ帴鐢紝鏈畬鎴愶級

**宸插仛瀹屽苟鎻愪氦**
- `798b0d1` / `99ff704`锛歚volume_guid` 浠?durable profile 涓€璺紶鍒?worker 鐨?
  `expected_volume_guid`锛? 澶勬帴绾匡紝鏍煎紡 `\\?\Volume{...}\`锛夈€?
- 瀹㈡満**姝ｉ潰璇佹嵁**锛歚verify_volume` 鎵撳嚭鐨?`volume_guid` 涓?`expected_guid` 涓€鑷淬€?
  `actual_disk=1 expected_disk=1` 鈥斺€?鍗锋槸**鎸夎韩浠?*璁ゅ嚭鏉ョ殑锛屼笉鍐嶉潬鐩樼銆?
- `stp`锛歴top 鍦?checkpoint 鏈В鍐虫椂鏀逛负**鍏峰悕缁堟€佸け璐?*锛屼笉 park锛堝彟涓€澶?`resolving`
  鏃╁墠宸叉敼锛夛紱`verify_volume` 鐨勮瘖鏂敼涓洪€愭潯鍒楀嚭姣忎釜鍙嫆缁濇潯浠躲€?
- `8b8e8fa`锛歟2e 澧炲姞 restart 鏍￠獙锛坰tart 鈫?鍚岀洏绗?鈫?璇诲洖 stop 鍓嶅啓鐨勬枃浠?鈫?鍐?stop锛夈€?

**鏈獙璇?/ 鏈В鍐?*
- restart **閫氳繃杩囦竴娆?*锛堣繍琛?`efbf62fe`锛屽叾鍚庣浜屾 stop 鍗″湪 `reconciling`锛夛紝
  浣嗕箣鍚庝袱娆″湪 `create-runtime` 鍙堝け璐ワ紱鏈€杩戜竴娆★紙骞插噣瀹㈡満 `2bbffea`锛変粛澶辫触涓旇€楁椂
  绾?5 鍒嗛挓锛屾寚鍚戝張涓€娆?300 s 绛夊緟銆?*缁撹锛歳estart 涓嶇ǔ瀹氾紝鏈揪鍒?T1-05 鐨勯獙鏀剁嚎銆?*
- 300 s 鐨?`reconciling` 绛夊緟浠嶄細鍑虹幇锛岃鏄庤繕鏈?`park` 钀界偣娌℃敼鎴愮粓鎬佸け璐?
  锛坄service_core.cpp:3950` 鐨勫洖婊氳矾寰勫皻鏈鐞嗭級銆?

**涓嬩竴姝ワ紙寤鸿鏂颁細璇濆紑澶村氨鍋氾級**
1. 纭噸鍚?+ 娓?`ProgramData\MyRamDisk\{profiles,operations}`锛?
2. 鍗曟杩愯骞?*瀹屾暣**璇昏 run 鐨?`stderr`锛堜笉鏄?tail锛夛紝纭澶辫触鏄?`create-runtime` 杩樻槸
   `reconciling`锛?
3. 鑻ユ槸 3950 鐨?park 鈫?鍚屾牱鏀逛负鍏峰悕缁堟€佸け璐ワ紱鑻ユ槸 `create-runtime` 鈫?璇?
   `Diagnostic verify_volume` 鐨勯€愭潯杈撳嚭瀹氫綅銆?

---

### 3.12 **宸茶 搂3.13 鍙栦唬**锛歴top 鐨?remove_mount 琚?runtime 浠嶇劧 dirty"鎷掔粷锛坄278d63e` 宸插鐞嗭級

**璇绘暟**锛坄run-fe0a8a2a` 鐨勬湇鍔℃棩蹇楋級锛?

```text
disk.stop 鈫?checkpoint completed 鈫?evidence-recorded profile state published
disk.stop 鈫?lifecycle-action failed
code=23559 step=service_worker_control.remove_mount
detail=The worker refused to remove: its runtime stands dirty
鈫?鎿嶄綔鍋滃湪 reconciling锛?00 s 瓒呮椂锛屾畫鐣?1 涓?worker
```

**鍚箟**锛氬喕缁?鈫?flush 鈫?checkpoint 鎻愪氦 鈫?鎷嗗嵏锛屽墠涓夋閮芥垚鍔燂紝绗洓姝ヨ鎷嗗嵏瀹堝崼鎷掔粷銆?
瀹堝崼鏈韩鏄鐨勶紙dirty 鏃舵媶鐩樼瓑浜庝涪鏁版嵁锛夛紱闂鏄?*娌℃湁浠讳綍璺緞鑳藉湪"鎻愪氦鍚庝粛鏈夋畫浣欏啓"鏃跺畨鍏ㄦ敹灏?*銆?

**淇硶锛堜袱澶勯€変竴锛屽缓璁?A锛?*

- **A. 鏈夌晫澶嶆**锛氭媶鍗歌鎷掍笖鍘熷洜涓?dirty 鏃讹紝鍐嶅仛涓€娆?`flush + checkpoint`锛岀劧鍚庨噸璇?
  `remove_mount` **涓€娆?*锛涚浜屾浠?dirty 鍒欎互鍏峰悕缁堟€佸け璐ユ敹灏?
  锛坄stop-dirty-after-recheckpoint`锛夛紝涓嶅啀 park銆?
- **B. worker 渚ц嚜鎰?*锛氳 `remove_and_stop` 鍦ㄥ垽瀹?dirty 涔嬪墠鍏?flush 涓€娆″苟閲嶆柊鍙?dirty锛?
  鏀瑰姩鏇村眬閮紝浣嗚纭 flush 涔嬪悗 dirty 璁℃暟纭疄浼氬綊闆躲€?

**钀界偣**锛歚src/management/service_core.cpp` 鐨勫姩浣滃け璐ュ垎鏀紙`remove_mount` 鐨勫け璐ュ鐞嗭級
锛?`src/host/m5_worker_runtime.cpp` 鐨?removal 璺緞銆傛敼瀹屽厛鍗曟锛屽啀 `-Repeat 20`銆?

**宸查獙璇侀€氳繃鐨勯儴鍒嗭紙鏈浼氳瘽鐨勫噣鏀剁泭锛?*锛歚volume_guid` 鍏眰鎺ョ嚎銆乣verify_volume` 鐨?
ownership 鍒ゆ嵁锛坅ttach-only 涓嶅啀瑕佹眰 owned锛夈€佹仮澶嶈矾寰勫厛鍒嗛厤鐩樼 鈥斺€?restart 宸茶兘浠?
`create-runtime` 涓€璺蛋鍒?閲嶅惎鍚庡啀娆?stop"銆?

---

### 3.13 褰撳墠鐪熷疄浣嶇疆锛?026-10-26 鏅氾紝鎺?搂3.12锛?

**restart 宸叉墦閫?*锛氫竴娆¤繍琛岀殑鏈嶅姟鏃ュ織鏄剧ず `disk.start` 鐨勫叓涓姩浣滃叏閮?
`action-result: completed` 骞跺埌杈?terminal锛?

```text
create-runtime 鈫?recover-bundle 鈫?start-proxy 鈫?create-device 鈫?
locate-physical-drive 鈫?verify-volume 鈫?assign-mount 鈫?readiness-io 鈫?terminal
```

**鍓╀笅鐨勫け璐?*锛氱揣鎺ョ潃鐨?*绗簩娆?stop** 浠ョ粓鎬佸け璐ョ粨鏉燂紙蹇€燂紝涓嶅啀鎸?300 绉掞級锛?

```text
kind=7 command=disk.stop phase=terminal
  message=terminal failure without a worker-action error; marker=flush
```

`marker` 鏄?`finalize_operation` 鏀跺埌鐨勭粓姝㈡爣璁帮紝鍐呭鎭板ソ鏄?*鍔ㄤ綔鍚?* `flush`鈥斺€斿嵆杩欐
stop 澶辫触鍦?`flush` 杩欎釜鍔ㄤ綔涓婏紝涓?*涓嶆槸 worker 鍔ㄤ綔杩斿洖鐨勯敊璇?*锛坄action-start: flush`
鐢氳嚦娌℃湁鍐欏嚭鏉ワ紝璇存槑澶辫触鍙戠敓鍦ㄥ姩浣滃惎鍔ㄩ樁娈碉紝渚嬪 runtime 鏈粦瀹氾級銆?

**搂3.12 鐨勭粨璁哄凡琚彇浠?*锛氫笉鍐嶆槸 `remove_mount` 鎷掔粷 dirty锛坄278d63e` 鐨?
flush-before-refusal 璁╅偅鏉′笉鍐嶈Е鍙戯級锛岀幇鍦ㄥ墠绉诲埌 flush 鏈韩銆?

**涓嬩竴姝?*锛氭煡 `service_core.cpp` 閲?鐢ㄥ姩浣滃悕褰?marker"鐨勯偅涓?finalize 璋冪敤鐐癸紝
纭绗簩娆?stop 鍦?flush 闃舵澶辫触鐨勫叿浣撳師鍥狅紙runtime 鏄惁宸茬粦瀹氥€佸嵎閿佹槸鍚﹀湪鎵嬶級锛?
淇畬鍐嶅崟娆?鈫?`-Repeat 20`銆?

---

### 3.14 2026-10-26 鏅氾細restart + 涓ゆ stop 鍏ㄩ€氾紝`cli_e2e -Repeat 20 鈫?20/20`

**楠屾敹璇绘暟锛堝共鍑€瀹㈡満锛?*锛歚integration.m5.cli_e2e` 脳 20 = **20/20**锛?
璇锋眰 `ad796d29c8c04ef58b71766fdaa28a3b`锛?0 娆″叡 5 鍒?50 绉掞紙12:07:48 鈫?12:13:38Z锛夈€?
姣忔杩唬閮借蛋瀹?create 鈫?checkpoint 鈫?stop 鈫?**restart** 鈫?**绗簩娆?stop** 鈫?forget 鈫?
瀹夎鏈嶅姟 install/answer/stop/delete銆傚鐓э細淇鍓嶅悓涓€鍛戒护 **18/20**銆?*17/20**锛屼袱娆′笉鍚屾壒娆°€?

#### 鈶?鍏堝姞璇婃柇锛堝彧鍔犳棩蹇楋紝涓嶆敼琛屼负锛?

`rollback_stop` 涔嬪墠鎶婂け璐ュ師鍥犲啓杩涙湇鍔℃棩蹇楋紱鍚屼竴涓?lambda 澶嶇敤鍒?quiesce / freeze /
flush / resume锛屽苟鍦ㄤ袱涓?park 鐐瑰悇鍐欎竴琛屻€傞娆″崟娆¤繍琛岀珛鍒昏鍑哄師鍥狅細

```text
kind=7 message=stop action failed: action=flush_exact_volume outcome=error
      domain=3 code=20999 native=0 step=flush detail=volume ownership contract is incomplete
```

`marker=flush` 鍙槸鎶?*鍔ㄤ綔鍚?*褰撴爣璁帮紙`finalize_operation` 鏀跺埌鐨勭粓姝㈡爣璁帮級锛?
閿欒鏈韩鍦?`rollback_stop({...})` 閲岃涓㈠純鈥斺€旇繖灏辨槸涓婁竴杞?鏃ュ織鍙墿 marker"鐨勫叏閮ㄥ師鍥犮€?

鍙﹀锛歟2e 姣忔杩唬鎶婅嚜宸辩殑 `service.log` / `trace.log` 鍙﹀瓨鍒?
`C:\m5\runs\<work-root>.{service,trace}.log`銆?*杩欎竴鏉℃槸蹇呴渶鐨?*锛歚last-*` 浼氳涓嬩竴娆?
杩唬瑕嗙洊锛?/20 閭ｆ澶辫触鏃跺彧鍓╀笅"鏈€鍚庝竴杞€氳繃"鐨勬棩蹇楋紝绛変簬娌℃湁璇佹嵁銆?

#### 鈶?鏍瑰洜涓€锛氭寜韬唤璁ゅ洖鐨勫嵎"涓嶅睘浜庤嚜宸?锛堝凡淇紝`windows_volume.cpp`锛?

`Win32VolumeAdapter::find_recovered_volume` 鍦?durable GUID 鍛戒腑鍞竴鍊欓€?鐨勫揩璺緞
鐩存帴杩斿洖瑙傛祴鍒扮殑鍗凤紝鑰?`enumerate_native_volumes` 缁欐瘡涓嵎閮芥墦
`operation_owned=false`锛坄windows_volume.cpp:490`锛夈€傚悓鍑芥暟鐨勫彟澶栦袱鏉￠€夋嫨璺緞
锛坄select_volume_for_disk`锛夐兘浼氳ˉ `owner_token` / `drive_letter` / `operation_owned=true`銆?

浜庢槸**鍙湁"閲嶅惎鍚庢寜韬唤璁ゅ洖鍗?杩欐潯璺?*鎷垮埌涓€涓棤涓荤殑鍗凤細`verify_native_volume_identity`
鐨勬墍鏈夋潈鍓嶇疆锛坄windows_volume.cpp:302`锛夊湪 flush 鏃舵嫆缁濄€傝€?`disk.create` 寤哄嚭鏉ョ殑鍗?
涓€鐩存槸鏈変富鐨勶紝鎵€浠?*绗竴娆?stop 姘歌繙姝ｅ父銆佺浜屾 stop 蹇呮寕**鈥斺€旇繖姝ｆ槸 搂3.13 鐨勭幇璞°€?

淇硶锛氬揩璺緞琛ヤ笂鍚屾牱鐨勪笁涓瓧娈碉紙涓嶅姩浠讳綍瀹堝崼锛夈€備慨瀹屽崟娆¤繍琛屽叏濂楅€氳繃
锛堣姹?`696c778500974694a68558a38b80c6cb`锛?4 绉掞級銆?

#### 鈶?鏍瑰洜浜岋細stop 鐨勫啓鍐荤粨浠庢湭杩愯杩囷紙T1-11 鐨勬鍒嗘敮锛屽凡淇級

20脳 闅忓嵆鏆撮湶绗簩灞傦細**18/20**锛屼袱娆￠兘鏄€?00 s 娌″埌缁堟€侊紝last state=reconciling銆嶃€?
璇婃柇杞?**17/20** 鎷垮埌浜嗗師鍥狅紝涓や釜绛惧悕锛?

```text
action=lifecycle-action code=23559 step=service_worker_control.remove_mount
  detail=The worker refused to remove: its runtime stands dirty
鈫?stop parked at the removal boundary: action=lifecycle-action stop_phase=5

checkpoint: disposition=3 鈥?           锛坈ommitted_needs_reconciliation锛?
鈫?checkpoint parked for reconciliation: phase=checkpoint-resolving
```

涓よ€呮槸鍚屼竴浠朵簨锛?*鏈€缁堟彁浜や箣鍚庡嵎浠嶅彲鍐?*锛屼簬鏄彁浜ゅ悗鍙堟湁 dirty銆傝€屽啓鍐荤粨
锛圱1-11锛宍b3fae6f` 寮曞叆锛夋湰璇ユ槸閭ｉ亾闂革細

```text
state_machine锛歴top 璁″垝 = quiesce_mutations, freeze_writes, flush_exact_volume, checkpoint_if_required, 鈥?
service_core锛歲uiesce 鍧楁妸 phase 缃负 quiesced 骞跺彂甯?
              绱ф帴鐫€鐨?freeze 鍧楀畧鍗啓鐨勬槸 `stop_state.phase == admitted`  鈬?姘镐笉鎴愮珛
              涔嬪悗 execution_actions.erase(begin, begin + 3) 浠嶆寜"鍓嶄笁涓凡鎵ц"鎶?freeze 鍒犳帀
```

`git show b3fae6f` 閲岄『搴忓凡缁忓姝わ紝鎵€浠?*鑷?T1-11 璧疯繖鏉″垎鏀氨鏄浠ｇ爜**锛岃鍒掗噷鐨?
`freeze_writes` 琚綋鎴?宸叉墽琛?璺宠繃銆俆1-11 褰撴椂閭ｄ唤 20/20 鏉ヨ嚜 worker 渚?
锛坄remove_and_stop` 鍏?flush 鍐嶅垽 dirty銆佹棤鍏变韩鐙崰鎵撳紑鏈夌晫閲嶈瘯锛夛紝涓嶆槸 service 渚ц繖涓姩浣溿€?

淇硶锛氬畧鍗敼鎴?`quiesced`锛坬uiesce 涔嬪悗銆乫lush 涔嬪墠锛夛紱worker 渚?
`DiskRuntime::freeze_writes` 骞傜瓑锛宍volume_flushed` 鐨勭画璺戣矾寰勪粛璺宠繃銆?

**瀹㈡満姝ｉ潰璇佹嵁**锛坄C:\m5\last-trace.log`锛?0/20 閭ｈ疆锛夛細

```text
Win32VolumeAdapter::lock: volume locked
dismount: through the retained lock          鈫?鎷嗗嵏澶嶇敤鍐荤粨閭ｆ妸閿侊紙T1-11 鐨勭 鈶?鏉¤姹傦級
```

骞朵笖鎵€鏈?stop 鐨勬渶缁堟彁浜ら兘鏄?`checkpoint: disposition=2`锛坄committed`锛夛紝
涓嶅啀鏈?park锛屼篃涓嶅啀鍑虹幇 `23559`銆?

#### 鈶?鏍瑰洜涓夛細閲嶅惎鍚庡嵎杩樻病鍙戝竷灏辫鏋氫妇锛堝凡淇紝`runtime_adapter.cpp`锛?

寮€鍐荤粨鍚?20脳 鍙樻垚 **17/20**锛屼絾澶辫触鍓嶇Щ鍒?restart 涓?*鍙樺揩**锛堟暣鎵?6 鍒嗛挓锛屼笉鍐?300 s锛夛細

```text
command=disk.start message=action=create-runtime code=21000 native=1168
  step=find_recovered_volume detail=no volume carries the recorded identity
```

鏂板缓璁惧鍚庡嵎灏氭湭琚灇涓惧埌锛坄ERROR_NOT_FOUND`锛夛紝涓嬩竴娆¤凯浠ｅ悓鏍疯兘璧锋潵 鈬?鏄彂甯冨欢杩熴€?
`ProductionRuntimeAdapter::find` 鐜板湪瀵?`not_found` 鍋?*鏈夌晫澶嶆**锛? s锛?50 ms 涓€娆★級锛?
**涓斿彧鍦ㄦ湁 durable GUID 鏃?*鎵嶅妫€锛氭病鏈夎褰曡韩浠芥椂"鎵句笉鍒?灏辨槸绛旀锛坮eclaim 鐨勬畫鐣欏彲鑳?
鐪熺殑娌′簡锛夛紝涓嶈绛夈€?

#### 鈶?椤哄甫淇帀锛氫笁澶勯檲鏃ф湡鏈涳紙`6e1bfcc` 閬楃暀锛?

`m4_runtime_adapter_tests`銆乣m4_disk_runtime_tests`锛堝紑鍙戞満锛変笌
`integration.m4.multi_instance`锛堝鏈烘暣濂楁椂鎵嶆毚闇诧級閮芥槸绾㈢殑锛?*涓庢湰娆℃敼鍔ㄦ棤鍏?*锛?
`6e1bfcc` 鎶婃仮澶嶈矾寰勬敼鎴?鍏堝垎閰嶇洏绗﹀啀鏍￠獙韬唤"锛堟墍鏈夋潈 token 鍦ㄦ枃浠剁郴缁熶笂锛屾病鎸傝浇鐐硅涓嶅埌锛夛紝
涓夊 fake 鐨勬湡鏈涢『搴忎粛鏄?`verify 鈫?assign`銆傚凡鏀规垚 `assign 鈫?verify`锛坄multi_instance` 鏄寜涓嬫爣
鏂█鐨?`events[6]/[7]`锛屼竴骞舵敼锛夛紝骞舵妸
`m4_disk_runtime_recovery_size_mismatch_does_not_mount` 鏀规垚瀹冪湡姝ｆ兂璇寸殑浜嬶細
**澶辫触鐨勯噸鍚笉璁哥暀涓嬫寕杞?*锛堢幇鍦ㄦ柇瑷€鐩樼琚敹鍥烇細`assign-letter 鈫?verify 鈫?remove-letter 鈫?
dismount 鈫?aim-stop 鈫?unlock`锛夈€備竷涓?fake-only 濂椾欢寮€鍙戞満鍏ㄧ豢銆?

**涓嬩竴姝?*锛歍1-05 鍙垽 PASSED锛坈reate/checkpoint/stop/restart/绗簩娆?stop/forget + 瀹夎鏈嶅姟锛?
20/20锛夛紱T1-06 璧风户缁€?*鏈獙璇?*锛歚stop` 鐨?`require_clean` 绛栫暐鍒嗘敮锛坋2e 鍙蛋
`checkpoint_required`锛夛紱park 浠嶄繚鐣欑粰"鐪熸 dirty"鐨勬儏褰紙鐜板湪涓嶅啀瑙﹀彂锛屼絾璺緞杩樺湪锛夈€?

#### 鈶?鏁村鍥炲綊锛堝鏈猴紝纭噸鍚?+ 娓呯┖鍚庯級

**151/154**锛堣姹?`8ec8aa88c34740308c2098e9dc90a7f3`锛? 鍒嗛挓锛夛紝涓変釜澶辫触閮戒笉鏄湰娆℃敼鍔細

| 澶辫触 | 鍘熷洜 | 澶勭疆 |
|---|---|---|
| `structural.m2.storage_independent_build` | 瀹㈡満鏃?Visual Studio锛歚vswhere.exe not found` | 鐜鍙楅樆锛屽巻娆″熀绾垮悓椤癸紝**涓嶆槸浜у搧澶辫触** |
| `structural.m5.execution_boundary` | 鍚屼笂 | 鍚屼笂 |
| `m5.production_execution_backend` | 鎵瑰唴鎸傚湪 `WorkerBindingStore::over_protected_root`锛坄store.ok()`锛宍m5_production_execution_backend_tests.cpp:514`锛?| 鍗曡窇 **3/3 閫氳繃** 鈬?鎵瑰唴闂村彂锛坄m5-vm-harness.md` 宸插垪鐨勫凡鐭ラ棿鍙戜箣涓€锛夛紝涓嶆槸鏈鏀瑰姩 |

鍚屾棩鏇存棭閭ｆ鏁村涔熸槸 151/154锛岀涓変釜澶辫触鏄?`integration.m4.multi_instance`
锛坄events[7] == "assign-letter"`锛屼笌 搂鈶?鍚屾簮锛夛紝宸蹭慨锛屽崟璺?
`multi_instance + cli_e2e` 鈫?**2/2**銆?*鏈垎鏀暣濂楃殑鏈熸湜鍊兼槸 152/154**
锛堜袱鏉?vswhere 姘歌繙绾級锛屽鍔?`m5.production_execution_backend` 鍦ㄦ壒鍐呯殑宸茬煡闂村彂銆?

---

### 3.15 2026-10-26 澶滐細T1-06 瀹屾垚锛堜腑鏂彁浜ゅ垽瀹氬彲杈撅級

**瀹屾暣浣撻獙鏂囨。**锛歚docs/validation/m5-t1-06-recovery-proof.md`銆傝鐐癸細

1. **浠?P4 閫?launch 璧峰彧璇绘鏌?worker**锛歚reconcile_checkpoint` 鍦ㄦ病鏈?session 鏃朵粠 durable
   profile **launch 浣嗕笉 start**锛岄棶 `recovery_proof`锛屾瘡涓垎鏀兘鍏抽棴 inspection 骞剁瓑杩涚▼閫€鍑猴紱
   launch 澶辫触浣滀负澶辫触涓婃姤銆傛敼鍓嶇敤 `require_session`锛屽彧鑳?adopt 娲荤潃鐨?runtime鈥斺€旇€屾湰闂
   鎭版伆鏄?鎻愪氦宸插湪 P4銆亀orker 宸蹭笉鍦?锛屾墍浠?*姝ｉ潰璺緞涓€娆￠兘璺戜笉鍒?*銆?
2. **鍙戠幇涓€涓纭鎻愪氦姘歌繙涓嶅彲鑳界殑缂洪櫡**锛氬€欓€夊垽鎹叚鏉￠噷鐨?prior generation 姣旂殑鏄?manifest 鐨?
   `base_generation`锛堝熀搴曢暅鍍忎唬娆★紝鎻愪氦 checkpoint 鏃朵笉鍓嶈繘锛夛紝鑰?correlation 鐨?prior 鏄笂涓€浠?
   checkpoint銆傚鏈鸿鏁?`mask=0x2`锛屽叾浣欎簲椤癸紙闀滃儚 uuid銆佷唬娆°€乶ative id銆乻lot銆乨igest锛?*鍏ㄧ浉鍚?*銆?
   淇硶锛氬€欓€夊垽鎹洖鍒拌鏍间簲鏉★紱prior 鍙敤浜庡惁瀹氬垽鎹紱`CheckpointRecoveryInspection::prior_generation`
   鏀瑰悕 `base_generation`銆備慨鍚?`verdict=2 mask=0x0`銆?
3. 搂14.3 涓冩潯鍏ㄩ儴鏈夎鏁帮紙鍚湡 worker/鐪?bundle 鐨勪笁鏉℃闈?鍚﹀畾璺緞銆佸彧璇绘€ф寚绾癸級銆?

**涓嬩竴姝?*锛歍1-07锛堝唴瀛樺彲瑙佹€?+ UNMAP 绔埌绔級銆?

### 3.16 2026-10-26 澶滐細T1-07 瀹屾垚锛堝唴瀛樺彲瑙佹€?+ UNMAP 绔埌绔級

**楠岃瘉鏂囨。**锛歚docs/validation/m5-t1-07-memory-visibility.md`銆傝鐐癸細

1. `resident_bytes` / `budget_bytes` 浠庡瓨鍌ㄥ眰璁℃暟鍣ㄤ竴璺埌 CLI锛歚disk list` / `disk show` 琛屽熬
   鍑虹幇 `mem=<resident>/<budget>`銆?*鏉ユ簮鏄瓨鍌ㄨ嚜宸辩殑 committed_bytes锛屼笉鏄繘绋嬪伐浣滈泦**銆?
2. **UNMAP 棣栨绔埌绔?*锛氬啓 16 MiB 鈫?resident 2 949 120 鈫?**19 660 800**锛堟伆濂?+16 MiB锛夆啋 鍒犻櫎鍚?
   鍥炲埌 **2 949 120**銆俠udget 1 073 741 824锛寃orker 宸ヤ綔闆?22 573 056锛堝悓閲忕骇锛夈€?
3. **绗竴娆￠噺涓嶅埌锛屽師鍥犲€煎緱璁?*锛氭櫘閫氬啓鍏ュ悗 resident 瀹屽叏涓嶅姩锛屾暟鎹繕鍦?Windows 鏂囦欢缂撳瓨閲岋紱
   鏀?*鍐欑┛** + 鏈夌晫杞鎵嶉噺鍒板瓨鍌ㄣ€傛墍璋?鍐欐枃浠跺悗鍐呭瓨绔嬪埢涓婂崌"鍦?Windows 涓婃槸缂撳瓨閿欒銆?
4. 姣忓眰閮介拤浜嗙敤渚嬶紙鍗忚 round-trip銆佹埅鏂姤 malformed銆乷bservation 鍙湪 alive 鏃跺甫鍊笺€亀ire銆佹覆鏌擄級銆?

**楠屾敹**锛氬共鍑€瀹㈡満 `cli_e2e -Repeat 20 鈫?20/20`锛堝紡 `541c8b4a鈥锛? 鍒?10 绉掞級銆?

**寤跺悗**锛歚disk release-memory`锛埪?.4锛夆€斺€斿畠闇€瑕佷竴鏉′粠 storage 鍏紑 `reclaim_to` 鍒?CLI
鍔ㄨ瘝鐨勫畬鏁寸旱鍒囷紝涓庡唴瀛樺帇鍔涘悓婧愩€?*T1-08 涔嬪悗浠嶆湭鍋氾紝瑙?搂3.17銆屾湭鍋氥€嶃€?*

---

### 3.17 2026-10-26 娣卞锛歍1-08 瀹屾垚锛堟鏌ョ偣绛栫暐缁堜簬鏈夋秷璐硅€咃級

**楠岃瘉鏂囨。**锛歚docs/validation/m5-t1-08-checkpoint-policy.md`銆傝鐐癸細

1. 鍥涗釜绛栫暐瀛楁锛坕nterval / dirty 闃堝€?/ 鍐呭瓨鍘嬪姏 / 浼戠湢锛変粠"瀛樹笅鏉ュ氨涓?鍙樻垚鐪熺殑浼氳Е鍙戙€?
   **瀹㈡満璇绘暟**锛歚generation moved 2 -> 3 on its own after 30.4 s`锛屽叏绋?*娌℃湁鍙戣繃 checkpoint 鍛戒护**銆?
2. 钀界偣锛氱函鍑芥暟 `checkpoint_due_reason()` + `ServiceMaintenance`锛圫YSTEM 韬唤 dispatch锛?
   **鍙仛鎸佷箙鍖栧噯鍏?*锛屾墽琛屼粛褰掕皟搴﹀櫒锛? 璋冨害鍣?claim-once tick锛?5 s锛?
   `SERVICE_ACCEPT_POWEREVENT` / `PBT_APMSUSPEND 鈫?ServiceHost::suspend()`銆?
   suspend **涓嶆槸 stop**锛氳澶囦笉鎷嗐€亀orker 涓嶉€€锛屽彧鍋氬敖鍔涜€屼负鐨勬鏌ョ偣銆?
3. **鎺ョ嚎鏃堕噺鍒颁袱涓己闄?*锛氣憼 interval 鏃堕挓娌℃湁璧风偣锛坄since` 鎭?0 鈬?姘镐笉鍒版湡锛夛紱
   鈶?鍛戒护瀵诲潃鐢ㄤ簡 mount target 鑰岄潪 profile uuid锛堟瘡娆?`The requested profile is not registered`锛夈€?
4. **涓€鏉¤寖鍥磋竟鐣?*锛氫袱鍧楃洏鍚屾椂娲荤潃鏃讹紝绗簩鍧楃殑鎷嗗嵏涓嶄細鐪熸绉婚櫎璁惧锛坮estart 鎵句笉鍒板嵎銆?
   forget `device_absent=0`锛夈€侻5 鏄崟鐩樹骇鍝侊紙G4 寤跺悗锛夛紝鎵€浠?e2e 鏀规垚涓€娆′竴鍧楃洏锛?
   杩欐潯涓嶆槸绛栫暐缂洪櫡锛屼絾鍊煎緱璁颁綇銆?
5. `forget` 鐨勪節涓悎鍙栭」鐜板湪鎷掔粷鏃?*閫愰」鎵撳嵃**锛堝拰 `verify_volume`銆乧heckpoint boundary 鍚屾簮锛夈€?

**楠屾敹**锛氬共鍑€瀹㈡満 `cli_e2e -Repeat 5 鈫?5/5`锛堣姹?`7aa88f14鈥锛? 鍒?9 绉掞級銆?

**鏈仛锛堣褰曪紝涓嶅綋浣滃凡瀹屾垚锛?*锛?
- `disk release-memory`锛埪?.4 鎵嬪姩閲婃斁锛夈€傚畠鍦?T1-07 涓?T1-08 鍚勮寤跺悗涓€娆★紝鐞嗙敱涓嶅彉锛?
  闇€瑕?storage 鍏紑 `reclaim_to` 鈫?M3 鈫?M4 runtime 鈫?worker 鍛戒护 鈫?鏈嶅姟鍔ㄤ綔/璁″垝 鈫?CLI 鍔ㄨ瘝
  鐨勫畬鏁寸旱鍒囥€?*瀹冧笉鍦ㄥ畬鎴愬彛寰勯噷**锛堝彛寰?= G1+G2+G3 楠屾敹 + T4-04 manifest + master-plan 搂8
  鐨勯仐鐣欐竻鍗曪紝鑰岃娓呭崟鍥涢」閲屾病鏈夊畠锛夛紝浣嗚璁?搂7.4 鎵胯杩囪繖涓懡浠わ紝鎵€浠ュ湪杩欓噷鐣欏悕銆?

**涓嬩竴姝?*锛歍1-09锛圡5-A 鍗曠洏楠屾敹 + 鏂數鎭㈠锛夈€傚畠渚濊禆 T1-06/07/08/11锛岀幇鍦ㄥ洓涓兘榻愪簡銆?

---

### 3.18 2026-10-26 娣卞锛歍1-09 杩涜涓紙寮€鏈鸿嚜鍔ㄦ仮澶嶅凡瀹屾垚锛屾柇鐢?harness 寰呮墦閫氾級

**宸插畬鎴愬苟鎻愪氦锛坄51e164d`锛?*锛氬紑鏈鸿嚜鍔ㄦ仮澶嶇殑娑堣垂鑰呫€?
- `ServiceCore::admit_auto_starts()`锛氬惎鍔ㄦ椂鎶?durable 鐘舵€?= running 涓?`auto_start` = true"鐨?
  profile 閫愪釜閲嶆柊**鍑嗗叆** `disk start`锛堝噯鍏ヨ€岄潪鎵ц锛屾墽琛屼粛褰掕皟搴﹀櫒锛夈€?
- `ServiceHost::start()` 鍦?recovery 鈫?sweep 鈫?scheduler 涔嬪悗銆佸叕寮€闈㈡墦寮€涔嬪墠璋冪敤瀹冿紱澶辫触鍗?
  鍚姩澶辫触锛堜笌 recovery/sweep 鍚岃锛夛紝鏉℃暟杩?`ServiceHostDiagnostics::auto_started_operations`銆?
- 娌℃湁杩欐锛屾柇鐢靛悗瀛楄妭浼氳鎭㈠銆佺洏鍗翠互 stopped 鍥炴潵绛変汉鎵嬪惎鈥斺€?鏈€澶氫涪涓€涓棿闅?鍙厬鐜颁竴鍗娿€?
- 涓や釜妯″瀷鐢ㄤ緥锛氫細鍥炴潵鐨勯偅寮犵洏琚笖浠呰鍑嗗叆涓€娆★紙`disk.start`锛夛紱`auto_start=false` 鐨勭洏涓嶇銆?
- 椤哄甫涓ゅ fixture 淇锛堥兘鐢?鎷掔粷瑕佽兘瀹氫綅"閫煎嚭鏉ワ級锛歚resolve` 鐜板湪鍜屼骇鍝佷竴鏍锋帴鍙?uuid锛?
  澶辫触鏃ュ織甯?`step=`銆?

**宸插啓濂姐€佹湭鎵撻€?*锛氭柇鐢甸獙鏀?harness
- `tests/e2e/test-m5-powerloss-prepare.ps1`锛堣 auto-start 鏈嶅姟銆佸缓 30 s 闂撮殧鐨勭洏銆佺疆 auto_start銆?
  鍐欐爣璁般€佺瓑涓€娆¤嚜鍔ㄦ彁浜ゃ€佹妸"蹇呴』娲讳笅鏉ョ殑浜嬪疄"鍐欒繘 `C:\m5\powerloss-state.txt`锛?
- `tests/e2e/test-m5-powerloss-verify.ps1`锛堝彧鐪嬩笉鍔細鏈嶅姟鏄惁鑷繁璧锋潵銆佺洏鏄惁鑷繁鍥炴潵銆?
  generation 鈮?鎻愪氦鍊笺€佹爣璁板瓧鑺備竴鑷淬€亀orker 鎭板ソ涓€涓級
- `tools/m5vm/Test-M5PowerLoss.ps1`锛坔ost 渚т笁娈碉細prepare 鈫?`vmrun stop hard` 鈫?寮€鏈?鈫?verify锛?
  鍒ゅ畾璇?harness 鑷繁鐨?`N of M passed`锛屽洜涓?**harness 鍦ㄧ敤渚嬪け璐ユ椂浠嶉€€鍑?0**鈥斺€旇繖鏉″凡缁忚俯杩囷級
- CMake 宸叉敞鍐?`integration.m5.powerloss_prepare` / `..._verify`锛?55 鈫?157 渚嬶級

**2026-10-27 00:52 绗叚娆¤鏁帮紙骞插噣涓夋璺戝畬锛涘墿涓嬬殑闂鏄?鏈嶅姟 RUNNING 浣嗘病鏈夊叕寮€绠￠亾"锛?*

骞插噣瀹㈡満锛堢‖閲嶅惎 + 娓?ProgramData + 鍒犳湇鍔★級璺戝畬鏁翠笁娈碉細
- `prepare` **1 of 1 passed**锛堣姹?`b0cb01b9鈥锛?2 s锛?
- 纭柇鐢?+ 寮€鏈?+ `verify` 鈫?**0 of 1**锛岃姹?`1280adbd鈥锛岃€楁椂 187 s
- 187 s 鈮?`Wait-ForService 180` + 7 s锛屽嵆**绗竴姝ュ氨澶辫触**锛欳LI 杩炰笉涓?

鏂數鍚庣洿鎺ユ墜鍔ㄧ‘璁わ紙涓嶅啀闈?harness锛夛細

```text
myrd-cli disk list 鈫?exit=3, 30016 ms
  "myrd-cli: the service is not available (is it installed and running?)"
sc query MyRamDiskPowerLoss 鈫?STATE: 4 RUNNING
```

**鎵€浠ョ幇鍦ㄧ殑鏈В闂鏄骇鍝佷晶鐨?*锛氭柇鐢甸噸鍚悗 **SCM 璇?RUNNING锛屼絾鍏紑绠￠亾涓嶅瓨鍦?*锛堟垨涓嶅彲杩炴帴锛夈€?
`ServiceHost::start()` 鑻ユ墦寮€鍏紑闈㈠け璐ヤ細鎶婄姸鎬佺疆 failed 骞惰鏈嶅姟鎶?STOPPED锛屾墍浠ヨ涔?
start() 鏍规湰娌¤蛋鍒?open锛岃涔?open 鎴愬姛鑰岀閬撳 CLI 涓嶅彲杈俱€?

**涓嬩竴娆¤鏌ョ殑椤哄簭锛堥兘寰堜究瀹滐級**锛?
**2026-10-27 00:55 杩藉姞锛堝叧閿竴姝ュ凡缁忔煡浜嗭級**锛氭柇鐢靛悗閭ｅ彴瀹㈡満涓?

```text
Get-Process myrd-service 鈫?娌℃湁杩涚▼
\\.\pipe\ 閲?myrd*     鈫?娌℃湁浠讳綍绠￠亾
C:\ProgramData\MyRamDisk\logs\service.log 鏈€鍚庡啓鍏?00:46:32锛堝湪鏂數涔嬪墠锛?
```

鑰?`sc query` 鍦ㄥ嚑鍒嗛挓鍓嶈繕鎶?RUNNING銆?*缁撹锛氭柇鐢靛悗鐨勬湇鍔¤繘绋嬭嚜宸遍€€鍑轰簡锛堢枒浼煎穿婧冿級锛?
鍏紑绠￠亾鍥犳浠庢病寤虹珛璧锋潵**銆傛墍浠?T1-09 鍓╀笅鐨勪笉鏄竴涓?鎭㈠閫昏緫"闂锛岃€屾槸
**"閲嶅惎鍚庢湇鍔¤繘绋嬩細姝?**鈥斺€斾笅涓€姝ュ繀椤荤湅 **Windows 搴旂敤绋嬪簭浜嬩欢鏃ュ織**锛圗vent ID 1000 /
WER锛夋嬁宕╂簝妯″潡涓庡亸绉伙紝鍐嶅喅瀹氭槸淇唬鐮佽繕鏄慨鐜銆俙service.log` 鍦?00:46:32 涔嬪悗娌℃湁浠讳綍
鏂拌锛岃鏄庢湇鍔?*鍦ㄨ繖娆″紑鏈洪噷杩炰竴鏉℃棩蹇楅兘娌″啓**鈥斺€斿畠寰堝彲鑳藉湪 `ServiceHost::start()` 涔嬪墠
锛堟垨鍒氬紑濮嬶級灏辨浜嗭紝杩欎竴鐐逛笌"宕╂簝"姣斾笌"閫昏緫鍗′綇"鏇村惢鍚堛€?

**2026-10-27 01:00 鍐嶈拷鍔狅紙璇佹嵁浜掔浉鐭涚浘锛屽繀椤绘寜鏃堕棿鎴抽噸娴嬶紝涓嶈鍐嶇寽锛?*

鍚屼竴鍙板鏈恒€佸嚑鍒嗛挓涔嬪唴锛?

| 鏃跺埢 | 瑙傚療 |
|---|---|
| 00:52 | `myrd-cli disk list` 鈫?exit 3銆宻ervice is not available銆嶏紝30 s锛沗sc query` 鈫?**RUNNING** |
| 00:55 | 娌℃湁 `myrd-service` 杩涚▼銆佹病鏈変换浣?`myrd*` 绠￠亾锛沗service.log` 鏈€鍚庝竴琛?**00:46:32**锛堟柇鐢靛墠锛?|
| 01:00 | `sc query` 鈫?**1060 鏈嶅姟涓嶅瓨鍦?*锛坄service.log` 浠嶆槸 00:46:32锛?|

涓夋潯瑙傚療涓嶈兘鍚屾椂涓虹湡锛歊UNNING 鐨勬湇鍔′笉鑳芥棦娌℃湁杩涚▼銆佸張娌℃湁绠￠亾锛屾洿涓嶈兘鍦ㄦ病鏈変汉鍒犲畠鐨勬儏鍐典笅
鍙樻垚"涓嶅瓨鍦?銆傚彲鑳界殑瑙ｉ噴鏈変笁绫伙紝**閮藉繀椤荤敤甯︽椂闂存埑鐨勯噸娴嬫潵鍖哄垎**锛?
(a) 鎴戣鍒扮殑 RUNNING 鏄?*涓婁竴娆″噯澶囬樁娈?*鐨勮緭鍑猴紙鍛戒护杈撳嚭涓蹭簡锛夛紱
(b) 瀹㈡満鍦ㄤ袱娆℃鏌ヤ箣闂?*閲嶅惎杩?*锛堜緥濡傛垜鍔犵殑 boot 閲嶈瘯鎴栧鏈鸿嚜宸卞浣嶏級锛屾湇鍔″洜姝ゅ洖鍒?
    "鏈畨瑁?锛坄C:\ProgramData` 閲岀殑 profile 鐩綍杩樺湪锛岃鏄庝笉鏄揩鐓у洖婊氾級锛?
(c) 鏈嶅姟瑁呭湪涓€涓?*姣忔寮€鏈洪兘浼氭秷澶辩殑浣嶇疆**锛堜緥濡?binPath 鎸囧悜 staging 鐩綍鑰岃娓呯悊锛夈€?

**涓嬩竴娆＄殑鍋氭硶锛堝啓鎴愭竻鍗曪紝鐓у仛锛?*锛?
1. 骞插噣瀹㈡満 鈫?`Test-M5PowerLoss.ps1` 鍙窇鍒?prepare锛坄-ResumeVerify` 涔嬪墠鎵嬪姩鍋滐級锛?
2. **绔嬪埢**璁板綍锛歚sc qc`锛坆inPath/start 绫诲瀷锛? `sc query` + `Get-Process myrd-service` +
   `\\.\pipe\` 杩囨护 + `service.log` 琛屾暟 + `Get-Date`锛屼綔涓?*鍩虹嚎蹇収**锛?
3. 鏂數 鈫?寮€鏈?鈫?**姣?15 绉?*閲囦竴娆″悓鏍风殑蹇収锛岃繛閲?3 鍒嗛挓骞跺甫鏃堕棿鎴宠緭鍑猴紱
4. 鐢ㄥ揩鐓у簭鍒楀垽鏂笂闈?(a)/(b)/(c) 鍝竴绫伙紝鍐嶅姩浠ｇ爜銆?

**2026-10-27 01:05 缁撹锛氫笂涓€杞殑"鐭涚浘"鏄祴閲忛€氶亾鐨勯棶棰橈紝涓嶆槸浜у搧鐨?*

鐢?*甯︽椂闂存埑鐨勫揩鐓?*锛坄C:\m5\snap.ps1`锛屽鏈烘湰鍦拌窇锛夊湪鏂數閲嶅惎鍚?31 绉掗噰鍒帮細

```text
01:03:38 state=RUNNING pid=3164 start=01:03:07 pipes=0 loglen=8605
sc qc 鈫?START_TYPE: 2 AUTO_START
        BINARY_PATH: 鈥out\build\msvc\src\Release\myrd-service.exe
```

鑰屽悓涓€鏃跺埢鏈嶅姟鑷繁鐨勬棩蹇楀凡缁忓啓鍒?`disk.start 鈥?terminal`鈥斺€?*鏂數鍚庣殑閭ｆ start 璧板畬浜嗗叏閮?
鍔ㄤ綔**锛坄start-proxy 鈫?create-device 鈫?locate-physical-drive 鈫?verify-volume 鈫?assign-mount 鈫?
readiness-io 鈫?terminal`锛夈€?

浜庢槸涓夋潯"鐭涚浘"瑙傚療鐨勮В閲婃槸锛?*鏅€?ssh 浼氳瘽涓嶆槸鎻愭潈浼氳瘽**锛圓GENTS 搂5锛歋SH 鏄綉缁滅櫥褰曘€佷笉鎻愭潈锛夛紝
鎵€浠?
- `\\.\pipe\` 鏋氫妇鍙兘琚嫆 鈫?鎴戠湅鍒扮殑 `pipes=0` **涓嶆槸**"娌℃湁绠￠亾"锛?
- 闈炴彁鏉冪殑 `myrd-cli disk list` 鎷夸笉鍒板叕寮€绠￠亾 鈫?鎴戠湅鍒扮殑 `service is not available` **涓嶆槸**
  "鏈嶅姟娌¤捣鏉?銆?

**鏁欒锛堝啓杩?lessons锛?*锛?*鍑℃槸瑕佺鍏紑绠￠亾銆佽繘绋嬨€佹湇鍔＄殑娴嬮噺锛屽彧鑳借蛋 harness 鐨勬彁鏉冮€氶亾
锛坈test 鐢ㄤ緥 + scheduled task锛夛紝涓嶈兘鐢ㄦ櫘閫?ssh 浼氳瘽涓嬬粨璁恒€?* 鎴戜笂涓€杞嵁姝ゅ啓涓嬬殑
"鏈嶅姟杩涚▼姝讳簡""鏈嶅姟涓嶅瓨鍦?涓ゆ潯閮借鏀跺洖銆?

**涓嬩竴姝?*锛氭妸蹇収鍋氭垚涓€涓?ctest 鐢ㄤ緥锛堟彁鏉冮€氶亾锛夛紝鍦?verify 涔嬪墠鎵撳嵃涓€娆★紱鐒跺悗鐢ㄥ畠鏇夸唬
鎴戣繖鍑犺疆鐨勬墜宸?ssh 鍒ゆ柇锛岄噸璺戜竴娆′笁娈点€倂erify 鐨?`Wait-ForService` 鑻ヤ粛瓒呮椂锛岄偅鎵嶆槸鐪熼棶棰樸€?


   `StartTime` 鏄笉鏄繖娆″紑鏈猴紱
2. `C:\ProgramData\MyRamDisk\logs\service.log` 閲岃繖娆″紑鏈虹殑鏈€鍚庡嚑琛屸€斺€?*鏈夋病鏈?* `auto-start`
   涓?subsequent 鐨?action 琛岋紙濡傛灉鏃ュ織鍋滃湪 `auto-start: 鈥etired鈥 涔嬪悗锛岃鏄?start() 鍗″湪
   `admit_auto_starts` 鎴栬皟搴﹀櫒鍚姩閲岋級锛?
3. `\\.\pipe\` 閲屾湁娌℃湁鍏紑绠￠亾鍚嶏紙`Get-ChildItem \\.\pipe\ | ? Name -match 'myrd'`锛夛紱
4. 鑻ユ湇鍔℃棩蹇?*瀹屽叏娌℃湁**杩欐寮€鏈虹殑琛岋紝璇存槑鏈嶅姟杩涚▼涓嶆槸杩欎竴鐗堜簩杩涘埗锛坰taging/鏈嶅姟 binPath 涓嶅锛夈€?

**T1-09 宸插畬鎴愮殑閮ㄥ垎**锛堟湁璇绘暟锛夛細鏂數鍚庢湇鍔¤嚜璧枫€佺洏鑷捣銆乬eneration 鈮?鎻愪氦鍊笺€佽剰鏁版嵁閲嶆斁
鎻愪氦鎴愪笅涓€浠ｏ紙`0af35d5` 鐨勬彁浜や俊鎭噷鏈夊畬鏁磋鏁帮級銆?*鏈畬鎴?*锛歷erify 绔埌绔竴娆＄豢锛?
浠ュ強涓婇潰閭ｆ潯"RUNNING 浣嗘棤绠￠亾"鐨勫師鍥犮€?

**2026-10-27 00:45 绗簲娆¤鏁帮紙T1-09 鐨勪骇鍝侀儴鍒嗗凡鍏ㄧ豢锛涘墿涓嬬殑鍏ㄥ湪 verify 鑴氭湰涓庡鏈轰晶锛?*

**鏂數鍚庤繛缁袱娆¤鍒扮殑浜у搧浜嬪疄**锛堣繖鎵嶆槸 T1-09 瑕佺殑锛夛細

```text
service state: RUNNING锛堣剼鏈粠鏈惎鍔ㄥ畠锛?
5003a664-鈥?T: ready running runtime=Running activity=Idle
            persistence=Dirty health=Healthy generation=4  mem=1048576/1073741824
recovered generation: 4 (last committed before the cut: 3)
```

鍗筹細**鏈嶅姟鑷繁璧锋潵銆佺洏鑷繁鍥炴潵銆乬eneration 鈮?鏂數鍓嶆彁浜ゅ€笺€乺esident 鍐呭瓨 1 MiB**銆?
`persistence=Dirty` 涓?generation 浠?3 璧板埌 4锛岃鏄庢仮澶嶆妸鎺夌數鏃剁殑鑴忔暟鎹噸鏀惧苟鎻愪氦浜嗕竴浠?
鈥斺€旇繖姝ｆ槸"鏈€澶氫涪涓€涓棿闅?鐨勫疄娴嬪舰鎬併€?

**鏈疆淇帀鐨勪笁澶勮剼鏈?宸ュ叿缂洪櫡**锛堥兘鏄?璁╀骇鍝佺湅璧锋潵鍧忎簡"鐨勯偅绫伙級锛?
1. 鐘舵€佹枃浠?*娌¤惤鐩?*锛氭柇鐢靛悗鍥炴潵鏄?0 瀛楄妭 鈫?鐢?`FileStream.Flush($true)` 鍐欙紱
2. `verify` 閲?`Join-Path $mount (powerloss-marker.txt)` 灏戝紩鍙凤紙`CommandNotFoundException`锛夛紱
3. 缂栨帓鍣ㄥ姞 `-ResumeVerify`锛堝彧璺?verify锛変笌 boot 澶辫触鑷姩閲嶈瘯涓€娆°€?

**鏈€鍚庝竴涓湭瑙ｇ殑鐜拌薄**锛歚-ResumeVerify` 閭ｆ `sc query` 鏄?RUNNING锛屼絾 CLI 杩炰笉涓婂叕寮€绠￠亾
锛坄Wait-ForService 180` 瓒呮椂锛夈€備袱娆¤鏁颁箣闂存垜鍓嶄竴娆?verify 澶辫触鍚?*娌℃湁鍋氭竻鐞?*锛屾墍浠ユ瀬鍙兘鏄?
**涓婁竴涓湇鍔″疄渚嬬殑绠￠亾娈嬬暀**銆備笅娆★細鏂數閲嶅惎 鈫?瀹屾暣璺戜竴娆′笁娈碉紙涓嶈 -ResumeVerify锛夛紝
鑻ヤ粛鍦?`Wait-ForService` 澶辫触锛屽厛鐪?`sc query` 鐨?PID 涓?`C:\ProgramData\MyRamDisk\logs\service.log`
鏄惁鏉ヨ嚜鍚屼竴杩涚▼銆?

**涓嬩竴姝ワ紙鏂颁細璇濅粠杩欓噷寮€濮嬶級**锛氬共鍑€瀹㈡満 鈫?涓夋涓€娆¤窇閫?鈫?鍐?
`docs/validation/m5-t1-09-power-loss.md`銆乼asklist T1-09 鈫?PASSED 鈫?杩?T1-10銆?

**2026-10-27 00:35 绗洓娆¤鏁帮紙prepare 绋冲畾閫氳繃锛屾柇鐢靛悗瀹㈡満鏈韩鏈夋鐜囪捣涓嶆潵锛?*

- `prepare` 鍦ㄥ共鍑€瀹㈡満涓?*杩炵画涓ゆ閫氳繃**锛堣姹?`d0667c91鈥 50 s銆乣8eae57a7鈥 52 s锛夛細
  瑁?auto-start 鏈嶅姟 鈫?寤?30 s 闂撮殧鐨勭洏 鈫?`auto_start` 鈫?鍐欐爣璁?鈫?绛夎嚜鍔ㄦ彁浜?鈫?钀界姸鎬併€?
- 鐘舵€佹枃浠舵敼 JSON 鍚?`verify` 浠嶆姤 `invalid JSON primitive`锛堝啓璇讳袱绔缂栫爜鐨勫鐞嗕粛涓嶄竴鑷达級锛?
  浜庢槸**鏀规垚涓嶄緷璧?JSON 鐨勫閿欒鍙?*锛堜竴鏉℃鍒欏悓鏃舵帴鍙?JSON 涓?`key=value`锛屽け璐ユ椂鎵撳嵃鍏ㄦ枃锛夈€?
- **纭柇鐢典箣鍚庡鏈烘湁姒傜巼璧蜂笉鏉?*锛氳繖娆?600 s 鍐?SSH 鏃犲搷搴旓紙绱绗簩娆★級銆備富鏈轰晶
  `vmrun stop hard` + `start nogui` 涔嬪悗 15 s 灏卞ソ浜嗭紝璇存槑鏄?*瀹㈡満鍚姩鍋跺彂鎸傛**锛屼笉鏄綉缁滈厤缃€?
  鈬?缂栨帓鍣ㄩ渶瑕侊細boot 绛夊緟瓒呮椂鍚?*鑷姩鏂數閲嶈瘯涓€娆?*锛屽苟涓旀彁渚?`-ResumeVerify`
  锛坧repare 涓庢柇鐢甸兘宸插畬鎴愩€佺姸鎬佹枃浠朵粛鍦ㄦ椂锛屽彧璺?verify锛屼笉閲嶈窇鍓嶄袱娈碉級銆?
- 鏂數鍚庣殑閭ｆ start 宸茬粡璧板畬鏁寸湡瀹炲簭鍒楋紙`6177380` 鐨勪慨澶嶏級锛屾墍浠ヤ笅涓€杞鐪嬬殑鍙墿
  **verify 鐨勪笁鏉℃柇瑷€**鑳藉惁璇诲埌姝ｇ‘鐨勭姸鎬佹枃浠躲€?

**涓嬩竴姝ワ紙鏂颁細璇濅粠杩欓噷寮€濮嬶級**锛?
1. 缁欑紪鎺掑櫒鍔?`-ResumeVerify`锛堣烦杩?prepare 涓庢柇鐢碉紝鐩存帴 verify锛変笌 boot 澶辫触鑷姩閲嶈瘯涓€娆★紱
2. 骞插噣瀹㈡満璺戜竴娆″畬鏁翠笁娈碉紱verify 閫氳繃鍗?T1-09 瀹屾垚锛堣鏁板啓杩?
   `docs/validation/m5-t1-09-power-loss.md`锛宼asklist T1-09 鈫?PASSED锛夛紱
3. 鐒跺悗杩?T1-10銆?

**2026-10-27 00:05 绗笁娆¤鏁帮紙harness 宸茶兘鍒ょ豢锛屽鏈轰細鍦ㄤ袱澶勫崱姝伙級**

- harness 鐨勫垽瀹氫慨濂戒簡锛屼袱澶勯兘鏄?*閲忓嚭鏉ョ殑**锛?
  鈶?harness 鐢?`Write-Host` 鎶ュ垽瀹氾紝璧扮殑鏄?information stream锛宍2>&1 | Out-String` **鎶撲笉鍒?*锛?
  浜庢槸鍖呰鍣ㄥ鐫€"灞忓箷涓婃槑鏄庡嵃鐫€ `0 of 1 passed`"鐨?run 璇?娌℃湁鍒ゅ畾琛?锛涙敼鐢?`6>&1`銆?
  鈶?staging 闃舵鍗扮殑鏄暟閲忎笉鏄垽瀹氾紝鐢ㄥ悓涓€鏉¤鍒欎細鎶婂垰鎴愬姛鐨?stage 鍒ゆ垚澶辫触锛泂o stage 鍙湅閫€鍑虹爜銆?
- **`-SkipStage` 浼氳瀹㈡満璺戞棫鐨?e2e 鑴氭湰**锛堣剼鏈殢 `-Stage` 鐨?git 褰掓。涓嬪彂锛夈€傝繖瑙ｉ噴浜?
  "uuid 瑙ｆ瀽宸蹭慨鍗翠粛鐒舵姤鍚屼竴涓敊"鈥斺€旈偅涓ゆ瀹㈡満璺戠殑鏄棫鏂囦欢銆備互鍚庢敼 e2e 鑴氭湰蹇呴』甯?`-Stage`銆?
- **瀹㈡満浼氬崱姝?*锛堜袱娆★紝閮藉湪 prepare 闃舵鍓嶅悗锛夛細SSH 鏃犲搷搴旓紝鍙湁纭柇鐢垫墠鑳芥仮澶嶏紱
  涓ゆ閮芥槸鍏堣窇杩囦竴杞柇鐢靛疄楠屼箣鍚庛€?*涓嬩竴娆″皾璇曞墠鍏堢‖閲嶅惎**锛屽苟鎶?璺戝墠閲嶅惎"褰撲綔杩欎釜
  harness 鐨勫浐瀹氬墠缃紝鑰屼笉鏄緥澶栧鐞嗐€?

**涓嬩竴姝ワ紙鏂颁細璇濅粠杩欓噷寮€濮嬶級**锛?
1. 纭噸鍚鏈?鈫?娓?`ProgramData\MyRamDisk\{profiles,operations,logs}`锛?
2. `Test-M5PowerLoss.ps1`锛?*涓嶅甫** `-SkipStage`锛夛紝涓€娆¤窇瀹屼笁娈碉紱
3. 鑻?prepare 鍐嶆鍗℃锛氬厛鐢?`sc query`/`Get-Process` 纭鏄?*鏈嶅姟**杩樻槸**瀹㈡満**鍗★紝
   鍐嶆妸 prepare 閲?瑁呮湇鍔♀啋start"杩欎竴姝ユ媶鎴愮嫭绔?ctest 鐢ㄤ緥锛堢幇鍦ㄥ畠鍜?create 鍦ㄤ竴涓敤渚嬮噷锛?
   涓€鍗″氨鍒嗕笉娓咃級锛?
4. 鏂數鍚?`verify` 鑻ヤ粛鎶?`blocked/needs-attention + PersistenceBlocked`锛岃
   `C:\ProgramData\MyRamDisk\logs\service.log`锛堟湰杞柊澧炵殑鏂囦欢鏃ュ織锛宍c97e9cc`锛夐噷
   `auto-start refused: step=鈥 閭ｄ竴琛屸€斺€斿師鍥犲氨鍦ㄩ偅閲屻€?

**2026-10-26 娣卞 绗簩娆¤鏁帮紙prepare 宸查€氾紝鍗″湪鏂數鍚庣殑 start锛?*

- `prepare` **閫氳繃**锛堣姹?`a9fb722dd5854923b25e754637f74ed4`锛?2 s锛夛細瑁?auto-start 鏈嶅姟 鈫?
  寤?30 s 闂撮殧鐨勭洏 鈫?`auto_start=true` 鈫?鍐欐爣璁?鈫?**鑷姩鎻愪氦鍒?generation 3** 鈫?鐘舵€佽惤鐩樸€?
  绗竴娆″け璐ョ殑鍘熷洜涓嶅湪鑴氭湰锛氬鏈轰笂娈嬬暀鐫€涓婁竴娆″皾璇曠殑鏈嶅姟涓?profile锛宍sc start` 涓庢棫瀹炰緥鍐茬獊锛?
  娓呭共鍑€鍚庝竴娆￠€氳繃锛坄sc start` 涔嬪悗 stdout 鏂湪 STATE 鍧楁槸**鏃у疄渚?*鐨勮〃鐜帮紝涓嶆槸鑴氭湰 bug锛夈€?
- 纭柇鐢碉紙`vmrun stop hard`锛? 寮€鏈哄悗锛?*鏈嶅姟鑷繁璧锋潵浜?*锛坄sc query` RUNNING锛屽紑鏈?23:04锛夛紝
  璇存槑 `start= auto` 涓?SCM 璺緞鎴愮珛銆?
- `verify`锛堣姹?`05e2971d25474ee79b2feb2b4faeeca7`锛夎鍒帮細

```text
6b710f86-鈥? U:  ready  blocked/needs-attention  runtime=Running
                persistence=PersistenceBlocked health=NeedsAttention
                generation=3  mem=0/0
recovered generation: 3 (last committed before the cut: 3)
```

鍗筹細**鐩樿鑷姩鎷夎捣浜嗭紙`runtime=Running`锛実eneration 绛変簬鏂數鍓嶇殑鎻愪氦鍊硷級锛屼絾杩愯闈㈡槸鍧忕殑**鈥斺€?
`PersistenceBlocked`銆乣mem=0/0`銆佺洏绗?U: 涓嶅瓨鍦ㄣ€佷篃娌℃湁 worker 杩涚▼銆傛墍浠?T1-09 鍓╀笅鐨勯棶棰樻槸
**鏂數鍚庣殑閭ｆ start 娌＄湡姝ｆ帴涓婂嵎**锛岃€屼笉鏄?娌℃湁鑷姩鎭㈠"銆?

**涓嬩竴姝ワ紙绗竴浠朵簨锛?*锛氭煡鏂數鍚庨偅娆?`disk start` 澶辫触鍦ㄥ摢涓€姝ャ€傚彲鐢ㄧ殑鍏ュ彛锛?
- `ServiceHostDiagnostics::auto_started_operations` 鍙粰鏉℃暟锛屼笉缁欏師鍥狅紱installed 鏈嶅姟鐨?stderr
  琚?SCM 涓㈡帀锛屾墍浠?*蹇呴』**璁╄繖鏉″師鍥犺惤杩涙湇鍔¤嚜宸辩殑鏃ュ織锛坄ServiceLogEvent`锛夎€屼笉鏄?stderr锛?
- 鍙枒鐐逛笌 搂3.18 鍓嶉潰璁扮殑"涓ゅ潡鐩?鐜拌薄鍚屾簮锛?*宕╂簝娈嬬暀鐨勮澶?鍙ユ焺**璁?`create-runtime` 鎺ヤ笉涓?
  锛坄find_recovered_volume` 鎵句笉鍒板嵎锛屾垨 `start_aim` 鎷掔粷锛夈€俆1-03 鐨?startup sweep 鏈鍏堝洖鏀?
  娈嬬暀锛宻weep 鍦?auto-start 涔嬪墠杩愯鈥斺€旀墍浠ヨ纭 sweep 鏄惁鐪熺殑鎶婃畫鐣欏綋"鍙洖鏀?澶勭悊浜嗐€?
- 鍐嶉『甯︿慨涓€澶勮剼鏈剢寮辩偣锛歚verify` 鍦ㄧ洏绗︿笉瀛樺湪鏃舵姏 `DriveNotFoundException`锛堢幇鍦ㄨ `trap`
  鎹曡幏骞舵墦鍗帮紝浣嗕笉鏄竴鏉″彲璇荤殑澶辫触锛夛紝搴旀敼鎴?璇讳笉鍒版爣璁?鐨勫叿鍚嶅け璐ュ苟闄勪笂 `disk list` 琛屻€?

---

## 4. 鏋勫缓涓庢祴閲忓惊鐜?

```powershell
# **姣忎竴涓柊浼氳瘽銆佹瘡涓€娆℃瀯寤轰箣鍓嶏細**
#   涓?dot-source 灏辨病鏈?LIB/INCLUDE锛岄摼鎺ヤ細鍦?LNK1181锛坆crypt.lib锛変笂澶辫触鈥斺€?
#   鐪嬭捣鏉ュ儚缂?SDK锛屽叾瀹炴槸鐜娌¤繘鏉ャ€傚喎浼氳瘽涓€瀹氫細鎾炰笂锛?026-10-26 澶嶇幇骞惰鍦?
#   `m5-review-2026-10-26.md` 搂3.3锛沗m5-lessons.md` 閲屽綋鏃舵病鍐欙紝鍒互涓哄啓杩囷級銆?
Set-ExecutionPolicy -Scope Process Bypass -Force
. .\tools\Enter-MsvcEnvironment.ps1

# 鏋勫缓锛堟敼杩囦换浣曞ご鏂囦欢 鈬?蹇呴』鍔?--clean-first锛岃 lessons 搂1锛?
cmake --build out/build/msvc --config Release --clean-first

# VM 鍥炶矾锛堝厛鍦ㄥ紑鍙戞満鏋勫缓锛屽啀閫佽繘瀹㈡満锛涙墽琛屽彧鍦ㄥ鏈猴級
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'
$env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
tools/m5vm/Invoke-M5Vm.ps1 -Probe
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release
tools/m5vm/Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 180
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '<鐢ㄤ緥鍚?' -Repeat 20
tools/m5vm/Invoke-M5Vm.ps1 -Run -RequestId <id>
```

鏁村涓€閬嶇害 9鈥?1 鍒嗛挓銆?*`-Stage` 涓嶆槸鍙€夌殑**锛氫簩杩涘埗鍦ㄥ紑鍙戞満鏋勫缓锛屾墽琛屽彧鍦ㄥ鏈恒€?
**`-Stage` 澶辫触浼氱暀涓嬭剰瀹㈡満**锛堝鍎?worker + 澶氫綑鐩?+ 娈嬬暀鐩樼锛夆噿 鍏堢‖閲嶅惎鍐嶉噸璇曘€?

---

## 5. 鐜浜嬪疄

| 椤?| 鍊?|
|---|---|
| 瀹㈡満 | VMware Workstation锛學indows 11 x64锛孧AC `00:0C:29:02:90:C1` |
| 鍦板潃 | `192.168.180.129`锛孲SH 鍒悕 **`myrd-vm`**锛堣剼鏈竴寰嬩紶鍒悕锛屼笉纭紪鐮佸湴鍧€锛?|
| 瀹㈡満璐︽埛 | `usr` / `123`锛涘瘑閽?`~\.ssh\id_ed25519` |
| VMX | `C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx` |
| vmrun | **`-T ws` 蹇呴』鏄涓€涓弬鏁?*锛沄M 鍔犲瘑锛屾瘡鏉￠兘瑕?`-vp 12345678` |
| 椹卞姩 | `phdskmnt` 蹇呴』 Running锛?*涓嶅緱瀹夎/鏇挎崲/鍗囩骇/鍗歌浇** |

**涓€鏉′笌浠撳簱鏃犲叧銆佷絾浼氭氮璐规椂闂寸殑宸ュ叿浜嬪疄**锛氭湰浼氳瘽鐨?agent shell 鍦?
`workspace-write` 娌欑涓?*姣忎釜瀛愯繘绋嬮兘浠?`0xC0000142`锛坄STATUS_DLL_INIT_FAILED`锛夐€€鍑?*锛?
杩?`Get-Process` 閮借捣涓嶆潵锛涘垏鍒?`danger-full-access` 鍚庢甯搞€?
**鐪嬪埌杩欎釜閿欒鐮佸厛鎬€鐤戞矙绠辨ā寮忥紝涓嶈鎬€鐤戞瀯寤烘垨浠撳簱銆?*

---

## 6. 寰呰鍐筹紙鍙湁杩欎簺闇€瑕佸仠涓嬫潵闂級

| # | 浜嬮」 | 鐘舵€?|
|---|---|---|
| 1 | Primo "Mini Tools" 鐨勮寖鍥村畾涔?| **鏈畾涔?* 鈬?涓嶅垪鍏ヨ寖鍥?|
| 2 | T4-02 鍗曟枃浠?VHDX 瀵煎嚭 | 宸茶鍐筹細淇濈暀 |
| 3 | 瀹屾垚鍙ｅ緞 | **宸茶鍐筹紙2026-10-17锛?*锛欸1 + G2 + G3 鍏ㄩ儴楠屾敹 + T4-04 manifest 鍐荤粨 + 搂8 閬楃暀娓呯┖锛汫4 寤跺悗 |
| 4 | G4 浣曟椂鎭㈠ | 鏈畾锛涙仮澶嶅墠缃潯浠惰 master-plan 搂6 |

鍏朵綑锛堝惈 T1-05 鐨?`stop` 璇箟锛夐兘鍦ㄥ疄鐜版潈闄愬唴锛?*鑷富鎺ㄨ繘锛屼笉瑕佸弽澶嶉棶**銆?

---

## 7. 纭€ц鍒欙紙鎽樺洓鏉★紝鍏ㄦ枃瑙?`m5-lessons.md`锛?

1. 鏀逛换浣曞ご鏂囦欢涔嬪悗銆佹祴閲忎箣鍓嶅繀椤?`--clean-first`锛涘惁鍒欏緱鍒?`0xC0000005` 鍋囧穿婧冦€?
2. **鍗曟缁挎槸寮辫瘉鎹?*锛涗慨澶嶇被缁撹 鈮?`-Repeat 20`锛屼换浣?缁?鐨勫０鏄庡繀椤诲啓鏄庤窇浜嗗嚑娆°€?
3. 寮€鍙戞満鍙窇**涓嶅垱寤虹湡瀹?worker / AIM 鐩?/ Windows 鍗?*鐨勬祴璇曪紱涓嶇‘瀹氭椂鎸?鍙兘涓?VM"銆?
4. 鏂█澶辫触蹇呴』鎵撳嵃 `operation / message / native / code`鈥斺€斾竴娆″鏈哄線杩?5鈥?0 鍒嗛挓锛?
   鐩茬寽鐨勪唬浠锋槸鏁磋疆銆?

---

## 8. 鍘嗗彶锛堝凡褰掓。锛屼笉瑕佸啀寮曠敤锛?

`feature/m5-unified-integration`銆乣feature/m5-supervisor-service` 鏄?2026-09 鐨勪袱鏉″疄鐜扮嚎锛?
宸茶 v3 鍙栦唬锛涘伐浣滄爲涓庡垎鏀凡浜?2026-10-25 鍒犻櫎锛屼粎浠?tag
`archive/m5-unified-integration`銆乣archive/m5-supervisor-service` 淇濈暀鍘嗗彶銆?
**褰撳墠浠ｇ爜閲屾病鏈変换浣曚笢瑗夸緷璧栧畠浠?*锛沗docs/m4-m5-scope.md` 閲屽瀹冧滑鐨勬弿杩板睘浜庡巻鍙茶儗鏅€?

---

### 3.22 2026-10-27 01:15：T3 开始（T3-01 第一部分已落地）

**已提交**：`src/product/service_view.{h,cpp}` —— 客户端可见的**中立词汇**
（`ServiceLifecycle` / `Runtime` / `Activity` / `Persistence` / `Health` / `Aggregate` /
`OperationState` / `Disposition`，加 `ServiceDiskView` / `ServiceOperationView`）。
值就是线上的数字，并用 `static_assert` 钉住；`ServiceAggregate` **只有四个值**，与服务自己算的
判定一一对应（客户端不许发明第五个）。

**为什么先做这个**：T3-01 要求 GUI 只链 `myrd_product_contracts` + `myrd_product_ipc` + 纯客户端
库，而服务的响应投影说的是 `product::RuntimeState` / `AggregateStatus` / `OperationState`
（management 的概念）。边界只能建立在一套两边都能说的词汇上。

**T3-01 剩余（下一步，机械执行）**：
1. 新建库 `myrd_product_client`（`src/client/response_view.{h,cpp}`）：把
   `src/management/service_response_wire.cpp` 的解码半边搬过去（约 230 行：`Reader`、
   `read_uuid`、`decode_profile`、`decode_operation`、`decode_error_field`、
   `decode_service_response`），产出 `ServiceDiskView` / `ServiceOperationView`；
2. management 新增 `project_view()` 的**穷尽 switch**（新增状态忘映射就编译失败），编码仍留在 management；
3. `myrd_cli` 改链 `myrd_product_client`（不再链 `myrd_management`）；
4. **负例测试**：配置期用 `get_target_property` 把 GUI 目标的 `LINK_LIBRARIES` 闭包写文件，
   ctest 断言不含 `myrd_management` / `myrd_persistence` / `myrd_storage` / `myrd_proxy` / `myrd_driver`；
5. T3-02 起才是 Win32 窗口/托盘/列表（`CreateWindowEx` + `NOTIFYICONDATA` + `ListView`），
   以及 T3-03 的进度一致性、T3-04 的三处确认流（force / accept_volatile_data_loss / confirmation_id）。

**T3 验收口径**：GUI 在客机上能列出盘/内存/generation；创建/启动/停止/checkpoint 可用且长操作
期间界面不假死；三处确认流分别出现；未提权时只读。
---

### 3.23 2026-10-27 01:40：T3-01…T3-04 完成，T3-05 待客机重跑

- **T3-01 完成**：客户端边界（`myrd_product_client` + 闭包检查，已证明能失败）。
- **T3-02 完成**：`myrd-gui.exe`（列表 + 托盘 + 3 秒刷新）；`aggregate` 是唯一判定（有测试）。
- **T3-03/T3-04 完成**：命令在**独立线程**发出（界面不假死）、进度跟操作状态并在终态停止、
  三处确认流（stop 仅在检查点未确认时、forget 需回显 uuid、权限拒绝给指令）。
- **T3-05 进行中**：`integration.m5.gui_workflow`（客机）已注册（159 例）。首次运行：服务装好并
  应答，但**建盘失败**（`the create ended failed`）——同一台客机跑了一整晚断电实验后的残留。
  **下一步：硬重启客机再跑一次**；若仍失败，先看 `disk list` 与 `C:\ProgramData\MyRamDisk\logs\service.log`。

**剩余**：T3-05 客机通过 → T4（安装/升级/卸载 + T4-04 release manifest 冻结）。
---

### 3.24 2026-10-27 01:50：T3 全部完成（GUI 五段）

**T3-01**：客户端边界 —— `myrd_product_client`（线格式 + 编解码）只链 `myrd_product_ipc` +
`myrd_product_contracts`；`myrd_cli` 与 `myrd-gui` 的**链接闭包**已写入构建目录并断言
（`tools/check_client_link_closure.cmake`，**configure 期**，因为 `cmake -P` 没有 target 会空检查报绿），
并**已证明能失败**（拿 `myrd_service` 当被试会拒绝）。

**T3-02**：`myrd-gui.exe`（列表 + 托盘 + 3 秒刷新）。**`aggregate` 是唯一判定**，有测试钉住
（runtime=running + aggregate=blocked 的盘必须显示 blocked）。内存列 resident/budget，未运行时
显示 "not running" 而不是 0。

**T3-03/T3-04**：命令在独立线程发出（界面不假死）、进度只跟操作状态并在终态停止并刷新；
三处确认流（stop 仅在检查点未确认时、forget 需回显 uuid、`authorization_denied` 给指令）。

**T3-05（客机，请求 `96bf7e5f…`，20 s）**：
```text
self-check: 1 disk(s)
  T:  running  Running  generation 2  2.8 MiB / 1024.0 MiB
tray: MyRamDisk: 1 disk
window-check: created 0x370088, found 0x370088
  process 3512 is alive after 6 s
PASS T3-05
```

**顺带抓到的真实缺陷**：T1-10 的默认脏数据阈值 64 MiB **大于 32 MiB 盘的容量**，校验拒绝，
`disk create` 以 `terminal failure without a worker-action error` 结束。修法：未显式指定时
**钳到请求容量**（阈值大于容量没有意义）。

**T4 计划（下一步从这里开始）**：
- **T4-01** 安装器：打包 / SCM 注册 / ProgramData ACL / `phdskmnt` **只检查不安装**；卸载必须安全下盘；
- **T4-02** Load Only 模式、导出单文件 VHDX（复用 compaction 的 fold writer）、易失盘模式、卷标/簇大小/卷序列号；
- **T4-03** 在线 compaction（第二套 durable phase + reconciliation + 自动触发；journal 增长有界可观测）；
- **T4-04** M5-E：干净 Debug/Release、M4 回归、冻结哈希、证据组校验工具（负例必须非零退出且**永不打印 PASS**）、release manifest。
---

### 3.25 2026-10-27 01:55：T4-01 完成（安装/卸载走通），T4 剩下三段

**客机读数**（请求 `48750655…`，37 s）：`INSTALLED` → 服务 RUNNING 且 `disk list` 应答 → 目录 ACL
符合 → 卸载把盘安全下盘、删服务、删文件、**保留** durable 记录 → `-PurgeData` 再清 →
`PASS T4-01`。

**profile store 的三条 ACL 规则（都是被拒后才学到的，写在这里省下次的时间）**：
1. 记录目录里**不许出现任何非管理 SID**（`Users: RX` 也会被拒：`A management ACL entry lacks
   required access`）⇒ profiles/operations **仅 SYSTEM + Administrators，FILE_ALL_ACCESS**；
2. DACL 必须是 **protected**（`A profile directory ACL is not protected`）⇒
   `icacls /inheritance:r` **不够**，要用 `SetAccessRuleProtection($true,$false)` + `Set-Acl`；
3. **数据根目录本身也被检查**（三个子目录都保护后仍报同一句）⇒ 根也要同样保护。
   唯一例外是 `logs`：它可以给 Users 只读（store 不检查它），因为"为什么失败"必须能被读到。

**手法**：installed 服务的 stderr 被 SCM 丢弃，但 **`myrd-service.exe --console` 从安装目录跑**
就能把 store 的原话打出来——三次定位全靠这一招。

**T4 剩余**：T4-02（Load Only / 单文件 VHDX 导出 / 易失盘模式 / 卷标·簇大小·卷序列号）→
T4-03（在线 compaction）→ T4-04（M5-E + release manifest 冻结）。
---

### 3.26 2026-10-27 02:00：T4-02 的施工图（已勘察，未开工）

T4-02 一句话：**Load Only 模式、单文件 VHDX 导出、易失盘模式、卷标/簇大小/卷序列号**。
勘察结果（`grep` 过一遍代码，不是凭印象）：**四件里没有一件是"加个开关"**——每件都是一条
CLI → control → wire → intent → profile → worker/卷 的完整纵切，而 profile 字段还有版本与校验成本。

**已有可复用的东西**：
- VHDX / fold 机制在 `src/persistence/compaction_coordinator.{h,cpp}`（`CompactionArtifactBuildRequest`
  已经在造"单文件产物"）与 `src/m3/m3_provider.cpp`（15 处 VHDX 相关）；
- 卷序列号已经采到：`m5_worker_runtime.cpp:258` 的 `evidence.volume_serial =
  identity.value().volume_serial_number`，并且已经在 worker 协议里传输
  （`m5_worker_protocol.cpp:344/655`）——**这条只差在 `disk show` 里显示出来**，是全组里最便宜的一件。

**按成本从低到高排的施工顺序**（建议照这个顺序，每步都能单独绿）：
1. **卷序列号可见**：`disk show` 增加一行（视图字段已有 `volume_serial`），改 CLI renderer + 一个模型用例；
2. **卷标 / 簇大小**：`LifecycleOperationIntent` 加两个字段（`std::wstring volume_label`、
   `std::uint32_t cluster_size_bytes`）→ `control::DiskCreateCommand` + `ipc_codec` →
   CLI `--volume-label` / `--cluster-size` → `profile::Profile` 追加两个字段（**追加到末尾并更新
   codec/validation/`kFields`**，注意 `profile_types.h` 的字段 tag 已经用到 26）→
   `m3_provider.cpp` 的卷格式化调用带上它们；
3. **易失盘模式**：`Profile` 加 `bool volatile_mode`；语义写清楚（**不做检查点、停止即弃**），
   在 `profile_validation` 里与 checkpoint 策略互斥校验，`disk create --volatile` 传下来；
4. **Load Only**：`M3Provider::start` 增加只读选项（store 以只读打开，执行器拒绝 checkpoint/
   compaction/带写停止），`disk start --load-only`；`disk list` 的 `activity` 要能表示这个状态；
5. **VHDX 导出**：`disk export --to <file.vhdx>`，复用 compaction 的 fold writer 产出折叠镜像，
   再套上 `m3_provider.cpp` 既有的 VHDX 头；导出必须**在只读快照上做**，不能影响正在运行的盘。

**验收口径**（每件都要客机读数）：卷标/簇大小在 `disk show` 与 `fsutil fsinfo` 里一致；易失盘
断电后**没有** checkpoint 可恢复（这是它的定义）；Load Only 成功的盘 `disk list` 显示只读且
worker 只为读启动；导出的 VHDX 能被 `Mount-DiskImage` 挂上且根目录可读。
### 3.27 2026-10-27 02:05：T4-02 第 1 步的实测勘察（比 3.26 的估计多一层）

想把**卷序列号**显示出来，实际要动三层（`grep` 确认）：

1. `WorkerRuntimeObservation`（`service_core.h`）**没有** `volume_serial` —— worker 协议里
   **有**（`m5_worker_protocol.cpp:344/655` 的 `status.bundle->volume_serial`，来自
   `m5_worker_runtime.cpp:258` 的 `evidence.volume_serial`），但**观测结构本身没带上来**；
   ⇒ 追加字段并在 `windows_worker_observation` 里从 status 填；
2. `product::ServiceDiskView`（`service_view.h`）没有该字段 ⇒ 追加，并让
   `src/client/response_view.cpp` 的 encode/decode 各写/读一个 tag；
3. `project_profile`（`management/service_response_wire.cpp`）从 `snapshot.runtime_observation` 填，
   CLI renderer 打出来，配一个模型用例 + 一个编解码往返断言。

**关于协议版本**：客户端与服务**始终一起发布**（设计如此，没有版本协商常量），所以追加字段
**不需要** minor 号——这一点写在这里，免得下一个人以为要先加版本号而把它拆成两次发布。

**这一步之后**才是卷标/簇大小（真正要动 profile schema），再之后是易失盘、Load Only、VHDX 导出。
### 3.28 2026-10-27 02:15：卷序列号那次尝试被回退，原因记在这里

**做了什么**：`WorkerRuntimeObservation` 加 `volume_serial`、`windows_worker_observation` 从
`status.bundle->volume_serial` 填、`ServiceProfileProjection` / `ServiceDiskView` 各加一个字段、
`encode_profile` / `decode_profile` 各写读一个 u64、`project_profile` 填、CLI 打 `serial=1A2B-3C4D`。

**结果**：编译通过，但 `m5.cli` 与 `m5.service_host` 的**编解码往返用例**失败
（`decoded.ok()` 为假）。两端读写顺序我核对过是一致的（has_mount, resident, budget, **serial**,
mount_length），所以不是顺序问题；**需要先拿到 decode 的具体错误文本**（下一步：给
`decode_service_response` 的错误加一行 stderr，或用一个小 driver 直接调它）。

**处理**：`git checkout -- src/` **全部回退**，重建后三个套件恢复绿。理由：在拿不到 decode 错误
的情况下继续改，只会把"不知道哪里错"变成"不知道哪里错且树是红的"。

**下一步（照做即可）**：
1. 用一个小 driver 编码一个含 `volume_serial` 的响应再解码，把 `decode` 的 `error().operation` 与
   `message` 打印出来——一次就能定位；
2. 拿到原因后再重做这次改动（字段与顺序可以照 3.27 的清单，已经验证过编译）；
3. 该改动完成后 T4-02 的第 1 步就结束，再进卷标/簇大小。
### 3.29 2026-10-27 02:25：卷序列号第二次失败——**不是陈旧对象，是布局真的不对称**

这次用 `--clean-first` 全量重建（排除"增量构建留下旧编码器"的假设），失败**照旧**：
`decode failed: A profile projection is truncated`（`m5.cli` 与 `m5.service_host` 的往返用例）。

**已核对**：`encode_profile` 写 `6×u8, u64 generation, uuid, u8 has_mount, u64 resident,
u64 budget, u64 serial, u16 mount_length, text`；`decode_profile` 读完全相同的顺序。
两边逐字对称，却仍在"固定区"那组读取处判定截断 ⇒ 问题**不在我改的那 8 个字节**，而在
**响应层切给 profile 的 payload 长度**（`put_field` / 字段切片）或 `Reader` 的边界语义。

**下一步只有一条路（10 行代码，一次定位）**：在 `decode_service_response` 里**临时打印每个字段的
tag 与 length**，以及 `decode_profile` 收到 payload 的**首 8 字节十六进制**——立刻就能看出
`profiles` 字段的声明长度是否比实际写入少 8（或 `Reader::u64` 是否在末尾拒绝读）。

**已经回退两次了**，不要再盲改：这条支线在拿到字节级证据前不值得第三次尝试。
**替代路线**：把卷序列号放到**响应级字段**（像 `mount_target` 那样），完全避开 profile 的固定布局——
`disk show` 只需要一块盘，够用；`disk list` 的每行序列号可以后面再补。
### 3.30 2026-10-27 02:35：字节级读数到手——**编码端少了那 8 字节**

探针（临时打印，已随回退移除）在客机无关的本机测试里给出两个数：

- `probe: decoded profile bytes=51`、`probe: decode payload=51` —— profile 的 payload 声明长度是 **51**；
- `probe: encoded profile bytes=12` —— 从 `budget` 之前量到结尾，只有 **12** 字节
  （= budget 8 + mount_length 2 + mount 2）。

51 恰好等于 **不含 serial** 的布局：`6 + 8 + uuid(2+16) + 1 + 8 + 8 + 2 = 51`。
12 也恰好是 **不含 serial** 的尾部：`8 + 2 + 2`。**含 serial 时应是 59 与 20。**

⇒ **解码端在读 serial（所以判截断），编码端没写**。原因：那两次改动里，编码端的
`put_u64(out, profile.volume_serial);` 与解码端的 `reader.u64(volume_serial)` **不是同一次编辑
加进去的**，而我在中途回退过一次，于是出现"半套改动"——这正是两次失败的真正原因，不是布局。

**重做时的正确做法（一次编辑里同时加三处，然后用那两个数验收）**：
1. `encode_profile`：在 `put_u64(out, profile.budget_bytes);` 之后加
   `put_u64(out, profile.volume_serial);`（期望尾部 12 → **20**）；
2. `decode_profile`：在 `budget_bytes` 之后声明并读 `volume_serial`（期望 payload 51 → **59**）；
3. `ServiceDiskView`/`ServiceProfileProjection` 各加字段、`project_profile` 填、CLI 打 `serial=`。
### 3.31 2026-10-27 02:45：卷序列号这条线**通了**（本机绿），客机读数欠一次干净机

- `a0bd1e9`：序列号穿过观测/两个投影/线格式到 renderer，`disk list` 与 GUI 行都显示
  `serial=A1B2-C3D4`（没有卷时**不显示**，GUI 自检显示 `serial=-`）；新增模型用例钉住往返与渲染；
  `--clean-first` + 八套件全绿。
- **元凶**：`kProfileFixedBytes` 这个**最小 payload 长度**守卫数的是旧布局（`6+8+16+1+8+8+2`），
  常量注释里已写明它值两次失败。
- **客机读数没拿到**：`integration.m5.gui_workflow` 3.5 秒失败——客机上还留着 T4-01 装的
  `MyRamDiskService`，两个服务不能同时拥有公开管道。**先卸载/重启再跑**。
- **下一步**：干净客机跑一次 `gui_workflow`（顺带就是序列号的客机读数），然后进 T4-02 第 2 步
  （卷标 / 簇大小：intent → command → codec → CLI → profile 追加字段 → 卷格式化）。
### 3.32 2026-10-27 02:55：GUI 用例失败的真因找到（缺数据目录），改造半途回退

**真因**（客机 `--console` 探针一次拿到）：
```text
myrd-service: build_state failed: op=profile_store.open_directory
              msg=Failed to open a required profile directory native=2
```
`native=2` = 文件不存在。T4-01 的用例用 `-PurgeData` 把 `C:\ProgramData\MyRamDisk` **整个删掉**了，
而 `gui_workflow` 用的是**裸 `sc create`**，从不创建那些目录 ⇒ 服务启动即失败（exit 0、无日志、
`sc start` 还可能回 1053）。**不是产品缺陷**：store 的 fail-closed 行为是正确的，缺的是用例的前置条件。

**正确改法（已试，但本轮没收尾，已回退）**：让 `gui_workflow` **走发布的安装器**
（`tools/install/Install-MyRamDisk.ps1`，它建目录并给 protected DACL），二进制改从
`C:\Program Files\MyRamDisk` 取；清理也改走卸载器。**回退原因**：那串替换把一个 `}` 弄没了，
脚本语法错误——这类多块替换必须一次只改一处并用 `Parser::ParseFile` 逐步验证，
不要在一次命令里叠三处。

**下一步**（按顺序，每步都跑 Parser 验证）：
1. 在用例里加 `$installDir` 并让三个二进制路径指向它（改一处，验证）；
2. 用安装器替换"裸 sc create + 启动"整块（改一处，验证）；
3. 清理改走卸载器（改一处，验证）；
4. 客机跑一次——这一次同时给出**卷序列号的客机读数**（自检现在会打 `serial=`）。
### 3.33 2026-10-27 03:05：GUI 用例恢复绿，**卷序列号拿到客机读数**

```text
install through the shipped installer
self-check: 1 disk(s)
  T:  running  Running  generation 2  2.8 MiB / 1024.0 MiB  serial=5A42-9D28
UNINSTALLED
PASS T3-05
```
（请求 `1e26484b…`，22 s）

- 之前的失败真因已确认：用例用裸 `sc create`，**数据目录不存在** ⇒ 服务 fail-closed
  （`profile_store.open_directory`, native 2；exit 0 且无日志，所以像崩溃）。改走**发布安装器**后一次通过。
- **卷序列号 = T4-02 第 1 步完成**（客机读数 `serial=5A42-9D28`）。它来自 worker 自己的 bundle 序列号，
  经观测/两个投影/线格式到渲染，按 `vol` 的样式显示；`kProfileFixedBytes` 守卫已随之 +8（注释里写明它值两次失败）。

**下一步**：T4-02 第 2 步——**卷标 / 簇大小**（intent → command → ipc_codec → CLI
`--volume-label` / `--cluster-size` → profile 追加两字段 → `m3_provider` 卷格式化带上它们）。
### 3.34 2026-10-27 03:15：下一步的最小可执行清单（T4-02 第 2 步：卷标 / 簇大小）

已经勘察过，这里只列**按顺序要动的点**，每步都能单独编译通过：

1. `src/control/command.h`：`DiskCreateCommand` 加 `std::wstring volume_label;` 与
   `std::uint32_t cluster_size_bytes{};`（末尾追加）；
2. `src/control/ipc_codec.cpp`：选两个未用的 tag 写/读这两个字段（该文件里 create 命令的
   tag 表就在附近，按现有风格追加，严格解码：未知 tag 报错）；
3. `src/cli/cli_parser.cpp`：`--volume-label <名字>`、`--cluster-size <N|NK|NM>`（复用
   `product::quantity_parser` 解析字节数），写进 `DiskCreateCommand`；
4. `src/management/service_core.h` 的 `LifecycleOperationIntent` 加同名字段，
   `service_core.cpp` 的 `register_lifecycle` 路径把它们带进 profile；
5. `src/profile/profile_types.h` 的 `Profile` 追加两个字段（`ProfileFieldTag` 已用到 26，
   继续 27/28），同步 `profile_codec.cpp` 的写/读/字段计数与 `profile_validation.cpp` 的范围校验
   （簇大小只接受 4096/8192/…/65536；卷标 ≤ 32 字符且非空）；
6. `src/m3/m3_provider.cpp` 的卷格式化调用带上这两个值（AIM 的 format 请求里有卷标与簇大小）；
7. 测试：`m5.cli`（解析与渲染）、`m4.checkpoint_operation`（profile 往返）、
   以及一个**客机**用例断言 `fsutil fsinfo volumeinfo <盘符>` 的卷标与簇大小与配置一致。

**纪律提醒（这轮踩过的两个坑，别再踩）**：
- 改 `profile_types.h`（头文件）后**必须 `--clean-first`**，否则会出现"半套改动"的假失败；
- **一次命令只做一处块替换**，每步都跑 `Parser::ParseFile` / 编译验证——上一轮三处叠加丢了一个 `}`。
### 3.35 2026-10-27 03:25：3.34 清单的两处修正（勘察结果，别再走弯路）

1. **字段 tag 号已经有了确定值**：`ProfileFieldTag` 目前用到 35（compaction 那组），所以卷标与簇大小
   用 **36 / 37**，追加在 `automatic_compaction` 之后；`profile_codec` 的写/读与字段计数按现有风格同步。
2. **卷格式化不在 `m3_provider.cpp`**：该文件里只有 `journal_format.h` 与错误码，**没有** volume create /
   label / cluster 的调用点。卷的创建走 **AIM 代理层**（`src/proxy/`）与 worker 侧，
   所以 3.34 第 6 步应改成：**在 worker 的卷创建路径（AIM format 请求）里带上卷标与簇大小**，
   对应 `src/proxy/*` + `src/host/m5_worker_runtime.cpp` 的 create-device 动作。
   ⇒ 这一步比 3.34 估计的更靠下，**需要先确认 AIM 的 format 请求结构里是否有这两个字段**（如果没有，
   卷标只能在格式化后用 `SetVolumeLabel` 设置，簇大小则必须先确认再格式化——这决定了 CLI 选项是
   "创建时"还是"创建前"语义）。

**结论**：第 2 步真正的第一件事是**读 AIM 的 format 请求结构**，不是先改 profile——
否则会做出一个存得下、却改不了卷的字段。
### 3.36 2026-10-27 03:35：**T4-02 的"卷标 / 簇大小"需要先做一个范围裁定**

全树搜索的结果（不是猜测）：

```
SetVolumeLabel / volume_label / cluster_size / FMIFS / FormatEx / IOCTL_DISK_FORMAT
  → 0 处命中
```

**本产品没有任何格式化卷的代码路径**。而 M5 的盘确实有文件系统（e2e 能写标记文件、能挂盘符），
说明文件系统是**别处带来的**（AIM 驱动呈现的镜像本身带格式，或 M3 阶段生成的镜像带格式），
不是我们在运行时格式化的。

**由此产生一个真实的取舍**，T4-02 的这两个子项因此不是"加两个字段"：

- **卷标**：若卷已格式化，可在挂上之后用 `SetVolumeLabel` 设置——**不需要格式化**，成本小；
- **簇大小**：**只有格式化时才能定**。要在运行时支持它，必须新增一条**镜像格式化**能力
  （生成带指定簇大小的新镜像，或在 worker 侧调用 FormatEx）——这是一个**新功能**，量级远超
  一个 CLI 选项，而且会改动"创建一块盘"的语义（谁负责格式化、失败如何回滚）。

**建议的裁定（需要拍板，我按此执行）**：
1. **卷标**：按"创建参数 + 挂载后 `SetVolumeLabel`"实现，工作正常即有客机读数（`fsutil fsinfo volumeinfo`
   能看到），**这一项保留在 T4-02**；
2. **簇大小**：从 T4-02 移出，作为**独立增记为遗留项**（需要"镜像格式化"这一新能力）。
   理由：它不是选项，是能力；把它塞进 T4-02 会让"产品化"这一段变成一次未评估的架构改动。

在这条裁定之前，**不动 profile 字段**——先存字段再发现改不了卷，比不存更糟。
### 3.37 2026-10-27 02:55：整套客机普查**已发起，结果待收**

- 请求 **`29ada3b770e943a482878ba3ac64293d`**（`-Run` 无 filter，160 个用例），02:29 发起。
  主机侧等待 1800 s 超时**不会中断客机运行**，按提示用下面这条收：
  ```powershell
  .\tools\m5vm\Invoke-M5Vm.ps1 -Run -RequestId 29ada3b770e943a482878ba3ac64293d -MinTimeoutSeconds 600
  ```
- 同时一条普通 ssh 探测（读 `C:\m5\control` 与进程）也**挂住了**，符合本会话早前记录的
  "客机在长时间设备类运行中会变慢/挂住"的现象；**收结果前先确认客机能应答**，
  必要时硬重启（会话里多次用过 `vmrun stop hard` + `start nogui`，15 s 内可 SSH）。
- **这条普查是 T4-04 "M4 回归 + 干净构建" 的客机侧基线**：期望 158/160
  （`structural.*` 两例在客机上因 `vswhere.exe not found` 失败，属已知）。
### 3.38 2026-10-27 03:15：整套客机普查**跑不完**（42 分钟未结束）——记为待查项

- 请求 `29ada3b7…`（160 例，02:29 发起）到 03:11 **仍未结束**；T1-05 时代的整套读数约 10 分钟。
- 同时：**普通 ssh 探测在 120 s 内没返回**（`Get-Date` 这种最轻的命令也挂），而同一分钟一次
  `echo up` 曾成功——**客机在整套设备类运行期间会不可预测地变慢**，与本会话早前记录一致。
- **处置**：停止等待（不等于中断客机运行），把这条记为 **T4-04 的前置问题**：
  整套普查必须先能稳定跑完，否则"干净构建 + M4 回归"没有基线。
- **下一步（按成本）**：
  1. 硬重启客机，确认 `echo up` + `Get-Date` 都能秒回；
  2. **分组跑**而不是整套跑：`-Filter '^unit\.'`、`'^model\.'`/`'^m5\.'`、`'^integration\.'` 三组分别计时，
     找出是哪一组把时间拖到 40 分钟（e2e 三例本身就是分钟级，但不应是 40 分钟）；
  3. 若某条 e2e 卡住，用它的 `run-<id>/*.stdout.txt` 与 `C:\m5\runs` 下的日志定位。
- **注意**：`-Run -RequestId` 收结果这条路径本身是好的（本次它正确识别"已提交，不重复触发"），
  问题在客机。
### 3.39 2026-10-27 03:20：staging 现在**跳过载荷拷贝**（不是失败），已加一道诚实的守卫

- 客机硬重启后 `echo up` 15 s、`Get-Date` 1.3 s——**客机本身是好的**，之前的"变慢"是整套 160 例的
  长运行期间出现的，分组跑之前不再整跑。
- 但 `-Stage` 现在**不拷贝载荷**：本地打印 `staged 160 tests` 之后，**没有**出现以往那句
  `staged to usr@myrd-vm:…`，客机侧 `out\build\msvc\tests\Release` 里 **0 个 exe**。
  ⇒ 不是拷贝失败，是**拷贝那一步被跳过**了。
- 我已加一道守卫（`tools/m5vm/Invoke-M5Vm.ps1`）：**拷贝前先在客机建好 tests 目录**，
  这样"计数"这一步不会再抛 `DirectoryNotFound`（那会把问题伪装成编译失败），而是如实报
  "0 个落地"。本次它如实报了，这正是我们要的行为。
- **下一步**：读 `Invoke-M5Vm.ps1` 里"本地 staging → git metadata 拷贝"之间的那段，
  找出本该执行载荷拷贝的分支为什么没走（可能是某个条件/开关在上次改动后不再成立）。
  这条不修，任何客机测量都跑不起来。
### 3.40 2026-10-27 03:35：staging 仍未落地，且**客机上查树也没回话**

- 守卫生效后读数一致：本地 `staged 122 test binaries`、客机 `0`，报错原文就是
  `staging did not land: the guest has 0 test executables ... but 122 were staged`。
- 随后那条"Stage 后立刻查客机目录树"的 ssh 探测**没有任何输出**（不是报错，是空）——
  说明要么该会话没执行，要么客机又处在本会话记录过的"能 echo、不能跑多行脚本"状态。
- **下一步（按顺序，别跳）**：
  1. 用**最短**的编码命令确认客机可写：`New-Item -Force C:\m5\probe.txt` 再 `Test-Path`；
  2. 用**单行** `cmd /c dir /b "C:\...\m5-lifecycle"` 看客机 worktree 到底有什么（cmd 比多行 PowerShell 稳）；
  3. 若 `out` 根本不存在，先手工 `mkdir` 出 `out\build\msvc\tests\Release`，再跑 `-Stage`，
     看 scp 是否报错（目前 `Copy-ToRemote` 的 scp 退出码 0 但我们没有逐项打印，可疑）；
  4. 若仍 0，把 `Copy-ToRemote` 临时改成**每项打印目标路径与结果**，一次就能看出哪一项没到。
- **这条是当前唯一阻塞**：修好之前，任何客机测量（包括 T4-04 的 M4 回归）都无法进行。
### 3.41 2026-10-27 03:50：staging 的嵌套是 scp 语义问题，**建议改用 tar 直传**

三次实测（都在客机上用 `cmd /c dir` 验证过）：

1. `scp -r local\out remote\out` 在 `remote\out` **已存在**时会创建 `remote\out\out` ⇒ 载荷全落在
   `out\out\build\...`，而 runner 只看 `out\build\...`，于是"客机有 0 个"；
2. **`\.` 内容拷贝技巧在 Windows OpenSSH 的 scp 上不成立**：它对嵌套文件报
   `dest open ... Failure`（看起来像权限问题，其实不是）；
3. "先删目标再拷"也**没解决**（本次客机仍是 `out\out` + `out\build` 两棵树并存）。

**推荐做法（不要再用 scp 拷目录）**：
```powershell
# 本地
tar.exe -czf $payloadTar -C $stageDir .
# 传输一个文件
Copy-ToRemote -Local $payloadTar -Remote "$guestRepoScp/.m5-payload.tgz"
# 客机解包（tar 已在该机可用，git metadata  Extract 就是这么做的）
Invoke-Remote "& 'C:\Windows\system32\tar.exe' -xzf '$guestRepoScp/.m5-payload.tgz' -C '$guestRepoScp'; Remove-Item '$guestRepoScp/.m5-payload.tgz' -Force"
```
这与"git metadata 用 tar 传"的成功做法一致，且**把 scp 的目录语义从问题里彻底移除**。

**在当前状态**：staging 会被计数守卫拒绝（这是好事，不会让客机跑出误导性失败）；
`out\out` 残留需在客机上 `rmdir /s /q ...\out` 清一次。
### 3.42 2026-10-27 03:45：staging 修好了，但**agent 不接请求**

- `eb04118`：载荷改为**单个 tar**（本地打包 → 传一个文件 → 客机 `tar -xzf`），staging 立刻恢复：
  `staged to usr@myrd-vm:…` 这行只在客机计数与本地一致后才打印。三条 scp 语义坑的经验已写进代码注释。
- 但用 `-Run -Filter '^unit\.'`（请求 `b8c62798…`）后 **10 分钟没有结果**。客机侧用 cmd 查到：
  - `C:\m5\control` 里**有** `b8c62798….request.ps1`（请求已提交）；
  - `cmd /c echo probe-ok` **秒回**（客机、ssh、cmd 都正常）；
  ⇒ **计划任务里的 agent 没去取这个请求**。最可能是之前那次 42 分钟的整套运行留下的 agent 实例
    还占着（它的锁/互斥），或者任务的触发器坏了。
- **下一步**：
  1. 用**单行、不含空格参数**的 cmd 查任务：`schtasks /query /fo csv` 再本地过滤（不要在同一条 ssh 里塞
     `findstr "a b"`，Windows 会把引号吃掉）；
  2. 看 `C:\m5\agent` 下 agent 自己的日志（它在里面写执行记录）；
  3. 若确认卡住：`schtasks /end /tn <任务名>` 再 `/run`，或直接重启客机（本会话多次证明 15 s 可恢复）。
- **纪律**：Windows 的 ssh 命令里**引号与空格会被吞**，多参数一律改用 `-EncodedCommand`；
  这次连 `tasklist /fi "..."` 都被拆坏，浪费了两轮。
### 3.43 2026-10-27 04:15：**VM harness 的 agent 停摆**（当前唯一阻塞）

证据（都在客机上取到，客机本身健康：硬重启后 `echo up` 15 s、`Get-Date` 1.3 s）：

- `C:\m5\control\m5-agent.log` 尾部：
  ```text
  2026-10-26T18:17:05 request 1e26484b… finished exit=0     ← 最后一次成功
  2026-10-26T18:19:35 agent start elevated=True drain=True
  2026-10-26T19:19:36 agent start elevated=True drain=True
  2026-10-26T19:53:51 agent start elevated=True drain=True
  ```
  ⇒ 18:19 之后**每次 agent 都只是"start"，再没有任何 "finished"**；中间那次整套 160 例的运行
  （`run-29ada3b7…`）到 04:03 还在写目录。
- 我杀掉了那个卡住的 runner（`pid=4712`，`Invoke-M5Tests`），**请求仍然不被处理**：
  `67f5bf1c…`（单个 `m5.gui` 用例）10 分钟无响应。
- ⇒ 需要**重建计划任务/agent 状态**，不是等一等就好。

**下一步（重建，按顺序）**：
1. `schtasks /query /fo csv` 找到 agent 任务名（本轮那条命令被 ssh 引号吃掉，**必须用 `-EncodedCommand`**）；
2. `schtasks /end /tn <名>` → `schtasks /delete /tn <名> /f` → 用 `tools/m5vm/Invoke-M5Vm.ps1 -Probe`
   里记录的注册命令**重新注册**（该脚本 `-Probe` 会打印三条通道与任务名）；
3. 清 `C:\m5\control\*.request.ps1` 残留，再跑一个单例（`^m5\.gui$`）确认 <60 s 返回；
4. 确认后再分组跑套件，恢复"每项以客机实测为准"。

**注意**：整套 160 例在本机跑不完（42 分钟未完）+ agent 单实例 ⇒ **以后一律分组跑**，
并在跑之前清残留请求。
### 3.44 2026-10-27 04:20：agent 恢复，**分组基线到手**（42 分钟那次是 agent 卡住，不是套件慢）

**恢复手法（有效，记住）**：
```powershell
# 客机上，EncodedCommand（ssh 会吞引号）
schtasks /end /tn 'MyRamDisk-M5Agent'        # 任务名见 -Probe 输出，本机为 MyRamDisk-M5-Agent
Get-ChildItem C:\m5\control -Filter '*.request.ps1' | Remove-Item -Force
```
之后一个单例 `^m5\.gui$` **5 s** 返回 1/1。

**分组读数（客机，Release）**：

| 组 | 用例数 | 用时 | 结果 |
|---|---|---|---|
| `^unit\.` | 96 | **210 s** | **95/96**，失败：`unit.m4.profile_codec` |
| `^model\.|^m5\.` | 13 | **128 s** | **12/13**，失败：`m5.production_execution_backend`（**已知在批内间发**，单独跑通过） |
| `^integration\.` | 51 | 未跑 | 待跑 |

⇒ **整套 42 分钟"跑不完"的真相是 agent 单实例被上一次运行卡住**，不是套件本身慢。
以后**分组跑**（unit 3.5 分钟 + model/m5 2 分钟 + integration 另算）。

**两个待查**：
1. `unit.m4.profile_codec`（本轮新出现，unit 组里唯一红）——很可能与 T1-10 的默认值或我今天的
   profile 相关改动有关，**下一步先看它**（它的失败信息会指出哪个字段/字节）；
2. `m5.production_execution_backend`——已知间发项，单独复跑确认。
### 3.45 2026-10-27 04:35：`unit.m4.profile_codec` 的红是**默认值改动的正确后果**

- 本机复现：`encoded.value() == minimal_golden()` 失败（不是客机问题）。
- 原因：T1-10 改了 `CheckpointPolicy` 默认值（interval 300 → **60**、dirty 阈值关 → **开 64 MiB**），
  而 `minimal_golden()` 是**字节级黄金样本**——默认值一变，编码字节当然变。**测试是对的，样本要更新。**
- **新的黄金字节**（从失败运行里打印出来的，可直接粘贴）在下一行；替换
  `tests/unit/m4_profile_codec_tests.cpp` 里 `minimal_golden()` 的 `from_hex(...)` 参数即可：

```text
4d595244505246000100000048000000fa010000000000000700000000000000b20100002300000097587e8adf5343a0f8c597c13bc0faa38748e8b56c68c931834bccca8fe77c42010001011000000000112233445566778899aabbccddeeff0200020108000000532d312d352d313803000201010000004104000501080000000068e5cf8b0100000500030101000000000600020104000000443a5c4207000201080000006469736b2e72646d0800010110000000102132435465768798a9bacbdcedfe0f090005010800000000000000020000000a00040104000000000200000b00040104000000001000000c000701000000000d000701000000000e000701000000000f00030101000000001000020102000000523a1100060101000000011200030101000000001300050108000000000000400000000014000501080000000000003800000000150005010800000000000030000000001600050108000000000000080000000017000601010000000118000501080000003c000000000000001900060101000000011a0005010800000000000004000000001b00060101000000011c00030101000000011d00030101000000011e00030101000000001f000601010000000120000401040000004600000021000401040000003200000022000401040000004000000023000401040000000a000000
```

- **这一项对 T4-04 有直接影响**：T4-04 要求"干净 Debug/Release"，而当前 unit 组有 1 红；
  更新黄金样本后 unit 应为 **96/96**，那是 T4-04 的基线。
- 备选做法（若不想硬编码）：把 `minimal_golden()` 改成"首次运行生成并比对 sha256"，但那会失去
  "字节级契约"的意义——**建议仍然更新明文黄金样本**，并在提交信息里写明"因 T1-10 默认值变更"。
### 3.46 2026-10-27 05:10：客机基线两段到手，integration 组仍在跑

- **`^unit\.`：96/96 通过（227 s，请求 `b37eed00…`）** —— 这是 T4-04 干净构建的第一段基线。
  之前那次 `0/96` 是**假失败**：上一次 `-Stage` 其实失败（被 `Select-Object -Last 1` 吞掉），
  `-Run` 跑在无二进制的树上，全报 `MISSING`。**纪律：`-Stage` 输出必须看到 `staged to usr@` 才算成。**
- **`^model\.|^m5\.`：12/13（128 s）**，唯一红 `m5.production_execution_backend`（已知批内间发）。
- **`^integration\.`**：请求 **`6f8fe4298c9149aab96fef4998e763c4`**，04:52 发起，25 分钟仍未回。
  按之前的经验，很可能是某条 e2e 卡住（`integration.m5.powerloss_verify` 在没有断电前置时会等 180 s，
  但 25 分钟意味着别的）。收集方式同前：
  ```powershell
  .\tools\m5vm\Invoke-M5Vm.ps1 -Run -RequestId 6f8fe4298c9149aab96fef4998e763c4 -MinTimeoutSeconds 600
  ```
- **下一步**：收这条结果；若卡住，把 integration 组再细分
  （`^integration\.m5\.`、`^integration\.m3\.`、`^integration\.m4\.`、其余），逐组计时——
  这正是 T4-04 "M4 回归"要用的分组口径。
### 3.47 2026-10-27 05:20：integration 组**卡住**（30+ 分钟），按计划细分

- 请求 `6f8fe429…`（`^integration\.`，51 例）自 04:52 起 **30 分钟未完成**，主机侧两次收集都超时。
- 期望时间约 12 分钟（`cli_e2e` ≈6、`powerloss_prepare` ≈1、`powerloss_verify` ≤3、`install_uninstall` ≈1、
  `gui_workflow` ≈0.5），所以这是**卡住**而不是慢。
- **下一步（照 3.46 的计划细分，逐组计时）**：
  ```powershell
  -Filter '^integration\.m5\.'      # 5 例，含 cli_e2e / powerloss / install / gui
  -Filter '^integration\.m3\.'
  -Filter '^integration\.m4\.'
  -Filter '^integration\.m1\.|^integration\.m2\.'
  ```
  卡在哪一组就单独跑那一条，并用它的 `C:\m5\control\run-<id>\*.stdout.txt` 与 `C:\m5\runs` 的日志定位。
- **注意**：`integration.m5.powerloss_verify` **单独跑必然失败**（它需要主机先断电），
  所以 m5 组的"预期"是 4 过 1 失败，不是全绿；这一点要写进 T4-04 的基线口径，否则会被误判为红。
### 3.48 2026-10-27 05:50：integration 组卡住的真因是**客机残留**，不是产品

`cli_e2e` 本次只跑了 3 秒，它的 stdout 说明了一切：

```text
service pid 3584 is answering: disk.list: completed
b4e5bd61-d856-4e67-85bc-50f7fd8380fa  T:  Ready  stopped/configured
        runtime=Stopped  activity=Idle  persistence=Dirty  health=Healthy
        generation=31  mem=0/0
1 disk(s)
```

客机上**留着一块之前运行没清掉的盘**（`T:`、generation 31、`persistence=Dirty`），
并且还有一个 **03:54 启动的 `myrd-m5-worker` 孤儿进程**仍在。于是 e2e 在"已经有盘"的机器上建盘、
做预检、等待终态，行为和干净机上完全不同——`powerloss_prepare` 迟了 30 分钟才开始，也是同一台机器
被这些残留拖住的表现。

**⇒ integration 组（以及任何 e2e）的前置条件必须写成脚本，不能靠"上次跑完应该清了"**：
```powershell
# 客机上，EncodedCommand
Get-Process myrd* -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($d in 'C:\ProgramData\MyRamDisk\profiles','C:\ProgramData\MyRamDisk\operations') {
  if (Test-Path $d) { Get-ChildItem -Force $d | Remove-Item -Recurse -Force }
}
sc.exe query MyRamDiskService > $null 2>&1; if ($LASTEXITCODE -eq 0) { sc.exe stop MyRamDiskService; sc.exe delete MyRamDiskService }
```
**下一步**：把这段做成 `tools/m5vm/Reset-M5Guest.ps1`（或 `Invoke-M5Vm.ps1 -Reset`），
**每次跑 e2e 组之前自动执行**，然后再跑 `^integration\.m5\.`。这样 integration 组的读数才有意义。
### 3.49 2026-10-27 06:00：`Reset-M5Guest.ps1` 落地，客机已干净

- `tools/m5vm/Reset-M5Guest.ps1`：停已知服务 → 杀 worker/service 进程 → **若还有 worker 存活或记录目录非空就
  `exit 1` 拒绝**（"拒绝"比"继续测"重要：脏机上的读数测的是机器）→ 清 `profiles`/`operations` →
  报告挂载卷（保留 `logs`，因为下一次失败的诊断要对着它读）。
- **实测**：重启前它拒绝了（`1 worker(s) survived`——那个 03:54 的 worker 卡在驱动调用里，
  只能重启才能消失）；重启后一次通过：`profiles 0`、`operations 0`、`mounted volumes: C:`、`RESET`。
- **用法**：客机上
  `powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1`
  （脚本已 scp 到 `C:\m5\`；仓库里是 `tools/m5vm/Reset-M5Guest.ps1`）。
- **下一步**：干净机上跑 `^integration\.m5\.`（5 例，预期 4 过 + `powerloss_verify` 单独必失），
  拿到第一个可信的 integration 读数；然后依次 `^integration\.m3\.`、`^integration\.m4\.`、其余。
### 3.50 2026-10-27 06:25：**干净客机上 `^integration\.m5\.` 也卡住**（30 分钟）

- 客机：硬重启 + `Reset-M5Guest.ps1` → `profiles 0 / operations 0 / 只有 C:`，staging 也确认成功
  （`staged to usr@`），然后请求 `2e2450f5…`（5 例）**30 分钟未完成**。
- 期间一条普通的 EncodedCommand 探测（列 `run-2e2450f5…` 目录 + agent 日志 + 进程）**也挂住**
  ⇒ 客机在跑这条 e2e 时又变得不可交互（本会话记录过的现象）。
- **结论**：这不是脏机（已排除），是**某一例 e2e 在干净机上卡死**。必须逐例隔离：
  ```powershell
  -Filter '^integration\.m5\.cli_e2e$'        # 单独跑，它在 05:17 那次只要 3 s 就失败过
  -Filter '^integration\.m5\.install_uninstall$'
  -Filter '^integration\.m5\.gui_workflow$'
  -Filter '^integration\.m5\.powerloss_prepare$'
  ```
  每例跑完先 `Reset-M5Guest`。**`powerloss_verify` 永远单独看**（需要主机断电）。
- **注意**：`cli_e2e` 在 05:17 那次 3 秒失败时，客机上有残留盘（已解释）；现在干净机上它是"卡住"还是
  "失败"未知——**先单独跑它**，并用 `C:\m5\runs\<work-root>.*.log` 与服务日志判断卡在哪一步。
### 3.51 2026-10-27 06:45：**`cli_e2e` 单独跑也卡住 —— 这是回归，不是客机问题**

- 客机：硬重启 → `Reset-M5Guest` → `RESET`（`mounted volumes: C:`），staging 成功，然后
  请求 `b4c523cc…`（**只跑 `cli_e2e` 一例**）**15 分钟未完成**；它的基线是 **6 分钟**，
  最近一次通过是 04:20 前后的 `gui_workflow`/其它 e2e。
- **时间线**（今天最后几小时的改动，全部只有 unit/model 覆盖，没有 e2e 覆盖）：
  1. 卷序列号纵切（`WorkerRuntimeObservation` / 两个投影 / 线格式 / renderer）；
  2. `Uuid()` **默认构造改为 public**（为了让线格式结构可逐字段构造）；
  3. `kProfileFixedBytes` +8；
  4. 两个测试文件的修正（黄金样本、校验前提）——纯测试。
- **最可疑的是第 2 条**：把一个"构造必须有身份"的类型放宽成可默认构造，**nil uuid 现在可以流进
  任何逐字段构造的结构**（例如请求 id、profile id）。若某处现在发的是 nil，服务的等待/寻址行为会
  与之前不同——这与"卡住而不是报错"的表现相符。
- **下一步（按成本）**：
  1. 在 `cli_e2e` 的失败点看服务日志：`C:\m5\runs\<work-root>.service.log`（每次迭代都会归档）；
     若最后一行停在某个 `action-start` 或 `checkpoint` 前，就能直接定位；
  2. 若确认与 nil uuid 有关：给 `Uuid` 加一个 `bool is_nil()` 并在**发出请求前**断言非 nil
     （或在 `build_command_request` 里拒绝 nil request id），把"可默认构造"的放宽重新收窄；
  3. 若与序列号字段有关：把该字段临时改回不发送，重跑 `cli_e2e` 二分。
- **纪律重复一次**：**unit/model 全绿不能替代一次 e2e**——这次改动都有单元覆盖，但 e2e 卡住了。
### 3.52 2026-10-27 06:50：**卡住的不是 e2e，是 agent 在"排空请求"里挂住**

客机 `m5-agent.log` 尾部（本地时间 05:17 / 05:49 / 06:25 三次）：

```text
2026-10-26T21:17:48 agent start elevated=True drain=True once=False request=
2026-10-26T21:49:55 agent start elevated=True drain=True once=False request=
2026-10-26T22:25:00 agent start elevated=True drain=True once=False request=
```
**只有 "agent start"，没有任何 "request … finished" / "drain complete"**；而 `C:\m5\control` 里
**三个请求文件都还在**（05:17、05:49、06:24）。跑目录 `run-b4c523cc…` **根本没被创建**。

⇒ 这一小时所有"卡住"（含 `cli_e2e` 15 分钟）**不是 e2e 卡、也不是产品卡**，而是
**agent 每次启动后在排空请求的过程中挂住**，请求因此永远不被执行。之前那些"integration 卡 30 分钟"
很可能也是同一个原因（而不是我 3.48/3.51 里写的产品回归——**那两条结论要收回**）。

**下一步（按成本）**：
1. 手工在客机上跑一次 runner，看它第一步做什么、停在哪：
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\agent\Invoke-M5Tests.ps1 -Filter '^m5\.gui$'
   ```
   （它可能会先做 `ctest --show-only`、`git` 或枚举 `tests.json`；**git 是最可疑的**：
   客机 worktree 的 `.git` 是**指向主仓的指针**，而这次 staging 重解了 git metadata）
2. 若确认卡在 git：把 runner 里对 git 的调用去掉或加超时（测试运行不需要版本信息）；
3. 修好后 `Reset-M5Guest` → 清 `C:\m5\control\*.request.ps1` → 再跑单例验证 <60 s。
### 3.53 2026-10-27 07:00：**runner 本身没问题**；是队列卡住 + 我一个无效探测

- **无效探测（先纠正自己）**：我第一次手工跑 runner 时**没给 `-RequestId`**，而它是
  `[Parameter(Mandatory)]` ⇒ PowerShell 停在交互提示上，60 秒"无输出且存活"——
  **那不是挂住，是在等输入**。带 `-RequestId probe-0001` 重跑：**`1 of 1 passed`，45 秒内完成**，
  并生成了 `run-probe-0001` 目录。
- **队列证据**：`C:\m5\control` 里三个请求文件都在，内容也**合法**（例如
  `& 'C:/m5/agent/Invoke-M5Tests.ps1' … -RequestId 'b4c523cc…' -Filter '^integration\.m5\.cli_e2e$' …`），
  但 `run-b4c523cc…` **从未被创建** ⇒ **那条请求根本没开始跑**。
- ⇒ 真正的原因是**agent 的排空循环卡在最老的一条请求上**（`18489f0e…`，05:17 的 m5 组），
  后面的请求永远排队。

**下一步（一次就够）**：
1. 客机清队列：`Get-ChildItem C:\m5\control -Filter '*.request.ps1' | Remove-Item -Force`；
2. `schtasks /end /tn 'MyRamDisk-M5-Agent'` → `schtasks /run /tn 'MyRamDisk-M5-Agent'`；
3. `Reset-M5Guest`；
4. **只提一个短请求**（`-Filter '^m5\.gui$'`）验证 <60 s，再提 `cli_e2e` 单例；
5. 若 `cli_e2e` 单例仍超过 10 分钟，**那才**是产品侧要查的问题——届时用本次已验证可用的手工 runner
   （带 `-RequestId`、加 `-TraceTestCases`）直接看它停在哪一例。
### 3.54 2026-10-27 07:05：队列修好后 **`cli_e2e` 的真实失败暴露出来**（这是可查的产品症状）

- 清队列 + `schtasks /end` + `/run` + `Reset-M5Guest` 之后：短请求 `^m5\.gui$` **5 s 通过**；
  `cli_e2e` **126 s 跑完（不再卡）并失败**。⇒ 之前一小时所有"卡住"都是**队列**，与产品无关。
- 失败原因（客机 `last-service.log` 原文）：
  ```text
  action=create-bundle code=4107 native=0 step=WorkerPipeChannel::read
  detail=pipe transfer exceeded its bounded wait
  WARNING: 1 worker process(es) survived the run
  ```
  ⇒ **`create-bundle` 等 worker 的私有管道超时**：worker 要么没起来、要么没在预算内应答；
  并且**留下 1 个 worker 进程**。
- **下一步（按成本）**：
  1. 看 `C:\m5\last-trace.log`（服务与 worker 共写的步骤轨迹）——它会指出 worker 停在哪个动作之前的哪一步；
  2. 单独跑一次 `myrd-m5-worker.exe` 看它能否独立启动（它需要 job object + 私有管道名参数）；
  3. 这类"worker 不应答"在本会话早期（T1-04/T1-05）出现过，`m5-lessons.md` 里可能有同源记录，
     先对照再动手；
  4. 若与今天的改动相关：**最先怀疑 `Uuid()` 默认构造放宽**（nil uuid 流进 worker 命令 → worker 拒绝/挂），
     验证方法是把该放宽**临时收回**（给需要默认构造的三个线格式结构改用 `std::optional<Uuid>`）
     再跑 `cli_e2e`。
### 3.55 2026-10-27 07:10：`create-bundle` 超时的**根因在 worker 的候选盘枚举**

`C:\m5\last-trace.log` 尾部（时间戳是同一台客机上的连续事件）：

```text
22:48:17.249Z enumerate_disk_candidates: opening the candidate
22:50:17.137Z opstore.write: waiting for gate          ← 正好 120 s 之后
```

⇒ worker 在 **`enumerate_disk_candidates` 打开某个候选盘时阻塞了整整 120 秒**（正好是那个
`pipe transfer exceeded its bounded wait` 的预算），期间不响应 `create-bundle`，于是服务判超时、
留下 1 个 worker。

这不是新现象：`m5-lessons.md` 里记过"打开每一个候选盘去问盘号，在设备正在退役的窗口里会阻塞约
11 秒"（G4 延后的理由之一）。**这次是 120 秒**，因为客机上还有**上一次 `cli_e2e` 留下的 AIM 代理设备**
——一个已经半死、又没被移除的设备会让 `CreateFile` 长时间挂住。

**两条路，建议都做**：
1. **客机侧（立即）**：`Reset-M5Guest.ps1` 增加"移除残留 AIM 设备"的能力（或在 e2e 前重启客机）。
   目前它只清 profile/operation 记录与进程，**设备还在**。
2. **产品侧（T4 应有）**：`enumerate_disk_candidates` 对**每个候选**的开盘操作加**有界超时**
   （例如 2 s），并把超时的候选记为"跳过"而不是让整个枚举挂住——一个第三方的坏设备**不应该**
   让我们的启动停 120 秒。这条应记入 T4 的遗留清单（属于"产品在恶劣设备环境下的健壮性"）。

**验证方法（便宜）**：客机重启 → `Reset` → 单独跑 `cli_e2e`；若通过，则确认根因是残留设备而非代码。
### 3.56 2026-10-27 07:15：**`cli_e2e` 通过（72 s）——"回归"撤回，根因是残留 AIM 设备**

- 客机：硬重启 → `Reset-M5Guest` → 单独跑 `cli_e2e` ⇒ **1 of 1 passed，72 s**（请求 `2f749544…`）。
- ⇒ **3.51 里"产品回归"的怀疑正式撤回**：今天的改动没有破坏 e2e。
- 真正的因果链（现在完整）：
  1. 上一次 e2e 留下**半死的 AIM 代理设备**；
  2. 下一次 e2e 的 worker 在 `enumerate_disk_candidates: opening the candidate` 上**阻塞 120 秒**；
  3. 服务的 `create-bundle` 等到管道超时（`pipe transfer exceeded its bounded wait`），判失败并留下 worker；
  4. 该 worker 又让客机变慢/不可交互，**看起来像产品卡死**。
- **⇒ e2e 的硬前置：每次 e2e 之前必须"干净启动"**（硬重启 + `Reset-M5Guest`）。
  这也解释了本会话反复出现的"跑几轮之后就不行了"。
- **待办（T4 遗留清单）**：`enumerate_disk_candidates` 对每个候选盘的开盘加**有界超时**（如 2 s），
  超时记为"跳过"——第三方坏设备不该让启动停 120 秒。这条与 G4 延后的理由是同一个根。

**当前可信基线（客机，干净启动）**：`unit 96/96`、`model/m5 12/13`、`integration.m5.cli_e2e PASS 72 s`。
`integration` 其余 4 例仍待跑（每次跑前重启+Reset）。
### 3.57 2026-10-27 07:20：**候选盘枚举加界生效 —— 连续两次 e2e 不需要重启**

| 运行 | 用例 | 用时 | 结果 | 请求 |
|---|---|---|---|---|
| 第 1 次（刚重启+Reset 后） | `integration.m5.cli_e2e` | **72 s** | **PASS** | `d4713c4e…` |
| 第 2 次（**不重启**，同一台机） | `integration.m5.cli_e2e` | **85 s** | **PASS** | `d82c88a0…` |

改动（`b1faaa3`）：`physical_drive_locator.cpp` 里三处仍调用**无界** `enumerate_disk_candidates()` 的
地方（其中一处就在 worker 的启动路径上）改为调用已有的 `enumerate_disk_candidates_within(kEnumerateTimeout)`
——那个包装函数自己的注释写的就是"runs it with a bound, because it has none of its own"。

**意义**：之前"跑一次 e2e 之后必须重启客机"的限制**消失了**。一个半死的第三方设备最多让枚举
多等 10 秒（被跳过），而不是让 `create-bundle` 等 120 秒后失败、再留下一个 worker 把整台客机拖慢。
这同时消除了本会话反复出现的"跑几轮就不行了"的主要来源。

**当前可信基线（客机）**：
- `^unit\.` **96/96**（227 s）
- `^model\.|^m5\.` **12/13**（唯一红为已知批内间发 `m5.production_execution_backend`）
- `integration.m5.cli_e2e` **连续两次 PASS（72 s / 85 s）**
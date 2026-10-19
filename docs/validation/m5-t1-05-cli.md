# T1-05 鍝嶅簲渚?+ CLI锛屼互鍙?T1-04 鐨勯獙鏀惰鏁?
**鐘舵€?*锛?- **T1-04 鈫?PASSED**锛堝鏈哄疄娴嬶細create 杩涜涓?20 娆?`disk list`锛屾渶澶?**111 ms**锛岄檺鍊?1000 ms锛夈€?- **T1-05 鈫?浠?PARTIAL**锛氬搷搴斾晶銆丆LI銆佸崗璁€佸紑鍙戞満娴嬭瘯**鍏ㄩ儴瀹屾垚骞堕獙璇?*锛?  **瀹㈡満绔埌绔湪 `create` 鐨勬墽琛岄樁娈靛け璐?*锛屼笖**澶辫触鍘熷洜褰撳墠鏃犳硶鍙栧緱**锛堣 搂7锛岃繖鏈韩鏄竴涓己闄凤級銆?
鏈枃璁板綍锛氬缓浜嗕粈涔堛€佹€庝箞楠岀殑銆侀€斾腑鍙戠幇骞朵慨鎺夌殑涓や釜鐪熺己闄枫€佸鏈鸿鏁般€佷互鍙婁笅涓€姝ャ€?
---

## 1. 杩欎竴姝ヤ箣鍓嶇己浠€涔堬紙宸叉牳瀹烇紝涓嶆槸澶嶈堪锛?
`docs/validation/m5-t1-05-scope.md` 宸叉妸杈圭晫鍐欐竻锛氬懡浠ら潰锛堢被鍨嬨€佺紪瑙ｇ爜銆佸抚銆佷紶杈撱€佹巿鏉冦€?`disk list` 澶勭悊鍣級**榻愬叏**锛岀己鐨勬槸
**(a) 鍛戒护 鈫?璇锋眰甯?*锛堝凡鐢?part 100 鐨?`build_command_request` 琛ヤ笂锛夈€?**(b) 鍝嶅簲 鈫?缁撴瀯鍖栫粨鏋?鈫?杈撳嚭 + 閫€鍑虹爜**銆?**(c) CLI 鍙墽琛?*銆?**(d) 鏈嶅姟绔妸 pipe 鎺ュ埌 core**銆?
鏈疆瀹炴祴鍙堝彂鐜颁袱鏉?*鏇寸‖**鐨勭己鍙ｏ紝瀹冧滑閮戒笉鏄?浠诲姟娓呭崟閲屽啓浜嗘病鍋?锛岃€屾槸**娌′汉鍐欒繃鐨勬帴缂?*锛?
| 缂哄彛 | 璇佹嵁 |
|---|---|
| **`PublicPipeServer` 鍦ㄧ敓浜ч噷浠庢湭琚疄渚嬪寲** | `git grep PublicPipeServer src` 鍙懡涓畠鑷繁鐨?`.h/.cpp`锛涙病鏈変换浣?handler 琚彁渚涜繃 |
| **`ServiceCore::dispatch` 鍙娴嬭瘯璋冪敤** | `git grep "\.dispatch(" src` 闆跺懡涓紱浜у搧閲屾病鏈変换浣曡姹傝兘璧板埌 core |
| **娌℃湁浠讳綍鍝嶅簲缂栬В鐮?* | `git grep "encode_response\|decode_response\|service_response_wire" src` 闆跺懡涓?|

鈬?**`public pipe 鈫?ServiceCore` 杩欐潯"鍞竴鎵ц閾捐矾"锛堜笉鍙橀噺 1锛夋鍓嶅彧瀛樺湪浜庢枃妗ｉ噷銆?*

---

## 2. 寤轰簡浠€涔堬紙鎸変緷璧栭『搴忥級

### 2.1 鍝嶅簲鏂囨。锛歚src/management/service_response_wire.{h,cpp}`

**杩欐槸"瀹㈡埛绔彲浠ョ煡閬撲粈涔?鐨勫敮涓€鍐冲畾鐐?*锛屾墍浠ュ畠鍚屾椂鏄姇褰变笌缂栬В鐮侊細

- `ServiceResponseWire`锛歚request_id` / `command` / `disposition` / `operation_id` /
  `operation` / `profiles` / `warnings` / `error` / `mount_target`锛?- `project_profile()`锛歚ManagedProfileSnapshot` 鈫?`ServiceProfileProjection`锛?  **`aggregate` 鐢ㄦ湇鍔＄鍚屼竴鏉¤鍒欑畻**锛坄aggregate_status(reported_profile_state(...))`锛夛紝
  鎵€浠ュ鎴风涓嶅彲鑳界畻鍑轰竴涓笌鏈嶅姟绔笉鍚岀殑鍒ゅ喅锛?- `encode_service_response()` = 鎶曞奖 + 缂栫爜锛沗decode_service_response()` 涓ユ牸瑙ｇ爜銆?
**鎶曞奖鏄湁鎰忔湁鎹熺殑**锛屼笖杩欎竴鐐瑰啓杩涗簡娉ㄩ噴锛氫笉鎼哄甫 bundle 璺緞銆乯ournal銆乷peration gate銆?checkpoint correlation銆?*`mount_target` 淇濈暀**锛堢敤鎴烽潬鐩樼璁ょ洏锛屼笖瀹冩鏄?`disk list`
蹇呴』鏈夌殑涓€鍒楋級鈥斺€斾负姝ょ粰 `ServiceProfileProjection` **杩藉姞**浜?`mount_target`锛堝甫榛樿鍊硷紝
涓嶆敼鍙樹换浣曟棦鏈夎仛鍚堝垵濮嬪寲锛夈€?
**涓ユ牸鎬?*锛堜笌鍛戒护缂栬В鐮佸悓绾э級锛氭湭鐭?tag銆侀噸澶?tag銆佸瓧娈垫埅鏂€佽鏁拌秺鐣屻€佹灇涓捐秺鐣屻€?灏鹃殢瀛楄妭銆佹枃妗?major 涓嶈璇嗐€乵inor 楂樹簬璇诲彇鑰?鈬?**鍏ㄩ儴鎷掔粷**锛屼笉鏄拷鐣ャ€?娴嬭瘯涓庡疄鐜板悓鏃惰瘉鏄?鎷掔粷"杩欎欢浜嬶紙瑙?搂4锛夈€?
### 2.2 绔偣锛歚src/service/public_command_endpoint.{h,cpp}`

`Frame` 鈫?`ServiceCommandRequest` 鈫?`dispatch` 鈫?缂栫爜 鈫?`Frame`銆?*姣忎竴澶勫彇鑸嶉兘鍐欏湪澶存枃浠堕噷**锛?
| 鍐冲畾 | 鐞嗙敱 |
|---|---|
| **caller 鏉ヨ嚜 pipe 杩炴帴锛岀粷涓嶆潵鑷抚** | 瀹㈡埛绔笉鑳借嚜绉拌韩浠?|
| 鎺у埗浣嶅彧璁や袱涓?flag锛?*鏈煡浣嶆嫆缁?* | 璁句簡浣嶅嵈闈欓粯涓嶅仛鏄渶鍧忕殑涓夌绛旀涔嬩竴 |
| **鏌ヨ涓嶅彇 gate** | 璋冨害鍣?*鏁村彴鎿嶄綔**鎸佹湁 gate锛坈reate 鍙揪鍒嗛挓绾э級锛涙煡璇㈣嫢鍙栵紙鍏变韩鎴栫嫭鍗狅級灏卞繀鐒剁瓑瀹冿紝T1-04 鐨勯獙鏀跺氨**涓嶅彲鑳芥垚绔?* |
| **鍙樻洿鍙湪鍏?鍑嗗叆"姝ュ彇 gate锛堟湁鐣岀瓑寰咃級** | 鍑嗗叆鍐欑殑鏄皟搴﹀櫒闅忓悗瑕?claim 鐨勮褰曪紱`dispatch` 鍑嗗叆瀹屽嵆杩斿洖锛宔xecution 鐢辫皟搴﹀櫒鎸?gate 鎵ц |
| **澶辫触涔熷洖涓€涓搷搴斿抚锛屼笉鏂繛鎺?* | 瀹㈡埛绔繀椤昏兘鍖哄垎"鏈嶅姟鎷掔粷"锛堥€€鍑虹爜 2/4锛変笌"鏈嶅姟涓嶅湪"锛堥€€鍑虹爜 3锛?|

gate 鐢?`std::mutex` 鏀逛负 **`std::timed_mutex`**锛坄ServiceExecutionScheduler` 涓?`ServiceRuntime` 鍚屾淇敼锛夛細涓€澶?鏈夌晫鐨勭瓑寰?搴旇鐢辩被鍨嬭〃杈撅紝鑰屼笉鏄潬杞銆?
### 2.3 鏈嶅姟渚ф帴绾匡細`src/service/public_pipe_surface.{h,cpp}` + `ServiceHost`

- `IPublicCommandSurface`锛坄open()` / `close()`锛夎繘 `ServiceHost`锛?- **`open()` 鍦?`start()` 鐨勬渶鍚?*锛坮ecovery 鈫?sweep 鈫?scheduler 鈫?**pipe**锛夛細
  鍦ㄨ皟搴﹀櫒璧锋潵涔嬪墠鎺ュ彈鍛戒护锛屼細鍑嗗叆涓€涓案杩滀笉浼氳鎵ц鐨?operation锛?- **`close()` 鍦?`stop_locked()` 鐨勬渶鍓?*锛堣璁?搂9锛氬叧闂噯鍏?鈫?鎽?pipe 鈫?鍋?scheduler锛夛紱
- 寮€鍙戞満娴嬭瘯鐢ㄨ褰曞櫒鏂█杩欎釜椤哄簭锛堜笌 FakeScheduler/FakeSweep 鍚屼竴鎵嬫硶锛夈€?
`ServiceRuntime` 缁勫悎鏍归儴鎶?`PublicPipeSurface` 鎸変緷璧栭『搴?emplace锛宍host()` 浼犺繘鍘汇€?
### 2.4 CLI锛歚src/cli/*` + `myrd-cli`

鎸夎璁?搂10.2 鐨勫垎灞傦細`cli_parser`锛堟枃鏈?鈫?`control::Command`锛夈€乣cli_renderer`
锛坱yped response 鈫?鏂囨湰 + 閫€鍑虹爜锛夛紝CLI 涓撳睘锛孏UI 灏嗘潵涓嶆秷璐广€?
**閫€鍑虹爜 0鈥?**锛坴2 搂10 鍘熻〃锛寁3 G2 寮曠敤锛夛細`0` 鎴愬姛 / `1` 浜у搧澶辫触 / `2` 鍙傛暟鎴?schema /
`3` 鏈嶅姟涓嶅彲鐢?/ `4` 鏉冮檺 / `5` 瓒呮椂鎴栫粨鏋滄湭纭畾銆傛槧灏勬槸**涓€涓嚱鏁?* `exit_code_for()`锛?甯︿竴寮犳祴璇曡〃锛岃€屼笉鏄暎鍦ㄥ悇璋冪敤鐐圭殑鏉′欢銆?
鍛戒护闈細`disk list|show|create|import|start|stop|checkpoint|compact|config set auto_start|
forget|repair-profile|rebind`銆乣operation show|wait|cancel`锛涢€夐」 `--wait` / `--no-wait` /
`--dry-run` / `--timeout` / `--pipe`銆?
**`--wait` 鏄鎴风琛屼负**锛氬噯鍏ュ悗寰幆 `operation wait`锛?*涓嶉暱鏈熷崰浣忎竴涓姹?*鈥斺€?鏈嶅姟绔鍒欐槸"瀹㈡埛绔秴鏃舵案涓嶅彇娑堜竴涓凡瀹夊叏杩愯鐨?operation"锛堜笉鍙橀噺 9锛夛紝
鎵€浠ユ斁寮冪瓑寰呮斁寮冪殑鏄瓑寰咃紝涓嶆槸鎿嶄綔銆?
`--force` 涓?`--accept-volatile-data-loss` **淇濇寔涓や釜鐙珛寮€鍏?*锛氬畠浠槸缁欑敤鎴风殑涓や釜涓嶅悓鎵胯锛?瀹㈡埛绔笉寰楄涓€涓殣鍚彟涓€涓紙璁捐 搂10.5.3锛夈€?
`myrd-cli` 涓?`myrd_cli` 閮芥寕 `myrd_assert_execution_boundary`锛堜笉鍙橀噺 2 鐨?鏂扮洰鏍囦篃瑕佹寕"锛夈€?
---

## 3. 閫斾腑鍙戠幇骞朵慨鎺夌殑涓や釜鐪熺己闄?
### 3.1 璋冨害鍣ㄥ湪閫€褰归€斾腑浠嶄細 claim 骞舵墽琛屼竴涓?operation锛?*淇鍓嶅悗 10/10 鈫?0/10**锛?
**鐜拌薄**锛歚m5.execution_scheduler` 鐨?`m5_scheduler_stop_reports_a_deadline_it_could_not_meet` **骞插噣閲嶇紪鍚庤繛璺?10 娆″叏绾?*锛?澶辫触鐐规槸 `m5_execution_scheduler_tests.cpp:455`锛?stop 涓嶅緱鎶婂畠涓柇鐨?operation 鏍囨垚鎴愬姛"锛夈€?
**杩欎竴鐐瑰緢閲嶈**锛歮aster-plan 搂8 绗?1 椤规浘鎶婂悓涓€澶勫垽涓?宸插叧闂?锛岀悊鐢辨槸"闄堟棫浜岃繘鍒?銆?**瀹炴祴鎺ㄧ炕浜嗚繖涓悊鐢?*锛歚--clean-first` 涔嬪悗浠?10/10 澶辫触銆?*瀹冧笉鏄檲鏃т骇鐗┿€?*

**鏍瑰洜锛堣浠ｇ爜鍗冲彲纭畾锛?*锛歚run()` 鐨?`while (!token.stop_requested())` 鍦?*鍙?gate 涔嬪墠**鍒ゆ柇锛?涓€涓?worker 鑻ユ鍋滃湪 gate 涓婏紝stop 涔嬪悗瀹冨彇鍒?gate 灏?*涓嶅啀鍒ゆ柇 stop**锛岀洿鎺?claim 骞舵墽琛屻€?浜庢槸"鍋滃湪涓€涓粈涔堥兘娌′腑鏂殑 stop"浠嶄細鎵ц涓€涓凡鍑嗗叆鐨勬搷浣溾€斺€?*瓒呭嚭瀹冨垰鍒氭姤鍛婄殑 deadline**锛?骞舵妸 operation 鏍囨垚 succeeded锛岃€屾墽琛岃€呮鍦ㄩ€€鍑恒€?
**淇**锛氬彇鍒?gate 涔嬪悗**鍐嶅垽涓€娆?stop**锛屽懡涓氨涓?claim銆?**娌℃湁涓㈠け**锛氳褰曟槸 durable 鐨勩€佷粛鏄?queued锛屼笅娆″惎鍔?reconcile 涔嬪悗浼氭墽琛屻€?
**璇绘暟**锛歚--clean-first` 骞插噣鏋勫缓鍚?**杩炵画 10 娆?鈫?10/10 閫氳繃**锛堜慨鍓?10/10 澶辫触锛夈€?
### 3.2 `profiles` 鏍圭洰褰曟槸鍏变韩鏍癸紝浣嗘敞鍐岃鍙栧櫒鍙鑷繁鐨勬潯鐩紙**鏈嶅姟浠庢姘歌繙璧蜂笉鏉?*锛?
**鐜拌薄**锛氬鏈轰笂鏈嶅姟鍙嶅鍚姩澶辫触锛?
```
myrd-service: start failed: op=profile_registration.list
              msg=The protected profiles directory contains an unknown or invalid entry
```

**鏍瑰洜**锛歚WorkerBindingStore` 鎸夎嚜宸辩殑鐞嗙敱鎶婃寚绾规斁鍦?`%ProgramData%\MyRamDisk\profiles\workers`锛堜笌 profile 璁板綍鍚屽涓€涓彈淇濇姢鏍癸紝鐪佹帀绗簩濂?ACL锛夛紝
鑰?`ProfileRegistrationStore::list_profiles()` 鎶婅鐩綍閲屼换浣曢潪 UUID銆侀潪
`lifecycle-*`/`recovery-*`/`forget-*` 鐨勬潯鐩垽涓?*涓嶅彲鎺ュ彈鐨勬潯鐩?*銆?
鈬?**绗竴娆″惎鍔ㄥ垱寤?`workers`锛涙鍚庢瘡涓€娆″惎鍔ㄩ兘澶辫触銆?*
杩欏彴瀹㈡満涓?`workers` 鐨勬椂闂存埑鏄?2026-10-17锛?*T1-02 鐨?`recovered=8` 璇绘暟鍥犳鏄渶鍚庝竴娆℃垚鍔熺殑鍚姩**銆?
**杩欐潯涓轰粈涔堜弗閲?*锛歍1-09 鐨勬柇鐢垫仮澶嶃€佷互鍙婁换浣?閲嶅惎鍚庢仮澶?鐨勯獙鏀讹紝鍓嶆彁閮芥槸**鏈嶅姟鑳藉啀娆″惎鍔?*銆?
**淇**锛氬湪 `allowed_root_metadata_leaf()` 閲?*鎸夊悕瀛?*鎵胯 `workers`锛屽苟鍐欐槑
**涓轰粈涔堜笉鍐欐垚"蹇界暐鎵€鏈夌洰褰?**鈥斺€斿彈淇濇姢鏍归噷鍑虹幇鎰忓鏉＄洰姝ｆ槸瑕佹姤鐨勯敊锛?鏀捐鐩綍绛変簬鏀惧純杩欐潯淇濇姢锛涙柊鐨勫叡浣嶈€呭繀椤?*鍏峰悕鍔犲叆**锛岄偅鏄簲褰撹瑕佹眰璁鸿瘉鐨勬椂鍒汇€?`worker_binding_store.h` 鐨勬敞閲婏紙鍘熷啓 `<root>\MyRamDisk\workers`锛屼笌浠ｇ爜涓嶄竴鑷达級涓€骞舵敼姝ｄ负瀹為檯璺緞锛?骞舵妸"鍏变韩鏍圭殑浠ｄ环"鍐欏湪浠樹唬浠风殑鍦版柟銆?
**璇绘暟**锛氫慨澶嶅悗瀹㈡満鏈嶅姟 `STATE : 4 RUNNING`锛宍disk list` 姝ｅ父杩斿洖涓ゆ潯鏃㈡湁 profile锛堣 搂5锛夈€?
---

## 4. 寮€鍙戞満娴嬭瘯锛堢函閫昏緫锛屼笉寤虹湡瀹炶澶囷級

| 鐩爣 | 缁撴灉 |
|---|---|
| `m5.cli`锛堟柊澧烇紝15 渚嬶級 | **15/15 閫氳繃** |
| `m5.service_host`锛堟柊澧?5 渚嬬鐐规祴璇曪紝鍏?21 渚嬶級 | **21/21 閫氳繃** |
| `m5.execution_scheduler` | **10/10 杩炵画閫氳繃**锛堜慨 搂3.1 鍓?10/10 澶辫触锛?|
| `structural.m5.read_write_separation` | 閫氳繃 |
| `structural.m5.execution_boundary` | 閫氳繃 |
| 骞插噣鏋勫缓锛坄--clean-first`锛屽叏鏍戯級 | **0 error / 0 warning** |

`m5.cli` 鏂█鐨勬牳蹇冩€ц川锛?
1. **寰€杩斾竴鑷?*锛歚disk list` 鍝嶅簲锛堝惈 2 涓?profile + 鎸傝浇鐐癸級缂栫爜鈫掕В鐮佸悗閫愬瓧娈电浉绛夛紱
   鍚?*闈?ASCII 鐨?error 娑堟伅**锛堝瀛楃杞崲鍙湁涓€澶勫疄鐜帮紝瑙?`ipc::utf16_to_utf8`锛夈€?2. **涓ユ牸鎷掔粷**锛氭湭鐭?tag / 閲嶅 tag / 鏋氫妇瓒婄晫 / 鎴柇 / 灏鹃殢瀛楄妭 / major 涓嶈璇?/ minor 鏇存柊锛?   鍏ㄩ儴澶辫触锛堢敤鎸夊亸绉绘敼瀛楄妭鏋勯€犺礋渚嬶紝涓嶉潬"鐩镐俊瀹炵幇"锛夈€?3. **閫€鍑虹爜鏄犲皠琛?*锛? 涓敊璇爜 + failed/reconciling/queued/wait_timeout 鍚勪竴鏉°€?4. **瑙ｆ瀽**锛歚disk create` 鍙樻垚**绫诲瀷鍖栧懡浠?*锛堝惈 size/interval 鐢变骇鍝佽嚜宸辩殑 parser 瑙ｉ噴锛夛紱
   鏈煡寮€鍏?缂哄€?闈炴硶 uuid/闈炴硶 size 鍏ㄩ儴 usage 閿欒銆?5. **娓叉煋**锛氳緭鍑?*鍒ゅ喅**锛坄aggregate`锛夎€屼笉鏄?durable 澹扮О锛涢敊璇甯?step 涓?code銆?
`m5.service_host` 鐨勭鐐规祴璇曪細

| 鐢ㄤ緥 | 鏂█ |
|---|---|
| 鏌ヨ鍦?gate 琚嫭鍗犳寔鏈夋椂浠嶄綔绛?| 杩?*灏辨槸 T1-04 鐨勭函閫昏緫褰㈡€?*锛氳嫢瀹冧笉鎴愮珛锛孷M 涓婃棤璁哄浣曢兘娴嬩笉鍑烘潵 |
| 鍙樻洿鍦?gate 琚寔鏈夋椂**鍦?deadline 鍐呰鎷?* | 鍥炵殑鏄搷搴斿抚锛坈ode `0x2203`锛夛紝涓嶆槸鏂摼锛涗笖**娌℃湁缂栭€?operation id** |
| 缂栫爜鍚庣殑杞借嵎**涓嶅惈 bundle 璺緞** | 杞借嵎瀛楄妭閲屾悳 `Images` / `work.myrd` 鍧囦负绌猴紝`R:` 瀛樺湪 |
| 澶辫触鍛戒护甯﹀洖**鏈嶅姟鑷繁鐨勯敊璇爜涓庢秷鎭?* | 涓庣洿鎺?`dispatch` 鐨勯敊璇€愬瓧娈电浉绛夛紝涓嶆槸绔偣鍙︾紪涓€涓?|
| 鏈煡鎺у埗浣嶈鎷?| code `0x2202` |

---

## 5. 瀹㈡満璇绘暟

### 5.1 T1-04锛歝reate 杩涜涓?`disk list` 1 绉掑唴杩斿洖 鈬?**PASSED**

鏂规硶锛坄tests/e2e/test-m5-cli-e2e.ps1`锛屽鏈哄彈闄愩€乣MYRD_M5VM_GUEST` 瀹堝崼锛夛細

1. `myrd-cli disk create ... --no-wait` 鎷垮埌 operation id锛?2. 寰幆锛氬厛 `operation show` 璇?*褰撳墠鐘舵€?*锛屽啀璁℃椂涓€娆?`disk list`锛屾妸 `(鐘舵€? 姣)` 璁颁笅鏉ワ紱
3. 鐩村埌 operation 缁堟€佹垨閲囨弧 20 娆★紱
4. 鏂█锛?*姣忔潯鏍锋湰 < 1000 ms**銆侀€€鍑虹爜鍏?0銆佷笖**鑷冲皯 3 鏉℃牱鏈槸鍦?create 浠嶅湪璺戞椂鍙栫殑**銆?
**涓夋鐙珛杩愯鐨勮鏁?*锛堝悓涓€濂楄剼鏈紝`create` 澶у皬 256 MiB锛夛細

| 杩愯 | 鏍锋湰鏁?| 鍏朵腑 create 杩涜涓?| 鏈€澶ф椂寤?| 澶囨敞 |
|---|---|---|---|---|
| 绗?1 娆?| 20 | 20 | **35 ms** | 鍏ㄩ儴 `state=running` |
| 绗?2 娆?| 20 | 20 | **26 ms** | 鍏ㄩ儴 `state=running` |
| 绗?3 娆?| 20 | 20 | **111 ms** | 鍏ㄩ儴 `state=running` |

鈬?**60 娆￠噰鏍枫€佸叏閮ㄥ彂鐢熷湪 create 杩涜涓€佹渶澶?111 ms锛岄檺鍊?1000 ms銆?*

**杩欐潯璇绘暟涓轰粈涔堝彲淇?*锛氭牱鏈殑"during create"涓嶆槸鍋囪锛岃€屾槸**姣忔閲囨牱鍓嶈鍒扮殑鐘舵€?*锛?鏈€灏?13 ms / 鏈€澶?111 ms 鐨勫垎甯冭鏄庡畠閲忕殑鏄懡浠ゆ湰韬紙鍚繘绋嬪惎鍔級锛屼笉鏄紦瀛樸€?
### 5.2 T1-05锛欳LI 鑳借繛涓婄湡瀹炴湇鍔★紱`create` 鐨勫け璐ュ師鍥犲凡鍙揪锛屽苟宸插畾浣?
**宸茶揪鎴?*锛?- `myrd-cli` 缁?*璁″垝浠诲姟锛堥潪缃戠粶鐧诲綍锛?*杩炰笂鐪熷疄鏈嶅姟鐨?public pipe锛?- `disk list` 姝ｅ父杩斿洖鏃㈡湁涓ゆ潯 profile锛堝惈鐩樼 `S:` / `R:`銆乣aggregate`銆乬eneration锛夛紱
- `disk show <涓嶅瓨鍦?` 姝ｇ‘杩斿洖 `error: The requested profile is not registered` /
  `code: 0x5604` / **閫€鍑虹爜 1**锛屽嵆"鏈嶅姟鎷掔粷"涓?鏈嶅姟涓嶅彲杈?琚纭尯鍒嗐€?
**璁╁け璐ュ師鍥犲彲杈撅紙鏈疆瀹屾垚鐨勭涓€鍗婏級**锛?`ServiceLogEvent` 澧炲姞 `message` 瀛楁銆佹柊澧?`ServiceLogKind::operation_failed`銆?鍦?`execute_operation` 鐨?*鍔ㄤ綔澶辫触鍒嗘敮**锛坄service_core.cpp` 鐨?`if (!result.ok())`锛?鍗虫墍鏈夊け璐ュ垎鏀殑涓婃父銆侀敊璇繕鍦ㄤ綔鐢ㄥ煙鍐呯殑鍞竴浣嶇疆锛夎褰?`action / code / native / step / detail`锛沗StderrServiceLogger` 鎵撳嵃璇ュ瓧娈点€?
**绗竴娆℃嬁鍒板師鍥狅紙璇锋眰 `0edd233fabfc4518b639e68069fab1c8`锛?*锛?
```
myrd-service: kind=7 command=disk.create phase=create-bundle
  message=action=create-bundle code=20994 native=0
          step=provision_new_volume detail=requested drive letter is occupied
```

鈬?**澶辫触鐐瑰畾浣嶅埌 `provision_new_volume`锛氳姹傜殑鐩樼宸茶鍗犵敤**锛坈ode `20994`锛夈€?鍗?create 璧板埌寤哄嵎閭ｄ竴姝ャ€佽鐩樼鍐茬獊鎷掓帀锛?*涓嶆槸** bundle銆佷笉鏄?worker 鍚姩銆佷笉鏄潈闄愩€?e2e 鑴氭湰閫夌洏绗︽椂鐢ㄧ殑鏄?`Test-Path` + `disk list` 涓ゆ潯鍒ゆ嵁锛?*涓庝骇鍝佽嚜宸辩殑"鐩樼琚崰鐢?鍒ゆ嵁涓嶄竴鑷?*鈥斺€?浜у搧鐪嬪埌鐨勬槸鍗?璁惧鍛藉悕绌洪棿閲岀殑鍗犵敤锛堟鍓嶅嚑娆″け璐ヨ繍琛岀暀涓嬬殑娈嬬暀锛夛紝鑴氭湰鐪嬩笉鍒般€?**鈬?涓嬩竴姝ワ細鍏堣涓よ竟鐨勫垽鎹竴鑷?*锛堢敤浜у搧鑷繁鐨勫垽鎹€夌洏绗︼紝鎴栧姞涓€鏉℃樉寮忕殑"娓呯悊/鏍￠獙鐩樼"鍓嶇疆锛夛紝
鍐嶉噸璺戠鍒扮锛涜繖灞炰簬**娴嬭瘯澶瑰叿鐨勪慨姝?*锛屼笉鏄骇鍝佺己闄风殑璇佹嵁銆?
**宸茬煡鐨勮竟鐣屼簨瀹?*锛堢敤浜庢帓闄ゅ亣璁撅紝鍧囦负瀹炴祴锛夛細
- 鏈嶅姟 `STATE : 4 RUNNING`锛宍disk list` 鍙敤 鈬?涓嶆槸"璧蜂笉鏉?锛?- 澶辫触鍚?`myrd-m5-worker` 杩涚▼鏁?**0** 鈬?worker 宸查€€鍑猴紙涓嶆槸鎸備綇锛夛紱
- 宸ヤ綔鐩綍閲?*鍑虹幇浜?`bundle`** 鈬?worker 纭疄璺戣繃銆佷笖鎴愬姛寤轰簡 bundle锛?- 鏈嶅姟鏃ュ織閲?*娌℃湁 provisioning helper 鐨勮緭鍑?* 鈬?澶辫触鐐瑰湪 helper 涔?*鍓?*銆?
**澶辫触鍘熷洜褰撳墠鏃犳硶鍙栧緱锛岃繖鏄湰杞殑绗簩涓彂鐜?*锛?- `OperationRecord` 鍙瓨 `terminal_result`锛堝嚑鐧惧瓧鑺傜殑**鏍囪涓?*锛屽 `evidence`/`reversed`锛夛紝
  **涓嶅瓨閿欒鏂囨湰**锛?- `ServiceLogEvent` 娌℃湁 message 瀛楁 鈬?`StderrServiceLogger` 鎵撲笉鍑哄師鍥狅紱
- 鍝嶅簲鎶曞奖鍙甫 `operation.state` 鈬?**CLI 涔熸病娉曞憡璇夌敤鎴蜂负浠€涔堝け璐?*銆?
鈬?**"涓€娆?create 澶辫触锛屾湇鍔°€佹棩蹇椼€丆LI 涓夊閮借涓嶆竻鍘熷洜"鏈韩鏄竴涓繀椤讳慨鐨勪骇鍝佺己鍙?*锛?涓斿畠鏄笅涓€姝ユ帓鏌ョ殑鍓嶇疆锛堜笉鏄彲閫夌殑浣撻獙鏀硅繘锛夈€?
### 5.3 鏁村鍩虹嚎锛堝鏈猴紝154 鏉★級

`tools/m5vm/Invoke-M5Vm.ps1 -Run`锛堣姹?`f79804f56e234d9191638e1eb4ee0fc8`锛夛細

```
151 of 154 passed
  FAIL structural.m2.storage_independent_build (exit 1)   鈫?瀹㈡満鏃?Visual Studio锛堝凡鐭ワ級
  FAIL structural.m5.execution_boundary        (exit 1)   鈫?鍚屼竴鍘熷洜锛堝凡鐭ワ級
  FAIL integration.m5.cli_e2e                  (exit 1)   鈫?搂5.2 鐨?create 澶辫触
```

**娴嬭瘯鐭╅樀 152 鈫?154**锛堟柊澧?`m5.cli` 涓?`integration.m5.cli_e2e`锛夈€?*浜у搧闈㈠叏缁?*锛?`m5.production_execution_backend`锛圱1-03 鐨勪慨澶嶄繚鎸佹湁鏁堬級銆乣m5.execution_scheduler`锛埪?.1 鐨勪慨澶嶅湪瀹㈡満鎴愮珛锛夈€?`m5.service_host`銆乣m5.cli`銆乣structural.m5.read_write_separation` 鍏ㄩ儴閫氳繃銆?**閲忔祴鑼冨洿**锛氭湰杞叏閲忓彧璺戜簡涓€娆★紝涓嶅绉?绋冲畾"锛涙湁鐙珛閲嶅璇绘暟鐨勬槸 搂3.1锛?0 娆★級涓?搂5.1锛? 娆?脳 20 閲囨牱锛夈€?
---

## 6. 鍙樻洿娓呭崟

| 鏂囦欢 | 鎬ц川 |
|---|---|
| `src/management/service_response_wire.{h,cpp}` | **鏂板**锛氬搷搴旀姇褰?+ 涓ユ牸缂栬В鐮?|
| `src/service/public_command_endpoint.{h,cpp}` | **鏂板**锛氬抚 鈫?core 鈫?甯?|
| `src/service/public_pipe_surface.{h,cpp}` | **鏂板**锛歚PublicPipeServer` + endpoint 鐨勫涓诲疄鐜?|
| `src/cli/cli_parser.{h,cpp}`, `cli_renderer.{h,cpp}`, `cli_main.cpp` | **鏂板**锛欳LI |
| `src/service/service_host.{h,cpp}` | `IPublicCommandSurface`锛沷pen 鏈€鍚庛€乧lose 鏈€鍓?|
| `src/service/service_runtime.{h,cpp}` | 缁勫悎 `PublicPipeSurface`锛沢ate 鏀?`timed_mutex`锛沗pipe_name` |
| `src/service/service_main.cpp` | `--console` **鏀逛负鎸佺画鏈嶅姟鍒?Ctrl+C**锛堝彲璇婃柇銆佸彲浣?e2e 澶瑰叿锛?|
| `src/management/execution_scheduler.{h,cpp}` | gate 鏀?`timed_mutex`锛?*搂3.1 鐨?stop 澶嶆** |
| `src/management/service_worker_control.{h,cpp}` | **鍛戒护闂?*锛氭墍鏈夊懡浠や覆琛岋紱`observe_runtime` **try_lock 鈫?`nullopt`** |
| `src/management/profile_registration_store.cpp` | **搂3.2**锛氭壙璁?`workers` 鍏变綅鑰?|
| `src/management/worker_binding_store.h` | 娉ㄩ噴涓庝唬鐮佷竴鑷达紙`profiles\workers`锛夛紝鍐欐槑鍏变韩鏍圭殑浠ｄ环 |
| `src/control/ipc_codec.{h,cpp}` | `utf16_to_utf8` / `utf8_to_utf16` 瀵煎嚭锛堝敮涓€瀹炵幇锛屼緵鍝嶅簲缂栬В鐮佸鐢級 |
| `src/management/service_core.h` | `ServiceProfileProjection` 杩藉姞 `mount_target`锛堝甫榛樿鍊硷級 |
| `tests/model/m5_cli_tests.cpp` | **鏂板** `m5.cli`锛?5 渚嬶級 |
| `tests/model/m5_service_host_tests.cpp` | **鏂板** 5 渚嬬鐐规祴璇曪紙鍚?gate 琚寔鏈変粛鍙煡璇?锛?|
| `tests/e2e/test-m5-cli-e2e.ps1` | **鏂板** `integration.m5.cli_e2e`锛堝鏈猴紝瀹堝崼 exit 2锛?|

### 6.1 `ServiceWorkerControl` 鐨勫懡浠ら棬锛堜负浠€涔堝繀椤诲姞锛?
worker 鐨勫懡浠ら€氶亾鏄?*涓ユ牸涓€娆¤姹備竴娆″簲绛斻€佹病鏈夐噸鍚屾**鐨勫簭鍒椼€傚湪 public pipe 鍑虹幇涔嬪墠锛?鍞竴璋冪敤鑰呮槸璋冨害鍣紙鍦?execution gate 涔嬪悗锛夛紝鎵€浠?涓嶄細鏈変袱涓嚎绋?鏄?*澶栭儴**缁欑殑淇濊瘉锛?绫昏嚜宸辨病鍐欎笅鏉ャ€俆1-04 瑕佹眰**涓€涓璺緞鍦ㄩ暱鍛戒护杩涜涓篃鑳借窇**锛岃繖鏉′繚璇佸繀椤昏惤鍒扮被閲岋細

- 鎵€鏈夊懡浠ゅ叆鍙ｅ彇 `command_gate_`锛?- **`observe_runtime` 鐢?`try_lock`锛屾嬁涓嶅埌灏辫繑鍥?`nullopt`锛?鏈娴?锛?*锛?  缁濅笉鎺掗槦绛?create 鈥斺€?閭ｆ槸 `disk list` 浼氳窡鐫€绛夊垎閽熺骇鐨勫師鍥狅紱
- `nullopt` 鏄?*鏇村急**鐨勭瓟妗堬紝涓嶆槸閿欑殑绛旀锛氳璺緞鎹鎶ュ憡 durable 鐘舵€侊紙transition 涓嵆
  `provisioning`/`starting`锛夛紝骞朵笖**涓嶄細**璋庢姤 `worker_alive == false`锛堥偅浼氭妸涓€鍧楁鍦ㄨ窇鐨勭洏 fail-closed锛夛紱
- `require_runtime_observation`锛堝彉鏇磋矾寰勶級**浠嶇劧鎺掗槦**锛氬喅瀹?鏄惁瀹夊叏"鐨勮皟鐢ㄨ€呬笉鑳借
  "閫氶亾蹇?褰撴垚"涓嶇煡閬?銆?
**浠ｄ环鍐欏湪鎴愬憳澹版槑澶?*锛氫竴鎶婇攣鏄?*鏁村璞?*绮掑害锛沺er-profile 涓茶鏄?T2-01 鐨勫伐浣滐紝
鍦ㄦ鍙戞槑绗簩妗ｇ矑搴︿細鍦ㄩ偅閲岃鎺ㄧ炕銆傚悗鏋滄槸"涓€涓?profile 鐨勫懡浠ゅ湪璺戞椂锛屽彟涓€涓?profile 鐨勮娴嬬瓟
'鏈娴?"鈥斺€斿湪 G4 寤跺悗鐨勫墠鎻愪笅鍙帴鍙楋紝鎭㈠ G4 鏃跺繀椤绘敼銆?
---

## 7. 涓嬩竴姝ワ紙鎸夐『搴忥級

1. **鎶?operation 鐨勫け璐ュ師鍥犲仛鎴愬彲杈剧殑**锛堜笁澶勶紝缂轰竴涓嶅彲锛夈€?*宸茬粡瀹氫綅鍒拌鏀圭殑浣嶇疆**锛?
   | 浣嶇疆 | 鐜扮姸 | 瑕佸仛浠€涔?|
   |---|---|---|
   | `src/management/service_core.h` `ServiceLogEvent` | 鍙湁 `kind/request_id/operation_id/profile_id/command/phase`锛?*娌℃湁 message** | 鍔犱竴涓?`std::string message`锛沗src/service/service_system.cpp:50` 鐨?`StderrServiceLogger::write` 涓€骞舵墦鍗?|
   | `src/management/service_core.cpp:6423` `finalize_operation(...)` 璋冪敤澶?| 浼犵殑鏄?`completion` + `worker_succeeded` + 鏍囪涓诧紝**澶辫触鍔ㄤ綔鐨?`Error` 鍦ㄨ繖涓€灞備箣鍓嶅氨琚涪鎺変簡** | 鎶婂け璐ュ姩浣滅殑 `Error`锛坄code` / `operation` / `message` / `native_code`锛変竴璺甫鍒?`finalize_operation`锛屽苟鍐欒繘锛坅锛夋棩蹇椾簨浠躲€侊紙b锛塦terminal_result` 鐨勭粨鏋勫寲澶辫触璇佹嵁 |
   | `src/management/service_core.h` `ServiceOperationProjection` + `service_response_wire` | 鍙湁 `state/phase` | 杩藉姞澶辫触鍘熷洜锛堝彲閫夊瓧娈碉紝甯﹂粯璁ゅ€硷紝鏃犻渶鐮村潖鏃㈡湁鍒濆鍖栵級锛孋LI 鎵嶈兘璇?涓轰粈涔堝け璐? |

   **娉ㄦ剰**锛歚terminal_result` 鐜板湪鏄?*鏍囪涓?*锛坄evidence` / `reversed` / `MYRD鈥锛夛紝鎶婂畠鎹㈡垚鎴栬拷鍔犵粨鏋勫寲澶辫触璇佹嵁灞炰簬 **durable 璁板綍鏍煎紡鍙樻洿**锛屽繀椤诲悓鏃跺鐞嗘棫璁板綍鐨勮鍙栵紙鐜版湁璇昏€呭鏃ц褰曞簲"鐓ф棫瑙ｈ涓烘棤鍘熷洜"锛岃€屼笉鏄嫆璇伙級銆?
2. 鐢?(1) 鐨勮鏁板畾浣?`create` 鍦ㄦ湇鍔￠噷鐨勫け璐ョ偣锛屼慨鎺夛紝鐒跺悗璺戝畬 T1-05 鐨勭鍒扮
   锛坈reate 鈫?list 鈫?show 鈫?璇诲啓 鈫?checkpoint 鈫?stop 鈫?鎭㈠ 鈫?forget锛夛紝骞?`-Repeat`銆?3. T1-05 鏀跺熬鍚庡洖鍒?T1-06锛堜緷璧?T1-03 + T1-05锛夈€?
**鍚屾椂璁颁笅涓ゆ潯寰呭姙**锛堜笉鏄樆濉為」锛夛細
- `integration.m5.cli_e2e` 鐩墠**鍖呭惈 T1-04 鐨勯獙鏀?*锛堝凡閫氳繃锛夛紱T1-05 鏀跺熬鍚庡簲鎶婁袱娈垫媶寮€锛?  璁?T1-04 鐨勮鏁扮嫭绔嬪彲閲嶅杩愯锛?- 瀹㈡満涓婁袱鏉?2026-09/10 鐨勬棦鏈?profile锛坄9e5a6b53` 鍦?`S:` blocked銆乣ea53d5dd` 鍦?`R:`
  `runtime=Stopping`锛夋潵鑷洿鏃╃殑浼氳瘽锛屼笉鏄湰杞骇鐢熴€傝剼鏈凡鏀逛负**鍔ㄦ€侀€夌┖闂茬洏绗?*浠ヤ笉鍙楀叾褰卞搷锛?  瀹冧滑鐨勫缃渶瑕佷竴娆′汉宸ュ垽鏂紙灞炰簬"澶栭儴閬楃暀鐘舵€?锛屼笉鏄湰杞彲浠ヨ嚜琛屾竻鐞嗙殑锛夈€?
---

## 8. 2026-10-18：把 create 一层层打通到的位置（每一步都是读数）

**方法**：把"失败原因不可达"逐个补上（`ServiceLogEvent::message`、worker 动作失败分支、
`finalize_operation` 的终态标记、`publish_registration` 的拒绝原因），再用客机 e2e 逐层推进。
**没有一次是靠猜**；每次失败都带着 `action / code / step / detail`。

| # | 读数 | 结论 / 处置 |
|---|---|---|
| 1 | `code=20994 step=provision_new_volume detail=requested drive letter is occupied` | 盘符占用；e2e 改为按**产品同一命名空间**选字母（`Win32_Volume`/`LogicalDisk`），产品拒绝即硬失败 |
| 2 | 残留 `R:`/`U:`：`QueryDosDevice` 有映射、`Get-Volume` 无卷；`mountvol /D` 与 `DefineDosDevice` **都删不掉** | 属挂载管理器残留，**只有重启能清**（实测：重启后全部 `rc=0`）。e2e 现在把"任何 DOS 名有应答"的字母一律排除，并打印其目标 |
| 3 | `code=21000 step=select_volume_for_disk detail=no volume matches ...` 且 helper 报成功 | 加了 10 s 有界重试（供给后卷的发布是异步的），并让拒绝**带计数**：`observed/foreign/other_letter/not_mounted/no_guid/extents/other_disk` |
| 4 | `code=22024 step=profile_registration.publish detail=... unknown or invalid entry` | **`workers` 是目录共位者**，而 publish 的枚举对**目录**不查文件白名单 ⇒ 与 `disk list` 判据不一致。已按名字放行（`shared_root_directory_leaf`），并写明"不写成忽略目录"的理由 |
| 5 | **`code=22022 step=service_core.checkpoint_boundary detail=The worker checkpoint correlation is invalid`**，终态标记 `worker` | **当前阻塞点**：create 的供给流程会做第一次 checkpoint，P4 correlation 校验失败。这是**产品缺陷候选**（不是环境），下一步从这里入手 |

**e2e 同时修正了两个夹具缺陷**（都会造成假失败）：
- `operation wait` 的一次调用是**有界观察**，不是"已完成"的承诺；脚本现在循环到终态（`Wait-ForTerminal`）；
- create 的时长超过一次等待预算，之前的脚本把"仍在 running"当成失败。

**量测范围**：T1-04 的读数在每一轮都复现（20 次采样全部落在 create 进行中，最大 26–135 ms）；
**T1-05 端到端仍未绿**，因此 **T1-05 保持 PARTIAL**。

### 8.1 当前阻塞点已精确到数值：create 的供给 checkpoint 的 P4 correlation

**读数（请求 `fc977fd68ca743fdb312cf03d138c402`，客机）**：

```
checkpoint boundary refused: profile=1 op_match=1 profile_match=1 image_match=1
  pid=5928 created=134367921189870409 session_gen=1
  prior=1 confirmed=0 candidate=2
```

`valid_checkpoint_boundary` 的七个合取项里，**唯一不成立的是**
`evidence.prior_generation == profile.checkpoint.confirmed_generation`：
**worker 说它要把 generation 1 推进到 2，而 durable 记录里 confirmed 是 0。**

**这说明什么（不是猜测，是等式）**：新建 bundle 在**供给完成之后**已经带有一个已落盘的 generation
（worker 打开时看到 prior=1），而服务侧这条 profile 的 `checkpoint.confirmed_generation`
还停在 0（本服务从未提交过 checkpoint）。**两侧对"当前是第几代"的认识差 1**，
而这条校验正是为捕捉这种跨层不一致而存在的——所以它拒绝得对，**问题在别处**。

**下一步的两条候选（必须先判定哪条对，不能猜）**：
1. **创建路径应在供给之后采纳 bundle 的当前 generation**（把它写进 durable profile），
   使首次 checkpoint 的 `prior` 与 `confirmed` 相等；或
2. **首次供给 checkpoint 的校验应允许 `prior == confirmed + 1`**，并写明理由（供给本身落了一代）。
   注意这**不是**放松校验：pid/creation/session/前几项仍然必须相等。

**判定依据应当来自 M3 的 generation 规则**（供给写没写第 0/1 代、谁拥有第一次提交），
不能用"让它过去"来选。**这也是它没有在本轮被改掉的原因**：
改动会落在 M3/M4 的既有证据上（T1-11 的 `-Repeat 20` 就在这条链上）。

**其余读数**：T1-04 在每一轮都复现（20/20 采样落在 create 进行中，最大 76 ms）；
e2e 本轮又修掉两个夹具缺陷（唯一 profile 名，避免历次失败留下的注册冲突；
"任何 DOS 名有应答"的盘符一律排除）。

### 8.2 判定：候选 1 成立（供给后的 bundle 已经站在 generation 1），并说明为什么不能靠"带个字段下来"

**依据（全部来自现有代码与读数，不是推断链）**：

1. `prior_generation` 来自 worker 的 `prepared`，即 **M3 自己**对 bundle 当前代的判断
   （`src/host/m5_worker_runtime.cpp:516-517`：`candidate.prior = prepared.prior`、
   `candidate.candidate = prepared.candidate`）——**不是服务侧推算**。
2. 客机读数 `prior=1 candidate=2` ⇒ **创建完成后 bundle 已经站在第 1 代**。
3. `m5.production_execution_backend` 之所以绿：它只断言
   `candidate == prior + 1`（`tests/integration/...:1342-1343`），
   **从不与 durable 的 `confirmed_generation` 比较**；那条比较只存在于服务核心，
   所以"测试绿 + 服务红"不矛盾，**它只是没测这一条**。
4. `BundleRegistrationEvidence` **没有 generation 字段**（`service_core.h`），
   所以服务不可能从既有证据里"顺手带下来"这一代。

**结论**：创建路径必须在**发布 profile 之前**确定 bundle 的当代，并把它写进
`CheckpointDurableState.confirmed_generation`（= 1），使首次 checkpoint 的
`prior == confirmed` 成立。这不能靠猜，也不能靠放宽校验：
`valid_checkpoint_boundary` 拒绝得对，**错的是 durable 侧起点写成了 0**。

**因此需要的一次改动（下一步，未做）**：
- 在 `create_and_verify_bundle` 之后，用**已存在的只读检查**（M3 `image_inspector`）
  读出该 bundle 的当代；
- 把它作为初始 durable checkpoint 状态的一部分发布（`confirmed_generation`）；
- 加一条**负例**：bundle 的当代与 durable 起点不一致时，发布必须拒绝（fail-closed），
  而不是照抄或清零。

**为什么本轮停在这里**：这条改动落在 **M3 generation 语义 + durable 发布顺序**上，
M4/T1-11 的既有证据整条压在这上面；在没有把 `image_inspector` 的只读契约读完之前动手，
风险远大于收益。**T1-05 保持 PARTIAL，阻塞点已从"不知道"变成"一个已判定的等式"**。

### 8.3 具体改动清单（下一轮照此执行，不必重新定位）

**约束**：服务侧**不能**自己读 bundle（执行边界：`myrd_management` 不得链接 M3/persistence）。
所以这一代必须**由 worker 报上来**，而 worker 已经有这个数：`WorkerRuntimeStatus.committed_generation`
（`src/host/m5_worker_protocol.h:345`，写在协议 310/612）。**不需要新增 IPC 消息。**

| # | 文件 | 改动 |
|---|---|---|
| 1 | `src/management/service_core.h` | `BundleRegistrationEvidence` **追加** `std::uint64_t committed_generation{}`（带默认值，不改变既有聚合初始化） |
| 2 | `src/management/service_worker_control.cpp` `register_bundle` | 建 `BundleRegistrationEvidence` 时填入该代：优先取 create 回复里的字段；回复没有就调 `session.require_runtime_observation(context())`（它已返回 `committed_generation`） |
| 3 | `src/management/service_core.cpp`（create 发布初始 durable 状态处） | 初始 `CheckpointDurableState.confirmed_generation` 由 `evidence.bundle->committed_generation` 决定，**不再是 0** |
| 4 | `tests/model/…`（`m5.service_orchestration` / `m5.cli` 所在模型层） | 正例：新 profile 的初始 `confirmed_generation` 等于证据里的一代；**负例**：证据代与既有 durable 记录不一致时发布被拒（fail-closed），既不清零也不照抄 |
| 5 | 客机 | `integration.m5.cli_e2e` 跑通后 `-Repeat 20`；`m5.production_execution_backend` 与 `integration.m5.cli_e2e` 都要在读数里 |

**验收判据**：create 的首次 checkpoint 走到 `valid_checkpoint_boundary` 时
`prior == confirmed`（不再差 1），并且 T1-05 的整链（create → list → show → 读写 →
checkpoint → stop → **重启恢复** → forget）全绿；断电恢复属 T1-09，本轮不冒充。

**风险**：第 3 步改变的是新建 profile 的 durable 起点，落在 M4/T1-11 证据链上。
因此第 4 步的**负例**是必须的——它证明"不一致就拒绝"，而不是把起点写成一个方便的值。

### 8.4 §8.3 已实施（步骤 1–3），create 前进到"durable 状态发布"

**改动**：`IServiceRegistrationStore::publish_registration` 两个重载各追加
`std::uint64_t bundle_generation = 0`；`ProfileRegistrationStore` 用它填
`CheckpointDurableState.confirmed_generation`（原来是 `CheckpointDurableState{}` 即 0）；
`ServiceCore` 在发布前用 `worker_.require_runtime_observation(...)` **严格**取该代
（取不到就不发布、记 `register` 标记与原因），并记一行
`publishing with bundle_generation=N`。

**读数（请求 `f7ea08f6284b4b04a84ee892a6ab6a27`）**：

```
kind=2 command=disk.create phase=publish-provisioning message=publishing with bundle_generation=1
kind=7 ... phase=terminal-indeterminate message=terminal failure without a worker-action error; marker=register
```

⇒ **取代正确（1）**，`valid_checkpoint_boundary` 那一条不再被触发；
create 现在停在**下一个**位置：`publish_registration` 返回
`RegistrationCommitDisposition::indeterminate`（不是 error，因此没有拒绝日志），
即 store 内部 **`publish_state_bytes` 失败**（`profile_registration_store.cpp` 里那条
`return {indeterminate, nullopt}`）。

**下一步（已定位，未做）**：查 `publish_state_bytes` 对**新建 profile** 的失败原因
（怀疑顺序：`write_next(0, ...)` 之后状态文件尚不存在，而 `publish_state_bytes` 需要既有记录做 revision 握手），
拿到它的具体错误后再改；同时补 §8.3 步骤 4 的**正例与负例**——目前
`ProfileRegistrationStore` 在测试里**零覆盖**，这正是这条缺陷能一路潜伏的原因。

**量测范围**：T1-04 本轮仍复现（20/20 采样，最大 44 ms）。**T1-05 保持 PARTIAL。**

### 8.5 generation 起点已修好并验证；create 现在卡在注册之后的下一步

**已修**：`publish_registration` 传入 `bundle_generation`；store 用它种
`CheckpointDurableState.confirmed_generation`；`ServiceCore` 里 `intended` 快照
**用同一个种子重建**（否则 store 组合出的快照与 `intended` 不等，一次**成功的提交**会被判成
`ambiguous`——这一条已由日志确认：`publish_registration disposition=0`（committed）
紧跟 `registration applied; continuing the plan`）。

**客机读数（请求 `c52f516882c6410585d126bfb9b137ee`，日志保留在 `C:\m5\last-service.log`）**：

```
kind=1 command=disk.create phase=queued
kind=2 command=disk.create phase=running
kind=2 ... phase=publish-provisioning message=publishing with bundle_generation=1
kind=7 ... phase=publish-provisioning message=publish_registration disposition=0 (the store reported no cause)
kind=2 ... phase=publish-provisioning message=registration applied; continuing the plan
（此后 600 s 内没有任何 lifecycle 事件，只有 operation.show 查询）
```

⇒ **注册这一步通了**（此前两步缺陷都已消除：起点 0 与 intended 快照不一致），
create 现在**卡在注册之后的计划步骤**（`create_runtime_and_worker` / checkpoint），
**不是失败，是挂住**。

**旁证**：同一台机器的旧日志里有一条
`action=create-runtime code=1 step=classify_bundle detail=bundle kind does not match runtime mode`
—— 说明"注册之后再起 runtime"这一步本就会因为**bundle 已经存在**而被模式判定拒绝；
现在表现成挂住（很可能是 worker 启动/等待的有界超时在重试）。

**下一步（已定位到函数）**：
1. `bind_worker` 的 launch 模式判定：`classify_bundle` 认为 bundle kind 与 runtime mode 不符；
   创建路径在注册之后应该用**采纳**模式而不是"新建"，或按 bundle 已存在的事实选模式；
2. 若是挂住而非拒绝，先把该处的等待预算与重试次数记进日志，再决定是"拒绝得更早"还是"允许采纳"。

**量测范围**：T1-04 每轮复现（20/20 采样，最大 86 ms）。**T1-05 仍 PARTIAL。**

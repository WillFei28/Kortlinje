# 時鐘直接顯示到站時間

部機自己打香港六個營運商嘅官方 API，唔經 VPS。WebUI 揀定一個站之後，Ticker 會喺 LED 滾 `路線 HH:MM…`，例如 `610 19:35`。

站會寫入 LittleFS（`Ticker` / `stop`），熄機再開會留低。

## 點用

1. WebUI 將 Mode 轉去 **Ticker**。主畫面先係五步揀站：交通、路線、方向、站、確認。
2. 方向一步一定要揀。撳 **Confirm／Save** 先會把站送去部機。
3. 部機拉官方時間，LED 滾一兩次之後返回 **Clock**。之後每 3 分鐘再拉一次，唔使留喺 Ticker 畫面。
4. 側欄會顯示已存路線、站名同方向。清除站會停止再拉 ETA。

確認頁只顯示交通、路線、站名同方向。開發備註（無 VPS、LED 格式）留喺 `webui/src/modes/Ticker.tsx` 嘅註解，唔出喺畫面。

用 IP 打開頁面唔會被轉去 `kortlinje.local`，避免揀站揀到一半被拉走。時鐘畫面先會要 LED 預覽；Ticker 揀站期間唔再推預覽，避免 WebSocket 同 ETA 搶。

## 存低嘅站

Confirm 寫入嘅 JSON 會原樣保存。輕鐵例子：

```json
{
  "operator": "lrt",
  "line": "610",
  "direction": "2",
  "direction_label": "元朗 → 屯門碼頭",
  "stop_id": "570",
  "stop_code": "FNR",
  "stop_name_tc": "豐年路",
  "seq": "4",
  "confirmed_at": "2026-11-03T11:15:06.036Z"
}
```

輕鐵同港鐵用 `direction_label` 入面最後一個 `→` 後面嗰段做終點。上面呢筆終點係「屯門碼頭」。

冇 `direction_label`（或者句字冇 `→`）嘅舊紀錄，輕鐵唔知要邊個方向，兩個方向都會出。而家 WebUI 要先揀方向先可以 Confirm，新儲存唔會再係呢種。

## 六個營運商

| 畫面 | `operator` | 時間來源 |
| --- | --- | --- |
| 九巴/龍運 | `kmb` | data.etabus.gov.hk |
| 城巴 | `ctb` | rt.data.gov.hk 城巴 |
| 地鐵 | `mtr` | rt.data.gov.hk 港鐵 |
| 輕鐵 | `lrt` | rt.data.gov.hk 輕鐵班次 |
| 接駁巴 | `mtrbus` | rt.data.gov.hk 港鐵巴士 |
| 小巴 | `gmb` | data.etagmb.gov.hk |

港鐵、輕鐵、接駁巴嘅路線同站表唔燒入 firmware。開機後 `EtaPickerExtension` 由 opendata.mtr.com.hk 下載去 LittleFS，再經 `/data/*.csv` 畀 WebUI。下載未完會回 503，頁面會再試。`webui/catalog/` 只係離線參考，部機唔讀呢個目錄。

## 輕鐵方向

同一個站通常有兩個方向，各佔一個月台。例如豐年路 610：

- 一個月台去元朗：19:28、19:38
- 一個月台去屯門碼頭：19:35

揀咗「元朗 → 屯門碼頭」就只留去屯門碼頭嗰個月台，屏幕係 `610 19:35`。去元朗嘅時間唔出。

舊紀錄冇寫方向時，兩個方向都出：`610 19:28 19:35 19:38`。如果兩邊同一個鐘，就會變成 `610 19:20 19:20`。

其他規則：

- 呢班冇去所選終點嘅車，就留空，顯示 `610 --`。唔會借另一個方向或者其他路線。
- 同一個已選月台上、終點唔同嘅短途車會保留。
- 705／706 循環線目的地包含「循環」，唔靠月台終點過濾。

## 港鐵方向

CSV 方向係 `UT`／`DT`，東鐵仲有 `LMC-UT`／`LMC-DT`，將軍澳綫有 `TKS-UT`／`TKS-DT`。API 用 `UP`／`DOWN`。

- `UP` 或 `…UT` 只取上行，`DOWN` 或 `…DT` 只取下行。
- 上行分岔會丟掉另一條支線嘅終點車：東鐵 `UT` 丟 `LMC`、`LMC-UT` 丟 `LOW`；將軍澳綫 `UT` 丟 `LHP`、`TKS-UT` 丟 `POA`。短途車（例如去大埔墟）保留。
- 下行支線會匯合，唔再按終點刪車。

## 屏幕點顯示

成功係 `路線 HH:MM HH:MM…`，最多四個時間，只留之後 30 分鐘內。

| 畫面 | 意思 |
| --- | --- |
| `610 19:35` | 窗口內有班 |
| `610 --` | 今次 API 有回應，但 30 分鐘內冇班 |
| `610 ?` | 上次成功已經超過 10 分鐘，或者今次 HTTP 失敗而手上冇可用 cache |
| `ETA fail` | 從來未成功攞到時間 |
| `...` | 剛轉站，下一輪先打 API |
| `No WiFi` | 未連上，而且未有 cache |
| `Bad stop` | 存住嘅 JSON 解唔到 |

每分鐘用緩存再濾一次，唔使打 API。09:01 嘅班次一到 09:02 就會消失，唔會成個鐘都顯示 09:01。

HTTP 失敗唔會當成冇班。未過期嘅時間繼續顯示，30 秒後再試。正常成功之後 180 秒先再拉。兩次 HTTP 至少隔 15 秒。開機後 20 秒先打第一次，等 OTA 同 WebUI 先通。

Ticker 只負責滾字。ETA 嘅 HTTP 喺 core0，約每 500ms 先睇一次要唔要拉，避免 HTTPS 卡住個 loop。

## 時間基準

港鐵同輕鐵用 API 自己嘅時間做錨點。之後用部機開機後行過嘅 `millis()` 推前，唔用部機時鐘。

咁做係因為兩件事先後出現過：

- 部機 NTP 可以同 API 差大約一個鐘，用部機時間會把未到嘅班濾走。
- 開機後 NTP 未對好就拉 ETA，部機時鐘仲係 1970。如果用「API 時間減部機時間」做偏移，NTP 一對好，偏移會變得很大，屏幕變成 `610 --`。

冇 API 時間嘅營運商（九巴等）先用 NTP。NTP 未到 2023-11 附近嘅秒數就當未對好，暫時顯示最快嗰幾個時間，唔做窗口過濾。

## Wi-Fi 同唔好吊死

ETA、WebSocket 同 OTA 都喺 core0。lwIP callback 可以喺 core0 持有鎖嘅時候再入來。共用狀態用 `IrqSafeLock`（`critical_section`），唔再用普通 mutex。

曾經連上過 Wi-Fi 之後斷線，唔會再呼叫 `WiFi.begin()` 或者 `WiFi.disconnect()`。呢兩個會拆走 netif，AsyncTCP 仲活著時 core0 會停低。斷線超過 60 秒就重開。

開機有帳號但未攞到 IP：只 `dhcp_start` 一次，等 lwIP 自己重試。唔好每秒 `dhcp_renew`，未有租約會把握手打回頭。45 秒仍然冇 IP 就重開。

Provisioning 進行中，`pollWifi` 唔會再自己 `WiFi.begin`。從未成功連上過嘅路徑仍然每 20 秒試一次 `WiFi.begin`。

Watchdog 大約 15 秒。死之前嘅階段寫入 scratch 0..1（bootrom 用 4..7），重開後 `/stats` 有 `last_hang_stage`、`last_hang_extension`、`last_reset_watchdog`。OTA 進度會餵 watchdog。

LittleFS 唔可以喺 HTTPS 進行中寫。轉站或清除如果撞正 HTTP，會記低，等 `pollIdle` 先寫。同一個站經 WebSocket 再送一次，唔會取消進行中嘅請求。

## WebUI 點燒入部機

`WebUiExtension.h` 用 `#embed` 讀 `webui/dist/*.gz`。SCons 只睇 `.cpp` 有冇改，唔會因為 gzip 更新就重編譯。結果係 OTA 成功，但部機仍然送舊頁。

`scripts/src/extensions/WebUI.py` 喺 npm build 之後，如果 `index.js.gz` 新過 `WebUiExtension.cpp.o`，就刪走個 object，逼下一次編譯重新嵌入。

## 未做

九巴同城巴聯營路線未合併。兩邊 API 各自只報自己嘅車，出站方向亦相反。而家揀邊個營運商就只見邊個營運商。

## 主要檔案

| 檔案 | 角色 |
| --- | --- |
| `firmware/src/services/EtaService.cpp` | 拉 API、方向過濾、緩存、格式 |
| `firmware/src/modes/TickerMode.cpp` | 滾字、幾耐再拉、失敗時保留未過期時間 |
| `firmware/src/extensions/EtaPickerExtension.cpp` | 下載站表、`/api/ticker/stop` |
| `firmware/include/services/HangTrace.h` | 死機麵包屑 |
| `firmware/include/services/IrqSafeLock.h` | core0 同 lwIP callback 共用嘅鎖 |
| `webui/src/modes/Ticker.tsx` | 五步揀站 |
| `webui/src/modes/eta/catalog.ts` | 六個營運商嘅路線、方向、站 |
| `scripts/src/extensions/WebUI.py` | 網頁更新之後強制重嵌 |
| `scripts/build_eta_picker.sh` | 提醒站表係運行時下載，唔係燒入去 |

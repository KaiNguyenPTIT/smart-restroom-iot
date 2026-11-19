# Smart Restroom — ESP32 + ThingsBoard

![ESP32](https://img.shields.io/badge/ESP32-Arduino%20framework-E7352C?logo=espressif&logoColor=white)
![MQTT](https://img.shields.io/badge/MQTT-ThingsBoard-660066?logo=mqtt&logoColor=white)
![PlatformIO](https://img.shields.io/badge/PlatformIO-esp32dev-F5822B?logo=platformio&logoColor=white)

Hệ thống tự động bật/tắt đèn và quạt cho nhà vệ sinh nhỏ, kèm giám sát từ xa qua ThingsBoard.

Bài toán: đèn và quạt bị bỏ quên khi không còn ai. Hệ thống dùng **hai đầu vào độc lập**
(siêu âm quét vùng phòng + tiệm cận tại lavabo) để suy luận trạng thái phòng, điều khiển
hai nhóm tải, và đẩy dữ liệu lên cloud để xem lịch sử và đổi chế độ từ xa.

## Nguyên tắc thiết kế: điều khiển chạy local, cloud chỉ giám sát

```
        ┌──────────────────── LOCAL (ESP32) ────────────────────┐
        │                                                       │
 HC-SR04 ──► đọc khoảng cách (60 ms) ──┐                        │
 (U)                                    ├──► suy luận trạng thái │
 Tiệm cận ──► mức LOW (mỗi vòng loop) ─┘    phòng + 2 bộ đếm    │
 (P)                                            │               │
                                                ▼               │
                              ┌─────────────────────────────┐   │
                              │  AUTO / MANUAL_ON / OFF     │   │
                              └──────────┬──────────────────┘   │
                                         ▼                      │
                              Relay 1 (GPIO5): đèn trần + quạt  │
                              Relay 2 (GPIO18): đèn gương       │
        └───────────────────────────────┬───────────────────────┘
                                        │ MQTT 1883
                                        ▼
                        ┌─────────── CLOUD (ThingsBoard) ───────────┐
                        │ telemetry → dashboard, lịch sử, export    │
                        │ RPC setMode ← nút AUTO / ON / OFF         │
                        └───────────────────────────────────────────┘
```

Đường điều khiển **không phụ thuộc Internet**. Mất mạng thì đèn/quạt vẫn chạy đúng theo cảm biến;
chỉ phần giám sát và đổi chế độ từ xa bị gián đoạn. Đây là lựa chọn có chủ đích: một hệ thống
chiếu sáng không nên ngừng hoạt động vì router reboot.

## Phần cứng

| Thành phần | Chân | Ghi chú |
|---|---|---|
| HC-SR04 TRIG / ECHO | GPIO32 / GPIO33 | ECHO 5 V qua chia áp 1 kΩ + 2 kΩ → ~3,3 V |
| Cảm biến tiệm cận lavabo | GPIO25 | `INPUT_PULLUP`, mức thấp = phát hiện |
| Relay đèn trần + quạt | GPIO5 | Kích mức thấp |
| Relay đèn gương | GPIO18 | Kích mức thấp |

Ngưỡng phát hiện: khoảng cách hợp lệ 2–400 cm, coi là "có người" khi **< 250 cm**.
Xung không nhận được trả `999` và **không** được tính là phát hiện.

## Máy trạng thái

Hai bộ đếm chạy độc lập, đều tự huỷ khi phát hiện lại:

```
U hoặc P = true            cả U và P = false
     │                            │
     ▼                            ▼
 roomOccupied = true  ──►  chờ 10 s (EMPTY_TIME)  ──►  roomOccupied = false
   (đèn trần + quạt bật)                                 (đèn trần + quạt tắt)

P = true                   P = false
     │                            │
     ▼                            ▼
autoMirrorState = true ──► chờ 3 s (MIRROR_OFF_DELAY) ──► autoMirrorState = false
   (đèn gương bật)                                        (đèn gương tắt)
```

Điểm dễ làm sai: **đèn gương chỉ phụ thuộc P**, không phụ thuộc U. Một người đi lại trong vùng
siêu âm vẫn làm đèn trần/quạt bật trong khi đèn gương đã tắt. Hai bộ đếm không dùng chung mốc thời gian.

Ba chế độ: `AUTO` (theo cảm biến), `MANUAL_ON`, `MANUAL_OFF`. Hai chế độ thủ công **chỉ ghi đè đầu ra** —
cảm biến vẫn được cập nhật liên tục, nên khi quay về AUTO thiết bị áp dụng ngay trạng thái thật,
không tạo chu kỳ chờ mới.

## Hàng đợi telemetry chịu mất mạng

Đây là phần phức tạp nhất và cũng là phần đáng nói nhất khi phỏng vấn.

- Vòng FIFO **1.800 bản ghi × 32 byte** trong RAM (`static_assert` chặn vượt ngân sách).
- Mỗi bản ghi chụp **toàn bộ giá trị tại thời điểm lấy mẫu**, không giữ con trỏ tới biến toàn cục
  (nếu giữ con trỏ, khi gửi bù sẽ gửi giá trị *hiện tại* chứ không phải giá trị *lịch sử* — sai dữ liệu).
- Timestamp gắn lúc lấy mẫu bằng đồng hồ đơn điệu 64-bit; nếu chưa có NTP thì lưu uptime và
  quy đổi lại sau khi đồng bộ giờ, trong cùng một lần chạy.
- **Không bao giờ gửi `ts = 0`/1970** — thà chờ NTP còn hơn làm hỏng chuỗi thời gian trên dashboard.
- Gửi tối đa **4 bản tin/giây**, mỗi lần `loop()` chỉ xử lý **một** bản ghi để không chặn việc đọc cảm biến.
- Hàng đợi đầy thì **bỏ bản ghi mới, giữ bản ghi cũ** — ưu tiên tính liên tục của lịch sử.

## ThingsBoard

| Hướng | Topic |
|---|---|
| Thiết bị → nền tảng | `v1/devices/me/telemetry` |
| Nền tảng → thiết bị | `v1/devices/me/rpc/request/+` |
| Thiết bị → nền tảng | `v1/devices/me/rpc/response/<requestId>` |

Telemetry key: `roomDistance`, `ultrasonicDetected`, `sinkProximity`, `roomOccupied`,
`ceilingLight`, `mirrorLight`, `fan`, `usageTime`, `mode`.
Sự kiện `roomEvent` (`ENTRY`/`EXIT`) gửi thành bản tin riêng, chỉ khi trạng thái đổi.

RPC `setMode` nhận `AUTO` | `ON` | `OFF`.

## Kiểm thử

Báo cáo định nghĩa **25 tình huống** chia ba mức bằng chứng — mỗi mức trả lời một câu hỏi khác nhau:

| Mức | Trả lời được | Không trả lời được |
|---|---|---|
| Mô phỏng (Wokwi) | Logic ngưỡng, chuyển trạng thái, hàng đợi | Độ trễ thật, vùng phủ của cảm biến |
| Bàn thử | Mức điện, cực tính relay, chia áp ECHO | Hành vi trong phòng thật |
| Trong phòng | Vị trí lắp, ngưỡng phù hợp | Độ chính xác tuyệt đối |

Ma trận gồm 13 tình huống trạng thái/chế độ (TC01–TC13) và 12 tình huống kết nối/dữ liệu (TC14–TC25).

## Hạn chế (đã biết và chấp nhận)

Ghi rõ ở đây thay vì để người đọc tự phát hiện:

- **Vùng khuất.** Người đứng lệch hướng siêu âm và không dùng lavabo có thể khiến cả U và P bằng false
  dù vẫn còn trong phòng → đèn tắt sai. Đây là giới hạn của mô hình đo, không phải bug.
- **Không nhận dạng danh tính**, không đếm số người. `roomOccupied` là suy luận có thời gian giữ.
- **Hai người đi sát nhau** hoặc ra vào trong khoảng trống ngắn hơn 10 giây có thể gộp thành một lượt.
- **Chưa có TLS** (cổng 1883). Token chỉ định danh thiết bị, không mã hoá đường truyền.
- `publish()` trả `true` nghĩa là đã ghi ra socket, **không** phải ThingsBoard đã lưu.
- Chưa có phản hồi từ tải (dòng điện/quang thông), nên trạng thái `ceilingLight` là **lệnh**,
  không phải xác nhận thiết bị đã bật.
- Chưa có phép đo tiết kiệm điện — báo cáo không suy ra con số này khi chưa đo.

## Chạy

**PlatformIO (thiết bị thật)**

```bash
cd firmware
pio run -t upload

# token không nằm trong source, truyền lúc build:
pio run -t upload --project-option 'build_flags=-DTB_TOKEN="<device-token>"'
```

**Wokwi** — board `board-esp32-devkit-c-v4`, mạng `Wokwi-GUEST`. Diagram gốc nằm trong báo cáo
(`docs/bao-cao-smart-restroom.docx`) và không được commit dưới dạng file riêng.

> **Bảo mật:** repo không chứa token ThingsBoard, mật khẩu Wi-Fi hay thông tin thiết bị thật.
> Token mẫu trong source đã được thay bằng placeholder và đọc từ `TB_TOKEN` khi build.

## Cấu trúc

```
firmware/
  src/main.cpp        toàn bộ logic: đọc cảm biến, 2 bộ đếm, hàng đợi, MQTT, RPC
  platformio.ini      board esp32dev, ArduinoJson 6.x, PubSubClient
docs/
  bao-cao-smart-restroom.docx   báo cáo đầy đủ (phân tích, thiết kế, 25 test case, hạn chế)
```

## Bối cảnh

Bài tập lớn môn **IoT và ứng dụng** — Học viện Công nghệ Bưu chính Viễn thông (PTIT), nhóm 05.
Tác giả phần mềm nhúng, hàng đợi telemetry và tích hợp ThingsBoard: Nguyễn Đình Khải.

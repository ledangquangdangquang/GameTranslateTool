# UnityTL: Việt hóa game không cần mod/hook

App viết bằng C++17, gồm giao diện cửa sổ (Dear ImGui + DirectX 11) và công cụ dòng lệnh.
App tìm chữ nằm sẵn trong file của game, dịch sang tiếng Việt, rồi **ghi thẳng vào file game**.
Không cần mod, không cần BepInEx/XUnity AutoTranslator, nên không còn lỗi do hook lúc game chạy.
Mỗi app chỉ là một file `.exe`, không cần cài gì thêm.

Hỗ trợ 4 engine: **Unity** (Mono lẫn IL2CPP), **GameMaker**, **Godot 4**, **Ren'Py**. App tự nhận ra
game thuộc engine nào từ nội dung thư mục (file `.exe` + `<Tên>_Data`, `data.win`, `<tên>.pck`, hay
`renpy/` + `game/`), không cần chọn tay.

## Định dạng chữ hỗ trợ

### Unity

| Kiểu | Ví dụ game | Ghi chú |
|---|---|---|
| **I2 Localization** (`LanguageSourceAsset`) | Rubinite | Đọc được cả khi file **bị lược typetree**: app tự dò cấu trúc, kiểm tra đọc/ghi lại phải ra đúng từng byte |
| **Unity Localization** (StringTable) | The Loopler | Mỗi ngôn ngữ một bảng, mặc định chỉ dịch bảng English |
| **JSON đa ngôn ngữ** trong TextAsset | ReStory | Kiểu `{"English": {...}, "French": {...}}` |
| **Pixel Crushers Dialogue System** | ReStory | Trường `en` / `Dialogue Text` / `Menu Text` |
| **`locstring`** (nhiều `LocalizedStringSO` rải rác) | TOEM 2 | Gom tất cả về một nguồn; cảnh báo: có game kiểu này không thật sự dùng lúc chạy |
| **`bigtsv`** (một bảng to nhiều ngôn ngữ trong 1 TextAsset) | TOEM 2 | TSV/CSV có header, cột `KEY` hoặc cột đầu làm khoá |
| **File rời `StreamingAssets/Localization/<ngôn ngữ>/*.csv`** | Magic Academy | Dòng `key;text`, một thư mục một ngôn ngữ, ngoài hệ Addressables |
| **File rời `StreamingAssets/Translations/<Language>/*.json`** | ShoSakyu | `{"guid": "text"}`, kèm `languages.json` liệt kê ngôn ngữ |

Loại file đọc được: `*.assets`, bundle UnityFS (không nén / LZ4 / LZ4HC) trong `StreamingAssets`, bản
build gói gọn `data.unity3d`, và hai kiểu file rời ở trên. Tự tắt kiểm tra CRC trong `catalog.bin`
(Addressables) cho bundle đã sửa, nếu không game sẽ hiện màn hình đen.

### GameMaker

Nhận diện khi thư mục có `data.win`. Đọc được `translations.ini` (mục `[english]`), `localization.json`
(mục `eng`/`english`), và `.csv` có cột `English` (dạng `key,English,French,...`).

### Godot 4

Nhận diện khi thư mục có `<tên>.pck` cạnh `<tên>.exe`. Đọc file `.json` tiếng Anh bên trong `.pck`.
`.pck` bị **mã hoá** (tuỳ chọn "Encrypt pack" lúc build) thì app không đọc được — cần khoá AES, hiện
chưa có cách lấy khoá tự động.

### Ren'Py

Nhận diện khi thư mục có `renpy/` + `game/` cạnh `<tên>.py`. App dùng đúng engine Ren'Py của game để tự
sinh file mẫu (`translate vietnamese`), rồi ghi bản dịch vào `game/tl/vietnamese/*.rpy` — game tự nạp,
không đụng vào script gốc. Đọc được cả game chỉ có `.rpyc` (không có `.rpy` gốc), và quét thêm chữ trên
màn hình (nút, tooltip) mà bản thân Ren'Py bỏ sót.

## Vá font chữ có dấu

Nếu chỉ dịch chữ mà không vá font, nhiều chữ tiếng Việt (ơ, ư, đ, ă, â, ê, ô, và các dấu) sẽ hiện ô vuông
hoặc mất tích vì font gốc của game không có glyph đó. App tự vá, theo đúng cách mỗi engine vẽ chữ:

- **GameMaker**: font là ảnh bitmap vẽ sẵn. App vẽ lại bằng font Windows (mặc định Constantia) rồi ghi
  đè vào chỗ của font Trung/Nhật/Hàn trong `data.win` — không dịch chuyển byte nào, đổi lại game mất
  font cho các ngôn ngữ đó.
- **Godot / Ren'Py**: font là `.ttf`/`.otf` thật. App thêm glyph còn thiếu vào thẳng file font (ghép từ
  chữ gốc + dấu, lấy dấu từ font Windows nếu font gốc không có), engine tự vẽ ra lúc chạy.
- **Unity (TextMeshPro, atlas SDF tĩnh)**: khó nhất, vì atlas được "nướng sẵn" lúc build, Unity không tự
  vẽ thêm glyph lúc chạy. App tự vẽ chữ qua GDI, tự tính SDF (Euclidean distance transform), rồi chèn
  glyph mới vào atlas theo đúng quy ước của chính font đó (đọc `_GradientScale` từ Material) — không đụng
  tới glyph cũ nào.

## Dùng app

1. Mở `unitytl.exe`, bấm **Chọn...** rồi chọn thư mục game (thư mục có file `.exe`, hoặc một thư mục cha
   của nó — với Unity, app tự tìm xuống tối đa 3 cấp).
2. App tự quét và liệt kê các **nguồn chữ** ở cột trái. Có thể bỏ tick một nguồn, hoặc đổi ngôn ngữ gốc
   của nó.
3. Chọn bộ dịch rồi bấm **Dịch các dòng chưa dịch**. App dịch từng dòng một, có thể **DỪNG** giữa chừng
   rồi chạy tiếp sau.
   - **Google (online)**: không cần cài gì thêm.
   - **Model local (GPU)**: dùng Tencent Hy-MT2-1.8B qua `llama-server` của llama.cpp. Bấm **Bật model**
     và đợi chữ "Model: đang chạy" rồi mới dịch.
     - App tự tìm `llm\llama.cpp\llama-server.exe` và file `.gguf` đầu tiên trong `llm\models\`, nằm cạnh
       thư mục `unitytl`.
     - Nếu chưa có, bấm **Tải model...**:
       - App kiểm tra card đồ hoạ rồi chọn bản phù hợp: Q8_0 hoặc Q4_K_M, và CUDA 13.4, CUDA 12.4 hoặc
         Vulkan.
       - App liệt kê từng file, nguồn và dung lượng để bạn xác nhận, rồi mới tải.
       - Đứt mạng thì lần sau tải tiếp. Tải xong app kiểm tra SHA-256, tự giải nén và tự điền đường dẫn.
     - Có thể đổi đường dẫn trong mục "Đường dẫn model".
     - Model dùng khoảng 3,3 GB VRAM và dịch mỗi dòng mất khoảng 0,5 giây.
     - Mỗi dòng được gửi nguyên văn kèm prompt chuẩn của Hy-MT. Nếu dòng có thuật ngữ trong bảng thuật
       ngữ, hoặc model làm mất tag, app chuyển sang cách che tag như với Google.
4. Rà soát trong bảng chữ:
   - Lọc theo *Chưa dịch* hoặc *Lỗi tag*.
   - Bấm vào một dòng để sửa bản dịch, app tự lưu.
5. Bấm **GHI VÀO GAME**, rồi mở game kiểm tra. Nếu game lỗi, bấm **Xem Player.log** (hoặc log tương ứng
   của engine).
6. Muốn trả game về như cũ thì bấm **Khôi phục file gốc**.

### Khi dịch, app tự lo những việc sau

- **Giữ nguyên tag và biến**: `<color=…>`, `{0}`, `[player]`, SmartFormat (`{X:plural:...}`), ký tự
  xuống dòng. Dòng nào bị dịch làm mất tag thì app không ghi, để lại tiếng Anh thay vì vỡ game.
- **Dùng bảng thuật ngữ** `_unitytl_glossary.json` (nút *Sửa bảng thuật ngữ*):
  - `keep`: tên riêng giữ nguyên, ví dụ `["Ruby", "Roy"]`.
  - `terms`: thuật ngữ dịch cố định trong câu, ví dụ `{"Talisman": "Bùa hộ mệnh"}`.
  - `whole`: dịch sẵn cả dòng cho các chữ ngắn hay bị dịch sai, ví dụ `{"Back": "Quay lại"}`.
- **Sửa lỗi viết hoa** khi bộ dịch trả về toàn chữ in hoa (ví dụ "KHÔNG" cho "No").
- **Câu trùng nhau chỉ dịch một lần**, kể cả khi nằm ở nguồn/file khác nhau.

### Khi ghi vào game, app tự lo những việc sau

- **Sao lưu file gốc** thành `.bak` ở lần ghi đầu tiên. Mọi lần ghi sau đều dựng lại từ `.bak`, nên sửa
  và ghi lại bao nhiêu lần cũng không hỏng file.
- **Lưu tiến độ dịch** trong `<thư mục game>/_unitytl.json`.

### Lưu ý

- Hãy để app **quét lần đầu khi game còn là bản gốc tiếng Anh**, vì file `.bak` tạo ra lúc đó được dùng
  làm bản gốc. Nếu game đã có `.bak` từ trước, app tự nhận các dòng đã dịch sẵn.
- **Tắt các bản dịch kiểu mod** (XUnity, `version.dll`…) để chúng không dịch đè lên bản của bạn.
- Dịch xong vài dòng nên **mở game kiểm tra ngay** trước khi dịch cả game — chữ hiện đúng không, font có
  bị lỗi không, rồi hẵng dịch tiếp.

## Dòng lệnh

```
unitytl-cli scan      <thư mục game>
unitytl-cli translate <thư mục game> [--all] [--limit N] [--local [--port 8080]]   # --local: llama-server phải đang chạy
unitytl-cli inject    <thư mục game> [--font "Constantia"] [--fold3]                # --font/--fold3: chỉ cho GameMaker
unitytl-cli restore   <thư mục game>
unitytl-cli log       <thư mục game>
unitytl-cli probe     <thư mục bất kỳ> [--top N]   # scan không ra gì? probe dò mọi file tìm đoạn chữ Anh
```

`probe` là lệnh chẩn đoán khi `scan` báo "0 nguồn" mà bạn ngờ game vẫn có chữ ở đâu đó: nó duyệt **toàn bộ file** trong thư mục (không chỉ `.assets`/bundle như `scan`) - với file Unity thì mở từng object ra đọc, với file thường thì đọc thẳng - tìm đoạn nào giống câu tiếng Anh (đủ dài, nhiều khoảng trắng, không phải tên biến/class), rồi xếp hạng file theo lượng chữ tìm được. File gây nhiễu không bao giờ chứa chữ riêng của game (runtime Mono, DLL engine Unity, file `_unitytl*` của chính app) đã bị lọc sẵn. Đây là cách đã tìm ra định dạng chữ của Magic Academy, ShoSakyu, Midnight Ritual và Escape from Duckov trước khi các kind tương ứng được viết thêm vào app.

Trong GUI, nút **"Dò file tìm chữ (probe)"** tự hiện ở cột trái khi quét không ra nguồn nào - kết quả in ra khung log phía dưới.

## Build

Cần MinGW và CMake (`scoop install mingw cmake`), rồi chạy `build.bat`. File `.exe` sẽ nằm trong `dist\`.
Build lại sau khi sửa code: `cmake --build build\cm -j 8`.

Mã nguồn trong `src/`:

| File | Nội dung |
|---|---|
| `bytes`, `json`, `lz4`, `bzip2`, `csv` | Tiện ích dùng chung (đọc/ghi nhị phân, JSON giữ thứ tự khoá, giải nén, CSV/TSV) |
| `bundle` | Đọc/ghi UnityFS |
| `serialized` | Đọc/ghi SerializedFile. Giữ nguyên metadata, chỉ sửa offset/size của object |
| `typetree` | Đọc/ghi object theo typetree, luôn kiểm tra đọc rồi ghi lại phải ra đúng byte cũ |
| `formats` | Các định dạng chữ trong file Unity (I2, StringTable, JSON, Dialogue System, `locstring`, `bigtsv`) |
| `catalog` | Vá CRC trong `catalog.bin` (Addressables) |
| `ttf` | Thêm chữ tiếng Việt vào file `.ttf`/`.otf` thật (Godot, Ren'Py); danh sách `kVietnamese` dùng chung |
| `unity_font` | Vá atlas SDF tĩnh của TextMeshPro (tự vẽ glyph qua GDI + tự tính SDF) |
| `gamemaker` | GameMaker: file chữ theo ngôn ngữ, vá font bitmap trong `data.win` (GDI vẽ chữ, WIC ghi PNG) |
| `godot` | Đọc/ghi file `.pck`, vá font `.fontdata` |
| `renpy` | Sinh file mẫu bằng chính Ren'Py của game, đọc/ghi `tl/<ngôn ngữ>/*.rpy`, đọc `.rpa` |
| `downloader` | Tải model dịch local + `llama-server` |
| `translate` | Google Translate + model local, bảo vệ tag, bảng thuật ngữ |
| `project` | Quét, lưu tiến độ, ghi vào game, khôi phục — điều phối chung cho cả 4 engine |
| `gui`, `cli` | Giao diện cửa sổ và dòng lệnh |

Chạy `tests/selftest.cpp` trên file game thật để kiểm tra mọi file đọc rồi ghi lại đều ra đúng byte gốc.
Các test khác trong `tests/` (xem comment đầu mỗi file để biết dòng lệnh build): `debug_diff` (dò chỗ
lệch khi round-trip), `gmtest`/`godottest`/`renpytest`/`ttftest` (vá font/đọc-ghi cho từng engine),
`dltest` (tải model), `tltest` (dịch thử một file JSON bằng model local).

## Giới hạn hiện tại

- Bundle nén LZMA, `catalog.json` của Addressables bản cũ, Dialogue System/StringTable bị lược typetree.
- Godot `.pck` bị mã hoá (cần khoá AES).
- Unity: chỉ vá được atlas SDF một kênh (Alpha8); chưa hỗ trợ MSDF nhiều kênh.
- GameMaker: game không có font CJK để mượn chỗ thì chưa vá được (cần dựng lại `data.win`).

Chi tiết kỹ thuật và ghi chú từng game đã làm nằm trong `CLAUDE.md` và `ROADMAP.md`.

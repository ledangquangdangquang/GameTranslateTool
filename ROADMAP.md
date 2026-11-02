# Kế hoạch UnityTL

## 1. Nút "Tải model" (dịch local khi máy chưa có model): ĐÃ XONG

Phần này nằm ở `src/downloader.cpp`. Có thể kiểm tra mà không phải tải lại vài GB bằng `tests/dltest.cpp` với 3 lệnh:
- `plan`: lập kế hoạch tải cho thư mục trống.
- `resume`: tải tiếp một file `.part` đang dở.
- `verify`: kiểm tra SHA-256 trên bản cài đã có.

Thiết kế đã làm:
- **Kiểm tra trước khi tải.** Nút **Tải model** hiện khi chưa tìm thấy server hoặc model. App xem máy có card NVIDIA không (`nvidia-smi` / DXGI), có đủ VRAM không, và ổ đĩa còn trống bao nhiêu.
- **Hộp xác nhận.** Liệt kê từng file sẽ tải, nguồn và dung lượng. Chỉ tải khi người dùng bấm đồng ý.
  - Model: `Hy-MT2-1.8B-Q8_0.gguf`, 1,91 GB, từ `huggingface.co/tencent/Hy-MT2-1.8B-GGUF`.
    Nếu VRAM dưới 3 GB thì gợi ý bản Q4_K_M (1,13 GB).
  - Chương trình chạy model `llama.cpp`, từ `github.com/ggml-org/llama.cpp/releases`:
    - Máy có card NVIDIA và driver từ 580 trở lên: bản CUDA 13.4 cùng `cudart`, khoảng 590 MB.
    - Driver cũ hơn: bản CUDA 12.4.
    - Máy không có card NVIDIA: bản Vulkan, 37 MB.
- **Tải.** Dùng WinHTTP, có thanh tiến độ và nút Dừng. Tải tiếp được khi đứt mạng (HTTP Range vào file `.part`). Tải ở luồng nền để giao diện không bị đứng.
- **Kiểm tra file.** So SHA-256 với `lfs.oid` lấy từ API Hugging Face (`/api/models/<repo>/tree/main?expand=true`). Nếu không khớp thì xóa file và báo lỗi.
- **Cài đặt.** Giải nén zip vào `llm\llama.cpp\` (dùng `tar.exe` có sẵn trong Windows, hoặc tự viết bộ giải nén zip). Sau đó tự điền đường dẫn vào cài đặt và cho bật model ngay.
- **Phiên bản llama.cpp.** Ghim một bản build đã chạy thử được (hiện là b11960) thay vì luôn lấy bản mới nhất. Có thêm nút "Kiểm tra bản mới".

## 2. Font tiếng Việt

**GameMaker: ĐÃ LÀM** (`src/gamemaker.cpp`, thử bằng `tests/gmtest.cpp`). Font Latin được vẽ lại bằng font Windows và
ghi vào chỗ của font CJK trong `data.win`, không dịch chuyển byte nào. Chưa làm: game không có font CJK để mượn chỗ
(cần dựng lại toàn bộ `data.win` như UndertaleModTool).

**Unity (font TMP tĩnh): ĐÃ LÀM** (`src/unity_font.cpp`, gọi từ `Project::inject_unity_fonts`). Khác với Godot,
TextMeshPro ở chế độ Atlas Population Mode = Static (mọi `TMP_FontAsset` gặp trong các game đã dịch cho tới nay)
không tự vẽ thêm glyph lúc chạy - atlas SDF được "nướng sẵn" lúc build, nên phải tự nướng glyph tiếng Việt và chèn
vào atlas:
- Vẽ từng chữ qua GDI (`GGO_GRAY8_BITMAP`) từ font Windows có sẵn (Cambria cho font serif, Consolas cho font mono,
  còn lại Segoe UI) ở độ phân giải gấp 4 lần kích cỡ cuối (để lấy mẫu mượt), rồi tự tính SDF đơn kênh bằng
  Euclidean distance transform đúng (thuật toán Felzenszwalb–Huttenlocher, 2 lượt 1 chiều) trên mặt nạ nhị phân,
  hạ mẫu (box filter) về kích cỡ cuối.
- Mã hoá theo đúng quy ước của chính font đó: đọc `_GradientScale` từ Material (`m_SavedProperties.m_Floats`),
  `byte = 255 * clamp(0.5 + 0.5*khoangCachTinh/_GradientScale, 0, 1)` - khớp với glyph gốc đã kiểm chứng bằng cách
  so giá trị byte thật trích từ atlas.
- Không repack lại atlas cũ: chèn glyph mới vào các hàng **thêm phía dưới** (height tăng, width giữ nguyên) bằng
  một bộ xếp hình chữ nhật kiểu "shelf" đơn giản - mọi glyph/rect cũ giữ nguyên y hệt từng byte.
- Pixel atlas thật nằm ở node `.resS` riêng trong bundle (Unity "streamed" texture), không nằm trong
  `SerializedFile` chứa `TMP_FontAsset`. Node đó được nối thêm một bản sao đầy đủ (pixel cũ + hàng mới) ở **cuối**
  node rồi trỏ lại `m_StreamData.offset/size` của `Texture2D` vào đó - vùng dữ liệu cũ bị bỏ không, không ai đụng
  tới, tránh phải tính lại offset của những asset khác có thể nằm chung node `.resS`.
- Chỉ chạy khi có dòng dịch chứa ký tự ngoài ASCII đang chờ ghi (giống cách GameMaker/Ren'Py quyết định có vá font
  hay không), và chỉ đụng tới những bundle thật sự có `TMP_FontAsset` - bundle khác giữ nguyên byte, không bị nén
  lại thành không nén.
- Đã kiểm chứng trên Magic Academy (2026-10-31): vá 23 `TMP_FontAsset` trong 7 bundle, Player.log sạch, chữ "ơ"
  trong "Trò chơi mới" ở menu chính hiện đúng trong game đang chạy.
- Chưa làm: font MSDF nhiều kênh (mọi font gặp tới nay đều là Alpha8 một kênh); chỉ vá toàn bộ font trong bundle
  thay vì chỉ font thật sự được dùng để hiển thị chữ đã dịch (tốn dung lượng hơn cần thiết).

**Godot: ĐÃ LÀM** (`src/ttf.cpp` + `fontdata_*` trong `src/godot.cpp`, thử bằng `tests/godottest.cpp` và
`tests/ttftest.cpp`). Font TrueType thiếu chữ Việt được thêm glyph ghép từ chữ và dấu của chính nó; dấu móc/râu lấy
từ Cambria Bold. Font chỉ có ASCII cũng vá được (mọi dấu lấy từ Cambria, `đ/Đ` = chữ gốc + gạch ngang, `ı` cắt từ `i`).
Chưa làm: font CFF (`OTTO`, không có glyph ghép), font không có chữ gốc Latin.

**Ren'Py: ĐÃ LÀM** (`src/renpy.cpp`): font đã vá đặt ở `game/tl/vietnamese/<đường dẫn cũ>`, Ren'Py tự lấy khi chọn tiếng Việt.

## 3. Định dạng còn thiếu

- Bundle nén LZMA.
- File `catalog.json` của Addressables bản cũ.
- Dialogue System hoặc StringTable trong file bị lược bỏ typetree. Cần sinh lại typetree từ `Managed\*.dll`, hoặc tự dò cấu trúc như đã làm với I2.

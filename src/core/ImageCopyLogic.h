#pragma once
#include <cstdint>
#include <string_view>

// "Copy image" -- when the chat offers it, and where the clipboard's pixels come
// from. Kept free of Qt so tests/image_copy_logic_test.cpp can pin both rules.
// MainWindow's context menu asks offerCopyImage; ImageClipboard follows the
// source order and passes the byte cap to MessageListModel::fetchFileBytes.
namespace talq {

// Whether a message's right-click menu offers "Copy image".
//
// hideDownload is the sharer asking that the file not be handed out. The
// Download entry already honours it (see MainWindow's context menu); a picture
// on the clipboard is handed out just as surely, so it follows the same rule.
// A fileId <= 0 is an upload still in flight: there is nothing on the server to
// fetch yet.
inline bool offerCopyImage(std::string_view mime, int fileId, bool hideDownload)
{
    return fileId > 0 && !hideDownload && mime.substr(0, 6) == "image/";
}

// Where the clipboard's pixels come from, in the order they are tried.
enum class ImageCopySource { Original, Preview, Failed };

// The ORIGINAL file is what the sender sent: full resolution and, for a
// screenshot, pixel-exact. The preview is the server's re-render -- scaled and
// re-encoded -- so it is only the first choice when this build has no decoder
// for the type (HEIC, WebP, TIFF...), which the server can still render.
inline ImageCopySource firstImageCopySource(bool canDecodeHere)
{
    return canDecodeHere ? ImageCopySource::Original : ImageCopySource::Preview;
}

// After `failed` produced no picture: the original falls back to the preview,
// and the preview is the last resort.
inline ImageCopySource nextImageCopySource(ImageCopySource failed)
{
    return failed == ImageCopySource::Original ? ImageCopySource::Preview
                                               : ImageCopySource::Failed;
}

// The largest original pulled into memory for the clipboard. Well above a phone
// photo or a 4K screenshot; a bigger file is copied from the preview instead of
// being buffered whole.
constexpr std::int64_t kImageCopyMaxOriginalBytes = 50LL * 1024 * 1024;

} // namespace talq

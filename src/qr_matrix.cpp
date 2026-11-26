#include "qr_matrix.h"
#include "qrcodegen.h"

#include <array>

QrMatrix encodeQr(const QString &text)
{
    if (text.isEmpty()) return {};
    std::array<uint8_t, qrcodegen_BUFFER_LEN_MAX> temporary;
    std::array<uint8_t, qrcodegen_BUFFER_LEN_MAX> encoded;
    if (!qrcodegen_encodeText(text.toUtf8().constData(), temporary.data(), encoded.data(),
                              qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        return {};
    }

    QrMatrix result;
    result.size = qrcodegen_getSize(encoded.data());
    result.modules.resize(result.size * result.size);
    for (int y = 0; y < result.size; ++y) {
        for (int x = 0; x < result.size; ++x) {
            result.modules[y * result.size + x] = qrcodegen_getModule(encoded.data(), x, y) ? 1 : 0;
        }
    }
    return result;
}

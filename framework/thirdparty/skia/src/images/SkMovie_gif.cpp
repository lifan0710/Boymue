/* libs/graphics/images/SkImageDecoder_libgif.cpp
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing, software 
** distributed under the License is distributed on an "AS IS" BASIS, 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#include "SkMovie.h"
#include "SkColor.h"
#include "SkColorPriv.h"
#include "SkStream.h"
#include "SkTemplates.h"
#include "SkUtils.h"

#include "gif_lib.h"

class SkGIFMovie : public SkMovie {
public:
    SkGIFMovie(SkStream* stream);
    virtual ~SkGIFMovie();

protected:
    virtual bool onGetInfo(Info*);
    virtual bool onSetTime(SkMSec);
    virtual bool onGetBitmap(SkBitmap*);
    
private:
    GifFileType* fGIF;
    int fCurrIndex;
    int fLastDrawIndex;
    SkBitmap fBackup;
};

static int Decode(GifFileType* fileType, GifByteType* out, int size) {
    SkStream* stream = (SkStream*) fileType->UserData;
    return (int) stream->read(out, size);
}

SkGIFMovie::SkGIFMovie(SkStream* stream)
{
    fGIF = DGifOpen( stream, Decode );
    if (NULL == fGIF)
        return;

    if (DGifSlurp(fGIF) != GIF_OK)
    {
        DGifCloseFile(fGIF);
        fGIF = NULL;
    }
    fCurrIndex = -1;
    fLastDrawIndex = -1;
}

SkGIFMovie::~SkGIFMovie()
{
    if (fGIF)
        DGifCloseFile(fGIF);
}

static SkMSec savedimage_duration(const SavedImage* image)
{
    for (int j = 0; j < image->ExtensionBlockCount; j++)
    {
        if (image->ExtensionBlocks[j].Function == GRAPHICS_EXT_FUNC_CODE)
        {
            int size = image->ExtensionBlocks[j].ByteCount;
            SkASSERT(size >= 4);
            const uint8_t* b = (const uint8_t*)image->ExtensionBlocks[j].Bytes;
            return ((b[2] << 8) | b[1]) * 10;
        }
    }
    return 0;
}

bool SkGIFMovie::onGetInfo(Info* info)
{
    if (NULL == fGIF)
        return false;

    SkMSec dur = 0;
    for (int i = 0; i < fGIF->ImageCount; i++)
        dur += savedimage_duration(&fGIF->SavedImages[i]);

    info->fDuration = dur;
    info->fWidth = fGIF->SWidth;
    info->fHeight = fGIF->SHeight;
    info->fIsOpaque = false;    // how to compute?
    return true;
}

bool SkGIFMovie::onSetTime(SkMSec time)
{
    if (NULL == fGIF)
        return false;

    SkMSec dur = 0;
    for (int i = 0; i < fGIF->ImageCount; i++)
    {
        dur += savedimage_duration(&fGIF->SavedImages[i]);
        if (dur >= time)
        {
            fCurrIndex = i;
            return fLastDrawIndex != fCurrIndex;
        }
    }
    fCurrIndex = fGIF->ImageCount - 1;
    return true;
}

static void copyLine(uint32_t* dst, const unsigned char* src, const ColorMapObject* cmap,
                     int transparent, int width)
{
    for (; width > 0; width--, src++, dst++) {
        if (*src != transparent) {
            const GifColorType& col = cmap->Colors[*src];
            *dst = SkPackARGB32(0xFF, col.Red, col.Green, col.Blue);
        }
    }
}

static void copyInterlaceGroup(SkBitmap* bm, const unsigned char*& src,
                               const ColorMapObject* cmap, int transparent, int copyWidth,
                               int copyHeight, const GifImageDesc& imageDesc, int rowStep,
                               int startRow)
{
    int row;
    // every 'rowStep'th row, starting with row 'startRow'
    for (row = startRow; row < copyHeight; row += rowStep) {
        uint32_t* dst = bm->getAddr32(imageDesc.Left, imageDesc.Top + row);
        copyLine(dst, src, cmap, transparent, copyWidth);
        src += imageDesc.Width;
    }

    // pad for rest height
    src += imageDesc.Width * ((imageDesc.Height - row + rowStep - 1) / rowStep);
}

static void blitInterlace(SkBitmap* bm, const SavedImage* frame, const ColorMapObject* cmap,
                          int transparent)
{
    int width = bm->width();
    int height = bm->height();
    GifWord copyWidth = frame->ImageDesc.Width;
    if (frame->ImageDesc.Left + copyWidth > width) {
        copyWidth = width - frame->ImageDesc.Left;
    }

    GifWord copyHeight = frame->ImageDesc.Height;
    if (frame->ImageDesc.Top + copyHeight > height) {
        copyHeight = height - frame->ImageDesc.Top;
    }

    // deinterlace
    const unsigned char* src = (unsigned char*)frame->RasterBits;

    // group 1 - every 8th row, starting with row 0
    copyInterlaceGroup(bm, src, cmap, transparent, copyWidth, copyHeight, frame->ImageDesc, 8, 0);

    // group 2 - every 8th row, starting with row 4
    copyInterlaceGroup(bm, src, cmap, transparent, copyWidth, copyHeight, frame->ImageDesc, 8, 4);

    // group 3 - every 4th row, starting with row 2
    copyInterlaceGroup(bm, src, cmap, transparent, copyWidth, copyHeight, frame->ImageDesc, 4, 2);

    copyInterlaceGroup(bm, src, cmap, transparent, copyWidth, copyHeight, frame->ImageDesc, 2, 1);
}

static void blitNormal(SkBitmap* bm, const SavedImage* frame, const ColorMapObject* cmap,
                       int transparent)
{
    int width = bm->width();
    int height = bm->height();
    const unsigned char* src = (unsigned char*)frame->RasterBits;
    uint32_t* dst = bm->getAddr32(frame->ImageDesc.Left, frame->ImageDesc.Top);
    GifWord copyWidth = frame->ImageDesc.Width;
    if (frame->ImageDesc.Left + copyWidth > width) {
        copyWidth = width - frame->ImageDesc.Left;
    }

    GifWord copyHeight = frame->ImageDesc.Height;
    if (frame->ImageDesc.Top + copyHeight > height) {
        copyHeight = height - frame->ImageDesc.Top;
    }

    int srcPad, dstPad;
    dstPad = width - copyWidth;
    srcPad = frame->ImageDesc.Width - copyWidth;
    for (; copyHeight > 0; copyHeight--) {
        copyLine(dst, src, cmap, transparent, copyWidth);
        src += frame->ImageDesc.Width;
        dst += width;
    }
}

static void fillRect(SkBitmap* bm, GifWord left, GifWord top, GifWord width, GifWord height,
                     uint32_t col)
{
    int bmWidth = bm->width();
    int bmHeight = bm->height();
    uint32_t* dst = bm->getAddr32(left, top);
    GifWord copyWidth = width;
    if (left + copyWidth > bmWidth) {
        copyWidth = bmWidth - left;
    }

    GifWord copyHeight = height;
    if (top + copyHeight > bmHeight) {
        copyHeight = bmHeight - top;
    }

    for (; copyHeight > 0; copyHeight--) {
        sk_memset32(dst, col, copyWidth);
        dst += bmWidth;
    }
}

static void drawFrame(SkBitmap* bm, const SavedImage* frame, const ColorMapObject* cmap)
{
    int transparent = -1;

    for (int i = 0; i < frame->ExtensionBlockCount; ++i) {
        ExtensionBlock* eb = frame->ExtensionBlocks + i;
        if (eb->Function == GRAPHICS_EXT_FUNC_CODE &&
            eb->ByteCount == 4) {
            bool has_transparency = ((eb->Bytes[0] & 1) == 1);
            if (has_transparency) {
                transparent = (unsigned char)eb->Bytes[3];
            }
        }
    }

    if (frame->ImageDesc.ColorMap != NULL) {
        // use local color table
        cmap = frame->ImageDesc.ColorMap;
    }

    if (cmap == NULL || cmap->ColorCount != (1 << cmap->BitsPerPixel)) {
        SkASSERT(!"bad colortable setup");
        return;
    }

    if (frame->ImageDesc.Interlace) {
        blitInterlace(bm, frame, cmap, transparent);
    } else {
        blitNormal(bm, frame, cmap, transparent);
    }
}

static bool checkIfWillBeCleared(const SavedImage* frame)
{
    for (int i = 0; i < frame->ExtensionBlockCount; ++i) {
        ExtensionBlock* eb = frame->ExtensionBlocks + i;
        if (eb->Function == GRAPHICS_EXT_FUNC_CODE &&
            eb->ByteCount == 4) {
            // check disposal method
            int disposal = ((eb->Bytes[0] >> 2) & 7);
            if (disposal == 2 || disposal == 3) {
                return true;
            }
        }
    }
    return false;
}

static void getTransparencyAndDisposalMethod(const SavedImage* frame, bool* trans, int* disposal)
{
    *trans = false;
    *disposal = 0;
    for (int i = 0; i < frame->ExtensionBlockCount; ++i) {
        ExtensionBlock* eb = frame->ExtensionBlocks + i;
        if (eb->Function == GRAPHICS_EXT_FUNC_CODE &&
            eb->ByteCount == 4) {
            *trans = ((eb->Bytes[0] & 1) == 1);
            *disposal = ((eb->Bytes[0] >> 2) & 7);
        }
    }
}

// return true if area of 'target' is completely covers area of 'covered'
static bool checkIfCover(const SavedImage* target, const SavedImage* covered)
{
    if (target->ImageDesc.Left <= covered->ImageDesc.Left
        && covered->ImageDesc.Left + covered->ImageDesc.Width <=
               target->ImageDesc.Left + target->ImageDesc.Width
        && target->ImageDesc.Top <= covered->ImageDesc.Top
        && covered->ImageDesc.Top + covered->ImageDesc.Height <=
               target->ImageDesc.Top + target->ImageDesc.Height) {
        return true;
    }
    return false;
}

static void disposeFrameIfNeeded(SkBitmap* bm, const SavedImage* cur, const SavedImage* next,
                                 SkBitmap* backup, SkColor color)
{
    // We can skip disposal process if next frame is not transparent
    // and completely covers current area
    bool curTrans;
    int curDisposal;
    getTransparencyAndDisposalMethod(cur, &curTrans, &curDisposal);
    bool nextTrans;
    int nextDisposal;
    getTransparencyAndDisposalMethod(next, &nextTrans, &nextDisposal);
    if ((curDisposal == 2 || curDisposal == 3)
        && (nextTrans || !checkIfCover(next, cur))) {
        switch (curDisposal) {
        // restore to background color
        // -> 'background' means background under this image.
        case 2:
            fillRect(bm, cur->ImageDesc.Left, cur->ImageDesc.Top,
                     cur->ImageDesc.Width, cur->ImageDesc.Height,
                     color);
            break;

        // restore to previous
        case 3:
            bm->swap(*backup);
            break;
        }
    }

    // Save current image if next frame's disposal method == 3
    if (nextDisposal == 3) {
        const uint32_t* src = bm->getAddr32(0, 0);
        uint32_t* dst = backup->getAddr32(0, 0);
        int cnt = bm->width() * bm->height();
        memcpy(dst, src, cnt*sizeof(uint32_t));
    }
}

bool SkGIFMovie::onGetBitmap(SkBitmap* bm)
{
    const GifFileType* gif = fGIF;
    if (NULL == gif)
        return false;

    if (gif->ImageCount < 1) {
        return false;
    }

    const int width = gif->SWidth;
    const int height = gif->SHeight;
    if (width <= 0 || height <= 0) {
        return false;
    }

    // no need to draw
    if (fLastDrawIndex >= 0 && fLastDrawIndex == fCurrIndex) {
        return true;
    }

    int startIndex = fLastDrawIndex + 1;
    if (fLastDrawIndex < 0 || !bm->readyToDraw()) {
        // first time

        startIndex = 0;

        // create bitmap
        bm->setConfig(SkBitmap::kARGB_8888_Config, width, height, 0);
        if (!bm->allocPixels(NULL)) {
            return false;
        }
        // create bitmap for backup
        fBackup.setConfig(SkBitmap::kARGB_8888_Config, width, height, 0);
        if (!fBackup.allocPixels(NULL)) {
            return false;
        }
    } else if (startIndex > fCurrIndex) {
        // rewind to 1st frame for repeat
        startIndex = 0;
    }

    int lastIndex = fCurrIndex;
    if (lastIndex < 0) {
        // first time
        lastIndex = 0;
    } else if (lastIndex > fGIF->ImageCount - 1) {
        // this block must not be reached.
        lastIndex = fGIF->ImageCount - 1;
    }

    SkColor bgColor = SkPackARGB32(0, 0, 0, 0);
    if (gif->SColorMap != NULL) {
        const GifColorType& col = gif->SColorMap->Colors[fGIF->SBackGroundColor];
        bgColor = SkColorSetARGB(0xFF, col.Red, col.Green, col.Blue);
    }

    static SkColor paintingColor = SkPackARGB32(0, 0, 0, 0);
    // draw each frames - not intelligent way
    for (int i = startIndex; i <= lastIndex; i++) {
        const SavedImage* cur = &fGIF->SavedImages[i];
        if (i == 0) {
            bool trans;
            int disposal;
            getTransparencyAndDisposalMethod(cur, &trans, &disposal);
            if (!trans && gif->SColorMap != NULL) {
                paintingColor = bgColor;
            } else {
                paintingColor = SkColorSetARGB(0, 0, 0, 0);
            }

            bm->eraseColor(paintingColor);
            fBackup.eraseColor(paintingColor);
        } else {
            // Dispose previous frame before move to next frame.
            const SavedImage* prev = &fGIF->SavedImages[i-1];
            disposeFrameIfNeeded(bm, prev, cur, &fBackup, paintingColor);
        }

        // Draw frame
        // We can skip this process if this index is not last and disposal
        // method == 2 or method == 3
        if (i == lastIndex || !checkIfWillBeCleared(cur)) {
            drawFrame(bm, cur, gif->SColorMap);
        }
    }

    // save index
    fLastDrawIndex = lastIndex;
    return true;
}

///////////////////////////////////////////////////////////////////////////////

#include "SkTRegistry.h"

SkMovie* Factory(SkStream* stream) {
    char buf[GIF_STAMP_LEN];
    if (stream->read(buf, GIF_STAMP_LEN) == GIF_STAMP_LEN) {
        if (memcmp(GIF_STAMP,   buf, GIF_STAMP_LEN) == 0 ||
                memcmp(GIF87_STAMP, buf, GIF_STAMP_LEN) == 0 ||
                memcmp(GIF89_STAMP, buf, GIF_STAMP_LEN) == 0) {
            // must rewind here, since our construct wants to re-read the data
            stream->rewind();
            return SkNEW_ARGS(SkGIFMovie, (stream));
        }
    }
    return NULL;
}

static SkTRegistry<SkMovie*, SkStream*> gReg(Factory);

#pragma once
#include <vector>
#include <cstring>
namespace EngineIcon {
inline std::vector<uint8_t> Encode(HICON icon) {
    ICONINFO info{};
    if (!icon || !GetIconInfo(icon,&info)) return {};
    BITMAP bitmap{};
    GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask,sizeof(bitmap),&bitmap);
    const uint32_t width=uint32_t((std::min)(LONG(256),bitmap.bmWidth));
    const uint32_t height=uint32_t((std::min)(LONG(256),info.hbmColor?bitmap.bmHeight:bitmap.bmHeight/2));
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (!width || !height) return {};
    BITMAPINFO dib{}; dib.bmiHeader={sizeof(BITMAPINFOHEADER),LONG(width),-LONG(height),1,32,BI_RGB};
    void* pixels{};
    HBITMAP surface=CreateDIBSection(nullptr,&dib,DIB_RGB_COLORS,&pixels,nullptr,0);
    HDC dc=CreateCompatibleDC(nullptr);
    if (!surface || !dc) { if(surface)DeleteObject(surface);if(dc)DeleteDC(dc);return {}; }
    const auto previous=SelectObject(dc,surface);
    memset(pixels,0,size_t(width)*height*4);
    DrawIconEx(dc,0,0,icon,width,height,0,nullptr,DI_NORMAL);
    std::vector<uint8_t> bytes(8+size_t(width)*height*4);
    memcpy(bytes.data(),&width,4);memcpy(bytes.data()+4,&height,4);
    memcpy(bytes.data()+8,pixels,bytes.size()-8);
    SelectObject(dc,previous);DeleteDC(dc);DeleteObject(surface);
    return bytes;
}
// Called only after ShellCore validated the exact dimensions and payload length.
inline HICON Decode(const uint8_t* data) {
    uint32_t width{},height{};memcpy(&width,data,4);memcpy(&height,data+4,4);
    BITMAPINFO dib{};dib.bmiHeader={sizeof(BITMAPINFOHEADER),LONG(width),-LONG(height),1,32,BI_RGB};
    void* pixels{};HBITMAP color=CreateDIBSection(nullptr,&dib,DIB_RGB_COLORS,&pixels,nullptr,0);
    HBITMAP mask=CreateBitmap(width,height,1,1,nullptr);
    if (!color || !mask) {if(color)DeleteObject(color);if(mask)DeleteObject(mask);return nullptr;}
    memcpy(pixels,data+8,size_t(width)*height*4);
    ICONINFO info{TRUE,0,0,mask,color};HICON icon=CreateIconIndirect(&info);
    DeleteObject(mask);DeleteObject(color);return icon;
}
}

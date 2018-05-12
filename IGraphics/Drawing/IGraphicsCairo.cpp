#include <cmath>

#include "png.h"

#include "IGraphicsCairo.h"

#ifdef OS_MAC
cairo_surface_t* LoadPNGResource(void* hInst, const WDL_String& path)
{
  return cairo_image_surface_create_from_png(path.Get());
}
#else //OS_WIN
class PNGStreamReader
{
public:
  PNGStreamReader(HMODULE hInst, const WDL_String &path)
  : mData(nullptr), mSize(0), mCount(0)
  {
    HRSRC resInfo = FindResource(hInst, path.Get(), "PNG");
    if (resInfo)
    {
      HGLOBAL res = LoadResource(hInst, resInfo);
      if (res)
      {
        mData = (uint8_t *) LockResource(res);
        mSize = SizeofResource(hInst, resInfo);
      }
    }
  }

  cairo_status_t Read(uint8_t* data, uint32_t length)
  {
    mCount += length;
    if (mCount <= mSize)
    {
      memcpy(data, mData + mCount - length, length);
      return CAIRO_STATUS_SUCCESS;
    }

    return CAIRO_STATUS_READ_ERROR;
  }

  static cairo_status_t StaticRead(void *reader, uint8_t *data, uint32_t length)
  {
    return ((PNGStreamReader*)reader)->Read(data, length);
  }
  
private:
  const uint8_t* mData;
  size_t mCount;
  size_t mSize;
};

cairo_surface_t* LoadPNGResource(void* hInst, const WDL_String& path)
{
  PNGStreamReader reader((HMODULE) hInst, path);
  return cairo_image_surface_create_from_png_stream(&PNGStreamReader::StaticRead, &reader);
}
#endif //OS_WIN

CairoBitmap::CairoBitmap(cairo_surface_t* s, int scale)
{
  cairo_surface_set_device_scale(s, scale, scale);
  int width = cairo_image_surface_get_width(s);
  int height = cairo_image_surface_get_height(s);
  
  SetBitmap(s, width, height, scale);
}
  
CairoBitmap::~CairoBitmap()
{
  cairo_surface_destroy((cairo_surface_t*) GetBitmap());
}

#pragma mark -

inline cairo_operator_t CairoBlendMode(const IBlend* pBlend)
{
  if (!pBlend)
  {
    return CAIRO_OPERATOR_OVER;
  }
  switch (pBlend->mMethod)
  {
    case kBlendClobber: return CAIRO_OPERATOR_OVER;
    case kBlendAdd: return CAIRO_OPERATOR_ADD;
    case kBlendColorDodge: return CAIRO_OPERATOR_COLOR_DODGE;
    case kBlendNone:
    default:
      return CAIRO_OPERATOR_OVER; // TODO: is this correct - same as clobber?
  }
}

#pragma mark -

IGraphicsCairo::IGraphicsCairo(IDelegate& dlg, int w, int h, int fps)
: IGraphicsPathBase(dlg, w, h, fps)
, mSurface(nullptr)
, mContext(nullptr)
{
}

IGraphicsCairo::~IGraphicsCairo() 
{
  if (mContext)
    cairo_destroy(mContext);
  
  if (mSurface)
    cairo_surface_destroy(mSurface);
}

APIBitmap* IGraphicsCairo::LoadAPIBitmap(const WDL_String& resourcePath, int scale)
{
  cairo_surface_t* pSurface = LoadPNGResource(GetPlatformInstance(), resourcePath);
    
  assert(cairo_surface_status(pSurface) == CAIRO_STATUS_SUCCESS); // Protect against typos in resource.h and .rc files.

  return new CairoBitmap(pSurface, scale);
}

APIBitmap* IGraphicsCairo::ScaleAPIBitmap(const APIBitmap* pBitmap, int scale)
{
  cairo_surface_t* pInSurface = (cairo_surface_t*) pBitmap->GetBitmap();
  
  int destW = (pBitmap->GetWidth() / pBitmap->GetScale()) * scale;
  int destH = (pBitmap->GetHeight() / pBitmap->GetScale()) * scale;
    
  // Create resources to redraw
    
  cairo_surface_t* pOutSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, destW, destH);
  cairo_t* pOutContext = cairo_create(pOutSurface);
    
  // Scale and paint (destroying the context / the surface is retained)
    
  cairo_scale(pOutContext, scale, scale);
  cairo_set_source_surface(pOutContext, pInSurface, 0, 0);
  cairo_paint(pOutContext);
  cairo_destroy(pOutContext);
    
  return new CairoBitmap(pOutSurface, scale);
}

void IGraphicsCairo::DrawBitmap(IBitmap& bitmap, const IRECT& dest, int srcX, int srcY, const IBlend* pBlend)
{
  PathStateSave();
  ClipRegion(dest);
  cairo_surface_t* surface = (cairo_surface_t*) bitmap.GetRawBitmap();
  cairo_set_source_surface(mContext, surface, std::round(dest.L) - srcX, (int) std::round(dest.T) - srcY);
  cairo_set_operator(mContext, CairoBlendMode(pBlend));
  cairo_paint_with_alpha(mContext, BlendWeight(pBlend));
  PathStateRestore();
}

void IGraphicsCairo::PathStroke(const IPattern& pattern, float thickness, const IStrokeOptions& options, const IBlend* pBlend)
{
  double dashArray[8];
  
  // First set options
  
  switch (options.mCapOption)
  {
    case kCapButt:   cairo_set_line_cap(mContext, CAIRO_LINE_CAP_BUTT);     break;
    case kCapRound:  cairo_set_line_cap(mContext, CAIRO_LINE_CAP_ROUND);    break;
    case kCapSquare: cairo_set_line_cap(mContext, CAIRO_LINE_CAP_SQUARE);   break;
  }
  
  switch (options.mJoinOption)
  {
    case kJoinMiter:   cairo_set_line_join(mContext, CAIRO_LINE_JOIN_MITER);   break;
    case kJoinRound:   cairo_set_line_join(mContext, CAIRO_LINE_JOIN_ROUND);   break;
    case kJoinBevel:   cairo_set_line_join(mContext, CAIRO_LINE_JOIN_BEVEL);   break;
  }
  
  cairo_set_miter_limit(mContext, options.mMiterLimit);
  
  for (int i = 0; i < options.mDash.GetCount(); i++)
    dashArray[i] = *(options.mDash.GetArray() + i);
  
  cairo_set_dash(mContext, dashArray, options.mDash.GetCount(), options.mDash.GetOffset());
  cairo_set_line_width(mContext, thickness);

  SetCairoSourcePattern(pattern, pBlend);
  if (options.mPreserve)
    cairo_stroke_preserve(mContext);
  else
    cairo_stroke(mContext);
}

void IGraphicsCairo::PathFill(const IPattern& pattern, const IFillOptions& options, const IBlend* pBlend) 
{
  cairo_set_fill_rule(mContext, options.mFillRule == kFillEvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
  SetCairoSourcePattern(pattern, pBlend);
  if (options.mPreserve)
    cairo_fill_preserve(mContext);
  else
    cairo_fill(mContext);
}

void IGraphicsCairo::SetCairoSourcePattern(const IPattern& pattern, const IBlend* pBlend)
{
  cairo_set_operator(mContext, CairoBlendMode(pBlend));
  
  switch (pattern.mType)
  {
    case kSolidPattern:
    {
      const IColor &color = pattern.GetStop(0).mColor;
      cairo_set_source_rgba(mContext, color.R / 255.0, color.G / 255.0, color.B / 255.0, (BlendWeight(pBlend) * color.A) / 255.0);
    }
    break;
      
    case kLinearPattern:
    case kRadialPattern:
    {
      cairo_pattern_t *cairoPattern;
      cairo_matrix_t matrix;
      const float *xform = pattern.mTransform;
      
      if (pattern.mType == kLinearPattern)
        cairoPattern = cairo_pattern_create_linear(0.0, 0.0, 1.0, 0.0);
      else
        cairoPattern = cairo_pattern_create_radial(0.0, 0.0, 0.0, 0.0, 0.0, 1.0);
      
      switch (pattern.mExtend)
      {
        case kExtendNone:      cairo_pattern_set_extend(cairoPattern, CAIRO_EXTEND_NONE);      break;
        case kExtendPad:       cairo_pattern_set_extend(cairoPattern, CAIRO_EXTEND_PAD);       break;
        case kExtendReflect:   cairo_pattern_set_extend(cairoPattern, CAIRO_EXTEND_REFLECT);   break;
        case kExtendRepeat:    cairo_pattern_set_extend(cairoPattern, CAIRO_EXTEND_REPEAT);    break;
      }
      
      for (int i = 0; i < pattern.NStops(); i++)
      {
        const IColorStop& stop = pattern.GetStop(i);
        cairo_pattern_add_color_stop_rgba(cairoPattern, stop.mOffset, stop.mColor.R / 255.0, stop.mColor.G / 255.0, stop.mColor.B / 255.0, (BlendWeight(pBlend) * stop.mColor.A) / 255.0);
      }
      
      cairo_matrix_init(&matrix, xform[0], xform[1], xform[2], xform[3], xform[4], xform[5]);
      cairo_pattern_set_matrix(cairoPattern, &matrix);
      cairo_set_source(mContext, cairoPattern);
      cairo_pattern_destroy(cairoPattern);
    }
    break;
  }
}

IColor IGraphicsCairo::GetPoint(int x, int y)
{
  // Convert suface to cairo image surface of one pixel (avoid copying the whole surface)
    
  cairo_surface_t* pOutSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t* pOutContext = cairo_create(pOutSurface);
  cairo_set_source_surface(pOutContext, mSurface, -x, -y);
  cairo_paint(pOutContext);
  cairo_surface_flush(pOutSurface);

  uint8_t* pData = cairo_image_surface_get_data(pOutSurface);
  uint32_t px = *((uint32_t*)(pData));
  
  cairo_surface_destroy(pOutSurface);
  cairo_destroy(pOutContext);
    
  int A = (px >> 0) & 0xFF;
  int R = (px >> 8) & 0xFF;
  int G = (px >> 16) & 0xFF;
  int B = (px >> 24) & 0xFF;
    
  return IColor(A, R, G, B);
}

bool IGraphicsCairo::DrawText(const IText& text, const char* str, IRECT& bounds, bool measure)
{
#if defined(OS_WIN) && defined(IGRAPHICS_FREETYPE)
  // TODO: lots!
  LoadFont("C:/Windows/Fonts/Verdana.ttf");

  assert(mFTFace != nullptr);

  cairo_font_face_t* pFace = cairo_ft_font_face_create_for_ft_face(mFTFace, 0);
  cairo_text_extents_t textExtents;
  cairo_font_extents_t fontExtents;

  cairo_set_font_face(mContext, pFace);

  cairo_font_extents(mContext, &fontExtents);
  cairo_text_extents(mContext, str, &textExtents);

  if (measure)
  {
    bounds = IRECT(0, 0, textExtents.width, fontExtents.height);
    cairo_font_face_destroy(pFace);
    return true;
  }

  double x = 0., y = 0.;

  switch (text.mAlign)
  {
    case IText::EAlign::kAlignNear:x = bounds.L; break;
    case IText::EAlign::kAlignFar: x = bounds.R - textExtents.width - textExtents.x_bearing; break;
    case IText::EAlign::kAlignCenter: x = bounds.L + ((bounds.W() - textExtents.width - textExtents.x_bearing) / 2.0); break;
    default: break;
  }

  // TODO: Add vertical alignment
  y = bounds.T + fontExtents.ascent;

  cairo_move_to(mContext, x, y);
  SetCairoSourceRGBA(text.mFGColor);
  cairo_show_text(mContext, str);
  cairo_font_face_destroy(pFace);
#endif
  
	return true;
}

bool IGraphicsCairo::MeasureText(const IText& text, const char* str, IRECT& bounds)
{
  return DrawText(text, str, bounds, true);
}

void IGraphicsCairo::SetPlatformContext(void* pContext)
{
  if (!pContext)
  {
    if (mContext)
      cairo_destroy(mContext);
    if (mSurface)
      cairo_surface_destroy(mSurface);
      
    mContext = nullptr;
    mSurface = nullptr;
  }
  else if(!mSurface)
  {
#ifdef OS_MAC
    mSurface = cairo_quartz_surface_create_for_cg_context(CGContextRef(pContext), Width(), Height());
    mContext = cairo_create(mSurface);
    cairo_surface_set_device_scale(mSurface, 1, -1);
    cairo_surface_set_device_offset(mSurface, 0, Height());
#else
    HDC dc = (HDC) pContext;
    mSurface = cairo_win32_surface_create_with_ddb(dc, CAIRO_FORMAT_ARGB32, Width(), Height());
    mContext = cairo_create(mSurface);
    cairo_surface_set_device_scale(mSurface, GetScale(), GetScale());
#endif
  }
  
  IGraphics::SetPlatformContext(pContext);
}

void IGraphicsCairo::RenderDrawBitmap()
{
  cairo_surface_flush(mSurface);

#ifdef OS_WIN
  PAINTSTRUCT ps;
  HWND hWnd = (HWND) GetWindow();
  HDC dc = BeginPaint(hWnd, &ps);
  HDC cdc = cairo_win32_surface_get_dc(mSurface);
  
  if (GetScale() == 1.f)
    BitBlt(dc, 0, 0, Width(), Height(), cdc, 0, 0, SRCCOPY);
  else
    StretchBlt(dc, 0, 0, WindowWidth(), WindowHeight(), cdc, 0, 0, Width(), Height(), SRCCOPY);

  EndPaint(hWnd, &ps);
#endif
}
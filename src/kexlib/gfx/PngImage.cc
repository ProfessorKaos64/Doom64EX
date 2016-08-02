// -*- mode: c++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 2016 Zohar Malamant
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
// 02111-1307, USA.
//
//-----------------------------------------------------------------------------

#include <kex/gfx/Image>
#include <png.h>
#include <stdlib.h>

using namespace kex::gfx;

namespace {
  struct PngImage : image_type {
      const char *mimetype() const override;
      bool detect(std::istream &) const override;
      Image load(std::istream &) const override;
      void save(std::ostream &, const Image &) const override;
  };

  const char *PngImage::mimetype() const
  { return "png"; }

  bool PngImage::detect(std::istream &s) const
  {
      static auto magic = "\x89PNG\r\n\x1a\n";
      char buf[sizeof(magic) + 1] = {};

      s.read(buf, sizeof magic);
      return std::strncmp(magic, buf, sizeof magic) == 0;
  }

  Image PngImage::load(std::istream &s) const
  {
      png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
      if (png_ptr == nullptr)
          throw image_load_error("Failed to create PNG read struct");

      png_infop infop = png_create_info_struct(png_ptr);
      if (infop == nullptr)
      {
          png_destroy_read_struct(&png_ptr, nullptr, nullptr);
          throw image_load_error("Failed to create PNG info struct");
      }

      if (setjmp(png_jmpbuf(png_ptr)))
      {
          png_destroy_read_struct(&png_ptr, &infop, nullptr);
          throw image_load_error("An error occurred in libpng");
      }

      png_set_read_fn(png_ptr, &s,
                      [](png_structp ctx, png_bytep area, png_size_t size) {
                          auto s = static_cast<std::istream *>(png_get_io_ptr(ctx));
                          s->read(reinterpret_cast<char *>(area), size);
                      });

      /* Grab offset information if available. This seems like a hack since this is
       * probably only used by Doom64EX and not a general thing as this file might imply. */
      int offsets[2];
      auto chunkFn = [](png_structp png_ptr, png_unknown_chunkp chunk) -> int {
          if (std::strncmp((char*)chunk->name, "grAb", 4) == 0 && chunk->size >= 8) {
              auto offsets = reinterpret_cast<int*>(png_get_user_chunk_ptr(png_ptr));
              auto data = reinterpret_cast<int*>(chunk->data);
              offsets[0] = kex::cpu::swap_be(data[0]);
              offsets[1] = kex::cpu::swap_be(data[1]);
              return 1;
          }

          return 0;
      };
      png_set_read_user_chunk_fn(png_ptr, offsets, chunkFn);

      png_uint_32 width, height;
      int bitDepth, colorType, interlaceMethod;
      png_read_info(png_ptr, infop);
      png_get_IHDR(png_ptr, infop, &width, &height, &bitDepth, &colorType, &interlaceMethod, nullptr, nullptr);

      png_set_strip_16(png_ptr);
      png_set_packing(png_ptr);
      png_set_interlace_handling(png_ptr);

      if (colorType == PNG_COLOR_TYPE_GRAY)
          png_set_expand(png_ptr);

      if (colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
          png_set_gray_to_rgb(png_ptr);

      png_read_update_info(png_ptr, infop);
      png_get_IHDR(png_ptr, infop, &width, &height, &bitDepth, &colorType, &interlaceMethod, nullptr, nullptr);

      pixel_format format;
      switch (colorType) {
          case PNG_COLOR_TYPE_RGB:
              format = pixel_format::rgb;
              break;

          case PNG_COLOR_TYPE_RGB_ALPHA:
              format = pixel_format::rgba;
              break;

          case PNG_COLOR_TYPE_PALETTE:
              switch (bitDepth) {
              case 8:
                  format = pixel_format::index8;
                  break;

              default:
                  throw image_load_error(fmt::format("Invalid PNG bit depth: {}", bitDepth));
              }
              break;

          default:
              throw image_load_error(fmt::format("Unknown PNG image color type: {}", colorType));
      }

      Image retval(width, height, format);
      png_bytep scanlines[height];
      for (size_t i = 0; i < height; i++)
          scanlines[i] = retval.scanline_ptr(i);

      if (colorType == PNG_COLOR_TYPE_PALETTE)
      {
          if (png_get_valid(png_ptr, infop, PNG_INFO_tRNS))
          {
              int palNum, transNum;
              png_bytep alpha;
              png_colorp colors;
              png_get_tRNS(png_ptr, infop, &alpha, &transNum, nullptr);
              png_get_PLTE(png_ptr, infop, &colors, &palNum);

              auto paldata = std::make_unique<Rgba[]>(palNum);

              for (int i = 0; i < palNum; i++)
              {
                  auto& c = paldata[i];
                  c.red = colors[i].red;
                  c.green = colors[i].green;
                  c.blue = colors[i].blue;
                  c.alpha = i < transNum ? alpha[i] : 0xff;
              }

              const auto &traits = get_pixel_traits(pixel_format::rgba);
              std::unique_ptr<uint8_t[]> p(reinterpret_cast<uint8_t*>(paldata.release()));
              retval.palette() = Palette(std::move(p), traits, traits.pal_mask, palNum);
          } else {
              png_colorp pal = nullptr;
              int numPal = 0;
              png_get_PLTE(png_ptr, infop, &pal, &numPal);

              for (auto &&c : retval.palette().map<Rgb>())
              {
                  auto p = *pal++;
                  c.red = p.red;
                  c.green = p.green;
                  c.blue = p.blue;
              }
          }
      }

      retval.offsets(offsets);

      png_read_image(png_ptr, scanlines);
      png_read_end(png_ptr, infop);

      return retval;
  }

  void PngImage::save(std::ostream &s, const Image &image) const
  {
      png_structp writep = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
      if (writep == nullptr)
          throw image_save_error("Failed getting png_structp");

      png_infop infop = png_create_info_struct(writep);
      if (infop == nullptr)
      {
          png_destroy_write_struct(&writep, nullptr);
          throw image_save_error("Failed getting png_infop");
      }

      if (setjmp(png_jmpbuf(writep)))
      {
          png_destroy_write_struct(&writep, &infop);
          throw image_save_error("Error occurred in libpng");
      }

      png_set_write_fn(writep, &s,
                       [](png_structp ctx, png_bytep data, png_size_t length) {
                           static_cast<std::ostream*>(png_get_io_ptr(ctx))->write((char*)data, length);
                       },
                       [](png_structp ctx) {
                           static_cast<std::ostream*>(png_get_io_ptr(ctx))->flush();
                       });

      Image copy;
      const Image *im = &image;

      int format;
      switch (image.format()) {
          case pixel_format::rgb:
               [[fallthrough]];
          case pixel_format::bgr:
              format = PNG_COLOR_TYPE_RGB;
              break;

          case pixel_format::rgba:
               [[fallthrough]];
          case pixel_format::bgra:
              format = PNG_COLOR_TYPE_RGB_ALPHA;
              break;

          default:
              throw image_save_error("Saving image with incompatible pixel format");
      }

      png_set_IHDR(writep, infop, im->width(), im->height(), 8,
                   format, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_DEFAULT);
      png_write_info(writep, infop);

      const uint8_t *scanlines[im->height()];
      for (int i = 0; i < im->height(); i++)
          scanlines[i] = im->scanline_ptr(i);

      png_write_image(writep, const_cast<png_bytepp>(scanlines));
      png_write_end(writep, infop);
      png_destroy_write_struct(&writep, &infop);
  }
}

std::unique_ptr<image_type> __initialize_png()
{
    return std::make_unique<PngImage>();
}
/*
 * Copyright (C) 2017 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sstream>

#include "gapii/cc/gles_spy.h"
#include "gapii/cc/gles_types.h"
#include "gapii/cc/spy.h"
#include "gapii/cc/state_serializer.h"

#include "gapis/api/gles/gles_pb/extras.pb.h"

namespace {
using namespace gapii;
using namespace gapii::GLenum;
using namespace gapii::GLbitfield;
using namespace gapii::EGLenum;

struct ImageData {
  std::unique_ptr<std::vector<uint8_t>> data;
  uint32_t width;
  uint32_t height;
  GLint sizedFormat;
  GLint dataFormat;
  GLint dataType;
};

class TempObject {
 public:
  TempObject(uint64_t id, const std::function<void()>& deleteId)
      : mId(id), mDeleteId(deleteId) {}
  uint64_t id() { return mId; }
  ~TempObject() { mDeleteId(); }

 private:
  uint64_t mId;
  std::function<void()> mDeleteId;
};

TempObject CreateAndBindFramebuffer(const GlesImports& imports,
                                    uint32_t target) {
  GLuint fb = 0;
  imports.glGenFramebuffers(1, &fb);
  imports.glBindFramebuffer(target, fb);
  return TempObject(fb, [=] {
    GLuint id = fb;
    imports.glDeleteFramebuffers(1, &id);
  });
}

TempObject CreateAndBindTexture2D(const GlesImports& imports, GLint w, GLint h,
                                  uint32_t sizedFormat) {
  GLuint tex = 0;
  imports.glGenTextures(1, &tex);
  imports.glBindTexture(GL_TEXTURE_2D, tex);
  imports.glTexStorage2D(GL_TEXTURE_2D, 1, sizedFormat, w, h);
  return TempObject(tex, [=] {
    GLuint id = tex;
    imports.glDeleteTextures(1, &id);
  });
}

TempObject CreateAndBindTextureExternal(const GlesImports& imports,
                                        EGLImageKHR handle) {
  GLuint tex = 0;
  imports.glGenTextures(1, &tex);
  imports.glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
  imports.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, handle);
  imports.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER,
                          GL_NEAREST);
  imports.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER,
                          GL_NEAREST);
  return TempObject(tex, [=] {
    GLuint id = tex;
    imports.glDeleteTextures(1, &id);
  });
}

// Creates temporary GL context which shares objects with the given application
// context. This makes it easier to do a lot of work without worrying about
// corrupting the state. For example, calling glGetError would be otherwise
// technically invalid without hacks.
TempObject CreateAndBindContext(const GlesImports& imports,
                                EGLContext sharedContext, EGLint version) {
  // Save old state.
  auto display = imports.eglGetCurrentDisplay();
  auto draw = imports.eglGetCurrentSurface(EGL_DRAW);
  auto read = imports.eglGetCurrentSurface(EGL_READ);
  auto oldCtx = imports.eglGetCurrentContext();

  // Find an EGL config.
  EGLConfig cfg;
  EGLint cfgAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  int one = 1;
  if (imports.eglChooseConfig(display, cfgAttribs, &cfg, 1, &one) ==
          EGL_FALSE ||
      one != 1) {
    GAPID_FATAL("MEC: Failed to choose EGL config: 0x%x",
                imports.eglGetError());
  }

  // Create an EGL context.
  EGLContext ctx;
  EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
  if ((ctx = imports.eglCreateContext(display, cfg, sharedContext,
                                      ctxAttribs)) == nullptr) {
    EGLint error = imports.eglGetError();
    if (sharedContext == nullptr || error != EGL_BAD_MATCH) {
      GAPID_WARNING("MEC: Failed to create EGL context: 0x%x", error);
    } else {
      GAPID_WARNING("MEC: BAD_MATCH creating shared context. Querying config.");
      EGLint configId = 42;
      if (imports.eglQueryContext(display, sharedContext, EGL_CONFIG_ID,
                                  &configId) == EGL_FALSE) {
        GAPID_WARNING("MEC: Failed to query the config ID of the context: 0x%x",
                      imports.eglGetError());
      } else {
        EGLint cfgAttribs[] = {EGL_CONFIG_ID, configId, EGL_NONE};
        if (imports.eglChooseConfig(display, cfgAttribs, &cfg, 1, &one) ==
                EGL_FALSE ||
            one != 1) {
          GAPID_WARNING("MEC: Failed to choose EGL config by id %d: 0x%x",
                        configId, imports.eglGetError());
        } else if ((ctx = imports.eglCreateContext(display, cfg, sharedContext,
                                                   ctxAttribs)) == nullptr) {
          GAPID_WARNING("MEC: Failed to create EGL context: 0x%x",
                        imports.eglGetError());
        }
      }
    }
  }

  if (ctx == nullptr) {
    return TempObject(reinterpret_cast<uint64_t>(ctx), [=] {
      if (imports.eglMakeCurrent(display, draw, read, oldCtx) == EGL_FALSE) {
        GAPID_FATAL("MEC: Failed to restore old EGL context");
      }
    });
  }

  // Create an EGL surface.
  EGLSurface surface;
  EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  if ((surface = imports.eglCreatePbufferSurface(display, cfg,
                                                 surfaceAttribs)) == nullptr) {
    GAPID_FATAL("MEC: Failed to create EGL surface: 0x%x",
                imports.eglGetError());
  }

  // Bind the EGL context.
  if (imports.eglMakeCurrent(display, surface, surface, ctx) == EGL_FALSE) {
    GAPID_FATAL("MEC: Failed to bind new EGL context: 0x%x",
                imports.eglGetError());
  }

  // Setup desirable default state for reading data.
  imports.glPixelStorei(GL_PACK_ALIGNMENT, 1);
  imports.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  imports.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  imports.glPixelStorei(GL_PACK_SKIP_ROWS, 0);

  return TempObject(reinterpret_cast<uint64_t>(ctx), [=] {
    if (imports.eglMakeCurrent(display, draw, read, oldCtx) == EGL_FALSE) {
      GAPID_FATAL("MEC: Failed to restore old EGL context");
    }
    imports.eglDestroySurface(display, surface);
    imports.eglDestroyContext(display, ctx);
  });
}

typedef struct {
  GLint r, g, b, a;
} swizzle_t;

class Sampler {
 public:
  Sampler(uint32_t target) : mTarget(target) {}

  virtual std::string getExtensions() const = 0;
  virtual std::string getUniform() const = 0;
  virtual std::string getSamplingExpression() const = 0;

  void bindTexture(const GlesImports& imports, GLint texId) const {
    imports.glBindTexture(mTarget, texId);
  }

  void getParams(const GlesImports& imports, swizzle_t* swizzle,
                 GLint* compMode) const {
    imports.glGetTexParameteriv(mTarget, GL_TEXTURE_SWIZZLE_R, &swizzle->r);
    imports.glGetTexParameteriv(mTarget, GL_TEXTURE_SWIZZLE_G, &swizzle->g);
    imports.glGetTexParameteriv(mTarget, GL_TEXTURE_SWIZZLE_B, &swizzle->b);
    imports.glGetTexParameteriv(mTarget, GL_TEXTURE_SWIZZLE_A, &swizzle->a);
    imports.glGetTexParameteriv(mTarget, GL_TEXTURE_COMPARE_MODE, compMode);
  }

  void setParams(const GlesImports& imports, swizzle_t swizzle,
                 GLint compMode) const {
    imports.glTexParameteri(mTarget, GL_TEXTURE_SWIZZLE_R, swizzle.r);
    imports.glTexParameteri(mTarget, GL_TEXTURE_SWIZZLE_G, swizzle.g);
    imports.glTexParameteri(mTarget, GL_TEXTURE_SWIZZLE_B, swizzle.b);
    imports.glTexParameteri(mTarget, GL_TEXTURE_SWIZZLE_A, swizzle.a);
    imports.glTexParameteri(mTarget, GL_TEXTURE_COMPARE_MODE, compMode);
  }

 private:
  uint32_t mTarget;
};

class Sampler2D : public Sampler {
 public:
  Sampler2D() : Sampler(GL_TEXTURE_2D) {}

  virtual std::string getExtensions() const { return ""; }

  virtual std::string getUniform() const { return "uniform sampler2D tex;"; }

  virtual std::string getSamplingExpression() const {
    return "texture2D(tex, texcoord)";
  }

  static const Sampler& get() {
    static const Sampler2D instance;
    return instance;
  }
};

class SamplerExternal : public Sampler {
 public:
  SamplerExternal() : Sampler(GL_TEXTURE_EXTERNAL_OES) {}

  virtual std::string getExtensions() const {
    return "#extension GL_OES_EGL_image_external : require\n";
  }

  virtual std::string getUniform() const {
    return "uniform samplerExternalOES tex;";
  }

  virtual std::string getSamplingExpression() const {
    return "texture2D(tex, texcoord)";
  }

  static const Sampler& get() {
    static const SamplerExternal instance;
    return instance;
  }
};

class Sampler3D : public Sampler {
 public:
  Sampler3D(float z) : Sampler(GL_TEXTURE_3D), mZ(z) {}

  virtual std::string getExtensions() const {
    return "#extension GL_OES_texture_3D : require\n";
  }

  virtual std::string getUniform() const { return "uniform sampler3D tex;"; }

  virtual std::string getSamplingExpression() const {
    std::ostringstream r;
    r << "texture3D(tex, vec3(texcoord, " << mZ << "))";
    return r.str();
  }

 private:
  float mZ;
};

void DrawTexturedQuad(const GlesImports& imports, GLsizei w, GLsizei h,
                      const Sampler& sampler) {
  GLint err;

  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Entered DrawTexturedQuad in error state: 0x%X", err);
  }

  std::string vsSource;
  vsSource += "precision highp float;\n";
  vsSource += "attribute vec2 position;\n";
  vsSource += "varying vec2 texcoord;\n";
  vsSource += "void main() {\n";
  vsSource += "  gl_Position = vec4(position, 0.5, 1.0);\n";
  vsSource += "  texcoord = position * vec2(0.5) + vec2(0.5);\n";
  vsSource += "}\n";

  std::string fsSource;
  fsSource += sampler.getExtensions();
  fsSource += "precision highp float;\n";
  fsSource += sampler.getUniform();
  fsSource += "varying vec2 texcoord;\n";
  fsSource += "void main() {\n";
  fsSource += "  gl_FragColor = " + sampler.getSamplingExpression() + ";\n";
  fsSource += "}\n";

  auto prog = imports.glCreateProgram();

  auto vs = imports.glCreateShader(GL_VERTEX_SHADER);
  char* vsSources[] = {const_cast<char*>(vsSource.data())};
  imports.glShaderSource(vs, 1, vsSources, nullptr);
  imports.glCompileShader(vs);
  imports.glAttachShader(prog, vs);

  auto fs = imports.glCreateShader(GL_FRAGMENT_SHADER);
  char* fsSources[] = {const_cast<char*>(fsSource.data())};
  imports.glShaderSource(fs, 1, fsSources, nullptr);
  imports.glCompileShader(fs);
  imports.glAttachShader(prog, fs);

  imports.glBindAttribLocation(prog, 0, "position");
  imports.glLinkProgram(prog);

  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Failed to create program: 0x%X", err);
  }

  GLint linkStatus = 0;
  imports.glGetProgramiv(prog, GL_LINK_STATUS, &linkStatus);
  if (linkStatus == 0) {
    char log[1024];
    imports.glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
    GAPID_FATAL("MEC: Failed to compile program:\n%s", log);
  }

  if ((err = imports.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)) !=
      GL_FRAMEBUFFER_COMPLETE) {
    GAPID_FATAL("MEC: Draw framebuffer incomplete: 0x%X", err);
  }

  imports.glDisable(GL_CULL_FACE);
  imports.glDisable(GL_DEPTH_TEST);
  imports.glViewport(0, 0, w, h);
  imports.glClearColor(0.0, 0.0, 0.0, 0.0);
  imports.glClear(GLbitfield::GL_COLOR_BUFFER_BIT);
  imports.glUseProgram(prog);
  GLfloat vb[] = {
      -1.0f, +1.0f,  // 2--4
      -1.0f, -1.0f,  // |\ |
      +1.0f, +1.0f,  // | \|
      +1.0f, -1.0f,  // 1--3
  };
  imports.glEnableVertexAttribArray(0);
  imports.glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, vb);
  imports.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Failed to draw quad: 0x%X", err);
  }

  imports.glDeleteShader(vs);
  imports.glDeleteShader(fs);
  imports.glDeleteProgram(prog);
}

ImageData ReadPixels(const GlesImports& imports, GLsizei w, GLsizei h) {
  ImageData img;
  uint32_t err;

  if ((err = imports.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)) !=
      GL_FRAMEBUFFER_COMPLETE) {
    GAPID_FATAL("MEC: ReadPixels: Framebuffer incomplete: 0x%X", err);
  }

  // Ask the driver what is the ideal format/type for reading the pixels.
  imports.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &img.dataFormat);
  imports.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &img.dataType);
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: ReadPixels: Failed to get data format/type: 0x%X", err);
  }
  GAPID_DEBUG("MEC: Reading pixels as format 0x%x and type 0x%x",
              img.dataFormat, img.dataType);

  auto spy = Spy::get();
  auto observer = spy->enter("subUncompressedImageSize", GlesSpy::kApiIndex);
  auto size = spy->subUncompressedImageSize(observer, [] {}, w, h,
                                            img.dataFormat, img.dataType);
  spy->exit();

  img.sizedFormat = GL_NONE;
  img.width = w;
  img.height = h;
  img.data.reset(new std::vector<uint8_t>(size));
  imports.glReadnPixels(0, 0, w, h, img.dataFormat, img.dataType,
                        img.data->size(), img.data->data());
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Failed to read pixels: 0x%X", err);
  }

  return img;
}

ImageData ReadTextureViaDrawQuad(const GlesImports& imports,
                                 const Sampler& sampler, GLint texID, GLsizei w,
                                 GLsizei h, uint32_t format, swizzle_t swizzle);

ImageData ReadOneChannelTextureViaDrawQuad(
    const GlesImports& imports, uint32_t kind, GLint texID, GLint layer,
    GLsizei w, GLsizei h, GLsizei d, uint32_t format, const char* name,
    uint32_t originalFormat, GLint rSwizzle) {
  ImageData result;
  switch (kind) {
    case GL_TEXTURE_2D:
      result =
          ReadTextureViaDrawQuad(imports, Sampler2D::get(), texID, w, h, format,
                                 {rSwizzle, GL_ZERO, GL_ZERO, GL_ONE});
      break;
    case GL_TEXTURE_3D: {
      Sampler3D sampler(1.0f / (2.0f * d) + (float)layer / d);
      result = ReadTextureViaDrawQuad(imports, sampler, texID, w, h, format,
                                      {rSwizzle, GL_ZERO, GL_ZERO, GL_ONE});
      break;
    }
    default:
      // TODO: Copy the layer/level to temporary 2D texture.
      GAPID_WARNING(
          "MEC: Reading of %s data for target 0x%x is not yet supported", name,
          kind);
      return result;
  }

  // Restore original format, so it doesn't show up as GL_RED in the UI.
  result.dataFormat = originalFormat;
  return result;
}

ImageData ReadTwoChannelTextureViaDrawQuad(
    const GlesImports& imports, uint32_t kind, GLint texID, GLint layer,
    GLsizei w, GLsizei h, GLsizei d, uint32_t format, const char* name,
    uint32_t originalFormat, GLint rSwizzle, GLint gSwizzle) {
  ImageData result;
  switch (kind) {
    case GL_TEXTURE_2D:
      result =
          ReadTextureViaDrawQuad(imports, Sampler2D::get(), texID, w, h, format,
                                 {rSwizzle, gSwizzle, GL_ZERO, GL_ONE});
      break;
    case GL_TEXTURE_3D: {
      Sampler3D sampler(1.0f / (2.0f * d) + (float)layer / d);
      result = ReadTextureViaDrawQuad(imports, sampler, texID, w, h, format,
                                      {rSwizzle, gSwizzle, GL_ZERO, GL_ONE});
    }
    default:
      // TODO: Copy the layer/level to temporary 2D texture.
      GAPID_WARNING(
          "MEC: Reading of %s data for target 0x%x is not yet supported", name,
          kind);
      return result;
  }

  // Restore original format, so it doesn't show up as GL_RG in the UI.
  result.dataFormat = originalFormat;
  return result;
}

ImageData ReadCompressedTexture(const GlesImports& imports, uint32_t kind,
                                GLint texID, GLint layer, GLsizei w, GLsizei h,
                                GLsizei d, uint32_t format, swizzle_t swizzle) {
  ImageData result;
  switch (kind) {
    case GL_TEXTURE_2D:
      result = ReadTextureViaDrawQuad(imports, Sampler2D::get(), texID, w, h,
                                      format, swizzle);
      break;
    case GL_TEXTURE_3D: {
      Sampler3D sampler(1.0f / (2.0f * d) + (float)layer / d);
      result = ReadTextureViaDrawQuad(imports, sampler, texID, w, h, format,
                                      swizzle);
      break;
    }
    default:
      // TODO: Copy the layer/level to temporary 2D texture.
      GAPID_WARNING(
          "MEC: Reading of compressed data for target 0x%x is not yet "
          "supported",
          kind);
      return result;
  }
  result.sizedFormat = format;
  return result;
}

ImageData ReadTexture(const GlesImports& imports, uint32_t kind, GLint texID,
                      GLint level, GLint layer, GLsizei w, GLsizei h, GLsizei d,
                      uint32_t format) {
  GAPID_DEBUG("MEC: Reading texture %d kind 0x%x %dx%d format 0x%x", texID,
              kind, w, h, format);
  switch (format) {
    /* depth and stencil */
    case GL_STENCIL_INDEX8:
      GAPID_WARNING("MEC: Reading of stencil data is not yet supported");
      break;
    case GL_DEPTH24_STENCIL8:
    case GL_DEPTH32F_STENCIL8:
      GAPID_WARNING("MEC: Reading of stencil data is not yet supported");
      // Fall through to depth
    case GL_DEPTH_COMPONENT16:
    case GL_DEPTH_COMPONENT24:
    case GL_DEPTH_COMPONENT32F:
      return ReadOneChannelTextureViaDrawQuad(imports, kind, texID, layer, w, h,
                                              d, GL_R32F, "depth",
                                              GL_DEPTH_COMPONENT, GL_RED);
    /* alpha and luminance */
    case GL_ALPHA8_EXT:
      return ReadOneChannelTextureViaDrawQuad(imports, kind, texID, layer, w, h,
                                              d, GL_R8, "alpha", GL_ALPHA,
                                              GL_ALPHA);
    case GL_ALPHA16F_EXT:
      return ReadOneChannelTextureViaDrawQuad(imports, kind, texID, layer, w, h,
                                              d, GL_R16F_EXT, "alpha", GL_ALPHA,
                                              GL_ALPHA);
    case GL_ALPHA32F_EXT:
      return ReadOneChannelTextureViaDrawQuad(imports, kind, texID, layer, w, h,
                                              d, GL_R32F, "alpha", GL_ALPHA,
                                              GL_ALPHA);
    case GL_LUMINANCE8_EXT:
      return ReadOneChannelTextureViaDrawQuad(imports, kind, texID, layer, w, h,
                                              d, GL_R8, "luminance",
                                              GL_LUMINANCE, GL_RED);
    case GL_LUMINANCE16F_EXT:
      return ReadOneChannelTextureViaDrawQuad(imports, kind, texID, layer, w, h,
                                              d, GL_R16F_EXT, "luminance",
                                              GL_LUMINANCE, GL_RED);
    case GL_LUMINANCE32F_EXT:
      return ReadOneChannelTextureViaDrawQuad(imports, kind, texID, layer, w, h,
                                              d, GL_R32F, "luminance",
                                              GL_LUMINANCE, GL_RED);
    case GL_LUMINANCE8_ALPHA8_EXT:
      return ReadTwoChannelTextureViaDrawQuad(
          imports, kind, texID, layer, w, h, d, GL_RG8, "luminance alpha",
          GL_LUMINANCE_ALPHA, GL_RED, GL_ALPHA);
    case GL_LUMINANCE_ALPHA16F_EXT:
      return ReadTwoChannelTextureViaDrawQuad(
          imports, kind, texID, layer, w, h, d, GL_RG16F_EXT, "luminance alpha",
          GL_LUMINANCE_ALPHA, GL_RED, GL_ALPHA);
    case GL_LUMINANCE_ALPHA32F_EXT:
      return ReadTwoChannelTextureViaDrawQuad(
          imports, kind, texID, layer, w, h, d, GL_RG32F, "luminance alpha",
          GL_LUMINANCE_ALPHA, GL_RED, GL_ALPHA);
    /* compressed 8bit RGB */
    case GL_COMPRESSED_RGB8_ETC2:
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_ATC_RGB_AMD:
    case GL_ETC1_RGB8_OES:
      return ReadCompressedTexture(imports, kind, texID, layer, w, h, d,
                                   GL_RGB8,
                                   {GL_RED, GL_GREEN, GL_BLUE, GL_ONE});
    /* compressed 8bit RGBA */
    case GL_COMPRESSED_RGBA_ASTC_4x4:
    case GL_COMPRESSED_RGBA_ASTC_5x4:
    case GL_COMPRESSED_RGBA_ASTC_5x5:
    case GL_COMPRESSED_RGBA_ASTC_6x5:
    case GL_COMPRESSED_RGBA_ASTC_6x6:
    case GL_COMPRESSED_RGBA_ASTC_8x5:
    case GL_COMPRESSED_RGBA_ASTC_8x6:
    case GL_COMPRESSED_RGBA_ASTC_8x8:
    case GL_COMPRESSED_RGBA_ASTC_10x5:
    case GL_COMPRESSED_RGBA_ASTC_10x6:
    case GL_COMPRESSED_RGBA_ASTC_10x8:
    case GL_COMPRESSED_RGBA_ASTC_10x10:
    case GL_COMPRESSED_RGBA_ASTC_12x10:
    case GL_COMPRESSED_RGBA_ASTC_12x12:
    case GL_COMPRESSED_RGBA8_ETC2_EAC:
    case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
    case GL_ATC_RGBA_EXPLICIT_ALPHA_AMD:
    case GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD:
      return ReadCompressedTexture(imports, kind, texID, layer, w, h, d,
                                   GL_RGBA8,
                                   {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA});
    /* compressed 8bit SRGB */
    case GL_COMPRESSED_SRGB8_ETC2:
      return ReadCompressedTexture(imports, kind, texID, layer, w, h, d,
                                   GL_SRGB8,
                                   {GL_RED, GL_GREEN, GL_BLUE, GL_ONE});
    /* compressed 8bit SRGBA */
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10:
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12:
    case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
    case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
      return ReadCompressedTexture(imports, kind, texID, layer, w, h, d,
                                   GL_SRGB8_ALPHA8,
                                   {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA});
    /* compressed 11bit R - Half floats have 11bit mantissa. */
    case GL_COMPRESSED_R11_EAC:
    case GL_COMPRESSED_SIGNED_R11_EAC:
      return ReadCompressedTexture(imports, kind, texID, layer, w, h, d,
                                   GL_R16F, {GL_RED, GL_ZERO, GL_ZERO, GL_ONE});
    /* compressed 11 bit RG - Half floats have 11bit mantissa. */
    case GL_COMPRESSED_RG11_EAC:
    case GL_COMPRESSED_SIGNED_RG11_EAC:
      return ReadCompressedTexture(imports, kind, texID, layer, w, h, d,
                                   GL_RG16F,
                                   {GL_RED, GL_GREEN, GL_ZERO, GL_ONE});
    /* formats that can be used as render targets */
    default: {
      auto readFb = CreateAndBindFramebuffer(imports, GL_FRAMEBUFFER);
      if (kind == GL_TEXTURE_CUBE_MAP) {
        uint32_t face = GL_TEXTURE_CUBE_MAP_POSITIVE_X + (layer % 6);
        imports.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       face, texID, level);
      } else if (layer == 0) {
        imports.glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     texID, level);
      } else {
        imports.glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          texID, level, layer);
      }
      return ReadPixels(imports, w, h);
    }
  }
  return ImageData{};
}

ImageData ReadTextureViaDrawQuad(const GlesImports& imports,
                                 const Sampler& sampler, GLint texID, GLsizei w,
                                 GLsizei h, uint32_t format,
                                 swizzle_t swizzle) {
  GAPID_DEBUG("MEC: Drawing quad to format 0x%x", format);
  GLint err;
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Entered RTvDrawQuad in error state: 0x%X", err);
  }

  auto drawFb = CreateAndBindFramebuffer(imports, GL_DRAW_FRAMEBUFFER);
  auto tmpTex = CreateAndBindTexture2D(imports, w, h, format);
  imports.glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               tmpTex.id(), 0);
  sampler.bindTexture(imports, texID);
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Failed to create framebuffer: 0x%X", err);
  }

  GLint oldCompMode = 0;
  swizzle_t oldSwizzle;
  sampler.getParams(imports, &oldSwizzle, &oldCompMode);
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Failed querying texture state: 0x%X", err);
  }

  sampler.setParams(imports, swizzle, GL_NONE);
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Failed setting texture state: 0x%X", err);
  }

  DrawTexturedQuad(imports, w, h, sampler);

  sampler.setParams(imports, oldSwizzle, oldCompMode);
  if ((err = imports.glGetError()) != 0) {
    GAPID_FATAL("MEC: Failed restoring texture state: 0x%X", err);
  }
  return ReadTexture(imports, GL_TEXTURE_2D, tmpTex.id(), 0, 0, w, h, 1,
                     format);
}

ImageData ReadRenderbuffer(const GlesImports& imports, Renderbuffer* rb) {
  auto img = rb->mImage;
  auto w = img->mWidth;
  auto h = img->mHeight;
  auto format = img->mSizedFormat;
  uint32_t attachment = GL_COLOR_ATTACHMENT0;
  switch (img->mDataFormat) {
    case GL_DEPTH_COMPONENT:
      attachment = GL_DEPTH_ATTACHMENT;
      break;
    case GL_DEPTH_STENCIL:
      attachment = GL_DEPTH_STENCIL_ATTACHMENT;
      break;
    case GL_STENCIL:
      attachment = GL_STENCIL_ATTACHMENT;
      break;
  }
  GAPID_DEBUG(
      "MEC: Reading renderbuffer %d format 0x%x type 0x%x sized 0x%x "
      "attachment 0x%x",
      rb->mID, img->mDataFormat, img->mDataType, format, attachment);
  if (attachment == GL_COLOR_ATTACHMENT0) {
    auto readFb = CreateAndBindFramebuffer(imports, GL_READ_FRAMEBUFFER);
    imports.glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_RENDERBUFFER, rb->mID);
    return ReadPixels(imports, w, h);
  } else {
    // Copy the renderbuffer data to temporary texture and then use the texture
    // reading path.
    auto readFb = CreateAndBindFramebuffer(imports, GL_READ_FRAMEBUFFER);
    auto drawFb = CreateAndBindFramebuffer(imports, GL_DRAW_FRAMEBUFFER);
    auto tmpTex = CreateAndBindTexture2D(imports, w, h, format);
    imports.glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, attachment,
                                      GL_RENDERBUFFER, rb->mID);
    imports.glFramebufferTexture(GL_DRAW_FRAMEBUFFER, attachment, tmpTex.id(),
                                 0);
    uint32_t mask =
        GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    imports.glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, mask, GL_NEAREST);
    return ReadTexture(imports, GL_TEXTURE_2D, tmpTex.id(), 0, 0, w, h, 1,
                       format);
  }
}

ImageData ReadExternal(const GlesImports& imports, EGLImageKHR handle,
                       GLsizei w, GLsizei h) {
  GAPID_DEBUG("MEC: Reading external texture 0x%p", handle);
  auto extTex = CreateAndBindTextureExternal(imports, handle);
  auto tmpTex = CreateAndBindTexture2D(imports, w, h, GL_RGBA8);
  auto fb = CreateAndBindFramebuffer(imports, GL_FRAMEBUFFER);
  imports.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, tmpTex.id(), 0);
  DrawTexturedQuad(imports, w, h, SamplerExternal::get());
  return ReadPixels(imports, w, h);
}

void SerializeAndUpdate(StateSerializer* serializer,
                        gapil::Ref<gapii::Image> current,
                        const ImageData& read) {
  if (read.data) {
    serializer->encodeBuffer<uint8_t>(
        read.data->size(), &current->mData,
        [serializer, &read](memory::Observation* obs) {
          serializer->sendData(obs, false, read.data->data(),
                               read.data->size());
        });
    current->mDataFormat = read.dataFormat;
    current->mDataType = read.dataType;
    if (read.sizedFormat) {
      current->mSizedFormat = read.sizedFormat;
    }
  }
}

}  // anonymous namespace

namespace gapii {
using namespace GLenum;
using namespace GLbitfield;
using namespace EGLenum;

void GlesSpy::GetEGLImageData(CallObserver* observer, EGLImageKHR handle,
                              GLsizei width, GLsizei height) {
  if (!should_trace(kApiIndex)) {
    return;
  }

  GAPID_DEBUG("MEC: Get EGLImage data: 0x%p x%xx%x", handle, width, height);
  auto tmpCtx = CreateAndBindContext(mImports, nullptr, 2);
  if (tmpCtx.id() == EGL_NO_CONTEXT) {
    return;
  }

  auto img = ReadExternal(mImports, handle, width, height);

  if (!img.data->empty()) {
    auto resIndex = sendResource(kApiIndex, img.data->data(), img.data->size());
    auto extra = new gles_pb::EGLImageData();
    extra->set_res_index(resIndex);
    extra->set_size(img.data->size());
    extra->set_width(width);
    extra->set_height(height);
    extra->set_format(img.dataFormat);
    extra->set_type(img.dataType);
    observer->encodeAndDelete(extra);
  }
}

void GlesSpy::serializeGPUBuffers(StateSerializer* serializer) {
  // Ensure we process shared objects only once.
  std::unordered_set<const void*> seen;
  auto once = [&](const void* ptr) { return seen.emplace(ptr).second; };

  for (auto& it : mState.EGLContexts) {
    auto handle = it.first;
    auto ctx = it.second;
    if (ctx->mOther.mDestroyed) {
      continue;
    }
    GAPID_DEBUG("MEC: processing context %d thread %s", ctx->mIdentifier,
                ctx->mOther.mThreadName.c_str());

    auto tmpCtx = CreateAndBindContext(mImports, handle, 3);
    if (tmpCtx.id() == EGL_NO_CONTEXT) {
      continue;
    }

    if (once(ctx->mObjects.mRenderbuffers.instance_ptr())) {
      for (auto& it : ctx->mObjects.mRenderbuffers) {
        auto rb = it.second;
        auto img = rb->mImage;
        if (img != nullptr) {
          auto newImg = ReadRenderbuffer(mImports, rb.get());
          SerializeAndUpdate(serializer, img, newImg);
        }
      }
    }
    if (once(ctx->mObjects.mTextures.instance_ptr())) {
      for (auto& it : ctx->mObjects.mTextures) {
        auto tex = it.second;
        auto eglImage = tex->mEGLImage.get();
        if (eglImage != nullptr) {
          if (once(eglImage)) {
            for (auto& it : eglImage->mImages) {
              auto img = it.second;
              auto newImg = ReadExternal(mImports, eglImage->mID, img->mWidth,
                                         img->mHeight);
              SerializeAndUpdate(serializer, img, newImg);
            }
          }
        } else {
          for (auto it : tex->mLevels) {
            auto level = it.first;
            GLsizei d = it.second.mLayers.count();
            for (auto it2 : it.second.mLayers) {
              auto layer = it2.first;
              auto img = it2.second;
              if (img->mSamples != 0) {
                GAPID_WARNING(
                    "MEC: Reading of multisample textures is not yet "
                    "supported");  // TODO
                continue;
              }
              auto newImg =
                  ReadTexture(mImports, tex->mKind, tex->mID, level, layer,
                              img->mWidth, img->mHeight, d, img->mSizedFormat);
              SerializeAndUpdate(serializer, img, newImg);
              GLint err;
              if ((err = mImports.glGetError()) != 0) {
                GAPID_FATAL("MEC: Failed to read texture %d: 0x%X", tex->mID,
                            err);
              }
            }
          }
        }
      }
    }
    /* TODO: read buffers from GPU. Currently disabled due to buffer data
             being required by draw calls. Need to be able to determine,
             which buffers have been written to by the GPU.
    if (once(ctx->mObjects.mBuffers.instance_ptr())) {
      for (auto& it : ctx->mObjects.mBuffers) {
        auto buffer = it.second;
        size_t size = buffer->mSize;
        if (size == 0) {
          continue;
        }
        GAPID_DEBUG("MEC: Reading buffer %d size %zu", buffer->mID, size);

        void* data;
        const uint32_t target = GL_ARRAY_BUFFER;
        if (buffer->mMapped) {
          if (buffer->mMapOffset != 0 ||
              static_cast<size_t>(buffer->mMapLength) != size) {
            // TODO: Implement - We can not unmap and remap since the
            // application has the existing pointer, and we can't copy the
            // buffer since copy is not allowed for mapped buffers.
            // Proposed solution: change glMapBuffer* to always map the whole
            // buffer, and return pointer inside that buffer to the user.
            GAPID_WARNING("MEC: Can not read partially mapped buffer")
            continue;
          }
          GAPID_DEBUG("MEC: buffer is application mapped");
          data = buffer->mMapPointer;
        } else {
          mImports.glBindBuffer(target, buffer->mID);
          data = mImports.glMapBufferRange(target, 0, size, GL_MAP_READ_BIT);
        }
        buffer->mData = serializer->encodeBuffer<uint8_t>(
            size, [=](memory::Observation* obs) {
              serializer->sendData(obs, false, data, size);
            });
        if (!buffer->mMapped) {
          mImports.glUnmapBuffer(target);
        }
      }
    }
    */
  }

  GAPID_DEBUG("MEC: done");
}

}  // namespace gapii

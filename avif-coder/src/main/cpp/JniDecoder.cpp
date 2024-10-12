//
// Created by Radzivon Bartoshyk on 15/09/2023.
//
#include <jni.h>
#include <string>
#include "libheif/heif.h"
#include "libyuv/libyuv.h"
#include "android/bitmap.h"
#include "libyuv/convert_argb.h"
#include <vector>
#include "JniException.h"
#include "SizeScaler.h"
#include <android/log.h>
#include <android/data_space.h>
#include <sys/system_properties.h>
#include "icc/lcms2.h"
#include "colorspace/colorspace.h"
#include <algorithm>
#include <limits>
#include "imagebits/RgbaF16bitToNBitU16.h"
#include "imagebits/RgbaF16bitNBitU8.h"
#include "imagebits/Rgb1010102.h"
#include "imagebits/CopyUnalignedRGBA.h"
#include "Support.h"
#include "JniBitmap.h"
#include "ReformatBitmap.h"
#include "IccRecognizer.h"
#include "thread"
#include <android/asset_manager_jni.h>
#include "imagebits/RGBAlpha.h"
#include "Eigen/Eigen"
#include "colorspace/ColorMatrix.h"
#include "definitions.h"
#include "ColorSpaceProfile.h"

using namespace std;

jobject decodeImplementationNative(JNIEnv *env, jobject thiz,
                                   AAssetManager *assetManager,
                                   std::vector<uint8_t> &srcBuffer, jint scaledWidth,
                                   jint scaledHeight, jint javaColorSpace, jint javaScaleMode,
                                   jint javaToneMapper) {
  PreferredColorConfig preferredColorConfig;
  ScaleMode scaleMode;
  CurveToneMapper toneMapper;
  if (!checkDecodePreconditions(env, javaColorSpace, &preferredColorConfig, javaScaleMode,
                                &scaleMode, javaToneMapper, &toneMapper)) {
    return static_cast<jobject>(nullptr);
  }

  std::shared_ptr<heif_context> ctx(heif_context_alloc(),
                                    [](heif_context *c) { heif_context_free(c); });
  if (!ctx) {
    std::string exception = "Can't create HEIF/AVIF decoder due to unknown reason";
    throwException(env, exception);
    return static_cast<jobject>(nullptr);
  }

  heif_context_set_max_decoding_threads(ctx.get(), (int) std::thread::hardware_concurrency());

  auto result = heif_context_read_from_memory_without_copy(ctx.get(), srcBuffer.data(),
                                                           srcBuffer.size(),
                                                           nullptr);
  if (result.code != heif_error_Ok) {
    std::string exception = "Can't read heif file exception";
    throwException(env, exception);
    return static_cast<jobject>(nullptr);
  }

  heif_image_handle *handlePtr;
  result = heif_context_get_primary_image_handle(ctx.get(), &handlePtr);
  if (result.code != heif_error_Ok || handlePtr == nullptr) {
    std::string exception = "Acquiring an image from file has failed";
    throwException(env, exception);
    return static_cast<jobject>(nullptr);
  }

  std::shared_ptr<heif_image_handle> handle(handlePtr, [](heif_image_handle *hd) {
    heif_image_handle_release(hd);
  });

  int bitDepth = heif_image_handle_get_chroma_bits_per_pixel(handle.get());
  bool useBitmapHalf16Floats = false;
  if (bitDepth < 0) {
    throwBitDepthException(env);
    return static_cast<jobject>(nullptr);
  }

  int osVersion = androidOSVersion();

  if (bitDepth > 8 && osVersion >= 26) {
    useBitmapHalf16Floats = true;
  }
  heif_image *imgPtr;
  std::shared_ptr<heif_decoding_options> options(heif_decoding_options_alloc(),
                                                 [](heif_decoding_options *deo) {
                                                   heif_decoding_options_free(deo);
                                                 });
  options->convert_hdr_to_8bit = false;
  options->ignore_transformations = false;
  result = heif_decode_image(handle.get(), &imgPtr, heif_colorspace_RGB,
                             useBitmapHalf16Floats ? heif_chroma_interleaved_RRGGBBAA_LE
                                                   : heif_chroma_interleaved_RGBA,
                             options.get());
  options.reset();

  if (result.code != heif_error_Ok || imgPtr == nullptr) {
    std::string exception = "Decoding an image has failed";
    throwException(env, exception);
    return static_cast<jobject>(nullptr);
  }

  std::shared_ptr<heif_image> img(imgPtr, [](heif_image *im) {
    heif_image_release(im);
  });

  std::vector<uint8_t> profile;
  std::string colorProfile;

  heif_color_profile_nclx *nclx = nullptr;
  bool hasNCLX = false;

  RecognizeICC(handle, img, profile, colorProfile, &nclx, &hasNCLX);

  bool alphaPremultiplied;
  if (heif_image_handle_has_alpha_channel(handle.get())) {
    alphaPremultiplied = heif_image_handle_is_premultiplied_alpha(handle.get()) != 0;
  } else {
    alphaPremultiplied = false;
  }

  int imageWidth;
  int imageHeight;
  int stride;
  aligned_uint8_vector initialData;

  imageWidth = heif_image_get_width(img.get(), heif_channel_interleaved);
  imageHeight = heif_image_get_height(img.get(), heif_channel_interleaved);
  auto scaleResult = RescaleImage(initialData, env, handle, img, &stride, useBitmapHalf16Floats,
                                  &imageWidth, &imageHeight, scaledWidth, scaledHeight,
                                  scaleMode);
  if (!scaleResult) {
    return static_cast<jobject >(nullptr);
  }

  img.reset();
  handle.reset();

  if (alphaPremultiplied && preferredColorConfig != Rgba_8888 && !useBitmapHalf16Floats) {
    // Remove premultiplied alpha because only in RGBA 8888 it is required
    coder::UnpremultiplyRGBA(reinterpret_cast<const uint8_t *>(initialData.data()),
                             stride,
                             reinterpret_cast<uint8_t *>(initialData.data()),
                             stride, imageWidth, imageHeight);
    alphaPremultiplied = false;
  }

  aligned_uint8_vector dstARGB = initialData;

  initialData.clear();

  if (!profile.empty()) {
    convertUseICC(dstARGB, stride, imageWidth, imageHeight, profile.data(),
                  profile.size(),
                  useBitmapHalf16Floats, &stride);
  } else if (hasNCLX && nclx &&
      nclx->transfer_characteristics != heif_transfer_characteristic_unspecified &&
      nclx->color_primaries != heif_color_primaries_unspecified) {
    Eigen::Matrix<float, 3, 2> primaries;
    if (nclx->color_primaries != heif_color_primaries_unspecified) {
      primaries << static_cast<float>(nclx->color_primary_red_x),
          static_cast<float>(nclx->color_primary_red_y),
          static_cast<float>(nclx->color_primary_green_x),
          static_cast<float>(nclx->color_primary_green_y),
          static_cast<float>(nclx->color_primary_blue_x),
          static_cast<float>(nclx->color_primary_blue_y);
    } else {
      primaries = getSRGBPrimaries();
    }
    Eigen::Vector2f whitePoint;
    if (nclx->color_primaries != heif_color_primaries_unspecified) {
      whitePoint << static_cast<float>(nclx->color_primary_white_x),
          static_cast<float>(nclx->color_primary_white_y);
    } else {
      whitePoint = getIlluminantD65();
    }

    Eigen::Matrix3f destinationProfile = GamutRgbToXYZ(getSRGBPrimaries(), getIlluminantD65());
    Eigen::Matrix3f sourceProfile = GamutRgbToXYZ(primaries, whitePoint);

    Eigen::Matrix3f conversion = destinationProfile.inverse() * sourceProfile;

    if (nclx->transfer_characteristics !=
        heif_transfer_characteristic_ITU_R_BT_2100_0_HLG &&
        nclx->transfer_characteristics != heif_transfer_characteristic_ITU_R_BT_2100_0_PQ) {
      toneMapper = TONE_SKIP;
    }

    TransferFunction forwardTrc = TransferFunction::Srgb;

    if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_ITU_R_BT_2100_0_HLG) {
      forwardTrc = TransferFunction::Hlg;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_SMPTE_ST_428_1) {
      forwardTrc = TransferFunction::Smpte428;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_ITU_R_BT_2100_0_PQ) {
      forwardTrc = TransferFunction::Pq;
    } else if (nclx->transfer_characteristics == heif_transfer_characteristic_linear) {
      forwardTrc = TransferFunction::Linear;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_ITU_R_BT_470_6_System_M) {
      forwardTrc = TransferFunction::Gamma2p2;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_ITU_R_BT_470_6_System_B_G) {
      forwardTrc = TransferFunction::Gamma2p8;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_ITU_R_BT_601_6) {
      forwardTrc = TransferFunction::Itur709;
    } else if (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_709_5) {
      forwardTrc = TransferFunction::Itur709;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_ITU_R_BT_2020_2_10bit ||
        nclx->transfer_characteristics ==
            heif_transfer_characteristic_ITU_R_BT_2020_2_12bit) {
      forwardTrc = TransferFunction::Itur709;
    } else if (nclx->transfer_characteristics == heif_transfer_characteristic_SMPTE_240M) {
      forwardTrc = TransferFunction::Smpte240;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_logarithmic_100) {
      forwardTrc = TransferFunction::Log100;
    } else if (nclx->transfer_characteristics ==
        heif_transfer_characteristic_logarithmic_100_sqrt10) {
      forwardTrc = TransferFunction::Log100Sqrt10;
    } else if (nclx->transfer_characteristics == heif_transfer_characteristic_IEC_61966_2_1) {
      forwardTrc = TransferFunction::Srgb;
    } else if (nclx->transfer_characteristics == heif_transfer_characteristic_IEC_61966_2_4) {
      forwardTrc = TransferFunction::Iec61966;
    } else if (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_1361) {
      forwardTrc = TransferFunction::Bt1361;
    } else if (nclx->transfer_characteristics == heif_transfer_characteristic_unspecified) {
      forwardTrc = TransferFunction::Srgb;
    }

    ITURColorCoefficients coeffs = colorPrimariesComputeYCoeffs(primaries, whitePoint);

    const float matrix[9] = {
        conversion(0, 0), conversion(0, 1), conversion(0, 2),
        conversion(1, 0), conversion(1, 1), conversion(1, 2),
        conversion(2, 0), conversion(2, 1), conversion(2, 2),
    };

    if (useBitmapHalf16Floats) {
      applyColorMatrix16Bit(reinterpret_cast<uint16_t *>(dstARGB.data()),
                            stride,
                            imageWidth,
                            imageHeight,
                            bitDepth,
                            matrix,
                            forwardTrc,
                            TransferFunction::Srgb,
                            toneMapper,
                            coeffs);
    } else {
      applyColorMatrix(reinterpret_cast<uint8_t *>(dstARGB.data()), stride, imageWidth,
                       imageHeight, matrix, forwardTrc, TransferFunction::Srgb, toneMapper,
                       coeffs);
    }
  }

  string imageConfig = useBitmapHalf16Floats ? "RGBA_F16" : "ARGB_8888";

  jobject hwBuffer = nullptr;

  try {
    coder::ReformatColorConfig(env, ref(dstARGB), ref(imageConfig), preferredColorConfig,
                               bitDepth, imageWidth,
                               imageHeight, &stride, &useBitmapHalf16Floats, &hwBuffer,
                               alphaPremultiplied);

    return createBitmap(env, ref(dstARGB), imageConfig, stride, imageWidth, imageHeight,
                        useBitmapHalf16Floats, hwBuffer);
  } catch (coder::ReformatBitmapError &err) {
    string exception(err.what());
    throwException(env, exception);
    return static_cast<jobject>(nullptr);
  }
}

extern "C"
JNIEXPORT jobject JNICALL
Java_com_radzivon_bartoshyk_avif_coder_HeifCoder_decodeImpl(JNIEnv *env, jobject thiz,
                                                            jobject assetManager,
                                                            jbyteArray byte_array, jint scaledWidth,
                                                            jint scaledHeight,
                                                            jint javaColorspace, jint scaleMode,
                                                            jint javaToneMapper) {
  try {
    auto totalLength = env->GetArrayLength(byte_array);
    std::vector<uint8_t> srcBuffer(totalLength);
    env->GetByteArrayRegion(byte_array, 0, totalLength,
                            reinterpret_cast<jbyte *>(srcBuffer.data()));
    AAssetManager *manager = nullptr;
    if (assetManager) {
      manager = AAssetManager_fromJava(env, assetManager);
    }
    return decodeImplementationNative(env, thiz, manager, srcBuffer,
                                      scaledWidth, scaledHeight,
                                      javaColorspace, scaleMode,
                                      javaToneMapper);
  } catch (std::bad_alloc &err) {
    std::string exception = "Not enough memory to decode this image";
    throwException(env, exception);
    return static_cast<jobject>(nullptr);
  }
}
extern "C"
JNIEXPORT jobject JNICALL
Java_com_radzivon_bartoshyk_avif_coder_HeifCoder_decodeByteBufferImpl(JNIEnv *env, jobject thiz,
                                                                      jobject assetManager,
                                                                      jobject byteBuffer,
                                                                      jint scaledWidth,
                                                                      jint scaledHeight,
                                                                      jint clrConfig,
                                                                      jint scaleMode,
                                                                      jint javaToneMapper) {
  try {
    auto bufferAddress = reinterpret_cast<uint8_t *>(env->GetDirectBufferAddress(byteBuffer));
    int length = (int) env->GetDirectBufferCapacity(byteBuffer);
    if (!bufferAddress || length <= 0) {
      std::string errorString = "Only direct byte buffers are supported";
      throwException(env, errorString);
      return nullptr;
    }
    std::vector<uint8_t> srcBuffer(length);
    std::copy(bufferAddress, bufferAddress + length, srcBuffer.begin());
    AAssetManager *manager = nullptr;
    if (assetManager) {
      manager = AAssetManager_fromJava(env, assetManager);
    }
    return decodeImplementationNative(env, thiz, manager, srcBuffer,
                                      scaledWidth, scaledHeight,
                                      clrConfig, scaleMode, javaToneMapper);
  } catch (std::bad_alloc &err) {
    std::string exception = "Not enough memory to decode this image";
    throwException(env, exception);
    return static_cast<jobject>(nullptr);
  }
}
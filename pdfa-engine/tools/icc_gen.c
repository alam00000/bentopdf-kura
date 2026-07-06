#include <lcms2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void cmyk_to_rgb(const double in[4], double out[3]) {
  double c = in[0], m = in[1], y = in[2], k = in[3];
  out[0] = (1.0 - c) * (1.0 - k);
  out[1] = (1.0 - m) * (1.0 - k);
  out[2] = (1.0 - y) * (1.0 - k);
}

static cmsHPROFILE hsrgb;
static cmsHPROFILE hlab;
static cmsHTRANSFORM rgb2lab;
static cmsHTRANSFORM lab2rgb;

static cmsInt32Number a2b_sampler(const cmsUInt16Number in[], cmsUInt16Number out[], void* cargo) {
  double cmyk[4], rgb[3];
  for (int i = 0; i < 4; i++) cmyk[i] = in[i] / 65535.0;
  cmyk_to_rgb(cmyk, rgb);
  cmsUInt16Number rgb16[3];
  for (int i = 0; i < 3; i++) rgb16[i] = (cmsUInt16Number)(rgb[i] * 65535.0 + 0.5);
  cmsDoTransform(rgb2lab, rgb16, out, 1);
  return 1;
}

static cmsInt32Number b2a_sampler(const cmsUInt16Number in[], cmsUInt16Number out[], void* cargo) {
  cmsUInt16Number rgb16[3];
  cmsDoTransform(lab2rgb, in, rgb16, 1);
  double r = rgb16[0] / 65535.0, g = rgb16[1] / 65535.0, b = rgb16[2] / 65535.0;
  double k = 1.0 - fmax(r, fmax(g, b));
  double c = 0, m = 0, y = 0;
  if (k < 0.9999) {
    c = (1.0 - r - k) / (1.0 - k);
    m = (1.0 - g - k) / (1.0 - k);
    y = (1.0 - b - k) / (1.0 - k);
  }
  out[0] = (cmsUInt16Number)(c * 65535.0 + 0.5);
  out[1] = (cmsUInt16Number)(m * 65535.0 + 0.5);
  out[2] = (cmsUInt16Number)(y * 65535.0 + 0.5);
  out[3] = (cmsUInt16Number)(k * 65535.0 + 0.5);
  return 1;
}

static cmsInt32Number gamut_sampler(const cmsUInt16Number in[], cmsUInt16Number out[], void* cargo) {
  out[0] = 0;
  return 1;
}

static int write_srgb(const char* path) {
  cmsHPROFILE p = cmsCreate_sRGBProfile();
  cmsSetProfileVersion(p, 2.1);
  cmsSetHeaderRenderingIntent(p, INTENT_RELATIVE_COLORIMETRIC);
  int ok = cmsSaveProfileToFile(p, path);
  cmsCloseProfile(p);
  return ok;
}

static int write_cmyk(const char* path) {
  hsrgb = cmsCreate_sRGBProfile();
  hlab = cmsCreateLab2Profile(NULL);
  rgb2lab = cmsCreateTransform(hsrgb, TYPE_RGB_16, hlab, TYPE_Lab_16, INTENT_RELATIVE_COLORIMETRIC, 0);
  lab2rgb = cmsCreateTransform(hlab, TYPE_Lab_16, hsrgb, TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC, 0);

  cmsHPROFILE p = cmsCreateProfilePlaceholder(NULL);
  cmsSetProfileVersion(p, 2.1);
  cmsSetDeviceClass(p, cmsSigOutputClass);
  cmsSetColorSpace(p, cmsSigCmykData);
  cmsSetPCS(p, cmsSigLabData);
  cmsSetHeaderRenderingIntent(p, INTENT_RELATIVE_COLORIMETRIC);

  cmsMLU* desc = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(desc, "en", "US", "Naive CMYK (composite over sRGB)");
  cmsWriteTag(p, cmsSigProfileDescriptionTag, desc);
  cmsMLU* copy = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(copy, "en", "US", "Generated with lcms2");
  cmsWriteTag(p, cmsSigCopyrightTag, copy);

  cmsCIEXYZ d50 = {cmsD50X, cmsD50Y, cmsD50Z};
  cmsWriteTag(p, cmsSigMediaWhitePointTag, &d50);

  cmsPipeline* a2b = cmsPipelineAlloc(NULL, 4, 3);
  cmsStage* clut1 = cmsStageAllocCLut16bit(NULL, 17, 4, 3, NULL);
  cmsStageSampleCLut16bit(clut1, a2b_sampler, NULL, 0);
  cmsPipelineInsertStage(a2b, cmsAT_END, clut1);
  cmsWriteTag(p, cmsSigAToB0Tag, a2b);
  cmsPipelineFree(a2b);

  cmsPipeline* b2a = cmsPipelineAlloc(NULL, 3, 4);
  cmsStage* clut2 = cmsStageAllocCLut16bit(NULL, 17, 3, 4, NULL);
  cmsStageSampleCLut16bit(clut2, b2a_sampler, NULL, 0);
  cmsPipelineInsertStage(b2a, cmsAT_END, clut2);
  cmsWriteTag(p, cmsSigBToA0Tag, b2a);
  cmsPipelineFree(b2a);

  cmsPipeline* gamut = cmsPipelineAlloc(NULL, 3, 1);
  cmsStage* clut3 = cmsStageAllocCLut16bit(NULL, 5, 3, 1, NULL);
  cmsStageSampleCLut16bit(clut3, gamut_sampler, NULL, 0);
  cmsPipelineInsertStage(gamut, cmsAT_END, clut3);
  cmsWriteTag(p, cmsSigGamutTag, gamut);
  cmsPipelineFree(gamut);

  int ok = cmsSaveProfileToFile(p, path);
  cmsCloseProfile(p);
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: icc_gen srgb.icc cmyk.icc\n");
    return 1;
  }
  if (!write_srgb(argv[1])) return 2;
  if (!write_cmyk(argv[2])) return 3;
  printf("wrote %s and %s\n", argv[1], argv[2]);
  return 0;
}

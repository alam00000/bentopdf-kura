#include "signer.hh"

#ifdef KURA_WITH_SIGNING

#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <cstdio>
#include <memory>

namespace kura {
namespace {
std::string opensslError() {
  unsigned long e = ERR_get_error();
  if (!e) return "unknown OpenSSL error";
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  return buf;
}
}

bool loadPkcs12(const std::string& path, const std::string& password, SigningKey& out,
                std::string& err) {
  std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "rb"), std::fclose);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  PKCS12* p12 = d2i_PKCS12_fp(f.get(), nullptr);
  if (!p12) {
    err = "not a PKCS#12 file: " + opensslError();
    return false;
  }
  EVP_PKEY* key = nullptr;
  X509* cert = nullptr;
  STACK_OF(X509)* chain = nullptr;
  int ok = PKCS12_parse(p12, password.c_str(), &key, &cert, &chain);
  PKCS12_free(p12);
  if (!ok || !key || !cert) {
    err = "cannot decrypt the PKCS#12 (wrong password?): " + opensslError();
    return false;
  }
  out.key = key;
  out.cert = cert;
  out.chain = chain;
  return true;
}

bool signDetached(const SigningKey& sk, const std::string& data, std::string& der,
                  std::string& err) {
  BIO* in = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
  if (!in) {
    err = "BIO allocation failed";
    return false;
  }
  int flags = CMS_DETACHED | CMS_BINARY | CMS_NOSMIMECAP | CMS_PARTIAL;
  CMS_ContentInfo* cms = CMS_sign(nullptr, nullptr, sk.chain, in, flags);
  if (!cms) {
    BIO_free(in);
    err = "CMS_sign failed: " + opensslError();
    return false;
  }
  if (!CMS_add1_signer(cms, sk.cert, sk.key, EVP_sha256(), flags)) {
    CMS_ContentInfo_free(cms);
    BIO_free(in);
    err = "CMS_add1_signer failed: " + opensslError();
    return false;
  }
  if (CMS_final(cms, in, nullptr, CMS_DETACHED | CMS_BINARY) <= 0) {
    CMS_ContentInfo_free(cms);
    BIO_free(in);
    err = "CMS_final failed: " + opensslError();
    return false;
  }
  BIO* out = BIO_new(BIO_s_mem());
  if (i2d_CMS_bio(out, cms) <= 0) {
    CMS_ContentInfo_free(cms);
    BIO_free(in);
    BIO_free(out);
    err = "DER encoding failed: " + opensslError();
    return false;
  }
  char* p = nullptr;
  long n = BIO_get_mem_data(out, &p);
  der.assign(p, static_cast<size_t>(n));
  CMS_ContentInfo_free(cms);
  BIO_free(in);
  BIO_free(out);
  return true;
}

void freeSigningKey(SigningKey& sk) {
  if (sk.key) EVP_PKEY_free(sk.key);
  if (sk.cert) X509_free(sk.cert);
  if (sk.chain) sk_X509_pop_free(sk.chain, X509_free);
  sk = SigningKey{};
}
}

#endif

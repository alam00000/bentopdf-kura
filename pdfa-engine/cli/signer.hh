#pragma once

#ifdef KURA_WITH_SIGNING

#include <openssl/ossl_typ.h>
#include <openssl/safestack.h>
#include <openssl/x509.h>

#include <string>

namespace kura {
struct SigningKey {
  EVP_PKEY* key = nullptr;
  X509* cert = nullptr;
  STACK_OF(X509)* chain = nullptr;
};

bool loadPkcs12(const std::string& path, const std::string& password, SigningKey& out,
                std::string& err);

bool signDetached(const SigningKey& sk, const std::string& data, std::string& der,
                  std::string& err);

void freeSigningKey(SigningKey& sk);
}

#endif

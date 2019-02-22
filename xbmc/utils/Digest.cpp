/*
 *  Copyright (C) 2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Digest.h"

#include "StringUtils.h"

#include <array>
#include <stdexcept>

#include <gcrypt.h>

namespace KODI
{
namespace UTILITY
{

namespace
{

int const TypeToInt(CDigest::Type type)
{
  switch (type)
  {
    case CDigest::Type::MD5:
      return GCRY_MD_MD5;
    case CDigest::Type::SHA1:
      return GCRY_MD_SHA1;
    case CDigest::Type::SHA256:
      return GCRY_MD_SHA256;
    case CDigest::Type::SHA512:
      return GCRY_MD_SHA512;
    default:
      throw std::invalid_argument("Unknown digest type");
  }
}

}

std::ostream& operator<<(std::ostream& os, TypedDigest const& digest)
{
  return os << "{" << CDigest::TypeToString(digest.type) << "}" << digest.value;
}

std::string CDigest::TypeToString(Type type)
{
  switch (type)
  {
    case Type::MD5:
      return "md5";
    case Type::SHA1:
      return "sha1";
    case Type::SHA256:
      return "sha256";
    case Type::SHA512:
      return "sha512";
    case Type::INVALID:
      return "invalid";
    default:
      throw std::invalid_argument("Unknown digest type");
  }
}

CDigest::Type CDigest::TypeFromString(std::string const& type)
{
  std::string typeLower{type};
  StringUtils::ToLower(typeLower);
  if (type == "md5")
  {
    return Type::MD5;
  }
  else if (type == "sha1")
  {
    return Type::SHA1;
  }
  else if (type == "sha256")
  {
    return Type::SHA256;
  }
  else if (type == "sha512")
  {
    return Type::SHA512;
  }
  else
  {
    throw std::invalid_argument(std::string("Unknown digest type \"") + type + "\"");
  }
}

void CDigest::MdCtxDeleter::operator()(gcry_md_hd_t context)
{
  gcry_md_close(context);
}

CDigest::CDigest(Type type)
    : m_context(), m_md(TypeToInt(type))
{
  gcry_md_hd_t hd = NULL;
  if (GPG_ERR_NO_ERROR != gcry_md_open(&hd, m_md, 0))
  {
    throw std::runtime_error("gcry_md_open failed");
  }
  m_context.reset(hd);
}

void CDigest::Update(std::string const& data)
{
  Update(data.c_str(), data.size());
}

void CDigest::Update(void const* data, std::size_t size)
{
  if (m_finalized)
  {
    throw std::logic_error("Finalized digest cannot be updated any more");
  }

  gcry_md_write(m_context.get(), data, size);
}

std::string CDigest::FinalizeRaw()
{
  if (m_finalized)
  {
    throw std::logic_error("Digest can only be finalized once");
  }

  m_finalized = true;

  std::array<unsigned char, 64> digest;
  std::size_t size = gcry_md_get_algo_dlen(m_md);
  if (size > digest.size())
  {
    throw std::runtime_error("Digest unexpectedly long");
  }
  memcpy(digest.data(), gcry_md_read(m_context.get(), m_md), size);
  return {reinterpret_cast<char*> (digest.data()), size};
}

std::string CDigest::Finalize()
{
  return StringUtils::ToHexadecimal(FinalizeRaw());
}

std::string CDigest::Calculate(Type type, std::string const& data)
{
  CDigest digest{type};
  digest.Update(data);
  return digest.Finalize();
}

std::string CDigest::Calculate(Type type, void const* data, std::size_t size)
{
  CDigest digest{type};
  digest.Update(data, size);
  return digest.Finalize();
}

}
}

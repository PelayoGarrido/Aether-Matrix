#pragma once
#include <array>
#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(_WIN32)
#include <bcrypt.h>
#endif
// The helper verifies its own model inputs, outside the DAW process.
inline void verifyMidiModel(const std::filesystem::path& path,uintmax_t size,const char* expected,const volatile std::sig_atomic_t& interrupted)
{
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path)!=size)
        throw std::runtime_error("Pinned composition model is missing or changed");
    std::ifstream input(path,std::ios::binary);if (!input) throw std::runtime_error("Cannot read composition model");
    std::array<unsigned char,65536> bytes{};std::array<unsigned char,32> digest{};
#if defined(__APPLE__)
    CC_SHA256_CTX hash{};CC_SHA256_Init(&hash);
#elif defined(_WIN32)
    struct Hash
    {
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE value=nullptr;
        ~Hash(){if (value) BCryptDestroyHash(value);if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);}
    } hash;
    if (BCryptOpenAlgorithmProvider(&hash.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 || BCryptCreateHash(hash.algorithm,&hash.value,nullptr,0,nullptr,0,0)<0)
        throw std::runtime_error("Cannot initialise composition SHA256 verification");
#endif
    uintmax_t read=0;
    while (read<size)
    {
        if (interrupted) throw std::runtime_error("Composition verification cancelled");
        const auto count=static_cast<std::streamsize>(std::min<uintmax_t>(bytes.size(),size-read));input.read(reinterpret_cast<char*>(bytes.data()),count);
        if (input.gcount()!=count) throw std::runtime_error("Composition model was truncated during verification");
#if defined(__APPLE__)
        CC_SHA256_Update(&hash,bytes.data(),static_cast<CC_LONG>(count));
#elif defined(_WIN32)
        if (BCryptHashData(hash.value,bytes.data(),static_cast<ULONG>(count),0)<0) throw std::runtime_error("Composition SHA256 verification failed");
#endif
        read+=static_cast<uintmax_t>(count);
    }
    if (input.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("Composition model grew during verification");
#if defined(__APPLE__)
    CC_SHA256_Final(digest.data(),&hash);
#elif defined(_WIN32)
    if (BCryptFinishHash(hash.value,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("Composition SHA256 verification failed");
#endif
    constexpr char hex[]="0123456789abcdef";std::string result;
    for (auto b:digest) {result.push_back(hex[b>>4]);result.push_back(hex[b&15]);}
    if (result!=expected) throw std::runtime_error("Composition model failed pinned SHA256 verification");
}

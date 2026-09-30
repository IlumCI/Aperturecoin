// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef APERTURE_GPU_H
#define APERTURE_GPU_H

#include <crypto/matmulpow_v2.h>
#include <crypto/matmulpow_v2_kernel.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

/**
 * GPU backend for ApertureMatMul v2 mining (doc/pouw-v2.md). One nonce runs
 * entirely on the device, per weight matmul:
 *   noise (BLAKE3 XOF) -> A' and W'^T -> every r x r x r ticket tile ->
 *   Fold -> ticket hash -> target check.
 * Tickets are numbered like the CPU kernels (op, then K-block s, column tile
 * j, row tile i), and Search() returns the lowest-numbered winning ticket, so
 * it agrees exactly with matmulpow_v2::SearchNonce.
 *
 * CUDA builds have two tile paths: a portable dp4a path for any r <= 32 and a
 * tensor-core (WMMA int8 m16n16k16) path for r == 32. HIP builds use the
 * portable path.
 */
namespace aperture_gpu {

struct DeviceInfo {
    int index{0};
    std::string name;
    int compute_major{0}, compute_minor{0};
};

/** Run the device math on the host against the consensus reference (no GPU needed). */
bool HostCheck(std::string& report);

/** Devices visible to the runtime (empty if none or no driver). */
std::vector<DeviceInfo> Devices(std::string* error = nullptr);

enum class TilePath { PORTABLE, TENSOR_CORE };

class Miner
{
public:
    /** Uploads the weights of every op once. Throws std::runtime_error on failure. */
    Miner(int device, unsigned int rank, const std::vector<matmulpow_v2::Op>& ops);
    ~Miner();
    Miner(const Miner&) = delete;
    Miner& operator=(const Miner&) = delete;

    /** Uploads the quantized forward-pass inputs of a template (rows x d_in per op). */
    void SetInputs(const std::vector<matmulpow_v2::OpInput>& inputs);

    /** Search one nonce; same result as matmulpow_v2::SearchNonce. */
    bool Search(const unsigned char sigma[32], const unsigned char target[32], TilePath path,
                matmulpow_v2::SearchHit& hit, uint64_t& tickets);

    /** All ticket hashes of one nonce in CPU order (for tests). */
    std::vector<std::array<unsigned char, 32>> Enumerate(const unsigned char sigma[32], TilePath path);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace aperture_gpu

#endif // APERTURE_GPU_H

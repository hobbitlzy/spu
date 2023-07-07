// Copyright 2023 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libspu/mpc/spdz2k/beaver/beaver_tinyot.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <random>

#include "Eigen/Core"
#include "yacl/base/dynamic_bitset.h"
#include "yacl/crypto/primitives/ot/ot_store.h"
#include "yacl/crypto/tools/prg.h"
#include "yacl/crypto/utils/rand.h"
#include "yacl/link/link.h"
#include "yacl/utils/matrix_utils.h"
#include "yacl/utils/serialize.h"

#include "libspu/mpc/common/prg_tensor.h"
#include "libspu/mpc/spdz2k/commitment.h"
#include "libspu/mpc/spdz2k/ot/kos_ote.h"
#include "libspu/mpc/spdz2k/ot/tiny_ot.h"
#include "libspu/mpc/utils/ring_ops.h"

namespace spu::mpc::spdz2k {
namespace {

// sqrt2k algorithm find the smallest root for residue in ring2K
// Polynomial time algorithm to find the root
// reference
// https://github.com/sagemath/sage/blob/2114066f877a28b7473bf9242b1bb11931f3ec3e/src/sage/rings/finite_rings/integer_mod.pyx#L3943
uint128_t inline Sqrt2k(uint128_t residue, uint128_t bits) {
  uint128_t x = 1;
  uint128_t N = residue;
  SPU_ENFORCE((N & 7) == 1);
  while (x < 8 && (N & 31) != ((x * x) & 31)) {
    x += 2;
  }
  uint128_t t = (N - x * x) >> 5;
  for (size_t i = 4; i < bits; ++i) {
    if (t & 1) {
      x |= (uint128_t)1 << i;
      t -= x - ((uint128_t)1 << (i - 1));
    }
    t >>= 1;
  }

  uint128_t half_mod = (uint128_t)1 << (bits - 1);
  uint128_t mask = half_mod + (half_mod - 1);
  auto l = [&mask](uint128_t val) { return val & mask; };
  return std::min({l(x), l(x + half_mod), l(-x), l(-x + half_mod)});
}

ArrayRef ring_sqrt2k(const ArrayRef& x, size_t bits = 0) {
  const auto field = x.eltype().as<Ring2k>()->field();
  const auto numel = x.numel();
  if (bits == 0) {
    bits = SizeOf(field) * 8;
  }

  ArrayRef ret = ring_zeros(field, x.numel());
  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using U = std::make_unsigned<ring2k_t>::type;

    auto x_data = ArrayView<U>(x);
    auto ret_data = ArrayView<U>(ret);
    yacl::parallel_for(0, numel, 4096, [&](int64_t beg, int64_t end) {
      for (int64_t idx = beg; idx < end; ++idx) {
        ret_data[idx] = Sqrt2k(x_data[idx], bits);
      }
    });
  });
  return ret;
}

// reference https://github.com/data61/MP-SPDZ/blob/master/Math/Z2k.hpp
uint128_t inline Invert2k(const uint128_t value, const size_t bits) {
  SPU_ENFORCE((value & 1) == 1);
  uint128_t ret = 1;
  for (size_t i = 0; i < bits; ++i) {
    if (!((value * ret >> i) & 1)) {
      ret += (uint128_t)1 << i;
    }
  }
  return ret;
}

ArrayRef ring_inv2k(const ArrayRef& x, size_t bits = 0) {
  const auto field = x.eltype().as<Ring2k>()->field();
  const auto numel = x.numel();
  if (bits == 0) {
    bits = SizeOf(field) * 8;
  }

  ArrayRef ret = ring_zeros(field, x.numel());
  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using U = std::make_unsigned<ring2k_t>::type;

    auto x_data = ArrayView<U>(x);
    auto ret_data = ArrayView<U>(ret);
    yacl::parallel_for(0, numel, 4096, [&](int64_t beg, int64_t end) {
      for (int64_t idx = beg; idx < end; ++idx) {
        ret_data[idx] = Invert2k(x_data[idx], bits);
      }
    });
  });
  return ret;
}

std::vector<bool> ring_cast_vector_boolean(const ArrayRef& x) {
  const auto field = x.eltype().as<Ring2k>()->field();

  std::vector<bool> res(x.numel());
  DISPATCH_ALL_FIELDS(field, "RingOps", [&]() {
    auto x_eigen = Eigen::Map<const Eigen::VectorX<ring2k_t>, 0,
                              Eigen::InnerStride<Eigen::Dynamic>>(
        &x.at<ring2k_t>(0), x.numel(),
        Eigen::InnerStride<Eigen::Dynamic>(x.stride()));
    yacl::parallel_for(0, x.numel(), 4096, [&](size_t start, size_t end) {
      for (size_t i = start; i < end; i++) {
        res[i] = static_cast<bool>(x_eigen[i] & 0x1);
      }
    });
  });
  return res;
}

}  // namespace

BeaverTinyOt::BeaverTinyOt(std::shared_ptr<yacl::link::Context> lctx)
    : seed_(yacl::crypto::SecureRandSeed()) {
  comm_ = std::make_shared<Communicator>(lctx);
  prg_state_ = std::make_shared<PrgState>(lctx);
  spdz2k_ot_primitives_ = std::make_shared<BasicOTProtocols>(comm_);

  auto buf = yacl::SerializeUint128(seed_);
  std::vector<yacl::Buffer> all_bufs =
      yacl::link::Gather(lctx, buf, 0, "BEAVER_TINY:SYNC_SEEDS");

  if (comm_->getRank() == 0) {
    // Collects seeds from all parties.
    for (size_t rank = 0; rank < comm_->getWorldSize(); ++rank) {
      PrgSeed seed = yacl::DeserializeUint128(all_bufs[rank]);
      tp_.setSeed(rank, comm_->getWorldSize(), seed);
    }
  }

  auto recv_opts_choices = yacl::dynamic_bitset<uint128_t>(kappa_);
  auto recv_opts_blocks = std::vector<uint128_t>(kappa_);

  auto send_opts_blocks = std::vector<std::array<uint128_t, 2>>(kappa_);

  if (comm_->getRank() == 0) {
    yacl::crypto::BaseOtRecv(comm_->lctx(), recv_opts_choices,
                             absl::MakeSpan(recv_opts_blocks));
    yacl::crypto::BaseOtSend(comm_->lctx(), absl::MakeSpan(send_opts_blocks));
  } else {
    yacl::crypto::BaseOtSend(comm_->lctx(), absl::MakeSpan(send_opts_blocks));
    yacl::crypto::BaseOtRecv(comm_->lctx(), recv_opts_choices,
                             absl::MakeSpan(recv_opts_blocks));
  }

  recv_opts_ = std::make_shared<yacl::crypto::OtRecvStore>(
      yacl::crypto::MakeOtRecvStore(recv_opts_choices, recv_opts_blocks));

  send_opts_ = std::make_shared<yacl::crypto::OtSendStore>(
      yacl::crypto::MakeOtSendStore(send_opts_blocks));

  // the choices of BaseOT options would be the delta in delta OT
  // which means that delta is the "key" in TinyOT
  tinyot_key_ = 0;
  for (size_t k = 0; k < kappa_; ++k) {
    if (recv_opts_->GetChoice(k)) {
      tinyot_key_ |= (uint128_t)1 << k;
    }
  }
}

uint128_t BeaverTinyOt::InitSpdzKey(FieldType field, size_t s) {
  spdz_key_ = yacl::crypto::SecureRandSeed();
  spdz_key_ &= ((uint128_t)1 << s) - 1;
  return spdz_key_;
}

// Refer to:
// Fig. 11 Protocol for authenticating secret-shared values
// SPDZ2k: Efficient MPC mod 2k for Dishonest Majority
// - https://eprint.iacr.org/2018/482.pdf
ArrayRef BeaverTinyOt::AuthArrayRef(const ArrayRef& x, FieldType field,
                                    size_t k, size_t s) {
  return DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using T = ring2k_t;

    // 1. l_ = max(l, r + s, 2s)
    SPDLOG_DEBUG("AuthArrayRef start with numel {}", x.numel());
    const int l = k + s;
    const int r = k;
    int l_ = std::max(l, r + static_cast<int>(s));
    l_ = std::max(l_, 2 * static_cast<int>(s));
    l_ = std::min(l_, static_cast<int>(SizeOf(field) * 8));
    SPU_ENFORCE(l_ >= static_cast<int>(SizeOf(field) * 8), "k = s");

    // 2. sample random masks
    int64_t t = x.numel();
    size_t new_numel = t + 1;
    ArrayRef x_hat(x.eltype(), new_numel);
    auto x_mask = ring_rand(field, 1);
    for (int i = 0; i < t; ++i) {
      x_hat.at<T>(i) = x.at<T>(i);
    }
    x_hat.at<T>(t) = x_mask.at<T>(0);

    // 3. every pair calls vole && 4. receives vole output
    size_t WorldSize = comm_->getWorldSize();
    size_t rank = comm_->getRank();

    std::vector<ArrayRef> a, b;
    auto alpha = ring_mul(ring_ones(field, new_numel), spdz_key_);
    for (size_t i = 0; i < WorldSize; ++i) {
      for (size_t j = 0; j < WorldSize; ++j) {
        if (i == j) {
          continue;
        }

        if (i == rank) {
          auto tmp = voleRecv(field, alpha);
          a.emplace_back(tmp);
        }
        if (j == rank) {
          auto tmp = voleSend(field, x_hat);
          b.emplace_back(tmp);
        }
      }
    }

    // 5. each party defines the MAC share
    auto a_b = ring_zeros(field, new_numel);
    for (size_t i = 0; i < WorldSize - 1; ++i) {
      ring_add_(a_b, ring_sub(a[i], b[i]));
    }

    auto m = ring_add(ring_mul(x_hat, spdz_key_), a_b);

    // Consistency check
    // 6. get l public random values
    auto pub_r = prg_state_->genPubl(field, new_numel);
    std::vector<int> rv;
    size_t numel = x.numel();
    for (size_t i = 0; i < numel; ++i) {
      rv.emplace_back(pub_r.at<T>(i));
    }
    rv.emplace_back(1);

    // 7. caculate x_angle && 8. caculate m_angle
    T x_angle = 0;
    T m_angle = 0;
    for (size_t i = 0; i < new_numel; ++i) {
      // x_hat, not x
      x_angle += rv[i] * x_hat.at<T>(i);
      m_angle += rv[i] * m.at<T>(i);
    }

    auto x_angle_sum =
        comm_->allReduce<T, std::plus>(std::vector{x_angle}, "allReduce x_ref");

    // 9. commmit and open
    auto z = m_angle - x_angle_sum[0] * spdz_key_;
    std::string z_str((char*)&z, sizeof(z));
    std::vector<std::string> recv_strs;
    SPU_ENFORCE(commit_and_open(comm_->lctx(), z_str, &recv_strs));
    SPU_ENFORCE(recv_strs.size() == WorldSize);

    // 10. check
    T plain_z = 0;
    for (const auto& str : recv_strs) {
      T t = *(reinterpret_cast<const T*>(str.data()));
      plain_z += t;
    }

    SPU_ENFORCE(plain_z == 0);

    // 11. output MAC share
    return m.slice(0, m.numel() - 1);
  });
}

BeaverTinyOt::Pair BeaverTinyOt::AuthCoinTossing(FieldType field, size_t size,
                                                 size_t k, size_t s) {
  auto rand = ring_rand(field, size);
  auto mac = AuthArrayRef(rand, field, k, s);
  return {rand, mac};
}

// Refer to:
// New Primitives for Actively-Secure MPC over Rings with Applications to
// Private Machine Learning.
// Figure 2: TinyOT share to binary SPDZ2K share conversion.
// - https://eprint.iacr.org/2019/599.pdf
BeaverTinyOt::Triple_Pair BeaverTinyOt::AuthAnd(FieldType field, size_t size,
                                                size_t s) {
  const size_t elsize = SizeOf(field);
  const size_t tinyot_num = size;
  // extra sigma bits = 64
  const size_t sigma = 64;

  auto [auth_a, auth_b, auth_c] =
      TinyMul(comm_, send_opts_, recv_opts_, tinyot_num, tinyot_key_);

  // we need extra sigma bits to check
  auto auth_r = RandomBits(comm_, send_opts_, recv_opts_, sigma, tinyot_key_);

  // For convenient, put a,b,c,r together
  // Then authorize them in SPDZ2k form
  // todo: maybe we can use uint64_t in FM64
  AuthBit auth_abcr{std::vector<bool>(3 * tinyot_num + sigma, false),
                    std::vector<uint128_t>(3 * tinyot_num + sigma, 0),
                    tinyot_key_};
  for (size_t i = 0; i < tinyot_num; ++i) {
    auth_abcr.choices[i] = auth_a.choices[i];
    auth_abcr.choices[tinyot_num + i] = auth_b.choices[i];
    auth_abcr.choices[tinyot_num * 2 + i] = auth_c.choices[i];
  }
  for (size_t i = 0; i < sigma; ++i) {
    auth_abcr.choices[tinyot_num * 3 + i] = auth_r.choices[i];
  }
  std::memcpy(&auth_abcr.mac[0], &auth_a.mac[0],
              tinyot_num * sizeof(uint128_t));
  std::memcpy(&auth_abcr.mac[tinyot_num], &auth_b.mac[0],
              tinyot_num * sizeof(uint128_t));
  std::memcpy(&auth_abcr.mac[tinyot_num * 2], &auth_c.mac[0],
              tinyot_num * sizeof(uint128_t));
  std::memcpy(&auth_abcr.mac[tinyot_num * 3], &auth_r.mac[0],
              sigma * sizeof(uint128_t));

  // Generate authorize bits in the form of B-Share
  ArrayRef spdz_choices(makeType<RingTy>(field), tinyot_num * 3 + sigma);

  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using U = std::make_unsigned<ring2k_t>::type;

    auto _choices = ArrayView<U>(spdz_choices);
    auto _size = auth_abcr.choices.size();
    // copy authbit choices
    yacl::parallel_for(0, _size, 4096, [&](int64_t beg, int64_t end) {
      for (int64_t idx = beg; idx < end; ++idx) {
        _choices[idx] = auth_abcr.choices[idx];
      }
    });
  });

  ArrayRef spdz_mac(makeType<RingTy>(field), tinyot_num * 3 + sigma);
  ArrayRef mask0(makeType<RingTy>(field), tinyot_num * 3 + sigma);
  ArrayRef mask1(makeType<RingTy>(field), tinyot_num * 3 + sigma);
  ArrayRef t(makeType<RingTy>(field), tinyot_num * 3 + sigma);
  auto ext_spdz_key =
      ring_mul(ring_ones(field, tinyot_num * 3 + sigma), spdz_key_);

  if (comm_->getRank() == 0) {
    rotRecv(field, spdz_choices, &t);
    auto recv = comm_->recv(comm_->nextRank(), makeType<RingTy>(field), "recv");

    rotSend(field, &mask0, &mask1);
    auto diff = ring_add(ring_sub(mask0, mask1), ext_spdz_key);
    comm_->sendAsync(comm_->nextRank(), diff, "send");
    spdz_mac = ring_add(t, ring_mul(spdz_choices, recv));
  } else {
    rotSend(field, &mask0, &mask1);
    auto diff = ring_add(ring_sub(mask0, mask1), ext_spdz_key);
    comm_->sendAsync(comm_->nextRank(), diff, "send");

    rotRecv(field, spdz_choices, &t);
    auto recv = comm_->recv(comm_->nextRank(), makeType<RingTy>(field), "recv");
    spdz_mac = ring_add(t, ring_mul(spdz_choices, recv));
  }
  spdz_mac = ring_sub(spdz_mac, mask0);
  spdz_mac = ring_add(spdz_mac, ring_mul(spdz_choices, ext_spdz_key));

  AuthBit check_tiny_bit = {std::vector<bool>(sigma, false),
                            std::vector<uint128_t>(sigma, 0), tinyot_key_};
  ArrayRef check_spdz_bit = ring_zeros(field, sigma);
  ArrayRef check_spdz_mac = ring_zeros(field, sigma);
  auto seed = GenSharedSeed(comm_);
  auto prg = yacl::crypto::Prg<uint64_t>(seed);

  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using U = std::make_unsigned<ring2k_t>::type;

    auto _spdz_bit = ArrayView<U>(spdz_choices);
    auto _spdz_mac = ArrayView<U>(spdz_mac);
    auto _check_spdz_bit = ArrayView<U>(check_spdz_bit);
    auto _check_spdz_mac = ArrayView<U>(check_spdz_mac);

    for (size_t i = 0; i < sigma; ++i) {
      _check_spdz_bit[i] = _spdz_bit[3 * tinyot_num + i];
      _check_spdz_mac[i] = _spdz_mac[3 * tinyot_num + i];
      check_tiny_bit.mac[i] = auth_abcr.mac[tinyot_num * 3 + i];
    }
    for (size_t j = 0; j < tinyot_num * 3; ++j) {
      // we can ignore check_tiny_bit.choices
      uint64_t ceof = prg();
      // sigma = 64
      for (size_t i = 0; i < sigma; ++i) {
        if (ceof & 1) {
          check_tiny_bit.mac[i] ^= auth_abcr.mac[j];
          _check_spdz_bit[i] += _spdz_bit[j];
          _check_spdz_mac[i] += _spdz_mac[j];
        }
        ceof >>= 1;
      }
    }
  });

  // Open sigma bits
  auto [open_bit, zero_mac] = BatchOpen(check_spdz_bit, check_spdz_mac, 1, s);
  check_tiny_bit.choices = ring_cast_vector_boolean(open_bit);

  // TINY Maccheck & SPDZ Maccheck!!
  size_t k = s;
  SPU_ENFORCE(TinyMacCheck(comm_, check_tiny_bit.choices, check_tiny_bit));
  SPU_ENFORCE(BatchMacCheck(open_bit, zero_mac, k, s));

  // Pack a,b,c and their mac
  auto a =
      ArrayRef(spdz_choices.buf(), spdz_choices.eltype(), tinyot_num, 1, 0);
  auto b = ArrayRef(spdz_choices.buf(), spdz_choices.eltype(), tinyot_num, 1,
                    tinyot_num * elsize);
  auto c = ArrayRef(spdz_choices.buf(), spdz_choices.eltype(), tinyot_num, 1,
                    2 * tinyot_num * elsize);

  auto a_mac = ArrayRef(spdz_mac.buf(), spdz_mac.eltype(), tinyot_num, 1, 0);
  auto b_mac = ArrayRef(spdz_mac.buf(), spdz_mac.eltype(), tinyot_num, 1,
                        tinyot_num * elsize);
  auto c_mac = ArrayRef(spdz_mac.buf(), spdz_mac.eltype(), tinyot_num, 1,
                        2 * tinyot_num * elsize);

  return {{a, b, c}, {a_mac, b_mac, c_mac}};
}

BeaverTinyOt::Triple BeaverTinyOt::dot(FieldType field, size_t M, size_t N,
                                       size_t K, size_t k, size_t s) {
  size_t WorldSize = comm_->getWorldSize();
  size_t rank = comm_->getRank();

  auto a = ring_rand(field, M * K);
  auto b = ring_rand(field, K * N);
  ring_bitmask_(a, 0, k);
  ring_bitmask_(b, 0, k);

  auto c = ring_mmul(a, b, M, N, K);

  // w = a * b + v
  std::vector<ArrayRef> w;
  std::vector<ArrayRef> v;
  // every pair calls voleDot
  for (size_t i = 0; i < WorldSize; ++i) {
    for (size_t j = 0; j < WorldSize; ++j) {
      if (i == j) {
        continue;
      }
      if (i == rank) {
        auto tmp = voleRecvDot(field, b, M, N, K);
        w.emplace_back(tmp);
      }
      if (j == rank) {
        auto tmp = voleSendDot(field, a, M, N, K);
        v.emplace_back(tmp);
      }
    }
  }

  for (size_t i = 0; i < WorldSize - 1; ++i) {
    ring_add_(c, ring_sub(w[i], v[i]));
  }
  return {a, b, c};
}

// Refer to:
// 6 PreProcessing: Creating Multiplication Triples,
// SPDZ2k: Efficient MPC mod 2k for Dishonest Majority
// - https://eprint.iacr.org/2018/482.pdf
BeaverTinyOt::Triple_Pair BeaverTinyOt::AuthDot(FieldType field, size_t M,
                                                size_t N, size_t K, size_t k,
                                                size_t s) {
  // Dot
  auto [a_ext, b, c_ext] = dot(field, 2 * M, N, K, k, s);

  // Authenticate
  auto a_ext_mac = AuthArrayRef(a_ext, field, k, s);
  auto b_mac = AuthArrayRef(b, field, k, s);
  auto c_ext_mac = AuthArrayRef(c_ext, field, k, s);

  auto a = a_ext.slice(0, M * K, 1);
  auto a_mac = a_ext_mac.slice(0, M * K, 1);
  auto c = c_ext.slice(0, M * N, 1);
  auto c_mac = c_ext_mac.slice(0, M * N, 1);

  // Sacrifice
  auto a2 = a_ext.slice(M * K, 2 * M * K, 1);
  auto a2_mac = a_ext_mac.slice(M * K, 2 * M * K, 1);
  auto c2 = c_ext.slice(M * N, 2 * M * N, 1);
  auto c2_mac = c_ext_mac.slice(M * N, 2 * M * N, 1);

  auto t = prg_state_->genPubl(field, M * M);
  auto rou = ring_sub(ring_mmul(t, a, M, K, M), a2);
  auto rou_mac = ring_sub(ring_mmul(t, a_mac, M, K, M), a2_mac);

  auto [pub_rou, check_rou_mac] = BatchOpen(rou, rou_mac, k, s);
  SPU_ENFORCE(BatchMacCheck(pub_rou, check_rou_mac, k, s));

  auto t_delta = ring_sub(ring_mmul(t, c, M, N, M), c2);
  auto delta = ring_sub(t_delta, ring_mmul(pub_rou, b, M, N, K));

  auto t_delta_mac = ring_sub(ring_mmul(t, c_mac, M, N, M), c2_mac);
  auto delta_mac = ring_sub(t_delta_mac, ring_mmul(pub_rou, b_mac, M, N, K));

  auto [pub_delta, check_delta_mac] = BatchOpen(delta, delta_mac, k, s);
  SPU_ENFORCE(BatchMacCheck(pub_delta, check_delta_mac, k, s));

  // Output
  return {{a, b, c}, {a_mac, b_mac, c_mac}};
}

BeaverTinyOt::Pair_Pair BeaverTinyOt::AuthTrunc(FieldType field, size_t size,
                                                size_t bits, size_t k,
                                                size_t s) {
  size_t nbits = k;

  auto [b_val, b_mac] = AuthRandBit(field, nbits * size, k, s);

  // compose
  ArrayRef r_val(b_val.eltype(), size);
  ArrayRef r_mac(b_val.eltype(), size);
  ArrayRef tr_val(b_val.eltype(), size);
  ArrayRef tr_mac(b_val.eltype(), size);

  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using PShrT = ring2k_t;
    auto _val = ArrayView<PShrT>(b_val);
    auto _mac = ArrayView<PShrT>(b_mac);
    auto _r_val = ArrayView<PShrT>(r_val);
    auto _r_mac = ArrayView<PShrT>(r_mac);
    auto _tr_val = ArrayView<PShrT>(tr_val);
    auto _tr_mac = ArrayView<PShrT>(tr_mac);
    pforeach(0, size, [&](int64_t idx) {
      _r_val[idx] = 0;
      _r_mac[idx] = 0;
      _tr_val[idx] = 0;
      _tr_mac[idx] = 0;
      for (size_t bit = 0; bit < nbits; bit++) {
        size_t flat_idx = idx * nbits + bit;
        _r_val[idx] += _val[flat_idx] << bit;
        _r_mac[idx] += _mac[flat_idx] << bit;
      }
      for (size_t bit = 0; bit + bits < nbits; bit++) {
        size_t flat_idx = idx * nbits + bits + bit;
        _tr_val[idx] += _val[flat_idx] << bit;
        _tr_mac[idx] += _mac[flat_idx] << bit;
      }

      for (size_t bit = nbits - bits; bit < nbits; bit++) {
        size_t flat_idx = idx * nbits + nbits - 1;
        _tr_val[idx] += _val[flat_idx] << bit;
        _tr_mac[idx] += _mac[flat_idx] << bit;
      }
    });
  });

  return {{r_val, tr_val}, {r_mac, tr_mac}};
}

// Refer to:
// New Primitives for Actively-Secure MPC over Rings with Applications to
// Private Machine Learning.
// Figure 5: Protocol for obtaining authenticated shared bits
// - https://eprint.iacr.org/2019/599.pdf
BeaverTinyOt::Pair BeaverTinyOt::AuthRandBit(FieldType field, size_t size,
                                             size_t k, size_t s) {
  auto u = ring_rand(field, size);
  ring_bitmask_(u, 0, k + 2);
  auto u_mac = AuthArrayRef(u, field, k + 2, s);

  auto y = ring_mul(u, 2);
  auto y_mac = ring_mul(u_mac, 2);
  auto ones = ring_ones(field, size);
  auto ones_mac = ring_mul(ones, spdz_key_);

  if (comm_->getRank() == 0) {
    ring_add_(y, ones);
  }
  ring_add_(y_mac, ones_mac);

  auto [beaver_vec, beaver_mac] = AuthMul(field, size, k, s);
  auto& [a, b, c] = beaver_vec;
  auto& [a_mac, b_mac, c_mac] = beaver_mac;

  auto e = ring_sub(y, a);
  auto e_mac = ring_sub(y_mac, a_mac);
  auto f = ring_sub(y, b);
  auto f_mac = ring_sub(y_mac, b_mac);

  // Open the least significant bit and Check them
  auto [p_e, pe_mac] = BatchOpen(e, e_mac, k + 2, s);
  auto [p_f, pf_mac] = BatchOpen(f, f_mac, k + 2, s);

  SPU_ENFORCE(BatchMacCheck(p_e, pe_mac, k, s));
  SPU_ENFORCE(BatchMacCheck(p_f, pf_mac, k, s));

  // Reserve the least significant bit only
  ring_bitmask_(p_e, 0, k + 2);
  ring_bitmask_(p_f, 0, k + 2);
  auto p_ef = ring_mul(p_e, p_f);

  // z = p_e * b + p_f * a + c;
  auto z = ring_add(ring_mul(p_e, b), ring_mul(p_f, a));
  ring_add_(z, c);
  if (comm_->getRank() == 0) {
    // z += p_e * p_f;
    ring_add_(z, p_ef);
  }

  // z_mac = p_e * b_mac + p_f * a_mac + c_mac + p_e * p_f * key;
  auto z_mac = ring_add(ring_mul(p_e, b_mac), ring_mul(p_f, a_mac));
  ring_add_(z_mac, c_mac);
  ring_add_(z_mac, ring_mul(p_ef, spdz_key_));

  auto [square, zero_mac] = BatchOpen(z, z_mac, k + 2, s);
  SPU_ENFORCE(BatchMacCheck(square, zero_mac, k, s));
  SPU_ENFORCE(ring_all_equal(ring_bitmask(square, 0, 1), ones));
  auto root = ring_sqrt2k(square, k + 2);
  auto root_inv = ring_inv2k(root, k + 2);
  auto root_inv_div2 = ring_rshift(root_inv, 1);

  auto d = ring_mul(root_inv_div2, y);
  auto d_mac = ring_mul(root_inv_div2, y_mac);
  ring_add_(d, u);
  ring_add_(d_mac, u_mac);
  if (comm_->getRank() == 0) {
    ring_add_(d, ones);
  }
  ring_add_(d_mac, ones_mac);

  return {d, d_mac};
}

ArrayRef BeaverTinyOt::genPublCoin(FieldType field, size_t numel) {
  ArrayRef res(makeType<RingTy>(field), numel);

  // generate new seed
  uint128_t seed = yacl::crypto::SecureRandSeed();
  std::vector<std::string> all_strs;

  std::string seed_str(reinterpret_cast<char*>(&seed), sizeof(seed));
  SPU_ENFORCE(commit_and_open(comm_->lctx(), seed_str, &all_strs));

  uint128_t public_seed = 0;
  for (const auto& str : all_strs) {
    uint128_t seed = *(reinterpret_cast<const uint128_t*>(str.data()));
    public_seed += seed;
  }

  const auto kAesType = yacl::crypto::SymmetricCrypto::CryptoType::AES128_CTR;
  yacl::crypto::FillPRand(
      kAesType, public_seed, 0, 0,
      absl::MakeSpan(static_cast<char*>(res.data()), res.buf()->size()));

  return res;
}

// Refer to:
// Procedure BatchCheck, 3.2 Batch MAC Checking with Random Linear
// Combinations, SPDZ2k: Efficient MPC mod 2k for Dishonest Majority
// - https://eprint.iacr.org/2018/482.pdf
//
// Check the opened value only
bool BeaverTinyOt::BatchMacCheck(const ArrayRef& open_value,
                                 const ArrayRef& mac, size_t k, size_t s) {
  SPDLOG_DEBUG("BatchMacCheck start...");
  SPU_ENFORCE(open_value.numel() == mac.numel());
  const auto field = open_value.eltype().as<Ring2k>()->field();
  const size_t mac_bits = k + s;
  const size_t key = spdz_key_;
  size_t num = open_value.numel();

  // 1. Generate ceof
  auto coef = genPublCoin(field, num);
  ring_bitmask_(coef, 0, s);

  // 3. check_value = coef * open_value
  //    check_mac = coef * mac
  auto check_value = ring_mmul(coef, open_value, 1, 1, num);
  auto check_mac = ring_mmul(coef, mac, 1, 1, num);

  // 4. local_mac = check_mac - check_value * key
  auto local_mac = ring_sub(check_mac, ring_mul(check_value, key));
  // commit and reduce all macs
  std::string mac_str(reinterpret_cast<char*>(local_mac.data()),
                      local_mac.numel() * local_mac.elsize());
  std::vector<std::string> all_mac_strs;
  SPU_ENFORCE(commit_and_open(comm_->lctx(), mac_str, &all_mac_strs));
  SPU_ENFORCE(all_mac_strs.size() == comm_->getWorldSize());

  // 5. compute the sum of all macs
  auto zero_mac = ring_zeros(field, 1);
  for (size_t i = 0; i < comm_->getWorldSize(); ++i) {
    const auto& _mac_str = all_mac_strs[i];
    auto buf = std::make_shared<yacl::Buffer>(_mac_str.data(), _mac_str.size());
    ArrayRef _mac(buf, zero_mac.eltype(), _mac_str.size() / SizeOf(field), 1,
                  0);
    ring_add_(zero_mac, _mac);
  }

  // 6. In B-share, the range of Mac is Z_2^{s+1}
  if (mac_bits != 0) {
    ring_bitmask_(zero_mac, 0, mac_bits);
  }

  // 7. verify whether the sum of all macs is zero
  auto res = ring_all_equal(zero_mac, ring_zeros(field, 1));
  SPDLOG_DEBUG("BatchMacCheck end with ret {}.", res);
  return res;
}

// Refer to:
// Procedure BatchCheck, 3.2 Batch MAC Checking with Random Linear
// Combinations, SPDZ2k: Efficient MPC mod 2k for Dishonest Majority
// - https://eprint.iacr.org/2018/482.pdf
//
// Open the value only
// Notice return { open_val , zero_mac = open_val * \sum spdz_key_ }
// the last kth bits in open_val is valid
std::pair<ArrayRef, ArrayRef> BeaverTinyOt::BatchOpen(const ArrayRef& value,
                                                      const ArrayRef& mac,
                                                      size_t k, size_t s) {
  static constexpr char kBindName[] = "batch_open";
  SPU_ENFORCE(value.numel() == mac.numel());
  const auto field = value.eltype().as<Ring2k>()->field();
  size_t field_bits = std::min(SizeOf(field) * 8, (size_t)64);
  auto [r_val, r_mac] = AuthCoinTossing(field, value.numel(), field_bits, s);
  // Open the low k_bits only
  // value = value + r * 2^k
  // mac = mac + r_mac * 2^k
  auto masked_val = ring_add(value, ring_lshift(r_val, k));
  auto masked_mac = ring_add(mac, ring_lshift(r_mac, k));

  // Because we would use Maccheck to comfirm the open value.
  // Thus, we don't need commit them.
  auto open_val = comm_->allReduce(ReduceOp::ADD, masked_val, kBindName);
  return {open_val, masked_mac};
}

void BeaverTinyOt::rotSend(FieldType field, ArrayRef* q0, ArrayRef* q1) {
  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using T = ring2k_t;

    SPDLOG_DEBUG("rotSend start with numel {}", q0->numel());
    SPU_ENFORCE(q0->numel() == q1->numel());
    size_t numel = q0->numel();
    T* data0 = reinterpret_cast<T*>(q0->data());
    T* data1 = reinterpret_cast<T*>(q1->data());

    SPU_ENFORCE(spdz2k_ot_primitives_ != nullptr);
    SPU_ENFORCE(spdz2k_ot_primitives_->GetSenderCOT() != nullptr);

    spdz2k_ot_primitives_->GetSenderCOT()->SendRMCC(
        absl::MakeSpan(data0, numel), absl::MakeSpan(data1, numel));
    spdz2k_ot_primitives_->GetSenderCOT()->Flush();

    SPDLOG_DEBUG("rotSend end");
  });
}

// todo: use dynamic_bitset instead of ArrayRef for `a` to improve performance
void BeaverTinyOt::rotRecv(FieldType field, const ArrayRef& a, ArrayRef* s) {
  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using T = ring2k_t;

    SPDLOG_DEBUG("rotRecv start with numel {}", a.numel());
    size_t numel = a.numel();
    std::vector<uint8_t> b_v(numel);
    for (size_t i = 0; i < numel; ++i) {
      b_v[i] = a.at<T>(i);
    }

    SPU_ENFORCE(spdz2k_ot_primitives_ != nullptr);
    SPU_ENFORCE(spdz2k_ot_primitives_->GetSenderCOT() != nullptr);
    SPU_ENFORCE(spdz2k_ot_primitives_->GetReceiverCOT() != nullptr);

    T* data = reinterpret_cast<T*>(s->data());
    spdz2k_ot_primitives_->GetReceiverCOT()->RecvRMCC(
        b_v, absl::MakeSpan(data, numel));
    spdz2k_ot_primitives_->GetReceiverCOT()->Flush();

    SPDLOG_DEBUG("rotRecv end");
  });
}

// Refer to:
// Appendix C. Implementing Vector-OLE mod 2^l, P35
// SPDZ2k: Efficient MPC mod 2k for Dishonest Majority
// - https://eprint.iacr.org/2018/482.pdf
ArrayRef BeaverTinyOt::voleSend(FieldType field, const ArrayRef& x) {
  return DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using T = ring2k_t;

    SPU_ENFORCE(spdz2k_ot_primitives_ != nullptr);
    SPU_ENFORCE(spdz2k_ot_primitives_->GetSenderCOT() != nullptr);

    size_t numel = x.numel();
    ArrayRef res(x.eltype(), numel);
    T* data = reinterpret_cast<T*>(res.data());
    spdz2k_ot_primitives_->GetSenderCOT()->SendVole(
        absl::MakeConstSpan(reinterpret_cast<const T*>(x.data()), numel),
        absl::MakeSpan(data, numel));

    return res;
  });
}

ArrayRef BeaverTinyOt::voleRecv(FieldType field, const ArrayRef& alpha) {
  return DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using T = ring2k_t;

    SPU_ENFORCE(spdz2k_ot_primitives_ != nullptr);
    SPU_ENFORCE(spdz2k_ot_primitives_->GetReceiverCOT() != nullptr);

    size_t size = alpha.numel();
    ArrayRef res(makeType<RingTy>(field), size);
    T* data = reinterpret_cast<T*>(res.data());
    spdz2k_ot_primitives_->GetReceiverCOT()->RecvVole(
        absl::MakeConstSpan(reinterpret_cast<const T*>(alpha.data()),
                            alpha.numel()),
        absl::MakeSpan(data, size));

    return res;
  });
}

// Private Matrix Multiplication by VOLE
// W = V + A dot B
// Sender: input A, receive V
//
// Input: (M, K) matrix
// Output: (M, N) matrix
ArrayRef BeaverTinyOt::voleSendDot(FieldType field, const ArrayRef& x, size_t M,
                                   size_t N, size_t K) {
  SPU_ENFORCE(x.numel() == static_cast<int64_t>(M * K));

  auto ret = ring_zeros(field, M * N);
  for (size_t i = 0; i < N; ++i) {
    // t: (M, K) matrix
    auto t = voleSend(field, x);

    // process the matrix
    auto ret_col = ret.slice(i, M * N, N);
    for (size_t j = 0; j < K; ++j) {
      ring_add_(ret_col, t.slice(j, M * K, K));
    }
  }

  return ret;
}

// Private Matrix Multiplication by VOLE
// W = V + A dot B
// Receiver: input B, receive W
//
// Input: (K, N) matrix
// Output: (M, N) matrix
ArrayRef BeaverTinyOt::voleRecvDot(FieldType field, const ArrayRef& alpha,
                                   size_t M, size_t N, size_t K) {
  SPU_ENFORCE(alpha.numel() == static_cast<int64_t>(K * N));

  auto ret = ring_zeros(field, M * N);
  for (size_t i = 0; i < N; ++i) {
    auto alpha_col = alpha.slice(i, K * N, N);

    ArrayRef alpha_ext(alpha.eltype(), M * K);
    for (size_t i = 0; i < M; ++i) {
      auto alpha_ext_row = alpha_ext.slice(i * K, (i + 1) * K, 1);
      ring_assign(alpha_ext_row, alpha_col);
    }

    // t: (m, k) matrix
    auto t = voleRecv(field, alpha_ext);

    // process the matrix
    auto ret_col = ret.slice(i, M * N, N);
    for (size_t j = 0; j < K; ++j) {
      ring_add_(ret_col, t.slice(j, M * K, K));
    }
  }

  return ret;
}

// Refer to:
// 6 PreProcessing: Creating Multiplication Triples,
// SPDZ2k: Efficient MPC mod 2k for Dishonest Majority
// - https://eprint.iacr.org/2018/482.pdf
BeaverTinyOt::Triple_Pair BeaverTinyOt::AuthMul(FieldType field, size_t size,
                                                size_t k, size_t s) {
  return DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using T = ring2k_t;

    SPDLOG_DEBUG("AuthMul start...");
    size_t tao = 4 * s + 2 * k;
    size_t expand_tao = tao * size;
    auto a = ring_randbit(field, expand_tao);

    auto b = ring_rand(field, size);
    auto b_arr = ring_zeros(field, expand_tao);
    for (size_t i = 0; i < expand_tao; ++i) {
      b_arr.at<T>(i) = b.at<T>(i / tao);
    }

    // Every ordered pair does following
    size_t WorldSize = comm_->getWorldSize();
    size_t rank = comm_->getRank();
    ArrayRef q0(makeType<RingTy>(field), expand_tao);
    ArrayRef q1(makeType<RingTy>(field), expand_tao);
    ArrayRef t_s(makeType<RingTy>(field), expand_tao);

    std::vector<ArrayRef> ci, cj;

    for (size_t i = 0; i < WorldSize; ++i) {
      for (size_t j = 0; j < WorldSize; ++j) {
        if (i == j) {
          continue;
        }

        if (i == rank) {
          rotRecv(field, a, &t_s);
          auto tmp = comm_->lctx()->Recv(j, "recv_d");
          ArrayRef recv_d(std::make_shared<yacl::Buffer>(tmp), a.eltype(),
                          a.numel(), a.stride(), a.offset());
          auto t = ring_add(t_s, ring_mul(a, recv_d));
          ci.emplace_back(t);
        }

        if (j == rank) {
          rotSend(field, &q0, &q1);
          auto d = ring_add(ring_sub(q0, q1), b_arr);
          comm_->lctx()->SendAsync(i, *(d.buf().get()), "send_d");
          cj.emplace_back(ring_neg(q0));
        }
      }
    }

    auto cij = ring_zeros(field, expand_tao);
    auto cji = ring_zeros(field, expand_tao);
    for (size_t i = 0; i < WorldSize - 1; ++i) {
      ring_add_(cij, ci[i]);
      ring_add_(cji, cj[i]);
    }

    // Construct c
    auto c = ring_mul(a, b_arr);
    auto other_c = ring_add(cij, cji);
    ring_add_(c, other_c);

    // Combine
    auto r = prg_state_->genPubl(field, expand_tao);
    auto r_hat = prg_state_->genPubl(field, expand_tao);
    auto ra = ring_mul(r, a);
    auto ra_hat = ring_mul(r_hat, a);
    auto rc = ring_mul(r, c);
    auto rc_hat = ring_mul(r_hat, c);

    ArrayRef cra = ring_zeros(field, size);
    ArrayRef cra_hat = ring_zeros(field, size);
    ArrayRef crc = ring_zeros(field, size);
    ArrayRef crc_hat = ring_zeros(field, size);

    for (size_t i = 0; i < expand_tao; ++i) {
      cra.at<T>(i / tao) += ra.at<T>(i);
      cra_hat.at<T>(i / tao) += ra_hat.at<T>(i);

      crc.at<T>(i / tao) += rc.at<T>(i);
      crc_hat.at<T>(i / tao) += rc_hat.at<T>(i);
    }

    // Authenticate
    auto a_mac = AuthArrayRef(cra, field, k, s);
    auto b_mac = AuthArrayRef(b, field, k, s);
    auto c_mac = AuthArrayRef(crc, field, k, s);

    auto a_hat_mac = AuthArrayRef(cra_hat, field, k, s);
    auto c_hat_mac = AuthArrayRef(crc_hat, field, k, s);

    // Sacrifice
    auto t = prg_state_->genPubl(field, size);
    auto rou = ring_sub(ring_mul(t, cra), cra_hat);
    auto rou_mac = ring_sub(ring_mul(t, a_mac), a_hat_mac);

    auto [pub_rou, check_rou_mac] = BatchOpen(rou, rou_mac, k, s);
    SPU_ENFORCE(BatchMacCheck(pub_rou, check_rou_mac, k, s));

    auto t_delta = ring_sub(ring_mul(t, crc), crc_hat);
    auto delta = ring_sub(t_delta, ring_mul(b, pub_rou));

    auto t_delta_mac = ring_sub(ring_mul(t, c_mac), c_hat_mac);
    auto delta_mac = ring_sub(t_delta_mac, ring_mul(b_mac, pub_rou));

    auto [pub_delta, check_delta_mac] = BatchOpen(delta, delta_mac, k, s);
    SPU_ENFORCE(BatchMacCheck(pub_delta, check_delta_mac, k, s));

    SPDLOG_DEBUG("AuthMul end");
    // Output
    return BeaverTinyOt::Triple_Pair{{cra, b, crc}, {a_mac, b_mac, c_mac}};
  });
}

}  // namespace spu::mpc::spdz2k
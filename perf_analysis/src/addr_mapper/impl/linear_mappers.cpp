#include <vector>

#include "base/base.h"
#include "dram/dram.h"
#include "addr_mapper/addr_mapper.h"
#include "memory_system/memory_system.h"
#include <bitset>

namespace Ramulator {

class LinearMapperBase : public IAddrMapper {
  public:
    IDRAM* m_dram = nullptr;

    int m_num_levels = -1;          // How many levels in the hierarchy?
    std::vector<int> m_addr_bits;   // How many address bits for each level in the hierarchy?
    Addr_t m_tx_offset = -1;

    int m_col_bits_idx = -1;
    int m_row_bits_idx = -1;


  protected:
    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) {
      m_dram = memory_system->get_ifce<IDRAM>();

      // Populate m_addr_bits vector with the number of address bits for each level in the hierachy
      const auto& count = m_dram->m_organization.count;
      m_num_levels = count.size();
      m_addr_bits.resize(m_num_levels);
      for (size_t level = 0; level < m_addr_bits.size(); level++) {
        m_addr_bits[level] = calc_log2(count[level]);
      }

      // Last (Column) address have the granularity of the prefetch size
      m_addr_bits[m_num_levels - 1] -= calc_log2(m_dram->m_internal_prefetch_size);

      int tx_bytes = m_dram->m_internal_prefetch_size * m_dram->m_channel_width / 8;
      m_tx_offset = calc_log2(tx_bytes);

      // Determine where are the row and col bits for ChRaBaRoCo and RoBaRaCoCh
      try {
        m_row_bits_idx = m_dram->m_levels("row");
      } catch (const std::out_of_range& r) {
        throw std::runtime_error(fmt::format("Organization \"row\" not found in the spec, cannot use linear mapping!"));
      }

      // Assume column is always the last level
      m_col_bits_idx = m_num_levels - 1;
    }

};


class ChRaBaRoCo final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, ChRaBaRoCo, "ChRaBaRoCo", "Applies a trival mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      for (int i = m_addr_bits.size() - 1; i >= 0; i--) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};


class RoBaRaCoCh final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, RoBaRaCoCh, "RoBaRaCoCh", "Applies a RoBaRaCoCh mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      req.addr_vec[0] = slice_lower_bits(addr, m_addr_bits[0]);
      req.addr_vec[m_addr_bits.size() - 1] = slice_lower_bits(addr, m_addr_bits[m_addr_bits.size() - 1]);
      for (int i = 1; i <= m_row_bits_idx; i++) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};


class MOP4CLXOR final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, MOP4CLXOR, "MOP4CLXOR", "Applies a MOP4CLXOR mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      // std::cout << "Binary: " << std::bitset<64>(req.addr) << std::endl; // Adjust size (e.g., 8, 16, 32) as needed

      Addr_t addr = req.addr >> m_tx_offset;

      req.addr_vec[m_col_bits_idx] = slice_lower_bits(addr, 2);
      for (int lvl = 0 ; lvl < m_row_bits_idx ; lvl++){
          req.addr_vec[lvl] = slice_lower_bits(addr, m_addr_bits[lvl]);
      }
      req.addr_vec[m_col_bits_idx] += slice_lower_bits(addr, m_addr_bits[m_col_bits_idx]-2) << 2;
      // std::printf("addr after 2nd col: %d\n", addr);
      Addr_t row_addr = addr;
      req.addr_vec[m_row_bits_idx] = slice_lower_bits(row_addr, m_addr_bits[m_row_bits_idx]);

      int row_xor_index = 0; 
      for (int lvl = 0 ; lvl < m_row_bits_idx ; lvl++){
        if (m_addr_bits[lvl] > 0){
          int mask = (req.addr_vec[m_row_bits_idx] >> row_xor_index) & ((1<<m_addr_bits[lvl])-1);
          // std::cout<<"LV: "<<lvl<<" First: "<<std::bitset<17>(req.addr_vec[m_row_bits_idx] >> row_xor_index)<<"second: "<<std::bitset<5>(1<<m_addr_bits[lvl] -1)<<std::endl;
          // std::cout<< "Mask: " << std::bitset<17>(mask)<<std::endl; // Adjust size (e.g., 8, 16, 32) as needed
          // std::cout<<"addr_vec:" << std::bitset<5>(req.addr_vec[lvl])<<std::endl; // Adjust size (e.g., 8, 16, 32) as needed

          req.addr_vec[lvl] = req.addr_vec[lvl] xor mask;
          row_xor_index += m_addr_bits[lvl];
        }
      }
      // std::printf("DRAM ADDR: CH: %d, RA: %d, BG: %d, BA: %d , Row: %d, COL %d\n", req.addr_vec[0], req.addr_vec[1], req.addr_vec[2], req.addr_vec[3], req.addr_vec[4], req.addr_vec[5]);
    }
};

#include <algorithm>
#include <cstdint>
#include <stdexcept>

class RubixPerm4CL final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(
      IAddrMapper, RubixPerm4CL, "RubixPerm4CL",
      "Applies a Rubix-style static keyed permutation with configurable gang locality."
  );

  private:
    // Preserve 2 low transaction-address bits by default = 4CL locality.
    int m_rubix_gang_bits = 2;

    // Number of low gang-address bits to permute.
    // -1: default to min(22, available gang-address bits)
    //  0: permute the full available gang address
    int m_rubix_perm_bits = -1;

    int m_rubix_rounds = 4;
    uint64_t m_rubix_seed = 1;

    int get_total_addr_bits() const {
      int total = 0;
      for (int b : m_addr_bits) {
        total += b;
      }
      return total;
    }

    int addr_width_bits() const {
      return static_cast<int>(8 * sizeof(Addr_t));
    }

    Addr_t mask_bits(int bits) const {
      if (bits <= 0) {
        return static_cast<Addr_t>(0);
      }
      if (bits >= addr_width_bits()) {
        return ~static_cast<Addr_t>(0);
      }
      return (static_cast<Addr_t>(1) << bits) - 1;
    }

    Addr_t take_low_bits(Addr_t& x, int bits) const {
      if (bits <= 0) {
        return static_cast<Addr_t>(0);
      }

      Addr_t val = x & mask_bits(bits);

      if (bits >= addr_width_bits()) {
        x = 0;
      } else {
        x >>= bits;
      }

      return val;
    }

    uint64_t mix64(uint64_t x) const {
      x += 0x9e3779b97f4a7c15ULL;
      x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
      x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
      x = x ^ (x >> 31);
      return x;
    }

    // Bijective permutation over exactly `bits` low bits.
    // This is not a many-to-one hash. The XOR-update structure is invertible.
    Addr_t permute_nbits(Addr_t x, int bits) const {
      if (bits <= 0) {
        return static_cast<Addr_t>(0);
      }

      if (bits == 1) {
        return static_cast<Addr_t>((static_cast<uint64_t>(x) ^ m_rubix_seed) & 1ULL);
      }

      const uint64_t full_mask = static_cast<uint64_t>(mask_bits(bits));
      uint64_t v = static_cast<uint64_t>(x) & full_mask;

      const int lbits = bits / 2;
      const int rbits = bits - lbits;

      const uint64_t lmask = (lbits >= 64) ? ~0ULL : ((1ULL << lbits) - 1ULL);
      const uint64_t rmask = (rbits >= 64) ? ~0ULL : ((1ULL << rbits) - 1ULL);

      uint64_t L = v & lmask;
      uint64_t R = (v >> lbits) & rmask;

      for (int r = 0; r < m_rubix_rounds; r++) {
        const uint64_t c1 = 0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(r + 1);
        const uint64_t c2 = 0xbf58476d1ce4e5b9ULL * static_cast<uint64_t>(r + 1);

        L ^= mix64(R ^ m_rubix_seed ^ c1) & lmask;
        R ^= mix64(L ^ (m_rubix_seed << 1) ^ c2) & rmask;
      }

      uint64_t out = ((R & rmask) << lbits) | (L & lmask);
      return static_cast<Addr_t>(out & full_mask);
    }

  public:
    void init() override {
      m_rubix_gang_bits = param<int>("rubix_gang_bits").default_val(2);
      m_rubix_perm_bits = param<int>("rubix_perm_bits").default_val(-1);
      m_rubix_rounds = param<int>("rubix_rounds").default_val(4);
      m_rubix_seed = param<uint64_t>("rubix_seed").default_val(1);
    }

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);

      const int total_bits = get_total_addr_bits();

      if (m_rubix_gang_bits < 0) {
        throw std::runtime_error("[RubixPerm4CL] rubix_gang_bits must be non-negative.");
      }

      if (m_rubix_gang_bits > m_addr_bits[m_col_bits_idx]) {
        throw std::runtime_error("[RubixPerm4CL] rubix_gang_bits exceeds column bits.");
      }

      const int max_perm_bits = total_bits - m_rubix_gang_bits;

      if (max_perm_bits <= 0) {
        throw std::runtime_error("[RubixPerm4CL] no address bits left to permute.");
      }

      if (max_perm_bits > addr_width_bits()) {
        throw std::runtime_error("[RubixPerm4CL] address width exceeds Addr_t capacity.");
      }

      if (m_rubix_perm_bits == -1) {
        m_rubix_perm_bits = std::min(22, max_perm_bits);
      } else if (m_rubix_perm_bits == 0) {
        m_rubix_perm_bits = max_perm_bits;
      }

      if (m_rubix_perm_bits < 0 || m_rubix_perm_bits > max_perm_bits) {
        throw std::runtime_error("[RubixPerm4CL] invalid rubix_perm_bits.");
      }

      if (m_rubix_rounds <= 0) {
        throw std::runtime_error("[RubixPerm4CL] rubix_rounds must be positive.");
      }
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);

      // Transaction-granular address.
      Addr_t lineaddr = req.addr >> m_tx_offset;

      // ------------------------------------------------------------
      // 1. Preserve low gang bits.
      // Default: 2 bits = 4CL locality.
      // ------------------------------------------------------------
      Addr_t gang_offset = lineaddr & mask_bits(m_rubix_gang_bits);

      if (m_rubix_gang_bits >= addr_width_bits()) {
        lineaddr = 0;
      } else {
        lineaddr >>= m_rubix_gang_bits;
      }

      // Now lineaddr is the gang address.
      Addr_t gang_addr = lineaddr;

      // ------------------------------------------------------------
      // 2. Permute low rubix_perm_bits of the gang address.
      // High gang-address bits are preserved, similar to AutoRFM's
      // addr_base concept.
      // ------------------------------------------------------------
      Addr_t gang_base;
      if (m_rubix_perm_bits >= addr_width_bits()) {
        gang_base = 0;
      } else {
        gang_base = gang_addr >> m_rubix_perm_bits;
      }

      Addr_t gang_low = gang_addr & mask_bits(m_rubix_perm_bits);
      Addr_t randomized_gang_low = permute_nbits(gang_low, m_rubix_perm_bits);

      Addr_t randomized_gang_addr;
      if (m_rubix_perm_bits >= addr_width_bits()) {
        randomized_gang_addr = randomized_gang_low;
      } else {
        randomized_gang_addr =
            (gang_base << m_rubix_perm_bits) | randomized_gang_low;
      }

      // ------------------------------------------------------------
      // 3. Slice randomized gang address into DRAM fields.
      // Preserve gang_offset as the low column bits.
      // ------------------------------------------------------------
      req.addr_vec[m_col_bits_idx] = static_cast<int>(gang_offset);

      Addr_t x = randomized_gang_addr;

      for (int lvl = 0; lvl < m_row_bits_idx; lvl++) {
        req.addr_vec[lvl] = static_cast<int>(take_low_bits(x, m_addr_bits[lvl]));
      }

      const int remaining_col_bits =
          m_addr_bits[m_col_bits_idx] - m_rubix_gang_bits;

      if (remaining_col_bits > 0) {
        Addr_t col_high = take_low_bits(x, remaining_col_bits);
        Addr_t col_val = gang_offset | (col_high << m_rubix_gang_bits);
        req.addr_vec[m_col_bits_idx] = static_cast<int>(col_val);
      }

      req.addr_vec[m_row_bits_idx] =
          static_cast<int>(take_low_bits(x, m_addr_bits[m_row_bits_idx]));
    }
};

}   // namespace Ramulator
#pragma once
#include "model.hpp"
#include <asio.hpp>
#include <memory>
#include <span>
namespace scada {
struct FinsAddress {
    std::uint8_t area{}, bit{};
    std::uint16_t word{}, count{};
    bool bits{};
    int position() const {
        return bits ? word * 16 + bit : word;
    }
};
FinsAddress fins_address(const Tag &tag);
Bytes fins_request(const Controller &c, const Tag &first, std::uint16_t count, std::uint8_t sid);
std::vector<Sample> fins_decode(const Controller &c, const std::vector<std::size_t> &tags, std::uint8_t sid,
                                std::span<const std::uint8_t> reply);
Bytes cip_path(const std::string &address);
Bytes cip_read(const Tag &tag);
double cip_value(Type type, std::span<const std::uint8_t> reply);
std::vector<std::vector<std::size_t>> read_plan(const Controller &c);
class PlcClient {
  public:
    virtual ~PlcClient() = default;
    virtual asio::awaitable<std::vector<Sample>> read(const std::vector<std::size_t> &tags) = 0;
    virtual void close() = 0;
};
std::shared_ptr<PlcClient> make_plc(asio::any_io_executor executor, const Controller &controller);
std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t p);
std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t p);
std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t p);
void put_be16(Bytes &b, std::uint16_t v);
void put_be32(Bytes &b, std::uint32_t v);
void put_le16(Bytes &b, std::uint16_t v);
} // namespace scada

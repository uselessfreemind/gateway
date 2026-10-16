#pragma once
#include "model.hpp"
#include <asio.hpp>
#include <memory>
#include <span>
namespace scada {
struct Item {
    std::uint8_t format{};
    std::vector<Item> list;
    Bytes data;
    static Item L(std::vector<Item> children = {});
    static Item U(std::uint32_t value);
    static Item B(std::uint8_t value);
    static Item A(const std::string &value);
    static Item F(double value);
    std::uint32_t uint() const;
    bool flag() const;
    Bytes encode() const;
    static Item decode(std::span<const std::uint8_t> data);
};
struct Frame {
    std::uint16_t session{};
    std::uint8_t stream{}, function{}, stype{};
    bool w{};
    std::uint32_t system{};
    Bytes body;
    Bytes encode() const;
    static Frame decode(std::span<const std::uint8_t> data);
    static Frame control(std::uint8_t type, std::uint32_t system, std::uint8_t status = 0);
};
class GemModel {
    const Config &config_;
    SharedState &state_;
    std::map<std::uint32_t, std::pair<const Controller *, const Tag *>> tags_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> reports_;
    std::vector<std::uint32_t> linked_;
    bool enabled_{};
    Item value(std::uint32_t id);

  public:
    bool communicating{}, online{};
    GemModel(const Config &, SharedState &);
    std::pair<std::uint8_t, std::optional<Item>> handle(std::uint8_t stream, std::uint8_t function,
                                                        const std::optional<Item> &item);
    std::optional<Item> event();
};
class HsmsServer : public std::enable_shared_from_this<HsmsServer> {
    asio::any_io_executor executor_;
    const Config &config_;
    SharedState &state_;
    std::shared_ptr<asio::ip::tcp::acceptor> listener_;
    std::shared_ptr<asio::ip::tcp::socket> socket_;
    std::shared_ptr<asio::steady_timer> timer_;
    bool stopped_{};

  public:
    HsmsServer(asio::any_io_executor, const Config &, SharedState &);
    asio::awaitable<void> run();
    void close();
};
} // namespace scada

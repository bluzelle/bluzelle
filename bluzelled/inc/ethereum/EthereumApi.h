#ifndef KEPLER_ETHEREUM_API_H_H
#define KEPLER_ETHEREUM_API_H_H

#include <cstdlib>

#include <string>
#include <strstream>

#include <boost/format.hpp>
#include <boost/asio.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include "EthereumToken.h"


using std::string;

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;


class EthereumApi {
    const string host_ = "ropsten.etherscan.io";
    const string etherscan_io_token_environment_variable_name = "ETHERSCAN_IO_API_TOKEN";

    const string get_token_balance_by_token_contract_address_format =
            "/api?module=account&action=tokenbalance&contractaddress=%s&address=%s&tag=latest&apikey=%s";

    string address_;
    tcp::resolver resolver_;
    tcp::socket sock_;
    string token_;

public:
    EthereumApi(string str_addr,
                boost::asio::io_service &ios)
            : address_(std::move(str_addr)),
              resolver_(ios), sock_(ios) {

        auto env = std::getenv(etherscan_io_token_environment_variable_name.c_str());
        if (env != nullptr)
            token_ = env;
    }

    double token_balance(const EthereumToken& t);

private:
    void connect_socket();

    void write_request(const string &target);

    string read_response();

    void close_socket();

    boost::property_tree::ptree parse_response(const string &body) const;

    template<typename T> T get_field(const boost::property_tree::ptree &tuple,
                                     const string &name) const;

    void check_token();
};


#endif //KEPLER_ETHEREUM_API_H_H

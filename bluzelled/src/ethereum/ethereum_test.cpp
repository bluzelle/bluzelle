#define BOOST_TEST_DYN_LINK
#define BOOST_USE_VALGRIND

#include <iostream>

#include <boost/test/unit_test.hpp>
#include <boost/asio.hpp>

#include "ethereum/EthereumApi.h"


// --run_test=test_token_balance
BOOST_AUTO_TEST_CASE( test_token_balance ) {
    try
        {
        boost::asio::io_service ios;

        EthereumApi api("0x006eae72077449caca91078ef78552c0cd9bce8f", ios);
        auto balance = api.token_balance(EthereumToken("0x2ba35056580b505690c03dfb1df58bc6b6cd9f89", 18));

        BOOST_CHECK(balance > 0.0);
        }
    catch (std::exception& ex)
        {
        std::cout << ex.what() << std::endl;
        BOOST_CHECK(false);
        }
}

// --run_test=test_token_balance_fail
BOOST_AUTO_TEST_CASE( test_token_balance_fail ) {
    try
        {
        boost::asio::io_service ios;

        EthereumApi api("0xffffffffffffffffffffffffffffffffffffffff", ios);
        auto balance = api.token_balance(EthereumToken("0x2ba35056580b505690c03dfb1df58bc6b6cd9f89", 18));

        BOOST_CHECK(balance == 0.0);
        }
    catch (std::exception& ex)
        {
        std::cout << ex.what() << std::endl;
        BOOST_CHECK(false);
        }
}